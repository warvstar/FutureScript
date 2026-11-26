// Single-pass compiler: the parser emits machine code directly (no AST, no IR), through a code generator:
// gen_a64.h (arm64) natively, gen_wasm.h in the WebAssembly build.
#include "compile.h"
#include <functional>
#include <map>
#include <set>
#include <unordered_set>
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
struct Pend { u32* stub; Fn* fn; int arity; Code k; std::string owner; bool lambda; Tier tier; uint64_t env = 0; };  // body to install + stub to repoint
struct At { const char* p; int line; std::string file; const char* src; };  // a place in the source, for errors found later
struct Member { int sym; bool over; At at; };                                // a method as written (for the override check)
struct ClassDef {
  int sym, super;
  std::vector<std::pair<int, Fn*>> methods;
  std::vector<Member> written;
  std::vector<std::pair<int, V>> fields;  // declared: `var name [: type] [= constant]`
  std::vector<Class::FieldType> ftypes;   // those with a type
};
struct FieldUse { int cls, sym; At at; };  // `self.name` in a method: checked against declared fields
// A fn/method in the source. retNum: it declares `: num`, so a direct call's result is known to be a number.
struct Span { const char *start = nullptr, *end = nullptr; int line = 0, endLine = 0, arity = 0; bool retNum = false; };
// Each fn/method's parameter and result types (0: none), as last compiled: an override must declare the same.
// eff: it declares effects (effMask, as written: effText), so an override must declare a subset.
struct Sig { std::vector<uint64_t> types; std::string text; bool eff = false; uint64_t effMask = 0; std::string effText; };
// Each effect's operations (qualified effect symbol -> (operation symbol, arity)), as last compiled: what performs
// and handlers compile against when the effect is not in the files being compiled (the REPL).
using EffOps = std::unordered_map<int, std::vector<std::pair<int, int>>>;
static EffOps effectOps;
static std::unordered_map<std::string, Sig> sigs;
struct Rec { uint64_t text, env; int line, arity; u32* body; std::vector<u32*> lambdas; bool repl; };  // what is installed now
static std::unordered_map<std::string, Rec> recs;
// Has the program declared an invariant? Then calls and field stores on objects other than self check them. Once
// true it stays true (part of every package's environment hash), so code compiled without the checks is compiled again with them.
static bool invariantsOn;
static std::unordered_map<std::string, u32*> invariantBodies;  // each class's hidden invariant method, installed
static std::unordered_map<int, uint64_t> varText;  // top-level var -> hash of its declaration's source
static std::map<std::pair<int, int>, Fn*> methodFns;  // (class sym, method sym): one Fn per method, kept across reloads
// Files: every imported file has a prefix (its stem, made unique). Its private top-level names are "prefix.name",
// so two files' private names never clash and other files can't reach them. A file imported `as` a name (a module)
// declares all its names that way, and other files reach its `pub` ones as alias.name. In a file imported plainly,
// `pub` names are global. The program's own file has no prefix: its names are global.
static std::unordered_map<std::string, std::string> nsOf;                             // imported file -> prefix
static std::unordered_map<std::string, bool> isModule;                                // imported file -> imported `as`
static std::unordered_map<std::string, std::unordered_map<int, std::string>> aliases;  // file -> its `as` names
static std::string base(const std::string& path) { return path.substr(path.rfind('/') + 1); }
// Packages (pkg.cpp). The program's own package (entryDir) has no prefix, like a program without one. Every other
// package has a prefix (its name, made unique), and the `pub` names of its plainly imported files are
// "prefix.name": visible across the package, and to other packages through `import "name"` (the alias) when its
// root file declares them or re-exports their file with `pub import`. Its files' prefixes are "prefix/stem".
static std::string entryDir;
std::vector<std::string> modulesPending;
static std::unordered_map<std::string, std::string> pkgPreOf;  // package dir -> prefix
static std::unordered_map<std::string, std::unordered_set<int>> surfaceOf;  // prefix -> its interface's names
static bool prefixTaken(const std::string& p, const std::string& self) {  // a file's or a package's prefix
  for (auto& [f, q] : nsOf) if (f != self && q == p) return true;
  for (auto& [d, q] : pkgPreOf) if (d != self && q == p) return true;
  return false;
}
// The sources of the code running now, and of the code the last compile replaced (what a failed reload goes back
// to); while revertReload compiles, files are read from `overlay` rather than from disk.
using Sources = std::vector<std::pair<std::string, std::string>>;
static Sources installed, replaced;
static const Sources* overlay;
static bool readSource(const std::string& path, std::string& out) {
  if (!overlay) return readFile(path, out);
  for (auto& [f, text] : *overlay) if (f == path) return out = text, true;
  return false;
}

struct Compiler : Gen {
  bool reload = false, echo = false, tiering = false, topTry = false, testing = false;  // testing: compile `test` blocks
  std::vector<TestCase> tests;
  std::string owner;
  struct Source { std::string text; bool compiled = false; };
  std::unordered_map<std::string, Source> sources;  // by path; nodes are stable, so text pointers stay valid
  std::vector<std::string> files, present;
  std::unordered_map<std::string, Span> spans;
  std::vector<char> declKind;  // by symbol
  std::vector<std::pair<std::string, int>> shifts;
  // What a body compiles against, per package: the declarations of its own package, and the interfaces of the
  // packages it imports. A body whose text and environment are as last compiled is reused, so a new declaration
  // in one package recompiles that package and those that import it (if it is in the interface), not every file.
  std::unordered_map<std::string, uint64_t> envOf, envFinal;                     // package prefix -> hash
  std::unordered_map<std::string, std::vector<std::pair<int, uint64_t>>> pubDecls;  // (name as written, or -2: always)
  std::unordered_map<std::string, std::set<std::string>> pkgImports;             // package prefix -> those it imports
  int pubRaw = -1;  // the declaration the prescan is at is pub (this name, as written) in a plainly imported file
  void mixEnv(uint64_t v, int raw) {
    const std::string& P = pkgPrefix(file);
    auto [it, fresh] = envOf.try_emplace(P, 1469598103934665603ull);
    it->second = (it->second ^ v) * 1099511628211ull;
    if (raw != -1) pubDecls[P].push_back({raw, v});
  }
  uint64_t envFor(const std::string& f) {
    const std::string& P = pkgPrefix(f);
    auto it = envFinal.find(P);
    return it != envFinal.end() ? it->second : 1469598103934665603ull;
  }
  std::vector<Pend> pend;
  std::vector<ClassDef> classes;
  std::vector<FieldUse> fieldUses;
  std::vector<std::pair<int, int>> fnDefs;  // (sym, arity)
  std::vector<int> inits;                   // top-level vars initialised by this chunk
  struct Extern { int sym; void* fn; std::string sig; char ret; };
  std::vector<Extern> externs;
  std::string patched, added, removed, reset;  // for the reload log
  std::unordered_map<std::string, std::vector<int>> pubs;  // file -> its top-level names marked `pub`
  std::vector<char> pubSym;                                 // by (prefixed) symbol: declared `pub`
  std::unordered_map<int, std::string> privateIn;           // a private name (as written) -> a file declaring it
  EffOps effOps;                                            // effects this compile declares (the prescan finds them)
  struct EffectDef { int sym; std::vector<EffOpDef> ops; };
  std::vector<EffectDef> effDefs;                           // installed by commit
  std::unordered_map<std::string, std::vector<std::string>> reexports;  // file -> files it imports with `pub import`
  std::unordered_map<std::string, std::string> pkgRootOf;               // package prefix -> its root file
  std::unordered_map<std::string, std::string> pkgNamed;                // package name -> its dir (one per name)
  std::unordered_map<std::string, const Package*> pkgCache;             // file -> its package
  std::vector<std::string> pkgDirs;                                     // the packages this compile uses
  std::vector<std::string> modFiles;                                    // the js and wasm modules it binds (watched)
  const Package* pkgOf(const std::string& f) {
    auto [it, fresh] = pkgCache.try_emplace(f, nullptr);
    if (fresh) {
      it->second = packageOf(f.empty() || f[0] == '<' ? mainFile : f);
      if (it->second && std::find(pkgDirs.begin(), pkgDirs.end(), it->second->dir) == pkgDirs.end()) pkgDirs.push_back(it->second->dir);
    }
    return it->second;
  }
  const std::string& pkgPrefix(const std::string& f) {  // "" in the program's own package, or outside any
    static const std::string none;
    const Package* p = pkgOf(f);
    if (!p || p->dir == entryDir) return none;
    auto it = pkgPreOf.find(p->dir);
    return it == pkgPreOf.end() ? none : it->second;
  }

