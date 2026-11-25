// Runtime: symbols, values, errors/stack traces, helpers called from JIT code, natives, executable memory.
#include "fut.h"
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <deque>
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <time.h>

VM vm;
std::unordered_map<int, Global> globals;
std::map<std::pair<int, int>, Fn*> methodFns, builtinMethods;
std::vector<V> consts;
jmp_buf* errJmp;
void (*pollHook)();
std::string* asmListing;
static_assert(offsetof(VM, helpers) == HOFF(0) && offsetof(VM, globals) == GOFF(0) && GOFF(MAXG) < (1 << 24));
static_assert(offsetof(Inst, cls) == INST_CLS && offsetof(Inst, slots) == INST_SLOTS && offsetof(Inst, n) == INST_N);
static_assert(sizeof(Clo) == CLO_ENV && sizeof(Str) == 16 && sizeof(Obj) == 8);

// ---------------- symbols ----------------
static std::deque<std::string> names;  // deque: stable storage for the views in `tab`
struct SymSlot { std::string_view s; uint32_t h; int id = -1; };
static std::vector<SymSlot> tab(1 << 10);  // open addressing, FNV-1a, power-of-two size
int intern(std::string_view s) {
  uint32_t h = 2166136261u;
  for (char c : s) h = (h ^ (uint8_t)c) * 16777619u;
  size_t m = tab.size() - 1, i = h & m;
  for (; tab[i].id >= 0; i = (i + 1) & m)
    if (tab[i].h == h && tab[i].s == s) return tab[i].id;
  int id = (int)names.size();
  tab[i] = {names.emplace_back(s), h, id};
  if (names.size() * 2 > tab.size()) {  // grow and reinsert
    std::vector<SymSlot> old(tab.size() * 2);
    old.swap(tab);
    for (auto& e : old)
      if (e.id >= 0) {
        size_t j = e.h & (tab.size() - 1);
        while (tab[j].id >= 0) j = (j + 1) & (tab.size() - 1);
        tab[j] = e;
      }
  }
  return id;
}
const std::string& symName(int s) { return names[s]; }

Global& global(int sym) {
  static int next = 0;
  auto [it, fresh] = globals.try_emplace(sym);
  if (fresh && (it->second.slot = next++) >= MAXG) throw std::runtime_error("too many globals");
  return it->second;
}
static std::deque<IC> ics;
IC* newIC() { return &ics.emplace_back(); }
void clearICs() { for (IC& i : ics) i.cls = nullptr; }

