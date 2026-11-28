// gpu fn: a top-level function compiled for the CPU like any other, and also translated to WGSL, which std/gpu runs on
// the GPU. The translation re-reads the function's source. It takes the part of fut that means the same on both: f32
// numbers, u32 whole numbers, bools and vectors (vec2f, vec3f, vec4f), locals, if / while / for over a range, calls of
// other gpu fns of its file, sqrt, floor, int, std/math's functions, the vector methods, and the file's gpu vars
// (constant numbers and tables of them). Anything else is a compile error naming what to write instead, so a gpu fn
// that compiles runs the same on the GPU as on the CPU (up to f32 rounding, and u32 + * << wrapping past 2^32).
//
// Each expression has a type here, as in WGSL. A whole-number literal takes the type of what it meets (2 is 2.0 beside
// an f32, 2u beside a u32). u32s stay u32 through & | ^ << >> + * %; - / ** and anything mixed with an f32 compute in
// f32, as fut computes every number. Where a u32 is wanted (a u32 parameter, var, result, bitwise operand or table
// index) an f32 is converted with u32(): on the CPU the same value fails the u32 type check or the bitwise operator
// unless it is whole, so the two agree wherever both run.
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
bool whole(double n) { return n == std::trunc(n) && std::fabs(n) < 9007199254740992.0; }
bool isU32(double n) { return whole(n) && n >= 0 && n < 4294967296.0; }
std::string u32Lit(double n) {
  char b[24];
  snprintf(b, sizeof b, "%.0fu", n);
  return b;
}

// A value's type. WHOLE: a whole-number literal, f32 or u32 by where it goes. TABLE: a gpu var list, until indexed.
enum Ty { F32, U32, BOOL, V2, V3, V4, WHOLE, TABLE, VOID };
const char* const TY_NAMES[] = {"f32", "u32", "bool", "vec2f", "vec3f", "vec4f", "f32", "a table", "nothing"};
bool isVec(int t) { return t >= V2 && t <= V4; }

// A gpu var: its WGSL constant, and its shape (no dims: a number).
struct Table { std::string name, wgsl; int elem = F32; std::vector<size_t> dims; };
struct E { std::string s; int t = F32; double n = 0; const Table* tab = nullptr; size_t depth = 0; };
struct Sig { std::vector<int> params; std::vector<std::string> names; int result = VOID; };

struct GpuCtx {  // what one translation (a gpu fn and what it uses) works from
  const std::vector<GpuFnSrc>& fns;
  const std::vector<GpuFnSrc>& vars;
  std::function<const std::string*(int)> alias;
  const GpuFail& failed;
  std::map<size_t, Sig> sigs;
  std::map<size_t, Table> tables;
  std::vector<size_t> tableOrder;  // as first used
  std::vector<size_t> tableBusy;
  const Sig& sig(size_t i);
  const Table& table(size_t i);
};

struct Tr {
  GpuCtx& sh;
  Lexer lx;
  Tok cur, prev;
  struct Local { int sym, t; bool typed; };
  std::vector<std::vector<Local>> scopes;
  std::vector<std::pair<size_t, Tok>> deps;  // the gpu fns it calls, and where it first does
  std::string out;
  int result = VOID, indent = 1, tmp = 0;

  [[noreturn]] void fail(const Tok& t, const std::string& m) { sh.failed(t.p, t.line, m), abort(); }
  void next() {
    prev = cur, cur = lx.next();
    if (cur.t == T_ERR) fail(cur, lx.err);
  }
  void start(const GpuFnSrc& s) { lx.p = s.p, lx.line = s.line, lx.err = nullptr, next(); }
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
  Local* local(int s) {
    for (auto sc = scopes.rbegin(); sc != scopes.rend(); ++sc)
      for (auto& l : *sc) if (l.sym == s) return &l;
    return nullptr;
  }
  static std::string L(int s) { return "l_" + symName(s); }
  static std::string tyName(int t) { return TY_NAMES[t]; }

