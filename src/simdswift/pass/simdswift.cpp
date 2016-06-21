//===------ simdswift.cpp - SIMD Swift harden pass ------------------------===//
//
//	 This pass duplicates all instructions ("swiftifies" a program) using
//   SIMD instructions (SSE/AVX for x86-64) and inserts checks at sync points: 
//   stores, branches, calls, etc.
//
//   This is a naive version that uses LLVM vector types in the hope that
//   LLVM can effectively lower vectors to SIMD instructions. As in turned out,
//   LLVM generates pretty bad code for vector instructions, so we discontinued
//   this version.
//	 
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "simdswift"

#include <llvm/Pass.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/Support/Casting.h>
#include <llvm/IR/Dominators.h>
#include <llvm/ADT/DepthFirstIterator.h>
#include <llvm/Transforms/Utils/BasicBlockUtils.h>
#include <llvm/Support/CommandLine.h>

#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <utility>
#include <tr1/memory>
#include <tr1/tuple>

using namespace llvm;

namespace {

static const std::string SIMD_SUFFIX(".simd");

//===----------------------------------------------------------------------===//
Type* getSimdType(Type* t) {	
	return VectorType::get(t, 2);  // 2 copies are enough for DMR
}

bool isSimdType(Type* t) {
	if (t->isVectorTy() && t->getVectorNumElements() == 2)
		return true;
	return false;
}

Value* createSimdValue(IRBuilder<>& irBuilder, Value* v) {
	UndefValue* undefvec = UndefValue::get(getSimdType(v->getType()));
	Value *first  = irBuilder.CreateInsertElement(undefvec, v, (uint64_t)0);
	Value *second = irBuilder.CreateInsertElement(first,    v, (uint64_t)1);
	return second;
}

bool isIgnoredFunc(Function* F) {
	static std::set<std::string> ignored {
		// Transactifier functions
		"tx_cond_start",
		"tx_start",
		"tx_end",
		"tx_abort",
		"tx_increment",
		"tx_pthread_mutex_lock",
		"tx_pthread_mutex_unlock",

		"__dummy__"
	};

	if (!F) {
		// conservative about function pointers
		return false;
	}
	
	StringRef fname = F->getName();
	if (fname.startswith("llvm.lifetime.end") ||
		fname.startswith("llvm.lifetime.start")) {
		// function is LLVM intrinsic that uses program variables, need to rewire
		// to use simd variables
		// TODO: maybe simply remove these intrinsics?
		return false;
	}

	if (fname.startswith("llvm.") || ignored.count(fname)) {
		// function is LLVM intrinsic or in a list of ignored
		return true;
	}
	return false;
}

//===----------------------------------------------------------------------===//
class ValueSimdMap{
	typedef std::map<Value*, Value*> ValueSimdMapType;
	ValueSimdMapType vsm;

	public:
	ValueSimdMap(): vsm() {}

	void add(Value* v, Value* simd) {
		bool isnew;
		ValueSimdMapType::iterator it;

		std::tr1::tie(it, isnew) = vsm.insert(std::make_pair(v, simd));
		assert (isnew && "[simd-swift] value already has a SIMD version");
	}

	Value* getSimd(Value *v, Value *inst_debug) {
		if (v == nullptr)
			return nullptr;

		if (isSimdType(v->getType()))
			return v;

		if (Constant* c = dyn_cast<Constant>(v)) {
			return ConstantVector::getSplat(2, c);
		}

		// no checks for BBs (labels), function declarations, inline asm and metadata
		if (isa<BasicBlock>(v) || isa<Function>(v) ||
			isa<InlineAsm>(v)  || isa<MetadataAsValue>(v) || 
			isa<InvokeInst>(v) || isa<LandingPadInst>(v))
			return nullptr;

		ValueSimdMapType::iterator it = vsm.find(v);

		if (it == vsm.end()){
			errs() << "Value '" << *v << "' has no SIMD version (for Instr '" << *inst_debug << "')\n";
		}
		assert(it != vsm.end() && "[simd-swift] value has no SIMD version");

		return it->second;
	}

	bool hasSimd(Value *v) {
		return vsm.end() != vsm.find(v);
	}
};

//===----------------------------------------------------------------------===//
class SwiftTransformer {
	ValueSimdMap simds;
	std::vector<Instruction*> origs;
	std::vector<PHINode*> phis;

