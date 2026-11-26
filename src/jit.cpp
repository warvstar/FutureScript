// The arm64 half of the runtime: executable memory, stack traces and try, fibers, C calls and process reaping.
#include "fut.h"
#include <cmath>
#include <dlfcn.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <map>
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <time.h>
static_assert(offsetof(VM, helpers) == HOFF(0) && offsetof(VM, globals) == GOFF(0) && GOFF(MAXG) < (1 << 24));
static_assert(offsetof(VM, ics) == IOFF(0) && IOFF(MAXIC) < (1 << 24) && IOFF(0) % 16 == 0);
static_assert(offsetof(VM, runs) == ROFF(0) && ROFF(NCLS + 1) <= 504);  // ldp reaches every run from x28
static_assert(offsetof(VM, allowed) == EOFF && offsetof(VM, hstack) == EOFF + 8 && offsetof(VM, who) == EOFF + 16 && offsetof(VM, inv) == EOFF + 24 && offsetof(VM, invObj) == EOFF + 32 && offsetof(VM, budgets) == EOFF + 20 && offsetof(VM, fuel) == FOFF);
static_assert(INST_SLOTS == 16 && sizeof(Inst) == 24 && CLO_ENV == 24 && CLS_IHDR == 8 && CLS_IRUN == 16);  // as JIT code assumes

// ---------------- line tables, stack traces, errors ----------------
struct LineTab { u32* hi; std::string name, file; std::vector<std::pair<int, int>> lines; };
static std::map<u32*, LineTab> lineTabs;  // keyed by code start
static std::string frameDesc(uintptr_t ra) {
  if (ra < 4096) return "";
  u32* pc = (u32*)ra - 1;  // the call instruction
  auto it = lineTabs.upper_bound(pc);
  if (it == lineTabs.begin() || pc >= (--it)->second.hi || it->second.file.empty()) return "";
  int line = 0;
  for (auto [i, l] : it->second.lines) if (it->first + i <= pc) line = l;
  return "  at " + it->second.name + " (" + it->second.file + ":" + std::to_string(line) + ")\n";
}
std::string codeWhere(void* ra) {  // frameDesc's "  at f (file:line)\n", as "f (file:line)"
  Locked code(codeMu);
  std::string d = frameDesc((uintptr_t)ra);
  return d.size() > 6 ? d.substr(5, d.size() - 6) : d;
}
template <class F> static void eachReturnAddress(F f, uintptr_t* fp = (uintptr_t*)__builtin_frame_address(0), uintptr_t* top = stackTop) {
  for (; fp && fp < top; fp = (uintptr_t*)fp[0]) {
    f(fp[1]);
    if ((uintptr_t*)fp[0] <= fp) break;
  }
}
// Jump into a try's handler in its JIT frame: restore sp, fp, the pinned registers, x0 = the error value.
// txn: transactions open at its entry; allowed, hstack, who: the effect state at its entry (effects.cpp)
struct Handler { Handler* prev; void *sp, *fp, *limit, *pc; uint64_t txn, allowed; V hstack; uint64_t who, inv; };
#if __has_feature(address_sanitizer)
// JIT code appends to a list's std::vector in place (listPush), inside capacity libc++ has marked unused; ASan's
// container annotations can't see that, so they would report the vector's next reallocation.
extern "C" const char* __asan_default_options() { return "detect_container_overflow=0"; }
extern "C" void __asan_handle_no_return();  // the frames we jump past must not stay poisoned
#else
static void __asan_handle_no_return() {}
#endif
__attribute__((naked)) static void enterHandler(Handler*, V, VM*) {
  asm("ldr x9, [x0, #8]\n mov sp, x9\n ldr x29, [x0, #16]\n ldr x26, [x0, #24]\n ldr x9, [x0, #32]\n"
      "mov x28, x2\n movz x27, #0x7ffc, lsl #48\n mov x0, x1\n br x9");
}
static TLS std::string fiberTrace;  // frames of a fiber that ended with an error, printed if nothing catches it
ISO_LOCAL(fiberTrace);
void traceAdd(const std::string& frames) { fiberTrace += frames; }
[[noreturn]] void throwValue(V v, void* ra) {
  if (auto* h = (Handler*)vm->handler)  // the transactions opened inside the try roll back before its handler runs
    vm->handler = h->prev, txnUnwind((u32)h->txn), budgetUnwind(u32(h->who >> 32)), effLoad({h->allowed, h->hstack, (u32)h->who, h->inv}), fiberTrace.clear(), __asan_handle_no_return(), enterHandler(h, v, vm);
  // Uncaught: the site that failed, then every JIT frame found by walking the frame-pointer chain.
  std::vector<std::string> fr;
  {
    Locked code(codeMu);  // the line tables, which another isolate's compile may be adding to (and no longjmp inside)
    if (auto d = frameDesc((uintptr_t)ra); !d.empty()) fr.push_back(d);
    bool seen = fr.empty();
    eachReturnAddress([&](uintptr_t r) {
      if (!seen && r == (uintptr_t)ra) seen = true;
      else if (auto d = frameDesc(r); !d.empty()) fr.push_back(d);
    });
  }
  std::string trace;
  for (size_t i = 0; i < fr.size(); i++) {
    if (fr.size() > 16 && i == 10) trace += "  ... " + std::to_string(fr.size() - 15) + " more frames\n", i = fr.size() - 5;
    trace += fr[i];
  }
  fiberTrace += trace;
  if (fiberCur) fiberCur->val = v, longjmp(*errJmp, 1);  // ends the fiber; fiberResume throws v in its resumer
  if (errValue) *errValue = v, *errSink = fiberTrace;  // an isolate's: its task keeps both (isolate.cpp)
  else if (errSink) *errSink = errorLine(v) + "\n" + fiberTrace;
  else fflush(stdout), fprintf(stderr, "runtime error: %s\n%s", errorLine(v).c_str(), fiberTrace.c_str());
  fiberTrace.clear();
  if (errJmp) longjmp(*errJmp, 1);
  exit(1);
}
static void rt_hot(void* t, V a1, V a2, V a3, V a4, V a5, V a6, V a7) {  // a tier-1 body reached its call count
  u32 m = 0, i = 0;
  for (V a : {a1, a2, a3, a4, a5, a6, a7}) m |= u32(isNum(a)) << i++;
  tierUp(t, m);
}

