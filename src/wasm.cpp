// The WebAssembly half of the runtime (jit.cpp is the arm64 one). fut runs inside a host (web/fut.js, in Node or a
// browser) that instantiates the modules this builds, switches fiber stacks (JSPI) and delivers file changes.
//  - Code: every function is a wasm function (V* window) -> V in the one function table that C++ function pointers
//    use too. A compile's functions go into one new module; installing it fills their table slots, and a stub is
//    just a table slot, so repointing a stub (hot reload) is one table.set.
//  - try/throw: wasm exceptions (tag `fut`, payload the value); the host prints uncaught ones with a stack trace.
//  - Fibers: each has its own shadow stack; the host suspends and resumes their wasm stacks with JSPI.
//  - Compile errors longjmp back to fut_compile (this libc++ has no C++ exceptions).
#include "wasm.h"
#include <cmath>
#include <map>

#define HOST(name) __attribute__((import_module("host"), import_name(#name)))
#define EXPORT(name) extern "C" __attribute__((export_name(#name)))
extern "C" {
HOST(install) void host_install(const uint8_t* mod, u32 len, const int32_t* ops, u32 nops);  // ops: table[dst] = table[src] (src -1: null)
HOST(grow) u32 host_grow(u32 n, u32 fill);                     // grow the table by n slots holding table[fill]
HOST(throw) [[noreturn]] void host_throw(V v);
HOST(restart) [[noreturn]] void host_restart();                // unwind to the host, which reruns main()
HOST(read) int host_read(const char* path);                    // a file's size (the host holds it), or -1
HOST(take) void host_take(char* dst);                          // copy out what the host holds
HOST(save) int host_save(const char* path, const char* text);  // write a file (text null: remove it); 0 if it failed
HOST(extern) int host_extern(const char* lib, const char* name);  // a host function's index, -1: none, -2: no such library
HOST(ffi) double host_ffi(int fn, const char* sig, const double* args, int ret);  // a string result: its length (in take)
HOST(resume) void host_resume(Fiber* f, int start);            // suspending: run f until it yields, returns or fails
HOST(yield) void host_yield(Fiber* f);                         // suspending: back to f's resumer
HOST(fiber_gone) void host_fiber_gone(Fiber* f);
HOST(sleep) void host_sleep(double seconds);                   // suspending
HOST(poll) void host_poll();                                   // suspending: run typed lines, reload changed files
HOST(run) int host_run(const char* args, const char* source, double timeout);  // run(): the result's length (in take), -1: none
}
static void* getsp() { void* p; asm volatile("global.get __stack_pointer\n\tlocal.set %0" : "=r"(p)); return p; }
static void setsp(void* p) { asm volatile("local.get %0\n\tglobal.set __stack_pointer" : : "r"(p)); }
extern char __stack_low, __stack_high;

