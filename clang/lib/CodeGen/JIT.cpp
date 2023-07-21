//===--- CodeGenAction.cpp - LLVM Code Generation Frontend Action ---------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#include <llvm/IR/BasicBlock.h>
#include <iostream>
#include "clang/CodeGen/CodeGenAction.h"
#include "CodeGenModule.h"
#include "CoverageMappingGen.h"
#include "CGCXXABI.h"
#include "MacroPPCallbacks.h"
#include "clang/AST/ASTConsumer.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclGroup.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/PrettyPrinter.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/DiagnosticIDs.h"
#include "clang/Basic/DiagnosticOptions.h"
#include "clang/Basic/FileManager.h"
#include "clang/Basic/FileSystemOptions.h"
#include "clang/Basic/LLVM.h"
#include "clang/Basic/MemoryBufferCache.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/Basic/TargetOptions.h"

#include "clang/CodeGen/BackendUtil.h"
#include "clang/CodeGen/ModuleBuilder.h"
#include "clang/Driver/Compilation.h"
#include "clang/Driver/Driver.h"
#include "clang/Driver/Job.h"
#include "clang/Driver/Options.h"
#include "clang/Driver/Tool.h"
#include "clang/Driver/ToolChain.h"
#include "clang/Frontend/ASTUnit.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/CompilerInvocation.h"
#include "clang/Frontend/FrontendDiagnostic.h"
#include "clang/Frontend/FrontendOptions.h"
#include "clang/Frontend/TextDiagnosticPrinter.h"
#include "clang/Lex/HeaderSearch.h"
#include "clang/Lex/HeaderSearchOptions.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Lex/PreprocessorOptions.h"
#include "clang/Parse/Parser.h"
#include "clang/Sema/Sema.h"
#include "clang/Sema/Template.h"
#include "clang/Sema/TemplateDeduction.h"
#include "clang/Serialization/ASTReader.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/IntrusiveRefCntPtr.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/CodeGen/MachineOptimizationRemarkEmitter.h"
#include "llvm/ExecutionEngine/ExecutionEngine.h"
#include "llvm/ExecutionEngine/JITSymbol.h"
#include "llvm/ExecutionEngine/Orc/CompileUtils.h"
#include "llvm/ExecutionEngine/Orc/ExecutionUtils.h"
#include "llvm/ExecutionEngine/Orc/IRCompileLayer.h"
#include "llvm/ExecutionEngine/Orc/LambdaResolver.h"
#include "llvm/ExecutionEngine/Orc/RTDyldObjectLinkingLayer.h"
#include "llvm/ExecutionEngine/RTDyldMemoryManager.h"
#include "llvm/ExecutionEngine/SectionMemoryManager.h"
#include "llvm/IR/DerivedTypes.h"
#include <llvm/IR/Function.h>
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/IR/DiagnosticInfo.h"
#include "llvm/IR/DiagnosticPrinter.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Mangler.h"
#include "llvm/IR/Module.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Option/OptTable.h"
#include "llvm/Option/Option.h"
#include "llvm/Pass.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/DynamicLibrary.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Host.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Mutex.h"
#include "llvm/Support/MutexGuard.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/VirtualFileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/Timer.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Support/YAMLTraits.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/IPO/Internalize.h"

#include <cassert>
#include <cstdlib> // ::getenv
#include <cstring>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>
#include <unordered_map>
#include <x86intrin.h>
using namespace clang;
using namespace llvm;

#define DEBUG_TYPE "clang-jit"
llvm::DenseMap<const void *, int> autotuneTable;
llvm::DenseMap<const void *, llvm::APInt*>Values;
long long *rtdsc_values;


namespace {
// FIXME: This is copied from lib/Frontend/ASTUnit.cpp

/// Gathers information from ASTReader that will be used to initialize
/// a Preprocessor.
class ASTInfoCollector : public ASTReaderListener {
  Preprocessor &PP;
  ASTContext *Context;
  HeaderSearchOptions &HSOpts;
  PreprocessorOptions &PPOpts;
  LangOptions &LangOpt;
  std::shared_ptr<clang::TargetOptions> &TargetOpts;
  IntrusiveRefCntPtr<TargetInfo> &Target;
  unsigned &Counter;
  bool InitializedLanguage = false;

public:
  ASTInfoCollector(Preprocessor &PP, ASTContext *Context,
                   HeaderSearchOptions &HSOpts, PreprocessorOptions &PPOpts,
                   LangOptions &LangOpt,
                   std::shared_ptr<clang::TargetOptions> &TargetOpts,
                   IntrusiveRefCntPtr<TargetInfo> &Target, unsigned &Counter)
      : PP(PP), Context(Context), HSOpts(HSOpts), PPOpts(PPOpts),
        LangOpt(LangOpt), TargetOpts(TargetOpts), Target(Target),
        Counter(Counter) {}

  bool ReadLanguageOptions(const LangOptions &LangOpts, bool Complain,
                           bool AllowCompatibleDifferences) override {
    if (InitializedLanguage)
      return false;

    LangOpt = LangOpts;
    InitializedLanguage = true;

    updated();
    return false;
  }

  bool ReadHeaderSearchOptions(const HeaderSearchOptions &HSOpts,
                               StringRef SpecificModuleCachePath,
                               bool Complain) override {
    this->HSOpts = HSOpts;
    return false;
  }

  bool ReadPreprocessorOptions(const PreprocessorOptions &PPOpts, bool Complain,
                               std::string &SuggestedPredefines) override {
    this->PPOpts = PPOpts;
    return false;
  }

  bool ReadTargetOptions(const clang::TargetOptions &TargetOpts, bool Complain,
                         bool AllowCompatibleDifferences) override {
    // If we've already initialized the target, don't do it again.
    if (Target)
      return false;

    this->TargetOpts = std::make_shared<clang::TargetOptions>(TargetOpts);
    Target =
        TargetInfo::CreateTargetInfo(PP.getDiagnostics(), this->TargetOpts);

    updated();
    return false;
  }

  void ReadCounter(const serialization::ModuleFile &M,
                   unsigned Value) override {
    Counter = Value;
  }

private:
  void updated() {
    if (!Target || !InitializedLanguage)
      return;

    // Inform the target of the language options.
    //
    // FIXME: We shouldn't need to do this, the target should be immutable once
    // created. This complexity should be lifted elsewhere.
    Target->adjust(LangOpt);

    // Initialize the preprocessor.
    PP.Initialize(*Target);

    if (!Context)
      return;

    // Initialize the ASTContext
    Context->InitBuiltinTypes(*Target);

    // Adjust printing policy based on language options.
    Context->setPrintingPolicy(PrintingPolicy(LangOpt));

    // We didn't have access to the comment options when the ASTContext was
    // constructed, so register them now.
    Context->getCommentCommandTraits().registerCommentOptions(
        LangOpt.CommentOpts);
  }
};

void fatal() {
  report_fatal_error("Clang JIT failed!");
}

// This is a variant of ORC's LegacyLookupFnResolver with a cutomized
// getResponsibilitySet behavior allowing us to claim responsibility for weak
// symbols in the loaded modules that we don't otherwise have.
// Note: We generally convert all IR level symbols to have strong linkage, but
// that won't cover everything (and especially doesn't cover the DW.ref.
// symbols created by the low-level EH logic on some platforms).
template <typename LegacyLookupFn>
class ClangLookupFnResolver final : public llvm::orc::SymbolResolver {
public:
  using ErrorReporter = std::function<void(Error)>;

  ClangLookupFnResolver(llvm::orc::ExecutionSession &ES,
                              LegacyLookupFn LegacyLookup,
                              ErrorReporter ReportError)
      : ES(ES), LegacyLookup(std::move(LegacyLookup)),
        ReportError(std::move(ReportError)) {}

  llvm::orc::SymbolNameSet
  getResponsibilitySet(const llvm::orc::SymbolNameSet &Symbols) final {
    llvm::orc::SymbolNameSet Result;

    for (auto &S : Symbols) {
      if (JITSymbol Sym = LegacyLookup(*S)) {
        // If the symbol exists elsewhere, and we have only a weak version,
        // then we're not responsible.
        continue;
      } else if (auto Err = Sym.takeError()) {
        ReportError(std::move(Err));
        return llvm::orc::SymbolNameSet();
      } else {
        Result.insert(S);
      }
    }

    return Result;
  }

  llvm::orc::SymbolNameSet
  lookup(std::shared_ptr<llvm::orc::AsynchronousSymbolQuery> Query,
                         llvm::orc::SymbolNameSet Symbols) final {
    return llvm::orc::lookupWithLegacyFn(ES, *Query, Symbols, LegacyLookup);
  }

private:
  llvm::orc::ExecutionSession &ES;
  LegacyLookupFn LegacyLookup;
  ErrorReporter ReportError;
};

template <typename LegacyLookupFn>
std::shared_ptr<ClangLookupFnResolver<LegacyLookupFn>>
createClangLookupResolver(llvm::orc::ExecutionSession &ES,
                          LegacyLookupFn LegacyLookup,
                          std::function<void(Error)> ErrorReporter) {
  return std::make_shared<ClangLookupFnResolver<LegacyLookupFn>>(
      ES, std::move(LegacyLookup), std::move(ErrorReporter));
}

class ClangJIT {
public:
  using ObjLayerT = llvm::orc::LegacyRTDyldObjectLinkingLayer;
  using CompileLayerT = llvm::orc::LegacyIRCompileLayer<ObjLayerT, llvm::orc::SimpleCompiler>;

  ClangJIT(DenseMap<StringRef, const void *> &LocalSymAddrs)
      : LocalSymAddrs(LocalSymAddrs),
        Resolver(createClangLookupResolver(
            ES,
            [this](const std::string &Name) {
              return findMangledSymbol(Name);
            },
            [](Error Err) { cantFail(std::move(Err), "lookupFlags failed"); })),
        TM(EngineBuilder().selectTarget()), DL(TM->createDataLayout()),
        ObjectLayer(ES,
                    [this](llvm::orc::VModuleKey) {
                      return ObjLayerT::Resources{
                          std::make_shared<SectionMemoryManager>(), Resolver};
                    }),
        CompileLayer(ObjectLayer, llvm::orc::SimpleCompiler(*TM)),
        CXXRuntimeOverrides(
            [this](const std::string &S) { return mangle(S); }) {
    llvm::sys::DynamicLibrary::LoadLibraryPermanently(nullptr);
  }

  ~ClangJIT() {
    // Run any destructors registered with __cxa_atexit.
    CXXRuntimeOverrides.runDestructors();

    // Run any IR destructors.
    for (auto &DtorRunner : IRStaticDestructorRunners)
      cantFail(DtorRunner.runViaLayer(CompileLayer));
  }

