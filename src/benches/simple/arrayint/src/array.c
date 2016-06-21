#include <stdio.h>
#include <stdlib.h>

#define MAXSIZE 1000*1000

#define MYINTTYPE long

MYINTTYPE arr[MAXSIZE];

__attribute__ ((noinline))
void arraywrite(MYINTTYPE* arr, int size) {
  int i = size-1;
  for (; i >= 0; i--) {
    arr[i] = i;
  }
}

__attribute__ ((noinline))
MYINTTYPE arrayread(MYINTTYPE* arr, int size) {
  MYINTTYPE r = 0;
  int i = size-1;
  for (; i >= 0; i--) {
    r += arr[i];
  }
  return r;
}

int main(int argc, char** argv) {
  int size = MAXSIZE;
  if (argc >= 2) {
    size = atoi(argv[1]);
  }

  MYINTTYPE r = 0;
  int j = 0;
  for (; j < 1000; j++) {
    arraywrite(arr, size);
    r += arrayread(arr, size);
  }

  // r overflows, so the result is kinda meaningless
  printf("sum: %d \n", r / 100000);
  return 0;
}
