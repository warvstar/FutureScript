// Single-pass compiler: the parser emits arm64 machine code directly (no AST, no IR).
#include "fut.h"
#include <algorithm>
#include <array>
#include <cstdarg>
#include <deque>
#include <filesystem>
#include <unordered_set>
using namespace a64;

// ---------------- lexer ----------------
enum { T_EOF = 256, T_NUM, T_STR, T_ID, T_EQ, T_NE, T_LE, T_GE, T_DOTDOT, T_AND, T_OR, T_ADDEQ, T_SUBEQ, T_MULEQ, T_DIVEQ };
static const char two[][3] = {"==", "!=", "<=", ">=", "..", "&&", "||", "+=", "-=", "*=", "/="};
struct Tok { int t = 0, line = 0, sym = -1, len = 0; bool nl = false; double n = 0; const char* p = ""; };
struct LexErr { const char* pos; int line; const char* msg; };
static bool idc(char c) { return ((c | 32) >= 'a' && (c | 32) <= 'z') || c == '_'; }
static bool dig(char c) { return c >= '0' && c <= '9'; }

struct Lexer {
  const char* p = "";
  int line = 1;
  Tok next() {
    Tok k;
    for (;; p++) {
      if (*p == '\n') k.nl = true, line++;
      else if (*p == '/' && p[1] == '/') while (p[1] && p[1] != '\n') p++;
      else if (*p != ' ' && *p != '\t' && *p != '\r') break;
    }
    k.line = line, k.p = p;
    if (!*p) return k.t = T_EOF, k;
    if (dig(*p)) {
      while (dig(*p)) k.n = k.n * 10 + (*p++ - '0');
      if ((*p == '.' && dig(p[1])) || *p == 'e' || *p == 'E') k.n = strtod(k.p, (char**)&p);
      return k.t = T_NUM, k;
    }
    if (idc(*p)) {
      while (idc(*p) || dig(*p)) p++;
      return k.t = T_ID, k.sym = intern({k.p, size_t(p - k.p)}), k;
    }
    if (*p == '"') {
      for (p++; *p != '"'; p++) {
        if (!*p) throw LexErr{k.p, k.line, "unterminated string"};
        if (*p == '\\' && p[1]) p++;
        if (*p == '\n') line++;
      }
      k.p++, k.len = int(p++ - k.p);
      return k.t = T_STR, k;
    }
    for (int i = 0; i < 11; i++)
      if (p[0] == two[i][0] && p[1] == two[i][1]) return p += 2, k.t = T_EQ + i, k;
    return k.t = *p++, k;
  }
};

// Binary operators: token -> (precedence, Op). Precedence 0 means "not a binary operator".
static const auto BIN = [] {
  std::array<std::pair<int, int>, T_DIVEQ + 1> b{};
  for (auto [t, p, o] : {std::tuple<int, int, int>{T_OR, 1, 0}, {T_AND, 2, 0}, {T_EQ, 3, O_EQ}, {T_NE, 3, O_NE}, {'<', 4, O_LT}, {T_LE, 4, O_LE},
                         {'>', 4, O_GT}, {T_GE, 4, O_GE}, {'+', 5, O_ADD}, {'-', 5, O_SUB}, {'*', 6, O_MUL}, {'/', 6, O_DIV}, {'%', 6, O_MOD}})
    b[t] = {p, o};
  return b;
}();
static int prec(int t) { return BIN[t].first; }
static int opOf(int t) { return BIN[t].second; }
static uint64_t fnv(const char* a, const char* b) {
  uint64_t h = 1469598103934665603ull;
  for (; a < b; a++) h = (h ^ (uint8_t)*a) * 1099511628211ull;
  return h;
}

// ---------------- compiler state ----------------
enum { LV_NONE = -1, LV_VAL, LV_LOCAL, LV_GLOBAL, LV_FIELD, LV_INDEX, LV_CAP };
struct LV { int k = LV_VAL, a = 0, b = 0; bool ro = false; };  // a not-yet-loaded place, so `x = ...` can store
struct Jumps {  // a few forward branches to patch to one place (no heap allocation)
  int v[6], n = 0;
  void push_back(int j) { v[n++] = j; }
  const int* begin() const { return v; }
  const int* end() const { return v + n; }
};
struct Loop { std::vector<int> brk, cont; };
struct Fun {  // per-function emission state
  Code k;
  std::vector<std::pair<int, int>> locals;  // (sym, home): home >= 0 is an sp slot, < 0 is register x19..x25
  std::vector<std::pair<int, LV>> caps;     // lambda captures: (sym, where it lives in the parent)
  Fun* parent = nullptr;
  int depth = 0, maxd = 0, label = 0, nreg = 0, maxreg = 0, scope = 0, cls = -1, selfHome = 0, cloHome = 0;
  size_t scopeAt = 0;  // first local of the innermost block
  std::vector<int> rets;  // `b` to the shared epilogue
  bool init = false, top = false, lambda = false, calls = false;
  Loop* loop = nullptr;
};
struct Pend { u32* stub; Fn* fn; int arity; Code k; std::string owner; bool lambda; };  // body to install + stub to repoint
struct ClassDef { int sym, super; std::vector<std::pair<int, Fn*>> methods; };
struct Span { const char *start = nullptr, *end = nullptr; int line = 0, endLine = 0, arity = 0; };  // a fn/method in the source
struct Rec { uint64_t text, env; int line, arity; u32* body; std::vector<u32*> lambdas; };       // what is installed now
static std::unordered_map<std::string, Rec> recs;

