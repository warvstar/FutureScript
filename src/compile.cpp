// Single-pass compiler: the parser emits machine code directly (no AST, no IR), through a code generator:
// gen_a64.h (arm64) natively, gen_wasm.h in the WebAssembly build.
#include "compile.h"
#include <map>
#if __wasm__
#include "gen_wasm.h"
#else
#include "gen_a64.h"
#endif

static uint64_t fnv(const char* a, const char* b) {
  uint64_t h = 1469598103934665603ull;
  for (; a < b; a++) h = (h ^ (uint8_t)*a) * 1099511628211ull;
  return h;
}
// A function's source and where it is installed, so it can be compiled again once it is hot.
struct Tier { std::string name, text, file; int line = 0, cls = -1, sym = -1; bool init = false; u32 *stub = nullptr, *body = nullptr, *fast = nullptr; Fn* fn = nullptr; bool failed = false; };
static std::unordered_map<std::string, Tier> tiers;  // by owner; nodes are stable, so tier-1 code embeds their address
struct Pend { u32* stub; Fn* fn; int arity; Code k; std::string owner; bool lambda; Tier tier; };  // body to install + stub to repoint
struct ClassDef { int sym, super; std::vector<std::pair<int, Fn*>> methods; };
struct Span { const char *start = nullptr, *end = nullptr; int line = 0, endLine = 0, arity = 0; };  // a fn/method in the source
struct Rec { uint64_t text, env; int line, arity; u32* body; std::vector<u32*> lambdas; bool repl; };  // what is installed now
static std::unordered_map<std::string, Rec> recs;
static std::unordered_map<int, uint64_t> varText;  // top-level var -> hash of its declaration's source
static std::map<std::pair<int, int>, Fn*> methodFns;  // (class sym, method sym): one Fn per method, kept across reloads
// Modules: a file imported `as` a name declares its top-level names as "prefix.name" (prefix: the file's stem).
static std::unordered_map<std::string, std::string> nsOf;                             // module path -> prefix
static std::unordered_map<std::string, std::unordered_map<int, std::string>> aliases;  // file -> its `as` names

struct Compiler : Gen {
  bool reload = false, echo = false, tiering = false, topTry = false;
  std::string owner;
  struct Source { std::string text; bool compiled = false; };
  std::unordered_map<std::string, Source> sources;  // by path; nodes are stable, so text pointers stay valid
  std::vector<std::string> files, present;
  std::unordered_map<std::string, Span> spans;
  std::vector<char> declKind;  // by symbol
  std::vector<std::pair<std::string, int>> shifts;
  uint64_t envHash = 1469598103934665603ull;
  std::vector<Pend> pend;
  std::vector<ClassDef> classes;
  std::vector<std::pair<int, int>> fnDefs;  // (sym, arity)
  std::vector<int> inits;                   // top-level vars initialised by this chunk
  struct Extern { int sym; void* fn; std::string sig; char ret; };
  std::vector<Extern> externs;
  std::string patched, added, removed, reset;  // for the reload log

  static void add(std::string& s, const std::string& n) { s += (s.empty() ? "" : ", ") + n; }
  const char* name(int s) { return symName(s).c_str(); }
  int qualify(int s) {  // a name declared at the top level of the current file
    auto it = nsOf.find(file);
    return it == nsOf.end() ? s : intern(it->second + "." + symName(s));
  }
  Tok topName(Tok t) {  // the top-level name t (just read) refers to; reads `.name` after a module alias
    const std::string* m = moduleAlias(t.sym);
    if (!m) return t.sym = globalSym(t.sym), t;
    if (!accept('.')) failAt(t.p, t.line, "'%s' is a module: use %s.name", name(t.sym), name(t.sym));
    Tok n = cur;
    if (!findGlobal(n.sym = intern(*m + "." + symName(ident())))) failAt(n.p, n.line, "module '%s' has no '%s'", name(t.sym), name(prev.sym));
    return n;
  }
  int globalSym(int s) { int q = qualify(s); return q != s && findGlobal(q) ? q : s; }  // own module first, then global
  const std::string* moduleAlias(int s) {
    auto f = aliases.find(file);
    auto it = f == aliases.end() ? decltype(f->second.end()){} : f->second.find(s);
    return f != aliases.end() && it != f->second.end() ? &it->second : nullptr;
  }
  Tok lex(Lexer& l) {
    Tok t = l.next();
    if (t.t == T_ERR) lexAtEnd = !*l.p, failAt(t.p, t.line, "%s", l.err);
    return t;
  }
  void next() { prev = cur, cur = lex(lx); }
  Tok peek() { Lexer s = lx; Tok t = s.next(); return t.t == T_ERR ? Tok{} : t; }  // lex errors surface on next()
  bool peekIs(int t) { return peek().t == t; }
  bool at(int t) { return cur.t == t; }
  bool atKw(int k) { return cur.t == T_ID && cur.sym == k; }
  bool accept(int t) { return at(t) ? next(), true : false; }
  void expect(int t) { if (!accept(t)) fail("expected '%c'", t); }
  int ident() {
    if (!at(T_ID) || cur.sym < NKW) fail("expected a name");
    return next(), prev.sym;
  }
  void konst(std::string_view s) { consts.push_back(box(newStr(s))), imm(0, consts.back()); }

  void proveOperands(int op, bool lnum, bool rnum, int lh, int rh) {
    if (op == O_ADD || op == O_EQ || op == O_NE) return;
    if (op <= O_MOD || op >= O_BAND || rnum) prove(lh);
    if (op <= O_MOD || op >= O_BAND || lnum) prove(rh);
  }
  void loopTop(Loop& lp) {  // facts must hold on every back-edge: tier 2 keeps those of locals the loop never assigns
    auto had = F->facts;
    F->facts.reset();
    if (F->tier2) for (auto& l : F->locals) if (tracked(l.home) && had[l.home + 128] && !assignedIn(l.sym)) prove(l.home);
    lp.tries = F->tries.size(), loopBegin(lp);
  }

