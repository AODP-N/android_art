/*
 * Copyright (C) 2014 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "inliner.h"

#include "art_method-inl.h"
#include "base/enums.h"
#include "builder.h"
#include "class_linker.h"
#include "constant_folding.h"
#include "dead_code_elimination.h"
#include "dex/verified_method.h"
#include "dex/verification_results.h"
#include "driver/compiler_driver-inl.h"
#include "driver/compiler_options.h"
#include "driver/dex_compilation_unit.h"
#include "instruction_simplifier.h"
#include "intrinsics.h"
#include "jit/jit.h"
#include "jit/jit_code_cache.h"
#include "mirror/class_loader.h"
#include "mirror/dex_cache.h"
#include "nodes.h"
#include "optimizing_compiler.h"
#include "reference_type_propagation.h"
#include "register_allocator_linear_scan.h"
#include "quick/inline_method_analyser.h"
#include "sharpening.h"
#include "ssa_builder.h"
#include "ssa_phi_elimination.h"
#include "scoped_thread_state_change-inl.h"
#include "thread.h"

namespace art {

static constexpr size_t kMaximumNumberOfHInstructions = 32;

// Limit the number of dex registers that we accumulate while inlining
// to avoid creating large amount of nested environments.
static constexpr size_t kMaximumNumberOfCumulatedDexRegisters = 64;

// Avoid inlining within a huge method due to memory pressure.
static constexpr size_t kMaximumCodeUnitSize = 4096;

void HInliner::Run() {
  const CompilerOptions& compiler_options = compiler_driver_->GetCompilerOptions();
  if ((compiler_options.GetInlineDepthLimit() == 0)
      || (compiler_options.GetInlineMaxCodeUnits() == 0)) {
    return;
  }
  if (caller_compilation_unit_.GetCodeItem()->insns_size_in_code_units_ > kMaximumCodeUnitSize) {
    return;
  }
  if (graph_->IsDebuggable()) {
    // For simplicity, we currently never inline when the graph is debuggable. This avoids
    // doing some logic in the runtime to discover if a method could have been inlined.
    return;
  }
  // Keep a copy of all blocks when starting the visit.
  ArenaVector<HBasicBlock*> blocks = graph_->GetReversePostOrder();
  DCHECK(!blocks.empty());
  // Because we are changing the graph when inlining,
  // we just iterate over the blocks of the outer method.
  // This avoids doing the inlining work again on the inlined blocks.
  for (HBasicBlock* block : blocks) {
    for (HInstruction* instruction = block->GetFirstInstruction(); instruction != nullptr;) {
      HInstruction* next = instruction->GetNext();
      HInvoke* call = instruction->AsInvoke();
      // As long as the call is not intrinsified, it is worth trying to inline.
      if (call != nullptr && call->GetIntrinsic() == Intrinsics::kNone) {
        if (kIsDebugBuild && IsCompilingWithCoreImage()) {
          // Debugging case: directives in method names control or assert on inlining.
          std::string callee_name = outer_compilation_unit_.GetDexFile()->PrettyMethod(
              call->GetDexMethodIndex(), /* with_signature */ false);
          // Tests prevent inlining by having $noinline$ in their method names.
          if (callee_name.find("$noinline$") == std::string::npos) {
            if (!TryInline(call)) {
              bool should_have_inlined = (callee_name.find("$inline$") != std::string::npos);
              CHECK(!should_have_inlined) << "Could not inline " << callee_name;
            }
          }
        } else {
          // Normal case: try to inline.
          TryInline(call);
        }
      }
      instruction = next;
    }
  }
}

static bool IsMethodOrDeclaringClassFinal(ArtMethod* method)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  return method->IsFinal() || method->GetDeclaringClass()->IsFinal();
}

/**
 * Given the `resolved_method` looked up in the dex cache, try to find
 * the actual runtime target of an interface or virtual call.
 * Return nullptr if the runtime target cannot be proven.
 */
static ArtMethod* FindVirtualOrInterfaceTarget(HInvoke* invoke, ArtMethod* resolved_method)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (IsMethodOrDeclaringClassFinal(resolved_method)) {
    // No need to lookup further, the resolved method will be the target.
    return resolved_method;
  }

  HInstruction* receiver = invoke->InputAt(0);
  if (receiver->IsNullCheck()) {
    // Due to multiple levels of inlining within the same pass, it might be that
    // null check does not have the reference type of the actual receiver.
    receiver = receiver->InputAt(0);
  }
  ReferenceTypeInfo info = receiver->GetReferenceTypeInfo();
  DCHECK(info.IsValid()) << "Invalid RTI for " << receiver->DebugName();
  if (!info.IsExact()) {
    // We currently only support inlining with known receivers.
    // TODO: Remove this check, we should be able to inline final methods
    // on unknown receivers.
    return nullptr;
  } else if (info.GetTypeHandle()->IsInterface()) {
    // Statically knowing that the receiver has an interface type cannot
    // help us find what is the target method.
    return nullptr;
  } else if (!resolved_method->GetDeclaringClass()->IsAssignableFrom(info.GetTypeHandle().Get())) {
    // The method that we're trying to call is not in the receiver's class or super classes.
    return nullptr;
  } else if (info.GetTypeHandle()->IsErroneous()) {
    // If the type is erroneous, do not go further, as we are going to query the vtable or
    // imt table, that we can only safely do on non-erroneous classes.
    return nullptr;
  }

  ClassLinker* cl = Runtime::Current()->GetClassLinker();
  PointerSize pointer_size = cl->GetImagePointerSize();
  if (invoke->IsInvokeInterface()) {
    resolved_method = info.GetTypeHandle()->FindVirtualMethodForInterface(
        resolved_method, pointer_size);
  } else {
    DCHECK(invoke->IsInvokeVirtual());
    resolved_method = info.GetTypeHandle()->FindVirtualMethodForVirtual(
        resolved_method, pointer_size);
  }

  if (resolved_method == nullptr) {
    // The information we had on the receiver was not enough to find
    // the target method. Since we check above the exact type of the receiver,
    // the only reason this can happen is an IncompatibleClassChangeError.
    return nullptr;
  } else if (!resolved_method->IsInvokable()) {
    // The information we had on the receiver was not enough to find
    // the target method. Since we check above the exact type of the receiver,
    // the only reason this can happen is an IncompatibleClassChangeError.
    return nullptr;
  } else if (IsMethodOrDeclaringClassFinal(resolved_method)) {
    // A final method has to be the target method.
    return resolved_method;
  } else if (info.IsExact()) {
    // If we found a method and the receiver's concrete type is statically
    // known, we know for sure the target.
    return resolved_method;
  } else {
    // Even if we did find a method, the receiver type was not enough to
    // statically find the runtime target.
    return nullptr;
  }
}

static uint32_t FindMethodIndexIn(ArtMethod* method,
                                  const DexFile& dex_file,
                                  uint32_t name_and_signature_index)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (IsSameDexFile(*method->GetDexFile(), dex_file)) {
    return method->GetDexMethodIndex();
  } else {
    return method->FindDexMethodIndexInOtherDexFile(dex_file, name_and_signature_index);
  }
}

static dex::TypeIndex FindClassIndexIn(mirror::Class* cls,
                                       const DexFile& dex_file,
                                       Handle<mirror::DexCache> dex_cache)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  dex::TypeIndex index;
  if (cls->GetDexCache() == nullptr) {
    DCHECK(cls->IsArrayClass()) << cls->PrettyClass();
    index = cls->FindTypeIndexInOtherDexFile(dex_file);
  } else if (!cls->GetDexTypeIndex().IsValid()) {
    DCHECK(cls->IsProxyClass()) << cls->PrettyClass();
    // TODO: deal with proxy classes.
  } else if (IsSameDexFile(cls->GetDexFile(), dex_file)) {
    DCHECK_EQ(cls->GetDexCache(), dex_cache.Get());
    index = cls->GetDexTypeIndex();
    // Update the dex cache to ensure the class is in. The generated code will
    // consider it is. We make it safe by updating the dex cache, as other
    // dex files might also load the class, and there is no guarantee the dex
    // cache of the dex file of the class will be updated.
    if (dex_cache->GetResolvedType(index) == nullptr) {
      dex_cache->SetResolvedType(index, cls);
    }
  } else {
    index = cls->FindTypeIndexInOtherDexFile(dex_file);
    // We cannot guarantee the entry in the dex cache will resolve to the same class,
    // as there may be different class loaders. So only return the index if it's
    // the right class in the dex cache already.
    if (index.IsValid() && dex_cache->GetResolvedType(index) != cls) {
      index = dex::TypeIndex::Invalid();
    }
  }

  return index;
}

class ScopedProfilingInfoInlineUse {
 public:
  explicit ScopedProfilingInfoInlineUse(ArtMethod* method, Thread* self)
      : method_(method),
        self_(self),
        // Fetch the profiling info ahead of using it. If it's null when fetching,
        // we should not call JitCodeCache::DoneInlining.
        profiling_info_(
            Runtime::Current()->GetJit()->GetCodeCache()->NotifyCompilerUse(method, self)) {
  }

  ~ScopedProfilingInfoInlineUse() {
    if (profiling_info_ != nullptr) {
      PointerSize pointer_size = Runtime::Current()->GetClassLinker()->GetImagePointerSize();
      DCHECK_EQ(profiling_info_, method_->GetProfilingInfo(pointer_size));
      Runtime::Current()->GetJit()->GetCodeCache()->DoneCompilerUse(method_, self_);
    }
  }

  ProfilingInfo* GetProfilingInfo() const { return profiling_info_; }

