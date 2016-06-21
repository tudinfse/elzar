# NOTE: this Swift takes no-simd (no_sse) native version as input
#       this hampers Swift a bit, but the comparison with AVX makes more sense

MKFILE_PATH := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
include $(MKFILE_PATH)/Makefile.common

UTILS := $(UTILS) $(MKFILE_PATH)/util/libc/build/native_nosse/libc-util.bc

# for final executable flags are with sse enabled...
FINALCCFLAGS := $(CCFLAGS) -mcpu=haswell -msse4.2
# ...but not for original files
CCFLAGS := $(CCFLAGS) -mno-avx -msse -fno-slp-vectorize -fno-vectorize

SWIFT_RUNTIME  = $(SWIFT_PATH)/runtime/swift.ll.checks-exit
SWIFT_PASSFILE = $(SWIFT_PATH)/pass/swift_pass.so
SWIFT_PASSNAME = -sswift

all:: $(BUILDPATH)/$(NAME)

clean::
	rm -f $(BUILDPATH)/$(NAME) $(BUILDPATH)/$(NAME).linked.bc $(BUILDPATH)/$(NAME).renamed.bc $(BUILDPATH)/$(NAME).toswift.bc $(BUILDPATH)/$(NAME).noinline.bc $(BUILDPATH)/$(NAME).final.bc

# link all sources + utils
$(BUILDPATH)/$(NAME).linked.bc: $(addprefix $(BUILDPATH)/, $(LLS)) $(UTILS)
	$(LLVM_LINK) -o $@ $^

# substitute libc functions + inline
$(BUILDPATH)/$(NAME).renamed.bc: $(BUILDPATH)/$(NAME).linked.bc
	$(LLVM_OPT) -load $(RENAME_PASSFILE) $(RENAME_PASSNAME) -inline $^ -o $@

# link all sources-to-process + runtime
$(BUILDPATH)/$(NAME).toswift.bc: $(BUILDPATH)/$(NAME).renamed.bc $(SWIFT_RUNTIME)
	$(LLVM_LINK) -o $@ $^

# swiftify
$(BUILDPATH)/$(NAME).final.bc: $(BUILDPATH)/$(NAME).toswift.bc
	$(LLVM_OPT) -load $(SWIFT_PASSFILE) $(SWIFT_PASSNAME) $(SWIFT_PASS_FLAGS) $^ -o $(BUILDPATH)/$(NAME).noinline.bc
	$(LLVM_OPT) -always-inline $(BUILDPATH)/$(NAME).noinline.bc -o $@

# executable
$(BUILDPATH)/$(NAME): $(BUILDPATH)/$(NAME).final.bc $(addprefix $(BUILDPATH)/, $(LLS2))
	$(LLVM_CLANGPP) $(FINALCCFLAGS) -o $@ $^ -I $(INCLUDE_DIRS) -L $(LIB_DIRS) $(LIBS)

