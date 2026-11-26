#include "harness.h"
static long fib(long n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
static long kernel(void) { return fib(32); }
RUN(kernel, "%ld")
