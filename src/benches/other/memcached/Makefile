# memcached 1.4.24

NAME= memcached
SRC = memcached hash jenkins_hash murmur3_hash slabs items assoc thread daemon stats util cache  


CCFLAGS := -std=gnu99 -DHAVE_CONFIG_H -I. -DNDEBUG -Wall #-Werror -pedantic

CCFLAGS := $(CCFLAGS) -U__OPTIMIZE__  # disable agressive "inlined asm" optimizations in libc

LIBS := -levent $(LIBS)

include ../../Makefile.$(ACTION)