  llvm::TargetMachine &getTargetMachine() { return *TM; }

  llvm::orc::VModuleKey addModule(std::unique_ptr<llvm::Module> M) {
    // Record the static constructors and destructors. We have to do this before
    // we hand over ownership of the module to the JIT.
    std::vector<std::string> CtorNames, DtorNames;
    for (auto Ctor : llvm::orc::getConstructors(*M))
      if (Ctor.Func && !Ctor.Func->hasAvailableExternallyLinkage())
        CtorNames.push_back(mangle(Ctor.Func->getName()));
    for (auto Dtor : llvm::orc::getDestructors(*M))
      if (Dtor.Func && !Dtor.Func->hasAvailableExternallyLinkage())
        DtorNames.push_back(mangle(Dtor.Func->getName()));

    auto K = ES.allocateVModule();
    cantFail(CompileLayer.addModule(K, std::move(M)));
    ModuleKeys.push_back(K);

    // Run the static constructors, and save the static destructor runner for
    // execution when the JIT is torn down.
    llvm::orc::LegacyCtorDtorRunner<CompileLayerT>
      CtorRunner(std::move(CtorNames), K);
    if (auto Err = CtorRunner.runViaLayer(CompileLayer)) {
      llvm::errs() << Err << "\n";
      fatal();
    }

    IRStaticDestructorRunners.emplace_back(std::move(DtorNames), K);

    return K;
  }

  void removeModule(llvm::orc::VModuleKey K) {
    ModuleKeys.erase(find(ModuleKeys, K));
    cantFail(CompileLayer.removeModule(K));
  }

  llvm::JITSymbol findSymbol(const std::string Name) {
    return findMangledSymbol(mangle(Name));
  }

private:
  std::string mangle(const std::string &Name) {
    std::string MangledName;
    {
      llvm::raw_string_ostream MangledNameStream(MangledName);
      llvm::Mangler::getNameWithPrefix(MangledNameStream, Name, DL);
    }
    return MangledName;
  }

  llvm::JITSymbol findMangledSymbol(const std::string &Name) {
    for (auto H : make_range(ModuleKeys.rbegin(), ModuleKeys.rend()))
      if (auto Sym = CompileLayer.findSymbolIn(H, Name,
                                               /*ExportedSymbolsOnly*/ false))
        return Sym;

    if (auto Sym = CXXRuntimeOverrides.searchOverrides(Name))
      return Sym;

    auto LSAI = LocalSymAddrs.find(Name);
    if (LSAI != LocalSymAddrs.end())
      return llvm::JITSymbol(llvm::pointerToJITTargetAddress(LSAI->second),
                             llvm::JITSymbolFlags::Exported);

    // If we can't find the symbol in the JIT, try looking in the host process.
    if (auto SymAddr = RTDyldMemoryManager::getSymbolAddressInProcess(Name))
      return llvm::JITSymbol(SymAddr, llvm::JITSymbolFlags::Exported);

#ifdef _WIN32
    // For Windows retry without "_" at beginning, as RTDyldMemoryManager uses
    // GetProcAddress and standard libraries like msvcrt.dll use names
    // with and without "_" (for example "_itoa" but "sin").
    if (Name.length() > 2 && Name[0] == '_')
      if (auto SymAddr =
              RTDyldMemoryManager::getSymbolAddressInProcess(Name.substr(1)))
        return llvm::JITSymbol(SymAddr, llvm::JITSymbolFlags::Exported);
#endif

    return nullptr;
  }

  DenseMap<StringRef, const void *> &LocalSymAddrs; 
  llvm::orc::ExecutionSession ES;
  std::shared_ptr<llvm::orc::SymbolResolver> Resolver;
  std::unique_ptr<llvm::TargetMachine> TM;
  const llvm::DataLayout DL;
  ObjLayerT ObjectLayer;
  CompileLayerT CompileLayer;
  std::vector<llvm::orc::VModuleKey> ModuleKeys;

  llvm::orc::LegacyLocalCXXRuntimeOverrides CXXRuntimeOverrides;
  std::vector<llvm::orc::LegacyCtorDtorRunner<CompileLayerT>>
    IRStaticDestructorRunners;
};

class BackendConsumer : public ASTConsumer {
  DiagnosticsEngine &Diags;
  BackendAction Action;
  const HeaderSearchOptions &HeaderSearchOpts;
  const CodeGenOptions &CodeGenOpts;
  const clang::TargetOptions &TargetOpts;
  const LangOptions &LangOpts;
  std::unique_ptr<raw_pwrite_stream> AsmOutStream;
  ASTContext *Context;
  std::string InFile;
  const PreprocessorOptions &PPOpts;
  LLVMContext &C;
  std::vector<std::unique_ptr<llvm::Module>> &DevLinkMods;
  CoverageSourceInfo *CoverageInfo;

  std::unique_ptr<CodeGenerator> Gen;

  void replaceGenerator() {
    Gen.reset(CreateLLVMCodeGen(Diags, InFile, HeaderSearchOpts, PPOpts,
                                CodeGenOpts, C, CoverageInfo));
  }

public:
  BackendConsumer(BackendAction Action, DiagnosticsEngine &Diags,
                  const HeaderSearchOptions &HeaderSearchOpts,
                  const PreprocessorOptions &PPOpts,
                  const CodeGenOptions &CodeGenOpts,
                  const clang::TargetOptions &TargetOpts,
                  const LangOptions &LangOpts, bool TimePasses,
                  const std::string &InFile,
                  std::unique_ptr<raw_pwrite_stream> OS, LLVMContext &C,
                  std::vector<std::unique_ptr<llvm::Module>> &DevLinkMods,
                  CoverageSourceInfo *CoverageInfo = nullptr)
      : Diags(Diags), Action(Action), HeaderSearchOpts(HeaderSearchOpts),
        CodeGenOpts(CodeGenOpts), TargetOpts(TargetOpts), LangOpts(LangOpts),
        AsmOutStream(std::move(OS)), Context(nullptr), InFile(InFile),
        PPOpts(PPOpts), C(C), DevLinkMods(DevLinkMods),
        CoverageInfo(CoverageInfo) { }

  llvm::Module *getModule() const { return Gen->GetModule(); }
  std::unique_ptr<llvm::Module> takeModule() {
    return std::unique_ptr<llvm::Module>(Gen->ReleaseModule());
  }

  CodeGenerator *getCodeGenerator() { return Gen.get(); }

  void HandleCXXStaticMemberVarInstantiation(VarDecl *VD) override {
    
    VD->dump();
    Gen->HandleCXXStaticMemberVarInstantiation(VD);
  }

  void Initialize(ASTContext &Ctx) override {
    replaceGenerator();
    Context = &Ctx;
    Gen->Initialize(Ctx);
  }

  bool HandleTopLevelDecl(DeclGroupRef D) override {
    Gen->HandleTopLevelDecl(D);
    return true;
  }

  void HandleInlineFunctionDefinition(FunctionDecl *D) override {
    Gen->HandleInlineFunctionDefinition(D);
  }

  void HandleInterestingDecl(DeclGroupRef D) override {
    HandleTopLevelDecl(D);
  }

  void HandleTranslationUnit(ASTContext &C) override {
      Gen->HandleTranslationUnit(C);

    // Silently ignore if we weren't initialized for some reason.
    if (!getModule())
      return;

    for (auto &BM : DevLinkMods) {
      std::unique_ptr<llvm::Module> M = llvm::CloneModule(*BM);
      M->setDataLayout(getModule()->getDataLayoutStr());
      M->setTargetTriple(getModule()->getTargetTriple());

      for (Function &F : *M)
        Gen->CGM().AddDefaultFnAttrs(F);

      bool Err = Linker::linkModules(
              *getModule(), std::move(M), llvm::Linker::Flags::LinkOnlyNeeded,
              [](llvm::Module &M, const llvm::StringSet<> &GVS) {
                internalizeModule(M, [&GVS](const llvm::GlobalValue &GV) {
                  return !GV.hasName() || (GVS.count(GV.getName()) == 0);
                });
              });

      if (Err)
        fatal();
    }

  }

  void HandleTagDeclDefinition(TagDecl *D) override {
    Gen->HandleTagDeclDefinition(D);
  }

  void HandleTagDeclRequiredDefinition(const TagDecl *D) override {
    Gen->HandleTagDeclRequiredDefinition(D);
  }

  void CompleteTentativeDefinition(VarDecl *D) override {
    Gen->CompleteTentativeDefinition(D);
  }

  void AssignInheritanceModel(CXXRecordDecl *RD) override {
    Gen->AssignInheritanceModel(RD);
  }

  void HandleVTable(CXXRecordDecl *RD) override {
    Gen->HandleVTable(RD);
  }

  void EmitOptimized() {
    EmitBackendOutput(Diags, HeaderSearchOpts, CodeGenOpts, TargetOpts,
                      LangOpts, Context->getTargetInfo().getDataLayout(),
                      getModule(), Action,
                      llvm::make_unique<llvm::buffer_ostream>(*AsmOutStream));
  }
};
template<typename T>
class AutotuneInfo
{
  public :

