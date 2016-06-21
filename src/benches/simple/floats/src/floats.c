#include <stdio.h>

__attribute__ ((noinline))
float foo(float x) {
  double y = 0.0;
  y = x + x;
  return (float)y;
}

__attribute__ ((noinline))
int bar(int x, float y) {
  float z = (int)x;
  z = z * y;
  return (int)z;
}

__attribute__ ((noinline))
char foobar(double x, double y) {
  if (x > y)
    return (char)x;
  if (x == y)
    return (char)y;
  return 0;
}

int main() {
  int r = 0;
  r += (int)foo((float)r);
  r += 1;
  r += bar(r, (float)r);
  r += 2;
  r += foobar((double)r, (double)r);
  r += 3;
  return r;
}
