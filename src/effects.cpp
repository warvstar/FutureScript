// Effects: what a function may touch outside itself, declared after its result type (`fn save(p): bool ! io`), and
// handlers that answer an operation in place (`handle { body } with clock() { return 1000 }`).
//
// Like types, a declaration is checked at run time, where it is written: entering a function that declares effects
// checks that the code calling it may perform them, then narrows what may be performed to the declared set until
// the function returns. Performing an operation (print, clock, Log.log) checks it against that set. Functions that
// declare nothing are unchecked and run under whatever their caller allows, with no extra code at all.
//
// The state is three words in the VM block: vm->allowed (the effects allowed now, low 44 bits, and a narrowing level
// above them), vm->who (the declaration that narrowed them, for messages) and vm->hstack (the innermost handler
// record). A declaring function saves the first two in its frame and restores them in its epilogue; try records save
// all three, so a throw puts them back; each fiber has its own (see fiberResume).
//
// A handler record is a list: [previous record, allowed (low, high half), who, op, handler, op, handler, ...]. A
// handler runs as an ordinary call on top of the code that performed the operation, with the effects and handlers
// that were in force where it was installed, so a handler for print may itself print (to the real output, or to an
// outer handler).
#include "fut.h"
#include <unordered_map>

namespace {
struct Site { std::string fn, text; uint64_t mask; };  // a function that declares effects
std::vector<Site> sites(1);
std::unordered_map<std::string, int> siteIds;
struct OpInfo {
  std::string name;          // "print()", "Log.log()"
  uint64_t effect = 0;       // its effect's bit (0: a user effect not defined yet)
  int arity = -1;
  bool user = false;
  std::vector<uint64_t> ptypes;
  std::vector<int> psites;
  uint64_t rtype = 0;
  int rsite = -1;
  Fn* fn = nullptr;
};
std::vector<OpInfo> ops(3);
std::unordered_map<std::string, int> opIds;  // user operations by qualified name
std::unordered_map<int, int> builtinOps;     // builtin native's symbol -> its operation
std::unordered_map<int, uint64_t> userBits;  // user effect symbol -> its bit
std::string effName[EFF_LEVEL];
std::vector<int> effOps[EFF_LEVEL];          // each effect's operations (a handle that answers all of them allows it)
int nextBit = EFF_BUILTIN;
int printOp;  // print(): deterministic where tests run (their output is captured), so it leaves a test cacheable
TLS std::vector<EffState> saved;    // effEnter (per isolate)
struct Pending { V clo = 0, rec = 0; int op = 0; uint64_t n = 0; };
TLS Pending pending;  // effPrep -> invokeHandler
ISO_LOCAL(saved);
ISO_LOCAL(pending);
}  // namespace

static int bitIndex(uint64_t bit) { return __builtin_ctzll(bit); }
static std::string names(uint64_t mask) {
  std::string s;
  for (int i = 0; i < EFF_LEVEL; i++) if (mask >> i & 1) s += (s.empty() ? "" : ", ") + effName[i];
  return s;
}
static std::string declares(const Site& s) { return s.mask ? "it declares " + s.text : "it declares no effects"; }

int effSite(const std::string& fn, uint64_t mask, const std::string& text) {
  auto [it, fresh] = siteIds.try_emplace(fn + '\0' + text, (int)sites.size());
  if (fresh) sites.push_back({fn, text, mask});
  return it->second;
}
uint64_t effBit(int sym) {
  if (auto it = userBits.find(sym); it != userBits.end()) return it->second;
  if (nextBit == EFF_LEVEL) return 0;
  effName[nextBit] = symName(sym);
  return userBits[sym] = 1ull << nextBit++;
}
int effOpId(const std::string& q) {
  auto [it, fresh] = opIds.try_emplace(q, (int)ops.size());
  if (fresh) ops.push_back({q + "()", 0, -1, true});
  return it->second;
}
Fn* effOpFn(int op) {
  OpInfo& o = ops[op];
  if (!o.fn) {
    o.fn = make<Fn>(), o.fn->op = op, o.fn->arity = o.arity, o.fn->code = nullptr;
    o.fn->name = intern(o.name.substr(0, o.name.size() - 2));
    consts.push_back(box(o.fn));
  }
  return o.fn;
}
void effDefine(int sym, const std::string& name, const std::vector<EffOpDef>& defs) {
  uint64_t bit = effBit(sym);
  int i = bitIndex(bit);
  effName[i] = name, effOps[i].clear();
  for (auto& d : defs) {
    OpInfo& o = ops[d.op];
    o.effect = bit, o.arity = d.arity, o.ptypes = d.ptypes, o.psites = d.psites, o.rtype = d.rtype, o.rsite = d.rsite;
    if (o.fn) o.fn->arity = d.arity;
    effOps[i].push_back(d.op);
  }
}
int effOpArity(int op) { return op > 0 && op < (int)ops.size() ? ops[op].arity : -1; }
uint64_t effOpEffect(int op) { return ops[op].effect; }
const std::string& effOpName(int op) { return ops[op].name; }
int effBuiltinOp(int sym) { auto it = builtinOps.find(sym); return it == builtinOps.end() ? 0 : it->second; }