  int nCalls;
  int sizeOfTable;
  int idAT;
  int idParameter;
  std::string variableName;
  QualType varTypeAutotune;
  T* Values;
  long long *rtdscValues;
  bool toOptimize;
  AutotuneInfo(QualType varTypeAutotune,int idAT)
  {
    this->nCalls = 0;
    this->toOptimize = true;
    this->idAT = idAT;
    this->varTypeAutotune = varTypeAutotune;
    this->sizeOfTable = 0;
  }
  void allocRtdscValues()
  {
    
    this->rtdscValues =  (long long *) malloc(this->sizeOfTable * sizeof(long long));
    this->idParameter = 0;
    this->nCalls++;
  }
  void updateRtdscValues(const char* filename)
  {
    FILE* rtdscCompteur = fopen(filename,"r");
    
    long long valueRTDSC;
    fscanf(rtdscCompteur, "%lld", &valueRTDSC);
    //Fonction attribution du i_parameter
    this->rtdscValues[this->nCalls-1] = valueRTDSC;
    this->idParameter++;
    if(this->nCalls >= this->sizeOfTable)
    {
      
      this->toOptimize = false;
      this->idParameter = 0;
      long long min = this->rtdscValues[0];
      for(int k = 1; k < this->sizeOfTable; k++)
      {
        if(min > this->rtdscValues[k])
        {
            this->idParameter = k;
            min = this->rtdscValues[k];
        }
      }
      std::cout << "Final MIN :\n";
    }
    std::cout << "Current id Param:" << this->idParameter << "\n";    
    this->nCalls++;
  }
  void readElements(VarDecl* TAB,unsigned ElemSize,llvm::APInt IntVal)
  {
    if (const auto *CAT = dyn_cast<ConstantArrayType>(TAB->getType())) 
    {
      
      auto const Sz = CAT->getSize();
      this->sizeOfTable = Sz.getZExtValue();

      this->Values = new llvm::APInt[Sz.getZExtValue()];  
      unsigned ElemNumIntWords = llvm::alignTo<8>(ElemSize);

      unsigned NumIntWords = llvm::alignTo<8>(Sz.getZExtValue());
      SmallVector<uint64_t, 2> IntWords(NumIntWords, 0);
      
      const char *Elem = (const char *) IntVal.getZExtValue();
      for (unsigned i = 0; i < Sz.getZExtValue(); ++i) {

        SmallVector<uint64_t, 2> ElemIntWords(ElemNumIntWords, 0);
    
        std::memcpy((char *) ElemIntWords.data(), Elem, ElemSize);
       
        Elem += ElemSize;
        
        llvm::APInt ElemVal(ElemSize*8, ElemIntWords);
        
        this->Values[i] = ElemVal; 
      }
    }
  }
  void addLLVMInstructions(const char* filename,std::string SMName,BackendConsumer* Consumer)
  {
    llvm::Function *myFunction = Consumer->getModule()->getFunction(SMName);
    auto* module = Consumer->getModule();
    if (!myFunction) {
    llvm::errs() << "La fonction 'my_function' n'a pas été trouvée.\n";
    
    }
    else
    {
      llvm::IRBuilder<> builder(Consumer->getModule()->getContext());
      
      llvm::BasicBlock* entryBlock = &myFunction->getEntryBlock();
      llvm::BasicBlock* lastBlock = &myFunction->back();

      // Définit le point d'insertion au début du bloc d'entrée
      builder.SetInsertPoint(entryBlock, entryBlock->getFirstInsertionPt());
      
      
      Value *toAutotune = module->getNamedGlobal(this->variableName);
      
      llvm::PointerType* PointerType = cast<llvm::PointerType>(toAutotune->getType());
      llvm::PointerType* ElementType = cast<llvm::PointerType>(PointerType->getElementType());
      llvm::Type* finalType = ElementType->getElementType();
      AllocaInst* pointerAlloca = builder.CreateAlloca(finalType,nullptr, "pointeur");
      
      builder.CreateStore(pointerAlloca, toAutotune);
      Value* actualValue = builder.CreateLoad(toAutotune);
      
      Value* newValue = ConstantInt::get(finalType, this->Values[this->idParameter]);
      
      builder.CreateStore(newValue,actualValue);
      if(this->toOptimize)
      {
        llvm::FunctionType* rdtscFuncType = llvm::FunctionType::get(llvm::Type::getInt64Ty(Consumer->getModule()->getContext()), false);
        llvm::FunctionCallee rdtscFunc = module->getOrInsertFunction("llvm.x86.rdtsc", rdtscFuncType);

        llvm::Value* rdtscResult = builder.CreateCall(rdtscFunc, {});
        
        builder.SetInsertPoint(lastBlock->getTerminator());
      
        llvm::Value* rdtscResultBis = builder.CreateCall(rdtscFunc);
        llvm::Value *timeDifference = builder.CreateSub(rdtscResultBis, rdtscResult);
        
        llvm::FunctionType* fileOpenFuncType = llvm::FunctionType::get(llvm::Type::getInt8PtrTy(module->getContext()), {llvm::Type::getInt8PtrTy(module->getContext()), llvm::Type::getInt8PtrTy(module->getContext())}, false);
        llvm::FunctionCallee fileOpenFunc = module->getOrInsertFunction("fopen", fileOpenFuncType);
        llvm::Value* filenameStr = builder.CreateGlobalStringPtr(filename, "filenameStr");
        llvm::Value* modeStr = builder.CreateGlobalStringPtr("w", "modeStr");

        llvm::Value* file = builder.CreateCall(fileOpenFunc, {filenameStr, modeStr}, "file");

        llvm::FunctionCallee fprintfFunc = module->getOrInsertFunction("fprintf", llvm::FunctionType::get(llvm::Type::getInt32Ty(module->getContext()), {llvm::Type::getInt8PtrTy(module->getContext()), llvm::Type::getInt8PtrTy(module->getContext())}, true));
        auto formatConstant = llvm::ConstantDataArray::getString(module->getContext(), "%lld\n", true);
        llvm::GlobalVariable* formatStrVar = new llvm::GlobalVariable(*module, formatConstant->getType(), true, llvm::GlobalValue::PrivateLinkage, formatConstant, ".str");
        auto  formatStrPtr = llvm::ConstantExpr::getBitCast(formatStrVar, llvm::Type::getInt8PtrTy(module->getContext()));
        llvm::Value* printfArgsForFile[] = {file,formatStrPtr, timeDifference };

        builder.CreateCall(fprintfFunc, llvm::ArrayRef<llvm::Value*>(printfArgsForFile, 3));

        llvm::FunctionType* fileCloseFuncType = llvm::FunctionType::get(llvm::Type::getInt32Ty(module->getContext()), {llvm::Type::getInt8PtrTy(module->getContext())}, false);
        llvm::FunctionCallee fileCloseFunc = module->getOrInsertFunction("fclose", fileCloseFuncType);

        builder.CreateCall(fileCloseFunc, file);
      }
    } 
  }
};

llvm::DenseMap<const void *,AutotuneInfo<llvm::APInt>*> AutotuneInfoMap;
class JFIMapDeclVisitor : public RecursiveASTVisitor<JFIMapDeclVisitor> {
  DenseMap<unsigned, FunctionDecl *> &Map;

public:
  explicit JFIMapDeclVisitor(DenseMap<unsigned, FunctionDecl *> &M)
    : Map(M) { }

  bool shouldVisitTemplateInstantiations() const { return true; }

  bool VisitFunctionDecl(const FunctionDecl *D) {
    if (auto *A = D->getAttr<JITFuncInstantiationAttr>())
      Map[A->getId()] = const_cast<FunctionDecl *>(D);
    return true;
  }
};

class JFICSMapDeclVisitor : public RecursiveASTVisitor<JFICSMapDeclVisitor> {
  DenseMap<unsigned, FunctionDecl *> &Map;
  SmallVector<FunctionDecl *, 1> CurrentFD;

public:
  explicit JFICSMapDeclVisitor(DenseMap<unsigned, FunctionDecl *> &M)
    : Map(M) { }

  bool TraverseFunctionDecl(FunctionDecl *FD) {
    CurrentFD.push_back(FD);
    bool Continue =
      RecursiveASTVisitor<JFICSMapDeclVisitor>::TraverseFunctionDecl(FD);
    CurrentFD.pop_back();

    return Continue;
  }

  bool VisitDeclRefExpr(DeclRefExpr *E) {
    auto *FD = dyn_cast<FunctionDecl>(E->getDecl());
    if (!FD)
      return true;

    auto *A = FD->getAttr<JITFuncInstantiationAttr>();
    if (!A)
      return true;

    Map[A->getId()] = CurrentFD.back();

    return true;
  }
};

unsigned LastUnique = 0;
std::unique_ptr<llvm::LLVMContext> LCtx;

bool InitializedDevTarget = false;

struct DevFileData {
  const char *Filename;
  const void *Data;
  size_t DataSize;
};

struct DevData {
  const char *Triple;
  const char *Arch;
  const char *ASTBuffer;
  size_t ASTBufferSize;
  const void *CmdArgs;
  size_t CmdArgsLen;
  DevFileData *FileData;
  size_t FileDataCnt;
};

struct CompilerData {
  std::unique_ptr<CompilerInvocation>     Invocation;
  std::unique_ptr<llvm::opt::OptTable>    Opts;
  IntrusiveRefCntPtr<DiagnosticOptions>   DiagOpts;
  std::unique_ptr<TextDiagnosticPrinter>  DiagnosticPrinter;
  llvm::IntrusiveRefCntPtr<llvm::vfs::InMemoryFileSystem> InMemoryFileSystem;
  IntrusiveRefCntPtr<DiagnosticsEngine>   Diagnostics;
  IntrusiveRefCntPtr<FileManager>         FileMgr;
  IntrusiveRefCntPtr<SourceManager>       SourceMgr;
  IntrusiveRefCntPtr<MemoryBufferCache>   PCMCache;
  std::unique_ptr<HeaderSearch>           HeaderInfo;
  std::unique_ptr<PCHContainerReader>     PCHContainerRdr;
  IntrusiveRefCntPtr<TargetInfo>          Target;
  std::shared_ptr<Preprocessor>           PP;
  IntrusiveRefCntPtr<ASTContext>          Ctx;
  std::shared_ptr<clang::TargetOptions>   TargetOpts;
  std::shared_ptr<HeaderSearchOptions>    HSOpts;
  std::shared_ptr<PreprocessorOptions>    PPOpts;
  IntrusiveRefCntPtr<ASTReader>           Reader;
  BackendConsumer*        Consumer;
  std::unique_ptr<Sema>                   S;
  TrivialModuleLoader                     ModuleLoader;
  std::unique_ptr<llvm::Module>           RunningMod;

  DenseMap<StringRef, const void *>       LocalSymAddrs;
  DenseMap<StringRef, ValueDecl *>        NewLocalSymDecls;
  std::unique_ptr<ClangJIT>               CJ;

  DenseMap<unsigned, FunctionDecl *>      FuncMap;
  const void* ASTBuffer; 
  
  // A map of each instantiation to the containing function. These might not be
  // unique, but should be unique for any place where it matters
  // (instantiations with from-string types).
  DenseMap<unsigned, FunctionDecl *>      CSFuncMap;

  std::unique_ptr<CompilerData>           DevCD;
  SmallString<1>                          DevAsm;
  std::vector<std::unique_ptr<llvm::Module>> DevLinkMods;
  std::string name_to_autotune;
  