// ---------------- C functions (extern fn): the thunk puts an Ext* in x7 and branches here ----------------
static V rt_ffi(V a0, V a1, V a2, V a3, V a4, V a5, V a6, Ext* e) {
  if (e->stub) ffiStub(e);
  volatile V a[] = {a0, a1, a2, a3, a4, a5, a6};  // on the stack, so string arguments stay alive
  int64_t x[8] = {};
  double d[8] = {};
  for (int i = 0, nx = 0, nd = 0; i < e->n; i++) {
    V v = a[i];
    char t = ffiCheck(e, i, v);
    if (t == 'd') d[nd++] = num(v);
    else x[nx++] = t == 's' ? (int64_t)as<Str>(v)->s() : t == 'B' ? (int64_t)bufBytes(v) : t == 'N' ? 0 : (int64_t)num(v);
  }
  // Integer and floating-point arguments use separate registers (x0..x7, d0..d7), so passing all 16 calls any
  // non-variadic C function correctly; it reads the ones it declares.
#define FFI_CALL(R) ((R(*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, double, double, double, \
                         double, double, double, double, double))e->fn)(x[0], x[1], x[2], x[3], x[4], x[5], x[6], x[7],  \
                                                                        d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7])
  if (e->ret == 'd') return box(FFI_CALL(double));
  int64_t r = FFI_CALL(int64_t);
  if (e->ret == 's') return r ? box(newStr((const char*)r)) : NIL;
  return e->ret == 'v' ? NIL : box(e->ret == 'i' ? (double)(int32_t)r : (double)r);
}

// ---------------- fibers ----------------
// fiberSwap saves the callee-saved registers on the current stack, stores sp in *save, switches to `to`
// and restores the registers saved there. A new fiber's stack holds such a frame "returning" to fiberStart.
#define SWAP_REGS(op) op " x19, x20, [sp]\n" op " x21, x22, [sp, #16]\n" op " x23, x24, [sp, #32]\n" op " x25, x26, [sp, #48]\n" \
  op " x27, x28, [sp, #64]\n" op " x29, x30, [sp, #80]\n" op " d8, d9, [sp, #96]\n" op " d10, d11, [sp, #112]\n"        \
  op " d12, d13, [sp, #128]\n" op " d14, d15, [sp, #144]\n"
