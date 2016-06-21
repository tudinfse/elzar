//===----- avxfloatswift.cpp - AVX Swift floats-only harden pass ---------===//
//
//	 This pass duplicates all floating-point instructions ("swiftifies" 
//   floating point operations) using SIMD instructions (AVX for x86-64) and 
//   inserts majority voting at sync points: stores, branches, calls, etc.
//
//     - Pass replicates data in the whole 256-bit register, 
//       e.g., <4 x double> or <8 x float>
//
//     - We assume, for simplicity, that only _one_ copy can be corrupted.
//
//     - Majority voting uses ptestz & ptestnzc AVX instructions and is unusual:
//       first the whole 256-bit register is checked that all copies are correct,
//       if not the low 2 copies are compared and if good are broadcasted to all,
//       if not the high 2 copies are broadcasted to all (because by assumption)
//       if one of the low copies is corrupted, then the high copies are good.
//
//   NOTE: this implementation follows UC Irvine paper by Zhi Chen et al.,
//         "Software Fault Tolerance for FPUs via Vectorization"
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

static cl::opt<bool>
	NoCheckOnAll("no-check-all", cl::Optional, cl::init(false),
	cl::desc("Disable absolutely all checks"));
static cl::opt<bool>
	NoCheckOnBranch("no-check-branch", cl::Optional, cl::init(false),
	cl::desc("Disable checks on branches"));
static cl::opt<bool>
	NoCheckOnStore("no-check-store", cl::Optional, cl::init(false),
	cl::desc("Disable checks on stores"));
static cl::opt<bool>
	NoCheckOnAtomic("no-check-atomic", cl::Optional, cl::init(false),
	cl::desc("Disable checks on atomics (cmpxchg, atomicrmw)"));
static cl::opt<bool>
	NoCheckOnCall("no-check-call", cl::Optional, cl::init(false),
	cl::desc("Disable checks on function calls"));