EffState effSave() { return {vm->allowed, vm->hstack, vm->who, vm->inv}; }
void effLoad(const EffState& s) { vm->allowed = s.allowed, vm->hstack = s.hstack, vm->who = s.who, vm->inv = s.inv; }
u32 effEnter() {
  saved.push_back(effSave());
  vm->allowed = EFF_BITS, vm->hstack = NIL, vm->who = 0, vm->inv = 0;
  return (u32)saved.size() - 1;
}
void effLeave(u32 depth) {
  if (depth >= saved.size()) return;
  effLoad(saved[depth]), saved.resize(depth);
}
void effMark(void (*mark)(V)) {
  mark(vm->hstack), mark(pending.clo), mark(pending.rec);
  for (auto& s : saved) mark(s.hstack);
}

// ---- errors ----
static std::string whoName() { return vm->who && vm->who < sites.size() ? sites[vm->who].fn : "this code"; }
[[noreturn]] static void forbidden(const std::string& what, uint64_t effect, void* ra) {
  const char* e = effName[bitIndex(effect)].c_str();
  if (effect == EFF_IO && (vm->who == WHO_ATOMIC || (txnAtomic() && (vm->allowed & effect))))
    rtError(ra, "@atomic %s performs io, which can't be undone inside atomic { } (do it after the block, or handle it inside the block)", what.c_str());
  if (vm->who && vm->who < sites.size())
    rtError(ra, "@effect %s performs %s, which %s does not declare (%s)", what.c_str(), e, sites[vm->who].fn.c_str(), declares(sites[vm->who]).c_str());
  rtError(ra, "@effect %s performs %s, which is not allowed here", what.c_str(), e);
}
static void rt_effect(uint64_t site) {  // entering a declaring function whose effects its caller may not perform
  Site& s = sites[site];
  std::string extra = names(s.mask & ~vm->allowed & EFF_BITS);
  if (vm->who == WHO_ATOMIC)
    rtError(RA(), "@atomic %s declares %s, which can't be undone inside atomic { }: it can't be called inside the block", s.fn.c_str(), extra.c_str());
  rtError(RA(), "@effect %s declares %s, but it is called from inside %s, which does not (%s)", s.fn.c_str(), extra.c_str(),
          whoName().c_str(), vm->who && vm->who < sites.size() ? declares(sites[vm->who]).c_str() : "");
}

// A runtime operation with no handler form (spawn, a channel's): this code must be allowed to perform it.
void effRequire(uint64_t effect, const char* what, void* ra) { if (!(vm->allowed & effect)) forbidden(what, effect, ra); }