  void combine(int s, int op) { mv(1, 0), ld(0, s), F->depth--, binop(op); }  // left in slot s, right in x0
  // x0 = x0 op <operand binding tighter than p>; returns whether the result is known to be a number
  bool rhs(int op, int p = 0, bool lnum = false, int lh = NOH) {  // lh: the left operand's local, if any
    bool rnum = right(p);
    int rh = rHome;
    binop(op, lnum, rnum), proveOperands(op, lnum, rnum, lh, rh);
    return (lnum && rnum && op <= O_MOD) || op >= O_BAND;
  }
  int rHome = NOH;  // the local right() loaded, if it was one
  bool right(int p, bool keep = true) {  // x1 = the right operand (x0 kept, if keep); returns whether it is known to be a number
    rHome = NOH;
    if (int k = simpleOperand(p)) return k == 2;
    int s = slot();
    if (keep) st(0, s);
    bool num = expr(p + 1);
    mv(1, 0), F->depth--, rHome = NOH;
    if (keep) ld(0, s);
    return num;
  }

  // ---- names ----
  LV resolve(Fun* f, int sym) {  // local, else captured from an enclosing function (lambdas), else LV_NONE
    for (auto i = f->locals.rbegin(); i != f->locals.rend(); ++i) if (i->sym == sym) return {LV_LOCAL, i->home, 0, false, i->num || proven(f, i->home)};
    for (size_t i = 0; i < f->caps.size(); i++) if (f->caps[i].first == sym) return {LV_CAP, (int)i};
    if (!f->parent) return {LV_NONE};
    LV p = resolve(f->parent, sym);
    if (p.k == LV_NONE) return p;
    f->caps.push_back({sym, p});
    return {LV_CAP, (int)f->caps.size() - 1};
  }
  void load(LV& v) {  // materialise v into x0
    switch (v.k) {
      case LV_LOCAL: ldl(0, v.a); break;
      case LV_GLOBAL: gmem(0, v.a); break;
      case LV_CAP: capLoad(v.a); break;
      case LV_FIELD: field(v.b, false); break;                                // object in x0
      case LV_INDEX: mv(1, 0), ld(0, v.a), F->depth--, helper(H_GETI); break;  // index in x0, object in slot a
    }
    v.k = LV_VAL;
  }

  // ---- calls: arguments go to consecutive slots (the "window") ----
  int exprList(int close, bool keepLast = false) {  // keepLast: leave the last value in x0, not in a slot
    int n = 0;
    if (!accept(close)) {
      do expr(), n++;
      while (accept(',') && (st(0, slot()), true));
      if (!keepLast) st(0, slot());
      expect(close);
    }
    return n;
  }
  void call(int sym, bool super) { int base = slot(); st(0, base), callWindow(base, exprList(')'), sym, super); }
  void construct(Global& g) {  // C(args) for a top-level class C
    int base = slot();
    gmem(0, g.slot), st(0, base);
    constructCall(base, exprList(')'));
  }
  int arityOf(int sym, Global& g) {
    if (auto it = spans.find(symName(sym)); it != spans.end()) return it->second.arity;
    return g.o && g.o->kind == K::Fn ? ((Fn*)g.o)->arity : -1;
  }
  void directCall(const Tok& t, Global& g) {  // known top-level fn: called through its stub; reloads repoint the stub
    int base = F->depth, n = exprList(')', true), a = arityOf(t.sym, g);
    if (a < 0) failAt(t.p, t.line, "'%s' is not defined", name(t.sym));
    if (a != n) failAt(t.p, t.line, "'%s' expects %d arguments, got %d", name(t.sym), a, n);
    callDirect(base, n, g.stub);
  }

  // ---- literals ----
  void strLit(const Tok& t) {  // "text {expr} text": interpolation compiles to concatenation
    const char *p = t.p, *e = t.p + t.len;
    std::string lit;
    bool any = false;
    auto flush = [&](bool force) {
      if (lit.empty() && !force) return;
      if (any) { int s = slot(); st(0, s), konst(lit), combine(s, O_ADD); }
      else konst(lit);
      any = true, lit.clear();
    };
    for (; p < e; p++) {
      if (*p == '\\' && p + 1 < e) {
        char c = *++p;
        lit += c == 'n' ? '\n' : c == 't' ? '\t' : c == '0' ? '\0' : c;
      } else if (*p == '{') {
        const char* q = p + 1;
        for (int d = 1; q < e && (d += (*q == '{') - (*q == '}')); q++) {}
        if (q >= e) failAt(p, t.line, "unclosed '{' in string");
        flush(true);  // always start from a string, so `"{x}"` is a string even when x is a number
        int s = slot(), line = t.line + (int)std::count(t.p, p, '\n');
        st(0, s), subExpr(p + 1, q, line), combine(s, O_ADD);
        p = q;
      } else lit += *p;
    }
    flush(!any);
  }
  // Parse other source text in the middle of this one (interpolations, imports), then resume.
  template <class Body> void detour(const std::string& f, const char* src, const char* at, int line, Body body) {
    Lexer sl = lx;
    Tok sc = cur, sp = prev;
    std::string sf = file;
    const char* ss = srcStart;
    file = f, srcStart = src, lx = Lexer{at, line}, next();
    body();
    lx = sl, cur = sc, prev = sp, file = sf, srcStart = ss;
  }
  void subExpr(const char* s, const char* e, int line) {  // compile source text [s, e) as one expression
    detour(file, srcStart, s, line, [&] {
      expr();
      if (cur.p != e) failAt(cur.p, cur.line, "expected '}' to end the interpolation");
    });
  }
  void collect(int base, int n, int h) { addsp(0, base), imm(1, n), helper(h), F->depth = base; }  // list/map from slots
  void mapLit() {  // {key: value, name: value}; a bare name before ':' is a string key
    int base = F->depth, n = 0;
    if (!accept('}')) {
      do {
        if (at(T_ID) && cur.sym >= NKW && peekIs(':')) next(), konst(symName(prev.sym));
        else expr();
        st(0, slot()), expect(':'), expr(), st(0, slot()), n++;
      } while (accept(','));
      expect('}');
    }
    collect(base, n, H_MAP);
  }
  void lambda(const Tok& t) {  // fn(params) { body }: captures enclosing locals by value into the closure
    if (tiering) compileFail("tier 2 does not handle lambdas", false);
    Fun f;
    f.parent = F, f.lambda = true;
    Fn* fn = make<Fn>();
    consts.push_back(box(fn));
    fn->code = newStub(), fn->name = intern("fn@" + std::to_string(t.line));
    fn->arity = function(f, -1, false, symName(fn->name), (u32*)fn->code, fn);
    int base = F->depth, n = (int)f.caps.size();
    for (auto& [s, src] : f.caps) { LV v = src; load(v), st(0, slot()); }
    closure(fn, base, n);
  }