 private:
  ArtMethod* const method_;
  Thread* const self_;
  ProfilingInfo* const profiling_info_;
};

HInliner::InlineCacheType HInliner::GetInlineCacheType(
    const Handle<mirror::ObjectArray<mirror::Class>>& classes)
  REQUIRES_SHARED(Locks::mutator_lock_) {
  uint8_t number_of_types = 0;
  for (; number_of_types < InlineCache::kIndividualCacheSize; ++number_of_types) {
    if (classes->Get(number_of_types) == nullptr) {
      break;
    }
  }

  if (number_of_types == 0) {
    return kInlineCacheUninitialized;
  } else if (number_of_types == 1) {
    return kInlineCacheMonomorphic;
  } else if (number_of_types == InlineCache::kIndividualCacheSize) {
    return kInlineCacheMegamorphic;
  } else {
    return kInlineCachePolymorphic;
  }
}

static mirror::Class* GetMonomorphicType(Handle<mirror::ObjectArray<mirror::Class>> classes)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  DCHECK(classes->Get(0) != nullptr);
  return classes->Get(0);
}

ArtMethod* HInliner::TryCHADevirtualization(ArtMethod* resolved_method) {
  if (!resolved_method->HasSingleImplementation()) {
    return nullptr;
  }
  if (Runtime::Current()->IsAotCompiler()) {
    // No CHA-based devirtulization for AOT compiler (yet).
    return nullptr;
  }
  if (outermost_graph_->IsCompilingOsr()) {
    // We do not support HDeoptimize in OSR methods.
    return nullptr;
  }
  PointerSize pointer_size = caller_compilation_unit_.GetClassLinker()->GetImagePointerSize();
  return resolved_method->GetSingleImplementation(pointer_size);
}

bool HInliner::TryInline(HInvoke* invoke_instruction) {
  if (invoke_instruction->IsInvokeUnresolved() ||
      invoke_instruction->IsInvokePolymorphic()) {
    return false;  // Don't bother to move further if we know the method is unresolved or an
                   // invoke-polymorphic.
  }

  ScopedObjectAccess soa(Thread::Current());
  uint32_t method_index = invoke_instruction->GetDexMethodIndex();
  const DexFile& caller_dex_file = *caller_compilation_unit_.GetDexFile();
  VLOG(compiler) << "Try inlining " << caller_dex_file.PrettyMethod(method_index);

  // We can query the dex cache directly. The verifier has populated it already.
  ArtMethod* resolved_method = invoke_instruction->GetResolvedMethod();
  ArtMethod* actual_method = nullptr;
  if (resolved_method == nullptr) {
    DCHECK(invoke_instruction->IsInvokeStaticOrDirect());
    DCHECK(invoke_instruction->AsInvokeStaticOrDirect()->IsStringInit());
    VLOG(compiler) << "Not inlining a String.<init> method";
    return false;
  } else if (invoke_instruction->IsInvokeStaticOrDirect()) {
    actual_method = resolved_method;
  } else {
    // Check if we can statically find the method.
    actual_method = FindVirtualOrInterfaceTarget(invoke_instruction, resolved_method);
  }

  bool cha_devirtualize = false;
  if (actual_method == nullptr) {
    ArtMethod* method = TryCHADevirtualization(resolved_method);
    if (method != nullptr) {
      cha_devirtualize = true;
      actual_method = method;
    }
  }

  if (actual_method != nullptr) {
    bool result = TryInlineAndReplace(invoke_instruction,
                                      actual_method,
                                      ReferenceTypeInfo::CreateInvalid(),
                                      /* do_rtp */ true,
                                      cha_devirtualize);
    if (result && !invoke_instruction->IsInvokeStaticOrDirect()) {
      if (cha_devirtualize) {
        // Add dependency due to devirtulization. We've assumed resolved_method
        // has single implementation.
        outermost_graph_->AddCHASingleImplementationDependency(resolved_method);
        MaybeRecordStat(kCHAInline);
      } else {
        MaybeRecordStat(kInlinedInvokeVirtualOrInterface);
      }
    }
    return result;
  }
  DCHECK(!invoke_instruction->IsInvokeStaticOrDirect());

  // Try using inline caches.
  return TryInlineFromInlineCache(caller_dex_file, invoke_instruction, resolved_method);
}

static Handle<mirror::ObjectArray<mirror::Class>> AllocateInlineCacheHolder(
    const DexCompilationUnit& compilation_unit,
    StackHandleScope<1>* hs)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  Thread* self = Thread::Current();
  ClassLinker* class_linker = compilation_unit.GetClassLinker();
  Handle<mirror::ObjectArray<mirror::Class>> inline_cache = hs->NewHandle(
      mirror::ObjectArray<mirror::Class>::Alloc(
          self,
          class_linker->GetClassRoot(ClassLinker::kClassArrayClass),
          InlineCache::kIndividualCacheSize));
  if (inline_cache == nullptr) {
    // We got an OOME. Just clear the exception, and don't inline.
    DCHECK(self->IsExceptionPending());
    self->ClearException();
    VLOG(compiler) << "Out of memory in the compiler when trying to inline";
  }
  return inline_cache;
}

bool HInliner::TryInlineFromInlineCache(const DexFile& caller_dex_file,
                                        HInvoke* invoke_instruction,
                                        ArtMethod* resolved_method)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  StackHandleScope<1> hs(Thread::Current());
  Handle<mirror::ObjectArray<mirror::Class>> inline_cache;
  InlineCacheType inline_cache_type = Runtime::Current()->IsAotCompiler()
      ? GetInlineCacheAOT(caller_dex_file, invoke_instruction, &hs, &inline_cache)
      : GetInlineCacheJIT(invoke_instruction, &hs, &inline_cache);

  switch (inline_cache_type) {
    case kInlineCacheNoData:
      break;

    case kInlineCacheUninitialized:
      VLOG(compiler) << "Interface or virtual call to "
                     << caller_dex_file.PrettyMethod(invoke_instruction->GetDexMethodIndex())
                     << " is not hit and not inlined";
      return false;

    case kInlineCacheMonomorphic:
      MaybeRecordStat(kMonomorphicCall);
      if (outermost_graph_->IsCompilingOsr()) {
        // If we are compiling OSR, we pretend this call is polymorphic, as we may come from the
        // interpreter and it may have seen different receiver types.
        return TryInlinePolymorphicCall(invoke_instruction, resolved_method, inline_cache);
      } else {
        return TryInlineMonomorphicCall(invoke_instruction, resolved_method, inline_cache);
      }

    case kInlineCachePolymorphic:
      MaybeRecordStat(kPolymorphicCall);
      return TryInlinePolymorphicCall(invoke_instruction, resolved_method, inline_cache);

    case kInlineCacheMegamorphic:
      VLOG(compiler) << "Interface or virtual call to "
                     << caller_dex_file.PrettyMethod(invoke_instruction->GetDexMethodIndex())
                     << " is megamorphic and not inlined";
      MaybeRecordStat(kMegamorphicCall);
      return false;

    case kInlineCacheMissingTypes:
      VLOG(compiler) << "Interface or virtual call to "
                     << caller_dex_file.PrettyMethod(invoke_instruction->GetDexMethodIndex())
                     << " is missing types and not inlined";
      return false;
  }
  UNREACHABLE();
}

HInliner::InlineCacheType HInliner::GetInlineCacheJIT(
    HInvoke* invoke_instruction,
    StackHandleScope<1>* hs,
    /*out*/Handle<mirror::ObjectArray<mirror::Class>>* inline_cache)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  DCHECK(Runtime::Current()->UseJitCompilation());

  ArtMethod* caller = graph_->GetArtMethod();
  // Under JIT, we should always know the caller.
  DCHECK(caller != nullptr);
  ScopedProfilingInfoInlineUse spiis(caller, Thread::Current());
  ProfilingInfo* profiling_info = spiis.GetProfilingInfo();

  if (profiling_info == nullptr) {
    return kInlineCacheNoData;
  }

  *inline_cache = AllocateInlineCacheHolder(caller_compilation_unit_, hs);
  if (inline_cache->Get() == nullptr) {
    // We can't extract any data if we failed to allocate;
    return kInlineCacheNoData;
  } else {
    Runtime::Current()->GetJit()->GetCodeCache()->CopyInlineCacheInto(
        *profiling_info->GetInlineCache(invoke_instruction->GetDexPc()),
        *inline_cache);
    return GetInlineCacheType(*inline_cache);
  }
}

HInliner::InlineCacheType HInliner::GetInlineCacheAOT(
    const DexFile& caller_dex_file,
    HInvoke* invoke_instruction,
    StackHandleScope<1>* hs,
    /*out*/Handle<mirror::ObjectArray<mirror::Class>>* inline_cache)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  DCHECK(Runtime::Current()->IsAotCompiler());
  const ProfileCompilationInfo* pci = compiler_driver_->GetProfileCompilationInfo();
  if (pci == nullptr) {
    return kInlineCacheNoData;
  }

  ProfileCompilationInfo::OfflineProfileMethodInfo offline_profile;
  bool found = pci->GetMethod(caller_dex_file.GetLocation(),
                              caller_dex_file.GetLocationChecksum(),
                              caller_compilation_unit_.GetDexMethodIndex(),
                              &offline_profile);
  if (!found) {
    return kInlineCacheNoData;  // no profile information for this invocation.
  }

  *inline_cache = AllocateInlineCacheHolder(caller_compilation_unit_, hs);
  if (inline_cache == nullptr) {
    // We can't extract any data if we failed to allocate;
    return kInlineCacheNoData;
  } else {
    return ExtractClassesFromOfflineProfile(invoke_instruction,
                                            offline_profile,
                                            *inline_cache);
  }
}