  CompilerData(const void *CmdArgs, unsigned CmdArgsLen,
               const void *ASTBuffer, size_t ASTBufferSize,
               const void *IRBuffer, size_t IRBufferSize,
               const void **LocalPtrs, unsigned LocalPtrsCnt,
               const void **LocalDbgPtrs, unsigned LocalDbgPtrsCnt,
               const DevData *DeviceData, unsigned DevCnt,
               int ForDev = -1) {
    this->ASTBuffer = ASTBuffer;
    bool IsForDev = (ForDev != -1);

    StringRef CombinedArgv((const char *) CmdArgs, CmdArgsLen);
    SmallVector<StringRef, 32> Argv;
    CombinedArgv.split(Argv, '\0', /*MaxSplit*/ -1, false);

    llvm::opt::ArgStringList CC1Args;
    for (auto &ArgStr : Argv)
    {  
      CC1Args.push_back(ArgStr.begin());
    }
    unsigned MissingArgIndex, MissingArgCount;
    Opts = driver::createDriverOptTable();
    llvm::opt::InputArgList ParsedArgs = Opts->ParseArgs(
      CC1Args, MissingArgIndex, MissingArgCount);

    DiagOpts = new DiagnosticOptions();
    ParseDiagnosticArgs(*DiagOpts, ParsedArgs);
    DiagnosticPrinter.reset(new TextDiagnosticPrinter(
      llvm::errs(), &*DiagOpts));
    Diagnostics = new DiagnosticsEngine(
      IntrusiveRefCntPtr<DiagnosticIDs>(new DiagnosticIDs()), &*DiagOpts,
      DiagnosticPrinter.get(), false);

    // Note that LangOpts, TargetOpts can also be read from the AST, but
    // CodeGenOpts need to come from the stored command line.

    Invocation.reset(new CompilerInvocation);
    CompilerInvocation::CreateFromArgs(*Invocation,
                                 const_cast<const char **>(CC1Args.data()),
                                 const_cast<const char **>(CC1Args.data()) +
                                 CC1Args.size(), *Diagnostics);
    Invocation->getFrontendOpts().DisableFree = false;
    Invocation->getCodeGenOpts().DisableFree = false;

    InMemoryFileSystem = new llvm::vfs::InMemoryFileSystem;
    FileMgr = new FileManager(FileSystemOptions(), InMemoryFileSystem);

    const char *Filename = "__clang_jit.pcm";
    StringRef ASTBufferSR((const char *) ASTBuffer, ASTBufferSize);
    InMemoryFileSystem->addFile(Filename, 0,
                                llvm::MemoryBuffer::getMemBufferCopy(ASTBufferSR));

    PCHContainerRdr.reset(new RawPCHContainerReader);
    SourceMgr = new SourceManager(*Diagnostics, *FileMgr,
                                  /*UserFilesAreVolatile*/ false);
    PCMCache = new MemoryBufferCache;
    HSOpts = std::make_shared<HeaderSearchOptions>();
    HSOpts->ModuleFormat = PCHContainerRdr->getFormat();
    HeaderInfo.reset(new HeaderSearch(HSOpts,
                                      *SourceMgr,
                                      *Diagnostics,
                                      *Invocation->getLangOpts(),
                                      /*Target=*/nullptr));
    PPOpts = std::make_shared<PreprocessorOptions>();

    unsigned Counter;

    PP = std::make_shared<Preprocessor>(
        PPOpts, *Diagnostics, *Invocation->getLangOpts(),
        *SourceMgr, *PCMCache, *HeaderInfo, ModuleLoader,
        /*IILookup=*/nullptr,
        /*OwnsHeaderSearch=*/false);

    // For parsing type names in strings later, we'll need to have Preprocessor
    // keep the Lexer around even after it hits the end of the each file (used
    // for each type name).
    PP->enableIncrementalProcessing();

    Ctx = new ASTContext(*Invocation->getLangOpts(), *SourceMgr,
                         PP->getIdentifierTable(), PP->getSelectorTable(),
                         PP->getBuiltinInfo());

    Reader = new ASTReader(*PP, Ctx.get(), *PCHContainerRdr, {},
                           /*isysroot=*/"",
                           /*DisableValidation=*/ false,
                           /*AllowPCHWithCompilerErrors*/ false);

    Reader->setListener(llvm::make_unique<ASTInfoCollector>(
      *PP, Ctx.get(), *HSOpts, *PPOpts, *Invocation->getLangOpts(),
      TargetOpts, Target, Counter));

    Ctx->setExternalSource(Reader);

    switch (Reader->ReadAST(Filename, serialization::MK_MainFile,
                            SourceLocation(), ASTReader::ARR_None)) {
    case ASTReader::Success:
      break;

    case ASTReader::Failure:
    case ASTReader::Missing:
    case ASTReader::OutOfDate:
    case ASTReader::VersionMismatch:
    case ASTReader::ConfigurationMismatch:
    case ASTReader::HadErrors:
      Diagnostics->Report(diag::err_fe_unable_to_load_pch);
      fatal();
      return;
    }

    PP->setCounterValue(Counter);

    // Now that we've read the language options from the AST file, change the JIT mode.
    Invocation->getLangOpts()->setCPlusPlusJIT(LangOptions::JITMode::JM_IsJIT);

    // Keep externally available functions, etc.
    Invocation->getCodeGenOpts().PrepareForLTO = true;

    BackendAction BA = Backend_EmitNothing;
    std::unique_ptr<raw_pwrite_stream> OS(new llvm::raw_null_ostream);

    if (IsForDev) {
       BA = Backend_EmitAssembly;
       OS.reset(new raw_svector_ostream(DevAsm));
    }

    Consumer = new BackendConsumer(
        BA, *Diagnostics, Invocation->getHeaderSearchOpts(),
        Invocation->getPreprocessorOpts(), Invocation->getCodeGenOpts(),
        Invocation->getTargetOpts(), *Invocation->getLangOpts(), false, Filename,
        std::move(OS), *LCtx, DevLinkMods);

    // Create a semantic analysis object and tell the AST reader about it.
    S.reset(new Sema(*PP, *Ctx, *Consumer));
    S->Initialize();
    Reader->InitializeSema(*S);

    // Tell the diagnostic client that we have started a source file.
    Diagnostics->getClient()->BeginSourceFile(PP->getLangOpts(), PP.get());

    JFIMapDeclVisitor(FuncMap).TraverseAST(*Ctx);
    JFICSMapDeclVisitor(CSFuncMap).TraverseAST(*Ctx);

    if (IRBufferSize) {
      llvm::SMDiagnostic Err;
      StringRef IRBufferSR((const char *) IRBuffer, IRBufferSize);
      RunningMod = parseIR(
        *llvm::MemoryBuffer::getMemBufferCopy(IRBufferSR), Err, *LCtx);

      for (auto &F : RunningMod->functions())
        if (!F.isDeclaration())
          F.setLinkage(llvm::GlobalValue::AvailableExternallyLinkage);

      for (auto &GV : RunningMod->global_values())
        if (!GV.isDeclaration()) {
          if (GV.hasAppendingLinkage())
            cast<GlobalVariable>(GV).setInitializer(nullptr);
          else if (isa<GlobalAlias>(GV))
            // Aliases cannot have externally-available linkage, so give them
            // private linkage.
            GV.setLinkage(llvm::GlobalValue::PrivateLinkage);
          else
            GV.setLinkage(llvm::GlobalValue::AvailableExternallyLinkage);
        }
    }

    Consumer->Initialize(*Ctx);
    
    for (unsigned Idx = 0; Idx < 2*LocalPtrsCnt; Idx += 2) {
      const char *Name = (const char *) LocalPtrs[Idx];
      
      const void *Ptr = LocalPtrs[Idx+1];
      LocalSymAddrs[Name] = Ptr;
    }

    for (unsigned Idx = 0; Idx < 2*LocalDbgPtrsCnt; Idx += 2) {
      const char *Name = (const char *) LocalDbgPtrs[Idx];
      const void *Ptr = LocalDbgPtrs[Idx+1];
      LocalSymAddrs[Name] = Ptr;
    }

    if (!IsForDev)
      CJ = llvm::make_unique<ClangJIT>(LocalSymAddrs);

    if (IsForDev)
      for (unsigned i = 0; i < DeviceData[ForDev].FileDataCnt; ++i) {
        StringRef FileBufferSR(
                    (const char *) DeviceData[ForDev].FileData[i].Data,
                    DeviceData[ForDev].FileData[i].DataSize);

        llvm::SMDiagnostic Err;
        DevLinkMods.push_back(parseIR(
          *llvm::MemoryBuffer::getMemBufferCopy(FileBufferSR), Err, *LCtx));
      }

    if (!IsForDev && Invocation->getLangOpts()->CUDA) {
      typedef int (*cudaGetDevicePtr)(int *);
      auto cudaGetDevice =
        (cudaGetDevicePtr) RTDyldMemoryManager::getSymbolAddressInProcess(
                                                     "cudaGetDevice");
      if (!cudaGetDevice) {
        llvm::errs() << "Could not find CUDA API functions; "
                        "did you forget to link with -lcudart?\n";
        fatal();
      }

      typedef int (*cudaGetDeviceCountPtr)(int *);
      auto cudaGetDeviceCount =
        (cudaGetDeviceCountPtr) RTDyldMemoryManager::getSymbolAddressInProcess(
                                                     "cudaGetDeviceCount");

      int SysDevCnt;
      if (cudaGetDeviceCount(&SysDevCnt)) {
        llvm::errs() << "Failed to get CUDA device count!\n";
        fatal();
      }

      typedef int (*cudaDeviceGetAttributePtr)(int *, int, int);
      auto cudaDeviceGetAttribute =
        (cudaDeviceGetAttributePtr) RTDyldMemoryManager::getSymbolAddressInProcess(
                                      "cudaDeviceGetAttribute");

      if (SysDevCnt) {
        int CDev;
        if (cudaGetDevice(&CDev))
          fatal();

        int CLMajor, CLMinor;
        if (cudaDeviceGetAttribute(
              &CLMajor, /*cudaDevAttrComputeCapabilityMajor*/ 75, CDev))
          fatal();
        if (cudaDeviceGetAttribute(
              &CLMinor, /*cudaDevAttrComputeCapabilityMinor*/ 76, CDev))
          fatal();

        SmallString<6> EffArch;
        raw_svector_ostream(EffArch) << "sm_" << CLMajor << CLMinor;

        SmallVector<StringRef, 2> DevArchs;
        for (unsigned i = 0; i < DevCnt; ++i) {
          if (!Triple(DeviceData[i].Triple).isNVPTX())
            continue;
          if (!StringRef(DeviceData[i].Arch).startswith("sm_"))
            continue;
          DevArchs.push_back(DeviceData[i].Arch);
        }

        std::sort(DevArchs.begin(), DevArchs.end());
        auto ArchI =
          std::upper_bound(DevArchs.begin(), DevArchs.end(), EffArch);
        if (ArchI == DevArchs.begin()) {
          llvm::errs() << "No JIT device configuration supports " <<
                          EffArch << "\n";
          fatal();
        }

        auto BestDevArch = *--ArchI;
        int BestDevIdx = 0;
        for (; BestDevIdx < (int) DevCnt; ++BestDevIdx) {
          if (!Triple(DeviceData[BestDevIdx].Triple).isNVPTX())
            continue;
          if (DeviceData[BestDevIdx].Arch == BestDevArch)
            break;
        }

        assert(BestDevIdx != (int) DevCnt && "Didn't find the chosen device data?");

        if (!InitializedDevTarget) {
          // In theory, we only need to initialize the NVPTX target here,
          // however, there doesn't seem to be any good way to know if the
          // NVPTX target is enabled.
          //
          // LLVMInitializeNVPTXTargetInfo();
          // LLVMInitializeNVPTXTarget();
          // LLVMInitializeNVPTXTargetMC();
          // LLVMInitializeNVPTXAsmPrinter();

          llvm::InitializeAllTargets();
          llvm::InitializeAllTargetMCs();
          llvm::InitializeAllAsmPrinters();

          InitializedDevTarget = true;
        }

        DevCD.reset(new CompilerData(
            DeviceData[BestDevIdx].CmdArgs, DeviceData[BestDevIdx].CmdArgsLen,
            DeviceData[BestDevIdx].ASTBuffer, DeviceData[BestDevIdx].ASTBufferSize,
            nullptr, 0, nullptr, 0, nullptr, 0, DeviceData, DevCnt, BestDevIdx));
      }
    }
  }

