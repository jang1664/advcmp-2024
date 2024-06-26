// Advanced Compilers
// VMO Lab.
#include "SimpleSCCP.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/raw_ostream.h"
#include <vector>

using namespace llvm;

AnalysisKey SimpleSCCPAnalysis::Key;

ConstantValue ConstantValue::meet(const ConstantValue &Other) const {
  assert(!(IsTop && IsBot) && "Can't be both Top and Bottom at same time.");
  assert(!(Other.IsTop && Other.IsBot) && "Can't be both Top and Bottom at same time.");

  if (IsTop) {
    return Other;
  } else if (Other.IsTop || *this == Other) {
    return *this;
  } else {
    return bot();
  }
}

bool ConstantValue::operator==(const ConstantValue &Other) const {
  bool IsEqualType = IsTop == Other.IsTop && IsBot == Other.IsBot;

  if (!IsEqualType)
    return false;
  if (!IsTop && !IsBot)
    return IntValue == Other.IntValue;
  return true;
}

bool ConstantValue::operator!=(const ConstantValue &Other) const { return !((*this) == Other); }

bool CFGEdge::operator<(const struct CFGEdge &Other) const {
  std::uintptr_t ThisFrom = reinterpret_cast<std::uintptr_t>(From);
  std::uintptr_t ThisTo = reinterpret_cast<std::uintptr_t>(To);
  std::uintptr_t OtherFrom = reinterpret_cast<std::uintptr_t>(Other.From);
  std::uintptr_t OtherTo = reinterpret_cast<std::uintptr_t>(Other.To);

  if (ThisFrom < OtherFrom)
    return true;
  else if (ThisFrom == OtherFrom && ThisTo < OtherTo)
    return true;
  else
    return false;
}

bool CFGEdge::operator==(const struct CFGEdge &Other) const {
  if (From == Other.From && To == Other.To)
    return true;
  return false;
}

/**
 * `visit` PHI instruction.
 *
 * The value of PHI is the meet of all values from accessible blocks.
 * Access basic algorithm variables through the reference `ThePass`.
 * (e.g., CFGWorset, DataflowFacts, ...)
 */
ConstantValue SimpleSCCPAnalysis::InstructionVisitor::visitPHINode(const PHINode &I) {
  ConstantValue NewValue = ConstantValue::top();
  const BasicBlock *FromBlock = I.getParent();
  for (int i = 0, n = I.getNumIncomingValues(); i < n; ++i) {
    Value *IncomingValue = I.getIncomingValue(i);
    if (Instruction *I = dyn_cast<Instruction>(IncomingValue)) {
      const BasicBlock *ToBlock = I->getParent();
      CFGEdge Edge = {ToBlock, FromBlock};
      if (ThePass.isExecutableEdge(Edge)) {
        ConstantValue IncomingConstant = ThePass.getConstantValue(*IncomingValue);
        NewValue = NewValue.meet(IncomingConstant);
      }
    }
  }
  return NewValue;
}

/**
 * `visit` branch instruction (br).
 *
 * Check the condition and append only the reachable successors.
 * When you don't know whether the condition will be true or not, append all
 * the successors. Also, append all successors when the given instruction is
 * unconditional branch.
 * Note that the value of the condition will be evaluated at previous visit.
 * Access basic algorithm variables through the reference `ThePass`.
 * (e.g., CFGWorset, DataflowFacts, ...)
 */
ConstantValue SimpleSCCPAnalysis::InstructionVisitor::visitBranchInst(const BranchInst &I) {
  if (I.isConditional()) {
    Value &Condition = *I.getCondition();
    ConstantValue C = ThePass.getConstantValue(Condition);

    if (C.isConstant()) {
      if (C.value() != 0)
        ThePass.CFGWorkset.insert({I.getParent(), I.getSuccessor(0)});
      else
        ThePass.CFGWorkset.insert({I.getParent(), I.getSuccessor(1)});
    } else {
      ThePass.appendExecutableSuccessors(I);
    }
  } else {
    ThePass.appendExecutableSuccessors(I);
  }
  return ConstantValue::bot();
}

/**
 * `visit` integer comparison instructions.
 *
 * Note that, LLVM uses 1-bit 1 and 0 to denote `true` and `false`.
 * Access basic algorithm variables through the reference `ThePass`.
 * (e.g., CFGWorset, DataflowFacts, ...)
 */