  // ---- expressions: result in x0 ----
  LV primary() {
    next();
    Tok t = prev;
    switch (t.t) {
      case T_NUM: return imm(0, box(t.n)), NUM;
      case T_STR: return strLit(t), LV{};
      case '(': { LV v; v.num = expr(); return expect(')'), v; }
      case '{': return mapLit(), LV{};
      case '[': { int base = F->depth, n = exprList(']'); return collect(base, n, H_LIST), LV{}; }
      case T_ID: break;
      default: failAt(t.p, t.line, t.t == T_EOF ? "unexpected end of file" : "unexpected '%c'", t.t);
    }
    switch (t.sym) {
      case K_true: return imm(0, TRUE_), LV{};
      case K_false: return imm(0, FALSE_), LV{};
      case K_nil: return imm(0, NIL), LV{};
      case K_fn: return lambda(t), LV{};
      case K_yield:  // yield [value]: suspend this fiber; evaluates to the value the next resume sends
        at('}') || at(')') || at(']') || at(',') || at(T_EOF) || cur.nl ? imm(0, NIL) : (void)expr();
        return helper(H_YIELD), LV{};
      case K_self: {
        LV v = resolve(F, K_self);
        if (v.k == LV_NONE) failAt(t.p, t.line, "'self' outside a method");
        return v.ro = true, v;
      }
      case K_super: {
        if (F->cls < 0) failAt(t.p, t.line, "'super' outside a method");
        expect('.');
        int s = ident();
        expect('('), ldl(0, F->selfHome), call(s, true);
        return {};
      }
    }
    if (t.sym < NKW) failAt(t.p, t.line, "unexpected '%s'", name(t.sym));
    if (LV v = resolve(F, t.sym); v.k != LV_NONE) return v;
    t = topName(t);
    Global* g = findGlobal(t.sym);
    if (!g) failAt(t.p, t.line, "undefined name '%s'", name(t.sym));
    if (g->kind == 'f' && at('(') && !cur.nl) return next(), directCall(t, *g), t.sym == F->self ? NUM : LV{};
    if (g->kind == 'c' && at('(') && !cur.nl) return next(), construct(*g), LV{};
    return {LV_GLOBAL, g->slot, 0, g->kind != 'v'};
  }
  LV postfix() {
    LV v = primary();
    for (;;) {
      if (accept('.')) {
        int s = ident();
        load(v);
        v = {LV_FIELD, 0, s};
        if (at('(') && !cur.nl) next(), call(s, false), v = {};
      } else if (at('[') && !cur.nl) {
        next(), load(v);
        int t = slot();
        st(0, t), expr(), expect(']'), v = {LV_INDEX, t};
      } else if (at('(') && !cur.nl) next(), load(v), call(0, false), v = {};
      else return v;
    }
  }
  LV unary() {
    if (accept('-')) {
      if (accept(T_NUM)) return imm(0, box(-prev.n)), NUM;
      return operand(), mv(1, 0), imm(0, 0), binop(O_SUB), LV{};
    }
    if (accept('!')) return operand(), notOp(), LV{};
    if (accept('~')) return operand(), hasLit = true, lit = -1, binop(O_BXOR), NUM;  // ~x is x ^ -1
    return postfix();
  }
  bool operand(int* h = nullptr) {  // x0 = a unary expression; returns whether it is known to be a number
    LV v = unary();
    if (h) *h = v.k == LV_LOCAL ? v.a : NOH;  // the local it came straight from, if any
    load(v);
    return v.num;
  }
  bool expr(int minp = 1) { int h; bool n = operand(&h); return binRest(minp, n, h), n; }
  // Precedence climbing; the left operand is in x0. num/h: whether it is a number / the local it came from.
  void binRest(int minp, bool& num, int& h) {
    for (int t, p; (p = prec(t = cur.t)) && p >= minp && !(t == '-' && cur.nl); h = NOH) {
      next();
      if (t == T_AND || t == T_OR) {
        int j = shortBegin(t == T_AND);
        auto f = F->facts;  // the right side may not run
        expr(p + 1), shortEnd(j), num = false, F->facts = f;
      } else num = rhs(opOf(t), p, num, h);
    }
  }
  // A right operand that is a local or a number literal (and binds no tighter operator) goes straight
  // into x1, so the left operand never round-trips through memory.
  // Returns 0 (not simple), 1 (local) or 2 (number).
  int simpleOperand(int p) {
    if (!at(T_NUM) && !(at(T_ID) && cur.sym >= NKW)) return 0;
    Tok n = peek();
    if (n.t == '.' || ((n.t == '(' || n.t == '[') && !n.nl) || prec(n.t) > p) return 0;
    if (at(T_NUM)) return hasLit = true, lit = cur.n, next(), 2;
    LV v = resolve(F, cur.sym);
    if (v.k != LV_LOCAL) return 0;
    return ldl(1, v.a), next(), rHome = v.a, v.num ? 2 : 1;
  }
  // An if/while condition. `a < b {` compiles to a compare and branch instead of building a boolean.
  Cond cond() {
    int lh;
    bool lnum = operand(&lh);
    binRest(5, lnum, lh);
    if (int t = cur.t, p = prec(t); p == 3 || p == 4) {
      next();
      int op = opOf(t);
      bool rnum = right(p);
      if (at('{')) {
        int rh = rHome;
        Cond c = compareCond(op, lnum, rnum);
        return proveOperands(op, lnum, rnum, lh, rh), c;
      }
      binop(op, lnum, rnum), proveOperands(op, lnum, rnum, lh, rHome);
      lnum = false, lh = NOH;
    }
    binRest(1, lnum, lh);
    return truthyCond();
  }

