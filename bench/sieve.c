#include "harness.h"
static long kernel(void) {
  long n = 10000000, count = 0;
  char* flags = malloc(n);
  memset(flags, 1, n);
  for (long i = 2; i < n; i++)
    if (flags[i]) {
      count++;
      for (long j = i * i; j < n; j += i) flags[j] = 0;
    }
  free(flags);
  return count;
}
RUN(kernel, "%ld")