struct Compiler {
  Lexer lx;
  Tok cur, prev;
  Fun* F = nullptr;
  bool reload = false, echo = false, lexAtEnd = false;
  std::string file, owner;
  const char* srcStart = "";
  std::deque<std::string> sources;
  std::unordered_map<std::string, const char*> srcOf;
  std::unordered_set<std::string> compiledFiles;
  std::vector<std::string> files, present;
  std::unordered_map<std::string, Span> spans;
  std::unordered_map<int, char> declKind;
  std::vector<std::pair<std::string, int>> shifts;
  uint64_t envHash = 1469598103934665603ull;
  std::vector<Pend> pend;
  std::vector<ClassDef> classes;
  std::vector<std::pair<int, int>> fnDefs;  // (sym, arity)
  std::vector<int> inits;                   // top-level vars initialised by this chunk

  // ---- errors: file:line:col, the source line and a caret ----
  [[noreturn]] void failAt(const char* pos, int line, const char* fmt, ...) {
    char b[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    const char *ls = pos, *le = pos;
    while (ls > srcStart && ls[-1] != '\n') ls--;
    while (*le && *le != '\n') le++;
    std::string caret;
    for (const char* c = ls; c < pos; c++) caret += *c == '\t' ? '\t' : ' ';
    throw CompileError(file + ":" + std::to_string(line) + ":" + std::to_string(pos - ls + 1) + ": error: " + b +
                             "\n  " + std::string(ls, le) + "\n  " + caret + "^", !*pos || lexAtEnd);
  }
  template <class... A> [[noreturn]] void fail(const char* fmt, A... a) { failAt(cur.p, cur.line, fmt, a...); }
  const char* name(int s) { return symName(s).c_str(); }
  Tok lex(Lexer& l) {
    try { return l.next(); } catch (LexErr& e) { lexAtEnd = !*l.p, failAt(e.pos, e.line, "%s", e.msg); }
  }
  void next() { prev = cur, cur = lex(lx); }
  Tok peek() { Lexer s = lx; try { return s.next(); } catch (LexErr&) { return {}; } }  // lex errors surface on next()
  bool peekIs(int t) { return peek().t == t; }
  bool at(int t) { return cur.t == t; }
  bool atKw(int k) { return cur.t == T_ID && cur.sym == k; }
  bool accept(int t) { return at(t) ? next(), true : false; }
  void expect(int t) { if (!accept(t)) fail("expected '%c'", t); }
  int ident() {
    if (!at(T_ID) || cur.sym < NKW) fail("expected a name");
    return next(), prev.sym;
  }

  // ---- emission ----
  int E(u32 i) {
    auto& c = F->k.c;
    if ((int)c.size() > F->label) {  // peephole, unless a branch may land between the two instructions
      u32 p = c.back();
      if ((p & 0xffe0ffe0) == mov(0, 0) && i == mov(p & 31, p >> 16 & 31)) return (int)c.size() - 1;  // mov a,b; mov b,a
      if ((p & 0xffc00000) == str(0, 0, 0) && i == (p | ldr(0, 0, 0))) return (int)c.size() - 1;  // str r,[m]; ldr r,[m]
    }
    auto& ln = F->k.lines;
    if (ln.empty() || ln.back().second != prev.line) ln.push_back({(int)F->k.c.size(), prev.line});
    F->k.c.push_back(i);
    return (int)F->k.c.size() - 1;
  }
  int here() { return F->label = (int)F->k.c.size(); }  // a branch target: peepholes must not cross it
  void patch(int at, int to = -1) {  // fix up a b / b.cond / cbz emitted at `at` to jump to `to` (default: here)
    u32& i = F->k.c[at];
    u32 d = u32((to < 0 ? here() : to) - at);
    i |= (i >> 26) == 5 ? d & 0x3ffffff : (d & 0x7ffff) << 5;
  }
  void helper(int h) { E(ldr(16, 28, HOFF(h))), E(blr(16)); }
  void imm(u32 r, V v) {
    bool first = true;
    for (u32 hw = 0; hw < 4; hw++)
      if (u32 part = (v >> (16 * hw)) & 0xffff) E(first ? movz(r, part, hw) : movk(r, part, hw)), first = false;
    if (first) E(movz(r, 0, 0));
  }
  void konst(std::string_view s) { consts.push_back(box(newStr(s))), imm(0, consts.back()); }
  void gmem(u32 r, int g, bool store = false) {  // ldr/str r, globals[g]; far slots go through x16
    u32 off = GOFF(g), base = 28;
    if (off >= 32760) E(addhi(16, 28, off >> 12)), base = 16, off &= 0xfff;
    E(store ? str(r, base, off) : ldr(r, base, off));
  }
  int slot() { F->maxd = std::max(F->maxd, F->depth + 1); return F->depth++; }
  void ld(u32 r, int s) { E(ldr(r, SP, s * 8)); }
  void st(u32 r, int s) { E(str(r, SP, s * 8)); }
  void addsp(u32 d, int s) {  // d = address of slot s
    u32 off = s * 8;
    if (off >> 12) E(addhi(d, SP, off >> 12)), E(addi(d, d, off & 0xfff));
    else E(addi(d, SP, off));
  }
  // Locals live in callee-saved x19..x25 while they last, then in slots.
  int home() {
    if (F->nreg < 7) return F->maxreg = std::max(F->maxreg, ++F->nreg), -F->nreg;
    return slot();
  }
  static u32 regOf(int h) { return u32(18 - h); }
  void ldl(u32 r, int h) { if (h >= 0) ld(r, h); else if (r != regOf(h)) E(mov(r, regOf(h))); }
  void stl(u32 r, int h) { if (h >= 0) st(r, h); else if (r != regOf(h)) E(mov(regOf(h), r)); }
  void falsy() { E(andnot1(9, 0)), E(cmp(9, 27)); }  // flags EQ <=> x0 is nil/false
  void poll() { E(ldrw(16, 28, 0)), E(cbzw(16) | 3 << 5), helper(H_POLL); }  // hot-reload safepoint
  void ret() { if (F->init) ldl(0, F->selfHome); F->rets.push_back(E(B)); }
  // The prologue is prepended once the frame size and the callee-saved registers used are known:
  //   stp x29,x30 / mov x29,sp / sub sp / stp x19..x26 pairs below x29 / stack check (only if it calls)
  Code finish() {
    int end = (int)F->k.c.size();
    if (F->rets.empty() || F->rets.back() != end - 1 || F->label >= end) F->init ? ldl(0, F->selfHome) : (void)E(mov(0, 27));
    else F->k.c.pop_back(), F->rets.pop_back();  // ends in an unbranched-to `return`: fall into the epilogue
    if (F->maxd > 4000) fail("function too large");
    int pairs = (F->maxreg + 1) / 2;
    u32 bytes = ((F->maxd * 8 + 15) & ~15) + 16 * pairs;
    for (int r : F->rets) patch(r);
    for (int k = 0; k < pairs; k++) E(ldp_off(19 + 2 * k, 20 + 2 * k, 29, -16 * (k + 1)));
    E(addi(SP, 29, 0)), E(ldp_post(29, 30)), E(RET);
    std::vector<u32> pro{stp_pre(29, 30), addi(29, SP, 0)};
    if (bytes & 0xfff) pro.push_back(subi(SP, SP, bytes & 0xfff));
    if (bytes >> 12) pro.push_back(subhi(SP, SP, bytes >> 12));
    for (int k = 0; k < pairs; k++) pro.push_back(stp_off(19 + 2 * k, 20 + 2 * k, 29, -16 * (k + 1)));
    if (F->calls) pro.insert(pro.end(), {cmpsp(26), bcond(HS) | 3 << 5, ldr(16, 28, HOFF(H_STACK)), blr(16)});
    Code& k = F->k;
    int n = (int)pro.size();
    k.c.insert(k.c.begin(), pro.begin(), pro.end());
    for (auto& r : k.rel) r.first += n;
    for (auto& l : k.lines) l.first += n;
    k.lines.insert(k.lines.begin(), {0, k.lines.empty() ? prev.line : k.lines[0].second});
    return std::move(k);
  }

  // x0 = left, x1 = right. Inline double fast path; everything else goes to rt_binop.
  static constexpr u32 CC[] = {EQ, NE, MI, LS, GT, GE};  // O_EQ..O_GE after fcmp (false when unordered)
  void numCheck(Jumps& slow, bool rnum) {  // branch to slow unless x0 (and x1) are numbers
    for (u32 r = 0; r < 2 - rnum; r++) E(mvn(9, r)), E(tstq(9)), slow.push_back(E(bcond(EQ)));
    E(fmovdx(0, 0)), E(fmovdx(1, 1));
  }
  void binop(int op, bool rnum = false) {  // rnum: x1 is known to be a number
    Jumps slow;
    int done = -1;
    if (op != O_MOD) {
      static const u32 fop[] = {FADD, FSUB, FMUL, FDIV};
      numCheck(slow, rnum);
      if (op <= O_DIV) E(fop[op]), E(fmovxd(0, 0));
      else E(fcmp(0, 1)), E(addi(10, 27, 1)), E(cinc(0, 10, CC[op - O_EQ]));
      done = E(B);
    }
    for (int j : slow) patch(j);
    imm(2, op), helper(H_BINOP);
    if (done >= 0) patch(done);
  }
  void combine(int s, int op) { E(mov(1, 0)), ld(0, s), F->depth--, binop(op); }  // left in slot s, right in x0
  void rhs(int op, int p = 0) {  // x0 = x0 op <operand binding tighter than p>
    if (int k = simpleOperand(p)) return binop(op, k == 2);
    int s = slot();
    st(0, s), expr(p + 1), combine(s, op);
  }

  // ---- inline caches: hit when x0 is an instance of ic->cls. Leaves x9 = object address. ----
  void icGuard(u32 icReg, Jumps& miss) {
    E(mvn(9, 0)), E(tstptr(9)), miss.push_back(E(bcond(NE)));
    E(andaddr(9, 0)), E(ldrb(10, 9)), E(cmpwi(10, (u32)K::Inst)), miss.push_back(E(bcond(NE)));
    E(ldr(10, 9, INST_CLS)), E(ldr(11, icReg, 0)), E(cmp(10, 11)), miss.push_back(E(bcond(NE)));
  }
  void field(int sym, bool set) {  // x0 = object (x2 = value to store); result in x0
    Jumps miss;
    u32 ic = set ? 3 : 2;
    imm(ic, (V)newIC()), icGuard(ic, miss);
    E(ldrw(11, ic, 8)), E(ldrw(12, 9, INST_N)), E(cmpw(11, 12)), miss.push_back(E(bcond(HS)));  // cached slot in range
    E(ldr(10, 9, INST_SLOTS));
    if (set) E(strreg(2, 10, 11)), E(mov(0, 2));
    else E(ldrreg(10, 10, 11)), E(addi(12, 27, 4)), E(cmp(10, 12)), miss.push_back(E(bcond(EQ))), E(mov(0, 10));  // ABSENT: slow
    int done = E(B);
    for (int m : miss) patch(m);
    imm(1, sym), helper(set ? H_SETF : H_GETF), patch(done);
  }

  // ---- names ----
  LV resolve(Fun* f, int sym) {  // local, else captured from an enclosing function (lambdas), else LV_NONE
    for (auto i = f->locals.rbegin(); i != f->locals.rend(); ++i) if (i->first == sym) return {LV_LOCAL, i->second};
    for (size_t i = 0; i < f->caps.size(); i++) if (f->caps[i].first == sym) return {LV_CAP, (int)i};
    if (!f->parent) return {LV_NONE};
    LV p = resolve(f->parent, sym);
    if (p.k == LV_NONE) return p;
    f->caps.push_back({sym, p});
    return {LV_CAP, (int)f->caps.size() - 1};
  }
  void cloEnv() { ldl(9, F->cloHome), E(andaddr(9, 9)); }  // x9 = this closure object
  void load(LV& v) {  // materialise v into x0
    switch (v.k) {
      case LV_LOCAL: ldl(0, v.a); break;
      case LV_GLOBAL: gmem(0, v.a); break;
      case LV_CAP: cloEnv(), E(ldr(0, 9, CLO_ENV + 8 * v.a)); break;
      case LV_FIELD: field(v.b, false); break;                                   // object in x0
      case LV_INDEX: E(mov(1, 0)), ld(0, v.a), F->depth--, helper(H_GETI); break;  // index in x0, object in slot a
    }
    v.k = LV_VAL;
  }

  // ---- calls: arguments go to consecutive slots (the "window"); x0..x7 carry the first 8, x8 points at all ----
  int exprList(int close) {
    int n = 0;
    if (!accept(close)) {
      do expr(), st(0, slot()), n++;
      while (accept(','));
      expect(close);
    }
    return n;
  }
  void args(int base, int n) {  // x0..x7 = the first 8; x8 = the window, read only for parameters past the 8th
    if (n > 8) addsp(8, base);
    for (int i = 0; i < n && i < 8; i++) ld(i, base + i);
  }
  void callWindow(int base, int n, int sym, bool super) {  // window[0] = callee or receiver
    Jumps miss;
    int have = -1;
    if (sym && !super) {  // method call: inline cache on the receiver's class
      ld(0, base), imm(4, (V)newIC()), icGuard(4, miss), E(ldr(9, 4, 16)), have = E(B);
    } else imm(4, 0);
    for (int m : miss) patch(m);
    addsp(0, base), imm(1, n), imm(2, sym);
    super ? gmem(3, global(F->cls).slot) : imm(3, 0);
    helper(H_PREP), E(mov(9, 0));
    if (have >= 0) patch(have);
    args(base, n + 1), E(blr(9)), F->depth = base, F->calls = true;
  }
  void call(int sym, bool super) { int base = slot(); st(0, base), callWindow(base, exprList(')'), sym, super); }
  int arityOf(int sym, Global& g) {
    if (auto it = spans.find(symName(sym)); it != spans.end()) return it->second.arity;
    return g.o && g.o->kind == K::Fn ? ((Fn*)g.o)->arity : -1;
  }
  void directCall(const Tok& t, Global& g) {  // known top-level fn: `bl stub`; reloads repoint the stub
    int base = F->depth, n = exprList(')'), a = arityOf(t.sym, g);
    if (a < 0) failAt(t.p, t.line, "'%s' is not defined", name(t.sym));
    if (a != n) failAt(t.p, t.line, "'%s' expects %d arguments, got %d", name(t.sym), a, n);
    args(base, n), F->k.rel.push_back({here(), g.stub}), E(BL), F->depth = base, F->calls = true;
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
    Fun f;
    f.parent = F, f.lambda = true;
    Fn* fn = make<Fn>();
    consts.push_back(box(fn));
    fn->code = newStub(), fn->name = intern("fn@" + std::to_string(t.line));
    fn->arity = function(f, -1, false, symName(fn->name), (u32*)fn->code, fn);
    int base = F->depth;
    for (auto& [s, src] : f.caps) { LV v = src; load(v), st(0, slot()); }
    imm(0, box(fn)), imm(1, f.caps.size()), addsp(2, base), helper(H_CLOSURE), F->depth = base;
  }

  // ---- expressions: result in x0 ----
  LV primary() {
    next();
    Tok t = prev;
    switch (t.t) {
      case T_NUM: return imm(0, box(t.n)), LV{};
      case T_STR: return strLit(t), LV{};
      case '(': return expr(), expect(')'), LV{};
      case '{': return mapLit(), LV{};
      case '[': { int base = F->depth, n = exprList(']'); return collect(base, n, H_LIST), LV{}; }
      case T_ID: break;
      default: failAt(t.p, t.line, t.t == T_EOF ? "unexpected end of file" : "unexpected '%c'", t.t);
    }
    switch (t.sym) {
      case K_true: return E(addi(0, 27, 2)), LV{};
      case K_false: return E(addi(0, 27, 1)), LV{};
      case K_nil: return E(mov(0, 27)), LV{};
      case K_fn: return lambda(t), LV{};
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
    auto it = globals.find(t.sym);
    if (it == globals.end()) failAt(t.p, t.line, "undefined name '%s'", name(t.sym));
    if (it->second.kind == 'f' && at('(') && !cur.nl) return next(), directCall(t, it->second), LV{};
    return {LV_GLOBAL, it->second.slot, 0, it->second.kind != 'v'};
  }
  LV postfix() {
    LV v = primary();
    for (;;) {
      if (accept('.')) {
        int s = ident();
        load(v);
        if (at('(') && !cur.nl) next(), call(s, false);
        else v = {LV_FIELD, 0, s};
      } else if (at('[') && !cur.nl) {
        next(), load(v);
        int t = slot();
        st(0, t), expr(), expect(']'), v = {LV_INDEX, t};
      } else if (at('(') && !cur.nl) next(), load(v), call(0, false);
      else return v;
    }
  }
  LV unary() {
    if (accept('-')) {
      if (accept(T_NUM)) return imm(0, box(-prev.n)), LV{};
      return operand(), E(mov(1, 0)), E(movz(0, 0, 0)), binop(O_SUB), LV{};
    }
    if (accept('!')) {
      return operand(), falsy(), E(addi(10, 27, 1)), E(cinc(0, 10, EQ)), LV{};
    }
    return postfix();
  }
  void operand() { LV v = unary(); load(v); }
  void expr(int minp = 1) { operand(), binRest(minp); }
  void binRest(int minp) {  // precedence climbing; the left operand is already in x0
    for (int t, p; (p = prec(t = cur.t)) && p >= minp && !(t == '-' && cur.nl);) {
      next();
      if (t == T_AND || t == T_OR) {
        falsy();
        int j = E(bcond(t == T_AND ? EQ : NE));
        expr(p + 1), patch(j);
      } else rhs(opOf(t), p);
    }
  }
  // A right operand that is a local or a number literal (and binds no tighter operator) goes straight
  // into x1, so the left operand never round-trips through memory.
  // Returns 0 (not simple), 1 (local) or 2 (number).
  int simpleOperand(int p) {
    if (!at(T_NUM) && !(at(T_ID) && cur.sym >= NKW)) return 0;
    Tok n = peek();
    if (n.t == '.' || ((n.t == '(' || n.t == '[') && !n.nl) || prec(n.t) > p) return 0;
    if (at(T_NUM)) return imm(1, box(cur.n)), next(), 2;
    LV v = resolve(F, cur.sym);
    if (v.k != LV_LOCAL) return 0;
    return ldl(1, v.a), next(), 1;
  }
  // An if/while condition. Returns the branches taken when it is false. `a < b {` compiles to a compare
  // and branch on the flags instead of building a boolean and testing it.
  Jumps cond() {
    operand(), binRest(5);
    Jumps no;
    if (int t = cur.t, p = prec(t); p == 3 || p == 4) {
      next();
      int op = opOf(t), k = simpleOperand(p);
      if (!k) { int s = slot(); st(0, s), expr(p + 1), E(mov(1, 0)), ld(0, s), F->depth--; }
      if (at('{')) {
        Jumps slow;
        numCheck(slow, k == 2);
        E(fcmp(0, 1)), no.push_back(E(bcond(CC[op - O_EQ] ^ 1)));
        int yes = E(B);
        for (int j : slow) patch(j);
        imm(2, op), helper(H_BINOP), falsy(), no.push_back(E(bcond(EQ))), patch(yes);
        return no;
      }
      binop(op, k == 2);
    }
    binRest(1), falsy(), no.push_back(E(bcond(EQ)));
    return no;
  }

  // ---- statements ----
  template <class Body> void scope(Body body) {  // locals, slots and registers taken inside are freed after
    size_t nl = F->locals.size(), outer = F->scopeAt;
    int d = F->depth, r = F->nreg;
    F->scope++, F->scopeAt = nl, body();
    F->scope--, F->scopeAt = outer, F->locals.resize(nl), F->depth = d, F->nreg = r;
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
          accept('=') ? expr() : (void)E(mov(0, 27));
          if (F->top && F->scope == 0) return gmem(0, global(s).slot, true), false;
          auto it = std::find_if(F->locals.begin() + F->scopeAt, F->locals.end(), [&](auto& l) { return l.first == s; });
          if (it != F->locals.end()) return stl(0, it->second), false;  // redeclared in the same block: reuse
          int h = home();
          return stl(0, h), F->locals.push_back({s, h}), false;
        }
        case K_if: return next(), ifStmt(), false;
        case K_while: {
          next();
          int top = here();
          return loop(top, cond(), [] {}), false;
        }
        case K_for: return next(), forStmt(), false;
        case K_return:
          next();
          if (F->top) fail("'return' outside a function");
          at('}') || at(T_EOF) || cur.nl ? (void)E(mov(0, 27)) : expr();
          return ret(), false;
        case K_break: case K_continue:
          next();
          if (!F->loop) failAt(prev.p, prev.line, "'%s' outside a loop", name(prev.sym));
          (prev.sym == K_break ? F->loop->brk : F->loop->cont).push_back(E(B));
          return false;
        case K_class: case K_import: fail("'%s' is only allowed at top level", name(cur.sym));
        case K_fn: if (!peekIs('(')) fail("named functions are only allowed at top level; use 'var f = fn(...) {...}'");
      }
    LV v = unary();
    int t = cur.t;
    if (t != '=' && (t < T_ADDEQ || t > T_DIVEQ)) return load(v), binRest(1), true;
    if (v.k == LV_VAL || v.ro) fail("cannot assign to this");
    next(), assign(v, t == '=' ? -1 : O_ADD + (t - T_ADDEQ));
    return false;
  }
  // body, then `step` (the continue target), a safepoint, and the back-edge to `top`
  template <class Step> void loop(int top, const Jumps& exit, Step step) {
    Loop lp, *outer = F->loop;
    F->loop = &lp, block(), F->loop = outer;
    int cont = here();
    step(), poll(), patch(E(B), top);
    for (int e : exit) patch(e);
    for (int b : lp.brk) patch(b);
    for (int c : lp.cont) patch(c, cont);
  }
  void ifStmt() {
    Jumps no = cond();
    block();
    int e = atKw(K_else) ? E(B) : -1;
    for (int j : no) patch(j);
    if (e < 0) return;
    next();
    atKw(K_if) ? (next(), ifStmt()) : block();
    patch(e);
  }
  void forStmt() {  // for x in a..b { }  |  for x in list/map/string { }
    int nm = ident();
    if (!atKw(K_in)) fail("expected 'in'");
    next();
    scope([&] {
      expr();
      bool range = accept(T_DOTDOT);
      if (range) helper(H_NUMCHK);
      int a = home();  // range: counter (a double); otherwise the collection
      stl(0, a);
      range ? (expr(), helper(H_NUMCHK)) : (void)E(movz(0, 0, 0));
      int b = home(), v = home();  // b: range end, otherwise a raw integer index
      stl(0, b), F->locals.push_back({nm, v});
      int top = here();
      Jumps exit;
      ldl(0, a), ldl(1, b);
      if (range) E(fmovdx(0, 0)), E(fmovdx(1, 1)), E(fcmp(0, 1)), exit.push_back(E(bcond(PL)));
      else helper(H_ITER), E(addi(9, 27, 3)), E(cmp(0, 9)), exit.push_back(E(bcond(EQ)));
      stl(0, v);
      loop(top, exit, [&] {
        if (range) ldl(0, a), E(fmovdx(0, 0)), E(FMOV1), E(FADD), E(fmovxd(0, 0)), stl(0, a);
        else ldl(0, b), E(addi(0, 0, 1)), stl(0, b);
      });
    });
  }
  void assign(LV v, int op) {  // op < 0: plain '=', else compound
    auto value = [&] { op < 0 ? expr() : rhs(op); };
    if (v.k == LV_LOCAL || v.k == LV_GLOBAL || v.k == LV_CAP) {
      if (op >= 0) { LV t = v; load(t); }
      value();
      if (v.k == LV_LOCAL) stl(0, v.a);
      else if (v.k == LV_GLOBAL) gmem(0, v.a, true);
      else cloEnv(), E(str(0, 9, CLO_ENV + 8 * v.a));
    } else if (v.k == LV_FIELD) {
      int o = slot();
      st(0, o);
      if (op >= 0) field(v.b, false);
      value(), E(mov(2, 0)), ld(0, o), field(v.b, true), F->depth--;
    } else {  // object in slot v.a, index in x0
      int i = slot();
      st(0, i);
      if (op >= 0) E(mov(1, 0)), ld(0, v.a), helper(H_GETI);
      value(), E(mov(2, 0)), ld(0, v.a), ld(1, i), helper(H_SETI), F->depth -= 2;
    }
  }

