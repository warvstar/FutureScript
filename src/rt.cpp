// Runtime: symbols, values, errors, helpers called from compiled code, natives. Portable: the arm64 half
// (executable memory, stack traces, fibers, C calls) is in jit.cpp, the WebAssembly half in wasm.cpp.
#include "fut.h"
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <time.h>

VM vm;
std::deque<Global> globals;
static std::vector<Fn*> builtins[8];  // by object kind, then method sym
std::vector<V> consts;
jmp_buf* errJmp;
void (*pollHook)();
std::string* asmListing;
bool verbose;
static_assert(offsetof(Obj, cid) == 4 && sizeof(Str) == 16 && sizeof(Obj) == 8 && offsetof(Class, isize) == CLS_IRUN + 4);

// ---------------- symbols ----------------
static std::deque<std::string> names;  // deque: stable storage for the views in `tab`
struct SymSlot { std::string_view s; uint32_t h; int id = -1; };
static std::vector<SymSlot> tab(1 << 10);  // open addressing, FNV-1a, power-of-two size
static SymSlot& symSlot(std::string_view s, uint32_t h) {  // s's slot, or the empty one to put it in
  size_t m = tab.size() - 1, i = h & m;
  while (tab[i].id >= 0 && !(tab[i].h == h && tab[i].s == s)) i = (i + 1) & m;
  return tab[i];
}
int intern(std::string_view s) {
  uint32_t h = 2166136261u;
  for (char c : s) h = (h ^ (uint8_t)c) * 16777619u;
  if (SymSlot& e = symSlot(s, h); e.id >= 0) return e.id;
  else e = {names.emplace_back(s), h, (int)names.size() - 1};
  if (names.size() * 2 > tab.size())  // grow and reinsert
    for (auto& e : std::exchange(tab, std::vector<SymSlot>(tab.size() * 2))) if (e.id >= 0) symSlot(e.s, e.h) = e;
  return (int)names.size() - 1;
}
const std::string& symName(int s) { return names[s]; }

Global& global(int sym) {
  static int next = 0;
  if (Global& g = growAt(globals, sym); g.slot < 0 && (g.slot = next++) >= MAXG) compileFail("too many globals", false);
  return globals[sym];
}
Global* findGlobal(int sym) { return (size_t)sym < globals.size() && globals[sym].slot >= 0 ? &globals[sym] : nullptr; }
static Fn* builtin(K k, uint64_t s) { return s < builtins[(int)k].size() ? builtins[(int)k][s] : nullptr; }
static int nics;
static constexpr uint64_t EMPTY = ~0ull;  // matches no header
int newIC() {
  if (nics == MAXIC) compileFail("too many inline caches", false);
  return vm.ics[nics].key = EMPTY, nics++;
}
u32 icEpoch = 1;
void clearICs() {
  icEpoch++;
  for (int i = 0; i < nics; i++) vm.ics[i].key = EMPTY;
}
static uint64_t header(Obj* o) { uint64_t h; memcpy(&h, o, 8); return h; }