  // ---- statements ----
  template <class Body> void scope(Body body) {  // locals, slots and registers taken inside are freed after
    size_t nl = F->locals.size(), outer = F->scopeAt;
    int d = F->depth, r = F->nreg, fr = F->nfreg;
    F->scope++, F->scopeAt = nl, body();
    F->scope--, F->scopeAt = outer, F->locals.resize(nl), F->depth = d, F->nreg = r, F->nfreg = fr;
  }
  void block() {
    expect('{');
    scope([&] {
      while (!accept('}')) at(T_EOF) ? fail("expected '}'") : (void)stmt();
    });
  }
  bool stmt() {  // returns true for a bare expression statement (the REPL echoes those)
    if (accept(';')) return false;
    if (at('{')) return block(), false;
    if (at(T_ID)) switch (cur.sym) {
        case K_var: {
          next();
          int s = ident();
          bool num = accept('=') ? expr() : (imm(0, NIL), false);
          if (F->top && F->scope == 0) return gmem(0, global(qualify(s)).slot, true), false;
          auto it = std::find_if(F->locals.begin() + F->scopeAt, F->locals.end(), [&](auto& l) { return l.sym == s; });
          if (it != F->locals.end()) return stl(0, it->home), false;  // redeclared in the same block: reuse
          int h = num ? fhome() : home();
          if (stl(0, h), num) prove(h);
          return F->locals.push_back({s, h}), false;
        }
        case K_if: return next(), ifStmt(), false;
        case K_while: {
          next();
          Loop lp;
          loopTop(lp);
          Cond c = cond();
          return loopExit(lp, c), loop(lp, [] {}), false;
        }
        case K_for: return next(), forStmt(), false;
        case K_try: return next(), tryStmt(), false;
        case K_throw: return next(), expr(), helper(H_THROW), F->live = false, false;
        case K_return:
          next();
          if (F->top) fail("'return' outside a function");
          F->retNum &= at('}') || at(T_EOF) || cur.nl ? (imm(0, NIL), false) : expr();
          return leaveTries(0), ret(), false;  // leaving every try
        case K_break: case K_continue:
          next();
          if (!F->loop) failAt(prev.p, prev.line, "'%s' outside a loop", name(prev.sym));
          leaveTries(F->loop->tries), jump(*F->loop, prev.sym == K_break), F->live = false;
          return false;
        case K_class: case K_import: fail("'%s' is only allowed at top level", name(cur.sym));
        case K_fn: if (!peekIs('(')) fail("named functions are only allowed at top level; use 'var f = fn(...) {...}'");
      }
    LV v = unary();
    int t = cur.t;
    if (t != '=' && (t < T_ADDEQ || t > T_DIVEQ)) {
      int h = v.k == LV_LOCAL ? v.a : NOH;
      bool n = v.num;
      return load(v), binRest(1, n, h), true;
    }
    if (v.k == LV_VAL || v.ro) fail("cannot assign to this");
    next(), assign(v, t == '=' ? -1 : O_ADD + (t - T_ADDEQ));
    return false;
  }
  // The body, then `step` (the continue target), a safepoint, and the back-edge to the top.
  template <class Step> void loop(Loop& lp, Step step) {
    Loop* outer = F->loop;
    bodyBegin(lp);
    F->loop = &lp, block(), F->loop = outer;
    contHere(lp), step(), loopBack(lp);
    F->facts.reset(), F->live = true;
    loopEnd(lp);
  }
  // try { } catch e { }: a throw (or runtime error) inside, at any call depth, continues at the catch with e = the value.
  void tryStmt() {
    TryAt t = tryBegin();
    F->tries.push_back(t.r), block(), F->tries.pop_back();
    bool l1 = F->live;
    tryCatch(t);
    F->facts.reset(), F->live = true;  // the handler: anything in the try may have run
    F->depth = t.r;
    if (!atKw(K_catch)) fail("expected 'catch'");
    next();
    scope([&] {
      if (at(T_ID) && !atKw(K_try)) {
        int s = ident(), h = home();
        stl(0, h), F->locals.push_back({s, h});
      }
      block();
    });
    tryEnd(t), F->live |= l1, F->facts.reset();
  }
  void ifStmt() {
    Cond no = cond();
    thenBegin(no);
    auto f0 = F->facts;
    block();
    auto f1 = F->facts;
    bool l1 = F->live, hasElse = atKw(K_else);
    int e = elseBegin(no, hasElse);
    F->facts = f0, F->live = true;
    if (hasElse) next(), atKw(K_if) ? (next(), ifStmt()) : block();
    ifEnd(e);
    if (!l1) return;                                         // the then-branch never falls through
    if (!F->live) return (void)(F->facts = f1, F->live = true);  // only the then-branch does
    F->facts &= f1;
  }
  bool hasTry() {  // does the function starting at cur (parameters, then body) contain `try`?
    Lexer s = lx;
    int d = 0;
    for (Tok p = cur; p.t != T_EOF && p.t != T_ERR; p = s.next())
      if (p.t == '{') d++;
      else if (p.t == '}' && --d == 0) return false;
      else if (p.t == T_ID && p.sym == K_try) return true;
    return false;
  }
  bool assignedIn(int sym) {  // might the block starting at cur assign to sym? (conservative token scan)
    Lexer s = lx;
    int d = 0;
    for (Tok p = cur, n; p.t != T_EOF && p.t != T_ERR; p = n) {
      if (p.t == '{') d++;
      else if (p.t == '}' && --d == 0) return false;
      n = s.next();
      if (p.t == T_ID && p.sym == sym && (n.t == '=' || (n.t >= T_ADDEQ && n.t <= T_DIVEQ))) return true;
    }
    return true;
  }
  void forStmt() {  // for x in a..b { }  |  for x in list/map/string/fiber { }
    int nm = ident();
    if (!atKw(K_in)) fail("expected 'in'");
    next();
    scope([&] {
      expr();
      Loop lp;
      if (accept(T_DOTDOT)) {
        int a = fhome(), b = (helper(H_NUMCHK), stl(0, a), expr(), helper(H_NUMCHK), fhome());
        stl(0, b);
        int v = assignedIn(nm) ? home() : numHome();
        loopTop(lp);
        F->locals.push_back({nm, v, !assignedIn(nm)});
        rangeTest(lp, a, b, v);
        return loop(lp, [&] { rangeStep(a); });
      }
      int a = home(), b = home(), v = home();  // the collection, a raw integer index, the element
      stl(0, a), imm(0, 0), stl(0, b), F->locals.push_back({nm, v});
      loopTop(lp);
      ldl(0, a), ldl(1, b), helper(H_ITER), exitIfEnd(lp);
      stl(0, v);
      loop(lp, [&] { ldl(0, b), addImm(0, 1), stl(0, b); });
    });
  }
  void assign(LV v, int op) {  // op < 0: plain '=', else compound
    if (v.k == LV_LOCAL && isFReg(v.a) && op >= 0 && op <= O_DIV) {
      unprove(v.a);
      bool rnum = right(0, false);  // the left side is the local itself
      return fpAssign(v.a, op, rnum);
    }
    auto value = [&](bool lnum = false) { return op < 0 ? expr() : rhs(op, 0, lnum, v.k == LV_LOCAL ? v.a : NOH); };
    if (v.k == LV_LOCAL || v.k == LV_GLOBAL || v.k == LV_CAP) {
      LV t = v;
      if (op >= 0) load(t);
      bool num = value(t.num);
      if (v.k == LV_LOCAL) {
        stl(0, v.a);
        if (num) prove(v.a);
      } else if (v.k == LV_GLOBAL) gmem(0, v.a, true);
      else capStore(v.a);
    } else if (v.k == LV_FIELD) {
      int o = slot();
      st(0, o);
      if (op >= 0) field(v.b, false);
      value(), mv(2, 0), ld(0, o), field(v.b, true), F->depth--;
    } else {  // object in slot v.a, index in x0
      int i = slot();
      st(0, i);
      if (op >= 0) mv(1, 0), ld(0, v.a), helper(H_GETI);
      value(), mv(2, 0), ld(0, v.a), ld(1, i), helper(H_SETI), F->depth -= 2;
    }
  }