// ---------------- values ----------------
static const char* typeName(V v) {
  if (isNum(v)) return "number";
  if (!isObj(v)) return v == NIL ? "nil" : "bool";
  static const char* n[] = {"string", "list", "function", "class", "instance", "map", "function"};
  return n[(int)obj(v)->kind];
}
std::string toStr(V v, bool quote) {
  if (isNum(v)) {
    char b[32];
    double d = num(v);
    if (std::fabs(d) < 1e15 && d == (double)(long long)d) snprintf(b, 32, "%lld", (long long)d);
    else snprintf(b, 32, "%.14g", d);
    return b;
  }
  if (!isObj(v)) return v == NIL ? "nil" : v == TRUE_ ? "true" : "false";
  Obj* o = obj(v);
  std::string s;
  switch (o->kind) {
    case K::Str: return quote ? "\"" + std::string(((Str*)o)->sv()) + "\"" : std::string(((Str*)o)->sv());
    case K::List:
      for (V x : ((List*)o)->v) s += (s.empty() ? "" : ", ") + toStr(x, true);
      return "[" + s + "]";
    case K::Map:
      for (auto& [k, x] : ((Map*)o)->e) s += (s.empty() ? "" : ", ") + toStr(k, true) + ": " + toStr(x, true);
      return "{" + s + "}";
    case K::Fn: return "<fn " + symName(((Fn*)o)->name) + ">";
    case K::Clo: return "<fn " + symName(((Clo*)o)->fn->name) + ">";
    case K::Class: return "<class " + symName(((Class*)o)->name) + ">";
    case K::Inst: return "<" + symName(((Inst*)o)->cls->name) + ">";
    default: return "?";
  }
}
static bool equal(V a, V b) {
  if (isNum(a) && isNum(b)) return num(a) == num(b);
  return a == b || (is(a, K::Str) && is(b, K::Str) && as<Str>(a)->sv() == as<Str>(b)->sv());
}
size_t KeyHash::operator()(V v) const {
  if (is(v, K::Str)) return as<Str>(v)->hash;
  return std::hash<V>()(isNum(v) && num(v) == 0 ? 0 : v);  // 0.0 and -0.0 are the same key
}
bool KeyEq::operator()(V a, V b) const { return equal(a, b); }

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
template <class F> static void eachReturnAddress(F f) {  // walk the frame-pointer chain
  for (auto* fp = (uintptr_t*)__builtin_frame_address(0); fp && fp < stackTop; fp = (uintptr_t*)fp[0]) {
    f(fp[1]);
    if ((uintptr_t*)fp[0] <= fp) break;
  }
}
[[noreturn]] void rtError(void* ra, const char* fmt, ...) {
  char msg[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);
  // The site that failed, then every JIT frame found by walking the frame-pointer chain.
  std::vector<std::string> fr;
  if (auto d = frameDesc((uintptr_t)ra); !d.empty()) fr.push_back(d);
  bool seen = fr.empty();
  eachReturnAddress([&](uintptr_t r) {
    if (!seen && r == (uintptr_t)ra) seen = true;
    else if (auto d = frameDesc(r); !d.empty()) fr.push_back(d);
  });
  fflush(stdout);
  fprintf(stderr, "runtime error: %s\n", msg);
  for (size_t i = 0; i < fr.size(); i++) {
    if (fr.size() > 16 && i == 10) fprintf(stderr, "  ... %zu more frames\n", fr.size() - 15), i = fr.size() - 5;
    fputs(fr[i].c_str(), stderr);
  }
  if (errJmp) longjmp(*errJmp, 1);
  exit(1);
}

