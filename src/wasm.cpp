// The WebAssembly half of the runtime (jit.cpp is the arm64 one). fut runs inside a host (web/fut.js, in Node or a
// browser) that instantiates the modules this builds, switches fiber stacks (JSPI) and delivers file changes.
//  - Code: every function is a wasm function (V* window) -> V in the one function table that C++ function pointers
//    use too. A compile's functions go into one new module; installing it fills their table slots, and a stub is
//    just a table slot, so repointing a stub (hot reload) is one table.set.
//  - try/throw: wasm exceptions (tag `fut`, payload the value); the host prints uncaught ones with a stack trace.
//  - Fibers: each has its own shadow stack; the host suspends and resumes their wasm stacks with JSPI.
//  - Compile errors longjmp back to fut_compile (this libc++ has no C++ exceptions).
#include "wasm.h"
#include <cerrno>
#include <cmath>
#include <map>
#include <unordered_map>

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
HOST(ffi) double host_ffi(int fn, const char* sig, const double* args, int ret, int* failed);  // a string or any result:
HOST(ffi_now) double host_ffi_now(int fn, const char* sig, const double* args, int ret, int* failed);  // the same, not
// suspending: inside a callback JavaScript makes synchronously (fut_sync), where nothing can suspend
static TLS int syncDepth;   // fut code JavaScript called synchronously, from inside an extern call (fut_sync): this thread's
static TLS bool syncPoll;   // a host poll wanted meanwhile: it waits until that code returns
                                                                                              // its length (in take);
                                                                                              // failed: the message's
HOST(ref_gone) void host_ref_gone(u32 id);                     // no fut code holds JavaScript value id any more
HOST(resume) void host_resume(Fiber* f, int start);            // suspending: run f until it yields, returns or fails
HOST(yield) void host_yield(Fiber* f);                         // suspending: back to f's resumer
HOST(fiber_gone) void host_fiber_gone(Fiber* f);
HOST(vm) void host_vm(VM* v);                                  // the running isolate's VM block: JIT code's global 1
HOST(ctx_switch) void host_ctx_switch(void* save, void* to);   // suspending: this context waits as `save`; `to` runs
HOST(ctx_gone) void host_ctx_gone(void* ctx);
HOST(trycall) u32 host_trycall();                             // try_call's table slot (instantiated now, if need be)
HOST(last_trace) int host_last_trace();                       // the last fut error thrown: its frames' text (in take)
HOST(where) int host_where();                                 // the innermost fut frame running: "f (file:line)" (in take)
HOST(cores) int host_cores();                                 // threads the host can run at once
HOST(sleep) void host_sleep(double seconds);                   // suspending
HOST(poll) void host_poll();                                   // suspending: run typed lines, reload changed files
HOST(run) int host_run(const char* args, const char* source, double timeout);  // suspending: run(): the result's length (in take), -1: none
HOST(fetch) int host_fetch(const char* reqs);                  // suspending: HTTP requests (a JSON list), at once; their
                                                               // results' JSON length (in take), -1: the host has no fetch
}
static void* getsp() { void* p; asm volatile("global.get __stack_pointer\n\tlocal.set %0" : "=r"(p)); return p; }
static void setsp(void* p) { asm volatile("local.get %0\n\tglobal.set __stack_pointer" : : "r"(p)); }
extern char __stack_low, __stack_high;

// The interpreter (interp.cpp), for a host that can't compile code: it runs each batch's bodies instead.
extern bool interpMode;
void interpInit();
void interpSetTryCall(u32 slot);
void interpAdd(const std::vector<u32>& slots, std::vector<std::vector<uint8_t>>& bodies, std::vector<int32_t>& ops);
void interpRetire(u32 slot);
extern void* interpTop;
std::string interpTrace_(const char* (*where)(int slot, int off));