// ---- handlers ----
static uint64_t recAllowed(List* r) { return (uint64_t)num(r->v[1]) | (uint64_t)num(r->v[2]) << 32; }
static bool findHandler(int op, V& clo, V& rec) {
  for (V h = vm->hstack; h != NIL; h = as<List>(h)->v[0]) {
    auto& v = as<List>(h)->v;
    for (size_t i = 4; i + 1 < v.size(); i += 2)
      if (num(v[i]) == op) return clo = v[i + 1], rec = h, true;
  }
  return false;
}
static bool usable(V rec, uint64_t effect) {  // allowed here, or installed under the same narrowing (the same function)
  return (vm->allowed & effect) || recAllowed(as<List>(rec)) >> EFF_LEVEL == vm->allowed >> EFF_LEVEL;
}
// Call handler clo for operation op with n arguments, in the context its record was installed in.
static V callHandler(V clo, V rec, int op, const V* args, uint64_t n) {
  List* r = as<List>(rec);
  EffState mine = effSave();
  volatile V keep[3] = {mine.hstack, clo, rec};  // in memory, so the GC sees them while the handler runs
  (void)keep;
  vm->allowed = recAllowed(r), vm->who = (u32)num(r->v[3]), vm->hstack = r->v[0];
  V a[8] = {clo};
  for (uint64_t i = 0; i < n && i < 7; i++) a[i + 1] = args[i];
  Fn* f = is(clo, K::Clo) ? as<Clo>(clo)->fn : as<Fn>(clo);
  V res = vmEnter(f->code, is(clo, K::Clo) ? a : a + 1);
  effLoad(mine);
  if (OpInfo& o = ops[op]; o.rtype && !typeAccepts(o.rtype, res, o.rsite)) typeFail(o.rsite, res, nullptr, o.rtype);
  return res;
}
static V invokeHandler(V a0, V a1, V a2, V a3, V a4, V a5, V a6, V a7) {  // effPrep sent a call here
  Pending p = pending;
  pending = {};
  V args[8] = {a0, a1, a2, a3, a4, a5, a6, a7};
  return callHandler(p.clo, p.rec, p.op, args, p.n);
}
static void* invokeCode;

void* effPrep(Fn* f, V* w, uint64_t n) {
  int op = f->op;
  OpInfo& o = ops[op];
  std::string what = op == OP_EXTERN ? symName(f->name) + "()" : o.name;
  int arity = op == OP_EXTERN || !o.user ? f->arity : o.arity;
  if (arity != (int)n) rtError(RA(), "@arity %s expects %d arguments, got %d", what.c_str(), arity, (int)n);
  if (!o.effect) rtError(RA(), "@effect %s: its effect is not defined", what.c_str());
  for (size_t i = 0; i < o.ptypes.size() && i < n; i++)
    if (o.ptypes[i] && !typeAccepts(o.ptypes[i], w[i + 1], o.psites[i])) typeFail(o.psites[i], w[i + 1], RA(), o.ptypes[i]);
  V clo, rec;
  if (findHandler(op, clo, rec)) {
    if (!usable(rec, o.effect)) forbidden(what, o.effect, RA());
    Fn* h = is(clo, K::Clo) ? as<Clo>(clo)->fn : is(clo, K::Fn) ? as<Fn>(clo) : nullptr;
    if (!h || h->arity - is(clo, K::Clo) != (int)n) rtError(RA(), "@arity the handler for %s must take %d parameters", what.c_str(), (int)n);
    pending = {clo, rec, op, n};
    memmove(w, w + 1, n * 8);
    return invokeCode;
  }
  if (!(vm->allowed & o.effect) || (o.effect == EFF_IO && txnAtomic())) forbidden(what, o.effect, RA());
  if (traceOn && op != printOp && (o.effect & (EFF_IO | EFF_CLOCK | EFF_RANDOM))) traceImpure();  // (print: captured under fut test)
  if (o.user) rtError(RA(), "@unhandled %s was performed, but nothing handles it (handle { ... } with %s { ... })", what.c_str(), o.name.substr(0, o.name.size() - 1).append("...)").c_str());
  memmove(w, w + 1, n * 8);
  return f->code;
}
V effYield(V v, bool* handled) {
  V clo, rec;
  *handled = false;
  if (findHandler(OP_YIELD, clo, rec)) {
    if (!usable(rec, EFF_YIELD)) forbidden("yield", EFF_YIELD, RA());
    return *handled = true, callHandler(clo, rec, OP_YIELD, &v, 1);
  }
  if (!(vm->allowed & EFF_YIELD)) forbidden("yield", EFF_YIELD, RA());
  return NIL;
}

