//===--------- instanalyzer.cpp - Rename Certain Functions pass -----------===//
//
//	 This pass shows stats of instructions in each func and in whole module:
//     - show total number of instructions
//     - show number of vector-related inst
//     - show number of inline-asm inst
//	 
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "InstAnalyzer"

#include <llvm/Pass.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/Support/Casting.h>

#include <set>
#include <map>
#include <vector>

using namespace llvm;

static cl::opt<bool>
	InstAnalyzePrintVec("instanalyze-print-vec", cl::Optional, cl::init(false),
	cl::desc("Enable printing of (all) vector instructions"));

static cl::opt<bool>
	InstAnalyzePrintAsm("instanalyze-print-asm", cl::Optional, cl::init(false),
	cl::desc("Enable printing of (all) inline-assembly instructions"));


namespace {

typedef std::map<Function*, size_t> InstCountMap;
typedef std::map<Function*, size_t> AsmCountMap;
typedef std::map<Function*, size_t> VecCountMap;

typedef std::vector<CallInst*> AsmInstVector;
typedef std::vector<Instruction*> VecInstVector;
typedef std::map<Function*, AsmInstVector> AsmInstMap;
typedef std::map<Function*, VecInstVector> VecInstMap;

class InstAnalyzerPass : public FunctionPass {
	Module* module;

	InstCountMap instcount;
	AsmCountMap  asmcount;
	VecCountMap  veccount;
	AsmInstMap   asminsts;
	VecInstMap   vecinsts;

	public:
	static char ID; // Pass identification, replacement for typeid

	InstAnalyzerPass(): FunctionPass(ID) { }

	virtual bool doInitialization(Module& M) {
		module = &M;
		return false;
	}

	virtual bool doFinalization(Module& M) {
		size_t totalinstcount = 0;
		size_t totalasmcount = 0;
		size_t totalveccount = 0;

		for (const auto &inst : instcount) {
			Function* F = inst.first;

			totalinstcount += instcount.find(F)->second;
			totalasmcount += asmcount.find(F)->second;
			totalveccount += veccount.find(F)->second;
		}
		errs() << "----- MODULE STATISTICS -----\n";
		errs() << "  Total number of instructions:        " << totalinstcount << "\n";
		errs() << "  Total number of assembly calls:      " << totalasmcount << "\n";
		errs() << "  Total number of vector instructions: " << totalveccount << "\n";
		errs() << "\n";

		errs() << "\n----- FUNCTION STATISTICS -----\n\n";
		for (const auto &inst : instcount) {
			Function* F = inst.first;
			errs() << F->getName() << "\n";

			errs() << "  Number of instructions:        " << instcount.find(F)->second << "\n";
			errs() << "  Number of assembly calls:      " << asmcount.find(F)->second << "\n";
			errs() << "  Number of vector instructions: " << veccount.find(F)->second << "\n";
			errs() << "\n";
		}

		if (InstAnalyzePrintVec) {
			errs() << "\n----- VECTOR INSTRUCTIONS STATISTICS -----\n\n";
			for (const auto &inst : instcount) {
				Function* F = inst.first;
				VecInstVector* vecs = &vecinsts.find(F)->second;
				if (vecs->empty())
					continue;

				size_t i = 0;
				errs() << F->getName() << "\n";
				for (auto vecinstIt = vecs->begin(); vecinstIt != vecs->end(); ++vecinstIt) {
					Instruction* vecinst = *vecinstIt;
					errs() << "[" << i++ << "]" << *vecinst << "\n";
				}
				errs() << "\n";
			}
		}

		if (InstAnalyzePrintAsm) {
			errs() << "\n----- ASSEMBLY CALLS STATISTICS -----\n\n";
			for (const auto &inst : instcount) {
				Function* F = inst.first;
				AsmInstVector* asms = &asminsts.find(F)->second;
				if (asms->empty())
					continue;

				size_t i = 0;
				errs() << F->getName() << "\n";
				for (auto asminstIt = asms->begin(); asminstIt != asms->end(); ++asminstIt) {
					Instruction* asminst = *asminstIt;
					errs() << "[" << i++ << "]" << *asminst << "\n";
				}
				errs() << "\n";
			}
		}

		return false;
	}

	virtual bool runOnFunction(Function &F) {
		// initialize local statistics counters
		size_t localinstcount = 0;
		size_t localasmcount = 0;
		size_t localveccount = 0;
		AsmInstVector localasminsts;
		VecInstVector localvecinsts;

		for (Function::iterator fi = F.begin(), fe = F.end(); fi != fe; ++fi) {
			BasicBlock* BB = fi;

			for (BasicBlock::iterator bi = BB->begin(), be = BB->end(); bi != be; ++bi) {
				Instruction* I = bi;

				localinstcount++;

				if (CallInst* call = dyn_cast<CallInst>(I)) {
					if (call->isInlineAsm()) {					
						localasmcount++;
						localasminsts.push_back(call);
					}
				}

				// check operands of instruction, if at least is vector, then increment vec counter
				for (unsigned i = 0; i < I->getNumOperands(); ++i) {
					Value *op = I->getOperand(i);
					if (op->getType()->isVectorTy()) {
						localveccount++;
						localvecinsts.push_back(I);
						break;
					}
				}
			}
		}

		instcount.insert(std::pair<Function*, size_t>(&F, localinstcount));
		asmcount.insert(std::pair<Function*, size_t>(&F, localasmcount));
		veccount.insert(std::pair<Function*, size_t>(&F, localveccount));

		asminsts.insert(std::pair<Function*, AsmInstVector>(&F, localasminsts));
		vecinsts.insert(std::pair<Function*, VecInstVector>(&F, localvecinsts));

		return false;
	}
};

char InstAnalyzerPass::ID = 0;
static RegisterPass<InstAnalyzerPass> X("instanalyze", "InstAnalyzerPass");

}
