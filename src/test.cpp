// Tests: the expect@ native behind `expect`, and the runner behind `fut test` (shared by the native and wasm drivers).
#include "fut.h"
#include <cmath>

// Shared by every isolate: tier-up registers sites again while others run, so a deque (its elements never move) and a
// lock around growth and lookup.
static std::deque<ExpectSite> sites;
static Mutex sitesMu;
int expectSite(ExpectSite s) { Locked l(sitesMu); return sites.push_back(std::move(s)), (int)sites.size() - 1; }
static const ExpectSite& siteAt(size_t i) { Locked l(sitesMu); return sites.at(i); }

// The first expect that failed in the running test. Kept even if the test catches the error expect throws.
struct Failure { bool set = false; int site = -1; std::string why, left, right, at, what; };  // at/what: where lists or maps differ

// expect's == compares lists and maps by contents (key order aside). On a mismatch inside them, `at` gets the
// path to the first difference and what differs there, e.g. `.items[1].qty: 3 vs 4`.
static std::string key(V k) {
  if (is(k, K::Str)) {
    auto s = as<Str>(k)->sv();
    bool name = !s.empty() && !(s[0] >= '0' && s[0] <= '9');
    for (char c : s) name &= (c | 32) >= 'a' && (c | 32) <= 'z' ? true : c == '_' || (c >= '0' && c <= '9');
    if (name) return "." + std::string(s);
  }
  return "[" + toStr(k, true) + "]";
}
using Compared = std::vector<std::pair<V, V>>;
static bool same(V a, V b, std::string& at, std::string& what, Compared& active) {
  if (isNum(a) && isNum(b)) return num(a) == num(b);
  if (is(a, K::Str) && is(b, K::Str)) return as<Str>(a)->sv() == as<Str>(b)->sv();
  auto differ = [&](std::string seg, V x, V y) {
    if (what.empty()) what = toStr(x, true) + " vs " + toStr(y, true);
    return at = seg + at, false;
  };
  bool lists = is(a, K::List) && is(b, K::List);
  bool maps = is(a, K::Map) && is(b, K::Map);
  if (!lists && !maps) return a == b;
  std::pair pair{a, b};
  for (auto p : active) if (p == pair) return true;
  active.push_back(pair);
  struct Pop { Compared& active; ~Pop() { active.pop_back(); } } pop{active};
  if (lists) {
    auto &x = as<List>(a)->v, &y = as<List>(b)->v;
    for (size_t i = 0; i < x.size() && i < y.size(); i++)
      if (!same(x[i], y[i], at, what, active)) return differ("[" + std::to_string(i) + "]", x[i], y[i]);
    if (x.size() != y.size()) return what = "length " + std::to_string(x.size()) + " vs " + std::to_string(y.size()), false;
    return true;
  }
  auto &x = as<Map>(a)->e, &y = as<Map>(b)->e;
  auto find = [&active](auto& e, V k) -> V* {
    std::string i1, i2;
    for (auto& [k2, v] : e) if (same(k, k2, i1, i2, active)) return &v;
    return nullptr;
  };
  for (auto& [k, v] : x) {
    V* w = find(y, k);
    if (!w) return what = "only on the left", at = key(k), false;
    if (!same(v, *w, at, what, active)) return differ(key(k), v, *w);
  }
  for (auto& [k, v] : y) if (!find(x, k)) return what = "only on the right", at = key(k), false;
  return true;
}
static TLS Failure failure;
ISO_LOCAL(failure);

static std::string oneLine(std::string s) {  // an expression's source on one line: a line break and its indent become one space
  std::string o;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] != '\n' && s[i] != '\r') { o += s[i]; continue; }
    while (!o.empty() && (o.back() == ' ' || o.back() == '\t')) o.pop_back();
    while (i + 1 < s.size() && (s[i + 1] == ' ' || s[i + 1] == '\t' || s[i + 1] == '\n' || s[i + 1] == '\r')) i++;
    o += ' ';
  }
  return o;
}
// "x is 10" lines worth showing: a side whose source is just its value (a literal) says nothing new.
static std::vector<std::string> details(const ExpectSite& s, const Failure& f) {
  std::vector<std::string> d;
  if (!f.why.empty()) d.push_back(f.why);
  auto bare = [](const std::string& x) { std::string o; for (char c : x) if (c != ' ' && c != '\n' && c != '\t' && c != '"') o += c; return o; };
  auto side = [&](const std::string& src, const std::string& val) { if (!src.empty() && bare(src) != bare(val)) d.push_back(oneLine(src) + " is " + val); };
  if (f.why.empty()) side(s.left, f.left);
  if (s.op >= 0 && f.why.empty()) side(s.right, f.right);
  if (!f.what.empty()) d.push_back(f.at.empty() ? f.what : "first difference at " + f.at + ": " + f.what);
  return d;
}

