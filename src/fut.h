// fut: a tiny class-based language that JIT-compiles straight to arm64 and hot-reloads by patching machine code.
#pragma once
#include <bit>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <initializer_list>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using V = uint64_t;
using u32 = uint32_t;
#ifdef __wasm__
constexpr bool WASM = true;  // the WebAssembly build (wasm.cpp, gen_wasm.h)
#else
constexpr bool WASM = false;
#endif

// ---- NaN-boxed values: doubles are themselves; everything else lives in the quiet-NaN space ----
constexpr V QNAN = 0x7ffc000000000000, SIGN = 1ull << 63, PTR = QNAN | SIGN, ADDR = 0xffffffffffff;
constexpr V NIL = QNAN, FALSE_ = QNAN | 1, TRUE_ = QNAN | 2, END = QNAN | 3, ABSENT = QNAN | 4;  // END/ABSENT: internal
inline bool isNum(V v) { return (v & QNAN) != QNAN; }
inline bool isObj(V v) { return (v & PTR) == PTR; }
inline bool truthy(V v) { return (v & ~1ull) != QNAN; }  // nil and false are the only falsy values
inline double num(V v) { return std::bit_cast<double>(v); }
inline V box(double d) { return std::bit_cast<V>(d); }
inline V boxb(bool b) { return b ? TRUE_ : FALSE_; }

// ---- heap objects (no vtables: JIT code reads Inst fields at fixed offsets) ----
enum class K : uint8_t { Str, List, Fn, Class, Inst, Map, Clo, Fiber, Ref, Free = 0xff };
// For an Inst, `nslots` and `cid` (its class id) are in the header and are zero for every other kind, so one
// 64-bit compare of the header checks kind, class and slot count together (the field inline cache).
// `zero` stays 0 for that compare (mark bits live in the GC's side bitmap, not here).
struct Obj { K kind; uint8_t zero; uint16_t nslots; u32 cid; };
struct Str : Obj {
  static constexpr K KIND = K::Str;
  u32 len, hash;
  char* s() { return (char*)(this + 1); }  // bytes stored inline after the header
  std::string_view sv() { return {s(), len}; }
};
struct List : Obj { static constexpr K KIND = K::List; std::vector<V> v; };
struct Fn : Obj { static constexpr K KIND = K::Fn; void* code; int arity, name, op = 0; };  // op: an effect operation (effects.cpp)
inline u32 classCount;
struct Class : Obj {
  static constexpr K KIND = K::Class;
  // Read by JIT constructors (see reshape): a new instance's header word, the VM offset of the run its
  // size class allocates from, and its cell size.
  uint64_t ihdr = 0;
  u32 irun = 0, isize = 0;
  int64_t name;
  u32 id = ++classCount, initEpoch = 0;
  Fn* init = nullptr;  // find(S_init), cached until the next reload
  Class* super;
  std::vector<std::pair<int, Fn*>> methods;  // few per class, and looked up only on inline-cache misses
  std::vector<int> layout;                    // field syms; the index is the slot, shared by all instances
  std::vector<std::pair<int, V>> fields;      // fields declared in the class body (`var hp = 10`) and their defaults
  struct FieldType { int sym; uint64_t type; int site; };
  std::vector<FieldType> ftypes;              // declared fields with a type (`var hp: num = 10`): checked on every store
  Fn* find(int s) {
    for (Class* c = this; c; c = c->super)
      for (auto [m, f] : c->methods) if (m == s) return f;
    return nullptr;
  }
  // A class that declares fields, or inherits declared ones, is closed: it has exactly those fields. A field
  // never assigned reads as its default. Open classes (no declarations) get fields by assignment.
  bool closed() { for (Class* c = this; c; c = c->super) if (!c->fields.empty()) return true; return false; }
  const FieldType* typed(int s) {
    for (Class* c = this; c; c = c->super)
      for (auto& t : c->ftypes) if (t.sym == s) return &t;
    return nullptr;
  }
  const V* declared(int s) {
    for (Class* c = this; c; c = c->super)
      for (auto& [f, d] : c->fields) if (f == s) return &d;
    return nullptr;
  }
};
struct Inst : Obj {  // slots point at the inline area sized when constructed, or at a malloc'd one after growth
  static constexpr K KIND = K::Inst;
  Class* cls;
  V* slots;
  V* inl() { return (V*)(this + 1); }
};
// Entries in insertion order, plus an open-addressing index: entry + 1, 0 empty, ~0u deleted (`used` counts both).
struct Map : Obj { static constexpr K KIND = K::Map; std::vector<std::pair<V, V>> e; std::vector<u32> idx; u32 used = 0; };
struct Ref : Obj { static constexpr K KIND = K::Ref; u32 id; };  // a JavaScript value fut code holds (wasm)
struct Clo : Obj { static constexpr K KIND = K::Clo; Fn* fn; u32 n; V* env() { return (V*)(this + 1); } };
// A fiber runs a function on a stack of its own; `yield` switches back to whoever resumed it.
struct Fiber : Obj {
  static constexpr K KIND = K::Fiber;
  V fn, val;  // the function, then the value passed across the last switch
  void* sp;   // saved stack pointer while switched out
  char *stack, *limit;
  uintptr_t* top;
  jmp_buf* err;  // its error handler, while switched out
  void* handler; // its innermost try, while switched out
  Fiber* resumer;
  uint64_t allowed;  // its effect state while switched out (effects.cpp): what it may perform, who narrowed it,
  V hstack;          // and its innermost handler
  u32 who;
  int state, fail, idx;  // fail: an error ended it (its resumer re-raises it); idx: in the live-fiber list
};
enum { FIB_NEW, FIB_SUSPENDED, FIB_RUNNING, FIB_DONE };
extern Fiber* fiberCur;                       // the running fiber (null: the main stack)
extern void* fiberMainSp;                     // the main stack while a fiber runs (for the GC)
extern uintptr_t* fiberMainTop;
void fiberFree(Fiber*);      // release its stack (the platform half: jit.cpp / wasm.cpp, as are the next four)
void fiberStack(Fiber*);     // give a new fiber its stack
void fiberRun(Fiber*, bool start);  // switch to it; returns when it yields or ends
void fiberYield(Fiber*);     // switch from it back to its resumer; returns when it is resumed
[[noreturn]] void restartMain();    // unwind to the driver, which reruns main()
V fiberCall(Fiber*);         // call its function with the first value sent
V newFiber(V fn);
V fiberResume(V f, V v);
V rt_yield(V v);
void sleepFor(double seconds);
double now();  // seconds, monotonic
constexpr int LIST_V = offsetof(List, v);  // its std::vector, which JIT code reads: the elements, then the end or the count
inline bool vecCount;                      // the second word is the element count (wasm's libc++), not an end pointer
constexpr int INST_SLOTS = offsetof(Inst, slots), CLO_ENV = sizeof(Clo), CLS_IHDR = offsetof(Class, ihdr), CLS_IRUN = offsetof(Class, irun);

