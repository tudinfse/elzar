NAME= libc-util
SRC = bzero memcpy memmove memset memcmp memchr strcmp strncmp strcat strlen strcpy strncpy strchr strrchr strstr strcasecmp strncasecmp strspn strchrnul strcspn strpbrk \
      isdigit islower isspace isupper toupper tolower \
      exp exp2 sqrt sqrtf log log10 scalbn scalbnf fabs fabsf pow powf modf modff ceil ceilf finite floor floorf cbrt cbrtf ldexp ldexpf nan frexp hypot

include ../../Makefile.common

ifeq ($(ACTION),native)
  CCFLAGS := $(CCFLAGS) -msse4.2 -mavx2
else
  CCFLAGS := $(CCFLAGS) -mno-avx -msse -fno-slp-vectorize -fno-vectorize
endif

all:: $(BUILDPATH)/$(NAME).bc

clean::
	rm -rf $(BUILDPATH)

$(BUILDPATH)/$(NAME).bc: $(addprefix $(BUILDPATH)/, $(LLS))
	$(LLVM_LINK) -o $@ $^



