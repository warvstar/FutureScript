// Single-pass compiler: the parser emits machine code directly (no AST, no IR), through a code generator:
// gen_a64.h (arm64) natively, gen_wasm.h in the WebAssembly build.
#include "compile.h"
#include <charconv>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <unordered_set>
#if __wasm__
#include "gen_wasm.h"
#else
#include "gen_a64.h"
#endif

CState::SourcePosition CState::sourcePosition(const char* at, int line) const {
  auto input = viewInputs.find(srcStart);
  if (input == viewInputs.end()) return {srcStart, at, line};
  const auto& [original, lowered] = input->second;
  size_t atByte = uintptr_t(at) - uintptr_t(srcStart);
  if (atByte > lowered.text.size()) return {srcStart, at, line};
  size_t offset = atByte == lowered.text.size() ? original.size() : lowered.origin(atByte);
  if (offset != std::string::npos)
    return {original.c_str(), original.c_str() + offset, 1 + int(std::count(original.begin(), original.begin() + offset, '\n'))};
  // A failure in generated code has no original column. Use the line it was emitted on.
  offset = 0;
  int on = 1;
  for (; on < line; on++) {
    size_t end = original.find('\n', offset);
    if (end == std::string::npos) break;
    offset = end + 1;
  }
  return {original.c_str(), original.c_str() + offset, on};
}

void CState::report(const char* pos, int line, const char* fmt, va_list ap) {
  const char* code = takeCode(fmt);
  char b[256];
  vsnprintf(b, sizeof b, fmt, ap);
  auto source = sourcePosition(pos, line);
  pos = source.at, line = source.line;
  for (auto& r : reports) if (r.diag.file == file && r.diag.line == line) return;
  const char *ls = pos, *le = pos;
  while (ls > source.start && ls[-1] != '\n') ls--;
  while (*le && *le != '\n') le++;
  std::string caret;
  for (const char* c = ls; c < pos; c++) caret += *c == '\t' ? '\t' : ' ';
  int col = int(pos - ls + 1);
  reports.push_back({{file, b, code ? code : "", line, col},
                     file + ":" + std::to_string(line) + ":" + std::to_string(col) + ": error: " + b +
                     (code ? " [" + std::string(code) + "]" : "") + "\n  " +
                     std::string(ls, le) + "\n  " + caret + "^"});
}
void CState::diagnose(const char* pos, int line, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  report(pos, line, fmt, ap);
  va_end(ap);
  if (!collectErrors) compileFail(reports.back().rendered, !*pos || lexAtEnd);
  if (reports.size() == 50) reportFailure();
}
void CState::failAt(const char* pos, int line, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  report(pos, line, fmt, ap);
  va_end(ap);
  errorPos = pos;
  if (!collectErrors) compileFail(reports.back().rendered, !*pos || lexAtEnd);
  if (!recovery || reports.size() == 50) reportFailure();
#if __wasm__
  longjmp(*recovery, 1);
#else
  throw ParseError{};
#endif
}
void CState::sortReports() {
  // Keep files in discovery order and diagnostics within each file in source order.
  std::vector<std::string> files;
  for (auto& r : reports)
    if (std::find(files.begin(), files.end(), r.diag.file) == files.end()) files.push_back(r.diag.file);
  std::stable_sort(reports.begin(), reports.end(), [&](const Report& a, const Report& b) {
    if (a.diag.file == b.diag.file) return a.diag.line < b.diag.line;
    return std::find(files.begin(), files.end(), a.diag.file) < std::find(files.begin(), files.end(), b.diag.file);
  });
}
std::string CState::diagnosticText() {
  sortReports();
  std::string msg;
  for (auto& r : reports) msg += (msg.empty() ? "" : "\n") + r.rendered;
  return msg;
}
void CState::reportFailure() {
  failureText = diagnosticText();  // owned by the compiler across a possible longjmp
  compileFail(failureText, false);
}
Global& CState::global(int sym) {
  if (Global* g = findGlobal(sym)) return *g;
  if (!stageGlobals) return ::global(sym);
  int slot = globalCount() + newGlobals.size();
  if (slot >= MAXG) compileFail("too many globals", false);
  newGlobals.push_back(sym);
  return declarations.emplace(sym, Global{.slot = slot}).first->second;
}

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
  std::vector<std::pair<int, K>> collectionInits;
  std::vector<Class::FieldType> ftypes;   // those with a type
  uint8_t sum = 0;                        // Class::sum: 1 an enum, 2 a variant
};
// enum Shape { Circle(r: num) Rect(w, h) Empty }: a closed class Shape with no instances, and a closed subclass of it
// per variant, named Shape.Circle, whose declared fields are the variant's. Shape.Circle(2) makes one; match takes
// them apart. Kept across compiles (the REPL), by the enum's symbol.
struct VariantDef { int raw, sym; std::vector<int> fields; };
struct EnumDef { int raw; std::vector<VariantDef> vs; };
static std::unordered_map<int, EnumDef> enumDefs;
struct FieldUse { int cls, sym; At at; };  // `self.name` in a method: checked against declared fields
// `x.name` where x is a variable with a declared type (a shape, or a class): checked against that type's fields once
// every type is known. Stores to x are checked, so x has its type wherever it is used.
static int editDistance(const std::string& a, const std::string& b) {  // for "did you mean"; swapping two neighbours is one edit
  std::vector<int> two(b.size() + 1), prev(b.size() + 1), row(b.size() + 1);
  for (size_t j = 0; j <= b.size(); j++) row[j] = (int)j;
  for (size_t i = 1; i <= a.size(); i++) {
    two.swap(prev), prev.swap(row);  // two: row i - 2, prev: row i - 1
    row[0] = (int)i;
    for (size_t j = 1; j <= b.size(); j++) {
      row[j] = std::min({prev[j] + 1, row[j - 1] + 1, prev[j - 1] + (a[i - 1] != b[j - 1])});
      if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1]) row[j] = std::min(row[j], two[j - 2] + 1);
    }
  }
  return row[b.size()];
}
struct TypedUse { int path, sym; At at; bool call; std::string var; };
constexpr uint64_t TY_NILLIT = TY_OPT;  // a static type (never a declared one): the value nil, as written
// A fn/method in the source. retNum: it declares `: num`, so a direct call's result is known to be a number.
struct Span { const char *start = nullptr, *end = nullptr; int line = 0, endLine = 0, arity = 0; bool retNum = false; };
// A contract's clause: where its condition starts, and whether it calls anything (then the checks run in a
// transaction that always rolls back, with no effects allowed: checking a contract changes nothing).
struct Clause { At at; bool calls = false; };
// A function's contract: the requires and ensures clauses between its signature and its body. fn: its `fn`; olds:
// each old( in ensures.
struct SpecFn { At fn; std::string name; std::vector<Clause> pre, post; std::vector<At> olds; bool calls = false; };
// A class's contract: its invariants, and its methods that have contracts (in order). at: the class's name, as
// written there (name).
struct ClassSpec { At at; std::string name; std::vector<Clause> invariants; std::vector<int> methods; };
// Each fn/method's parameter and result types (0: none), as last compiled: an override must declare the same.
// eff: it declares effects (effMask, as written: effText), so an override must declare a subset.
struct Sig { std::vector<uint64_t> types; std::string text; bool eff = false; uint64_t effMask = 0; std::string effText; std::vector<int> params; };
// Each effect's operations (qualified effect symbol -> (operation symbol, arity)), as last compiled: what performs
// and handlers compile against when the effect is not in the files being compiled (the REPL).
using EffOps = std::unordered_map<int, std::vector<std::pair<int, int>>>;
static EffOps effectOps;
static std::unordered_map<std::string, Sig> sigs;
struct Rec { uint64_t text, env; int line, arity; u32* body; std::vector<u32*> lambdas; bool repl; };  // what is installed now
static std::unordered_map<std::string, Rec> recs;
static std::unordered_map<int, uint64_t> varText;  // top-level var -> hash of its declaration's source
static std::map<std::pair<int, int>, Fn*> methodFns;  // (class sym, method sym): one Fn per method, kept across reloads
// Files: every imported file has a prefix (its stem, made unique). Its private top-level names are "prefix.name",
// so two files' private names never clash and other files can't reach them. A file imported `as` a name (a module)
// declares all its names that way, and other files reach its `pub` ones as alias.name. In a file imported plainly,
// `pub` names are global. The program's own file has no prefix: its names are global.
static std::string base(const std::string& path) { return path.substr(path.rfind('/') + 1); }
static std::unordered_map<std::string, u32*> invBodies;  // each class's hidden invariant method, installed
std::vector<std::string> modulesPending;
struct Namespaces {
  std::unordered_map<std::string, std::string> nsOf;  // imported file -> unique prefix
  std::unordered_map<std::string, bool> isModule;  // imported file -> imported with a name
  std::unordered_map<std::string, std::vector<std::string>> plainModules;
  std::unordered_map<std::string, std::unordered_map<int, std::string>> aliases;
  std::string entryDir, entryPath;
  std::unordered_map<std::string, std::string> pkgPreOf;  // package dir -> prefix
  std::unordered_map<std::string, std::unordered_set<int>> surfaceOf;  // prefix -> interface names
  bool prefixTaken(const std::string& p, const std::string& self) {
    for (auto& [f, q] : nsOf) if (f != self && q == p) return true;
    for (auto& [d, q] : pkgPreOf) if (d != self && q == p) return true;
    return false;
  }
};
static Namespaces installedNames;
// The sources of the code running now, and of the code the last compile replaced (what a failed reload goes back
// to); while the previous code is restored, files are read from `overlay` rather than from disk.
using Sources = std::vector<std::pair<std::string, std::string>>;
static Sources installed, replaced;
static const Sources* overlay;
static bool readSource(const std::string& path, std::string& out, const CompileCheck* check) {
  if (check) for (auto& [file, text] : check->buffers)
    if (file == path) return out = text, true;
  if (!overlay) return readFile(path, out);
  for (auto& [f, text] : *overlay) if (f == path) return out = text, true;
  return false;
}

// All of this state belongs to one editor check. Compiler symbols (including qualified names)
// and frame homes are temporary. The index sent to the editor identifies bindings by source site.
struct Navigation {
  using Site = CompileSymbols::Site;
  using Role = CompileSymbols::Role;
  CompileSymbols& out;
  struct Source {
    const char* start;
    size_t bytes;
    int file;
    const LoweredView* lowered;
    const std::string* original;
  };
  std::unordered_map<std::string, Source> sources;
  std::map<Site, int> declared;
  std::unordered_map<int, int> globals, supers, constructors;
  std::map<std::pair<int, int>, int> fields, methods, aliases, operations;
  std::unordered_set<int> reassigned;
  std::unordered_map<int, uint64_t> paramTypes;
  const char *ctorStart = nullptr, *ctorEnd = nullptr;
  int ctorClass = -1;
  struct MemberUse { Site site; int name, cls, binding; uint64_t type; bool call, super; Role role; int path = 0; };
  std::vector<MemberUse> members;

  Site site(const std::string& file, const char* start, const Tok& t) const {
    auto s = sources.find(file);
    if (t.t != T_ID || s == sources.end() || s->second.start != start) return {};
    auto pos = uintptr_t(t.p), base = uintptr_t(start);
    if (pos < base || pos - base >= s->second.bytes || !idc(*t.p)) return {};
    const char* end = t.p + 1;
    while (idc(*end) || dig(*end)) end++;
    size_t length = end - t.p, offset = pos - base;
    if (auto* lowered = s->second.lowered) offset = lowered->origin(offset, length);
    if (offset == std::string::npos) return {};
    return {s->second.file, int(offset), int(length)};
  }
  void use(Site site, int id, Role role = CompileSymbols::Read) {
    if (site.file < 0 || id < 0) return;
    if (role == CompileSymbols::Read) {
      const auto& src = sources.at(out.files[site.file]);
      if (src.lowered && std::binary_search(src.lowered->writes.begin(), src.lowered->writes.end(), size_t(site.offset)))
        role = CompileSymbols::Write;
    }
    out.occurrences.push_back({site, id, role});
  }
  std::string_view keyword(Site site) const {
    const auto& src = sources.at(out.files[site.file]);
    if (!src.lowered) return {};
    const std::string& text = *src.original;
    size_t end = site.offset;
    while (end && (text[end - 1] == ' ' || text[end - 1] == '\t')) end--;
    size_t start = end;
    while (start && (idc(text[start - 1]) || dig(text[start - 1]))) start--;
    return std::string_view(text).substr(start, end - start);
  }
  int declare(Site site, const Tok& t, std::string_view kind, std::string_view container, int earlier = -1) {
    if (site.file < 0) return -1;
    auto [it, fresh] = declared.emplace(site, earlier >= 0 ? earlier : int(out.declarations.size()));
    if (fresh) {
      if (earlier < 0) {
        std::string_view word = keyword(site);
        if ((kind == "fn" && word == "view") || (kind == "var" && word == "state")) kind = word;
        out.declarations.push_back({site, std::string(t.p, site.length), std::string(kind), std::string(container), {}});
      }
      use(site, it->second, CompileSymbols::Decl);
    }
    return it->second;
  }
  void collision(int first, int second) {
    if (first < 0 || second < 0 || first == second) return;
    Site a = out.declarations[first].site, b = out.declarations[second].site;
    out.collisions.push_back(std::minmax(a, b));
  }
  template <class Map, class Key> static int get(const Map& map, const Key& key) {
    auto it = map.find(key);
    return it == map.end() ? -1 : it->second;
  }
};

struct Compiler : Gen, Namespaces {
  Compiler() : Namespaces(installedNames) {}
  bool reload = false, echo = false, tiering = false, topTry = false, testing = false;  // testing: compile `test` blocks
  std::vector<TestCase> tests;
  std::string owner;
  Navigation* nav = nullptr;
  // fut test --changed: who the code being compiled belongs to, for the top-level names it uses (traceRef): refKey if
  // set, else owner (a function, a method and their lambdas), else testRefs (a test body; "<top>": top-level code).
  std::string refKey, testRefs = "<top>";
  std::vector<std::pair<const char*, const char*>> traceCuts;  // fns, classes, vars and tests: hashed on their own
  struct Source {
    std::string text;
    std::optional<LoweredView> view;
    bool compiled = false;
    const std::string& code() const { return view ? view->text : text; }
  };
  std::unordered_map<std::string, Source> sources;  // by path; nodes are stable, so text pointers stay valid
  std::vector<std::string> files, present;
  std::unordered_map<std::string, Span> spans;
  std::vector<char> declKind;  // by symbol
  // Metadata used by parsing and code generation is published together with the code.
  decltype(::enumDefs) enumDefs = ::enumDefs;
  decltype(::sigs) sigs = ::sigs;
  decltype(::varText) varText;
  decltype(::methodFns) newMethods;
  decltype(tiers) newTiers;
  std::vector<u32*> newStubs;
  bool published = false;
  std::string* testMissing = nullptr;
  std::unordered_map<const char*, const char*> invalidDecls;  // declaration start -> failed prescan token
  std::unordered_set<std::string> unavailableModules;

  template <class Work> bool attempt(Work work) {
    if (!collectErrors) return work(), true;
#if __wasm__
    // Syntax failures occur outside runtime locks. This jump only unwinds the parser;
    // compileFail still goes to the platform guard for failures that abandon the compile.
    jmp_buf jb, *outer = recovery;
    recovery = &jb;
    if (setjmp(jb)) return recovery = outer, false;
    work();
    recovery = outer;
#else
    bool outer = std::exchange(recovery, true);
    try { work(); }
    catch (ParseError&) { return recovery = outer, false; }
    recovery = outer;
#endif
    return true;
  }