inline Obj* obj(V v) { return (Obj*)(v & ADDR); }
inline V box(Obj* o) { return (V)o | PTR; }
inline bool is(V v, K k) { return isObj(v) && obj(v)->kind == k; }
template <class T> T* as(V v) { return (T*)obj(v); }
// Small objects are rounded up to a size class; each 64KB page holds cells of one class. Bigger ones get pages of their own.
inline constexpr u32 sizeClasses[] = {16, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768, 1024, 1536, 2048, 4096, 8192, 16384};
constexpr int NCLS = sizeof sizeClasses / sizeof *sizeClasses;
constexpr int sizeClass(size_t bytes) { int c = 0; while (c < NCLS && sizeClasses[c] < bytes) c++; return c; }  // NCLS: large
void* gcAlloc(size_t bytes);
void gcOwn(Obj*);  // o holds malloc'd memory: finalize it when the GC finds it dead
template <class T> T* make(size_t extra = 0) {
  T* o = new (gcAlloc(sizeof(T) + extra)) T();
  o->kind = T::KIND;
  if constexpr (T::KIND == K::List || T::KIND == K::Map || T::KIND == K::Class || T::KIND == K::Fiber || T::KIND == K::Ref) gcOwn(o);
  return o;
}
Str* newStr(std::string_view, std::string_view = {});
void gcInit();
extern uintptr_t* stackTop;

// ---- symbols: keywords are interned first so `sym < NKW` means keyword ----
#define KEYWORDS(X) X(var) X(fn) X(class) X(if) X(else) X(while) X(for) X(in) X(return) X(break) X(continue) \
  X(true) X(false) X(nil) X(self) X(super) X(import) X(extern) X(yield) X(try) X(catch) X(throw)
