//===- PNaClABIVerifyModule.cpp - Verify PNaCl ABI rules --------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Verify module-level PNaCl ABI requirements (specifically those that do not
// require looking at the function bodies)
//
//
//===----------------------------------------------------------------------===//

#include "llvm/Pass.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Analysis/NaCl.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include "PNaClABITypeChecker.h"
using namespace llvm;

namespace llvm {
cl::opt<bool>
PNaClABIAllowDebugMetadata("pnaclabi-allow-debug-metadata",
  cl::desc("Allow debug metadata during PNaCl ABI verification."),
  cl::init(false));

}

static cl::opt<bool>
PNaClABIAllowDevIntrinsics("pnaclabi-allow-dev-intrinsics",
  cl::desc("Allow all LLVM intrinsics during PNaCl ABI verification."),
  cl::init(true));  // TODO(jvoung): Make this false by default.

namespace {
// This pass should not touch function bodies, to stay streaming-friendly
class PNaClABIVerifyModule : public ModulePass {
 public:
  static char ID;
  PNaClABIVerifyModule() :
      ModulePass(ID),
      Reporter(new PNaClABIErrorReporter),
      ReporterIsOwned(true) {
    initializePNaClABIVerifyModulePass(*PassRegistry::getPassRegistry());
  }
  explicit PNaClABIVerifyModule(PNaClABIErrorReporter *Reporter_) :
      ModulePass(ID),
      Reporter(Reporter_),
      ReporterIsOwned(false) {
    initializePNaClABIVerifyModulePass(*PassRegistry::getPassRegistry());
  }
  ~PNaClABIVerifyModule() {
    if (ReporterIsOwned)
      delete Reporter;
  }
  bool runOnModule(Module &M);
  virtual void print(raw_ostream &O, const Module *M) const;
 private:
  void checkGlobalValueCommon(const GlobalValue *GV);
  bool isWhitelistedIntrinsic(const Function *F, unsigned ID);
  bool isWhitelistedMetadata(const NamedMDNode *MD);
  PNaClABITypeChecker TC;
  PNaClABIErrorReporter *Reporter;
  bool ReporterIsOwned;
};

static const char *linkageName(GlobalValue::LinkageTypes LT) {
  // This logic is taken from PrintLinkage in lib/VMCore/AsmWriter.cpp
  switch (LT) {
    case GlobalValue::ExternalLinkage: return "external";
    case GlobalValue::PrivateLinkage:       return "private ";
    case GlobalValue::LinkerPrivateLinkage: return "linker_private ";
    case GlobalValue::LinkerPrivateWeakLinkage: return "linker_private_weak ";
    case GlobalValue::InternalLinkage:      return "internal ";
    case GlobalValue::LinkOnceAnyLinkage:   return "linkonce ";
    case GlobalValue::LinkOnceODRLinkage:   return "linkonce_odr ";
    case GlobalValue::LinkOnceODRAutoHideLinkage:
      return "linkonce_odr_auto_hide ";
    case GlobalValue::WeakAnyLinkage:       return "weak ";
    case GlobalValue::WeakODRLinkage:       return "weak_odr ";
    case GlobalValue::CommonLinkage:        return "common ";
    case GlobalValue::AppendingLinkage:     return "appending ";
    case GlobalValue::DLLImportLinkage:     return "dllimport ";
    case GlobalValue::DLLExportLinkage:     return "dllexport ";
    case GlobalValue::ExternalWeakLinkage:  return "extern_weak ";
    case GlobalValue::AvailableExternallyLinkage:
      return "available_externally ";
    default:
      return "unknown";
  }
}

} // end anonymous namespace

// Check linkage type and section attributes, which are the same for
// GlobalVariables and Functions.
void PNaClABIVerifyModule::checkGlobalValueCommon(const GlobalValue *GV) {
  assert(!isa<GlobalAlias>(GV));
  const char *GVTypeName = isa<GlobalVariable>(GV) ?
      "Variable " : "Function ";
  switch (GV->getLinkage()) {
    // TODO(dschuff): Disallow external linkage
    case GlobalValue::ExternalLinkage:
    case GlobalValue::AvailableExternallyLinkage:
    case GlobalValue::InternalLinkage:
    case GlobalValue::PrivateLinkage:
      break;
    default:
      Reporter->addError() << GVTypeName << GV->getName()
                           << " has disallowed linkage type: "
                           << linkageName(GV->getLinkage()) << "\n";
  }
  if (GV->hasSection()) {
    Reporter->addError() << GVTypeName << GV->getName() <<
        " has disallowed \"section\" attribute\n";
  }
}

static bool TypeAcceptable(const Type *T,
                           const ArrayRef<Type*> &AcceptableTypes) {
  for (ArrayRef<Type*>::iterator I = AcceptableTypes.begin(),
       E = AcceptableTypes.end(); I != E; ++I)
    if (*I == T)
      return true;
  return false;
}