  static void add(std::string& s, const std::string& n) { s += (s.empty() ? "" : ", ") + n; }
  const char* name(int s) { return symName(s).c_str(); }
  static std::string display(const std::string& nm) { return nm.starts_with("fn@") ? "the function at line " + nm.substr(3) : nm + "()"; }
  bool isPub(int s) {  // s (as written) is declared `pub` in the current file
    auto it = pubs.find(file);
    return it != pubs.end() && std::find(it->second.begin(), it->second.end(), s) != it->second.end();
  }
  int qualify(int s) {  // the symbol of a name declared at the top level of the current file
    auto it = nsOf.find(file);
    if (it == nsOf.end()) return s;
    if (!isModule[file] && isPub(s)) {
      const std::string& P = pkgPrefix(file);
      return P.empty() ? s : intern(P + "." + symName(s));
    }
    return intern(it->second + "." + symName(s));
  }
  Tok topName(Tok t) {  // the top-level name t (just read) refers to; reads `.name` after a module alias
    const std::string* m = moduleAlias(t.sym);
    if (!m) return t.sym = globalSym(t.sym), t;
    auto root = pkgRootOf.find(*m);
    auto surface = surfaceOf.find(*m);
    bool pkg = root != pkgRootOf.end() || surface != surfaceOf.end();  // tier 2 has only the surface
    if (!accept('.')) failAt(t.p, t.line, "'%s' is a %s: use %s.name", name(t.sym), pkg ? "package" : "module", name(t.sym));
    Tok n = cur;
    n.sym = intern(*m + "." + symName(ident()));
    int raw = prev.sym;
    bool declared = declKind.empty() ? findGlobal(n.sym) != nullptr : (size_t)n.sym < declKind.size() && declKind[n.sym];  // tier 2, the REPL: as installed
    if (pkg) {
      if (surface != surfaceOf.end() && surface->second.count(raw) && declared) return n;
      std::string rf = root != pkgRootOf.end() ? ::base(root->second) : "its root file";
      if (declared)
        failAt(n.p, n.line, "'%s' is pub inside package %s but not part of its interface: only what %s makes pub (or re-exports with pub import) is", name(raw), name(t.sym), rf.c_str());
      failAt(n.p, n.line, "package %s has no '%s' in its interface (what %s makes pub, or re-exports with pub import)", name(t.sym), name(raw), rf.c_str());
    }
    if (!declared) failAt(n.p, n.line, "module '%s' has no '%s'", name(t.sym), name(prev.sym));
    if ((size_t)n.sym >= pubSym.size() || !pubSym[n.sym]) failAt(n.p, n.line, "'%s' is private to module '%s' (mark it pub there to use it here)", name(prev.sym), name(t.sym));
    return n;
  }
  [[noreturn]] void undefined(const Tok& t) {
    const std::string& full = symName(t.sym);
    int s = intern(full.substr(full.rfind('.') + 1));  // as written: globalSym may have put a package's prefix on it
    if (auto it = privateIn.find(s); it != privateIn.end() && it->second != file && pkgOf(it->second) == pkgOf(file))
      failAt(t.p, t.line, "'%s' is private to %s (mark it pub there to use it here)", name(s), ::base(it->second).c_str());
    if (const std::string& P = pkgPrefix(file); !P.empty())
      if (Global* g = findGlobal(s); g && g->kind != 'n')
        failAt(t.p, t.line, "undefined name '%s' (package %s sees its own names, its dependencies' through their names, and builtins; not the program's)", name(s), pkgOf(file)->name.c_str());
    failAt(t.p, t.line, "undefined name '%s'", name(s));
  }
  int globalSym(int s) {  // the file's own names first, then its package's, then global (builtins only, in a package)
    if (auto it = nsOf.find(file); it != nsOf.end())
      if (int q = intern(it->second + "." + symName(s)); findGlobal(q)) return q;
    const std::string& P = pkgPrefix(file);
    if (P.empty()) return s;
    int q = intern(P + "." + symName(s));
    if (findGlobal(q)) return q;
    Global* g = findGlobal(s);
    return g && g->kind == 'n' ? s : q;
  }
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
  void next() { const char* e = lx.p; prev = cur, cur = lex(lx), prevEnd = e; }
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

  // ---- types: num str bool list map fn any, or a class (geom.Shape for a module's); `?` after one allows nil ----
  struct Ty { uint64_t code = 0; std::string text; };  // code 0: none written
  Ty type() {
    Tok t = cur;
    Ty ty;
    if (atKw(K_fn)) next(), ty.code = TY_FN;
    else {
      if (!at(T_ID) || cur.sym < NKW) fail("expected a type (num, str, bool, list, map, fn, any, or a class)");
      next();
      static const char* const kinds[] = {"any", "num", "str", "bool", "list", "map"};
      for (int i = 0; i < 6; i++) if (symName(prev.sym) == kinds[i]) ty.code = TY_ANY + i;
      if (!ty.code) {
        Tok n = topName(prev);
        Global* g = findGlobal(n.sym);
        if (!g || g->kind != 'c') failAt(t.p, t.line, "unknown type '%s' (num, str, bool, list, map, fn, any, or a class)", std::string(t.p, prevEnd).c_str());
        ty.code = tyClass(n.sym);
      }
    }
    ty.text = (ty.code & 15) == TY_CLS ? symName(int(ty.code >> 8)) : std::string(t.p, prevEnd);  // a module's class: bank.Account
    if (at('?') && !cur.nl) {
      if (ty.code == TY_ANY) fail("'any' already includes nil");
      next(), ty.code |= TY_OPT, ty.text += "?";
    }
    return ty;
  }
  void checkType(const Ty& ty, const std::string& what, bool known = false) {  // x0 must have type ty (known: it is a number)
    if (ty.code && !(known && (ty.code & 15) == TY_NUM)) typeCheck(ty.code, typeSite(what, ty.text), ty.code == TY_NUM);
  }
  // A var's declared type, checked at every store to it (from closures too). A top-level var's comes from this
  // compile's declaration, parsed on first use (a function above the declaration may store to it), else from the
  // last compile that declared it (the REPL, another module).
  struct VarTy { std::string file; const char *src = nullptr, *at = nullptr; int line = 0; bool done = false; uint64_t code = 0; int site = -1; };
  std::unordered_map<int, VarTy> varTypes;  // the top-level vars this compile declares; at: their type's text
  std::pair<uint64_t, int> globalType(int sym) {
    auto it = varTypes.find(sym);
    if (it == varTypes.end()) return {global(sym).type, global(sym).typeSite};
    VarTy& v = it->second;
    if (v.at && !v.done) {
      v.done = true;
      detour(v.file, v.src, v.at, v.line, [&] {
        Ty t = type();
        if (t.code != TY_ANY) v.code = t.code, v.site = typeSite("var '" + symName(sym) + "'", t.text);
      });
    }
    return {v.code, v.site};
  }
  std::pair<uint64_t, int> varType(Fun* f, const LV& v) {  // its declared type (0: none) and its check's site
    if (v.k == LV_GLOBAL) return globalType(v.b);
    if (v.k == LV_CAP) return varType(f->parent, f->caps[v.a].second);
    if (v.k == LV_LOCAL)
      for (auto i = f->locals.rbegin(); i != f->locals.rend(); ++i) if (i->home == v.a) return {i->ty, i->site};
    return {0, -1};
  }
  void storeCheck(std::pair<uint64_t, int> t, bool num) {  // x0, about to be stored to a var of type t
    if (t.first && !(num && (t.first & 15) == TY_NUM)) typeCheck(t.first, t.second, t.first == TY_NUM);
  }

  // ---- effects: io clock random yield, or an `effect` (geom.Log for a module's) ----
  uint64_t effectName(std::string& text) {
    Tok t = cur;
    if (atKw(K_yield)) return next(), text += "yield", EFF_YIELD;
    int s = ident();
    const std::string& n = symName(s);
    if (n == "io" || n == "clock" || n == "random") return text += n, n == "io" ? EFF_IO : n == "clock" ? EFF_CLOCK : EFF_RANDOM;
    Tok q = topName(prev);
    Global* g = findGlobal(q.sym);
    if (!g || g->kind != 'e') failAt(t.p, t.line, "unknown effect '%s' (io, clock, random, yield, or one declared with 'effect')", std::string(t.p, prevEnd).c_str());
    uint64_t b = effBit(q.sym);
    if (!b) failAt(t.p, t.line, "too many effects (a program can declare at most %d)", EFF_LEVEL - 4);
    return text += std::string(t.p, prevEnd), b;
  }
  const std::vector<std::pair<int, int>>* opsOf(int effSym) {
    if (auto it = effOps.find(effSym); it != effOps.end()) return &it->second;
    if (auto it = effectOps.find(effSym); it != effectOps.end()) return &it->second;
    return nullptr;
  }
  // E.op after the effect's name (just read, as t): the operation's id and arity
  std::pair<int, int> effectOp(const Tok& t) {
    if (!accept('.')) failAt(t.p, t.line, "'%s' is an effect: perform its operations as %s.name(...)", name(t.sym), name(t.sym));
    Tok ot = cur;
    int o = ident();
    if (auto* l = opsOf(t.sym))
      for (auto [s, a] : *l) if (s == o) return {effOpId(symName(t.sym) + "." + symName(o)), a};
    failAt(ot.p, ot.line, "effect '%s' has no operation '%s'", name(t.sym), name(o));
  }
  void perform(const Tok& t) {  // E.op(args): a call that rt_prep sends to the nearest handler (see effPrep)
    Tok ot = cur;
    auto [op, arity] = effectOp(t);
    if (!at('(') || cur.nl) failAt(ot.p, ot.line, "expected '(': perform an operation by calling it");
    next();
    int base = slot();
    imm(0, box(effOpFn(op))), st(0, base);
    int n = exprList(')');
    if (n != arity) failAt(ot.p, ot.line, "'%s' expects %d arguments, got %d", effOpName(op).c_str(), arity, n);
    callWindow(base, n, 0, false);
  }
  // handle { body } with op(params) { ... } ...: each handler is compiled first, as a closure, so it is ready when
  // the body starts; then handle@begin([op, handler, ...]), the body, and handle@end where it ends normally.
  void handleStmt() {
    Mark body = mark();
    skipBlock();
    if (!atKw(S_with)) fail("expected 'with': handle { ... } with op(params) { ... }");
    int base = F->depth, n = 0;
    while (atKw(S_with) && peek().t == T_ID) {
      next();
      Tok t = cur;
      int op = 0, arity = -1;
      if (atKw(K_yield)) next(), op = OP_YIELD, arity = 1;
      else {
        ident();
        Tok q = topName(prev);
        Global* g = findGlobal(q.sym);
        if (g && g->kind == 'e') std::tie(op, arity) = effectOp(q);
        else if (g && g->kind == 'n' && (op = effBuiltinOp(q.sym))) arity = effOpArity(op);
        else failAt(t.p, t.line, "'%s' is not an effect's operation (print, clock, sleep, random, run, yield, the graphics builtins, or Effect.name)", std::string(t.p, prevEnd).c_str());
      }
      imm(0, box((double)op)), st(0, slot());
      if (!at('(')) fail("expected '(' and the operation's parameters");
      Fn* h = lambda(t);
      if (h->arity - 1 != arity) failAt(t.p, t.line, "a handler for %s takes %d parameters, not %d", effOpName(op).c_str(), arity, h->arity - 1);
      st(0, slot()), n += 2;
    }
    collect(base, n, H_LIST);
    callNative("handle@begin", true);
    Mark after = mark();
    rewind(body);
    F->handles++, block(), F->handles--;
    if (F->live) callNative("handle@end", true);
    rewind(after);
  }

