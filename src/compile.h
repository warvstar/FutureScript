// Compiler internals shared by the parser (compile.cpp) and the code generators (gen_a64.h, gen_wasm.h).
#pragma once
#include "fut.h"
#include <algorithm>
#include <array>
#include <bitset>
#include <cstdarg>
#include <cstdlib>
#include <functional>
#include <map>
#include <unordered_map>

struct CompileDiagnostic {
  std::string file, message, code;
  int line = 1, column = 1;  // one-based byte positions
};
struct CompileSymbols {
  struct Site {
    int file = -1, offset = 0, length = 0;
    auto operator<=>(const Site&) const = default;
  };
  struct Declaration {
    Site site;
    std::string name, kind, container, signature;
  };
  enum Role { Read, Write, Decl };
  struct Occurrence { Site site; int declaration; Role role; };
  std::vector<std::string> files;
  std::vector<Declaration> declarations;
  std::vector<Occurrence> occurrences;
  std::vector<std::vector<size_t>> lineStarts;  // byte positions in the analyzed snapshot, for CLI references
  struct UnresolvedMember { Site site; std::string name; Role role; };
  std::vector<UnresolvedMember> unresolved;
  std::vector<std::pair<Site, Site>> collisions;  // overlapping declarations; a repeated site marks a name implicit in the source
};
struct CompileCheck {
  using Buffers = std::vector<std::pair<std::string, std::string_view>>;
  struct Import { std::string from, to; int line, column; };
  const Buffers& buffers;  // paths normalized with normalize; these sources take precedence over disk
  std::string (*normalize)(const std::string&) = canonicalPath;
  std::vector<Import> imports;  // filled even if the compile fails
  CompileSymbols symbols;
  bool referenceQuery = false;
};
// Requires rtInit(). Checks source and tests without loading externs, writing traces or installing code.
std::vector<CompileDiagnostic> compileDiagnostics(const std::string& path, CompileCheck& check);
// fut map of a program that doesn't compile: the files its imports reach, read from disk (api.cpp; native only)
Compiled programFromSource(const std::string& path);

// ---------------- lexer ----------------
enum { T_EOF = 256, T_NUM, T_STR, T_ID, T_EQ, T_NE, T_LE, T_GE, T_DOTDOT, T_AND, T_OR, T_ADDEQ, T_SUBEQ, T_MULEQ, T_DIVEQ, T_SHL, T_SHR, T_POW, T_OPT_DOT, T_ERR };
inline const char two[][3] = {"==", "!=", "<=", ">=", "..", "&&", "||", "+=", "-=", "*=", "/=", "<<", ">>", "**", "?."};
struct Tok { int t = 0, line = 0, sym = -1, len = 0; bool nl = false; double n = 0; const char* p = ""; };
inline bool idc(char c) { return ((c | 32) >= 'a' && (c | 32) <= 'z') || c == '_'; }
inline bool dig(char c) { return c >= '0' && c <= '9'; }

// From a string's opening quote to just past its closing one (false: it has none). An interpolation's code may hold
// strings of its own ("{join(xs, ", ")}"): a quote inside braces starts one, which ends on its line.
inline bool skipString(const char*& p, int& line, bool inner = false) {
  int depth = 0;
  for (p++;; p++) {
    if (!*p || (inner && *p == '\n')) return false;
    if (*p == '\\' && p[1]) { if (*++p == '\n') line++; continue; }
    if (*p == '\n') line++;
    else if (*p == '{') depth++;
    else if (*p == '}' && depth) depth--;
    else if (*p == '"') {
      if (!depth) return p++, true;
      if (!skipString(p, line, true)) return false;
      p--;
    }
  }
}

struct Lexer {
  const char* p = "";
  int line = 1;
  bool internal = false;  // code the compiler writes: names may contain '@' (the runtime's prop@gen), which users' can't
  const char* err = nullptr;  // a T_ERR token's message
  const char* end = nullptr;  // outer string's closing quote, while reading an interpolation
  Tok next(bool symbols = true) {
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
      if (symbols) k.sym = intern({k.p, size_t(p - k.p)});
      return k.t = T_ID, k;
    }
    if (*p == '"') {
      int at = line;
      if (!skipString(p, line)) {  // braces that don't pair up: the string ends at its next quote, and its holes report
        p = k.p, line = at;
        for (p++; *p && *p != '"'; p++) {
          if (*p == '\\' && p[1]) p++;
          if (*p == '\n') line++;
        }
        if (!*p) return err = "unterminated string", k.t = T_ERR, k;
        p++;
      }
      k.p++, k.len = int(p - 1 - k.p);
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
    if ((unsigned char)*p >= 0x80) {  // (a token is ASCII: past it, the compiler would index its tables with it)
      err = "this character can be in a string or a comment, not in code";
      for (p++; ((unsigned char)*p & 0xC0) == 0x80; p++) {}
      return k.t = T_ERR, k;
    }
    return k.t = *p++, k;
  }
};

// View lowering keeps copied fragments tied to their original byte ranges. Synthetic tokens have no range.
struct ViewSpan { size_t begin, end, original; };
struct LoweredView {
  std::string text;
  std::vector<ViewSpan> spans;
  std::vector<size_t> writes;  // original identifier offsets of state assignments lowered to calls
  size_t origin(size_t offset, size_t length = 1) const;
};
bool lowerViews(const std::string& source, const std::string& path, LoweredView& out);