// We accept bswap for a limited set of types (i16, i32, i64).
// The various backends are able to generate instructions to
// implement the intrinsic.  Also, i16 and i64 are easy to
// implement as along as there is a way to do i32.
static bool isWhitelistedBswap(const Function *F) {
  FunctionType *FT = F->getFunctionType();
  if (FT->getNumParams() != 1)
    return false;
  Type *ParamType = FT->getParamType(0);
  LLVMContext &C = F->getContext();
  Type *AcceptableTypes[] = { Type::getInt16Ty(C),
                              Type::getInt32Ty(C),
                              Type::getInt64Ty(C) };
  return TypeAcceptable(ParamType, AcceptableTypes);
}

bool PNaClABIVerifyModule::isWhitelistedIntrinsic(const Function *F,
                                                  unsigned ID) {
  // Keep 3 categories of intrinsics for now.
  // (1) Allowed always
  // (2) Never allowed
  // (3) "Dev" intrinsics, which may or may not be allowed.
  // "Dev" intrinsics are controlled by the PNaClABIAllowDevIntrinsics flag.
  // Please keep these sorted or grouped in a sensible way, within
  // each category.
  switch(ID) {
    // Disallow by default.
    default: return false;
    // (1) Always allowed.
    case Intrinsic::bswap: return isWhitelistedBswap(F);
    case Intrinsic::invariant_end:
    case Intrinsic::invariant_start:
    case Intrinsic::lifetime_end:
    case Intrinsic::lifetime_start:
    case Intrinsic::memcpy:
    case Intrinsic::memmove:
    case Intrinsic::memset:
    case Intrinsic::nacl_read_tp:
    case Intrinsic::nacl_setjmp:
    case Intrinsic::nacl_longjmp:
    case Intrinsic::trap:
      return true;

    // (2) Known to be never allowed.
    case Intrinsic::not_intrinsic:
    // Trampolines depend on a target-specific-sized/aligned buffer.
    case Intrinsic::adjust_trampoline:
    case Intrinsic::init_trampoline:
    // CXX exception handling is not stable.
    case Intrinsic::eh_dwarf_cfa:
    case Intrinsic::eh_return_i32:
    case Intrinsic::eh_return_i64:
    case Intrinsic::eh_sjlj_callsite:
    case Intrinsic::eh_sjlj_functioncontext:
    case Intrinsic::eh_sjlj_longjmp:
    case Intrinsic::eh_sjlj_lsda:
    case Intrinsic::eh_sjlj_setjmp:
    case Intrinsic::eh_typeid_for:
    case Intrinsic::eh_unwind_init:
    // We do not want to expose addresses to the user.
    case Intrinsic::frameaddress:
    case Intrinsic::returnaddress:
    // Not supporting stack protectors.
    case Intrinsic::stackprotector:
    // Var-args handling is done w/out intrinsics.
    case Intrinsic::vacopy:
    case Intrinsic::vaend:
    case Intrinsic::vastart:
      return false;

    // (3) Dev intrinsics.
    case Intrinsic::dbg_declare:
    case Intrinsic::dbg_value:
      return PNaClABIAllowDevIntrinsics || PNaClABIAllowDebugMetadata;
    case Intrinsic::cos: // Rounding not defined: support with fast-math?
    case Intrinsic::ctlz: // Support via compiler_rt if arch doesn't have it?
    case Intrinsic::ctpop: // Support via compiler_rt if arch doesn't have it?
    case Intrinsic::cttz: // Support via compiler_rt if arch doesn't have it?
    case Intrinsic::exp: // Rounding not defined: support with fast-math?
    case Intrinsic::exp2: // Rounding not defined: support with fast-math?
    case Intrinsic::expect: // From __builtin_expect.
    case Intrinsic::flt_rounds: // For FLT_ROUNDS macro from float.h.
      // We do not have fesetround() in newlib, can we return a
      // consistent rounding mode though?
    case Intrinsic::log: // Rounding not defined: support with fast-math?
    case Intrinsic::log2: // Rounding not defined: support with fast-math?
    case Intrinsic::log10: // Rounding not defined: support with fast-math?
    case Intrinsic::nacl_target_arch: // Used by translator self-build.
    case Intrinsic::pow: // Rounding is supposed to be the same as libm.
    case Intrinsic::powi: // Rounding not defined: support with fast-math?
    case Intrinsic::prefetch: // Could ignore if target doesn't support?
    case Intrinsic::sin: // Rounding not defined: support with fast-math?
    case Intrinsic::sqrt: // Rounding is defined, but setting errno up to libm.
    case Intrinsic::stackrestore: // Used to support C99 VLAs.
    case Intrinsic::stacksave: // Used to support C99 VLAs.
    // the *_with_overflow return struct types, so we'll need to fix these.
    case Intrinsic::sadd_with_overflow: // Introduced by -ftrapv
    case Intrinsic::ssub_with_overflow:
    case Intrinsic::uadd_with_overflow:
    case Intrinsic::usub_with_overflow:
    case Intrinsic::smul_with_overflow:
    case Intrinsic::umul_with_overflow: // Introduced by c++ new[x * y].
      return PNaClABIAllowDevIntrinsics;
  }
}

