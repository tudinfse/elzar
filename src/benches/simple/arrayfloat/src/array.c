#include <stdio.h>
#include <stdlib.h>

#define MAXSIZE 1000*1000

#define MYTYPE float

MYTYPE arr[MAXSIZE];

__attribute__ ((noinline))
void arraywrite(MYTYPE* arr, int size) {
  int i = size-1;
  MYTYPE f = (MYTYPE)i;
  for (; i >= 0; i--) {
    arr[i] = f;
    f -= 1.0;
  }
}

__attribute__ ((noinline))
MYTYPE arrayread(MYTYPE* arr, int size) {
  MYTYPE r = 0.0;
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

  MYTYPE r = 0.0;
  int j = 0;
  for (; j < 1000; j++) {
    arraywrite(arr, size);
    r += arrayread(arr, size);
  }

  printf("sum: %f \n", r);
  return 0;
}
