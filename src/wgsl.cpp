// gpu fn: a top-level function compiled for the CPU like any other, and also translated to WGSL, which std/gpu runs on
// the GPU. The translation re-reads the function's source. It takes the part of fut that means the same on both: f32
// numbers, u32 whole numbers, bools, vectors (vec2f, vec3f, vec4f), matrices (mat2x2f, mat3x3f, mat4x4f), structs (a
// `type Name = {field: type, ...}` of the file), locals, if / while / for over a range / match on a u32 (a switch),
// calls of other gpu fns of its file and of files it imports (x.name), sqrt, floor, int, unpack4x8unorm, std/math's
// functions (mix and smoothstep too), the vector methods, and gpu vars (constant numbers and tables of them) of its file
// and its imports. An import's types (x.Name) are structs too. Anything else is a compile error naming what to write instead, so a gpu fn that compiles runs the
// same on the GPU as on the CPU (up to f32 rounding, and u32 + * << wrapping past 2^32).
//
// Each expression has a type here, as in WGSL. A whole-number literal takes the type of what it meets (2 is 2.0 beside
// an f32, 2u beside a u32). u32s stay u32 through & | ^ << >> + * %; - / ** and anything mixed with an f32 compute in
// f32, as fut computes every number. Where a u32 is wanted (a u32 parameter, var, result, bitwise operand or index) an
// f32 is converted with u32(): on the CPU the same value fails the u32 type check or the bitwise operator unless it is
// whole, so the two agree wherever both run.
//
// Vectors, matrices and structs are values on the GPU. On the CPU they are objects, so a gpu fn never changes one in
// place except v.x = e and v[i] = e on a local vector, which the CPU compiles as v = a copy with that changed (vec@set,
// vec@at). A file's functions and gpu vars are named stem_name in WGSL (its file's name), so two files' translations go
// in one shader; structs keep their names, so hand-written WGSL beside them can use them. wgsl([f, g]) joins several
// translations, declaring once what they share.
#include "compile.h"
#include <cmath>
#include <map>
#include <mutex>

namespace {
const char* const MATH[] = {"acos", "asin", "atan", "atan2", "ceil", "cos", "exp", "log", "log2", "pow", "sin", "tan",
                            "trunc", "abs", "min", "max", "clamp", "sign", "fract"};
const int MATH_ARITY[] = {1, 1, 1, 2, 1, 1, 1, 1, 1, 2, 1, 1, 1, 1, 2, 2, 3, 1, 1};

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
// STRUCT + i: the file's i-th struct (GpuCtx::structs).
enum Ty { F32, U32, BOOL, V2, V3, V4, M2, M3, M4, WHOLE, TABLE, VOID, STRUCT = 32 };
const char* const TY_NAMES[] = {"f32", "u32", "bool", "vec2f", "vec3f", "vec4f", "mat2x2f", "mat3x3f", "mat4x4f", "f32", "a table", "nothing"};
bool isVec(int t) { return t >= V2 && t <= V4; }
bool isMat(int t) { return t >= M2 && t <= M4; }
bool isScalar(int t) { return t == F32 || t == U32 || t == WHOLE; }
int size(int t) { return isVec(t) ? t - V2 + 2 : t - M2 + 2; }

// A gpu var: its WGSL constant, and its shape (no dims: a number).
struct Table { std::string name, id, wgsl; int elem = F32; std::vector<size_t> dims; };  // id: its WGSL name
struct Struct { std::string name, wgsl; std::vector<std::pair<std::string, int>> fields; };
struct E { std::string s; int t = F32; double n = 0; const Table* tab = nullptr; size_t depth = 0; };
struct Sig { std::vector<int> params; std::vector<std::string> names; int result = VOID; };

using Key = std::pair<const GpuFile*, size_t>;  // a file's i-th gpu fn, gpu var or type
struct GpuCtx {  // what one translation (a gpu fn and what it uses, from its file and the files it imports) works from
  const GpuModule& module;
  const GpuFail& failed;
  std::map<Key, Sig> sigs;
  std::map<Key, Table> tables;
  std::vector<Key> tableOrder;  // as first used
  std::map<Key, int> structOf;  // a type decl -> its struct
  std::vector<Struct> structs;  // in the order they are declared to WGSL (a struct after those it holds)
  std::vector<Key> busy;        // the types being read, so a struct can't hold itself
  const Sig& sig(Key k);
  const Table& table(Key k);
  int strukt(Key k, const GpuFile& at, const Tok& from);
};
std::string wgslName(const GpuFile& f, int sym) { return f.stem + "_" + symName(sym); }

struct Tr {
  GpuCtx& sh;
  const GpuFile& home;  // the file whose source this reads
  Lexer lx;
  Tok cur, prev;
  struct Local { int sym, t; bool typed; };
  std::vector<std::vector<Local>> scopes;
  std::vector<std::pair<Key, Tok>> deps;  // the gpu fns it calls, and where it first does
  std::string out;
  int result = VOID, indent = 1, tmp = 0;
  int arms = 0;  // match arms open inside the innermost loop: WGSL's break there would leave only the switch

