// Compiler internals shared by the parser (compile.cpp) and the code generators (gen_a64.h, gen_wasm.h).
#pragma once
#include "fut.h"
#include <algorithm>
#include <array>
#include <bitset>
#include <cstdarg>
#include <cstdlib>
#include <functional>
#include <unordered_map>

// ---------------- lexer ----------------
enum { T_EOF = 256, T_NUM, T_STR, T_ID, T_EQ, T_NE, T_LE, T_GE, T_DOTDOT, T_AND, T_OR, T_ADDEQ, T_SUBEQ, T_MULEQ, T_DIVEQ, T_SHL, T_SHR, T_POW, T_OPT_DOT, T_ERR };
inline const char two[][3] = {"==", "!=", "<=", ">=", "..", "&&", "||", "+=", "-=", "*=", "/=", "<<", ">>", "**", "?."};
struct Tok { int t = 0, line = 0, sym = -1, len = 0; bool nl = false; double n = 0; const char* p = ""; };
inline bool idc(char c) { return ((c | 32) >= 'a' && (c | 32) <= 'z') || c == '_'; }
inline bool dig(char c) { return c >= '0' && c <= '9'; }

struct Lexer {
  const char* p = "";
  int line = 1;
  bool internal = false;  // code the compiler writes: names may contain '@' (the runtime's prop@gen), which users' can't
  const char* err = nullptr;  // a T_ERR token's message
  const char* end = nullptr;  // outer string's closing quote, while reading an interpolation
  Tok next() {
    Tok k;
    for (;; p++) {
      if (*p == '\n') k.nl = true, line++;
      else if (*p == '/' && p[1] == '/') while (p + 1 != end && p[1] && p[1] != '\n') p++;
      else if (*p != ' ' && *p != '\t' && *p != '\r') break;
    }
    k.line = line, k.p = p;
    if (p == end || !*p) return k.t = T_EOF, k;
    if (dig(*p)) {
      if (*p == '0' && (p[1] | 32) == 'x' && (dig(p[2]) || ((p[2] | 32) >= 'a' && (p[2] | 32) <= 'f')))  // 0xRRGGBB
        return k.n = (double)strtoull(p, (char**)&p, 16), k.t = T_NUM, k;
      while (dig(*p)) k.n = k.n * 10 + (*p++ - '0');
      if ((*p == '.' && dig(p[1])) || *p == 'e' || *p == 'E') k.n = strtod(k.p, (char**)&p);
      else if (p - k.p > 15) k.n = strtod(std::string(k.p, p).c_str(), nullptr);  // round the whole decimal token once
      return k.t = T_NUM, k;
    }
    if (idc(*p)) {
      while (idc(*p) || dig(*p) || (internal && *p == '@')) p++;
      return k.t = T_ID, k.sym = intern({k.p, size_t(p - k.p)}), k;
    }
    if (*p == '"') {
      for (p++; *p != '"'; p++) {
        if (!*p) return err = "unterminated string", k.t = T_ERR, k;
        if (*p == '\\' && p[1]) p++;
        if (*p == '\n') line++;
      }
      k.p++, k.len = int(p++ - k.p);
      return k.t = T_STR, k;
    }
    if ((*p == '=' || *p == '!') && p[1] == '=' && p[2] == '=') {
      err = *p == '=' ? "use '==' for equality" : "use '!=' for inequality";
      return p += 3, k.t = T_ERR, k;
    }
    if ((p[0] == '=' && p[1] == '>') || (p[0] == '+' && p[1] == '+')) {
      err = *p == '=' ? "write a lambda as fn(x) { return x }" : "use '+= 1' to increment";
      return p += 2, k.t = T_ERR, k;
    }
    for (int i = 0; i < int(sizeof two / sizeof *two); i++)
      if (p[0] == two[i][0] && p[1] == two[i][1]) return p += 2, k.t = T_EQ + i, k;
    return k.t = *p++, k;
  }
};

// A type's tokens (see type()), from L: atoms joined by |, each maybe followed by ?. Leaves L after it.
template <class Next> void skipType(Lexer& L, Next next) {
  for (;;) {
    Lexer s = L;
    Tok t = next(s);
    auto balanced = [&](int open, int close) {
      for (int d = 1; d > 0;) {
        Tok x = next(L);
        if (x.t == T_EOF) return;
        d += (x.t == open) - (x.t == close) - (close == '>' && x.t == T_SHR ? 2 : 0);
      }
    };
    if (t.t == '{') L = s, balanced('{', '}');
    else if (t.t == '(') L = s, balanced('(', ')');
    else if (t.t == T_STR || t.t == T_NUM) L = s;
    else if (t.t == '-') L = s, next(L);
    else if (t.t == T_ID) {
      L = s;
      if (Lexer d = L; next(d).t == '.' && next(d).t == T_ID) L = d;
      if (Lexer a = L; [&] { Tok o = next(a); return o.t == '<' && !o.nl; }()) L = a, balanced('<', '>');
    } else return;
    if (Lexer q = L; [&] { Tok o = next(q); return o.t == '?' && !o.nl; }()) L = q;
    if (Lexer u = L; [&] { Tok o = next(u); return o.t != '|' || o.nl; }()) return;
    else L = u;
  }
}

