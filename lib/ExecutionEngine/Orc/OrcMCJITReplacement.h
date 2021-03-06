//===---- OrcMCJITReplacement.h - Orc based MCJIT replacement ---*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Orc based MCJIT replacement.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_EXECUTIONENGINE_ORC_ORCMCJITREPLACEMENT_H
#define LLVM_LIB_EXECUTIONENGINE_ORC_ORCMCJITREPLACEMENT_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ExecutionEngine/ExecutionEngine.h"
#include "llvm/ExecutionEngine/GenericValue.h"
#include "llvm/ExecutionEngine/JITSymbol.h"
#include "llvm/ExecutionEngine/RuntimeDyld.h"
#include "llvm/ExecutionEngine/Orc/CompileUtils.h"
#include "llvm/ExecutionEngine/Orc/IRCompileLayer.h"
#include "llvm/ExecutionEngine/Orc/LazyEmittingLayer.h"
#include "llvm/ExecutionEngine/Orc/ObjectLinkingLayer.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Mangler.h"
#include "llvm/Object/Archive.h"
#include "llvm/Object/Binary.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace llvm {
namespace orc {

class OrcMCJITReplacement : public ExecutionEngine {
  // OrcMCJITReplacement needs to do a little extra book-keeping to ensure that
  // Orc's automatic finalization doesn't kick in earlier than MCJIT clients are
  // expecting - see finalizeMemory.
  class MCJITReplacementMemMgr : public MCJITMemoryManager {
  public:
    MCJITReplacementMemMgr(OrcMCJITReplacement &M,
                           std::shared_ptr<MCJITMemoryManager> ClientMM)
      : M(M), ClientMM(std::move(ClientMM)) {}

    uint8_t *allocateCodeSection(uintptr_t Size, unsigned Alignment,
                                 unsigned SectionID,
                                 StringRef SectionName) override {
      uint8_t *Addr =
          ClientMM->allocateCodeSection(Size, Alignment, SectionID,
                                        SectionName);
      M.SectionsAllocatedSinceLastLoad.insert(Addr);
      return Addr;
    }

    uint8_t *allocateDataSection(uintptr_t Size, unsigned Alignment,
                                 unsigned SectionID, StringRef SectionName,
                                 bool IsReadOnly) override {
      uint8_t *Addr = ClientMM->allocateDataSection(Size, Alignment, SectionID,
                                                    SectionName, IsReadOnly);
      M.SectionsAllocatedSinceLastLoad.insert(Addr);
      return Addr;
    }

    void reserveAllocationSpace(uintptr_t CodeSize, uint32_t CodeAlign,
                                uintptr_t RODataSize, uint32_t RODataAlign,
                                uintptr_t RWDataSize,
                                uint32_t RWDataAlign) override {
      return ClientMM->reserveAllocationSpace(CodeSize, CodeAlign,
                                              RODataSize, RODataAlign,
                                              RWDataSize, RWDataAlign);
    }

    bool needsToReserveAllocationSpace() override {
      return ClientMM->needsToReserveAllocationSpace();
    }

    void registerEHFrames(uint8_t *Addr, uint64_t LoadAddr,
                          size_t Size) override {
      return ClientMM->registerEHFrames(Addr, LoadAddr, Size);
    }

    void deregisterEHFrames(uint8_t *Addr, uint64_t LoadAddr,
                            size_t Size) override {
      return ClientMM->deregisterEHFrames(Addr, LoadAddr, Size);
    }

    void notifyObjectLoaded(RuntimeDyld &RTDyld,
                            const object::ObjectFile &O) override {
      return ClientMM->notifyObjectLoaded(RTDyld, O);
    }

    void notifyObjectLoaded(ExecutionEngine *EE,
                            const object::ObjectFile &O) override {
      return ClientMM->notifyObjectLoaded(EE, O);
    }

    bool finalizeMemory(std::string *ErrMsg = nullptr) override {
      // Each set of objects loaded will be finalized exactly once, but since
      // symbol lookup during relocation may recursively trigger the
      // loading/relocation of other modules, and since we're forwarding all
      // finalizeMemory calls to a single underlying memory manager, we need to
      // defer forwarding the call on until all necessary objects have been
      // loaded. Otherwise, during the relocation of a leaf object, we will end
      // up finalizing memory, causing a crash further up the stack when we
      // attempt to apply relocations to finalized memory.
      // To avoid finalizing too early, look at how many objects have been
      // loaded but not yet finalized. This is a bit of a hack that relies on
      // the fact that we're lazily emitting object files: The only way you can
      // get more than one set of objects loaded but not yet finalized is if
      // they were loaded during relocation of another set.
      if (M.UnfinalizedSections.size() == 1)
        return ClientMM->finalizeMemory(ErrMsg);
      return false;
    }

  private:
    OrcMCJITReplacement &M;
    std::shared_ptr<MCJITMemoryManager> ClientMM;
  };

  class LinkingResolver : public JITSymbolResolver {
  public:
    LinkingResolver(OrcMCJITReplacement &M) : M(M) {}