// ---------------- values ----------------
const char* typeName(V v) {
  if (isNum(v)) return "number";
  if (!isObj(v)) return v == NIL ? "nil" : "bool";
  static const char* n[] = {"string", "list", "function", "class", "instance", "map", "function", "fiber"};
  return n[(int)obj(v)->kind];
}
std::string toStr(V v, bool quote) {
  if (isNum(v)) {
    char b[32];
    double d = num(v);
    if (std::fabs(d) < 1e15 && d == (double)(long long)d) snprintf(b, 32, "%lld", (long long)d);
    else if (std::fabs(d) < 1e19 && d == std::trunc(d)) snprintf(b, 32, "%.0f", d);  // whole: every digit it holds
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
    case K::Fiber: return "<fiber>";
    default: return "?";
  }
}
static bool equal(V a, V b) {
  if (isNum(a) && isNum(b)) return num(a) == num(b);
  return a == b || (is(a, K::Str) && is(b, K::Str) && as<Str>(a)->sv() == as<Str>(b)->sv());
}
static std::string_view text(V v, std::string& tmp) { return is(v, K::Str) ? as<Str>(v)->sv() : std::string_view(tmp = toStr(v)); }
static u32* mapSlot(Map* m, V k) {  // k's index slot, else the slot to insert it at
  static u32 none = 0;  // for an empty index; never written, as mapSet grows the index first
  if (m->idx.empty()) return &none;
  bool small = isNum(k) && std::fabs(num(k)) < 1e9 && num(k) == (int)num(k);  // includes -0.0, the same key as 0
  u32 h = is(k, K::Str) ? as<Str>(k)->hash : small ? (u32)(int)num(k) : (u32)((k * 0x9E3779B97F4A7C15ull) >> 32), *tomb = nullptr;
  for (size_t mask = m->idx.size() - 1;; h++) {
    u32* s = &m->idx[h & mask];
    if (!*s) return tomb ? tomb : s;
    if (*s == ~0u) tomb = tomb ? tomb : s;
    else if (equal(m->e[*s - 1].first, k)) return s;
  }
}
static long mapFind(Map* m, V k) { u32 s = *mapSlot(m, k); return s && s != ~0u ? (long)s - 1 : -1; }

[[noreturn]] void rtError(void* ra, const char* fmt, ...) {  // raise an error with a message string
  char msg[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);
  throwValue(box(newStr(msg)), ra);
}
static void rt_throw(V v) { throwValue(v, RA()); }