bool PNaClABIVerifyModule::isWhitelistedMetadata(const NamedMDNode *MD) {
  return MD->getName().startswith("llvm.dbg.") && PNaClABIAllowDebugMetadata;
}

bool PNaClABIVerifyModule::runOnModule(Module &M) {
  if (!M.getModuleInlineAsm().empty()) {
    Reporter->addError() <<
        "Module contains disallowed top-level inline assembly\n";
  }

  for (Module::const_global_iterator MI = M.global_begin(), ME = M.global_end();
       MI != ME; ++MI) {
    // Check types of global variables and their initializers
    if (!TC.isValidType(MI->getType())) {
      // GVs are pointers, so print the pointed-to type for clarity
      Reporter->addError() << "Variable " << MI->getName() <<
          " has disallowed type: " <<
          PNaClABITypeChecker::getTypeName(MI->getType()->getContainedType(0))
          + "\n";
    } else if (MI->hasInitializer()) {
      // If the type of the global is bad, no point in checking its initializer
      Type *T = TC.checkTypesInConstant(MI->getInitializer());
      if (T) {
        Reporter->addError() << "Initializer for " << MI->getName() <<
            " has disallowed type: " <<
            PNaClABITypeChecker::getTypeName(T) << "\n";
      }
    }

    checkGlobalValueCommon(MI);

    if (MI->isThreadLocal()) {
      Reporter->addError() << "Variable " << MI->getName() <<
          " has disallowed \"thread_local\" attribute\n";
    }
  }

  // No aliases allowed for now.
  for (Module::alias_iterator MI = M.alias_begin(),
           E = M.alias_end(); MI != E; ++MI) {
    Reporter->addError() << "Variable " << MI->getName() <<
        " is an alias (disallowed)\n";
  }

  for (Module::const_iterator MI = M.begin(), ME = M.end(); MI != ME; ++MI) {
    // Check intrinsics.
    if (MI->isIntrinsic()
        && !isWhitelistedIntrinsic(MI, MI->getIntrinsicID())) {
      Reporter->addError() << "Function " << MI->getName()
                           << " is a disallowed LLVM intrinsic\n";
    }

    // Check types of functions and their arguments
    FunctionType *FT = MI->getFunctionType();
    if (!TC.isValidType(FT->getReturnType())) {
      Reporter->addError() << "Function " << MI->getName() <<
          " has disallowed return type: " <<
          PNaClABITypeChecker::getTypeName(FT->getReturnType()) << "\n";
    }
    for (unsigned I = 0, E = FT->getNumParams(); I < E; ++I) {
      Type *PT = FT->getParamType(I);
      if (!TC.isValidType(PT)) {
        Reporter->addError() << "Function " << MI->getName() << " argument " <<
            I + 1 << " has disallowed type: " <<
            PNaClABITypeChecker::getTypeName(PT) << "\n";
      }
    }
    // Pointers to varargs function types are not yet disallowed, but
    // we do disallow defining or calling functions of varargs types.
    if (MI->isVarArg()) {
      Reporter->addError() << "Function " << MI->getName() <<
          " is a variable-argument function (disallowed)\n";
    }

    checkGlobalValueCommon(MI);

    if (MI->hasGC()) {
      Reporter->addError() << "Function " << MI->getName() <<
          " has disallowed \"gc\" attribute\n";
    }
  }

  // Check named metadata nodes
  for (Module::const_named_metadata_iterator I = M.named_metadata_begin(),
           E = M.named_metadata_end(); I != E; ++I) {
    if (!isWhitelistedMetadata(I)) {
      Reporter->addError() << "Named metadata node " << I->getName()
                           << " is disallowed\n";
    } else {
      // Check the types in the metadata.
      for (unsigned i = 0, e = I->getNumOperands(); i != e; i++) {
        if (Type *T = TC.checkTypesInMDNode(I->getOperand(i))) {
          Reporter->addError() << "Named metadata node " << I->getName()
                               << " refers to disallowed type: "
                               << PNaClABITypeChecker::getTypeName(T) << "\n";
        }
      }
    }
  }

  Reporter->checkForFatalErrors();
  return false;
}

// This method exists so that the passes can easily be run with opt -analyze.
// In this case the default constructor is used and we want to reset the error
// messages after each print (this is more of an issue for the FunctionPass
// than the ModulePass)
void PNaClABIVerifyModule::print(llvm::raw_ostream &O, const Module *M) const {
  Reporter->printErrors(O);
  Reporter->reset();
}

char PNaClABIVerifyModule::ID = 0;
INITIALIZE_PASS(PNaClABIVerifyModule, "verify-pnaclabi-module",
                "Verify module for PNaCl", false, true)

ModulePass *llvm::createPNaClABIVerifyModulePass(
    PNaClABIErrorReporter *Reporter) {
  return new PNaClABIVerifyModule(Reporter);
}