HInliner::InlineCacheType HInliner::ExtractClassesFromOfflineProfile(
    const HInvoke* invoke_instruction,
    const ProfileCompilationInfo::OfflineProfileMethodInfo& offline_profile,
    /*out*/Handle<mirror::ObjectArray<mirror::Class>> inline_cache)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  const auto it = offline_profile.inline_caches.find(invoke_instruction->GetDexPc());
  if (it == offline_profile.inline_caches.end()) {
    return kInlineCacheUninitialized;
  }

  const ProfileCompilationInfo::DexPcData& dex_pc_data = it->second;

  if (dex_pc_data.is_missing_types) {
    return kInlineCacheMissingTypes;
  }
  if (dex_pc_data.is_megamorphic) {
    return kInlineCacheMegamorphic;
  }

  DCHECK_LE(dex_pc_data.classes.size(), InlineCache::kIndividualCacheSize);
  Thread* self = Thread::Current();
  // We need to resolve the class relative to the containing dex file.
  // So first, build a mapping from the index of dex file in the profile to
  // its dex cache. This will avoid repeating the lookup when walking over
  // the inline cache types.
  std::vector<ObjPtr<mirror::DexCache>> dex_profile_index_to_dex_cache(
        offline_profile.dex_references.size());
  for (size_t i = 0; i < offline_profile.dex_references.size(); i++) {
    bool found = false;
    for (const DexFile* dex_file : compiler_driver_->GetDexFilesForOatFile()) {
      if (offline_profile.dex_references[i].MatchesDex(dex_file)) {
        dex_profile_index_to_dex_cache[i] =
            caller_compilation_unit_.GetClassLinker()->FindDexCache(self, *dex_file);
        found = true;
      }
    }
    if (!found) {
      VLOG(compiler) << "Could not find profiled dex file: "
          << offline_profile.dex_references[i].dex_location;
      return kInlineCacheMissingTypes;
    }
  }

  // Walk over the classes and resolve them. If we cannot find a type we return
  // kInlineCacheMissingTypes.
  int ic_index = 0;
  for (const ProfileCompilationInfo::ClassReference& class_ref : dex_pc_data.classes) {
    ObjPtr<mirror::DexCache> dex_cache =
        dex_profile_index_to_dex_cache[class_ref.dex_profile_index];
    DCHECK(dex_cache != nullptr);
    ObjPtr<mirror::Class> clazz = dex_cache->GetResolvedType(class_ref.type_index);
    if (clazz != nullptr) {
      inline_cache->Set(ic_index++, clazz);
    } else {
      VLOG(compiler) << "Could not resolve class from inline cache in AOT mode "
          << caller_compilation_unit_.GetDexFile()->PrettyMethod(
              invoke_instruction->GetDexMethodIndex()) << " : "
          << caller_compilation_unit_
              .GetDexFile()->StringByTypeIdx(class_ref.type_index);
      return kInlineCacheMissingTypes;
    }
  }
  return GetInlineCacheType(inline_cache);
}

HInstanceFieldGet* HInliner::BuildGetReceiverClass(ClassLinker* class_linker,
                                                   HInstruction* receiver,
                                                   uint32_t dex_pc) const {
  ArtField* field = class_linker->GetClassRoot(ClassLinker::kJavaLangObject)->GetInstanceField(0);
  DCHECK_EQ(std::string(field->GetName()), "shadow$_klass_");
  HInstanceFieldGet* result = new (graph_->GetArena()) HInstanceFieldGet(
      receiver,
      field,
      Primitive::kPrimNot,
      field->GetOffset(),
      field->IsVolatile(),
      field->GetDexFieldIndex(),
      field->GetDeclaringClass()->GetDexClassDefIndex(),
      *field->GetDexFile(),
      dex_pc);
  // The class of a field is effectively final, and does not have any memory dependencies.
  result->SetSideEffects(SideEffects::None());
  return result;
}

bool HInliner::TryInlineMonomorphicCall(HInvoke* invoke_instruction,
                                        ArtMethod* resolved_method,
                                        Handle<mirror::ObjectArray<mirror::Class>> classes) {
  DCHECK(invoke_instruction->IsInvokeVirtual() || invoke_instruction->IsInvokeInterface())
      << invoke_instruction->DebugName();

  const DexFile& caller_dex_file = *caller_compilation_unit_.GetDexFile();
  dex::TypeIndex class_index = FindClassIndexIn(
      GetMonomorphicType(classes), caller_dex_file, caller_compilation_unit_.GetDexCache());
  if (!class_index.IsValid()) {
    VLOG(compiler) << "Call to " << ArtMethod::PrettyMethod(resolved_method)
                   << " from inline cache is not inlined because its class is not"
                   << " accessible to the caller";
    return false;
  }

  ClassLinker* class_linker = caller_compilation_unit_.GetClassLinker();
  PointerSize pointer_size = class_linker->GetImagePointerSize();
  if (invoke_instruction->IsInvokeInterface()) {
    resolved_method = GetMonomorphicType(classes)->FindVirtualMethodForInterface(
        resolved_method, pointer_size);
  } else {
    DCHECK(invoke_instruction->IsInvokeVirtual());
    resolved_method = GetMonomorphicType(classes)->FindVirtualMethodForVirtual(
        resolved_method, pointer_size);
  }
  DCHECK(resolved_method != nullptr);
  HInstruction* receiver = invoke_instruction->InputAt(0);
  HInstruction* cursor = invoke_instruction->GetPrevious();
  HBasicBlock* bb_cursor = invoke_instruction->GetBlock();
  Handle<mirror::Class> monomorphic_type = handles_->NewHandle(GetMonomorphicType(classes));
  if (!TryInlineAndReplace(invoke_instruction,
                           resolved_method,
                           ReferenceTypeInfo::Create(monomorphic_type, /* is_exact */ true),
                           /* do_rtp */ false,
                           /* cha_devirtualize */ false)) {
    return false;
  }

  // We successfully inlined, now add a guard.
  AddTypeGuard(receiver,
               cursor,
               bb_cursor,
               class_index,
               monomorphic_type,
               invoke_instruction,
               /* with_deoptimization */ true);

  // Run type propagation to get the guard typed, and eventually propagate the
  // type of the receiver.
  ReferenceTypePropagation rtp_fixup(graph_,
                                     outer_compilation_unit_.GetDexCache(),
                                     handles_,
                                     /* is_first_run */ false);
  rtp_fixup.Run();

  MaybeRecordStat(kInlinedMonomorphicCall);
  return true;
}

void HInliner::AddCHAGuard(HInstruction* invoke_instruction,
                           uint32_t dex_pc,
                           HInstruction* cursor,
                           HBasicBlock* bb_cursor) {
  HShouldDeoptimizeFlag* deopt_flag = new (graph_->GetArena())
      HShouldDeoptimizeFlag(graph_->GetArena(), dex_pc);
  HInstruction* compare = new (graph_->GetArena()) HNotEqual(
      deopt_flag, graph_->GetIntConstant(0, dex_pc));
  HInstruction* deopt = new (graph_->GetArena()) HDeoptimize(compare, dex_pc);

  if (cursor != nullptr) {
    bb_cursor->InsertInstructionAfter(deopt_flag, cursor);
  } else {
    bb_cursor->InsertInstructionBefore(deopt_flag, bb_cursor->GetFirstInstruction());
  }
  bb_cursor->InsertInstructionAfter(compare, deopt_flag);
  bb_cursor->InsertInstructionAfter(deopt, compare);

  // Add receiver as input to aid CHA guard optimization later.
  deopt_flag->AddInput(invoke_instruction->InputAt(0));
  DCHECK_EQ(deopt_flag->InputCount(), 1u);
  deopt->CopyEnvironmentFrom(invoke_instruction->GetEnvironment());
  outermost_graph_->IncrementNumberOfCHAGuards();
}

HInstruction* HInliner::AddTypeGuard(HInstruction* receiver,
                                     HInstruction* cursor,
                                     HBasicBlock* bb_cursor,
                                     dex::TypeIndex class_index,
                                     Handle<mirror::Class> klass,
                                     HInstruction* invoke_instruction,
                                     bool with_deoptimization) {
  ClassLinker* class_linker = caller_compilation_unit_.GetClassLinker();
  HInstanceFieldGet* receiver_class = BuildGetReceiverClass(
      class_linker, receiver, invoke_instruction->GetDexPc());
  if (cursor != nullptr) {
    bb_cursor->InsertInstructionAfter(receiver_class, cursor);
  } else {
    bb_cursor->InsertInstructionBefore(receiver_class, bb_cursor->GetFirstInstruction());
  }

  const DexFile& caller_dex_file = *caller_compilation_unit_.GetDexFile();
  bool is_referrer = (klass.Get() == outermost_graph_->GetArtMethod()->GetDeclaringClass());
  // Note that we will just compare the classes, so we don't need Java semantics access checks.
  // Note that the type index and the dex file are relative to the method this type guard is
  // inlined into.
  HLoadClass* load_class = new (graph_->GetArena()) HLoadClass(graph_->GetCurrentMethod(),
                                                               class_index,
                                                               caller_dex_file,
                                                               klass,
                                                               is_referrer,
                                                               invoke_instruction->GetDexPc(),
                                                               /* needs_access_check */ false);
  HLoadClass::LoadKind kind = HSharpening::ComputeLoadClassKind(
      load_class, codegen_, compiler_driver_, caller_compilation_unit_);
  DCHECK(kind != HLoadClass::LoadKind::kInvalid)
      << "We should always be able to reference a class for inline caches";
  // Insert before setting the kind, as setting the kind affects the inputs.
  bb_cursor->InsertInstructionAfter(load_class, receiver_class);
  load_class->SetLoadKind(kind);
  // In AOT mode, we will most likely load the class from BSS, which will involve a call
  // to the runtime. In this case, the load instruction will need an environment so copy
  // it from the invoke instruction.
  if (load_class->NeedsEnvironment()) {
    DCHECK(Runtime::Current()->IsAotCompiler());
    load_class->CopyEnvironmentFrom(invoke_instruction->GetEnvironment());
  }

  HNotEqual* compare = new (graph_->GetArena()) HNotEqual(load_class, receiver_class);
  bb_cursor->InsertInstructionAfter(compare, load_class);
  if (with_deoptimization) {
    HDeoptimize* deoptimize = new (graph_->GetArena()) HDeoptimize(
        compare, invoke_instruction->GetDexPc());
    bb_cursor->InsertInstructionAfter(deoptimize, compare);
    deoptimize->CopyEnvironmentFrom(invoke_instruction->GetEnvironment());
  }
  return compare;
}