// ---------------- code: modules, table slots, line tables ----------------
JitBatch::JitBatch() {}
JitBatch::~JitBatch() {}
struct Func { Code k; u32 slot; };
static std::vector<Func> batch;            // installed by the next jitCollect
static std::vector<int32_t> ops;           // table copies to make after it
static std::vector<u32> freeSlots, retired;
static u32 undefSlot;
struct Mod { size_t bytes; int refs; };
static std::vector<Mod> mods;
struct LineTab { std::string name, file; std::vector<std::pair<int, int>> lines; int mod; };  // lines: (module offset, line)
static std::map<u32, LineTab> lineTabs;    // by body slot
static u32 slot() {
  if (freeSlots.empty()) {
    u32 first = host_grow(64, undefSlot);
    for (u32 i = 64; i-- > 0;) freeSlots.push_back(first + i);
  }
  u32 s = freeSlots.back();
  return freeSlots.pop_back(), s;
}
u32* newStub() { return (u32*)(uintptr_t)slot(); }
u32* jitInstall(Code& k) {
  u32 s = slot();
  batch.push_back({std::move(k), s});
  return (u32*)(uintptr_t)s;
}
void patchStub(u32* stub, void* target) { ops.push_back((int32_t)wa::slotOf(stub)), ops.push_back((int32_t)wa::slotOf(target)); }
void jitRetire(u32* body) { retired.push_back(wa::slotOf(body)); }
void jitShiftLines(u32* body, int delta) {
  if (auto it = lineTabs.find(wa::slotOf(body)); it != lineTabs.end()) for (auto& l : it->second.lines) l.second += delta;
}
size_t jitLive() {
  size_t n = 0;
  for (auto& m : mods) n += m.refs ? m.bytes : 0;
  return n;
}
// Direct calls: each body has a "replaced" cell (never freed: old code keeps its address), set once its stub leads
// elsewhere or it is retired; a body whose cell is set sends a direct call on through its stub (Gen::finish).
static std::map<u32, u32> stubTo;           // stub slot -> the body it leads to
static std::map<u32, uint8_t*> cells;       // body slot -> its cell
static uint8_t* newCell() {
  static uint8_t* p;
  static size_t left;
  if (!left) p = (uint8_t*)calloc(4096, 1), left = 4096;
  return left--, p++;
}
static void replaced(u32 body) {
  if (auto it = cells.find(body); it != cells.end()) *it->second = 1, cells.erase(it);
}
static void section(std::vector<uint8_t>& m, uint8_t id, const std::vector<uint8_t>& body) {
  m.push_back(id), wa::uleb(m, body.size()), m.insert(m.end(), body.begin(), body.end());
}
static void name(std::vector<uint8_t>& o, const std::string& s) { wa::uleb(o, s.size()), o.insert(o.end(), s.begin(), s.end()); }
void jitCollect() {  // install the batch as one module, then make the queued table changes
  using namespace wa;
  for (u32 s : retired) {
    ops.push_back((int32_t)s), ops.push_back(-1), freeSlots.push_back(s);
    if (auto it = lineTabs.find(s); it != lineTabs.end()) mods[it->second.mod].refs--, lineTabs.erase(it);
  }
  retired.clear();
  std::map<u32, u32> next = stubTo;  // where each stub leads once the queued changes are made
  for (size_t i = 0; i < ops.size(); i += 2) if (ops[i + 1] >= 0) next[ops[i]] = ops[i + 1];
  std::map<u32, u32> inBatch;        // body slot -> function index
  std::vector<u32> bodies;
  for (size_t i = 0; i < batch.size(); i++) inBatch[batch[i].slot] = (u32)i, bodies.push_back(batch[i].slot);
  std::vector<uint8_t*> fresh;       // the batch's cells (they go live after the queued changes)
  for (size_t i = 0; i < batch.size(); i++) fresh.push_back(newCell());
  std::vector<uint8_t> m{0, 'a', 's', 'm', 1, 0, 0, 0}, s;
  if (!batch.empty()) {
    wa::uleb(s, 2 + NH), funcType(s, FUT_SIG);
    for (int h = 0; h < NH; h++) funcType(s, HSIG[h]);
    funcType(s, "i:l");
    section(m, 1, s), s.clear();
    wa::uleb(s, 4);
    name(s, "env"), name(s, "memory"), s.insert(s.end(), {2, 0, 0});
    name(s, "env"), name(s, "table"), s.insert(s.end(), {1, 0x70, 0, 0});
    name(s, "env"), name(s, "sp"), s.insert(s.end(), {3, I32, 1});
    name(s, "env"), name(s, "tag"), s.insert(s.end(), {4, 0}), wa::uleb(s, 1 + H_THROW);
    section(m, 2, s), s.clear();
    wa::uleb(s, batch.size());
    for (size_t i = 0; i < batch.size(); i++) s.push_back(0);
    section(m, 3, s), s.clear();
    wa::uleb(s, batch.size());
    for (size_t i = 0; i < batch.size(); i++) name(s, std::to_string(batch[i].slot)), s.push_back(0), wa::uleb(s, i);
    section(m, 7, s), s.clear();
    // Code: each body is its locals (fp; r0..r17; four temporaries; cached locals) and its instructions. Line tables record module offsets.
    // Branch hints (a custom section that must precede the code): by function, offsets within its body.
    std::vector<uint8_t> code, hints, hs;
    u32 hinted = 0;
    wa::uleb(code, batch.size());
    int mod = (int)mods.size();
    for (size_t fi = 0; fi < batch.size(); fi++) {
      auto& f = batch[fi];
      for (size_t r = 0; r < f.k.rel.size(); r++) {  // calls to known functions (see RelKind), as padded LEB128
        auto [at, target] = f.k.rel[r];
        u32 stub = slotOf(target);
        auto body = next.find(stub);
        auto j = body == next.end() ? inBatch.end() : inBatch.find(body->second);
        auto leb = [&](int o, u32 v) { for (int i = 0; i < 5; i++) f.k.c[o + i] = uint8_t((v >> (7 * i) & 0x7f) | (i < 4 ? 0x80 : 0)); };
        switch (r < f.k.relKind.size() ? f.k.relKind[r] : REL_SLOT) {
          case REL_SLOT: leb(at, stub); break;
          case REL_OWN: leb(at, slotOf(fresh[fi])); break;
          case REL_CALL:
            if (j != inBatch.end()) f.k.c[at] = CALL, leb(at + 1, j->second), f.k.c[at + 6] = f.k.c[at + 7] = f.k.c[at + 8] = NOP;
            else f.k.c[at] = I32_CONST, leb(at + 1, stub), f.k.c[at + 6] = CALL_INDIRECT, f.k.c[at + 7] = f.k.c[at + 8] = 0;
            break;
        }
      }
      std::vector<uint8_t> runs{1, I32, NREG, I64, 4, I32}, locals;  // then the locals' caches, in runs of one type
      u32 nruns = 3;
      auto& w = f.k.wlocals;
      for (size_t i = 0, j; i < w.size(); i = j, nruns++) {
        for (j = i; j < w.size() && w[j] == w[i]; j++);
        wa::uleb(runs, j - i), runs.push_back(w[i]);
      }
      wa::uleb(locals, nruns), locals.insert(locals.end(), runs.begin(), runs.end());
      if (!f.k.hints.empty()) {
        wa::uleb(hs, fi), wa::uleb(hs, f.k.hints.size()), hinted++;
        for (auto [at, h] : f.k.hints) wa::uleb(hs, locals.size() + at), hs.push_back(1), hs.push_back(h);
      }
      wa::uleb(code, locals.size() + f.k.c.size());
      code.insert(code.end(), locals.begin(), locals.end());
      LineTab& t = lineTabs[f.slot] = {std::move(f.k.name), std::move(f.k.file), {}, mod};
      for (auto [at, line] : f.k.lines) t.lines.push_back({int(code.size() + at), line});  // + where `code` lands, below
      code.insert(code.end(), f.k.c.begin(), f.k.c.end());
    }
    if (hinted) {
      name(hints, "metadata.code.branch_hint"), wa::uleb(hints, hinted), hints.insert(hints.end(), hs.begin(), hs.end());
      section(m, 0, hints);
    }
    size_t base = m.size() + 1 + 5;  // section id + a 5-byte size (padded below) = where `code` starts
    for (auto& f : batch) for (auto& l : lineTabs[f.slot].lines) l.first += (int)base;
    m.push_back(10);
    for (int i = 0; i < 5; i++) m.push_back(uint8_t((code.size() >> (7 * i) & 0x7f) | (i < 4 ? 0x80 : 0)));
    m.insert(m.end(), code.begin(), code.end());
    std::vector<uint8_t> names, sub;  // function names, for stack traces: name@slot
    name(names, "name"), wa::uleb(sub, batch.size());
    for (size_t i = 0; i < batch.size(); i++) wa::uleb(sub, i), name(sub, lineTabs[batch[i].slot].name + "@" + std::to_string(batch[i].slot));
    names.push_back(1), wa::uleb(names, sub.size()), names.insert(names.end(), sub.begin(), sub.end());
    section(m, 0, names);
    mods.push_back({m.size(), (int)batch.size()});
    batch.clear();
  }
  if (m.size() > 8 || !ops.empty()) host_install(m.size() > 8 ? m.data() : nullptr, (u32)m.size(), ops.data(), (u32)ops.size() / 2);
  for (size_t i = 0; i < fresh.size(); i++) cells[bodies[i]] = fresh[i];
  for (size_t i = 0; i < ops.size(); i += 2) {
    u32 dst = ops[i];
    if (ops[i + 1] < 0) { replaced(dst), stubTo.erase(dst); continue; }  // a retired body
    auto it = stubTo.find(dst);
    if (it != stubTo.end() && it->second != (u32)ops[i + 1]) replaced(it->second);
    stubTo[dst] = ops[i + 1];
  }
  ops.clear();
}
static Code thunk(std::initializer_list<uint8_t> c, const char* nm) { Code k; k.c = c, k.name = nm; return k; }
static void i32(Code& k, int32_t v) { k.c.push_back(wa::I32_CONST), wa::sleb(k.c, v); }
u32* staleThunk(int sym) {  // rt_stale(sym)
  using namespace wa;
  Code k = thunk({LOCAL_GET, SP, GLOBAL_SET, 0, I64_CONST}, "<stale>");  // C++ frames go below the caller's
  wa::sleb(k.c, sym), i32(k, 0), i32(k, (int32_t)slotOf(vm.helpers[H_STALE]));
  k.c.insert(k.c.end(), {CALL_INDIRECT, 1 + H_STALE, 0, UNREACHABLE, ENDOP});
  return jitInstall(k);
}
static V rt_ffi(V* w, Ext* e) {
  double a[8] = {};
  for (int i = 0; i < e->n; i++) {
    V v = w[i];
    a[i] = ffiCheck(e, i, v) == 's' ? (double)(uintptr_t)as<Str>(v)->s() : num(v);
  }
  double r = host_ffi((int)(uintptr_t)e->fn - 1, e->sig, a, e->ret);
  if (e->ret == 's') {
    if (r < 0) return NIL;
    std::string s((size_t)r, 0);
    return host_take(s.data()), box(newStr(s));
  }
  return e->ret == 'v' ? NIL : box(e->ret == 'i' ? (double)(int32_t)(int64_t)r : r);
}
u32* ffiCode(Ext* e) {  // rt_ffi(window, e)
  using namespace wa;
  Code k = thunk({LOCAL_GET, SP, GLOBAL_SET, 0, LOCAL_GET, W}, "<extern>");
  i32(k, (int32_t)slotOf(e)), i32(k, (int32_t)slotOf(vm.helpers[H_FFI]));
  k.c.insert(k.c.end(), {CALL_INDIRECT, 1 + H_FFI, 0, ENDOP});
  return jitInstall(k);
}
// A C++ function V f(V* window) callable from fut code: a wasm adapter with the fut signature tells C++ where the
// fut stack is (the caller's sp) and passes the window on.
void* futAdapter(void* cpp) {
  using namespace wa;
  Code k = thunk({LOCAL_GET, SP, GLOBAL_SET, 0, LOCAL_GET, W}, "<native>");
  i32(k, (int32_t)slotOf(cpp)), k.c.insert(k.c.end(), {CALL_INDIRECT, 1 + NH, 0, ENDOP});
  return jitInstall(k);
}
void* externLookup(const std::string& lib, const std::string& name, const char** err) {
  int i = host_extern(lib.c_str(), name.c_str());
  if (i == -2) *err = "cannot load";
  return i < 0 ? nullptr : (void*)(uintptr_t)(i + 1);
}
V vmEnter(void* code, V* args) {  // fut code runs below this C++ frame
  return ((V(*)(V*, void*, V, V, V, V))code)(args, getsp(), args[0], args[1], args[2], args[3]);
}

