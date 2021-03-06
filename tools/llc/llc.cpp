//===-- llc.cpp - Implement the LLVM Native Code Generator ----------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This is the llc code generator driver. It provides a convenient
// command-line interface for generating native assembly-language code
// or C code, given LLVM bitcode.
//
//===----------------------------------------------------------------------===//


#include "llvm/IR/LLVMContext.h"
#include "llvm/ADT/Triple.h"
#include "llvm/Assembly/PrintModulePass.h"
#include "llvm/CodeGen/CommandFlags.h"
#include "llvm/CodeGen/LinkAllAsmWriterComponents.h"
#include "llvm/CodeGen/LinkAllCodegenComponents.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Module.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/MC/SubtargetFeature.h"
#include "llvm/Pass.h"
#include "llvm/PassManager.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/FormattedStream.h"
#include "llvm/Support/Host.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/PluginLoader.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Target/TargetLibraryInfo.h"
#include "llvm/Target/TargetMachine.h"
#include <memory>
using namespace llvm;

// General options for llc.  Other pass-specific options are specified
// within the corresponding llc passes, and target-specific options
// and back-end code generation options are specified with the target machine.
//
static cl::opt<std::string>
InputFilename(cl::Positional, cl::desc("<input bitcode>"), cl::init("-"));

static cl::opt<std::string>
OutputFilename("o", cl::desc("Output filename"), cl::value_desc("filename"));

static cl::opt<unsigned>
TimeCompilations("time-compilations", cl::Hidden, cl::init(1u),
                 cl::value_desc("N"),
                 cl::desc("Repeat compilation N times for timing"));

static cl::opt<unsigned>
Threads("thread", cl::Hidden, cl::init(1u),
                 cl::value_desc("N"),
                 cl::desc("Start N Threads to compile module in parallel"));

// Determine optimization level.
static cl::opt<char>
OptLevel("O",
         cl::desc("Optimization level. [-O0, -O1, -O2, or -O3] "
                  "(default = '-O2')"),
         cl::Prefix,
         cl::ZeroOrMore,
         cl::init(' '));

static cl::opt<std::string>
TargetTriple("mtriple", cl::desc("Override target triple for module"));

cl::opt<bool> NoVerify("disable-verify", cl::Hidden,
                       cl::desc("Do not verify input module"));

cl::opt<bool>
DisableSimplifyLibCalls("disable-simplify-libcalls",
                        cl::desc("Disable simplify-libcalls"),
                        cl::init(false));

static int compileModule(char**, LLVMContext&);

// GetFileNameRoot - Helper function to get the basename of a filename.
static inline std::string
GetFileNameRoot(const std::string &InputFilename) {
  std::string IFN = InputFilename;
  std::string outputFilename;
  int Len = IFN.length();
  if ((Len > 2) &&
      IFN[Len-3] == '.' &&
      ((IFN[Len-2] == 'b' && IFN[Len-1] == 'c') ||
       (IFN[Len-2] == 'l' && IFN[Len-1] == 'l'))) {
    outputFilename = std::string(IFN.begin(), IFN.end()-3); // s/.bc/.s/
  } else {
    outputFilename = IFN;
  }
  return outputFilename;
}

static tool_output_file *GetOutputStream(const char *TargetName,
                                         Triple::OSType OS,
                                         const char *ProgName) {
  // If we don't yet have an output filename, make one.
  if (OutputFilename.empty()) {
    if (InputFilename == "-")
      OutputFilename = "-";
    else {
      OutputFilename = GetFileNameRoot(InputFilename);

      switch (FileType) {
      case TargetMachine::CGFT_AssemblyFile:
        if (TargetName[0] == 'c') {
          if (TargetName[1] == 0)
            OutputFilename += ".cbe.c";
          else if (TargetName[1] == 'p' && TargetName[2] == 'p')
            OutputFilename += ".cpp";
          else
            OutputFilename += ".s";
        } else
          OutputFilename += ".s";
        break;
      case TargetMachine::CGFT_ObjectFile:
        if (OS == Triple::Win32)
          OutputFilename += ".obj";
        else
          OutputFilename += ".o";
        break;
      case TargetMachine::CGFT_Null:
        OutputFilename += ".null";
        break;
      }
    }
  }

  // Decide if we need "binary" output.
  bool Binary = false;
  switch (FileType) {
  case TargetMachine::CGFT_AssemblyFile:
    break;
  case TargetMachine::CGFT_ObjectFile:
  case TargetMachine::CGFT_Null:
    Binary = true;
    break;
  }

  // Open the file.
  std::string error;
  sys::fs::OpenFlags OpenFlags = sys::fs::F_None;
  if (Binary)
    OpenFlags |= sys::fs::F_Binary;
  tool_output_file *FDOut = new tool_output_file(OutputFilename.c_str(), error,
                                                 OpenFlags);
  if (!error.empty()) {
    errs() << error << '\n';
    delete FDOut;
    return 0;
  }

  return FDOut;
}