  void restoreFuncDeclContext(FunctionDecl *FunD) {
    // NOTE: This mirrors the corresponding code in
    // Parser::ParseLateTemplatedFuncDef (which is used to late parse a C++
    // function template in Microsoft mode).

    struct ContainingDC {
      ContainingDC(DeclContext *DC, bool ShouldPush) : Pair(DC, ShouldPush) {}
      llvm::PointerIntPair<DeclContext *, 1, bool> Pair;
      DeclContext *getDC() { return Pair.getPointer(); }
      bool shouldPushDC() { return Pair.getInt(); }
    };

    SmallVector<ContainingDC, 4> DeclContextsToReenter;
    DeclContext *DD = FunD;
    DeclContext *NextContaining = S->getContainingDC(DD);
    while (DD && !DD->isTranslationUnit()) {
      bool ShouldPush = DD == NextContaining;
      DeclContextsToReenter.push_back({DD, ShouldPush});
      if (ShouldPush)
        NextContaining = S->getContainingDC(DD);
      DD = DD->getLexicalParent();
    }

    // Reenter template scopes from outermost to innermost.
    for (ContainingDC CDC : reverse(DeclContextsToReenter)) {
      (void) S->ActOnReenterTemplateScope(S->getCurScope(),
                                           cast<Decl>(CDC.getDC()));
      if (CDC.shouldPushDC())
        S->PushDeclContext(S->getCurScope(), CDC.getDC());
    }
  }