ConstantValue SimpleSCCPAnalysis::InstructionVisitor::visitICmpInst(const ICmpInst &I) {
  int64_t Int1, Int2, Result;
  Value &Operand1 = *I.getOperand(0);
  Value &Operand2 = *I.getOperand(1);
  ConstantValue CV1 = ThePass.getConstantValue(Operand1);
  ConstantValue CV2 = ThePass.getConstantValue(Operand2);

  if (CV1.isBot() || CV2.isBot())
    return ConstantValue::bot();

  if (CV1.isTop() || CV2.isTop())
    return ConstantValue::top();

  Int1 = CV1.value();
  Int2 = CV2.value();

  switch (I.getPredicate()) {
  case CmpInst::ICMP_SLT:
  case CmpInst::ICMP_ULT:
    Result = Int1 < Int2 ? 1 : 0;
    break;
  case CmpInst::ICMP_SLE:
  case CmpInst::ICMP_ULE:
    Result = Int1 <= Int2 ? 1 : 0;
    break;
  case CmpInst::ICMP_SGT:
  case CmpInst::ICMP_UGT:
    Result = Int1 > Int2 ? 1 : 0;
    break;
  case CmpInst::ICMP_SGE:
  case CmpInst::ICMP_UGE:
    Result = Int1 >= Int2 ? 1 : 0;
    break;
  case CmpInst::ICMP_EQ:
    Result = Int1 == Int2 ? 1 : 0;
    break;
  case CmpInst::ICMP_NE:
    Result = Int1 != Int2 ? 1 : 0;
    break;
  default:
    return ConstantValue::bot();
  }
  return ConstantValue(Result);
}

/**
 * `visit` binary operators.
 *
 * Most of LLVM instructions are binary operators.
 * You just need to implement only the given cases.
 * Access basic algorithm variables through the reference `ThePass`.
 * (e.g., CFGWorset, DataflowFacts, ...)
 */
ConstantValue SimpleSCCPAnalysis::InstructionVisitor::visitBinaryOperator(const BinaryOperator &I) {
  int64_t Int1, Int2, Result;
  Value &Operand1 = *I.getOperand(0);
  Value &Operand2 = *I.getOperand(1);
  ConstantValue CV1 = ThePass.getConstantValue(Operand1);
  ConstantValue CV2 = ThePass.getConstantValue(Operand2);

  if (CV1.isBot() || CV2.isBot())
    return ConstantValue::bot();

  if (CV1.isTop() || CV2.isTop())
    return ConstantValue::top();

  Int1 = CV1.value();
  Int2 = CV2.value();

  switch (I.getOpcode()) {
  case Instruction::BinaryOps::Add:
    Result = Int1 + Int2;
    break;
  case Instruction::BinaryOps::Sub:
    Result = Int1 - Int2;
    break;
  case Instruction::BinaryOps::Mul:
    Result = Int1 * Int2;
    break;
  case Instruction::BinaryOps::UDiv:
  case Instruction::BinaryOps::SDiv:
    Result = Int1 / Int2;
    break;
  case Instruction::BinaryOps::And:
    Result = Int1 & Int2;
    break;
  case Instruction::BinaryOps::Or:
    Result = Int1 | Int2;
    break;
  case Instruction::BinaryOps::Xor:
    Result = Int1 ^ Int2;
    break;
  default:
    return ConstantValue::bot();
  }
  return ConstantValue(Result);
}

/**
 * Default fallback function of `visit`.
 */
ConstantValue SimpleSCCPAnalysis::InstructionVisitor::visitInstruction(const Instruction &I) {
  return ConstantValue::bot();
}

SimpleSCCPAnalysis::DataflowFactsTy SimpleSCCPAnalysis::run(Function &F,
                                                            FunctionAnalysisManager &FAM) {
  analyze(F);
  return DataflowFacts;
}

/**
 * Run SCCP algorithm.
 *
 * This pass will be called per function manner.
 * You can check LLVM's implementation at
 * https://github.com/llvm/llvm-project/blob/llvmorg-17.0.6/llvm/lib/Transforms/Utils/SCCPSolver.cpp#L1833
 */
