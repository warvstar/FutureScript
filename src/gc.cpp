// Allocator + GC. Objects live in one reserved region of 64KB pages; each page holds one size class
// (or is part of a large object), so "is this word a heap pointer?" is O(1) arithmetic.
// Marking is conservative over the machine stack and registers, precise over the heap.
//
// Mark bits live in a side bitmap, one bit per 16-byte granule, set at the start of each marked object.
// When a collection ends that bitmap becomes the map of live objects, and there is no sweep: allocation
// bumps through the runs of free cells between live ones (vm.runs, which JIT code bumps directly), so a
// collection never touches a dead object. Objects that own malloc'd memory are registered (gcOwn) and
// finalized when found dead; a page with no live object goes back to the page pool.
#include "fut.h"
#include <algorithm>
#include <array>
#include <cstdlib>
#if !__wasm__
#include <sys/mman.h>
#endif

uintptr_t* stackTop;
// In wasm, values in wasm locals can't be scanned, so collections happen only at safepoints, where every value
// that matters is in memory: an allocation that would collect asks for one (gcWanted, vm.poll) and goes on.
bool gcWanted;

namespace {
constexpr size_t PAGE = 1 << 16, REGION = (WASM ? 256ull << 20 : 8ull << 30), NPAGES = REGION / PAGE, GRAN = 16, PBITS = PAGE / GRAN;
constexpr size_t MIN_HEAP = (8 << 20) / PAGE;
constexpr u32 NONE = ~0u;
struct Page { uint8_t type, cls; bool cold; u32 n; };  // type: 0 free, 1 small, 2 large head, 3 large tail; cold: madvised

uint8_t* base;
uint64_t *live, *marks;  // bit per granule: an object starts here and is allocated / was marked this collection
std::vector<Page> pages;
std::vector<u32> pool;   // free pages, most recently freed last (still warm)
size_t bump, inUse, limit = MIN_HEAP;
std::vector<u32> avail[NCLS];  // pages with live objects (so maybe free cells too), per class
struct Scan { u32 page = NONE, at = 0; } scans[NCLS];  // the page being searched for free runs, and where
uintptr_t runStart[NCLS];      // where vm.runs[c] began: [runStart, cur) has been allocated
std::vector<Obj*> work, owners;

constexpr auto classOf = [] {  // size class by 16-byte granule
  std::array<uint8_t, 16384 / 16 + 1> t{};
  for (size_t g = 0; g < t.size(); g++) t[g] = (uint8_t)sizeClass(g * 16);
  return t;
}();
uint8_t* addr(size_t i) { return base + i * PAGE; }
size_t bitOf(uintptr_t p) { return (p - (uintptr_t)base) / GRAN; }
bool test(const uint64_t* b, size_t i) { return b[i >> 6] >> (i & 63) & 1; }
void set(uint64_t* b, size_t i) { b[i >> 6] |= 1ull << (i & 63); }
size_t nextSet(const uint64_t* b, size_t from, size_t lim) {  // first set bit in [from, lim), else lim
  size_t w = from >> 6;
  uint64_t m = b[w] & (~0ull << (from & 63));
  while (!m) {
    if (++w << 6 >= lim) return lim;
    m = b[w];
  }
  return std::min(lim, (w << 6) + std::countr_zero(m));
}

size_t takePages(size_t n) {
  if (n == 1 && !pool.empty()) {
    size_t i = pool.back();
    pool.pop_back();
    return i;
  }
  if (bump + n > NPAGES) fputs("fut: out of memory\n", stderr), exit(1);
  return (bump += n) - n;
}
// Cell starts of class c repeat every k granules from the page start, so in bitmap word w (of a page
// starting at granule g0) they are the bits 0, k, 2k.. shifted to the first one in that word.
constexpr auto pats = [] {
  std::array<uint64_t, NCLS> p{};
  for (int c = 0; c < NCLS; c++)
    for (size_t j = 0; j < 64; j += sizeClasses[c] / GRAN) p[c] |= 1ull << j;
  return p;
}();
uint64_t cellsIn(int c, size_t w, size_t g0) {
  size_t k = sizeClasses[c] / GRAN, sh = (k - ((w << 6) - g0) % k) % k;
  return sh < 64 ? pats[c] << sh : 0;
}
void setCells(int c, uintptr_t s, uintptr_t e) {  // cells [s, e) of class c, in one page, were allocated
  if (s >= e) return;
  size_t a = bitOf(s), b = bitOf(e), g0 = a / PBITS * PBITS;
  for (size_t w = a >> 6, last = (b - 1) >> 6; w <= last; w++) {
    uint64_t m = cellsIn(c, w, g0);
    if (w == a >> 6) m &= ~0ull << (a & 63);
    if (w == last) m &= ~0ull >> (63 - ((b - 1) & 63));
    live[w] |= m;
  }
}
size_t nextFree(int c, size_t from, size_t lim, size_t g0) {  // first free cell of class c in [from, lim), else lim
  for (size_t w = from >> 6; w << 6 < lim; w++)
    if (uint64_t m = cellsIn(c, w, g0) & ~live[w] & (w == from >> 6 ? ~0ull << (from & 63) : ~0ull))
      return std::min(lim, (w << 6) + std::countr_zero(m));
  return lim;
}
void retire(int c) {  // the current run is done with: what was allocated from it is now in `live`
  setCells(c, runStart[c], vm.runs[c].cur);
  runStart[c] = vm.runs[c].cur = vm.runs[c].end = 0;
}
void startRun(int c, size_t page, size_t from, size_t to) {  // granules within the page
  runStart[c] = vm.runs[c].cur = (uintptr_t)addr(page) + from * GRAN, vm.runs[c].end = (uintptr_t)addr(page) + to * GRAN;
}
bool nextRun(int c) {  // point vm.runs[c] at the next free run in a page of class c that has live objects
  size_t k = sizeClasses[c] / GRAN, end = PAGE / sizeClasses[c] * k;  // granules: one cell, the whole cells
  for (Scan& s = scans[c];;) {
    if (s.page == NONE) {
      if (avail[c].empty()) return false;
      s = {avail[c].back(), 0}, avail[c].pop_back();
    }
    size_t g0 = (size_t)s.page * PBITS, from = nextFree(c, g0 + s.at, g0 + end, g0) - g0;
    if (from < end) {
      size_t to = nextSet(live, g0 + from, g0 + end) - g0;  // the next live object ends the run
      s.at = u32(to);
      return startRun(c, s.page, from, to), true;
    }
    s.page = NONE;
  }
}

Obj* find(uintptr_t p) {  // the allocated object starting exactly at p, if any
  uintptr_t b = (uintptr_t)base;
  if (p < b || p >= b + bump * PAGE || (p & (GRAN - 1))) return nullptr;
  size_t i = (p - b) / PAGE, off = (p - b) % PAGE;
  if (pages[i].type == 1) {
    if (off % sizeClasses[pages[i].cls] || off + sizeClasses[pages[i].cls] > PAGE) return nullptr;
  } else if (pages[i].type != 2 || off) return nullptr;
  return test(live, bitOf(p)) ? (Obj*)p : nullptr;
}
bool marked(Obj* o) { return test(marks, bitOf((uintptr_t)o)); }
void mark(Obj* o) {
  if (!o) return;
  size_t i = bitOf((uintptr_t)o);
  uint64_t& w = marks[i >> 6];
  if (uint64_t m = 1ull << (i & 63); !(w & m)) w |= m, work.push_back(o);
}
void markV(V v) { if (isObj(v)) mark(obj(v)); }
__attribute__((no_sanitize("address"))) void scan(uintptr_t* a, uintptr_t* b) {
  for (; a < b; a++) if (Obj* o = find((*a & PTR) == PTR ? *a & ADDR : *a)) mark(o);
}
void trace(Obj* o) {
  switch (o->kind) {
    case K::List: for (V v : ((List*)o)->v) markV(v); break;
    case K::Map: for (auto& [k, v] : ((Map*)o)->e) markV(k), markV(v); break;
    case K::Class:
      mark(((Class*)o)->super);
      for (auto& [s, f] : ((Class*)o)->methods) mark(f);
      for (auto& [s, d] : ((Class*)o)->fields) markV(d);
      break;
    case K::Inst: mark(((Inst*)o)->cls); for (u32 k = 0; k < o->nslots; k++) markV(((Inst*)o)->slots[k]); break;
    case K::Clo: mark(((Clo*)o)->fn); for (u32 k = 0; k < ((Clo*)o)->n; k++) markV(((Clo*)o)->env()[k]); break;
    case K::Fiber: {  // a switched-out fiber's stack (its saved registers included) is scanned like the main stack
      Fiber* f = (Fiber*)o;
      markV(f->fn), markV(f->val), markV(f->hstack), mark(f->resumer);
      if (f != fiberCur && f->stack) scan((uintptr_t*)f->sp, f->top);
      break;
    }
    default: break;
  }
}
void finalize(Obj* o) {
  switch (o->kind) {
    case K::List: ((List*)o)->~List(); break;
    case K::Map: ((Map*)o)->~Map(); break;
    case K::Class: ((Class*)o)->~Class(); break;
    case K::Inst: if (((Inst*)o)->slots != ((Inst*)o)->inl()) free(((Inst*)o)->slots); break;
    case K::Fiber: fiberFree((Fiber*)o); break;
    case K::Ref: refFree((Ref*)o); break;
    default: break;
  }
  o->kind = K::Free;
}

void collect() {
  for (int c = 0; c < NCLS; c++) retire(c), scans[c] = {}, avail[c].clear();
  // JIT locals live in callee-saved registers (x19..x25, d8..d15). Each is either still in its register
  // (setjmp saves it) or was spilled by some C++ frame on the way here, this one included: its prologue
  // may save them below its frame pointer and reuse the registers. So scan from sp, not the frame pointer.
#if __wasm__
  uintptr_t* sp;  // the shadow stack: fut frames and C++ locals whose address is taken
  asm volatile("global.get __stack_pointer\n\tlocal.set %0" : "=r"(sp));
#else
  jmp_buf regs;
  setjmp(regs);
  uintptr_t* sp;
  asm volatile("mov %0, sp" : "=r"(sp));
  scan((uintptr_t*)&regs, (uintptr_t*)((char*)&regs + sizeof regs));  // (ASan may put it off the stack)
#endif
  scan(sp, stackTop);  // the current stack (a fiber's, if one is running)
  if (fiberCur) mark(fiberCur), scan((uintptr_t*)fiberMainSp, fiberMainTop);
  for (V v : vm.globals) markV(v);
  for (V v : consts) markV(v);
  txnMark(markV);
  jsMark(markV);
  effMark(markV);
  for (auto& g : globals) mark(g.o);
  while (!work.empty()) {
    Obj* o = work.back();
    work.pop_back();
    trace(o);
  }
  std::erase_if(owners, [](Obj* o) { return !marked(o) && (finalize(o), true); });
  inUse = 0;
  for (size_t i = 0; i < bump; i++) {
    Page& p = pages[i];
    if (p.type == 1) {
      size_t n = 0;
      for (size_t w = i * PBITS / 64; w < (i + 1) * PBITS / 64; w++) n += std::popcount(marks[w]);
      if (!n) p = {}, pool.push_back((u32)i);
      else if (inUse++, n < PAGE / sizeClasses[p.cls]) avail[p.cls].push_back((u32)i);
    } else if (p.type == 2) {
      if (marked((Obj*)addr(i))) inUse += p.n;
      else for (size_t j = i, n = p.n; j < i + n; j++) pages[j] = {}, pool.push_back((u32)j);
    }
  }
  for (auto& a : avail) std::reverse(a.begin(), a.end());  // allocate from low addresses up
  std::swap(live, marks);
  memset(marks, 0, bump * PBITS / 8);
  limit = std::max(MIN_HEAP, 2 * inUse);
#if !__wasm__
  // Keep as many free pages warm as the heap may grow by before the next collection; give the rest back.
  for (size_t j = 0, keep = limit - inUse; j + keep < pool.size(); j++)
    if (Page& p = pages[pool[j]]; !p.cold) madvise(addr(pool[j]), PAGE, MADV_FREE), p.cold = true;
#endif
}
bool due() {  // over the limit: collect now (natively), or at the next safepoint (wasm)
  if (inUse + 1 <= limit) return false;
  if (!WASM) return collect(), true;
  return gcWanted = true, vm.poll |= 2, false;
}
}

