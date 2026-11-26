// Transactions: `atomic { }` blocks, and the run of each hot reload's new top level and reload() hook. While one
// is open, the first write to an object (an instance's fields, a list, a map, a closure's captured variables)
// copies what the object held, and the globals are copied when it opens. If an error escapes it, all of that is
// put back; otherwise the copies are dropped, or handed to the enclosing transaction, which may still roll back.
// Nothing needs declaring: any code can run inside one. Locals of the running functions are not copied (no other
// code can see them), and neither is the outside world (printed output, files).
#include "fut.h"
#include <algorithm>
#include <unordered_set>

namespace {
struct Saved { Obj* o; std::vector<V> v; std::vector<u32> idx; u32 used = 0; };  // a map's entries: key, value, ...
struct Level {
  Fiber* owner;                  // the fiber that opened it (null: the main stack)
  bool atomic = false;           // an atomic block (not a reload): it takes io away; allowed/who as it found them
  bool contract = false;         // a spec's checks: no effects at all, and always rolled back; allowed/who as above
  uint64_t allocs = 0;           // (contract) gcAllocations() when it opened: what the checks allocate isn't counted
  uint64_t allowed = 0;
  u32 who = 0;
  std::vector<V> globals;        // as they were when it opened
  std::vector<Saved> saved;      // each object as it was before its first write
  std::unordered_set<Obj*> seen;
  struct After { void (*run)(void*); void (*drop)(void*); void* p; };
  std::vector<After> after;      // what waits for the outermost commit: sends to channels (isolate.cpp)
};
TLS std::vector<Level> levels;  // per isolate
ISO_LOCAL(levels);
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
    case K::Buf: case K::BufI32: case K::BufU8: bufSave(o, s.v); break;
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
    case K::Buf: case K::BufI32: case K::BufU8: bufRestore(s.o, s.v); break;
    default: break;
  }
}
static void depthChanged() {
  vm->txn = (u32)levels.size();
  if (!vm->txn && txnPollLater) txnPollLater = false, __atomic_fetch_or(&vm->poll, 1u, __ATOMIC_RELEASE);  // a reload waited
}

void clearSetICs();
void txnBegin() {
  if (levels.empty()) clearSetICs();  // stores now go through rt_setf, which saves the instance first
  Level& l = levels.emplace_back();
  l.owner = fiberCur, l.globals.assign(vm->globals, vm->globals + globalCount());
  depthChanged();
}
void txnCommit() {
  if (levels.empty()) return;
  Level top = std::move(levels.back());
  levels.pop_back();
  if (!levels.empty()) {  // the enclosing transaction keeps the oldest copy of each object
    Level& o = levels.back();
    for (auto& s : top.saved) if (o.seen.insert(s.o).second) o.saved.push_back(std::move(s));
    o.after.insert(o.after.end(), top.after.begin(), top.after.end());
  }
  depthChanged();
  if (levels.empty()) for (auto& a : top.after) a.run(a.p);  // committed for good: other isolates may see it now
}
void txnAbort() {
  if (levels.empty()) return;
  Level& l = levels.back();
  for (auto& s : l.saved) restore(s);
  std::copy(l.globals.begin(), l.globals.end(), vm->globals);
  for (auto& a : l.after) a.drop(a.p);
  levels.pop_back(), depthChanged();
}
void txnAfter(void (*run)(void*), void (*drop)(void*), void* p) { levels.back().after.push_back({run, drop, p}); }
void txnUnwind(u32 depth) { while (levels.size() > depth) txnAbort(); }
void txnTouch(Obj* o) {
  Level& l = levels.back();
  if (l.seen.insert(o).second) l.saved.push_back(save(o));
}
bool txnAtomic() {
  for (auto& l : levels) if (l.atomic) return true;
  return false;
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
// An atomic block can't undo io, so it takes io away until it ends (a narrowing, like a declared function's): only a
// handler installed inside the block can answer an io operation there.
static V atomicBegin() {
  txnBegin();
  Level& l = levels.back();
  l.atomic = true, l.allowed = vm->allowed, l.who = vm->who;
  vm->allowed = ((vm->allowed >> EFF_LEVEL) + 1) << EFF_LEVEL | (vm->allowed & EFF_BITS & ~EFF_IO), vm->who = WHO_ATOMIC;
  return NIL;
}
static V atomicEnd(V v) {
  if (!levels.empty() && levels.back().atomic) vm->allowed = levels.back().allowed, vm->who = levels.back().who;
  return txnCommit(), v;
}
// contract@begin(who) / contract@end(): around a spec's checks that call code. Checking a contract must not change
// what the program does, so they run with no effects allowed (who: the contract, for messages) and whatever they
// changed is put back when they end. A failed check raises, and whatever catches the error rolls back as usual.
static V contractBegin(V who) {
  txnBegin();
  Level& l = levels.back();
  l.contract = true, l.allowed = vm->allowed, l.who = vm->who, l.allocs = gcAllocations();
  vm->allowed = ((vm->allowed >> EFF_LEVEL) + 1) << EFF_LEVEL, vm->who = (u32)num(who);
  return NIL;
}
static V contractEnd() {
  if (levels.empty() || !levels.back().contract) return NIL;
  Level& l = levels.back();
  gcUncounted += gcAllocations() - l.allocs;
  vm->allowed = l.allowed, vm->who = l.who, txnAbort();
  return NIL;
}
static V rt_capset(V v, Clo* c, uint64_t i) { return txnTouch(c), c->env()[i] = v; }  // a captured variable, inside one
static V rt_txunwind(V v, uint64_t depth) { return txnUnwind((u32)depth), v; }      // a try caught an error
void txnInit() {
  native<atomicBegin>("atomic@begin"), native<atomicEnd>("atomic@end");
  native<contractBegin>("contract@begin"), native<contractEnd>("contract@end");
  vm->helpers[H_CAPSET] = (void*)rt_capset, vm->helpers[H_TXUNWIND] = (void*)rt_txunwind;
}
