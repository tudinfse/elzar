//===------- avxswift.cpp - SIMD Swift harden pass ------------------------===//
//
//	 This pass duplicates all instructions ("swiftifies" a program) using
//   SIMD instructions (AVX for x86-64) and inserts majority voting at sync 
//   points: stores, branches, calls, etc.
//
//     - Pass replicates data in the whole 256-bit register, e.g., <4 x i64>,
//       <8 x float>, <32 x i8>.
//
//     - We assume, for simplicity, that only _one_ copy can be corrupted.
//
//     - Majority voting uses ptestz & ptestnzc AVX instructions and is unusual:
//       first the whole 256-bit register is checked that all copies are correct,
//       if not the low 2 copies are compared and if good are broadcasted to all,
//       if not the high 2 copies are broadcasted to all (because by assumption)
//       if one of the low copies is corrupted, then the high copies are good.
//
//     - Checks on branches are implemented as shadow Basic Blocks and NOT as
//       update to branch condition (in contrast to checks on loads/stores).
//       This is because CodeGen cannot peephole-optimize sequence of AVX instr
//       scattered between several BBs. The code is ugly, but produces minimal
//       ultra-fast assembly.
//
//
//   TODO:
//     - SSE/AVX doesn't have truncation int -> int. This leads to cumbersome
//       slow assembly in cases like "int x = (char) x", up to 7x overhead.
//
//     - There is no way to postpone or ignore checks on loads since we do
//       majority voting. If ignoring the checks, a segfault can happen, and
//       program crashes (note: there is slight hope that segfault handler
//       could be of some help). But we target availability, so we must mask
//       such faults. To this end, we must check each load address before load.
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
	NoCheckOnLoad("no-check-load", cl::Optional, cl::init(false),
	cl::desc("Disable checks on loads"));
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
Function* check_i64;
Function* check_i32;
Function* check_i16;
Function* check_i8;

void findHelperFuncs(Module &M) {
	exitfunc    = M.getFunction("SIMDSWIFT_exit");
	mask_i64    = M.getFunction("SIMDSWIFT_mask_i64");
	check_double= M.getFunction("SIMDSWIFT_check_double");
	check_float = M.getFunction("SIMDSWIFT_check_float");
	check_i64   = M.getFunction("SIMDSWIFT_check_i64");
	check_i32   = M.getFunction("SIMDSWIFT_check_i32");
	check_i16   = M.getFunction("SIMDSWIFT_check_i16");
	check_i8    = M.getFunction("SIMDSWIFT_check_i8");

	assert(exitfunc && mask_i64 && check_double && check_float && check_i64 && check_i32 && check_i16 && check_i8 &&
		"SIMDSWIFT functions are not found (requires linked runtime");
}

unsigned getSimdNum(Type* t) {
	switch (t->getTypeID()) {
	case Type::IntegerTyID: {
		unsigned s = t->getPrimitiveSizeInBits();
		if (s == 1)
			return 4;
		if (s != 8 && s != 16 && s != 32 && s != 64) {
		    errs() << "[simd-swift warning] handling illegal type " << *t << "\n";
		    return 4;
		}
		return 256 / s;
	}
	case Type::PointerTyID: return 4;
	case Type::DoubleTyID: return 4;
	case Type::FloatTyID: return 8;
	default:                
	    errs() << "don't know how to handle type " << *t << "\n";
		assert(!"cannot work on not 8-, 16-, 32- and 64-bit types");
		break;
	}
	return 0; // unreachable
}

Type* getSimdType(Type* t) {
	if (t->isIntegerTy(1)) 
		return VectorType::get(Type::getInt64Ty(getGlobalContext()), 4);
	return VectorType::get(t, getSimdNum(t));
}

bool isSimdType(Type* t) {
	if (t->isVectorTy() && t->getVectorNumElements() == getSimdNum(t))
		return true;
	return false;
}

