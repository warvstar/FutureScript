// Buffers: `buffer(n)` (n zeros) or `buffer(list)` (its numbers) makes a fixed-length run of numbers that lives
// outside every heap, so the GC never scans or moves it and isolates can hand it to each other without copying it
// (docs/isolates.md, phase 6). `i32buffer(...)` and `u8buffer(...)` hold 32-bit integers and bytes: a store there
// takes a whole number in the type's range, and raises otherwise. An isolate holds a handle (a Buf, in its
// heap; its kind is the element type) on refcounted memory (BufData). An f64 buffer holds doubles, which are a fut
// number's bits, so JIT code reads b[i] as it reads a list's element (see index() in gen_a64.h and gen_wasm.h) and
// stores into it after checking the value is a number.
#include "fut.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

struct BufData {
  std::atomic<int> rc{1};
  std::atomic<const void*> holder;  // the isolate whose handles may use it; null while a message carries it
  std::atomic<bool> writing{false};  // a parallel for's body writes it (its isolates' handles reach only [i])
  K kind;
  size_t n;
  void* d;
};

namespace {
constexpr double MAX_LEN = 1u << 30;
size_t width(K k) { return k == K::Buf ? 8 : k == K::BufI32 ? 4 : 1; }
const char* typeOf(K k) { return k == K::Buf ? "f64" : k == K::BufI32 ? "i32" : "u8"; }

void point(Buf* b, BufData* s) {  // the handle's view of s (an f64 one laid out as a list's vector: see LIST_V)
  b->s = s, b->d = s ? s->d : nullptr, b->n = s ? s->n : 0;
  b->endOrCount = vecCount ? b->n : (uintptr_t)((char*)b->d + b->n * width(b->kind));
}
BufData* alloc(K k, size_t n) {
  void* d = calloc(n ? n : 1, width(k));
  if (!d) ERR("@memory buffer(%zu): out of memory", n);
  BufData* s = new BufData;
  s->kind = k, s->n = n, s->d = d, s->holder = isolateId();
  return s;
}
void release(BufData* s) {
  if (s && s->rc.fetch_sub(1, std::memory_order_acq_rel) == 1) free(s->d), delete s;
}
V handle(BufData* s) {
  Buf* b = make<Buf>();
  b->kind = s->kind, point(b, s);
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
// What a store writes: a number, and for i32 and u8 a whole one in range.
double element(K k, V v, const char* what) {
  if (!isNum(v)) ERR("@argument %s: a buffer holds numbers, got %s", what, typeName(v));
  double x = num(v);
  if (k == K::BufI32 && !(x == std::trunc(x) && x >= -2147483648.0 && x <= 2147483647.0))
    ERR("@argument %s: an i32 buffer holds whole numbers from -2^31 to 2^31 - 1, got %s", what, toStr(v).c_str());
  if (k == K::BufU8 && !(x == std::trunc(x) && x >= 0 && x <= 255))
    ERR("@argument %s: a u8 buffer holds whole numbers from 0 to 255, got %s", what, toStr(v).c_str());
  return x;
}
void put(Buf* b, size_t i, double x) {
  if (b->kind == K::Buf) ((double*)b->d)[i] = x;
  else if (b->kind == K::BufI32) ((int32_t*)b->d)[i] = (int32_t)x;
  else ((uint8_t*)b->d)[i] = (uint8_t)x;
}
size_t clamp(V v, size_t n, const char* fn) {  // as rt.cpp's clampIx
  if (!isNum(v)) ERR("@argument %s expects numbers, got %s", fn, typeName(v));
  double d = num(v);
  if (d != std::trunc(d)) ERR("@index %s: an index must be a whole number, got %g", fn, d);
  return d < 0 ? 0 : d > (double)n ? n : (size_t)d;
}

const char* ctor(K k) { return k == K::Buf ? "buffer" : k == K::BufI32 ? "i32buffer" : "u8buffer"; }
template <K k> V newBuf(V v) {  // buffer(n | list), i32buffer(...), u8buffer(...)
  if (isNum(v)) {
    double n = num(v);
    if (n != std::trunc(n) || n < 0 || n > MAX_LEN) ERR("@argument %s(n): n must be a whole number from 0 to 2^30, got %s", ctor(k), toStr(v).c_str());
    return handle(alloc(k, (size_t)n));
  }
  if (!is(v, K::List)) ERR("@argument %s() expects a length or a list of numbers, got %s", ctor(k), typeName(v));
  volatile V held = v;
  (void)held;
  auto& l = as<List>(v)->v;
  std::vector<double> xs(l.size());
  for (size_t i = 0; i < l.size(); i++) xs[i] = element(k, l[i], (std::string(ctor(k)) + "(list): element " + std::to_string(i)).c_str());
  V h = handle(alloc(k, xs.size()));
  for (size_t i = 0; i < xs.size(); i++) put(as<Buf>(h), i, xs[i]);
  return h;
}
V b_fill(V b, V v) {  // b.fill(x) -> b: every element x
  Buf* h = live(b);
  double x = element(h->kind, v, "fill()");
  touch(h);
  for (size_t i = 0; i < h->n; i++) put(h, i, x);
  return b;
}
V b_toList(V b) {
  volatile V held = b;
  (void)held;
  live(b);
  List* l = make<List>();
  Buf* h = as<Buf>(b);
  l->v.resize(h->n);
  for (size_t i = 0; i < h->n; i++) l->v[i] = box(bufAt(h, i));
  return box(l);
}
V b_slice(V b, V a, V e) {  // a new buffer of b's type with a copy of b[a..e], clamped
  Buf* h = live(b);
  size_t i = clamp(a, h->n, "slice()"), j = clamp(e, h->n, "slice()"), w = width(h->kind);
  BufData* s = alloc(h->kind, i < j ? j - i : 0);
  if (i < j) memcpy(s->d, (char*)h->d + i * w, (j - i) * w);
  return handle(s);
}
V b_type(V b) { return box(newStr(typeOf(as<Buf>(b)->kind))); }
}  // namespace

namespace {
double read(K k, void* d, size_t i) { return k == K::Buf ? ((double*)d)[i] : k == K::BufI32 ? ((int32_t*)d)[i] : ((uint8_t*)d)[i]; }
size_t own(Buf* h, V i) {  // a parallel for's handle on a buffer its body writes: b[i] only at the iteration's i
  size_t at = whole(i, h->s->n);
  if ((double)at != parAt || num(i) != parAt)
    ERR("@parallel a parallel for's body writes this buffer, so it may reach it only at [i] (here i is %s, and the index %s)",
        toStr(box(parAt)).c_str(), toStr(i).c_str());
  return at;
}
}  // namespace
double bufAt(Buf* b, size_t i) { return read(b->kind, b->d, i); }
V bufGet(V b, V i) {
  Buf* h = as<Buf>(b);
  if (h->gone == AT) return box(read(h->kind, h->s->d, own(h, i)));
  h = live(b);
  return box(bufAt(h, whole(i, h->n)));
}
V bufSet(V b, V i, V v) {
  Buf* h = as<Buf>(b);
  if (h->gone == AT) {
    size_t at = own(h, i);
    double x = element(h->kind, v, "b[i] = v");
    if (vm->txn) ERR("@parallel a parallel for's body can't write its buffer inside atomic { } (write it after the block)");
    Buf t = *h;
    t.d = h->s->d;
    return put(&t, at, x), v;
  }
  h = live(b);
  size_t at = whole(i, h->n);
  return put(h, at, element(h->kind, v, "b[i] = v")), v;
}
Buf* bufLive(V b) { return live(b); }
void bufFree(Obj* o) { release(((Buf*)o)->s), point((Buf*)o, nullptr), ((Buf*)o)->s = nullptr; }
size_t bufLength(V b) { return as<Buf>(b)->gone == AT ? as<Buf>(b)->s->n : live(b)->n; }
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
  Buf* b = make<Buf>();  // length 0 here, so compiled code's b[i] always calls bufGet / bufSet
  b->kind = s->kind, point(b, nullptr), b->s = s, b->gone = AT;
  return box(b);
}
void bufSave(Obj* o, std::vector<V>& v) {  // its bytes, in as many Vs as they need
  Buf* b = (Buf*)o;
  size_t bytes = b->n * width(b->kind);
  v.assign((bytes + 7) / 8, 0);
  memcpy(v.data(), b->d, bytes);
}
void bufRestore(Obj* o, const std::vector<V>& v) {
  Buf* b = (Buf*)o;
  memcpy(b->d, v.data(), std::min(v.size() * 8, b->n * width(b->kind)));
}
void bufInit() {
  native<newBuf<K::Buf>>("buffer"), native<newBuf<K::BufI32>>("i32buffer"), native<newBuf<K::BufU8>>("u8buffer");
  for (K k : {K::Buf, K::BufI32, K::BufU8})
    method<b_fill>(k, intern("fill")), method<b_toList>(k, intern("toList")), method<b_slice>(k, intern("slice")),
    method<b_type>(k, intern("type"));
}