	void extractSimdOpAndSubstitute(IRBuilder<> irBuilder, Instruction* I, unsigned idxOp) {		
		Value* simdOp = simds.getSimd(I->getOperand(idxOp), I);
		if (simdOp) {
			// extract from SIMD pointer operand and substitute in instr
			Value* newOp = irBuilder.CreateExtractElement(simdOp, (uint64_t)0);
			I->setOperand(idxOp, newOp);
		}
	}

	public:
	void simdInst(Instruction* I) {
#if 0		
		if (I->use_empty())
			return;
		if (I->isTerminator())
			errs() << I->getParent()->getParent()->getName() << "::  cannot shadow terminator instruction " << *I << "\n";			
		assert (!I->isTerminator() && "cannot shadow terminator instruction");
#endif		

		// ignore/panic on unsupported instructions
		if (isa<InvokeInst>(I) || isa<LandingPadInst>(I) || isa<ResumeInst>(I)) {
		    /* TODO: handle these instructions */
			return;
		}
		if (isa<ExtractValueInst>(I) || isa<InsertValueInst>(I)) {
		    errs() << "Found a extractvalue/insertvalue in original code: " << *I << "\n";
			assert(!"[simd-swift] extractvalue/insertvalue not implemented (rely on -scalarrepl)");
		}
		if (isa<ExtractElementInst>(I) || isa<InsertElementInst>(I) || isa<ShuffleVectorInst>(I)) {
		    errs() << "Found a vector instruction in original code: " << *I << "\n";
			assert(!"[simd-swift] do not know how to transform vector instructions");
		}

		// first deal with terminators, they are special cases
		BasicBlock::iterator instIt(I);
		IRBuilder<> irBuilderBefore(instIt->getParent(), instIt);

		// --- terminators --- //
		if (ReturnInst* RI = dyn_cast<ReturnInst>(I)) {
			if (RI->getReturnValue())
				extractSimdOpAndSubstitute(irBuilderBefore, RI, 0);
			return;
		}
		if (SwitchInst* SI = dyn_cast<SwitchInst>(I)) {
			extractSimdOpAndSubstitute(irBuilderBefore, SI, 0);
			return;
		}
		if (BranchInst* BI = dyn_cast<BranchInst>(I)) {
			if (BI->isUnconditional())
				return;
			extractSimdOpAndSubstitute(irBuilderBefore, I, 0);
			return;
		}
		if (IndirectBrInst* IBI = dyn_cast<IndirectBrInst>(I)) {
			extractSimdOpAndSubstitute(irBuilderBefore, IBI, 0);
			return;
		}
		if (isa<UnreachableInst>(I)) {
			// ignore unreachable
			return;
		}		

		// now deal with non-terminator instructions
		IRBuilder<> irBuilderAfter(instIt->getParent(), ++instIt);
		Instruction* IS = nullptr;

		// --- data flow --- //
		if (BinaryOperator* BO = dyn_cast<BinaryOperator>(I)) {
			Value* simdOp0 = simds.getSimd(I->getOperand(0), I);
			Value* simdOp1 = simds.getSimd(I->getOperand(1), I);
			IS = BinaryOperator::Create(BO->getOpcode(), simdOp0, simdOp1, I->getName() + SIMD_SUFFIX);
			(cast<BinaryOperator>(IS))->copyIRFlags(I);
		}

		if (CmpInst* CI = dyn_cast<CmpInst>(I)) {
			Value* simdOp0 = simds.getSimd(I->getOperand(0), I);
			Value* simdOp1 = simds.getSimd(I->getOperand(1), I);
			IS = CmpInst::Create(CI->getOpcode(), CI->getPredicate(), simdOp0, simdOp1, I->getName() + SIMD_SUFFIX);
		}

		if (SelectInst* SI = dyn_cast<SelectInst>(I)) {
			Value* simdCondition  = simds.getSimd(SI->getCondition(), SI);
			Value* simdTrueValue  = simds.getSimd(SI->getTrueValue(), SI);
			Value* simdFalseValue = simds.getSimd(SI->getFalseValue(), SI);
			IS = SelectInst::Create(simdCondition, simdTrueValue, simdFalseValue, I->getName() + SIMD_SUFFIX);
		}

		if (GetElementPtrInst* GEP = dyn_cast<GetElementPtrInst>(I)) {
			Value* simdPtrOp = simds.getSimd(GEP->getPointerOperand(), GEP);
			Type*  simdPtrTy = GEP->getSourceElementType();

			std::vector<Value*> simdIdxOps;
			for (auto idx = GEP->idx_begin(); idx != GEP->idx_end(); ++idx){
				simdIdxOps.push_back(simds.getSimd(*idx, GEP));
			}
			if (GEP->isInBounds())
				IS = GetElementPtrInst::CreateInBounds(simdPtrTy, simdPtrOp, ArrayRef<Value*>(simdIdxOps), I->getName() + SIMD_SUFFIX);
			else
				IS = GetElementPtrInst::Create(simdPtrTy, simdPtrOp, ArrayRef<Value*>(simdIdxOps), I->getName() + SIMD_SUFFIX);
		}

		if (CastInst* CI = dyn_cast<CastInst>(I)) {
			Value* simdValue  = simds.getSimd(I->getOperand(0), I);
			Type*  simdDestTy = getSimdType(CI->getDestTy());
			IS = CastInst::Create(CI->getOpcode(), simdValue, simdDestTy, I->getName() + SIMD_SUFFIX);
		}		

		// --- Phis are special --- //
		if (PHINode* PI = dyn_cast<PHINode>(I)) {
			Type*  simdTy  = getSimdType(PI->getType());
			IS = PHINode::Create(simdTy, PI->getNumIncomingValues(), I->getName() + SIMD_SUFFIX);
			// remember to rewire this Phi afterwards
			phis.push_back(PI);
		}

		// --- memory related --- //
		if (AllocaInst* AI = dyn_cast<AllocaInst>(I)) {
			extractSimdOpAndSubstitute(irBuilderBefore, I, 0);
			// move ptr to allocated into simd vector
			Value* ptrSimd = createSimdValue(irBuilderAfter, AI);
			simds.add(I, ptrSimd);
			return;
		}

		if (LoadInst* LI = dyn_cast<LoadInst>(I)) {
			// conservatively treat all loads as atomics
			extractSimdOpAndSubstitute(irBuilderBefore, I, 0);
			// move loaded value into simd vector
			Value* loadedSimd = createSimdValue(irBuilderAfter, LI);
			simds.add(I, loadedSimd);
			return;
		}

		if (StoreInst* SI = dyn_cast<StoreInst>(I)) {
			// conservatively treat all stores as atomics
			extractSimdOpAndSubstitute(irBuilderBefore, SI, 0);
			extractSimdOpAndSubstitute(irBuilderBefore, SI, 1);
			return;
		}

		if (AtomicCmpXchgInst* AXI = dyn_cast<AtomicCmpXchgInst>(I)) {
			// treat cmpxchg as a load-store instruction
			extractSimdOpAndSubstitute(irBuilderBefore, I, 0);
			extractSimdOpAndSubstitute(irBuilderBefore, I, 1);
			extractSimdOpAndSubstitute(irBuilderBefore, I, 2);
			// move loaded value into simd vector
			Value* loadedSimd = createSimdValue(irBuilderAfter, AXI);
			simds.add(I, loadedSimd);
			return;
		}

		if (AtomicRMWInst* ARI = dyn_cast<AtomicRMWInst>(I)) {
			// treat rmw as a load-store instruction
			extractSimdOpAndSubstitute(irBuilderBefore, I, 0);
			extractSimdOpAndSubstitute(irBuilderBefore, I, 1);
			// move read value into simd vector
			Value* readSimd = createSimdValue(irBuilderAfter, ARI);
			simds.add(I, readSimd);
			return;
		}

		// --- function calls --- //
		if (CallInst* CI = dyn_cast<CallInst>(I)) {
			// do not shadow calls to "ignored" functions
			if (isIgnoredFunc(CI->getCalledFunction()))
			 	return;

			for (unsigned idxArgOp = 0; idxArgOp < CI->getNumArgOperands(); idxArgOp++) {
				// extract each argument (note that ArgOperands map to Operands)
				extractSimdOpAndSubstitute(irBuilderBefore, CI, idxArgOp);
			}

			// move return value into simd vector (if there is return value)
			if (!CI->getType()->isVoidTy()) {
				Value* readSimd = createSimdValue(irBuilderAfter, CI);
				simds.add(I, readSimd);
			}
			return;
		}

		// --- random stuff --- //
		if (VAArgInst* VAI = dyn_cast<VAArgInst>(I)) {
			extractSimdOpAndSubstitute(irBuilderBefore, I, 0);
			// move read VA value into simd vector
			Value* readSimd = createSimdValue(irBuilderAfter, VAI);
			simds.add(I, readSimd);
			return;
		}

		if (isa<FenceInst>(I)) {
			// ignore fences
			return;
		}

		// finally replace the original instruction
		if (IS) {
			simds.add(I, IS);
			// we constructed a simd instruction, add it, but defer removing original
			irBuilderAfter.Insert(IS);
			origs.push_back(I);
			return;
		}

		errs() << "Found unknown instruction " << *I << "\n";
		assert(!"[simd-swift] cannot handle unknown instruction");
	}