  // ---- declarations ----
  // Compile `(params) { body }` and queue it to be installed behind `stub`. Returns the arity.
  int function(Fun& f, int cls, bool init, const std::string& nm, u32* stub, Fn* fn) {
    f.cls = cls, f.init = init, f.k.name = nm, f.k.file = file;
    Fun* outer = F;
    F = &f;
    std::vector<int> homes;
    if (cls >= 0) homes.push_back(f.selfHome = home()), f.locals.push_back({K_self, f.selfHome});
    if (f.lambda) homes.push_back(f.cloHome = home());  // the closure itself, for captured variables
    expect('(');
    if (!accept(')')) {
      do { int s = ident(), h = home(); f.locals.push_back({s, h}), homes.push_back(h); } while (accept(','));
      expect(')');
    }
    int arity = (int)homes.size();
    if (arity > 64) fail("too many parameters");
    for (int i = 0; i < arity; i++) i < 8 ? stl(i, homes[i]) : (E(ldr(9, 8, 8 * i)), stl(9, homes[i]));
    block();
    pend.push_back({stub, fn, arity, finish(), owner, f.lambda});
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
    int s = ident();
    std::string q = symName(s);
    if (unchanged(q)) return fnDefs.push_back({s, recs[q].arity});
    Fun f;
    owner = q, fnDefs.push_back({s, function(f, -1, false, q, global(s).stub, nullptr)});
  }
  void classDecl() {
    ClassDef cd{ident(), -1, {}};
    if (accept(':')) {
      auto it = globals.find(cd.super = ident());
      if (it == globals.end() || it->second.kind != 'c' || cd.super == cd.sym) failAt(prev.p, prev.line, "bad superclass '%s'", name(cd.super));
    }
    expect('{');
    while (!accept('}')) {
      if (!atKw(K_fn)) fail("expected 'fn' in class body");
      next();
      int m = ident();
      for (auto& [x, f] : cd.methods) if (x == m) failAt(prev.p, prev.line, "duplicate method '%s'", name(m));
      Fn*& f = methodFns[{cd.sym, m}];
      if (!f) f = make<Fn>(), f->code = newStub(), f->name = m;
      cd.methods.push_back({m, f});
      std::string q = symName(cd.sym) + "." + symName(m);
      if (unchanged(q)) continue;
      Fun fun;
      owner = q, function(fun, cd.sym, m == S_init, q, (u32*)f->code, f);
    }
    classes.push_back(std::move(cd));
  }
  void declare(const Tok& t, char kind, int arity) {
    Global& g = global(t.sym);
    if (g.kind == 'n') failAt(t.p, t.line, "cannot redefine builtin '%s'", name(t.sym));
    if (auto [it, fresh] = declKind.try_emplace(t.sym, kind); !fresh && (kind != 'v' || it->second != 'v'))
      failAt(t.p, t.line, "'%s' is already defined", name(t.sym));
    g.kind = kind;
    if (kind == 'f' && !g.stub) g.stub = newStub();
    envHash = (envHash ^ (uint64_t(t.sym) << 16 | uint64_t(kind) << 8 | uint64_t(arity))) * 1099511628211ull;
  }
  std::string resolvePath(std::string_view rel) {
    std::filesystem::path p = std::filesystem::path(file).parent_path() / rel;
    std::error_code ec;
    auto c = std::filesystem::weakly_canonical(p, ec);
    return (ec ? p : c).string();
  }
  // Token pre-pass: learn every top-level name, arity and fn/method source span, following imports.
  void prescan(const std::string& path, const char* src) {
    std::string savedFile = file;
    const char* savedStart = srcStart;
    file = path, srcStart = src, srcOf[path] = src;
    struct Open { std::string q; const char* start; int line, depth; };
    std::vector<Open> open;
    Lexer L{src};
    int depth = 0, cls = -1;
    for (Tok t = lex(L), p; t.t != T_EOF; p = t, t = lex(L)) {
      bool afterKw = p.t == T_ID && t.t == T_ID;
      if (t.t == '{') depth++;
      else if (t.t == '}') {
        if (--depth == 0) cls = -1;
        if (!open.empty() && depth == open.back().depth) {
          Span& sp = spans[open.back().q];
          sp.start = open.back().start, sp.end = L.p, sp.line = open.back().line, sp.endLine = L.line;
          open.pop_back();
        }
      } else if (afterKw && p.sym == K_fn && (depth == 0 || (depth == 1 && cls >= 0))) {
        std::string q = depth ? symName(cls) + "." + symName(t.sym) : symName(t.sym);
        if (spans.count(q)) failAt(t.p, t.line, "'%s' is already defined", q.c_str());
        Lexer s = L;
        int n = 0;
        if (lex(s).t == '(') for (Tok a = lex(s); a.t != ')' && a.t != T_EOF; a = lex(s)) n += a.t == T_ID;
        spans[q].arity = n;
        open.push_back({q, p.p, p.line, depth});
        if (!depth) declare(t, 'f', n);
      } else if (depth == 0 && afterKw && (p.sym == K_class || p.sym == K_var)) {
        declare(t, p.sym == K_class ? 'c' : 'v', 0);
        if (p.sym == K_class) cls = t.sym;
      } else if (depth == 0 && t.t == T_STR && p.t == T_ID && p.sym == K_import) {
        std::string ip = resolvePath({t.p, (size_t)t.len});
        if (srcOf.count(ip)) continue;
        std::string& text = sources.emplace_back();
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
      if (atKw(K_import)) {
        next();
        if (!at(T_STR)) fail("expected a file name");
        next();
        std::string ip = resolvePath({prev.p, (size_t)prev.len});
        if (compiledFiles.insert(ip).second) detour(ip, srcOf.at(ip), srcOf.at(ip), 1, [&] { topLevel(); });
        continue;
      }
      // On reload, top-level statements are compiled (to validate them) and then dropped, except
      // initialisers of vars that don't exist yet. Existing state survives the reload.
      size_t c = F->k.c.size(), r = F->k.rel.size(), l = F->k.lines.size();
      bool keep = !reload;
      if (Tok n = peek(); atKw(K_var) && n.t == T_ID) keep |= !global(n.sym).inited, inits.push_back(n.sym);
      owner.clear();
      if (stmt() && echo) helper(H_ECHO);
      if (!keep) F->k.c.resize(c), F->k.rel.resize(r), F->k.lines.resize(l);
    }
  }

  Compiled run(const std::string& path, const char* src) {
    files.push_back(path), compiledFiles.insert(path);
    prescan(path, src);
    file = path, srcStart = src, lx = Lexer{src}, next();
    Fun top;
    top.top = true, top.k.name = "<top>", top.k.file = path, F = &top;
    topLevel();
    Code chunk = finish();
    return commit(chunk);
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
    std::string patched, added, removed;
    auto add = [](std::string& s, const std::string& n) { s += (s.empty() ? "" : ", ") + n; };
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
      if (p.fn) p.fn->arity = p.arity;
      if (p.lambda) { lambdas[p.owner].push_back(body); continue; }
      Span& sp = spans.at(p.owner);
      uint64_t text = fnv(sp.start, sp.end);
      auto old = recs.find(p.owner);
      if (old != recs.end()) {
        jitRetire(old->second.body), add(patched, p.owner);
        if (p.owner == "main" && reload && old->second.text != text) out.mainChanged = true;
      } else if (reload) add(added, p.owner);
      recs[p.owner] = {text, envHash, sp.line, p.arity, body, std::move(lambdas[p.owner])};
    }
    for (auto& [q, d] : shifts) {
      Rec& r = recs[q];
      r.line += d, jitShiftLines(r.body, d);
      for (u32* b : r.lambdas) jitShiftLines(b, d);
    }
    if (!echo) {  // in a file (not the REPL), definitions that disappeared from the source are dropped
      std::unordered_set<std::string> here(present.begin(), present.end());
      std::erase_if(recs, [&](auto& r) { return !here.count(r.first) && (add(removed, r.first), true); });
    }
    for (auto [s, a] : fnDefs) {
      Fn* f = globalObj<Fn>(s);
      f->code = global(s).stub, f->arity = a;
    }
    for (auto& cd : classes) globalObj<Class>(cd.sym);  // all exist before any superclass link is made
    for (auto& cd : classes) {
      Class* c = globalObj<Class>(cd.sym);
      c->super = cd.super >= 0 ? (Class*)global(cd.super).o : nullptr;
      c->methods.clear();
      for (auto [m, f] : cd.methods) c->methods[m] = f;
    }
    for (int s : inits) global(s).inited = true;
    clearICs();  // method tables or arities may have changed
    fix(chunk);
    out.chunk = jitInstall(chunk);
    jitCollect();
    out.log = patched.empty() && added.empty() && removed.empty() ? "no code changes" : "";
    for (auto [label, list] : {std::pair{"patched ", &patched}, {"added ", &added}, {"removed ", &removed}})
      if (!list->empty()) out.log += (out.log.empty() ? "" : "; ") + (label + *list);
    return out;
  }
};

Compiled compile(const std::string& path, bool reload, bool echo, const std::string* text) {
  JitBatch writable;  // nothing JIT-compiled runs during a compile
  Compiler c;
  c.reload = reload, c.echo = echo;
  std::string& src = c.sources.emplace_back();
  if (text) src = *text;
  else if (!readFile(path, src)) throw std::runtime_error("cannot read " + path);
  return c.run(path, src.c_str());
}