  // ---- declarations ----
  // Compile `(params) { body }` and queue it to be installed behind `stub`. Returns the arity.
  int function(Fun& f, int cls, bool init, const std::string& nm, u32* stub, Fn* fn) {
    f.cls = cls, f.init = init, f.k.name = nm, f.k.file = file;
    Fun* outer = F;
    F = &f, f.noRegs = hasTry();
    std::vector<int> homes;
    if (cls >= 0) homes.push_back(f.selfHome = home()), f.locals.push_back({K_self, f.selfHome});
    if (f.lambda) homes.push_back(f.cloHome = home());  // the closure itself, for captured variables
    int first = (int)homes.size();  // the first declared parameter
    expect('(');
    if (!accept(')')) {
      do {
        int s = ident(), h = home();  // not d8..: arguments arrive, and calls pass them, in x registers
        f.locals.push_back({s, h}), homes.push_back(h);
      } while (accept(','));
      expect(')');
    }
    int arity = (int)homes.size();
    if (arity > 64) fail("too many parameters");
    for (int i = 0; i < arity; i++) param(i, homes[i]);
    for (int i = first; i < arity && i < 8; i++)
      if (f.spec >> (i - first) & 1) f.guard.push_back(i), prove(homes[i]);
    Tier* tier = nullptr;
    if (!f.lambda && !tiering) hotCounter(tier = &tiers[nm], first, homes);
    block();
    if (f.live) f.retNum = false;  // can end without a return: returns nil
    pend.push_back({stub, fn, arity, finish(), owner, f.lambda, {}});
    if (tier) {
      Span& sp = spans.at(nm);
      pend.back().tier = {nm, std::string(sp.start, sp.end), file, sp.line, cls, cls < 0 ? intern(nm) : -1, init, stub, nullptr, nullptr, fn, false};
    }
    F = outer;
    return arity;
  }
  bool unchanged(const std::string& q) {  // incremental reload: skip a body whose text and environment match
    present.push_back(q);
    Span& sp = spans.at(q);
    auto it = recs.find(q);
    if (it == recs.end() || it->second.text != fnv(sp.start, sp.end) || it->second.env != envHash) return false;
    if (it->second.line != sp.line) shifts.push_back({q, sp.line - it->second.line});
    lx.p = sp.end, lx.line = sp.endLine, next();
    return true;
  }
  void fnDecl() {
    int s = qualify(ident());
    std::string q = symName(s);
    if (unchanged(q)) return fnDefs.push_back({s, recs[q].arity});
    Fun f;
    owner = q, fnDefs.push_back({s, function(f, -1, false, q, global(s).stub, nullptr)});
  }
  void classDecl() {
    ClassDef cd{qualify(ident()), -1, {}};
    if (accept(':')) {
      ident();
      Global* g = findGlobal(cd.super = topName(prev).sym);
      if (!g || g->kind != 'c' || cd.super == cd.sym) failAt(prev.p, prev.line, "bad superclass '%s'", name(cd.super));
    }
    expect('{');
    while (!accept('}')) {
      if (!atKw(K_fn)) fail("expected 'fn' in class body");
      next();
      int m = ident();
      for (auto& [x, f] : cd.methods) if (x == m) failAt(prev.p, prev.line, "duplicate method '%s'", name(m));
      Fn*& f = methodFns[{cd.sym, m}];
      if (!f) f = make<Fn>(), f->code = newStub(), f->name = m, consts.push_back(box(f));
      cd.methods.push_back({m, f});
      std::string q = symName(cd.sym) + "." + symName(m);
      if (unchanged(q)) continue;
      Fun fun;
      owner = q, function(fun, cd.sym, m == S_init, q, (u32*)f->code, f);
    }
    classes.push_back(std::move(cd));
  }
  // extern ["library"] fn name(type, ...) [: type] -- a C function (a host function in wasm); types: num int long ptr str (and void)
  void externDecl() {
    std::string lib;
    Tok lt = cur;
    if (accept(T_STR)) lib.assign(prev.p, prev.len);
    if (!atKw(K_fn)) fail("expected 'fn'");
    next();
    Tok t = cur;
    int s = ident();
    const char* err = nullptr;
    void* fn = externLookup(lib, symName(s), &err);
    if (err) failAt(lt.p, lt.line, "cannot load '%s'", lib.c_str());
    if (!fn) failAt(t.p, t.line, "no C function '%s'", name(s));
    auto type = [&](bool ret) {
      int ty = ident();
      for (const char* n : {"num", "int", "long", "ptr", "str", "void"})
        if (symName(ty) == n && (ret || n[0] != 'v')) return n[0] == 'n' ? 'd' : n[0];
      failAt(prev.p, prev.line, "unknown C type '%s' (num, int, long, ptr, str%s)", name(ty), ret ? ", void" : "");
    };
    std::string sig;
    expect('(');
    if (!accept(')')) {
      do sig += type(false); while (accept(','));
      expect(')');
    }
    if (sig.size() > 7) failAt(t.p, t.line, "extern functions take at most 7 parameters");
    externs.push_back({qualify(s), fn, sig, accept(':') ? type(true) : 'v'});
  }
  void declare(const Tok& t, char kind, int arity) {
    Global& g = global(t.sym);
    if (g.kind == 'n') failAt(t.p, t.line, "cannot redefine builtin '%s'", name(t.sym));
    if (char& k = growAt(declKind, t.sym); k && (kind != 'v' || k != 'v')) failAt(t.p, t.line, "'%s' is already defined", name(t.sym));
    else k = kind;
    g.kind = kind;
    if (kind == 'f' && !g.stub) g.stub = newStub();
    envHash = (envHash ^ (uint64_t(t.sym) << 16 | uint64_t(kind) << 8 | uint64_t(arity))) * 1099511628211ull;
  }
  std::string resolvePath(std::string_view rel) {
    std::string p(rel);
    if (size_t d = file.rfind('/'); p[0] != '/' && d != std::string::npos) p = file.substr(0, d + 1) + p;
    return canonicalPath(p);
  }
  // Token pre-pass: learn every top-level name, arity and fn/method source span, following imports.
  void prescan(const std::string& path, const char* src) {
    std::string savedFile = file;
    const char* savedStart = srcStart;
    file = path, srcStart = src, aliases[path].clear();
    auto q = [&](Tok t) { return t.sym = qualify(t.sym), t; };
    struct Open { std::string q; const char* start; int line, depth; };
    std::vector<Open> open;
    Lexer L{src};
    int depth = 0, cls = -1;
    bool ext = false;  // after `extern` (a C function: no body)
    for (Tok t = lex(L), p; t.t != T_EOF; p = t, t = lex(L)) {
      bool afterKw = p.t == T_ID && t.t == T_ID;
      topTry |= t.t == T_ID && t.sym == K_try && open.empty();  // a try in top-level code
      if (t.t == T_ID && t.sym == K_extern && !depth) ext = true;
      else if (ext && afterKw && p.sym == K_fn) {
        Lexer s = L;
        int n = 0;
        if (lex(s).t == '(') for (Tok a = lex(s); a.t != ')' && a.t != T_EOF; a = lex(s)) n += a.t == T_ID;
        declare(q(t), 'x', n), ext = false;
      } else if (t.t == '{') depth++;
      else if (t.t == '}') {
        if (--depth == 0) cls = -1;
        if (!open.empty() && depth == open.back().depth) {
          Span& sp = spans[open.back().q];
          sp.start = open.back().start, sp.end = L.p, sp.line = open.back().line, sp.endLine = L.line;
          open.pop_back();
        }
      } else if (afterKw && p.sym == K_fn && (depth == 0 || (depth == 1 && cls >= 0))) {
        std::string qn = depth ? symName(cls) + "." + symName(t.sym) : symName(qualify(t.sym));
        if (spans.count(qn)) failAt(t.p, t.line, "'%s' is already defined", qn.c_str());
        Lexer s = L;
        int n = 0;
        if (lex(s).t == '(') for (Tok a = lex(s); a.t != ')' && a.t != T_EOF; a = lex(s)) n += a.t == T_ID;
        spans[qn].arity = n;
        open.push_back({qn, p.p, p.line, depth});
        if (!depth) declare(q(t), 'f', n);
      } else if (depth == 0 && afterKw && (p.sym == K_class || p.sym == K_var)) {
        declare(q(t), p.sym == K_class ? 'c' : 'v', 0);
        if (p.sym == K_class) cls = qualify(t.sym);
      } else if (depth == 0 && t.t == T_STR && p.t == T_ID && p.sym == K_import) {
        std::string ip = resolvePath({t.p, (size_t)t.len});
        Lexer s = L;
        if (Tok a = lex(s); a.t == T_ID && a.sym == S_as) {  // import "file" as name: a module
          Tok n = lex(s);
          if (n.t != T_ID || n.sym < NKW) failAt(n.p, n.line, "expected a module name after 'as'");
          std::string& pre = nsOf[ip];
          if (pre.empty()) {  // the file's stem, made unique
            pre = ip.substr(ip.rfind('/') + 1), pre = pre.substr(0, pre.find('.'));
            for (bool clash = true; clash;) {
              clash = false;
              for (auto& [f, p2] : nsOf) clash |= f != ip && p2 == pre;
              if (clash) pre += "_";
            }
          }
          aliases[path][n.sym] = pre;
        }
        if (sources.count(ip)) continue;
        std::string& text = sources[ip].text;
        if (!readFile(ip, text)) failAt(t.p, t.line, "cannot read '%s'", ip.c_str());
        files.push_back(ip), prescan(ip, text.c_str());
      }
    }
    file = savedFile, srcStart = savedStart;
  }