  int type() {
    Tok t = cur;
    int s = ident();
    const std::string& n = symName(s);
    if (at('?') || at('|')) fail(cur, "a gpu fn's types are num, u32, bool, vec2f, vec3f or vec4f (no ? or |)");
    if (n == "num" || n == "f32") return F32;
    if (n == "u32") return U32;
    if (n == "bool") return BOOL;
    if (n == "vec2f" || n == "vec3f" || n == "vec4f") return V2 + (n[3] - '2');
    fail(t, "a gpu fn's parameters and result are num, u32, bool, vec2f, vec3f or vec4f, not " + n);
  }

  // e as a want: a whole literal becomes either kind of number, a u32 an f32, and an f32 a u32 (see the top)
  std::string to(const E& e, int want, const Tok& t) {
    if (e.t == want && want != TABLE) return e.s;
    if (want == F32 && e.t == WHOLE) return lit(e.n);
    if (want == F32 && e.t == U32) return "f32(" + e.s + ")";
    if (want == U32 && e.t == WHOLE) {
      if (!isU32(e.n)) fail(t, lit(e.n) + " is not a u32 (a whole number from 0 to 4294967295)");
      return u32Lit(e.n);
    }
    if (want == U32 && e.t == F32) return "u32(" + e.s + ")";
    if (e.t == TABLE) fail(t, e.tab->name + " is a table: index it (" + e.tab->name + "[i])");
    fail(t, "expected " + (want == WHOLE ? std::string("a number") : tyName(want)) + ", got " + tyName(e.t));
  }
  int num(const E& e, const Tok& t) {  // a scalar's type, as arithmetic takes it
    if (e.t == BOOL || e.t == VOID || e.t == TABLE) to(e, F32, t);
    return e.t;
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
  std::string cond() {
    Tok t = cur;
    E c = expr();
    if (c.t != BOOL) fail(t, "a gpu fn's conditions are bools, not " + tyName(c.t) + ": compare (x != 0)");
    return c.s;
  }
  void stmt() {
    Tok t = cur;
    if (atKw(K_var)) {
      next();
      Tok n = cur;
      int s = ident();
      int ty = accept(':') ? type() : -1;
      if (!accept('=')) fail(n, "a var in a gpu fn needs a value: var " + symName(s) + " = ...");
      Tok vt = cur;
      E e = expr();
      bool typed = ty >= 0;
      if (!typed) ty = e.t == WHOLE ? F32 : e.t;
      if (ty == TABLE) fail(vt, e.tab->name + " is a table: a local holds one of its numbers (" + e.tab->name + "[i])");
      if (ty == VOID) fail(vt, "this gpu fn returns nothing: a var needs a value");
      std::string v = to(e, ty, vt);
      scopes.back().push_back({s, ty, typed});
      return line("var " + L(s) + ": " + tyName(ty) + " = " + v + ";");
    }
    if (atKw(K_if)) {
      next();
      line("if (" + cond() + ") {"), body();
      while (atKw(K_else)) {
        next();
        if (!atKw(K_if)) { line("} else {"), body(); break; }
        next();
        line("} else if (" + cond() + ") {"), body();
      }
      return line("}");
    }
    if (atKw(K_while)) {
      next();
      line("while (" + cond() + ") {"), body();
      return line("}");
    }
    if (atKw(K_for)) {
      next();
      int s = ident();
      if (!atKw(K_in)) fail(cur, "expected 'in'");
      next();
      Tok at0 = cur;
      std::string a = to(expr(), F32, at0);
      if (!accept(T_DOTDOT)) fail(cur, "a gpu fn loops over a range: for i in a..b");
      Tok at1 = cur;
      std::string b = to(expr(), F32, at1), end = "t_" + std::to_string(tmp++);
      line("{"), indent++;
      line("let " + end + " = " + b + ";");
      line("for (var " + L(s) + " = " + a + "; " + L(s) + " < " + end + "; " + L(s) + " += 1.0) {");
      scopes.push_back({{s, F32, true}}), body(), scopes.pop_back();
      line("}"), indent--;
      return line("}");
    }
    if (atKw(K_return)) {
      next();
      bool value = !(at('}') || cur.nl || at(T_EOF));
      if (value && result == VOID) fail(t, "this gpu fn returns nothing: give it a result type to return a value");
      if (!value && result != VOID) fail(t, "this gpu fn returns " + tyName(result) + ": return a value");
      if (!value) return line("return;");
      Tok vt = cur;
      return line("return " + to(expr(), result, vt) + ";");
    }
    if (atKw(K_break) || atKw(K_continue)) return next(), line(prev.sym == K_break ? "break;" : "continue;");
    if (at(T_ID) && cur.sym >= NKW) {
      Tok n = peek();
      int op = n.t;
      if (op == '=' || op == T_ADDEQ || op == T_SUBEQ || op == T_MULEQ || op == T_DIVEQ) {
        int s = ident();
        Local* l = local(s);
        if (!l) {
          for (auto& v : sh.vars) if (v.sym == s) fail(t, symName(s) + " is a gpu var: it never changes");
          fail(t, "a gpu fn assigns only its parameters and locals, not " + symName(s));
        }
        int lt = l->t;
        bool typed = l->typed;
        Tok o = cur;
        next();
        Tok vt = cur;
        E v = expr();
        if (op != '=') v = binop(op == T_ADDEQ ? '+' : op == T_SUBEQ ? '-' : op == T_MULEQ ? '*' : '/', {L(s), lt}, v, o);
        if (lt == U32 && !typed && v.t == F32)
          fail(vt, symName(s) + " holds a u32 (its first value was one), and this is an f32: declare it var " + symName(s) +
                       ": num to hold fractions, or var " + symName(s) + ": u32 to keep whole numbers");
        return line(L(s) + " = " + to(v, lt, vt) + ";");
      }
      if (n.t == '.') {
        Lexer s = lx;
        s.next(), s.next();
        if (Tok a = s.next(); a.t == '=' || (a.t >= T_ADDEQ && a.t <= T_DIVEQ))
          fail(t, "vectors don't change in place: make a new one (v = vec2f(v.x, 1))");
      }
      if (n.t == '[')
        for (auto& v : sh.vars) if (v.sym == cur.sym) fail(t, symName(v.sym) + " is a gpu var: it never changes");
    }
    fail(t, "a gpu fn's statements are var, assignment, if, while, for, return, break and continue");
  }

  // ---- expressions: fut's precedence, fully parenthesized ----
  E binop(int t, const E& l, const E& r, const Tok& op) {
    auto o = [&] { return t < 256 ? std::string(1, (char)t) : std::string(two[t - T_EQ]); };
    if (t == T_AND || t == T_OR) {
      if (l.t != BOOL || r.t != BOOL) fail(op, o() + " in a gpu fn joins bools, got " + tyName(l.t) + " and " + tyName(r.t));
      return {"(" + l.s + " " + o() + " " + r.s + ")", BOOL};
    }
    if (t == T_EQ || t == T_NE || t == '<' || t == T_LE || t == '>' || t == T_GE) {
      if (l.t == BOOL || r.t == BOOL) {
        if (l.t != r.t || (t != T_EQ && t != T_NE)) fail(op, "a bool compares only with == and != to a bool");
        return {"(" + l.s + " " + o() + " " + r.s + ")", BOOL};
      }
      if (isVec(l.t) || isVec(r.t)) fail(op, "a gpu fn compares numbers, not vectors: compare their fields (v.x " + o() + " w.x)");
      num(l, op), num(r, op);
      bool u = (l.t == U32 || r.t == U32) && l.t != F32 && r.t != F32;
      int ty = u ? U32 : F32;
      return {"(" + to(l, ty, op) + " " + o() + " " + to(r, ty, op) + ")", BOOL};
    }
    if (t == '|' || t == '^' || t == '&' || t == T_SHL || t == T_SHR) {
      if (isVec(l.t) || isVec(r.t)) fail(op, o() + " in a gpu fn takes u32s, not vectors");
      return {"(" + to(l, U32, op) + " " + o() + " " + to(r, U32, op) + ")", U32};
    }
    auto mod = [](const std::string& a, const std::string& b) { return "(" + a + " - " + b + " * floor(" + a + " / " + b + "))"; };  // fut's %: the sign of the divisor
    if (isVec(l.t) || isVec(r.t)) {
      if (isVec(l.t) && isVec(r.t) && l.t != r.t) fail(op, o() + " on vectors of different sizes: " + tyName(l.t) + " and " + tyName(r.t));
      int vt = isVec(l.t) ? l.t : r.t;
      std::string a = isVec(l.t) ? l.s : to(l, F32, op), b = isVec(r.t) ? r.s : to(r, F32, op);
      return {t == '%' ? mod(a, b) : "(" + a + " " + o() + " " + b + ")", vt};
    }
    num(l, op), num(r, op);
    bool u = (t == '+' || t == '*' || t == '%') && (l.t == U32 || r.t == U32) && l.t != F32 && r.t != F32;
    int ty = u ? U32 : F32;
    std::string a = to(l, ty, op), b = to(r, ty, op);
    return {t == '%' && !u ? mod(a, b) : "(" + a + " " + o() + " " + b + ")", ty};
  }
  E expr(int minp = 1) {
    E l = unary();
    for (int t, p; (p = prec(t = cur.t)) && p >= minp && !(t == '-' && cur.nl);) {
      Tok op = cur;
      next();
      if (t == '?') {
        if (l.t != BOOL) fail(op, "?: in a gpu fn chooses by a bool, not " + tyName(l.t));
        Tok at = cur;
        E a = expr();
        expect(':');
        E b = expr();
        int ty = a.t;
        if (a.t != b.t || a.t == WHOLE) {
          if (isVec(a.t) || isVec(b.t) || a.t == BOOL || b.t == BOOL) fail(at, "?: in a gpu fn chooses between values of one type, got " + tyName(a.t) + " and " + tyName(b.t));
          num(a, at), num(b, at);
          ty = (a.t == U32 || b.t == U32) && a.t != F32 && b.t != F32 ? U32 : F32;
        }
        l = {"select(" + to(b, ty, at) + ", " + to(a, ty, at) + ", " + l.s + ")", ty};
        continue;
      }
      E r = expr(p == 3 || p == 4 ? 5 : p + 1);
      l = binop(t, l, r, op);
    }
    return l;
  }
  E unary() {
    Tok t = cur;
    if (accept('-')) {
      if (at(T_NUM) && peek().t != T_POW) {
        next();
        double n = -prev.n;
        return whole(n) ? E{"(" + lit(n) + ")", WHOLE, n} : E{"(" + lit(n) + ")", F32};
      }
      E u = unary();
      if (u.t == WHOLE) return {"(" + lit(-u.n) + ")", WHOLE, -u.n};
      if (isVec(u.t)) return {"(-" + u.s + ")", u.t};
      num(u, t);
      return {"(-" + to(u, F32, t) + ")", F32};
    }
    if (accept('!')) {
      E u = unary();
      if (u.t != BOOL) fail(t, "! in a gpu fn takes a bool, not " + tyName(u.t));
      return {"(!" + u.s + ")", BOOL};
    }
    E v = postfix();
    if (!accept(T_POW)) return v;
    Tok o = prev;
    E e = unary();
    if (isVec(v.t) || isVec(e.t)) {
      if (v.t != e.t) fail(o, "** on a vector takes a vector of its size");
      return {"pow(" + v.s + ", " + e.s + ")", v.t};
    }
    return {"pow(" + to(v, F32, o) + ", " + to(e, F32, o) + ")", F32};
  }
  std::vector<std::pair<E, Tok>> args() {  // after '('
    std::vector<std::pair<E, Tok>> a;
    if (accept(')')) return a;
    do {
      Tok t = cur;
      a.push_back({expr(), t});
    } while (accept(','));
    expect(')');
    return a;
  }
  void arity(const Tok& t, const std::string& f, size_t got, size_t n) {
    if (got != n) fail(t, f + "() takes " + std::to_string(n) + (n == 1 ? " argument" : " arguments") + ", got " + std::to_string(got));
  }
  // numbers to f32, or vectors of one size: what sqrt, floor and std/math take (a vector elementwise)
  std::pair<std::string, int> numeric(const std::string& n, const std::vector<std::pair<E, Tok>>& a) {
    int vt = -1;
    for (auto& [e, t] : a) if (isVec(e.t)) vt = e.t;
    std::string s;
    for (auto& [e, t] : a) {
      if (vt >= 0 && e.t != vt) fail(t, n + "() on vectors takes vectors of one size, got " + tyName(vt) + " and " + tyName(e.t));
      s += (s.empty() ? "" : ", ") + (vt >= 0 ? e.s : to(e, F32, t));
    }
    return {s, vt >= 0 ? vt : F32};
  }
  E postfix() {
    Tok t = cur;
    E v;
    if (accept(T_NUM)) v = whole(prev.n) ? E{lit(prev.n), WHOLE, prev.n} : E{lit(prev.n), F32};
    else if (accept('(')) v = expr(), expect(')');  // (every operator's WGSL is parenthesized already)
    else if (atKw(K_true) || atKw(K_false)) next(), v = {prev.sym == K_true ? "true" : "false", BOOL};
    else if (at(T_ID) && cur.sym >= NKW) next(), v = named(t);
    else if (at(T_STR)) fail(t, "a gpu fn has no strings");
    else if (at('[')) fail(t, "a gpu fn has no lists: use vec2f, vec3f or vec4f, or a gpu var for a table");
    else if (at('{')) fail(t, "a gpu fn has no maps");
    else if (atKw(K_fn)) fail(t, "a gpu fn has no lambdas");
    else if (atKw(K_nil)) fail(t, "a gpu fn has no nil");
    else fail(t, "expected a value");
    while (at('[') && !cur.nl && v.t == TABLE) {
      next();
      Tok it = cur;
      E i = expr();
      expect(']');
      v.s += "[" + to(i, U32, it) + "]";
      if (++v.depth == v.tab->dims.size()) v.t = v.tab->elem;
    }
    while (at('.')) {
      next();
      Tok f = cur;
      int s = ident();
      const std::string& n = symName(s);
      if (!isVec(v.t)) fail(f, "a gpu fn's " + tyName(v.t) + " has no ." + n + ": only vectors have fields and methods");
      if (at('(') && !cur.nl) {
        next();
        auto a = args();
        size_t want = n == "length" || n == "normalize" ? 0 : n == "dot" || n == "cross" ? 1 : 9;
        if (want == 9) fail(f, "a gpu fn's vectors have the methods length(), dot(v), normalize() and cross(v), not " + n + "()");
        arity(f, n, a.size(), want);
        if (want == 1 && a[0].first.t != v.t) fail(a[0].second, n + "() takes a " + tyName(v.t) + ", got " + tyName(a[0].first.t));
        if (n == "cross" && v.t != V3) fail(f, "cross() is for vec3f");
        v = {n + "(" + v.s + (want ? ", " + a[0].first.s : "") + ")", n == "normalize" || n == "cross" ? v.t : F32};
        continue;
      }
      if (n.size() > 4 || n.find_first_not_of("xyzw") != std::string::npos) {
        if (n.size() > 4 || n.find_first_not_of("rgba") != std::string::npos) fail(f, "a vector's fields are x y z w (or r g b a), and swizzles of them like xy, not " + n);
      }
      v = {v.s + "." + n, n.size() == 1 ? F32 : int(V2 + n.size() - 2)};
    }
    if (at('[') && !cur.nl) fail(cur, v.tab ? v.tab->name + " has " + std::to_string(v.tab->dims.size()) + " dimension" + (v.tab->dims.size() == 1 ? "" : "s")
                                            : std::string("a gpu fn indexes only its gpu vars: use v.x, v.y, ... for a vector"));
    if (at('(') && !cur.nl) fail(cur, "a gpu fn calls functions by name");
    return v;
  }
  E named(const Tok& t) {  // a name, just read
    int s = t.sym;
    const std::string& n = symName(s);
    bool call = at('(') && !cur.nl;
    if (Local* l = local(s)) {
      if (call) fail(t, n + " is a local: a gpu fn calls functions by name");
      return {L(s), l->t};
    }
    if (call) {
      next();
      auto a = args();
      if (n == "vec2f" || n == "vec3f" || n == "vec4f") {
        arity(t, n, a.size(), size_t(n[3] - '0'));
        std::string s;
        for (auto& [e, at] : a) s += (s.empty() ? "" : ", ") + to(e, F32, at);
        return {n + "(" + s + ")", V2 + (n[3] - '2')};
      }
      if (n == "sqrt" || n == "floor" || n == "int") {
        arity(t, n, a.size(), 1);
        auto [s, ty] = numeric(n, a);
        return {(n == "int" ? "trunc" : n) + "(" + s + ")", ty};
      }
      for (size_t i = 0; i < sh.fns.size(); i++)
        if (sh.fns[i].sym == s) {
          if (std::none_of(deps.begin(), deps.end(), [&](auto& d) { return d.first == i; })) deps.push_back({i, t});
          const Sig& g = sh.sig(i);
          arity(t, n, a.size(), g.params.size());
          std::string s;
          for (size_t k = 0; k < a.size(); k++) {
            s += (k ? ", " : "") + to(a[k].first, g.params[k], a[k].second);
          }
          return {"f_" + n + "(" + s + ")", g.result};
        }
      fail(t, n + " is not a gpu fn of this file: a gpu fn calls gpu fns, sqrt, floor, int, vec2f/3f/4f and std/math's functions");
    }
    if (at('.') && peek().t == T_ID) {  // a module's member
      const std::string* m = sh.alias(s);
      if (!m) fail(t, "a gpu fn reads only its parameters, locals and gpu vars: pass " + n + " in as a parameter");
      next();
      Tok f = cur;
      int fs = ident();
      const std::string& fn = symName(fs);
      if (*m == "math" && !(at('(') && !cur.nl)) {
        if (fn == "pi") return {lit(M_PI), F32};
        if (fn == "e") return {lit(M_E), F32};
      }
      if (*m == "math" && at('(') && !cur.nl) {
        next();
        auto a = args();
        for (size_t i = 0; i < sizeof MATH / sizeof *MATH; i++)
          if (fn == MATH[i]) {
            arity(f, fn, a.size(), (size_t)MATH_ARITY[i]);
            auto [s, ty] = numeric(fn, a);
            return {fn + "(" + s + ")", ty};
          }
      }
      fail(f, "a gpu fn calls std/math's " + std::string("acos asin atan atan2 ceil cos exp log log2 pow sin tan trunc abs min max clamp sign") +
                  " and reads math.pi and math.e, not " + *m + "." + fn);
    }
    for (size_t i = 0; i < sh.vars.size(); i++)
      if (sh.vars[i].sym == s) {
        const Table& g = sh.table(i);
        if (g.dims.empty()) return {"g_" + n, g.elem};
        return {"g_" + n, TABLE, 0, &g};
      }
    fail(t, "a gpu fn reads only its parameters, locals and gpu vars: pass " + n + " in as a parameter, or declare it gpu var");
  }

  // ---- a function's header, and the whole function: its WGSL, renamed f_name, its parameters copied into locals ----
  Sig header(std::vector<int>* syms = nullptr) {
    expect(T_ID);  // fn
    ident();
    expect('(');
    Sig g;
    if (!accept(')')) {
      do {
        Tok p = cur;
        int x = ident();
        if (!accept(':')) fail(p, "give each parameter of a gpu fn a type: " + symName(x) + ": num");
        g.params.push_back(type()), g.names.push_back(symName(x));
        if (syms) syms->push_back(x);
      } while (accept(','));
      expect(')');
    }
    if (accept(':')) g.result = type();
    return g;
  }
  GpuInfo function(const GpuFnSrc& f) {
    start(f);
    std::vector<int> ps;
    Sig g = header(&ps);
    result = g.result;
    for (int d = 0; !(d == 0 && at('{')); next()) {  // effects, contracts
      if (at(T_EOF)) fail(cur, "expected '{'");
      d += nesting(cur);
    }
    GpuInfo info;
    info.names = g.names;
    for (int p : g.params) info.types.push_back(tyName(p));
    info.result = result == VOID ? "" : tyName(result);
    std::string head = "fn f_" + symName(f.sym) + "(";
    for (size_t i = 0; i < ps.size(); i++) head += (i ? ", " : "") + std::string("p_") + symName(ps[i]) + ": " + info.types[i];
    out = head + ")" + (info.result.empty() ? "" : " -> " + info.result) + " {\n";
    scopes.emplace_back();
    for (size_t i = 0; i < ps.size(); i++) line("var " + L(ps[i]) + " = p_" + symName(ps[i]) + ";"), scopes.back().push_back({ps[i], g.params[i], true});
    block();
    out += "}\n";
    info.wgsl = out, info.entry = "f_" + symName(f.sym);
    return info;
  }

  // ---- a gpu var: var NAME [: num | u32 | list<...>] = a number, or a list of them, or of such lists ----
  Table table(const GpuFnSrc& v) {
    start(v);
    next();  // var
    Table g;
    g.name = symName(ident());
    size_t want = 0;
    bool typed = accept(':');
    if (typed) {
      int closes = 0;
      for (;;) {
        Tok t = cur;
        int s = ident();
        const std::string& n = symName(s);
        if (n == "list" && at('<')) { next(), want++; continue; }
        if (n == "num" || n == "f32") g.elem = F32;
        else if (n == "u32") g.elem = U32;
        else fail(t, "a gpu var's type is num, u32, or a list<> of them, not " + n);
        break;
      }
      while (closes < (int)want) {
        if (accept('>')) closes++;
        else if (accept(T_SHR)) closes += 2;
        else fail(cur, "expected '>'");
      }
      if (closes != (int)want) fail(prev, "expected '>'");
    }
    if (!accept('=')) fail(cur, "a gpu var needs a value: gpu var " + g.name + " = ...");
    std::vector<double> vals;
    std::vector<Tok> where;
    Tok t = cur;
    leaves(vals, g.dims, 0, where);
    if (typed && g.dims.size() != want) fail(t, "gpu var " + g.name + " is " + (want ? "a list" : "a number") + " nested " + std::to_string(want) + " deep, but its value is nested " + std::to_string(g.dims.size()));
    if (!cur.nl && !at(T_EOF)) fail(cur, "a gpu var's value is a number, or a list of them (or of such lists), written out");
    for (size_t i = 0; i < vals.size(); i++)
      if (g.elem == U32 && !isU32(vals[i])) fail(where[i], lit(vals[i]) + " is not a u32 (a whole number from 0 to 4294967295)");
    std::string et = tyName(g.elem);
    std::vector<std::string> types(g.dims.size() + 1);
    types[g.dims.size()] = et;
    for (size_t d = g.dims.size(); d-- > 0;) types[d] = "array<" + types[d + 1] + ", " + std::to_string(g.dims[d]) + ">";
    size_t k = 0;
    std::function<std::string(size_t)> emit = [&](size_t d) -> std::string {
      if (d == g.dims.size()) { double x = vals[k++]; return g.elem == U32 ? u32Lit(x) : lit(x); }
      std::string s = types[d] + "(";
      for (size_t i = 0; i < g.dims[d]; i++) s += (i ? ", " : "") + emit(d + 1);
      return s + ")";
    };
    g.wgsl = "const g_" + g.name + ": " + types[0] + " = " + emit(0) + ";\n";
    return g;
  }
  // A number, or a list of these, all lists at one depth of one length: vals in order, dims each depth's length.
  void leaves(std::vector<double>& vals, std::vector<size_t>& dims, size_t depth, std::vector<Tok>& where) {
    Tok t = cur;
    if (accept('[')) {
      size_t n = 0;
      while (!accept(']')) {
        if (at(T_EOF)) fail(cur, "expected ']'");
        leaves(vals, dims, depth + 1, where), n++;
        if (!accept(',') && !at(']')) fail(cur, "expected ',' or ']'");
      }
      if (n == 0) fail(t, "a gpu var's lists have something in them");
      if (dims.size() <= depth) dims.resize(depth + 1, 0);
      if (dims[depth] && dims[depth] != n) fail(t, "a gpu var's lists at one depth are one length: this has " + std::to_string(n) + ", another " + std::to_string(dims[depth]));
      dims[depth] = n;
      return;
    }
    bool neg = accept('-');
    if (!accept(T_NUM)) fail(cur, "a gpu var's value is a number, or a list of them (or of such lists), written out");
    if (leafDepth < 0) leafDepth = (int)depth;
    if (leafDepth != (int)depth) fail(t, "a gpu var's lists hold numbers or lists, the same at every depth");
    vals.push_back(neg ? -prev.n : prev.n), where.push_back(t);
  }
  int leafDepth = -1;
};

const Sig& GpuCtx::sig(size_t i) {
  if (auto it = sigs.find(i); it != sigs.end()) return it->second;
  Tr t{*this};
  t.start(fns[i]);
  return sigs[i] = t.header();
}
const Table& GpuCtx::table(size_t i) {
  if (auto it = tables.find(i); it != tables.end()) return it->second;
  Tr t{*this};
  Table g = t.table(vars[i]);
  tableOrder.push_back(i);
  return tables[i] = std::move(g);
}
}  // namespace

GpuInfo wgslOf(const std::vector<GpuFnSrc>& fns, const std::vector<GpuFnSrc>& vars, size_t i, const std::function<const std::string*(int)>& alias, const GpuFail& fail) {
  GpuCtx sh{fns, vars, alias, fail};
  std::vector<GpuInfo> done(fns.size());
  std::vector<char> state(fns.size(), 0);  // 1 translating, 2 translated
  std::string all;
  std::function<void(size_t, const Tok*)> visit = [&](size_t k, const Tok* from) {
    if (state[k] == 2) return;
    if (state[k] == 1) fail(from ? from->p : fns[k].p, from ? from->line : fns[k].line, "a gpu fn can't call itself, directly or through others: the GPU has no stack"), abort();
    state[k] = 1;
    Tr t{sh};
    done[k] = t.function(fns[k]);
    for (auto& [d, at] : t.deps) visit(d, &at);
    all += done[k].wgsl + "\n";
    state[k] = 2;
  };
  visit(i, nullptr);
  std::string consts;
  for (size_t k : sh.tableOrder) consts += sh.tables[k].wgsl;
  GpuInfo r = done[i];
  r.wgsl = consts + (consts.empty() ? "" : "\n") + all;
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