bool HInliner::TryInlinePolymorphicCall(HInvoke* invoke_instruction,
                                        ArtMethod* resolved_method,
                                        Handle<mirror::ObjectArray<mirror::Class>> classes) {
  DCHECK(invoke_instruction->IsInvokeVirtual() || invoke_instruction->IsInvokeInterface())
      << invoke_instruction->DebugName();

  if (TryInlinePolymorphicCallToSameTarget(invoke_instruction, resolved_method, classes)) {
    return true;
  }

  ClassLinker* class_linker = caller_compilation_unit_.GetClassLinker();
  PointerSize pointer_size = class_linker->GetImagePointerSize();
  const DexFile& caller_dex_file = *caller_compilation_unit_.GetDexFile();

  bool all_targets_inlined = true;
  bool one_target_inlined = false;
  for (size_t i = 0; i < InlineCache::kIndividualCacheSize; ++i) {
    if (classes->Get(i) == nullptr) {
      break;
    }
    ArtMethod* method = nullptr;

    Handle<mirror::Class> handle = handles_->NewHandle(classes->Get(i));
    if (invoke_instruction->IsInvokeInterface()) {
      method = handle->FindVirtualMethodForInterface(resolved_method, pointer_size);
    } else {
      DCHECK(invoke_instruction->IsInvokeVirtual());
      method = handle->FindVirtualMethodForVirtual(resolved_method, pointer_size);
    }

    HInstruction* receiver = invoke_instruction->InputAt(0);
    HInstruction* cursor = invoke_instruction->GetPrevious();
    HBasicBlock* bb_cursor = invoke_instruction->GetBlock();

    dex::TypeIndex class_index = FindClassIndexIn(
        handle.Get(), caller_dex_file, caller_compilation_unit_.GetDexCache());
    HInstruction* return_replacement = nullptr;
    if (!class_index.IsValid() ||
        !TryBuildAndInline(invoke_instruction,
                           method,
                           ReferenceTypeInfo::Create(handle, /* is_exact */ true),
                           &return_replacement)) {
      all_targets_inlined = false;
    } else {
      one_target_inlined = true;

      VLOG(compiler) << "Polymorphic call to " << ArtMethod::PrettyMethod(resolved_method)
                     << " has inlined " << ArtMethod::PrettyMethod(method);

      // If we have inlined all targets before, and this receiver is the last seen,
      // we deoptimize instead of keeping the original invoke instruction.
      bool deoptimize = all_targets_inlined &&
          (i != InlineCache::kIndividualCacheSize - 1) &&
          (classes->Get(i + 1) == nullptr);

      if (outermost_graph_->IsCompilingOsr()) {
        // We do not support HDeoptimize in OSR methods.
        deoptimize = false;
      }
      HInstruction* compare = AddTypeGuard(receiver,
                                           cursor,
                                           bb_cursor,
                                           class_index,
                                           handle,
                                           invoke_instruction,
                                           deoptimize);
      if (deoptimize) {
        if (return_replacement != nullptr) {
          invoke_instruction->ReplaceWith(return_replacement);
        }
        invoke_instruction->GetBlock()->RemoveInstruction(invoke_instruction);
        // Because the inline cache data can be populated concurrently, we force the end of the
        // iteration. Otherhwise, we could see a new receiver type.
        break;
      } else {
        CreateDiamondPatternForPolymorphicInline(compare, return_replacement, invoke_instruction);
      }
    }
  }

  if (!one_target_inlined) {
    VLOG(compiler) << "Call to " << ArtMethod::PrettyMethod(resolved_method)
                   << " from inline cache is not inlined because none"
                   << " of its targets could be inlined";
    return false;
  }

  MaybeRecordStat(kInlinedPolymorphicCall);

  // Run type propagation to get the guards typed.
  ReferenceTypePropagation rtp_fixup(graph_,
                                     outer_compilation_unit_.GetDexCache(),
                                     handles_,
                                     /* is_first_run */ false);
  rtp_fixup.Run();
  return true;
}

void HInliner::CreateDiamondPatternForPolymorphicInline(HInstruction* compare,
                                                        HInstruction* return_replacement,
                                                        HInstruction* invoke_instruction) {
  uint32_t dex_pc = invoke_instruction->GetDexPc();
  HBasicBlock* cursor_block = compare->GetBlock();
  HBasicBlock* original_invoke_block = invoke_instruction->GetBlock();
  ArenaAllocator* allocator = graph_->GetArena();

  // Spit the block after the compare: `cursor_block` will now be the start of the diamond,
  // and the returned block is the start of the then branch (that could contain multiple blocks).
  HBasicBlock* then = cursor_block->SplitAfterForInlining(compare);

  // Split the block containing the invoke before and after the invoke. The returned block
  // of the split before will contain the invoke and will be the otherwise branch of
  // the diamond. The returned block of the split after will be the merge block
  // of the diamond.
  HBasicBlock* end_then = invoke_instruction->GetBlock();
  HBasicBlock* otherwise = end_then->SplitBeforeForInlining(invoke_instruction);
  HBasicBlock* merge = otherwise->SplitAfterForInlining(invoke_instruction);

  // If the methods we are inlining return a value, we create a phi in the merge block
  // that will have the `invoke_instruction and the `return_replacement` as inputs.
  if (return_replacement != nullptr) {
    HPhi* phi = new (allocator) HPhi(
        allocator, kNoRegNumber, 0, HPhi::ToPhiType(invoke_instruction->GetType()), dex_pc);
    merge->AddPhi(phi);
    invoke_instruction->ReplaceWith(phi);
    phi->AddInput(return_replacement);
    phi->AddInput(invoke_instruction);
  }

  // Add the control flow instructions.
  otherwise->AddInstruction(new (allocator) HGoto(dex_pc));
  end_then->AddInstruction(new (allocator) HGoto(dex_pc));
  cursor_block->AddInstruction(new (allocator) HIf(compare, dex_pc));

  // Add the newly created blocks to the graph.
  graph_->AddBlock(then);
  graph_->AddBlock(otherwise);
  graph_->AddBlock(merge);

  // Set up successor (and implictly predecessor) relations.
  cursor_block->AddSuccessor(otherwise);
  cursor_block->AddSuccessor(then);
  end_then->AddSuccessor(merge);
  otherwise->AddSuccessor(merge);

  // Set up dominance information.
  then->SetDominator(cursor_block);
  cursor_block->AddDominatedBlock(then);
  otherwise->SetDominator(cursor_block);
  cursor_block->AddDominatedBlock(otherwise);
  merge->SetDominator(cursor_block);
  cursor_block->AddDominatedBlock(merge);

  // Update the revert post order.
  size_t index = IndexOfElement(graph_->reverse_post_order_, cursor_block);
  MakeRoomFor(&graph_->reverse_post_order_, 1, index);
  graph_->reverse_post_order_[++index] = then;
  index = IndexOfElement(graph_->reverse_post_order_, end_then);
  MakeRoomFor(&graph_->reverse_post_order_, 2, index);
  graph_->reverse_post_order_[++index] = otherwise;
  graph_->reverse_post_order_[++index] = merge;


  graph_->UpdateLoopAndTryInformationOfNewBlock(
      then, original_invoke_block, /* replace_if_back_edge */ false);
  graph_->UpdateLoopAndTryInformationOfNewBlock(
      otherwise, original_invoke_block, /* replace_if_back_edge */ false);

  // In case the original invoke location was a back edge, we need to update
  // the loop to now have the merge block as a back edge.
  graph_->UpdateLoopAndTryInformationOfNewBlock(
      merge, original_invoke_block, /* replace_if_back_edge */ true);
}