  // ---- top level ----
  void topLevel() {
    while (!at(T_EOF)) {
      if (atKw(K_fn)) { next(), fnDecl(); continue; }
      if (atKw(K_class)) { next(), classDecl(); continue; }
      if (atKw(K_extern)) { next(), externDecl(); continue; }
      if (atKw(K_import)) {
        next();
        if (!at(T_STR)) fail("expected a file name");
        next();
        std::string ip = resolvePath({prev.p, (size_t)prev.len});
        if (at(T_ID) && cur.sym == S_as) next(), ident();
        if (Source& s = sources.at(ip); !s.compiled) s.compiled = true, detour(ip, s.text.c_str(), s.text.c_str(), 1, [&] { topLevel(); });
        continue;
      }
      // On reload, top-level statements are compiled (to validate them) and then dropped, except
      // initialisers of vars that are new or whose declaration was edited. Other state survives the reload.
      size_t c = F->k.c.size(), r = F->k.rel.size(), l = F->k.lines.size();
      bool keep = !reload;
      const char* start = cur.p;
      int v = -1;
      if (Tok n = peek(); atKw(K_var) && n.t == T_ID) v = qualify(n.sym), keep |= !global(v).inited, inits.push_back(v);
      owner.clear();
      if (stmt() && echo) helper(H_ECHO);
      if (v >= 0 && !echo) {
        const char* end = cur.p;
        while (end > start && (uint8_t)end[-1] <= ' ') end--;
        uint64_t& h = varText[v], t = fnv(start, end);
        if (reload && h && h != t) keep = true, add(reset, symName(v));
        h = t;
      }
      if (!keep) F->k.c.resize(c), F->k.rel.resize(r), F->k.lines.resize(l);
    }
  }

