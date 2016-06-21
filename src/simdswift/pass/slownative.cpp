//===-------- slownative.cpp - native slower pass ------------------------===//
//
//	 This pass implements a "slow" native version to make a comparison with
//   our "ideal" (i.e., with new AVX instructions) avxswift.

//   Pass changes nothing in the native version except for:
//     - AVX "extract" is inserted before each load/store/atomic
//     - AVX "broadcast" is inserted after each load/atomic
//     - AVX "ptest" is inserted before each branch

//   These AVX instructions are inserted as dummy volatile inline asm:
//   they cannot be removed by LLVM CodeGen and thus account for AVX overhead.
//	 
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "avxswift"

#include <llvm/Pass.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InlineAsm.h>
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
#include <llvm/IR/MDBuilder.h>
#include <llvm/IR/Metadata.h>

#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <utility>
#include <tr1/memory>
#include <tr1/tuple>

using namespace llvm;

namespace {

//===----------------------------------------------------------------------===//
Function* dummy_extract;
Function* dummy_broadcast;
Function* dummy_ptest;

void findHelperFuncs(Module &M) {
	dummy_extract    = M.getFunction("SIMDSWIFT_dummy_extract");
	dummy_broadcast  = M.getFunction("SIMDSWIFT_dummy_broadcast");
	dummy_ptest      = M.getFunction("SIMDSWIFT_dummy_ptest");

	assert(dummy_extract && dummy_broadcast && dummy_ptest &&
		"SIMDSWIFT dummy functions are not found (requires linked runtime");
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

	StringRef fname = F->getName();
	if (fname.startswith("llvm.") || fname.startswith("SIMDSWIFT") || ignored.count(fname)) {
		// function is LLVM intrinsic or simd-swift helper or in a list of ignored
		return true;
	}
	return false;
}


//===----------------------------------------------------------------------===//
class SwiftTransformer {
	Module* module;

	public:
	void simdInst(Instruction* I) {
//		errs() << "Working on I " << *I << "\n";

		// first deal with terminators, they are special cases
		BasicBlock::iterator instIt(I);
		IRBuilder<> irBuilderBefore(instIt->getParent(), instIt);

		if (BranchInst* BI = dyn_cast<BranchInst>(I)) {
			if (BI->isUnconditional())	return;			
			if (isa<Constant>(BI->getCondition()))	return;
 
			irBuilderBefore.CreateCall(dummy_ptest);
			return;
		}

		// now deal with non-terminator instructions
		IRBuilder<> irBuilderAfter(instIt->getParent(), ++instIt);

		if (LoadInst* LI = dyn_cast<LoadInst>(I)) {
			if (!isa<Constant>(LI->getPointerOperand())) {
				// extract address
				irBuilderBefore.CreateCall(dummy_extract);
			}
			irBuilderAfter.CreateCall(dummy_broadcast);  // broadcast loaded value
			return;
		}

		if (StoreInst* SI = dyn_cast<StoreInst>(I)) {
			if (!isa<Constant>(SI->getPointerOperand())) {
				// extract address
				irBuilderBefore.CreateCall(dummy_extract);
			}
			if (!isa<Constant>(SI->getValueOperand())) {
				// extract address
				irBuilderBefore.CreateCall(dummy_extract);
			}
			return;
		}

		if (isa<AtomicCmpXchgInst>(I)) {
			for (int i = 0; i < 3; i++) {
				if (!isa<Constant>(I->getOperand(i)))
					irBuilderBefore.CreateCall(dummy_extract);
			}
			irBuilderAfter.CreateCall(dummy_broadcast);
			return;
		}

		if (isa<AtomicRMWInst>(I)) {
			for (int i = 0; i < 2; i++) {
				if (!isa<Constant>(I->getOperand(i)))
					irBuilderBefore.CreateCall(dummy_extract);
			}
			irBuilderAfter.CreateCall(dummy_broadcast);
			return;
		}

		return;
	}

	SwiftTransformer(Module* M) {
		module = M;
	}

};

class SwiftPass : public FunctionPass {

	public:
	static char ID; // Pass identification, replacement for typeid

	SwiftPass(): FunctionPass(ID) { }

	virtual bool doInitialization(Module& M) {
		errs() << "[RUNNING PASS: slownative]\n";
		findHelperFuncs(M);
		return true;
	}

	virtual bool runOnFunction(Function &F) {
		if (isIgnoredFunc(&F))
			return false;

		// previously the function was compiled with no-sse no-avx attrs,
		// let's remove them now for later codegen pass with new attrs
		AttributeSet toRemoveAS;
		toRemoveAS = toRemoveAS.addAttribute(F.getContext(), AttributeSet::FunctionIndex, "target-features");
		toRemoveAS = toRemoveAS.addAttribute(F.getContext(), AttributeSet::FunctionIndex, "target-cpu");
		F.removeAttributes(AttributeSet::FunctionIndex, toRemoveAS);

		std::set<BasicBlock*> visited;

//		errs() << "Working on function: " << F.getName() << "\n";

		DominatorTree& DT = getAnalysis<DominatorTreeWrapperPass>().getDomTree();
		SwiftTransformer swifter(F.getParent());

		// walk through BBs in the dominator tree order
		for (df_iterator<DomTreeNode*> DI = df_begin(DT.getRootNode()),	E = df_end(DT.getRootNode()); DI != E; ++DI){
			BasicBlock *BB = DI->getBlock();
			visited.insert(BB);

			for (BasicBlock::iterator instIt = BB->begin (); instIt != BB->end (); ) {
				// Swift can add instructions after the current instruction,
				// so we first memorize the next original instruction and after
				// modifications we jump to it, skipping swift-added ones
				BasicBlock::iterator nextIt = std::next(instIt);

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
static RegisterPass<SwiftPass> X("avxswift", "AVX-Swift Pass");

}