bool HInliner::TryInlinePolymorphicCallToSameTarget(
    HInvoke* invoke_instruction,
    ArtMethod* resolved_method,
    Handle<mirror::ObjectArray<mirror::Class>> classes) {
  // This optimization only works under JIT for now.
  if (!Runtime::Current()->UseJitCompilation()) {
    return false;
  }

  if (graph_->GetInstructionSet() == kMips64) {
    // TODO: Support HClassTableGet for mips64.
    return false;
  }
  ClassLinker* class_linker = caller_compilation_unit_.GetClassLinker();
  PointerSize pointer_size = class_linker->GetImagePointerSize();

  DCHECK(resolved_method != nullptr);
  ArtMethod* actual_method = nullptr;
  size_t method_index = invoke_instruction->IsInvokeVirtual()
      ? invoke_instruction->AsInvokeVirtual()->GetVTableIndex()
      : invoke_instruction->AsInvokeInterface()->GetImtIndex();

  // Check whether we are actually calling the same method among
  // the different types seen.
  for (size_t i = 0; i < InlineCache::kIndividualCacheSize; ++i) {
    if (classes->Get(i) == nullptr) {
      break;
    }
    ArtMethod* new_method = nullptr;
    if (invoke_instruction->IsInvokeInterface()) {
      new_method = classes->Get(i)->GetImt(pointer_size)->Get(
          method_index, pointer_size);
      if (new_method->IsRuntimeMethod()) {
        // Bail out as soon as we see a conflict trampoline in one of the target's
        // interface table.
        return false;
      }
    } else {
      DCHECK(invoke_instruction->IsInvokeVirtual());
      new_method = classes->Get(i)->GetEmbeddedVTableEntry(method_index, pointer_size);
    }
    DCHECK(new_method != nullptr);
    if (actual_method == nullptr) {
      actual_method = new_method;
    } else if (actual_method != new_method) {
      // Different methods, bailout.
      VLOG(compiler) << "Call to " << ArtMethod::PrettyMethod(resolved_method)
                     << " from inline cache is not inlined because it resolves"
                     << " to different methods";
      return false;
    }
  }

  HInstruction* receiver = invoke_instruction->InputAt(0);
  HInstruction* cursor = invoke_instruction->GetPrevious();
  HBasicBlock* bb_cursor = invoke_instruction->GetBlock();

  HInstruction* return_replacement = nullptr;
  if (!TryBuildAndInline(invoke_instruction,
                         actual_method,
                         ReferenceTypeInfo::CreateInvalid(),
                         &return_replacement)) {
    return false;
  }

  // We successfully inlined, now add a guard.
  HInstanceFieldGet* receiver_class = BuildGetReceiverClass(
      class_linker, receiver, invoke_instruction->GetDexPc());

  Primitive::Type type = Is64BitInstructionSet(graph_->GetInstructionSet())
      ? Primitive::kPrimLong
      : Primitive::kPrimInt;
  HClassTableGet* class_table_get = new (graph_->GetArena()) HClassTableGet(
      receiver_class,
      type,
      invoke_instruction->IsInvokeVirtual() ? HClassTableGet::TableKind::kVTable
                                            : HClassTableGet::TableKind::kIMTable,
      method_index,
      invoke_instruction->GetDexPc());

  HConstant* constant;
  if (type == Primitive::kPrimLong) {
    constant = graph_->GetLongConstant(
        reinterpret_cast<intptr_t>(actual_method), invoke_instruction->GetDexPc());
  } else {
    constant = graph_->GetIntConstant(
        reinterpret_cast<intptr_t>(actual_method), invoke_instruction->GetDexPc());
  }

  HNotEqual* compare = new (graph_->GetArena()) HNotEqual(class_table_get, constant);
  if (cursor != nullptr) {
    bb_cursor->InsertInstructionAfter(receiver_class, cursor);
  } else {
    bb_cursor->InsertInstructionBefore(receiver_class, bb_cursor->GetFirstInstruction());
  }
  bb_cursor->InsertInstructionAfter(class_table_get, receiver_class);
  bb_cursor->InsertInstructionAfter(compare, class_table_get);

  if (outermost_graph_->IsCompilingOsr()) {
    CreateDiamondPatternForPolymorphicInline(compare, return_replacement, invoke_instruction);
  } else {
    HDeoptimize* deoptimize = new (graph_->GetArena()) HDeoptimize(
        compare, invoke_instruction->GetDexPc());
    bb_cursor->InsertInstructionAfter(deoptimize, compare);
    deoptimize->CopyEnvironmentFrom(invoke_instruction->GetEnvironment());
    if (return_replacement != nullptr) {
      invoke_instruction->ReplaceWith(return_replacement);
    }
    invoke_instruction->GetBlock()->RemoveInstruction(invoke_instruction);
  }

  // Run type propagation to get the guard typed.
  ReferenceTypePropagation rtp_fixup(graph_,
                                     outer_compilation_unit_.GetDexCache(),
                                     handles_,
                                     /* is_first_run */ false);
  rtp_fixup.Run();

  MaybeRecordStat(kInlinedPolymorphicCall);

  return true;
}

bool HInliner::TryInlineAndReplace(HInvoke* invoke_instruction,
                                   ArtMethod* method,
                                   ReferenceTypeInfo receiver_type,
                                   bool do_rtp,
                                   bool cha_devirtualize) {
  HInstruction* return_replacement = nullptr;
  uint32_t dex_pc = invoke_instruction->GetDexPc();
  HInstruction* cursor = invoke_instruction->GetPrevious();
  HBasicBlock* bb_cursor = invoke_instruction->GetBlock();
  if (!TryBuildAndInline(invoke_instruction, method, receiver_type, &return_replacement)) {
    if (invoke_instruction->IsInvokeInterface()) {
      // Turn an invoke-interface into an invoke-virtual. An invoke-virtual is always
      // better than an invoke-interface because:
      // 1) In the best case, the interface call has one more indirection (to fetch the IMT).
      // 2) We will not go to the conflict trampoline with an invoke-virtual.
      // TODO: Consider sharpening once it is not dependent on the compiler driver.
      const DexFile& caller_dex_file = *caller_compilation_unit_.GetDexFile();
      uint32_t dex_method_index = FindMethodIndexIn(
          method, caller_dex_file, invoke_instruction->GetDexMethodIndex());
      if (dex_method_index == DexFile::kDexNoIndex) {
        return false;
      }
      HInvokeVirtual* new_invoke = new (graph_->GetArena()) HInvokeVirtual(
          graph_->GetArena(),
          invoke_instruction->GetNumberOfArguments(),
          invoke_instruction->GetType(),
          invoke_instruction->GetDexPc(),
          dex_method_index,
          method,
          method->GetMethodIndex());
      HInputsRef inputs = invoke_instruction->GetInputs();
      for (size_t index = 0; index != inputs.size(); ++index) {
        new_invoke->SetArgumentAt(index, inputs[index]);
      }
      invoke_instruction->GetBlock()->InsertInstructionBefore(new_invoke, invoke_instruction);
      new_invoke->CopyEnvironmentFrom(invoke_instruction->GetEnvironment());
      if (invoke_instruction->GetType() == Primitive::kPrimNot) {
        new_invoke->SetReferenceTypeInfo(invoke_instruction->GetReferenceTypeInfo());
      }
      return_replacement = new_invoke;
    } else {
      // TODO: Consider sharpening an invoke virtual once it is not dependent on the
      // compiler driver.
      return false;
    }
  }
  if (cha_devirtualize) {
    AddCHAGuard(invoke_instruction, dex_pc, cursor, bb_cursor);
  }
  if (return_replacement != nullptr) {
    invoke_instruction->ReplaceWith(return_replacement);
  }
  invoke_instruction->GetBlock()->RemoveInstruction(invoke_instruction);
  FixUpReturnReferenceType(method, return_replacement);
  if (do_rtp && ReturnTypeMoreSpecific(invoke_instruction, return_replacement)) {
    // Actual return value has a more specific type than the method's declared
    // return type. Run RTP again on the outer graph to propagate it.
    ReferenceTypePropagation(graph_,
                             outer_compilation_unit_.GetDexCache(),
                             handles_,
                             /* is_first_run */ false).Run();
  }
  return true;
}

bool HInliner::TryBuildAndInline(HInvoke* invoke_instruction,
                                 ArtMethod* method,
                                 ReferenceTypeInfo receiver_type,
                                 HInstruction** return_replacement) {
  if (method->IsProxyMethod()) {
    VLOG(compiler) << "Method " << method->PrettyMethod()
                   << " is not inlined because of unimplemented inline support for proxy methods.";
    return false;
  }

  // Check whether we're allowed to inline. The outermost compilation unit is the relevant
  // dex file here (though the transitivity of an inline chain would allow checking the calller).
  if (!compiler_driver_->MayInline(method->GetDexFile(),
                                   outer_compilation_unit_.GetDexFile())) {
    if (TryPatternSubstitution(invoke_instruction, method, return_replacement)) {
      VLOG(compiler) << "Successfully replaced pattern of invoke "
                     << method->PrettyMethod();
      MaybeRecordStat(kReplacedInvokeWithSimplePattern);
      return true;
    }
    VLOG(compiler) << "Won't inline " << method->PrettyMethod() << " in "
                   << outer_compilation_unit_.GetDexFile()->GetLocation() << " ("
                   << caller_compilation_unit_.GetDexFile()->GetLocation() << ") from "
                   << method->GetDexFile()->GetLocation();
    return false;
  }

  bool same_dex_file = IsSameDexFile(*outer_compilation_unit_.GetDexFile(), *method->GetDexFile());

  const DexFile::CodeItem* code_item = method->GetCodeItem();

  if (code_item == nullptr) {
    VLOG(compiler) << "Method " << method->PrettyMethod()
                   << " is not inlined because it is native";
    return false;
  }

  size_t inline_max_code_units = compiler_driver_->GetCompilerOptions().GetInlineMaxCodeUnits();
  if (code_item->insns_size_in_code_units_ > inline_max_code_units) {
    VLOG(compiler) << "Method " << method->PrettyMethod()
                   << " is too big to inline: "
                   << code_item->insns_size_in_code_units_
                   << " > "
                   << inline_max_code_units;
    return false;
  }

  if (code_item->tries_size_ != 0) {
    VLOG(compiler) << "Method " << method->PrettyMethod()
                   << " is not inlined because of try block";
    return false;
  }

  if (!method->IsCompilable()) {
    VLOG(compiler) << "Method " << method->PrettyMethod()
                   << " has soft failures un-handled by the compiler, so it cannot be inlined";
  }

  if (!method->GetDeclaringClass()->IsVerified()) {
    uint16_t class_def_idx = method->GetDeclaringClass()->GetDexClassDefIndex();
    if (Runtime::Current()->UseJitCompilation() ||
        !compiler_driver_->IsMethodVerifiedWithoutFailures(
            method->GetDexMethodIndex(), class_def_idx, *method->GetDexFile())) {
      VLOG(compiler) << "Method " << method->PrettyMethod()
                     << " couldn't be verified, so it cannot be inlined";
      return false;
    }
  }

  if (invoke_instruction->IsInvokeStaticOrDirect() &&
      invoke_instruction->AsInvokeStaticOrDirect()->IsStaticWithImplicitClinitCheck()) {
    // Case of a static method that cannot be inlined because it implicitly
    // requires an initialization check of its declaring class.
    VLOG(compiler) << "Method " << method->PrettyMethod()
                   << " is not inlined because it is static and requires a clinit"
                   << " check that cannot be emitted due to Dex cache limitations";
    return false;
  }

  if (!TryBuildAndInlineHelper(
          invoke_instruction, method, receiver_type, same_dex_file, return_replacement)) {
    return false;
  }

  VLOG(compiler) << "Successfully inlined " << method->PrettyMethod();
  MaybeRecordStat(kInlinedInvoke);
  return true;
}