inline bool pubDecl(const Tok& t) {
  return t.t == T_ID && !t.nl && (t.sym == K_fn || t.sym == K_class || t.sym == K_var || t.sym == K_extern ||
                                 t.sym == S_effect || t.sym == S_enum || t.sym == K_import || t.sym == S_type);
}
inline bool clauseEnds(int depth, const Tok& prev, const Tok& t) {
  return depth == 0 && prev.t && (t.t == '{' || t.t == '}' || t.nl || t.t == T_EOF);
}
inline int nesting(const Tok& t) {
  return (t.t == '(' || t.t == '[' || t.t == '{') - (t.t == ')' || t.t == ']' || t.t == '}');
}

// Binary operators: token -> (precedence, Op). Precedence 0 means "not a binary operator".
inline const auto BIN = [] {
  std::array<std::pair<int, int>, T_SHR + 1> b{};
  // As in Python: comparisons bind looser than the bitwise operators, which bind looser than arithmetic.
  for (auto [t, p, o] : {std::tuple<int, int, int>{T_OR, 1, 0}, {T_AND, 2, 0}, {T_EQ, 3, O_EQ}, {T_NE, 3, O_NE}, {'<', 4, O_LT}, {T_LE, 4, O_LE},
                         {'>', 4, O_GT}, {T_GE, 4, O_GE}, {'|', 5, O_BOR}, {'^', 6, O_BXOR}, {'&', 7, O_BAND}, {T_SHL, 8, O_SHL},
                         {T_SHR, 8, O_SHR}, {'+', 9, O_ADD}, {'-', 9, O_SUB}, {'*', 10, O_MUL}, {'/', 10, O_DIV}, {'%', 10, O_MOD}})
    b[t] = {p, o};
  b['?'] = {1, 0};
  return b;
}();
inline int prec(int t) { return t <= T_SHR ? BIN[t].first : 0; }
inline int opOf(int t) { return BIN[t].second; }