// main - Entry point for the llc compiler.
//
int main(int argc, char **argv) {
  sys::PrintStackTraceOnErrorSignal();
  PrettyStackTraceProgram X(argc, argv);

  // Enable debug stream buffering.
  EnableDebugBuffering = true;

  llvm_shutdown_obj Y;  // Call llvm_shutdown() on exit.

  // Initialize targets first, so that --version shows registered targets.
  InitializeAllTargets();
  InitializeAllTargetMCs();
  InitializeAllAsmPrinters();
  InitializeAllAsmParsers();

  // Initialize codegen and IR passes used by llc so that the -print-after,
  // -print-before, and -stop-after options work.
  PassRegistry *Registry = PassRegistry::getPassRegistry();
  initializeCore(*Registry);
  initializeCodeGen(*Registry);
  initializeLoopStrengthReducePass(*Registry);
  initializeLowerIntrinsicsPass(*Registry);
  initializeUnreachableBlockElimPass(*Registry);

  // Register the target printer for --version.
  cl::AddExtraVersionPrinter(TargetRegistry::printRegisteredTargetsForVersion);

  cl::ParseCommandLineOptions(argc, argv, "llvm system compiler\n");

  // Compile the module TimeCompilations times to give better compile time
  // metrics.
  for (unsigned I = TimeCompilations; I; --I) {
    LLVMContext Context;
    if (int RetVal = compileModule(argv, Context))
      return RetVal;
  }
  return 0;
}

static void deletePMList(unsigned size, PassManager **PMList, TargetMachine **TMList) {
  for (unsigned index = 0; index < size; index++) {
    delete PMList[index];
    delete TMList[index];
  }
  delete []PMList;
  delete []TMList;
}