// ---------------- helpers called from JIT code (see Helper enum) ----------------
static V rt_binop(V a, V b, uint64_t op) {
  static const char* ops[] = {"+", "-", "*", "/", "%", "==", "!=", "<", "<=", ">", ">="};
  if (isNum(a) && isNum(b) && op == O_MOD) return box(num(a) - num(b) * std::floor(num(a) / num(b)));  // the rest ran inline
  if (op == O_EQ || op == O_NE) return boxb(equal(a, b) == (op == O_EQ));
  if (op == O_ADD && (is(a, K::Str) || is(b, K::Str))) return box(newStr(toStr(a) + toStr(b)));
  if (op == O_ADD && is(a, K::List) && is(b, K::List)) {
    List* l = make<List>();
    l->v = as<List>(a)->v;
    l->v.insert(l->v.end(), as<List>(b)->v.begin(), as<List>(b)->v.end());
    return box(l);
  }
  if (op >= O_LT && is(a, K::Str) && is(b, K::Str)) {
    int c = as<Str>(a)->sv().compare(as<Str>(b)->sv());
    return boxb(op == O_LT ? c < 0 : op == O_LE ? c <= 0 : op == O_GT ? c > 0 : c >= 0);
  }
  ERR("bad operands for '%s': %s and %s", ops[op], typeName(a), typeName(b));
}
// Fields: slow paths of the inline caches emitted by the compiler; they also fill the cache.
static V mapKey(uint64_t s) { return box(newStr(std::string(symName(s)))); }  // m.name is m["name"]
static V rt_geti(V a, V i);
static V rt_seti(V a, V i, V v);
static V rt_getf(V o, uint64_t s, IC* ic) {
  if (is(o, K::Map)) return rt_geti(o, mapKey(s));
  if (is(o, K::Inst)) {
    Inst* in = as<Inst>(o);
    auto it = in->cls->layout.find((int)s);
    if (it != in->cls->layout.end() && it->second < in->n && in->slots[it->second] != ABSENT) {
      if (ic) ic->cls = in->cls, ic->idx = it->second;
      return in->slots[it->second];
    }
  }
  ERR("%s has no field '%s'", typeName(o), symName(s).c_str());
}
static V rt_setf(V o, uint64_t s, V v, IC* ic) {
  if (is(o, K::Map)) return rt_seti(o, mapKey(s), v);
  if (!is(o, K::Inst)) ERR("cannot set field '%s' on %s", symName(s).c_str(), typeName(o));
  Inst* in = as<Inst>(o);
  u32 i = in->cls->layout.try_emplace((int)s, (u32)in->cls->layout.size()).first->second;
  if (i >= in->n) {
    u32 n = (u32)in->cls->layout.size();
    in->slots = (V*)realloc(in->slots, n * 8);
    std::fill(in->slots + in->n, in->slots + n, ABSENT);
    in->n = n;
  }
  if (ic) ic->cls = in->cls, ic->idx = i;
  return in->slots[i] = v;
}
#define INDEX(i, n) (isNum(i) && num(i) >= 0 && num(i) < (n) ? (size_t)num(i) : (ERR("index %s out of range (len %zu)", toStr(i).c_str(), (size_t)(n)), 0))
#define NUMARG(v, f) (isNum(v) ? num(v) : (ERR(f "() expects a number"), 0.0))
static V char1(V s, size_t i) { return box(newStr({as<Str>(s)->s() + i, 1})); }
static void mapSet(Map* m, V k, V v) {
  auto [it, fresh] = m->idx.try_emplace(k, (u32)m->e.size());
  if (fresh) m->e.push_back({k, v});
  else m->e[it->second].second = v;
}
static V rt_geti(V a, V i) {
  if (is(a, K::Map)) {
    auto it = as<Map>(a)->idx.find(i);
    return it == as<Map>(a)->idx.end() ? NIL : as<Map>(a)->e[it->second].second;
  }
  if (!is(a, K::List) && !is(a, K::Str)) ERR("cannot index %s", typeName(a));
  if (is(a, K::List)) return as<List>(a)->v[INDEX(i, as<List>(a)->v.size())];
  return char1(a, INDEX(i, as<Str>(a)->len));
}
static V rt_seti(V a, V i, V v) {
  if (is(a, K::Map)) return mapSet(as<Map>(a), i, v), v;
  if (!is(a, K::List)) ERR("cannot assign into %s", typeName(a));
  auto& l = as<List>(a)->v;
  return l[INDEX(i, l.size())] = v;
}
static V rt_iter(V c, uint64_t i) {
  if (is(c, K::List)) return i < as<List>(c)->v.size() ? as<List>(c)->v[i] : END;
  if (is(c, K::Map)) return i < as<Map>(c)->e.size() ? as<Map>(c)->e[i].first : END;
  if (!is(c, K::Str)) ERR("cannot iterate over %s", typeName(c));
  return i < as<Str>(c)->len ? char1(c, i) : END;
}
static V rt_list(V* w, uint64_t n) {
  List* l = make<List>();
  l->v.assign(w, w + n);
  return box(l);
}
static V rt_map(V* w, uint64_t n) {
  Map* m = make<Map>();
  for (uint64_t i = 0; i < n; i++) mapSet(m, w[2 * i], w[2 * i + 1]);
  return box(m);
}
static V rt_closure(V fn, uint64_t n, V* w) {
  Clo* c = make<Clo>(n * 8);
  c->fn = as<Fn>(fn), c->n = (u32)n;
  memcpy(c->env(), w, n * 8);
  return box(c);
}
static V rt_ident(V self) { return self; }  // constructor of a class without init
// Resolve a call. w[0] is the callee or receiver, w[1..n] the args; on return the JIT loads x0..x7 from w (x8 = w).
static void* rt_prep(V* w, uint64_t n, uint64_t sym, V sup, IC* ic) {
  V r = w[0];
  Fn* f = nullptr;
  if (sym && is(r, K::Map) && !builtinMethods.count({(int)K::Map, (int)sym})) {  // m.f(x) calls m["f"]
    auto it = as<Map>(r)->idx.find(mapKey(sym));
    if (it == as<Map>(r)->idx.end()) ERR("map has no key '%s' to call", symName(sym).c_str());
    r = w[0] = as<Map>(r)->e[it->second].second, sym = 0;
  }
  if (sym) {  // method call (sym 0 is the keyword 'var', never a method name)
    if (sup) f = as<Class>(sup)->super ? as<Class>(sup)->super->find(sym) : nullptr;
    else if (is(r, K::Inst)) f = as<Inst>(r)->cls->find(sym);
    else if (isObj(r)) {
      auto it = builtinMethods.find({(int)obj(r)->kind, (int)sym});
      f = it == builtinMethods.end() ? nullptr : it->second;
    }
    if (!f) ERR("%s has no method '%s'", typeName(r), symName(sym).c_str());
    if (ic && is(r, K::Inst) && f->arity == (int)n + 1) ic->cls = as<Inst>(r)->cls, ic->code = f->code;
  } else if (is(r, K::Fn)) {
    f = as<Fn>(r);
    if (f->arity != (int)n) ERR("%s expects %d arguments, got %d", symName(f->name).c_str(), f->arity, (int)n);
    memmove(w, w + 1, n * 8);
    return f->code;
  } else if (is(r, K::Clo)) f = as<Clo>(r)->fn;
  else if (is(r, K::Class)) {
    w[0] = box(make<Inst>());
    as<Inst>(w[0])->cls = as<Class>(r);
    if (!(f = as<Class>(r)->find(S_init))) {
      if (n) ERR("%s takes no arguments (no init)", symName(as<Class>(r)->name).c_str());
      return (void*)rt_ident;
    }
  } else ERR("cannot call %s", typeName(r));
  if (f->arity != (int)n + 1) ERR("%s expects %d arguments, got %d", symName(f->name).c_str(), f->arity - 1, (int)n);
  return f->code;
}
static V rt_numchk(V v) { return isNum(v) ? v : (ERR("expected a number, got %s", typeName(v)), v); }
static void rt_stack() { ERR("stack overflow"); }
static void rt_undef() { ERR("call to undefined function"); }
static void rt_stale(uint64_t sym, void* ra) {
  rtError(ra, "stale call to '%s': its parameters changed, but this caller is old code that is still running",
          symName(sym).c_str());
}
static void rt_poll() { if (pollHook) pollHook(); }
static V rt_echo(V v) {
  if (v != NIL) printf("%s\n", toStr(v, true).c_str());
  return v;
}