void SimpleSCCPAnalysis::analyze(Function &F) {
  CFGWorkset.clear();
  CFGWorkset.insert(CFGEdge{nullptr, &F.getEntryBlock()});
  SSAWorkset.clear();
  ExecutableEdges.clear();
  DataflowFacts.clear();

  while (!CFGWorkset.empty() || !SSAWorkset.empty()) {
    if (!CFGWorkset.empty()) {
      CFGEdge Edge = *CFGWorkset.begin();
      const BasicBlock *ToBlock = Edge.To;
      CFGWorkset.erase(Edge);
      ExecutableEdges.insert(Edge);

      // visit phis
      for (const PHINode &Phi : ToBlock->phis()) {
        visit(Phi);
      }

      // visit instructions if it is the first visit
      if (isFirstVisit(*ToBlock)) {
        for (const Instruction &I : *ToBlock) {
          if (dyn_cast<PHINode>(&I)) {
            continue;
          } else {
            visit(I);
          }
        }
      }

      // if one successor, add to CFGset
      const Instruction *TI = ToBlock->getTerminator();
      if (TI->getNumSuccessors() == 1) {
        CFGEdge NewEdge = {ToBlock, TI->getSuccessor(0)};
        if (ExecutableEdges.count(NewEdge) == 0) {
          ExecutableEdges.insert(NewEdge);
        }
        CFGWorkset.insert(NewEdge);
      }
    } else {
      const Instruction *V = *SSAWorkset.begin();
      SSAWorkset.erase(V);
      if (isa<PHINode>(*V)) {
        visit(*dyn_cast<PHINode>(V));
      } else if (isExecutableBlock(*V->getParent())) {
        visit(*V);
      }
    }
  }
  //***************************** TODO 2 END *****************************
}

/**
 * Visit an instruction. (`analyze`-helper)
 */
void SimpleSCCPAnalysis::visit(const Instruction &I) {
  ConstantValue NewLatticeValue = TheVisitor.visit(const_cast<Instruction &>(I));
  // Set default value.
  ConstantValue OldLatticeValue = ConstantValue::top();
  const Value *V = dyn_cast<Value>(&I);
  Value *V_ = const_cast<Value *>(V);
  auto OldLatticeValueIt = DataflowFacts.find(&I);

  if (OldLatticeValueIt != DataflowFacts.end()) {
    OldLatticeValue = OldLatticeValueIt->second;
    if (NewLatticeValue != OldLatticeValue) {
      DataflowFacts[V_] = NewLatticeValue;
      for (User *User : V_->users()) {
        if (Instruction *UserInst = dyn_cast<Instruction>(User)) {
          SSAWorkset.insert(UserInst);
        }
      }
    }
  } else {
    DataflowFacts.insert(std::make_pair(V_, NewLatticeValue));
    for (User *User : V_->users()) {
      if (Instruction *UserInst = dyn_cast<Instruction>(User)) {
        SSAWorkset.insert(UserInst);
      }
    }
  }
}

bool SimpleSCCPAnalysis::isFirstVisit(const BasicBlock &BB) {
  unsigned int ExecutableIncomingEdgeCount = 0;
  const BasicBlock *TheBlock = &BB;

  for (const auto &Edge : ExecutableEdges) {
    if (Edge.To == TheBlock) {
      ++ExecutableIncomingEdgeCount;
      if (ExecutableIncomingEdgeCount > 1)
        return false;
    }
  }
  if (ExecutableIncomingEdgeCount != 1)
    return false;
  return true;
}

bool SimpleSCCPAnalysis::isExecutableBlock(const BasicBlock &BB) {
  const BasicBlock *TheBlock = &BB;

  for (const auto &Edge : ExecutableEdges) {
    if (Edge.To == TheBlock)
      return true;
  }
  return false;
}

bool SimpleSCCPAnalysis::isExecutableEdge(const CFGEdge &CE) {
  return ExecutableEdges.count(CE) != 0;
}

void SimpleSCCPAnalysis::appendExecutableSuccessors(const BranchInst &I) {
  for (const BasicBlock *BB : I.successors()) {
    CFGEdge Candidate = CFGEdge{I.getParent(), BB};
    if (ExecutableEdges.count(Candidate) == 0)
      CFGWorkset.insert(Candidate);
  }
}

/**
 * Get or wrap `ConstantValue` of the given `Value`.
 *
 * If the `Value` is an integer constant, wrap the integer into `ConstantValue`.
 * Else, search the corresponding `ConstantValue` from the `DataflowFacts`.
 * If there is no cue about the given `Value`, just return the TOP.
 */