  [[noreturn]] void fail(const Tok& t, const std::string& m) { sh.failed(home, t.p, t.line, m), abort(); }
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
  std::string tyName(int t) { return t >= STRUCT ? sh.structs[t - STRUCT].name : TY_NAMES[t]; }

  // The file an import names (import "./x.fut" as x), if it has gpu fns, gpu vars or types.
  const std::string* alias(int s) {
    if (!home.aliases) return nullptr;
    auto it = home.aliases->find(s);
    return it == home.aliases->end() ? nullptr : &it->second;
  }
  const GpuFile* imported(int s) {
    const std::string* m = alias(s);
    return m ? sh.module(*m) : nullptr;
  }

  int type() {
    Tok t = cur;
    int s = ident();
    const std::string& n = symName(s);
    if (at('.') && !cur.nl && peek().t == T_ID) {  // x.Name: a type of an imported file
      const GpuFile* g = imported(s);
      if (!g) fail(t, "a gpu fn's types are its file's and those of files it imports (import \"./x.fut\" as x), and " + n + " is not such an import");
      next();
      Tok f = cur;
      int ts = ident();
      for (size_t i = 0; i < g->types.size(); i++)
        if (g->types[i].sym == ts) return STRUCT + sh.strukt({g, i}, home, f);
      fail(f, n + " has no type " + symName(ts));
    }
    if (at('?') || at('|')) fail(cur, "a gpu fn's types are num, u32, bool, vectors, matrices and the file's structs (no ? or |)");
    if (n == "num" || n == "f32") return F32;
    if (n == "u32") return U32;
    if (n == "bool") return BOOL;
    if (n == "vec2f" || n == "vec3f" || n == "vec4f") return V2 + (n[3] - '2');
    if (n == "mat2x2f" || n == "mat3x3f" || n == "mat4x4f") return M2 + (n[3] - '2');
    for (size_t i = 0; i < home.types.size(); i++)
      if (home.types[i].sym == s) return STRUCT + sh.strukt({&home, i}, home, t);
    fail(t, "a gpu fn's types are num, u32, bool, vec2f/3f/4f, mat2x2f/3x3f/4x4f and types of {fields} (the file's, or x.Name of an import), not " + n);
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
  void num(const E& e, const Tok& t) {  // a scalar, as arithmetic takes it
    if (!isScalar(e.t)) to(e, F32, t);
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
  void loopBody() {
    int outer = arms;
    arms = 0, body(), arms = outer;
  }
  std::string cond() {
    Tok t = cur;
    E c = expr();
    if (c.t != BOOL) fail(t, "a gpu fn's conditions are bools, not " + tyName(c.t) + ": compare (x != 0)");
    return c.s;
  }
  // match k { 0 { } 1, 2 { } 4..8 { } else { } }: WGSL's switch, on a u32, each range listed out as its cases
  void match() {
    next();
    Tok st = cur;
    E v = expr();
    if (v.t != U32 && v.t != WHOLE)
      fail(st, "a gpu fn matches a u32 (WGSL's switch), not " + tyName(v.t) + (v.t == F32 ? ": declare it u32 (var k: u32 = ...)" : ""));
    line("switch (" + to(v, U32, st) + ") {"), indent++;
    expect('{');
    std::vector<double> seen;
    bool otherwise = false;
    while (!accept('}')) {
      if (at(T_EOF)) fail(cur, "expected '}'");
      if (atKw(K_else)) {
        next(), line("default: {"), arms++, body(), arms--, line("}"), otherwise = true;
        if (!accept('}')) fail(cur, "the else arm is the last arm of a match: expected '}'");
        break;
      }
      std::string cases;
      do {
        Tok vt = cur;
        auto number = [&] {
          bool neg = accept('-');
          if (!at(T_NUM)) fail(cur, "a gpu fn's match has arms of numbers (1), several (1, 2) and ranges (1..4), then else");
          next();
          return neg ? -prev.n : prev.n;
        };
        double a = number(), b = a + 1;
        if (accept(T_DOTDOT)) b = number();
        if (!isU32(a) || !isU32(b - 1) || b <= a)
          fail(vt, "a u32 matches whole numbers from 0 to 4294967295, and a range a..b holds a up to, not including, b");
        if (b - a > 256) fail(vt, "a range in a gpu fn's match holds at most 256 numbers: WGSL lists each as a case");
        for (double x = a; x < b; x++) {
          if (std::find(seen.begin(), seen.end(), x) != seen.end()) fail(vt, "an earlier arm of this match already holds " + u32Lit(x));
          seen.push_back(x), cases += (cases.empty() ? "" : ", ") + u32Lit(x);
        }
      } while (accept(','));
      line("case " + cases + ": {"), arms++, body(), arms--, line("}");
    }
    if (!otherwise) line("default: {}");
    indent--, line("}");
  }
  static int compound(int op) { return op == T_ADDEQ ? '+' : op == T_SUBEQ ? '-' : op == T_MULEQ ? '*' : '/'; }
  void stmt() {
    Tok t = cur;
    if (accept(';')) return;  // as on the CPU, a ; separates statements on one line
    if (atKw(K_var)) {
      next();
      Tok n = cur;
      int s = ident();
      int ty = accept(':') ? type() : -1;
      if (!accept('=')) fail(n, "a var in a gpu fn needs a value: var " + symName(s) + " = ...");
      Tok vt = cur;
      E e = value(ty);
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
      line("while (" + cond() + ") {"), loopBody();
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
      scopes.push_back({{s, F32, true}}), loopBody(), scopes.pop_back();
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
      return line("return " + to(this->value(result), result, vt) + ";");
    }
    if (atKw(K_break) || atKw(K_continue)) {
      if (arms) fail(t, std::string("a gpu fn's match arm doesn't ") + (cur.sym == K_break ? "break" : "continue") +
                            " its loop (in WGSL's switch it would leave the match): set a flag, and test it after the match");
      return next(), line(prev.sym == K_break ? "break;" : "continue;");
    }
    if (at(T_ID) && cur.sym >= NKW && symName(cur.sym) == "match" && !local(cur.sym)) {
      Tok n = peek();
      if (!n.nl && n.t != '=' && n.t != '.' && n.t != T_EOF && n.t != '{' && !(n.t >= T_ADDEQ && n.t <= T_DIVEQ)) return match();
    }
    if (at(T_ID) && cur.sym >= NKW) {
      Tok n = peek();
      int op = n.t;
      auto assigns = [](int o) { return o == '=' || (o >= T_ADDEQ && o <= T_DIVEQ); };
      if (assigns(op)) {
        int s = ident();
        Local* l = local(s);
        if (!l) {
          for (auto& v : home.vars) if (v.sym == s) fail(t, symName(s) + " is a gpu var: it never changes");
          fail(t, "a gpu fn assigns only its parameters and locals, not " + symName(s));
        }
        int lt = l->t;
        bool typed = l->typed;
        Tok o = cur;
        next();
        Tok vt = cur;
        E v = value(op == '=' ? lt : -1);
        if (op != '=') v = binop(compound(op), {L(s), lt}, v, o);
        if (lt == U32 && !typed && v.t == F32)
          fail(vt, symName(s) + " holds a u32 (its first value was one), and this is an f32: declare it var " + symName(s) +
                       ": num to hold fractions, or var " + symName(s) + ": u32 to keep whole numbers");
        return line(L(s) + " = " + to(v, lt, vt) + ";");
      }
      if (n.t == '.') {  // v.x = e: one component of a local vector
        Lexer s = lx;
        s.next();
        Tok f = s.next(), a = s.next();
        if (assigns(a.t)) {
          int vs = ident();
          Local* l = local(vs);
          if (!l) fail(t, "a gpu fn changes components only of its own vectors: " + symName(vs) + " isn't a parameter or local");
          if (l->t >= STRUCT) fail(t, "a gpu fn's structs don't change in place: make a new one (" + symName(vs) + " = {...})");
          if (!isVec(l->t)) fail(t, symName(vs) + " is " + tyName(l->t) + ": only a vector's components can be set");
          next();
          int fs = ident();
          const std::string& fn = symName(fs);
          if (fn.size() != 1 || std::string("xyzw").find(fn[0]) >= (size_t)size(l->t))
            fail(f, "a " + tyName(l->t) + " sets one of its components, " + std::string("xyzw").substr(0, size(l->t)) + ", not " + fn);
          Tok o = cur;
          next();
          Tok vt = cur;
          E v = expr();
          std::string c = L(vs) + "." + fn;
          if (a.t != '=') v = binop(compound(a.t), {c, F32}, v, o);
          if (isVec(v.t)) fail(vt, "a component is a number, not " + tyName(v.t));
          return line(c + " = " + to(v, F32, vt) + ";");
        }
      }
      if (n.t == '[') {  // v[i] = e: one component of a local vector, by index
        for (auto& v : home.vars) if (v.sym == cur.sym) fail(t, symName(v.sym) + " is a gpu var: it never changes");
        int vs = ident();
        Local* l = local(vs);
        if (!l) fail(t, "a gpu fn changes components only of its own vectors: " + symName(vs) + " isn't a parameter or local");
        if (!isVec(l->t)) fail(t, "a gpu fn sets by index only a vector's components, and " + symName(vs) + " is " + tyName(l->t) + (isMat(l->t) ? ": make a new matrix" : ""));
        next();
        Tok it = cur;
        E i = expr();
        expect(']');
        Tok o = cur;
        if (!assigns(o.t)) fail(o, "expected '=': a gpu fn's statement v[i] sets a component (v[i] = e)");
        next();
        Tok vt = cur;
        E v = expr();
        std::string c = L(vs) + "[" + to(i, U32, it) + "]";
        if (o.t != '=') v = binop(compound(o.t), {c, F32}, v, o);
        if (isVec(v.t)) fail(vt, "a component is a number, not " + tyName(v.t));
        return line(c + " = " + to(v, F32, vt) + ";");
      }
    }
    fail(t, "a gpu fn's statements are var, assignment, if, match, while, for, return, break and continue");
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
      if (!isScalar(l.t) || !isScalar(r.t)) fail(op, o() + " in a gpu fn takes u32s, not " + tyName(isScalar(l.t) ? r.t : l.t));
      return {"(" + to(l, U32, op) + " " + o() + " " + to(r, U32, op) + ")", U32};
    }
    if (isMat(l.t) || isMat(r.t)) {  // m * v, v * m, m * m, m * k
      if (t != '*') fail(op, "a gpu fn multiplies matrices (m * v, v * m, m * m, m * k), and doesn't " + o() + " them");
      int n = size(isMat(l.t) ? l.t : r.t);
      if (isMat(l.t) && isMat(r.t)) {
        if (l.t != r.t) fail(op, "* on matrices of different sizes: " + tyName(l.t) + " and " + tyName(r.t));
        return {"(" + l.s + " * " + r.s + ")", l.t};
      }
      const E& o2 = isMat(l.t) ? r : l;
      if (isVec(o2.t)) {
        if (size(o2.t) != n) fail(op, "* on a " + tyName(isMat(l.t) ? l.t : r.t) + " and a " + tyName(o2.t) + ": a vector of " + std::to_string(n));
        return {"(" + l.s + " * " + r.s + ")", V2 + n - 2};
      }
      num(o2, op);
      return {"(" + (isMat(l.t) ? l.s : to(l, F32, op)) + " * " + (isMat(r.t) ? r.s : to(r, F32, op)) + ")", M2 + n - 2};
    }
    auto mod = [](const std::string& a, const std::string& b) { return "(" + a + " - " + b + " * floor(" + a + " / " + b + "))"; };  // fut's %: the sign of the divisor
    if (isVec(l.t) || isVec(r.t)) {
      if (isVec(l.t) && isVec(r.t) && l.t != r.t) fail(op, o() + " on vectors of different sizes: " + tyName(l.t) + " and " + tyName(r.t));
      if (!isVec(l.t)) num(l, op);
      if (!isVec(r.t)) num(r, op);
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
  // An expression where a want is expected: a struct's {field: value, ...} needs to know which struct it makes.
  E value(int want) { return want >= STRUCT && at('{') ? structLit(want) : expr(); }
  E structLit(int ty) {
    const Struct& st = sh.structs[ty - STRUCT];
    Tok t = cur;
    expect('{');
    std::vector<std::string> vals(st.fields.size());
    std::vector<char> given(st.fields.size(), 0);
    while (!accept('}')) {
      if (at(T_EOF)) fail(cur, "expected '}'");
      Tok k = cur;
      if (!at(T_ID)) fail(cur, "expected a field of " + st.name);
      next();
      const std::string& kn = symName(prev.sym);
      size_t i = 0;
      while (i < st.fields.size() && st.fields[i].first != kn) i++;
      if (i == st.fields.size()) fail(k, st.name + " has no field " + kn);
      if (given[i]) fail(k, kn + " is given twice");
      expect(':');
      Tok vt = cur;
      vals[i] = to(value(st.fields[i].second), st.fields[i].second, vt), given[i] = 1;
      accept(',');
    }
    std::string s = st.name + "(";
    for (size_t i = 0; i < vals.size(); i++) {
      if (!given[i]) fail(t, st.name + " needs every field: " + st.fields[i].first + " is missing");
      s += (i ? ", " : "") + vals[i];
    }
    return {s + ")", ty};
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
        if (isMat(a.t) || isMat(b.t) || a.t >= STRUCT || b.t >= STRUCT)
          fail(at, "?: in a gpu fn chooses between numbers, bools and vectors (WGSL's select): use if for " + tyName(isMat(a.t) || a.t >= STRUCT ? a.t : b.t));
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
    num(v, o), num(e, o);
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
      if (vt < 0) num(e, t);
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
    else if (at('{')) fail(t, "a gpu fn makes a struct where its type is known: return {...}, var p: Name = {...}, or an argument");
    else if (atKw(K_fn)) fail(t, "a gpu fn has no lambdas");
    else if (atKw(K_nil)) fail(t, "a gpu fn has no nil");
    else fail(t, "expected a value");
    for (;;) {
      if (at('[') && !cur.nl) {
        Tok bt = cur;
        if (v.t != TABLE && !isVec(v.t) && !isMat(v.t))
          fail(bt, "a gpu fn indexes its gpu vars, vectors and matrices, not " + tyName(v.t));
        next();
        Tok it = cur;
        E i = expr();
        expect(']');
        v.s += "[" + to(i, U32, it) + "]";
        if (v.t == TABLE) {
          if (++v.depth == v.tab->dims.size()) v.t = v.tab->elem, v.tab = nullptr;
        } else v = {v.s, isMat(v.t) ? V2 + size(v.t) - 2 : F32};
        continue;
      }
      if (!at('.')) break;
      next();
      Tok f = cur;
      int s = ident();
      const std::string& n = symName(s);
      if (v.t >= STRUCT) {
        const Struct& st = sh.structs[v.t - STRUCT];
        size_t i = 0;
        while (i < st.fields.size() && st.fields[i].first != n) i++;
        if (i == st.fields.size()) fail(f, st.name + " has no field " + n);
        if (at('(') && !cur.nl) fail(f, "a gpu fn's struct has fields, not methods");
        v = {v.s + "." + n, st.fields[i].second};
        continue;
      }
      if (!isVec(v.t)) fail(f, "a gpu fn's " + tyName(v.t) + " has no ." + n + (isMat(v.t) ? ": index a matrix's columns, m[i]" : ": only vectors and structs have fields"));
      if (at('(') && !cur.nl) {
        next();
        auto a = args();
        size_t want = n == "length" || n == "normalize" ? 0 : n == "dot" || n == "cross" || n == "distance" ? 1 : 9;
        if (want == 9) fail(f, "a gpu fn's vectors have the methods length(), dot(v), distance(v), normalize() and cross(v), not " + n + "()");
        arity(f, n, a.size(), want);
        if (want == 1 && a[0].first.t != v.t) fail(a[0].second, n + "() takes a " + tyName(v.t) + ", got " + tyName(a[0].first.t));
        if (n == "cross" && v.t != V3) fail(f, "cross() is for vec3f");
        v = {n + "(" + v.s + (want ? ", " + a[0].first.s : "") + ")", n == "normalize" || n == "cross" ? v.t : F32};
        continue;
      }
      if (n.size() > 4 || n.find_first_not_of("xyzw") != std::string::npos) {
        if (n.size() > 4 || n.find_first_not_of("rgba") != std::string::npos) fail(f, "a vector's fields are x y z w (or r g b a), and swizzles of them like xy, not " + n);
      }
      for (char c : n) {
        size_t k = std::string("xyzw").find(c);
        if (k == std::string::npos) k = std::string("rgba").find(c);
        if ((int)k >= size(v.t)) fail(f, "a " + tyName(v.t) + " has no ." + std::string(1, c));
      }
      v = {v.s + "." + n, n.size() == 1 ? F32 : int(V2 + n.size() - 2)};
    }
    if (at('(') && !cur.nl) fail(cur, "a gpu fn calls functions by name");
    return v;
  }
  // A call of a gpu fn (k), past its '('. n: its name as written.
  E callGpu(Key k, const Tok& t, const std::string& n) {
    if (std::none_of(deps.begin(), deps.end(), [&](auto& d) { return d.first == k; })) deps.push_back({k, t});
    const Sig g = sh.sig(k);
    std::string a;
    size_t i = 0;
    if (!accept(')')) {
      do {
        Tok at = cur;
        if (i >= g.params.size()) arity(t, n, i + 1, g.params.size());
        a += (i ? ", " : "") + to(value(g.params[i]), g.params[i], at), i++;
      } while (accept(','));
      expect(')');
    }
    arity(t, n, i, g.params.size());
    return {wgslName(*k.first, k.first->fns[k.second].sym) + "(" + a + ")", g.result};
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
      for (size_t i = 0; i < home.fns.size(); i++)
        if (home.fns[i].sym == s) return callGpu({&home, i}, t, n);
      auto a = args();
      if (n == "vec2f" || n == "vec3f" || n == "vec4f") {  // a number for each component, one for all, or numbers and vectors
        int k = n[3] - '0', got = 0;
        std::string s;
        for (auto& [e, at] : a) {
          if (isVec(e.t)) s += (s.empty() ? "" : ", ") + e.s, got += size(e.t);
          else num(e, at), s += (s.empty() ? "" : ", ") + to(e, F32, at), got++;
        }
        if (got != k && !(a.size() == 1 && got == 1))
          fail(t, n + "() takes one number for every component, or numbers and vectors of " + std::to_string(k) + " components in all, got " + std::to_string(got));
        return {n + "(" + s + ")", V2 + (k - 2)};
      }
      if (n == "unpack4x8unorm") {
        arity(t, n, a.size(), 1);
        return {"unpack4x8unorm(" + to(a[0].first, U32, a[0].second) + ")", V4};
      }
      if (n == "mat2x2f" || n == "mat3x3f" || n == "mat4x4f") {
        int k = n[3] - '0';
        arity(t, n, a.size(), (size_t)k);
        std::string s;
        for (auto& [e, at] : a) {
          if (e.t != V2 + k - 2) fail(at, n + "() takes its columns, each a vec" + std::to_string(k) + "f, not " + tyName(e.t));
          s += (s.empty() ? "" : ", ") + e.s;
        }
        return {n + "(" + s + ")", M2 + k - 2};
      }
      if (n == "sqrt" || n == "floor" || n == "int") {
        arity(t, n, a.size(), 1);
        auto [s, ty] = numeric(n, a);
        return {(n == "int" ? "trunc" : n) + "(" + s + ")", ty};
      }
      fail(t, n + " is not a gpu fn of this file: a gpu fn calls gpu fns (its file's, or x.name of an import), sqrt, floor, int, vec2f/3f/4f, mat2x2f/3x3f/4x4f, unpack4x8unorm and std/math's functions");
    }
    if (at('.') && peek().t == T_ID) {  // a module's member
      const std::string* m = alias(s);
      if (m) {
        next();
        Tok f = cur;
        int fs = ident();
        const std::string& fn = symName(fs);
        if (const GpuFile* g = sh.module(*m)) {  // an imported file's gpu fn or gpu var
          if (at('(') && !cur.nl) {
            next();
            for (size_t i = 0; i < g->fns.size(); i++)
              if (g->fns[i].sym == fs) return callGpu({g, i}, f, n + "." + fn);
            fail(f, n + "." + fn + " is not a gpu fn: a gpu fn calls only gpu fns, of its file and of files it imports");
          }
          for (size_t i = 0; i < g->vars.size(); i++)
            if (g->vars[i].sym == fs) {
              const Table& v = sh.table({g, i});
              return {wgslName(*g, fs), v.dims.empty() ? v.elem : TABLE, 0, v.dims.empty() ? nullptr : &v};
            }
          fail(f, n + "." + fn + " is not a gpu var: a gpu fn reads only its parameters, locals and gpu vars");
        }
        if (*m != "math") fail(t, n + " has no gpu fns or gpu vars: a gpu fn calls gpu fns of its file, of files it imports and std/math's functions");
        if (*m == "math" && !(at('(') && !cur.nl)) {
          if (fn == "pi") return {lit(M_PI), F32};
          if (fn == "e") return {lit(M_E), F32};
        }
        if (*m == "math" && at('(') && !cur.nl) {
          next();
          auto a = args();
          if (fn == "mix") {  // a and b numbers or vectors of one size; t a number, or a vector of theirs
            arity(f, fn, a.size(), 3);
            auto& [x, xt] = a[0];
            auto& [y, yt] = a[1];
            auto& [k, kt] = a[2];
            int ty = isVec(x.t) ? x.t : F32;
            if ((isVec(x.t) || isVec(y.t)) && x.t != y.t) fail(yt, "mix() blends values of one type, got " + tyName(x.t) + " and " + tyName(y.t));
            if (isVec(k.t) && k.t != ty) fail(kt, "mix()'s t is a number or a " + tyName(ty) + ", got " + tyName(k.t));
            auto arg = [&](const E& e, const Tok& at) { return isVec(e.t) ? e.s : (num(e, at), to(e, F32, at)); };
            return {"mix(" + arg(x, xt) + ", " + arg(y, yt) + ", " + arg(k, kt) + ")", ty};
          }
          if (fn == "smoothstep") {
            arity(f, fn, a.size(), 3);
            std::string s;
            for (auto& [e, at] : a) {
              if (!isScalar(e.t)) fail(at, "smoothstep() takes numbers, got " + tyName(e.t));
              s += (s.empty() ? "" : ", ") + to(e, F32, at);
            }
            return {"smoothstep(" + s + ")", F32};
          }
          for (size_t i = 0; i < sizeof MATH / sizeof *MATH; i++)
            if (fn == MATH[i]) {
              arity(f, fn, a.size(), (size_t)MATH_ARITY[i]);
              auto [s, ty] = numeric(fn, a);
              return {fn + "(" + s + ")", ty};
            }
        }
        fail(f, "a gpu fn calls std/math's " + std::string("acos asin atan atan2 ceil cos exp log log2 pow sin tan trunc abs min max clamp sign fract mix smoothstep") +
                    " and reads math.pi and math.e, not " + *m + "." + fn);
      }
    }
    for (size_t i = 0; i < home.vars.size(); i++)
      if (home.vars[i].sym == s) {
        const Table& g = sh.table({&home, i});
        if (g.dims.empty()) return {wgslName(home, s), g.elem};
        return {wgslName(home, s), TABLE, 0, &g};
      }
    fail(t, "a gpu fn reads only its parameters, locals and gpu vars: pass " + n + " in as a parameter, or declare it gpu var");
  }

  // ---- a function's header, and the whole function: its WGSL, named stem_name, its parameters copied into locals ----
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
    info.name = symName(f.sym), info.entry = wgslName(home, f.sym);
    std::string head = "fn " + info.entry + "(";
    for (size_t i = 0; i < ps.size(); i++) head += (i ? ", " : "") + std::string("p_") + symName(ps[i]) + ": " + info.types[i];
    out = head + ")" + (info.result.empty() ? "" : " -> " + info.result) + " {\n";
    scopes.emplace_back();
    for (size_t i = 0; i < ps.size(); i++) line("var " + L(ps[i]) + " = p_" + symName(ps[i]) + ";"), scopes.back().push_back({ps[i], g.params[i], true});
    block();
    out += "}\n";
    info.wgsl = out;
    return info;
  }

  // ---- a struct: type Name = {field: type, ...} ----
  Struct strukt(const GpuFnSrc& d) {
    start(d);
    next();  // type
    Struct st;
    st.name = symName(ident());
    expect('=');
    Tok t = cur;
    if (!accept('{')) fail(t, "a gpu fn's struct is a type of {fields}: type " + st.name + " = {x: num, ...}");
    while (!accept('}')) {
      if (at(T_EOF)) fail(cur, "expected '}'");
      Tok f = cur;
      int s = ident();
      for (auto& [n, _] : st.fields) if (n == symName(s)) fail(f, symName(s) + " is listed twice");
      expect(':');
      int ft = type();
      if (ft == VOID || ft == TABLE) fail(f, "a struct's fields are numbers, bools, vectors, matrices and structs");
      st.fields.push_back({symName(s), ft});
      accept(',');
    }
    if (st.fields.empty()) fail(t, "a struct has fields");
    st.wgsl = "struct " + st.name + " {\n";
    for (auto& [n, ft] : st.fields) st.wgsl += "  " + n + ": " + tyName(ft) + ",\n";
    st.wgsl += "}\n";
    return st;
  }

  // ---- a gpu var: var NAME [: num | u32 | list<...>] = a number, or a list of them, or of such lists ----
  Table table(const GpuFnSrc& v) {
    start(v);
    next();  // var
    Table g;
    int name = ident();
    g.name = symName(name);
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
    g.id = wgslName(home, name);
    g.wgsl = "const " + g.id + ": " + types[0] + " = " + emit(0) + ";\n";
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

const Sig& GpuCtx::sig(Key k) {
  if (auto it = sigs.find(k); it != sigs.end()) return it->second;
  Tr t{*this, *k.first};
  t.start(k.first->fns[k.second]);
  Sig g = t.header();
  return sigs[k] = g;
}
const Table& GpuCtx::table(Key k) {
  if (auto it = tables.find(k); it != tables.end()) return it->second;
  Tr t{*this, *k.first};
  Table g = t.table(k.first->vars[k.second]);
  tableOrder.push_back(k);
  return tables[k] = std::move(g);
}
int GpuCtx::strukt(Key k, const GpuFile& at, const Tok& from) {
  if (auto it = structOf.find(k); it != structOf.end()) return it->second;
  if (std::find(busy.begin(), busy.end(), k) != busy.end())
    failed(at, from.p, from.line, "a struct can't hold itself, directly or through others"), abort();
  busy.push_back(k);
  Tr t{*this, *k.first};
  Struct st = t.strukt(k.first->types[k.second]);
  busy.pop_back();
  for (auto& o : structs)  // WGSL has one struct of a name, and structs keep theirs
    if (o.name == st.name)
      failed(at, from.p, from.line, "two files' structs are named " + st.name + ", and a shader holds one struct of a name: rename one"), abort();
  structs.push_back(std::move(st));
  return structOf[k] = (int)structs.size() - 1;
}
}  // namespace

GpuInfo wgslOf(const GpuFile& file, size_t i, const GpuModule& module, const GpuFail& fail) {
  GpuCtx sh{module, fail};
  std::map<Key, GpuInfo> done;
  std::map<Key, char> state;  // 1 translating, 2 translated
  std::string all;
  std::vector<std::pair<std::string, std::string>> fnDecls;
  std::function<void(Key, const GpuFile*, const Tok*)> visit = [&](Key k, const GpuFile* in, const Tok* from) {
    const GpuFnSrc& f = k.first->fns[k.second];
    if (state[k] == 2) return;
    if (state[k] == 1) fail(from ? *in : *k.first, from ? from->p : f.p, from ? from->line : f.line, "a gpu fn can't call itself, directly or through others: the GPU has no stack"), abort();
    state[k] = 1;
    Tr t{sh, *k.first};
    done[k] = t.function(f);
    for (auto& [d, at] : t.deps) visit(d, k.first, &at);
    all += done[k].wgsl + "\n";
    fnDecls.push_back({done[k].entry, done[k].wgsl});
    state[k] = 2;
  };
  visit({&file, i}, nullptr, nullptr);
  std::string decls;
  GpuInfo r = done[{&file, i}];
  for (auto& st : sh.structs) decls += st.wgsl, r.decls.push_back({st.name, st.wgsl});
  for (Key k : sh.tableOrder) decls += sh.tables[k].wgsl, r.decls.push_back({sh.tables[k].id, sh.tables[k].wgsl});
  r.decls.insert(r.decls.end(), fnDecls.begin(), fnDecls.end());
  r.wgsl = decls + (decls.empty() ? "" : "\n") + all;
  return r;
}

// ---- what the runtime keeps: each gpu fn's translation, by its function's name ----
static Mutex gpuMu;
static std::unordered_map<int, GpuInfo> gpuInfos;
void gpuInfoSet(int sym, GpuInfo i) {
  std::lock_guard g(gpuMu);
  gpuInfos[sym] = std::move(i);
}
static GpuInfo gpuInfoOf(V f) {
  if (!is(f, K::Fn)) ERR("@argument wgsl() expects a gpu fn, or a list of them, got %s", typeName(f));
  GpuInfo i;
  bool found = false;
  {  // (no error while holding the lock: an error unwinds past the guard)
    std::lock_guard g(gpuMu);
    auto it = gpuInfos.find(as<Fn>(f)->name);
    if (it != gpuInfos.end()) i = it->second, found = true;
  }
  if (!found) ERR("@argument wgsl(): %s is not a gpu fn (declare it gpu fn %s(...))", symName(as<Fn>(f)->name).c_str(), symName(as<Fn>(f)->name).c_str());
  return i;
}
// wgsl([f, g, ...]): their translations in one, each struct, const and fn once (what they share, they share), and the
// entries, in order. Two declarations of one name that differ (structs of one name from two files) are an error.
static V wgslOfAll(V fs) {
  std::vector<GpuInfo> infos;
  for (V f : as<List>(fs)->v) infos.push_back(gpuInfoOf(f));
  std::map<std::string, std::string> seen;
  std::string types, fns, clash;
  for (auto& i : infos)
    for (auto& [name, text] : i.decls) {
      auto [it, fresh] = seen.emplace(name, text);
      if (!fresh) {
        if (it->second != text && clash.empty()) clash = name;
        continue;
      }
      (text.compare(0, 3, "fn ") == 0 ? fns : types) += text + (text.compare(0, 3, "fn ") == 0 ? "\n" : "");
    }
  if (!clash.empty()) ERR("@argument wgsl(): these gpu fns use two different declarations named %s (structs of one name in two files?): rename one", clash.c_str());
  Map* m = make<Map>();
  volatile V keep[2] = {box(m)};
  (void)keep;
  List* entries = make<List>();
  keep[1] = box(entries);
  for (auto& i : infos) entries->v.push_back(box(newStr(i.entry)));
  mapPut(m, "wgsl", box(newStr(types + (types.empty() ? "" : "\n") + fns))), mapPut(m, "entries", box(entries));
  return box(m);
}
static V n_wgsl(V f) {
  if (is(f, K::List)) return wgslOfAll(f);
  GpuInfo i = gpuInfoOf(f);
  Map* m = make<Map>();
  volatile V keep[3] = {box(m)};
  (void)keep;
  List* names = make<List>();
  keep[1] = box(names);
  List* types = make<List>();
  keep[2] = box(types);
  for (auto& n : i.names) names->v.push_back(box(newStr(n)));
  for (auto& t : i.types) types->v.push_back(box(newStr(t)));
  mapPut(m, "wgsl", box(newStr(i.wgsl))), mapPut(m, "entry", box(newStr(i.entry))), mapPut(m, "name", box(newStr(i.name))), mapPut(m, "result", box(newStr(i.result)));
  mapPut(m, "names", box(names)), mapPut(m, "types", box(types));
  return box(m);
}
void gpuInit() { native<n_wgsl>("wgsl"); }
