// freeze(v): a deep copy of v that never changes, in the frozen heap (gc.cpp), which every isolate reads in place:
// sending a frozen value sends a pointer, not a copy (isolate.cpp's Writer, 'F'). Its lists, maps, instances and
// buffers are FROZEN, so a store to one goes to its helper, where touch() raises; strings and closures can't change
// anyway. A frozen value already frozen is shared, not copied again; functions, classes and constant strings are
// compiled code's (the image), and stay as they are. thaw(v) is the way back: a copy in the running isolate's heap
// that it may change. docs/isolates.md, phase 7.
#include "fut.h"
#include <cstdarg>
#include <unordered_map>

namespace {
constexpr int MAX_DEPTH = 10000;
bool image(Obj* o) { return gcInImage(o) && (o->kind == K::Fn || o->kind == K::Class || o->kind == K::Str); }

// While freeze runs (no fut code does), bits of the header mark what it has visited: on the path from the root (a
// cycle if reached again), done, and reached more than once (whose copy is shared, as the original was).
constexpr uint8_t PATH = 0x40, DONE = 0x20, TWICE = 0x10;

// What freeze can't copy, and cycles, found before anything is made: an error leaves nothing half made. (An error
// is raised after this is gone: raising unwinds past destructors.)
struct Check {
  std::vector<Obj*> seen;  // (their bits are cleared after)
  std::string err;
  __attribute__((format(printf, 2, 3))) void fail(const char* f, ...) {
    if (!err.empty()) return;
    char b[300];
    va_list a;
    va_start(a, f), vsnprintf(b, sizeof b, f, a), va_end(a), err = b;
  }
  void operator()(V v, int depth) {
    if (!isObj(v) || !err.empty()) return;
    Obj* o = obj(v);
    if (o->kind == K::Str || gcFrozen(o) || image(o)) return;  // (a string is copied where it is found)
    if (o->typed & DONE) return void(o->typed |= TWICE);
    if (o->typed & PATH) return fail("@frozen freeze() can't copy a value that contains itself (a frozen value has no cycles)");
    if (depth > MAX_DEPTH) return fail("@frozen freeze() can't copy a value nested more than %d deep", MAX_DEPTH);
    o->typed |= PATH, seen.push_back(o);
    switch (o->kind) {
      case K::List: for (V x : ((List*)o)->v) (*this)(x, depth + 1); break;
      case K::Map: for (auto& [k, x] : ((Map*)o)->e) (*this)(k, depth + 1), (*this)(x, depth + 1); break;
      case K::Inst: for (u32 k = 0; k < o->nslots; k++) (*this)(((Inst*)o)->slots[k], depth + 1); break;
      case K::Clo:
        if (!gcInImage(((Clo*)o)->fn)) return fail("@frozen freeze() can't copy this function: it isn't compiled code's");
        for (u32 k = 0; k < ((Clo*)o)->n; k++) (*this)(((Clo*)o)->env()[k], depth + 1);
        break;
      case K::Buf:
        if (((Buf*)o)->gone) return fail("@frozen freeze() can't copy this buffer: this isolate no longer has it (it was sent, or a parallel for lent it)");
        break;
      default: return fail("@frozen freeze() can't copy %s: only plain data (numbers, strings, lists, maps, instances, functions, buffers)", typeName(v));
    }
    o->typed = (o->typed & ~PATH) | DONE;
  }
  ~Check() { for (Obj* o : seen) o->typed &= ~(PATH | DONE | TWICE); }
};

struct Copy {  // (inside a FreezeScope: what it makes is frozen)
  FreezeScope& fs;
  std::unordered_map<Obj*, V> made;  // the copies of what is reached TWICE
  V child(V v) {  // v copied, as a frozen object's part: a reference on it
    V c = (*this)(v);
    if (isObj(c) && gcFrozen(obj(c))) fs.ref(obj(c));
    return c;
  }
  V operator()(V v) {
    if (!isObj(v)) return v;
    Obj* o = obj(v);
    if (gcFrozen(o) || image(o)) return v;
    bool twice = o->typed & TWICE;
    if (twice)
      if (auto it = made.find(o); it != made.end()) return it->second;
    V r;
    switch (o->kind) {
      case K::Str: r = box(newStr(((Str*)o)->sv())); break;
      case K::List: {
        auto& src = ((List*)o)->v;
        List* l = make<List>();
        l->typed |= FROZEN, l->v.reserve(src.size()), r = box(l);
        for (V x : src) l->v.push_back(child(x));
        break;
      }
      case K::Map: {
        Map* m = make<Map>();
        m->typed |= FROZEN, r = box(m);
        for (auto& [k, x] : ((Map*)o)->e) { V ck = child(k), cx = child(x); mapSet(m, ck, cx); }
        break;
      }
      case K::Inst: {
        Inst* s = (Inst*)o;
        Inst* in = make<Inst>(o->nslots * 8);
        in->cls = s->cls, in->cid = s->cid, in->nslots = o->nslots, in->slots = in->inl(), in->typed |= FROZEN;
        for (u32 k = 0; k < o->nslots; k++) in->slots[k] = NIL;
        r = box(in);
        for (u32 k = 0; k < o->nslots; k++) in->slots[k] = child(s->slots[k]);
        break;
      }
      case K::Clo: {
        Clo* s = (Clo*)o;
        Clo* c = make<Clo>(s->n * 8);
        c->fn = s->fn, c->n = s->n, r = box(c);
        for (u32 k = 0; k < s->n; k++) c->env()[k] = NIL;
        for (u32 k = 0; k < s->n; k++) c->env()[k] = child(s->env()[k]);
        break;
      }
      case K::Buf: {
        Buf* s = (Buf*)o;
        Buf* b = newBuf(s->et, s->n);
        memcpy(b->p, s->p, s->n * bufElem(s->et)), b->typed |= FROZEN, r = box(b);
        break;
      }
      default: abort();  // (Check refused it)
    }
    if (twice) made[o] = r;
    return r;
  }
};

V n_freeze(V v) {
  if (!isObj(v) || image(obj(v)) || gcFrozen(obj(v))) return v;
  V r;
  std::string e;
  {
    Check c;
    c(v, 0);
    if ((e = c.err).empty()) {
      FreezeScope fs;
      r = Copy{fs}(v);
      fs.hold(obj(r));
    }
  }
  if (!e.empty()) ERR("@frozen %s", e.c_str() + 8);  // (each message has the code, then the text)
  return r;
}

// thaw(v): a copy the running isolate may change (frozen or not: a deep copy either way, as a message would make).
struct Thaw {
  std::unordered_map<Obj*, V> made;
  std::vector<V> keep;  // (GC roots while it allocates: gcBuilding)
  bool deep = false;    // (raised after this is gone, as freeze's)
  V operator()(V v, int depth) {
    if (!isObj(v) || deep) return v;
    Obj* o = obj(v);
    if (image(o)) return v;
    if (auto it = made.find(o); it != made.end()) return it->second;
    if (depth > MAX_DEPTH) return deep = true, v;
    V r;
    switch (o->kind) {
      case K::Str: return v;  // (a string never changes: a frozen one is shared as it is)
      case K::List: {
        List* l = make<List>();
        keep.push_back(r = made[o] = box(l));
        for (V x : ((List*)o)->v) { V c = (*this)(x, depth + 1); l->v.push_back(c); }
        return r;
      }
      case K::Map: {
        Map* m = make<Map>();
        keep.push_back(r = made[o] = box(m));
        for (auto& [k, x] : ((Map*)o)->e) { V ck = (*this)(k, depth + 1), cx = (*this)(x, depth + 1); mapSet(m, ck, cx); }
        return r;
      }
      case K::Inst: {
        Inst* s = (Inst*)o;
        Inst* in = make<Inst>(o->nslots * 8);
        in->cls = s->cls, in->cid = s->cid, in->nslots = o->nslots, in->slots = in->inl();
        for (u32 k = 0; k < o->nslots; k++) in->slots[k] = NIL;
        keep.push_back(r = made[o] = box(in));
        for (u32 k = 0; k < o->nslots; k++) in->slots[k] = (*this)(s->slots[k], depth + 1);
        return r;
      }
      case K::Clo: {
        Clo* s = (Clo*)o;
        Clo* c = make<Clo>(s->n * 8);
        c->fn = s->fn, c->n = s->n;
        for (u32 k = 0; k < s->n; k++) c->env()[k] = NIL;
        keep.push_back(r = made[o] = box(c));
        for (u32 k = 0; k < s->n; k++) c->env()[k] = (*this)(s->env()[k], depth + 1);
        return r;
      }
      case K::Buf: {
        Buf* s = (Buf*)o;
        if (s->gone) return v;  // (as it is: reaching it raises as it would have)
        Buf* b = newBuf(s->et, s->n);
        memcpy(b->p, s->p, s->n * bufElem(s->et));
        keep.push_back(r = made[o] = box(b));
        return r;
      }
      default: return v;  // (channels, tasks, ...: as they are)
    }
  }
};
V n_thaw(V v) {
  volatile V held = v;
  (void)held;
  V r;
  bool deep;
  {
    Thaw t;
    std::vector<V>* outer = gcBuilding(&t.keep);
    r = t(v, 0), deep = t.deep;
    gcBuilding(outer);
  }
  if (deep) ERR("@frozen thaw() can't copy a value nested more than %d deep", MAX_DEPTH);
  return r;
}
V n_isFrozen(V v) { return boxb(isObj(v) && (gcFrozen(obj(v)) || image(obj(v)))); }
V n_frozenLive() { return box((double)frozenLive()); }
}  // namespace

void freezeInit() {
  native<n_freeze>("freeze"), native<n_thaw>("thaw"), native<n_isFrozen>("isFrozen"), native<n_frozenLive>("frozenCount");
}