#define SYM_ENUM(k) K_##k,
#define SYM_STR(k) #k,
#define EXTRA_SYMS(X) X(init) X(main) X(push) X(pop) X(len) X(has) X(remove) X(keys) X(values) X(reload) X(resume) X(send) X(done) X(as) X(test) X(expect) X(throws) X(prints) X(pub) X(override) X(atomic) X(effect) X(handle) X(with)
#define SYM_S(k) S_##k,
enum { KEYWORDS(SYM_ENUM) NKW, S_FIRST = NKW - 1, EXTRA_SYMS(SYM_S) };
int intern(std::string_view);
const std::string& symName(int);

// ---- the VM block pinned in x28; JIT code reaches helpers and globals with one ldr ----
enum Helper { H_POLL, H_STACK, H_BINOP, H_GETF, H_SETF, H_GETI, H_SETI, H_PREP, H_LIST, H_ITER, H_NUMCHK, H_UNDEF,
              H_MAP, H_CLOSURE, H_STALE, H_ECHO, H_FFI, H_YIELD, H_HOT, H_THROW, H_CAPSET, H_TXUNWIND, H_TYPECHK, H_EFFECT, NH };
enum Op { O_ADD, O_SUB, O_MUL, O_DIV, O_MOD, O_EQ, O_NE, O_LT, O_LE, O_GT, O_GE, O_BAND, O_BOR, O_BXOR, O_SHL, O_SHR };
constexpr int MAXG = 1 << 16;
// Per-site inline cache. key: the receiver's header word (methods compare only its class id); val: slot or code.
struct alignas(16) IC { uint64_t key; uint64_t val; };
constexpr int MAXIC = 1 << 19;
// The free cells allocation is bumping through, per size class: JIT code allocates with cur += size while
// cur + size <= end. runs[NCLS] stays empty (0, 0): constructors of instances too big for a class point at it.
struct Run { uintptr_t cur, end; };
// handler: the innermost active `try` (a record in its JIT frame: prev, sp, fp, x26, handler pc, txn at entry).
// txn: how many transactions are open (see txn.cpp); JIT code reads it (arm64: at +4).
// allowed, who, hstack: the effects code may perform now, the declaration that narrowed them, the innermost
// handler (effects.cpp). Functions that declare effects read and write the first two; try records save all three.
#ifdef __wasm__  // limit, then poll: one i64 load at &limit tests both on a function's entry (gen_wasm.h)
struct VM { void* limit; u32 poll; void* handler; u32 txn; void* helpers[NH]; Run runs[NCLS + 1]; uint64_t allowed; V hstack; u32 who, pad; V globals[MAXG]; IC ics[MAXIC]; };
#else
struct VM { u32 poll, txn; void* limit; void* handler; void* helpers[NH]; Run runs[NCLS + 1]; uint64_t allowed; V hstack; u32 who, pad; V globals[MAXG]; IC ics[MAXIC]; };
#endif
extern VM vm;
constexpr int HOFF(int h) { return 24 + 8 * h; }
constexpr int ROFF(int c) { return 24 + 8 * NH + 16 * c; }
constexpr int EOFF = ROFF(NCLS + 1);  // vm.allowed; vm.hstack at +8, vm.who at +16
constexpr int GOFF(int g) { return EOFF + 24 + 8 * g; }
constexpr u32 VM_HANDLER = 16;
constexpr u32 IOFF(int i) { return (GOFF(MAXG) + 15) / 16 * 16 + 16 * i; }
int newIC(bool set = false);  // set: a field store's, which must miss while a transaction is open
extern u32 icEpoch;  // bumped whenever method tables may have changed
void clearICs();
void reshape(Class*);  // after its layout grew: what JIT constructors allocate for it

struct Global { int slot = -1; char kind = 'v'; u32* stub = nullptr; Obj* o = nullptr; bool inited = false; };  // kind: v f c n(ative)
extern std::deque<Global> globals;  // by symbol (slot -1: not declared); a deque, so references survive growth
extern std::vector<V> consts;       // GC roots forever
Global& global(int sym);
Global* findGlobal(int sym);
template <class C> auto& growAt(C& v, size_t i) { return v.size() <= i ? v.resize(i + 1), v[i] : v[i]; }  // v[i], growing v to fit