__attribute__((naked)) static void fiberSwap(void**, void*) {
  asm("sub sp, sp, #160\n" SWAP_REGS("stp") "mov x9, sp\n str x9, [x0]\n mov sp, x1\n" SWAP_REGS("ldp") "add sp, sp, #160\n ret");
}
constexpr size_t FIBER_STACK = 1 << 20;  // reserved; pages are only touched as they are used
static TLS char* mainLimit;  // the isolate's main stack, while a fiber runs
static TLS jmp_buf* mainErr;
static TLS void* mainHandler;
static TLS std::vector<Fiber*> fibers;  // every fiber not yet collected (their suspended frames count for jitCollect)
ISO_LOCAL(mainLimit);
ISO_LOCAL(mainErr);
ISO_LOCAL(mainHandler);
ISO_LOCAL(fibers);
static void switchTo(Fiber* to) {   // from the running context to `to` (null: the main stack)
  Fiber* from = fiberCur;
  if (from) from->err = errJmp, from->handler = vm->handler;
  else fiberMainTop = stackTop, mainLimit = (char*)vm->limit, mainErr = errJmp, mainHandler = vm->handler;
  fiberCur = to, stackTop = to ? to->top : fiberMainTop, vm->limit = to ? to->limit : mainLimit, errJmp = to ? to->err : mainErr;
  vm->handler = to ? to->handler : mainHandler;
  fiberSwap(from ? &from->sp : &fiberMainSp, to ? to->sp : fiberMainSp);
}
static void fiberStart() {  // the bottom frame of every fiber
  Fiber* f = fiberCur;
  jmp_buf jb;
  errJmp = &jb;
  if (int r = setjmp(jb)) f->fail = r;  // an uncaught error (its value in val) or a main() restart: the resumer re-raises it
  else f->val = fiberCall(f);
  f->state = FIB_DONE, f->fn = NIL;
  switchTo(f->resumer);
  __builtin_unreachable();
}
void fiberFree(Fiber* f) {
  if (!f->stack) return;
  munmap(f->stack, FIBER_STACK), f->stack = nullptr;
  fibers[f->idx] = fibers.back(), fibers[f->idx]->idx = f->idx, fibers.pop_back();
}
void fiberStack(Fiber* b) {
  char* s = (char*)mmap(nullptr, FIBER_STACK, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  if (s == MAP_FAILED) ERR("@memory out of memory for fiber stacks");
  mprotect(s, 16 << 10, PROT_NONE);
  b->stack = s, b->limit = s + (64 << 10), b->top = (uintptr_t*)(s + FIBER_STACK);
  uintptr_t* sp = b->top - 20;  // a fiberSwap frame: x29 = 0 ends frame walks, x30 = fiberStart
  sp[11] = (uintptr_t)fiberStart;
  b->sp = sp, b->idx = (int)fibers.size(), fibers.push_back(b);
}
// Isolates switch contexts the same way (isolate.cpp): an isolate's stack is a fiberSwap frame while it is out.
void ctxSwap(void** save, void* to) { fiberSwap(save, to); }
void* ctxFrame(uintptr_t* top, void (*entry)()) {
  uintptr_t* sp = top - 20;  // as fiberStack: x29 = 0 ends frame walks, x30 = entry
  memset(sp, 0, 20 * sizeof *sp);
  sp[11] = (uintptr_t)entry;
  return sp;
}
void fiberRun(Fiber* f, bool) { switchTo(f); }
void fiberYield(Fiber* f) { switchTo(f->resumer); }
void restartMain() { longjmp(*errJmp, 2); }
// ---------------- executable memory ----------------
static uint8_t* jit;
static size_t jitUsed;
constexpr size_t JIT_SIZE = 128 << 20;  // one region keeps every bl/b within reach (+-128MB)
static std::map<u32*, size_t> freeBlocks;  // reclaimed code: address -> bytes
static std::vector<u32*> retired;
static u32* trap;
static V (*enter)(void* code, const V* args, VM* vm);

static TLS int writeDepth;  // write mode is per thread (pthread_jit_write_protect_np)
static TLS uint8_t *dirtyLo, *dirtyHi;
JitBatch::JitBatch() { if (!writeDepth++) pthread_jit_write_protect_np(0), dirtyLo = jit + JIT_SIZE, dirtyHi = jit; }
JitBatch::~JitBatch() {
  if (--writeDepth) return;
  pthread_jit_write_protect_np(1);
  if (dirtyLo < dirtyHi) sys_icache_invalidate(dirtyLo, dirtyHi - dirtyLo);
}
// A forked child (fut test runs each test in one) can start with this thread's JIT pages in write mode, so the
// first instruction fetched from them faults (about 1 fork in 25); set execute mode explicitly.
void jitForked() { pthread_jit_write_protect_np(writeDepth ? 0 : 1); }
// Written code is made visible to instruction fetch right away, not at the end of the batch: another isolate's
// thread may follow a stub patched later in the same batch into it, and would fetch stale bytes.
static void jitWrite(u32* p, const u32* src, size_t n) {
  JitBatch b;
  memcpy(p, src, n * 4);
  if (isolates > 1) sys_icache_invalidate(p, n * 4);
  else dirtyLo = std::min(dirtyLo, (uint8_t*)p), dirtyHi = std::max(dirtyHi, (uint8_t*)(p + n));
}
static u32* jitAlloc(size_t n) {
  size_t bytes = (n * 4 + 15) & ~size_t(15);
  for (auto it = freeBlocks.begin(); it != freeBlocks.end(); ++it)
    if (it->second >= bytes) {
      u32* p = it->first;
      size_t rest = it->second - bytes;
      freeBlocks.erase(it);
      if (rest) freeBlocks[p + bytes / 4] = rest;
      return p;
    }
  if (jitUsed + bytes > JIT_SIZE) fputs("fut: out of JIT memory\n", stderr), exit(1);
  return (u32*)(jit + (jitUsed += bytes) - bytes);
}
// Direct calls `bl` straight to a function's body rather than through its stub (saving a taken branch per
// call). Every such site is recorded, and after each reload re-pointed at wherever its stub now leads, so it
// behaves exactly as if it still went through the stub (new body, stale-arity trap or undefined trap).
static std::map<u32*, u32*> callSites;  // site -> the stub it stands for
static u32* through(u32* stub) { return stub + (int32_t(*stub << 6) >> 6); }
static void jitFree(u32* p, size_t bytes) {
  callSites.erase(callSites.lower_bound(p), callSites.lower_bound(p + bytes / 4));
  std::vector<u32> brk(bytes / 4, a64::BRK);  // anything that still jumps here traps loudly
  jitWrite(p, brk.data(), brk.size());
  auto it = freeBlocks.emplace(p, bytes).first;
  if (auto nx = std::next(it); nx != freeBlocks.end() && it->first + it->second / 4 == nx->first)
    it->second += nx->second, freeBlocks.erase(nx);
  if (it != freeBlocks.begin())
    if (auto pv = std::prev(it); pv->first + pv->second / 4 == it->first) pv->second += it->second, freeBlocks.erase(it);
}
static void listing(const Code& k) {  // --asm: an assembler file of .inst words with function/line labels
  if (k.file.empty()) return;
  static int uniq;
  std::string& s = *asmListing;
  int u = uniq++;
  s += "\"" + k.name + "#" + std::to_string(u) + "\":\n";
  for (size_t i = 0, li = 0; i < k.c.size(); i++) {
    for (; li < k.lines.size() && k.lines[li].first == (int)i; li++)
      s += "\"" + k.name + " line " + std::to_string(k.lines[li].second) + "#" + std::to_string(u) + "." + std::to_string(li) + "\":\n";
    char b[32];
    snprintf(b, 32, "  .inst 0x%08x\n", k.c[i]);
    s += b;
  }
}
u32* jitInstall(Code& k) {
  u32* p = jitAlloc(k.c.size());
  for (auto [i, t] : k.rel) callSites[p + i] = (u32*)t, k.c[i] |= u32(through((u32*)t) - (p + i)) & 0x3ffffff;
  jitWrite(p, k.c.data(), k.c.size());
  if (asmListing) listing(k);
  lineTabs[p] = {p + k.c.size(), std::move(k.name), std::move(k.file), std::move(k.lines)};
  return p;
}
void jitRetire(u32* body) { retired.push_back(body); }
size_t jitLive() {
  size_t f = 0;
  for (auto& [p, n] : freeBlocks) f += n;
  return jitUsed - f;
}
void jitCollect() {  // free retired code that no frame on the stack will return into; relink direct calls
  std::vector<uintptr_t> ras;
  auto keep = [&](uintptr_t r) { ras.push_back(r); };
  eachReturnAddress(keep);  // this stack, then suspended ones (their x29 is in the fiberSwap frame at sp[10])
  if (fiberCur) eachReturnAddress(keep, (uintptr_t*)((uintptr_t*)fiberMainSp)[10], fiberMainTop);
  for (Fiber* f : fibers)
    if (f != fiberCur && f->stack && f->state != FIB_NEW) eachReturnAddress(keep, (uintptr_t*)((uintptr_t*)f->sp)[10], f->top);
  std::erase_if(retired, [&](u32* b) {
    if (isolates > 1) return false;  // only this isolate's stacks were looked at: keep it until it is alone
    auto it = lineTabs.find(b);
    if (it == lineTabs.end()) return true;
    for (uintptr_t r : ras) if ((u32*)r > b && (u32*)r <= it->second.hi) return false;
    jitFree(b, ((it->second.hi - b) * 4 + 15) & ~size_t(15));
    lineTabs.erase(it);
    return true;
  });
  for (auto [site, stub] : callSites)
    if (u32 bl = a64::BL | (u32(through(stub) - site) & 0x3ffffff); *site != bl) jitWrite(site, &bl, 1);
}
void jitShiftLines(u32* body, int delta) {
  if (auto it = lineTabs.find(body); it != lineTabs.end()) for (auto& l : it->second.lines) l.second += delta;
}
// The hot-reload primitive: rewrite one `b target` instruction in place (single aligned 4-byte store).
void patchStub(u32* s, void* target) {
  u32 b = a64::B | (u32((u32*)target - s) & 0x3ffffff);
  jitWrite(s, &b, 1);
}
static std::vector<u32*> unusedStubs;
void discardStub(u32* stub) { unusedStubs.push_back(stub); }
u32* newStub() {  // stubs are packed 4 bytes apart; only unpublished stubs can be reused
  if (!unusedStubs.empty()) {
    u32* stub = unusedStubs.back();
    return unusedStubs.pop_back(), stub;
  }
  static u32 *pool, *poolEnd;
  if (pool == poolEnd) pool = jitAlloc(1024), poolEnd = pool + 1024;
  patchStub(pool, trap);
  return pool++;
}
u32* ffiCode(Ext* e) {  // x7 = e; b rt_ffi
  using namespace a64;
  Code k{{ldr(16, 28, HOFF(H_FFI)), br(16)}, {}, {}, "<extern>", ""};
  for (u32 hw = 4; hw-- > 0;) k.c.insert(k.c.begin(), (hw ? movk : movz)(7, (uintptr_t)e >> (16 * hw) & 0xffff, hw));
  return jitInstall(k);
}
u32* staleThunk(int sym) {  // x1 = caller's return address (b from the stub leaves x30 intact)
  using namespace a64;
  Code k{{mov(1, 30), movz(0, sym & 0xffff, 0), movk(0, (u32)sym >> 16, 1), ldr(16, 28, HOFF(H_STALE)), blr(16)}, {}, {}, "<stale>", ""};
  return jitInstall(k);
}
V vmEnter(void* code, V* args) { return enter(code, args, vm); }
bool readFile(const std::string& path, std::string& out) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return false;
  fseek(f, 0, SEEK_END), out.resize(ftell(f)), rewind(f);
  out.resize(fread(out.data(), 1, out.size(), f)), fclose(f);
  return true;
}
bool saveFile(const std::string& path, const std::string* text) {
  if (!text) return remove(path.c_str()) == 0;
  FILE* f = fopen(path.c_str(), "wb");
  if (!f) return false;
  bool ok = fwrite(text->data(), 1, text->size(), f) == text->size();
  return fclose(f) == 0 && ok;
}


