#include "harness.h"
static double kernel(void) {
  double s = 0, sign = 1;
  for (long i = 0; i < 50000000; i++) s += sign / (2.0 * i + 1), sign = -sign;
  return s * 4;
}
RUN(kernel, "%.17g")
