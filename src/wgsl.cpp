// gpu fn: a top-level function compiled for the CPU like any other, and also translated to WGSL, which std/gpu runs on
// the GPU. The translation re-reads the function's source. It takes the part of fut that means the same on both: f32
// numbers, bools and vectors (vec2f, vec3f, vec4f), locals, if / while / for over a range, and calls of other gpu fns
// of its file, sqrt, floor, int, std/math's functions and the vector methods. Anything else is a compile error naming
// what to write instead, so a gpu fn that compiles runs the same on the GPU as on the CPU (up to f32 rounding).
#include "compile.h"
#include <cmath>
#include <map>
#include <mutex>

namespace {
const char* const MATH[] = {"acos", "asin", "atan", "atan2", "ceil", "cos", "exp", "log", "log2", "pow", "sin", "tan",
                            "trunc", "abs", "min", "max", "clamp", "sign"};
const int MATH_ARITY[] = {1, 1, 1, 2, 1, 1, 1, 1, 1, 2, 1, 1, 1, 1, 2, 2, 3, 1};

std::string lit(double n) {
  char b[40];
  snprintf(b, sizeof b, "%.9g", n);
  std::string s = b;
  if (s.find_first_of(".en") == std::string::npos) s += ".0";
  return s;
}

struct Tr {
  const std::vector<GpuFnSrc>& all;
  std::function<const std::string*(int)> alias;
  const GpuFail& failed;
  Lexer lx;
  Tok cur, prev;
  std::vector<std::vector<int>> scopes;
  std::vector<std::pair<size_t, Tok>> deps;  // the gpu fns it calls, and where it first does
  std::string out, result;
  int indent = 1, tmp = 0;

  [[noreturn]] void fail(const Tok& t, const std::string& m) { failed(t.p, t.line, m), abort(); }
  void next() {
    prev = cur, cur = lx.next();
    if (cur.t == T_ERR) fail(cur, lx.err);
  }
  bool at(int t) { return cur.t == t; }
  bool atKw(int k) { return cur.t == T_ID && cur.sym == k; }
  bool accept(int t) { return at(t) ? (next(), true) : false; }
  Tok peek() { Lexer s = lx; return s.next(); }
  void expect(int t) {
    if (!accept(t)) fail(cur, std::string("expected '") + (char)t + "'");
  }
  int ident() {
    if (!at(T_ID) || cur.sym < NKW) fail(cur, "expected a name");
    return next(), prev.sym;
  }
  void line(const std::string& s) { out += std::string(2 * indent, ' ') + s + "\n"; }
  bool local(int s) {
    for (auto& sc : scopes) for (int x : sc) if (x == s) return true;
    return false;
  }
  static std::string L(int s) { return "l_" + symName(s); }

  std::string type() {
    Tok t = cur;
    int s = ident();
    const std::string& n = symName(s);
    if (at('?') || at('|')) fail(cur, "a gpu fn's types are num, bool, vec2f, vec3f or vec4f (no ? or |)");
    if (n == "num" || n == "f32") return "f32";
    if (n == "bool" || n == "vec2f" || n == "vec3f" || n == "vec4f") return n;
    fail(t, "a gpu fn's parameters and result are num, bool, vec2f, vec3f or vec4f, not " + n);
  }