  Compiled run(const std::string& path, const char* src) {
    files.push_back(path), sources[path].compiled = true;
    prescan(path, src);
    file = path, srcStart = src, lx = Lexer{src}, next();
    Fun top;
    top.top = true, top.noRegs = topTry, top.k.name = "<top>", top.k.file = path, F = &top;
    topLevel();
    Code chunk = finish();
    return commit(chunk);
  }

  // Tier 2: compile t's source again with its numeric parameters proven (behind an entry guard that falls
  // back to the tier-1 body) and, if `assume`, its recursive calls returning numbers. False if that failed.
  bool tier2(Tier& t, u32 spec, bool assume) {
    tiering = true, file = owner = t.file, srcStart = t.text.c_str(), lx = Lexer{srcStart, t.line};
    next(), next(), next();  // fn name (
    Fun f;
    f.tier2 = true, f.spec = spec, f.self = assume && t.cls < 0 ? t.sym : -1, deopt = t.body;
    function(f, t.cls, t.init, t.name, t.stub, t.fn);
    if (f.self >= 0 && !f.retNum) return false;
    Code& k = pend[0].k;
    bool guarded = !f.guard.empty();
    size_t last = k.c.size() - 1;
    u32* body = jitInstall(k);
    if (!WASM && guarded) patchStub(body + last, t.body);  // deopt: the branch to the tier-1 body (wasm: in the code)
    patchStub(t.stub, t.fast = body), jitCollect();       // jitCollect relinks direct calls
    if (verbose) fprintf(stderr, "[fut] tier 2: %s\n", t.name.c_str());
    return true;
  }