    JITSymbol findSymbol(const std::string &Name) override {
      return M.ClientResolver->findSymbol(Name);
    }

    JITSymbol findSymbolInLogicalDylib(const std::string &Name) override {
      if (auto Sym = M.findMangledSymbol(Name))
        return Sym;
      return M.ClientResolver->findSymbolInLogicalDylib(Name);
    }

  private:
    OrcMCJITReplacement &M;
  };

private:

  static ExecutionEngine *
  createOrcMCJITReplacement(std::string *ErrorMsg,
                            std::shared_ptr<MCJITMemoryManager> MemMgr,
                            std::shared_ptr<JITSymbolResolver> Resolver,
                            std::unique_ptr<TargetMachine> TM) {
    return new OrcMCJITReplacement(std::move(MemMgr), std::move(Resolver),
                                   std::move(TM));
  }

public:
  static void Register() {
    OrcMCJITReplacementCtor = createOrcMCJITReplacement;
  }

  OrcMCJITReplacement(
      std::shared_ptr<MCJITMemoryManager> MemMgr,
      std::shared_ptr<JITSymbolResolver> ClientResolver,
      std::unique_ptr<TargetMachine> TM)
      : ExecutionEngine(TM->createDataLayout()), TM(std::move(TM)),
        MemMgr(*this, std::move(MemMgr)), Resolver(*this),
        ClientResolver(std::move(ClientResolver)), NotifyObjectLoaded(*this),
        NotifyFinalized(*this),
        ObjectLayer(NotifyObjectLoaded, NotifyFinalized),
        CompileLayer(ObjectLayer, SimpleCompiler(*this->TM)),
        LazyEmitLayer(CompileLayer) {}

  void addModule(std::unique_ptr<Module> M) override {

    // If this module doesn't have a DataLayout attached then attach the
    // default.
    if (M->getDataLayout().isDefault()) {
      M->setDataLayout(getDataLayout());
    } else {
      assert(M->getDataLayout() == getDataLayout() && "DataLayout Mismatch");
    }
    Modules.push_back(std::move(M));
    std::vector<Module *> Ms;
    Ms.push_back(&*Modules.back());
    LazyEmitLayer.addModuleSet(std::move(Ms), &MemMgr, &Resolver);
  }

  void addObjectFile(std::unique_ptr<object::ObjectFile> O) override {
    std::vector<std::unique_ptr<object::ObjectFile>> Objs;
    Objs.push_back(std::move(O));
    ObjectLayer.addObjectSet(std::move(Objs), &MemMgr, &Resolver);
  }

  void addObjectFile(object::OwningBinary<object::ObjectFile> O) override {
    std::vector<std::unique_ptr<object::OwningBinary<object::ObjectFile>>> Objs;
    Objs.push_back(
      llvm::make_unique<object::OwningBinary<object::ObjectFile>>(
        std::move(O)));
    ObjectLayer.addObjectSet(std::move(Objs), &MemMgr, &Resolver);
  }

  void addArchive(object::OwningBinary<object::Archive> A) override {
    Archives.push_back(std::move(A));
  }

  uint64_t getSymbolAddress(StringRef Name) {
    return findSymbol(Name).getAddress();
  }

  JITSymbol findSymbol(StringRef Name) {
    return findMangledSymbol(Mangle(Name));
  }

  void finalizeObject() override {
    // This is deprecated - Aim to remove in ExecutionEngine.
    // REMOVE IF POSSIBLE - Doesn't make sense for New JIT.
  }

  void mapSectionAddress(const void *LocalAddress,
                         uint64_t TargetAddress) override {
    for (auto &P : UnfinalizedSections)
      if (P.second.count(LocalAddress))
        ObjectLayer.mapSectionAddress(P.first, LocalAddress, TargetAddress);
  }

  uint64_t getGlobalValueAddress(const std::string &Name) override {
    return getSymbolAddress(Name);
  }

  uint64_t getFunctionAddress(const std::string &Name) override {
    return getSymbolAddress(Name);
  }

  void *getPointerToFunction(Function *F) override {
    uint64_t FAddr = getSymbolAddress(F->getName());
    return reinterpret_cast<void *>(static_cast<uintptr_t>(FAddr));
  }

  void *getPointerToNamedFunction(StringRef Name,
                                  bool AbortOnFailure = true) override {
    uint64_t Addr = getSymbolAddress(Name);
    if (!Addr && AbortOnFailure)
      llvm_unreachable("Missing symbol!");
    return reinterpret_cast<void *>(static_cast<uintptr_t>(Addr));
  }

  GenericValue runFunction(Function *F,
                           ArrayRef<GenericValue> ArgValues) override;

  void setObjectCache(ObjectCache *NewCache) override {
    CompileLayer.setObjectCache(NewCache);
  }

  void setProcessAllSections(bool ProcessAllSections) override {
    ObjectLayer.setProcessAllSections(ProcessAllSections);
  }

private:
  JITSymbol findMangledSymbol(StringRef Name) {
    if (auto Sym = LazyEmitLayer.findSymbol(Name, false))
      return Sym;
    if (auto Sym = ClientResolver->findSymbol(Name))
      return Sym;
    if (auto Sym = scanArchives(Name))
      return Sym;

    return nullptr;
  }