  // ---- statements ----
  void block() {
    expect('{');
    scopes.emplace_back();
    while (!accept('}')) {
      if (at(T_EOF)) fail(cur, "expected '}'");
      stmt();
    }
    scopes.pop_back();
  }
  void body() { indent++, block(), indent--; }
  void stmt() {
    Tok t = cur;
    if (atKw(K_var)) {
      next();
      Tok n = cur;
      int s = ident();
      std::string ty = accept(':') ? ": " + type() : "";
      if (!accept('=')) fail(n, "a var in a gpu fn needs a value: var " + symName(s) + " = ...");
      std::string e = expr();
      scopes.back().push_back(s);
      return line("var " + L(s) + ty + " = " + e + ";");
    }
    if (atKw(K_if)) {
      next();
      line("if (" + expr() + ") {"), body();
      while (atKw(K_else)) {
        next();
        if (!atKw(K_if)) { line("} else {"), body(); break; }
        next();
        line("} else if (" + expr() + ") {"), body();
      }
      return line("}");
    }
    if (atKw(K_while)) {
      next();
      line("while (" + expr() + ") {"), body();
      return line("}");
    }
    if (atKw(K_for)) {
      next();
      int s = ident();
      if (!atKw(K_in)) fail(cur, "expected 'in'");
      next();
      std::string a = expr();
      if (!accept(T_DOTDOT)) fail(cur, "a gpu fn loops over a range: for i in a..b");
      std::string b = expr(), end = "t_" + std::to_string(tmp++);
      line("{"), indent++;
      line("let " + end + " = " + b + ";");
      line("for (var " + L(s) + " = " + a + "; " + L(s) + " < " + end + "; " + L(s) + " += 1.0) {");
      scopes.push_back({s}), body(), scopes.pop_back();
      line("}"), indent--;
      return line("}");
    }
    if (atKw(K_return)) {
      next();
      bool value = !(at('}') || cur.nl || at(T_EOF));
      if (value && result.empty()) fail(t, "this gpu fn returns nothing: give it a result type to return a value");
      if (!value && !result.empty()) fail(t, "this gpu fn returns " + result + ": return a value");
      return line(value ? "return " + expr() + ";" : "return;");
    }
    if (atKw(K_break) || atKw(K_continue)) return next(), line(prev.sym == K_break ? "break;" : "continue;");
    if (at(T_ID) && cur.sym >= NKW) {
      Tok n = peek();
      int op = n.t;
      if (op == '=' || op == T_ADDEQ || op == T_SUBEQ || op == T_MULEQ || op == T_DIVEQ) {
        int s = ident();
        if (!local(s)) fail(t, "a gpu fn assigns only its parameters and locals, not " + symName(s));
        next();
        const char* ops[] = {"=", "+=", "-=", "*=", "/="};
        return line(L(s) + " " + ops[op == '=' ? 0 : op - T_ADDEQ + 1] + " " + expr() + ";");
      }
      if (n.t == '.') {
        Lexer s = lx;
        s.next(), s.next();
        if (Tok a = s.next(); a.t == '=' || (a.t >= T_ADDEQ && a.t <= T_DIVEQ))
          fail(t, "vectors don't change in place: make a new one (v = vec2f(v.x, 1))");
      }
    }
    fail(t, "a gpu fn's statements are var, assignment, if, while, for, return, break and continue");
  }