// ---------------- compiler state ----------------
enum { LV_NONE = -1, LV_VAL, LV_LOCAL, LV_GLOBAL, LV_FIELD, LV_INDEX, LV_CAP };
struct LV { int k = LV_VAL, a = 0, b = 0; bool ro = false, num = false, own = false; };  // own: self.name  // a not-yet-loaded place, so `x = ...` can store
constexpr LV NUM{LV_VAL, 0, 0, false, true};  // a value known to be a number
struct Jumps {  // a few forward branches to patch to one place (no heap allocation)
  int v[6], n = 0;
  void push_back(int j) { v[n++] = j; }
  const int* begin() const { return v; }
  const int* end() const { return v + n; }
};
// A loop being compiled. brk/cont/exit: branches to patch (arm64); depth: its blocks' nesting (wasm).
struct Loop { std::vector<int> brk, cont; Jumps exit; size_t tries = 0; int top = 0, contAt = 0, depth = 0, atomics = 0, handles = 0; };
constexpr int HOT = 1000;  // calls before a function is compiled again (tier 2)
struct TryAt { int r = 0, pc = 0, done = 0; };  // a try being compiled: its record's first slot, branches to patch
struct Fun {  // per-function emission state
  Code k;
  struct Local { int sym, home; bool num = false; uint64_t ty = 0; int site = -1; };  // ty: its declared type (0: none), checked at every store  // home >= 0 is a frame slot, < 0 a register (arm64)
  std::vector<Local> locals;                          // num: always holds a number (a range loop variable)
  std::vector<std::pair<int, LV>> caps;     // lambda captures: (sym, where it lives in the parent)
  Fun* parent = nullptr;
  int depth = 0, maxd = 0, label = 0, nreg = 0, maxreg = 0, nfreg = 0, maxfreg = 0, scope = 0, cls = -1, selfHome = 0, cloHome = 0;
  int blocks = 0;      // wasm: structured blocks open at this point
  u32* stub = nullptr; // wasm: a top-level function's stub (direct calls reach its body; see Gen::finish)
  int vmLocal = -1;    // wasm: the i32 local holding the VM block's address (read from global 1 on entry), once used
  std::vector<int> wloc, wlocf;  // wasm: the i64 / f64 wasm local caching each frame slot used as a home (-1: none)
  std::vector<bool> wnum;         // wasm: homes of locals that start as numbers: cached in f64 locals
  size_t scopeAt = 0;  // first local of the innermost block
  std::vector<int> rets;  // `b` to the shared epilogue
  bool init = false, top = false, lambda = false, calls = false, test = false;  // test: a `test` block's body (no parameters)
  bool arrow = false;  // a lambda written `{ n -> body }` (expect grows): one parameter, inside its braces
  std::bitset<1024> facts;  // locals proven to hold numbers here, by home + 128 (see proveOperands)
  bool live = true;        // false after return/break/continue, until the next join point
  Loop* loop = nullptr;
  // Tier 2 (a recompile of a hot function): spec = parameters it speculates are numbers (bit i: the i-th
  // declared one); self = the function's own name if its recursive calls are assumed to return numbers,
  // which holds if every return is a proven number (retNum).
  bool tier2 = false, retNum = true;
  int npar = 0;
  u32 spec = 0;
  int lambdaName = -1;  // a named lambda resolves its own name to the incoming closure
  int self = -1;
  std::vector<u32> guard;  // registers holding speculated parameters, checked on entry
  // A function containing `try` keeps every local in its frame (a throw skips the epilogues of the frames it
  // unwinds, so callee-saved registers can't be trusted there) and saves all of x19..x26, d8..d15 for its caller.
  bool noRegs = false;
  std::vector<int> tries;  // slots of the active try records, outermost first
  int atomics = 0;         // atomic blocks open here: return, break and continue commit them
  int handles = 0;         // handle bodies open here: return, break and continue remove their handlers
  // Declared effects (`fn f() ! io`): where the caller's vm->allowed and vm->who are kept (arm64: two frame slots;
  // wasm: two locals) while the function runs; the epilogue every return goes through puts them back. -1: none.
  int effHome = -1;
  int fuelL = -1;  // wasm: an i64 local the step count passes through (gen_wasm.h fuel())
  // Its declared result type (`fn f(): num`; 0: none), checked in the epilogue every return goes through, so after
  // the function's own try blocks: a function never catches its own result's failed check.
  uint64_t retTy = 0;
  int retSite = -1;
  // Its spec's exit checks (ensures, the class invariant): compiled where every return arrives, after the result's
  // type check (x0: the result), before the caller's effects are put back.
  std::function<void()> exit;
};

// What the parser and the code generators share: the token stream, the function being emitted, errors.
struct CState {
  Lexer lx;
  Tok cur, prev;
  const char* prevEnd = "";  // just past prev (source text of an expression: its first token's p to here)
  Fun* F = nullptr;
  u32* deopt = nullptr;  // tier 2: the tier-1 body a failed entry guard runs instead
  bool lexAtEnd = false;
  std::string file;
  const char* srcStart = "";
  // A number-literal right operand is not loaded into a register; the binop fast path uses it directly.
  bool hasLit = false;
  double lit = 0;
  int pinLine = 0;  // nonzero: the line code is attributed to (a spec's clauses, compiled into the function they describe)
  int line() const { return pinLine ? pinLine : prev.line; }

  [[noreturn]] void failAt(const char* pos, int line, const char* fmt, ...) {  // file:line:col, the line, a caret
    const char* code = takeCode(fmt);
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
    compileFail(file + ":" + std::to_string(line) + ":" + std::to_string(pos - ls + 1) + ": error: " + b +
                    (code ? " [" + std::string(code) + "]" : "") + "\n  " +
                    std::string(ls, le) + "\n  " + caret + "^", !*pos || lexAtEnd);
  }
  template <class... A> [[noreturn]] void fail(const char* fmt, A... a) { failAt(cur.p, cur.line, fmt, a...); }
  int slot() { F->maxd = std::max(F->maxd, F->depth + 1); return F->depth++; }
  // ---- locals proven to be numbers: rt_binop raises unless the operands of -, *, /, % are numbers (and of
  // <, <=, >, >= when the other side is one), so once such an operation has run, a local operand stays a
  // number until it is assigned. Facts survive straight-line code and if/else joins; loop heads clear them.
  static constexpr int NOH = -(1 << 30);  // "not a local"
  static bool tracked(int h) { return h >= -128 && h < 1024 - 128; }  // registers and the first slots
  bool proven(Fun* f, int h) { return tracked(h) && f->facts[h + 128]; }
  void prove(int h) { if (tracked(h)) F->facts[h + 128] = true; }
  void unprove(int h) { if (tracked(h)) F->facts[h + 128] = false; }
};