// expect@(site, left, right): checks left op right (or left's truthiness); a failure is recorded and raised.
static V expectAt(V site, V l, V r) {
  const ExpectSite& s = siteAt((size_t)num(site));
  Failure f;
  bool ok;
  if (s.throws && l == ABSENT) ok = false, f.why = "nothing was thrown";
  else if (s.op < 0) ok = s.throws || (l != NIL && l != FALSE_);
  else if (s.op == O_EQ || s.op == O_NE) {
    Compared active;
    ok = same(l, r, f.at, f.what, active) == (s.op == O_EQ);
    if (ok || s.op == O_NE) f.at.clear(), f.what.clear();
  } else if (isNum(l) && isNum(r)) {  // compiled code compares numbers inline; binopValue expects not to see them
    double x = num(l), y = num(r);
    ok = s.op == O_LT ? x < y : s.op == O_LE ? x <= y : s.op == O_GT ? x > y : x >= y;
  } else { V c = binopValue(l, r, s.op); ok = c != NIL && c != FALSE_; }
  if (ok) return NIL;
  f.set = true, f.site = (int)num(site);
  if (l != ABSENT) f.left = toStr(l, true);
  if (s.op >= 0) f.right = toStr(r, true);
  std::string msg = "expect failed: " + oneLine(s.text);
  auto d = details(s, f);
  for (size_t i = 0; i < d.size(); i++) msg += (i ? ", " : " (") + d[i] + (i + 1 == d.size() ? ")" : "");
  if (!failure.set) failure = f;
  ERR("@expect %s", msg.c_str());
}
// contract@fail(site, left, right, who): a contract's clause was false (the compiled code checked it). Raises an error
// saying whose fault it is (the site's contract: "the caller broke f()'s contract: requires"), the condition and its
// values. An invariant's site starts with \x01, for who (the method whose call just ended, or a store to a field).
static V contractFail(V site, V l, V r, V who) {
  const ExpectSite& s = sites.at((size_t)num(site));
  Failure f;
  f.left = toStr(l, true);
  if (s.op >= 0) f.right = toStr(r, true);
  std::string head = s.contract;
  if (head.starts_with('\x01')) head = (who != NIL ? toStr(who) : "a call") + head.substr(1);
  std::string msg = head + " " + oneLine(s.text);
  auto d = s.op >= 0 ? details(s, f) : std::vector<std::string>{};  // (a condition checked as one: "is false" adds nothing)
  for (size_t i = 0; i < d.size(); i++) msg += (i ? ", " : " (") + d[i] + (i + 1 == d.size() ? ")" : "");
  // whose fault: the caller's (requires), the function's (ensures), or the class's (an invariant)
  ERR(s.contract.ends_with(": requires") ? "@requires %s" : s.contract.ends_with(": ensures") ? "@ensures %s" : "@invariant %s", msg.c_str());
}
// expect allocs { ... }: allocs@begin() before the block, allocs@end(what it returned) after: objects allocated between.
static V allocsBegin() { return box((double)gcAllocations()); }
static V allocsEnd(V start) { return box((double)gcAllocations() - num(start)); }

