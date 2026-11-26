// Runtime: symbols, values, errors, helpers called from compiled code, natives. Portable: the arm64 half
// (executable memory, stack traces, fibers, C calls) is in jit.cpp, the WebAssembly half in wasm.cpp.
#include "fut.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdarg>
#include <unordered_map>
#include <unordered_set>
#include <time.h>
#if __wasm__
extern "C" __attribute__((import_module("host"), import_name("pow"))) double host_pow(double, double);
#else
#include <sys/mman.h>
#endif

std::deque<Global> globals;
static std::vector<Fn*> builtins[16];  // by object kind, then method sym
std::vector<V> consts;
std::string mainFile;
void (*pollHook)();
std::string* asmListing;
bool verbose, noTier;
static_assert(offsetof(Obj, cid) == 4 && sizeof(Str) == 16 && sizeof(Obj) == 8 && offsetof(Class, isize) == CLS_IRUN + 4);

// ---------------- symbols ----------------
// Names live in blocks that never move (stable storage for the views in `tab`), so symName reads without a lock
// while another isolate's compile interns more. Interning locks.
constexpr int NB = 1024;
static std::atomic<std::string*> nameBlocks[1 << 12];
static std::atomic<int> nnames;
static Mutex symMu;
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
  Locked g(symMu);
  SymSlot& e = symSlot(s, h);
  if (e.id >= 0) return e.id;
  int id = nnames.load(std::memory_order_relaxed);
  if (id >= NB * (1 << 12)) fputs("fut: too many names\n", stderr), exit(1);
  std::string* b = nameBlocks[id / NB].load(std::memory_order_relaxed);
  if (!b) nameBlocks[id / NB].store(b = new std::string[NB], std::memory_order_release);
  b[id % NB] = s;
  e = {b[id % NB], h, id};
  nnames.store(id + 1, std::memory_order_release);
  if ((size_t)(id + 1) * 2 > tab.size())
    for (auto& e : std::exchange(tab, std::vector<SymSlot>(tab.size() * 2))) if (e.id >= 0) symSlot(e.s, e.h) = e;
  return id;
}
const std::string& symName(int s) { return nameBlocks[s / NB].load(std::memory_order_acquire)[s % NB]; }

