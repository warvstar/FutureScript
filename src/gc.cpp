// Allocator + GC. Objects live in one reserved region of 64KB pages; each page holds one size class
// (or is part of a large object), so "is this word a heap pointer?" is O(1) arithmetic.
// Marking is conservative over the machine stack and registers, precise over the heap.
//
// Mark bits live in a side bitmap, one bit per 16-byte granule, set at the start of each marked object.
// When a collection ends that bitmap becomes the map of live objects, and there is no sweep: allocation
// bumps through the runs of free cells between live ones (vm->runs, which JIT code bumps directly), so a
// collection never touches a dead object. Objects that own malloc'd memory are registered (gcOwn) and
// finalized when found dead; a page with no live object goes back to the page pool.
#include "fut.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#if !__wasm__
#include <sys/mman.h>
#endif

// Isolates: every isolate has a heap of its own, and the image (what compiled code points at: constants, functions,
// classes) is one more that is never collected. They share the region, the page table and the bitmaps, but each
// page has one owner, and a heap allocates in, marks and frees only its own pages. A word of a page's bitmap
// covers no other page, so isolates collect at the same time, each without stopping the others. Only taking and
// returning pages locks (pageMu).
//
// In wasm, values in wasm locals can't be scanned, so collections happen only at safepoints, where every value
// that matters is in memory: an allocation that would collect asks for one (Heap::wanted, vm->poll) and goes on.

namespace {
constexpr size_t PAGE = 1 << 16, GRAN = 16, PBITS = PAGE / GRAN;
size_t REGION = WASM ? 256ull << 20 : 8ull << 30, NPAGES = REGION / PAGE;  // the heap's address range; gcRegion may set it
constexpr size_t MIN_HEAP = (8 << 20) / PAGE;
constexpr u32 NONE = ~0u;
// type: 0 free, 1 small, 2 large head, 3 large tail; cold: madvised; owner: the heap's id (0: none)
struct Page { uint8_t type, cls; bool cold; uint16_t owner; u32 n; };

uint8_t* base;
uint64_t *live, *marks;  // bit per granule: an object starts here and is allocated / was marked this collection
std::vector<Page> pages;
uint16_t owner(size_t i) { return __atomic_load_n(&pages[i].owner, __ATOMIC_RELAXED); }  // read by other isolates' scans
void setOwner(size_t i, uint16_t o) { __atomic_store_n(&pages[i].owner, o, __ATOMIC_RELAXED); }
std::vector<u32> pool;   // free pages, most recently freed last (still warm)
size_t bump;
Mutex pageMu;            // pool, bump, and pages[] entries changing owner
struct Scan { u32 page = NONE, at = 0; };
}  // namespace

struct Heap {
  uint16_t id;
  bool image = false, wanted = false;
  Run* runs;                     // where allocation bumps: the isolate's VM block (JIT code bumps it too), or own
  Run own[NCLS + 1] = {};
  std::vector<u32> avail[NCLS];  // pages with live objects (so maybe free cells too), per class
  Scan scans[NCLS];              // the page being searched for free runs, and where
  uintptr_t runStart[NCLS] = {}; // where runs[c] began: [runStart, cur) has been allocated
  std::vector<u32> mine;         // its small pages and the first page of each large object
  std::vector<Obj*> work, owners;
  size_t inUse = 0, limit = MIN_HEAP;
  uint64_t allocated = 0;        // objects allocated from retired runs, and large ones (see gcAllocations)
};