static HInstruction* GetInvokeInputForArgVRegIndex(HInvoke* invoke_instruction,
                                                   size_t arg_vreg_index)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  size_t input_index = 0;
  for (size_t i = 0; i < arg_vreg_index; ++i, ++input_index) {
    DCHECK_LT(input_index, invoke_instruction->GetNumberOfArguments());
    if (Primitive::Is64BitType(invoke_instruction->InputAt(input_index)->GetType())) {
      ++i;
      DCHECK_NE(i, arg_vreg_index);
    }
  }
  DCHECK_LT(input_index, invoke_instruction->GetNumberOfArguments());
  return invoke_instruction->InputAt(input_index);
}

// Try to recognize known simple patterns and replace invoke call with appropriate instructions.
bool HInliner::TryPatternSubstitution(HInvoke* invoke_instruction,
                                      ArtMethod* resolved_method,
                                      HInstruction** return_replacement) {
  InlineMethod inline_method;
  if (!InlineMethodAnalyser::AnalyseMethodCode(resolved_method, &inline_method)) {
    return false;
  }

  switch (inline_method.opcode) {
    case kInlineOpNop:
      DCHECK_EQ(invoke_instruction->GetType(), Primitive::kPrimVoid);
      *return_replacement = nullptr;
      break;
    case kInlineOpReturnArg:
      *return_replacement = GetInvokeInputForArgVRegIndex(invoke_instruction,
                                                          inline_method.d.return_data.arg);
      break;
    case kInlineOpNonWideConst:
      if (resolved_method->GetShorty()[0] == 'L') {
        DCHECK_EQ(inline_method.d.data, 0u);
        *return_replacement = graph_->GetNullConstant();
      } else {
        *return_replacement = graph_->GetIntConstant(static_cast<int32_t>(inline_method.d.data));
      }
      break;
    case kInlineOpIGet: {
      const InlineIGetIPutData& data = inline_method.d.ifield_data;
      if (data.method_is_static || data.object_arg != 0u) {
        // TODO: Needs null check.
        return false;
      }
      Handle<mirror::DexCache> dex_cache(handles_->NewHandle(resolved_method->GetDexCache()));
      HInstruction* obj = GetInvokeInputForArgVRegIndex(invoke_instruction, data.object_arg);
      HInstanceFieldGet* iget = CreateInstanceFieldGet(dex_cache, data.field_idx, obj);
      DCHECK_EQ(iget->GetFieldOffset().Uint32Value(), data.field_offset);
      DCHECK_EQ(iget->IsVolatile() ? 1u : 0u, data.is_volatile);
      invoke_instruction->GetBlock()->InsertInstructionBefore(iget, invoke_instruction);
      *return_replacement = iget;
      break;
    }
    case kInlineOpIPut: {
      const InlineIGetIPutData& data = inline_method.d.ifield_data;
      if (data.method_is_static || data.object_arg != 0u) {
        // TODO: Needs null check.
        return false;
      }
      Handle<mirror::DexCache> dex_cache(handles_->NewHandle(resolved_method->GetDexCache()));
      HInstruction* obj = GetInvokeInputForArgVRegIndex(invoke_instruction, data.object_arg);
      HInstruction* value = GetInvokeInputForArgVRegIndex(invoke_instruction, data.src_arg);
      HInstanceFieldSet* iput = CreateInstanceFieldSet(dex_cache, data.field_idx, obj, value);
      DCHECK_EQ(iput->GetFieldOffset().Uint32Value(), data.field_offset);
      DCHECK_EQ(iput->IsVolatile() ? 1u : 0u, data.is_volatile);
      invoke_instruction->GetBlock()->InsertInstructionBefore(iput, invoke_instruction);
      if (data.return_arg_plus1 != 0u) {
        size_t return_arg = data.return_arg_plus1 - 1u;
        *return_replacement = GetInvokeInputForArgVRegIndex(invoke_instruction, return_arg);
      }
      break;
    }
    case kInlineOpConstructor: {
      const InlineConstructorData& data = inline_method.d.constructor_data;
      // Get the indexes to arrays for easier processing.
      uint16_t iput_field_indexes[] = {
          data.iput0_field_index, data.iput1_field_index, data.iput2_field_index
      };
      uint16_t iput_args[] = { data.iput0_arg, data.iput1_arg, data.iput2_arg };
      static_assert(arraysize(iput_args) == arraysize(iput_field_indexes), "Size mismatch");
      // Count valid field indexes.
      size_t number_of_iputs = 0u;
      while (number_of_iputs != arraysize(iput_field_indexes) &&
          iput_field_indexes[number_of_iputs] != DexFile::kDexNoIndex16) {
        // Check that there are no duplicate valid field indexes.
        DCHECK_EQ(0, std::count(iput_field_indexes + number_of_iputs + 1,
                                iput_field_indexes + arraysize(iput_field_indexes),
                                iput_field_indexes[number_of_iputs]));
        ++number_of_iputs;
      }
      // Check that there are no valid field indexes in the rest of the array.
      DCHECK_EQ(0, std::count_if(iput_field_indexes + number_of_iputs,
                                 iput_field_indexes + arraysize(iput_field_indexes),
                                 [](uint16_t index) { return index != DexFile::kDexNoIndex16; }));

      // Create HInstanceFieldSet for each IPUT that stores non-zero data.
      Handle<mirror::DexCache> dex_cache;
      HInstruction* obj = GetInvokeInputForArgVRegIndex(invoke_instruction, /* this */ 0u);
      bool needs_constructor_barrier = false;
      for (size_t i = 0; i != number_of_iputs; ++i) {
        HInstruction* value = GetInvokeInputForArgVRegIndex(invoke_instruction, iput_args[i]);
        if (!value->IsConstant() || !value->AsConstant()->IsZeroBitPattern()) {
          if (dex_cache.GetReference() == nullptr) {
            dex_cache = handles_->NewHandle(resolved_method->GetDexCache());
          }
          uint16_t field_index = iput_field_indexes[i];
          HInstanceFieldSet* iput = CreateInstanceFieldSet(dex_cache, field_index, obj, value);
          invoke_instruction->GetBlock()->InsertInstructionBefore(iput, invoke_instruction);

          // Check whether the field is final. If it is, we need to add a barrier.
          PointerSize pointer_size = InstructionSetPointerSize(codegen_->GetInstructionSet());
          ArtField* resolved_field = dex_cache->GetResolvedField(field_index, pointer_size);
          DCHECK(resolved_field != nullptr);
          if (resolved_field->IsFinal()) {
            needs_constructor_barrier = true;
          }
        }
      }
      if (needs_constructor_barrier) {
        HMemoryBarrier* barrier = new (graph_->GetArena()) HMemoryBarrier(kStoreStore, kNoDexPc);
        invoke_instruction->GetBlock()->InsertInstructionBefore(barrier, invoke_instruction);
      }
      *return_replacement = nullptr;
      break;
    }
    default:
      LOG(FATAL) << "UNREACHABLE";
      UNREACHABLE();
  }
  return true;
}

HInstanceFieldGet* HInliner::CreateInstanceFieldGet(Handle<mirror::DexCache> dex_cache,
                                                    uint32_t field_index,
                                                    HInstruction* obj)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  PointerSize pointer_size = InstructionSetPointerSize(codegen_->GetInstructionSet());
  ArtField* resolved_field = dex_cache->GetResolvedField(field_index, pointer_size);
  DCHECK(resolved_field != nullptr);
  HInstanceFieldGet* iget = new (graph_->GetArena()) HInstanceFieldGet(
      obj,
      resolved_field,
      resolved_field->GetTypeAsPrimitiveType(),
      resolved_field->GetOffset(),
      resolved_field->IsVolatile(),
      field_index,
      resolved_field->GetDeclaringClass()->GetDexClassDefIndex(),
      *dex_cache->GetDexFile(),
      // Read barrier generates a runtime call in slow path and we need a valid
      // dex pc for the associated stack map. 0 is bogus but valid. Bug: 26854537.
      /* dex_pc */ 0);
  if (iget->GetType() == Primitive::kPrimNot) {
    // Use the same dex_cache that we used for field lookup as the hint_dex_cache.
    ReferenceTypePropagation rtp(graph_, dex_cache, handles_, /* is_first_run */ false);
    rtp.Visit(iget);
  }
  return iget;
}