// expect grows [allocs] { n -> ... } like "n^2": grows@(site, body, growth, by allocations). The growth is an upper
// bound: n^k, optionally times log n ("1", "log n", "n", "n log n", "n^2", ...). The body runs for sizes n that double,
// and the check fits how its cost grows on a log-log scale: it fails if, divided by the growth, the cost still grows
// (slope over tolerance). Time is the best of 3 runs of a batch lasting 2 ms or more, from the n at which one call
// takes 50 us, measured afresh up to twice before it fails; allocations are counted for n = 64..2048 (exact, so their tolerance is tight).
// With FUT_UNTIMED set (build.py tsan, where the sanitizer and N isolates per core bend every timing), time isn't
// measured: those checks pass, and the summary says they weren't measured. Allocation checks still run. The same
// goes where the clock stands still while code runs (a Cloudflare Worker's moves only across I/O): the host says so
// (fut_untimed: web/worker-runtime.js), and a clock that doesn't move across 65536 calls is taken to have stopped.
namespace {
struct Growth { double k = 0; bool lg = false; };
bool parseGrowth(std::string_view m, Growth& g) {
  auto trim = [](std::string_view& v) { while (!v.empty() && v.front() == ' ') v.remove_prefix(1); while (!v.empty() && v.back() == ' ') v.remove_suffix(1); };
  trim(m);
  if (m == "1") return g = {0, false}, true;
  if (m.ends_with("log n")) g.lg = true, m.remove_suffix(5), trim(m);
  if (m.empty() && g.lg) return g.k = 0, true;
  if (m.empty() || m[0] != 'n') return false;
  m.remove_prefix(1);
  if (m.empty()) return g.k = 1, true;
  if (m[0] != '^' || m.size() < 2) return false;
  std::string e(m.substr(1));
  char* end;
  g.k = strtod(e.c_str(), &end);
  return *end == 0 && g.k >= 0 && g.k <= 10;
}
std::string growthName(const Growth& g) {
  char b[32];
  snprintf(b, sizeof b, "n^%g", g.k);
  std::string s = g.k == 0 ? "" : g.k == 1 ? "n" : b;
  return g.lg ? (s.empty() ? "log n" : s + " log n") : s.empty() ? "1" : s;
}
double slope(const std::vector<std::pair<double, double>>& p, Growth g) {  // of log(cost / growth) against log n
  double sx = 0, sy = 0, sxx = 0, sxy = 0, n = (double)p.size();
  for (auto [x, y] : p) {
    double lx = log(x), ly = log(std::max(y, 1e-12)) - g.k * lx - (g.lg ? log(lx) : 0);
    sx += lx, sy += ly, sxx += lx * lx, sxy += lx * ly;
  }
  return (n * sxy - sx * sy) / (n * sxx - sx * sx);
}
}  // namespace
static bool clockStopped = false;
void setUntimed(bool on) { clockStopped = on; }
#if __wasm__
static bool untimed() { return clockStopped; }  // no environment there (the host gives the module none)
#else
static bool untimed() { static bool u = getenv("FUT_UNTIMED"); return u || clockStopped; }
#endif
static V growsAt(V site, V clo, V growth, V byAllocs) {
  const ExpectSite& s = sites.at((size_t)num(site));
  Growth want;
  if (!is(growth, K::Str) || !parseGrowth(as<Str>(growth)->sv(), want))
    ERR("@argument expect grows: '%s' is not a growth (\"1\", \"log n\", \"n\", \"n log n\", \"n^2\", \"n^3\")", toStr(growth, false).c_str());
  volatile V keep = clo;
  (void)keep;
  Fn* f = as<Clo>(clo)->fn;
  auto call = [&](double n) { V a[8] = {clo, box(n)}; vmEnter(f->code, a); };
  bool allocs = byAllocs == TRUE_;
  if (!allocs && untimed()) return NIL;
  std::vector<std::pair<double, double>> pts;
  if (allocs)
    for (double n = 64; n <= 2048; n *= 2) {
      uint64_t a0 = gcAllocations();
      call(n);
      pts.push_back({n, (double)(gcAllocations() - a0)});
    }
  else {
    traceImpure();  // timed: not a result fut test --changed can reuse
    auto measure = [&](double n) {
      double best = 1e30;
      for (int k = 0; k < 3; k++)
        for (int reps = 1;; reps *= 2) {
          double t0 = now();
          for (int i = 0; i < reps; i++) call(n);
          double dt = now() - t0;
          if (dt == 0 && reps >= 1 << 16) return clockStopped = true, 0.0;
          if (dt >= 0.002 || reps >= 1 << 20) { best = std::min(best, dt / reps); break; }
        }
      return best;
    };
    for (int attempt = 0; attempt < 3; attempt++) {  // a machine under load can bend one fit; a wrong growth bends every one
      pts.clear();
      double n = 16, t = measure(n), start = now();
      while (!clockStopped && t < 50e-6 && n < (1 << 20)) t = measure(n *= 2);
      for (pts.push_back({n, t}); !clockStopped && pts.size() < 5 && t < 0.25 && now() - start < 3; pts.push_back({n, t})) t = measure(n *= 2);
      if (clockStopped || pts.size() < 3 || slope(pts, want) <= 0.4) return NIL;
    }
  }
  double tol = allocs ? 0.1 : 0.4;
  if (pts.size() < 3 || slope(pts, want) <= tol) return NIL;
  Growth seen{-1};
  for (Growth g : {Growth{0}, {0, true}, {1}, {1, true}, {2}, {2, true}, {3}})
    if (slope(pts, g) <= tol) { seen = g; break; }
  char b[64];
  snprintf(b, sizeof b, "n^%.1f", slope(pts, {}));
  Failure fl;
  fl.set = true, fl.site = (int)num(site);
  fl.why = std::string(allocs ? "its allocations grow like " : "it grows like ") + (seen.k < 0 ? b : growthName(seen)) + ", not " + growthName(want) + " (n = ";
  for (size_t i = 0; i < pts.size(); i++) {
    char v[48];
    snprintf(v, sizeof v, allocs ? "%s%g: %.0f" : "%s%g: %.3g ms", i ? ", " : "", pts[i].first, allocs ? pts[i].second : pts[i].second * 1e3);
    fl.why += v;
  }
  fl.why += ")";
  if (!failure.set) failure = fl;
  ERR("@expect expect failed: %s (%s)", oneLine(s.text).c_str(), fl.why.c_str());
}
// ---- property tests from specs (the compiler writes the loop: see Compiler::specProperties) ----
static std::deque<PropSite> props;  // shared by every isolate, like the expect sites
static Mutex propsMu;
int propSite(PropSite p) {
  p.expect = expectSite({p.file, p.text, "", "", p.line});
  Locked l(propsMu);
  return props.push_back(std::move(p)), (int)props.size() - 1;
}
namespace {
TLS uint64_t prng = 1;  // per isolate: each runs its property tests from the same seed
ISO_LOCAL(prng);
uint64_t rnd() { return prng ^= prng >> 12, prng ^= prng << 25, prng ^= prng >> 27, prng * 0x2545F4914F6CDD1Dull; }
int below(int n) { return n <= 0 ? 0 : (int)(rnd() % (uint64_t)n); }
double unit() { return double(rnd() >> 11) * 0x1p-53; }
// A random value of type t; size (1..50) grows with the input's number, so the first inputs are small ones.
V gen(uint64_t t, int size, int depth = 0) {
  if ((t & TY_OPT) && !below(8)) return NIL;
  switch (t & 15) {
    case TY_NUM: {
      static const double edge[] = {0, 1, -1, 2, -2, 0.5, -0.5, 3, 10, -10, 100, 1000, 1e6, -1e6, 0.1, 255, 256};
      switch (below(4)) {
        case 0: return box(edge[below(sizeof edge / sizeof *edge)]);
        case 1: return box(double(below(2 * size + 1) - size));
        case 2: return box(std::round((unit() * 2 - 1) * size * 100) / 100);
        default: return box(double(below(2 * size * size + 1) - size * size));
      }
    }
    case TY_STR: {
      static const char* const parts[] = {"a", "b", "c", "x", "y", "z", "A", "Z", "0", "1", "9", " ", "_", "-", ".", ",", "\"", "\\", "\n", "\xc3\xa9"};
      std::string str;
      for (int i = below(std::min(size, 12) + 1); i > 0; i--) str += parts[below(sizeof parts / sizeof *parts)];
      return box(newStr(str));
    }
    case TY_BOOL: return boxb(below(2));
    case TY_LIST: {
      List* l = make<List>();
      V lv = box(l);  // on the stack (scanned) while the elements allocate
      for (int i = below(std::min(size, 8) + 1); i > 0; i--) { V e = gen(depth < 1 ? TY_ANY : TY_NUM, size, depth + 1); l->v.push_back(e); }
      return lv;
    }
    case TY_MAP: {
      Map* m = make<Map>();
      V mv = box(m);
      static const char* const keys[] = {"a", "b", "id", "name", "x", "y", "count", "k"};
      for (int i = below(std::min(size, 5) + 1); i > 0; i--) mapPut(m, keys[below(8)], gen(depth < 1 ? TY_ANY : TY_NUM, size, depth + 1));
      return mv;
    }
    default: {  // any
      static const uint64_t kinds[] = {TY_NUM, TY_NUM, TY_STR, TY_STR, TY_BOOL, TY_LIST, TY_MAP};
      if (!below(10)) return NIL;
      return gen(kinds[below(depth < 1 ? 7 : 5)], size, depth);
    }
  }
}
V listOf(const std::vector<V>& v) {  // (each of v must be reachable from elsewhere while this allocates)
  List* l = make<List>();
  return l->v = v, box(l);
}
V replaced(V list, size_t i, V v) {
  List* l = make<List>();
  return l->v = as<List>(list)->v, l->v[i] = v, box(l);
}
// Smaller versions of v (a value of type t), biggest steps first, onto out (a list, so the GC sees them).
void shrinks(V v, uint64_t t, List* out) {
  auto add = [&](V c) { out->v.push_back(c); };
  if (v == NIL) return;
  if ((t & TY_OPT) || (t & 15) == TY_ANY) add(NIL);
  if (isNum(v)) {
    double x = num(v);
    if (x == 0) return;
    add(box(0.0));
    if (x != std::trunc(x)) add(box(std::trunc(x)));
    if (std::fabs(x) >= 4) add(box(std::trunc(x / 2)));
    if (std::fabs(x) >= 2) add(box(std::trunc(x) - (x > 0 ? 1 : -1)));
    if (x < 0) add(box(-x));
  } else if (is(v, K::Str)) {
    std::string str(as<Str>(v)->sv());
    if (str.empty()) return;
    add(box(newStr("")));
    if (str.size() > 1) add(box(newStr(str.substr(0, str.size() / 2))));
    for (size_t i = 0; i < str.size() && i < 16; i++) add(box(newStr(str.substr(0, i) + str.substr(i + 1))));
  } else if (v == TRUE_) add(FALSE_);
  else if (is(v, K::List)) {
    auto& e = as<List>(v)->v;
    if (e.empty()) return;
    add(listOf({}));
    if (e.size() > 1) add(listOf({e.begin(), e.begin() + e.size() / 2}));
    for (size_t i = 0; i < e.size(); i++) {
      std::vector<V> w = e;
      w.erase(w.begin() + i), add(listOf(w));
    }
    for (size_t i = 0; i < e.size(); i++) {
      size_t at = out->v.size();
      shrinks(e[i], TY_ANY, out);
      for (size_t j = at; j < out->v.size(); j++) out->v[j] = replaced(v, i, out->v[j]);
    }
  } else if (is(v, K::Map)) {
    auto& e = as<Map>(v)->e;
    if (e.empty()) return;
    add(box(make<Map>()));
    for (size_t i = 0; i < e.size(); i++) {
      Map* m = make<Map>();
      add(box(m));
      for (size_t j = 0; j < e.size(); j++) if (j != i) mapPut(m, as<Str>(e[j].first)->sv(), e[j].second);
    }
    for (size_t i = 0; i < e.size(); i++) {  // a value
      size_t at = out->v.size();
      shrinks(e[i].second, TY_ANY, out);
      for (size_t j = at; j < out->v.size(); j++) {
        Map* m = make<Map>();
        V c = out->v[j];
        for (size_t k = 0; k < e.size(); k++) mapPut(m, as<Str>(e[k].first)->sv(), k == i ? c : e[k].second);
        out->v[j] = box(m);
      }
    }
  }
}
// Each argument's shrinks, as whole argument lists.
void shrinkArgs(V args, const std::vector<uint64_t>& types, List* out) {
  for (size_t i = 0; i < types.size() && i < as<List>(args)->v.size(); i++) {
    size_t at = out->v.size();
    shrinks(as<List>(args)->v[i], types[i], out);
    for (size_t j = at; j < out->v.size(); j++) out->v[j] = replaced(args, i, out->v[j]);
  }
}
std::string showCall(const std::string& f, V args) {
  std::string s = f + "(";
  for (size_t i = 0; i < as<List>(args)->v.size(); i++) s += (i ? ", " : "") + toStr(as<List>(args)->v[i], true);
  return s + ")";
}
}  // namespace
static PropSite& propAt(V site) { Locked l(propsMu); return props.at((size_t)num(site)); }
static V propGen(V site, V n) {
  PropSite& p = propAt(site);
  int i = (int)num(n), size = std::min(1 + i / 2, 50);
  if (!i) {  // the same inputs every run: seeded by the test
    prng = 1469598103934665603ull;
    for (char c : p.text) prng = (prng ^ (uint8_t)c) * 1099511628211ull;
    prng |= 1;
  }
  List* in = make<List>();
  V iv = box(in);
  auto args = [&](const std::vector<uint64_t>& t) {
    List* a = make<List>();
    V av = box(a);
    for (uint64_t x : t) { V v = gen(x, size); a->v.push_back(v); }
    return av;
  };
  if (!p.cls) return as<List>(iv)->v = as<List>(args(p.types[0]))->v, iv;
  in->v.push_back(args(p.types[0]));
  List* calls = make<List>();
  in->v.push_back(box(calls));
  for (int k = p.types.size() > 1 ? below(std::min(size, 20) + 1) : 0; k > 0; k--) {
    int m = below((int)p.types.size() - 1);
    List* c = make<List>();
    calls->v.push_back(box(c));
    c->v.push_back(box((double)m));
    c->v.push_back(args(p.types[m + 1]));
  }
  return iv;
}
// The k-th smaller input than `in` (nil: none left). A class's: fewer calls first, then smaller arguments.
static V propShrink(V site, V in, V k) {
  PropSite& p = propAt(site);
  List* out = make<List>();
  V keep = box(out);
  if (!p.cls) shrinkArgs(in, p.types[0], out);
  else {
    V init = as<List>(in)->v[0], calls = as<List>(in)->v[1];
    auto& c = as<List>(calls)->v;
    size_t at = out->v.size();
    if (c.size() > 1) out->v.push_back(listOf({}));
    if (c.size() > 2) out->v.push_back(listOf({c.begin(), c.begin() + c.size() / 2}));
    for (size_t i = 0; i < c.size(); i++) {
      std::vector<V> w = c;
      w.erase(w.begin() + i), out->v.push_back(listOf(w));
    }
    for (size_t i = 0; i < c.size(); i++) {  // a call's arguments (not which method it calls)
      V call = c[i];
      size_t b = out->v.size();
      shrinkArgs(as<List>(call)->v[1], p.types[(size_t)num(as<List>(call)->v[0]) + 1], out);
      for (size_t j = b; j < out->v.size(); j++) out->v[j] = replaced(calls, i, replaced(call, 1, out->v[j]));
    }
    for (size_t j = at; j < out->v.size(); j++) out->v[j] = replaced(in, 1, out->v[j]);
    size_t b = out->v.size();
    shrinkArgs(init, p.types[0], out);
    for (size_t j = b; j < out->v.size(); j++) out->v[j] = replaced(in, 0, out->v[j]);
  }
  size_t i = (size_t)num(k);
  (void)keep;
  return i < out->v.size() ? out->v[i] : NIL;
}
static V propDiscard(V site, V e) {  // e says the input broke a requires of the function (or a method) under test
  return boxb(is(e, K::Str) && as<Str>(e)->sv().starts_with(propAt(site).discard));
}
[[noreturn]] static void propFailed(PropSite& p, const std::string& why) {
  Failure f;
  f.set = true, f.site = p.expect, f.why = why;
  if (!failure.set) failure = f;
  std::string one = why;
  for (size_t i; (i = one.find("\n    ")) != std::string::npos;) one.replace(i, 5, "; ");
  ERR("@expect expect failed: %s (%s)", p.text.c_str(), one.c_str());
}
static V propFail(V site, V in, V e, V n) {
  PropSite& p = propAt(site);
  std::string why;
  if (!p.cls) why = showCall(p.names[0], in);
  else {
    why = "var o = " + showCall(p.names[0], as<List>(in)->v[0]);
    for (V c : as<List>(as<List>(in)->v[1])->v) why += "\n    o." + showCall(p.names[(size_t)num(as<List>(c)->v[0]) + 1], as<List>(c)->v[1]);
  }
  char b[64];
  snprintf(b, sizeof b, "(random input %d, shrunk)", (int)num(n));
  propFailed(p, why + "\n    " + (is(e, K::Str) ? std::string(as<Str>(e)->sv()) : "threw " + toStr(e, true)) + "\n    " + b);
}
static V propDone(V site, V ok, V n) {  // too few inputs met the requires to call it tested
  if (num(ok) >= 20) return NIL;
  char b[160];
  snprintf(b, sizeof b, "only %d of %d random inputs met its requires: give its parameters narrower types, or test it by hand", (int)num(ok), (int)num(n));
  propFailed(propAt(site), b);
}
// expect prints { ... }: print() output inside the block is collected (nested captures stack) and becomes the value.
struct Capture { std::string* prev; std::string buf; };
static TLS std::vector<Capture*> captures;
ISO_LOCAL(captures);
std::string* outBase() { return captures.empty() ? outSink : captures.front()->prev; }
static void endCaptures() {  // a test that returned or crashed out of a capture
  for (; !captures.empty(); captures.pop_back()) outSink = captures.back()->prev, delete captures.back();
}
static std::string popCapture() {
  if (captures.empty()) return "";
  Capture* c = captures.back();
  std::string s = std::move(c->buf);
  outSink = c->prev, captures.pop_back(), delete c;
  return s;
}
static V captureBegin() { captures.push_back(new Capture{outSink, {}}), outSink = &captures.back()->buf; return NIL; }
static V captureEnd() { return box(newStr(popCapture())); }
static V captureAbort(V e) { popCapture(), throwValue(e, RA()); }  // the block threw: stop capturing, rethrow