// gpu fn (wgsl.cpp): a top-level function also translated to WGSL. Each file's gpu fns, from their `fn`; a gpu fn
// calls others of its file by name. alias(s): the package a module alias names ("math"), if s is one.
struct GpuFnSrc { int sym; const char* p; int line; };
struct GpuInfo { std::string wgsl, entry, result; std::vector<std::string> names, types; };
// fail(p, line, message) reports what the GPU can't do, and does not return (a compile error)
using GpuFail = std::function<void(const char*, int, const std::string&)>;
GpuInfo wgslOf(const std::vector<GpuFnSrc>& fns, size_t i, const std::function<const std::string*(int)>& alias, const GpuFail& fail);
void gpuInfoSet(int sym, GpuInfo);

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
enum { LV_NONE = -1, LV_VAL, LV_LOCAL, LV_GLOBAL, LV_FIELD, LV_INDEX, LV_CAP, LV_SHARED, LV_BAD };  // LV_SHARED: a shared var (a: its cell)
struct LV { int k = LV_VAL, a = 0, b = 0; bool ro = false, num = false, own = false; int path = 0; };  // own: self.name  // a not-yet-loaded place, so `x = ...` can store
constexpr LV NUM{LV_VAL, 0, 0, false, true};  // a value known to be a number
struct Jumps {  // a few forward branches to patch to one place (no heap allocation)
  int v[12], n = 0;
  void push_back(int j) { if (n == 12) abort(); v[n++] = j; }
  const int* begin() const { return v; }
  const int* end() const { return v + n; }
};
// A loop being compiled. brk/cont/exit: branches to patch (arm64); depth: its blocks' nesting (wasm).
struct Loop { std::vector<int> brk, cont; Jumps exit; size_t tries = 0; int top = 0, contAt = 0, depth = 0, atomics = 0, handles = 0; };
constexpr int HOT = 1000;  // calls before a function is compiled again (tier 2)
struct TryAt { int r = 0, pc = 0, done = 0; };  // a try being compiled: its record's first slot, branches to patch
struct Fun {  // per-function emission state
  Code k;
  struct Local { int sym, home; bool num = false; uint64_t ty = 0; int site = -1, symbol = -1, path = 0; bool made = false; };  // home: frame slot (>= 0) or arm64 register; symbol: editor declaration; made: holds a lambda made here (inferred effects)
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
  Loop* retry = nullptr;  // the outermost atomic { } being compiled: its exits commit, or run it again (atomicStmt)
  // Tier 2 (a recompile of a hot function): spec = parameters it speculates are numbers (bit i: the i-th
  // declared one); self = the function's own name if its recursive calls are assumed to return numbers,
  // which holds if every return is a proven number (retNum).
  bool tier2 = false, retNum = true;
  int npar = 0;
  std::vector<int> paramHomes;  // a named function's declared parameters' homes, in order (inferred effects)
  u32 spec = 0;
  int lambdaName = -1;  // a named lambda resolves its own name to the incoming closure
  int lambdaSymbol = -1;
  // A parallel for's body (`parallel for i in a..b { }`, compiled as a lambda of i): its variable, and the variables
  // from outside it that it writes as name[...] = (each a closure slot + 1, or -(global slot + 1); and its name).
  int parVar = -1;
  std::vector<std::pair<int, std::string>> parWrites;
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
  // What it declares, for the compile-time check of what its body calls and performs (effDecl: it has a `!`, even a bare one).
  bool effDecl = false;
  uint64_t effMask = 0;
  std::string effWho, effText;
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
  bool collectErrors = false;
  CompileCheck* editorCheck = nullptr;
  std::string file;
  const char* srcStart = "";
  struct ViewInput { const std::string& original; const LoweredView& lowered; };
  std::unordered_map<const char*, ViewInput> viewInputs;
  struct SourcePosition { const char *start, *at; int line; };
  SourcePosition sourcePosition(const char* at, int line) const;
  // A number-literal right operand is not loaded into a register; the binop fast path uses it directly.
  bool hasLit = false;
  double lit = 0;
  int pinLine = 0;  // nonzero: the line code is attributed to (a spec's clauses, compiled into the function they describe)
  int line() const { return pinLine ? pinLine : prev.line; }

  struct ParseError {};
#if __wasm__
  jmp_buf* recovery = nullptr;
#else
  bool recovery = false;
#endif
  struct Report { CompileDiagnostic diag; std::string rendered; };
  std::vector<Report> reports;
  std::string failureText;
  const char* errorPos = nullptr;
  void report(const char* pos, int line, const char* fmt, va_list ap);
  void diagnose(const char* pos, int line, const char* fmt, ...);
  [[noreturn]] void failAt(const char* pos, int line, const char* fmt, ...);
  template <class... A> [[noreturn]] void fail(const char* fmt, A... a) { failAt(cur.p, cur.line, fmt, a...); }
  void sortReports();
  std::string diagnosticText();
  [[noreturn]] void reportFailure();
  int firstIC = -1;
  int cache(bool store = false) {
    if (editorCheck || !reports.empty()) return 0;
    int ic = newIC(store);
    if (firstIC < 0) firstIC = ic;
    return ic;
  }
  std::map<int, Global> declarations;
  std::vector<int> newGlobals;
  bool stageGlobals = false;
  Global* findGlobal(int sym) {
    auto it = declarations.find(sym);
    return it == declarations.end() ? ::findGlobal(sym) : &it->second;
  }
  Global& global(int sym);
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