  // ---- expressions: fut's precedence, fully parenthesized ----
  std::string expr(int minp = 1) {
    std::string l = unary();
    for (int t, p; (p = prec(t = cur.t)) && p >= minp && !(t == '-' && cur.nl);) {
      Tok op = cur;
      next();
      if (t == '?') {
        std::string a = expr();
        expect(':');
        std::string b = expr();
        l = "select(" + b + ", " + a + ", " + l + ")";
        continue;
      }
      if (t == '|' || t == '^' || t == '&' || t == T_SHL || t == T_SHR) fail(op, "a gpu fn has no bitwise operators (its numbers are f32)");
      std::string r = expr(p == 3 || p == 4 ? 5 : p + 1);
      if (t == '%') { l = "(" + l + " - " + r + " * floor(" + l + " / " + r + "))"; continue; }  // fut's %: the sign of the divisor
      std::string o = t < 256 ? std::string(1, (char)t) : two[t - T_EQ];
      l = "(" + l + " " + o + " " + r + ")";
    }
    return l;
  }
  std::string unary() {
    if (accept('-')) {
      if (at(T_NUM) && peek().t != T_POW) return next(), "(" + lit(-prev.n) + ")";
      return "(-" + unary() + ")";
    }
    if (accept('!')) return "(!" + unary() + ")";
    std::string v = postfix();
    if (!accept(T_POW)) return v;
    return "pow(" + v + ", " + unary() + ")";
  }
  std::vector<std::string> args() {  // after '('
    std::vector<std::string> a;
    if (accept(')')) return a;
    do a.push_back(expr()); while (accept(','));
    expect(')');
    return a;
  }
  static std::string join(const std::vector<std::string>& a) {
    std::string s;
    for (auto& x : a) s += (s.empty() ? "" : ", ") + x;
    return s;
  }
  void arity(const Tok& t, const std::string& f, const std::vector<std::string>& a, size_t n) {
    if (a.size() != n) fail(t, f + "() takes " + std::to_string(n) + (n == 1 ? " argument" : " arguments") + ", got " + std::to_string(a.size()));
  }
  std::string postfix() {
    Tok t = cur;
    std::string v;
    if (accept(T_NUM)) v = lit(prev.n);
    else if (accept('(')) v = "(" + expr() + ")", expect(')');
    else if (atKw(K_true) || atKw(K_false)) next(), v = prev.sym == K_true ? "true" : "false";
    else if (at(T_ID) && cur.sym >= NKW) next(), v = named(t);
    else if (at(T_STR)) fail(t, "a gpu fn has no strings");
    else if (at('[')) fail(t, "a gpu fn has no lists: use vec2f, vec3f or vec4f");
    else if (at('{')) fail(t, "a gpu fn has no maps");
    else if (atKw(K_fn)) fail(t, "a gpu fn has no lambdas");
    else if (atKw(K_nil)) fail(t, "a gpu fn has no nil");
    else fail(t, "expected a value");
    while (at('.')) {
      next();
      Tok f = cur;
      int s = ident();
      const std::string& n = symName(s);
      if (at('(') && !cur.nl) {
        next();
        auto a = args();
        size_t want = n == "length" || n == "normalize" ? 0 : n == "dot" || n == "cross" ? 1 : 9;
        if (want == 9) fail(f, "a gpu fn's vectors have the methods length(), dot(v), normalize() and cross(v), not " + n + "()");
        arity(f, n, a, want);
        a.insert(a.begin(), v);
        v = n + "(" + join(a) + ")";
        continue;
      }
      if (n.size() > 4 || n.find_first_not_of("xyzw") != std::string::npos) {
        if (n.size() > 4 || n.find_first_not_of("rgba") != std::string::npos) fail(f, "a vector's fields are x y z w (or r g b a), and swizzles of them like xy, not " + n);
      }
      v += "." + n;
    }
    if (at('[') && !cur.nl) fail(cur, "a gpu fn indexes nothing: use v.x, v.y, ...");
    if (at('(') && !cur.nl) fail(cur, "a gpu fn calls functions by name");
    return v;
  }
  std::string named(const Tok& t) {  // a name, just read
    int s = t.sym;
    const std::string& n = symName(s);
    bool call = at('(') && !cur.nl;
    if (local(s)) {
      if (call) fail(t, n + " is a local: a gpu fn calls functions by name");
      return L(s);
    }
    if (call) {
      next();
      auto a = args();
      if (n == "vec2f" || n == "vec3f" || n == "vec4f") return arity(t, n, a, size_t(n[3] - '0')), n + "(" + join(a) + ")";
      if (n == "sqrt" || n == "floor") return arity(t, n, a, 1), n + "(" + join(a) + ")";
      if (n == "int") return arity(t, n, a, 1), "trunc(" + join(a) + ")";
      for (size_t i = 0; i < all.size(); i++)
        if (all[i].sym == s) {
          if (std::none_of(deps.begin(), deps.end(), [&](auto& d) { return d.first == i; })) deps.push_back({i, t});
          return "f_" + n + "(" + join(a) + ")";
        }
      fail(t, n + " is not a gpu fn of this file: a gpu fn calls gpu fns, sqrt, floor, int, vec2f/3f/4f and std/math's functions");
    }
    if (at('.') && peek().t == T_ID) {  // a module's member
      const std::string* m = alias(s);
      if (!m) fail(t, "a gpu fn reads only its parameters and locals: pass " + n + " in as a parameter");
      next();
      Tok f = cur;
      int fs = ident();
      const std::string& fn = symName(fs);
      if (*m == "math" && !(at('(') && !cur.nl)) {
        if (fn == "pi") return lit(M_PI);
        if (fn == "e") return lit(M_E);
      }
      if (*m == "math" && at('(') && !cur.nl) {
        next();
        auto a = args();
        for (size_t i = 0; i < sizeof MATH / sizeof *MATH; i++)
          if (fn == MATH[i]) return arity(f, fn, a, (size_t)MATH_ARITY[i]), fn + "(" + join(a) + ")";
      }
      fail(f, "a gpu fn calls std/math's " + std::string("acos asin atan atan2 ceil cos exp log log2 pow sin tan trunc abs min max clamp sign") +
                  " and reads math.pi and math.e, not " + *m + "." + fn);
    }
    fail(t, "a gpu fn reads only its parameters and locals: pass " + n + " in as a parameter");
  }