// run(file) or run({file, source, args, timeout}): run fut as a child process; {code, out, err, timedOut}.
// A relative file is found next to the program being run; source is written to a temporary file.
static V n_run(V o) {
  std::string file, src;
  bool hasSrc = false;
  std::vector<std::string> args;
  double timeout = 10;
  auto strOf = [](V v, const char* what) {
    if (!is(v, K::Str)) ERR("@argument run(): %s must be a string, got %s", what, typeName(v));
    return std::string(as<Str>(v)->sv());
  };
  if (is(o, K::Str)) file = strOf(o, "the file");
  else if (is(o, K::Map)) {
    for (auto& [k, v] : as<Map>(o)->e) {
      std::string key = strOf(k, "an option name");
      if (key == "file") file = strOf(v, "file");
      else if (key == "source") src = strOf(v, "source"), hasSrc = true;
      else if (key == "timeout") timeout = isNum(v) && num(v) > 0 ? num(v) : (ERR("@argument run(): timeout must be a positive number"), 0.0);
      else if (key == "args" && is(v, K::List)) for (V a : as<List>(v)->v) args.push_back(strOf(a, "each of args"));
      else ERR("@argument run(): unknown option '%s' (file, source, args, timeout)", key.c_str());
    }
  } else ERR("@argument run() expects a file name or {file, source, args, timeout}, got %s", typeName(o));
  if (hasSrc == !file.empty()) ERR("@argument run(): give exactly one of file and source");
  if (!file.empty() && file[0] != '/') file = canonicalPath(mainFile.substr(0, mainFile.rfind('/') + 1) + file);
  ProcResult r;
  if (!runProcess(args, file, hasSrc ? &src : nullptr, timeout, r)) ERR("@host run(): could not start fut");
  Map* m = make<Map>();
  volatile V keep = box(m);  // on the stack while the strings below allocate
  (void)keep;
  mapPut(m, "code", box((double)r.code)), mapPut(m, "out", box(newStr(r.out))), mapPut(m, "err", box(newStr(r.err)));
  mapPut(m, "timedOut", boxb(r.timedOut));
  return box(m);
}
void testInit() {
  native<expectAt>("expect@"), native<captureBegin>("capture@begin"), native<captureEnd>("capture@end");
  native<captureAbort>("capture@abort"), native<n_run>("run"), native<contractFail>("contract@fail");
  native<propGen>("prop@gen"), native<propShrink>("prop@shrink"), native<propDiscard>("prop@discard");
  native<propFail>("prop@fail"), native<propDone>("prop@done");
  native<allocsBegin>("allocs@begin"), native<allocsEnd>("allocs@end"), native<growsAt>("grows@");
}

