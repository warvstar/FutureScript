#include "harness.h"
#include <pthread.h>
#ifndef THREADS
#define THREADS 1
#endif
static int pixel(double cx, double cy) {
  double x = 0, y = 0;
  int i = 0;
  for (; i < 200; i++) {
    double x2 = x * x, y2 = y * y;
    if (x2 + y2 > 4) return i;
    y = 2 * x * y + cy, x = x2 - y2 + cx;
  }
  return i;
}
enum { SIZE = 1000 };
static long part[THREADS];
static void* rows(void* a) {  // rows interleaved across threads, so the cheap and expensive ones spread evenly
  long t = (long)a, s = 0;
  for (int py = t; py < SIZE; py += THREADS)
    for (int px = 0; px < SIZE; px++) s += pixel(px * 3.0 / SIZE - 2, py * 3.0 / SIZE - 1.5);
  part[t] = s;
  return NULL;
}
static long kernel(void) {
  pthread_t th[THREADS];
  for (long t = 0; t < THREADS; t++) pthread_create(&th[t], NULL, rows, (void*)t);
  long total = 0;
  for (long t = 0; t < THREADS; t++) pthread_join(th[t], NULL), total += part[t];
  return total;
}
RUN(kernel, "%ld")