  // ---- commit: nothing above touched live code; from here on we patch it ----
  template <class T> T* globalObj(int sym) {  // the Fn/Class bound to a top-level name, kept across reloads
    Global& g = global(sym);
    if (!g.o || g.o->kind != T::KIND) g.o = make<T>(), ((T*)g.o)->name = sym;
    vm.globals[g.slot] = box(g.o);
    return (T*)g.o;
  }
  Compiled commit(Code& chunk) {
    Compiled out;
    out.files = files;
    // A function whose parameter count changed gets a fresh stub; the old stub now traps stale callers.
    std::unordered_map<void*, void*> remap;
    for (auto [s, a] : fnDefs) {
      Global& g = global(s);
      if (!g.o || g.o->kind != K::Fn || ((Fn*)g.o)->arity == a) continue;
      u32* fresh = newStub();
      patchStub(g.stub, staleThunk(s));
      remap[g.stub] = fresh, g.stub = fresh;
    }
    auto fix = [&](Code& k) { for (auto& [i, t] : k.rel) if (auto it = remap.find(t); it != remap.end()) t = it->second; };
    std::unordered_map<std::string, std::vector<u32*>> lambdas;
    for (auto& p : pend) {
      if (auto it = remap.find(p.stub); it != remap.end()) p.stub = (u32*)it->second;
      fix(p.k);
      u32* body = jitInstall(p.k);
      patchStub(p.stub, body);
      if (!p.lambda) {  // a new tier-1 body: any tier-2 version of the old one goes
        Tier& t = tiers[p.owner];
        if (t.fast) jitRetire(t.fast);
        t = std::move(p.tier), t.stub = p.stub, t.body = body;
      }
      if (p.fn) p.fn->arity = p.arity;
      if (p.lambda) { lambdas[p.owner].push_back(body); continue; }
      Span& sp = spans.at(p.owner);
      uint64_t text = fnv(sp.start, sp.end);
      auto old = recs.find(p.owner);
      if (old != recs.end()) {
        jitRetire(old->second.body), add(patched, p.owner);
        if (p.owner == "main" && reload && old->second.text != text) out.mainChanged = true;
      } else if (reload) add(added, p.owner);
      recs[p.owner] = {text, envHash, sp.line, p.arity, body, std::move(lambdas[p.owner]), echo};
    }
    for (auto& [q, d] : shifts) {
      Rec& r = recs[q];
      r.line += d, jitShiftLines(r.body, d);
      for (u32* b : r.lambdas) jitShiftLines(b, d);
    }
    if (!echo) {  // in a file (not the REPL), definitions that disappeared from the source are dropped (not ones typed in)
      std::sort(present.begin(), present.end());
      std::erase_if(recs, [&](auto& r) { return !r.second.repl && !std::binary_search(present.begin(), present.end(), r.first) && (add(removed, r.first), true); });
    }
    for (auto [s, a] : fnDefs) {
      Fn* f = globalObj<Fn>(s);
      f->code = global(s).stub, f->arity = a;
    }
    for (auto& e : externs) {
      Fn* f = globalObj<Fn>(e.sym);
      f->code = ffiThunk(e.fn, e.sig, e.ret, e.sym), f->arity = (int)e.sig.size();
    }
    for (auto& cd : classes) globalObj<Class>(cd.sym);  // all exist before any superclass link is made
    for (auto& cd : classes) {
      Class* c = globalObj<Class>(cd.sym);
      c->super = cd.super >= 0 ? (Class*)global(cd.super).o : nullptr;
      c->methods = cd.methods;
    }
    for (int s : inits) global(s).inited = true;
    clearICs();  // method tables or arities may have changed
    fix(chunk);
    out.chunk = jitInstall(chunk);
    jitCollect();
    out.log = patched.empty() && added.empty() && removed.empty() && reset.empty() ? "no code changes" : "";
    for (auto [label, list] : {std::pair{"patched ", &patched}, {"added ", &added}, {"removed ", &removed}, {"reset ", &reset}})
      if (!list->empty()) out.log += (out.log.empty() ? "" : "; ") + (label + *list);
    return out;
  }
};

void tierUp(void* p, u32 numArgs) {
  Tier& t = *(Tier*)p;
  if (t.fast || t.failed || !t.body) return;
  JitBatch writable;
  static Compiler* c;  // on the heap: a compile error may skip its destructor (wasm longjmps)
  for (bool assume : {true, false}) {
    struct { Tier* t; u32 m; bool assume, ok; } job{&t, numArgs, assume, false};
    bool done = guardCompile([](void* v) {
      auto& j = *(decltype(job)*)v;
      delete c, c = new Compiler, j.ok = c->tier2(*j.t, j.m, j.assume);
    }, &job);
    if (!done) break;  // something tier 2 does not handle (a lambda)
    if (job.ok) return;
  }
  t.failed = true;
}
// The compile in progress lives on the heap: a compile error unwinds past compile() (wasm: by longjmp, which runs
// no destructors), and the next compile frees what it left.
static Compiler* active;
Compiled compile(const std::string& path, bool reload, bool echo, const std::string* text) {
  JitBatch writable;  // nothing JIT-compiled runs during a compile
  delete active;
  Compiler& c = *(active = new Compiler);
  c.reload = reload, c.echo = echo;
  std::string& src = c.sources[path].text;
  if (text) src = *text;
  else if (!readFile(path, src)) compileFail("cannot read " + path, false);
  Compiled out = c.run(path, src.c_str());
  delete active, active = nullptr;
  return out;
}
