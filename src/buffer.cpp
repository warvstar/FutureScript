// Buffers: f64s, f32s, u32s, i32s and u8s hold numbers packed as C and the GPU want them: 64- and 32-bit floats,
// 32-bit whole numbers (unsigned or signed), bytes. `f32s(n)` makes n zeros, `f32s(list)` or `f32s(buffer)` its
// numbers. The length is fixed, and a store is checked: a non-number, or a number a whole-number buffer can't hold,
// raises (an f32 store rounds). The numbers live outside every heap, so the GC never scans or moves them, and
// isolates hand them to each other without copying (docs/isolates.md, phase 6). An isolate holds a handle (a Buf, in
// its heap) on refcounted memory (BufData). Compiled code indexes each type inline (index() in gen_a64.h and
// gen_wasm.h) through the handle's p and n.
#include "fut.h"
#include <atomic>
#include <cmath>
#include <cstring>

struct BufData {
  std::atomic<int> rc{1};
  std::atomic<const void*> holder;  // the isolate whose handles may use it; null while a message carries it
  std::atomic<bool> writing{false};  // a parallel for's body writes it (its isolates' handles reach only [i])
  char et;
  size_t n;
  uint8_t* d;
};

namespace {
void point(Buf* b, BufData* s) { b->s = s, b->p = s ? s->d : nullptr, b->n = s ? s->n : 0; }
BufData* alloc(char et, size_t n) {
  if (n > (1ull << 31)) ERR("@argument %s(): %zu elements is more than a buffer holds (2^31)", bufName(et), n);
  uint8_t* d = (uint8_t*)calloc(n ? n : 1, bufElem(et));
  if (!d) ERR("@host %s(): out of memory for %zu elements", bufName(et), n);
  BufData* s = new BufData;
  s->et = et, s->n = n, s->d = d, s->holder = isolateId();
  return s;
}
void release(BufData* s) {
  if (s && s->rc.fetch_sub(1, std::memory_order_acq_rel) == 1) free(s->d), delete s;
}
V handle(BufData* s) {
  Buf* b = make<Buf>();
  b->et = s->et, point(b, s);
  return box(b);
}
enum { LIVE, SENT, ELSEWHERE, AT };
Buf* live(V v) {  // a handle that still reaches its buffer
  Buf* b = as<Buf>(v);
  if (b->gone == AT) ERR("@parallel a parallel for's body writes this buffer, so it may reach it only at [i] (its own iteration's element)");
  if (b->gone == SENT) ERR("@send this buffer was sent to another isolate: sending a buffer moves it (send back what you still need)");
  if (b->gone == ELSEWHERE) ERR("@send this buffer belongs to another isolate: a spawned isolate's globals don't take their buffers (pass one as an argument to move it)");
  return b;
}
size_t whole(V i, size_t n) {  // b[i]: as a list's index (rt.cpp's INDEX)
  if (isNum(i) && num(i) >= 0 && num(i) < (double)n) return (size_t)num(i);
  ERR("@index index %s out of range (len %zu)%s", toStr(i).c_str(), n,
      isNum(i) && num(i) < 0 ? "; use b[b.len() - 1] for the last element" : "");
}
V read(char et, const uint8_t* p, size_t i) {
  switch (et) {
    case 'd': return box(((const double*)p)[i]);
    case 'f': return box((double)((const float*)p)[i]);
    case 'u': return box((double)((const uint32_t*)p)[i]);
    case 'i': return box((double)((const int32_t*)p)[i]);
    default: return box((double)p[i]);
  }
}
void put(char et, uint8_t* p, size_t i, V v) {  // checked
  if (!isNum(v)) ERR("@type cannot store %s in %s: it holds numbers", typeName(v), bufName(et));
  double d = num(v);
  if (et == 'd') return void(((double*)p)[i] = d);
  if (et == 'f') return void(((float*)p)[i] = (float)d);
  double lo = et == 'i' ? -2147483648.0 : 0, hi = et == 'u' ? 4294967295.0 : et == 'i' ? 2147483647.0 : 255;
  if (!(d >= lo && d <= hi && d == std::trunc(d)))
    ERR("@type cannot store %s in %s: it holds whole numbers from %.0f to %.0f", toStr(v).c_str(), bufName(et), lo, hi);
  if (et == 'u') ((uint32_t*)p)[i] = (uint32_t)d;
  else if (et == 'i') ((int32_t*)p)[i] = (int32_t)d;
  else p[i] = (uint8_t)d;
}
size_t own(Buf* h, V i) {  // a parallel for's handle on a buffer its body writes: b[i] only at the iteration's i
  size_t at = whole(i, h->s->n);
  if (num(i) != parAt)
    ERR("@parallel a parallel for's body writes this buffer, so it may reach it only at [i] (here i is %s, and the index %s)",
        toStr(box(parAt)).c_str(), toStr(i).c_str());
  return at;
}
size_t clamp(V v, size_t n, const char* fn) {  // as rt.cpp's clampIx
  if (!isNum(v)) ERR("@argument %s expects numbers, got %s", fn, typeName(v));
  double d = num(v);
  if (d != std::trunc(d)) ERR("@index %s: an index must be a whole number, got %g", fn, d);
  return d < 0 ? 0 : d > (double)n ? n : (size_t)d;
}

// f32s(n): n zeros; f32s(list) or f32s(buffer): its numbers, each checked as it is stored
template <char ET> V n_buf(V a) {
  if (isNum(a)) {
    double d = num(a);
    if (!(d >= 0 && d == std::trunc(d))) ERR("@argument %s() expects a length (a whole number, 0 or more), a list or a buffer, got %s", bufName(ET), toStr(a).c_str());
    return handle(alloc(ET, (size_t)d));
  }
  volatile V held = a;
  (void)held;
  if (is(a, K::List)) {
    auto& l = as<List>(a)->v;
    V h = handle(alloc(ET, l.size()));
    for (size_t i = 0; i < l.size(); i++) put(ET, as<Buf>(h)->p, i, l[i]);
    return h;
  }
  if (is(a, K::Buf)) {
    Buf* s = live(a);
    V h = handle(alloc(ET, s->n));
    for (size_t i = 0; i < s->n; i++) put(ET, as<Buf>(h)->p, i, bufGet(s, i));
    return h;
  }
  ERR("@argument %s() expects a length, a list or a buffer, got %s", bufName(ET), typeName(a));
}
V b_fill(V b, V v) {  // b.fill(x) -> b: every element x
  Buf* h = live(b);
  touch(h);
  for (size_t i = 0; i < h->n; i++) put(h->et, h->p, i, v);
  return b;
}
V b_toList(V b) {
  volatile V held = b;
  (void)held;
  live(b);
  List* l = make<List>();
  Buf* h = as<Buf>(b);
  l->v.resize(h->n);
  for (size_t i = 0; i < h->n; i++) l->v[i] = bufGet(h, i);
  return box(l);
}
V b_slice(V b, V a, V z) {  // a copy of b[a..z] (clamped), of b's type
  Buf* h = live(b);
  size_t i = clamp(a, h->n, "slice()"), j = clamp(z, h->n, "slice()"), e = bufElem(h->et);
  BufData* s = alloc(h->et, i < j ? j - i : 0);
  if (i < j) memcpy(s->d, h->p + i * e, (j - i) * e);
  return handle(s);
}
V b_bytes(V b) { Buf* h = live(b); return box((double)(h->n * bufElem(h->et))); }
}  // namespace