  u32* parserStub() {
    if (editorCheck || !reports.empty()) return (u32*)(uintptr_t)1;  // code generation uses its address as an operand
    u32* stub = newStub();
    newStubs.push_back(stub);
    return stub;
  }
  Tier* tierFor(const std::string& name) {
    if (auto it = tiers.find(name); it != tiers.end()) return &it->second;
    return &newTiers[name];
  }
  Fn*& methodFn(int cls, int m) {
    auto key = std::make_pair(cls, m);
    if (auto it = methodFns.find(key); it != methodFns.end()) return it->second;
    return newMethods[key];
  }
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
    return (it != envFinal.end() ? it->second : 1469598103934665603ull) ^ (stepsOn ? 0x5354455053ull : 0);  // on: all recompile
  }
  std::vector<Pend> pend;
  std::vector<ClassDef> classes;
  std::vector<FieldUse> fieldUses;
  std::vector<TypedUse> typedUses;
  // A direct call of a top-level fn from a function that declares effects (outside its handle blocks): checked, once
  // every fn's declaration is known, against what the caller declares.
  struct EffCall { int callee; uint64_t allowed; std::string who, text; At at; };
  std::vector<EffCall> effCalls;
  std::unordered_map<uint64_t, std::string> effBitNames;  // a user effect's bit -> its name, as first declared
  // Values whose static type is known where they meet a declared type: a literal, a constructor, or something declared
  // (a typed var, parameter or field). Checked once every type is known; a clash no value could pass is an error.
  struct ArgVal { int path = 0; bool num = false; const char* p = nullptr; int line = 0; };  // path: its type; num: a number
  struct CallSite { int callee; std::vector<ArgVal> args; std::string file; const char* src; int cls = -1; const char* at = nullptr; int line = 0; int recv = 0; };  // a direct call of a top-level fn, (cls >= 0) a constructor's, or (recv: the receiver's type path) a method's, callee its name
  struct Expect { uint64_t type; int typePath, field; ArgVal value; std::string what, file; const char* src; };  // typePath: a field's
  std::vector<CallSite> callSites;
  std::vector<Expect> expects;
  // What each named function does, for fut map: the operations its code performs (its lambdas' too: a lambda made
  // here runs on this function's behalf, or on whoever's that called it, which is reached through here as well), and
  // the calls it makes, followed once every function is compiled (inferEffects). A call of a value is followed only
  // when the value is one of the function's own parameters (what the caller passes, it does) or a lambda made here;
  // any other (a field, a variable, a lambda's own parameter, a map's function) marks the function as calling code it
  // can't see. A function that declares effects does at most those: the runtime holds it to them. Recorded only when
  // asked for (inferEffectsWanted: fut map), so other compiles don't pay for it.
  enum { ARG_SAFE = -1, ARG_OTHER = -2 };  // an argument's origin: made here (a lambda, a literal, a named fn), from elsewhere; or a parameter's index
  struct EffSite { char kind; int sym, cls, path; std::vector<int> args; };  // f fn, m method (cls: self's class; path: the receiver's type), s super method, c constructor, x extern, b budget
  struct EffFn {
    std::string file, name;
    int line = 0, cls = -1;
    bool declared = false, unknown = false;
    uint64_t mask = 0, ops = 0, calls = 0;    // declared; performed; parameters called (bit i: the i-th)
    std::vector<int> params;                  // the parameters' symbols
    std::vector<EffSite> sites;
    std::unordered_set<int> assigned, called;  // names assigned again anywhere in it; names of lambdas made here it calls
  };
  std::unordered_map<std::string, EffFn> effFns;
  std::vector<std::pair<Fun*, int>> handledOps;  // the operations each open handle block answers, in the function it is in
  const char *lamStart = nullptr, *lamEnd = nullptr;  // the last lambda compiled
  struct { const char* p = nullptr; int origin = ARG_OTHER, sym = -1; } lastName;  // the last name read as a value
  bool fibers = false;  // the program makes a fiber (its send resumes code made elsewhere)
  std::vector<std::pair<int, int>> fnDefs;  // (sym, arity)
  std::vector<int> inits;                   // top-level vars initialised by this chunk
  struct Extern { int sym; void* fn; std::string sig; char ret; bool pure, rt = false; std::string stub; };  // pure: declared `!`, performs nothing; rt: fn is the runtime's Fn; stub: what a call raises (a module this build doesn't load)
  std::vector<Extern> externs;
  std::string patched, added, removed, reset;  // for the reload log
  std::unordered_map<std::string, std::vector<int>> pubs;  // file -> its top-level names marked `pub`
  std::vector<char> pubSym;                                 // by (prefixed) symbol: declared `pub`
  std::unordered_map<int, std::string> privateIn;           // a private name (as written) -> a file declaring it
  EffOps effOps;                                            // effects this compile declares (the prescan finds them)
  std::map<std::string, SpecFn> specFns;                    // "Account.withdraw" or "sqrt" -> its contract
  std::vector<std::string> fnSpecs;                         // the top-level functions with one, in order
  std::map<int, ClassSpec> classSpecs;                      // class symbol -> its invariants and methods with contracts
  std::vector<int> classOrder;                              // those classes, in order
  std::unordered_map<int, std::pair<At, int>> classAt;      // each class this compile declares: its name, and where
  std::unordered_map<int, At> superAt;                      // ... and where its superclass is named
  std::unordered_set<int> invClasses;                       // classes whose methods check invariants (theirs, a super's or a subclass's)
  std::vector<std::pair<std::string, At>> todos;            // functions whose body is todo(): pending under fut test
  const std::unordered_map<const char*, int>* olds = nullptr;  // compiling ensures: each old(...)'s home, by where it is
  const char* pubContract = nullptr;  // compiling a pub declaration's contract: what it is (it may use only pub names)
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
      it->second = packageOf(f.empty() || f[0] == '<' ? entryPath : f);
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
  void prepareSource(const std::string& path, Source& s) {
    if (lowerViews(s.text, path, s.view.emplace()))
      viewInputs.emplace(s.code().c_str(), ViewInput{s.text, *s.view});
    else s.view.reset();
    if (!nav) return;
    int id = int(nav->out.files.size());
    nav->sources.emplace(path, Navigation::Source{s.code().c_str(), s.code().size(), id, s.view ? &*s.view : nullptr, &s.text});
    nav->out.files.push_back(path);
    if (editorCheck->referenceQuery) {
      auto& lines = nav->out.lineStarts.emplace_back();
      lines.push_back(0);
      for (size_t i = 0; i < s.text.size(); i++) if (s.text[i] == '\n') lines.push_back(i + 1);
    }
  }
  Navigation::Site indexSite(const Tok& t) { return nav->site(file, srcStart, t); }
  static CompileSymbols::Role useRole(const Tok& following) {
    return following.t == '=' || (following.t >= T_ADDEQ && following.t <= T_DIVEQ) ? CompileSymbols::Write : CompileSymbols::Read;
  }
  void indexGlobal(const Tok& t, CompileSymbols::Role role = CompileSymbols::Read) {
    nav->use(indexSite(t), Navigation::get(nav->globals, t.sym), role);
  }
  int indexLocal(const Tok& t, std::string_view kind, std::string_view type = {}, int earlier = -1, std::string_view container = {}) {
    if (container.empty()) container = owner.empty() ? F->k.name : owner;
    int id = nav->declare(indexSite(t), t, kind, container, earlier);
    if (id >= 0 && earlier < 0 && !type.empty()) {
      auto& d = nav->out.declarations[id];
      d.signature = (kind == "var" ? "var " : "") + d.name + ": ";
      d.signature += type;
    }
    return id;
  }
  static int localSymbol(Fun* f, LV v) {
    if (v.k == LV_CAP) return localSymbol(f->parent, f->caps[v.a].second);
    if (v.k != LV_LOCAL) return -1;
    if (f->lambda && v.a == f->cloHome) return f->lambdaSymbol;
    for (auto it = f->locals.rbegin(); it != f->locals.rend(); ++it) if (it->home == v.a) return it->symbol;
    return -1;
  }
  int binding(LV v) {
    if (v.k == LV_GLOBAL || v.k == LV_SHARED) return Navigation::get(nav->globals, v.b);
    return localSymbol(F, v);
  }
  static int selfClass(Fun* f, LV v) {
    if (v.k == LV_CAP) return selfClass(f->parent, f->caps[v.a].second);
    return v.k == LV_LOCAL && f->cls >= 0 && v.a == f->selfHome ? f->cls : -1;
  }
  // Field types may be declared later in this file or in an importer. Record the
  // steps now and resolve them after parsing. Use separate alias nodes so invalidating
  // a reassigned local never changes the declared type of its source field.
  // field: a field's symbol; -1 the parent's own type (an alias); EACH what `for x in` the parent gives (a list<T>'s
  // T, a map<K, V>'s K); AT what parent[i] gives (T; V or nil); CALL the declared result of method `result` of the
  // parent's class (overrides declare the same types). result >= 0 without a parent: the declared result of that fn.
  enum { EACH = -2, AT = -3, CALL = -4 };
  struct TypePath { int parent, field; uint64_t type; int result = -1; };
  std::vector<TypePath> paths{{0, -1, 0}};
  int addPath(int parent, int field, uint64_t type = 0) {
    if (!parent && !type) return 0;
    paths.push_back({parent, field, type});
    return int(paths.size()) - 1;
  }
  Fun::Local* pathLocal(Fun* f, LV v) {
    if (v.k == LV_CAP) return pathLocal(f->parent, f->caps[v.a].second);
    if (v.k == LV_LOCAL)
      for (auto i = f->locals.rbegin(); i != f->locals.rend(); ++i) if (i->home == v.a) return &*i;
    return nullptr;
  }
  int valuePath(LV v) {
    if (tiering) return 0;
    if (v.path) return v.path;
    if (auto* local = pathLocal(F, v); local && local->path) return local->path;
    int cls = selfClass(F, v);
    return addPath(0, -1, cls >= 0 ? tyClass(cls) : varType(F, v).first);
  }
  void indexMember(LV receiver, const Tok& member, bool call, int path = 0) {
    int cls = selfClass(F, receiver);
    uint64_t ty = 0;
    int id = binding(receiver);
    if (receiver.k == LV_GLOBAL || receiver.k == LV_LOCAL || receiver.k == LV_CAP) {
      if (receiver.k == LV_GLOBAL)
        if (auto* g = findGlobal(receiver.b); g && g->kind == 'c') cls = receiver.b;
      if (cls < 0) ty = varType(F, receiver).first;
      if (!ty && id >= 0)
        if (auto p = nav->paramTypes.find(id); p != nav->paramTypes.end()) ty = p->second;
    }
    auto site = indexSite(member);
    // Constructor inference is local to its function/file.
    if (receiver.k == LV_CAP || (id >= 0 && receiver.k == LV_GLOBAL && nav->out.declarations[id].site.file != site.file)) id = -1;
    if (site.file >= 0 && (path || cls >= 0 || ty || id >= 0 || editorCheck->referenceQuery))
      nav->members.push_back({site, member.sym, cls, id, ty, call, false, useRole(cur), path});
  }
  int memberPath(LV receiver, const Tok& field, bool call) {
    int path = valuePath(receiver);
    if (nav) indexMember(receiver, field, call, path);
    if (tiering) return 0;
    At at{field.p, field.line, file, srcStart};
    if (selfClass(F, receiver) >= 0) {
      if (!call) fieldUses.push_back({selfClass(F, receiver), field.sym, at});
    } else if (path) {
      std::string name = receiver.k == LV_FIELD ? symName(receiver.b) : varName(receiver);
      typedUses.push_back({path, field.sym, at, call, name});
    }
    return call ? 0 : addPath(path, field.sym);
  }
  void finishIndex() {
    for (auto& u : nav->members) {
      int cls = u.cls;
      uint64_t inferred = u.type ? u.type : paths[u.path].type;
      if (cls < 0 && inferred) {
        uint64_t ty = resolveNow(inferred) & ~TY_OPT;
        if ((ty & 15) == TY_CLS) cls = int(ty >> 8);
      }
      if (cls < 0 && u.binding >= 0 && !u.type && !nav->reassigned.contains(u.binding))
        cls = Navigation::get(nav->constructors, u.binding);
      if (u.super) cls = Navigation::get(nav->supers, cls);
      auto lookup = [&](auto& table) {
        int c = cls;
        for (size_t depth = 0; c >= 0 && depth <= nav->supers.size(); depth++, c = Navigation::get(nav->supers, c)) {
          int id = Navigation::get(table, std::pair{c, u.name});
          if (id >= 0) return id;
        }
        return -1;
      };
      int id = u.call ? lookup(nav->methods) : lookup(nav->fields);
      if (id < 0 && u.role != CompileSymbols::Write) id = u.call ? lookup(nav->fields) : lookup(nav->methods);
      nav->use(u.site, id, u.role);
      if (editorCheck->referenceQuery && id < 0 && cls < 0) nav->out.unresolved.push_back({u.site, symName(u.name), u.role});
    }
    auto& uses = nav->out.occurrences;
    auto key = [](const auto& u) { return std::tuple(u.site, u.declaration, u.role); };
    std::sort(uses.begin(), uses.end(), [&](const auto& a, const auto& b) { return key(a) < key(b); });
    uses.erase(std::unique(uses.begin(), uses.end(), [&](const auto& a, const auto& b) { return key(a) == key(b); }), uses.end());
    // The compiler allows overlapping fields in a class hierarchy. Keep these pairs
    // so rename can refuse to introduce another overlap even when there are no uses.
    for (auto& [key, id] : nav->fields) {
      auto [cls, name] = key;
      for (size_t depth = 0; cls >= 0 && depth <= nav->supers.size(); depth++, cls = Navigation::get(nav->supers, cls)) {
        nav->collision(id, Navigation::get(nav->fields, std::pair{cls, name}));
        nav->collision(id, Navigation::get(nav->methods, std::pair{cls, name}));
      }
    }
    for (auto& [key, id] : nav->methods) {
      auto [cls, name] = key;
      cls = Navigation::get(nav->supers, cls);
      for (size_t depth = 0; cls >= 0 && depth <= nav->supers.size(); depth++, cls = Navigation::get(nav->supers, cls))
        nav->collision(id, Navigation::get(nav->fields, std::pair{cls, name}));
    }
    for (auto& [path, names] : aliases) {
      auto source = nav->sources.find(path);
      if (source == nav->sources.end()) continue;
      file = path;
      for (auto& [raw, _] : names) {
        int alias = Navigation::get(nav->aliases, std::pair{source->second.file, raw});
        int sym = globalSym(raw), global = Navigation::get(nav->globals, sym);
        nav->collision(alias, global);
        // Implicit package aliases and builtins have no indexed declaration.
        if (alias < 0 && global >= 0) {
          auto site = nav->out.declarations[global].site;
          nav->out.collisions.emplace_back(site, site);
        } else if (alias >= 0 && global < 0) {
          if (auto* g = findGlobal(sym); g && g->kind == 'n') {
            auto site = nav->out.declarations[alias].site;
            nav->out.collisions.emplace_back(site, site);
          }
        }
      }
    }
  }
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
  Tok topName(Tok t, bool asValue = false) {  // the top-level name t (just read) refers to; reads `.name` after a module alias
    const std::string* m = moduleAlias(t.sym);
    if (!m) return t.sym = globalSym(t.sym), t;
    if (nav) {
      auto site = indexSite(t);
      nav->use(site, Navigation::get(nav->aliases, std::pair{site.file, t.sym}));
    }
    auto root = pkgRootOf.find(*m);
    auto surface = surfaceOf.find(*m);
    bool pkg = root != pkgRootOf.end() || surface != surfaceOf.end();  // tier 2 has only the surface
    if (!accept('.')) failAt(t.p, t.line, "@not-a-value '%s' is a %s: use %s.name", name(t.sym), pkg ? "package" : "module", name(t.sym));
    Tok n = cur;
    n.sym = intern(*m + "." + symName(ident()));
    int raw = prev.sym;
    if (!pkg && !findGlobal(n.sym))  // a file imported plainly elsewhere first: its pub names are global
      for (auto& [ip, pre] : nsOf)
        if (pre == *m && !isModule[ip]) {
          std::string keep = file;
          file = ip;
          int g = qualify(raw);
          file = keep;
          if (findGlobal(g) && g < (int)pubSym.size() && pubSym[g]) return n.sym = g, n;
        }
    bool declared = declKind.empty() ? findGlobal(n.sym) != nullptr : (size_t)n.sym < declKind.size() && declKind[n.sym];  // tier 2, the REPL: as installed
    if (pkg) {
      if (surface != surfaceOf.end() && surface->second.count(raw) && declared) return n;
      std::string rf = root != pkgRootOf.end() ? ::base(root->second) : "its root file";
      if (declared)
        failAt(n.p, n.line, "@not-in-interface '%s' is pub inside package %s but not part of its interface: only what %s makes pub (or re-exports with pub import) is", name(raw), name(t.sym), rf.c_str());
      failAt(n.p, n.line, "@not-in-interface package %s has no '%s' in its interface (what %s makes pub, or re-exports with pub import)", name(t.sym), name(raw), rf.c_str());
    }
    if (!declared) {
      if (!asValue) failAt(n.p, n.line, "@undefined-name module '%s' has no '%s'", name(t.sym), name(prev.sym));
      if (!unavailableModules.contains(*m)) missingName(n, "@undefined-name module '%s' has no '%s'", name(t.sym), name(prev.sym));
      n.sym = -1;
      return n;
    }
    if ((size_t)n.sym >= pubSym.size() || !pubSym[n.sym]) failAt(n.p, n.line, "@private '%s' is private to module '%s' (mark it pub there to use it here)", name(prev.sym), name(t.sym));
    return n;
  }
  static const char* logicHint(const Tok& t) {
    if (t.t != T_ID) return nullptr;
    if (symName(t.sym) == "and") return "use '&&'";
    if (symName(t.sym) == "or") return "use '||'";
    return nullptr;
  }
  void undefined(const Tok& t) {
    const std::string& full = symName(t.sym);
    int s = intern(full.substr(full.rfind('.') + 1));  // as written: globalSym may have put a package's prefix on it
    if (auto it = privateIn.find(s); it != privateIn.end() && it->second != file && pkgOf(it->second) == pkgOf(file))
      failAt(t.p, t.line, "@private '%s' is private to %s (mark it pub there to use it here)", name(s), ::base(it->second).c_str());
    if (const std::string& P = pkgPrefix(file); !P.empty())
      if (Global* g = findGlobal(s); g && g->kind != 'n')
        failAt(t.p, t.line, "@undefined-name undefined name '%s' (package %s sees its own names, its dependencies' through their names, and builtins; not the program's)", name(s), pkgOf(file)->name.c_str());
    const auto& word = symName(s);
    const char* hint = logicHint(Tok{.t = T_ID, .sym = s});
    if (word == "null" || word == "None" || word == "undefined") hint = "use 'nil'";
    else if (word == "not") hint = "use '!'";
    else if (word == "elif") hint = "use 'else if'";
    else if (word == "let" || word == "const") hint = "declare a variable with 'var'";
    else if (word == "function" || word == "def") hint = "declare a function with 'fn'";
    if (hint) return missingName(t, "@undefined-name undefined name '%s': %s", name(s), hint);
    if (auto f = aliases.find(file); f != aliases.end())  // a pub name of a module or package this file imported with a name
      for (auto& [a, m] : f->second) {
        int q = intern(m + "." + word);
        bool pkg = pkgRootOf.count(m) || surfaceOf.count(m);
        if (findGlobal(q) && (pkg ? !surfaceOf.count(m) || surfaceOf[m].count(s) : (size_t)q < pubSym.size() && pubSym[q]))
          return missingName(t, "@undefined-name undefined name '%s': it is in %s, so write %s.%s", name(s), name(a), name(a), name(s));
      }
    missingName(t, "@undefined-name undefined name '%s'", name(s));
  }
  template<class... A> void missingName(const Tok& t, const char* fmt, A... args) {
    if (!testMissing) return diagnose(t.p, t.line, fmt, args...);
    if (!testMissing->empty()) return;
    const char* code = takeCode(fmt);
    char b[256];
    snprintf(b, sizeof b, fmt, args...);
    auto source = sourcePosition(t.p, t.line);
    const char* ls = source.at;
    while (ls > source.start && ls[-1] != '\n') ls--;
    *testMissing = std::string(b) + " [" + code + "] at " + ::base(file) + ":" +
                   std::to_string(source.line) + ":" + std::to_string(source.at - ls + 1);
  }
  int globalSym(int s) {  // the file's own names first, then its package's, then global (builtins only, in a package)
    if (auto it = nsOf.find(file); it != nsOf.end())
      if (int q = intern(it->second + "." + symName(s)); findGlobal(q)) return q;
    if (auto it = plainModules.find(file); it != plainModules.end())  // a module (imported `as` elsewhere) imported plainly here
      for (const std::string& ip : it->second)
        if (int q = intern(nsOf[ip] + "." + symName(s)); findGlobal(q) && q < (int)pubSym.size() && pubSym[q]) return q;
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
  bool prescanning = false;
  Tok lex(Lexer& l) {
    Tok t = l.next();
    if (prescanning && t.t == T_ERR) {
      lexAtEnd = !*l.p;
      diagnose(t.p, t.line, "@syntax %s", l.err);
      lexAtEnd = false;
    }
    return t;
  }
  void checkToken() {
    if (cur.t == T_ERR) lexAtEnd = !*lx.p, failAt(cur.p, cur.line, "@syntax %s", lx.err);
  }
  template<class... A> [[noreturn]] void fail(const char* fmt, A... args) { checkToken(); CState::fail(fmt, args...); }
  void next() {
    checkToken();
    prev = cur, prevEnd = lx.p, cur = lx.next();
    if (!collectErrors) checkToken();
  }
  Tok peek() { Lexer s = lx; Tok t = s.next(); return t.t == T_ERR ? Tok{} : t; }  // lex errors surface on next()
  bool peekIs(int t) { return peek().t == t; }
  bool at(int t) { return cur.t == t; }
  bool atKw(int k) { return cur.t == T_ID && cur.sym == k; }
  bool accept(int t) { return at(t) ? next(), true : false; }
  void expect(int t) {
    if (accept(t)) return;
    checkToken();
    if (const char* hint = logicHint(cur); hint && !cur.nl)
      fail("@syntax expected '%c'; %s", t, hint);
    fail("@syntax expected '%c'", t);
  }
  int ident() {
    checkToken();
    if (!at(T_ID) || cur.sym < NKW) fail("@syntax expected a name");
    return next(), prev.sym;
  }
  void konst(std::string_view s, int r = 0) { consts.push_back(box(newStr(s))), imm(r, consts.back()); }

  // ---- types: num str bool list map fn any, a class (geom.Shape for a module's) or a named type; a literal ("GET",
  // 3, true, nil); a shape ({name: str, age: num?}); list<T>, map<V>, map<K, V>; a union of them (A | B); `?` after
  // one allows nil. Compound ones are interned by their structure, so equal types have equal codes. ----
  struct Ty { uint64_t code = 0; std::string text; };  // code 0: none written
  Ty declaredType(std::string_view& written) {
    if (!accept(':')) return {};
    const char* start = nav ? tokStart(cur) : nullptr;
    Ty t = type();
    if (nav) written = {start, size_t(prevEnd - start)};
    return t;
  }
  std::unordered_map<int, uint64_t> typeDefs;  // this compile's `type Name = ...` definitions (committed to globals)
  static std::string codeKey(uint64_t c) { return std::to_string(c); }
  Ty type() {
    std::vector<Ty> alts{typeAtom()};
    while (at('|') && !cur.nl) next(), alts.push_back(typeAtom());
    if (alts.size() == 1) return alts[0];
    TypeDesc d;
    d.k = 'u';
    std::string key = "u", text;
    for (auto& a : alts) d.alts.push_back(a.code), key += "," + codeKey(a.code), text += (text.empty() ? "" : " | ") + a.text;
    return {typeIntern(key, d), text};
  }
  void closeAngle() {  // the '>' ending list<...> or map<...>; in list<list<num>>, '>>' is two of them
    if (at(T_SHR)) cur.t = '>', cur.p++;
    else expect('>');
  }
  Ty typeAtom() {
    Tok t = cur;
    Ty ty;
    auto lit = [&](TypeDesc d, const std::string& key) { d.k = 'l'; ty.code = typeIntern(key, d); };
    if (atKw(K_fn)) next(), ty.code = TY_FN;
    else if (accept('(')) {
      ty = type(), expect(')');
      ty.text = "(" + ty.text + ")";
    } else if (at(T_STR)) {
      TypeDesc d;
      d.lit = 's', d.s = std::string(cur.p, (size_t)cur.len);
      if (d.s.find('{') != std::string::npos || d.s.find('\\') != std::string::npos) fail("@unknown-type a literal type is plain text: no escapes or {");
      next(), lit(d, "ls" + d.s);
    } else if (at(T_NUM) || (at('-') && peekIs(T_NUM))) {
      bool neg = accept('-');
      TypeDesc d;
      d.lit = 'n', d.n = neg ? -cur.n : cur.n;
      next(), lit(d, "ln" + std::to_string(d.n));
    } else if (atKw(K_true) || atKw(K_false) || atKw(K_nil)) {
      TypeDesc d;
      d.lit = 'v', d.v = atKw(K_true) ? TRUE_ : atKw(K_false) ? FALSE_ : NIL;
      next(), lit(d, "lv" + std::to_string(d.v));
    } else if (accept('{')) {  // a shape: name: type, one per field (commas or newlines between)
      TypeDesc d;
      d.k = 's';
      std::string key = "s";
      while (!accept('}')) {
        if (at(T_EOF)) fail("@syntax expected '}'");
        int f = at(T_STR) ? (next(), intern(std::string(prev.p, (size_t)prev.len))) : at(T_ID) ? (next(), prev.sym) : (fail("@syntax expected a field name"), 0);
        for (auto& [g, _] : d.fields) if (g == f) failAt(prev.p, prev.line, "@duplicate field '%s' is listed twice", name(f));
        expect(':');
        Ty ft = type();
        d.fields.push_back({f, ft.code}), key += "," + std::to_string(f) + ":" + codeKey(ft.code);
        accept(',');
      }
      ty.code = typeIntern(key, d);
    } else {
      if (!at(T_ID) || cur.sym < NKW) fail("@unknown-type expected a type (num, str, bool, list, map, fn, any, a class, a named type, a literal, or a {shape})");
      next();
      static const char* const kinds[] = {"any", "num", "str", "bool", "list", "map"};
      for (int i = 0; i < 6; i++) if (symName(prev.sym) == kinds[i]) ty.code = TY_ANY + i;
      for (char et : {'d', 'f', 'u', 'i', 'b'}) if (symName(prev.sym) == bufName(et)) ty.code = TY_BUF | uint64_t(et) << 8;
      for (int n = 2; n <= 4; n++) if (symName(prev.sym) == "vec" + std::to_string(n) + "f") ty.code = TY_BUF | uint64_t('f') << 8 | uint64_t(n) << 16;
      if (symName(prev.sym) == "u32") {  // a whole number 0 <= n < 2^32: a gpu fn's u32
        TypeDesc d;
        d.k = 'w';
        ty.code = typeIntern("w", d);
      }
      for (int n = 2; n <= 4; n++)  // mat2x2f ...: a list of n columns, each a vecNf
        if (symName(prev.sym) == "mat" + std::to_string(n) + "x" + std::to_string(n) + "f") {
          TypeDesc d;
          d.k = 'X', d.n = n;
          ty.code = typeIntern("X" + std::to_string(n), d);
        }
      if ((ty.code == TY_LIST || ty.code == TY_MAP) && at('<') && !cur.nl) {  // list<T>, map<V>, map<K, V>
        bool list = ty.code == TY_LIST;
        next();
        TypeDesc d;
        Ty a = type(), b;
        if (!list && accept(',')) b = type();
        closeAngle();
        d.k = list ? 'L' : 'M', d.key = b.code ? a.code : TY_ANY, d.elem = b.code ? b.code : a.code;
        ty.code = typeIntern(std::string(list ? "L" : "M") + codeKey(d.key) + "," + codeKey(d.elem), d);
      } else if (!ty.code) {
        Tok n = topName(prev);
        Global* g = findGlobal(n.sym);
        if (!g || (g->kind != 'c' && g->kind != 't')) {
          diagnose(t.p, t.line, "@unknown-type unknown type '%s' (num, str, bool, list, map, fn, any, a class, or a named type)", std::string(t.p, prevEnd).c_str());
          if (at('?') && !cur.nl) next();
          return {TY_ANY, std::string(t.p, prevEnd)};
        }
        if (nav) indexGlobal(n);
        if (auto it = enumDefs.find(n.sym); g->kind == 'c' && it != enumDefs.end() && at('.') && !cur.nl) {
          next();
          n.sym = variantNamed(it->first, cur)->sym;
          if (nav) { Tok variant = cur; variant.sym = n.sym; indexGlobal(variant); }
          next();
        }
        ty.code = g->kind == 'c' ? tyClass(n.sym) : TY_NAMED | uint64_t(n.sym) << 8;
      }
    }
    ty.text = (ty.code & 15) == TY_CLS ? symName(int(ty.code >> 8)) : ty.text.empty() ? squashed(t.p, prevEnd) : ty.text;  // a module's class: bank.Account
    if (at('?') && !cur.nl) {
      if (ty.code == TY_ANY) fail("@unknown-type 'any' already includes nil");
      next(), ty.code |= TY_OPT, ty.text += "?";
    }
    return ty;
  }
  static std::string squashed(const char* a, const char* b) {  // source text on one line, runs of space as one
    std::string s;
    for (; a < b; a++) {
      if (*a == '/' && a + 1 < b && a[1] == '/') { while (a < b && *a != '\n') a++; continue; }
      bool sp = *a == ' ' || *a == '\t' || *a == '\n' || *a == '\r';
      if (sp) { if (!s.empty() && s.back() != ' ' && s.back() != '{' && s.back() != '<') s += ' '; }
      else {
        if ((*a == '}' || *a == '>') && !s.empty() && s.back() == ' ') s.pop_back();
        if (*a == '}' && !s.empty() && s.back() == ',') s.pop_back();
        s += *a;
      }
    }
    return s;
  }
  // What the compiler knows of a type now: a named one through this compile's definitions, then committed ones.
  uint64_t resolveNow(uint64_t t) {
    for (int n = 0; (t & 15) == TY_NAMED && n < 32; n++) {
      auto it = typeDefs.find(int(t >> 8));
      uint64_t d = it != typeDefs.end() ? it->second : findGlobal(int(t >> 8)) && findGlobal(int(t >> 8))->kind == 't' ? findGlobal(int(t >> 8))->type : 0;
      if (!d) return t;
      t = d | (t & TY_OPT);
    }
    return t;
  }
  bool resolveNamed(uint64_t t, int self) {  // is t (a definition of self) just self again, through other names?
    for (int n = 0; (t & 15) == TY_NAMED && n < 32; n++) {
      if (int(t >> 8) == self) return true;
      auto it = typeDefs.find(int(t >> 8));
      if (it == typeDefs.end()) return false;
      t = it->second;
    }
    return false;
  }
  bool hasNamed(uint64_t t, int depth = 0) {  // does checking t depend on a named type (not known for sure until commit)?
    if ((t & 15) == TY_NAMED) return true;
    const TypeDesc* d = (t & 15) == TY_EXT ? typeDesc(t) : nullptr;
    if (!d || depth > 16) return false;
    for (uint64_t a : d->alts) if (hasNamed(a, depth + 1)) return true;
    for (auto& [_, f] : d->fields) if (hasNamed(f, depth + 1)) return true;
    return (d->key && hasNamed(d->key, depth + 1)) || (d->elem && hasNamed(d->elem, depth + 1));
  }
  bool acceptsNow(uint64_t t, V v) { return hasNamed(t) ? typeAccepts(resolveNow(t), v) || hasNamed(resolveNow(t)) : typeAccepts(t, v); }
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
        const char* start = nav ? tokStart(cur) : nullptr;
        Ty t = type();
        if (nav)
          if (int id = Navigation::get(nav->globals, sym); id >= 0) {
            auto& d = nav->out.declarations[id];
            d.signature = "var " + d.name + ": ";
            d.signature.append(start, prevEnd);
          }
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
  std::string varName(const LV& v) {  // a variable's name, for messages
    if (v.k == LV_GLOBAL) return symName(v.b);
    if (v.k == LV_CAP) return symName(F->caps[v.a].first);
    for (auto i = F->locals.rbegin(); i != F->locals.rend(); ++i) if (i->home == v.a) return symName(i->sym);
    return "it";
  }
  void storeCheck(std::pair<uint64_t, int> t, bool num) {  // x0, about to be stored to a var of type t
    if (t.first && !(num && (t.first & 15) == TY_NUM)) typeCheck(t.first, t.second, t.first == TY_NUM);
  }

  // ---- effects: io clock random yield spawn chan, or an `effect` (geom.Log for a module's) ----
  int effectProbe = 0;
  std::unordered_map<int, uint64_t> checkedEffects;
  std::vector<std::string> checkedOps;
  int effSite(const std::string& fn, uint64_t mask, const std::string& text) {
    return editorCheck ? 0 : ::effSite(fn, mask, text);
  }
  uint64_t effectBit(int sym) {
    if (!editorCheck) return effBit(sym);
    auto [it, added] = checkedEffects.try_emplace(sym, 0);
    if (added) it->second = effBit(sym, &effectProbe);
    return it->second;
  }
  int operationId(const std::string& name) {
    if (!editorCheck) return effOpId(name);
    checkedOps.push_back(name + "()");
    return -(int)checkedOps.size();
  }
  const std::string& operationName(int op) { return op < 0 ? checkedOps[-op - 1] : effOpName(op); }
  uint64_t effectName(std::string& text) {
    Tok t = cur;
    if (atKw(K_yield)) return next(), text += "yield", EFF_YIELD;
    int s = ident();
    const std::string& n = symName(s);
    for (auto [b, bit] : {std::pair{"io", EFF_IO}, {"clock", EFF_CLOCK}, {"random", EFF_RANDOM}, {"spawn", EFF_SPAWN}, {"chan", EFF_CHAN}})
      if (n == b) return text += n, bit;
    Tok q = topName(prev);
    Global* g = findGlobal(q.sym);
    if (!g || g->kind != 'e') failAt(t.p, t.line, "@unknown-effect unknown effect '%s' (io, clock, random, yield, spawn, chan, or one declared with 'effect')", std::string(t.p, prevEnd).c_str());
    if (nav) indexGlobal(q);
    uint64_t b = effectBit(q.sym);
    if (!b) failAt(t.p, t.line, "@limit too many effects (a program can declare at most %d)", EFF_LEVEL - EFF_BUILTIN);
    effBitNames.try_emplace(b, t.p, prevEnd);
    return text += std::string(t.p, prevEnd), b;
  }
  const std::vector<std::pair<int, int>>* opsOf(int effSym) {
    if (auto it = effOps.find(effSym); it != effOps.end()) return &it->second;
    if (auto it = effectOps.find(effSym); it != effectOps.end()) return &it->second;
    return nullptr;
  }
  // E.op after the effect's name (just read, as t): the operation's id and arity
  std::pair<int, int> effectOp(const Tok& t) {
    if (!accept('.')) failAt(t.p, t.line, "@not-a-value '%s' is an effect: perform its operations as %s.name(...)", name(t.sym), name(t.sym));
    Tok ot = cur;
    int o = ident();
    if (auto* l = opsOf(t.sym))
      for (auto [s, a] : *l) if (s == o) {
        if (nav) nav->use(indexSite(ot), Navigation::get(nav->operations, std::pair{t.sym, o}));
        return {operationId(symName(t.sym) + "." + symName(o)), a};
      }
    failAt(ot.p, ot.line, "@undefined-name effect '%s' has no operation '%s'", name(t.sym), name(o));
  }
  void perform(const Tok& t) {  // E.op(args): a call that rt_prep sends to the nearest handler (see effPrep)
    Tok ot = cur;
    auto [op, arity] = effectOp(t);
    const std::string& what = operationName(op);  // "Log.log()"
    uint64_t bit = effectBit(t.sym);
    effBitNames.try_emplace(bit, what.substr(0, what.rfind('.')));
    effectUse(t, what, bit), effDid(op, bit);
    if (!at('(') || cur.nl) failAt(ot.p, ot.line, "@syntax expected '(': perform an operation by calling it");
    next();
    int base = slot();
    imm(0, editorCheck ? NIL : box(effOpFn(op))), st(0, base);
    int n = exprList(')');
    if (n != arity) return diagnose(ot.p, ot.line, "@arity '%s' expects %d arguments, got %d", operationName(op).c_str(), arity, n),
                          F->depth = base, imm(0, NIL);
    callWindow(base, n, 0, false);
  }
  // handle { body } with op(params) { ... } ...: each handler is compiled first, as a closure, so it is ready when
  // the body starts; then handle@begin([op, handler, ...]), the body, and handle@end where it ends normally.
  void handleStmt() {
    Mark body = mark();
    skipBlock();
    if (!atKw(S_with)) fail("@syntax expected 'with': handle { ... } with op(params) { ... }");
    int base = F->depth, n = 0;
    std::vector<int> answers;
    while (atKw(S_with) && peek().t == T_ID) {
      next();
      Tok t = cur;
      int op = 0, arity = -1;
      if (atKw(K_yield)) next(), op = OP_YIELD, arity = 1;
      else {
        ident();
        Tok q = topName(prev);
        Global* g = findGlobal(q.sym);
        if (g && g->kind == 'e') {
          if (nav) indexGlobal(q);
          std::tie(op, arity) = effectOp(q);
        }
        else if (g && g->kind == 'n' && (op = effBuiltinOp(q.sym))) arity = effOpArity(op);
        else failAt(t.p, t.line, "@unknown-effect '%s' is not an effect's operation (print, clock, sleep, random, run, yield, the graphics builtins, or Effect.name)", std::string(t.p, prevEnd).c_str());
      }
      imm(0, box((double)op)), st(0, slot()), answers.push_back(op);
      if (!at('(')) fail("@syntax expected '(' and the operation's parameters");
      Fn* h = lambda(t);
      if (h->arity - 1 != arity) diagnose(t.p, t.line, "@arity a handler for %s takes %d parameters, not %d", operationName(op).c_str(), arity, h->arity - 1);
      st(0, slot()), n += 2;
    }
    collect(base, n, H_LIST);
    callNative("handle@begin", true);
    Mark after = mark();
    rewind(body);
    size_t answered = handledOps.size();
    for (int op : answers) handledOps.push_back({F, op});
    F->handles++, block(), F->handles--;
    handledOps.resize(answered);
    if (F->live) callNative("handle@end", true);
    rewind(after);
  }

  void proveOperands(int op, bool lnum, bool rnum, int lh, int rh) {
    if (op <= O_MOD || op == O_EQ || op == O_NE) return;  // arithmetic also takes an f32s (vectors): no proof
    if (op >= O_BAND || rnum) prove(lh);
    if (op >= O_BAND || lnum) prove(rh);
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
    bool rnum = right(p, true, lh, op);
    int rh = rHome;
    binop(op, lnum, rnum), proveOperands(op, lnum, rnum, lh, rh);
    return (lnum && rnum && op <= O_MOD) || op >= O_BAND;
  }
  int rHome = NOH;  // the local right() loaded, if it was one
  // x1 = the right operand (x0 kept, if keep); returns whether it is known to be a number. lh: the local x0 was just
  // loaded from, if any. Then x0 need not be saved around the right operand: it is loaded again after (no expression
  // can assign a local), which also keeps it in its FP register for the arithmetic, when it lives in one.
  const char* literalEnd = nullptr;  // where the last [...] or {...} literal ended
  bool right(int p, bool keep = true, int lh = NOH, int op = -1) {
    rHome = NOH;
    if (int k = simpleOperand(p)) return k == 2;
    int s = slot();
    if (keep && lh != NOH) unload(0, lh);
    else if (keep) st(0, s);
    Tok first = cur;
    bool literal = at('[') || at('{');
    bool num = expr(p + 1);
    if ((op == O_EQ || op == O_NE) && literal && literalEnd == prevEnd) freshCompare(op, first);
    mv(1, 0), F->depth--, rHome = NOH;
    if (keep) lh != NOH ? ldl(0, lh) : ld(0, s);
    return num;
  }

  // ---- names ----
  LV resolve(Fun* f, int sym) {  // local, else captured from an enclosing function (lambdas), else LV_NONE
    for (auto i = f->locals.rbegin(); i != f->locals.rend(); ++i) if (i->sym == sym) return {LV_LOCAL, i->home, 0, false, i->num || proven(f, i->home)};
    if (sym == f->lambdaName) return {LV_LOCAL, f->cloHome, 0, true};
    for (size_t i = 0; i < f->caps.size(); i++) if (f->caps[i].first == sym) return {LV_CAP, (int)i, 0, f->caps[i].second.ro};
    if (!f->parent) return {LV_NONE};
    LV p = resolve(f->parent, sym);
    if (p.k == LV_NONE) return p;
    f->caps.push_back({sym, p});
    return {LV_CAP, (int)f->caps.size() - 1, 0, p.ro};
  }
  void load(LV& v) {
    switch (v.k) {
      case LV_LOCAL: ldl(0, v.a); break;
      case LV_GLOBAL: gmem(0, v.a); break;
      case LV_CAP: capLoad(v.a); break;
      case LV_SHARED: imm(0, box((double)v.a)), callNative("shared@get", true); break;
      case LV_FIELD: field(v.b, false); break;
      case LV_INDEX: mv(1, 0), ld(0, v.a), F->depth--, index(false); break;  // index in x0, object in slot a
    }
    v.k = LV_VAL;
  }

  // ---- calls: arguments go to consecutive slots (the "window") ----
  // keepLast: leave the last value in x0, not in a slot; vals: each argument's static type; origins: where each came from
  int exprList(int close, bool keepLast = false, std::vector<ArgVal>* vals = nullptr, std::vector<int>* origins = nullptr) {
    int n = 0;
    if (!accept(close)) {
      do {
        ArgVal a{0, false, cur.p, cur.line};
        Tok first = cur;
        a.num = expr(1, vals ? &a.path : nullptr), n++;
        if (vals) vals->push_back(a);
        if (origins && inferEffectsWanted) origins->push_back(argOrigin(first));
      } while (accept(',') && (st(0, slot()), true));
      if (!keepLast) st(0, slot());
      expect(close);
    }
    return n;
  }
  void pushCall(std::vector<int>* origins = nullptr) {  // x.push(v): appended inline when x is a list with room (listPush), else the method call
    int base = slot();
    st(0, base);
    int n = exprList(')', true, nullptr, origins);
    if (n == 1) return listPush(base);
    if (n) st(0, slot());
    callWindow(base, n, S_push, false);
  }
  void call(int sym, bool super, std::vector<int>* origins = nullptr, std::vector<ArgVal>* vals = nullptr) {
    int base = slot();
    st(0, base), callWindow(base, exprList(')', false, vals, origins), sym, super);
  }
  void printCall(const Tok& t, Global& g) {
    int base = slot();
    gmem(0, g.slot), st(0, base);
    int n = exprList(')');
    if (!n) return diagnose(t.p, t.line, "@arity 'print' expects at least 1 argument, got 0"), F->depth = base, imm(0, NIL);
    collect(base + 1, n, H_PRINTARGS), st(0, slot());
    callWindow(base, 1, 0, false);
  }
  static int vecSize(int sym) {  // vec2f, vec3f, vec4f: 2, 3, 4
    const std::string& n = symName(sym);
    return n.size() == 5 && n.compare(0, 3, "vec") == 0 && n[3] >= '2' && n[3] <= '4' && n[4] == 'f' ? n[3] - '0' : 0;
  }
  // vec3f(x, y, z) calls the native; any other count, like vec3f(0) or vec3f(v.xy, 1), goes through vec@of(size, args),
  // which spreads one number to every component, or joins numbers and vectors as WGSL's constructors do.
  void vecCall(Global& g, int size) {
    int base = slot();
    gmem(0, g.slot), st(0, base);
    int n = exprList(')');
    if (n == size) return callWindow(base, n, 0, false);
    collect(base + 1, n, H_LIST), st(0, slot());
    imm(0, box((double)size)), st(0, slot());
    gmem(0, findGlobal(intern("vec@of"))->slot), st(0, base), callWindow(base, 2, 0, false);
  }
  void construct(Global& g, int cls, const Tok& name) {  // C(args) for a top-level class C
    int base = slot();
    const char* at = name.p;
    int line = name.line;
    gmem(0, g.slot), st(0, base);
    std::vector<int> origins;
    std::vector<ArgVal> args;
    int n = exprList(')', false, tiering ? nullptr : &args, &origins);
    if (!tiering) callSites.push_back({-1, std::move(args), file, srcStart, cls, at, line});  // checked once every class is known
    constructCall(base, n);
    effRecord('c', cls, std::move(origins));
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
    std::vector<ArgVal> args;
    std::vector<int> origins;
    int base = F->depth, n = exprList(')', true, tiering ? nullptr : &args, &origins), a = arityOf(t.sym, g);
    effRecord('f', t.sym, std::move(origins));
    if (a < 0) diagnose(t.p, t.line, "@undefined-name '%s' is not defined", name(t.sym));
    else if (a != n) diagnose(t.p, t.line, "@arity '%s' expects %d arguments, got %d", name(t.sym), a, n);
    if (a != n) return F->depth = base, imm(0, NIL);
    if (n && !tiering) callSites.push_back({t.sym, std::move(args), file, srcStart});
    callDirect(base, n, g.stub);
  }

  // ---- literals ----
  // The escape after a backslash (p is at the character after it): appends what it stands for to out and returns its
  // last character. \u{HEX} is a Unicode code point, written as UTF-8. Any other escape is an error, so a typo
  // can't quietly become a letter.
  const char* escape(const char* p, const char* e, std::string& out, int line) {
    switch (*p) {
      case 'n': return out += '\n', p;
      case 't': return out += '\t', p;
      case 'r': return out += '\r', p;
      case '0': return out += '\0', p;
      case '"': case '\\': case '{': case '}': return out += *p, p;
      case 'u': {
        const char* q = p + 1;
        uint32_t cp = 0;
        int n = 0;
        auto hex = [](char c) { return dig(c) ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10 : -1; };
        if (q < e && *q == '{')
          for (q++; q < e && hex(*q) >= 0; q++, n++) cp = n < 7 ? cp * 16 + hex(*q) : cp;
        if (!n || q >= e || *q != '}') failAt(p - 1, line, "@syntax \\u needs a code point in hex between braces, as \\u{e9}");
        if (n > 6 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
          failAt(p - 1, line, "@syntax \\u{%.*s} isn't a Unicode code point (0 to 10FFFF, not D800 to DFFF)", n, p + 2);
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800) out += (char)(0xC0 | cp >> 6), out += (char)(0x80 | (cp & 63));
        else if (cp < 0x10000) out += (char)(0xE0 | cp >> 12), out += (char)(0x80 | (cp >> 6 & 63)), out += (char)(0x80 | (cp & 63));
        else out += (char)(0xF0 | cp >> 18), out += (char)(0x80 | (cp >> 12 & 63)), out += (char)(0x80 | (cp >> 6 & 63)), out += (char)(0x80 | (cp & 63));
        return q;
      }
    }
    if (*p > ' ' && *p < 0x7f) failAt(p - 1, line, "@syntax unknown escape '\\%c': use \\n \\t \\r \\0 \\\" \\\\ \\{ \\} or \\u{hex}", *p);
    failAt(p - 1, line, "@syntax unknown escape: a backslash goes before n t r 0 \" \\ { } or u{hex}");
  }
  void strLit(const Tok& t) {  // "text {expr} text": interpolation compiles to concatenation
    const char *p = t.p, *e = t.p + t.len;
    std::string lit;
    bool any = false;
    int line = t.line;
    auto flush = [&](bool force) {
      if (lit.empty() && !force) return;
      if (any) { int s = slot(); st(0, s), konst(lit), combine(s, O_ADD); }
      else konst(lit);
      any = true, lit.clear();
    };
    for (; p < e; p++) {
      if (*p == '\\' && p + 1 < e) {
        p = escape(p + 1, e, lit, line);
      } else if (*p == '{') {
        flush(true);  // always start from a string, so `"{x}"` is a string even when x is a number
        int s = slot();
        const char* start = p;
        st(0, s), p = interpolation(p + 1, e, line), combine(s, O_ADD);
        line += (int)std::count(start, p, '\n');
      } else {
        line += *p == '\n';
        lit += *p;
      }
    }
    flush(!any);
  }
  // Parse other source text in the middle of this one (interpolations, imports), then resume.
  struct Mark { Lexer lx; Tok cur, prev; const char *prevEnd, *srcStart; std::string file; };  // a place in the source
  Mark mark() { return {lx, cur, prev, prevEnd, srcStart, file}; }
  void rewind(const Mark& m) { lx = m.lx, cur = m.cur, prev = m.prev, prevEnd = m.prevEnd, srcStart = m.srcStart, file = m.file; }
  enum Boundary { Statement, Member, Top };
  bool startsItem(const Tok& t, Lexer after, Boundary where) {
    if (t.t != T_ID) return false;
    Tok n = after.next();
    if (where == Member)
      return t.sym == K_var || t.sym == K_fn || t.sym == S_invariant ||
             (t.sym == S_override && n.t == T_ID && n.sym == K_fn);
    if (t.sym == K_var || t.sym == K_if || t.sym == K_while || t.sym == K_for || t.sym == K_try ||
        t.sym == K_return || t.sym == K_throw || t.sym == K_break || t.sym == K_continue) return true;
    if (t.sym == K_fn) return where == Top || n.t == T_ID;
    if (t.sym == S_atomic || t.sym == S_handle) return n.t == '{';
    if (t.sym == S_parallel || t.sym == S_shared) return n.t == T_ID && (n.sym == K_for || n.sym == K_var);
    return where == Top && (t.sym == K_import || t.sym == K_class || t.sym == K_extern ||
                            (pubDecl(n) && t.sym == S_pub) || gpuFn(t, n) || gpuVar(t, n) ||
                            t.sym == S_enum || t.sym == S_effect || t.sym == S_type || t.sym == S_test);
  }
  // Replay the tokens from the boundary, including braces the failed parse had already consumed.
  // A missing ')' or ']' cannot keep recovery from finding a new statement on the next line.
  void synchronize(const Mark& start, const char* reached, Boundary where) {
    rewind(start);
    int braces = 0, brackets = 0;
    for (bool first = true; !at(T_EOF); first = false) {
      if (!braces && at('}') && where != Top) return;
      if (!first && !braces && tokStart(cur) >= reached) {
        if (at(';')) { prev = cur, prevEnd = lx.p, cur = lx.next(); return; }
        if ((!brackets || cur.nl) && startsItem(cur, lx, where)) return;
        if (cur.nl && !at('{') && !atKw(K_else) && !atKw(K_catch)) return;
      }
      if (at('{')) braces++;
      else if (at('}') && braces) braces--;
      else if (at('(') || at('[')) brackets++;
      else if ((at(')') || at(']')) && brackets) brackets--;
      prev = cur, prevEnd = lx.p, cur = lx.next();
    }
  }
  bool skipInvalid(Boundary where) {
    if (auto it = invalidDecls.find(cur.p); it != invalidDecls.end()) {
      synchronize(mark(), it->second, where);
      return true;
    }
    return false;
  }
  template<class Work> void item(Boundary where, Work work) {
    if (!collectErrors) return work();
    if (skipInvalid(where)) return;
    Mark start = mark();
    Fun* fun = F;
    // Keep the emitted prefix in place. The rest of Fun includes local homes, numeric facts and open scopes.
    Code& k = fun->k;
    Code prefix = std::move(k);
    Fun saved = *fun;
    k = std::move(prefix);
    size_t nc = k.c.size(), nr = k.rel.size(), nl = k.lines.size();
    size_t nw = k.wlocals.size(), nk = k.relKind.size(), nh = k.hints.size();
    struct LoopMark {
      Loop* loop;
      size_t brk, cont;
      explicit LoopMark(Loop* l) : loop(l), brk(l ? l->brk.size() : 0), cont(l ? l->cont.size() : 0) {}
      void restore() { if (loop) loop->brk.resize(brk), loop->cont.resize(cont); }
    } loop(fun->loop), retry(fun->retry);
#if __wasm__
    size_t tee = setF == fun && setAt < nc ? setAt : SIZE_MAX;
    Word oldOp = tee != SIZE_MAX ? k.c[tee] : 0;
#endif
    size_t np = pend.size(), nf = fieldUses.size(), nt = typedUses.size(), ne = effCalls.size(), nc2 = callSites.size(), nx = expects.size(), nho = handledOps.size();
    std::string o = owner, r = refKey, tr = testRefs;
    auto missing = testMissing;
    auto oldValues = olds;
    auto contract = pubContract;
    int pinned = pinLine;
    if (attempt([&] { checkToken(); work(); })) return;
    const char* reached = start.cur.p;
    if (srcStart == start.srcStart && lx.end == start.lx.end)
      reached = errorPos == prev.p ? tokStart(prev) : tokStart(cur);
    k.c.resize(nc), k.rel.resize(nr), k.lines.resize(nl), k.wlocals.resize(nw), k.relKind.resize(nk), k.hints.resize(nh);
#if __wasm__
    if (tee != SIZE_MAX) k.c[tee] = oldOp;
    setAt = SIZE_MAX, setF = nullptr;
    std::fill(std::begin(pa), std::end(pa), Arg{});
#else
    beginFun();
#endif
    saved.k = std::move(k);
    *fun = std::move(saved);
    F = fun, loop.restore(), retry.restore();
    pend.resize(np), fieldUses.resize(nf), typedUses.resize(nt), effCalls.resize(ne), callSites.resize(nc2), expects.resize(nx), handledOps.resize(nho);
    owner = std::move(o), refKey = std::move(r), testRefs = std::move(tr), testMissing = missing;
    olds = oldValues, pubContract = contract, pinLine = pinned, hasLit = lexAtEnd = false;
    synchronize(start, reached, where);
    if (where == Statement && start.cur.t == T_ID && (start.cur.sym == K_var || start.cur.sym == K_fn)) {
      Lexer look = start.lx;
      Tok name = look.next();
      if (name.t == T_ID && name.sym >= NKW &&
          std::none_of(F->locals.begin() + F->scopeAt, F->locals.end(), [&](auto& l) { return l.sym == name.sym; })) {
        int h = home();
        imm(0, NIL), stl(0, h), F->locals.push_back({name.sym, h});
        if (nav) F->locals.back().symbol = indexLocal(name, start.cur.sym == K_var ? "var" : "fn");
      }
    }
  }
  template <class Body> void detour(const std::string& f, const char* src, const char* at, int line, Body body, const char* end = nullptr) {
    Mark m = mark();
    file = f, srcStart = src, lx = Lexer{at, line}, lx.end = end, cur = {}, next();
    if (!attempt([&] { checkToken(); body(); })) {
      rewind(m);
#if __wasm__
      longjmp(*recovery, 1);
#else
      throw ParseError{};
#endif
    }
    rewind(m);
  }
  FormatSpec formatSpec(const char* start, const char* end, int line) {
    FormatSpec f;
    const char* p = start;
    auto alignment = [](char c) { return c == '<' || c == '>' || c == '^'; };
    auto bad = [&] {
      failAt(start, line, "@syntax invalid format spec '%.*s': expected [fill][< > ^][+][0][width][,][.precision][f e x X b o] (width up to %u, precision up to %u)",
             (int)(end - start), start, (unsigned)UINT16_MAX, (unsigned)FormatSpec::maxPrecision);
    };
    auto take = [&](char c) { return p < end && *p == c ? (++p, true) : false; };
    auto digits = [&](unsigned limit) {
      unsigned n = 0;
      while (p < end && dig(*p)) {
        n = n * 10 + (*p++ - '0');
        if (n > limit) bad();
      }
      return n;
    };
    if (end - p >= 2 && alignment(p[1])) {
      if (*p < ' ' || *p > '~' || *p == '{') bad();
      f.fill = *p++;
    }
    if (p < end && alignment(*p)) f.align = *p++;
    if (take('+')) f.flags |= FormatSpec::Sign;
    if (!f.align && p < end && *p == '0') f.flags |= FormatSpec::Zero;
    f.width = digits(UINT16_MAX);
    if (take(',')) f.flags |= FormatSpec::Group;
    if (take('.')) {
      if (p == end || !dig(*p)) bad();
      f.precision = digits(FormatSpec::maxPrecision);
    }
    if (p < end && *p && strchr("fexXbo", *p)) f.type = *p++;
    if (p != end) bad();
    if (f.type && strchr("xXbo", f.type) && (f.precision != FormatSpec::noPrecision || (f.flags & FormatSpec::Group)))
      failAt(start, line, "@syntax invalid format spec '%.*s': x, X, b and o allow alignment, sign and width",
             (int)(end - start), start);
    return f;
  }
  const char* interpolation(const char* s, const char* end, int line) {
    const char* close = nullptr;
    detour(file, srcStart, s, line, [&] {
      // Nested expressions consume their own colons. Only the suffix left by expr() is format text.
      expr();
      const char* marker = cur.p;
      const char* suffix = marker;
      bool debug = at('=');
      if (debug) {
        suffix++;
        while (suffix < end && strchr(" \t\r\n", *suffix)) suffix++;
      }
      if (suffix == end) failAt(s - 1, line, "@syntax this '{' starts code that never ends: close it with '}', or write \\{ for a brace");
      FormatSpec spec;
      bool formatted = *suffix == ':';
      if (formatted) {
        const char* start = suffix + 1;
        suffix = std::find(start, end, '}');
        if (suffix == end) failAt(s - 1, line, "@syntax this '{' starts code that never ends: close it with '}', or write \\{ for a brace");
        spec = formatSpec(start, suffix, cur.line + (int)std::count(marker, start, '\n'));
      }
      if (*suffix != '}') failAt(suffix, cur.line, "@syntax expected '}' to end the code in this string, or write \\{ for a brace");
      if (debug || formatted) {
        imm(1, std::bit_cast<V>(spec));
        if (debug) konst(std::string(s, marker) + " = ", 2);
        else imm(2, NIL);
        helper(H_FORMAT);
      }
      close = suffix;
    }, end);
    return close;
  }
  void collect(int base, int n, int h) { addsp(0, base), imm(1, n), helper(h), F->depth = base; }  // list/map from slots
  void mapLit() {  // {key: value, name: value}; a bare name before ':' is a string key, keywords too ({class: "x"}),
    int base = F->depth, n = 0;  // except the ones that are values (true false nil self)
    if (!accept('}')) {
      do {
        bool value = atKw(K_true) || atKw(K_false) || atKw(K_nil) || atKw(K_self);
        if (at(T_ID) && !value && peekIs(':')) next(), konst(symName(prev.sym));
        else expr();
        st(0, slot()), expect(':'), expr(), st(0, slot()), n++;
      } while (accept(','));
      expect('}');
    }
    collect(base, n, H_MAP);
  }
  Fn* lambda(const Tok& t, bool arrow = false, int sym = -1, int parVar = -1, int binding = -1) {  // fn(params) { body }: captures enclosing locals by value into the closure
    if (tiering) compileFail("tier 2 does not handle lambdas", false);
    Fun f;
    f.parent = F, f.lambda = true, f.arrow = arrow, f.lambdaName = sym, f.parVar = parVar;
    if (nav) f.lambdaSymbol = binding;
    Fn* fn = make<Fn>();
    consts.push_back(box(fn));
    fn->code = parserStub(), fn->name = sym >= 0 ? sym : intern((parVar >= 0 ? "parallel@" : "fn@") + std::to_string(t.line));
    fn->arity = function(f, -1, false, symName(fn->name), (u32*)fn->code, fn);
    parWrites = std::move(f.parWrites);
    int base = F->depth, n = (int)f.caps.size();
    for (auto& [s, src] : f.caps) { LV v = src; load(v), st(0, slot()); }
    closure(fn, base, n);
    lamStart = t.p, lamEnd = prevEnd;
    return fn;
  }

  // parallel for i in a..b { body }: parallel@(a, b, body, writes) runs body(i) for each i on isolates of the pool
  // and returns when all have (isolate.cpp). The body is a lambda of i. It may not assign a variable from outside
  // it (its result would depend on which isolate ran which i); `writes` lists the outside variables it writes as
  // name[...] = (the buffers each iteration may reach only at [i]), as [code, name, ...] (see Fun::parWrites).
  std::vector<std::pair<int, std::string>> parWrites;
  void parallelFor() {
    Tok kw = prev;
    if (F->atomics) failAt(kw.p, kw.line, "@atomic parallel for inside atomic { }: its isolates can't be undone (run it after the block)");
    next();  // for
    int nm = ident();
    int symbol = nav ? indexLocal(prev, "var") : -1;
    if (!atKw(K_in)) fail("@syntax expected 'in'");
    next();
    int base = slot(), lo = slot(), hi = slot(), fs = slot(), ws = slot();
    if (!expr()) helper(H_NUMCHK);
    st(0, lo);
    if (!accept(T_DOTDOT)) fail("@syntax parallel for runs over a range: parallel for i in a..b { }");
    if (!expr()) helper(H_NUMCHK);
    st(0, hi);
    if (!at('{')) fail("@syntax expected '{' (the body of the parallel for)");
    lambda(kw, false, -1, nm, symbol), st(0, fs);
    int lb = F->depth;
    for (auto& [code, name] : parWrites) imm(0, box((double)code)), st(0, slot()), konst(name), st(0, slot());
    collect(lb, (int)parWrites.size() * 2, H_LIST), st(0, ws);
    gmem(0, findGlobal(intern("parallel@"))->slot), st(0, base), callWindow(base, 4, 0, false);
    F->depth = base;
  }

  // spawn f(args): spawn@(f, [args]) starts an isolate running f(args) and gives its task (isolate.cpp). f is a
  // function value: a name, a field (`spawn jobs.run(x)` reads the field run, it does not call a method), or a lambda.
  void spawnExpr() {
    int base = slot(), fs = slot();
    LV spawned{LV_VAL};  // what it runs: a lambda made here (LV_VAL), a name, or a field
    if (atKw(K_fn)) next(), lambda(prev);
    else {
      Tok t = cur;
      int s = ident();
      LV v = resolve(F, s);
      if (v.k == LV_NONE) {
        t.sym = s, t = topName(t, true);
        if (Global* g = findGlobal(t.sym)) {
          v = {LV_GLOBAL, g->slot, t.sym, true};
          if (nav) indexGlobal(t);
        }
        else {
          if (t.sym >= 0) undefined(t);
          imm(0, NIL), v = {LV_BAD};
        }
      } else if (nav) nav->use(indexSite(t), binding(v));
      while (accept('.')) {
        int f = ident();
        int path = memberPath(v, prev, false);
        load(v), v = {LV_FIELD, 0, f}, v.path = path;
      }
      spawned = v.k == LV_VAL ? LV{LV_BAD} : v;
      load(v);
    }
    st(0, fs);
    if (!at('(') || cur.nl) fail("@syntax expected the arguments of the function to spawn: spawn f(args)");
    next();
    std::vector<int> origins;
    int lb = F->depth, n = exprList(')', false, nullptr, &origins);
    effDid(INT_MIN, EFF_SPAWN), effSpawned(spawned, std::move(origins));  // the isolate may do what this one may
    collect(lb, n, H_LIST), st(0, slot());
    gmem(0, findGlobal(intern("spawn@"))->slot), st(0, base), callWindow(base, 2, 0, false);
  }

  // ---- enums ----
  int enumOf(int cls) {  // the enum cls is or is a variant of, else -1
    if (enumDefs.count(cls)) return cls;
    for (auto& [s, e] : enumDefs) for (auto& v : e.vs) if (v.sym == cls) return s;
    return -1;
  }
  const VariantDef* variantNamed(int esym, const Tok& t) {  // t: a variant's name, as written after Shape.
    EnumDef& e = enumDefs.at(esym);
    if (t.t == T_ID) for (auto& v : e.vs) if (v.raw == t.sym) return &v;
    std::string all;
    for (auto& v : e.vs) add(all, symName(v.raw));
    failAt(t.p, t.line, "@undefined-name enum %s has no variant '%s' (it has %s)", name(esym), std::string(t.p, t.t == T_ID ? symName(t.sym).size() : 1).c_str(), all.c_str());
  }
  // Shape.Circle(args), after `Shape.`: a new instance whose fields are the arguments, set in order. A variant with
  // no fields is one value (Shape.Empty), made on first use and kept in a hidden global.
  int variantValue(const Tok& et) {  // returns the variant's class
    const VariantDef& vd = *variantNamed(et.sym, cur);
    Tok vt = cur;
    if (nav) { Tok indexed = vt; indexed.sym = vd.sym; indexGlobal(indexed); }
    next();
    Global& g = global(vd.sym);
    int n = (int)vd.fields.size();
    if (!n) {
      if (at('(') && !cur.nl) failAt(vt.p, vt.line, "@arity %s has no fields: write it without parentheses", name(vd.sym));
      Global& one = global(intern(symName(vd.sym) + "@one"));
      gmem(0, one.slot);
      Cond no = truthyCond();
      thenBegin(no);
      int e = elseBegin(no, true);
      int base = slot();
      gmem(0, g.slot), st(0, base), constructCall(base, 0), gmem(0, one.slot, true);
      ifEnd(e);
      gmem(0, one.slot);
      return vd.sym;
    }
    std::string fs;
    for (int f : vd.fields) add(fs, symName(f));
    if (!at('(') || cur.nl) failAt(vt.p, vt.line, "@arity %s has fields: make one with %s(%s)", name(vd.sym), name(vd.sym), fs.c_str());
    next();
    int base = slot();
    gmem(0, g.slot), st(0, base), constructCall(base, 0);
    int o = slot(), k = 0;
    st(0, o);
    if (!accept(')')) {
      do {
        expr();
        if (k < n) mv(2, 0), ld(0, o), field(vd.fields[k], true);
        k++;
      } while (accept(','));
      expect(')');
    }
    if (k != n) diagnose(vt.p, vt.line, "@arity %s expects %d arguments (%s), got %d", name(vd.sym), n, fs.c_str(), k);
    ld(0, o), F->depth = base;
    return vd.sym;
  }
  // match value { Circle(r) { ... } Rect(w, _) { ... } Empty { ... } else { ... } }: each arm tests the value's
  // variant (is@) and binds its fields in order (_ skips one); arms nest like else-if. Without else, the arms must
  // cover every variant of the enum (a compile error names those missing), and a value that is not one of them raises.
  bool isMatch() {
    if (resolve(F, S_match).k != LV_NONE || findGlobal(globalSym(S_match))) return false;
    Tok n = peek();
    return !n.nl && n.t != '=' && n.t != '.' && n.t != T_EOF && n.t != '{' && !(n.t >= T_ADDEQ && n.t <= T_DIVEQ);
  }
  struct MatchAt { Tok at; int subject, esym = -1; std::vector<bool> seen; bool nums = false; std::vector<std::pair<double, double>> taken; };
  void callRt2(const char* nm, int subject, int cls) {  // nm(subject, the class cls)
    int base = slot();
    ldl(0, subject), st(0, slot()), gmem(0, global(cls).slot), st(0, slot());
    gmem(0, findGlobal(intern(nm))->slot), st(0, base), callWindow(base, 2, 0, false);
  }
  void matchStmt() {
    MatchAt m{prev, 0};
    expr();
    scope([&] {
      m.subject = home();
      stl(0, m.subject), F->locals.push_back({-1, m.subject});
      expect('{');
      matchArms(m);
    });
  }
  void matchArms(MatchAt& m) {
    if (bool end = accept('}'); end || (atKw(K_else) && (m.esym >= 0 || m.nums))) {
      if (!end) {
        next(), block();
        if (!accept('}')) fail("@syntax the else arm is the last arm of a match: expected '}'");
        return;
      }
      if (m.nums) return;  // no arm holds the number: nothing happens
      if (m.esym < 0) failAt(m.at.p, m.at.line, "@syntax a match needs arms: Variant(fields) { ... }");
      std::string miss;
      for (size_t i = 0; i < m.seen.size(); i++) if (!m.seen[i]) add(miss, symName(enumDefs[m.esym].vs[i].raw));
      if (!miss.empty()) failAt(m.at.p, m.at.line, "@match match on %s misses %s: add an arm for each, or else { }", name(m.esym), miss.c_str());
      callRt2("match@fail", m.subject, m.esym), F->live = false;
      return;
    }
    if (m.nums || (m.esym < 0 && (at(T_NUM) || (at('-') && peek().t == T_NUM)))) return numArms(m);
    Tok pt = cur;
    int esym = -1;
    if (at(T_ID) && peekIs('.')) {  // Shape.Circle, geom.Shape.Circle
      next();
      Tok e = topName(prev);
      if (!enumDefs.count(e.sym)) failAt(pt.p, pt.line, "@match '%s' is not an enum", std::string(pt.p, prevEnd).c_str());
      if (nav) indexGlobal(e);
      esym = e.sym, expect('.');
    } else if (at(T_ID) && cur.sym >= NKW) {
      if (m.esym >= 0) esym = m.esym;
      else {
        std::string all;
        for (auto& [s, e] : enumDefs)
          if (globalSym(e.raw) == s || (s < (int)pubSym.size() && pubSym[s]))
            for (auto& v : e.vs) if (v.raw == cur.sym) esym = s, add(all, symName(s));
        if (esym < 0) fail("@match '%s' is not a variant of any enum here", name(cur.sym));
        if (all.find(',') != std::string::npos) fail("@match '%s' is a variant of several enums (%s): write which, Enum.%s", name(cur.sym), all.c_str(), name(cur.sym));
      }
    } else fail("@syntax expected an arm: a variant (with names for its fields), or else");
    if (m.esym >= 0 && esym != m.esym) failAt(pt.p, pt.line, "@match this match is on %s, and '%s' is not one of its variants", name(m.esym), std::string(pt.p, cur.p + symName(cur.sym).size()).c_str());
    if (m.esym < 0) m.esym = esym, m.seen.assign(enumDefs[esym].vs.size(), false);
    if (traceOn && !tiering && !editorCheck) traceRef(!refKey.empty() ? refKey : !owner.empty() ? owner : testRefs, esym);  // the arms depend on its variants
    const VariantDef& vd = *variantNamed(esym, cur);
    if (nav) { Tok variant = cur; variant.sym = vd.sym; indexGlobal(variant); }
    size_t vi = &vd - enumDefs[esym].vs.data();
    if (m.seen[vi]) failAt(pt.p, pt.line, "@duplicate this match already has an arm for %s", name(vd.raw));
    m.seen[vi] = true;
    next();
    std::vector<int> binds;
    std::vector<Tok> bindTokens;
    if (at('(') && !cur.nl) {
      for (next(); !accept(')');) {
        binds.push_back(ident());
        if (nav) bindTokens.push_back(prev);
        if (!at(')')) expect(',');
      }
      if (binds.size() != vd.fields.size()) {
        std::string fs;
        for (int f : vd.fields) add(fs, symName(f));
        failAt(pt.p, pt.line, "@arity %s has %d fields (%s), and the arm names %d", name(vd.sym), (int)vd.fields.size(), fs.c_str(), (int)binds.size());
      }
    }
    callRt2("is@", m.subject, vd.sym);
    Cond no = truthyCond();
    thenBegin(no);
    auto f0 = F->facts;
    scope([&] {
      static const int skip = intern("_");
      for (size_t i = 0; i < binds.size(); i++) {
        if (binds[i] == skip) continue;
        ldl(0, m.subject), field(vd.fields[i], false);
        int h = home();
        stl(0, h), F->locals.push_back({binds[i], h});
        // The arm runs only on this variant, so the name holds its field: the field's declared type, if any (until assigned).
        if (!tiering) F->locals.back().path = addPath(addPath(addPath(0, -1, tyClass(vd.sym)), vd.fields[i]), -1);
        if (nav) {
          int id = F->locals.back().symbol = indexLocal(bindTokens[i], "var");
          for (auto old = F->locals.begin() + F->scopeAt; old != F->locals.end() - 1; ++old)
            if (old->sym == binds[i]) nav->collision(old->symbol, id);
        }
      }
      block();
    });
    auto f1 = F->facts;
    bool l1 = F->live;
    int e = elseBegin(no, true);
    F->facts = f0, F->live = true;
    matchArms(m);
    ifEnd(e);
    if (!l1) return;
    if (!F->live) return (void)(F->facts = f1, F->live = true);
    F->facts &= f1;
  }
  // match n { 0 { } 1, 2 { } 4..8 { } else { } }: an arm holds numbers and ranges (a..b: from a up to, not including,
  // b), and runs when the value is == one of them; arms nest like else-if. No arm holding it runs nothing.
  void numArms(MatchAt& m) {
    m.nums = true;
    auto number = [&] {
      bool neg = accept('-');
      if (!at(T_NUM)) fail("@syntax a match on numbers has arms of numbers (1), several (1, 2) and ranges (1..4), then else");
      next();
      return neg ? -prev.n : prev.n;
    };
    std::vector<std::pair<double, double>> vs;  // [lo, hi); a single number is [n, n]
    do {
      Tok vt = cur;
      double a = number(), b = a;
      if (accept(T_DOTDOT)) {
        b = number();
        if (!(b > a)) failAt(vt.p, vt.line, "@match %g..%g holds nothing: a range a..b runs from a up to, not including, b", a, b);
      }
      for (auto [lo, hi] : m.taken)
        if (lo == hi ? (a == b ? a == lo : lo >= a && lo < b) : (a == b ? a >= lo && a < hi : a < hi && lo < b))
          failAt(vt.p, vt.line, "@duplicate an earlier arm of this match already holds %s", std::string(vt.p, prevEnd).c_str());
      vs.push_back({a, b}), m.taken.push_back({a, b});
    } while (accept(','));
    std::vector<int> ors;
    for (size_t i = 0; i < vs.size(); i++) {  // the value == a || (lo <= the value && the value < hi) || ...
      if (i) ors.push_back(shortBegin(false));
      auto [a, b] = vs[i];
      ldl(0, m.subject), imm(1, box(a)), binop(a == b ? O_EQ : O_GE);
      if (a != b) {
        int j = shortBegin(true);
        ldl(0, m.subject), imm(1, box(b)), binop(O_LT), shortEnd(j);
      }
    }
    for (size_t i = ors.size(); i-- > 0;) shortEnd(ors[i]);
    Cond no = truthyCond();
    thenBegin(no);
    auto f0 = F->facts;
    scope([&] { block(); });
    auto f1 = F->facts;
    bool l1 = F->live;
    int e = elseBegin(no, true);
    F->facts = f0, F->live = true;
    matchArms(m);
    ifEnd(e);
    if (!l1) return;
    if (!F->live) return (void)(F->facts = f1, F->live = true);
    F->facts &= f1;
  }

  // ---- expressions: result in x0 ----
  LV primary() {
    next();
    Tok t = prev;
    const char* start = t.p;
    switch (t.t) {
      case T_NUM: return imm(0, box(t.n)), NUM;
      case T_STR: return strLit(t), typedValue(TY_STR);
      case '(': { LV v; v.num = expr(1, &v.path); return expect(')'), v; }
      case '{': return mapLit(), literalEnd = prevEnd, typedValue(TY_MAP);
      case '[': { int base = F->depth, n = exprList(']'); return collect(base, n, H_LIST), literalEnd = prevEnd, typedValue(TY_LIST); }
      case T_ID: break;
      default: failAt(t.p, t.line, t.t == T_EOF ? "@syntax unexpected end of file" : "@syntax unexpected '%c'", t.t);
    }
    switch (t.sym) {
      case K_true: return imm(0, TRUE_), typedValue(TY_BOOL);
      case K_false: return imm(0, FALSE_), typedValue(TY_BOOL);
      case K_nil: return imm(0, NIL), typedValue(TY_NILLIT);
      case K_fn: return lambda(t), typedValue(TY_FN);
      case K_yield:  // yield [value]: suspend this fiber; evaluates to the value the next resume sends
        if (F->atomics) failAt(t.p, t.line, "@atomic 'yield' inside atomic { }: other code would run while it is half done (yield after it ends)");
        at('}') || at(')') || at(']') || at(',') || at(T_EOF) || cur.nl ? imm(0, NIL) : (void)expr();
        return effDid(OP_YIELD, EFF_YIELD), helper(H_YIELD), LV{};
      case K_self: {
        LV v = resolve(F, K_self);
        if (v.k == LV_NONE) failAt(t.p, t.line, "@misplaced 'self' outside a method");
        return v.ro = true, v;
      }
      case K_super: {
        if (F->cls < 0) failAt(t.p, t.line, "@misplaced 'super' outside a method");
        expect('.');
        int s = ident();
        if (nav) nav->members.push_back({indexSite(prev), s, F->cls, -1, 0, true, true, CompileSymbols::Read});
        std::vector<int> origins;
        expect('('), ldl(0, F->selfHome), call(s, true, &origins);
        effRecord('s', s, std::move(origins), F->cls);
        return {};
      }
    }
    if (t.sym < NKW) failAt(t.p, t.line, "@syntax unexpected '%s'", name(t.sym));
    if (t.sym == S_spawn && at(T_ID) && !cur.nl && resolve(F, S_spawn).k == LV_NONE) return spawnExpr(), LV{};
    if (t.sym == S_old && olds && at('(') && !cur.nl) {  // in ensures: the value it had on entry (computed then)
      auto it = olds->find(t.p);
      if (it == olds->end()) failAt(t.p, t.line, "@misplaced old() is a value as it was on entry: it belongs in an ensures clause");
      for (int d = 0; next(), d += (prev.t == '(') - (prev.t == ')'), d > 0;) if (at(T_EOF)) fail("@syntax expected ')'");
      return ldl(0, it->second), LV{};
    }
    if (LV v = resolve(F, t.sym); v.k != LV_NONE) {
      if (nav) nav->use(indexSite(t), binding(v), useRole(cur));
      if (inferEffectsWanted && !tiering) lastName.p = t.p, lastName.origin = valueOrigin(v, &lastName.sym);
      return v;
    }
    t = topName(t, true);
    if (t.sym < 0) return imm(0, NIL), LV{LV_BAD};
    Global* g = findGlobal(t.sym);
    if (!g && t.sym == S_todo && at('(') && !cur.nl) {  // todo(): a body not written yet (fut test: pending)
      next(), expect(')');
      Fun* o = F;
      while (o->lambda && o->parent) o = o->parent;
      konst(display(o->k.name) + " is not implemented yet"), helper(H_THROW), F->live = false;
      return {};
    }
    if (!g) return undefined(t), imm(0, NIL), LV{LV_BAD};
    if (traceOn && !tiering && !editorCheck) traceRef(!refKey.empty() ? refKey : !owner.empty() ? owner : testRefs, t.sym);
    if (pubContract && g->kind != 'n' && !(t.sym < (int)pubSym.size() && pubSym[t.sym]))  // the .api shows the clause, not the name
      failAt(t.p, t.line, "@private %s can only use what its readers can see, and '%s' is private: make it pub, or write the condition without it",
             pubContract, name(prev.sym));
    if (nav) indexGlobal(t, useRole(cur));
    if (g->kind == 'n' && at('(') && !cur.nl)
      if (int op = effBuiltinOp(t.sym)) effectUse(t, effOpName(op), effOpEffect(op)), effDid(op, effOpEffect(op));
    if (static const int fiber = intern("fiber"); g->kind == 'n' && t.sym == fiber) fibers = true;
    if (g->kind == 'n' && at('(') && !cur.nl && symName(t.sym) == "print") return next(), printCall(t, *g), LV{};
    if (g->kind == 'n' && at('(') && !cur.nl)
      if (int size = vecSize(t.sym)) return next(), vecCall(*g, size), LV{};
    if (g->kind == 'f' && at('(') && !cur.nl && effectsChecked())
      effCalls.push_back({t.sym, F->effMask, F->effWho, F->effText, At{t.p, t.line, file, srcStart}});
    if (g->kind == 'f' && at('(') && !cur.nl) return next(), directCall(t, *g), t.sym == F->self || returnsNum(t.sym) ? NUM : resultOf(t.sym);
    if (g->kind == 'c' && enumDefs.count(t.sym)) {
      if (at('.') && !cur.nl) return next(), constructed(variantValue(t));
      if (at('(') && !cur.nl) failAt(t.p, t.line, "@class %s is an enum: make one of its variants, %s.%s", name(t.sym), name(t.sym), name(enumDefs[t.sym].vs.empty() ? t.sym : enumDefs[t.sym].vs[0].raw));
    }
    if (g->kind == 'c' && at('(') && !cur.nl) {
      next(), construct(*g, t.sym, t);
      if (nav) nav->ctorStart = start, nav->ctorEnd = prevEnd, nav->ctorClass = t.sym;
      return constructed(t.sym);
    }
    if (g->kind == 'e') return perform(t), LV{};
    if (g->kind == 'f') effRecord('f', t.sym, {}), lastName = {t.p, ARG_SAFE, -1};  // a fn as a value: whoever gets it may call it
    if (g->kind == 't') failAt(t.p, t.line, "@not-a-value '%s' is a type: use it after ':' (x: %s)", name(t.sym), name(t.sym));
    if (g->kind == 's') return {LV_SHARED, g->cell, t.sym};
    return {LV_GLOBAL, g->slot, t.sym, g->kind != 'v' || gpuVarSyms.count(t.sym) > 0};  // b: its symbol, for its type
  }
  // Inside a function that declares effects, outside its handle blocks, exactly those are allowed (its entry narrows
  // them to the declared set), so an operation of any other effect, or a call of a fn that declares any other, raises
  // whenever it runs: a compile error. Not in tier 2 (its first compile reported it), nor where a handler may answer.
  bool effectsChecked() { return F->effDecl && !F->handles && !tiering; }
  std::string effectNames(uint64_t mask) {
    static const char* builtin[EFF_BUILTIN] = {"io", "clock", "random", "yield", "spawn", "chan"};
    std::string s;
    for (int i = 0; i < EFF_LEVEL; i++)
      if (mask >> i & 1) add(s, i < EFF_BUILTIN ? builtin[i] : effBitNames.count(1ull << i) ? effBitNames[1ull << i] : "an effect");
    return s;
  }
  static std::string declaresText(uint64_t mask, const std::string& text) { return mask ? "it declares " + text : "it declares no effects"; }
  void effectUse(const Tok& t, const std::string& what, uint64_t effect) {  // performing an operation of effect
    if (!effectsChecked() || !effect || (effect & F->effMask)) return;
    diagnose(t.p, t.line, "@effect %s performs %s, which %s does not declare (%s)", what.c_str(), effectNames(effect).c_str(), F->effWho.c_str(),
             declaresText(F->effMask, F->effText).c_str());
  }
  // ---- what each function does (see EffFn) ----
  EffFn* effFn() {  // the named function the code being compiled belongs to (a lambda's: the one it is in)
    if (tiering || !inferEffectsWanted) return nullptr;
    Fun* o = F;
    while (o->lambda && o->parent) o = o->parent;
    if (o->lambda || o->test || o->top) return nullptr;
    auto it = effFns.find(o->k.name);
    return it == effFns.end() ? nullptr : &it->second;
  }
  void effDid(int op, uint64_t effect) {  // performing op, of effect, here
    EffFn* e = effFn();
    if (!e) return;
    for (auto& [f, o] : handledOps) if (f == F && o == op) return;  // a handler around it, in this function, answers it
    e->ops |= effect;
  }
  void effRecord(char kind, int sym, std::vector<int> args, int cls = -1, int path = 0) {
    if (EffFn* e = effFn()) e->sites.push_back({kind, sym, cls, path, std::move(args)});
  }
  void effAssigned(int sym) { if (EffFn* e = effFn()) e->assigned.insert(sym); }
  // Where the value in v came from: one of the named function's own parameters (its index), a lambda made in the
  // function (ARG_SAFE; sym: its name, to check it isn't assigned again), or anywhere else (ARG_OTHER).
  int valueOrigin(LV v, int* sym = nullptr) {
    if (sym) *sym = -1;
    Fun* f = F;
    while (v.k == LV_CAP && f->lambda && f->parent) v = f->caps[v.a].second, f = f->parent;
    if (v.k != LV_LOCAL) return ARG_OTHER;
    if (f->lambda && v.a == f->cloHome) return ARG_SAFE;  // a named lambda, calling itself
    for (auto i = f->locals.rbegin(); i != f->locals.rend(); ++i)
      if (i->home == v.a) {
        if (sym) *sym = i->sym;
        if (i->made) return ARG_SAFE;
        break;
      }
    if (f->lambda || f->test || f->top) return ARG_OTHER;
    for (size_t j = 0; j < f->paramHomes.size(); j++) if (f->paramHomes[j] == v.a) return (int)j;
    return ARG_OTHER;
  }
  // An argument, just compiled (first: its first token): a literal, a lambda or a fn named here, which do nothing on
  // their own, or a lambda made here, or one of the function's parameters, or anything else.
  int argOrigin(const Tok& first) {
    if (tiering || !inferEffectsWanted) return ARG_OTHER;
    if (lamStart == first.p && lamEnd == prevEnd) return ARG_SAFE;
    if (prev.p != first.p) return ARG_OTHER;  // more than one token: its value may be a function from anywhere
    if (first.t == T_NUM || first.t == T_STR) return ARG_SAFE;
    if (first.t != T_ID) return ARG_OTHER;
    if (first.sym == K_true || first.sym == K_false || first.sym == K_nil) return ARG_SAFE;
    if (lastName.p != first.p) return ARG_OTHER;
    if (lastName.origin == ARG_SAFE && lastName.sym >= 0)
      if (EffFn* e = effFn()) e->called.insert(lastName.sym);
    return lastName.origin;
  }
  // A call of the value in v (not yet loaded): a parameter's, a lambda made here, an extern or a builtin, or one the
  // function can't follow. True for budget(n, f), which calls f: its arguments' origins are recorded after it.
  bool effCallOf(const LV& v) {
    EffFn* e = effFn();
    if (!e) return false;
    if (v.k == LV_GLOBAL && v.ro)
      if (Global* g = findGlobal(v.b); g && (g->kind == 'n' || g->kind == 'x' || g->kind == 'f')) {
        static const int budget = intern("budget");
        if (g->kind != 'n') effRecord(g->kind, v.b, {});
        return g->kind == 'n' && v.b == budget;
      }
    int sym, o = valueOrigin(v, &sym);
    if (o >= 0) e->calls |= 1ull << o;
    else if (o == ARG_SAFE && sym >= 0) e->called.insert(sym);
    else if (o == ARG_OTHER) e->unknown = true;
    return false;
  }
  void effSpawned(const LV& v, std::vector<int> args) {  // spawn v(args)
    EffFn* e = effFn();
    if (!e || v.k == LV_VAL) return;  // a lambda made here
    if (v.k == LV_GLOBAL)
      if (Global* g = findGlobal(v.b); g && g->kind == 'f') return effRecord('f', v.b, std::move(args));
    effCallOf(v);
  }

  // A value just made by C(...) or Enum.Variant(...) is exactly that class, so `var p = C()` gives p its type (until p
  // is assigned again) and p.name is checked against C's declared fields and methods, as if p were declared `p: C`.
  static int builtinKind(uint64_t t) {  // 'l' a list type (list, list<T>), 's' a str type (str, a "literal"), else 0
    if ((t & 15) == TY_LIST) return 'l';
    if ((t & 15) == TY_STR) return 's';
    if (const TypeDesc* d = (t & 15) == TY_EXT ? typeDesc(t) : nullptr) {
      if (d->k == 'L') return 'l';  // (not a matrix: a gpu fn reads one differently, and says so)
      if (d->k == 'l' && d->lit == 's') return 's';
    }
    return 0;
  }
  LV constructed(int cls) { return typedValue(tyClass(cls)); }
  LV resultOf(int fn) {  // a call of top-level fn: its declared result's type, once every signature is known
    LV v;
    if (!tiering) paths.push_back({0, -1, 0, fn}), v.path = int(paths.size()) - 1;
    return v;
  }
  LV typedValue(uint64_t type) {  // a value whose type the compiler knows here (a literal's, a constructor's)
    LV v;
    if (!tiering) v.path = addPath(0, -1, type);
    return v;
  }
  LV postfix() {
    return postfixRest(primary());
  }
  LV postfixRest(LV v) {
    for (;;) {
      if (at('.') || at(T_OPT_DOT)) {
        bool optional = at(T_OPT_DOT);
        next();
        Tok ft = cur;
        int s = at(T_ID) ? (next(), prev.sym) : ident();  // keywords too: m.class, as {class: ...} writes
        bool isCall = at('(') && !cur.nl;
        int receiver = inferEffectsWanted && !tiering && isCall ? valuePath(v) : 0, self = selfClass(F, v);
        int recv = !tiering && isCall && s != S_push ? valuePath(v) : 0;  // its type, if known: the method's params are checked
        int path = memberPath(v, ft, isCall);
        bool own = v.k == LV_LOCAL && F->cls >= 0 && v.a == F->selfHome;
        load(v);
        Cond no;
        auto facts = F->facts;
        bool live = F->live;
        if (optional) no = nonNilCond(), thenBegin(no);
        v = {LV_FIELD, 0, s}, v.own = own, v.path = path;
        if (at('(') && !cur.nl) {
          std::vector<int> origins;
          std::vector<ArgVal> args;
          next(), s == S_push ? pushCall(&origins) : call(s, false, &origins, recv ? &args : nullptr), v = {};
          if (recv) paths.push_back({recv, CALL, 0, s}), v.path = int(paths.size()) - 1;
          if (recv) callSites.push_back({s, std::move(args), file, srcStart, -1, ft.p, ft.line, recv});
          effRecord('m', s, std::move(origins), self, receiver);
        }
        if (optional) {
          v = postfixRest(v);
          int resultPath = v.path;
          load(v), ifEnd(elseBegin(no, false));
          F->facts = facts, F->live = live;
          LV result;
          result.path = resultPath;
          return result;
        }
      } else if (at('[') && !cur.nl) {
        LV base = v;
        int elemOf = tiering ? 0 : valuePath(v);
        next(), load(v);
        int t = slot();
        st(0, t);
        if (at(':')) imm(0, box(0.0));
        else expr();
        if (accept(':')) {
          int a = slot();
          mv(1, 0), ld(0, t), st(0, a), st(1, slot());
          if (at(']')) imm(0, box(std::numeric_limits<double>::infinity()));
          else expr();
          st(0, slot()), expect(']');
          internalCall("slice@", t, 3);
          v = {};
        } else {
          expect(']'), v = {LV_INDEX, t}, v.path = addPath(elemOf, AT);
          if (F->parVar >= 0 && (base.k == LV_CAP || base.k == LV_GLOBAL))  // from outside a parallel for's body
            v.b = base.k == LV_CAP ? base.a + 1 : -(base.a + 1), v.own = true, parBase = varName(base);
        }
      } else if (at('(') && !cur.nl) {
        std::vector<int> origins;
        bool budget = effCallOf(v);
        next(), load(v), call(0, false, budget ? &origins : nullptr), v = {};
        if (budget) effRecord('b', 0, std::move(origins));
      } else return v;
    }
  }
  LV unary() {
    if (accept('-')) {
      if (at(T_NUM) && !peekIs(T_POW)) return next(), imm(0, box(-prev.n)), NUM;
      bool num = operand();
      return neg(num), num ? NUM : LV{};
    }
    if (accept('!')) return operand(), notOp(), LV{};
    if (accept('~')) return operand(), hasLit = true, lit = -1, binop(O_BXOR), NUM;  // ~x is x ^ -1
    LV v = postfix();
    if (!accept(T_POW)) return v;
    load(v);
    int base = slot();
    st(0, slot()), operand(), st(0, slot());
    internalCall("pow@", base, 2);
    return NUM;
  }
  bool operand(int* h = nullptr, int* path = nullptr) {  // x0 = a unary expression; returns whether it is known to be a number
    LV v = unary();
    if (path) *path = valuePath(v);
    if (h) *h = v.k == LV_LOCAL ? v.a : NOH;  // the local it came straight from, if any
    load(v);
    return v.num;
  }
  bool expr(int minp = 1, int* path = nullptr) {
    int h;
    bool n = operand(&h, path);
    const char* end = prevEnd;
    binRest(minp, n, h);
    if (path && end != prevEnd) *path = 0;
    return n;
  }
  bool ternaryBranch() {
    if (at(T_EOF) || at('}') || at(']') || at(')') || at(':') || at(',') || at(';'))
      fail("@syntax write a ternary as 'condition ? yes : no'");
    return expr();
  }
  static bool comparison(int p) { return p == 3 || p == 4; }
  // x == [...] or x == {...}: == on a list or map asks whether it is the same one, and one made right here is new.
  void freshCompare(int op, const Tok& at) {
    const char* what = *at.p == '[' ? "list" : "map";
    diagnose(at.p, at.line, "@compare this %s is always %s: the %s here is a new one, and %s on a %s compares identity (the same %s?), not contents. "
             "To test for empty, use len(x) == 0; in a test, give the comparison its own expect (expect a == b compares contents)",
             op == O_EQ ? "==" : "!=", op == O_EQ ? "false" : "true", what, op == O_EQ ? "==" : "!=", what, what);
  }
  void singleComparison() {
    if (comparison(prec(cur.t))) fail("@syntax chained comparison: write a < b && b < c");
  }
  // Precedence climbing; the left operand is in x0. num/h: whether it is a number / the local it came from.
  void binRest(int minp, bool& num, int& h) {
    for (int t, p; (p = prec(t = cur.t)) && p >= minp && !(t == '-' && cur.nl); h = NOH) {
      next();
      if (t == '?') {
        Cond no = truthyCond();
        thenBegin(no);
        auto facts = F->facts;
        bool live = F->live;
        bool yes = ternaryBranch();
        if (!accept(':')) fail("@syntax write a ternary as 'condition ? yes : no'");
        int end = elseBegin(no, true);
        auto thenFacts = F->facts;
        bool thenLive = F->live;
        F->facts = facts, F->live = live;
        bool otherwise = ternaryBranch();
        ifEnd(end);
        F->facts &= thenFacts, F->live |= thenLive;
        num = yes && otherwise;
      } else if (t == T_AND || t == T_OR) {
        int j = shortBegin(t == T_AND);
        auto f = F->facts;  // the right side may not run
        bool live = F->live;
        expr(p + 1), shortEnd(j), num = false, F->facts = f, F->live = live;
      } else {
        num = rhs(opOf(t), comparison(p) ? 4 : p, num, h);
        if (comparison(p)) singleComparison();
      }
    }
  }
  // A right operand that is a local or a number literal (and binds no tighter operator) goes straight
  // into x1, so the left operand never round-trips through memory.
  // Returns 0 (not simple), 1 (local) or 2 (number).
  int simpleOperand(int p) {
    if (!at(T_NUM) && !(at(T_ID) && cur.sym >= NKW)) return 0;
    Tok n = peek();
    if (n.t == '.' || n.t == T_OPT_DOT || n.t == T_POW || ((n.t == '(' || n.t == '[') && !n.nl) || prec(n.t) > p) return 0;
    if (at(T_NUM)) return hasLit = true, lit = cur.n, next(), 2;
    LV v = resolve(F, cur.sym);
    if (v.k != LV_LOCAL) return 0;
    if (nav) nav->use(indexSite(cur), binding(v));
    return ldl(1, v.a), next(), rHome = v.a, v.num ? 2 : 1;
  }
  // An if/while condition. `a < b {` compiles to a compare and branch instead of building a boolean.
  Cond cond() {
    int lh;
    bool lnum = operand(&lh);
    binRest(5, lnum, lh);
    if (int t = cur.t, p = prec(t); comparison(p)) {
      next();
      int op = opOf(t);
      bool rnum = right(4, true, lh, op);
      singleComparison();
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
      while (!accept('}')) at(T_EOF) ? fail("@syntax expected '}'") : item(Statement, [&] { stmt(); });
    });
  }
  bool stmt() {  // returns true for a bare expression statement (the REPL echoes those)
    if (accept(';')) return false;
    if (at('{')) return block(), false;
    if (at(T_ID) && cur.sym == S_parallel && peek().t == T_ID && peek().sym == K_for) return next(), parallelFor(), false;
    if (at(T_ID) && cur.sym == S_shared && peek().t == T_ID && peek().sym == K_var && !peek().nl) return next(), sharedVar(), false;
    if (at(T_ID)) switch (cur.sym) {
        case K_var: {
          next();
          Tok n = cur;
          int s = ident();
          std::string_view written;
          bool annotated = at(':');
          Ty ty = declaredType(written);
          if (ty.code == TY_ANY) ty.code = 0;
          bool top = F->top && F->scope == 0, init = accept('=');
          std::string what = "var '" + symName(top ? qualify(s) : s) + "'";
          if (ty.code && !init && !acceptsNow(ty.code, NIL)) diagnose(n.p, n.line, "@type %s is %s, so it needs an initial value", what.c_str(), ty.text.c_str());
          Tok v = cur;
          int path = 0, vpath = 0;
          bool inherit = !top && !annotated, checked = init && ty.code && !tiering;  // inherit: an untyped local takes the value's type
          bool num = init ? expr(1, inherit || checked ? &vpath : nullptr) : (imm(0, NIL), false);
          if (inherit) path = vpath;
          if (ty.code && num && (ty.code & 15) != TY_NUM && ((ty.code & 15) <= TY_CLS || (ty.code & 15) == TY_BUF)) diagnose(v.p, v.line, "@type %s is %s, but its initial value is a number", what.c_str(), ty.text.c_str());
          else if (checked && !num) expects.push_back({ty.code, 0, -1, {vpath, false, v.p, v.line}, what, file, srcStart});
          checkType(ty, what, num);
          if (top) {
            int sym = qualify(s);
            if (nav && init && v.p == nav->ctorStart && prevEnd == nav->ctorEnd)
              if (int id = Navigation::get(nav->globals, sym); id >= 0) nav->constructors[id] = nav->ctorClass;
            if (gpuFreeze) gpuFreeze = false, callNative("freeze", true);
            return gmem(0, global(sym).slot, true), false;
          }
          int site = ty.code ? typeSite(what, ty.text) : -1;
          bool isNum = ty.code == TY_NUM;  // every store checks it, so it stays a number
          bool made = init && lamStart == v.p && lamEnd == prevEnd;  // var f = fn(...) { ... }
          auto it = std::find_if(F->locals.begin() + F->scopeAt, F->locals.end(), [&](auto& l) { return l.sym == s; });
          if (it != F->locals.end()) {  // redeclared in the same block: reuse
            if (it->path && (F->loop || F->retry)) paths[it->path].parent = 0;
            it->path = 0, it->made = false;
            effAssigned(s);
            it->ty = ty.code, it->site = site, it->num = isNum;
            if (nav) {
              it->symbol = indexLocal(n, "var", written, it->symbol);
              if (it->symbol >= 0) nav->reassigned.insert(it->symbol);
            }
            return stl(0, it->home), false;
          }
          int h = num || isNum ? fhome() : home();
          if (stl(0, h), num) prove(h);
          F->locals.push_back({s, h, isNum, ty.code, site});
          F->locals.back().path = addPath(path, -1), F->locals.back().made = made;
          if (nav) {
            int id = F->locals.back().symbol = indexLocal(n, "var", written);
            if (id >= 0 && init && v.p == nav->ctorStart && prevEnd == nav->ctorEnd) nav->constructors[id] = nav->ctorClass;
          }
          return false;
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
          if (F->top) fail("@misplaced 'return' outside a function");
          if (F->parVar >= 0) failAt(prev.p, prev.line, "@parallel a parallel for's body can't return: every iteration runs (write each result to a buffer at [i])");
        {
          ArgVal rv{0, false, cur.p, cur.line};
          bool bare = at('}') || at(T_EOF) || cur.nl, typed = F->retTy && !tiering && !gpuBody;
          bool n = bare ? (imm(0, NIL), false) : expr(1, typed ? &rv.path : nullptr);
          if (typed) {
            if (bare) rv.path = typedValue(TY_NILLIT).path, rv.p = prev.p, rv.line = prev.line;
            rv.num = n, expects.push_back({F->retTy, 0, -1, rv, typeSiteWhat(F->retSite), file, srcStart});
          }
          for (int i = 0; i < F->atomics; i++)  // the last is the outermost: it may run the block again
            i == F->atomics - 1 && F->retry ? commitOrRetry(true) : callNative("atomic@end", true);  // passes x0 through
          for (int i = 0; i < F->handles; i++) callNative("handle@end", true);
          leaveTries(0);                          // leaving every try
          F->retNum &= n || F->retTy == TY_NUM;  // the epilogue checks the result type
          return ret(), false;
        }
        case K_break: case K_continue:
          next();
          if (!F->loop && F->parVar >= 0) failAt(prev.p, prev.line, "@parallel a parallel for's body can't %s: every iteration runs", name(prev.sym));
          if (!F->loop) failAt(prev.p, prev.line, "@misplaced '%s' outside a loop", name(prev.sym));
          for (int i = F->atomics; i > F->loop->atomics; i--)
            i == 1 && F->retry ? commitOrRetry(false) : callNative("atomic@end", true);
          for (int i = F->handles; i > F->loop->handles; i--) callNative("handle@end", true);
          leaveTries(F->loop->tries), jump(*F->loop, prev.sym == K_break), F->live = false;
          return false;
        case K_class: case K_import: fail("@misplaced '%s' is only allowed at top level", name(cur.sym));
        case K_fn:
          if (peek().t == T_ID) {
            if (F->top) fail("@misplaced declare a named function at top level or inside another function");
            return next(), localFunction(), false;
          }
          break;
        case S_expect: if (isExpect()) return expectStmt(), false; break;
        case S_atomic: if (peekIs('{') && !peek().nl) return next(), atomicStmt(), false; break;
        case S_handle: if (peekIs('{') && !peek().nl) return next(), handleStmt(), false; break;
        case S_match: if (isMatch()) return next(), matchStmt(), false; break;
      }
    if (gpuBody && at(T_ID) && cur.sym >= NKW) {  // v.x = e, v[i] = e (v a local vector): v = a copy with that changed
      Lexer s = lx;
      Tok d = s.next(), f = s.next(), o = s.next();
      LV lv = d.t == '.' && f.t == T_ID && (o.t == '=' || (o.t >= T_ADDEQ && o.t <= T_DIVEQ)) ? resolve(F, cur.sym) : LV{LV_NONE};
      if (lv.k == LV_LOCAL) {
        next(), next(), next(), next();
        int base = slot(), a = slot(), b = slot(), c = slot(), e = slot();
        expr(), st(0, e);
        imm(0, box((double)(o.t == '=' ? -1 : O_ADD + (o.t - T_ADDEQ)))), st(0, c);
        imm(0, box((double)f.sym)), st(0, b);
        ldl(0, lv.a), st(0, a);
        internalCall("vec@set", base, 4);
        stl(0, lv.a);
        return false;
      }
      lv = d.t == '[' ? resolve(F, cur.sym) : LV{LV_NONE};
      if (lv.k == LV_LOCAL) {  // v[i] = e: v = vec@at(v, i, op, e)
        next(), next();
        int base = slot(), a = slot(), b = slot(), c = slot(), e = slot();
        expr(), st(0, b);
        expect(']');
        int op = cur.t;
        if (op != '=' && (op < T_ADDEQ || op > T_DIVEQ)) fail("@gpu expected '=': a gpu fn's statement v[i] sets a component (v[i] = e)");
        next();
        expr(), st(0, e);
        imm(0, box((double)(op == '=' ? -1 : O_ADD + (op - T_ADDEQ)))), st(0, c);
        ldl(0, lv.a), st(0, a);
        internalCall("vec@at", base, 4);
        stl(0, lv.a);
        return false;
      }
    }
    LV v = unary();
    int t = cur.t;
    if (t != '=' && (t < T_ADDEQ || t > T_DIVEQ)) {
      int h = v.k == LV_LOCAL ? v.a : NOH;
      bool n = v.num;
      return load(v), binRest(1, n, h), true;
    }
    if (v.k == LV_BAD) return next(), expr(), false;
    if (v.k == LV_GLOBAL && gpuVarSyms.count(v.b)) fail("@gpu %s is a gpu var: it never changes", name(v.b));
    if (v.k == LV_VAL || v.ro) fail("@misplaced cannot assign to this");
    next(), assign(v, t == '=' ? -1 : O_ADD + (t - T_ADDEQ));
    if (auto* local = pathLocal(F, v); local && local->path) {
      if (F->loop || F->retry || v.k == LV_CAP) paths[local->path].parent = 0;
      local->path = 0;
    }
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
    if (!atKw(K_catch)) fail("@syntax expected 'catch'");
    next();
    scope([&] {
      if (at(T_ID) && !atKw(K_try)) {
        int s = ident(), h = home();
        stl(0, h), F->locals.push_back({s, h});
        if (nav) F->locals.back().symbol = indexLocal(prev, "var");
      }
      block();
    });
    tryEnd(t), F->live |= l1, F->facts.reset();
  }
  // atomic { }: its changes to state other code can see (fields, lists, maps, globals, captured variables) all
  // happen, or none do: an error leaving it rolls them back (txn.cpp) before whatever catches the error runs.
  // It commits wherever control leaves it normally: its end, return, break or continue.
  // shared var name = value: one variable for every isolate (shared.cpp). Its first run sets it; a reload's keeps it.
  void sharedVar() {
    next();
    if (!F->top || F->scope) fail("@misplaced 'shared var' is only allowed at top level");
    Tok n = cur;
    int s = ident();
    if (at(':')) failAt(cur.p, cur.line, "@syntax a shared var has no declared type");
    if (!accept('=')) failAt(n.p, n.line, "@syntax shared var '%s' needs an initial value", name(s));
    expr();
    Global* g = findGlobal(qualify(s));
    if (!g || g->kind != 's') failAt(n.p, n.line, "internal: shared var '%s' wasn't declared", name(s));
    sharedCall("shared@init", g->cell);
  }
  // The outermost atomic block of a function is also a loop: a conflict on a shared var (atomic@commit returning
  // false, at any exit) rolls it back and runs it again, with the function's locals as they were when it started.
  // An atomic block inside it joins it (and one in a function called inside it joins it at run time).
  void atomicStmt() {
    if (F->atomics) {
      callNative("atomic@begin", false);
      F->atomics++, block(), F->atomics--;
      if (F->live) callNative("atomic@end", true);
      return;
    }
    std::vector<std::pair<int, int>> saves;  // (home, slot)
    for (auto& l : F->locals) {
      int t = slot();
      ldl(0, l.home), st(0, t), saves.push_back({l.home, t});
    }
    Loop rl;
    rl.tries = F->tries.size(), rl.handles = F->handles, rl.depth = 0;
    loopBegin(rl), bodyBegin(rl);
    callNative("atomic@begin", false);
    F->retry = &rl, F->atomics++, block(), F->atomics--;
    bool live = F->live;
    if (live) commitOrRetry(false), jump(rl, true);
    F->retry = nullptr;
    F->live = true, contHere(rl);
    for (auto [h, t] : saves) ld(0, t), stl(0, h);
    F->facts.reset(), loopBack(rl), loopEnd(rl);
    F->facts.reset(), F->live = live;
  }
  // The outermost atomic block ends here: commit, or (another isolate changed a shared var it read) run it again.
  void commitOrRetry(bool keep) {  // keep: x0 survives (a return's value)
    int k = slot();
    if (keep) st(0, k);
    callNative("atomic@commit", false);
    Cond c = truthyCond();
    continueUnless(*F->retry, c);
    if (keep) ld(0, k);
    F->depth = k;
  }
  void sharedCall(const char* nm, int cell) {  // nm(cell, x0)
    int base = slot(), a = slot(), b = slot();
    st(0, b), imm(0, box((double)cell)), st(0, a), internalCall(nm, base, 2);
  }
  static const char* tokStart(const Tok& t) { return t.t == T_STR ? t.p - 1 : t.p; }  // a string token's p is past its quote
  // expect <condition>: `expect` starts a statement unless a variable or function of that name is in scope.
  bool isExpect() {
    if (resolve(F, S_expect).k != LV_NONE || findGlobal(globalSym(S_expect))) return false;
    Tok n = peek();
    return !n.nl && n.t != '=' && n.t != '.' && n.t != T_EOF && !(n.t >= T_ADDEQ && n.t <= T_DIVEQ);
  }
  void internalCall(const char* name, int base, int arity) {
    gmem(0, findGlobal(intern(name))->slot), st(0, base), callWindow(base, arity, 0, false);
  }
  void callNative(const char* nm, bool arg) {  // nm(x0) or nm(): a runtime function the compiler calls by name
    int base = slot();
    if (arg) st(0, slot());
    internalCall(nm, base, arg);
  }
  // expect grows [allocs] { n -> body } like "n": grows@(site, the body as a closure, the growth, by allocations)
  // runs it for sizes n that double, fits how its time (or allocations) grows, and fails if faster than the growth.
  void growsStmt(int base, int siteAt, ExpectSite& s) {
    next();
    bool allocs = atKw(S_allocs) ? (next(), true) : false;
    Tok bt = cur;
    { Lexer l = lx; Tok a = l.next(), b = l.next(), c = l.next();
      if (!at('{') || a.t != T_ID || b.t != '-' || c.t != '>') fail("@syntax expected { n -> ... }: the code to measure, for a size n"); }
    lambda(bt, true), st(0, slot());
    if (!atKw(S_like)) fail("@syntax expected 'like' and how it may grow: \"1\", \"log n\", \"n\", \"n log n\", \"n^2\", \"n^3\"");
    next();
    const char* gs = tokStart(cur);
    expr(), st(0, slot());
    s.right.assign(gs, prevEnd);
    imm(0, allocs ? TRUE_ : FALSE_), st(0, slot());
    s.text = std::string(allocs ? "grows allocs" : "grows") + " { ... } like " + s.right;
    gmem(0, findGlobal(intern("grows@"))->slot), st(0, base);
    imm(0, box((double)expectSite(std::move(s)))), st(0, siteAt);
    callWindow(base, 4, 0, false);
  }
  // expect a op b | expect x | expect throws/prints/allocs { ... } [op value] | expect grows ... like g. Calls
  // expect@(site, left, right), which checks and, on failure, raises an error naming the source text and the values.
  // `expect a == b && c == d`: each part of a top-level && chain is checked as its own expect, so its == compares
  // contents and a failure shows that part's values. (An outer || or ternary keeps it one condition.)
  bool conjuncts() {
    Lexer s = lx;
    bool any = false;
    for (int d = 0; ; ) {
      Tok t = s.next();
      if (t.t == T_EOF || t.t == T_ERR || (d == 0 && (t.t == ';' || (t.nl && !prec(t.t))))) break;
      if (t.t == '(' || t.t == '[' || t.t == '{') d++;
      else if (t.t == ')' || t.t == ']' || t.t == '}') { if (--d < 0) break; }
      else if (d == 0 && (t.t == T_OR || t.t == '?')) return false;
      else if (d == 0 && t.t == T_AND) any = true;
    }
    return any;
  }
  void expectStmt() {
    int line = cur.line;
    next();
    if (atKw(S_grows) && !peek().nl && (peekIs('{') || (peek().t == T_ID && peek().sym == S_allocs))) {
      int base = slot(), siteAt = slot();
      gmem(0, findGlobal(intern("expect@"))->slot), st(0, base);
      ExpectSite s{file, "", "", "", line};
      return growsStmt(base, siteAt, s);
    }
    bool parts = !at(T_AND) && conjuncts();
    do expectOne(line, parts);
    while (parts && accept(T_AND));
  }
  void expectOne(int line, bool part) {  // part: stop at && (the caller checks the next part)
    int base = slot(), siteAt = slot();
    gmem(0, findGlobal(intern("expect@"))->slot), st(0, base);
    ExpectSite s{file, "", "", "", line};
    const char* start = tokStart(cur);
    bool throws = s.throws = atKw(S_throws) && peekIs('{'), prints = atKw(S_prints) && peekIs('{'), lnum = false;
    bool allocs = atKw(S_allocs) && peekIs('{');
    int lh = NOH;
    if (allocs) {  // how many objects the block allocated (gcAllocations)
      next();
      callNative("allocs@begin", false);
      int a = slot();
      st(0, a), block(), ld(0, a), F->depth = a;
      callNative("allocs@end", true);
      s.left = "allocs { ... }";
    } else if (prints) {  // what print() wrote inside, as a string; a throw stops the capture and goes on
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
    if (int t = cur.t, p = prec(t); comparison(p)) {
      next();
      const char* rs = tokStart(cur);
      expr(5);
      singleComparison();
      s.op = opOf(t), s.right.assign(rs, prevEnd);
      if (prec(cur.t) && !(part && at(T_AND))) {  // `expect a == b || c`: one condition, checked for truthiness
        if (throws || prints || allocs) fail("@syntax expect %s { } compares with one value", throws ? "throws" : prints ? "prints" : "allocs");
        mv(1, 0), ld(0, ls), binop(s.op);
        bool n = false;
        int h = NOH;
        binRest(1, n, h), st(0, ls), s.op = -1, s.left.assign(start, prevEnd), s.right.clear(), imm(0, NIL);
      }
    } else if (!throws && !prints && !allocs) binRest(part ? 3 : 1, lnum, lh), st(0, ls), s.left.assign(start, prevEnd), imm(0, NIL);
    else imm(0, NIL);
    st(0, slot());
    s.text = throws || prints || allocs ? (throws ? "throws { ... }" : prints ? "prints { ... }" : "allocs { ... }") + std::string(afterLeft, prevEnd)
                                        : std::string(start, prevEnd);
    imm(0, box((double)expectSite(std::move(s)))), st(0, siteAt);
    callWindow(base, 3, 0, false);
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
    int symbol = nav ? indexLocal(prev, "var") : -1;
    if (!atKw(K_in)) fail("@syntax expected 'in'");
    next();
    scope([&] {
      int each = 0;
      bool num = expr(1, tiering ? nullptr : &each);
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
        if (nav) F->locals.back().symbol = symbol;
        rangeTest(lp, a, b, v);
        return loop(lp, [&] { rangeStep(a); });
      }
      int a = home(), b = home(), v = home();  // the collection, a raw integer index, the element
      stl(0, a), imm(0, 0), stl(0, b), F->locals.push_back({nm, v});
      F->locals.back().path = addPath(addPath(each, EACH), -1);
      if (nav) F->locals.back().symbol = symbol;
      loopTop(lp);
      ldl(0, a), ldl(1, b), helper(H_ITER), exitIfEnd(lp);
      stl(0, v);
      loop(lp, [&] { ldl(0, b), addImm(0, 1), stl(0, b); });
    });
  }
  std::string parBase;  // the name of the last outside variable indexed in a parallel for's body
  void assign(LV v, int op) {  // op < 0: plain '=', else compound
    if (nav)
      if (int id = binding(v); id >= 0) nav->reassigned.insert(id);
    if (v.k == LV_CAP) effAssigned(F->caps[v.a].first);
    else if (Fun::Local* l = v.k == LV_LOCAL ? pathLocal(F, v) : nullptr) effAssigned(l->sym);
    if (F->parVar >= 0 && (v.k == LV_CAP || v.k == LV_GLOBAL))
      fail("@parallel a parallel for's body can't assign '%s', from outside it: each iteration runs on its own (write each result to a buffer at [i])",
           varName(v).c_str());
    if (F->parVar >= 0 && v.k == LV_INDEX && v.own) {
      auto w = std::make_pair(v.b, parBase);
      if (std::find(F->parWrites.begin(), F->parWrites.end(), w) == F->parWrites.end()) F->parWrites.push_back(w);
    }
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
    if (v.k == LV_SHARED) {
      LV t = v;
      if (op >= 0) load(t);
      value();
      return sharedCall("shared@set", v.a);
    }
    if (v.k == LV_LOCAL || v.k == LV_GLOBAL || v.k == LV_CAP) {
      LV t = v;
      if (op >= 0) load(t);
      ArgVal a{0, false, cur.p, cur.line};
      bool checked = op < 0 && ty.first && !tiering;
      bool num = checked ? expr(1, &a.path) : value(t.num);
      if (checked) a.num = num, expects.push_back({ty.first, 0, -1, a, typeSiteWhat(ty.second), file, srcStart});
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
      ArgVal a{0, false, cur.p, cur.line};
      bool checked = op < 0 && v.path && !tiering;  // a field whose type may be declared: known once every class is
      if (checked) a.num = expr(1, &a.path), expects.push_back({0, v.path, v.b, a, "", file, srcStart});
      else value();
      mv(2, 0), ld(0, o), field(v.own ? v.b : int64_t(v.b | SETF_OUTSIDE), true);
      if (!v.own) {  // from outside the object: if its class has invariants (rt_setf says so), they must hold now
        int keep = slot();
        st(0, keep);
        int j = invPendingBegin(), b = slot(), w = slot();
        mv(0, 1), st(0, b), konst("storing to field '" + symName(v.b) + "'"), st(0, w), callWindow(b, 1, invariantSym(), false);
        invPendingEnd(j), ld(0, keep), F->depth = keep;
      }
      F->depth--;
    } else {  // object in slot v.a, index in x0
      int i = slot();
      st(0, i);
      if (op >= 0) mv(1, 0), ld(0, v.a), index(false);
      value(), mv(2, 0), ld(0, v.a), ld(1, i), index(true), F->depth -= 2;
    }
  }

  // ---- declarations ----
  void localFunction() {
    Tok t = cur;
    int sym = ident();
    int id = -1;
    if (nav) {
      auto first = F->locals.begin() + F->scopeAt, last = F->locals.end();
      auto old = std::find_if(first, last, [&](auto& l) { return l.sym == sym; });
      id = indexLocal(prev, "fn", {}, old == last ? -1 : old->symbol);
      if (id >= 0 && old != last) nav->reassigned.insert(id);
    }
    lambda(t, false, sym, -1, id);
    auto it = std::find_if(F->locals.begin() + F->scopeAt, F->locals.end(), [&](auto& l) { return l.sym == sym; });
    if (it != F->locals.end()) {
      int h = it->home;
      *it = {sym, h};
      if (nav) it->symbol = id;
      effAssigned(sym);  // it held something else before
      return stl(0, h);
    }
    int h = home();
    stl(0, h), F->locals.push_back({sym, h});
    F->locals.back().made = true;
    if (nav) F->locals.back().symbol = id;
  }
  static constexpr const char* defaultArgument =
      "@syntax fut has no default arguments; pass a map of options, e.g. fn f(a, opts: {b: num?})";
  // Compile `(params) { body }` and queue it to be installed behind `stub`. Returns the arity.
  int function(Fun& f, int cls, bool init, const std::string& nm, u32* stub, Fn* fn) {
    f.cls = cls, f.init = init, f.k.name = nm, f.k.file = file;
    Tok nameTok = prev;  // for errors about the whole function
    const SpecFn* spec = nullptr;  // its contract
    if (!f.lambda && !f.test)
      if (auto it = specFns.find(nm); it != specFns.end()) spec = &it->second;
    bool inv = cls >= 0 && !f.lambda && !f.test && invClasses.count(cls);  // an outside call checks invariants when it ends
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
    EffFn* eff = f.lambda || f.test || tiering || !inferEffectsWanted ? nullptr : &(effFns[nm] = EffFn{file, symName(nameTok.sym), nameTok.line, cls});
    if (f.parVar >= 0) {  // a parallel for's body: its variable is the parameter, and the block follows
      int h = home();
      f.locals.push_back({f.parVar, h}), homes.push_back(h), ptys.push_back({f.parVar, Ty{}});
      if (nav) f.locals.back().symbol = f.lambdaSymbol;
    } else if (f.arrow) {  // { n -> body }
      expect('{');
      int s = ident(), h = home();
      f.locals.push_back({s, h}), homes.push_back(h), ptys.push_back({s, Ty{}});
      if (nav) f.locals.back().symbol = indexLocal(prev, "parameter");
      expect('-'), expect('>');
    } else if (!f.test) {
      expect('(');
      if (!accept(')')) {
        do {
          int s = ident(), h = home();  // not d8..: arguments arrive, and calls pass them, in x registers
          Tok param = prev;
          f.locals.push_back({s, h}), homes.push_back(h);
          std::string_view written;
          ptys.push_back({s, declaredType(written)});
          if (nav) {
            int id = f.locals.back().symbol = indexLocal(param, "parameter", written);
            if (id >= 0 && ptys.back().second.code) nav->paramTypes[id] = ptys.back().second.code;
            for (auto old = f.locals.begin(); old != f.locals.end() - 1; ++old)
              if (old->sym == s) nav->collision(old->symbol, id);
            if (f.lambdaName == s) nav->collision(f.lambdaSymbol, id);
          }
          if (at('=')) failAt(cur.p, cur.line, defaultArgument);
          if (Ty& t = ptys.back().second; t.code && t.code != TY_ANY)  // stores to it are checked too
            f.locals.back().ty = t.code, f.locals.back().num = t.code == TY_NUM, f.locals.back().site = typeSite("argument '" + symName(s) + "' of " + display(nm), t.text);
        } while (accept(','));
        expect(')');
      }
      Ty r;
      if (at(':')) {
        Tok c = cur;
        next(), r = type();
        if (init) failAt(c.p, c.line, "@class init can't declare a result type: it returns the new instance");
        f.retTy = r.code, f.retSite = typeSite("the result of " + display(nm), r.text);
      }
      if (at('!')) {  // declared effects: ! io, clock (an empty set: performs nothing)
        next(), edecl = true, etext = "!";
        if (!at('{') && !atKw(S_requires) && !atKw(S_ensures))
          do etext += etext.size() > 1 ? ", " : " ", emask |= effectName(etext);
          while (accept(','));
      }
      if (nav)
        if (int id = Navigation::get(nav->declared, indexSite(nameTok)); id >= 0 && prevEnd > nameTok.p) {
          auto& d = nav->out.declarations[id];
          d.signature = (d.kind == "view" ? "view " : "fn ") + squashed(nameTok.p, prevEnd);
        }
      while (atKw(S_requires) || atKw(S_ensures)) {  // its contract (the prescan has it): compiled below
        next();
        for (int d = 0; !(d == 0 && (at('{') || at('}') || cur.nl)); next()) {
          if (at(T_EOF)) fail("@syntax expected '{'");
          d += nesting(cur);
        }
      }
      if (!f.lambda && !tiering) {  // for the override check: "(num, _): str" (_: untyped)
        Sig& g = sigs[nm];
        g.types.clear(), g.text = "(";
        g.params.clear();
        for (auto& [s, ty] : ptys) g.types.push_back(ty.code), g.params.push_back(s), g.text += (g.text.size() > 1 ? ", " : "") + (ty.code ? ty.text : "_");
        g.types.push_back(r.code), g.text += ")" + (r.code ? ": " + r.text : "");
        g.eff = edecl, g.effMask = emask, g.effText = etext;
      }
    }
    int arity = (int)homes.size();
    if (arity > 64) fail("@limit too many parameters");
    if (eff) {
      f.paramHomes.assign(homes.begin() + first, homes.end());
      for (auto& [s, ty] : ptys) eff->params.push_back(s);
      eff->declared = edecl, eff->mask = emask;
    }
    for (int i = 0; i < arity; i++) param(i, homes[i]);
    if (traceOn && !editorCheck && !f.test && !(f.lambda && owner.empty())) traceTouch(traceMark(f.lambda ? owner : nm));  // fut test: it ran
    for (int i = first; i < arity && i < 8; i++)
      if (f.spec >> (i - first) & 1) f.guard.push_back(i), prove(homes[i]);
    for (size_t i = 0; i < ptys.size(); i++) {  // tier 2: a number parameter its entry guard proved needs no check
      auto& [s, ty] = ptys[i];
      int h = homes[first + i];
      if (!ty.code || (ty.code == TY_NUM && proven(F, h))) continue;
      ldl(0, h), checkType(ty, "argument '" + symName(s) + "' of " + display(nm));
      if (ty.code == TY_NUM) prove(h);
    }
    if (edecl) effectsIn(emask, effSite(display(nm), emask, etext)), f.effDecl = true, f.effMask = emask, f.effWho = display(nm), f.effText = etext;
    if (spec || inv) contract(f, nm, nameTok.line, spec, inv, ptys, homes, first);
    Tier* tier = nullptr;  // (tier 2 compiles a function again without its contract: one with a contract stays in tier 1)
    if (!f.lambda && !f.test && !tiering && !spec && !inv && !noTier && !editorCheck)
      hotCounter(tier = tierFor(nm), first, homes);
    if (!f.arrow) block();
    else scope([&] { while (!accept('}')) at(T_EOF) ? fail("@syntax expected '}'") : item(Statement, [&] { stmt(); }); });
    if (f.live) f.retNum = false;  // can end without a return: returns nil
    pend.push_back({stub, fn, arity, finish(), owner, f.lambda || f.test, {}, envFor(file)});  // a test body is installed like a lambda
    if (tier && spans[nm].end) {
      Span& sp = spans[nm];
      pend.back().tier = {nm, std::string(sp.start, sp.end), file, sp.line, cls, cls < 0 ? intern(nm) : -1, init, stub, nullptr, nullptr, fn, false};
    }
    F = outer;
    return arity;
  }
  // ---- contracts: requires and ensures on a function, invariant in a class ----
  // A clause's condition is the rest of its line, up to a '{' or '}' there (a function's body, the class's end).
  bool wholeClause() {  // does an outer || or ternary prevent checking each && part on its own?
    Lexer s = lx;
    Tok p, t = cur;
    for (int d = 0; t.t != T_EOF && t.t != T_ERR && !clauseEnds(d, p, t); p = t, t = s.next(), d += nesting(p))
      if (d == 0 && (t.t == T_OR || t.t == '?')) return true;
    return false;
  }
  // The names a contract's clauses see: self, the function's parameters and, in ensures, result. Code is attributed to
  // the function's first line.
  template <class B> void clauses(const std::vector<Fun::Local>& names, int line, B body) {
    auto saved = std::move(F->locals);
    size_t sa = F->scopeAt;
    F->locals = names, F->scopeAt = 0, pinLine = line;
    body();
    F->locals = std::move(saved), F->scopeAt = sa, pinLine = 0;
  }
  // One condition, compiled to inline checks: each `&&` part on its own, so a failure names the part and its values
  // (one check, if the condition has a `||` or ternary outside brackets). A false part calls contract@fail, which raises.
  void check(const At& a, const std::string& head, int whoHome = NOH) {  // whoHome: inv@'s who
    detour(a.file, a.src, a.p, a.line, [&] {
      int more = wholeClause() ? 1 : 3;  // the operators one part goes on through (3: not &&)
      for (;;) {
        ExpectSite s{file, "", "", "", cur.line};
        s.contract = head;
        int base = slot(), siteAt = slot(), ls = slot(), rs = slot(), ws = slot();
        const char* start = tokStart(cur);
        int lh;
        bool lnum = operand(&lh);
        binRest(5, lnum, lh);
        if (int t = cur.t, p = prec(t); comparison(p)) {
          s.left.assign(start, prevEnd), next();
          const char* r0 = tokStart(cur);
          st(0, ls);
          bool rnum = expr(5);
          singleComparison();
          s.op = opOf(t), s.right.assign(r0, prevEnd);
          st(0, rs), mv(1, 0), ld(0, ls), hasLit = false, binop(s.op, lnum, rnum);
          if (prec(cur.t) >= more) {  // `a < b || c`: one value, checked for truthiness
            bool n = false;
            int h = NOH;
            binRest(more, n, h), st(0, ls), s.op = -1, s.right.clear(), s.left.assign(start, prevEnd);
          }
        } else {
          if (prec(cur.t) >= more) binRest(more, lnum, lh);
          st(0, ls), s.left.assign(start, prevEnd);
        }
        s.text.assign(start, prevEnd);
        notOp();
        Cond no = truthyCond();
        thenBegin(no);
        if (s.op < 0) imm(0, NIL), st(0, rs);
        gmem(0, findGlobal(intern("contract@fail"))->slot), st(0, base);
        imm(0, box((double)expectSite(std::move(s)))), st(0, siteAt);
        whoHome != NOH ? ldl(0, whoHome) : imm(0, NIL), st(0, ws);
        callWindow(base, 4, 0, false);
        ifEnd(elseBegin(no, false));
        F->depth = base;
        if (more == 1 || !at(T_AND)) break;
        next();
      }
      if (!at('{') && !at('}') && !cur.nl) fail("@syntax expected the condition to end here (a clause is one line)");
    });
  }
  // A function with a contract: requires, then each old(...) into a home, on entry; ensures where every return
  // arrives (Fun::exit). A method of a class with invariants (its own, a superclass's or a subclass's) checks them
  // when an outside call ends, by calling the object's hidden invariant method: invEnter tells whether this is one
  // (no call on the same object is running), so a method calling its own object's methods mid-change is not.
  void contract(Fun& f, const std::string& nm, int line, const SpecFn* spec, bool inv,
                const std::vector<std::pair<int, Ty>>& ptys, const std::vector<int>& homes, int first) {
    bool calls = spec && spec->calls;
    std::string fn = display(nm);
    int who = calls ? effSite("the contract of " + fn, 0, "!") : 0;
    int g = f.cls >= 0 ? f.cls : intern(nm);
    bool pub = g < (int)pubSym.size() && pubSym[g];  // a pub declaration's contract is in its .api
    std::string what = "the contract of " + fn + ", a pub " + (f.cls >= 0 ? "class's method," : "function,");
    std::vector<Fun::Local> in, out;  // what clauses see: on entry, and on exit (where a parameter the body assigns is a copy)
    if (f.cls >= 0) in.push_back({K_self, f.selfHome});
    out = in;
    for (size_t i = 0; spec && i < ptys.size(); i++) {
      int h = homes[first + i];
      in.push_back({ptys[i].first, h});
      if (nav) in.back().symbol = localSymbol(&f, {LV_LOCAL, h});
      if (!spec->post.empty() && assignedIn(ptys[i].first)) { int c = home(); ldl(0, h), stl(0, c), h = c; }
      out.push_back({ptys[i].first, h});
      if (nav) out.back().symbol = in.back().symbol;
    }
    auto begin = [=, this] { if (calls) imm(0, box((double)who)), callNative("contract@begin", true); };
    auto end = [=, this] { if (calls) callNative("contract@end", false); };
    auto oldAt = std::make_shared<std::unordered_map<const char*, int>>();
    if (spec && (!spec->pre.empty() || !spec->olds.empty())) {
      begin();
      pubContract = pub ? what.c_str() : nullptr;
      clauses(in, line, [&] {
        for (auto& c : spec->pre) check(c.at, "the caller broke " + fn + "'s contract: requires");
        for (auto& o : spec->olds) {
          int h = home();
          detour(o.file, o.src, o.p, o.line, [&] { next(), expect('('), expr(), expect(')'); });
          stl(0, h), (*oldAt)[o.p] = h;
        }
      });
      pubContract = nullptr, end();
    }
    int outer = -1, rec = -1;  // invEnter's answer (an outside call), and its record's two frame slots
    if (inv) rec = slot(), slot(), ldl(0, f.selfHome), invEnter(rec), stl(0, outer = home());
    if ((!spec || spec->post.empty()) && !inv) return;
    f.exit = [=, this] {
      int rh = home();
      stl(0, rh);
      if (spec && !spec->post.empty()) {
        auto names = out;
        names.push_back({S_result, rh});
        olds = oldAt.get(), begin(), pubContract = pub ? what.c_str() : nullptr;
        clauses(names, line, [&] { for (auto& c : spec->post) check(c.at, fn + " broke its contract: ensures"); });
        end(), olds = nullptr, pubContract = nullptr;
      }
      if (inv) {
        ldl(0, outer);
        Cond no = truthyCond();
        thenBegin(no);
        int b = slot(), w = slot();
        ldl(0, f.selfHome), st(0, b), konst(fn), st(0, w), callWindow(b, 1, invariantSym(), false), F->depth = b;
        ifEnd(elseBegin(no, false));
        invLeave(rec);
      }
      ldl(0, rh);
    };
  }
  // A class's invariants as a hidden method, inv@(who): each checked with no effects allowed (and what a clause's
  // calls change put back), then the superclass's (inv@ on a class without invariants answers the receiver). who
  // names what the error blames: the method whose call ended, or a store to a field from outside.
  void invariantMethod(ClassDef& cd, const ClassSpec& cs) {
    int inv = invariantSym();
    Fn*& fn = methodFn(cd.sym, inv);
    if (!fn) fn = make<Fn>(), fn->code = parserStub(), fn->name = inv, consts.push_back(box(fn));
    cd.methods.push_back({inv, fn});
    std::string K = symName(cd.sym), q = K + ".inv@";
    Fun f, *outer = F;
    f.cls = cd.sym, f.init = true, f.k.name = q, f.k.file = file, F = &f, beginFun();  // init: it returns self
    f.selfHome = home(), f.locals.push_back({K_self, f.selfHome});
    int who = home();
    param(0, f.selfHome), param(1, who);
    if (traceOn && !editorCheck) traceTouch(traceMark(q));
    bool calls = false;
    for (auto& c : cs.invariants) calls |= c.calls;
    if (calls) imm(0, box((double)effSite("the invariant of " + K, 0, "!"))), callNative("contract@begin", true);
    std::string what = "the invariant of " + K + ", a pub class,";
    if (cd.sym < (int)pubSym.size() && pubSym[cd.sym]) pubContract = what.c_str();
    refKey = q;
    clauses({{K_self, f.selfHome}}, cs.at.line, [&] { for (auto& c : cs.invariants) check(c.at, "\x01 broke " + K + "'s invariant:", who); });
    refKey.clear();
    pubContract = nullptr;
    if (calls) callNative("contract@end", false);
    if (cd.super >= 0) {
      int b = slot(), w = slot();
      ldl(0, f.selfHome), st(0, b), ldl(0, who), st(0, w), callWindow(b, 1, inv, true), F->depth = b;
    }
    pend.push_back({(u32*)fn->code, fn, 2, finish(), q, true, {}, envFor(file)});
    F = outer;
  }
  bool unchanged(const std::string& q) {  // incremental reload: skip a body whose text and environment match
    present.push_back(q);
    Span& sp = spans[q];
    if (!sp.end) return false;
    auto it = recs.find(q);
    if (editorCheck || it == recs.end() || it->second.text != fnv(sp.start, sp.end) || it->second.env != envFor(file)) return false;
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
  // Scalar defaults are shared by all instances.
  V constant() {
    const char* why = "a field's default must be a number, string, true, false, nil, [] or {} (set other values in init)";
    if (accept('-')) return at(T_NUM) ? (next(), box(-prev.n)) : (fail("@class %s", why), NIL);
    if (accept(T_NUM)) return box(prev.n);
    if (atKw(K_true) || atKw(K_false) || atKw(K_nil)) return next(), prev.sym == K_true ? TRUE_ : prev.sym == K_false ? FALSE_ : NIL;
    if (!at(T_STR)) fail("@class %s", why);
    std::string s;
    int line = cur.line;
    for (const char *p = cur.p, *e = p + cur.len; p < e; p++) {
      if (*p == '{') fail("@class a field's default can't interpolate: set it in init");
      if (*p != '\\' || p + 1 == e) { line += *p == '\n'; s += *p; continue; }
      p = escape(p + 1, e, s, line);
    }
    next();
    return consts.push_back(box(newStr(s))), consts.back();
  }
  void classDecl() {
    ClassDef cd{qualify(ident()), -1, {}};
    if (accept(':')) {
      ident();
      Tok base = topName(prev);
      Global* g = findGlobal(cd.super = base.sym);
      if (!g || g->kind != 'c' || cd.super == cd.sym) failAt(prev.p, prev.line, "@class bad superclass '%s'", name(cd.super));
      if (enumOf(cd.super) >= 0) failAt(prev.p, prev.line, "@class '%s' belongs to an enum: its variants are its only classes (add one to the enum)", name(cd.super));
      if (nav) indexGlobal(base), nav->supers[cd.sym] = cd.super;
    }
    expect('{');
    while (!accept('}')) {
      if (at(T_EOF)) {
        // A class has always reported the expected member at an unfinished body.
        fail("@syntax expected 'fn', 'override fn', 'var' or 'invariant' in class body");
      }
      item(Member, [&] {
        if (atKw(S_pub)) fail("@misplaced a class's fields and methods are public when the class is: put 'pub' before 'class'");
        if (atKw(K_var)) {  // var name [= constant]: a declared field
          next();
          int s = ident();
          Tok field = prev;
          for (auto& [x, d] : cd.fields) if (x == s) failAt(prev.p, prev.line, "@duplicate duplicate field '%s'", name(s));
          cd.fields.push_back({s, NIL});
          std::string_view written;
          Ty ty = declaredType(written);
          if (nav) nav->fields[{cd.sym, s}] = indexLocal(field, "field", written, -1, symName(cd.sym));
          bool given = accept('=');
          Tok dt = cur;
          bool collection = given && (at('[') || at('{'));
          V d = NIL;
          if (collection) {
            bool list = accept('[');
            if (!list) next();
            if (!accept(list ? ']' : '}')) fail("@class only empty collection field initializers are supported (set other values in init)");
            cd.collectionInits.push_back({s, list ? K::List : K::Map});
            if (ty.code) d = list ? box(make<List>()) : box(make<Map>());
          } else if (given) d = constant();
          if (ty.code && given && !acceptsNow(ty.code, d)) diagnose(dt.p, dt.line, "@type field '%s' is %s, but its default is not", name(s), ty.text.c_str());
          if (ty.code) cd.ftypes.push_back({s, ty.code, typeSite("field '" + symName(s) + "' of " + symName(cd.sym), ty.text)});
          cd.fields.back().second = collection ? NIL : d;
          return;
        }
        if (atKw(S_invariant) && !peek().nl) {  // (the prescan has it: compiled into the methods)
          if (!cur.nl && prev.t != '{') fail("@syntax an invariant goes on its own line");
          next();
          for (int d = 0; !(d == 0 && (at('}') || cur.nl)); next()) {
            if (at(T_EOF)) fail("@syntax expected '}'");
            if (d == 0 && at('{')) fail("@syntax an invariant ends at the end of its line (wrap a lambda in parentheses)");
            d += nesting(cur);
          }
          return;
        }
        bool over = atKw(S_override) && peek().t == T_ID && peek().sym == K_fn;
        if (over) next();
        if (skipInvalid(Member)) return;
        if (!atKw(K_fn)) fail("@syntax expected 'fn', 'override fn', 'var' or 'invariant' in class body");
        next();
        Tok mt = cur;
        int m = ident();
        for (auto& [x, f] : cd.methods) if (x == m) failAt(prev.p, prev.line, "@duplicate duplicate method '%s'", name(m));
        for (auto& [x, d] : cd.fields) if (x == m) failAt(prev.p, prev.line, "@duplicate '%s' is already a field", name(m));
        Fn*& f = methodFn(cd.sym, m);
        if (!f) f = make<Fn>(), f->code = parserStub(), f->name = m, consts.push_back(box(f));
        cd.methods.push_back({m, f}), cd.written.push_back({m, over, {mt.p, mt.line, file, srcStart}});
        std::string q = symName(cd.sym) + "." + symName(m);
        if (unchanged(q)) return;
        Fun fun;
        owner = q, function(fun, cd.sym, m == S_init, q, (u32*)f->code, f);
      });
    }
    if (auto it = classSpecs.find(cd.sym); it != classSpecs.end() && !it->second.invariants.empty()) invariantMethod(cd, it->second);
    classes.push_back(std::move(cd));
  }
  void enumDecl() {
    int s = qualify(ident());
    expect('{');
    auto found = enumDefs.find(s);
    if (found == enumDefs.end()) fail("@syntax expected a variant: a name, with its fields in parentheses if it has any (Circle(r: num))");
    EnumDef& e = found->second;
    ClassDef base{s, -1, {}};
    base.sum = 1, classes.push_back(std::move(base));
    for (size_t i = 0; !accept('}');) {
      if (accept(',')) continue;
      ident();
      if (i >= e.vs.size()) failAt(prev.p, prev.line, "@syntax expected a variant");
      ClassDef cd{e.vs[i++].sym, s, {}};
      cd.sum = 2;
      if (at('(') && !cur.nl)
        for (next(); !accept(')');) {
          int f = ident();
          Tok field = prev;
          std::string_view written;
          Ty ty = declaredType(written);
          if (nav) nav->fields[{cd.sym, f}] = indexLocal(field, "field", written, -1, symName(cd.sym));
          if (ty.code && ty.code != TY_ANY) cd.ftypes.push_back({f, ty.code, typeSite("field '" + symName(f) + "' of " + symName(cd.sym), ty.text)});
          cd.fields.push_back({f, NIL});
          if (!at(')')) expect(',');
        }
      classes.push_back(std::move(cd));
    }
  }
  // extern ["library"] fn name(type, ...) [: type] -- a C function (a host function in wasm); types: num int long ptr str (and void)
  // extern [lib] fn name(types): result. lib: a C library ("libm.dylib"; none: the program's own symbols, or on
  // wasm the host's JavaScript), or a module of this file's package (its fut.pkg lists it: js, wasm or c). A module
  // this build doesn't load (a js or wasm one natively, a c one in wasm) gives a stub: the program compiles, and
  // its other code runs; a call raises [host].
  void externDecl() {
    std::string lib, shownLib, stub;
    Tok lt = cur;
    const ForeignMod* fm = nullptr;
    if (accept(T_STR)) lib.assign(prev.p, prev.len), shownLib = "'" + lib + "'";
    else if (at(T_ID) && !atKw(K_fn)) {
      std::string m = symName(ident());
      const Package* p = pkgOf(file);
      if (!p) failAt(lt.p, lt.line, "@extern '%s' would be a module of a package, and %s is in no package (a package's fut.pkg lists its modules: js %s ./%s.js)", m.c_str(), ::base(file).c_str(), m.c_str(), m.c_str());
      fm = p->module(m, !WASM);
      if (!fm) failAt(lt.p, lt.line, "@extern package %s has no module '%s': add 'js %s ./file.js' (or wasm, or c) to %s", p->name.c_str(), m.c_str(), m.c_str(), p->manifest.c_str());
      if ((fm->kind == "c") != !WASM) {  // what calling it says ("dom.create() is JavaScript (std/ui's dom.js), ...")
        bool c = fm->kind == "c", std = inStd(p->dir);
        stub = m + ".%s() is " + (c ? "a C library" : fm->kind == "js" ? "JavaScript" : "WebAssembly") + " (" + (std ? "std/" : "package ") +
               p->name + "'s " + (c ? fm->target : ::base(fm->target)) + "), which only the " +
               (c ? "native build loads: run this with fut" : "wasm build loads: run this in a browser or with node web/run.mjs") +
               (std ? "" : c ? ", or add a js or wasm version (js " + m + " <file>) to " + p->manifest : ", or add a C version (c " + m + " <library>) to " + p->manifest);
      }
      lib = fm->kind == "c" ? fm->target : fm->kind + (fm->main ? "+main:" : ":") + fm->target, shownLib = "module " + m + " (" + fm->target + ")";
      if (stub.empty() && fm->kind != "c" && std::find(modFiles.begin(), modFiles.end(), fm->target) == modFiles.end()) modFiles.push_back(fm->target);
    }
    if (!atKw(K_fn)) fail("@syntax expected 'fn'");
    next();
    Tok t = cur;
    int s = ident();
    if (!stub.empty()) stub.replace(stub.find("%s"), 2, symName(s));
    const char* err = nullptr;
    // extern "fut": a function of the runtime's own (fmath.cpp: math with the same results on every platform)
    Global* rtg = lib == "fut" ? findGlobal(intern("fut@" + symName(s))) : nullptr;
    if (lib == "fut" && !rtg) failAt(t.p, t.line, "@extern the runtime has no function '%s'", name(s));
    // An editor check never loads a project's shared libraries: even dlopen can run their code.
    void* fn = rtg ? (void*)rtg->o : editorCheck || !stub.empty() ? (void*)1 : externLookup(lib, symName(s), &err);
    if (err && !strcmp(err, "pending")) {  // the host loads it, then compiles again
      if (std::find(modulesPending.begin(), modulesPending.end(), lib) == modulesPending.end()) modulesPending.push_back(lib);
      fn = (void*)1;
    } else if (err) failAt(lt.p, lt.line, "@extern cannot load %s", shownLib.c_str());
    if (!fn) failAt(t.p, t.line, fm && fm->kind != "c" ? "@extern module %s has no function '%s'" : "@extern no C function '%s'", fm ? fm->name.c_str() : name(s), name(s));
    // any: a value copied as JSON (lists and maps; functions and JS objects by reference); fn: a fut function
    // JavaScript can call. Both only into JavaScript; a wasm module's functions take numbers.
    bool js = fm ? fm->kind == "js" : WASM;  // (natively, a js module's stub)
    auto type = [&](bool ret) {
      int ty = atKw(K_fn) ? (next(), K_fn) : ident();
      std::string n = ty == K_fn ? "fn" : symName(ty);
      bool num = n == "num" || n == "int" || n == "long" || (ret && n == "void");
      if (fm && fm->kind == "wasm" && !num) failAt(prev.p, prev.line, "@extern a wasm module's functions take and return numbers (num, int, long%s)", ret ? ", void" : "");
      if ((n == "any" || (n == "fn" && !ret)) && !js) failAt(prev.p, prev.line, "@extern '%s' values cross only into JavaScript (a js module, or the wasm build's host)", n.c_str());
      if (n == "any") return 'a';
      if (n == "fn" && !ret) return 'f';
      for (const char* c : {"num", "int", "long", "ptr", "str", "void"})
        if (n == c && (ret || c[0] != 'v')) return c[0] == 'n' ? 'd' : c[0];
      failAt(prev.p, prev.line, "@extern unknown extern type '%s' (num, int, long, ptr, str%s%s)", n.c_str(), js ? ", any, fn" : "", ret ? ", void" : "");
    };
    std::string sig;
    expect('(');
    if (!accept(')')) {
      do sig += type(false); while (accept(','));
      expect(')');
    }
    if (sig.size() > 7) failAt(t.p, t.line, "@limit extern functions take at most 7 parameters");
    char ret = accept(':') ? type(true) : 'v';
    if (nav)
      if (int id = Navigation::get(nav->globals, qualify(s)); id >= 0)
        nav->out.declarations[id].signature = "extern fn " + squashed(t.p, prevEnd);
    // A bare `!` declares that the function performs nothing (sin, strlen): the caller's promise, as its types are.
    // Without it, a call performs io.
    bool pure = at('!') && !cur.nl;
    if (pure && (next(), at(T_ID) && !cur.nl)) fail("@extern an extern fn performs io, or with a bare '!' nothing: it can't declare other effects");
    if (rtg && (sig != std::string(((Fn*)rtg->o)->arity, 'd') || ret != 'd' || !pure))
      failAt(t.p, t.line, "@extern the runtime's %s takes %d numbers and returns one, performing nothing: (%s): num !", name(s), ((Fn*)rtg->o)->arity,
             ((Fn*)rtg->o)->arity == 1 ? "num" : "num, num");
    externs.push_back({qualify(s), fn, sig, ret, pure, rtg != nullptr, stub});
  }
  // effect Name { fn op(params) [: type] ... }: operations with no body. Code performs one as Name.op(args); the
  // nearest handler for it answers (handle { } with Name.op(params) { }).
  void effectDecl() {
    Tok nt = cur;
    int raw = ident(), s = qualify(raw);
    for (const char* b : {"io", "clock", "random", "yield", "spawn", "chan"})
      if (symName(raw) == b) failAt(nt.p, nt.line, "@duplicate '%s' is a builtin effect", b);
    if (!effectBit(s)) failAt(nt.p, nt.line, "@limit too many effects (a program can declare at most %d)", EFF_LEVEL - EFF_BUILTIN);
    expect('{');
    EffectDef d{s, {}};
    std::vector<int> seen;
    while (!accept('}')) {
      if (atKw(S_pub)) fail("@misplaced an effect's operations are public when the effect is: put 'pub' before 'effect'");
      if (!atKw(K_fn)) fail("@syntax expected 'fn' (an operation: fn name(params)) in an effect");
      next();
      Tok ot = cur;
      int o = ident();
      if (std::find(seen.begin(), seen.end(), o) != seen.end()) failAt(ot.p, ot.line, "@duplicate duplicate operation '%s'", name(o));
      seen.push_back(o);
      std::string q = symName(s) + "." + symName(o);
      EffOpDef od{operationId(q), 0, {}, 0, {}, -1};
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
      if (od.arity > 7) failAt(ot.p, ot.line, "@limit an operation takes at most 7 parameters");
      if (accept(':')) {
        Ty r = type();
        od.rtype = r.code, od.rsite = typeSite("the result of " + q + "()", r.text);
      }
      if (at('{')) fail("@misplaced an effect's operations have no body: a handler gives one (handle { } with %s(...) { })", q.c_str());
      d.ops.push_back(std::move(od));
    }
    effDefs.push_back(std::move(d));
  }
  void skipBlock() {  // a balanced { ... }, unparsed
    expect('{');
    for (int d = 1; d; next()) {
      if (at(T_EOF)) fail("@syntax expected '}'");
      d += at('{') - at('}');
    }
  }
  // test "name" { body }: when testing, compiled as a function of no parameters, listed in Compiled::tests.
  // A body naming something not defined yet compiles to nothing and the test is pending (write the spec first).
  void testDecl() {
    int line = prev.line;
    const char* from = prev.p;  // `test`
    next();
    std::string nm;
    for (const char *p = prev.p, *e = p + prev.len; p < e; p++) nm += *p == '\\' && p + 1 < e ? *++p : *p;
    if (!at('{')) fail("@syntax expected '{' after the test's name");
    Mark body = mark();
    skipBlock();
    if (!testing) return;
    Mark after = mark();
    rewind(body);
    std::string so = std::exchange(owner, "");
    if (editorCheck) {
      Fun f;
      f.test = true;
      function(f, -1, false, "test \"" + nm + "\"", parserStub(), nullptr);
      owner = std::move(so);
      return;
    }
    TestCase tc{nm, file, "", line, nullptr, traceHash(from, after.prevEnd)};
    std::string sr = testRefs;
    testRefs = traceTestRefs(tc.hash);
    Fun f;
    f.test = true;
    size_t np = pend.size();
    auto* missing = std::exchange(testMissing, &tc.pending);
    u32* stub = parserStub();
    function(f, -1, false, "test \"" + nm + "\"", stub, nullptr);
    if (tc.pending.empty()) tc.code = stub;
    else pend.resize(np), rewind(after);
    testMissing = missing, owner = std::move(so), testRefs = std::move(sr);
    tests.push_back(std::move(tc));
  }
  void declare(const Tok& t, char kind, int arity) {
    Global& g = global(t.sym);
    if (g.kind == 'n') failAt(t.p, t.line, "@duplicate cannot redefine builtin '%s'", name(t.sym));
    if (char& k = growAt(declKind, t.sym); k && (kind != 'v' || k != 'v')) failAt(t.p, t.line, "@duplicate '%s' is already defined", name(t.sym));
    else k = kind;
    Global& staged = declarations.try_emplace(t.sym, g).first->second;
    staged.kind = kind;
    if (kind == 'f' && !staged.stub) staged.stub = parserStub();
    if (kind == 's' && (staged.cell = editorCheck ? 0 : sharedCell(symName(t.sym))) < 0) failAt(t.p, t.line, "@limit too many shared vars");
    mixEnv(uint64_t(t.sym) << 16 | uint64_t(kind) << 8 | uint64_t(arity), pubRaw), pubRaw = -1;
  }
  void indexDeclaration(const Tok& t, std::string_view kind, std::string_view container = {}) {
    int earlier = Navigation::get(nav->globals, t.sym);
    int id = nav->declare(indexSite(t), t, kind, container, earlier);
    if (id >= 0) {
      nav->globals[t.sym] = id;
      if (earlier >= 0) nav->reassigned.insert(id);
    }
  }
  std::string resolvePath(std::string_view rel) {
    std::string p(rel);
    if (size_t d = file.rfind('/'); p[0] != '/' && d != std::string::npos) p = file.substr(0, d + 1) + p;
    return editorCheck ? editorCheck->normalize(p) : canonicalPath(p);
  }
  // What `import "x"` names: a file of this package, by path; or another package, by name (one its manifest lists
  // with `dep`), meaning that package's root file. pkg: the package's prefix (its alias), when imported by name.
  struct Target { std::string path, pkg; };
  Target importTarget(const Tok& t) {
    std::string s(t.p, (size_t)t.len);
    const Package* here = pkgOf(file);
    if (s.empty()) failAt(t.p, t.line, "@syntax expected a file or package name");
    if (s.starts_with("std/") && !s.ends_with(".fut")) {  // a standard library package: no dep line needed
      std::string n = s.substr(4);
      const Package* p = n.empty() || n.find('/') != std::string::npos || stdRoot.empty() ? nullptr : packageAt(stdRoot + "/" + n);
      if (!p || p->name != n) failAt(t.p, t.line, "@import no standard library package '%s'%s", s.c_str(), stdRoot.empty() ? " (the standard library was not found: set FUT_STD to its directory)" : "");
      return linkPackage(t, p, here);
    }
    if (s.find('/') == std::string::npos && !s.ends_with(".fut")) {  // a package
      if (!here) failAt(t.p, t.line, "@import '%s' is a package name, and %s is in no package: put a fut.pkg next to it (name app, then dep %s <its directory>), or import a file by path (\"./%s.fut\")", s.c_str(), ::base(file).c_str(), s.c_str(), s.c_str());
      if (s == here->name) failAt(t.p, t.line, "@import this file is in package %s: import its files by path (\"./file.fut\")", s.c_str());
      const PackageDep* d = nullptr;
      for (auto& x : here->deps) if (x.name == s) d = &x;
      if (!d) failAt(t.p, t.line, "@import package %s does not list '%s': add 'dep %s <its directory>' to %s", here->name.c_str(), s.c_str(), s.c_str(), here->manifest.c_str());
      const Package* p = packageAt(d->dir);
      std::string at = here->manifest + ":" + std::to_string(d->line);
      if (!p) failAt(t.p, t.line, "@import %s has no fut.pkg (dep %s, %s)", d->dir.c_str(), s.c_str(), at.c_str());
      if (p->name != s) failAt(t.p, t.line, "@import the package in %s is named '%s', not '%s' (%s)", d->dir.c_str(), p->name.c_str(), s.c_str(), at.c_str());
      return linkPackage(t, p, here);
    }
    std::string ip = resolvePath(s);
    if (const Package* there = pkgOf(ip); there != here) {
      if (there) failAt(t.p, t.line, "@import '%s' is in package %s: import it by name (import \"%s\")%s", s.c_str(), there->name.c_str(), inStd(there->dir) ? ("std/" + there->name).c_str() : there->name.c_str(), here || inStd(there->dir) ? "" : ", from a package that lists it with dep");
      failAt(t.p, t.line, "@import '%s' is outside package %s: a package's files are the ones below its fut.pkg (%s)", s.c_str(), here->name.c_str(), here->dir.c_str());
    }
    return {ip, ""};
  }
  // An import of package p (by name, or from the standard library) from a file of package here (null: none).
  Target linkPackage(const Tok& t, const Package* p, const Package* here) {
    std::string s = p->name;
    {
      std::vector<std::string> chain{here ? here->name : ""};  // packages depend on each other in one direction only
      std::unordered_set<std::string> seen;
      std::function<bool(const Package*)> reaches = [&](const Package* q) {
        chain.push_back(q->name);
        if (q == here) return true;
        if (seen.insert(q->dir).second)
          for (auto& e : q->deps) if (const Package* r = packageAt(e.dir); r && reaches(r)) return true;
        return chain.pop_back(), false;
      };
      if (here && reaches(p)) {
        std::string c;
        for (auto& n : chain) c += (c.empty() ? "" : " -> ") + n;
        failAt(t.p, t.line, "@import packages can't depend on each other in a cycle: %s (move what both need into a package of its own)", c.c_str());
      }
      if (std::string& o = pkgNamed[(inStd(p->dir) ? "std/" : "") + p->name]; o.empty()) o = p->dir;  // std/x and x may both exist
      else if (o != p->dir) failAt(t.p, t.line, "@import two packages are named '%s': %s and %s", s.c_str(), o.c_str(), p->dir.c_str());
      std::string root = editorCheck ? editorCheck->normalize(p->root) : p->root;
      pkgOf(root);
      std::string& pre = pkgPreOf[p->dir];
      if (pre.empty()) for (pre = p->name; prefixTaken(pre, p->dir);) pre += "_";
      pkgRootOf[pre] = root, pkgImports[pkgPrefix(file)].insert(pre);
      return {std::move(root), pre};
    }
  }
  // A clause's condition: from L (after its keyword) to the end of its line, or to a '{' or '}' there (a function's
  // body, the class's end). Notes its calls and (in ensures: olds) each old(...). Leaves L before what ends it.
  void clauseScan(Lexer& L, Clause& c, std::vector<At>* olds) {
    Lexer b = L;
    Tok p, t = lex(L);
    if (t.nl || t.t == '{' || t.t == '}' || t.t == T_EOF) failAt(t.p, t.line, "@syntax expected a condition (on the same line)");
    c.at = {t.p, t.line, file, srcStart};
    for (int d = 0; t.t != T_EOF && !clauseEnds(d, p, t); b = L, p = t, t = lex(L), d += nesting(p)) {
      if (t.t != '(' || t.nl || !(p.t == ')' || p.t == ']' || (p.t == T_ID && (p.sym >= NKW || p.sym == K_fn)))) continue;
      if (p.t != T_ID || p.sym != S_old) c.calls = true;
      else if (!olds) failAt(p.p, p.line, "@misplaced old() is a value as it was on entry: it belongs in an ensures clause");
      else olds->push_back({p.p, p.line, file, srcStart});
    }
    L = b;
  }
  void skipType(Lexer& at) { ::skipType(at, [&](Lexer& s) { return lex(s); }); }
  bool isTypeDecl(const Lexer& at) {  // `type Name =` next (after `type`)? Otherwise `type` is an ordinary name
    Lexer s = at;
    Tok n = lex(s), e = lex(s);
    return n.t == T_ID && n.sym >= NKW && !n.nl && e.t == '=' && !e.nl;
  }
  // After the parameters, scan the result type, effects and contract. Leave L before the body, so braces in
  // a type or a clause aren't taken for the body's.
  void declScan(Lexer& L, const std::string& qn, const Tok& fnTok, int cls, int m) {
    Lexer s = L;
    SpecFn f;
    f.fn = {fnTok.p, fnTok.line, file, srcStart}, f.name = symName(m);
    for (;;) {
      if (Lexer r = s; lex(r).t == ':') { skipType(s = r), L = s; continue; }  // its result type
      Tok k = lex(s);
      if (k.t == '{') {
        Lexer r = s;
        Tok a = lex(r), o = lex(r), c = lex(r), e = lex(r);
        if (a.t == T_ID && a.sym == S_todo && o.t == '(' && c.t == ')' && e.t == '}') todos.push_back({qn, f.fn});
      }
      if (k.t == '{' || k.t == '}' || k.t == T_EOF) break;
      bool post = k.t == T_ID && k.sym == S_ensures;
      if (!post && !(k.t == T_ID && k.sym == S_requires)) { L = s; continue; }  // its effects
      Clause c;
      clauseScan(s, c, post ? &f.olds : nullptr);
      f.calls |= c.calls, (post ? f.post : f.pre).push_back(c), L = s;
    }
    if (f.pre.empty() && f.post.empty()) return;
    if (cls < 0) fnSpecs.push_back(qn);
    else classSpec(cls).methods.push_back(m);
    specFns[qn] = std::move(f);
  }
  // An enum's body, from its '{' (L) to past its '}': declares the enum and each variant (a class, its arity) and
  // notes their fields, which construction and match compile against.
  void enumScan(Lexer& L, Tok d) {
    int raw = pubRaw;
    bool pub = d.sym < (int)pubSym.size() && pubSym[d.sym];
    declare(d, 'c', 0);
    if (nav) indexDeclaration(d, "enum");
    EnumDef& e = enumDefs[d.sym] = {d.sym, {}};
    e.raw = intern(symName(d.sym).substr(symName(d.sym).rfind('.') + 1));
    lex(L);  // {
    for (Tok v = lex(L); v.t != '}'; v = lex(L)) {
      if (v.t == ',') continue;
      if (v.t == T_EOF) failAt(v.p, v.line, "@syntax expected '}' to end the enum");
      if (v.t != T_ID || v.sym < NKW) failAt(v.p, v.line, "@syntax expected a variant: a name, with its fields in parentheses if it has any (Circle(r: num))");
      for (auto& o : e.vs) if (o.raw == v.sym) failAt(v.p, v.line, "@duplicate duplicate variant '%s'", name(v.sym));
      VariantDef vd{v.sym, intern(symName(d.sym) + "." + symName(v.sym)), {}};
      if (Lexer r = L; lex(r).t == '(') {
        L = r;
        for (Tok a = lex(L); a.t != ')';) {
          if (a.t != T_ID || a.sym < NKW) failAt(a.p, a.line, "@syntax expected a field name");
          for (int f : vd.fields) if (f == a.sym) failAt(a.p, a.line, "@duplicate duplicate field '%s'", name(a.sym));
          vd.fields.push_back(a.sym);
          for (int d = 0; a = lex(L), a.t != T_EOF && (d > 0 || (a.t != ',' && a.t != ')'));)  // its type: map<str, num>, {x: num, y: num}
            d += (a.t == '(' || a.t == '{' || a.t == '[' || a.t == '<') - (a.t == ')' || a.t == '}' || a.t == ']' || a.t == '>') - 2 * (a.t == T_SHR);
          if (a.t == ',') a = lex(L);
          else if (a.t != ')') failAt(a.p, a.line, "@syntax expected ')' after the variant's fields");
        }
      }
      Tok vt = v;
      vt.sym = vd.sym, pubRaw = raw;
      declare(vt, 'c', (int)vd.fields.size());
      if (nav) indexDeclaration(vt, "variant", symName(d.sym));
      if (pub) growAt(pubSym, vd.sym) = true;
      for (int f : vd.fields) mixEnv(uint64_t(vd.sym) << 32 | uint64_t(f) << 2 | 1, raw);
      e.vs.push_back(std::move(vd));
    }
  }
  ClassSpec& classSpec(int cls) {
    auto [it, fresh] = classSpecs.try_emplace(cls);
    if (fresh) it->second.at = classAt[cls].first, it->second.name = symName(classAt[cls].second), classOrder.push_back(cls);
    return it->second;
  }
  // Token pre-pass: learn every top-level name, arity and fn/method source span, following imports.
  void prescan(const std::string& path, const char* src) {
    std::string savedFile = file;
    const char* savedStart = srcStart;
    bool wasScanning = std::exchange(prescanning, true);
    file = path, srcStart = src, aliases[path].clear(), plainModules[path].clear();
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
    const char* externStart = nullptr;
    bool ext = false;  // after `extern` (a C function: no body)
    // The parameters after a declaration's name: how many (a name after `(` or `,`; one after `:` is a type), and
    // whether its result is declared `: num` (and nothing else: `: num?` may be nil).
    auto params = [&](bool* retNum = nullptr, Lexer* after = nullptr) {
      Lexer s = L;
      int n = 0, last = '(';
      if (lex(s).t != '(') return 0;
      for (int d = 1; d > 0; ) {  // a name after `(` or `,` at the parameters' own level (not inside a {shape})
        Tok a = lex(s);
        if (a.t == T_EOF) return n;
        if (d == 1 && a.t == '=') diagnose(a.p, a.line, defaultArgument);
        if (d == 1 && (a.t == '}' || (a.t == '{' && last != ':' && last != '|' && last != '<' && last != '(' && last != ','))) return n;
        if (d == 1 && a.nl && a.t == T_ID && (a.sym == K_var || a.sym == K_class || a.sym == K_import)) return n;
        if ((d += nesting(a) + (a.t == '<') - (a.t == '>') - 2 * (a.t == T_SHR)) == 1) n += a.t == T_ID && (last == '(' || last == ',');
        last = a.t;
      }
      if (after) *after = s;
      if (Lexer r = s; retNum && lex(r).t == ':') {
        Tok a = lex(r), b = lex(r);
        *retNum = a.t == T_ID && symName(a.sym) == "num" && !((b.t == '?' || b.t == '|') && !b.nl);
      }
      return n;
    };
    for (Tok t = lex(L), p, pp; t.t != T_EOF; pp = p, p = t, t = lex(L)) {
      Lexer afterToken = L;
      const char* declaration = t.p;
      int here = depth;
      if (attempt([&] {
        bool afterKw = p.t == T_ID && t.t == T_ID && t.sym >= NKW;
        topTry |= tryWord(t) && open.empty();  // a try in top-level code
        if (!editorCheck && t.t == T_ID && !stepsOn) {  // names budget or stepCount: instrument the code (fuel.cpp)
          static const int b = intern("budget"), c = intern("stepCount");
          stepsOn = t.sym == b || t.sym == c;
        }
        if (t.t == T_ID && t.sym == S_pub && !depth) {
          Lexer s = L;
          Tok n = lex(s);
          Lexer s2 = s;
          Tok n2 = lex(s2);
          pubNext = pubDecl(n) || gpuFn(n, n2) || gpuVar(n, n2);
        } else if (t.t == T_ID && t.sym == K_extern && !depth) ext = true, externStart = t.p;
        else if (ext && afterKw && p.sym == K_fn) {
          declaration = externStart, ext = false;
          Tok d = q(t);
          declare(d, 'x', params());
          if (nav) indexDeclaration(d, "extern fn");
        } else if (t.t == '{') depth++;
        else if (t.t == '}') {
          if ((depth = std::max(0, depth - 1)) == 0) cls = -1, eff = -1;
          if (!open.empty() && depth == open.back().depth) {
            Span& sp = spans[open.back().q];
            sp.start = open.back().start, sp.end = L.p, sp.line = open.back().line, sp.endLine = L.line;
            open.pop_back();
          }
        } else if (afterKw && p.sym == K_fn && depth == 1 && eff >= 0) {  // an effect's operation
          declaration = p.p;
          int n = params();
          effOps[eff].push_back({t.sym, n});
          if (nav) nav->operations[{eff, t.sym}] = nav->declare(indexSite(t), t, "fn", symName(eff));
          mixEnv(uint64_t(eff) << 40 | uint64_t(t.sym) << 8 | uint64_t(n), -2);
        } else if (depth == 0 && afterKw && p.sym == S_enum && !t.nl && t.sym >= NKW && [&] { Lexer s = L; return lex(s).t == '{'; }()) {
          declaration = p.p;
          enumScan(L, q(t));
        } else if (depth == 0 && afterKw && p.sym == S_effect && !t.nl && [&] { Lexer s = L; return lex(s).t == '{'; }()) {
          declaration = p.p;
          Tok d = q(t);
          declare(d, 'e', 0);
          if (nav) indexDeclaration(d, "effect");
          effOps[eff = d.sym].clear();
        } else if (afterKw && p.sym == K_fn && (depth == 0 || (depth == 1 && cls >= 0))) {
          declaration = p.p;
          Tok d = depth ? t : q(t);
          std::string qn = depth ? symName(cls) + "." + symName(t.sym) : symName(d.sym);
          if (spans.count(qn)) failAt(t.p, t.line, "@duplicate '%s' is already defined", qn.c_str());
          Span& sp = spans[qn];
          Lexer after = L;
          int n = sp.arity = params(&sp.retNum, &after);
          open.push_back({qn, p.p, p.line, depth});
          if (!depth) declare(d, 'f', n | (sp.retNum ? 128 : 0));  // callers rely on a `: num` result
          if (nav) {
            if (!depth) indexDeclaration(d, "fn");
            else nav->methods[{cls, t.sym}] = nav->declare(indexSite(t), t, "method", symName(cls));
          }
          if (after.p != L.p) {
            L = after;
            declScan(L, qn, p, depth ? cls : -1, t.sym);
          }
        } else if (depth == 0 && t.t == T_ID && t.sym == S_type && isTypeDecl(L)) {  // type Name = <type>
          Tok n = lex(L);
          lex(L);  // =
          const char* from = L.p;
          Tok d = q(n);
          int raw = pubRaw;  // (declare clears it)
          declare(d, 't', 0);
          if (nav) indexDeclaration(d, "type");
          skipType(L);  // past it: its braces are a type's, not a block's
          mixEnv(fnv(from, L.p) ^ uint64_t(d.sym) << 32, raw);  // code that checks fields against it compiles against it
        } else if (depth == 0 && afterKw && (p.sym == K_class || p.sym == K_var)) {
          declaration = p.p;
          Tok d = q(t);
          bool shared = p.sym == K_var && pp.t == T_ID && pp.sym == S_shared && !p.nl;
          if (p.sym == K_var && gpuVar(pp, p)) gpuVarSyms.insert(d.sym);
          declare(d, p.sym == K_class ? 'c' : shared ? 's' : 'v', 0);
          if (nav) indexDeclaration(d, p.sym == K_class ? "class" : "var");
          if (p.sym == K_class) {
            enumDefs.erase(d.sym);  // (an enum by this name before a reload)
            cls = d.sym, classAt[cls] = {{t.p, t.line, file, srcStart}, t.sym};
            Lexer s = L;
            if (lex(s).t == ':') if (Tok n = lex(s); n.t == T_ID) {
              superAt[cls] = {n.p, n.line, file, srcStart};
              Lexer base = s;
              if (lex(base).t == '.' && lex(base).t == T_ID) s = base;
              mixEnv(fnv(n.p, s.p) ^ uint64_t(cls) << 32, -2);
            }
          }
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
        } else if (depth == 1 && cls >= 0 && t.t == T_ID && t.sym == S_invariant && (t.nl || p.t == '{') && [&] { Lexer s = L; return !lex(s).nl; }()) {
          Clause c;
          const char* from = L.p;
          clauseScan(L, c, nullptr), classSpec(cls).invariants.push_back(c);
          mixEnv(fnv(from, L.p) ^ uint64_t(cls) << 32, -1);  // editing one recompiles the class's methods
        } else if (depth == 1 && cls >= 0 && afterKw && p.sym == K_var) {  // a declared field: methods compile against them
          Lexer field = L;
          uint64_t signature = uint64_t(cls) << 32 | uint64_t(t.sym) << 2 | 1;
          if (lex(field).t == ':') {
            skipType(field);
            signature ^= fnv(L.p, field.p);
          }
          mixEnv(signature, -2);
        } else if (depth == 0 && t.t == T_STR && p.t == T_ID && p.sym == K_import) {
          declaration = p.p;
          bool re = pubNext;
          pubNext = false;
          size_t site = 0;
          if (editorCheck) {
            auto source = sourcePosition(p.p, p.line);
            const char* start = source.at;
            while (start > source.start && start[-1] != '\n') start--;
            site = editorCheck->imports.size();
            editorCheck->imports.push_back({path, "", source.line, int(source.at - start + 1)});
          }
          Target tg = importTarget(t);
          std::string ip = tg.path;
          if (editorCheck) editorCheck->imports[site].to = ip;
          Lexer s = L;
          Tok a = lex(s), n = a.t == T_ID && a.sym == S_as ? lex(s) : Tok{};
          if (a.t == T_ID && a.sym == S_as && (n.t != T_ID || n.sym < NKW)) failAt(n.p, n.line, "@syntax expected a %s name after 'as'", tg.pkg.empty() ? "module" : "package");
          if (re && !tg.pkg.empty()) failAt(t.p, t.line, "@import 'pub import' re-exports a file of this package; a package's dependencies are not part of its interface");
          if (re && n.t == T_ID) failAt(n.p, n.line, "@import 'pub import' re-exports the file's pub names as they are: leave out 'as'");
          if (re) reexports[path].push_back(ip);
          auto claim = [&](int a, const std::string& m) {  // one name, one module: a second import can't quietly take it
            auto [it, fresh] = aliases[path].emplace(a, m);
            if (!fresh && it->second != m)
              failAt(n.t == T_ID ? n.p : t.p, n.t == T_ID ? n.line : t.line, "@import '%s' already names another import in this file: give one of them a different name with 'as'", symName(a).c_str());
            if (nav && n.t == T_ID) {
              auto site = indexSite(n);
              auto key = std::pair{site.file, a};
              int id = nav->declare(site, n, "import", "", Navigation::get(nav->aliases, key));
              if (id >= 0) nav->aliases[key] = id;
            }
          };
          if (!tg.pkg.empty()) claim(n.t == T_ID ? n.sym : intern(::base(std::string(t.p, (size_t)t.len))), tg.pkg);  // the package's name
          if (sources.count(ip) && !nsOf.count(ip)) return;  // the program's own file: already here, with global names
          std::string& pre = nsOf[ip];
          if (pre.empty()) {  // the file's stem (after its package's prefix), made unique
            pre = ip.substr(ip.rfind('/') + 1), pre = pre.substr(0, pre.find('.'));
            if (const std::string& P = pkgPrefix(ip); !P.empty()) pre = P + "/" + pre;
            while (prefixTaken(pre, ip)) pre += "_";
          }
          if (tg.pkg.empty() && n.t == T_ID) {  // import "file" as name: a module (unless an earlier import already made its pub names global)
            claim(n.sym, pre);
            if (!sources.count(ip)) isModule[ip] = true;
          } else if (tg.pkg.empty() && isModule[ip]) plainModules[path].push_back(ip);
          if (sources.count(ip)) return;
          Source& imported = sources[ip];
          if (!readSource(ip, imported.text, editorCheck)) {
            unavailableModules.insert(tg.pkg.empty() ? pre : tg.pkg);
            failAt(t.p, t.line, "@import cannot read '%s'", ip.c_str());
          }
          prepareSource(ip, imported);
          files.push_back(ip), prescan(ip, imported.code().c_str());
        }
      })) continue;
      invalidDecls[declaration] = errorPos;
      L = afterToken, depth = here, pubNext = false, pubRaw = -1;
    }
    file = savedFile, srcStart = savedStart, prescanning = wasScanning;
  }

  // ---- top level ----
  bool atPub() {  // `pub` before a top-level declaration (the prescan noted it); elsewhere `pub` is an ordinary name
    if (!atKw(S_pub)) return false;
    Tok n = peek();
    Lexer s = lx;
    s.next();
    Tok n2 = s.next();
    return pubDecl(n) || gpuFn(n, n2) || gpuVar(n, n2);
  }
  // gpu fn: also translated to WGSL (wgsl.cpp), after the whole program is compiled
  static bool gpuFn(const Tok& a, const Tok& b) {
    static const int gpu = intern("gpu");
    return a.t == T_ID && a.sym == gpu && b.t == T_ID && b.sym == K_fn && !b.nl;
  }
  // gpu var: a constant the file's gpu fns read (a number, or a list of them, or of such lists). It never changes:
  // assigning it is a compile error and its value is frozen, so the CPU and the GPU see the same one.
  static bool gpuVar(const Tok& a, const Tok& b) {
    static const int gpu = intern("gpu");
    return a.t == T_ID && a.sym == gpu && b.t == T_ID && b.sym == K_var && !b.nl;
  }
  std::unordered_set<int> gpuVarSyms;  // qualified: they can't be assigned
  bool gpuFreeze = false;               // the top-level var being compiled is a gpu var: freeze its value
  struct GpuDecl { std::string file; const char* src; size_t i; int q; };
  std::unordered_map<std::string, GpuFile> gpuFiles;  // by file: its gpu fns, gpu vars and types
  std::vector<std::string> gpuOrder;                 // the files of gpuFiles, as first seen
  GpuFile& gpuFile() {
    auto [it, fresh] = gpuFiles.try_emplace(file);
    if (fresh) gpuOrder.push_back(file), it->second.path = file;
    it->second.src = srcStart;
    return it->second;
  }
  bool gpuBody = false;                              // compiling a gpu fn: v.x = e makes v a new vector; its result's checks are wgsl.cpp's ([gpu])
  std::vector<GpuDecl> gpuDecls;
  std::vector<std::pair<int, GpuInfo>> gpuInfos;
  void gpuDecl() {  // at `fn`
    const char* p = cur.p;
    int line = cur.line;
    next();
    Tok n = cur;
    struct Back { bool& b; ~Back() { b = false; } } back{gpuBody};  // also when the body fails
    gpuBody = true;
    fnDecl();
    auto& fs = gpuFile().fns;
    fs.push_back({n.sym, p, line});
    gpuDecls.push_back({file, srcStart, fs.size() - 1, qualify(n.sym)});
  }
  void checkGpu() {
    if (gpuDecls.empty()) return;
    std::unordered_set<std::string> stems;
    for (auto& path : gpuOrder) {  // each file's name as a WGSL identifier, made unique: its gpu fns are stem_name
      GpuFile& g = gpuFiles[path];
      if (auto it = aliases.find(path); it != aliases.end()) g.aliases = &it->second;
      if (g.fns.empty() && g.vars.empty()) continue;  // (types only: structs keep their names)
      std::string b = path.substr(path.find_last_of("/\\") == std::string::npos ? 0 : path.find_last_of("/\\") + 1);
      if (b.size() > 4 && b.compare(b.size() - 4, 4, ".fut") == 0) b.resize(b.size() - 4);
      for (char& c : b) if (!isalnum((unsigned char)c)) c = '_';
      g.stem = b.empty() || isdigit((unsigned char)b[0]) || b[0] == '_' ? "m" + b : b;
      while (!stems.insert(g.stem).second) g.stem += "_";
    }
    GpuModule module = [&](const std::string& m) -> const GpuFile* {
      for (auto& [ip, pre] : nsOf)
        if (pre == m)
          if (auto it = gpuFiles.find(ip); it != gpuFiles.end()) return &it->second;
      return nullptr;
    };
    for (auto& d : gpuDecls) {
      auto fail = [&](const GpuFile& in, const char* p, int line, const std::string& m) {
        file = in.path, srcStart = in.src;
        failAt(p, line, "@gpu %s", m.c_str());
      };
      attempt([&] { gpuInfos.emplace_back(d.q, wgslOf(gpuFiles[d.file], d.i, module, fail)); });
    }
  }
  void topLevel() {
    while (!at(T_EOF)) item(Top, [&] {
      const char* item = cur.p;  // where the declaration starts, `pub` included (fut test --changed hashes it)
      bool trace = traceOn && testing && !editorCheck;
      if (atPub()) next();
      if (skipInvalid(Top)) return;
      if (atKw(K_fn)) { next(), fnDecl(); if (trace) traceFn(item); return; }
      if (at(T_ID) && [&] { Lexer s = lx; return gpuFn(cur, s.next()); }()) { next(); if (skipInvalid(Top)) return; gpuDecl(); if (trace) traceFn(item); return; }
      if (atKw(K_class)) { next(), classDecl(); if (trace) traceClassDecl(item); return; }
      if (atKw(K_extern)) { next(), externDecl(); return; }
      if (atKw(S_effect) && peek().t == T_ID && !peek().nl && peek().sym >= NKW) { next(), effectDecl(); return; }
      if (atKw(S_enum) && peek().t == T_ID && !peek().nl && peek().sym >= NKW) { next(), enumDecl(); if (trace) traceEnum(item); return; }
      if (atKw(S_test) && peekIs(T_STR)) { next(), testDecl(); if (trace) traceCuts.push_back({item, prevEnd}); return; }
      if (atKw(S_type) && isTypeDecl(lx)) {  // type Name = <type>: checked where it is used, by name
        gpuFile().types.push_back({peek().sym, cur.p, cur.line});  // (a gpu fn's struct, if one uses it)
        next();
        Tok n = cur;
        int s = qualify(ident());
        expect('=');
        const char* start = nav ? tokStart(cur) : nullptr;
        Ty t = type();
        if (resolveNamed(t.code, s)) failAt(n.p, n.line, "@unknown-type type %s is defined as itself", name(s));
        typeDefs[s] = t.code;
        if (nav)
          if (int id = Navigation::get(nav->globals, s); id >= 0)
            nav->out.declarations[id].signature = "type " + symName(n.sym) + " = " + std::string(start, prevEnd);
        return;
      }
      if (atKw(K_import)) {
        next();
        if (!at(T_STR)) fail("@syntax expected a file or package name");
        next();
        std::string ip = importTarget(prev).path;
        if (at(T_ID) && cur.sym == S_as) next(), ident();
        if (auto it = sources.find(ip); it != sources.end() && !it->second.compiled) {
          Source& s = it->second;
          s.compiled = true;
          detour(ip, s.code().c_str(), s.code().c_str(), 1, [&] { topLevel(); });
        }
        return;
      }
      if (at(T_ID) && [&] { Lexer s = lx; return gpuVar(cur, s.next()); }()) {
        next();
        gpuFile().vars.push_back({peek().sym, cur.p, cur.line});
        gpuFreeze = true;
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
        auto old = ::varText.find(v);
        uint64_t& h = varText.try_emplace(v, old != ::varText.end() ? old->second : 0).first->second, t = fnv(start, end);
        if (reload && h && h != t) keep = true, add(reset, symName(v));
        h = t;
        if (trace) traceCuts.push_back({item, end}), traceDefine("v:" + symName(v), traceHash(item, end));
      }
      if (!keep) F->k.c.resize(c), F->k.rel.resize(r), F->k.lines.resize(l);
    });
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
    for (auto& [P, h] : envOf) {
      uint64_t e = h;
      for (auto& D : pkgImports[P]) e = (e ^ surfEnv[D]) * 1099511628211ull;
      envFinal[P] = e;
    }
    invariantClasses();
    file = path, srcStart = src, lx = Lexer{src}, cur = {}, next();
    Fun top;
    top.top = true, top.noRegs = topTry, top.k.name = "<top>", top.k.file = path, F = &top;
    topLevel();
    if (!modulesPending.empty()) compileFail("waiting for the host to load modules", false);
    if (testing && !editorCheck && reports.empty()) {
      todosPending(), specProperties();
      if (traceOn) traceFiles();
    }
    checkClasses();
    checkGpu();
    if (inferEffectsWanted && reports.empty()) inferEffects();
    for (auto& [sym, v] : varTypes) attempt([&] { globalType(sym); });
    if (editorCheck || !reports.empty()) return {};
    Code chunk = finish();
    return commit(chunk);
  }

  // ---- fut test --changed: the hashes a test's trace is checked against (testcache.cpp) ----
  // The tokens of [a, b) outside the ranges in skip (sorted), and not `override`, with the line breaks between them
  // (they can end a statement): comments and other spacing don't count, nor where in its file the text is. Mixed in:
  // every top-level declaration of each name the text mentions, wherever it is, so a new declaration that a name now
  // resolves to (a file's private `helper` hiding the global one) changes the hash of the code that names it.
  std::unordered_map<std::string, uint64_t> declsNamed;  // a name as written -> its declarations (qualified, kind, pub)
  uint64_t traceHash(const char* a, const char* b, const std::vector<std::pair<const char*, const char*>>& skip = {}) {
    if (declsNamed.empty())
      for (size_t s = 0; s < declKind.size(); s++)
        if (char k = declKind[s]) {
          const std::string& q = symName((int)s);
          std::string e = q + ' ' + k + (s < pubSym.size() && pubSym[s] ? " pub" : "");
          declsNamed[q.substr(q.rfind('.') + 1)] += fnv(e.data(), e.data() + e.size());  // (order-free)
        }
    uint64_t h = 1469598103934665603ull, names = 0;
    std::unordered_set<int> seen;
    size_t i = 0;
    Lexer L{a};
    for (Tok k = L.next(); k.t != T_EOF && k.t != T_ERR && k.p < b; k = L.next()) {
      while (i < skip.size() && skip[i].second <= k.p) i++;
      if ((i < skip.size() && skip[i].first <= k.p) || (k.t == T_ID && k.sym == S_override)) continue;
      h = (h ^ (uint64_t)k.t ^ (uint64_t)k.nl << 32) * 1099511628211ull;
      for (const char* c = k.p; c < L.p; c++) h = (h ^ (uint8_t)*c) * 1099511628211ull;
      auto named = [&](const std::string& n) {
        if (auto it = declsNamed.find(n); it != declsNamed.end()) names += it->second;
      };
      if (k.t == T_ID && k.sym >= NKW && seen.insert(k.sym).second) named(symName(k.sym));
      if (k.t == T_STR && memchr(k.p, '{', k.len))  // "{name}" interpolates: every word in it (a few more is harmless)
        for (const char *c = k.p, *e = k.p + k.len; c < e;) {
          if (!idc(*c)) { c++; continue; }
          const char* w = c;
          while (c < e && (idc(*c) || dig(*c))) c++;
          if (int sym = intern({w, size_t(c - w)}); sym >= NKW && seen.insert(sym).second) named(symName(sym));
        }
    }
    return (h ^ names) * 1099511628211ull;
  }
  void traceFn(const char* item) {
    traceCuts.push_back({item, prevEnd});
    traceDefine("f:" + symName(fnDefs.back().first), traceHash(item, prevEnd));
  }
  // A class: each method's text, and its shape (the rest: its superclass, fields, invariants), which its methods and
  // lookups on its instances depend on, with its superclasses' (testcache.cpp chains them).
  void traceClassDecl(const char* item) {
    traceCuts.push_back({item, prevEnd});
    ClassDef& cd = classes.back();
    std::string K = symName(cd.sym);
    std::vector<std::pair<const char*, const char*>> bodies;
    for (auto& [m, f] : cd.methods) {
      std::string q = K + "." + symName(m);
      traceMethod(q, K);
      if (m == invariantSym()) { traceDefine("f:" + q, 1); continue; }  // (its clauses are in the shape)
      Span& sp = spans.at(q);
      bodies.push_back({sp.start, sp.end}), traceDefine("f:" + q, traceHash(sp.start, sp.end));
    }
    std::sort(bodies.begin(), bodies.end());
    traceClass(K, cd.super >= 0 ? symName(cd.super) : "", traceHash(item, prevEnd, bodies));
  }
  void traceEnum(const char* item) {  // an enum and its variants: classes whose shape is the enum's text
    traceCuts.push_back({item, prevEnd});
    uint64_t h = traceHash(item, prevEnd);
    int e = -1;
    for (auto it = classes.rbegin(); it != classes.rend() && e < 0; ++it) if (it->sum == 1) e = it->sym;
    for (auto& cd : classes) if (cd.sum && (cd.sym == e || cd.super == e)) traceClass(symName(cd.sym), cd.super >= 0 ? symName(cd.super) : "", h);
  }
  void traceFiles() {  // each file's top-level code other than its fns, classes, vars and tests
    std::sort(traceCuts.begin(), traceCuts.end());
    for (auto& f : files) {
      const std::string& text = sources.at(f).code();
      const char *a = text.c_str(), *b = a + text.size();
      std::vector<std::pair<const char*, const char*>> cuts;
      for (auto& c : traceCuts) if (c.first >= a && c.first < b) cuts.push_back(c);
      traceDefine("t:" + canonicalPath(f), traceHash(a, b, cuts));
    }
  }

  // The classes whose methods check invariants when an outside call ends: each with invariants, its superclasses
  // (a method they define may run on an instance of it) and its subclasses (they inherit its invariants).
  void invariantClasses() {
    std::unordered_map<int, int> sup;
    for (auto& [c, at] : superAt) attempt([&] { detour(at.file, at.src, at.p, at.line, [&] { ident(), sup[c] = topName(prev).sym; }); });
    auto up = [&](int k) { auto it = sup.find(k); return it == sup.end() ? -1 : it->second; };
    auto has = [&](int k) { auto it = classSpecs.find(k); return it != classSpecs.end() && !it->second.invariants.empty(); };
    for (auto& [d, _] : classAt)
      for (int k = d, n = 0; k >= 0 && n < 256; k = up(k), n++)
        if (has(k)) {
          for (int a = d, m = 0; a >= 0 && m < 256; a = up(a), m++) invClasses.insert(a);
          break;
        }
  }
  // A function whose body is todo() is a pending test: its signature and contract are written, its code isn't yet.
  void todosPending() {
    for (auto& [q, at] : todos) tests.push_back({q + "()", at.file, q + "() is not implemented yet", at.line});
  }
  // Under fut test, each function with a contract, and each class with one (its invariants, or methods with
  // contracts), becomes a property test: the function on random inputs of its parameters' types, or an instance
  // under random calls of its methods with contracts (all of them, for a class with only invariants). An input that breaks a
  // requires is thrown away; any other error (an ensures, the invariant, a runtime error) fails the test, with the
  // input shrunk to a small one that still fails. The test is fut code the compiler writes (on one line, so traces
  // name the function's or class's line), compiled in its file; prop@ natives (test.cpp) generate, shrink and report.
  std::deque<std::string> synth;  // the text of those tests (compiled here; nothing keeps pointing into it after)
  static bool generatable(uint64_t t) { return (t & 15) >= TY_ANY && (t & 15) <= TY_MAP; }
  bool paramTypes(const std::string& q, std::vector<uint64_t>& out) {  // q's parameter types, if every one can be generated
    auto it = sigs.find(q);
    if (!spans.count(q) || it == sigs.end()) return false;
    out.assign(it->second.types.begin(), it->second.types.end() - 1);  // (the last is the result's)
    return std::all_of(out.begin(), out.end(), generatable);
  }
  static std::string argList(const char* v, size_t n) {
    std::string a;
    for (size_t i = 0; i < n; i++) a += (i ? ", " : "") + std::string(v) + "[" + std::to_string(i) + "]";
    return a;
  }
  // The numbers written in q (its contract and body), each with its neighbors: random inputs try them.
  void spanNums(const std::string& q, std::vector<double>& out) {
    auto it = spans.find(q);
    if (it == spans.end() || !it->second.start) return;
    Lexer l{it->second.start, it->second.line, true};
    for (Tok t = l.next(); t.t != T_EOF && t.p < it->second.end; t = l.next())
      if (t.t == T_NUM)
        for (double d : {t.n, t.n - 1, t.n + 1})
          if (out.size() < 48 && std::find(out.begin(), out.end(), d) == out.end()) out.push_back(d);
  }
  // How to make q's arguments from generated values, in the current file (see PropSite::shape): types gets the
  // values' types, code each argument's code over a@, and pre the statements that build its class arguments. A
  // class argument is built by its init (or with none, C()) from generated arguments of its own; when building one
  // throws, the input is thrown away (prop@unbuilt). False: an argument can't be made.
  bool argPlan(const std::string& q, std::vector<uint64_t>& types, std::vector<std::pair<std::string, int>>& shape, std::string& pre, std::vector<std::string>& code, int depth = 0) {
    auto it = sigs.find(q);
    if (!spans.count(q) || it == sigs.end()) return false;
    std::vector<uint64_t> params(it->second.types.begin(), it->second.types.end() - 1);
    for (uint64_t t : params) {
      if (generatable(t)) {
        code.push_back("a@[" + std::to_string(types.size()) + "]"), types.push_back(t), shape.push_back({"", 0});
        continue;
      }
      uint64_t r = resolveNow(t) & ~TY_OPT;
      int c = (r & 15) == TY_CLS ? int(r >> 8) : -1;
      if (c < 0 || depth >= 2 || enumOf(c) >= 0) return false;
      std::string full = symName(c), raw = full.substr(full.rfind('.') == std::string::npos ? 0 : full.rfind('.') + 1);
      if (globalSym(intern(raw)) != c) return false;  // not reachable by its plain name here
      int owner = -1;
      for (int s = c, hops = 0; s >= 0 && owner < 0; hops++) {  // its init, or the one it inherits (classes this compile defines)
        auto cd = std::find_if(classes.begin(), classes.end(), [&](auto& x) { return x.sym == s; });
        if (cd == classes.end() || hops > 16) return false;
        if (spans.count(symName(s) + ".init")) owner = s;
        s = cd->super;
      }
      size_t at = shape.size();
      shape.push_back({raw, 0});
      std::vector<std::string> sub;
      if (owner >= 0 && !argPlan(symName(owner) + ".init", types, shape, pre, sub, depth + 1)) return false;
      shape[at].second = (int)sub.size();
      std::string b = "b@" + std::to_string(at), args;
      for (auto& x : sub) args += (args.empty() ? "" : ", ") + x;
      pre += "var " + b + " = nil; try { " + b + " = " + raw + "(" + args + ") } catch x@ { throw \"prop@unbuilt\" }; ";
      code.push_back(b);
    }
    return true;
  }
  bool isTodo(const std::string& q) { return std::any_of(todos.begin(), todos.end(), [&](auto& t) { return t.first == q; }); }
  void specProperties() {
    struct One { std::string n; At at; bool isFn; const ClassSpec* cs; std::string raw; };
    std::vector<One> all;
    for (auto& q : fnSpecs) all.push_back({q, specFns.at(q).fn, true, nullptr, specFns.at(q).name});
    for (int c : classOrder) all.push_back({symName(c), classSpecs.at(c).at, false, &classSpecs.at(c), classSpecs.at(c).name});
    for (auto& d : all) {
      if (inStd(d.at.file) && !inStd(canonicalPath(entryPath))) continue;  // the standard library's are tested by its own tests
      std::string n = d.n, call;
      PropSite ps;
      ps.file = d.at.file, ps.cls = !d.isFn, ps.names = {n};
      if (d.isFn) {
        std::vector<uint64_t> t;
        std::vector<std::string> code;
        std::string pre, keep = file;
        file = d.at.file;
        bool ok = !isTodo(n) && argPlan(n, t, ps.shape, pre, code);
        file = keep;
        if (!ok) continue;
        if (std::all_of(ps.shape.begin(), ps.shape.end(), [](auto& x) { return x.first.empty(); })) ps.shape.clear();
        ps.types.push_back(t), ps.line = d.at.line;
        spanNums(n, ps.nums);
        ps.text = n + "() to keep its contract on random inputs", ps.discard = "the caller broke " + n + "()'s contract:";
        call = pre + d.raw + "(";
        for (size_t i = 0; i < code.size(); i++) call += (i ? ", " : "") + code[i];
        call += ")";
      } else {
        std::vector<uint64_t> t;
        if (isTodo(n + ".init") || (spans.count(n + ".init") && !paramTypes(n + ".init", t))) continue;
        ps.types.push_back(t), ps.line = d.at.line;
        std::vector<int> ms;  // its methods with contracts, else (only invariants) every method
        for (int m : d.cs->methods) if (m != S_init) ms.push_back(m);
        if (ms.empty() && d.cs->methods.empty())
          for (auto& cd : classes) if (symName(cd.sym) == n) for (auto& [m, f] : cd.methods) if (m != S_init && m != invariantSym()) ms.push_back(m);
        std::string dispatch;
        for (int m : ms) {
          std::vector<uint64_t> mt;
          if (isTodo(n + "." + symName(m)) || !paramTypes(n + "." + symName(m), mt)) continue;
          dispatch += std::string(ps.names.size() > 1 ? " else " : "") + "if m@ == " + std::to_string(ps.names.size() - 1) + " { o@." + symName(m) + "(" + argList("x@", mt.size()) + ") }";
          ps.names.push_back(symName(m)), ps.types.push_back(mt);
        }
        if (ps.names.size() == 1 && d.cs->invariants.empty()) continue;
        spanNums(n + ".init", ps.nums);
        for (size_t i = 1; i < ps.names.size(); i++) spanNums(n + "." + ps.names[i], ps.nums);
        ps.text = n + " to keep its contract over random calls", ps.discard = "the caller broke " + n + ".";
        call = "var x@ = a@[0]; var o@ = " + d.raw + "(" + argList("x@", t.size()) + "); for c@ in a@[1] { var m@ = c@[0]; x@ = c@[1]; try { " +
               dispatch + " } catch e@ { if !prop@discard(S@, e@) { throw e@ } } }";
      }
      // (its own names end in '@', so they can't hide the program's)
      std::string& src = synth.emplace_back(
          "test \"contract " + n + (d.isFn ? "() on random inputs" : " on random calls") + "\" { var S@ = " + std::to_string(propSite(ps)) +
          "; var f@ = fn(a@) { " + call + " }; var ok@ = 0; var n@ = 0; while ok@ < 100 && n@ < 1000 { var in@ = prop@gen(S@, n@); n@ += 1; "
          "var threw@ = false; var e@ = nil; try { f@(in@) } catch x@ { threw@ = true; e@ = x@ }; if !threw@ { ok@ += 1 } else if !prop@discard(S@, e@) { var k@ = 0; var steps@ = 0; "
          "while steps@ < 2000 { var c@ = prop@shrink(S@, in@, k@); if c@ == nil { break }; steps@ += 1; var threw2@ = false; var e2@ = nil; "
          "try { f@(c@) } catch x@ { threw2@ = true; e2@ = x@ }; if threw2@ && !prop@discard(S@, e2@) { in@ = c@; e@ = e2@; k@ = 0 } else { k@ += 1 } }; "
          "prop@fail(S@, in@, e@, n@) } }; prop@done(S@, ok@, n@) }");
      Mark m = mark();
      file = d.at.file, srcStart = src.c_str(), lx = Lexer{srcStart, ps.line, true}, next(), next();
      testDecl();
      rewind(m);
    }
  }
  // ---- checks that need every class: overrides, and `self.name` against declared fields ----
  // A class as this compile defines it, else as the running program has it (a class defined at the REPL may
  // extend one from the program).
  struct ClassView { int super = -1; std::vector<std::pair<int, int>> methods; const std::vector<std::pair<int, V>>* fields = nullptr; const std::vector<Class::FieldType>* types = nullptr; };
  void diagnoseHere(const At& a, const char* fmt, const std::string& x, const std::string& y = "", const std::string& z = "", const std::string& w = "") {
    file = a.file, srcStart = a.src, lexAtEnd = false;
    diagnose(a.p, a.line, fmt, x.c_str(), y.c_str(), z.c_str(), w.c_str());
  }
  static std::string params(int n) { return std::to_string(n) + (n == 1 ? " parameter" : " parameters"); }
  // A value's static type as a kind a check compares: list<T> is a list, a literal type its kind; 0 if not one.
  uint64_t valueKind(uint64_t v) {
    if (v == TY_NILLIT) return v;
    uint64_t opt = v & TY_OPT;
    v = resolveNow(v);
    opt |= v & TY_OPT, v &= ~TY_OPT;
    if ((v & 15) == TY_EXT) {
      const TypeDesc* x = typeDesc(v);
      if (!x) return 0;
      if (x->k == 'L') v = TY_LIST;
      else if (x->k == 'M') v = TY_MAP;
      else if (x->k == 'l') v = x->lit == 's' ? TY_STR : x->lit == 'n' ? TY_NUM : x->v == NIL ? TY_NILLIT : TY_BOOL;
      else return 0;  // a union or a shape: not one kind
      if (v == TY_NILLIT) return v;
    }
    return (v & 15) >= TY_NUM && (v & 15) <= TY_CLS ? v | opt : 0;
  }
  std::string staticText(uint64_t v) {  // for messages: what the value is known to be
    v = valueKind(v);
    if (v == TY_NILLIT) return "nil";
    std::string s;
    switch (v & 15) {
      case TY_NUM: s = "a number"; break;
      case TY_STR: s = "a string"; break;
      case TY_BOOL: s = "a bool"; break;
      case TY_LIST: s = "a list"; break;
      case TY_MAP: s = "a map"; break;
      case TY_FN: s = "a function"; break;
      case TY_CLS: {
        int c = int(v >> 8), e = enumOf(c);
        const VariantDef* vd = nullptr;
        if (e >= 0 && e != c) for (auto& x : enumDefs.at(e).vs) if (x.sym == c) vd = &x;
        s = vd && vd->fields.empty() ? symName(c) : (vd ? "a " : "an instance of ") + symName(c);  // Shape.Empty, a Shape.Circle
        break;
      }
      default: return "a value";
    }
    return v & TY_OPT ? s + " or nil" : s;
  }
  // Whether no value of static type v passes a check of declared type d, mirroring types.cpp accepts. Both must be
  // known; anything unknown (0, any, a union value, a shape value) is not a clash. related(a, b): could an instance be
  // both class a's and class b's (one is the other's ancestor, or the compiler can't tell).
  template <class Related> bool contradicts(uint64_t d, uint64_t v, Related& related, int depth = 0) {
    if (!d || !v || depth > 8) return false;
    d = resolveNow(d);
    const TypeDesc* x = (d & 15) == TY_EXT ? typeDesc(d) : nullptr;
    if ((v = valueKind(v)) == TY_NILLIT) {
      if (d & TY_OPT) return false;
      switch (d & 15) {
        case TY_NUM: case TY_STR: case TY_BOOL: case TY_LIST: case TY_MAP: case TY_FN: case TY_CLS: case TY_BUF: return true;
        case TY_EXT:
          if (!x) return false;
          if (x->k == 'u') return std::all_of(x->alts.begin(), x->alts.end(), [&](uint64_t a) { return contradicts(a, v, related, depth + 1); });
          if (x->k == 'l') return !(x->lit == 'v' && x->v == NIL);
          return x->k == 's' || x->k == 'L' || x->k == 'M';
        default: return false;
      }
    }
    if (!v) return false;
    if (v & TY_OPT) {  // it may be nil: a clash only if nil fails too
      if (!contradicts(d, TY_NILLIT, related, depth + 1)) return false;
      v &= ~TY_OPT;
    }
    uint64_t k = v & 15;
    switch (d & 15) {
      case TY_NUM: case TY_STR: case TY_BOOL: case TY_LIST: case TY_MAP: case TY_FN: return k != (d & 15);
      case TY_BUF: return true;  // no literal, constructor or declared kind here is a buffer
      case TY_CLS: return k != TY_CLS || !related(int(v >> 8), int(d >> 8));
      case TY_EXT:
        if (!x) return false;
        switch (x->k) {
          case 'u': return std::all_of(x->alts.begin(), x->alts.end(), [&](uint64_t a) { return contradicts(a, v, related, depth + 1); });
          case 'l': return x->lit == 's' ? k != TY_STR : x->lit == 'n' ? k != TY_NUM : x->v == NIL || k != TY_BOOL;
          case 's': return k != TY_MAP && k != TY_CLS;  // a shape takes a map or an instance with its fields
          case 'L': return k != TY_LIST;
          case 'M': return k != TY_MAP;
        }
        return false;
      default: return false;
    }
  }
  // What each function does, followed through its calls (see EffFn): grown from what each does itself until nothing
  // changes. A method call's callees are the methods of that name taking that many arguments: in the receiver's class,
  // its superclasses and subclasses when its class is known; else every class's, which is certain only when one class
  // has such a method and no builtin has the name. Otherwise what they do is what the call may do ("may", with the
  // names), apart from what it does for sure. A call of a name no class has as a method may be a map's function (it
  // can't be followed). Channel and task methods do chan, and send may resume a fiber's code.
  std::vector<FnEffects> inferred;
  void inferEffects() {
    std::unordered_map<int, int> superOf;
    std::unordered_map<int, std::vector<std::pair<int, std::string>>> named;  // method -> (class, its EffFn key)
    for (auto& cd : classes) {
      superOf[cd.sym] = cd.super;
      for (auto& [m, f] : cd.methods) named[m].push_back({cd.sym, symName(cd.sym) + "." + symName(m)});
    }
    auto below = [&](int a, int b) {  // a is b or one of its subclasses
      for (int n = 0; a >= 0 && n < 1000; a = superOf.contains(a) ? superOf[a] : -1, n++) if (a == b) return true;
      return false;
    };
    static const int recv = intern("recv"), tryRecv = intern("tryRecv"), close = intern("close"), join = intern("join");
    static const int builtins[] = {S_len, S_pop, S_push, S_has, S_remove, S_keys, S_values, S_send, S_done, intern("slice"), intern("byte"),
                                   intern("find"), intern("split"), intern("trim"), intern("bytes"), intern("cross"), intern("dot"),
                                   intern("fill"), intern("length"), intern("normalize"), intern("toList"), recv, tryRecv, close, join};
    struct R { uint64_t eff = 0, maybe = 0, calls = 0; bool unknown = false; std::set<int> via; };
    std::unordered_map<std::string, R> r;
    for (auto& [k, e] : effFns) {
      if (e.declared) { r[k] = {e.mask}; continue; }
      R x{e.ops, 0, e.calls, e.unknown};
      for (size_t j = 0; j < e.params.size() && j < 64; j++) if (x.calls >> j & 1 && e.assigned.contains(e.params[j])) x.unknown = true;
      for (int c : e.called) if (e.assigned.contains(c)) x.unknown = true;
      r[k] = x;
    }
    // What a call reaches (x: what it does besides). sure: whether they are what it calls, not just what it may.
    auto callees = [&](const EffSite& c, R& x, std::vector<std::string>& out, bool& sure) {
      sure = true;
      auto all = [&](int m, auto keep) {
        if (auto it = named.find(m); it != named.end())
          for (auto& [cls, k] : it->second)
            if (keep(cls) && (c.kind == 'c' || !spans.contains(k) || spans[k].arity == (int)c.args.size())) out.push_back(k);
      };
      switch (c.kind) {
        case 'f': out.push_back(symName(c.sym)); break;
        case 'c': all(S_init, [&](int cls) { return below(c.sym, cls); }); break;
        case 's': {
          int base = superOf.contains(c.cls) ? superOf[c.cls] : -1;
          all(c.sym, [&](int cls) { return below(base, cls); });
          break;
        }
        case 'x':
          for (auto& e : externs) if (e.sym == c.sym && !e.pure) x.eff |= EFF_IO;
          break;
        case 'b': break;  // budget(n, f): its call of f is applied below
        case 'm': {
          int cls = c.cls;
          uint64_t t = c.path ? resolveNow(paths[c.path].type) & ~TY_OPT : 0;
          if (cls < 0 && (t & 15) == TY_CLS) cls = int(t >> 8);
          if (cls >= 0) { all(c.sym, [&](int d) { return below(cls, d) || below(d, cls); }); break; }
          bool builtin = std::find(std::begin(builtins), std::end(builtins), c.sym) != std::end(builtins);
          if (t && (t & 15) != TY_EXT) {  // a list, string, map, ...: its builtin methods, or a map's function
            if ((t & 15) == TY_MAP && !builtin) x.unknown = true;
            break;
          }
          all(c.sym, [](int) { return true; });
          bool chan = c.sym == S_send || c.sym == recv || c.sym == tryRecv || c.sym == close || c.sym == S_done || (c.sym == join && c.args.empty());
          sure = !builtin && out.size() == 1;
          if (chan) (sure || out.empty() ? x.eff : x.maybe) |= EFF_CHAN;
          if (chan && out.size()) x.via.insert(c.sym);
          if (c.sym == S_send && fibers) x.unknown = true;
          if (!builtin && out.empty()) x.unknown = true;  // no class has it: a map's function
          break;
        }
      }
    };
    auto apply = [&](uint64_t calls, const std::vector<int>& args, R& x) {  // a callee calls these parameters: what was passed
      for (int j = 0; j < 64; j++)
        if (calls >> j & 1) {
          int o = j < (int)args.size() ? args[j] : ARG_OTHER;
          if (o >= 0) x.calls |= 1ull << o;
          else if (o == ARG_OTHER) x.unknown = true;
        }
    };
    for (bool changed = true; changed;) {
      changed = false;
      for (auto& [k, e] : effFns) {
        if (e.declared) continue;
        R x = r[k];
        std::vector<std::string> to;
        for (auto& c : e.sites) {
          bool sure;
          to.clear();
          callees(c, x, to, sure);
          if (c.kind == 'b') apply(2, c.args, x);
          for (auto& name : to) {
            auto it = r.find(name);
            if (it == r.end()) { x.unknown = true; continue; }
            const R& y = it->second;
            if (sure) x.eff |= y.eff, x.maybe |= y.maybe, x.unknown |= y.unknown, x.via.insert(y.via.begin(), y.via.end());
            else if (y.eff | y.maybe) x.maybe |= y.eff | y.maybe, x.via.insert(c.sym), x.via.insert(y.via.begin(), y.via.end());
            apply(y.calls, c.args, x);
          }
        }
        R& old = r[k];
        if (x.eff != old.eff || x.maybe != old.maybe || x.calls != old.calls || x.unknown != old.unknown || x.via.size() != old.via.size())
          old = std::move(x), changed = true;
      }
    }
    static const char* builtin[EFF_BUILTIN] = {"io", "clock", "random", "yield", "spawn", "chan"};
    auto names = [&](uint64_t mask, std::vector<std::string>& out) {
      for (int i = 0; i < EFF_LEVEL; i++)
        if (mask >> i & 1) out.push_back(i < EFF_BUILTIN ? builtin[i] : effBitNames.count(1ull << i) ? effBitNames[1ull << i] : "an effect");
    };
    for (auto& [k, e] : effFns) {
      if (e.declared || k.find('@') != std::string::npos) continue;
      R& x = r[k];
      FnEffects o{e.file, e.name, e.line, {}, {}, x.unknown};
      names(x.eff, o.does), names(x.maybe & ~x.eff, o.may);
      if (!o.may.empty()) for (int m : x.via) o.via.push_back(symName(m));
      for (size_t j = 0; j < e.params.size() && j < 64; j++) if (x.calls >> j & 1) o.params.push_back(symName(e.params[j]));
      inferred.push_back(std::move(o));
    }
  }
  void checkClasses() {
    std::unordered_map<int, ClassView> views;
    for (auto& cd : classes) {
      auto [it, inserted] = views.try_emplace(cd.sym, ClassView{cd.super, {}, &cd.fields, &cd.ftypes});
      if (inserted) for (auto& [m, f] : cd.methods) if (m != invariantSym())
        it->second.methods.push_back({m, spans[symName(cd.sym) + "." + symName(m)].arity});
    }
    auto view = [&](int sym) -> const ClassView* {
      if (auto it = views.find(sym); it != views.end()) return &it->second;
      Global* g = findGlobal(sym);
      if (!g || !g->o || g->o->kind != K::Class) return nullptr;
      Class* c = (Class*)g->o;
      auto& v = views[sym];
      v.super = c->super ? (int)c->super->name : -1;
      v.fields = &c->fields;
      v.types = &c->ftypes;
      for (auto& [m, f] : c->methods) if (m != invariantSym()) v.methods.push_back({m, f->arity - 1});
      return &v;
    };
    for (auto& cd : classes) {
      if (cd.super < 0 || !superAt.contains(cd.sym)) continue;
      const ClassView* v;
      std::unordered_set<int> seen{cd.sym};
      for (int s = cd.super; s >= 0 && (v = view(s)); s = v->super) {
        if (seen.insert(s).second) continue;
        diagnoseHere(superAt.at(cd.sym), "@class bad superclass '%s'", symName(cd.super));
        views.at(cd.sym).super = cd.super = -1;
        break;
      }
    }
    // Methods: one that replaces a superclass's method says `override`, and takes the same parameters; one
    // that says `override` replaces something. So adding a method to a base class can't silently capture a
    // subclass's unrelated method of the same name, and renaming one can't silently orphan an override.
    for (auto& cd : classes)
      for (auto& w : cd.written) {
        if (w.sym == S_init) continue;  // constructors don't override
        const ClassView* v;
        int owner = -1, arity = -1;
        for (int s = cd.super; s >= 0 && owner < 0 && (v = view(s)); s = v->super)
          for (auto [m, a] : v->methods) if (m == w.sym) owner = s, arity = a;
        std::string m = symName(w.sym), q = symName(cd.sym) + "." + m;
        if (owner < 0 && w.over) diagnoseHere(w.at, "@override '%s' is marked override, but no superclass of %s has a method '%s'", m, symName(cd.sym), m);
        if (owner < 0) continue;
        if (!w.over) diagnoseHere(w.at, "@override '%s' overrides %s: write 'override fn %s' if that is intended", m, symName(owner) + "." + m, m);
        if (int mine = spans[q].arity; mine != arity)
          diagnoseHere(w.at, "@override '%s' takes %s, but %s, which it overrides, takes %s", m, params(mine), symName(owner) + "." + m, params(arity));
        // Callers that hold a superclass's instance rely on its method's types, so an override declares the same.
        auto a = sigs.find(symName(owner) + "." + m), b = sigs.find(q);
        if (a != sigs.end() && b != sigs.end() && a->second.types != b->second.types)
          diagnoseHere(w.at, "@override '%s' must declare the same types as %s, which it overrides: %s, not %s", m, symName(owner) + "." + m, a->second.text, b->second.text);
        // ...and may perform only what the method it replaces declares (a subset: doing less breaks no caller).
        if (a != sigs.end() && b != sigs.end() && a->second.eff && (!b->second.eff || (b->second.effMask & ~a->second.effMask)))
          diagnoseHere(w.at, "@override '%s' must declare effects within those of %s, which it overrides (%s), not %s", m, symName(owner) + "." + m,
                   a->second.effText, b->second.eff ? b->second.effText : "none (unchecked)");
      }
    // Parents precede children. An untyped shadowing field stops lookup in its base.
    for (auto& p : paths) {
      if (p.result >= 0 && !p.parent)
        if (auto it = sigs.find(symName(p.result)); it != sigs.end() && !it->second.types.empty()) p.type = it->second.types.back();
      if (!p.parent) continue;
      uint64_t parent = paths[p.parent].type;
      if (p.field == -1) { p.type = parent; continue; }
      uint64_t t = resolveNow(parent) & ~TY_OPT;
      if (p.field == CALL) {
        for (int c = (t & 15) == TY_CLS ? int(t >> 8) : -1, hops = 0; c >= 0 && hops < 64; hops++) {  // the class that defines it
          const ClassView* v = view(c);
          if (!v) break;
          if (std::any_of(v->methods.begin(), v->methods.end(), [&](auto& m) { return m.first == p.result; })) {
            if (auto it = sigs.find(symName(c) + "." + symName(p.result)); it != sigs.end() && !it->second.types.empty()) p.type = it->second.types.back();
            break;
          }
          c = v->super;
        }
        continue;
      }
      if (p.field == EACH || p.field == AT) {
        const TypeDesc* d = (t & 15) == TY_EXT ? typeDesc(t) : nullptr;
        if (d && d->k == 'L') p.type = d->elem;
        else if (d && d->k == 'M') p.type = p.field == EACH ? d->key : d->elem ? d->elem | TY_OPT : 0;
        continue;
      }
      if ((t & 15) == TY_CLS) {
        for (auto* v = view(int(t >> 8)); v; v = view(v->super)) {
          for (auto& f : *v->types) if (f.sym == p.field) p.type = f.type;
          if (p.type || std::any_of(v->fields->begin(), v->fields->end(), [&](auto& f) { return f.first == p.field; })) break;
          if (v->super < 0) break;
        }
      } else if ((t & 15) == TY_EXT) {
        if (auto* d = typeDesc(t); d && d->k == 's')
          for (auto [f, type] : d->fields) if (f == p.field) p.type = type;
      }
    }
    // x.name where x has a declared type: a shape's fields; a class's methods, and its fields if it declares them.
    for (auto& u : typedUses) {
      uint64_t type = paths[u.path].type;
      uint64_t t = resolveNow(type) & ~TY_OPT;
      std::vector<int> names;
      std::string what;
      bool ok = false, isMethod = false;
      if ((t & 15) == TY_CLS) {
        const ClassView* v;
        bool closed = false;
        for (int s = int(t >> 8); s >= 0 && (v = view(s)); s = v->super) {
          for (auto& [f, d] : *v->fields) closed = true, names.push_back(f), ok |= f == u.sym;
          for (auto& [m, a] : v->methods) names.push_back(m), isMethod |= m == u.sym;
        }
        ok |= isMethod || !closed;  // an open class gets fields by assignment
        what = symName(int(t >> 8));
      } else if (const TypeDesc* d = (t & 15) == TY_EXT ? typeDesc(t) : nullptr; d && d->k == 's') {
        for (auto& [f, _] : d->fields) names.push_back(f), ok |= f == u.sym;
        if (u.call) for (int m : {S_has, S_remove, S_keys, S_values, S_len}) ok |= m == u.sym;  // a map's methods
        what = (type & 15) == TY_NAMED ? symName(int(type >> 8)) : typeText(t);
      } else if (int bk = builtinKind(t); bk && !(resolveNow(type) & TY_OPT)) {
        // A list or str has a fixed set of methods and no fields: anything else throws every time ([no-method], [no-field]).
        K k = bk == 'l' ? K::List : K::Str;
        if (u.call && builtinMethod(k, u.sym)) continue;
        const char* kind = k == K::List ? "list" : "string";
        std::string name = symName(u.sym), x = u.var == "it" ? (k == K::List ? "l" : "s") : u.var;  // "it": no name (a literal)
        std::string about = std::string(kind) + " has no " + (u.call ? "method" : "field") + " '" + name + "' (" +
                            (u.call ? "a " + std::string(kind) + "'s methods: " + builtinMethods(k) : "a " + std::string(kind) + " has methods but no fields") +
                            ")" + memberHint(k, name, u.call, x);
        diagnoseHere(u.at, u.call ? "@no-method %s" : "@no-field %s", about);
        continue;
      } else continue;
      if (ok) continue;
      std::string list, near;
      int nearest = 3;          // suggest the closest name within two edits
      std::vector<int> listed;  // a method a subclass overrides is listed once
      for (int n : names) {
        if (std::find(listed.begin(), listed.end(), n) != listed.end()) continue;
        listed.push_back(n);
        list += (list.empty() ? "" : ", ") + symName(n);
        if (int d = editDistance(symName(n), symName(u.sym)); d < nearest) nearest = d, near = symName(n);
      }
      std::string msg = "'" + symName(u.sym) + "' is not " + (u.call ? "a method or field" : "a field") + " of " + what + " (" + u.var +
                        " is " + what + ": " + (list.empty() ? "it has none" : list) + ")" + (near.empty() ? "" : "; did you mean '" + near + "'?");
      diagnoseHere(u.at, "@no-field %s", msg);
    }
    // Calls from functions that declare effects, of fns that declare effects they don't.
    for (auto& c : effCalls) {
      auto it = sigs.find(symName(c.callee));
      if (it == sigs.end() || !it->second.eff) continue;
      if (uint64_t extra = it->second.effMask & ~c.allowed)
        diagnoseHere(c.at, "@effect %s declares %s, but it is called from inside %s, which does not (%s)", display(symName(c.callee)),
                     effectNames(extra), c.who, declaresText(c.allowed, c.text));
    }
    // A value whose type is known (a literal, a constructor, something declared) where a declared type can never take
    // it: an argument of a direct call, a result, a store to a typed var or field. It would fail every time it ran.
    auto related = [&](int a, int b) {
      for (int pass = 0; pass < 2; pass++, std::swap(a, b)) {
        std::unordered_set<int> seen;
        for (int s = a; s >= 0 && seen.insert(s).second;) {
          if (s == b) return true;
          const ClassView* v = view(s);
          if (!v) return true;  // not known here: don't claim they differ
          s = v->super;
        }
      }
      return false;
    };
    auto valueType = [&](const ArgVal& a) -> uint64_t { return a.num ? TY_NUM : a.path ? paths[a.path].type : 0; };
    for (auto& c : callSites) {
      std::string callee = c.cls < 0 ? symName(c.callee) : "";
      if (c.recv) {  // x.m(args), x of a known class: the m it finds (an override declares the same params)
        uint64_t t = resolveNow(paths[c.recv].type);
        if ((t & TY_OPT) || (t & 15) != TY_CLS) continue;
        int owner = -1;
        for (int s = int(t >> 8); s >= 0 && owner < 0;) {
          const ClassView* v = view(s);
          if (!v) break;
          for (auto [m, a] : v->methods) if (m == c.callee) owner = s;
          s = v->super;
        }
        if (owner < 0) continue;  // not a method of it: [no-field] says so
        callee = symName(owner) + "." + symName(c.callee);
      }
      if (c.cls >= 0) {  // C(args): C's init, or the one it inherits; none takes no arguments
        int owner = -1;
        bool known = true;
        for (int s = c.cls; s >= 0 && owner < 0;) {
          const ClassView* v = view(s);
          if (!v) { known = false; break; }
          for (auto [m, a] : v->methods) if (m == S_init) owner = s;
          s = v->super;
        }
        if (!known) continue;
        if (owner < 0) {
          if (!c.args.empty())
            diagnoseHere(At{c.at, c.line, c.file, c.src}, "@arity %s takes no arguments (it has no init), got %s", symName(c.cls) + "()", std::to_string(c.args.size()));
          continue;
        }
        callee = symName(owner) + ".init";
      }
      auto it = sigs.find(callee);
      if (it == sigs.end()) continue;
      const Sig& s = it->second;
      if ((c.cls >= 0 || c.recv) && c.args.size() != s.params.size()) {
        std::string ps;
        for (size_t i = 0; i < s.params.size(); i++) ps += (i ? ", " : "") + symName(s.params[i]) + (s.types[i] ? ": " + typeText(s.types[i]) : "");
        size_t n = s.params.size();
        diagnoseHere(At{c.at, c.line, c.file, c.src}, "@arity %s expects %s (%s), got %s", c.recv ? callee + "()" : symName(c.cls) + "()",
                     std::to_string(n) + (n == 1 ? " argument" : " arguments"), ps, std::to_string(c.args.size()));
        continue;
      }
      for (size_t i = 0; i < c.args.size() && i < s.params.size() && i + 1 < s.types.size(); i++)
        if (uint64_t d = s.types[i], v = valueType(c.args[i]); contradicts(d, v, related))
          diagnoseHere(At{c.args[i].p, c.args[i].line, c.file, c.src}, "@type argument '%s' of %s must be %s, got %s", symName(s.params[i]),
                       c.cls >= 0 ? symName(c.cls) + "()" : c.recv ? callee + "()" : std::string(display(symName(c.callee))), typeText(d), staticText(v));
    }
    for (auto& e : expects) {
      uint64_t d = e.type;
      std::string what = e.what;
      if (e.typePath) {  // a field: its declared type (if any), named after the class that declares it
        d = paths[e.typePath].type, what = "field '" + symName(e.field) + "'";
        uint64_t t = resolveNow(paths[paths[e.typePath].parent].type) & ~TY_OPT;
        for (int s = d && (t & 15) == TY_CLS ? int(t >> 8) : -1; s >= 0;) {
          const ClassView* v = view(s);
          if (!v) break;
          if (std::any_of(v->types->begin(), v->types->end(), [&](auto& f) { return f.sym == e.field; })) { what += " of " + symName(s); break; }
          s = v->super;
        }
      }
      if (uint64_t v = valueType(e.value); contradicts(d, v, related))
        diagnoseHere(At{e.value.p, e.value.line, e.file, e.src}, "@type %s must be %s, got %s", what, typeText(d), staticText(v));
    }
    // Fields: in a class that declares its fields (or inherits declared ones), self.name must be one of them.
    for (auto& u : fieldUses) {
      const ClassView* v;
      bool closed = false, found = false;
      for (int s = u.cls; s >= 0 && (v = view(s)); s = v->super)
        for (auto& [f, d] : *v->fields) closed = true, found |= f == u.sym;
      if (closed && !found) diagnoseHere(u.at, "@no-field %s has no field '%s' (declare it in the class: var %s)", symName(u.cls), symName(u.sym), symName(u.sym));
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
    vm->globals[g.slot] = box(g.o);
    return (T*)g.o;
  }
  Compiled commit(Code& chunk) {
    published = true;
    for (int s : newGlobals) ::global(s);
    for (auto& [s, g] : declarations) ::global(s) = g;
    declarations.clear(), stageGlobals = false;
    ::enumDefs = std::move(enumDefs), ::sigs = std::move(sigs);
    installedNames = static_cast<Namespaces&>(*this);
    mainFile = installedNames.entryPath;
    methodFns.merge(newMethods), tiers.merge(newTiers);
    for (auto [s, h] : varText) ::varText[s] = h;
    for (auto& [q, info] : gpuInfos) gpuInfoSet(q, std::move(info));
    Compiled out;
    out.files = files, out.tests = std::move(tests), out.effects = std::move(inferred);
    for (auto& f : files) out.sources.push_back({f, sources.at(f).code()});
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
      if (p.lambda && p.owner.ends_with(".inv@")) {  // a class's invariants, compiled every time: the old body goes
        u32*& old = invBodies[p.owner];
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
      if (e.rt) f->code = ((Fn*)e.fn)->code, f->arity = ((Fn*)e.fn)->arity, f->op = 0;
      else f->code = ffiThunk(e.fn, e.sig, e.ret, e.sym, e.stub), f->arity = (int)e.sig.size(), f->op = e.pure ? 0 : OP_EXTERN;  // performs io
    }
    for (auto& d : effDefs) effDefine(d.sym, symName(d.sym), d.ops);
    for (auto& [s, c] : typeDefs) global(s).type = c;  // named types: checks look them up when they run
    for (auto& [s, l] : effOps) effectOps[s] = l;
    std::string collectionNotes;
    if (reload) for (auto& cd : classes) {
      auto* old = global(cd.sym).o;
      if (!old || old->kind != K::Class) continue;
      for (auto [field, kind] : cd.collectionInits) {
        if (((Class*)old)->declared(field)) continue;
        collectionNotes += "\n[fut] " + symName(cd.sym) + "." + symName(field) + " starts as " +
                           (kind == K::List ? "[]" : "{}") + " on construction. Unset on existing instances until assigned.";
      }
    }
    for (auto& cd : classes) globalObj<Class>(cd.sym);  // all exist before any superclass link is made
    for (auto& cd : classes) {
      Class* c = globalObj<Class>(cd.sym);
      c->super = cd.super >= 0 ? (Class*)global(cd.super).o : nullptr;
      c->collectionInits = cd.collectionInits;
      c->methods = cd.methods, c->fields = cd.fields, c->ftypes = cd.ftypes, c->sum = cd.sum;
      Locked g(layoutMu);
      size_t n = c->layout.size();
      for (auto& [f, d] : cd.fields) c->layout.add(f);
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
    out.log += collectionNotes;
    if (!echo) {
      replaced = std::move(installed);
      installed.clear();
      for (auto& f : files) installed.push_back({f, std::move(sources.at(f).text)});
    }
    return out;
  }
};

void tierUp(void* p, u32 numArgs) {
  Tier& t = *(Tier*)p;
  Locked code(codeMu);
  if (t.fast || t.failed || !t.body) return;  // (another isolate may have just compiled it)
  ImageScope image;
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
// Keep the compile on the heap, so it can be destroyed after a WASM longjmp past its working stack.
struct CompileJob {
  Compiler& c;
  const std::string& path;
  const std::string* text;
  Compiled out;
};
static bool compileInput(CompileJob& j, std::string& err, bool& atEnd) {
  size_t constants = consts.size();
  bool steps = stepsOn;
  bool ok = guardCompile([](void* ctx) {
    auto& [c, path, text, out] = *(CompileJob*)ctx;
    packagesReset(), modulesPending.clear();
    if (!c.echo) {
      c.entryPath = path;
      const Package* p = packageOf(path);
      c.entryDir = p ? p->dir : "";
    }
    auto& source = c.sources[path];
    if (text) source.text = *text;
    else if (!readSource(path, source.text, c.editorCheck)) compileFail("cannot read " + path, false);
    c.prepareSource(path, source);
    out = c.run(path, source.code().c_str());
  }, &j, &err, &atEnd);
  if (!j.c.published) {
    for (u32* stub : j.c.newStubs) discardStub(stub);
    if (j.c.firstIC >= 0) discardICs(j.c.firstIC);
    consts.resize(constants), stepsOn = steps;
  }
  return ok;
}
Compiled compile(const std::string& path, bool reload, bool echo, const std::string* text, bool tests) {
  Locked code(codeMu);  // one compile at a time, whichever isolate asks (tierUp too)
  ImageScope image;     // what it makes, compiled code points at, in every isolate
  JitBatch writable;
  auto c = std::make_unique<Compiler>();
  c->reload = reload, c->echo = echo, c->testing = tests;
  c->collectErrors = !echo, c->stageGlobals = true;
  CompileJob j{*c, path, text};
  std::string err;
  bool atEnd = false;
  bool ok = compileInput(j, err, atEnd);
  if (ok && c->reports.empty()) return std::move(j.out);
  if (c->collectErrors && !c->reports.empty() && c->failureText.empty()) {
    std::string diags = c->diagnosticText();
    if (!err.empty()) diags += "\n" + err;
    err = std::move(diags);
  }
  c.reset();
#if __wasm__
  static TLS std::string failure;
  failure = std::move(err);
  compileFail(failure, atEnd);
#else
  compileFail(err, atEnd);
#endif
}
static CompileDiagnostic inputDiagnostic(std::string_view text) {
  CompileDiagnostic d;
  std::string_view head = text.substr(0, text.find('\n'));
  constexpr std::string_view marker = ": error: ";
  auto number = [](std::string_view s, int& n) {
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), n);
    return ec == std::errc{} && p == s.data() + s.size() && n > 0;
  };
  for (size_t e = head.find(marker); e != std::string_view::npos; e = head.find(marker, e + marker.size())) {
    size_t c = head.substr(0, e).rfind(':');
    int n;
    if (c == std::string_view::npos || !number(head.substr(c + 1, e - c - 1), n)) continue;
    size_t l = head.substr(0, c).rfind(':');
    if (l != std::string_view::npos && number(head.substr(l + 1, c - l - 1), d.line))
      d.column = n, d.file = head.substr(0, l);
    else d.line = n, d.file = head.substr(0, c);  // a package manifest has a line only
    head.remove_prefix(e + marker.size());
    break;
  }
  d.message = head;
  d.code = splitCode(d.message);
  return d;
}
std::vector<CompileDiagnostic> compileDiagnostics(const std::string& path, CompileCheck& check) {
  Locked code(codeMu);
  ScratchImage image;
  auto c = std::make_unique<Compiler>();
  c->editorCheck = &check;
  c->collectErrors = c->stageGlobals = c->testing = true;
  check.imports.clear();
  check.symbols = {};
  Navigation nav{check.symbols};
  c->nav = &nav;
  std::string entry = check.normalize(path);
  CompileJob job{*c, entry, nullptr};
  std::string err;
  bool atEnd = false;
  auto pending = std::move(modulesPending);
  bool ok = compileInput(job, err, atEnd);
  c->finishIndex();
  modulesPending = std::move(pending);
  c->sortReports();
  std::vector<CompileDiagnostic> out;
  for (auto& r : c->reports) out.push_back(std::move(r.diag));
  if (!ok && c->failureText.empty()) {
    // Manifests, view syntax and resource limits can fail before the parser reports a diagnostic.
    auto d = inputDiagnostic(err);
    d.file = d.file.empty() ? entry : check.normalize(d.file);
    if (d.code == "manifest" && !check.imports.empty() && check.imports.back().to.empty())
      check.imports.back().to = d.file;
    out.push_back(std::move(d));
  }
  return out;
}
static bool revertReload(const std::vector<int>& newVars, std::string* err) {
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

void rollbackReload(const std::vector<int>& newVars) {
  txnAbort();
  std::string err;
  if (revertReload(newVars, &err)) fputs("[fut] reload rolled back: the program's data and code are as they were before it\n", stderr);
  else fprintf(stderr, "[fut] reload rolled back its data, but the previous code did not compile again: %s\n", err.c_str());
}
