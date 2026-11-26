// Shared by the C versions: run kernel() REPS times, print its checksum, the first run's time and the best.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#define REPS 5
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
#define RUN(kernel, fmt)                                                         \
  int main(void) {                                                               \
    double best = 1e9, first = 0;                                                \
    __typeof__(&kernel) volatile kp = kernel; /* opaque: every rep runs */       \
    for (int r = 0; r < REPS; r++) {                                             \
      double t = now();                                                          \
      __typeof__(kernel()) c = kp();                                             \
      double ms = (now() - t) * 1000;                                            \
      if (r == 0) first = ms, printf("check " fmt "\n", c);                     \
      if (ms < best) best = ms;                                                  \
    }                                                                            \
    printf("first %.3f\nbest %.3f\n", first, best);                              \
  }