// ---- JIT ----
#if __wasm__
using Word = uint8_t;
#else
using Word = u32;
#endif
struct Code { std::vector<Word> c; std::vector<std::pair<int, void*>> rel; std::vector<std::pair<int, int>> lines; std::string name, file;
             std::vector<uint8_t> wlocals, relKind; std::vector<std::pair<int, uint8_t>> hints; };  // wasm: extra locals, rel kinds, branch hints
u32* jitInstall(Code&);
struct JitBatch { JitBatch(); ~JitBatch(); };  // hold code writable across many writes; one icache flush at the end
void jitRetire(u32* body);     // body was replaced; free it once no frame returns into it
void jitCollect();
void jitForked();  // in a child after fork(): put this thread's view of JIT memory back in execute mode
size_t jitLive();  // bytes of machine code currently allocated
void jitShiftLines(u32* body, int delta);
u32* newStub();
void patchStub(u32* stub, void* target);
u32* staleThunk(int sym);
void tierUp(void* tier, u32 numArgs);  // compile a hot function again (numArgs: which arguments were numbers)
extern bool verbose;
struct Ext { void* fn; int n, sym; char ret, sig[8]; };  // an extern fn. sig/ret: d(ouble) i(nt) l(ong) p(ointer) s(tring) v(oid)
u32* ffiThunk(void* fn, const std::string& sig, char ret, int sym);  // code calling C function fn (see extern)
u32* ffiCode(Ext*);            // the platform's thunk for it
char ffiCheck(Ext*, int i, V v);  // argument i's type letter; raises if v does not fit it
V vmCall(void* code, std::initializer_list<V> args = {});
V vmEnter(void* code, V* args);  // args: 8 values
extern std::string* asmListing;

struct TestCase { std::string name, file, pending; int line = 0; void* code = nullptr; };  // a `test "name" { }` block
// An interface file: the `pub` declarations of a file outside any package, or a package's surface (its root file's,
// and those it re-exports), with the package's name and version.
struct ApiUnit { std::string file, api, label; std::vector<std::string> parts; std::string pkg, version; };
struct Compiled {
  void* chunk = nullptr;
  bool mainChanged = false;
  std::string log;
  std::vector<std::string> files;
  std::vector<std::pair<std::string, std::string>> sources;  // each file compiled and its text
  std::vector<TestCase> tests;
  std::vector<int> newVars;  // top-level vars it initialized for the first time
  std::vector<ApiUnit> apis;  // the interface files its sources have
  std::vector<std::string> packages;  // directories of the packages it uses (fut map)
};
struct CompileError : std::runtime_error {  // atEnd: the source ended too early (the REPL reads more)
  bool atEnd;
  CompileError(const std::string& m, bool end) : std::runtime_error(m), atEnd(end) {}
};
// tests: compile `test` blocks into Compiled::tests (otherwise they are skipped)
Compiled compile(const std::string& path, bool reload, bool echo, const std::string* text = nullptr, bool tests = false);
bool guardCompile(void (*f)(void*), void* ctx, std::string* err = nullptr);  // f(ctx); false if it failed with a compile error (*err)
// A hot reload whose top level or reload() failed: forget the vars it added and compile the sources it replaced
// again, without running them (its data was rolled back already). False if that failed (*err).
bool revertReload(const std::vector<int>& newVars, std::string* err);
[[noreturn]] void compileFail(const std::string& msg, bool atEnd);  // natively throws CompileError; in wasm, longjmps
bool readFile(const std::string& path, std::string& out);
bool saveFile(const std::string& path, const std::string* text);  // null text: remove the file
std::string canonicalPath(const std::string& path);
void* externLookup(const std::string& lib, const std::string& name, const char** err);  // an extern's function
void rtInit();
void platformInit();  // the JIT or wasm half of rtInit (jit.cpp / wasm.cpp)
// Builtins. native<f>("name") registers a C++ function V f(V...); method<f>(kind, sym) one taking the receiver first.
// In wasm every callable takes its arguments as a window in memory, so f is wrapped to that form.
template <class R, class... A> constexpr int nargs(R (*)(A...)) { return sizeof...(A); }
void* futAdapter(void* cpp);  // wasm: an adapter giving a V f(V* window) the fut calling convention
template <auto f> void* callable() {
#if __wasm__
  return futAdapter((void*)+[](V* w) -> V { return [&]<size_t... I>(std::index_sequence<I...>) { return f(w[I]...); }(std::make_index_sequence<nargs(f)>{}); });
#else
  return (void*)f;
#endif
}
void native(const char* name, void* f, int arity);
void method(K k, int sym, void* f, int arity);
template <auto f> void native(const char* name) { native(name, callable<f>(), nargs(f)); }
template <auto f> void method(K k, int sym) { method(k, sym, callable<f>(), nargs(f)); }
void gfxInit(), gfxPump();  // window, drawing, input (gfx.cpp)
std::string toStr(V, bool quote = false);
const char* typeName(V);
extern void (*pollHook)();
extern jmp_buf* errJmp;
extern std::string* outSink;  // when set, print() appends here instead of writing to stdout (a test's output)
extern std::string* errSink;  // when set, an uncaught error's message and trace go here instead of to stderr