  // ---- a whole function: its WGSL, renamed f_name, its parameters copied into locals so they can be assigned ----
  GpuInfo function(const GpuFnSrc& f) {
    lx.p = f.p, lx.line = f.line, lx.err = nullptr;
    next();
    expect(T_ID);  // fn
    int s = ident();
    expect('(');
    GpuInfo info;
    std::vector<int> ps;
    if (!accept(')')) {
      do {
        Tok p = cur;
        int x = ident();
        if (!accept(':')) fail(p, "give each parameter of a gpu fn a type: " + symName(x) + ": num");
        ps.push_back(x), info.names.push_back(symName(x)), info.types.push_back(type());
      } while (accept(','));
      expect(')');
    }
    if (accept(':')) result = type();
    for (int d = 0; !(d == 0 && at('{')); next()) {  // effects, contracts
      if (at(T_EOF)) fail(cur, "expected '{'");
      d += nesting(cur);
    }
    info.result = result;
    std::string head = "fn f_" + symName(s) + "(";
    for (size_t i = 0; i < ps.size(); i++) head += (i ? ", " : "") + std::string("p_") + symName(ps[i]) + ": " + info.types[i];
    out = head + ")" + (result.empty() ? "" : " -> " + result) + " {\n";
    for (int p : ps) line("var " + L(p) + " = p_" + symName(p) + ";");
    scopes.push_back(ps);
    block();
    out += "}\n";
    info.wgsl = out, info.entry = "f_" + symName(s);
    return info;
  }
};
}  // namespace

GpuInfo wgslOf(const std::vector<GpuFnSrc>& fns, size_t i, const std::function<const std::string*(int)>& alias, const GpuFail& fail) {
  std::vector<GpuInfo> done(fns.size());
  std::vector<std::vector<size_t>> calls(fns.size());
  std::vector<char> state(fns.size(), 0);  // 1 translating, 2 translated
  std::string all;
  std::function<void(size_t, const Tok*)> visit = [&](size_t k, const Tok* from) {
    if (state[k] == 2) return;
    if (state[k] == 1) fail(from ? from->p : fns[k].p, from ? from->line : fns[k].line, "a gpu fn can't call itself, directly or through others: the GPU has no stack"), abort();
    state[k] = 1;
    Tr t{fns, alias, fail};
    done[k] = t.function(fns[k]);
    for (auto& [d, at] : t.deps) visit(d, &at);
    all += done[k].wgsl + "\n";
    state[k] = 2;
  };
  visit(i, nullptr);
  GpuInfo r = done[i];
  r.wgsl = all;
  return r;
}

// ---- what the runtime keeps: each gpu fn's translation, by its function's name ----
static Mutex gpuMu;
static std::unordered_map<int, GpuInfo> gpuInfos;
void gpuInfoSet(int sym, GpuInfo i) {
  std::lock_guard g(gpuMu);
  gpuInfos[sym] = std::move(i);
}
static V n_wgsl(V f) {
  if (!is(f, K::Fn)) ERR("@argument wgsl() expects a gpu fn, got %s", typeName(f));
  GpuInfo i;
  bool found = false;
  {  // (no error while holding the lock: an error unwinds past the guard)
    std::lock_guard g(gpuMu);
    auto it = gpuInfos.find(as<Fn>(f)->name);
    if (it != gpuInfos.end()) i = it->second, found = true;
  }
  if (!found) ERR("@argument wgsl(): %s is not a gpu fn (declare it gpu fn %s(...))", symName(as<Fn>(f)->name).c_str(), symName(as<Fn>(f)->name).c_str());
  Map* m = make<Map>();
  volatile V keep[3] = {box(m)};
  (void)keep;
  List* names = make<List>();
  keep[1] = box(names);
  List* types = make<List>();
  keep[2] = box(types);
  for (auto& n : i.names) names->v.push_back(box(newStr(n)));
  for (auto& t : i.types) types->v.push_back(box(newStr(t)));
  mapPut(m, "wgsl", box(newStr(i.wgsl))), mapPut(m, "entry", box(newStr(i.entry))), mapPut(m, "result", box(newStr(i.result)));
  mapPut(m, "names", box(names)), mapPut(m, "types", box(types));
  return box(m);
}
void gpuInit() { native<n_wgsl>("wgsl"); }