  void proveOperands(int op, bool lnum, bool rnum, int lh, int rh) {
    if (op == O_ADD || op == O_EQ || op == O_NE) return;
    if (op <= O_MOD || op >= O_BAND || rnum) prove(lh);
    if (op <= O_MOD || op >= O_BAND || lnum) prove(rh);
  }
  void loopTop(Loop& lp) {  // facts must hold on every back-edge: tier 2 keeps those of locals the loop never assigns
    auto had = F->facts;
    F->facts.reset();
    if (F->tier2) for (auto& l : F->locals) if (tracked(l.home) && had[l.home + 128] && !assignedIn(l.sym)) prove(l.home);
    lp.tries = F->tries.size(), lp.atomics = F->atomics, lp.handles = F->handles, loopBegin(lp);
  }

  void combine(int s, int op) { mv(1, 0), ld(0, s), F->depth--, binop(op); }  // left in slot s, right in x0
  // x0 = x0 op <operand binding tighter than p>; returns whether the result is known to be a number
  bool rhs(int op, int p = 0, bool lnum = false, int lh = NOH) {  // lh: the left operand's local, if any
    bool rnum = right(p, true, lh);
    int rh = rHome;
    binop(op, lnum, rnum), proveOperands(op, lnum, rnum, lh, rh);
    return (lnum && rnum && op <= O_MOD) || op >= O_BAND;
  }
  int rHome = NOH;  // the local right() loaded, if it was one
  // x1 = the right operand (x0 kept, if keep); returns whether it is known to be a number. lh: the local x0 was just
  // loaded from, if any. Then x0 need not be saved around the right operand: it is loaded again after (no expression
  // can assign a local), which also keeps it in its FP register for the arithmetic, when it lives in one.
  bool right(int p, bool keep = true, int lh = NOH) {
    rHome = NOH;
    if (int k = simpleOperand(p)) return k == 2;
    int s = slot();
    if (keep && lh != NOH) unload(0, lh);
    else if (keep) st(0, s);
    bool num = expr(p + 1);
    mv(1, 0), F->depth--, rHome = NOH;
    if (keep) lh != NOH ? ldl(0, lh) : ld(0, s);
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
  void load(LV& v) {
    switch (v.k) {
      case LV_LOCAL: ldl(0, v.a); break;
      case LV_GLOBAL: gmem(0, v.a); break;
      case LV_CAP: capLoad(v.a); break;
      case LV_FIELD: field(v.b, false); break;
      case LV_INDEX: mv(1, 0), ld(0, v.a), F->depth--, index(false); break;  // index in x0, object in slot a
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
  void pushCall() {  // x.push(v): appended inline when x is a list with room (listPush), else the method call
    int base = slot();
    st(0, base);
    int n = exprList(')', true);
    if (n == 1) return listPush(base);
    if (n) st(0, slot());
    callWindow(base, n, S_push, false);
  }
  void call(int sym, bool super, bool check = false) {  // check: then the receiver's invariants
    int base = slot();
    st(0, base), callWindow(base, exprList(')'), sym, super);
    if (check) invariantAfter(base);
  }
  // After a call or a field store on the object in slot base (x0: the result, kept): its class's invariants, by
  // calling its hidden invariant method (none: rt_prep returns the receiver). Frees base.
  void invariantAfter(int base) {
    F->depth = base + 1;
    int keep = slot(), w = slot();
    st(0, keep), ld(0, base), st(0, w), callWindow(w, 0, invariantSym(), false), ld(0, keep);
    F->depth = base;
  }
  void construct(Global& g) {  // C(args) for a top-level class C
    int base = slot();
    gmem(0, g.slot), st(0, base);
    constructCall(base, exprList(')'));
    if (invariantsOn) { int w = slot(); st(0, w), callWindow(w, 0, invariantSym(), false); }  // the new instance's
  }
  bool returnsNum(int sym) {  // a top-level fn declared `: num`: its returns check that, so callers may rely on it
    auto it = spans.find(symName(sym));
    return it != spans.end() && it->second.retNum;
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
  struct Mark { Lexer lx; Tok cur, prev; const char *prevEnd, *srcStart; std::string file; };  // a place in the source
  Mark mark() { return {lx, cur, prev, prevEnd, srcStart, file}; }
  void rewind(const Mark& m) { lx = m.lx, cur = m.cur, prev = m.prev, prevEnd = m.prevEnd, srcStart = m.srcStart, file = m.file; }
  template <class Body> void detour(const std::string& f, const char* src, const char* at, int line, Body body) {
    Mark m = mark();
    file = f, srcStart = src, lx = Lexer{at, line}, next();
    body(), rewind(m);
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
  Fn* lambda(const Tok& t) {  // fn(params) { body }: captures enclosing locals by value into the closure
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
    return fn;
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
        if (F->atomics) failAt(t.p, t.line, "'yield' inside atomic { }: other code would run while it is half done (yield after it ends)");
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
    if (!g) undefined(t);
    if (g->kind == 'f' && at('(') && !cur.nl) return next(), directCall(t, *g), t.sym == F->self || returnsNum(t.sym) ? NUM : LV{};
    if (g->kind == 'c' && at('(') && !cur.nl) return next(), construct(*g), LV{};
    if (g->kind == 'e') return perform(t), LV{};
    return {LV_GLOBAL, g->slot, t.sym, g->kind != 'v'};  // b: its symbol, for its type
  }
  LV postfix() {
    LV v = primary();
    for (;;) {
      if (accept('.')) {
        Tok ft = cur;
        int s = ident();
        bool own = v.k == LV_LOCAL && F->cls >= 0 && v.a == F->selfHome;  // self.name
        if (own && !tiering && !(at('(') && !cur.nl))
          fieldUses.push_back({F->cls, s, {ft.p, ft.line, file, srcStart}});  // self.name: checked once every class is known
        load(v);
        v = {LV_FIELD, 0, s}, v.own = own;
        if (at('(') && !cur.nl) {  // a method call; one on another object then checks its invariants (push: not inline)
          bool check = invariantsOn && !own;
          next(), s == S_push && !check ? pushCall() : call(s, false, check), v = {};
        }
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
      bool num = operand();
      return neg(num), num ? NUM : LV{};
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
      bool rnum = right(p, true, lh);
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
          Tok n = cur;
          int s = ident();
          Ty ty = accept(':') ? type() : Ty{};
          if (ty.code == TY_ANY) ty = {};
          bool top = F->top && F->scope == 0, init = accept('=');
          std::string what = "var '" + symName(top ? qualify(s) : s) + "'";
          if (ty.code && !init && !(ty.code & TY_OPT)) failAt(n.p, n.line, "%s is %s, so it needs an initial value", what.c_str(), ty.text.c_str());
          Tok v = cur;
          bool num = init ? expr() : (imm(0, NIL), false);
          if (ty.code && num && (ty.code & 15) != TY_NUM) failAt(v.p, v.line, "%s is %s, but its initial value is a number", what.c_str(), ty.text.c_str());
          checkType(ty, what, num);
          if (top) return gmem(0, global(qualify(s)).slot, true), false;
          int site = ty.code ? typeSite(what, ty.text) : -1;
          bool isNum = ty.code == TY_NUM;  // every store checks it, so it stays a number
          auto it = std::find_if(F->locals.begin() + F->scopeAt, F->locals.end(), [&](auto& l) { return l.sym == s; });
          if (it != F->locals.end()) {  // redeclared in the same block: reuse
            it->ty = ty.code, it->site = site, it->num = isNum;
            return stl(0, it->home), false;
          }
          int h = num || isNum ? fhome() : home();
          if (stl(0, h), num) prove(h);
          return F->locals.push_back({s, h, isNum, ty.code, site}), false;
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
        {
          bool n = at('}') || at(T_EOF) || cur.nl ? (imm(0, NIL), false) : expr();
          for (int i = 0; i < F->atomics; i++) callNative("atomic@end", true);  // passes x0 through
          for (int i = 0; i < F->handles; i++) callNative("handle@end", true);
          leaveTries(0);                          // leaving every try
          F->retNum &= n || F->retTy == TY_NUM;  // the epilogue checks the result type
          return ret(), false;
        }
        case K_break: case K_continue:
          next();
          if (!F->loop) failAt(prev.p, prev.line, "'%s' outside a loop", name(prev.sym));
          for (int i = F->atomics; i > F->loop->atomics; i--) callNative("atomic@end", true);
          for (int i = F->handles; i > F->loop->handles; i--) callNative("handle@end", true);
          leaveTries(F->loop->tries), jump(*F->loop, prev.sym == K_break), F->live = false;
          return false;
        case K_class: case K_import: fail("'%s' is only allowed at top level", name(cur.sym));
        case K_fn: if (!peekIs('(')) fail("named functions are only allowed at top level; use 'var f = fn(...) {...}'"); break;
        case S_expect: if (isExpect()) return expectStmt(), false; break;
        case S_atomic: if (peekIs('{') && !peek().nl) return next(), atomicStmt(), false; break;
        case S_handle: if (peekIs('{') && !peek().nl) return next(), handleStmt(), false; break;
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
  // A try around the block at cur; `after` runs when the block completes. Leaves the handler open (x0 = the value
  // thrown): anything in the block may have run.
  template <class After> TryAt tryBlock(After after) {
    TryAt t = tryBegin();
    F->tries.push_back(t.r), block(), F->tries.pop_back();
    after(), tryCatch(t);
    F->facts.reset(), F->live = true, F->depth = t.r;
    return t;
  }
  void tryStmt() {
    bool l1 = true;
    TryAt t = tryBlock([&] { l1 = F->live; });
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
  // atomic { }: its changes to state other code can see (fields, lists, maps, globals, captured variables) all
  // happen, or none do: an error leaving it rolls them back (txn.cpp) before whatever catches the error runs.
  // It commits wherever control leaves it normally: its end, return, break or continue.
  void atomicStmt() {
    callNative("atomic@begin", false);
    F->atomics++, block(), F->atomics--;
    if (F->live) callNative("atomic@end", true);
  }
  static const char* tokStart(const Tok& t) { return t.t == T_STR ? t.p - 1 : t.p; }  // a string token's p is past its quote
  // expect <condition>: `expect` starts a statement unless a variable or function of that name is in scope.
  bool isExpect() {
    if (resolve(F, S_expect).k != LV_NONE || findGlobal(globalSym(S_expect))) return false;
    Tok n = peek();
    return !n.nl && n.t != '=' && n.t != '.' && n.t != T_EOF && !(n.t >= T_ADDEQ && n.t <= T_DIVEQ);
  }
  void callNative(const char* nm, bool arg) {  // nm(x0) or nm(): a runtime function the compiler calls by name
    int base = slot();
    if (arg) st(0, slot());
    gmem(0, findGlobal(intern(nm))->slot), st(0, base), callWindow(base, arg, 0, false);
  }
  // expect a op b | expect x | expect throws { ... } [op value] | expect prints { ... } [op value]. Calls
  // expect@(site, left, right), which checks and, on failure, raises an error naming the source text and the values.
  void expectStmt() {
    ExpectSite s{file, "", "", "", cur.line};
    next(), check(std::move(s));
  }
  // The condition at cur, checked by expect@ with s (a contract's: a plain condition, no throws or prints).
  void check(ExpectSite s) {
    int base = slot(), siteAt = slot();
    gmem(0, findGlobal(intern("expect@"))->slot), st(0, base);
    const char* start = tokStart(cur);
    bool throws = s.throws = !s.contract && atKw(S_throws) && peekIs('{'), prints = !s.contract && atKw(S_prints) && peekIs('{'), lnum = false;
    int lh = NOH;
    if (prints) {  // what print() wrote inside, as a string; a throw stops the capture and goes on
      next();
      callNative("capture@begin", false);
      TryAt t = tryBlock([&] { callNative("capture@end", false); });
      callNative("capture@abort", true), tryEnd(t);
      s.left = "the output";
    } else if (throws) {  // the thrown value, else ABSENT
      next();
      TryAt t = tryBlock([&] { imm(0, ABSENT); });
      tryEnd(t);
      s.left = "the error";
    } else lnum = operand(&lh), binRest(5, lnum, lh), s.left.assign(start, prevEnd);
    const char* afterLeft = prevEnd;
    int ls = slot();
    st(0, ls);
    if (int t = cur.t, p = prec(t); p == 3 || p == 4) {
      next();
      const char* rs = tokStart(cur);
      expr(p + 1);
      s.op = opOf(t), s.right.assign(rs, prevEnd);
      if (prec(cur.t)) {  // `expect a == b && c`: one condition, checked for truthiness
        if (throws || prints) fail("expect %s { } compares with one value", throws ? "throws" : "prints");
        mv(1, 0), ld(0, ls), binop(s.op);
        bool n = false;
        int h = NOH;
        binRest(1, n, h), st(0, ls), s.op = -1, s.left.assign(start, prevEnd), s.right.clear(), imm(0, NIL);
      }
    } else if (!throws && !prints) binRest(1, lnum, lh), st(0, ls), s.left.assign(start, prevEnd), imm(0, NIL);
    else imm(0, NIL);
    st(0, slot());
    s.text = throws || prints ? (throws ? "throws { ... }" : "prints { ... }") + std::string(afterLeft, prevEnd) : std::string(start, prevEnd);
    bool contract = s.contract;
    int op = s.op, e = -1;
    V site = box((double)expectSite(std::move(s)));
    if (contract) {  // a contract tests inline and calls expect@ only to report a failure
      if (op >= 0) mv(1, 0), ld(0, ls), binop(op);
      else ld(0, ls);
      Cond no = truthyCond();
      thenBegin(no), e = elseBegin(no, true);
    }
    imm(0, site), st(0, siteAt);
    callWindow(base, 3, 0, false);
    if (contract) ifEnd(e);
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
    if (!l1) return;
    if (!F->live) return (void)(F->facts = f1, F->live = true);  // only the then-branch does
    F->facts &= f1;
  }
  // Does pred(token, the one after) hold anywhere from cur to the end of the first { } block? (`open`: if it never ends)
  template <class P> bool ahead(bool open, P pred) {
    Lexer s = lx;
    int d = 0;
    for (Tok p = cur, n; p.t != T_EOF && p.t != T_ERR; p = n) {
      if (p.t == '{') d++;
      else if (p.t == '}' && --d == 0) return false;
      n = s.next();
      if (pred(p, n)) return true;
    }
    return open;
  }
  static bool tryWord(const Tok& t) { return t.t == T_ID && (t.sym == K_try || t.sym == S_throws || t.sym == S_prints); }
  bool hasTry() { return ahead(false, [](const Tok& p, const Tok&) { return tryWord(p); }); }
  bool assignedIn(int sym) {  // might the block starting at cur assign to sym? (conservative)
    return ahead(true, [&](const Tok& p, const Tok& n) { return p.t == T_ID && p.sym == sym && (n.t == '=' || (n.t >= T_ADDEQ && n.t <= T_DIVEQ)); });
  }
  void forStmt() {  // for x in a..b { }  |  for x in list/map/string/fiber { }
    int nm = ident();
    if (!atKw(K_in)) fail("expected 'in'");
    next();
    scope([&] {
      bool num = expr();
      Loop lp;
      if (accept(T_DOTDOT)) {  // the bounds must be numbers (checked unless known)
        int a = fhome();
        if (!num) helper(H_NUMCHK);
        stn(0, a);
        if (!expr()) helper(H_NUMCHK);
        int b = fhome();
        stn(0, b);
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
    auto ty = varType(F, v);
    if (v.k == LV_LOCAL && isFReg(v.a) && op >= 0 && op <= O_DIV) {
      unprove(v.a);
      bool rnum = right(0, false);  // the left side is the local itself
      if (!ty.first || rnum) return fpAssign(v.a, op, rnum);  // a number op= a number: a number
      ldl(0, v.a), binop(op, v.num, false), storeCheck(ty, false), stl(0, v.a);
      if (v.num) prove(v.a);
      return;
    }
    auto value = [&](bool lnum = false) { return op < 0 ? expr() : rhs(op, 0, lnum, v.k == LV_LOCAL ? v.a : NOH); };
    if (v.k == LV_LOCAL || v.k == LV_GLOBAL || v.k == LV_CAP) {
      LV t = v;
      if (op >= 0) load(t);
      bool num = value(t.num);
      storeCheck(ty, num);
      if (v.k == LV_LOCAL) {
        stl(0, v.a);
        if (num) prove(v.a);
      } else if (v.k == LV_GLOBAL) gmem(0, v.a, true);
      else capStore(v.a);
    } else if (v.k == LV_FIELD) {
      int o = slot();
      st(0, o);
      if (op >= 0) field(v.b, false);
      value(), mv(2, 0), ld(0, o), field(v.b, true);
      if (invariantsOn && !v.own) invariantAfter(o);
      else F->depth--;
    } else {  // object in slot v.a, index in x0
      int i = slot();
      st(0, i);
      if (op >= 0) mv(1, 0), ld(0, v.a), index(false);
      value(), mv(2, 0), ld(0, v.a), ld(1, i), index(true), F->depth -= 2;
    }
  }

  // ---- declarations ----
  // Compile `(params) { body }` and queue it to be installed behind `stub`. Returns the arity.
  int function(Fun& f, int cls, bool init, const std::string& nm, u32* stub, Fn* fn) {
    f.cls = cls, f.init = init, f.k.name = nm, f.k.file = file;
    Fun* outer = F;
    F = &f, f.noRegs = hasTry(), beginFun();
    if (cls < 0 && !f.lambda && !f.test) f.stub = stub;
    std::vector<int> homes;
    std::vector<std::pair<int, Ty>> ptys;  // each declared parameter's symbol and type
    bool edecl = false;                    // declared effects, their bits and their text (`! io, clock`)
    uint64_t emask = 0;
    std::string etext;
    if (cls >= 0) homes.push_back(f.selfHome = home()), f.locals.push_back({K_self, f.selfHome});
    if (f.lambda) homes.push_back(f.cloHome = home());  // the closure itself, for captured variables
    int first = (int)homes.size();  // the first declared parameter
    if (!f.test) {
      expect('(');
      if (!accept(')')) {
        do {
          int s = ident(), h = home();  // not d8..: arguments arrive, and calls pass them, in x registers
          f.locals.push_back({s, h}), homes.push_back(h);
          ptys.push_back({s, accept(':') ? type() : Ty{}});
          if (Ty& t = ptys.back().second; t.code && t.code != TY_ANY)  // stores to it are checked too
            f.locals.back().ty = t.code, f.locals.back().num = t.code == TY_NUM, f.locals.back().site = typeSite("argument '" + symName(s) + "' of " + display(nm), t.text);
        } while (accept(','));
        expect(')');
      }
      Ty r;
      if (at(':')) {
        Tok c = cur;
        next(), r = type();
        if (init) failAt(c.p, c.line, "init can't declare a result type: it returns the new instance");
        f.retTy = r.code, f.retSite = typeSite("the result of " + display(nm), r.text);
      }
      if (at('!')) {  // declared effects: ! io, clock (an empty set: performs nothing)
        next(), edecl = true, etext = "!";
        if (!at('{') && !atKw(S_requires))
          do etext += etext.size() > 1 ? ", " : " ", emask |= effectName(etext);
          while (accept(','));
      }
      if (!f.lambda && !tiering) {  // for the override check: "(num, _): str" (_: untyped)
        Sig& g = sigs[nm];
        g.types.clear(), g.text = "(";
        for (auto& [s, ty] : ptys) g.types.push_back(ty.code), g.text += (g.text.size() > 1 ? ", " : "") + (ty.code ? ty.text : "_");
        g.types.push_back(r.code), g.text += ")" + (r.code ? ": " + r.text : "");
        g.eff = edecl, g.effMask = emask, g.effText = etext;
      }
    }
    int arity = (int)homes.size();
    if (arity > 64) fail("too many parameters");
    for (int i = 0; i < arity; i++) param(i, homes[i]);
    for (int i = first; i < arity && i < 8; i++)
      if (f.spec >> (i - first) & 1) f.guard.push_back(i), prove(homes[i]);
    for (size_t i = 0; i < ptys.size(); i++) {  // tier 2: a number parameter its entry guard proved needs no check
      auto& [s, ty] = ptys[i];
      int h = homes[first + i];
      if (!ty.code || (ty.code == TY_NUM && proven(F, h))) continue;
      ldl(0, h), checkType(ty, "argument '" + symName(s) + "' of " + display(nm));
      if (ty.code == TY_NUM) prove(h);
    }
    if (edecl) effectsIn(emask, effSite(display(nm), emask, etext));
    if (atKw(S_requires)) {  // requires <condition>[, <condition>]..., checked on entry, performing nothing
      int h = effNarrow(0, effSite(display(nm) + "'s requires", 0, "!"));
      while (atKw(S_requires)) {
        next();
        do {
          ExpectSite s{file, "", "", "", cur.line};
          s.contract = true, s.head = display(nm) + " requires ";
          check(std::move(s));
        } while (accept(','));
      }
      effEnd(h);
    }
    Tier* tier = nullptr;
    if (!f.lambda && !f.test && !tiering) hotCounter(tier = &tiers[nm], first, homes);
    block();
    if (f.live) f.retNum = false;  // can end without a return: returns nil
    pend.push_back({stub, fn, arity, finish(), owner, f.lambda || f.test, {}, envFor(file)});  // a test body is installed like a lambda
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
    if (it == recs.end() || it->second.text != fnv(sp.start, sp.end) || it->second.env != envFor(file)) return false;
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
  // A field's default: the class keeps it and every instance shares it, so it is a constant (a list or map
  // default would be one object shared by all; such fields are set in init).
  V constant() {
    const char* why = "a field's default must be a number, string, true, false or nil (set other values in init)";
    if (accept('-')) return at(T_NUM) ? (next(), box(-prev.n)) : (fail("%s", why), NIL);
    if (accept(T_NUM)) return box(prev.n);
    if (atKw(K_true) || atKw(K_false) || atKw(K_nil)) return next(), prev.sym == K_true ? TRUE_ : prev.sym == K_false ? FALSE_ : NIL;
    if (!at(T_STR)) fail("%s", why);
    std::string s;
    for (const char *p = cur.p, *e = p + cur.len; p < e; p++) {
      if (*p == '{') fail("a field's default can't interpolate: set it in init");
      if (*p != '\\' || p + 1 == e) { s += *p; continue; }
      char c = *++p;
      s += c == 'n' ? '\n' : c == 't' ? '\t' : c == '0' ? '\0' : c;
    }
    next();
    return consts.push_back(box(newStr(s))), consts.back();
  }
  void classDecl() {
    ClassDef cd{qualify(ident()), -1, {}};
    if (accept(':')) {
      ident();
      Global* g = findGlobal(cd.super = topName(prev).sym);
      if (!g || g->kind != 'c' || cd.super == cd.sym) failAt(prev.p, prev.line, "bad superclass '%s'", name(cd.super));
    }
    expect('{');
    std::vector<Mark> invs;  // invariant clauses: compiled at the end, into the hidden invariant method
    while (!accept('}')) {
      if (atKw(S_pub)) fail("a class's fields and methods are public when the class is: put 'pub' before 'class'");
      if (atKw(S_invariant) && !peek().nl) {  // invariant <condition>: parsed here only to find its end
        next(), invs.push_back(mark());
        Fun scratch, *outer = F;
        size_t np = pend.size(), nu = fieldUses.size();
        scratch.cls = cd.sym, F = &scratch, scratch.selfHome = home(), scratch.locals.push_back({K_self, scratch.selfHome});
        expr();
        F = outer, pend.resize(np), fieldUses.resize(nu), hasLit = false;
        continue;
      }
      if (atKw(K_var)) {  // var name [= constant]: a declared field
        next();
        int s = ident();
        for (auto& [x, d] : cd.fields) if (x == s) failAt(prev.p, prev.line, "duplicate field '%s'", name(s));
        Ty ty = accept(':') ? type() : Ty{};
        bool given = accept('=');
        Tok dt = cur;
        V d = given ? constant() : NIL;
        if (ty.code && given && !typeAccepts(ty.code, d)) failAt(dt.p, dt.line, "field '%s' is %s, but its default is not", name(s), ty.text.c_str());
        if (ty.code) cd.ftypes.push_back({s, ty.code, typeSite("field '" + symName(s) + "' of " + symName(cd.sym), ty.text)});
        cd.fields.push_back({s, d});
        continue;
      }
      bool over = atKw(S_override) && peek().t == T_ID && peek().sym == K_fn;
      if (over) next();
      if (!atKw(K_fn)) fail("expected 'fn', 'override fn', 'var' or 'invariant' in class body");
      next();
      Tok mt = cur;
      int m = ident();
      for (auto& [x, f] : cd.methods) if (x == m) failAt(prev.p, prev.line, "duplicate method '%s'", name(m));
      for (auto& [x, d] : cd.fields) if (x == m) failAt(prev.p, prev.line, "'%s' is already a field", name(m));
      Fn*& f = methodFns[{cd.sym, m}];
      if (!f) f = make<Fn>(), f->code = newStub(), f->name = m, consts.push_back(box(f));
      cd.methods.push_back({m, f}), cd.written.push_back({m, over, {mt.p, mt.line, file, srcStart}});
      std::string q = symName(cd.sym) + "." + symName(m);
      if (unchanged(q)) continue;
      Fun fun;
      owner = q, function(fun, cd.sym, m == S_init, q, (u32*)f->code, f);
    }
    if (!invs.empty()) invariantMethod(cd, invs);
    classes.push_back(std::move(cd));
  }
  // A class's invariants become a hidden method that checks each, performing nothing, then the superclass's; it
  // returns self. Code outside the class calls it after constructing an instance, calling a method on it, or
  // storing to one of its fields (see invariantAfter); a method's own calls and stores on self don't, so it may
  // break an invariant while it works, as long as it is whole again when the caller gets control back.
  void invariantMethod(ClassDef& cd, const std::vector<Mark>& invs) {
    int inv = invariantSym();
    Fn*& fn = methodFns[{cd.sym, inv}];
    if (!fn) fn = make<Fn>(), fn->code = newStub(), fn->name = inv, consts.push_back(box(fn));
    cd.methods.push_back({inv, fn});
    std::string q = symName(cd.sym) + ".invariant";
    Mark after = mark();
    Fun f, *outer = F;
    f.cls = cd.sym, f.init = true, f.k.name = q, f.k.file = file, F = &f;  // init: it returns self
    f.selfHome = home(), f.locals.push_back({K_self, f.selfHome}), param(0, f.selfHome);
    effectsIn(0, effSite(symName(cd.sym) + "'s invariant", 0, "!"));
    for (const Mark& m : invs) {
      rewind(m);
      ExpectSite s{file, "", "", "", cur.line};
      s.contract = true, s.head = symName(cd.sym) + "'s invariant ", s.tail = " does not hold";
      check(std::move(s));
    }
    if (cd.super >= 0) { int b = slot(); ldl(0, f.selfHome), st(0, b), callWindow(b, 0, inv, true); }
    pend.push_back({(u32*)fn->code, fn, 1, finish(), q + "@", true, {}});  // owner: a method may be named invariant
    F = outer, rewind(after);
  }
  // extern ["library"] fn name(type, ...) [: type] -- a C function (a host function in wasm); types: num int long ptr str (and void)
  // extern [lib] fn name(types): result. lib: a C library ("libm.dylib"; none: the program's own symbols, or on
  // wasm the host's JavaScript), or a module of this file's package (its fut.pkg lists it: js, wasm or c).
  void externDecl() {
    std::string lib, shownLib;
    Tok lt = cur;
    const ForeignMod* fm = nullptr;
    if (accept(T_STR)) lib.assign(prev.p, prev.len), shownLib = "'" + lib + "'";
    else if (at(T_ID) && !atKw(K_fn)) {
      std::string m = symName(ident());
      const Package* p = pkgOf(file);
      if (!p) failAt(lt.p, lt.line, "'%s' would be a module of a package, and %s is in no package (a package's fut.pkg lists its modules: js %s ./%s.js)", m.c_str(), ::base(file).c_str(), m.c_str(), m.c_str());
      fm = p->module(m, !WASM);
      if (!fm) failAt(lt.p, lt.line, "package %s has no module '%s': add 'js %s ./file.js' (or wasm, or c) to %s", p->name.c_str(), m.c_str(), m.c_str(), p->manifest.c_str());
      if (WASM && fm->kind == "c") failAt(lt.p, lt.line, "module %s is a C library (c %s %s), which only the native build loads: add a js or wasm version of it to %s", m.c_str(), m.c_str(), fm->target.c_str(), p->manifest.c_str());
      if (!WASM && fm->kind != "c") failAt(lt.p, lt.line, "module %s is %s (%s %s %s), which only the wasm build loads: add a C version (c %s <library>) to %s", m.c_str(), fm->kind == "js" ? "JavaScript" : "WebAssembly", fm->kind.c_str(), m.c_str(), ::base(fm->target).c_str(), m.c_str(), p->manifest.c_str());
      lib = fm->kind == "c" ? fm->target : fm->kind + ":" + fm->target, shownLib = "module " + m + " (" + fm->target + ")";
      if (fm->kind != "c" && std::find(modFiles.begin(), modFiles.end(), fm->target) == modFiles.end()) modFiles.push_back(fm->target);
    }
    if (!atKw(K_fn)) fail("expected 'fn'");
    next();
    Tok t = cur;
    int s = ident();
    const char* err = nullptr;
    void* fn = externLookup(lib, symName(s), &err);
    if (err && !strcmp(err, "pending")) {  // the host loads it, then compiles again
      if (std::find(modulesPending.begin(), modulesPending.end(), lib) == modulesPending.end()) modulesPending.push_back(lib);
      fn = (void*)1;
    } else if (err) failAt(lt.p, lt.line, "cannot load %s", shownLib.c_str());
    if (!fn) failAt(t.p, t.line, fm && fm->kind != "c" ? "module %s has no function '%s'" : "no C function '%s'", fm ? fm->name.c_str() : name(s), name(s));
    // any: a value copied as JSON (lists and maps; functions and JS objects by reference); fn: a fut function
    // JavaScript can call. Both only into JavaScript; a wasm module's functions take numbers.
    bool js = WASM && (!fm || fm->kind == "js");
    auto type = [&](bool ret) {
      int ty = atKw(K_fn) ? (next(), K_fn) : ident();
      std::string n = ty == K_fn ? "fn" : symName(ty);
      bool num = n == "num" || n == "int" || n == "long" || (ret && n == "void");
      if (fm && fm->kind == "wasm" && !num) failAt(prev.p, prev.line, "a wasm module's functions take and return numbers (num, int, long%s)", ret ? ", void" : "");
      if ((n == "any" || (n == "fn" && !ret)) && !js) failAt(prev.p, prev.line, "'%s' values cross only into JavaScript (a js module, or the wasm build's host)", n.c_str());
      if (n == "any") return 'a';
      if (n == "fn" && !ret) return 'f';
      for (const char* c : {"num", "int", "long", "ptr", "str", "void"})
        if (n == c && (ret || c[0] != 'v')) return c[0] == 'n' ? 'd' : c[0];
      failAt(prev.p, prev.line, "unknown extern type '%s' (num, int, long, ptr, str%s%s)", n.c_str(), js ? ", any, fn" : "", ret ? ", void" : "");
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
  // effect Name { fn op(params) [: type] ... }: operations with no body. Code performs one as Name.op(args); the
  // nearest handler for it answers (handle { } with Name.op(params) { }).
  void effectDecl() {
    Tok nt = cur;
    int raw = ident(), s = qualify(raw);
    for (const char* b : {"io", "clock", "random", "yield"})
      if (symName(raw) == b) failAt(nt.p, nt.line, "'%s' is a builtin effect", b);
    if (!effBit(s)) failAt(nt.p, nt.line, "too many effects (a program can declare at most %d)", EFF_LEVEL - 4);
    expect('{');
    EffectDef d{s, {}};
    std::vector<int> seen;
    while (!accept('}')) {
      if (atKw(S_pub)) fail("an effect's operations are public when the effect is: put 'pub' before 'effect'");
      if (!atKw(K_fn)) fail("expected 'fn' (an operation: fn name(params)) in an effect");
      next();
      Tok ot = cur;
      int o = ident();
      if (std::find(seen.begin(), seen.end(), o) != seen.end()) failAt(ot.p, ot.line, "duplicate operation '%s'", name(o));
      seen.push_back(o);
      std::string q = symName(s) + "." + symName(o);
      EffOpDef od{effOpId(q), 0, {}, 0, {}, -1};
      expect('(');
      if (!accept(')')) {
        do {
          int p = ident();
          Ty ty = accept(':') ? type() : Ty{};
          od.ptypes.push_back(ty.code), od.psites.push_back(ty.code ? typeSite("argument '" + symName(p) + "' of " + q + "()", ty.text) : -1);
          od.arity++;
        } while (accept(','));
        expect(')');
      }
      if (od.arity > 7) failAt(ot.p, ot.line, "an operation takes at most 7 parameters");
      if (accept(':')) {
        Ty r = type();
        od.rtype = r.code, od.rsite = typeSite("the result of " + q + "()", r.text);
      }
      if (at('{')) fail("an effect's operations have no body: a handler gives one (handle { } with %s(...) { })", q.c_str());
      d.ops.push_back(std::move(od));
    }
    effDefs.push_back(std::move(d));
  }
  void skipBlock() {  // a balanced { ... }, unparsed
    expect('{');
    for (int d = 1; d; next()) {
      if (at(T_EOF)) fail("expected '}'");
      d += at('{') - at('}');
    }
  }
  // test "name" { body }: when testing, compiled as a function of no parameters, listed in Compiled::tests.
  // A body naming something not defined yet compiles to nothing and the test is pending (write the spec first).
  void testDecl() {
    int line = prev.line;
    next();
    std::string nm;
    for (const char *p = prev.p, *e = p + prev.len; p < e; p++) nm += *p == '\\' && p + 1 < e ? *++p : *p;
    if (!at('{')) fail("expected '{' after the test's name");
    Mark body = mark();
    skipBlock();
    if (!testing) return;
    Mark after = mark();
    rewind(body);
    TestCase tc{nm, file, "", line, nullptr};
    Fun f;
    f.test = true;
    struct Job { Compiler* c; Fun* f; std::string name; u32* stub; } job{this, &f, "test \"" + nm + "\"", newStub()};
    Fun* outer = F;
    size_t np = pend.size();
    std::string err, so = owner;
    owner.clear();
    if (guardCompile([](void* v) { auto& j = *(Job*)v; j.c->function(*j.f, -1, false, j.name, j.stub, nullptr); }, &job, &err)) tc.code = job.stub;
    else {
      std::string head = err.substr(0, err.find('\n'));
      size_t at = head.find(": error: ");
      std::string msg = at == std::string::npos ? head : head.substr(at + 9), loc = at == std::string::npos ? "" : head.substr(0, at);
      bool missing = msg.starts_with("undefined name '") || msg.ends_with("' is not defined") || (msg.starts_with("module '") && msg.contains("' has no '"));
      if (!missing) compileFail(err, false);
      F = outer, pend.resize(np), hasLit = false, rewind(after);
      tc.pending = msg + (loc.empty() ? "" : " at " + loc.substr(loc.rfind('/') + 1));
    }
    owner = so;
    tests.push_back(std::move(tc));
  }
  void declare(const Tok& t, char kind, int arity) {
    Global& g = global(t.sym);
    if (g.kind == 'n') failAt(t.p, t.line, "cannot redefine builtin '%s'", name(t.sym));
    if (char& k = growAt(declKind, t.sym); k && (kind != 'v' || k != 'v')) failAt(t.p, t.line, "'%s' is already defined", name(t.sym));
    else k = kind;
    g.kind = kind;
    if (kind == 'f' && !g.stub) g.stub = newStub();
    mixEnv(uint64_t(t.sym) << 16 | uint64_t(kind) << 8 | uint64_t(arity), pubRaw), pubRaw = -1;
  }
  std::string resolvePath(std::string_view rel) {
    std::string p(rel);
    if (size_t d = file.rfind('/'); p[0] != '/' && d != std::string::npos) p = file.substr(0, d + 1) + p;
    return canonicalPath(p);
  }
  // What `import "x"` names: a file of this package, by path; or another package, by name (one its manifest lists
  // with `dep`), meaning that package's root file. pkg: the package's prefix (its alias), when imported by name.
  struct Target { std::string path, pkg; };
  Target importTarget(const Tok& t) {
    std::string s(t.p, (size_t)t.len);
    const Package* here = pkgOf(file);
    if (s.empty()) failAt(t.p, t.line, "expected a file or package name");
    if (s.find('/') == std::string::npos && !s.ends_with(".fut")) {  // a package
      if (!here) failAt(t.p, t.line, "'%s' is a package name, and %s is in no package: put a fut.pkg next to it (name app, then dep %s <its directory>), or import a file by path (\"./%s.fut\")", s.c_str(), ::base(file).c_str(), s.c_str(), s.c_str());
      if (s == here->name) failAt(t.p, t.line, "this file is in package %s: import its files by path (\"./file.fut\")", s.c_str());
      const PackageDep* d = nullptr;
      for (auto& x : here->deps) if (x.name == s) d = &x;
      if (!d) failAt(t.p, t.line, "package %s does not list '%s': add 'dep %s <its directory>' to %s", here->name.c_str(), s.c_str(), s.c_str(), here->manifest.c_str());
      const Package* p = packageAt(d->dir);
      std::string at = here->manifest + ":" + std::to_string(d->line);
      if (!p) failAt(t.p, t.line, "%s has no fut.pkg (dep %s, %s)", d->dir.c_str(), s.c_str(), at.c_str());
      if (p->name != s) failAt(t.p, t.line, "the package in %s is named '%s', not '%s' (%s)", d->dir.c_str(), p->name.c_str(), s.c_str(), at.c_str());
      std::vector<std::string> chain{here->name};  // packages depend on each other in one direction only
      std::unordered_set<std::string> seen;
      std::function<bool(const Package*)> reaches = [&](const Package* q) {
        chain.push_back(q->name);
        if (q == here) return true;
        if (seen.insert(q->dir).second)
          for (auto& e : q->deps) if (const Package* r = packageAt(e.dir); r && reaches(r)) return true;
        return chain.pop_back(), false;
      };
      if (reaches(p)) {
        std::string c;
        for (auto& n : chain) c += (c.empty() ? "" : " -> ") + n;
        failAt(t.p, t.line, "packages can't depend on each other in a cycle: %s (move what both need into a package of its own)", c.c_str());
      }
      if (std::string& o = pkgNamed[p->name]; o.empty()) o = p->dir;
      else if (o != p->dir) failAt(t.p, t.line, "two packages are named '%s': %s and %s", s.c_str(), o.c_str(), p->dir.c_str());
      pkgOf(p->root);
      std::string& pre = pkgPreOf[p->dir];
      if (pre.empty()) for (pre = p->name; prefixTaken(pre, p->dir);) pre += "_";
      pkgRootOf[pre] = p->root, pkgImports[pkgPrefix(file)].insert(pre);
      return {p->root, pre};
    }
    std::string ip = resolvePath(s);
    if (const Package* there = pkgOf(ip); there != here) {
      if (there) failAt(t.p, t.line, "'%s' is in package %s: import it by name (import \"%s\")%s", s.c_str(), there->name.c_str(), there->name.c_str(), here ? "" : ", from a package that lists it with dep");
      failAt(t.p, t.line, "'%s' is outside package %s: a package's files are the ones below its fut.pkg (%s)", s.c_str(), here->name.c_str(), here->dir.c_str());
    }
    return {ip, ""};
  }
  // Token pre-pass: learn every top-level name, arity and fn/method source span, following imports.
  void prescan(const std::string& path, const char* src) {
    std::string savedFile = file;
    const char* savedStart = srcStart;
    file = path, srcStart = src, aliases[path].clear();
    bool pubNext = false;  // after `pub`: the next top-level declaration is public
    auto q = [&](Tok t) {  // a top-level name as declared: note whether it is public, then its symbol
      if (pubNext) pubs[path].push_back(t.sym);
      else if (nsOf.count(path)) privateIn.emplace(t.sym, path);
      pubRaw = pubNext && !isModule[path] ? t.sym : -1;
      t.sym = qualify(t.sym);
      if (pubNext) growAt(pubSym, t.sym) = true;
      return pubNext = false, t;
    };
    struct Open { std::string q; const char* start; int line, depth; };
    std::vector<Open> open;
    Lexer L{src};
    int depth = 0, cls = -1, eff = -1;  // eff: inside `effect Name { }` (its operations have no bodies)
    bool ext = false;  // after `extern` (a C function: no body)
    // The parameters after a declaration's name: how many (a name after `(` or `,`; one after `:` is a type), and
    // whether its result is declared `: num` (and nothing else: `: num?` may be nil).
    auto params = [&](bool* retNum = nullptr) {
      Lexer s = L;
      int n = 0, last = '(';
      if (lex(s).t != '(') return 0;
      for (Tok a = lex(s); a.t != ')' && a.t != T_EOF; last = a.t, a = lex(s)) n += a.t == T_ID && (last == '(' || last == ',');
      if (Lexer r = s; retNum && lex(r).t == ':') {
        Tok a = lex(r), b = lex(r);
        *retNum = a.t == T_ID && symName(a.sym) == "num" && !(b.t == '?' && !b.nl);
      }
      return n;
    };
    for (Tok t = lex(L), p; t.t != T_EOF; p = t, t = lex(L)) {
      bool afterKw = p.t == T_ID && t.t == T_ID;
      topTry |= tryWord(t) && open.empty();  // a try in top-level code
      if (t.t == T_ID && t.sym == S_pub && !depth) {
        Lexer s = L;
        Tok n = lex(s);
        pubNext = n.t == T_ID && !n.nl && (n.sym == K_fn || n.sym == K_class || n.sym == K_var || n.sym == K_extern || n.sym == S_effect || n.sym == K_import);
      } else if (t.t == T_ID && t.sym == K_extern && !depth) ext = true;
      else if (ext && afterKw && p.sym == K_fn) declare(q(t), 'x', params()), ext = false; else if (t.t == '{') depth++;
      else if (t.t == '}') {
        if (--depth == 0) cls = -1, eff = -1;
        if (!open.empty() && depth == open.back().depth) {
          Span& sp = spans[open.back().q];
          sp.start = open.back().start, sp.end = L.p, sp.line = open.back().line, sp.endLine = L.line;
          open.pop_back();
        }
      } else if (afterKw && p.sym == K_fn && depth == 1 && eff >= 0) {  // an effect's operation
        int n = params();
        effOps[eff].push_back({t.sym, n});
        mixEnv(uint64_t(eff) << 40 | uint64_t(t.sym) << 8 | uint64_t(n), -2);
      } else if (depth == 0 && afterKw && p.sym == S_effect && !t.nl && [&] { Lexer s = L; return lex(s).t == '{'; }()) {
        Tok d = q(t);
        declare(d, 'e', 0);
        effOps[eff = d.sym].clear();
      } else if (afterKw && p.sym == K_fn && (depth == 0 || (depth == 1 && cls >= 0))) {
        Tok d = depth ? t : q(t);
        std::string qn = depth ? symName(cls) + "." + symName(t.sym) : symName(d.sym);
        if (spans.count(qn)) failAt(t.p, t.line, "'%s' is already defined", qn.c_str());
        Span& sp = spans[qn];
        int n = sp.arity = params(&sp.retNum);
        open.push_back({qn, p.p, p.line, depth});
        if (!depth) declare(d, 'f', n | (sp.retNum ? 128 : 0));  // callers rely on a `: num` result
      } else if (depth == 0 && afterKw && (p.sym == K_class || p.sym == K_var)) {
        Tok d = q(t);
        declare(d, p.sym == K_class ? 'c' : 'v', 0);
        if (p.sym == K_class) cls = d.sym;
        else if (VarTy& v = varTypes[d.sym] = {}; [&] { Lexer s = L; return lex(s).t == ':'; }()) {  // a typed var
          Lexer s = L;
          lex(s), v.file = path, v.src = src, v.at = s.p, v.line = s.line;
          const char* e = s.p;
          for (Lexer r = s;;) {
            Tok k = lex(r);
            if (k.t == '=' || k.t == ';' || k.t == T_EOF || k.nl) break;
            e = r.p;
          }
          mixEnv(fnv(s.p, e) ^ uint64_t(d.sym) << 32, -2);  // its stores check it, in importers too
        }
      } else if (depth == 1 && cls >= 0 && t.t == T_ID && t.sym == S_invariant && [&] { Lexer s = L; return !lex(s).nl; }()) {
        invariantsOn = true;
      } else if (depth == 1 && cls >= 0 && afterKw && p.sym == K_var) {  // a declared field: methods compile against them
        mixEnv(uint64_t(cls) << 32 | uint64_t(t.sym) << 2 | 1, -2);
      } else if (depth == 0 && t.t == T_STR && p.t == T_ID && p.sym == K_import) {
        bool re = pubNext;
        pubNext = false;
        Target tg = importTarget(t);
        std::string ip = tg.path;
        Lexer s = L;
        Tok a = lex(s), n = a.t == T_ID && a.sym == S_as ? lex(s) : Tok{};
        if (a.t == T_ID && a.sym == S_as && (n.t != T_ID || n.sym < NKW)) failAt(n.p, n.line, "expected a %s name after 'as'", tg.pkg.empty() ? "module" : "package");
        if (re && !tg.pkg.empty()) failAt(t.p, t.line, "'pub import' re-exports a file of this package; a package's dependencies are not part of its interface");
        if (re && n.t == T_ID) failAt(n.p, n.line, "'pub import' re-exports the file's pub names as they are: leave out 'as'");
        if (re) reexports[path].push_back(ip);
        if (!tg.pkg.empty()) aliases[path][n.t == T_ID ? n.sym : intern({t.p, (size_t)t.len})] = tg.pkg;  // the package's name
        if (sources.count(ip) && !nsOf.count(ip)) continue;  // the program's own file: already here, with global names
        std::string& pre = nsOf[ip];
        if (pre.empty()) {  // the file's stem (after its package's prefix), made unique
          pre = ip.substr(ip.rfind('/') + 1), pre = pre.substr(0, pre.find('.'));
          if (const std::string& P = pkgPrefix(ip); !P.empty()) pre = P + "/" + pre;
          while (prefixTaken(pre, ip)) pre += "_";
        }
        if (tg.pkg.empty() && n.t == T_ID) aliases[path][n.sym] = pre, isModule[ip] = true;  // import "file" as name: a module
        if (sources.count(ip)) continue;
        std::string& text = sources[ip].text;
        if (!readSource(ip, text)) failAt(t.p, t.line, "cannot read '%s'", ip.c_str());
        files.push_back(ip), prescan(ip, text.c_str());
      }
    }
    file = savedFile, srcStart = savedStart;
  }

  // ---- top level ----
  bool atPub() {  // `pub` before a top-level declaration (the prescan noted it); elsewhere `pub` is an ordinary name
    if (!atKw(S_pub)) return false;
    Tok n = peek();
    return n.t == T_ID && !n.nl && (n.sym == K_fn || n.sym == K_class || n.sym == K_var || n.sym == K_extern || n.sym == S_effect || n.sym == K_import);
  }
  void topLevel() {
    while (!at(T_EOF)) {
      if (atPub()) next();
      if (atKw(K_fn)) { next(), fnDecl(); continue; }
      if (atKw(K_class)) { next(), classDecl(); continue; }
      if (atKw(K_extern)) { next(), externDecl(); continue; }
      if (atKw(S_effect) && peek().t == T_ID && !peek().nl && peek().sym >= NKW) { next(), effectDecl(); continue; }
      if (atKw(S_test) && peekIs(T_STR)) { next(), testDecl(); continue; }
      if (atKw(K_import)) {
        next();
        if (!at(T_STR)) fail("expected a file or package name");
        next();
        std::string ip = importTarget(prev).path;
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
    for (auto& [pre, root] : pkgRootOf) {  // each package's interface: its root file's pub names, and re-exports'
      auto& names = surfaceOf[pre];
      names.clear();
      std::vector<std::string> todo{root};
      std::unordered_set<std::string> seen;
      while (!todo.empty()) {
        std::string f = todo.back();
        todo.pop_back();
        if (!seen.insert(f).second) continue;
        if (auto it = pubs.find(f); it != pubs.end()) names.insert(it->second.begin(), it->second.end());
        if (auto it = reexports.find(f); it != reexports.end()) todo.insert(todo.end(), it->second.begin(), it->second.end());
      }
    }
    std::unordered_map<std::string, uint64_t> surfEnv;  // each package's interface, as hashed for its importers
    for (auto& [P, decls] : pubDecls) {
      uint64_t h = 1469598103934665603ull;
      auto names = surfaceOf.find(P);
      for (auto [raw, v] : decls)
        if (raw == -2 || (names != surfaceOf.end() && names->second.count(raw))) h = (h ^ v) * 1099511628211ull;
      surfEnv[P] = h;
    }
    for (auto& [P, _] : pkgImports) envOf.try_emplace(P, 1469598103934665603ull);
    if (invariantsOn)  // every call and store site, in every package, checks them
      for (auto& [P, h] : envOf) h = (h ^ 0x1a7a) * 1099511628211ull;
    for (auto& [P, h] : envOf) {
      uint64_t e = h;
      for (auto& D : pkgImports[P]) e = (e ^ surfEnv[D]) * 1099511628211ull;
      envFinal[P] = e;
    }
    file = path, srcStart = src, lx = Lexer{src}, next();
    Fun top;
    top.top = true, top.noRegs = topTry, top.k.name = "<top>", top.k.file = path, F = &top;
    topLevel();
    if (!modulesPending.empty()) compileFail("waiting for the host to load modules", false);
    checkClasses();
    for (auto& [sym, v] : varTypes) globalType(sym);  // each typed var's type, for commit
    Code chunk = finish();
    return commit(chunk);
  }

  // ---- checks that need every class: overrides, and `self.name` against declared fields ----
  // A class as this compile defines it, else as the running program has it (a class defined at the REPL may
  // extend one from the program).
  struct ClassView { int super = -1; std::vector<std::pair<int, int>> methods; const std::vector<std::pair<int, V>>* fields = nullptr; };
  bool view(int sym, ClassView& v) {
    for (auto& cd : classes)
      if (cd.sym == sym) {
        v = {cd.super, {}, &cd.fields};
        for (auto& [m, f] : cd.methods) if (m != invariantSym()) v.methods.push_back({m, spans.at(symName(sym) + "." + symName(m)).arity});
        return true;
      }
    Global* g = findGlobal(sym);
    if (!g || !g->o || g->o->kind != K::Class) return false;
    Class* c = (Class*)g->o;
    v = {c->super ? (int)c->super->name : -1, {}, &c->fields};
    for (auto& [m, f] : c->methods) if (m != invariantSym()) v.methods.push_back({m, f->arity - 1});  // a method's Fn counts self
    return true;
  }
  [[noreturn]] void failHere(const At& a, const char* fmt, const std::string& x, const std::string& y = "", const std::string& z = "", const std::string& w = "") {
    file = a.file, srcStart = a.src, lexAtEnd = false;
    failAt(a.p, a.line, fmt, x.c_str(), y.c_str(), z.c_str(), w.c_str());
  }
  static std::string params(int n) { return std::to_string(n) + (n == 1 ? " parameter" : " parameters"); }
  void checkClasses() {
    // Methods: one that replaces a superclass's method says `override`, and takes the same parameters; one
    // that says `override` replaces something. So adding a method to a base class can't silently capture a
    // subclass's unrelated method of the same name, and renaming one can't silently orphan an override.
    for (auto& cd : classes)
      for (auto& w : cd.written) {
        if (w.sym == S_init) continue;  // constructors don't override
        ClassView v;
        int owner = -1, arity = -1;
        for (int s = cd.super; s >= 0 && owner < 0 && view(s, v); s = v.super)
          for (auto [m, a] : v.methods) if (m == w.sym) owner = s, arity = a;
        std::string m = symName(w.sym), q = symName(cd.sym) + "." + m;
        if (owner < 0 && w.over) failHere(w.at, "'%s' is marked override, but no superclass of %s has a method '%s'", m, symName(cd.sym), m);
        if (owner < 0) continue;
        if (!w.over) failHere(w.at, "'%s' overrides %s: write 'override fn %s' if that is intended", m, symName(owner) + "." + m, m);
        if (int mine = spans.at(q).arity; mine != arity)
          failHere(w.at, "'%s' takes %s, but %s, which it overrides, takes %s", m, params(mine), symName(owner) + "." + m, params(arity));
        // Callers that hold a superclass's instance rely on its method's types, so an override declares the same.
        auto a = sigs.find(symName(owner) + "." + m), b = sigs.find(q);
        if (a != sigs.end() && b != sigs.end() && a->second.types != b->second.types)
          failHere(w.at, "'%s' must declare the same types as %s, which it overrides: %s, not %s", m, symName(owner) + "." + m, a->second.text, b->second.text);
        // ...and may perform only what the method it replaces declares (a subset: doing less breaks no caller).
        if (a != sigs.end() && b != sigs.end() && a->second.eff && (!b->second.eff || (b->second.effMask & ~a->second.effMask)))
          failHere(w.at, "'%s' must declare effects within those of %s, which it overrides (%s), not %s", m, symName(owner) + "." + m,
                   a->second.effText, b->second.eff ? b->second.effText : "none (unchecked)");
      }
    // Fields: in a class that declares its fields (or inherits declared ones), self.name must be one of them.
    for (auto& u : fieldUses) {
      ClassView v;
      bool closed = false, found = false;
      for (int s = u.cls; s >= 0 && view(s, v); s = v.super)
        for (auto& [f, d] : *v.fields) closed = true, found |= f == u.sym;
      if (closed && !found) failHere(u.at, "%s has no field '%s' (declare it in the class: var %s)", symName(u.cls), symName(u.sym), symName(u.sym));
    }
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
    out.files = files, out.tests = std::move(tests);
    for (auto& f : files) out.sources.push_back({f, sources.at(f).text});
    for (auto& f : files) {  // interfaces: a file's outside packages; a package's (its root's, with re-exports) inside
      if (f.empty() || f[0] == '<') continue;
      const Package* p = pkgOf(f);
      if (!p) { out.apis.push_back({f, apiPath(f), ::base(f), {f}, "", ""}); continue; }
      if (canonicalPath(f) != p->root) continue;
      ApiUnit u{f, apiPath(f), "package " + p->name, {}, p->name, p->version};
      std::vector<std::string> todo{f};
      while (!todo.empty()) {
        std::string g = todo.front();
        todo.erase(todo.begin());
        if (std::find(u.parts.begin(), u.parts.end(), g) != u.parts.end()) continue;
        u.parts.push_back(g);
        if (auto it = reexports.find(g); it != reexports.end()) todo.insert(todo.end(), it->second.begin(), it->second.end());
      }
      out.apis.push_back(std::move(u));
    }
    out.packages = pkgDirs;
    for (auto& d : pkgDirs) if (const Package* p = packageAt(d)) out.files.push_back(p->manifest);  // watched too
    for (auto& m : modFiles) out.files.push_back(m);
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
      if (p.lambda && p.owner.ends_with(".invariant@")) {  // a class's invariants, compiled every time: the old body goes
        u32*& old = invariantBodies[p.owner];
        if (old) jitRetire(old);
        old = body;
        continue;
      }
      if (p.lambda) { lambdas[p.owner].push_back(body); continue; }
      Span& sp = spans.at(p.owner);
      uint64_t text = fnv(sp.start, sp.end);
      auto old = recs.find(p.owner);
      if (old != recs.end()) {
        jitRetire(old->second.body), add(patched, p.owner);
        if (p.owner == "main" && reload && old->second.text != text) out.mainChanged = true;
      } else if (reload) add(added, p.owner);
      recs[p.owner] = {text, p.env, sp.line, p.arity, body, std::move(lambdas[p.owner]), echo};
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
      f->code = ffiThunk(e.fn, e.sig, e.ret, e.sym), f->arity = (int)e.sig.size(), f->op = OP_EXTERN;  // performs io
    }
    for (auto& d : effDefs) effDefine(d.sym, symName(d.sym), d.ops);
    for (auto& [s, l] : effOps) effectOps[s] = l;
    for (auto& cd : classes) globalObj<Class>(cd.sym);  // all exist before any superclass link is made
    for (auto& cd : classes) {
      Class* c = globalObj<Class>(cd.sym);
      c->super = cd.super >= 0 ? (Class*)global(cd.super).o : nullptr;
      c->methods = cd.methods, c->fields = cd.fields, c->ftypes = cd.ftypes;
      size_t n = c->layout.size();
      for (auto& [f, d] : cd.fields) if (std::find(c->layout.begin(), c->layout.end(), f) == c->layout.end()) c->layout.push_back(f);
      if (c->layout.size() != n) reshape(c);  // new instances get room for every declared field
    }
    for (int s : inits) if (!global(s).inited) global(s).inited = true, out.newVars.push_back(s);
    for (auto& [sym, v] : varTypes) global(sym).type = v.code, global(sym).typeSite = v.site;
    clearICs();  // method tables or arities may have changed
    fix(chunk);
    out.chunk = jitInstall(chunk);
    jitCollect();
    out.log = patched.empty() && added.empty() && removed.empty() && reset.empty() ? "no code changes" : "";
    for (auto [label, list] : {std::pair{"patched ", &patched}, {"added ", &added}, {"removed ", &removed}, {"reset ", &reset}})
      if (!list->empty()) out.log += (out.log.empty() ? "" : "; ") + (label + *list);
    if (!echo) replaced = std::move(installed), installed = out.sources;
    return out;
  }
};

void tierUp(void* p, u32 numArgs) {
  Tier& t = *(Tier*)p;
  if (t.fast || t.failed || !t.body) return;
  JitBatch writable;
  static Compiler* c;  // on the heap, like `active` below
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
Compiled compile(const std::string& path, bool reload, bool echo, const std::string* text, bool tests) {
  JitBatch writable;  // nothing JIT-compiled runs during a compile
  delete active;
  Compiler& c = *(active = new Compiler);
  c.reload = reload, c.echo = echo, c.testing = tests;
  packagesReset(), modulesPending.clear();
  if (!echo) {
    mainFile = path;
    const Package* p = packageOf(path);
    entryDir = p ? p->dir : "";
  }
  std::string& src = c.sources[path].text;
  if (text) src = *text;
  else if (!readSource(path, src)) compileFail("cannot read " + path, false);
  Compiled out = c.run(path, src.c_str());
  delete active, active = nullptr;
  return out;
}
bool revertReload(const std::vector<int>& newVars, std::string* err) {
  for (int v : newVars) global(v).inited = false;  // if they come back, they are initialized then
  Sources to = replaced;
  if (to.empty()) return *err = "nothing to go back to", false;
  overlay = &to;
  Compiled c;
  auto f = [](void* p) { auto& [c, to] = *(std::pair<Compiled*, Sources*>*)p; *c = compile(to->front().first, true, false); };
  std::pair<Compiled*, Sources*> ctx{&c, &to};
  bool ok = guardCompile(f, &ctx, err);
  overlay = nullptr;
  if (ok) jitRetire((u32*)c.chunk);  // the old top level does not run again: the data is as it was
  return ok;
}