// ---- tests (test.cpp): `expect` sites, and the runner behind `fut test` ----
struct ExpectSite { std::string file, text, left, right; int line = 0, op = -1; bool throws = false; };  // op < 0: truthiness
int expectSite(ExpectSite);  // its number, passed to the expect@ native
void testInit();
V binopValue(V a, V b, int op);  // a op b, as the language computes it (raises on bad operands)
struct TestRun { bool json = false; std::string filter; double timeout = 10; };
extern std::string mainFile;  // the program's file: run() finds relative files next to it
void mapPut(Map*, std::string_view key, V v);
struct ProcResult { int code = 0; std::string out, err; bool timedOut = false; };
// Run this fut executable with args, then file (or a temporary file holding *source); false if it could not start.
int reap(int pid, std::vector<std::pair<int, std::string*>> pipes, double timeout, bool& timedOut);  // native only
bool runProcess(const std::vector<std::string>& args, const std::string& file, const std::string* source, double timeout, ProcResult&);
void testsBegin(const TestRun&);
void* testStart(const TestCase&);                         // the code to call, or null (pending or filtered out: reported)
void testFinish(const TestCase&, bool errored, const std::string& err);  // after a call: err = message + trace
std::string testReport(const TestCase&, bool errored, const std::string& err, char& code);  // testFinish's two halves:
void testRecord(char code, const std::string& text);                                         // (an isolated test's child reports)
int testsEnd();
void reportCompileError(const std::string& msg);          // fut test: in the chosen format
void reportTopError(const std::string& msg);
// ---- transactions (txn.cpp): atomic { } and hot reloads ----
void txnInit();
void txnBegin();
void txnCommit();
void txnAbort();                // roll back the innermost
void txnUnwind(u32 depth);      // roll back until depth are open
void txnTouch(Obj*);            // o is about to change: save it, unless the innermost transaction has already
inline void touch(Obj* o) { if (vm.txn) txnTouch(o); }
bool txnOpenIn(Fiber*);         // does f have a transaction open? (it may not yield)
bool txnAtomic();               // is an atomic block open? (io can't be undone there)
void txnMark(void (*mark)(V));  // GC roots: what rollbacks would put back
extern bool txnPollLater;       // a reload or typed line waits for the transactions to end
int globalCount();              // global slots in use

// ---- types (types.cpp): optional annotations, checked at run time where they are written ----
// A type is a kind, TY_OPT if nil is allowed too (`str?`), and for a class its symbol (bits 8..).
enum : uint64_t { TY_ANY = 1, TY_NUM, TY_STR, TY_BOOL, TY_LIST, TY_MAP, TY_FN, TY_CLS, TY_OPT = 16 };
constexpr uint64_t tyClass(int sym) { return TY_CLS | uint64_t(sym) << 8; }
bool typeAccepts(uint64_t type, V v);
int typeSite(const std::string& what, const std::string& type);  // what a check names when it fails: its number
[[noreturn]] void typeFail(int site, V v, void* ra);
void typesInit();