// ---------------- errors ----------------
[[noreturn]] void throwValue(V v, void*) { host_throw(v); }
static jmp_buf* compileJmp;
static std::string compileErr;
static bool compileAtEnd;
void compileFail(const std::string& msg, bool atEnd) { compileErr = msg, compileAtEnd = atEnd, longjmp(*compileJmp, 1); }
bool guardCompile(void (*f)(void*), void* ctx, std::string* err) {  // a compile outside fut_compile (tier 2, a test body)
  jmp_buf jb, *outer = compileJmp;
  compileJmp = &jb;
  if (setjmp(jb)) return compileJmp = outer, err ? (void)(*err = compileErr) : batch.clear(), false;
  f(ctx), compileJmp = outer;
  return true;
}

// ---------------- fibers: the host switches wasm stacks; we switch the shadow stack, limit and GC roots ----------------
constexpr size_t FIBER_STACK = 256 << 10;
static char* mainLimit;
void fiberFree(Fiber* f) {
  if (f->stack) free(f->stack), f->stack = nullptr, host_fiber_gone(f);
}
void fiberStack(Fiber* b) {
  b->stack = (char*)aligned_alloc(16, FIBER_STACK);
  if (!b->stack) ERR("out of memory for fiber stacks");
  b->limit = b->stack + (16 << 10), b->top = (uintptr_t*)(b->stack + FIBER_STACK), b->sp = b->top;
}
static void use(Fiber* f) { fiberCur = f, stackTop = f ? f->top : fiberMainTop, vm.limit = f ? f->limit : mainLimit; }
static void enter(Fiber* f) { use(f), setsp(f ? f->sp : fiberMainSp); }  // f (null: main) runs again, on its shadow stack
static void leave() {  // save the running context's shadow stack
  if (Fiber* f = fiberCur) f->sp = getsp();
  else fiberMainSp = getsp(), fiberMainTop = stackTop, mainLimit = (char*)vm.limit;
}
__attribute__((noinline)) static void fiberBody(Fiber* f) {
  use(f), f->val = fiberCall(f);
  f->state = FIB_DONE, f->fn = NIL;
}
EXPORT(fut_fiber_start) void fiberStart(Fiber* f) { setsp(f->top), fiberBody(f); }  // no frame of its own: none on the resumer's stack
EXPORT(fut_fiber_fail) void fiberFail(Fiber* f, int kind, V v) {  // an error (1) or a main() restart (2) ended it
  f->fail = kind, f->val = kind == 3 ? box(newStr((const char*)(uintptr_t)v)) : v, f->state = FIB_DONE, f->fn = NIL;
  if (kind == 3) f->fail = 1;
}
void fiberRun(Fiber* f, bool start) {
  Fiber* from = fiberCur;
  leave(), fiberCur = f, host_resume(f, start), enter(from);
}
void fiberYield(Fiber* f) { leave(), host_yield(f), enter(f); }
void restartMain() { host_restart(); }