static int nextGlobal;
int globalCount() { return nextGlobal; }
Global& global(int sym) {
  if (Global& g = growAt(globals, sym); g.slot < 0 && (g.slot = nextGlobal++) >= MAXG) compileFail("too many globals", false);
  return globals[sym];
}
Global* findGlobal(int sym) { return (size_t)sym < globals.size() && globals[sym].slot >= 0 ? &globals[sym] : nullptr; }
Global* fnNamed(int sym) {
  Global* g = findGlobal(sym);
  return g && g->kind == 'f' && g->o ? g : nullptr;
}
static Fn* builtin(K k, uint64_t s) { return s < builtins[(int)k].size() ? builtins[(int)k][s] : nullptr; }
// Inline caches live in each isolate's VM block, at the same index in all of them. A new one is made empty in
// every block (no isolate runs the code using it yet); filling and clearing touch only the running isolate's.
static int nics;
static constexpr uint64_t EMPTY = ~0ull;  // matches no header
static std::vector<int> setICs;
static Mutex icMu;  // nics, setICs, vms
static std::vector<VM*> vms;
#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
extern "C" void AnnotateIgnoreWritesBegin(const char*, int), AnnotateIgnoreWritesEnd(const char*, int);
#define TSAN_UNSEEN_BEGIN() AnnotateIgnoreWritesBegin(__FILE__, __LINE__)
#define TSAN_UNSEEN_END() AnnotateIgnoreWritesEnd(__FILE__, __LINE__)
#endif
#endif
#ifndef TSAN_UNSEEN_BEGIN
#define TSAN_UNSEEN_BEGIN()
#define TSAN_UNSEEN_END()
#endif
int newIC(bool set) {
  Locked g(icMu);
  if (nics == MAXIC) compileFail("too many inline caches", false);
  if (set) setICs.push_back(nics);
  // Each isolate's slot for the new cache, written from the compiling thread. The isolate's own writes to it (rt_setf)
  // come after: only code compiled from here names the slot, and that code reaches the isolate published under codeMu.
  // TSan doesn't see that ordering, since what carries it is JIT code, which it doesn't instrument, so these writes
  // are hidden from it (and only these).
  TSAN_UNSEEN_BEGIN();
  for (VM* v : vms) v->ics[nics].key = EMPTY;
  TSAN_UNSEEN_END();
  return nics++;
}
void clearSetICs() { Locked g(icMu); for (int i : setICs) vm->ics[i].key = EMPTY; }
u32 icEpoch = 1;
void clearICs() {  // a reload: every isolate is stopped
  Locked g(icMu);
  icEpoch++;
  for (VM* v : vms) for (int i = 0; i < nics; i++) v->ics[i].key = EMPTY;
}
VM* vmNew() {
#if __wasm__
  VM* v = (VM*)calloc(1, sizeof(VM));
#else  // most of it (globals, caches) is only touched as it is used
  VM* v = (VM*)mmap(nullptr, sizeof(VM), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
  if (v == MAP_FAILED) perror("mmap"), exit(1);
#endif
  Locked g(icMu);
  if (!vms.empty()) memcpy(v->helpers, vms[0]->helpers, sizeof v->helpers);
  for (V& x : v->globals) x = NIL;
  for (int i = 0; i < nics; i++) v->ics[i].key = EMPTY;
  v->allowed = EFF_BITS, v->hstack = NIL;  // unchecked, no handlers (as effectsInit)
  vms.push_back(v);
  return v;
}
void vmFree(VM* v) {
  {
    Locked g(icMu);
    std::erase(vms, v);
  }
#if __wasm__
  free(v);
#else
  munmap(v, sizeof(VM));
#endif
}
void eachVM(void (*f)(VM*, void*), void* ctx) { Locked g(icMu); for (VM* v : vms) f(v, ctx); }
static uint64_t header(Obj* o) { uint64_t h; memcpy(&h, o, 8); return h; }

// ---------------- values ----------------
const char* typeName(V v) {
  if (isNum(v)) return "number";
  if (!isObj(v)) return v == NIL ? "nil" : "bool";
  static const char* n[] = {"string", "list", "function", "class", "instance", "map", "function", "fiber", "js value", "channel", "task"};
  return n[(int)obj(v)->kind];
}
static std::string numberText(double d) {
  if (std::isnan(d)) return "(0/0)";
  if (std::isinf(d)) return d < 0 ? "-1e309" : "1e309";
  if (std::fabs(d) <= 9007199254740992.0 && d == std::trunc(d))
    return d == 0 && std::signbit(d) ? "-0" : std::to_string((int64_t)d);

  // Get the shortest round-tripping significand, then choose where to put the decimal point.
  char buf[32];
  auto end = std::to_chars(buf, buf + sizeof buf, d, std::chars_format::scientific).ptr;
  const char* e = std::find(buf, end, 'e');
  int exponent = 0;
  std::from_chars(e + 2, end, exponent);
  if (e[1] == '-') exponent = -exponent;
  if (exponent < -6 || exponent >= 21) return std::string(buf, e + 1 - buf) + std::to_string(exponent);
  std::string out = d < 0 ? "-" : "";
  const char* first = buf + out.size();
  std::string digits(first, first + 1);
  if (first + 1 < e) digits.append(first + 2, e);
  int point = exponent + 1;
  if (point <= 0) return out + "0." + std::string(-point, '0') + digits;
  digits.resize(std::max(digits.size(), (size_t)point), '0');
  if ((size_t)point < digits.size()) digits.insert(point, 1, '.');
  return out + digits;
}

static bool bareKey(std::string_view s) {
  auto letter = [](char c) { return ((c | 32) >= 'a' && (c | 32) <= 'z') || c == '_'; };
  if (s.empty() || !letter(s.front())) return false;
  for (char c : s) if (!letter(c) && !(c >= '0' && c <= '9')) return false;
  static constexpr std::string_view keywords[] = {
#define KEYWORD_TEXT(k) #k,
    KEYWORDS(KEYWORD_TEXT)
#undef KEYWORD_TEXT
  };
  return std::find(std::begin(keywords), std::end(keywords), s) == std::end(keywords);
}
static void renderString(std::string& out, std::string_view s) {
  out += '"';
  for (char c : s) {
    switch (c) {
      case '\n': out += "\\n"; break;
      case '\t': out += "\\t"; break;
      case '\0': out += "\\0"; break;
      case '"': case '\\': case '{': out += '\\'; [[fallthrough]];
      default: out += c;
    }
  }
  out += '"';
}

static void render(std::string& out, V v, bool quote, std::unordered_set<Obj*>& active) {
  if (isNum(v)) { out += numberText(num(v)); return; }
  if (!isObj(v)) { out += v == NIL ? "nil" : v == TRUE_ ? "true" : "false"; return; }
  Obj* o = obj(v);
  switch (o->kind) {
    case K::Str: {
      auto s = as<Str>(v)->sv();
      if (quote) renderString(out, s);
      else out += s;
      return;
    }
    case K::Fn: out += "<fn " + symName(as<Fn>(v)->name) + ">"; return;
    case K::Clo: out += "<fn " + symName(as<Clo>(v)->fn->name) + ">"; return;
    case K::Class: out += "<class " + symName(as<Class>(v)->name) + ">"; return;
    case K::Fiber: out += "<fiber>"; return;
    case K::Ref: out += "<js value>"; return;
    case K::Chan: out += "<channel>"; return;
    case K::Task: out += "<task>"; return;
    case K::List: case K::Map: case K::Inst: break;
    default: out += '?'; return;
  }
  char here;  // nested too deep for the C stack (a fiber's is small): cut off; natively, where fut code would stop
  if (active.size() >= 1000 || (!WASM && &here < (char*)vm->limit)) { out += o->kind == K::List ? "[...]" : o->kind == K::Map ? "{...}" : "<...>"; return; }
  if (!active.insert(o).second) { out += "<cycle>"; return; }
  bool first = true;
  auto separator = [&] { if (!std::exchange(first, false)) out += ", "; };
  if (o->kind == K::List) {
    out += '[';
    for (V x : as<List>(v)->v) {
      separator();
      render(out, x, true, active);
    }
    out += ']';
  } else if (o->kind == K::Map) {
    out += '{';
    for (auto& [k, x] : as<Map>(v)->e) {
      separator();
      bool bare = is(k, K::Str) && bareKey(as<Str>(k)->sv());
      render(out, k, !bare, active);
      out += ": ";
      render(out, x, true, active);
    }
    out += '}';
  } else if (Inst* in = as<Inst>(v); in->cls->sum == 2) {  // an enum's variant: as constructed, Shape.Circle(2)
    out += symName(in->cls->name);
    if (!in->cls->fields.empty()) {
      out += '(';
      for (auto& [f, _] : in->cls->fields) {
        separator();
        render(out, variantField(in, f), true, active);
      }
      out += ')';
    }
  } else {
    out += symName(in->cls->name);
    out += '{';
    // Declared fields first, the base class's before the subclass's (one never assigned shows its default), then
    // the ones made by assignment, in slot order.
    std::vector<Class*> chain;
    for (Class* c = in->cls; c; c = c->super) chain.insert(chain.begin(), c);
    std::vector<int> names;
    for (Class* c : chain)
      for (auto& [f, _] : c->fields) if (std::find(names.begin(), names.end(), f) == names.end()) names.push_back(f);
    const int* slots = in->cls->layout.data.load(std::memory_order_acquire);
    for (u32 i = 0; i < in->nslots; i++)
      if (in->slots[i] != ABSENT && std::find(names.begin(), names.end(), slots[i]) == names.end()) names.push_back(slots[i]);
    for (int f : names) {
      V x = NIL;
      bool method = false;
      if (!fieldOf(v, f, &x, &method) || method) continue;
      separator();
      out += symName(f);
      out += ": ";
      render(out, x, true, active);
    }
    out += '}';
  }
  active.erase(o);
}
std::string toStr(V v, bool quote) {
  std::string out;
  std::unordered_set<Obj*> active;
  render(out, v, quote, active);
  return out;
}
static bool equal(V a, V b) {
  if (isNum(a) && isNum(b)) return num(a) == num(b);
  return a == b || (is(a, K::Str) && is(b, K::Str) && as<Str>(a)->sv() == as<Str>(b)->sv());
}
V variantField(Inst* in, int s) {
  int i = in->cls->layout.find(s);
  V v = i >= 0 && i < in->nslots ? in->slots[i] : ABSENT;
  return v == ABSENT ? NIL : v;
}
// ==: equal, and two values of one enum variant when their fields are (an enum's values are data, compared by value)
static bool sameValue(V a, V b, int depth = 0) {
  if (equal(a, b)) return true;
  if (!is(a, K::Inst) || !is(b, K::Inst) || depth > 200) return false;
  Inst *x = as<Inst>(a), *y = as<Inst>(b);
  if (x->cls != y->cls || x->cls->sum != 2) return false;
  for (auto& [f, d] : x->cls->fields) if (!sameValue(variantField(x, f), variantField(y, f), depth + 1)) return false;
  return true;
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

[[noreturn]] void rtError(void* ra, const char* fmt, ...) {
  const char* code = takeCode(fmt);
  char msg[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);
  V v = box(newStr(msg));
  if (code) noteRaised(v, msg, code);
  throwValue(v, ra);
}
static void rt_throw(V v) { throwValue(v, RA()); }

// ---------------- helpers called from JIT code (see Helper enum) ----------------
static V rt_format(V v, V packed, V prefix) {
  FormatSpec f = std::bit_cast<FormatSpec>(packed);
  bool numeric = isNum(v), precise = f.precision != FormatSpec::noPrecision;
  bool base = f.type && strchr("xXbo", f.type);
  if (!numeric && (f.type || f.flags))
    ERR("@argument this format needs a number, got %s", typeName(v));
  if (!numeric && precise && !is(v, K::Str))
    ERR("@argument format precision needs a number or string, got %s", typeName(v));
  double d = numeric ? num(v) : 0;
  if (base && (!std::isfinite(d) || std::trunc(d) != d))
    ERR("@argument format '%c' needs a finite whole number", f.type);

  std::string s;
  if (base) {
    int radix = f.type == 'b' ? 2 : f.type == 'o' ? 8 : 16;
    const char* digits = f.type == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
    double n = std::fabs(d);
    // The radices are powers of two, so every digit of even a large whole double is exact.
    do {
      s += digits[(int)std::fmod(n, radix)];
      n = std::trunc(n / radix);
    } while (n != 0);
    if (std::signbit(d)) s += '-';
    std::reverse(s.begin(), s.end());
  } else if (numeric && (precise || f.type || ((f.flags & FormatSpec::Group) && std::isfinite(d) && std::trunc(d) == d))) {
    // At most 309 digits before the decimal point for a finite double.
    char buf[FormatSpec::maxPrecision + 312];
    int precision = precise ? f.precision : f.type ? 6 : 0;
    snprintf(buf, sizeof buf, f.type == 'e' ? "%.*e" : "%.*f", precision, d);
    s = buf;
  } else {
    s = toStr(v);
    if (precise && s.size() > f.precision) s.resize(f.precision);
  }
  if (f.flags & FormatSpec::Group) {
    size_t first = !s.empty() && s[0] == '-';
    size_t last = first;
    while (last < s.size() && s[last] >= '0' && s[last] <= '9') last++;
    while (last - first > 3) { last -= 3; s.insert(last, 1, ','); }
  }
  if ((f.flags & FormatSpec::Sign) && s[0] != '-') s.insert(0, 1, '+');
  if (s.size() < f.width) {
    size_t padding = f.width - s.size();
    char align = f.align ? f.align : numeric ? '>' : '<';
    if (f.flags & FormatSpec::Zero) s.insert(s[0] == '+' || s[0] == '-', padding, '0');
    else {
      size_t left = align == '>' ? padding : align == '^' ? padding / 2 : 0;
      s.insert(0, left, f.fill);
      s.append(padding - left, f.fill);
    }
  }
  if (prefix != NIL) return box(newStr(as<Str>(prefix)->sv(), s));
  return box(newStr(s));
}

static V rt_binop(V a, V b, uint64_t op) {
  static const char* ops[] = {"+", "-", "*", "/", "%", "==", "!=", "<", "<=", ">", ">=", "&", "|", "^", "<<", ">>"};
  if (op >= O_BAND) {  // integer operators: on whole numbers (exact up to 2^53), computed in 64 bits
    auto whole = [](V v) { return isNum(v) && std::fabs(num(v)) < 9.2e18 && num(v) == std::trunc(num(v)); };
    if (!whole(a) || !whole(b)) ERR("@operand '%s' needs whole numbers, got %s and %s", ops[op], toStr(a, true).c_str(), toStr(b, true).c_str());
    int64_t x = (int64_t)num(a), y = (int64_t)num(b);
    bool in = y >= 0 && y < 64;
    return box((double)(op == O_BAND ? x & y : op == O_BOR ? x | y : op == O_BXOR ? x ^ y
                       : op == O_SHL ? (in ? int64_t(uint64_t(x) << y) : 0) : x >> (in ? y : 63)));
  }
  if (isNum(a) && isNum(b) && op == O_MOD) return box(num(a) - num(b) * std::floor(num(a) / num(b)));  // the rest ran inline
  if (op == O_EQ || op == O_NE) return boxb(sameValue(a, b) == (op == O_EQ));
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
  ERR("@operand bad operands for '%s': %s and %s", ops[op], typeName(a), typeName(b));
}
// Fields: slow paths of the inline caches emitted by the compiler; they also fill the cache.
// Strings the runtime makes for m.name keys (m["name"]) and single characters are made once and kept in consts.
// Per isolate, since they are in its heap: isolateMark keeps them.
static TLS std::vector<V> keyStrs;
static TLS V charStrs[256];
ISO_LOCAL(keyStrs);
ISO_LOCAL(charStrs);
static V cached(V& slot, std::string_view s) { return slot ? slot : (slot = box(newStr(s))); }
static V mapKey(uint64_t s) { return cached(growAt(keyStrs, s), symName(s)); }
// What a shape type checks: o's field or key named s, if it has one (a map's key; an instance's field, or its method).
bool fieldOf(V o, int s, V* out, bool* method) {
  if (is(o, K::Map)) {
    long i = mapFind(as<Map>(o), mapKey((uint64_t)s));
    return i >= 0 ? (*out = as<Map>(o)->e[i].second, true) : false;
  }
  if (!is(o, K::Inst)) return false;
  Inst* in = as<Inst>(o);
  if (u32 i = (u32)in->cls->layout.find(s); i < in->nslots && in->slots[i] != ABSENT) return *out = in->slots[i], true;
  if (const V* d = in->cls->declared(s)) return *out = *d, true;
  if (in->cls->find(s)) return *out = NIL, *method = true, true;  // a method: a shape's fn field accepts it
  return false;
}
void isolateMark(void (*mark)(V)) {
  for (V v : keyStrs) if (v) mark(v);
  for (V v : charStrs) if (v) mark(v);
}
static V rt_geti(V a, V i);
static V rt_seti(V a, V i, V v);
static V rt_getf(V o, uint64_t s, IC* ic) {
  if (is(o, K::Map)) return rt_geti(o, mapKey(s));
  if (is(o, K::Inst)) {
    Inst* in = as<Inst>(o);
    if (u32 i = (u32)in->cls->layout.find((int)s); i < in->nslots && in->slots[i] != ABSENT) {
      if (ic) *ic = {header(in), i};
      return in->slots[i];
    }
    if (const V* d = in->cls->declared((int)s)) return *d;  // never assigned (or declared since it was made): the default
    if (in->cls->closed()) ERR("@no-field %s has no field '%s'", symName(in->cls->name).c_str(), symName(s).c_str());
  }
  ERR("@no-field %s has no field '%s'", typeName(o), symName(s).c_str());
}
// What JIT constructors read from a class: a new instance has room for every field the class has had so far.
// Hold layoutMu. Isolates construct while it changes: the cell size is written (one word) before the header that
// says how many slots, and constructors read the header first (ldar), so a cell always has room for its slots.
void reshape(Class* c) {
  u32 cap = (u32)std::min<size_t>(c->layout.size(), 0xffff);
  int k = sizeClass(sizeof(Inst) + cap * 8);
  uint64_t run = u32((char*)&vm->runs[k] - (char*)vm) | (uint64_t)(k < NCLS ? sizeClasses[k] : 16) << 32;  // too big: runs[NCLS] is empty, so 16 never fits
  __atomic_store_n((uint64_t*)&c->irun, run, __ATOMIC_RELAXED);
  __atomic_store_n(&c->ihdr, (uint64_t)K::Inst | (uint64_t)cap << 16 | (uint64_t)c->id << 32, __ATOMIC_RELEASE);
}
static V rt_setf(V o, uint64_t s, V v, IC* ic) {
  bool outside = s & SETF_OUTSIDE;
  s &= ~SETF_OUTSIDE;
  if (is(o, K::Map)) return rt_seti(o, mapKey(s), v);
  if (!is(o, K::Inst)) ERR("@no-field cannot set field '%s' on %s", symName(s).c_str(), typeName(o));
  Inst* in = as<Inst>(o);
  touch(in);  // in a transaction: saved before its first change (the cache isn't filled, so every store comes here)
  if (in->cls->closed() && !in->cls->declared((int)s))
    ERR("@no-field %s has no field '%s' (its fields are declared: add 'var %s' to the class)", symName(in->cls->name).c_str(), symName(s).c_str(), symName(s).c_str());
  int f = in->cls->layout.find((int)s);
  if (f < 0) {
    Locked g(layoutMu);
    f = (int)in->cls->layout.add((int)s), reshape(in->cls);
  }
  u32 i = (u32)f;
  if (i >= in->nslots) {  // a field this instance had no room for: move the slots out of line
    u32 n = (u32)in->cls->layout.size();
    if (n > 0xffff) ERR("@limit too many fields");
    V* s = (V*)malloc(n * 8);
    std::copy(in->slots, in->slots + in->nslots, s), std::fill(s + in->nslots, s + n, ABSENT);
    if (in->slots != in->inl()) free(in->slots);
    else gcOwn(in);  // the GC frees the slots when the instance dies
    in->slots = s, in->nslots = (uint16_t)n;
  }
  const Class::FieldType* ty = in->cls->typed((int)s);
  if (ty && !typeAccepts(ty->type, v, ty->site)) typeFail(ty->site, v, RA(), ty->type);
  // A store from outside an instance whose class has invariants: the site checks them next (bit 33 of the cached slot
  // tells its fast path to ask the same, through vm->invObj).
  bool inv = outside && in->cls->find(invariantSym());
  if (inv) vm->invObj = o;
  // A `num` field's store sites check inline (bit 32 of the slot tells them to); other typed fields' stores come here.
  if (ic && !vm->txn && (!ty || ty->type == TY_NUM)) *ic = {header(in), i | (ty ? 1ull << 32 : 0) | (inv ? 1ull << 33 : 0)};
  return in->slots[i] = v;
}
#define INDEX(i, n) (isNum(i) && num(i) >= 0 && num(i) < (n) ? (size_t)num(i) : \
  (ERR("@index index %s out of range (len %zu)%s", toStr(i).c_str(), (size_t)(n), \
       isNum(i) && num(i) < 0 ? "; use l[l.len() - 1] for the last element" : ""), 0))
#define NUMARG(v, f) (isNum(v) ? num(v) : (ERR("@argument " f "() expects a number"), 0.0))
static V char1(V s, size_t i) { return cached(charStrs[(uint8_t)as<Str>(s)->s()[i]], {as<Str>(s)->s() + i, 1}); }
void mapSet(Map* m, V k, V v) {
  if ((m->used + 1) * 4 > m->idx.size() * 3) {  // grow (and drop deleted slots): rebuild the index
    m->idx.assign(std::bit_ceil(m->e.size() * 2 + 8), 0), m->used = (u32)m->e.size();
    for (u32 i = 0; i < m->e.size(); i++) *mapSlot(m, m->e[i].first) = i + 1;
  }
  u32* s = mapSlot(m, k);
  if (*s && *s != ~0u) return void(m->e[*s - 1].second = v);
  m->used += !*s, *s = (u32)m->e.size() + 1, m->e.push_back({k, v});
}
void mapPut(Map* m, std::string_view k, V v) {
  volatile V keep[] = {box(m), v};  // on the stack while the key allocates
  (void)keep, mapSet(m, box(newStr(k)), v);
}
static V rt_geti(V a, V i) {
  if (is(a, K::Map)) {
    long e = mapFind(as<Map>(a), i);
    return e < 0 ? NIL : as<Map>(a)->e[e].second;
  }
  if (!is(a, K::List) && !is(a, K::Str)) ERR("@index cannot index %s", typeName(a));
  if (is(a, K::List)) return as<List>(a)->v[INDEX(i, as<List>(a)->v.size())];
  return char1(a, INDEX(i, as<Str>(a)->len));
}
static V rt_seti(V a, V i, V v) {
  if (is(a, K::Map)) {
    if (obj(a)->typed) typeWrite(obj(a), i, v, RA());
    return touch(obj(a)), mapSet(as<Map>(a), i, v), v;
  }
  if (!is(a, K::List)) ERR("@index cannot assign into %s", typeName(a));
  auto& l = as<List>(a)->v;
  size_t at = INDEX(i, l.size());
  if (obj(a)->typed) typeWrite(obj(a), NIL, v, RA());
  return touch(obj(a)), l[at] = v;
}
static V rt_iter(V c, uint64_t i) {
  if (is(c, K::List)) return i < as<List>(c)->v.size() ? as<List>(c)->v[i] : END;
  if (is(c, K::Map)) return i < as<Map>(c)->e.size() ? as<Map>(c)->e[i].first : END;
  if (is(c, K::Chan)) return chanNext(c, RA());  // each value received, until it is closed and drained
  if (is(c, K::Fiber)) {  // each yielded value; the fiber's return value ends the loop
    V r = as<Fiber>(c)->state == FIB_DONE ? END : fiberResume(c, NIL);
    return as<Fiber>(c)->state == FIB_DONE ? END : r;
  }
  if (!is(c, K::Str)) ERR("@not-iterable cannot iterate over %s", typeName(c));
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
int invariantSym() { static int s = intern("inv@"); return s; }
static const char* methodPackage(V receiver, const std::string& method) {
  static const char* const lists[] = {"all", "any", "contains", "count", "filter", "find", "flatten",
    "indexOf", "map", "max", "min", "range", "reduce", "reverse", "sort", "sortable", "sortBy", "sum", "unique"};
  static const char* const strings[] = {"count", "endsWith", "lines", "lower", "padEnd", "padStart",
    "repeat", "replace", "reverse", "startsWith", "upper", "words"};
  if (is(receiver, K::List) && std::find(std::begin(lists), std::end(lists), method) != std::end(lists)) return "lists";
  if (is(receiver, K::Str) && std::find(std::begin(strings), std::end(strings), method) != std::end(strings)) return "strings";
  return nullptr;
}
static void* rt_prep(V* w, uint64_t n, uint64_t sym, V sup, IC* ic) {
  V r = w[0];
  Fn* f = nullptr;
  IC* ctor = sym ? nullptr : ic;
  if (sym && is(r, K::Map) && !builtin(K::Map, sym) && (int)sym != invariantSym()) {  // m.f(x) calls m["f"]
    long e = mapFind(as<Map>(r), mapKey(sym));
    if (e < 0) ERR("@not-callable map has no key '%s' to call", symName(sym).c_str());
    r = w[0] = as<Map>(r)->e[e].second, sym = 0;
  }
  if (sym) {  // method call (sym 0 is the keyword 'var', never a method name)
    if (Class* c = sup ? as<Class>(sup)->super : is(r, K::Inst) ? as<Inst>(r)->cls : nullptr; c && traceOn) traceLookup(c->name, (int)sym);
    if (sup) f = as<Class>(sup)->super ? as<Class>(sup)->super->find(sym) : nullptr;
    else if (is(r, K::Inst)) f = as<Inst>(r)->cls->find(sym);
    else if (isObj(r)) f = builtin(obj(r)->kind, sym);
    if (!f && (int)sym == invariantSym()) {  // no invariant to check: the receiver is the result
      if (ic && is(r, K::Inst) && !sup) *ic = {header(obj(r)), (uint64_t)identCode};
      return identCode;
    }
    if (!f) {
      const auto& method = symName(sym);
      if (const char* pkg = methodPackage(r, method))
        ERR("@no-method %s has no method '%s'; import \"std/%s\" and use %s.%s%s",
            typeName(r), method.c_str(), pkg, pkg, method.c_str(), method == "sort" ? "(l)" : "");
      ERR("@no-method %s has no method '%s'", typeName(r), method.c_str());
    }
    if (ic && is(r, K::Inst) && f->arity == (int)n + 1) *ic = {header(obj(r)), (uint64_t)f->code};
  } else if (is(r, K::Fn)) {
    f = as<Fn>(r);
    if (f->op) return effPrep(f, w, n);  // an effect's operation: a handler, the builtin, or an error
    if (f->arity != (int)n) {
      if (traceOn) traceCalled(f->name);
      ERR("@arity %s expects %d arguments, got %d", symName(f->name).c_str(), f->arity, (int)n);
    }
    memmove(w, w + 1, n * 8);
    return f->code;
  } else if (is(r, K::Clo)) f = as<Clo>(r)->fn;
  else if (is(r, K::Class)) {
    Class* c = as<Class>(r);
    if (traceOn) traceLookup(c->name, S_init);
    if (!__atomic_load_n(&c->ihdr, __ATOMIC_ACQUIRE)) {  // its first construction: what JIT constructors read
      Locked g(layoutMu);
      reshape(c);
    }
    u32 cap = (u32)std::min<size_t>(c->layout.size(), 0xffff);
    Inst* in = make<Inst>(cap * 8);
    in->cls = c, in->cid = c->id, in->nslots = (uint16_t)cap, in->slots = in->inl();
    for (V* p = in->slots; p < in->slots + cap; p++) __atomic_store_n(p, ABSENT, __ATOMIC_RELAXED);  // no memset call
    w[0] = box(in);
    if (__atomic_load_n(&c->initEpoch, __ATOMIC_ACQUIRE) != icEpoch)  // isolates may race to fill it: same value
      __atomic_store_n(&c->init, c->find(S_init), __ATOMIC_RELAXED), __atomic_store_n(&c->initEpoch, icEpoch, __ATOMIC_RELEASE);
    if (!(f = __atomic_load_n(&c->init, __ATOMIC_RELAXED))) {
      if (n) ERR("@arity %s takes no arguments (no init)", symName(as<Class>(r)->name).c_str());
      if (ctor) *ctor = {r, (uint64_t)identCode};
      return identCode;
    }
    if (f->arity != (int)n + 1) ERR("@arity %s expects %d arguments, got %d", symName(f->name).c_str(), f->arity - 1, (int)n);
    if (ctor) *ctor = {r, (uint64_t)f->code};  // the site allocates inline from now on (until the next reload)
    return f->code;
  } else ERR("@not-callable cannot call %s", typeName(r));
  if (f->arity != (int)n + 1) ERR("@arity %s expects %d arguments, got %d", symName(f->name).c_str(), f->arity - 1, (int)n);
  return f->code;
}
static V rt_numchk(V v) { return isNum(v) ? v : (ERR("@operand expected a number, got %s", typeName(v)), v); }
static void rt_poll();
static void rt_stack() {  // wasm: the entry check also fails while a safepoint is wanted (gen_wasm.h)
  char here;
  if (WASM && vm->poll && &here + 256 >= (char*)vm->limit) return rt_poll();
  ERR("@stack-overflow stack overflow");
}
static void rt_undef() { ERR("@not-callable call to undefined function"); }
static void rt_stale(uint64_t sym, void* ra) {
  rtError(ra, "@stale-call stale call to '%s': its parameters changed, but this caller is old code that is still running",
          symName(sym).c_str());
}

// ---------------- fibers and C calls: the parts both platforms share ----------------
V vmCall(void* code, std::initializer_list<V> a) {
  V args[8] = {};
  std::copy(a.begin(), a.end(), args);
  return vmEnter(code, args);
}
V fiberCall(Fiber* f) {
  return is(f->fn, K::Clo) ? vmCall(as<Clo>(f->fn)->fn->code, {f->fn, f->val}) : vmCall(as<Fn>(f->fn)->code, {f->val});
}
V newFiber(V fn) {
  Fn* f = is(fn, K::Clo) ? as<Clo>(fn)->fn : is(fn, K::Fn) ? as<Fn>(fn) : (ERR("@fiber fiber() expects a function"), nullptr);
  if (f->arity - is(fn, K::Clo) > 1) ERR("@arity a fiber's function takes at most one parameter (the first value sent)");
  Fiber* b = make<Fiber>();
  b->fn = fn, b->val = NIL, fiberStack(b);
  b->allowed = vm->allowed, b->who = vm->who, b->hstack = NIL, b->inv = 0;  // it may do what its creator may, and has its own handlers
  return box(b);
}
V fiberResume(V fv, V v) {  // run fv until it yields (giving the yielded value) or returns (its result)
  Fiber* f = as<Fiber>(fv);
  if (f->state == FIB_DONE) ERR("@fiber resume of a finished fiber");
  if (f->state == FIB_RUNNING) ERR("@fiber resume of a running fiber");
  bool start = f->state == FIB_NEW;
  f->val = v, f->resumer = fiberCur, f->state = FIB_RUNNING;
  EffState mine = effSave();  // each fiber has its own effect state (allowed effects, handlers)
  volatile V keep = mine.hstack;
  (void)keep;
  effLoad({f->allowed, f->hstack, f->who, f->inv});
  fiberRun(f, start);
  f->allowed = vm->allowed, f->hstack = vm->hstack, f->who = vm->who, f->inv = vm->inv;
  effLoad(mine);
  if (f->state == FIB_DONE) {
    fiberFree(f);
    if (int r = f->fail) {
      V e = f->val;
      f->fail = 0, f->val = NIL;
      if (r == 2) restartMain();
      throwValue(e, RA());  // catchable here; else reported with both traces
    }
  }
  return std::exchange(f->val, NIL);
}
V rt_yield(V v) {
  bool handled;
  V r = effYield(v, &handled);  // a handler answers in place; otherwise this code must be allowed to yield
  if (handled) return r;
  Fiber* f = fiberCur;
  if (!f) ERR("@fiber yield outside a fiber");
  if (txnOpenIn(f)) ERR("@atomic yield inside atomic { }: other code would run while it is half done (yield after it ends)");
  f->val = v, f->state = FIB_SUSPENDED;
  fiberYield(f);
  return std::exchange(f->val, NIL);  // sent by the next resume
}
char ffiCheck(Ext* e, int i, V v) {
  char t = e->sig[i];
  if (t == 'a') return t;  // any: toJs checks what it can pass
  if (t == 'f' ? !is(v, K::Fn) && !is(v, K::Clo) : t == 's' ? !is(v, K::Str) : !isNum(v) || (t != 'd' && !(std::fabs(num(v)) < 9.2e18)))
    ERR("@argument %s() argument %d: expected %s, got %s", symName(e->sym).c_str(), i + 1, t == 'f' ? "a function" : t == 's' ? "a string" : "a number", typeName(v));
  return t;
}
u32* ffiThunk(void* fn, const std::string& sig, char ret, int sym) {
  static std::unordered_map<std::string, u32*> made;  // one per function and signature, kept across reloads
  u32*& t = made[sig + ret + symName(sym) + std::to_string((uintptr_t)fn)];
  if (t) return t;
  Ext* e = new Ext{fn, (int)sig.size(), sym, ret, {}};
  memcpy(e->sig, sig.data(), sig.size());
  return t = ffiCode(e);
}
static void rt_poll() {
  // (atomic: another thread sets bits, the ticker for one)
  if (isolateStopped()) __atomic_store_n(&vm->poll, 0, __ATOMIC_RELAXED), ERR("@isolate stopped: the isolate that started this one ended with an error");
  u32 bits = __atomic_load_n(&vm->poll, __ATOMIC_RELAXED);
  if (bits & 16) isolateReloadPoint();  // a reload stops this isolate
  if (bits & 8) isolateYield();         // a pool isolate's turn is up
  if (pollHook) pollHook();
}

static V rt_echo(V v) {
  if (v != NIL) printf("%s\n", toStr(v, true).c_str());
  return v;
}

// ---------------- natives ----------------
static Mutex outMu;  // an isolate's workers print to its outSink too
void emitOut(std::string_view s) {
  if (outSink) { Locked l(outMu); outSink->append(s).push_back('\n'); return; }
  flockfile(stdout), fwrite(s.data(), 1, s.size(), stdout), putc_unlocked('\n', stdout), funlockfile(stdout);
}
static V n_print(V v) {
  isolatePoint();  // seeded isolates: whose line comes first is one of the choices
  std::string t;
  return emitOut(text(v, t)), NIL;
}
static V rt_printArgs(V* w, uint64_t n) {
  if (n == 1 && is(w[0], K::Str)) return w[0];
  std::string out;
  std::unordered_set<Obj*> active;
  for (uint64_t i = 0; i < n; i++) {
    if (i) out += ' ';
    render(out, w[i], false, active);
  }
  return box(newStr(out));
}
V binopValue(V a, V b, int op) { return rt_binop(a, b, op); }
static V n_str(V v) { return is(v, K::Str) ? v : box(newStr(toStr(v))); }
static V n_len(V v) {
  if (is(v, K::Str)) return box((double)as<Str>(v)->len);
  if (is(v, K::List)) return box((double)as<List>(v)->v.size());
  if (is(v, K::Map)) return box((double)as<Map>(v)->e.size());
  ERR("@argument len() of %s", typeName(v));
}
double now() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static V n_clock() { return box(now()); }
// Strings and lists: slice, find, split, trim, join, and parseNum. Indexes count bytes, as len() and s[i] do.
static std::string_view svArg(V v, const char* fn) {
  if (!is(v, K::Str)) ERR("@argument %s expects a string, got %s", fn, typeName(v));
  return as<Str>(v)->sv();
}
static size_t clampIx(V v, size_t n, const char* fn) {
  if (!isNum(v)) ERR("@argument %s expects numbers, got %s", fn, typeName(v));
  double d = num(v);
  if (d != std::trunc(d)) ERR("@index %s: an index must be a whole number, got %g", fn, d);
  return d < 0 ? 0 : d > (double)n ? n : (size_t)d;
}
static V s_slice(V s, V a, V b) {  // s.slice(from, to): from..to exclusive, clamped to the string
  volatile V held = s;  // on the stack while newStr allocates: v points into it
  (void)held;
  std::string_view v = as<Str>(s)->sv();
  size_t i = clampIx(a, v.size(), "slice()"), j = clampIx(b, v.size(), "slice()");
  return box(newStr(i < j ? v.substr(i, j - i) : std::string_view{}));
}
static V s_find(V s, V sub) {  // the index of the first sub, or -1
  size_t i = as<Str>(s)->sv().find(svArg(sub, "find()"));
  return box(i == std::string_view::npos ? -1.0 : (double)i);
}
static V s_split(V s, V sep) {
  volatile V held = s;  // on the stack while newStr allocates: v points into it
  (void)held;
  std::string_view v = as<Str>(s)->sv(), d = svArg(sep, "split()");
  if (d.empty()) ERR("@argument split() needs a separator that isn't empty");
  List* l = make<List>();
  volatile V keep = box(l);
  (void)keep;
  for (size_t i = 0;;) {
    size_t j = v.find(d, i);
    V part = box(newStr(v.substr(i, j == std::string_view::npos ? std::string_view::npos : j - i)));
    l->v.push_back(part);
    if (j == std::string_view::npos) break;
    i = j + d.size();
  }
  return box(l);
}
static V s_trim(V s) {
  volatile V held = s;  // on the stack while newStr allocates: v points into it
  (void)held;
  std::string_view v = as<Str>(s)->sv();
  auto ws = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
  while (!v.empty() && ws(v.front())) v.remove_prefix(1);
  while (!v.empty() && ws(v.back())) v.remove_suffix(1);
  return box(newStr(v));
}
static V l_join(V l, V sep) {  // strings as they are, other values as str() shows them
  volatile V held = l;
  (void)held;
  std::string_view d = svArg(sep, "join()");
  std::string o;
  bool first = true;
  for (V x : as<List>(l)->v) {
    if (!first) o += d;
    first = false;
    if (is(x, K::Str)) o += as<Str>(x)->sv();
    else o += toStr(x);
  }
  return box(newStr(o));
}
static V l_slice(V l, V a, V b) {
  volatile V held = l;
  (void)held;
  auto& v = as<List>(l)->v;
  size_t i = clampIx(a, v.size(), "slice()"), j = clampIx(b, v.size(), "slice()");
  List* r = make<List>();
  if (i < j) r->v.assign(as<List>(l)->v.begin() + i, as<List>(l)->v.begin() + j);
  return box(r);
}
static V n_slice(V v, V a, V b) {
  if (is(v, K::Str)) return s_slice(v, a, b);
  if (is(v, K::List)) return l_slice(v, a, b);
  ERR("@index cannot slice %s; use a string or list", typeName(v));
}
static V n_pow(V a, V b) {
  if (!isNum(a) || !isNum(b)) ERR("@operand '**' expects numbers, got %s and %s", typeName(a), typeName(b));
#if __wasm__
  return box(host_pow(num(a), num(b)));
#else
  return box(std::pow(num(a), num(b)));
#endif
}
static V n_parseNum(V s) {  // the number a string spells (surrounding spaces allowed), or nil
  std::string t(svArg(s, "parseNum()"));
  const char* p = t.c_str();
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
  if (!*p) return NIL;
  char* end;
  double d = strtod(p, &end);
  if (end == p) return NIL;
  while (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r') end++;
  return *end ? NIL : box(d);
}
std::string_view strViewArg(V v, const char* fn, const char* what) {
  if (!is(v, K::Str)) ERR("@argument %s(): %s must be a string, got %s", fn, what, typeName(v));
  return as<Str>(v)->sv();
}
double numArg(V v, const char* fn, const char* what) {
  if (!isNum(v) || num(v) <= 0) ERR("@argument %s(): %s must be a positive number", fn, what);
  return num(v);
}
FetchRequest fetchRequest(V v, const char* fn) {
  FetchRequest r;
  if (is(v, K::Str)) return r.url = strViewArg(v, fn, "the url"), r;
  if (!is(v, K::Map)) ERR("@argument %s() expects a url or {url, method, headers, body, timeout}, got %s", fn, typeName(v));
  for (auto& [k, x] : as<Map>(v)->e) {
    std::string_view key = strViewArg(k, fn, "an option name");
    if (key == "url") r.url = strViewArg(x, fn, "url");
    else if (key == "method") r.method = strViewArg(x, fn, "method");
    else if (key == "body") r.body = strViewArg(x, fn, "body");
    else if (key == "timeout") r.timeout = numArg(x, fn, "timeout");
    else if (key == "headers") {
      if (!is(x, K::Map)) ERR("@argument %s(): headers must be a map of strings", fn);
      r.headers = as<Map>(x);
      for (auto& [hk, hv] : r.headers->e) strViewArg(hk, fn, "a header name"), strViewArg(hv, fn, "a header value");
    } else ERR("@argument %s(): unknown option '%s' (url, method, headers, body, timeout)", fn, key.data());
  }
  if (r.url.empty()) ERR("@argument %s(): no url", fn);
  return r;
}
static V n_time() {  // seconds since 1970
  timespec t;
  clock_gettime(CLOCK_REALTIME, &t);
  return box(t.tv_sec + t.tv_nsec / 1e9);
}
static V n_json(V v) { return box(newStr(toJs(v))); }  // JSON text, as for JavaScript
static V n_parseJson(V s) {
  if (!is(s, K::Str)) ERR("@argument parseJson() expects a string, got %s", typeName(s));
  return fromJs(std::string(as<Str>(s)->sv()));
}
static V n_sleep(V v) { return sleepFor(NUMARG(v, "sleep")), NIL; }
static V n_floor(V v) { return box(std::floor(NUMARG(v, "floor"))); }
static V n_sqrt(V v) { return box(std::sqrt(NUMARG(v, "sqrt"))); }
static V l_push(V l, V v) {
  if (obj(l)->typed) typeWrite(obj(l), NIL, v, RA());
  touch(obj(l)), as<List>(l)->v.push_back(v);
  return l;
}
static V l_pop(V l) {
  auto& v = as<List>(l)->v;
  if (v.empty()) ERR("@index pop() from empty list");
  touch(obj(l));
  V x = v.back();
  return v.pop_back(), x;
}
static V m_has(V m, V k) { return boxb(mapFind(as<Map>(m), k) >= 0); }
static V m_remove(V m, V k) {  // the last entry moves into the hole
  Map* p = as<Map>(m);
  long i = mapFind(p, k);
  if (i < 0) return NIL;
  touch(p);
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
  vm->globals[g.slot] = box(g.o = nativeFn(name, f, arity));
}
void method(K k, int sym, void* f, int arity) { consts.push_back(box(growAt(builtins[(int)k], sym) = nativeFn(symName(sym).c_str(), f, arity))); }

void rtInit() {
  {  // the list fast paths in JIT code read a std::vector directly (see LIST_V)
    List l;
    l.v.reserve(8), l.v.push_back(1), l.v.push_back(2);
    V* const* p = (V* const*)((char*)&l + LIST_V);
    V* d = l.v.data();
    vecCount = (uintptr_t)p[1] == 2;
    if (p[0] != d || p[1] != (vecCount ? (V*)2 : d + 2) || p[2] != (vecCount ? (V*)8 : d + 8))
      fputs("internal: unexpected std::vector layout\n", stderr), abort();
  }
  static const char* syms[] = {KEYWORDS(SYM_STR) EXTRA_SYMS(SYM_STR)};
  for (const char* s : syms) intern(s);
  vm = vmNew();  // the main isolate's
  gcInit();
  heapUse(heapNew(vm->runs));
  ImageScope image;  // the natives, and what the runtime keeps for good
  void* h[NH] = {(void*)rt_poll, (void*)rt_stack, (void*)rt_binop, (void*)rt_getf, (void*)rt_setf, (void*)rt_geti,
                 (void*)rt_seti, (void*)rt_prep, (void*)rt_list, (void*)rt_iter, (void*)rt_numchk, (void*)rt_undef,
                 (void*)rt_map, (void*)rt_closure, (void*)rt_stale, (void*)rt_echo, nullptr, (void*)rt_yield, nullptr, (void*)rt_throw};
  h[H_PRINTARGS] = (void*)rt_printArgs;
  memcpy(vm->helpers, h, sizeof h);  // H_FFI and H_HOT: platformInit
  vm->helpers[H_FORMAT] = (void*)rt_format;
  identCode = callable<+[](V self) { return self; }>();
  platformInit();
  txnInit();
  typesInit();
  native<n_print>("print"), native<n_str>("str"), native<n_len>("len"), native<n_clock>("clock"), native<n_sleep>("sleep");
  native<+[](V v) {
    if (is(v, K::Str)) ERR("@argument int() expects a number; use parseNum(s) to parse a string");
    return box(std::trunc(NUMARG(v, "int")));
  }>("int");
  native<n_slice>("slice@"), native<n_pow>("pow@");
  native<n_floor>("floor"), native<n_sqrt>("sqrt"), native<newFiber>("fiber");
  native<n_time>("time"), native<n_json>("json"), native<n_parseJson>("parseJson");
  method<+[](V f) { return fiberResume(f, NIL); }>(K::Fiber, S_resume), method<fiberResume>(K::Fiber, S_send);
  method<+[](V f) { return boxb(as<Fiber>(f)->state == FIB_DONE); }>(K::Fiber, S_done);
  method<l_push>(K::List, S_push), method<l_pop>(K::List, S_pop), method<n_len>(K::List, S_len);
  method<n_len>(K::Str, S_len), method<n_len>(K::Map, S_len);
  gfxInit();
  method<m_has>(K::Map, S_has), method<m_remove>(K::Map, S_remove);
  method<+[](V s, V sub) {  // str.has(part): does it contain part?
    if (!is(sub, K::Str)) ERR("@argument has() on a string expects a string, got %s", typeName(sub));
    return boxb(as<Str>(s)->sv().find(as<Str>(sub)->sv()) != std::string_view::npos);
  }>(K::Str, S_has);
  method<s_slice>(K::Str, intern("slice")), method<s_find>(K::Str, intern("find")), method<s_split>(K::Str, intern("split"));
  method<s_trim>(K::Str, intern("trim")), method<l_join>(K::List, intern("join")), method<l_slice>(K::List, intern("slice"));
  native<n_parseNum>("parseNum");
  // match: is@(value, variant class) tests an arm; match@fail(value, enum) is reached when no arm takes the value
  native<+[](V v, V c) { return boxb(is(v, K::Inst) && is(c, K::Class) && as<Inst>(v)->cls->name == as<Class>(c)->name); }>("is@");
  native<+[](V v, V e) {
    ERR("@match match on %s got %s, which is not one of its variants", symName(as<Class>(e)->name).c_str(), toStr(v, true).c_str());
    return NIL;
  }>("match@fail");
  method<+[](V m) { return mapList(m, true); }>(K::Map, S_keys);
  method<+[](V m) { return mapList(m, false); }>(K::Map, S_values);
  testInit();
  sysInit();
  isolatesInit();
  effectsInit();  // after the natives it marks as operations
}