  std::string instantiateTemplate(const void *NTTPValues, const char **TypeStrings,
                                  unsigned Idx) {
    
    FunctionDecl *FD = FuncMap[Idx];
    if (!FD)
      fatal();
    FunctionTemplateDecl *FTDBis = FD->getPrimaryTemplate();
    TemplateParameterList* TPL = FTDBis->getTemplateParameters();
    
    
    int compteur = 0;
    int idAT = -1;
    QualType autotunedType;
    
    
    {
      for (TemplateParameterList::iterator it = TPL->begin(); it != TPL->end(); ++it) {
          
          NamedDecl* param = *it;

          if(ValueDecl* valueDecl = dyn_cast<ValueDecl>(param))
          {
            if(valueDecl->getType().isAutotuneQualified())
            {
              if(AutotuneInfoMap.find(ASTBuffer) == AutotuneInfoMap.end())
              {
                
                AutotuneInfo<llvm::APInt>* AutotuneActuel = new AutotuneInfo<llvm::APInt>(valueDecl->getType(),idAT);
                autotunedType = valueDecl->getType();
                idAT = compteur;
                AutotuneInfoMap[ASTBuffer] = AutotuneActuel;
              }
              else
              {
                AutotuneInfoMap[ASTBuffer]->varTypeAutotune = valueDecl->getType();
              }
            }
            

          }
        compteur ++;
        }

    }
    
    RecordDecl *RD =
      Ctx->buildImplicitRecord(llvm::Twine("__clang_jit_args_")
                               .concat(llvm::Twine(Idx))
                               .concat(llvm::Twine("_t"))
                               .str());

    RD->startDefinition();

    enum TASaveKind {
      TASK_None,
      TASK_Type,
      TASK_Value
    };

    SmallVector<TASaveKind, 8> TAIsSaved;

    auto *FTSI = FD->getTemplateSpecializationInfo();
    
    
    for (auto &TA : FTSI->TemplateArguments->asArray()) {
      

      auto HandleTA = [&](const TemplateArgument &TA) {
        if (TA.getKind() == TemplateArgument::Type)
          if (TA.getAsType()->isJITFromStringType()) {
            TAIsSaved.push_back(TASK_Type);
            return;
          }

        if (TA.getKind() != TemplateArgument::Expression) {
          TAIsSaved.push_back(TASK_None);
          return;
        }

        SmallVector<PartialDiagnosticAt, 8> Notes;
        Expr::EvalResult Eval;
        Eval.Diag = &Notes;
        
        if (TA.getAsExpr()->
              EvaluateAsConstantExpr(Eval, Expr::EvaluateForMangling, *Ctx)) {
          TAIsSaved.push_back(TASK_None);
          return;
        }
        
        QualType FieldTy = TA.getNonTypeTemplateArgumentType();
        if(compteur == idAT)
        {
       
          QualType SubFieldTy = FieldTy.getTypePtr()->getPointeeType();
          if(dyn_cast<ConstantArrayType>(SubFieldTy))
          {
            
          FieldTy = AutotuneInfoMap[ASTBuffer]->varTypeAutotune;
          }
        }
        auto *Field = FieldDecl::Create(
            *Ctx, RD, SourceLocation(), SourceLocation(), /*Id=*/nullptr,
            FieldTy, Ctx->getTrivialTypeSourceInfo(FieldTy, SourceLocation()),
            /*BW=*/nullptr, /*Mutable=*/false, /*InitStyle=*/ICIS_NoInit);
        Field->setAccess(AS_public);
        RD->addDecl(Field);
        
        //Field->print(llvm::outs());
        
        
        TAIsSaved.push_back(TASK_Value);
      };

      if (TA.getKind() == TemplateArgument::Pack) {
        for (auto &PTA : TA.getPackAsArray())
          HandleTA(PTA);
        continue;
      }

      HandleTA(TA);
      
    }

    RD->completeDefinition();
    RD->addAttr(PackedAttr::CreateImplicit(*Ctx));
    
    const ASTRecordLayout &RLayout = Ctx->getASTRecordLayout(RD);
    assert(Ctx->getCharWidth() == 8 && "char is not 8 bits!");

    QualType RDTy = Ctx->getRecordType(RD);
    auto Fields = cast<RecordDecl>(RDTy->getAsTagDecl())->field_begin();
    
    
    SmallVector<TemplateArgument, 8> Builder;

    unsigned TAIdx = 0, TSIdx = 0;
    for (auto &TA : FTSI->TemplateArguments->asArray()) {
       
      auto HandleTA = [&](const TemplateArgument &TA,
                          SmallVector<TemplateArgument, 8> &Builder) {
        
      
        if (TAIsSaved[TAIdx] == TASK_Type) {
          PP->ResetForJITTypes();

          PP->setPredefines(TypeStrings[TSIdx]);
          PP->EnterMainSourceFile();

          Parser P(*PP, *S, /*SkipFunctionBodies*/true, /*JITTypes*/true);

          // Reset this to nullptr so that when we call
          // Parser::Initialize it has the clean slate it expects.
          S->CurContext = nullptr;

          P.Initialize();

          Sema::ContextRAII TUContext(*S, Ctx->getTranslationUnitDecl());

          auto CSFMI = CSFuncMap.find(Idx);
          if (CSFMI != CSFuncMap.end()) {
	  // Note that this restores the Ctx->of the function in which the
	  // template was instantiated, but not the state *within* the
	  // function, so local types will remain unavailable.

            auto *FunD = CSFMI->second;
            restoreFuncDeclContext(FunD);
            S->CurContext = S->getContainingDC(FunD);
          }

          TypeResult TSTy = P.ParseTypeName();
          if (TSTy.isInvalid())
            fatal();

          QualType TypeFromString = Sema::GetTypeFromParser(TSTy.get());
          TypeFromString = Ctx->getCanonicalType(TypeFromString);

          Builder.push_back(TemplateArgument(TypeFromString));

          ++TSIdx;
          ++TAIdx;
          return;
        }

        if (TAIsSaved[TAIdx++] != TASK_Value) {
          Builder.push_back(TA);
          return;
        }

        assert(TA.getKind() == TemplateArgument::Expression &&
               "Only expressions template arguments handled here");

        QualType FieldTy = TA.getNonTypeTemplateArgumentType();

        assert(!FieldTy->isMemberPointerType() &&
               "Can't handle member pointers here without ABI knowledge");
        
        
        auto *Fld = *Fields++;
        unsigned Offset = RLayout.getFieldOffset(Fld->getFieldIndex()) / 8;
        unsigned Size = Ctx->getTypeSizeInChars(FieldTy).getQuantity();

        unsigned NumIntWords = llvm::alignTo<8>(Size);
        SmallVector<uint64_t, 2> IntWords(NumIntWords, 0);
        std::memcpy((char *) IntWords.data(),
                    ((const char *) NTTPValues) + Offset, Size);
        llvm::APInt IntVal(Size*8, IntWords);
         
        QualType CanonFieldTy = Ctx->getCanonicalType(FieldTy);

        if (FieldTy->isIntegralOrEnumerationType()) {
          
          llvm::APSInt SIntVal(IntVal,
                               FieldTy->isUnsignedIntegerOrEnumerationType());
          Builder.push_back(TemplateArgument(*Ctx, SIntVal, CanonFieldTy));
        } else {
          assert(FieldTy->isPointerType() || FieldTy->isReferenceType() ||
                 FieldTy->isNullPtrType());
          if (IntVal.isNullValue()) {
            Builder.push_back(TemplateArgument(CanonFieldTy, /*isNullPtr*/true));
          } else {
	  // Note: We always generate a new global for pointer values here.
	  // This provides a new potential way to introduce an ODR violation:
	  // If you also generate an instantiation using the same pointer value
	  // using some other symbol name, this will generate a different
	  // instantiation.

	  // As we guarantee that the template parameters are not allowed to
	  // point to subobjects, this is useful for optimization because each
	  // of these resolve to distinct underlying objects.

            llvm::SmallString<256> GlobalName("__clang_jit_symbol_");
            IntVal.toString(GlobalName, 16, false);

	  // To this base name we add the mangled type. Stack/heap addresses
	  // can be reused with variables of different type, and these should
	  // have different names even if they share the same address;
            auto &CGM = Consumer->getCodeGenerator()->CGM();
            llvm::raw_svector_ostream MOut(GlobalName);
            CGM.getCXXABI().getMangleContext().mangleTypeName(CanonFieldTy, MOut);

            auto NLDSI = NewLocalSymDecls.find(GlobalName);
            if (NLDSI != NewLocalSymDecls.end()) {
                Builder.push_back(TemplateArgument(NLDSI->second, CanonFieldTy));
            } else {
              Sema::ContextRAII TUContext(*S, Ctx->getTranslationUnitDecl());
              SourceLocation Loc = FTSI->getPointOfInstantiation();

              QualType STy = CanonFieldTy->getPointeeType();
              
              
              auto &II = PP->getIdentifierTable().get(GlobalName);

              if (STy->isFunctionType()) {
                auto *TAFD =
                  FunctionDecl::Create(*Ctx, S->CurContext, Loc, Loc, &II,
                                       STy, /*TInfo=*/nullptr, SC_Extern, false,
                                       STy->isFunctionProtoType());
                TAFD->setImplicit();

                if (const FunctionProtoType *FT = dyn_cast<FunctionProtoType>(STy)) {
                  SmallVector<ParmVarDecl*, 16> Params;
                  for (unsigned i = 0, e = FT->getNumParams(); i != e; ++i) {
                    ParmVarDecl *Parm =
                      ParmVarDecl::Create(*Ctx, TAFD, SourceLocation(), SourceLocation(),
                                          nullptr, FT->getParamType(i), /*TInfo=*/nullptr,
                                          SC_None, nullptr);
                    Parm->setScopeInfo(0, i);
                    Params.push_back(Parm);
                  }

                  TAFD->setParams(Params);
                }

                NewLocalSymDecls[II.getName()] = TAFD;
                Builder.push_back(TemplateArgument(TAFD, CanonFieldTy));
              } else {
                
                bool MadeArray = false;
                auto *TPL = FTSI->getTemplate()->getTemplateParameters();
                if (TPL->size() >= TAIdx) {
                  auto *Param = TPL->getParam(TAIdx-1);
                  if (NonTypeTemplateParmDecl *NTTP =
                        dyn_cast<NonTypeTemplateParmDecl>(Param)) {
                   
                    QualType OrigTy = NTTP->getType()->getPointeeType();
                    
                    OrigTy = OrigTy.getDesugaredType(*Ctx);
                    
               
                    bool IsArray = false;
                    llvm::APInt Sz;
                    QualType ElemTy;
                    if (const auto *DAT = dyn_cast<DependentSizedArrayType>(OrigTy)) {
                      Expr* SzExpr = DAT->getSizeExpr();
                      auto *NewTAL = TemplateArgumentList::CreateCopy(*Ctx, Builder);
                      MultiLevelTemplateArgumentList SubstArgs(*NewTAL);

                      SmallVector<Expr *, 1> NewSzExprVec;
                      if (!S->SubstExprs(SzExpr, /*IsCall*/ false, SubstArgs, NewSzExprVec)) {
                        Expr::EvalResult NewSzResult;
                        if (NewSzExprVec[0]->EvaluateAsInt(NewSzResult, *Ctx)) {
                          Sz = NewSzResult.Val.getInt();
                          ElemTy = DAT->getElementType();
                          IsArray = true;
                        }
                      }  
                    } else if (const auto *CAT = dyn_cast<ConstantArrayType>(OrigTy)) {
                      Sz = CAT->getSize();
                      ElemTy = CAT->getElementType();
                      IsArray = true;
                    }
                    
                      
                      
                    if (IsArray && (ElemTy->isIntegerType() ||
                                    ElemTy->isFloatingType())) {
                      
                      QualType ArrTy =
                        Ctx->getConstantArrayType(ElemTy,
                                                  Sz, clang::ArrayType::Normal, 0);

                      SmallVector<Expr *, 16> Vals;
                      unsigned ElemSize = Ctx->getTypeSizeInChars(ElemTy).getQuantity();
                      unsigned ElemNumIntWords = llvm::alignTo<8>(ElemSize);
                      const char *Elem = (const char *) IntVal.getZExtValue();
                      for (unsigned i = 0; i < Sz.getZExtValue(); ++i) {
                        SmallVector<uint64_t, 2> ElemIntWords(ElemNumIntWords, 0);

                        std::memcpy((char *) ElemIntWords.data(), Elem, ElemSize);
                        Elem += ElemSize;

                        llvm::APInt ElemVal(ElemSize*8, ElemIntWords);
                        
                        if (ElemTy->isIntegerType()) {
                          Vals.push_back(new (*Ctx) IntegerLiteral(
                            *Ctx, ElemVal, ElemTy, Loc));
                        } else {
                          llvm::APFloat ElemValFlt(Ctx->getFloatTypeSemantics(ElemTy), ElemVal);
                          Vals.push_back(FloatingLiteral::Create(*Ctx, ElemValFlt,
                                                                 false, ElemTy, Loc));
                        }
                      }
                      
                      InitListExpr *InitL = new (*Ctx) InitListExpr(*Ctx, Loc, Vals, Loc);
                      InitL->setType(ArrTy);

                      auto *TAVD =
                        VarDecl::Create(*Ctx, S->CurContext, Loc, Loc, &II,
                                        ArrTy, Ctx->getTrivialTypeSourceInfo(ArrTy, Loc),
                                        SC_Extern);
                      TAVD->setImplicit();
                      TAVD->setConstexpr(true);
                      TAVD->setInit(InitL);

                      NewLocalSymDecls[II.getName()] = TAVD;
                      Builder.push_back(TemplateArgument(TAVD, Ctx->getLValueReferenceType(ArrTy)));

                      MadeArray = true;
                    }
                  }
                } 

                if (!MadeArray) {
                  
                  
                                   
                  
                  auto *TAVD =
                    VarDecl::Create(*Ctx, S->CurContext, Loc, Loc, &II,
                                    AutotuneInfoMap[ASTBuffer]->varTypeAutotune, Ctx->getTrivialTypeSourceInfo(STy, Loc),
                                    SC_Extern);
                  
                  auto * TAB = VarDecl::Create(*Ctx, S->CurContext, Loc, Loc, &II,
                                    STy, Ctx->getTrivialTypeSourceInfo(STy, Loc),
                                    SC_Extern);
                  
                  
                  if(AutotuneInfoMap[ASTBuffer]->nCalls == 0)
                  {
                    
                    const auto *CAT = dyn_cast<ConstantArrayType>(TAB->getType());
                    auto ElemTy = CAT->getElementType();
                    
                    unsigned ElemSize = Ctx->getTypeSizeInChars(ElemTy).getQuantity();
                   
                    AutotuneInfoMap[ASTBuffer]->readElements(TAB,ElemSize,IntVal);
                    
                  }
                  
                  
                
                 
                  TAVD->setImplicit();
                  
                  
                  AutotuneInfoMap[ASTBuffer]->variableName = TAVD->getNameAsString();
                  NewLocalSymDecls[II.getName()] = TAVD;

                  CanonFieldTy = AutotuneInfoMap[ASTBuffer]->varTypeAutotune->getPointeeType()  ; 
                  Builder.push_back(TemplateArgument(TAVD, CanonFieldTy));
                  
                }
              }

              LocalSymAddrs[II.getName()] = (const void *) IntVal.getZExtValue();
            }
          }
        }
      };

      if (TA.getKind() == TemplateArgument::Pack) {
        SmallVector<TemplateArgument, 8> PBuilder;
        for (auto &PTA : TA.getPackAsArray())
          HandleTA(PTA, PBuilder);
        Builder.push_back(TemplateArgument::CreatePackCopy(*Ctx, PBuilder));
        continue;
      }

      HandleTA(TA, Builder);
    }

    SourceLocation Loc = FTSI->getPointOfInstantiation();
    auto *NewTAL = TemplateArgumentList::CreateCopy(*Ctx, Builder);
    clang::PrintingPolicy policy(clang::LangOptions());
    //policy.adjustForCPlusPlus();
    MultiLevelTemplateArgumentList SubstArgs(*NewTAL);

    auto *FunctionTemplate = FTSI->getTemplate();
    DeclContext *Owner = FunctionTemplate->getDeclContext();
    if (FunctionTemplate->getFriendObjectKind())
      Owner = FunctionTemplate->getLexicalDeclContext();

    std::string SMName;
    FunctionTemplateDecl *FTD = FTSI->getTemplate();
    sema::TemplateDeductionInfo Info(Loc);
    {
      Sema::InstantiatingTemplate Inst(
        *S, Loc, FTD, NewTAL->asArray(),
        Sema::CodeSynthesisContext::ExplicitTemplateArgumentSubstitution, Info);

      S->setCurScope(S->TUScope = new Scope(nullptr, Scope::DeclScope, PP->getDiagnostics()));

      Sema::ContextRAII TUContext(*S, Ctx->getTranslationUnitDecl());

      auto *Specialization = cast_or_null<FunctionDecl>(
        S->SubstDecl(FunctionTemplate->getTemplatedDecl(), Owner, SubstArgs));
      if (!Specialization || Specialization->isInvalidDecl())
        fatal();

      Specialization->setTemplateSpecializationKind(TSK_ExplicitInstantiationDefinition, Loc);
      S->InstantiateFunctionDefinition(Loc, Specialization, true, true, true);

      SMName = Consumer->getCodeGenerator()->CGM().getMangledName(Specialization);
    }

    if (Diagnostics->hasErrorOccurred())
      fatal();
    
    return SMName;
  }

  void emitAllNeeded(bool CheckExisting = true) {
    // There might have been functions/variables with local linkage that were
    // only used by JIT functions. These would not have been used during
    // initial code generation for this translation unit, and so not emitted.
    // We need to make sure that they're emited now (if they're now necessary).

    // Note that we skip having the code generator visiting the decl if it is
    // already defined or already present in our running module. Note that this
    // is not sufficient to prevent all redundant code generation (this might
    // also happen during the instantiation of the top-level function
    // template), and this is why we merge the running module into the new one
    // with the running-module overriding new entities.
    
    
    
    SmallSet<StringRef, 16> LastDeclNames;
    bool Changed;
    do {
      Changed = false;

      Consumer->getCodeGenerator()->CGM().EmitAllDeferred([&](GlobalDecl GD) {
        auto MName = Consumer->getCodeGenerator()->CGM().getMangledName(GD);
        if (!CheckExisting || !CJ->findSymbol(MName)) {
          Changed = true;
          return false;
        }

        return true;
      });

      SmallSet<StringRef, 16> DeclNames;
      for (auto &F : Consumer->getModule()->functions())
        if (F.isDeclaration() && !F.isIntrinsic())
          if (!LastDeclNames.count(F.getName()))
            DeclNames.insert(F.getName());

      for (auto &GV : Consumer->getModule()->global_values())
        if (GV.isDeclaration())
          if (!LastDeclNames.count(GV.getName()))
            DeclNames.insert(GV.getName());

      for (auto &DeclName : DeclNames) {
        if (CheckExisting && CJ->findSymbol(DeclName))
          continue;

        Decl *D = const_cast<Decl *>(Consumer->getCodeGenerator()->
                                       GetDeclForMangledName(DeclName));
        if (!D)
          continue;

        Consumer->HandleInterestingDecl(DeclGroupRef(D));
        LastDeclNames.insert(DeclName);
        Changed = true;
      }
    } while (Changed);
    
  }