Buf* newBuf(char et, size_t n) { return as<Buf>(handle(alloc(et, n))); }
const char* bufName(char et) { return et == 'd' ? "f64s" : et == 'f' ? "f32s" : et == 'u' ? "u32s" : et == 'i' ? "i32s" : "u8s"; }
V bufGet(const Buf* b, size_t i) { return read(b->et, b->p, i); }
V bufIndex(V b, V i) {
  Buf* h = as<Buf>(b);
  if (h->gone == AT) return read(h->et, h->s->d, own(h, i));
  h = live(b);
  return bufGet(h, whole(i, h->n));
}
V bufStore(V b, V i, V v) {
  Buf* h = as<Buf>(b);
  if (h->gone == AT) {
    size_t at = own(h, i);
    if (vm->txn) ERR("@parallel a parallel for's body can't write its buffer inside atomic { } (write it after the block)");
    return put(h->et, h->s->d, at, v), v;
  }
  h = live(b);
  return put(h->et, h->p, whole(i, h->n), v), v;
}
Buf* bufLive(V b) { return live(b); }
size_t bufLength(V b) { return as<Buf>(b)->gone == AT ? as<Buf>(b)->s->n : live(b)->n; }
uint8_t* bufBytes(V b) { return live(b)->p; }
void bufFree(Obj* o) { release(((Buf*)o)->s), point((Buf*)o, nullptr); }
BufData* bufTake(Obj* o) {
  Buf* b = live(box(o));
  if (o->typed & FROZEN) ERR("@parallel a parallel for's body can't send a buffer from outside it");
  if (vm->txn) ERR("@atomic a buffer can't be sent inside atomic { }: sending moves it, which can't be undone (send it after the block)");
  return b->s->rc.fetch_add(1, std::memory_order_relaxed), b->s;
}
void bufSent(Obj* o) {
  Buf* b = (Buf*)o;
  if (b->gone) return;  // (the same handle twice in one message)
  b->s->holder.store(nullptr, std::memory_order_release);
  release(b->s), point(b, nullptr), b->gone = SENT;
}
V bufGive(BufData* s) {
  const void *me = isolateId(), *none = nullptr;
  if (!s->holder.compare_exchange_strong(none, me) && none != me)
    ERR("@send this buffer was already received by another isolate (a task's result holding one can be joined by one isolate)");
  s->rc.fetch_add(1, std::memory_order_relaxed);
  return handle(s);
}
V bufElsewhere() {
  Buf* b = make<Buf>();
  point(b, nullptr), b->gone = ELSEWHERE;
  return box(b);
}
void bufRelease(BufData* s) { release(s); }
BufData* bufShare(Obj* o) {
  Buf* b = live(box(o));
  return b->s->rc.fetch_add(1, std::memory_order_relaxed), b->s;
}
BufData* bufWriting(Obj* o, bool on) {
  BufData* s = on ? live(box(o))->s : ((Buf*)o)->s;
  if (s) s->writing.store(on, std::memory_order_relaxed);  // (the isolates it is lent to start after: their start orders it)
  return s;
}
V bufLend(BufData* s) {
  s->rc.fetch_add(1, std::memory_order_relaxed);
  if (!s->writing.load(std::memory_order_relaxed)) {
    V h = handle(s);
    return obj(h)->typed |= FROZEN, h;
  }
  Buf* b = make<Buf>();  // n 0 here, so compiled code's b[i] always calls bufIndex / bufStore
  b->et = s->et, point(b, nullptr), b->s = s, b->gone = AT;
  return box(b);
}
void bufInit() {
  native<n_buf<'d'>>("f64s"), native<n_buf<'f'>>("f32s"), native<n_buf<'u'>>("u32s"), native<n_buf<'i'>>("i32s"), native<n_buf<'b'>>("u8s");
  method<b_fill>(K::Buf, intern("fill")), method<b_toList>(K::Buf, intern("toList")), method<b_slice>(K::Buf, intern("slice")),
      method<b_bytes>(K::Buf, intern("bytes"));
}