static int compileModule(char **argv, LLVMContext &Context) {
  // Load the module to be compiled...
  SMDiagnostic Err;
  OwningPtr<Module> M;
  Module *mod = 0;
  Triple TheTriple;
  Triple OrigTriple;

  bool SkipModule = MCPU == "help" ||
                    (!MAttrs.empty() && MAttrs.front() == "help");

  // If user just wants to list available options, skip module loading
  if (!SkipModule) {
    M.reset(ParseIRFile(InputFilename, Err, Context));
    mod = M.get();
    if (mod == 0) {
      Err.print(argv[0], errs());
      return 1;
    }

    OrigTriple.setTriple(mod->getTargetTriple());

    // If we are supposed to override the target triple, do so now.
    if (!TargetTriple.empty())
      mod->setTargetTriple(Triple::normalize(TargetTriple));
    TheTriple = Triple(mod->getTargetTriple());
  } else {
    TheTriple = Triple(Triple::normalize(TargetTriple));
  }

  if (TheTriple.getTriple().empty())
    TheTriple.setTriple(sys::getDefaultTargetTriple());

  // Get the target specific parser.
  std::string Error;
  const Target *TheTarget = TargetRegistry::lookupTarget(MArch, TheTriple,
                                                         Error);
  if (!TheTarget) {
    errs() << argv[0] << ": " << Error;
    return 1;
  }

  // Package up features to be passed to target/subtarget
  std::string FeaturesStr;
  if (MAttrs.size()) {
    SubtargetFeatures Features;
    for (unsigned i = 0; i != MAttrs.size(); ++i)
      Features.AddFeature(MAttrs[i]);
    FeaturesStr = Features.getString();
  }

  CodeGenOpt::Level OLvl = CodeGenOpt::Default;
  switch (OptLevel) {
  default:
    errs() << argv[0] << ": invalid optimization level.\n";
    return 1;
  case ' ': break;
  case '0': OLvl = CodeGenOpt::None; break;
  case '1': OLvl = CodeGenOpt::Less; break;
  case '2': OLvl = CodeGenOpt::Default; break;
  case '3': OLvl = CodeGenOpt::Aggressive; break;
  }

  TargetOptions Options;
  Options.LessPreciseFPMADOption = EnableFPMAD;
  Options.NoFramePointerElim = DisableFPElim;
  Options.AllowFPOpFusion = FuseFPOps;
  Options.UnsafeFPMath = EnableUnsafeFPMath;
  Options.NoInfsFPMath = EnableNoInfsFPMath;
  Options.NoNaNsFPMath = EnableNoNaNsFPMath;
  Options.HonorSignDependentRoundingFPMathOption =
      EnableHonorSignDependentRoundingFPMath;
  Options.UseSoftFloat = GenerateSoftFloatCalls;
  if (FloatABIForCalls != FloatABI::Default)
    Options.FloatABIType = FloatABIForCalls;
  Options.NoZerosInBSS = DontPlaceZerosInBSS;
  Options.GuaranteedTailCallOpt = EnableGuaranteedTailCallOpt;
  Options.DisableTailCalls = DisableTailCalls;
  Options.StackAlignmentOverride = OverrideStackAlignment;
  Options.TrapFuncName = TrapFuncName;
  Options.PositionIndependentExecutable = EnablePIE;
  Options.EnableSegmentedStacks = SegmentedStacks;
  Options.UseInitArray = UseInitArray;

  unsigned funcNum = 0;
  for (Module::iterator I = mod->begin(), E = mod->end(); I != E; ++I) {
    if (!I->isDeclaration()) {
      I->setSeq(funcNum++);
    }
  }
  if (funcNum == 0) {
    funcNum = 1;
  }
  if (Threads > funcNum) {
    Threads = funcNum;
  }

  PassManager **PMList = new PassManager*[Threads];
  TargetMachine **TMList = new TargetMachine*[Threads];
  for (unsigned thd = 0; thd < Threads; thd++) {
    PMList[thd] = new PassManager();
    TMList[thd] = TheTarget->createTargetMachine(TheTriple.getTriple(),
                                            MCPU, FeaturesStr, Options,
                                            RelocModel, CMModel, OLvl);
    assert(TMList[thd] && "Could not allocate target machine!");
    assert(mod && "Should have exited after outputting help!");
    TargetMachine &Target = *TMList[thd];

    if (DisableDotLoc)
      Target.setMCUseLoc(false);

    if (DisableCFI)
      Target.setMCUseCFI(false);

    if (EnableDwarfDirectory)
      Target.setMCUseDwarfDirectory(true);

    if (GenerateSoftFloatCalls)
      FloatABIForCalls = FloatABI::Soft;

    if (OrigTriple.getOS() == llvm::Triple::NDK) {
      if (OrigTriple.getArch() == llvm::Triple::le32 ||
          OrigTriple.getArch() == llvm::Triple::le64) {
        Target.setMCNoExecStack(true);
      }
    }

    // Disable .loc support for older OS X versions.
    if (TheTriple.isMacOSX() &&
        TheTriple.isMacOSXVersionLT(10, 6))
      Target.setMCUseLoc(false);
  }
  // Figure out where we are going to send the output.
  OwningPtr<tool_output_file> Out
    (GetOutputStream(TheTarget->getName(), TheTriple.getOS(), argv[0]));
  if (!Out) return 1;

  // Build up all of the passes that we want to do to the module.

  for (unsigned thd = 0; thd < Threads; thd++) {
    TargetMachine &Target = *TMList[thd];
    // Add an appropriate TargetLibraryInfo pass for the module's triple.
    TargetLibraryInfo *TLI = new TargetLibraryInfo(TheTriple);
    if (DisableSimplifyLibCalls)
      TLI->disableAllFunctions();
    PMList[thd]->add(TLI);

    // Add intenal analysis passes from the target machine.
    Target.addAnalysisPasses(*PMList[thd]);

    // Add the target data from the target machine, if it exists, or the module.
    if (const DataLayout *TD = Target.getDataLayout())
      PMList[thd]->add(new DataLayout(*TD));
    else
      PMList[thd]->add(new DataLayout(mod));

    // Override default to generate verbose assembly.
    Target.setAsmVerbosityDefault(true);

    if (RelaxAll) {
      if (FileType != TargetMachine::CGFT_ObjectFile)
        errs() << argv[0]
               << ": warning: ignoring -mc-relax-all because filetype != obj";
      else
        Target.setMCRelaxAll(true);
    }
  }

  {
    formatted_raw_ostream FOS(Out->os());

    AnalysisID StartAfterID = 0;
    AnalysisID StopAfterID = 0;
    const PassRegistry *PR = PassRegistry::getPassRegistry();
    if (!StartAfter.empty()) {
      const PassInfo *PI = PR->getPassInfo(StartAfter);
      if (!PI) {
        errs() << argv[0] << ": start-after pass is not registered.\n";
        deletePMList(Threads, PMList, TMList);
        return 1;
      }
      StartAfterID = PI->getTypeInfo();
    }
    if (!StopAfter.empty()) {
      const PassInfo *PI = PR->getPassInfo(StopAfter);
      if (!PI) {
        errs() << argv[0] << ": stop-after pass is not registered.\n";
        deletePMList(Threads, PMList, TMList);
        return 1;
      }
      StopAfterID = PI->getTypeInfo();
    }
    for (unsigned thd = 0; thd < Threads; thd++) {
      TargetMachine &Target = *TMList[thd];
      // Ask the target to add backend passes as necessary.
      if (Target.addPassesToEmitFile(*PMList[thd], FOS, FileType, NoVerify,
                                     StartAfterID, StopAfterID)) {
        errs() << argv[0] << ": target does not support generation of this"
               << " file type!\n";
        deletePMList(Threads, PMList, TMList);
        return 1;
      }
      if (thd > 0) {
        PMList[0]->addChild(PMList[thd]);
      }
    }
    // Before executing passes, print the final values of the LLVM options.
    cl::PrintOptionValues();

    PMList[0]->run(*mod);
    deletePMList(Threads, PMList, TMList);
  }

  // Declare success.
  Out->keep();

  return 0;
}
