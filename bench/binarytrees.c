#include "harness.h"
typedef struct Node { struct Node *l, *r; } Node;
static Node* make(int d) {
  Node* n = malloc(sizeof *n);
  if (d == 0) n->l = n->r = NULL;
  else n->l = make(d - 1), n->r = make(d - 1);
  return n;
}
static long check(Node* n) { return n->l ? 1 + check(n->l) + check(n->r) : 1; }
static void drop(Node* n) { if (n->l) drop(n->l), drop(n->r); free(n); }
static long kernel(void) {
  int max = 16;
  Node* s = make(max + 1);
  long total = check(s);
  drop(s);
  Node* lived = make(max);
  for (int d = 4; d <= max; d += 2)
    for (long i = 0, iters = 1L << (max - d + 4); i < iters; i++) {
      Node* t = make(d);
      total += check(t), drop(t);
    }
  total += check(lived), drop(lived);
  return total;
}
RUN(kernel, "%ld")
