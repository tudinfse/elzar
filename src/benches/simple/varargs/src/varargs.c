#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

__attribute__ ((noinline))
int varargfunc(const char *fmt, ...) {
    char val_str[10];
    int vlen;
    va_list ap;

    va_start(ap, fmt);
    vlen = vsnprintf(val_str, sizeof(val_str) - 1, fmt, ap);
    va_end(ap);

    return vlen;
}

int main(int argc, char** argv) {
  int r = varargfunc("%d\n", 123);
  printf("r: %d \n", r);
  return 0;
}