void compileFail(const std::string& msg, bool atEnd) { throw CompileError(msg, atEnd); }
bool guardCompile(void (*f)(void*), void* ctx, std::string* err, bool* atEnd) {
  try { f(ctx); }
  catch (CompileError& e) {
    if (err) *err = e.what();
    if (atEnd) *atEnd = e.atEnd;
    return false;
  }
  return true;
}
std::string canonicalPath(const std::string& p) {
  char* r = realpath(p.c_str(), nullptr);
  std::string s = r ? r : p;
  return free(r), s;
}
void* externLookup(const std::string& lib, const std::string& name, const char** err) {
  void* h = lib.empty() ? RTLD_DEFAULT : dlopen(lib.c_str(), RTLD_LAZY);
  if (!h) return *err = "cannot load", nullptr;
  return dlsym(h, name.c_str());
}
void sleepFor(double s) {
  fflush(stdout);  // live loops sleep every frame; keep piped output flowing
  timespec t{(time_t)s, (long)((s - (time_t)s) * 1e9)};
  nanosleep(&t, nullptr);
}
void threadInit() {  // this thread's stack, for the GC and the JIT's overflow checks
  pthread_t self = pthread_self();
  stackTop = (uintptr_t*)pthread_get_stackaddr_np(self);
  vm->limit = (char*)stackTop - pthread_get_stacksize_np(self) + (256 << 10);
}
void platformInit() {
  threadInit();
  vm->helpers[H_FFI] = (void*)rt_ffi, vm->helpers[H_HOT] = (void*)rt_hot;
  jit = (uint8_t*)mmap(nullptr, JIT_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
  if (jit == MAP_FAILED) perror("mmap"), exit(1);
  using namespace a64;
  Code t{{ldr(16, 28, HOFF(H_UNDEF)), blr(16)}, {}, {}, "<undefined>", ""};
  trap = jitInstall(t);
  // enter(code, args[8], vm): save the callee-saved regs we pin (x26 stack limit, x27 QNAN, x28 VM), load args, call.
  Code e{{stp_pre(29, 30), addi(29, SP, 0), stp_pre(26, 27), stp_pre(28, 19), mov(9, 0), mov(28, 2), ldr(26, 28, 8),
          movz(27, 0x7ffc, 3), mov(10, 1)}, {}, {}, "<enter>", ""};
  for (u32 i = 0; i < 8; i++) e.c.push_back(ldr(i, 10, i * 8));
  for (u32 i : {blr(9), ldp_post(28, 19), ldp_post(26, 27), ldp_post(29, 30), RET}) e.c.push_back(i);
  enter = (V(*)(void*, const V*, VM*))jitInstall(e);
}

// ---------------- child processes: run(), isolated tests ----------------
// Read the child's pipes until they all close, or kill it when the timeout passes; returns its wait status.
int reap(int pid, std::vector<std::pair<int, std::string*>> pipes, double timeout, bool& timedOut) {
  double end = now() + timeout;
  for (size_t open = pipes.size(); open;) {
    double left = end - now();
    if (left <= 0) { timedOut = true, kill(pid, SIGKILL); break; }
    std::vector<pollfd> fds;
    for (auto& p : pipes) fds.push_back({p.first, POLLIN, 0});  // poll skips the closed ones (-1)
    if (poll(fds.data(), fds.size(), (int)(left * 1000) + 1) <= 0) continue;
    for (size_t i = 0; i < fds.size(); i++) {
      if (!fds[i].revents) continue;
      char b[4096];
      ssize_t k = read(fds[i].fd, b, sizeof b);
      if (k > 0) pipes[i].second->append(b, k);
      else close(fds[i].fd), pipes[i].first = -1, open--;
    }
  }
  for (auto& p : pipes) if (p.first >= 0) close(p.first);
  int st = 0;
  return waitpid(pid, &st, 0), st;
}