namespace {
TLS Heap* cur;         // the running isolate's heap
TLS int imaging;       // ImageScope depth: allocations go to the image
TLS Heap* target;      // where allocations go: the image while imaging, else cur (one thread-local read per allocation)
Heap* imageHeap;
std::atomic<uint16_t> nextId{1};
Heap& H() { return *target; }

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

size_t takePages(Heap& h, size_t n) {  // n pages for h; the caller sets their type
  Locked g(pageMu);
  size_t i;
  if (n == 1 && !pool.empty()) i = pool.back(), pool.pop_back();
  else {
    if (bump + n > NPAGES) fputs("fut: out of memory\n", stderr), exit(1);
    i = (bump += n) - n;
  }
  for (size_t j = i; j < i + n; j++) setOwner(j, h.id), pages[j].cold = false;
  h.mine.push_back((u32)i);
  return i;
}
void freePages(size_t i, size_t n) {  // their bits must be clear
  Locked g(pageMu);
  for (size_t j = i; j < i + n; j++) pages[j].type = pages[j].cls = 0, pages[j].n = 0, setOwner(j, 0), pool.push_back((u32)j);
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
void retire(Heap& h, int c) {  // the current run is done with: what was allocated from it is now in `live`
  h.allocated += (h.runs[c].cur - h.runStart[c]) / sizeClasses[c];
  setCells(c, h.runStart[c], h.runs[c].cur);
  h.runStart[c] = h.runs[c].cur = h.runs[c].end = 0;
}
void startRun(Heap& h, int c, size_t page, size_t from, size_t to) {  // granules within the page
  h.runStart[c] = h.runs[c].cur = (uintptr_t)addr(page) + from * GRAN, h.runs[c].end = (uintptr_t)addr(page) + to * GRAN;
}
bool nextRun(Heap& h, int c) {  // point runs[c] at the next free run in a page of class c that has live objects
  size_t k = sizeClasses[c] / GRAN, end = PAGE / sizeClasses[c] * k;  // granules: one cell, the whole cells
  for (Scan& s = h.scans[c];;) {
    if (s.page == NONE) {
      if (h.avail[c].empty()) return false;
      s = {h.avail[c].back(), 0}, h.avail[c].pop_back();
    }
    size_t g0 = (size_t)s.page * PBITS, from = nextFree(c, g0 + s.at, g0 + end, g0) - g0;
    if (from < end) {
      size_t to = nextSet(live, g0 + from, g0 + end) - g0;  // the next live object ends the run
      s.at = u32(to);
      return startRun(h, c, s.page, from, to), true;
    }
    s.page = NONE;
  }
}

// ---- collecting one heap (g): the others go on running ----
// The marking functions take the heap being collected, rather than read a thread-local for every object.
ISO_LOCAL(cur);
ISO_LOCAL(imaging);
ISO_LOCAL(target);
TLS Heap* G;  // the same heap, for markV: the callback that the runtime's root walkers (isolateMark, txnMark...) take
ISO_LOCAL(G);
Obj* find(Heap& g, uintptr_t p) {  // the allocated object of g starting exactly at p, if any
  uintptr_t b = (uintptr_t)base;
  if (p < b || p >= b + REGION || (p & (GRAN - 1))) return nullptr;
  size_t i = (p - b) / PAGE, off = (p - b) % PAGE;
  if (owner(i) != g.id) return nullptr;  // another isolate's, the image's, or free (only g's thread makes a page g's)
  if (pages[i].type == 1) {
    if (off % sizeClasses[pages[i].cls] || off + sizeClasses[pages[i].cls] > PAGE) return nullptr;
  } else if (pages[i].type != 2 || off) return nullptr;
  return test(live, bitOf(p)) ? (Obj*)p : nullptr;
}
bool marked(Obj* o) { return test(marks, bitOf((uintptr_t)o)); }
void mark(Heap& g, Obj* o) {  // objects of other heaps (the image's) are not g's to mark, and point at none of g's
  if (!o || owner(((uintptr_t)o - (uintptr_t)base) / PAGE) != g.id) return;
  size_t i = bitOf((uintptr_t)o);
  uint64_t& w = marks[i >> 6];
  if (uint64_t m = 1ull << (i & 63); !(w & m)) w |= m, g.work.push_back(o);
}
void mark(Heap& g, V v) { if (isObj(v)) mark(g, obj(v)); }
void markV(V v) { mark(*G, v); }
__attribute__((no_sanitize("address"))) void scan(Heap& g, uintptr_t* a, uintptr_t* b) {
  for (; a < b; a++) if (Obj* o = find(g, (*a & PTR) == PTR ? *a & ADDR : *a)) mark(g, o);
}
void trace(Heap& g, Obj* o) {
  switch (o->kind) {
    case K::List: for (V v : ((List*)o)->v) mark(g, v); break;
    case K::Map: for (auto& [k, v] : ((Map*)o)->e) mark(g, k), mark(g, v); break;
    case K::Class:
      mark(g, ((Class*)o)->super);
      for (auto& [s, f] : ((Class*)o)->methods) mark(g, f);
      for (auto& [s, d] : ((Class*)o)->fields) mark(g, d);
      break;
    case K::Inst: mark(g, ((Inst*)o)->cls); for (u32 k = 0; k < o->nslots; k++) mark(g, ((Inst*)o)->slots[k]); break;
    case K::Clo: mark(g, ((Clo*)o)->fn); for (u32 k = 0; k < ((Clo*)o)->n; k++) mark(g, ((Clo*)o)->env()[k]); break;
    case K::Fiber: {  // a switched-out fiber's stack (its saved registers included) is scanned like the main stack
      Fiber* f = (Fiber*)o;
      mark(g, f->fn), mark(g, f->val), mark(g, f->hstack), mark(g, f->resumer);
      if (f != fiberCur && f->stack) scan(g, (uintptr_t*)f->sp, f->top);
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
    case K::Chan: case K::Task: sharedFree(o); break;
    case K::Buf: free(((Buf*)o)->p); break;
    default: break;
  }
  o->kind = K::Free;
}
size_t words(size_t page, size_t n) { return n * PBITS / 64; }  // bitmap words of n pages
uint64_t* wordsAt(uint64_t* b, size_t page) { return b + page * PBITS / 64; }

void collect(Heap& h) {
  Heap& g = h;
  G = &h;
  for (int c = 0; c < NCLS; c++) retire(h, c), h.scans[c] = {}, h.avail[c].clear();
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
  scan(g, (uintptr_t*)&regs, (uintptr_t*)((char*)&regs + sizeof regs));  // (ASan may put it off the stack)
#endif
  scan(g, sp, stackTop);  // the current stack (a fiber's, if one is running)
  if (fiberCur) mark(g, fiberCur), scan(g, (uintptr_t*)fiberMainSp, fiberMainTop);
  for (V v : vm->globals) mark(g, v);
  isolateMark(markV);
  isolatesMark(markV);
  txnMark(markV);
  jsMark(markV);
  effMark(markV);
  while (!h.work.empty()) {
    Obj* o = h.work.back();
    h.work.pop_back();
    trace(g, o);
  }
  std::erase_if(h.owners, [](Obj* o) { return !marked(o) && (finalize(o), true); });
  h.inUse = 0;
  std::vector<u32> keep;
  for (u32 i : h.mine) {
    Page& p = pages[i];
    size_t n = p.type == 2 ? p.n : 1, used = 0;
    for (size_t w = 0; w < words(i, n); w++) used += std::popcount(wordsAt(marks, i)[w]);
    memcpy(wordsAt(live, i), wordsAt(marks, i), words(i, n) * 8), memset(wordsAt(marks, i), 0, words(i, n) * 8);
    if (!used) { freePages(i, n); continue; }
    keep.push_back(i), h.inUse += n;
    if (p.type == 1 && used < PAGE / sizeClasses[p.cls]) h.avail[p.cls].push_back(i);
  }
  h.mine = std::move(keep);
  for (auto& a : h.avail) std::sort(a.rbegin(), a.rend());  // allocate from low addresses up
  h.limit = std::max(MIN_HEAP, 2 * h.inUse);
  G = nullptr;
#if !__wasm__
  // Keep as many free pages warm as the heap may grow by before the next collection; give the rest back.
  Locked lock(pageMu);
  for (size_t j = 0, keep = h.limit - h.inUse; j + keep < pool.size(); j++)
    if (Page& p = pages[pool[j]]; !p.cold) madvise(addr(pool[j]), PAGE, MADV_FREE), p.cold = true;
#endif
}
bool due(Heap& h) {  // over the limit: collect now (natively), or at the next safepoint (wasm). The image never.
  if (h.image || h.inUse + 1 <= h.limit) return false;
  if (!WASM) return collect(h), true;
  return h.wanted = true, __atomic_fetch_or(&vm->poll, 2u, __ATOMIC_RELAXED), false;  // (the ticker sets bits too)
}
}  // namespace

void gcRegion(size_t bytes) { REGION = bytes / PAGE * PAGE, NPAGES = REGION / PAGE; }  // before gcInit
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
  imageHeap = new Heap{nextId++};
  imageHeap->image = true, imageHeap->runs = imageHeap->own;
}
Heap* heapNew(Run* runs) {
  Heap* h = new Heap{nextId++};
  if (!h->id) fputs("fut: too many isolates\n", stderr), exit(1);
  return h->runs = runs, h;
}
void heapUse(Heap* h) { cur = h; if (!imaging) target = h; }
void heapFree(Heap* h) {  // an isolate ended: finalize what needs it and give every page back
  for (int c = 0; c < NCLS; c++) h->runStart[c] = h->runs[c].cur = h->runs[c].end = 0;
  for (Obj* o : h->owners) finalize(o);
  for (u32 i : h->mine) {
    size_t n = pages[i].type == 2 ? pages[i].n : 1;
    memset(wordsAt(live, i), 0, words(i, n) * 8), memset(wordsAt(marks, i), 0, words(i, n) * 8), freePages(i, n);
  }
  delete h;
}
ImageScope::ImageScope() { if (!imaging++) target = imageHeap; }
ImageScope::~ImageScope() { if (!--imaging) target = cur; }
// (wasm: a compile error longjmps past ImageScope's destructor; where it lands puts the depth back)
int imageDepth() { return imaging; }
void imageUnwind(int depth) { if ((imaging = depth) == 0) target = cur; }
bool gcInImage(Obj* o) {
  uintptr_t p = (uintptr_t)o, b = (uintptr_t)base;
  return p >= b && p < b + REGION && owner((p - b) / PAGE) == imageHeap->id;
}
void gcOwn(Obj* o) { H().owners.push_back(o); }
// Objects the running isolate allocated so far. Each takes one cell of its size class, so the runs being bumped
// through (by JIT code too) say how many came from them: counting costs allocation nothing.
TLS uint64_t gcUncounted;
ISO_LOCAL(gcUncounted);
uint64_t gcAllocations() {
  uint64_t n = cur->allocated - gcUncounted;
  for (int c = 0; c < NCLS; c++) n += (cur->runs[c].cur - cur->runStart[c]) / sizeClasses[c];
  return n;
}
void gcCollect() { cur->wanted = false, collect(*cur); }
bool gcWanted() { return cur->wanted; }

static void* allocLarge(Heap& h, size_t bytes) {
  size_t n = (bytes + PAGE - 1) / PAGE;
  if (h.inUse + n > h.limit) h.inUse += n - 1, due(h), h.inUse -= n - 1;
  size_t i = takePages(h, n);
  h.inUse += n, h.allocated++;
  pages[i].type = 2, pages[i].cls = 0, pages[i].n = (u32)n;
  for (size_t j = 1; j < n; j++) pages[i + j].type = 3, pages[i + j].n = 0;
  set(live, i * PBITS);
  return addr(i);
}
static void refill(Heap& h, int c) {  // runs[c] has no room: the next free run, else a fresh page (collecting first if due)
  retire(h, c);
  if (nextRun(h, c)) return;
  if (due(h) && nextRun(h, c)) return;
  size_t i = takePages(h, 1);
  pages[i].type = 1, pages[i].cls = (uint8_t)c, pages[i].n = 0, h.inUse++;
  startRun(h, c, i, 0, PAGE / sizeClasses[c] * (sizeClasses[c] / GRAN));
}
void* gcAlloc(size_t bytes) {
  Heap& h = H();
  if (bytes > sizeClasses[NCLS - 1]) return allocLarge(h, bytes);
  int c = classOf[(bytes + 15) / 16];
  Run& r = h.runs[c];
  if (r.end - r.cur < sizeClasses[c]) refill(h, c);
  return (void*)std::exchange(r.cur, r.cur + sizeClasses[c]);
}

Str* newStr(std::string_view a, std::string_view b) {  // a + b; the caller keeps a's and b's owners alive
  Str* o = make<Str>(a.size() + b.size() + 1);
  o->len = (u32)(a.size() + b.size()), o->hash = 2166136261u;
  memcpy(o->s(), a.data(), a.size()), memcpy(o->s() + a.size(), b.data(), b.size()), o->s()[o->len] = 0;
  for (char c : o->sv()) o->hash = (o->hash ^ (uint8_t)c) * 16777619u;
  return o;
}