  void *resolveFunction(const void *NTTPValues, const char **TypeStrings,
                        unsigned Idx) {
    std::string SMName = instantiateTemplate(NTTPValues, TypeStrings, Idx);
    
    if(AutotuneInfoMap[ASTBuffer]->nCalls == 0)
    {
      AutotuneInfoMap[ASTBuffer]->allocRtdscValues();
    }
    else if(AutotuneInfoMap[ASTBuffer]->toOptimize) 
    {
      
      AutotuneInfoMap[ASTBuffer]->updateRtdscValues("/tmp/rtdsc.txt");
    }
    
    AutotuneInfoMap[ASTBuffer]->addLLVMInstructions("/tmp/rtdsc.txt",SMName,Consumer);
    
    
    
    // Now we know the name of the symbol, check to see if we already have it.
    if (auto SpecSymbol = CJ->findSymbol(SMName))
      if (SpecSymbol.getAddress())
        return (void *) llvm::cantFail(SpecSymbol.getAddress());

    if (DevCD)
      DevCD->instantiateTemplate(NTTPValues, TypeStrings, Idx);

    emitAllNeeded();
    
    
    
    if (DevCD)
      DevCD->emitAllNeeded(false);

    // Before anything gets optimized, mark the top-level symbol we're
    // generating so that it doesn't get eliminated by the optimizer.

    auto *TopGV =
      cast<GlobalObject>(Consumer->getModule()->getNamedValue(SMName));
    assert(TopGV && "Didn't generate the desired top-level symbol?");

    TopGV->setLinkage(llvm::GlobalValue::ExternalLinkage);
    TopGV->setComdat(nullptr);

    // Finalize the module, generate module-level metadata, etc.

    if (DevCD) {
      DevCD->Consumer->HandleTranslationUnit(*DevCD->Ctx);
      DevCD->Consumer->EmitOptimized();

      // We have now created the PTX output, but what we really need as a
      // fatbin that the CUDA runtime will recognize.

      // The outer header of the fat binary is documented in the CUDA
      // fatbinary.h header. As mentioned there, the overall size must be a
      // multiple of eight, and so we must make sure that the PTX is.
      // We also need to make sure that the buffer is explicitly null
      // terminated (cuobjdump, at least, seems to assume that it is).
      DevCD->DevAsm += '\0';
      while (DevCD->DevAsm.size() % 8)
        DevCD->DevAsm += '\0';

      // NVIDIA, unfortunatly, does not provide full documentation on their
      // fatbin format. There is some information on the outer header block in
      // the CUDA fatbinary.h header. Also, it is possible to figure out more
      // about the format by creating fatbins using the provided utilities
      // and then observing what cuobjdump reports about the resulting files.
      // There are some other online references which shed light on the format,
      // including https://reviews.llvm.org/D8397 and FatBinaryContext.{cpp,h}
      // from the GPU Ocelot project (https://github.com/gtcasl/gpuocelot).

      SmallString<128> FatBin;
      llvm::raw_svector_ostream FBOS(FatBin);

      struct FatBinHeader {
        uint32_t Magic;      // 0x00
        uint16_t Version;    // 0x04
        uint16_t HeaderSize; // 0x06
        uint32_t DataSize;   // 0x08
        uint32_t unknown0c;  // 0x0c
      public:
        FatBinHeader(uint32_t DataSize)
            : Magic(0xba55ed50), Version(1),
              HeaderSize(sizeof(*this)), DataSize(DataSize), unknown0c(0) {}
      };

      enum FatBinFlags {
        AddressSize64 = 0x01,
        HasDebugInfo = 0x02,
        ProducerCuda = 0x04,
        HostLinux = 0x10,
        HostMac = 0x20,
        HostWindows = 0x40
      };

      struct FatBinFileHeader {
        uint16_t Kind;             // 0x00
        uint16_t unknown02;        // 0x02
        uint32_t HeaderSize;       // 0x04
        uint32_t DataSize;         // 0x08
        uint32_t unknown0c;        // 0x0c
        uint32_t CompressedSize;   // 0x10
        uint32_t SubHeaderSize;    // 0x14
        uint16_t VersionMinor;     // 0x18
        uint16_t VersionMajor;     // 0x1a
        uint32_t CudaArch;         // 0x1c
        uint32_t unknown20;        // 0x20
        uint32_t unknown24;        // 0x24
        uint32_t Flags;            // 0x28
        uint32_t unknown2c;        // 0x2c
        uint32_t unknown30;        // 0x30
        uint32_t unknown34;        // 0x34
        uint32_t UncompressedSize; // 0x38
        uint32_t unknown3c;        // 0x3c
        uint32_t unknown40;        // 0x40
        uint32_t unknown44;        // 0x44
        FatBinFileHeader(uint32_t DataSize, uint32_t CudaArch, uint32_t Flags)
            : Kind(1 /*PTX*/), unknown02(0x0101), HeaderSize(sizeof(*this)),
              DataSize(DataSize), unknown0c(0), CompressedSize(0),
              SubHeaderSize(HeaderSize - 8), VersionMinor(2), VersionMajor(4),
              CudaArch(CudaArch), unknown20(0), unknown24(0), Flags(Flags), unknown2c(0),
              unknown30(0), unknown34(0), UncompressedSize(0), unknown3c(0),
              unknown40(0), unknown44(0) {}
      };

      uint32_t CudaArch;
      StringRef(DevCD->Invocation->getTargetOpts().CPU)
        .drop_front(3 /*sm_*/).getAsInteger(10, CudaArch);

      uint32_t Flags = ProducerCuda;
      if (DevCD->Invocation->getCodeGenOpts().getDebugInfo() >=
            codegenoptions::LimitedDebugInfo)
        Flags |= HasDebugInfo;

      if (Triple(DevCD->Invocation->getTargetOpts().Triple).getArch() ==
            Triple::nvptx64)
        Flags |= AddressSize64;

      if (Triple(Invocation->getTargetOpts().Triple).isOSWindows())
        Flags |= HostWindows;
      else if (Triple(Invocation->getTargetOpts().Triple).isOSDarwin())
        Flags |= HostMac;
      else
        Flags |= HostLinux;

      FatBinFileHeader FBFHdr(DevCD->DevAsm.size(), CudaArch, Flags);
      FatBinHeader FBHdr(DevCD->DevAsm.size() + FBFHdr.HeaderSize);

      FBOS.write((char *) &FBHdr, FBHdr.HeaderSize);
      FBOS.write((char *) &FBFHdr, FBFHdr.HeaderSize);
      FBOS << DevCD->DevAsm;

      if (::getenv("CLANG_JIT_CUDA_DUMP_DYNAMIC_FATBIN")) {
        SmallString<128> Path;
        auto EC = llvm::sys::fs::createUniqueFile(
                      llvm::Twine("clang-jit-") +
                      llvm::sys::path::filename(Invocation->getCodeGenOpts().
                                                  MainFileName) +
                      llvm::Twine("-%%%%.fatbin"), Path,
                    llvm::sys::fs::owner_read | llvm::sys::fs::owner_write);
        if (!EC) {
          raw_fd_ostream DOS(Path, EC);
          if (!EC)
            DOS << FatBin;
        }
      }

      Consumer->getCodeGenerator()->CGM().getCodeGenOpts().GPUBinForJIT =
        FatBin;
      DevCD->DevAsm.clear();
    }

    // Finalize translation unit. No optimization yet.
    Consumer->HandleTranslationUnit(*Ctx);

    // First, mark everything we've newly generated with external linkage. When
    // we generate additional modules, we'll mark these functions as available
    // externally, and so we're likely to inline them, but if not, we'll need
    // to link with the ones generated here.

    for (auto &F : Consumer->getModule()->functions()) {
      F.setLinkage(llvm::GlobalValue::ExternalLinkage);
      F.setComdat(nullptr);
    }

    auto IsLocalUnnamedConst = [](llvm::GlobalValue &GV) {
      if (!GV.hasAtLeastLocalUnnamedAddr() || !GV.hasLocalLinkage())
        return false;

      auto *GVar = dyn_cast<llvm::GlobalVariable>(&GV);
      if (!GVar || !GVar->isConstant())
        return false;

      return true;
    };

    for (auto &GV : Consumer->getModule()->global_values()) {
      if (IsLocalUnnamedConst(GV) || GV.hasAppendingLinkage())
        continue;

      GV.setLinkage(llvm::GlobalValue::ExternalLinkage);
      if (auto *GO = dyn_cast<llvm::GlobalObject>(&GV))
        GO->setComdat(nullptr);
    }

    // Here we link our previous cache of definitions, etc. into this module.
    // This includes all of our previously-generated functions (marked as
    // available externally). We prefer our previously-generated versions to
    // our current versions should both modules contain the same entities (as
    // the previously-generated versions have already been optimized).

    // We need to be specifically careful about constants in our module,
    // however. Clang will generate all string literals as .str (plus a
    // number), and these from previously-generated code will conflict with the
    // names chosen for string literals in this module.

    for (auto &GV : Consumer->getModule()->global_values()) {
      if (!IsLocalUnnamedConst(GV) && !GV.getName().startswith("__cuda_"))
        continue;

      if (!RunningMod->getNamedValue(GV.getName()))
        continue;

      llvm::SmallString<16> UniqueName(GV.getName());
      unsigned BaseSize = UniqueName.size();
      do {
        // Trim any suffix off and append the next number.
        UniqueName.resize(BaseSize);
        llvm::raw_svector_ostream S(UniqueName);
        S << "." << ++LastUnique;
      } while (RunningMod->getNamedValue(UniqueName));

      GV.setName(UniqueName);
    }

    // Clang will generate local init/deinit functions for variable
    // initialization, CUDA registration, etc. and these can't be shared with
    // the base part of the module (as they specifically initialize variables,
    // etc. that we just generated).

    for (auto &F : Consumer->getModule()->functions()) {
      // FIXME: This likely covers the set of TU-local init/deinit functions
      // that can't be shared with the base module. There should be a better
      // way to do this (e.g., we could record all functions that
      // CreateGlobalInitOrDestructFunction creates? - ___cuda_ would still be
      // a special case).
      if (!F.getName().startswith("__cuda_") &&
          !F.getName().startswith("_GLOBAL_") &&
          !F.getName().startswith("__GLOBAL_") &&
          !F.getName().startswith("__cxx_"))
        continue;

      if (!RunningMod->getFunction(F.getName()))
        continue;

      llvm::SmallString<16> UniqueName(F.getName());
      unsigned BaseSize = UniqueName.size();
      do {
        // Trim any suffix off and append the next number.
        UniqueName.resize(BaseSize);
        llvm::raw_svector_ostream S(UniqueName);
        S << "." << ++LastUnique;
      } while (RunningMod->getFunction(UniqueName));

      F.setName(UniqueName);
    }

    if (Linker::linkModules(*Consumer->getModule(), llvm::CloneModule(*RunningMod),
                            Linker::Flags::OverrideFromSrc))
      fatal();

    // Aliases are not allowed to point to functions with available_externally linkage.
    // We solve this by replacing these aliases with the definition of the aliasee.
    // Candidates are identified first, then erased in a second step to avoid invalidating the iterator.
    auto& LinkedMod = *Consumer->getModule();
    SmallPtrSet<GlobalAlias*, 4> ToReplace;
    for (auto& Alias : LinkedMod.aliases()) {
      // Aliases may point to other aliases but we only need to alter the lowest level one
      // Only function declarations are relevant
      auto Aliasee = dyn_cast<Function>(Alias.getAliasee());
      if (!Aliasee || !Aliasee->isDeclarationForLinker()) {
        continue;
      }
      assert(Aliasee->hasAvailableExternallyLinkage() && "Broken module: alias points to declaration");
      ToReplace.insert(&Alias);
    }

    for (auto* Alias : ToReplace) {
      auto Aliasee = cast<Function>(Alias->getAliasee());

      llvm::ValueToValueMapTy VMap;
      Function* AliasReplacement = llvm::CloneFunction(Aliasee, VMap);

      AliasReplacement->setLinkage(Alias->getLinkage());
      Alias->replaceAllUsesWith(AliasReplacement);

      SmallString<32> AliasName = Alias->getName();
      Alias->eraseFromParent();
      AliasReplacement->setName(AliasName);
    }

    // Optimize the merged module, containing both the newly generated IR as well as
    // previously emitted code marked available_externally.
    Consumer->EmitOptimized();

    std::unique_ptr<llvm::Module> ToRunMod =
        llvm::CloneModule(*Consumer->getModule());

    CJ->addModule(std::move(ToRunMod));

    // Now that we've generated code for this module, take them optimized code
    // and mark the definitions as available externally. We'll link them into
    // future modules this way so that they can be inlined.

    for (auto &F : Consumer->getModule()->functions())
      if (!F.isDeclaration())
        F.setLinkage(llvm::GlobalValue::AvailableExternallyLinkage);

    for (auto &GV : Consumer->getModule()->global_values())
      if (!GV.isDeclaration()) {
        if (GV.hasAppendingLinkage())
          cast<GlobalVariable>(GV).setInitializer(nullptr);
        else if (isa<GlobalAlias>(GV))
          // Aliases cannot have externally-available linkage, so give them
          // private linkage.
          GV.setLinkage(llvm::GlobalValue::PrivateLinkage);
        else
          GV.setLinkage(llvm::GlobalValue::AvailableExternallyLinkage);
      }

    // OverrideFromSrc is needed here too, otherwise globals marked available_externally are not considered.
    if (Linker::linkModules(*RunningMod, Consumer->takeModule(),
                            Linker::Flags::OverrideFromSrc))
      fatal();

    Consumer->Initialize(*Ctx);

    auto SpecSymbol = CJ->findSymbol(SMName);
    assert(SpecSymbol && "Can't find the specialization just generated?");

    if (!SpecSymbol.getAddress())
      fatal();

    return (void *) llvm::cantFail(SpecSymbol.getAddress());
  }
};

llvm::sys::SmartMutex<false> Mutex;
bool InitializedTarget = false;
llvm::DenseMap<const void *, std::unique_ptr<CompilerData>> TUCompilerData;

struct InstInfo {
  InstInfo(const char *InstKey, const void *NTTPValues,
           unsigned NTTPValuesSize, const char **TypeStrings,
           unsigned TypeStringsCnt)
    : Key(InstKey),
      NTArgs(StringRef((const char *) NTTPValues, NTTPValuesSize)) {
    for (unsigned i = 0, e = TypeStringsCnt; i != e; ++i)
      TArgs.push_back(StringRef(TypeStrings[i]));
  }

