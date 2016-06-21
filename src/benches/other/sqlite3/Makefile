# sqlite3 version 3.8.9, with YCSB workloads

NAME= sqlite3
SRC = sqlite3
SRC2= bench

CCFLAGS := -DNDEBUG -DSQLITE_THREADSAFE=1 -DSQLITE_OMIT_LOAD_EXTENSION $(CCFLAGS)

CCFLAGS := $(CCFLAGS) '-DLONGDOUBLE_TYPE=double' # do not use long-double (x86_fp80 in LLVM)

CCFLAGS := $(CCFLAGS) -U__OPTIMIZE__  # disable agressive "inlined asm" optimizations in libc

include ../../Makefile.$(ACTION)