HInstanceFieldSet* HInliner::CreateInstanceFieldSet(Handle<mirror::DexCache> dex_cache,
                                                    uint32_t field_index,
                                                    HInstruction* obj,
                                                    HInstruction* value)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  PointerSize pointer_size = InstructionSetPointerSize(codegen_->GetInstructionSet());
  ArtField* resolved_field = dex_cache->GetResolvedField(field_index, pointer_size);
  DCHECK(resolved_field != nullptr);
  HInstanceFieldSet* iput = new (graph_->GetArena()) HInstanceFieldSet(
      obj,
      value,
      resolved_field,
      resolved_field->GetTypeAsPrimitiveType(),
      resolved_field->GetOffset(),
      resolved_field->IsVolatile(),
      field_index,
      resolved_field->GetDeclaringClass()->GetDexClassDefIndex(),
      *dex_cache->GetDexFile(),
      // Read barrier generates a runtime call in slow path and we need a valid
      // dex pc for the associated stack map. 0 is bogus but valid. Bug: 26854537.
      /* dex_pc */ 0);
  return iput;
}

bool HInliner::TryBuildAndInlineHelper(HInvoke* invoke_instruction,
                                       ArtMethod* resolved_method,
                                       ReferenceTypeInfo receiver_type,
                                       bool same_dex_file,
                                       HInstruction** return_replacement) {
  DCHECK(!(resolved_method->IsStatic() && receiver_type.IsValid()));
  ScopedObjectAccess soa(Thread::Current());
  const DexFile::CodeItem* code_item = resolved_method->GetCodeItem();
  const DexFile& callee_dex_file = *resolved_method->GetDexFile();
  uint32_t method_index = resolved_method->GetDexMethodIndex();
  ClassLinker* class_linker = caller_compilation_unit_.GetClassLinker();
  Handle<mirror::DexCache> dex_cache(handles_->NewHandle(resolved_method->GetDexCache()));
  Handle<mirror::ClassLoader> class_loader(handles_->NewHandle(
      resolved_method->GetDeclaringClass()->GetClassLoader()));

  DexCompilationUnit dex_compilation_unit(
      class_loader.ToJObject(),
      class_linker,
      callee_dex_file,
      code_item,
      resolved_method->GetDeclaringClass()->GetDexClassDefIndex(),
      method_index,
      resolved_method->GetAccessFlags(),
      /* verified_method */ nullptr,
      dex_cache);

  bool requires_ctor_barrier = false;

  if (dex_compilation_unit.IsConstructor()) {
    // If it's a super invocation and we already generate a barrier there's no need
    // to generate another one.
    // We identify super calls by looking at the "this" pointer. If its value is the
    // same as the local "this" pointer then we must have a super invocation.
    bool is_super_invocation = invoke_instruction->InputAt(0)->IsParameterValue()
        && invoke_instruction->InputAt(0)->AsParameterValue()->IsThis();
    if (is_super_invocation && graph_->ShouldGenerateConstructorBarrier()) {
      requires_ctor_barrier = false;
    } else {
      Thread* self = Thread::Current();
      requires_ctor_barrier = compiler_driver_->RequiresConstructorBarrier(self,
          dex_compilation_unit.GetDexFile(),
          dex_compilation_unit.GetClassDefIndex());
    }
  }

  InvokeType invoke_type = invoke_instruction->GetInvokeType();
  if (invoke_type == kInterface) {
    // We have statically resolved the dispatch. To please the class linker
    // at runtime, we change this call as if it was a virtual call.
    invoke_type = kVirtual;
  }

  const int32_t caller_instruction_counter = graph_->GetCurrentInstructionId();
  HGraph* callee_graph = new (graph_->GetArena()) HGraph(
      graph_->GetArena(),
      callee_dex_file,
      method_index,
      requires_ctor_barrier,
      compiler_driver_->GetInstructionSet(),
      invoke_type,
      graph_->IsDebuggable(),
      /* osr */ false,
      caller_instruction_counter);
  callee_graph->SetArtMethod(resolved_method);

  // When they are needed, allocate `inline_stats_` on the Arena instead
  // of on the stack, as Clang might produce a stack frame too large
  // for this function, that would not fit the requirements of the
  // `-Wframe-larger-than` option.
  if (stats_ != nullptr) {
    // Reuse one object for all inline attempts from this caller to keep Arena memory usage low.
    if (inline_stats_ == nullptr) {
      void* storage = graph_->GetArena()->Alloc<OptimizingCompilerStats>(kArenaAllocMisc);
      inline_stats_ = new (storage) OptimizingCompilerStats;
    } else {
      inline_stats_->Reset();
    }
  }
  HGraphBuilder builder(callee_graph,
                        &dex_compilation_unit,
                        &outer_compilation_unit_,
                        resolved_method->GetDexFile(),
                        *code_item,
                        compiler_driver_,
                        codegen_,
                        inline_stats_,
                        resolved_method->GetQuickenedInfo(class_linker->GetImagePointerSize()),
                        dex_cache,
                        handles_);

  if (builder.BuildGraph() != kAnalysisSuccess) {
    VLOG(compiler) << "Method " << callee_dex_file.PrettyMethod(method_index)
                   << " could not be built, so cannot be inlined";
    return false;
  }

  if (!RegisterAllocator::CanAllocateRegistersFor(*callee_graph,
                                                  compiler_driver_->GetInstructionSet())) {
    VLOG(compiler) << "Method " << callee_dex_file.PrettyMethod(method_index)
                   << " cannot be inlined because of the register allocator";
    return false;
  }

  size_t parameter_index = 0;
  bool run_rtp = false;
  for (HInstructionIterator instructions(callee_graph->GetEntryBlock()->GetInstructions());
       !instructions.Done();
       instructions.Advance()) {
    HInstruction* current = instructions.Current();
    if (current->IsParameterValue()) {
      HInstruction* argument = invoke_instruction->InputAt(parameter_index);
      if (argument->IsNullConstant()) {
        current->ReplaceWith(callee_graph->GetNullConstant());
      } else if (argument->IsIntConstant()) {
        current->ReplaceWith(callee_graph->GetIntConstant(argument->AsIntConstant()->GetValue()));
      } else if (argument->IsLongConstant()) {
        current->ReplaceWith(callee_graph->GetLongConstant(argument->AsLongConstant()->GetValue()));
      } else if (argument->IsFloatConstant()) {
        current->ReplaceWith(
            callee_graph->GetFloatConstant(argument->AsFloatConstant()->GetValue()));
      } else if (argument->IsDoubleConstant()) {
        current->ReplaceWith(
            callee_graph->GetDoubleConstant(argument->AsDoubleConstant()->GetValue()));
      } else if (argument->GetType() == Primitive::kPrimNot) {
        if (!resolved_method->IsStatic() && parameter_index == 0 && receiver_type.IsValid()) {
          run_rtp = true;
          current->SetReferenceTypeInfo(receiver_type);
        } else {
          current->SetReferenceTypeInfo(argument->GetReferenceTypeInfo());
        }
        current->AsParameterValue()->SetCanBeNull(argument->CanBeNull());
      }
      ++parameter_index;
    }
  }

  // We have replaced formal arguments with actual arguments. If actual types
  // are more specific than the declared ones, run RTP again on the inner graph.
  if (run_rtp || ArgumentTypesMoreSpecific(invoke_instruction, resolved_method)) {
    ReferenceTypePropagation(callee_graph,
                             dex_compilation_unit.GetDexCache(),
                             handles_,
                             /* is_first_run */ false).Run();
  }

  size_t number_of_instructions_budget = kMaximumNumberOfHInstructions;
  size_t number_of_inlined_instructions =
      RunOptimizations(callee_graph, code_item, dex_compilation_unit);
  number_of_instructions_budget += number_of_inlined_instructions;

  HBasicBlock* exit_block = callee_graph->GetExitBlock();
  if (exit_block == nullptr) {
    VLOG(compiler) << "Method " << callee_dex_file.PrettyMethod(method_index)
                   << " could not be inlined because it has an infinite loop";
    return false;
  }

  bool has_one_return = false;
  for (HBasicBlock* predecessor : exit_block->GetPredecessors()) {
    if (predecessor->GetLastInstruction()->IsThrow()) {
      if (invoke_instruction->GetBlock()->IsTryBlock()) {
        // TODO(ngeoffray): Support adding HTryBoundary in Hgraph::InlineInto.
        VLOG(compiler) << "Method " << callee_dex_file.PrettyMethod(method_index)
                       << " could not be inlined because one branch always throws and"
                       << " caller is in a try/catch block";
        return false;
      } else if (graph_->GetExitBlock() == nullptr) {
        // TODO(ngeoffray): Support adding HExit in the caller graph.
        VLOG(compiler) << "Method " << callee_dex_file.PrettyMethod(method_index)
                       << " could not be inlined because one branch always throws and"
                       << " caller does not have an exit block";
        return false;
      } else if (graph_->HasIrreducibleLoops()) {
        // TODO(ngeoffray): Support re-computing loop information to graphs with
        // irreducible loops?
        VLOG(compiler) << "Method " << callee_dex_file.PrettyMethod(method_index)
                       << " could not be inlined because one branch always throws and"
                       << " caller has irreducible loops";
        return false;
      }
    } else {
      has_one_return = true;
    }
  }

  if (!has_one_return) {
    VLOG(compiler) << "Method " << callee_dex_file.PrettyMethod(method_index)
                   << " could not be inlined because it always throws";
    return false;
  }

  size_t number_of_instructions = 0;

  bool can_inline_environment =
      total_number_of_dex_registers_ < kMaximumNumberOfCumulatedDexRegisters;

  // Skip the entry block, it does not contain instructions that prevent inlining.
  for (HBasicBlock* block : callee_graph->GetReversePostOrderSkipEntryBlock()) {
    if (block->IsLoopHeader()) {
      if (block->GetLoopInformation()->IsIrreducible()) {
        // Don't inline methods with irreducible loops, they could prevent some
        // optimizations to run.
        VLOG(compiler) << "Method " << callee_dex_file.PrettyMethod(method_index)
                       << " could not be inlined because it contains an irreducible loop";
        return false;
      }
      if (!block->GetLoopInformation()->HasExitEdge()) {
        // Don't inline methods with loops without exit, since they cause the
        // loop information to be computed incorrectly when updating after
        // inlining.
        VLOG(compiler) << "Method " << callee_dex_file.PrettyMethod(method_index)
                       << " could not be inlined because it contains a loop with no exit";
        return false;
      }
    }

    for (HInstructionIterator instr_it(block->GetInstructions());
         !instr_it.Done();
         instr_it.Advance()) {
      if (number_of_instructions++ == number_of_instructions_budget) {
        VLOG(compiler) << "Method " << callee_dex_file.PrettyMethod(method_index)
                       << " is not inlined because its caller has reached"
                       << " its instruction budget limit.";
        return false;
      }
      HInstruction* current = instr_it.Current();
      if (!can_inline_environment && current->NeedsEnvironment()) {
        VLOG(compiler) << "Method " << callee_dex_file.PrettyMethod(method_index)
                       << " is not inlined because its caller has reached"
                       << " its environment budget limit.";
        return false;
      }

      if (current->NeedsEnvironment() &&
          !CanEncodeInlinedMethodInStackMap(*caller_compilation_unit_.GetDexFile(),
                                            resolved_method)) {
        VLOG(compiler) << "Method " << callee_dex_file.PrettyMethod(method_index)
                       << " could not be inlined because " << current->DebugName()
                       << " needs an environment, is in a different dex file"
                       << ", and cannot be encoded in the stack maps.";
        return false;
      }

      if (!same_dex_file && current->NeedsDexCacheOfDeclaringClass()) {
        VLOG(compiler) << "Method " << callee_dex_file.PrettyMethod(method_index)
                       << " could not be inlined because " << current->DebugName()
                       << " it is in a different dex file and requires access to the dex cache";
        return false;
      }

      if (current->IsUnresolvedStaticFieldGet() ||
          current->IsUnresolvedInstanceFieldGet() ||
          current->IsUnresolvedStaticFieldSet() ||
          current->IsUnresolvedInstanceFieldSet()) {
        // Entrypoint for unresolved fields does not handle inlined frames.
        VLOG(compiler) << "Method " << callee_dex_file.PrettyMethod(method_index)
                       << " could not be inlined because it is using an unresolved"
                       << " entrypoint";
        return false;
      }
    }
  }
  number_of_inlined_instructions_ += number_of_instructions;

  DCHECK_EQ(caller_instruction_counter, graph_->GetCurrentInstructionId())
      << "No instructions can be added to the outer graph while inner graph is being built";

  const int32_t callee_instruction_counter = callee_graph->GetCurrentInstructionId();
  graph_->SetCurrentInstructionId(callee_instruction_counter);
  *return_replacement = callee_graph->InlineInto(graph_, invoke_instruction);

  DCHECK_EQ(callee_instruction_counter, callee_graph->GetCurrentInstructionId())
      << "No instructions can be added to the inner graph during inlining into the outer graph";

  if (stats_ != nullptr) {
    DCHECK(inline_stats_ != nullptr);
    inline_stats_->AddTo(stats_);
  }

  return true;
}