Value* createSimdValue(IRBuilder<>& irBuilder, Value* v) {
	if (v->getType()->isIntegerTy(1))
		v = irBuilder.CreateZExt(v, Type::getInt64Ty(getGlobalContext()));
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

		if (Constant* c = dyn_cast<Constant>(v)) {
			if (c->getType()->isIntegerTy(1))
				c = ConstantExpr::getSExt(c, Type::getInt64Ty(getGlobalContext()));
			unsigned num = getSimdNum(c->getType());
			if (isa<GetElementPtrInst>(I))
				num = 4;  // for GEPs, always need 4 copies for each index arg
			return ConstantVector::getSplat(num, c);
		}

		// no checks for BBs (labels), function declarations, inline asm and metadata
		if (isa<BasicBlock>(v) || isa<Function>(v) ||
			isa<InlineAsm>(v)  || isa<MetadataAsValue>(v) || 
			isa<InvokeInst>(v) || isa<LandingPadInst>(v))
			return nullptr;

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
			if (newOp->getType()->getPrimitiveSizeInBits() > I->getOperand(idxOp)->getType()->getPrimitiveSizeInBits())
				newOp = irBuilder.CreateTrunc(newOp, I->getOperand(idxOp)->getType());
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
			if (RI->getReturnValue() && !RI->getReturnValue()->getType()->isStructTy()) {
				// if ret is not void and not struct (see also insert/extractvalue)
				extractSimdOpAndSubstitute(irBuilderBefore, RI, 0);
			}
			return;
		}

		if (SwitchInst* SI = dyn_cast<SwitchInst>(I)) {
			extractSimdOpAndSubstitute(irBuilderBefore, SI, 0);
			return;
		}

		if (BranchInst* BI = dyn_cast<BranchInst>(I)) {
			if (BI->isUnconditional())
				return;			
			if (isa<Constant>(BI->getCondition()))
				return;
 
			Value* avxcond = simds.getSimd(BI->getCondition(), BI);

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
			// modify all comparisons to return <4 x i64> for uniformity
			Value* simdOp0 = simds.getSimd(I->getOperand(0), I);
			Value* simdOp1 = simds.getSimd(I->getOperand(1), I);

			Value* cmp = nullptr;
			if (isa<ICmpInst>(CI))
				cmp = irBuilderBefore.CreateICmp(CI->getPredicate(), simdOp0, simdOp1);
			else
				cmp = irBuilderBefore.CreateFCmp(CI->getPredicate(), simdOp0, simdOp1);

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

			// simdCondition is <4 x i64>, we need <N x i1> where N is the number of elements of true/false value
			unsigned numel = simdTrueValue->getType()->getVectorNumElements();
			Type* i1CondType = VectorType::get(IntegerType::get(getGlobalContext(), 1), numel);
			Value* i1Cond = irBuilderBefore.CreateBitCast(simdCondition, VectorType::get(IntegerType::get(getGlobalContext(), 256/numel), numel));
			i1Cond = irBuilderBefore.CreateTrunc(i1Cond, i1CondType);

			IS = SelectInst::Create(i1Cond, simdTrueValue, simdFalseValue, I->getName() + SIMD_SUFFIX);
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

			if (I->getOperand(0)->getType()->isIntegerTy(1)) {
				// i1 was transformed to <4 x i64>, truncate it back to <4 x i1> for uniformity
				Type* i1CondType = VectorType::get(IntegerType::get(getGlobalContext(), 1), simdValue->getType()->getVectorNumElements());
				simdValue = irBuilderBefore.CreateTrunc(simdValue, i1CondType);
			}

			unsigned simdNumSrc = simdValue->getType()->getVectorNumElements();
			unsigned simdNumDst = simdDestTy->getVectorNumElements();
			if (simdNumSrc != simdNumDst) {
				// e.g., <8 x i32> to <4 x i64> --> need to take 4 lower elements, i.e., make <4 x i32>
				// e.g., <4 x i64> to <32 x i8> --> need to replicate to 32 elements, i.e., make <32 x i64>
				// TODO: this probably results in a very inefficient AVX code, check and write smth better
				UndefValue* undefValue = UndefValue::get(simdValue->getType());
				std::vector<Constant*> consts;
				for (unsigned i = 0; i < simdNumDst; i++)
					consts.push_back(ConstantInt::get(Type::getInt32Ty(getGlobalContext()), (uint64_t)i % simdNumSrc));
				Value* mask = ConstantVector::get(consts);
				simdValue = irBuilderBefore.CreateShuffleVector(simdValue, undefValue, mask);
			}

			IS = CastInst::Create(CI->getOpcode(), simdValue, simdDestTy, I->getName() + SIMD_SUFFIX);
		}		

		// --- Phis are special --- //
		if (PHINode* PI = dyn_cast<PHINode>(I)) {
			if (PI->getType()->isStructTy()) {
				// in a extractvalue corner-case, PHI node can be used to drag
				// a returned struct to another BB, detect & ignore this case
				return;
			}
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

		// --- extractvalue/insertvalue for structs --- //
		// This is a corner-case: we rely on -scalarrepl to replace all aggregates,
		// but in cases of funcs returning structs, the structs are filled with
		// insertvalue by callee and decomposed with extractvalue by caller.
		// We consider only this case and consider all extracts/inserts to work
		// on scalar structs -- since we decode on func boundaries anyway.
		if (ExtractValueInst* EVI = dyn_cast<ExtractValueInst>(I)) {
			// extracted a return value of func, replicate it for future use
			// see also CallInst
			Value* extractedSimd = createSimdValue(irBuilderAfter, EVI);
			simds.add(I, extractedSimd);
			return;
		}

		if (InsertValueInst* IVI = dyn_cast<InsertValueInst>(I)) {
			// inserting a value into return struct of func, see also ReturnInst
			// we need to substitute only the value-to-insert operand
			extractSimdOpAndSubstitute(irBuilderBefore, IVI, IVI->getInsertedValueOperandIndex());
			return;
		}

		// --- function calls --- //
		if (CallInst* CI = dyn_cast<CallInst>(I)) {
			// --- special treatment for some LLVM intrinsics
			if (CI->getCalledFunction() && CI->getCalledFunction()->getName().startswith("llvm.bswap")) {
				// bswap works on up to 256-bit integers, so we need to bitcast its simd arg
				IntegerType *intTy = Type::getIntNTy(getGlobalContext(), 256);
				Value* simdArg = simds.getSimd(CI->getArgOperand(0), CI);

				Value* tobswap  = irBuilderBefore.CreateBitCast(simdArg, intTy);
				Function *bswap = Intrinsic::getDeclaration(module, Intrinsic::bswap, intTy);
  				Value* bswapped = irBuilderBefore.CreateCall(bswap, tobswap);
				Value* bitcasted= irBuilderBefore.CreateBitCast(bswapped, simdArg->getType(), I->getName() + SIMD_SUFFIX);

				simds.add(I, bitcasted);
				origs.push_back(I);
  				return;
			}

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
			if (CI->getCalledFunction() == nullptr) {				
				// also, if a function pointer call, extract function pointer
				Value* simdFuncPtr = simds.getSimd(CI->getCalledValue(), I);	
				Value* newFuncPtr  = irBuilderBefore.CreateExtractElement(simdFuncPtr, (uint64_t)0);
				CI->setCalledFunction(newFuncPtr);

				// mark that we need to check the SIMD function pointer
				tocheck.push_back(std::tuple<Instruction*, Instruction*, unsigned>(CI, dyn_cast<Instruction>(simdFuncPtr), -1));
			}

			for (unsigned idxArgOp = 0; idxArgOp < CI->getNumArgOperands(); idxArgOp++) {
				// extract each argument (note that ArgOperands map to Operands)
				extractSimdOpAndSubstitute(irBuilderBefore, CI, idxArgOp);
			}
			// move return value into simd vector (if there is return value and
			// it's not a struct, see also extractvalue/insertvalue)
			if (!CI->getType()->isVoidTy() && !CI->getType()->isStructTy()) {
				Value* retSimd = createSimdValue(irBuilderAfter, CI);
				simds.add(I, retSimd);
			}
			return;
		}

		// --- random stuff --- //
		if (VAArgInst* VAI = dyn_cast<VAArgInst>(I)) {
			extractSimdOpAndSubstitute(irBuilderBefore, I, 0);
			// move read VA value into simd vector
			Value* vaSimd = createSimdValue(irBuilderAfter, VAI);
			simds.add(I, vaSimd);
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

		Type* i64SimdTy = getSimdType(Type::getInt64Ty(getGlobalContext()));

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
			if (NoCheckOnLoad && isa<LoadInst>(userI)) continue;
			if (NoCheckOnStore && isa<StoreInst>(userI)) continue;
			if (NoCheckOnAtomic && (isa<AtomicCmpXchgInst>(userI) || isa<AtomicRMWInst>(userI))) continue;
			if (NoCheckOnCall && isa<CallInst>(userI)) continue;

			BasicBlock::iterator it(userI);
			IRBuilder<> irBuilderBefore(it);

			Value* newVecOp = nullptr;
			if (I->getType()->getVectorElementType()->isPointerTy()) {
				// all pointers are casted to i64
				Value* casted = irBuilderBefore.CreatePtrToInt(I, i64SimdTy);
				Value* corrected = irBuilderBefore.CreateCall(check_i64, casted);
				newVecOp = irBuilderBefore.CreateIntToPtr(corrected, I->getType());
			} else if (I->getType()->getVectorElementType()->isIntegerTy(64)) {
				newVecOp = irBuilderBefore.CreateCall(check_i64, I);
			} else if (I->getType()->getVectorElementType()->isIntegerTy(32)) {
				newVecOp = irBuilderBefore.CreateCall(check_i32, I);
			} else if (I->getType()->getVectorElementType()->isIntegerTy(16)) {
				newVecOp = irBuilderBefore.CreateCall(check_i16, I);
			} else if (I->getType()->getVectorElementType()->isIntegerTy(8)) {
				newVecOp = irBuilderBefore.CreateCall(check_i8, I);
			} else if (I->getType()->getVectorElementType()->isDoubleTy()) {
				newVecOp = irBuilderBefore.CreateCall(check_double, I);
			} else if (I->getType()->getVectorElementType()->isFloatTy()) {
				newVecOp = irBuilderBefore.CreateCall(check_float, I);
			} else {
			    errs() << "don't know how to handle type " << *I->getType() << "\n";
				assert(!"cannot work on not 8-, 16-, 32- and 64-bit types");
				break;
			}

			// extract from SIMD pointer operand
			Value* newOp = irBuilderBefore.CreateExtractElement(newVecOp, (uint64_t)0);

			// substitute in instr
			if (CallInst* CI = dyn_cast<CallInst>(userI)) {
				if (CI->getCalledFunction() == nullptr && opIdx == (unsigned)-1) {
					// special case of function pointers, need a special method
					CI->setCalledFunction(newOp);
					continue;
				}
			}

			if (newOp->getType()->getPrimitiveSizeInBits() > userI->getOperand(opIdx)->getType()->getPrimitiveSizeInBits())
				newOp = irBuilderBefore.CreateTrunc(newOp, userI->getOperand(opIdx)->getType());
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
		errs() << "[RUNNING PASS: avxswift]\n";
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
