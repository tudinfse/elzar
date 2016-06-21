#include <stdio.h>
#include <stdint.h>

__attribute__ ((noinline))
int8_t truncation(int64_t x, int64_t y, int64_t z) {
  asm (""); // not to optimize the function away
  return (int8_t)x + (int8_t)y + (int8_t)z;
}

int main() {
  int64_t i;
  int64_t x = 42;
  int8_t  r = 0;
  for (i = 0; i < 1000*1000*1000; i++)
    r += truncation(x, x, x);
  return r;
}