// ---- effects (effects.cpp): declared effect sets (`fn f() ! io, clock`), checked at run time, and handlers ----
// vm.allowed holds the effects code may perform now in its low 44 bits (all set: unchecked) and, above them, a
// level that each narrowing (a declared function, an atomic block) raises by one. vm.who names the narrowing.
constexpr int EFF_LEVEL = 44;
constexpr uint64_t EFF_IO = 1, EFF_CLOCK = 2, EFF_RANDOM = 4, EFF_YIELD = 8, EFF_BITS = (1ull << EFF_LEVEL) - 1;
constexpr u32 WHO_ATOMIC = ~0u;  // vm.who inside an atomic block (it takes io away)
enum { OP_YIELD = 1, OP_EXTERN = 2 };  // operations with fixed ids; builtins and user effects' get theirs at startup / compile time
int effSite(const std::string& fn, uint64_t mask, const std::string& text);  // a declaring function: what errors name
uint64_t effBit(int sym);                     // a user effect's bit (0: too many effects)
int effOpId(const std::string& qualified);    // "Log.log" -> its operation id, kept for good
Fn* effOpFn(int op);                          // the Fn a perform calls (rt_prep resolves it, see effPrep)
struct EffOpDef { int op, arity; std::vector<uint64_t> ptypes; uint64_t rtype; std::vector<int> psites; int rsite; };
void effDefine(int sym, const std::string& name, const std::vector<EffOpDef>& ops);  // at commit: an `effect` and its operations
int effOpArity(int op);                       // -1: not an operation
uint64_t effOpEffect(int op);
const std::string& effOpName(int op);
int effBuiltinOp(int sym);                    // print, clock, ...: the builtin's operation id, else 0
void* effPrep(Fn* f, V* w, uint64_t n);       // rt_prep, calling an operation: a handler, the builtin, or an error
V effYield(V v, bool* handled);               // `yield`: a handler's answer (handled), else checks it may yield
struct EffState { uint64_t allowed; V hstack; u32 who; };
EffState effSave();
void effLoad(const EffState&);
u32 effEnter();             // a driver runs code (top level, main, REPL line, reload): unchecked, no handlers
void effLeave(u32 depth);   // and back
void effMark(void (*mark)(V));
void effectsInit();

// ---- packages (pkg.cpp): a directory with a fut.pkg manifest ----
struct ForeignMod { std::string kind, name, target; int line; };  // kind: js, wasm or c; target: its file or library
struct PackageDep { std::string name, dir; int line; };
struct Package {
  std::string dir, manifest, name, version, root;  // root: the file whose pub names are the package's interface
  std::vector<PackageDep> deps;
  std::vector<ForeignMod> mods;
  const ForeignMod* module(const std::string& name, bool native) const;  // the one for this target, else any, else null
};
extern std::vector<std::string> modulesPending;  // wasm: modules ("js:/path") a compile waits for the host to load
const Package* packageOf(const std::string& file);  // the nearest fut.pkg at or above the file's directory, or null
const Package* packageAt(const std::string& dir);   // dir's own fut.pkg, or null
void packagesReset();                               // each compile reads the manifests again
bool parseVersion(const std::string& v, int out[3]);

// ---- public interfaces (api.cpp): `pub` declarations, kept in .api files ----
struct ApiIssue {
  std::string file, api, status;           // status: "missing" (no .api yet), "changed", "stale" (nothing is pub
                                           // now), "breaking" (declarations went, and the version did not move)
  std::vector<std::string> removed, added;  // declarations, one per line
  std::string want;                         // the .api text it should have
  std::string version, needs;               // breaking: the version, and the least it must become
};
std::string interfaceOf(const std::string& path, const std::string& text);  // "" if it declares nothing pub
std::string apiPath(const std::string& file);                               // geom.fut -> geom.api
std::vector<ApiIssue> checkInterfaces(const Compiled&);
int reportInterfaces(const std::vector<ApiIssue>&, bool json, const std::string& program);  // fut test; the number
int writeInterfaces(const Compiled&, const std::string& program);                           // fut api; the number
int printMap(const Compiled&, bool pubOnly, bool json, const std::string& only, const std::string& program);  // fut map

// ---- values crossing into JavaScript (jsval.cpp): JSON text, lists and maps copied; a function fut code passes
// is pinned (an id JavaScript calls it by); a JavaScript value that is not data comes back as a Ref (by its id) ----
std::string toJs(V v);                // raises for what can't cross (an instance, a map with non-string keys, ...)
V fromJs(const std::string& json);
V jsPinned(int id);                   // the function pinned as id (nil if none)
void jsUnpin(int id);                 // JavaScript dropped it
void jsMark(void (*mark)(V));         // GC roots: what JavaScript holds, and what fromJs is building
void refFree(Ref*);                   // the GC found a Ref dead: JavaScript may let the value go
extern void (*refRelease)(u32 id);    // the host's side of that (wasm)