// ---------------- code: modules, table slots, line tables ----------------
JitBatch::JitBatch() {}
JitBatch::~JitBatch() {}
// Threads (the threads build): each thread has its own instance of fut.wasm and of every module the compiler
// emitted, and its own function table, which must hold what every other thread's holds, slot for slot. Every change
// to a table is made by a thread holding codeMu, which first catches up (tableSync), then logs it; the others make
// the changes they have yet to before they could reach them: whenever they switch isolates, decode a message (it may
// hold a function compiled since), or reach a safepoint (bit 32 asks them to: tier 2 code to adopt).
#if ONE_THREAD
static u32 tableInstall(const uint8_t* mod, u32 len, const int32_t* ops, u32 nops) { return host_install(mod, len, ops, nops), 0; }
static u32 tableGrow(u32 n, u32 fill) { return host_grow(n, fill); }
static u32 tableTryCall() { return host_trycall(); }
void codeSync() {}
#else
struct TableOp { int kind; std::vector<uint8_t> mod; std::vector<int32_t> ops; u32 n, fill; };  // 0 install, 1 grow, 2 try_call
static std::deque<TableOp> tableLog;  // (under codeMu)
static std::atomic<u32> tableLen;
static thread_local u32 tableAt;      // how much of it this thread's table has
// The C library has no thread exit hooks for thread_local destructors. fut's threads (a pool's, the ticker) never
// end, and the main thread's end is the process's: nothing to run.
extern "C" int __cxa_thread_atexit(void (*)(void*), void*, void*) { return 0; }
// exit(), replaced: the C library's runs the atexit handlers and closes stdio under locks, which a worker thread
// can't, as the main thread goes on (and its JavaScript can't end the process): a worker asks the main thread to
// (isolatesExit) and waits. On the main thread, output is flushed and the process ends; static objects are left as
// they are, and other threads may still be using them.
extern "C" void exit(int code) {
  fflush(stdout), fflush(stderr);
  if (!isolateMainThread()) {
    unwindLocks(nullptr);  // the main thread may need one to reach its exit check
    isolatesExit(code);
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    static pthread_cond_t never = PTHREAD_COND_INITIALIZER;
    pthread_mutex_lock(&mu);
    for (;;) pthread_cond_wait(&never, &mu);
  }
  _Exit(code);
}
// malloc's memory (the C library's sbrk, replaced). A thread that grows the memory sees the new size at once, but
// V8 tells the other threads' instances (and their JavaScript) asynchronously, and until they hear, memory past the
// old end is out of bounds to them. So grow well ahead of use: what malloc hands out was grown long before. (The
// linked initial memory is large too, and malloc starts with that: most programs never grow it.)
extern "C" void* sbrk(intptr_t inc) {
  constexpr size_t PAGE = 65536, AHEAD = 64 << 20;
  static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
  static size_t brk;
  pthread_mutex_lock(&mu);
  size_t have = __builtin_wasm_memory_size(0) * PAGE;
  if (!brk) brk = have;  // (malloc has had __heap_base..__heap_end, the linked initial memory)
  void* r = (void*)brk;
  if (inc < 0 || (uint64_t)brk + inc + AHEAD > (uint64_t)4 << 30) r = (void*)-1, errno = ENOMEM;  // (size_t is 32 bits)
  else if (brk + inc + AHEAD > have && __builtin_wasm_memory_grow(0, (brk + inc + AHEAD - have + PAGE - 1) / PAGE) == SIZE_MAX &&
           brk + inc > have && __builtin_wasm_memory_grow(0, (brk + inc - have + PAGE - 1) / PAGE) == SIZE_MAX)
    r = (void*)-1, errno = ENOMEM;  // (not even without the room ahead)
  else brk += inc;
  pthread_mutex_unlock(&mu);
  return r;
}
static void replay(const TableOp& o) {
  if (o.kind == 0) host_install(o.mod.empty() ? nullptr : o.mod.data(), (u32)o.mod.size(), o.ops.data(), (u32)o.ops.size() / 2);
  else if (o.kind == 1) host_grow(o.n, o.fill);
  else host_trycall();
}
static void tableSync() {  // (codeMu held)
  for (u32 n = tableLen.load(std::memory_order_acquire); tableAt < n; tableAt++) replay(tableLog[tableAt]);
}
void codeSync() {
  if (tableAt == tableLen.load(std::memory_order_acquire)) return;
  Locked code(codeMu);
  tableSync();
}
static u32 logged(TableOp&& o) {  // make the change here, then log it for the other threads
  Locked code(codeMu);
  tableSync();
  u32 r = o.kind == 0 ? (replay(o), 0) : o.kind == 1 ? host_grow(o.n, o.fill) : host_trycall();
  tableLog.push_back(std::move(o)), tableAt = tableLen.fetch_add(1, std::memory_order_release) + 1;
  eachVM([](VM* v, void*) { if (v != vm) __atomic_fetch_or(&v->poll, 32u, __ATOMIC_RELAXED); }, nullptr);
  return r;
}
static u32 tableInstall(const uint8_t* mod, u32 len, const int32_t* ops, u32 nops) {
  return logged({0, std::vector<uint8_t>(mod, mod + (mod ? len : 0)), std::vector<int32_t>(ops, ops + 2 * nops), 0, 0});
}
static u32 tableGrow(u32 n, u32 fill) { return logged({1, {}, {}, n, fill}); }
static u32 tableTryCall() { return logged({2, {}, {}, 0, 0}); }
#endif
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
    u32 first = tableGrow(64, undefSlot);
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
// fetch and fetchAll, as in sys.cpp, but the host makes the requests (its fetch(), all at once); readFile and
// writeFile go to the host's read and save (a page: its files, or browser storage); env has no variables (nil), as
// a page has none. exec: not in this build yet.
static V fetchList(const std::string& json, const char* fn) {  // -> the list of results
  int n = host_fetch(json.c_str());
  if (n < 0) ERR("@host %s(): this host cannot make HTTP requests", fn);
  std::string r(n, 0);
  host_take(r.data());
  return fromJs(r);
}
static V n_fetch(V req) {
  fetchRequest(req, "fetch");
  return as<List>(fetchList("[" + toJs(req) + "]", "fetch"))->v.at(0);
}
static V n_fetchAll(V reqs) {
  if (!is(reqs, K::List)) ERR("@argument fetchAll() expects a list, got %s", typeName(reqs));
  for (V v : as<List>(reqs)->v) fetchRequest(v, "fetchAll");
  return fetchList(toJs(reqs), "fetchAll");
}
static V n_readFile(V path) {
  std::string s;
  return readFile(std::string(strViewArg(path, "readFile", "the path")), s) ? box(newStr(s)) : NIL;
}
static V n_writeFile(V path, V text) {
  std::string p(strViewArg(path, "writeFile", "the path")), s(strViewArg(text, "writeFile", "the text"));
  if (!saveFile(p, &s)) ERR("@host writeFile(): could not write '%s'", p.c_str());
  return NIL;
}
static V n_env(V name) { strViewArg(name, "env", "the name"); return NIL; }
void sysInit() {
  native<n_fetch>("fetch"), native<n_fetchAll>("fetchAll"), native<n_env>("env");
  native<n_readFile>("readFile"), native<n_writeFile>("writeFile");
}
void patchStub(u32* stub, void* target) { ops.push_back((int32_t)wa::slotOf(stub)), ops.push_back((int32_t)wa::slotOf(target)); }
void jitRetire(u32* body) { Locked code(codeMu); retired.push_back(wa::slotOf(body)); }  // (fut_retire: the host)
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
  // (an interpreted body may still run on another thread, which a compiled one's module keeps alive: those wait
  // until the program is down to one isolate)
  for (u32 s : interpMode && isolates > 1 ? std::vector<u32>{} : retired) {
    ops.push_back((int32_t)s), ops.push_back(-1), freeSlots.push_back(s);
    if (interpMode) interpRetire(s);
    if (auto it = lineTabs.find(s); it != lineTabs.end()) mods[it->second.mod].refs--, lineTabs.erase(it);
  }
  if (!(interpMode && isolates > 1)) retired.clear();
  std::map<u32, u32> next = stubTo;  // where each stub leads once the queued changes are made
  for (size_t i = 0; i < ops.size(); i += 2) if (ops[i + 1] >= 0) next[ops[i]] = ops[i + 1];
  std::map<u32, u32> inBatch;        // body slot -> function index
  std::vector<u32> bodies;
  for (size_t i = 0; i < batch.size(); i++) inBatch[batch[i].slot] = (u32)i, bodies.push_back(batch[i].slot);
  std::vector<uint8_t*> fresh;       // the batch's cells (they go live after the queued changes)
  for (size_t i = 0; i < batch.size(); i++) fresh.push_back(newCell());
  std::vector<uint8_t> m{0, 'a', 's', 'm', 1, 0, 0, 0}, s;
  std::vector<std::vector<uint8_t>> interpBodies;  // interpMode: each body (locals, then code)
  if (!batch.empty()) {
    wa::uleb(s, 2 + NH), funcType(s, FUT_SIG);
    for (int h = 0; h < NH; h++) funcType(s, HSIG[h]);
    funcType(s, "i:l");
    section(m, 1, s), s.clear();
    wa::uleb(s, 5);
    name(s, "env"), name(s, "memory"), s.insert(s.end(), {2, ONE_THREAD ? 0 : 3, 0});  // (threads: shared, as fut.wasm's)
    if (!ONE_THREAD) wa::uleb(s, 65536);
    name(s, "env"), name(s, "table"), s.insert(s.end(), {1, 0x70, 0, 0});
    name(s, "env"), name(s, "sp"), s.insert(s.end(), {3, I32, 1});
    name(s, "env"), name(s, "vm"), s.insert(s.end(), {3, I32, 1});  // global 1: the running isolate's VM block
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
      for (auto [at, line] : f.k.lines) t.lines.push_back({int((interpMode ? locals.size() : code.size()) + at), line});  // + where `code` lands, below
                                                                                                    // (interpMode: offsets in the body)
      code.insert(code.end(), f.k.c.begin(), f.k.c.end());
      if (interpMode) interpBodies.push_back(locals), interpBodies.back().insert(interpBodies.back().end(), f.k.c.begin(), f.k.c.end());
    }
    if (hinted) {
      name(hints, "metadata.code.branch_hint"), wa::uleb(hints, hinted), hints.insert(hints.end(), hs.begin(), hs.end());
      section(m, 0, hints);
    }
    size_t base = m.size() + 1 + 5;  // section id + a 5-byte size (padded below) = where `code` starts
    if (!interpMode) for (auto& f : batch) for (auto& l : lineTabs[f.slot].lines) l.first += (int)base;
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
  if (interpBodies.size()) {  // no module: the bodies' slots get trampolines into the interpreter, before the queued changes
    std::vector<int32_t> all;
    interpAdd(bodies, interpBodies, all);
    all.insert(all.end(), ops.begin(), ops.end());
    tableInstall(nullptr, 0, all.data(), (u32)all.size() / 2);
  } else if (m.size() > 8 || !ops.empty()) tableInstall(m.size() > 8 ? m.data() : nullptr, (u32)m.size(), ops.data(), (u32)ops.size() / 2);
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
  wa::sleb(k.c, sym), i32(k, 0), i32(k, (int32_t)slotOf(vm->helpers[H_STALE]));
  k.c.insert(k.c.end(), {CALL_INDIRECT, 1 + H_STALE, 0, UNREACHABLE, ENDOP});
  return jitInstall(k);
}
static V rt_ffi(V* w, Ext* e) {
  if (e->stub) ffiStub(e);
  double a[8] = {};
  std::string json[8];  // any and fn arguments, as JSON text
  for (int i = 0; i < e->n; i++) {
    V v = w[i];
    char t = ffiCheck(e, i, v);
    if (t == 'a' && is(v, K::Ref)) a[i] = -(double)as<Ref>(v)->id;  // a JavaScript value (a DOM node): its id, negated
    else if (t == 'a' || t == 'f') json[i] = toJs(v), a[i] = (double)(uintptr_t)json[i].c_str();
    else a[i] = t == 's' ? (double)(uintptr_t)as<Str>(v)->s() : t == 'B' ? (double)(uintptr_t)bufBytes(v) : t == 'N' ? 0 : num(v);  // a buffer: its address in memory
  }
  int failed = 0;
  double r = (syncDepth ? host_ffi_now : host_ffi)((int)(uintptr_t)e->fn - 1, e->sig, a, e->ret, &failed);
  if (failed) {  // it threw: a fut error here, which try can catch
    std::string m((size_t)r, 0);
    host_take(m.data());
    ERR("@js %s(): %s", symName(e->sym).c_str(), m.c_str());
  }
  if (e->ret == 'a' && r <= -2) return jsRef((u32)(-r - 2));  // a JavaScript value: its id, as -2 - id
  if (e->ret == 's' || e->ret == 'a') {
    if (r < 0) return NIL;
    std::string s((size_t)r, 0);
    host_take(s.data());
    return e->ret == 'a' ? fromJs(s) : box(newStr(s));
  }
  return e->ret == 'v' ? NIL : box(e->ret == 'i' ? (double)(int32_t)(int64_t)r : r);
}
u32* ffiCode(Ext* e) {  // rt_ffi(window, e)
  using namespace wa;
  Code k = thunk({LOCAL_GET, SP, GLOBAL_SET, 0, LOCAL_GET, W}, "<extern>");
  i32(k, (int32_t)slotOf(e)), i32(k, (int32_t)slotOf(vm->helpers[H_FFI]));
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
// What each host function index names (lib, a newline, the name): a thread whose host didn't bind it (only the
// compiling thread's did) looks it up itself (fut_extern_name).
static std::vector<std::string> externNames;  // (under codeMu)
void* externLookup(const std::string& lib, const std::string& name, const char** err) {
  int i = host_extern(lib.c_str(), name.c_str());
  if (i >= 0) {
    Locked code(codeMu);
    if ((size_t)i >= externNames.size()) externNames.resize(i + 1);
    externNames[i] = lib + "\n" + name;
  }
  if (i == -2) *err = "cannot load";
  if (i == -3) *err = "pending";  // a module the host has not loaded yet
  return i < 0 ? nullptr : (void*)(uintptr_t)(i + 1);
}
// Fut code runs below this C++ frame. It lowers __stack_pointer on its way into C++ helpers and never raises it
// again, so put it back: otherwise each call from C++ starts lower, and a loop of them (expect grows) overflows.
V vmEnter(void* code, V* args) {
  void* sp = getsp();
  V r = ((V(*)(V*, void*, V, V, V, V))code)(args, sp, args[0], args[1], args[2], args[3]);
  setsp(sp);
  return r;
}

// ---------------- errors ----------------
[[noreturn]] void throwValue(V v, void*) { host_throw(v); }
void traceAdd(const std::string&) {}  // wasm traces come from the engine
std::string codeWhere(void*) { return ""; }
int hostCores() { return host_cores(); }
std::string hereWhere() {
  std::string s;
  if (int n = host_where(); n > 0) s.resize(n), host_take(s.data());
  return s;
}
static TLS jmp_buf* compileJmp;
static TLS std::string compileErr;
static TLS bool compileAtEnd;
void compileFail(const std::string& msg, bool atEnd) { compileErr = msg, compileAtEnd = atEnd, longjmp(*compileJmp, 1); }
// Where a compile error lands (no C++ exceptions: a longjmp, past destructors): close what the compile had open
// since the setjmp, as unwinding would. (The batch and queued table changes are dropped by the caller.)
struct Guard {
  jmp_buf* outer = compileJmp;
#if !ONE_THREAD
  Locked* locks = lockTop;
#endif
  int image = imageDepth();
  void landed() {
    compileJmp = outer, imageUnwind(image);
#if !ONE_THREAD
    unwindLocks(locks);
#endif
  }
};
bool guardCompile(void (*f)(void*), void* ctx, std::string* err) {  // a compile outside fut_compile (tier 2, a test body)
  jmp_buf jb;
  Guard g;
  compileJmp = &jb;
  if (setjmp(jb)) return g.landed(), err ? (void)(*err = compileErr) : batch.clear(), false;
  f(ctx), compileJmp = g.outer;
  return true;
}

// ---------------- fibers: the host switches wasm stacks; we switch the shadow stack, limit and GC roots ----------------
constexpr size_t FIBER_STACK = 256 << 10;
static char* mainLimit;
static std::map<Fiber*, void*> interpTops;  // interpMode: each switched-out context's innermost interpreted frame
ISO_LOCAL(mainLimit);
ISO_LOCAL(interpTops);
ISO_LOCAL(interpTop);
void fiberFree(Fiber* f) {
  if (interpMode) interpTops.erase(f);
  if (f->stack) free(f->stack), f->stack = nullptr, host_fiber_gone(f);
}
void fiberStack(Fiber* b) {
  b->stack = (char*)aligned_alloc(16, FIBER_STACK);
  if (!b->stack) ERR("@memory out of memory for fiber stacks");
  b->limit = b->stack + (16 << 10), b->top = (uintptr_t*)(b->stack + FIBER_STACK), b->sp = b->top;
}
static void use(Fiber* f) {
  fiberCur = f, stackTop = f ? f->top : fiberMainTop, vm->limit = f ? f->limit : mainLimit;
  if (interpMode) { auto it = interpTops.find(f); interpTop = it == interpTops.end() ? nullptr : it->second; }
}
static void enter(Fiber* f) { use(f), setsp(f ? f->sp : fiberMainSp); }  // f (null: main) runs again, on its shadow stack
static void leave() {  // save the running context's shadow stack
  if (interpMode) interpTops[fiberCur] = interpTop;
  if (Fiber* f = fiberCur) f->sp = getsp();
  else fiberMainSp = getsp(), fiberMainTop = stackTop, mainLimit = (char*)vm->limit;
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

// ---------------- isolates' contexts (isolate.cpp): each its own wasm stack, which the host suspends and resumes ----
// A context is named by its shadow stack pointer while it waits (a fresh one by the top of its stack), which no other
// context can have.
static std::unordered_map<void*, void (*)()> freshCtx;  // made, not yet started: their entry
static Mutex ctxMu;                                     // (made on one thread, started on another)
void* ctxFrame(uintptr_t* top, void (*entry)()) { Locked l(ctxMu); return freshCtx[top] = entry, top; }
void ctxSwap(void** save, void* to) {
  void* sp = getsp();
  *save = sp, host_ctx_switch(sp, to), setsp(sp);
}
EXPORT(fut_ctx_start) void ctxStart(void* top) {  // the host starts a fresh context: on its own stack, at its entry
  setsp(top);
  void (*entry)();
  {
    Locked l(ctxMu);
    auto it = freshCtx.find(top);
    entry = it->second, freshCtx.erase(it);
  }
  entry();
}
void ctxGone(void* ctx) { host_ctx_gone(ctx); }
extern u32 tryCallSlot;  // interp.cpp
struct TryBox { int64_t payload; void (*body)(void*); void* ctx; };  // payload first: try_call stores it there
static int tryTramp(TryBox* b) { return b->body(b->ctx), 0; }
bool catchFut(void (*body)(void*), void* ctx, V* err, std::string* trace) {
  if (!__atomic_load_n(&tryCallSlot, __ATOMIC_ACQUIRE)) {
    Locked code(codeMu);
    if (!tryCallSlot) __atomic_store_n(&tryCallSlot, tableTryCall(), __ATOMIC_RELEASE);
  }
  codeSync();  // (another thread may have made it)
  TryBox b{0, body, ctx};
  if (((int (*)(u32, TryBox*))(uintptr_t)tryCallSlot)(wa::slotOf((void*)tryTramp), &b) != -1) return false;
  if (err) *err = (V)b.payload;
  if (int n; trace && (n = host_last_trace()) > 0) trace->resize(n), host_take(trace->data());
  return true;
}

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
static void safepoint() {  // rt_poll: a collection the allocator asked for, then the host's events
  u32 p = __atomic_fetch_and(&vm->poll, ~(1u | 2u | 32u), __ATOMIC_RELAXED);  // (others are set by other threads, for rt_poll)
  codeSync();
  if (gcWanted()) gcCollect();
  if (p & 1) syncDepth ? void(syncPoll = true) : vm->txn ? void(txnPollLater = true) : host_poll();  // reloads and typed lines wait for transactions to end
}
static V undefFn() { ERR("@not-callable call to undefined function"); }
static void rt_hot(void* t, V* args, int n) {  // a tier-1 body reached its call count
  u32 m = 0;
  for (int i = 0; i < n; i++) m |= u32(isNum(args[i])) << i;
  tierUp(t, m);
}
void vmSwitched() { host_vm(vm), codeSync(); }
void platformInit() {
  stackTop = (uintptr_t*)&__stack_high, vm->limit = &__stack_low + (64 << 10), vmSwitched();
  vm->helpers[H_FFI] = (void*)rt_ffi, vm->helpers[H_HOT] = (void*)rt_hot;
  undefSlot = wa::slotOf(callable<undefFn>());
  jitCollect();
  for (u32 s : freeSlots) ops.push_back((int32_t)s), ops.push_back((int32_t)undefSlot);  // grown before undefFn existed
  jitCollect();
  pollHook = safepoint;
}

// ---------------- the host's entry points ----------------
static TLS std::string text;  // what the last string-returning export returned
static Compiled last;
EXPORT(fut_std) void futStd(const char* dir) { stdRoot = canonicalPath(dir); }  // the host says where std/ is
// flags: 1 verbose, 2 no tier 2 (nothing compiles after a program loads: a Cloudflare Worker can't compile code)
// 4 interpret the compiled code (interp.cpp) instead of installing it: a host that can't compile at all), 8 count steps
EXPORT(fut_init) void futInit(int flags) {  // installs the natives' adapters
  verbose = flags & 1, noTier = flags & 6, interpMode = flags & 4;
  stepsOn = flags & 8;  // count steps in all code, budget() or not (fuel.cpp)
  if (interpMode) interpInit();
  rtInit(), jitCollect(), refRelease = host_ref_gone;
}
EXPORT(fut_extern_name) const char* futExternName(int i) {  // "lib\nname" of host function i, or ""
  Locked code(codeMu);
  return (text = i >= 0 && (size_t)i < externNames.size() ? externNames[i] : ""), text.c_str();
}
EXPORT(fut_set_trycall) void futSetTryCall(u32 slot) { interpSetTryCall(slot); }  // the host's try_call (see interp.cpp)
EXPORT(fut_isolates) int futIsolates() { return isolates; }  // running now: the program's first, and those it spawned
EXPORT(fut_seed) void futSeed(double seed) { isolateSeed = (uint64_t)seed; }  // --seed n
EXPORT(fut_threads) void futThreads(int n) { isolateThreads = n; }               // --threads n
#if !ONE_THREAD
EXPORT(fut_exit_request) void futExitRequest(int code) { unwindLocks(nullptr), isolatesExit(code); }  // a worker thread's exit (or trap)
#endif
EXPORT(fut_heap) void futHeap(u32 mb) { gcRegion(size_t(mb) << 20); }  // before fut_init
void setUntimed(bool on);  // test.cpp
EXPORT(fut_untimed) void futUntimed(u32 on) { setUntimed(on); }  // a host whose clock stands still while code runs
EXPORT(fut_alloc) char* futAlloc(u32 n) { return (char*)malloc(n); }
EXPORT(fut_free) void futFree(char* p) { free(p); }
// Compile a file (text null) or REPL input; returns the chunk's slot, 0 after an error (printed), -1 if input ended early,
// -2 if it needs modules the host has not loaded yet (fut_info(3) lists them).
EXPORT(fut_compile) int futCompile(const char* path, int reload, int echo, const char* src, int tests) {
  jmp_buf jb;
  Guard g;
  compileJmp = &jb;
  if (setjmp(jb)) {
    g.landed(), batch.clear(), ops.clear();
    if (!modulesPending.empty()) return -2;  // the host loads them (fut_info(3)) and compiles again
    if (echo && compileAtEnd) return -1;
    if (tests) return reportCompileError(compileErr), 0;
    return fflush(stdout), fprintf(stderr, "%s\n", compileErr.c_str()), 0;
  }
  std::string t = src ? src : "";
  last = compile(path, reload, echo, src ? &t : nullptr, tests);
  compileJmp = g.outer;
  return (int)wa::slotOf(last.chunk);
}
// 0: the reload log, 1: the files compiled, 2: the last compile error, 3: the modules it waits for
EXPORT(fut_info) const char* futInfo(int what) {
  text = what == 0 ? last.log : what == 2 ? compileErr : "";
  if (what == 1) for (auto& f : last.files) text += f + "\n";
  if (what == 3) for (auto& m : modulesPending) text += m + "\n";
  return text.c_str();
}
EXPORT(fut_main_changed) int futMainChanged() { return last.mainChanged; }
EXPORT(fut_fn) int futFn(const char* nm) {  // a top-level fn of no arguments: its stub, else 0
  Global* g = fnNamed(intern(nm));
  return g && ((Fn*)g->o)->arity == 0 ? (int)wa::slotOf(g->stub) : 0;
}
EXPORT(fut_pin_fn) int futPinFn(const char* nm) {  // a top-level fn, pinned for fut_apply: its id, else -1
  Global* g = fnNamed(intern(nm));
  return g ? jsPin(box(g->o)) : -1;
}
EXPORT(fut_call) V futCall(int slot) { return vmCall((void*)(uintptr_t)slot); }
EXPORT(fut_retire) void futRetire(int slot) { jitRetire((u32*)(uintptr_t)slot); }
EXPORT(fut_replay_begin) void futReplayBegin(const char* log, int record) { replayBegin(log, record); }  // see replay.cpp
EXPORT(fut_replay_log) const char* futReplayLog() { return (text = replayLog()).c_str(); }
EXPORT(fut_str) const char* futStr(V v) { return (text = toStr(v)).c_str(); }
EXPORT(fut_error_line) const char* futErrorLine(V v) { return (text = errorLine(v)).c_str(); }  // with its [code]
// The program ended (failed: with an error): wait for the isolates it started (stopping them if it failed). "" or
// what fails it now: a deadlock among them, or one that failed with nothing joining it. (Suspending: call it promising.)
EXPORT(fut_isolates_finish) const char* futIsolatesFinish(int failed) { return (text = isolatesFinish(failed)).c_str(); }
EXPORT(fut_where) const char* futWhere(int slot, int off) {  // "name (file:line)" for a frame at module offset off
  Locked code(codeMu);  // (another thread's compile may be adding to them)
  auto it = lineTabs.find(slot);
  if (it == lineTabs.end() || it->second.file.empty()) return "";
  int line = 0;
  for (auto [at, l] : it->second.lines) if (at <= off) line = l;
  return (text = it->second.name + " (" + it->second.file + ":" + std::to_string(line) + ")").c_str();
}
// interpMode: the interpreted frames' trace (as fut_where gives them) when a fut exception is thrown; reset: the
// host caught one, so no interpreted frame is active
EXPORT(fut_interp_trace) const char* futInterpTraceText() { return (text = interpTrace_(futWhere)).c_str(); }
EXPORT(fut_interp_reset) void futInterpReset() { interpTop = nullptr; }
EXPORT(fut_live) u32 futLive() { return (u32)jitLive(); }
// fut test: the host calls the top level, then each test's code, catching errors (see test.cpp for the rest)
EXPORT(fut_tests_begin) void futTestsBegin(int json, const char* filter) { testsBegin({json != 0, filter}); }
EXPORT(fut_test_count) int futTestCount() { return (int)last.tests.size(); }
EXPORT(fut_test_name) const char* futTestName(int i) { return last.tests[i].name.c_str(); }
EXPORT(fut_test_start) int futTestStart(int i) { return (int)wa::slotOf(testStart(last.tests[i])); }
EXPORT(fut_test_finish) void futTestFinish(int i, int errored, const char* err) { testFinish(last.tests[i], errored, err); }
EXPORT(fut_test_top_error) void futTestTopError(const char* err) { reportTopError(err); }
EXPORT(fut_tests_end) int futTestsEnd() { return testsEnd(); }
// The last compile's interfaces: mode 0 reports them as text, 1 as JSON, 2 writes them (fut api); the number
EXPORT(fut_explain) int futExplain(const char* code) { int r = explain(*code ? code : nullptr); return fflush(stdout), fflush(stderr), r; }
EXPORT(fut_api) int futApi(int mode, const char* program) {
  return mode == 2 ? writeInterfaces(last, program) : reportInterfaces(checkInterfaces(last), mode == 1, program);
}
EXPORT(fut_poll) u32* futPoll() { return &vm->poll; }
// fut map: the declarations of the last compile; the number shown
EXPORT(fut_map) int futMap(int pub, int json, const char* only, const char* program) { return printMap(last, pub, json, only, program); }
// A fut function JavaScript holds (pinned as id), called with args (a JSON array): its result as JSON text. An
// error in it propagates as a fut exception. Called directly while an extern call is in progress (a callback, as
// Array.map makes), else at a safepoint, through a promising wrapper.
static TLS std::string applied;  // (a thread's: fut functions run on several at once)
EXPORT(fut_sync) void futSync(int d) {  // the host calls fut code synchronously (1), and it returned (-1)
  syncDepth += d;
  if (!syncDepth && syncPoll) syncPoll = false, __atomic_fetch_or(&vm->poll, 1u, __ATOMIC_RELAXED);
}
EXPORT(fut_apply) const char* futApply(int id, const char* args) {
  V f = jsPinned(id);
  if (f == NIL) ERR("@js a fut function called from JavaScript after it was released");
  V a = fromJs(args);
  std::vector<V>& v = as<List>(a)->v;
  Fn* fn = is(f, K::Clo) ? as<Clo>(f)->fn : as<Fn>(f);
  int want = fn->arity - is(f, K::Clo);
  if ((int)v.size() > want) v.resize(want);  // JavaScript passes extra arguments freely (index, array, ...)
  while ((int)v.size() < want) v.push_back(NIL);
  if (want > 7) ERR("@js a function called from JavaScript takes at most 7 parameters");
  V args8[8] = {};
  int k = 0;
  if (is(f, K::Clo)) args8[k++] = f;
  for (V x : v) args8[k++] = x;
  volatile V keep = a;  // the arguments stay alive while it runs
  V r = vmEnter(fn->code, args8);
  (void)keep;
  return (applied = toJs(r)).c_str();
}
EXPORT(fut_unpin) void futUnpin(int id) { jsUnpin(id); }
// Transactions, for the host: a hot reload's top level and reload() hook run inside one (see hostPoll in fut.js),
// and an error that reaches the host rolls back those opened since the call began.
EXPORT(fut_txn_depth) u32 futTxnDepth() { return vm->txn; }
// Code the host runs (top level, main, a REPL line, a reload) is unchecked and starts with no handlers, whatever the
// code it interrupts may do; leaving puts that code's effect state back (see effEnter).
EXPORT(fut_eff_enter) u32 futEffEnter() { return effEnter(); }
EXPORT(fut_eff_leave) void futEffLeave(u32 depth) { effLeave(depth); }
EXPORT(fut_txn_begin) void futTxnBegin() { txnBegin(); }
EXPORT(fut_txn_commit) void futTxnCommit() { txnCommit(); }
EXPORT(fut_txn_unwind) void futTxnUnwind(u32 depth) { txnUnwind(depth); }
EXPORT(fut_reload_failed) void futReloadFailed() { rollbackReload(last.newVars); }
#if !ONE_THREAD
// A reload with isolates running (hostPoll in fut.js), as main.cpp's hotReload: stop the others (1 if there were
// any), patch, copy the globals to them (sync), have each run the new top level and reload() in a transaction of its
// own (parts: "" or the first error), then all commit or all roll back (verdict).
EXPORT(fut_isolates_stop) int futIsolatesStop() { return isolatesStop(); }
EXPORT(fut_isolates_sync) void futIsolatesSync() { isolatesSync(); }
EXPORT(fut_isolates_parts) const char* futIsolatesParts(u32 chunk, u32 hook) {
  return (text = isolatesParts((void*)(uintptr_t)chunk, hook ? (void*)(uintptr_t)hook : nullptr)).c_str();
}
EXPORT(fut_isolates_verdict) void futIsolatesVerdict(int commit) { isolatesVerdict(commit); }
#endif