  InstInfo(const StringRef &R) : Key(R) { }

  // The instantiation key (these are always constants, so we don't need to
  // allocate storage for them).
  StringRef Key;

  // The buffer of non-type arguments (this is packed).
  SmallString<16> NTArgs;

  // Vector of string type names.
  SmallVector<SmallString<32>, 1> TArgs;
};

struct ThisInstInfo {
  ThisInstInfo(const char *InstKey, const void *NTTPValues,
               unsigned NTTPValuesSize, const char **TypeStrings,
               unsigned TypeStringsCnt)
    : InstKey(InstKey), NTTPValues(NTTPValues), NTTPValuesSize(NTTPValuesSize),
      TypeStrings(TypeStrings), TypeStringsCnt(TypeStringsCnt) { }

  const char *InstKey;

  const void *NTTPValues;
  unsigned NTTPValuesSize;

  const char **TypeStrings;
  unsigned TypeStringsCnt;
 
};

struct InstMapInfo {
  static inline InstInfo getEmptyKey() {
    return InstInfo(DenseMapInfo<StringRef>::getEmptyKey());
  }

  static inline InstInfo getTombstoneKey() {
    return InstInfo(DenseMapInfo<StringRef>::getTombstoneKey());
  }

  static unsigned getHashValue(const InstInfo &II) {
    using llvm::hash_code;
    using llvm::hash_combine;
    using llvm::hash_combine_range;

    hash_code h = hash_combine_range(II.Key.begin(), II.Key.end());
    h = hash_combine(h, hash_combine_range(II.NTArgs.begin(),
                                           II.NTArgs.end()));
    for (auto &TA : II.TArgs)
      h = hash_combine(h, hash_combine_range(TA.begin(), TA.end()));

    return (unsigned) h;
  }
  
  static unsigned getHashValue(const ThisInstInfo &TII) {
    using llvm::hash_code;
    using llvm::hash_combine;
    using llvm::hash_combine_range;

    hash_code h =
      hash_combine_range(TII.InstKey, TII.InstKey + std::strlen(TII.InstKey));
    h = hash_combine(h, hash_combine_range((const char *) TII.NTTPValues,
                                           ((const char *) TII.NTTPValues) +
                                             TII.NTTPValuesSize));
    for (unsigned int i = 0, e = TII.TypeStringsCnt; i != e; ++i)
      h = hash_combine(h,
                       hash_combine_range(TII.TypeStrings[i],
                                          TII.TypeStrings[i] +
                                            std::strlen(TII.TypeStrings[i])));

    return (unsigned) h;
  }

  static bool isEqual(const InstInfo &LHS, const InstInfo &RHS) {
    return LHS.Key    == RHS.Key &&
           LHS.NTArgs == RHS.NTArgs &&
           LHS.TArgs  == RHS.TArgs;
  }

  static bool isEqual(const ThisInstInfo &LHS, const InstInfo &RHS) {
    return isEqual(RHS, LHS);
  }

  static bool isEqual(const InstInfo &II, const ThisInstInfo &TII) {
    if (II.Key != StringRef(TII.InstKey))
      return false;
    if (II.NTArgs != StringRef((const char *) TII.NTTPValues,
                               TII.NTTPValuesSize))
      return false;
    if (II.TArgs.size() != TII.TypeStringsCnt)
      return false;
    for (unsigned int i = 0, e = TII.TypeStringsCnt; i != e; ++i)
      if (II.TArgs[i] != StringRef(TII.TypeStrings[i]))
        return false;

    return true; 
  }
};

llvm::sys::SmartMutex<false> IMutex;
llvm::DenseMap<InstInfo, void *, InstMapInfo> Instantiations;

} // anonymous namespace

extern "C"
#ifdef _MSC_VER
__declspec(dllexport)
#endif
void *__clang_jit(const void *CmdArgs, unsigned CmdArgsLen,
                  const void *ASTBuffer, size_t ASTBufferSize,
                  const void *IRBuffer, size_t IRBufferSize,
                  const void **LocalPtrs, unsigned LocalPtrsCnt,
                  const void **LocalDbgPtrs, unsigned LocalDbgPtrsCnt,
                  const DevData *DeviceData, unsigned DevCnt,
                  const void *NTTPValues, unsigned NTTPValuesSize,
                  const char **TypeStrings, unsigned TypeStringsCnt,
                  const char *InstKey, unsigned Idx) {
  
  {
    
    llvm::MutexGuard Guard(IMutex);
    
    auto IIAutotune = AutotuneInfoMap.find(ASTBuffer);
    
    if(IIAutotune == AutotuneInfoMap.end())
    {
    auto II =
      Instantiations.find_as(ThisInstInfo(InstKey, NTTPValues, NTTPValuesSize,
                                          TypeStrings, TypeStringsCnt));
    if (II != Instantiations.end())
      {
      
      return II->second;
      }
    }
  }
  
  llvm::MutexGuard Guard(Mutex);

  if (!InitializedTarget) {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();

    LCtx.reset(new LLVMContext);

    InitializedTarget = true;
  }

  CompilerData *CD;
  auto TUCDI = TUCompilerData.find(ASTBuffer);
  
  
  if (TUCDI == TUCompilerData.end() || AutotuneInfoMap.find(ASTBuffer) != AutotuneInfoMap.end()) {
    CD = new CompilerData(CmdArgs, CmdArgsLen, ASTBuffer, ASTBufferSize,
                          IRBuffer, IRBufferSize, LocalPtrs, LocalPtrsCnt,
                          LocalDbgPtrs, LocalDbgPtrsCnt, DeviceData, DevCnt);
    TUCompilerData[ASTBuffer].reset(CD);
  } else {
    CD = TUCDI->second.get();
    
  }
  
  void *FPtr = CD->resolveFunction(NTTPValues, TypeStrings, Idx);

  {
    llvm::MutexGuard Guard(IMutex);
    Instantiations[InstInfo(InstKey, NTTPValues, NTTPValuesSize,
                            TypeStrings, TypeStringsCnt)] = FPtr;
  }

  return FPtr;
}