namespace {

static const std::string SIMD_SUFFIX(".simd");

//===----------------------------------------------------------------------===//
Function* exitfunc;
Function* mask_i64;
Function* check_double;
Function* check_float;

void findHelperFuncs(Module &M) {
	exitfunc    = M.getFunction("SIMDSWIFT_exit");
	mask_i64    = M.getFunction("SIMDSWIFT_mask_i64");
	check_double= M.getFunction("SIMDSWIFT_check_double");
	check_float = M.getFunction("SIMDSWIFT_check_float");

	assert(exitfunc && mask_i64 && check_double && check_float &&
		"SIMDSWIFT functions are not found (requires linked runtime");
}

unsigned getSimdNum(Type* t) {
	switch (t->getTypeID()) {
	case Type::DoubleTyID: return 4;
	case Type::FloatTyID: return 8;
	default: return 0;
	}
	return 0; // unreachable
}

Type* getSimdType(Type* t) {
	unsigned simdnum = getSimdNum(t);
	if (!simdnum)
		return nullptr;
	return VectorType::get(t, simdnum);
}

bool isSimdType(Type* t) {
	if (t->isVectorTy() && t->getVectorNumElements() == getSimdNum(t))
		return true;
	return false;
}

Value* createSimdValue(IRBuilder<>& irBuilder, Value* v) {
	Value* val = UndefValue::get(getSimdType(v->getType()));
	for (unsigned i = 0; i < getSimdNum(v->getType()); i++)
		val = irBuilder.CreateInsertElement(val, v, (uint64_t)i);
	return val;
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
		// function pointers are not ignored
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

	if (fname.startswith("llvm.") || fname.startswith("SIMDSWIFT") || ignored.count(fname)) {
		// function is LLVM intrinsic or simd-swift helper or in a list of ignored
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

	Value* getSimd(Value *v, Value *I) {
		if (v == nullptr)
			return nullptr;

		if (isSimdType(v->getType()))
			return v;

		// no checks for BBs (labels), function declarations, inline asm and metadata
		if (isa<BasicBlock>(v) || isa<Function>(v) ||
			isa<InlineAsm>(v)  || isa<MetadataAsValue>(v) || 
			isa<InvokeInst>(v) || isa<LandingPadInst>(v))
			return nullptr;

		if (v->getType()->isIntegerTy(1)) {
			// special case of i1 conditions for those branches
			// that work on floats/doubles
			ValueSimdMapType::iterator it = vsm.find(v);
			if (it != vsm.end())
				return it->second;
			return nullptr;
		}

		// only floats & doubles
		if (!v->getType()->isFloatTy() && !v->getType()->isDoubleTy())
			return nullptr;

		if (Constant* c = dyn_cast<Constant>(v)) {
			unsigned num = getSimdNum(c->getType());
			return ConstantVector::getSplat(num, c);
		}

		ValueSimdMapType::iterator it = vsm.find(v);

		if (it == vsm.end()){
			errs() << "Value '" << *v << "' has no SIMD version (for Instr '" << *I << "')\n";
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
	std::vector< std::tuple<Instruction*, Instruction*, unsigned> > tocheck;

	Module* module;

	void extractSimdOpAndSubstitute(IRBuilder<> irBuilder, Instruction* I, unsigned idxOp) {		
		Value* simdOp = simds.getSimd(I->getOperand(idxOp), I);
		if (simdOp) {
			// mark that we need to check the SIMD operand
			tocheck.push_back(std::tuple<Instruction*, Instruction*, unsigned>(I, dyn_cast<Instruction>(simdOp), idxOp));

			// extract from SIMD operand and substitute in instr
			Value* newOp = irBuilder.CreateExtractElement(simdOp, (uint64_t)0);
			I->setOperand(idxOp, newOp);
		}
	}

	Value* getSimdAllOnes() {
		// always return <4 x i64>
		return ConstantVector::getSplat(4, ConstantInt::get(Type::getInt64Ty(getGlobalContext()), (uint64_t)0xFFFFFFFFFFFFFFFF));
	}

	public:
	void simdInst(Instruction* I) {
//		errs() << "Working on I " << *I << "\n";
		// ignore/panic on unsupported instructions
		if (isa<InvokeInst>(I) || isa<LandingPadInst>(I) || isa<ResumeInst>(I)) {
		    /* TODO: handle these instructions */
		    errs() << "Found an invoke/landingpad/resume in original code: " << *I << "\n";
			assert(!"[simd-swift] do not know how to work with C++ exceptions");
			return;
		}
		if (isa<ExtractElementInst>(I) || isa<InsertElementInst>(I) || isa<ShuffleVectorInst>(I)) {
		    errs() << "Found a vector instruction in original code: " << *I << "\n";
			assert(!"[simd-swift] do not know how to transform vector instructions");
			return;
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

		if (isa<SwitchInst>(I)) {
			// switches are only for integers, silently ignore
			return;
		}

		if (BranchInst* BI = dyn_cast<BranchInst>(I)) {
			if (BI->isUnconditional())
				return;
			if (isa<Constant>(BI->getCondition()))
				return;
 
			Value* avxcond = simds.getSimd(BI->getCondition(), BI);
			if (!avxcond)
				return;

			// ptest works only on <4 x i64> so need to bitcast
			avxcond = irBuilderBefore.CreateBitCast(avxcond,
						VectorType::get(Type::getInt64Ty(getGlobalContext()), 4));

			// TRICKY PART FOLLOWS:
			// insert minimal LLVM code to branch on condition using AVX ptestz
			// code is adapted from test/CodeGen/X86/avx-brcond.ll and vec_setcc.ll

			// first insert AVX ptestz intrinsic that ADDs with all-ones vector
			// then  insert not-equal comparison with 0
			std::vector<Value *> args;
			args.push_back(avxcond);
			args.push_back(getSimdAllOnes());

			tocheck.push_back(std::tuple<Instruction*, Instruction*, unsigned>(BI, cast<Instruction>(avxcond), 0));

			Function *ptestz = Intrinsic::getDeclaration(module, Intrinsic::x86_avx_ptestz_256);
			Value* res = irBuilderBefore.CreateCall(ptestz, args);
			Value* newcond = irBuilderBefore.CreateICmpEQ(res, ConstantInt::get(Type::getInt32Ty(getGlobalContext()), 0));
			// finally substitute condition in original branch
			BI->setCondition(newcond);
			return;
		}

		if (isa<IndirectBrInst>(I)) {
			// indirect branches are only for pointers, silently ignore
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

			// special case when fcmp result is used in binary operations, e.g., 'or'
			bool specialcase = false;
			if (simdOp0 && simdOp0->getType()->getScalarType()->isIntegerTy(64)) {
				simdOp0 = irBuilderBefore.CreateExtractElement(simdOp0, (uint64_t)0);
				simdOp0 = irBuilderBefore.CreateTrunc(simdOp0, IntegerType::get(getGlobalContext(), 1));
				I->setOperand(0, simdOp0);
				specialcase = true;
			}
			if (simdOp1 && simdOp1->getType()->getScalarType()->isIntegerTy(64)) {
				simdOp1 = irBuilderBefore.CreateExtractElement(simdOp1, (uint64_t)0);
				simdOp1 = irBuilderBefore.CreateTrunc(simdOp1, IntegerType::get(getGlobalContext(), 1));
				I->setOperand(1, simdOp1);
				specialcase = true;
			}
			if (specialcase)
				return;

			if (!simdOp0 || !simdOp1) {
				// not floats/doubles, silently ignore
				return;
			}


			IS = BinaryOperator::Create(BO->getOpcode(), simdOp0, simdOp1, I->getName() + SIMD_SUFFIX);
			(cast<BinaryOperator>(IS))->copyIRFlags(I);
		}

		if (isa<ICmpInst>(I)) {
			// integer compares, silently ignore
			return;
		}

		if (FCmpInst* FCI = dyn_cast<FCmpInst>(I)) {
			// modify all comparisons to return <4 x i64> for uniformity
			Value* simdOp0 = simds.getSimd(I->getOperand(0), I);
			Value* simdOp1 = simds.getSimd(I->getOperand(1), I);

			Value* cmp = irBuilderBefore.CreateFCmp(FCI->getPredicate(), simdOp0, simdOp1);

			// cmp result is <4/8/16/32 x i1>, we need <4 x i64> --> sign extend and bitcast
			unsigned simdNum = cmp->getType()->getVectorNumElements();
			cmp = irBuilderBefore.CreateSExt(cmp, 
						VectorType::get(IntegerType::get(getGlobalContext(), 256/simdNum), simdNum));
			cmp = irBuilderBefore.CreateBitCast(cmp,
						VectorType::get(Type::getInt64Ty(getGlobalContext()), 4));

			simds.add(I, cmp);
			origs.push_back(I);
			return;
		}

		if (SelectInst* SI = dyn_cast<SelectInst>(I)) {
			Value* simdCondition  = simds.getSimd(SI->getCondition(), SI);
			Value* simdTrueValue  = simds.getSimd(SI->getTrueValue(), SI);
			Value* simdFalseValue = simds.getSimd(SI->getFalseValue(), SI);

			if (!simdTrueValue || !simdFalseValue) {
				// selected value is not float/double, silently ignore
				return;
			}

			Value* i1Cond = nullptr;
			if (simdCondition) {
				// simdCondition is <4 x i64>, we need <N x i1> where N is the number of elements of true/false value
				unsigned numel = simdTrueValue->getType()->getVectorNumElements();
				Type* i1CondType = VectorType::get(IntegerType::get(getGlobalContext(), 1), numel);
				i1Cond = irBuilderBefore.CreateBitCast(simdCondition, VectorType::get(IntegerType::get(getGlobalContext(), 256/numel), numel));
				i1Cond = irBuilderBefore.CreateTrunc(i1Cond, i1CondType);
			} else {
				i1Cond = SI->getCondition();
			}

			IS = SelectInst::Create(i1Cond, simdTrueValue, simdFalseValue, I->getName() + SIMD_SUFFIX);
		}

		if (isa<GetElementPtrInst>(I)) {
			// works with pointers, so ignore
			return;
		}

		if (CastInst* CI = dyn_cast<CastInst>(I)) {
			if (isa<AddrSpaceCastInst>(I) || isa<IntToPtrInst>(I) || isa<PtrToIntInst>(I) ||
				isa<SExtInst>(I) || isa<ZExtInst>(I) || isa<TruncInst>(I)) {
				// casts dealing with integers/pointers, ignore
				return;
			}

			if (isa<BitCastInst>(I)) {
				// TODO: probably bitcasts concerning floats/doubles, need to deal with them
				if (CI->getOperand(0)->getType()->isFloatTy() || CI->getOperand(0)->getType()->isDoubleTy()) {
					extractSimdOpAndSubstitute(irBuilderBefore, CI, 0);
				}
				else if (CI->getDestTy()->isFloatTy() || CI->getDestTy()->isDoubleTy()) {
					Value* fpSimd = createSimdValue(irBuilderAfter, CI);
					simds.add(I, fpSimd);
				}
				return;
			}

			if (isa<FPExtInst>(I)) {
				// only possible option: <8 x float> to <4 x double>
				Value* simdValue  = simds.getSimd(CI->getOperand(0), I);
				Type*  simdDestTy = getSimdType(CI->getDestTy());

				UndefValue* undefValue = UndefValue::get(simdValue->getType());
				std::vector<Constant*> consts;
				for (unsigned i = 0; i < 4; i++)
					consts.push_back(ConstantInt::get(Type::getInt32Ty(getGlobalContext()), (uint64_t)i));
				Value* mask = ConstantVector::get(consts);
				simdValue = irBuilderBefore.CreateShuffleVector(simdValue, undefValue, mask);

				IS = CastInst::Create(CI->getOpcode(), simdValue, simdDestTy, I->getName() + SIMD_SUFFIX);
			}

			if (isa<FPTruncInst>(I)) {
				// only possible option: <4 x double> to <8 x float>
				Value* simdValue  = simds.getSimd(CI->getOperand(0), I);
				Type*  simdDestTy = getSimdType(CI->getDestTy());

				UndefValue* undefValue = UndefValue::get(simdValue->getType());
				std::vector<Constant*> consts;
				for (unsigned i = 0; i < 8; i++)
					consts.push_back(ConstantInt::get(Type::getInt32Ty(getGlobalContext()), (uint64_t)i % 4));
				Value* mask = ConstantVector::get(consts);
				simdValue = irBuilderBefore.CreateShuffleVector(simdValue, undefValue, mask);

				IS = CastInst::Create(CI->getOpcode(), simdValue, simdDestTy, I->getName() + SIMD_SUFFIX);
			}

			if (isa<FPToSIInst>(I) || isa<FPToUIInst>(I)) {
				extractSimdOpAndSubstitute(irBuilderBefore, CI, 0);
				return;
			}

			if (isa<SIToFPInst>(I) || isa<UIToFPInst>(I)) {
				Value* fpSimd = createSimdValue(irBuilderAfter, CI);
				simds.add(I, fpSimd);
				return;
			}
		}		

		// --- Phis are special --- //
		if (PHINode* PI = dyn_cast<PHINode>(I)) {
			Type*  simdTy = getSimdType(PI->getType());
			if (!simdTy)
				return;
			// remember to rewire this Phi afterwards
			IS = PHINode::Create(simdTy, PI->getNumIncomingValues(), I->getName() + SIMD_SUFFIX);
			phis.push_back(PI);
		}

		// --- memory related --- //
		if (isa<AllocaInst>(I)) {
			// works with pointers and integers, silently ignore
			return;
		}

		if (LoadInst* LI = dyn_cast<LoadInst>(I)) {
			// move loaded value into simd vector if it's float/double
			if (!LI->getType()->isFloatTy() && !LI->getType()->isDoubleTy())
				return;
			Value* loadedSimd = createSimdValue(irBuilderAfter, LI);
			simds.add(I, loadedSimd);
			return;
		}

		if (StoreInst* SI = dyn_cast<StoreInst>(I)) {
			extractSimdOpAndSubstitute(irBuilderBefore, SI, 0);
			return;
		}

		if (AtomicCmpXchgInst* AXI = dyn_cast<AtomicCmpXchgInst>(I)) {
			// cmpxchg returns a struct {ty, i1}, we don't want to mess with it
		    errs() << "Found a cmpxchg instruction in original code: " << *AXI << "\n";
			assert(!"[simd-swift] do not know how to transform cmpxchg instructions");
			return;
		}

		if (AtomicRMWInst* ARI = dyn_cast<AtomicRMWInst>(I)) {
			if (!ARI->getType()->isFloatTy() && !ARI->getType()->isDoubleTy())
				return;
			extractSimdOpAndSubstitute(irBuilderBefore, I, 1);
			Value* readSimd = createSimdValue(irBuilderAfter, ARI);
			simds.add(I, readSimd);
			return;
		}

		if (isa<ExtractValueInst>(I)) {
			return;
		}
		if (isa<InsertValueInst>(I)) {
			return;
		}

		// --- function calls --- //
		if (CallInst* CI = dyn_cast<CallInst>(I)) {
			// --- do not shadow calls to "ignored" functions
			if (isIgnoredFunc(CI->getCalledFunction()))
			 	return;

			if (CI->isInlineAsm()) {
				InlineAsm* IA = cast<InlineAsm>(CI->getCalledValue());
				if (IA->getAsmString().empty()) {
					// it is an empty asm, used sometimes to turn off optimizations
					// not hurting for us, so let's skip it
					return;
				}
				// TODO: we do not work with inline assembly right now
				errs() << "Found inline assembly " << *I << "\n";
				assert(!"[simd-swift] cannot handle inline assembly");
				return;
			}

			// --- in regular cases, check args before and replicate after the call
			for (unsigned idxArgOp = 0; idxArgOp < CI->getNumArgOperands(); idxArgOp++) {
				// extract each argument (note that ArgOperands map to Operands)
				extractSimdOpAndSubstitute(irBuilderBefore, CI, idxArgOp);
			}
			// move return value into simd vector if it's float/double
			if (CI->getType()->isFloatTy() || CI->getType()->isDoubleTy()) {
				Value* retSimd = createSimdValue(irBuilderAfter, CI);
				simds.add(I, retSimd);
			}
			return;
		}

		// --- random stuff --- //
		if (isa<VAArgInst>(I)) {
			// works on pointers, silently ignore
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

		// make a SIMD version for each function arg if it's float/double
		for (auto arg = F.arg_begin(); arg != F.arg_end(); ++arg){
			if (arg->getType()->isFloatTy() || arg->getType()->isDoubleTy()) {
				Value* simdarg = createSimdValue(irBuilder, arg);
				simds.add(arg, simdarg);
			}
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
			if (I->getNumUses() > 0) {
			    errs() << "[simd-swift warning] instruction is still used and cannot be removed: " << *I << "\n";
			    continue;
			}
			I->eraseFromParent();
		}
	}

	void insertChecks() {
		if (NoCheckOnAll)
			return;

		for (auto tocheckIt = tocheck.rbegin(); tocheckIt != tocheck.rend(); ++tocheckIt) {
			Instruction* userI = std::get<0>(*tocheckIt);
			Instruction* I  = std::get<1>(*tocheckIt);
			unsigned opIdx  = std::get<2>(*tocheckIt);

			if (isa<BranchInst>(userI)) {
				if (NoCheckOnBranch) continue;

				// --- check on branches --- //
				BasicBlock::iterator it(userI);
				IRBuilder<> irBuilderBefore(it);

				std::vector<Value *> args;
				args.push_back(I);
				args.push_back(getSimdAllOnes());

				Function *ptestnzc = Intrinsic::getDeclaration(module, Intrinsic::x86_avx_ptestnzc_256);
				Value* check = irBuilderBefore.CreateCall(ptestnzc, args);
				Value* checkcond = irBuilderBefore.CreateICmpEQ(check, ConstantInt::get(Type::getInt32Ty(getGlobalContext()), 1));

				llvm::MDBuilder mdBuilder(llvm::getGlobalContext());
				TerminatorInst* checkTerm = SplitBlockAndInsertIfThen(checkcond, it, true, mdBuilder.createBranchWeights(1, 10000));
				IRBuilder<> irBuilderCheck(checkTerm);
				Value* correctedI = irBuilderCheck.CreateCall(mask_i64, I);

				BranchInst* origBI  = cast<BranchInst>(userI);
				BranchInst* cloneBI = cast<BranchInst>(userI->clone());

				std::vector<Value *> args2;
				args2.push_back(correctedI);
				args2.push_back(getSimdAllOnes());
				Function *ptestz = Intrinsic::getDeclaration(module, Intrinsic::x86_avx_ptestz_256);
				Value* res = irBuilderCheck.CreateCall(ptestz, args2);
				Value* newcond = irBuilderCheck.CreateICmpEQ(res, ConstantInt::get(Type::getInt32Ty(getGlobalContext()), 0));
				cloneBI->setCondition(newcond);
				irBuilderCheck.Insert(cloneBI);

				// need to add newly inserted BB to PHINodes that used original BB
				for (unsigned succidx = 0; succidx < origBI->getNumSuccessors(); succidx++) {
					BasicBlock *Succ = origBI->getSuccessor(succidx);
				    for (auto II = Succ->begin(), IE = Succ->end(); II != IE; ++II) {
				        PHINode *PN = dyn_cast<PHINode>(II);
				        if (!PN)  break;

				        Value* v = nullptr;
				        if ((v = PN->getIncomingValueForBlock(origBI->getParent())) != nullptr)
				         	PN->addIncoming(v, cloneBI->getParent());
					}
				}

				checkTerm->eraseFromParent();
				continue;
			}

			// --- check on non-branches: loads and stores --- //
			if (!I) continue;
			if (NoCheckOnStore && isa<StoreInst>(userI)) continue;
			if (NoCheckOnAtomic && (isa<AtomicCmpXchgInst>(userI) || isa<AtomicRMWInst>(userI))) continue;
			if (NoCheckOnCall && isa<CallInst>(userI)) continue;

			BasicBlock::iterator it(userI);
			IRBuilder<> irBuilderBefore(it);

			Value* newVecOp = nullptr;
			if (I->getType()->getVectorElementType()->isDoubleTy()) {
				newVecOp = irBuilderBefore.CreateCall(check_double, I);
			} else if (I->getType()->getVectorElementType()->isFloatTy()) {
				newVecOp = irBuilderBefore.CreateCall(check_float, I);
			} else {
			    errs() << "don't know how to handle type " << *I->getType() << "\n";
				assert(!"cannot work on not float/double types");
				break;
			}

			// extract from SIMD pointer operand
			Value* newOp = irBuilderBefore.CreateExtractElement(newVecOp, (uint64_t)0);
			userI->setOperand(opIdx, newOp);
		}
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
		errs() << "[RUNNING PASS: avxfloatswift]\n";
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
		swifter.insertChecks();
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
static RegisterPass<SwiftPass> X("avxswift", "AVX-Swift Pass");

}