ConstantValue SimpleSCCPAnalysis::getConstantValue(const Value &Value) {
  if (const ConstantInt *CI = dyn_cast<ConstantInt>(&Value))
    return ConstantValue(CI->getSExtValue());

  auto FoundIt = DataflowFacts.find(&Value);
  if (FoundIt != DataflowFacts.end())
    return FoundIt->getSecond();

  return ConstantValue::top();
}

/**
 * Fold constants with analysis result.
 *
 * While removing or RAUW (Replace All Uses With) dangling instructions of
 * constants, one should be cautious with `br` and `phi` instructions.
 * If the condition of the branch instruction is replaced with a constant,
 * then the branch should be changed into unconditional branch.
 * By dropping branch path, a destination block may no longer be reachable.
 * Thus, one should also manage the dangling incoming block at phi nodes.
 */
bool SimpleSCCPTransform::foldConstants(Function &F, AnalysisResultTy &DataflowFacts) {
  // using DataflowFactsTy = llvm::DenseMap<llvm::Value *, ConstantValue>;
  bool madeChange = false;

  std::vector<Instruction *> AbondonedInst;
  std::vector<BasicBlock *> AbondonedBlock;
  IRBuilder<> IRB(F.getContext());

  //* TODO 1 - constant substitution (folding)
  //******************************** TODO 1 ********************************

  // TODO:
  // - Replace all Uses of the constants.
  // - Some conditional branches (`br`) might need to be replaced by
  // unconditional branch.

  // loop through all the dataflow facts and RAUW
  for (auto [V, CV] : DataflowFacts) {
    if (CV.isConstant()) {
      if (auto *I = dyn_cast<Instruction>(V)) {
        if (I->use_empty()) {
          AbondonedInst.push_back(I);
        } else {
          I->replaceAllUsesWith(ConstantInt::get(I->getType(), CV.value()));
          AbondonedInst.push_back(I);
        }
      }
    }
  }

  for (auto *I : AbondonedInst) {
    I->removeFromParent();
  }
  AbondonedInst.clear();

  // loop through all terminators and replace conditional branch with unconditional branch if
  // condition is constant
  for (llvm::BasicBlock &BB : F) {
    llvm::Instruction *TI = BB.getTerminator();
    if (auto *BI = dyn_cast<BranchInst>(TI)) {
      if (BI->isConditional()) {
        if (auto *CI = dyn_cast<ConstantInt>(BI->getCondition())) {
          if (CI->isOne()) {
            IRB.SetInsertPoint(TI);
            llvm::BranchInst *NewBI = IRB.CreateBr(BI->getSuccessor(0));
            TI->replaceAllUsesWith(NewBI);
            AbondonedInst.push_back(TI);
            // AbondonedBlock.push_back(BI->getSuccessor(1));
            madeChange = true;
          } else if (CI->isZero()) {
            IRB.SetInsertPoint(TI);
            llvm::BranchInst *NewBI = IRB.CreateBr(BI->getSuccessor(1));
            TI->replaceAllUsesWith(NewBI);
            AbondonedInst.push_back(TI);
            // AbondonedBlock.push_back(BI->getSuccessor(0));
            madeChange = true;
          }
        }
      }
    }
  }

  for (llvm::Instruction *I : AbondonedInst) {
    I->removeFromParent();
  }
  AbondonedInst.clear();

  // remove unreachable blocks
  std::set<BasicBlock *> Reachable;
  std::vector<BasicBlock *> WorkList;
  BasicBlock *Entry = &F.getEntryBlock();

  // Perform a DFS to find all reachable basic blocks
  WorkList.push_back(Entry);
  Reachable.insert(Entry);

  while (!WorkList.empty()) {
    BasicBlock *Current = WorkList.back();
    WorkList.pop_back();

    for (succ_iterator SI = succ_begin(Current), SE = succ_end(Current); SI != SE; ++SI) {
      BasicBlock *Succ = *SI;
      if (Reachable.insert(Succ).second) {
        WorkList.push_back(Succ);
      }
    }
  }

  // Collect unreachable basic blocks
  std::vector<BasicBlock *> Unreachable;
  for (BasicBlock &BB : F) {
    if (Reachable.find(&BB) == Reachable.end()) {
      Unreachable.push_back(&BB);
    }
  }

  // Remove unreachable basic blocks
  for (BasicBlock *BB : Unreachable) {
    // Remove all uses of the basic block to prevent dangling references
    BB->replaceAllUsesWith(UndefValue::get(BB->getType()));
    // Erase the block
    BB->eraseFromParent();
  }

  // loop through all the phi nodes and remove incoming block if it is not reachable
  for (llvm::BasicBlock &BB : F) {
    for (llvm::PHINode &PN : BB.phis()) {
      for (unsigned i = 0; i < PN.getNumIncomingValues(); i++) {
        llvm::BasicBlock *IncomingBlock = PN.getIncomingBlock(i);
        if (!IncomingBlock) {
          continue; // reached from outside
        }
        PN.removeIncomingValue(i);
        madeChange = true;
      }
    }
  }

  //****************************** TODO 1 END ******************************
  return madeChange;
}