// ---------------- the runner ----------------
static TestRun opts;
static int passed, failed, errored, pending, cachedPasses;
static std::string later;  // --changed, text: failures and pending tests, shown after the summary line
static std::string out;  // the running test's print() output
static double t0, tAll;
static std::string json(const std::string& s) {
  std::string o = "\"";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') o += '\\', o += (char)c;
    else if (c == '\n') o += "\\n";
    else if (c == '\t') o += "\\t";
    else if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
    else o += (char)c;
  }
  return o + "\"";
}
static std::string indent(const std::string& s, const char* pre) {
  std::string o;
  for (size_t i = 0; i < s.size();) {
    size_t j = s.find('\n', i);
    if (j == std::string::npos) j = s.size();
    o += pre + s.substr(i, j - i) + "\n", i = j + 1;
  }
  return o;
}
static std::string where(const TestCase& t) { return t.file.substr(t.file.rfind('/') + 1) + ":" + std::to_string(t.line); }
static void emit(const std::string& s) { fflush(stdout), fwrite(s.data(), 1, s.size(), stdout), fflush(stdout); }
// --changed is terse: passes are counted, not listed; in text, what failed follows the summary line.
static void say(const std::string& s, bool pass) {
  if (!opts.changed) emit(s);
  else if (!pass) opts.json ? emit(s) : (void)(later += s);
}