// ---------------- helpers called from JIT code (see Helper enum) ----------------
static V rt_binop(V a, V b, uint64_t op) {
  static const char* ops[] = {"+", "-", "*", "/", "%", "==", "!=", "<", "<=", ">", ">=", "&", "|", "^", "<<", ">>"};
  if (op >= O_BAND) {  // integer operators: on whole numbers (exact up to 2^53), computed in 64 bits
    auto whole = [](V v) { return isNum(v) && std::fabs(num(v)) < 9.2e18 && num(v) == std::trunc(num(v)); };
    if (!whole(a) || !whole(b)) ERR("'%s' needs whole numbers, got %s and %s", ops[op], toStr(a, true).c_str(), toStr(b, true).c_str());
    int64_t x = (int64_t)num(a), y = (int64_t)num(b);
    bool in = y >= 0 && y < 64;
    return box((double)(op == O_BAND ? x & y : op == O_BOR ? x | y : op == O_BXOR ? x ^ y
                       : op == O_SHL ? (in ? int64_t(uint64_t(x) << y) : 0) : x >> (in ? y : 63)));
  }
  if (isNum(a) && isNum(b) && op == O_MOD) return box(num(a) - num(b) * std::floor(num(a) / num(b)));  // the rest ran inline
  if (op == O_EQ || op == O_NE) return boxb(equal(a, b) == (op == O_EQ));
  if (op == O_ADD && (is(a, K::Str) || is(b, K::Str))) {
    std::string x, y;
    volatile V keep[] = {a, b};  // on the stack, so the GC sees the operands while newStr allocates
    return (void)keep, box(newStr(text(a, x), text(b, y)));
  }
  if (op == O_ADD && is(a, K::List) && is(b, K::List)) {
    List* l = make<List>();
    l->v = as<List>(a)->v;
    l->v.insert(l->v.end(), as<List>(b)->v.begin(), as<List>(b)->v.end());
    return box(l);
  }
  if (op >= O_LT && op <= O_GE && is(a, K::Str) && is(b, K::Str)) {
    int c = as<Str>(a)->sv().compare(as<Str>(b)->sv());
    return boxb(op == O_LT ? c < 0 : op == O_LE ? c <= 0 : op == O_GT ? c > 0 : c >= 0);
  }
  ERR("bad operands for '%s': %s and %s", ops[op], typeName(a), typeName(b));
}
// Fields: slow paths of the inline caches emitted by the compiler; they also fill the cache.
// Strings the runtime makes for m.name keys (m["name"]) and single characters are made once and kept in consts.
static V cached(V& slot, std::string_view s) { return slot ? slot : (consts.push_back(box(newStr(s))), slot = consts.back()); }
static V mapKey(uint64_t s) { static std::vector<V> keys; return cached(growAt(keys, s), symName(s)); }
static V rt_geti(V a, V i);
static V rt_seti(V a, V i, V v);
static V rt_getf(V o, uint64_t s, IC* ic) {
  if (is(o, K::Map)) return rt_geti(o, mapKey(s));
  if (is(o, K::Inst)) {
    Inst* in = as<Inst>(o);
    auto& l = in->cls->layout;
    if (u32 i = u32(std::find(l.begin(), l.end(), (int)s) - l.begin()); i < in->nslots && in->slots[i] != ABSENT) {
      if (ic) *ic = {header(in), i};
      return in->slots[i];
    }
  }
  ERR("%s has no field '%s'", typeName(o), symName(s).c_str());
}
// What JIT constructors read from a class: a new instance has room for every field the class has had so far.
static void reshape(Class* c) {
  u32 cap = (u32)std::min<size_t>(c->layout.size(), 0xffff);
  int k = sizeClass(sizeof(Inst) + cap * 8);
  c->ihdr = (uint64_t)K::Inst | (uint64_t)cap << 16 | (uint64_t)c->id << 32;
  c->irun = ROFF(k), c->isize = k < NCLS ? sizeClasses[k] : 16;  // too big: runs[NCLS] is empty, so 16 never fits
}
static V rt_setf(V o, uint64_t s, V v, IC* ic) {
  if (is(o, K::Map)) return rt_seti(o, mapKey(s), v);
  if (!is(o, K::Inst)) ERR("cannot set field '%s' on %s", symName(s).c_str(), typeName(o));
  Inst* in = as<Inst>(o);
  auto& l = in->cls->layout;
  u32 i = u32(std::find(l.begin(), l.end(), (int)s) - l.begin());
  if (i == l.size()) l.push_back((int)s), reshape(in->cls);
  if (i >= in->nslots) {  // a field this instance had no room for: move the slots out of line
    u32 n = (u32)in->cls->layout.size();
    if (n > 0xffff) ERR("too many fields");
    V* s = (V*)malloc(n * 8);
    std::copy(in->slots, in->slots + in->nslots, s), std::fill(s + in->nslots, s + n, ABSENT);
    if (in->slots != in->inl()) free(in->slots);
    else gcOwn(in);  // the GC frees the slots when the instance dies
    in->slots = s, in->nslots = (uint16_t)n;
  }
  if (ic) *ic = {header(in), i};
  return in->slots[i] = v;
}
#define INDEX(i, n) (isNum(i) && num(i) >= 0 && num(i) < (n) ? (size_t)num(i) : (ERR("index %s out of range (len %zu)", toStr(i).c_str(), (size_t)(n)), 0))
#define NUMARG(v, f) (isNum(v) ? num(v) : (ERR(f "() expects a number"), 0.0))
static V char1(V s, size_t i) { static V c[256]; return cached(c[(uint8_t)as<Str>(s)->s()[i]], {as<Str>(s)->s() + i, 1}); }
static void mapSet(Map* m, V k, V v) {
  if ((m->used + 1) * 4 > m->idx.size() * 3) {  // grow (and drop deleted slots): rebuild the index
    m->idx.assign(std::bit_ceil(m->e.size() * 2 + 8), 0), m->used = (u32)m->e.size();
    for (u32 i = 0; i < m->e.size(); i++) *mapSlot(m, m->e[i].first) = i + 1;
  }
  u32* s = mapSlot(m, k);
  if (*s && *s != ~0u) return void(m->e[*s - 1].second = v);
  m->used += !*s, *s = (u32)m->e.size() + 1, m->e.push_back({k, v});
}
static V rt_geti(V a, V i) {
  if (is(a, K::Map)) {
    long e = mapFind(as<Map>(a), i);
    return e < 0 ? NIL : as<Map>(a)->e[e].second;
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
  if (is(c, K::Fiber)) {  // each yielded value; the fiber's return value ends the loop
    V r = as<Fiber>(c)->state == FIB_DONE ? END : fiberResume(c, NIL);
    return as<Fiber>(c)->state == FIB_DONE ? END : r;
  }
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
static void* identCode;  // constructor of a class without init: returns self
// Resolve a call. w[0] is the callee or receiver, w[1..n] the args; on return the JIT loads x0..x7 from w (x8 = w).
// ic: a method call's cache (key: receiver header), or a constructor site's (key: the class value).
static void* rt_prep(V* w, uint64_t n, uint64_t sym, V sup, IC* ic) {
  V r = w[0];
  Fn* f = nullptr;
  IC* ctor = sym ? nullptr : ic;
  if (sym && is(r, K::Map) && !builtin(K::Map, sym)) {  // m.f(x) calls m["f"]
    long e = mapFind(as<Map>(r), mapKey(sym));
    if (e < 0) ERR("map has no key '%s' to call", symName(sym).c_str());
    r = w[0] = as<Map>(r)->e[e].second, sym = 0;
  }
  if (sym) {  // method call (sym 0 is the keyword 'var', never a method name)
    if (sup) f = as<Class>(sup)->super ? as<Class>(sup)->super->find(sym) : nullptr;
    else if (is(r, K::Inst)) f = as<Inst>(r)->cls->find(sym);
    else if (isObj(r)) f = builtin(obj(r)->kind, sym);
    if (!f) ERR("%s has no method '%s'", typeName(r), symName(sym).c_str());
    if (ic && is(r, K::Inst) && f->arity == (int)n + 1) *ic = {header(obj(r)), (uint64_t)f->code};
  } else if (is(r, K::Fn)) {
    f = as<Fn>(r);
    if (f->arity != (int)n) ERR("%s expects %d arguments, got %d", symName(f->name).c_str(), f->arity, (int)n);
    memmove(w, w + 1, n * 8);
    return f->code;
  } else if (is(r, K::Clo)) f = as<Clo>(r)->fn;
  else if (is(r, K::Class)) {
    Class* c = as<Class>(r);
    reshape(c);
    u32 cap = (u32)std::min<size_t>(c->layout.size(), 0xffff);
    Inst* in = make<Inst>(cap * 8);
    in->cls = c, in->cid = c->id, in->nslots = (uint16_t)cap, in->slots = in->inl();
    for (V* p = in->slots; p < in->slots + cap; p++) __atomic_store_n(p, ABSENT, __ATOMIC_RELAXED);  // no memset call
    w[0] = box(in);
    if (c->initEpoch != icEpoch) c->init = c->find(S_init), c->initEpoch = icEpoch;
    if (!(f = c->init)) {
      if (n) ERR("%s takes no arguments (no init)", symName(as<Class>(r)->name).c_str());
      if (ctor) *ctor = {r, (uint64_t)identCode};
      return identCode;
    }
    if (f->arity != (int)n + 1) ERR("%s expects %d arguments, got %d", symName(f->name).c_str(), f->arity - 1, (int)n);
    if (ctor) *ctor = {r, (uint64_t)f->code};  // the site allocates inline from now on (until the next reload)
    return f->code;
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
static V n_print(V v) { std::string t; auto s = text(v, t); return fwrite(s.data(), 1, s.size(), stdout), putchar('\n'), NIL; }
static V n_str(V v) { return is(v, K::Str) ? v : box(newStr(toStr(v))); }
static V n_len(V v) {
  if (is(v, K::Str)) return box((double)as<Str>(v)->len);
  if (is(v, K::List)) return box((double)as<List>(v)->v.size());
  if (is(v, K::Map)) return box((double)as<Map>(v)->e.size());
  ERR("len() of %s", typeName(v));
}
static V n_clock() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return box(t.tv_sec + t.tv_nsec * 1e-9); }
static V n_sleep(V v) { return sleepFor(NUMARG(v, "sleep")), NIL; }
static V n_floor(V v) { return box(std::floor(NUMARG(v, "floor"))); }
static V n_sqrt(V v) { return box(std::sqrt(NUMARG(v, "sqrt"))); }
static V l_push(V l, V v) { as<List>(l)->v.push_back(v); return l; }
static V l_pop(V l) {
  auto& v = as<List>(l)->v;
  if (v.empty()) ERR("pop() from empty list");
  V x = v.back();
  return v.pop_back(), x;
}
static V m_has(V m, V k) { return boxb(mapFind(as<Map>(m), k) >= 0); }
static V m_remove(V m, V k) {  // the last entry moves into the hole
  Map* p = as<Map>(m);
  long i = mapFind(p, k);
  if (i < 0) return NIL;
  V v = p->e[i].second;
  *mapSlot(p, k) = ~0u;
  if (i + 1 != (long)p->e.size()) p->e[i] = p->e.back(), *mapSlot(p, p->e[i].first) = (u32)i + 1;
  return p->e.pop_back(), v;
}
static V mapList(V m, bool keys) {
  List* l = make<List>();
  for (auto& [k, v] : as<Map>(m)->e) l->v.push_back(keys ? k : v);
  return box(l);
}

static Fn* nativeFn(const char* name, void* f, int arity) {
  Fn* o = make<Fn>();
  o->code = f, o->arity = arity, o->name = intern(name);
  return o;
}
void native(const char* name, void* f, int arity) {
  Global& g = global(intern(name));
  g.kind = 'n';
  vm.globals[g.slot] = box(g.o = nativeFn(name, f, arity));
}
void method(K k, int sym, void* f, int arity) { consts.push_back(box(growAt(builtins[(int)k], sym) = nativeFn(symName(sym).c_str(), f, arity))); }

void rtInit() {
  static const char* syms[] = {KEYWORDS(SYM_STR) EXTRA_SYMS(SYM_STR)};
  for (const char* s : syms) intern(s);
  for (V& g : vm.globals) g = NIL;
  void* h[NH] = {(void*)rt_poll, (void*)rt_stack, (void*)rt_binop, (void*)rt_getf, (void*)rt_setf, (void*)rt_geti,
                 (void*)rt_seti, (void*)rt_prep, (void*)rt_list, (void*)rt_iter, (void*)rt_numchk, (void*)rt_undef,
                 (void*)rt_map, (void*)rt_closure, (void*)rt_stale, (void*)rt_echo, nullptr, (void*)rt_yield, nullptr, (void*)rt_throw};
  memcpy(vm.helpers, h, sizeof h);  // H_FFI and H_HOT: platformInit
  gcInit();
  identCode = callable<+[](V self) { return self; }>();
  platformInit();
  native<n_print>("print"), native<n_str>("str"), native<n_len>("len"), native<n_clock>("clock"), native<n_sleep>("sleep");
  native<+[](V v) { return box(std::trunc(NUMARG(v, "int"))); }>("int");
  native<n_floor>("floor"), native<n_sqrt>("sqrt"), native<newFiber>("fiber");
  method<+[](V f) { return fiberResume(f, NIL); }>(K::Fiber, S_resume), method<fiberResume>(K::Fiber, S_send);
  method<+[](V f) { return boxb(as<Fiber>(f)->state == FIB_DONE); }>(K::Fiber, S_done);
  method<l_push>(K::List, S_push), method<l_pop>(K::List, S_pop), method<n_len>(K::List, S_len);
  method<n_len>(K::Str, S_len), method<n_len>(K::Map, S_len);
  gfxInit();
  method<m_has>(K::Map, S_has), method<m_remove>(K::Map, S_remove);
  method<+[](V m) { return mapList(m, true); }>(K::Map, S_keys);
  method<+[](V m) { return mapList(m, false); }>(K::Map, S_values);
}