  JITSymbol scanArchives(StringRef Name) {
    for (object::OwningBinary<object::Archive> &OB : Archives) {
      object::Archive *A = OB.getBinary();
      // Look for our symbols in each Archive
      auto OptionalChildOrErr = A->findSym(Name);
      if (!OptionalChildOrErr)
        report_fatal_error(OptionalChildOrErr.takeError());
      auto &OptionalChild = *OptionalChildOrErr;
      if (OptionalChild) {
        // FIXME: Support nested archives?
        Expected<std::unique_ptr<object::Binary>> ChildBinOrErr =
            OptionalChild->getAsBinary();
        if (!ChildBinOrErr) {
          // TODO: Actually report errors helpfully.
          consumeError(ChildBinOrErr.takeError());
          continue;
        }
        std::unique_ptr<object::Binary> &ChildBin = ChildBinOrErr.get();
        if (ChildBin->isObject()) {
          std::vector<std::unique_ptr<object::ObjectFile>> ObjSet;
          ObjSet.push_back(std::unique_ptr<object::ObjectFile>(
              static_cast<object::ObjectFile *>(ChildBin.release())));
          ObjectLayer.addObjectSet(std::move(ObjSet), &MemMgr, &Resolver);
          if (auto Sym = ObjectLayer.findSymbol(Name, true))
            return Sym;
        }
      }
    }
    return nullptr;
  }

  class NotifyObjectLoadedT {
  public:
    typedef std::vector<std::unique_ptr<RuntimeDyld::LoadedObjectInfo>>
        LoadedObjInfoListT;

    NotifyObjectLoadedT(OrcMCJITReplacement &M) : M(M) {}

    template <typename ObjListT>
    void operator()(ObjectLinkingLayerBase::ObjSetHandleT H,
                    const ObjListT &Objects,
                    const LoadedObjInfoListT &Infos) const {
      M.UnfinalizedSections[H] = std::move(M.SectionsAllocatedSinceLastLoad);
      M.SectionsAllocatedSinceLastLoad = SectionAddrSet();
      assert(Objects.size() == Infos.size() &&
             "Incorrect number of Infos for Objects.");
      for (unsigned I = 0; I < Objects.size(); ++I)
        M.MemMgr.notifyObjectLoaded(&M, getObject(*Objects[I]));
    }

  private:
    static const object::ObjectFile& getObject(const object::ObjectFile &Obj) {
      return Obj;
    }

    template <typename ObjT>
    static const object::ObjectFile&
    getObject(const object::OwningBinary<ObjT> &Obj) {
      return *Obj.getBinary();
    }

    OrcMCJITReplacement &M;
  };

  class NotifyFinalizedT {
  public:
    NotifyFinalizedT(OrcMCJITReplacement &M) : M(M) {}

    void operator()(ObjectLinkingLayerBase::ObjSetHandleT H) {
      M.UnfinalizedSections.erase(H);
    }

  private:
    OrcMCJITReplacement &M;
  };

  std::string Mangle(StringRef Name) {
    std::string MangledName;
    {
      raw_string_ostream MangledNameStream(MangledName);
      Mang.getNameWithPrefix(MangledNameStream, Name, getDataLayout());
    }
    return MangledName;
  }

  typedef ObjectLinkingLayer<NotifyObjectLoadedT> ObjectLayerT;
  typedef IRCompileLayer<ObjectLayerT> CompileLayerT;
  typedef LazyEmittingLayer<CompileLayerT> LazyEmitLayerT;

  std::unique_ptr<TargetMachine> TM;
  MCJITReplacementMemMgr MemMgr;
  LinkingResolver Resolver;
  std::shared_ptr<JITSymbolResolver> ClientResolver;
  Mangler Mang;

  NotifyObjectLoadedT NotifyObjectLoaded;
  NotifyFinalizedT NotifyFinalized;

  ObjectLayerT ObjectLayer;
  CompileLayerT CompileLayer;
  LazyEmitLayerT LazyEmitLayer;

  // We need to store ObjLayerT::ObjSetHandles for each of the object sets
  // that have been emitted but not yet finalized so that we can forward the
  // mapSectionAddress calls appropriately.
  typedef std::set<const void *> SectionAddrSet;
  struct ObjSetHandleCompare {
    bool operator()(ObjectLayerT::ObjSetHandleT H1,
                    ObjectLayerT::ObjSetHandleT H2) const {
      return &*H1 < &*H2;
    }
  };
  SectionAddrSet SectionsAllocatedSinceLastLoad;
  std::map<ObjectLayerT::ObjSetHandleT, SectionAddrSet, ObjSetHandleCompare>
      UnfinalizedSections;

  std::vector<object::OwningBinary<object::Archive>> Archives;
};

} // end namespace orc
} // end namespace llvm

#endif // LLVM_LIB_EXECUTIONENGINE_ORC_MCJITREPLACEMENT_H
