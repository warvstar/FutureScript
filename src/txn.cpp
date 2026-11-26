// Transactions: `atomic { }` blocks, and the run of each hot reload's new top level and reload() hook. While one
// is open, the first write to an object (an instance's fields, a list, a map, a closure's captured variables)
// copies what the object held, and the globals are copied when it opens. If an error escapes it, all of that is
// put back; otherwise the copies are dropped, or handed to the enclosing transaction, which may still roll back.
// Nothing needs declaring: any code can run inside one. Locals of the running functions are not copied (no other
// code can see them), and neither is the outside world (printed output, files).
#include "fut.h"
#include <algorithm>
#include <unordered_set>

bool txnPollLater;
namespace {
struct Saved { Obj* o; std::vector<V> v; std::vector<u32> idx; u32 used = 0; };  // a map's entries: key, value, ...
struct Level {
  Fiber* owner;                  // the fiber that opened it (null: the main stack)
  std::vector<V> globals;        // as they were when it opened
  std::vector<Saved> saved;      // each object as it was before its first write
  std::unordered_set<Obj*> seen;
};
std::vector<Level> levels;
}  // namespace

static Saved save(Obj* o) {
  Saved s{o};
  switch (o->kind) {
    case K::List: s.v = ((List*)o)->v; break;
    case K::Map: {
      Map* m = (Map*)o;
      for (auto& [k, v] : m->e) s.v.push_back(k), s.v.push_back(v);
      s.idx = m->idx, s.used = m->used;
      break;
    }
    case K::Inst: s.v.assign(((Inst*)o)->slots, ((Inst*)o)->slots + o->nslots); break;
    case K::Clo: s.v.assign(((Clo*)o)->env(), ((Clo*)o)->env() + ((Clo*)o)->n); break;
    default: break;
  }
  return s;
}
static void restore(Saved& s) {
  switch (s.o->kind) {
    case K::List: ((List*)s.o)->v = std::move(s.v); break;
    case K::Map: {
      Map* m = (Map*)s.o;
      m->e.clear();
      for (size_t i = 0; i < s.v.size(); i += 2) m->e.push_back({s.v[i], s.v[i + 1]});
      m->idx = std::move(s.idx), m->used = s.used;
      break;
    }
    case K::Inst: {  // slots only grow: fields added since read as never assigned
      Inst* in = (Inst*)s.o;
      std::copy(s.v.begin(), s.v.end(), in->slots), std::fill(in->slots + s.v.size(), in->slots + in->nslots, ABSENT);
      break;
    }
    case K::Clo: std::copy(s.v.begin(), s.v.end(), ((Clo*)s.o)->env()); break;
    default: break;
  }
}
static void depthChanged() {
  vm.txn = (u32)levels.size();
  if (!vm.txn && txnPollLater) txnPollLater = false, __atomic_fetch_or(&vm.poll, 1u, __ATOMIC_RELEASE);  // a reload waited
}

void clearSetICs();
void txnBegin() {
  if (levels.empty()) clearSetICs();  // stores now go through rt_setf, which saves the instance first
  Level& l = levels.emplace_back();
  l.owner = fiberCur, l.globals.assign(vm.globals, vm.globals + globalCount());
  depthChanged();
}
void txnCommit() {
  if (levels.empty()) return;
  Level top = std::move(levels.back());
  levels.pop_back();
  if (!levels.empty()) {  // the enclosing transaction keeps the oldest copy of each object
    Level& o = levels.back();
    for (auto& s : top.saved) if (o.seen.insert(s.o).second) o.saved.push_back(std::move(s));
  }
  depthChanged();
}
void txnAbort() {
  if (levels.empty()) return;
  Level& l = levels.back();
  for (auto& s : l.saved) restore(s);
  std::copy(l.globals.begin(), l.globals.end(), vm.globals);
  levels.pop_back(), depthChanged();
}
void txnUnwind(u32 depth) { while (levels.size() > depth) txnAbort(); }
void txnTouch(Obj* o) {
  Level& l = levels.back();
  if (l.seen.insert(o).second) l.saved.push_back(save(o));
}
bool txnOpenIn(Fiber* f) {
  for (auto& l : levels) if (l.owner == f) return true;
  return false;
}
void txnMark(void (*markV)(V)) {
  for (auto& l : levels) {
    for (V v : l.globals) markV(v);
    for (auto& s : l.saved) {
      markV(box(s.o));
      for (V v : s.v) markV(v);
    }
  }
}

// atomic { }: the compiler calls atomic@begin before the block, and atomic@end (passing x0 through) wherever
// control leaves it normally: its end, return, break, continue. An error leaving it is rolled back by whatever
// catches it (see throwValue, and the try handlers the compilers emit).
static V atomicBegin() { return txnBegin(), NIL; }
static V atomicEnd(V v) { return txnCommit(), v; }
static V rt_capset(V v, Clo* c, uint64_t i) { return txnTouch(c), c->env()[i] = v; }  // a captured variable, inside one
static V rt_txunwind(V v, uint64_t depth) { return txnUnwind((u32)depth), v; }      // a try caught an error
void txnInit() {
  native<atomicBegin>("atomic@begin"), native<atomicEnd>("atomic@end");
  vm.helpers[H_CAPSET] = (void*)rt_capset, vm.helpers[H_TXUNWIND] = (void*)rt_txunwind;
}
