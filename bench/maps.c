#include "harness.h"
// What a C programmer would write: open addressing, FNV-1a, keys copied into the table, growth by doubling.
typedef struct { char* k; long v; } Slot;
static Slot* tab; static size_t cap, used;
static unsigned long hash(const char* s) { unsigned long h = 1469598103934665603ul; for (; *s; s++) h = (h ^ (unsigned char)*s) * 1099511628211ul; return h; }
static Slot* find(const char* k) {
  for (size_t i = hash(k) & (cap - 1);; i = (i + 1) & (cap - 1))
    if (!tab[i].k || !strcmp(tab[i].k, k)) return &tab[i];
}
static void grow(void) {
  Slot* old = tab; size_t n = cap;
  cap = cap ? cap * 2 : 16, tab = calloc(cap, sizeof *tab);
  for (size_t i = 0; i < n; i++) if (old[i].k) *find(old[i].k) = old[i];
  free(old);
}
static long kernel(void) {
  tab = NULL, cap = used = 0, grow();
  char k[32];
  for (long i = 0; i < 2000000; i++) {
    snprintf(k, sizeof k, "key%ld", (i * 7) % 100000);
    Slot* s = find(k);
    if (!s->k) {
      s->k = strdup(k), s->v = 1;
      if (++used * 4 > cap * 3) grow();
    } else s->v++;
  }
  long r = (long)used * 1000 + find("key14")->v;
  for (size_t i = 0; i < cap; i++) free(tab[i].k);
  free(tab);
  return r;
}
RUN(kernel, "%ld")