void gcInit() {
#if __wasm__
  base = (uint8_t*)aligned_alloc(PAGE, REGION), live = (uint64_t*)calloc(2 * REGION / GRAN / 8, 1);
  if (!base || !live) fputs("fut: out of memory\n", stderr), exit(1);
#else
  base = (uint8_t*)mmap(nullptr, REGION, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
  live = (uint64_t*)mmap(nullptr, 2 * REGION / GRAN / 8, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
  if (base == MAP_FAILED || live == MAP_FAILED) perror("mmap"), exit(1);
#endif
  marks = live + REGION / GRAN / 64;
  pages.resize(NPAGES);
}
void gcOwn(Obj* o) { owners.push_back(o); }
void gcCollect() { gcWanted = false, collect(); }

static void* allocLarge(size_t bytes) {
  size_t n = (bytes + PAGE - 1) / PAGE;
  if (inUse + n > limit) inUse += n - 1, due(), inUse -= n - 1;
  size_t i = takePages(n);
  inUse += n;
  pages[i] = {2, 0, false, (u32)n};
  for (size_t j = 1; j < n; j++) pages[i + j] = {3, 0, false, 0};
  set(live, i * PBITS);
  return addr(i);
}
static void refill(int c) {  // vm.runs[c] has no room: the next free run, else a fresh page (collecting first if due)
  retire(c);
  if (nextRun(c)) return;
  if (due() && nextRun(c)) return;
  size_t i = takePages(1);
  pages[i] = {1, (uint8_t)c, false, 0}, inUse++;
  startRun(c, i, 0, PAGE / sizeClasses[c] * (sizeClasses[c] / GRAN));
}
void* gcAlloc(size_t bytes) {
  if (bytes > sizeClasses[NCLS - 1]) return allocLarge(bytes);
  int c = classOf[(bytes + 15) / 16];
  Run& r = vm.runs[c];
  if (r.end - r.cur < sizeClasses[c]) refill(c);
  return (void*)std::exchange(r.cur, r.cur + sizeClasses[c]);
}

Str* newStr(std::string_view a, std::string_view b) {  // a + b; the caller keeps a's and b's owners alive
  Str* o = make<Str>(a.size() + b.size() + 1);
  o->len = (u32)(a.size() + b.size()), o->hash = 2166136261u;
  memcpy(o->s(), a.data(), a.size()), memcpy(o->s() + a.size(), b.data(), b.size()), o->s()[o->len] = 0;
  for (char c : o->sv()) o->hash = (o->hash ^ (uint8_t)c) * 16777619u;
  return o;
}