	void simdArgs(Function& F, Instruction* firstI) {
		// add SIMD args' definitions before firstI
		BasicBlock::iterator instIt(firstI);
		IRBuilder<> irBuilder(instIt->getParent(), instIt);

		// make a SIMD version for each function arg
		for (auto arg = F.arg_begin(); arg != F.arg_end(); ++arg){
			Value* simdarg = createSimdValue(irBuilder, arg);
			simds.add(arg, simdarg);
		}
	}

	void rewirePhis() {
		// substitute all incoming values in all collected phis with simds
		for (auto phiIt = phis.begin(); phiIt != phis.end(); ++phiIt) {
			PHINode* PI = *phiIt;
			if (!isa<PHINode>(simds.getSimd(PI, PI))) {
				errs() << "Could not find SIMD Phi for " << *PI << "\n";
				assert(!"[simd-swift] could not find simd phi to rewire");				
			}

			PHINode* newPI = cast<PHINode>(simds.getSimd(PI, PI));
			for (auto BB = PI->block_begin(); BB != PI->block_end(); BB++) {
				Value *simdIncomingValue = simds.getSimd(PI->getIncomingValueForBlock(*BB), PI);
				if (simdIncomingValue)
					newPI->addIncoming(simdIncomingValue, *BB);
			}
		}
	}