void testsBegin(const TestRun& o) { opts = o, passed = failed = errored = pending = cachedPasses = 0, later.clear(), tAll = now(), traceBegin(o); }

void* testStart(const TestCase& t) {
  if (!opts.filter.empty() && t.name.find(opts.filter) == std::string::npos) return nullptr;
  if (!t.pending.empty()) {
    pending++;
    if (opts.json) say("{\"type\":\"test\",\"name\":" + json(t.name) + ",\"file\":" + json(t.file) + ",\"line\":" + std::to_string(t.line) +
                       ",\"status\":\"pending\",\"reason\":" + json(t.pending) + "}\n", false);
    else say("pending " + t.name + " (" + where(t) + "): " + t.pending + "\n", false);
    return nullptr;
  }
  if (traceCached(t)) return cachedPasses++, nullptr;
  traceStart(t);
  failure = {}, out.clear(), endCaptures(), outSink = &out;
  t0 = now();
  return t.code;
}

std::string testReport(const TestCase& t, bool err, const std::string& e, char& code) {
  outSink = nullptr, endCaptures();
  double ms = (now() - t0) * 1e3;
  const char* status = failure.set ? "fail" : err ? "error" : "pass";
  code = status[0];
  if (opts.json) {
    std::string j = "{\"type\":\"test\",\"name\":" + json(t.name) + ",\"file\":" + json(t.file) + ",\"line\":" + std::to_string(t.line) +
                    ",\"status\":\"" + status + "\"";
    char b[32];
    snprintf(b, sizeof b, "%.3f", ms), j += ",\"ms\":" + std::string(b);
    if (failure.set) {
      const ExpectSite& s = siteAt(failure.site);
      j += ",\"expect\":{\"file\":" + json(s.file) + ",\"line\":" + std::to_string(s.line) + ",\"expr\":" + json(oneLine(s.text));
      if (!failure.why.empty()) j += ",\"reason\":" + json(failure.why);
      if (!failure.left.empty()) j += ",\"left\":{\"expr\":" + json(oneLine(s.left)) + ",\"value\":" + json(failure.left) + "}";
      if (s.op >= 0) j += ",\"right\":{\"expr\":" + json(oneLine(s.right)) + ",\"value\":" + json(failure.right) + "}";
      if (!failure.what.empty()) j += ",\"diff\":{\"at\":" + json(failure.at) + ",\"what\":" + json(failure.what) + "}";
      j += "}";
    } else if (err) {
      size_t nl = e.find('\n');
      std::string msg = e.substr(0, nl), c = splitCode(msg);
      j += ",\"message\":" + json(msg) + (c.empty() ? "" : ",\"code\":" + json(c)) + ",\"trace\":" + json(nl == std::string::npos ? "" : e.substr(nl + 1));
    }
    if ((failure.set || err) && !isolateSeedNote().empty()) j += ",\"seed\":" + std::to_string(isolateSeed);
    return j + ",\"output\":" + json(out) + "}\n" + traceReport(t);
  }
  std::string s = std::string(failure.set ? "FAIL    " : err ? "ERROR   " : "ok      ") + t.name;
  if (failure.set) {
    const ExpectSite& x = siteAt(failure.site);
    s += " (" + where(t) + ")\n  " + x.file.substr(x.file.rfind('/') + 1) + ":" + std::to_string(x.line) + ": expect " + oneLine(x.text) + "\n";
    for (auto& d : details(x, failure)) s += "    " + d + "\n";
  } else if (err) s += " (" + where(t) + ")\n" + indent("runtime error: " + e.substr(0, e.find_last_not_of('\n') + 1), "  ");
  else s += "\n";
  if ((failure.set || err) && !out.empty()) s += "  output:\n" + indent(out.substr(0, out.size() - (out.back() == '\n')), "    ");
  if (failure.set || err) s += indent(isolateSeedNote(), "  ");
  return s + traceReport(t);
}
void testRecord(char code, const std::string& text) {
  size_t z = text.find('\0');  // the test's trace follows its report (fut test --changed)
  traceResult(code, z == std::string::npos ? "" : text.substr(z + 1));
  outSink = nullptr, (code == 'f' ? failed : code == 'e' ? errored : passed)++, say(text.substr(0, z), code == 'p');
}
void testFinish(const TestCase& t, bool err, const std::string& e) {
  char code;
  std::string text = testReport(t, err, e, code);
  testRecord(code, text);
}