// ---------------- files, paths, time ----------------
bool readFile(const std::string& path, std::string& out) {
  int n = host_read(path.c_str());
  if (n < 0) return false;
  out.resize(n), host_take(out.data());
  return true;
}
bool saveFile(const std::string& path, const std::string* text) { return host_save(path.c_str(), text ? text->c_str() : nullptr); }
std::string canonicalPath(const std::string& p) {  // resolve . and .. (the host gives absolute paths)
  std::vector<std::string> parts;
  for (size_t i = 0, j; i <= p.size(); i = j + 1) {
    j = p.find('/', i);
    if (j == std::string::npos) j = p.size();
    std::string s = p.substr(i, j - i);
    if (s == "..") { if (!parts.empty()) parts.pop_back(); }
    else if (!s.empty() && s != ".") parts.push_back(s);
  }
  std::string r;
  for (auto& s : parts) r += "/" + s;
  return r.empty() ? "/" : r;
}
void sleepFor(double s) { fflush(stdout), host_sleep(s); }
// The host runs fut in a child process (Node: web/run.mjs again). args: one per line, the file (empty: source) last.
// Its answer: "code timedOut outBytes\n" then stdout and stderr.
bool runProcess(const std::vector<std::string>& args, const std::string& file, const std::string* source, double timeout, ProcResult& r) {
  std::string a;
  for (auto& x : args) a += x + "\n";
  int n = host_run((a + file).c_str(), source ? source->c_str() : nullptr, timeout);
  if (n < 0) return false;
  std::string res(n, 0);
  host_take(res.data());
  int to = 0;
  size_t outLen = 0, nl = res.find('\n');
  if (nl == std::string::npos || sscanf(res.c_str(), "%d %d %zu", &r.code, &to, &outLen) != 3) return false;
  r.timedOut = to, r.out = res.substr(nl + 1, outLen), r.err = res.substr(nl + 1 + outLen);
  return true;
}
void gcCollect();
extern bool gcWanted;
static void safepoint() {  // rt_poll: a collection the allocator asked for, then the host's events
  u32 p = vm.poll;
  vm.poll = 0;
  if (gcWanted) gcCollect();
  if (p & 1) vm.txn ? void(txnPollLater = true) : host_poll();  // reloads and typed lines wait for transactions to end
}
static V undefFn() { ERR("call to undefined function"); }
static void rt_hot(void* t, V* args, int n) {  // a tier-1 body reached its call count
  u32 m = 0;
  for (int i = 0; i < n; i++) m |= u32(isNum(args[i])) << i;
  tierUp(t, m);
}
void platformInit() {
  stackTop = (uintptr_t*)&__stack_high, vm.limit = &__stack_low + (64 << 10);
  vm.helpers[H_FFI] = (void*)rt_ffi, vm.helpers[H_HOT] = (void*)rt_hot;
  undefSlot = wa::slotOf(callable<undefFn>());
  jitCollect();
  for (u32 s : freeSlots) ops.push_back((int32_t)s), ops.push_back((int32_t)undefSlot);  // grown before undefFn existed
  jitCollect();
  pollHook = safepoint;
}