// ---------------- natives ----------------
static V n_print(V v) { std::string s = toStr(v) + "\n"; fwrite(s.data(), 1, s.size(), stdout); return NIL; }
static V n_str(V v) { return box(newStr(toStr(v))); }
static V n_len(V v) {
  if (is(v, K::Str)) return box((double)as<Str>(v)->len);
  if (is(v, K::List)) return box((double)as<List>(v)->v.size());
  if (is(v, K::Map)) return box((double)as<Map>(v)->e.size());
  ERR("len() of %s", typeName(v));
}
static V n_clock() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return box(t.tv_sec + t.tv_nsec * 1e-9); }
static V n_sleep(V v) {
  double s = NUMARG(v, "sleep");
  fflush(stdout);  // live loops sleep every frame; keep piped output flowing
  timespec t{(time_t)s, (long)((s - (time_t)s) * 1e9)};
  nanosleep(&t, nullptr);
  return NIL;
}
static V n_floor(V v) { return box(std::floor(NUMARG(v, "floor"))); }
static V n_sqrt(V v) { return box(std::sqrt(NUMARG(v, "sqrt"))); }
static V l_push(V l, V v) { as<List>(l)->v.push_back(v); return l; }
static V l_pop(V l) {
  auto& v = as<List>(l)->v;
  if (v.empty()) ERR("pop() from empty list");
  V x = v.back();
  v.pop_back();
  return x;
}
static V m_has(V m, V k) { return boxb(as<Map>(m)->idx.count(k)); }
static V m_remove(V m, V k) {
  Map* p = as<Map>(m);
  auto it = p->idx.find(k);
  if (it == p->idx.end()) return NIL;
  u32 i = it->second;
  V v = p->e[i].second;
  p->idx.erase(it);
  if (i + 1 != p->e.size()) p->e[i] = p->e.back(), p->idx[p->e[i].first] = i;
  p->e.pop_back();
  return v;
}
static V mapList(V m, bool keys) {
  List* l = make<List>();
  for (auto& [k, v] : as<Map>(m)->e) l->v.push_back(keys ? k : v);
  return box(l);
}
static V m_keys(V m) { return mapList(m, true); }
static V m_values(V m) { return mapList(m, false); }