	void removeOriginalInsts() {
		// to avoid circular dependencies because of Phis, first empty them
		for (auto phiIt = phis.begin(); phiIt != phis.end(); ++phiIt) {
			PHINode* PI = *phiIt;
			while (PI && PI->getNumIncomingValues() > 0)
				PI->removeIncomingValue((unsigned)0, false);
		}

		for (auto instIt = origs.rbegin(); instIt != origs.rend(); ++instIt) {
			Instruction* I = *instIt;
			I->eraseFromParent();
		}
	}

	SwiftTransformer() {
		// nothing to do
	}

};

class SwiftPass : public FunctionPass {

	public:
	static char ID; // Pass identification, replacement for typeid

	SwiftPass(): FunctionPass(ID) { }

	virtual bool doInitialization(Module& M) {
		errs() << "[RUNNING PASS: simdswift]\n";
		return true;
	}

	virtual bool runOnFunction(Function &F) {
		std::set<BasicBlock*> visited;

		if (isIgnoredFunc(&F)) return false;

//		errs() << "Working on function: " << F.getName() << "\n";

		DominatorTree& DT = getAnalysis<DominatorTreeWrapperPass>().getDomTree();
		SwiftTransformer swifter;

		bool shadowedArgs = false;

		// walk through BBs in the dominator tree order
		for (df_iterator<DomTreeNode*> DI = df_begin(DT.getRootNode()),	E = df_end(DT.getRootNode()); DI != E; ++DI){
			BasicBlock *BB = DI->getBlock();
			visited.insert(BB);

			for (BasicBlock::iterator instIt = BB->begin (); instIt != BB->end (); ) {
				// Swift can add instructions after the current instruction,
				// so we first memorize the next original instruction and after
				// modifications we jump to it, skipping swift-added ones
				BasicBlock::iterator nextIt = std::next(instIt);

				if (!shadowedArgs) {
					swifter.simdArgs(F, instIt);
					shadowedArgs = true;
				}

				swifter.simdInst(instIt);
				instIt = nextIt;
			}
		}

		// walk through BBs not covered by dominator tree (case for landing pads)
		for (Function::iterator BB = F.begin(), BE = F.end(); BB != BE; ++BB) {
			if (visited.count(BB) > 0)
				continue;

			for (BasicBlock::iterator instIt = BB->begin (); instIt != BB->end (); ) {
				BasicBlock::iterator nextIt = std::next(instIt);
				swifter.simdInst(instIt);
				instIt = nextIt;
			}
		}

		swifter.rewirePhis();
		swifter.removeOriginalInsts();

		// inform that we always modify a function
		return true;
	}

	virtual bool doFinalization(Module& M) {
		return false;
	}

	virtual void getAnalysisUsage(AnalysisUsage& UA) const {
		UA.addRequired<DominatorTreeWrapperPass>();
		FunctionPass::getAnalysisUsage(UA);
	}
};

char SwiftPass::ID = 0;
static RegisterPass<SwiftPass> X("simdswift", "SIMD-Swift Pass");

}