// ---------------- the host's entry points ----------------
static std::string text;  // what the last string-returning export returned
static Compiled last;
EXPORT(fut_init) void futInit(int v) { verbose = v, rtInit(), jitCollect(); }  // installs the natives' adapters
EXPORT(fut_alloc) char* futAlloc(u32 n) { return (char*)malloc(n); }
EXPORT(fut_free) void futFree(char* p) { free(p); }
// Compile a file (text null) or REPL input; returns the chunk's slot, 0 after an error (printed), -1 if input ended early.
EXPORT(fut_compile) int futCompile(const char* path, int reload, int echo, const char* src, int tests) {
  jmp_buf jb, *outer = compileJmp;
  compileJmp = &jb;
  if (setjmp(jb)) {
    compileJmp = outer, batch.clear(), ops.clear();
    if (echo && compileAtEnd) return -1;
    if (tests) return reportCompileError(compileErr), 0;
    return fflush(stdout), fprintf(stderr, "%s\n", compileErr.c_str()), 0;
  }
  std::string t = src ? src : "";
  last = compile(path, reload, echo, src ? &t : nullptr, tests);
  compileJmp = outer;
  return (int)wa::slotOf(last.chunk);
}
EXPORT(fut_info) const char* futInfo(int what) {  // 0: the reload log, 1: the files compiled, 2: the last compile error
  text = what == 0 ? last.log : what == 2 ? compileErr : "";
  if (what == 1) for (auto& f : last.files) text += f + "\n";
  return text.c_str();
}
EXPORT(fut_main_changed) int futMainChanged() { return last.mainChanged; }
EXPORT(fut_fn) int futFn(const char* nm) {  // a top-level fn of no arguments: its stub, else 0
  Global* g = findGlobal(intern(nm));
  return g && g->kind == 'f' && g->o && ((Fn*)g->o)->arity == 0 ? (int)wa::slotOf(g->stub) : 0;
}
EXPORT(fut_call) V futCall(int slot) { return vmCall((void*)(uintptr_t)slot); }
EXPORT(fut_retire) void futRetire(int slot) { jitRetire((u32*)(uintptr_t)slot); }
EXPORT(fut_str) const char* futStr(V v) { return (text = toStr(v)).c_str(); }
EXPORT(fut_where) const char* futWhere(int slot, int off) {  // "name (file:line)" for a frame at module offset off
  auto it = lineTabs.find(slot);
  if (it == lineTabs.end() || it->second.file.empty()) return "";
  int line = 0;
  for (auto [at, l] : it->second.lines) if (at <= off) line = l;
  return (text = it->second.name + " (" + it->second.file + ":" + std::to_string(line) + ")").c_str();
}
EXPORT(fut_live) u32 futLive() { return (u32)jitLive(); }
// fut test: the host calls the top level, then each test's code, catching errors (see test.cpp for the rest)
EXPORT(fut_tests_begin) void futTestsBegin(int json, const char* filter) { testsBegin({json != 0, filter}); }
EXPORT(fut_test_count) int futTestCount() { return (int)last.tests.size(); }
EXPORT(fut_test_start) int futTestStart(int i) { return (int)wa::slotOf(testStart(last.tests[i])); }
EXPORT(fut_test_finish) void futTestFinish(int i, int errored, const char* err) { testFinish(last.tests[i], errored, err); }
EXPORT(fut_test_top_error) void futTestTopError(const char* err) { reportTopError(err); }
EXPORT(fut_tests_end) int futTestsEnd() { return testsEnd(); }
// The last compile's interfaces: mode 0 reports them as text, 1 as JSON, 2 writes them (fut api); the number
EXPORT(fut_api) int futApi(int mode, const char* program) {
  return mode == 2 ? writeInterfaces(last.sources, program) : reportInterfaces(checkInterfaces(last.sources), mode == 1, program);
}
EXPORT(fut_poll) u32* futPoll() { return &vm.poll; }
// Transactions, for the host: a hot reload's top level and reload() hook run inside one (see hostPoll in fut.js),
// and an error that reaches the host rolls back those opened since the call began.
EXPORT(fut_txn_depth) u32 futTxnDepth() { return vm.txn; }
EXPORT(fut_txn_begin) void futTxnBegin() { txnBegin(); }
EXPORT(fut_txn_commit) void futTxnCommit() { txnCommit(); }
EXPORT(fut_txn_unwind) void futTxnUnwind(u32 depth) { txnUnwind(depth); }
EXPORT(fut_reload_failed) void futReloadFailed() {  // roll back the reload's transaction, then its code
  txnAbort();
  std::string err;
  if (revertReload(last.newVars, &err)) fputs("[fut] reload rolled back: the program's data and code are as they were before it\n", stderr);
  else fprintf(stderr, "[fut] reload rolled back its data, but the previous code did not compile again: %s\n", err.c_str());
}
