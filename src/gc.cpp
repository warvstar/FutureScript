// Allocator + GC. Objects live in one reserved region of 64KB pages; each page holds one size class
// (or is the head of a large object), so "is this word a heap pointer?" is O(1) arithmetic.
// Mark-sweep, conservative over the machine stack and registers, precise over the heap.
#include "fut.h"
#include <algorithm>
#include <cstdlib>
#include <sys/mman.h>

uintptr_t* stackTop;

namespace {
constexpr size_t PAGE = 1 << 16, REGION = 8ull << 30, NPAGES = REGION / PAGE;
constexpr u32 sizes[] = {16, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768, 1024, 1536, 2048, 4096, 8192, 16384};
constexpr int NCLS = sizeof sizes / sizeof *sizes;
struct Page { uint8_t type, cls; u32 n; };  // type: 0 unused, 1 small, 2 large head, 3 large tail
struct FreeCell { Obj h; FreeCell* next; };

uint8_t* base;
std::vector<Page> pages;
std::vector<size_t> freePages;
size_t bump, allocated, threshold = 8 << 20;
FreeCell* freeList[NCLS];
std::vector<Obj*> work;

uint8_t* addr(size_t i) { return base + i * PAGE; }
size_t takePages(size_t n) {
  if (n == 1 && !freePages.empty()) {
    size_t i = freePages.back();
    freePages.pop_back();
    return i;
  }
  if (bump + n > NPAGES) fputs("fut: out of memory\n", stderr), exit(1);
  return (bump += n) - n;
}
void releasePage(size_t i) {
  pages[i] = {};
  madvise(addr(i), PAGE, MADV_FREE);
  freePages.push_back(i);
}
Obj* find(uintptr_t p) {  // the object starting exactly at p, if any
  uintptr_t b = (uintptr_t)base;
  if (p < b || p >= b + bump * PAGE || (p & 7)) return nullptr;
  size_t i = (p - b) / PAGE, off = (p - b) % PAGE;
  if (pages[i].type == 1) {
    if (off % sizes[pages[i].cls] || off + sizes[pages[i].cls] > PAGE) return nullptr;
  } else if (pages[i].type != 2 || off) return nullptr;
  return ((Obj*)p)->kind == K::Free ? nullptr : (Obj*)p;
}
void mark(Obj* o) { if (o && !o->mark) o->mark = true, work.push_back(o); }
void markV(V v) { if (isObj(v)) mark(obj(v)); }
__attribute__((no_sanitize("address"))) void scan(uintptr_t* a, uintptr_t* b) {
  for (; a < b; a++) if (Obj* o = find((*a & PTR) == PTR ? *a & ADDR : *a)) mark(o);
}
void trace(Obj* o) {
  switch (o->kind) {
    case K::List: for (V v : ((List*)o)->v) markV(v); break;
    case K::Map: for (auto& [k, v] : ((Map*)o)->e) markV(k), markV(v); break;
    case K::Class: mark(((Class*)o)->super); for (auto& [s, f] : ((Class*)o)->methods) mark(f); break;
    case K::Inst: mark(((Inst*)o)->cls); for (u32 k = 0; k < ((Inst*)o)->n; k++) markV(((Inst*)o)->slots[k]); break;
    case K::Clo: mark(((Clo*)o)->fn); for (u32 k = 0; k < ((Clo*)o)->n; k++) markV(((Clo*)o)->env()[k]); break;
    default: break;
  }
}
void finalize(Obj* o) {
  switch (o->kind) {
    case K::List: ((List*)o)->~List(); break;
    case K::Map: ((Map*)o)->~Map(); break;
    case K::Class: ((Class*)o)->~Class(); break;
    case K::Inst: free(((Inst*)o)->slots); break;
    default: break;
  }
  o->kind = K::Free;
}
}  // namespace

void gcInit() {
  base = (uint8_t*)mmap(nullptr, REGION, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
  if (base == MAP_FAILED) perror("mmap"), exit(1);
  pages.resize(NPAGES);
}

static void gcCollect() {
  jmp_buf regs;
  setjmp(regs);  // spills callee-saved registers (where JIT locals live) so the scan sees them
  scan((uintptr_t*)&regs, (uintptr_t*)((char*)&regs + sizeof regs));
  scan((uintptr_t*)__builtin_frame_address(0), stackTop);
  for (V v : vm.globals) markV(v);
  for (V v : consts) markV(v);
  for (auto& [k, f] : methodFns) mark(f);
  for (auto& [k, f] : builtinMethods) mark(f);
  for (auto& [k, g] : globals) mark(g.o);
  while (!work.empty()) {
    Obj* o = work.back();
    work.pop_back();
    trace(o);
  }
  size_t live = 0;
  std::fill(freeList, freeList + NCLS, nullptr);
  for (size_t i = 0; i < bump; i++) {
    if (pages[i].type == 2) {
      Obj* o = (Obj*)addr(i);
      if (o->mark) o->mark = false, live += pages[i].n * PAGE;
      else {
        finalize(o);
        for (size_t j = 0, n = pages[i].n; j < n; j++) releasePage(i + j);
      }
    }
    if (pages[i].type != 1) continue;
    u32 sz = sizes[pages[i].cls];
    FreeCell* head = freeList[pages[i].cls];
    size_t used = 0;
    for (size_t off = 0; off + sz <= PAGE; off += sz) {
      Obj* o = (Obj*)(addr(i) + off);
      if (o->kind != K::Free && o->mark) { o->mark = false, used++; continue; }
      if (o->kind != K::Free) finalize(o);
      ((FreeCell*)o)->next = head, head = (FreeCell*)o;
    }
    if (used) freeList[pages[i].cls] = head, live += used * sz;
    else releasePage(i);  // whole page empty: give it back (its cells never made it onto the list)
  }
  allocated = 0, threshold = std::max<size_t>(8 << 20, live);
}

void* gcAlloc(size_t bytes) {
  if ((allocated += bytes) > threshold) gcCollect(), allocated = bytes;
  if (bytes > sizes[NCLS - 1]) {
    size_t n = (bytes + PAGE - 1) / PAGE, i = takePages(n);
    pages[i] = {2, 0, (u32)n};
    for (size_t j = 1; j < n; j++) pages[i + j] = {3, 0, 0};
    return addr(i);
  }
  int c = 0;
  while (sizes[c] < bytes) c++;
  if (!freeList[c]) {  // carve a fresh page into cells
    size_t i = takePages(1);
    pages[i] = {1, (uint8_t)c, 0};
    for (size_t off = 0; off + sizes[c] <= PAGE; off += sizes[c]) {
      auto* f = (FreeCell*)(addr(i) + off);
      f->h.kind = K::Free, f->next = freeList[c], freeList[c] = f;
    }
  }
  FreeCell* f = freeList[c];
  freeList[c] = f->next;
  return f;
}

Str* newStr(std::string_view s) {
  Str* o = make<Str>(s.size() + 1);
  o->len = (u32)s.size(), o->hash = 2166136261u;
  for (char c : s) o->hash = (o->hash ^ (uint8_t)c) * 16777619u;
  memcpy(o->s(), s.data(), s.size()), o->s()[s.size()] = 0;
  return o;
}