int testsEnd() {
  double ms = (now() - tAll) * 1e3;
  char b[256];
  traceEnd();
  if (opts.changed) {  // "12 run, 340 cached, 0 failed", then what failed
    int ran = passed + failed + errored;
    if (opts.json) snprintf(b, sizeof b, "{\"type\":\"summary\",\"run\":%d,\"cached\":%d,\"passed\":%d,\"failed\":%d,\"errors\":%d,\"pending\":%d,\"ms\":%.3f}\n",
                            ran, cachedPasses, passed, failed, errored, pending, ms);
    else snprintf(b, sizeof b, "%d run, %d cached, %d failed%s (%.1f ms)\n", ran, cachedPasses, failed + errored,
                  pending ? (", " + std::to_string(pending) + " pending").c_str() : "", ms);
    emit(b + later);
    return failed || errored || pending ? 1 : 0;
  }
  if (opts.json) snprintf(b, sizeof b, "{\"type\":\"summary\",\"passed\":%d,\"failed\":%d,\"errors\":%d,\"pending\":%d,\"ms\":%.3f%s}\n",
                          passed, failed, errored, pending, ms, untimed() ? ",\"untimed\":true" : "");
  else {
    std::string parts;
    for (auto [n, w] : {std::pair{failed, "failed"}, {errored, errored == 1 ? "error" : "errors"}, {pending, "pending"}})
      if (n) parts += ", " + std::to_string(n) + " " + w;
    int total = passed + failed + errored + pending;
    snprintf(b, sizeof b, "%d test%s: %d passed%s (%.1f ms%s)\n", total, total == 1 ? "" : "s", passed, parts.c_str(), ms,
             !untimed() ? "" : clockStopped ? "; expect grows by time not measured: the clock stands still while code runs here"
                                            : "; FUT_UNTIMED: expect grows by time not measured");
  }
  emit(b);
  return failed || errored || pending ? 1 : 0;
}