// ---------------- executable memory ----------------
static uint8_t* jit;
static size_t jitUsed;
constexpr size_t JIT_SIZE = 128 << 20;  // one region keeps every bl/b within reach (+-128MB)
static std::map<u32*, size_t> freeBlocks;  // reclaimed code: address -> bytes
static std::vector<u32*> retired;
static u32* trap;
static V (*enter)(void* code, const V* args, VM* vm);

static int writeDepth;
static uint8_t *dirtyLo, *dirtyHi;
JitBatch::JitBatch() {
  if (!writeDepth++) pthread_jit_write_protect_np(0), dirtyLo = jit + JIT_SIZE, dirtyHi = jit;
}
JitBatch::~JitBatch() {
  if (--writeDepth) return;
  pthread_jit_write_protect_np(1);
  if (dirtyLo < dirtyHi) sys_icache_invalidate(dirtyLo, dirtyHi - dirtyLo);
}
static void jitWrite(u32* p, const u32* src, size_t n) {
  JitBatch b;
  memcpy(p, src, n * 4);
  dirtyLo = std::min(dirtyLo, (uint8_t*)p), dirtyHi = std::max(dirtyHi, (uint8_t*)(p + n));
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
static void jitFree(u32* p, size_t bytes) {
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
  for (auto [i, t] : k.rel) k.c[i] |= u32((u32*)t - (p + i)) & 0x3ffffff;
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
void jitCollect() {  // free retired code that no frame on the stack will return into
  std::vector<uintptr_t> ras;
  eachReturnAddress([&](uintptr_t r) { ras.push_back(r); });
  std::erase_if(retired, [&](u32* b) {
    auto it = lineTabs.find(b);
    if (it == lineTabs.end()) return true;
    for (uintptr_t r : ras) if ((u32*)r > b && (u32*)r <= it->second.hi) return false;
    jitFree(b, ((it->second.hi - b) * 4 + 15) & ~size_t(15));
    lineTabs.erase(it);
    return true;
  });
}
void jitShiftLines(u32* body, int delta) {
  if (auto it = lineTabs.find(body); it != lineTabs.end()) for (auto& l : it->second.lines) l.second += delta;
}
// The hot-reload primitive: rewrite one `b target` instruction in place (single aligned 4-byte store).
void patchStub(u32* s, void* target) {
  u32 b = a64::B | (u32((u32*)target - s) & 0x3ffffff);
  jitWrite(s, &b, 1);
}
u32* newStub() {  // stubs are packed 4 bytes apart and never freed
  static u32 *pool, *poolEnd;
  if (pool == poolEnd) pool = jitAlloc(1024), poolEnd = pool + 1024;
  patchStub(pool, trap);
  return pool++;
}
u32* staleThunk(int sym) {  // x1 = caller's return address (b from the stub leaves x30 intact)
  using namespace a64;
  Code k{{mov(1, 30), movz(0, sym & 0xffff, 0), movk(0, (u32)sym >> 16, 1), ldr(16, 28, HOFF(H_STALE)), blr(16)}, {}, {}, "<stale>", ""};
  return jitInstall(k);
}
V vmCall(void* code, std::initializer_list<V> a) {
  V args[8] = {};
  std::copy(a.begin(), a.end(), args);
  return enter(code, args, &vm);
}
bool readFile(const std::string& path, std::string& out) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return false;
  fseek(f, 0, SEEK_END), out.resize(ftell(f)), rewind(f);
  out.resize(fread(out.data(), 1, out.size(), f)), fclose(f);
  return true;
}

static Fn* nativeFn(const char* name, void* f, int arity) {
  Fn* o = make<Fn>();
  o->code = f, o->arity = arity, o->name = intern(name);
  return o;
}
static void native(const char* name, void* f, int arity) {
  Global& g = global(intern(name));
  g.kind = 'n';
  vm.globals[g.slot] = box(g.o = nativeFn(name, f, arity));
}
static void method(K k, int sym, void* f, int arity) { builtinMethods[{(int)k, sym}] = nativeFn(symName(sym).c_str(), f, arity); }

void rtInit() {
  static const char* syms[] = {KEYWORDS(SYM_STR) EXTRA_SYMS(SYM_STR)};
  for (const char* s : syms) intern(s);
  pthread_t self = pthread_self();
  stackTop = (uintptr_t*)pthread_get_stackaddr_np(self);
  vm.limit = (char*)stackTop - pthread_get_stacksize_np(self) + (256 << 10);
  for (V& g : vm.globals) g = NIL;
  void* h[NH] = {(void*)rt_poll, (void*)rt_stack, (void*)rt_binop, (void*)rt_getf, (void*)rt_setf, (void*)rt_geti,
                 (void*)rt_seti, (void*)rt_prep, (void*)rt_list, (void*)rt_iter, (void*)rt_numchk, (void*)rt_undef,
                 (void*)rt_map, (void*)rt_closure, (void*)rt_stale, (void*)rt_echo};
  memcpy(vm.helpers, h, sizeof h);
  gcInit();

  jit = (uint8_t*)mmap(nullptr, JIT_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
  if (jit == MAP_FAILED) perror("mmap"), exit(1);
  using namespace a64;
  Code t{{ldr(16, 28, HOFF(H_UNDEF)), blr(16)}, {}, {}, "<undefined>", ""};
  trap = jitInstall(t);
  // enter(code, args[8], &vm): save the callee-saved regs we pin (x26 stack limit, x27 QNAN, x28 VM), load args, call.
  Code e{{stp_pre(29, 30), addi(29, SP, 0), stp_pre(26, 27), stp_pre(28, 19), mov(9, 0), mov(28, 2), ldr(26, 28, 8),
          movz(27, 0x7ffc, 3), mov(10, 1)}, {}, {}, "<enter>", ""};
  for (u32 i = 0; i < 8; i++) e.c.push_back(ldr(i, 10, i * 8));
  for (u32 i : {blr(9), ldp_post(28, 19), ldp_post(26, 27), ldp_post(29, 30), RET}) e.c.push_back(i);
  enter = (V(*)(void*, const V*, VM*))jitInstall(e);

  native("print", (void*)n_print, 1), native("str", (void*)n_str, 1), native("len", (void*)n_len, 1);
  native("clock", (void*)n_clock, 0), native("sleep", (void*)n_sleep, 1);
  native("floor", (void*)n_floor, 1), native("sqrt", (void*)n_sqrt, 1);
  method(K::List, S_push, (void*)l_push, 2), method(K::List, S_pop, (void*)l_pop, 1), method(K::List, S_len, (void*)n_len, 1);
  method(K::Str, S_len, (void*)n_len, 1), method(K::Map, S_len, (void*)n_len, 1);
  method(K::Map, S_has, (void*)m_has, 2), method(K::Map, S_remove, (void*)m_remove, 2);
  method(K::Map, S_keys, (void*)m_keys, 1), method(K::Map, S_values, (void*)m_values, 1);
}