size_t HInliner::RunOptimizations(HGraph* callee_graph,
                                  const DexFile::CodeItem* code_item,
                                  const DexCompilationUnit& dex_compilation_unit) {
  // Note: if the outermost_graph_ is being compiled OSR, we should not run any
  // optimization that could lead to a HDeoptimize. The following optimizations do not.
  HDeadCodeElimination dce(callee_graph, inline_stats_, "dead_code_elimination$inliner");
  HConstantFolding fold(callee_graph, "constant_folding$inliner");
  HSharpening sharpening(callee_graph, codegen_, dex_compilation_unit, compiler_driver_, handles_);
  InstructionSimplifier simplify(callee_graph, codegen_, inline_stats_);
  IntrinsicsRecognizer intrinsics(callee_graph, inline_stats_);

  HOptimization* optimizations[] = {
    &intrinsics,
    &sharpening,
    &simplify,
    &fold,
    &dce,
  };

  for (size_t i = 0; i < arraysize(optimizations); ++i) {
    HOptimization* optimization = optimizations[i];
    optimization->Run();
  }

  size_t number_of_inlined_instructions = 0u;
  if (depth_ + 1 < compiler_driver_->GetCompilerOptions().GetInlineDepthLimit()) {
    HInliner inliner(callee_graph,
                     outermost_graph_,
                     codegen_,
                     outer_compilation_unit_,
                     dex_compilation_unit,
                     compiler_driver_,
                     handles_,
                     inline_stats_,
                     total_number_of_dex_registers_ + code_item->registers_size_,
                     depth_ + 1);
    inliner.Run();
    number_of_inlined_instructions += inliner.number_of_inlined_instructions_;
  }

  return number_of_inlined_instructions;
}

static bool IsReferenceTypeRefinement(ReferenceTypeInfo declared_rti,
                                      bool declared_can_be_null,
                                      HInstruction* actual_obj)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (declared_can_be_null && !actual_obj->CanBeNull()) {
    return true;
  }

  ReferenceTypeInfo actual_rti = actual_obj->GetReferenceTypeInfo();
  return (actual_rti.IsExact() && !declared_rti.IsExact()) ||
          declared_rti.IsStrictSupertypeOf(actual_rti);
}

ReferenceTypeInfo HInliner::GetClassRTI(mirror::Class* klass) {
  return ReferenceTypePropagation::IsAdmissible(klass)
      ? ReferenceTypeInfo::Create(handles_->NewHandle(klass))
      : graph_->GetInexactObjectRti();
}

bool HInliner::ArgumentTypesMoreSpecific(HInvoke* invoke_instruction, ArtMethod* resolved_method) {
  // If this is an instance call, test whether the type of the `this` argument
  // is more specific than the class which declares the method.
  if (!resolved_method->IsStatic()) {
    if (IsReferenceTypeRefinement(GetClassRTI(resolved_method->GetDeclaringClass()),
                                  /* declared_can_be_null */ false,
                                  invoke_instruction->InputAt(0u))) {
      return true;
    }
  }

  // Iterate over the list of parameter types and test whether any of the
  // actual inputs has a more specific reference type than the type declared in
  // the signature.
  const DexFile::TypeList* param_list = resolved_method->GetParameterTypeList();
  for (size_t param_idx = 0,
              input_idx = resolved_method->IsStatic() ? 0 : 1,
              e = (param_list == nullptr ? 0 : param_list->Size());
       param_idx < e;
       ++param_idx, ++input_idx) {
    HInstruction* input = invoke_instruction->InputAt(input_idx);
    if (input->GetType() == Primitive::kPrimNot) {
      mirror::Class* param_cls = resolved_method->GetClassFromTypeIndex(
          param_list->GetTypeItem(param_idx).type_idx_,
          /* resolve */ false);
      if (IsReferenceTypeRefinement(GetClassRTI(param_cls),
                                    /* declared_can_be_null */ true,
                                    input)) {
        return true;
      }
    }
  }

  return false;
}

bool HInliner::ReturnTypeMoreSpecific(HInvoke* invoke_instruction,
                                      HInstruction* return_replacement) {
  // Check the integrity of reference types and run another type propagation if needed.
  if (return_replacement != nullptr) {
    if (return_replacement->GetType() == Primitive::kPrimNot) {
      // Test if the return type is a refinement of the declared return type.
      if (IsReferenceTypeRefinement(invoke_instruction->GetReferenceTypeInfo(),
                                    /* declared_can_be_null */ true,
                                    return_replacement)) {
        return true;
      } else if (return_replacement->IsInstanceFieldGet()) {
        HInstanceFieldGet* field_get = return_replacement->AsInstanceFieldGet();
        ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
        if (field_get->GetFieldInfo().GetField() ==
              class_linker->GetClassRoot(ClassLinker::kJavaLangObject)->GetInstanceField(0)) {
          return true;
        }
      }
    } else if (return_replacement->IsInstanceOf()) {
      // Inlining InstanceOf into an If may put a tighter bound on reference types.
      return true;
    }
  }

  return false;
}

void HInliner::FixUpReturnReferenceType(ArtMethod* resolved_method,
                                        HInstruction* return_replacement) {
  if (return_replacement != nullptr) {
    if (return_replacement->GetType() == Primitive::kPrimNot) {
      if (!return_replacement->GetReferenceTypeInfo().IsValid()) {
        // Make sure that we have a valid type for the return. We may get an invalid one when
        // we inline invokes with multiple branches and create a Phi for the result.
        // TODO: we could be more precise by merging the phi inputs but that requires
        // some functionality from the reference type propagation.
        DCHECK(return_replacement->IsPhi());
        mirror::Class* cls = resolved_method->GetReturnType(false /* resolve */);
        return_replacement->SetReferenceTypeInfo(GetClassRTI(cls));
      }
    }
  }
}

}  // namespace art