// handle { body } with op(params) { ... } ...: the compiler builds [op, handler, ...] and calls handle@begin before
// the body, and handle@end (passing x0 through) wherever control leaves the body normally. A handle that answers
// every operation of an effect also allows that effect inside its body (so declaring functions can be called there).
static V handleBegin(V list) {
  List* l = as<List>(list);
  uint64_t widen = 0;
  for (size_t i = 0; i + 1 < l->v.size(); i += 2) {
    uint64_t e = ops[(int)num(l->v[i])].effect;
    if (!e || (widen & e)) continue;
    bool all = true;
    for (int op : effOps[bitIndex(e)]) {
      bool has = false;
      for (size_t j = 0; j + 1 < l->v.size(); j += 2) has |= num(l->v[j]) == op;
      all &= has;
    }
    if (all) widen |= e;
  }
  uint64_t a = vm->allowed;
  l->v.insert(l->v.begin(), {vm->hstack, box((double)(a & 0xffffffff)), box((double)(a >> 32)), box((double)vm->who)});
  vm->hstack = list, vm->allowed = a | widen;
  return NIL;
}
static V handleEnd(V v) {
  if (vm->hstack != NIL) {
    List* r = as<List>(vm->hstack);
    vm->allowed = recAllowed(r), vm->hstack = r->v[0];
  }
  return v;
}

// Around a method of a class with invariants (its own, a superclass's or a subclass's). The invariant is checked when
// an outside call ends, not when a method calls its own object's methods partway through a change: rt_inventer
// answers whether a call on self is already running, and links this one's record (two slots in its frame: the
// previous record, self) into vm->inv; the method unlinks it as it returns. A throw unlinks it with the frame: try
// records and fibers save and restore vm->inv with the rest of the effect state. Nothing is allocated.
static V rt_inventer(V self, uint64_t* rec) {
  bool outer = true;
  for (auto* r = (uint64_t*)vm->inv; r && outer; r = (uint64_t*)r[0]) outer = r[1] != self;
  rec[0] = vm->inv, rec[1] = self, vm->inv = (uint64_t)(uintptr_t)rec;
  return boxb(outer);
}

static TLS uint64_t rng;
ISO_LOCAL(rng);
static V n_random() {  // xorshift64*, seeded from the clock: [0, 1)
  if (!rng) rng = (uint64_t)(now() * 1e9) | 1;
  rng ^= rng >> 12, rng ^= rng << 25, rng ^= rng >> 27;
  return box((double)((rng * 0x2545F4914F6CDD1Dull) >> 11) * 0x1p-53);
}

void effectsInit() {
  vm->allowed = EFF_BITS, vm->hstack = NIL, vm->who = 0, vm->inv = 0, vm->invObj = 0;
  effName[0] = "io", effName[1] = "clock", effName[2] = "random", effName[3] = "yield", effName[4] = "spawn", effName[5] = "chan";
  ops[OP_YIELD] = {"yield", EFF_YIELD, 1}, ops[OP_EXTERN] = {"extern", EFF_IO, -1};
  effOps[3] = {OP_YIELD}, effOps[0] = {OP_EXTERN};  // externs can't be handled, so no handle allows io as a whole
  vm->helpers[H_EFFECT] = (void*)rt_effect;
  native<n_random>("random");
  native<handleBegin>("handle@begin"), native<handleEnd>("handle@end");
  vm->helpers[H_INVENTER] = (void*)rt_inventer;
  invokeCode = callable<invokeHandler>();
  for (auto [name, e] : std::initializer_list<std::pair<const char*, uint64_t>>{
           {"print", EFF_IO}, {"run", EFF_IO}, {"window", EFF_IO}, {"present", EFF_IO}, {"clear", EFF_IO}, {"rect", EFF_IO},
           {"circle", EFF_IO}, {"key", EFF_IO}, {"mousex", EFF_IO}, {"mousey", EFF_IO}, {"mousedown", EFF_IO},
           {"fetch", EFF_IO}, {"fetchAll", EFF_IO}, {"exec", EFF_IO}, {"execAll", EFF_IO}, {"readFile", EFF_IO},
           {"writeFile", EFF_IO}, {"env", EFF_IO}, {"time", EFF_CLOCK}, {"clock", EFF_CLOCK}, {"sleep", EFF_CLOCK}, {"random", EFF_RANDOM}}) {
    int sym = intern(name);
    Global* g = findGlobal(sym);
    if (!g || !g->o || g->o->kind != K::Fn) continue;
    Fn* f = (Fn*)g->o;
    f->op = (int)ops.size(), builtinOps[sym] = f->op;
    ops.push_back({std::string(name) + "()", e, f->arity, false, {}, {}, 0, -1, f});
    effOps[bitIndex(e)].push_back(f->op);
    if (!strcmp(name, "print")) printOp = f->op;
  }
}