/**
 * Run Simple SCCP Transform.
 */
PreservedAnalyses SimpleSCCPTransform::run(Function &F, FunctionAnalysisManager &FAM) {
  auto &DataflowFacts = FAM.getResult<SimpleSCCPAnalysis>(F);

  bool Changed = foldConstants(F, DataflowFacts);

  if (Changed)
    return PreservedAnalyses::none();
  return PreservedAnalyses::all();
}

PreservedAnalyses SimpleSCCPPrinter::run(Function &F, FunctionAnalysisManager &FAM) {
  auto &DataflowFacts = FAM.getResult<SimpleSCCPAnalysis>(F);
  ROS << DataflowFacts;
  return PreservedAnalyses::all();
}

raw_ostream &operator<<(raw_ostream &ROS, const CFGEdge &CE) {
  std::string From = "NULL";
  std::string To = "NULL";

  if (CE.From)
    From = CE.From->getName();
  if (CE.To)
    To = CE.To->getName();

  errs() << From << " -> " << To;

  return ROS;
}

raw_ostream &operator<<(raw_ostream &ROS, const ConstantValue &C) {
  if (C.isTop())
    ROS << "{ TOP }";
  else if (C.isBot())
    ROS << "{ BOTTOM }";
  else
    ROS << "{ " << C.value() << " }";
  return ROS;
}

raw_ostream &operator<<(raw_ostream &ROS, const SimpleSCCPAnalysis::DataflowFactsTy &DF) {
  for (const auto &Entry : DF)
    errs() << getId(Entry.getFirst()) << " : " << Entry.getSecond() << "\n";
  return ROS;
}

/*******************************************************************************
 *                        Pass Plugin Registration
 ******************************************************************************/

// Hiding callback functions for registration steps inside anonymous namespace.
namespace {
/**
 * Register pass to the pipeline.
 *
 * Registered pass can be called from `opt` by its name.
 */
bool regSimpleSCCPTransformToPipeline(StringRef Name, FunctionPassManager &FPM,
                                      ArrayRef<PassBuilder::PipelineElement>) {
  // Check called name.
  // This name will be called via `-passes=(Name)`
  // e.g., opt -passes='print<simple-sccp>' ...
  if (Name != "simple-sccp")
    return false;
  FPM.addPass(SimpleSCCPTransform());
  return true;
}

bool regSimpleSCCPPrinterToPipeline(StringRef Name, FunctionPassManager &FPM,
                                    ArrayRef<PassBuilder::PipelineElement>) {
  // Check called name.
  // This name will be called via `-passes=(Name)`
  // e.g., opt -passes='print<simple-sccp>' ...
  if (Name != "print<simple-sccp>")
    return false;
  FPM.addPass(SimpleSCCPPrinter(errs()));
  return true;
}

/**
 * Register pass to the analysis manager.
 *
 * Registered pass' result can be fetched by `FAM.getResult<Pass>(F)`.
 */
void regSimpleSCCPToAM(FunctionAnalysisManager &FAM) {
  FAM.registerPass([&]() { return SimpleSCCPAnalysis(); });
}

void PBHook(PassBuilder &PB) {
  // Register callbacks to the pass builder.
  PB.registerAnalysisRegistrationCallback(regSimpleSCCPToAM);
  PB.registerPipelineParsingCallback(regSimpleSCCPTransformToPipeline);
  PB.registerPipelineParsingCallback(regSimpleSCCPPrinterToPipeline);
}

PassPluginLibraryInfo getSCCPPrinterPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION,
          "Advanced Compilers - Simple Sparse Conditional Constants Propagation",
          LLVM_VERSION_STRING, PBHook};
}
} // namespace

// Pass registeration.
extern "C" ::llvm::PassPluginLibraryInfo LLVM_ATTRIBUTE_WEAK llvmGetPassPluginInfo() {
  return getSCCPPrinterPluginInfo();
}