// "path:line:col: error: message\n  source\n  ^" -> its parts, for --json
void reportCompileError(const std::string& m) {
  if (!opts.json) return (void)(fflush(stdout), fprintf(stderr, "%s\n", m.c_str()));
  std::string head = m.substr(0, m.find('\n')), j = "{\"type\":\"compile_error\"";
  size_t e = head.find(": error: ");
  int line = 0, col = 0;
  if (e != std::string::npos) {
    size_t c2 = head.rfind(':', e - 1), c1 = c2 == std::string::npos || !c2 ? std::string::npos : head.rfind(':', c2 - 1);
    if (c1 != std::string::npos && sscanf(head.c_str() + c1, ":%d:%d", &line, &col) == 2)
      j += ",\"file\":" + json(head.substr(0, c1)) + ",\"line\":" + std::to_string(line) + ",\"col\":" + std::to_string(col);
    head = head.substr(e + 9);
  }
  std::string c = splitCode(head);
  emit(j + ",\"message\":" + json(head) + (c.empty() ? "" : ",\"code\":" + json(c)) + ",\"text\":" + json(m) + "}\n");
}
void reportTopError(const std::string& m) {
  if (!opts.json) return (void)(fflush(stdout), fprintf(stderr, "runtime error: %s", m.c_str()));
  size_t nl = m.find('\n');
  std::string msg = m.substr(0, nl), c = splitCode(msg);
  emit("{\"type\":\"error\",\"message\":" + json(msg) + (c.empty() ? "" : ",\"code\":" + json(c)) + ",\"trace\":" + json(nl == std::string::npos ? "" : m.substr(nl + 1)) + "}\n");
}