[[noreturn]] void throwValue(V v, void* ra);  // to the innermost try, else report it (ra: where, for the trace)
[[noreturn]] void rtError(void* ra, const char* fmt, ...);
#if __wasm__
#define RA() nullptr  // wasm traces come from the engine's stack, not return addresses
#else
#define RA() __builtin_return_address(0)
#endif
#define ERR(...) rtError(RA(), __VA_ARGS__)

// ---- arm64 encoders (checked against the system assembler) ----
namespace a64 {
constexpr u32 SP = 31, EQ = 0, NE = 1, HS = 2, LO = 3, MI = 4, PL = 5, HI = 8, LS = 9, GE = 10, LT = 11, GT = 12;
constexpr u32 ldr(u32 t, u32 n, u32 off) { return 0xf9400000 | (off / 8) << 10 | n << 5 | t; }
constexpr u32 str(u32 t, u32 n, u32 off) { return 0xf9000000 | (off / 8) << 10 | n << 5 | t; }
constexpr u32 ldrw(u32 t, u32 n, u32 off) { return 0xb9400000 | (off / 4) << 10 | n << 5 | t; }
constexpr u32 ldrb(u32 t, u32 n) { return 0x39400000 | n << 5 | t; }
constexpr u32 ldrreg(u32 t, u32 n, u32 m) { return 0xf8607800 | m << 16 | n << 5 | t; }  // ldr t, [n, m, lsl #3]
constexpr u32 strreg(u32 t, u32 n, u32 m) { return 0xf8207800 | m << 16 | n << 5 | t; }
constexpr u32 strpost(u32 t, u32 n, u32 imm) { return 0xf8000400 | (imm & 0x1ff) << 12 | n << 5 | t; }  // str t, [n], #imm
constexpr u32 addxw(u32 d, u32 n, u32 m) { return 0x8b204000 | m << 16 | n << 5 | d; }                 // add d, n, w(m), uxtw
constexpr u32 addlsr(u32 d, u32 n, u32 m, u32 sh) { return 0x8b400000 | m << 16 | sh << 10 | n << 5 | d; }  // add d, n, m, lsr #sh
constexpr u32 orrptr(u32 d, u32 n) { return 0xb24e3400 | n << 5 | d; }                                 // orr d, n, #PTR
constexpr u32 stp_off(u32 a, u32 b, u32 n, int off) { return 0xa9000000 | u32((off / 8) & 0x7f) << 15 | b << 10 | n << 5 | a; }
constexpr u32 ldp_off(u32 a, u32 b, u32 n, int off) { return 0xa9400000 | u32((off / 8) & 0x7f) << 15 | b << 10 | n << 5 | a; }
constexpr u32 mov(u32 d, u32 m) { return 0xaa0003e0 | m << 16 | d; }
constexpr u32 addi(u32 d, u32 n, u32 imm) { return 0x91000000 | imm << 10 | n << 5 | d; }
constexpr u32 addhi(u32 d, u32 n, u32 imm) { return 0x91400000 | imm << 10 | n << 5 | d; }  // add d, n, #imm, lsl #12
constexpr u32 subi(u32 d, u32 n, u32 imm) { return 0xd1000000 | imm << 10 | n << 5 | d; }
constexpr u32 subhi(u32 d, u32 n, u32 imm) { return 0xd1400000 | imm << 10 | n << 5 | d; }
constexpr u32 movz(u32 d, u32 imm, u32 hw) { return 0xd2800000 | hw << 21 | imm << 5 | d; }
constexpr u32 movk(u32 d, u32 imm, u32 hw) { return 0xf2800000 | hw << 21 | imm << 5 | d; }
constexpr u32 stp_pre(u32 a, u32 b) { return 0xa9bf0000 | b << 10 | SP << 5 | a; }   // stp a, b, [sp, #-16]!
constexpr u32 ldp_post(u32 a, u32 b) { return 0xa8c10000 | b << 10 | SP << 5 | a; }  // ldp a, b, [sp], #16
constexpr u32 ADR = 0x10000000;  // adr x, label (patched like a conditional branch)
constexpr u32 blr(u32 n) { return 0xd63f0000 | n << 5; }
constexpr u32 br(u32 n) { return 0xd61f0000 | n << 5; }
constexpr u32 RET = 0xd65f03c0, B = 0x14000000, BL = 0x94000000, BRK = 0xd4200000;
constexpr u32 bcond(u32 c) { return 0x54000000 | c; }
constexpr u32 cbzw(u32 t) { return 0x34000000 | t; }
constexpr u32 cmp(u32 n, u32 m) { return 0xeb00001f | m << 16 | n << 5; }
constexpr u32 cmpw(u32 n, u32 m) { return 0x6b00001f | m << 16 | n << 5; }
constexpr u32 cmpi(u32 n, u32 imm) { return 0xf100001f | imm << 10 | n << 5; }
constexpr u32 cmpwi(u32 n, u32 imm) { return 0x7100001f | imm << 10 | n << 5; }
constexpr u32 cmpsp(u32 m) { return 0xeb2063ff | m << 16; }
constexpr u32 andnot1(u32 d, u32 n) { return 0x927ff800 | n << 5 | d; }  // and d, n, #~1
constexpr u32 andaddr(u32 d, u32 n) { return 0x9240bc00 | n << 5 | d; } // and d, n, #0xffffffffffff
constexpr u32 strw(u32 t, u32 n, u32 off) { return 0xb9000000 | (off / 4) << 10 | n << 5 | t; }
constexpr u32 bicsz(u32 n, u32 m) { return 0xea20001f | m << 16 | n << 5; }                      // bics xzr, n, m (n & ~m)
constexpr u32 lsri(u32 d, u32 n, u32 sh) { return 0xd340fc00 | sh << 16 | n << 5 | d; }          // lsr d, n, #sh
constexpr u32 orrlsl(u32 d, u32 n, u32 m, u32 sh) { return 0xaa000000 | m << 16 | sh << 10 | n << 5 | d; }  // orr d, n, m, lsl #sh
constexpr u32 mvn(u32 d, u32 m) { return 0xaa2003e0 | m << 16 | d; }
constexpr u32 tstq(u32 n) { return 0xf24e301f | n << 5; }    // tst n, #QNAN
constexpr u32 tstptr(u32 n) { return 0xf24e341f | n << 5; }  // tst n, #PTR
constexpr u32 cinc(u32 d, u32 n, u32 c) { return 0x9a800400 | n << 16 | (c ^ 1) << 12 | n << 5 | d; }
constexpr u32 fmovdx(u32 d, u32 n) { return 0x9e670000 | n << 5 | d; }
constexpr u32 fmovxd(u32 d, u32 n) { return 0x9e660000 | n << 5 | d; }
constexpr u32 fcmp(u32 n, u32 m) { return 0x1e602000 | m << 16 | n << 5; }
constexpr u32 fcmpz(u32 n) { return 0x1e602008 | n << 5; }                            // fcmp d, #0.0
constexpr u32 fcvtzu(u32 d, u32 n) { return 0x9e790000 | n << 5 | d; }                // x = (uint64) d, truncating
constexpr u32 ucvtf(u32 d, u32 n) { return 0x9e630000 | n << 5 | d; }                 // d = (double) x, unsigned
constexpr u32 sub(u32 d, u32 n, u32 m) { return 0xcb000000 | m << 16 | n << 5 | d; }
constexpr u32 fmovi(u32 d, u32 imm8) { return 0x1e601000 | imm8 << 13 | d; }  // fmov d, #imm (see fimm8)
constexpr u32 FMOV1 = fmovi(1, 0x70);                                            // fmov d1, #1.0
constexpr u32 farith(int op, u32 d, u32 n, u32 m) {  // fadd/fsub/fmul/fdiv d, n, m (op: O_ADD..O_DIV)
  const u32 o[] = {0x1e602800, 0x1e603800, 0x1e600800, 0x1e601800};
  return o[op] | m << 16 | n << 5 | d;
}
constexpr u32 stpd(u32 a, u32 b, u32 n, int off) { return 0x6d000000 | u32((off / 8) & 0x7f) << 15 | b << 10 | n << 5 | a; }
constexpr u32 ldpd(u32 a, u32 b, u32 n, int off) { return 0x6d400000 | u32((off / 8) & 0x7f) << 15 | b << 10 | n << 5 | a; }
}
