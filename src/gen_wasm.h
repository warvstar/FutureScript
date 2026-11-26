// WebAssembly code generator: the same parser drives it as drives gen_a64.h. Each fut function becomes a wasm
// function (window pointer) -> value; wasm.cpp packs a compile's functions into one module and installs them in the
// shared function table, which is what calls go through, so a reload is a table update.
// Values a collection must see live in memory: locals in the frame (on the shadow stack, which the GC scans), the
// "registers" r0.. are wasm locals that hold only temporaries, and a collection runs only at safepoints.
#pragma once
#include "compile.h"
#include "wasm.h"
using namespace wa;

struct Gen : CState {
  struct Cond {};  // a condition is an i32 left on the wasm stack

  // ---- emission ----
  void b(uint8_t x) { F->k.c.push_back(x); }
  void op(uint8_t o) {  // start an instruction (and note its source line)
    auto& ln = F->k.lines;
    if (ln.empty() || ln.back().second != line()) ln.push_back({(int)F->k.c.size(), line()});
    b(o);
  }
  void u(uint64_t v) { uleb(F->k.c, v); }
  // local.set x; local.get x -> local.tee x (nothing can come between: both are emitted back to back)
  size_t setAt = SIZE_MAX;
  Fun* setF = nullptr;
  void get(u32 l) {
    auto& c = F->k.c;
    if (setF == F && setAt < c.size() && c[setAt] == LOCAL_SET && c.size() - setAt == 1 + ulebLen(l) && lebAt(setAt + 1) == l) {
      c[setAt] = LOCAL_TEE, setAt = SIZE_MAX;
      return;
    }
    op(LOCAL_GET), u(l);
  }
  void set(u32 l) { setAt = F->k.c.size(), setF = F, op(LOCAL_SET), u(l); }
  static size_t ulebLen(u32 v) { size_t n = 1; while (v >>= 7) n++; return n; }
  u32 lebAt(size_t i) { u32 v = 0; for (int s = 0;; s += 7) { uint8_t x = F->k.c[i++]; v |= u32(x & 0x7f) << s; if (!(x & 0x80)) return v; } }
  void i32c(int32_t v) { op(I32_CONST), sleb(F->k.c, v); }
  void i64c(int64_t v) { op(I64_CONST), sleb(F->k.c, v); }
  void mem(uint8_t o, u32 align, u32 off) { op(o), u(align), u(off); }
  // Branch hints (likely / unlikely) on the type checks and slow paths: without them V8 treats both arms of an
  // `if` alike, and a helper call on the slow path makes it spill the loop's values around the fast path too.
  enum { NOHINT = -1, UNLIKELY = 0, LIKELY = 1 };
  void hint(int h) { if (h >= 0) F->k.hints.push_back({(int)F->k.c.size(), (uint8_t)h}); }
  void open(uint8_t kind, uint8_t type = VOID, int h = NOHINT) { hint(h), op(kind), b(type), F->blocks++; }  // block / loop / if
  void close() { op(ENDOP), F->blocks--; }
  void br(uint8_t o, int depth, int h = NOHINT) { hint(h), op(o), u(F->blocks - depth); }  // to the depth-th open block
  void addr(u32 r) { get(R0 + r), op(I32_WRAP); }  // the object address in register r (a pointer's low 32 bits)
  // Tag tests by shifts, whose immediates are a byte or two (QNAN and PTR are ten): bits 50..62 all set = not a
  // number; bits 50..63 all set = a pointer.
  void tagBits(bool ptr) { i64c(ptr ? 50 : 1), op(ptr ? I64_SHR_U : I64_SHL), ptr ? void() : (i64c(51), op(I64_SHR_U)); }
  void isNumber(u32 r) { get(R0 + r), tagBits(false), i64c(0x1fff), op(I64_NE); }
  void isPtr(u32 r, bool eq) { get(R0 + r), tagBits(true), i64c(0x3fff), op(eq ? I64_EQ : I64_NE); }
  void truthy() { get(R0), i64c(1), op(I64_OR), i64c((int64_t)FALSE_), op(I64_NE); }  // not nil, not false
  void callTable(u32 type) { op(CALL_INDIRECT), u(type), u(0); }  // the table index is on the stack
  // Fut code keeps its stack pointer in FP (a parameter derived), not in the C++ global __stack_pointer: it
  // writes the global only on the way into C++ (helpers), so C++ frames go below this one.
  void syncSp() { get(FP), op(GLOBAL_SET), u(0); }
  // Helper arguments: in r0.., or set by karg / warg just before the call (no trip through a register).
  struct Arg { char k = 0; int64_t v = 0; };
  Arg pa[5];
  void karg(u32 r, int64_t v) { pa[r] = {'k', v}; }  // a constant
  void warg(u32 r, int s) { pa[r] = {'w', s}; }       // the address of slot s
  void helper(int h) {  // result in r0
    syncSp();
    const char* s = HSIG[h];
    for (u32 i = 0; s[i] != ':'; i++) {
      bool w = s[i] == 'i';
      if (pa[i].k == 'k') w ? i32c((int32_t)pa[i].v) : i64c(pa[i].v);
      else if (pa[i].k == 'w') window((int)pa[i].v), w ? void() : op(I64_EXTEND_U);
      else get(R0 + i), w ? op(I32_WRAP) : void();
      pa[i] = {};
    }
    i32c(slotOf(vm->helpers[h])), callTable(1 + h);
    if (char r = strchr(s, ':')[1]) r == 'i' ? (op(I64_EXTEND_U), set(R0)) : set(R0);
  }
  void imm(u32 r, V v) { i64c((int64_t)v), set(R0 + r); }
  void mv(u32 d, u32 s) { get(R0 + s), set(R0 + d); }
  void addImm(u32 r, u32 k) { get(R0 + r), i64c(k), op(I64_ADD), set(R0 + r); }
  void gmem(u32 r, int g, bool store = false) {
    i32c(0);
    store ? (get(R0 + r), mem(I64_STORE, 3, slotOf(&vm->globals[g]))) : (mem(I64_LOAD, 3, slotOf(&vm->globals[g])), set(R0 + r));
  }
  int npar() { return F->npar; }
  void base(int s) { get(s < npar() ? W : FP); }                 // the pointer slot s is addressed from
  u32 off(int s) { return 8 * (s < npar() ? s : s - npar()); }
  void ld(u32 r, int s) { base(s), mem(I64_LOAD, 3, off(s)), set(R0 + r); }
  void st(u32 r, int s) { base(s), get(R0 + r), mem(I64_STORE, 3, off(s)); }
  void window(int s) { base(s), off(s) ? (i32c(off(s)), op(I32_ADD)) : void(); }  // address of slot s (an i32 on the stack)
  void addsp(u32 d, int s) { window(s), op(I64_EXTEND_U), set(R0 + d); }
  // A local's home is a frame slot, so the collector (which scans the shadow stack) sees it, and a wasm local
  // caching its value, which is what reads use. Stores write both, except where the value is a number (no
  // pointer to keep alive): range counters and `x op= y` on a local that started as a number stay in wasm locals.
  // Those locals are f64 (values are doubles' bits; moving them through f64 locals keeps every bit), so a
  // loop-carried `s += x` stays in FP registers.
  u32 wl(int h) {
    bool f = isFReg(h);
    auto& m = f ? F->wlocf : F->wloc;
    if ((int)m.size() <= h) m.resize(h + 1, -1);
    if (m[h] < 0) m[h] = (int)F->k.wlocals.size(), F->k.wlocals.push_back(f ? F64 : I64);
    return L0 + m[h];
  }
  int mark(int h, bool num) {
    if ((int)F->wnum.size() <= h) F->wnum.resize(h + 1);
    return F->wnum[h] = num, h;
  }
  int home() { return mark(slot(), false); }
  int fhome() { return mark(slot(), true); }
  int numHome() { return fhome(); }
  void ldl(u32 r, int h) { get(wl(h)), isFReg(h) ? op(I64_REINTERPRET) : void(), set(R0 + r); }
  void stl(u32 r, int h) {
    unprove(h);
    if (isFReg(h)) get(R0 + r), op(F64_REINTERPRET), set(wl(h)), base(h), get(R0 + r), mem(I64_STORE, 3, off(h));
    else base(h), get(R0 + r), op(LOCAL_TEE), u(wl(h)), mem(I64_STORE, 3, off(h));
  }
  void stn(u32 r, int h) {  // a value known to be a number: an f64 home needs no frame copy (nothing to keep alive)
    if (!isFReg(h)) return stl(r, h);
    unprove(h), get(R0 + r), op(F64_REINTERPRET), set(wl(h));
  }
  void param(int i, int h) {  // its home is the caller's window slot: nothing to copy; the first NA arrive in locals
    if (i != h || F->npar != i) fail("param order");
    F->npar = i + 1, unprove(h);
    i < (int)NA ? get(A0 + i) : (get(W), mem(I64_LOAD, 3, 8 * i));
    set(wl(h));
  }
  void poll() {  // safepoint: hot reload, REPL lines, garbage collection
    i32c(0), mem(I32_LOAD, 2, slotOf(&vm->poll)), open(IF, VOID, UNLIKELY), syncSp(), i32c(slotOf(vm->helpers[H_POLL])), callTable(1 + H_POLL), close();
  }
  void ret() { if (F->init) ldl(0, F->selfHome); br(BR, 0), F->live = false; }  // to the epilogue

  // Frame: FP = sp - size (on the shadow stack shared with C++; sp is the caller's FP), a stack check and a
  // safepoint, then the body in a block that `return` leaves with the result. Nothing to undo on the way out.
  Code finish() {
    if (F->live) F->init ? ldl(0, F->selfHome) : imm(0, NIL);  // falling off the end
    op(ENDOP);                             // the body block
    if (F->retTy) typeCheck(F->retTy, F->retSite, F->retTy == TY_NUM);  // the result, outside every try: see Fun::retTy
    if (F->exit) F->live = true, F->facts.reset(), F->exit();          // its spec's exit checks (they may use the frame)
    if (F->maxd > 4000) fail("@limit function too large");
    u32 size = ((F->maxd - F->npar) * 8 + 15) & ~15u;
    if (!size && F->calls) size = 16;      // keeps the stack check for recursion
    if (F->effHome >= 0) {  // the caller's effects
      i32c(0), get(F->effHome), mem(I64_STORE, 3, slotOf(&vm->allowed));
      i32c(0), get(F->effHome + 1), op(I32_WRAP), mem(I32_STORE, 2, slotOf(&vm->who));
    }
    get(R0), op(ENDOP);
    Code& k = F->k;
    std::vector<uint8_t> body = std::move(k.c);
    auto lines = std::move(k.lines);
    auto hints = std::move(k.hints);
    auto rels = std::move(k.rel);
    auto kinds = std::move(k.relKind);
    k.c.clear(), k.lines.clear(), k.hints.clear(), k.rel.clear(), k.relKind.clear(), setF = nullptr;
    // Replaced (reloaded, tier 2, stale arity): a direct call continues through the stub. Except a tier-2 body's
    // failed guard, which enters here with the window's low bit set (marked below): it runs this body.
    if (F->stub) {
      op(I32_CONST), rel(F->stub, REL_OWN), mem(I32_LOAD8_U, 0, 0), open(IF, VOID, UNLIKELY);
      get(W), i32c(1), op(I32_AND), open(IF, VOID), get(W), i32c(1), op(I32_XOR), set(W), op(ELSE);
      for (u32 p = 0; p < A0 + NA; p++) get(p);
      op(I32_CONST), rel(F->stub, REL_SLOT), op(RETURN_CALL_INDIRECT), u(0), u(0), close(), close();
    }
    for (u32 i : F->guard) {  // tier 2: a speculated argument that is not a number continues in the tier-1 body
      i < NA ? get(A0 + i) : (get(W), mem(I64_LOAD, 3, 8 * i));
      tagBits(false), i64c(0x1fff), op(I64_EQ), open(IF, VOID, UNLIKELY);
      get(W), F->stub ? (i32c(1), op(I32_OR)) : void();  // the mark (see above)
      for (u32 p = SP; p < A0 + NA; p++) get(p);
      i32c((int32_t)slotOf(deopt)), op(RETURN_CALL_INDIRECT), u(0), u(0), close();  // a tail call: no extra frame
    }
    get(SP);
    if (size) i32c(size), op(I32_SUB);
    set(FP);
    if (size) {
      // FP < limit, or a safepoint wanted (poll, the high half, nonzero): one load and compare (see rt_stack)
      get(FP), op(I64_EXTEND_U), i32c(0), mem(I64_LOAD, 3, slotOf(&vm->limit)), op(I64_LT_U);
      open(IF, VOID, UNLIKELY), syncSp(), i32c(slotOf(vm->helpers[H_STACK])), callTable(1 + H_STACK), close();
    }
    // The stack check is also the safepoint (a function that calls has a frame). A leaf needs none of its own:
    // its caller has one, and so do its loops.
    b(BLOCK), b(VOID);
    int n = (int)k.c.size();
    k.c.insert(k.c.end(), body.begin(), body.end());
    k.lines = {{0, lines.empty() ? prev.line : lines[0].second}};
    for (auto [at, line] : lines) k.lines.push_back({at + n, line});
    k.relKind.resize(k.rel.size());
    for (size_t i = 0; i < rels.size(); i++)
      k.rel.push_back({rels[i].first + n, rels[i].second}), k.relKind.push_back(i < kinds.size() ? kinds[i] : REL_SLOT);
    for (auto [at, h] : hints) k.hints.push_back({at + n, h});
    return std::move(k);
  }

  // ---- operators: r0 = left, r1 = right (or the literal `lit`). Two numbers: inline; anything else: rt_binop ----
  void slowOp(int op) {
    if (hasLit) karg(1, (int64_t)box(lit));
    karg(2, op), helper(H_BINOP);
  }
  bool checks(bool lnum, bool rnum) {  // leaves "both are numbers" (i32) if that needs checking
    bool l = !lnum, r = !rnum && !hasLit;
    if (l) isNumber(0);
    if (r) isNumber(1);
    if (l && r) op(I32_AND);
    return l || r;
  }
  void operands() { get(R0), op(F64_REINTERPRET), hasLit ? (op(F64_CONST), f64(lit)) : (get(R0 + 1), op(F64_REINTERPRET)); }
  void f64(double d) { uint64_t v = std::bit_cast<uint64_t>(d); for (int i = 0; i < 8; i++) b(uint8_t(v >> (8 * i))); }
  static constexpr uint8_t CMP[] = {F64_EQ, F64_NE, F64_LT, F64_LE, F64_GT, F64_GE};
  void fast(int o) {  // the result of two numbers, as a value
    if (o <= O_DIV) operands(), op(F64_ADD + o), op(I64_REINTERPRET);
    else i64c(TRUE_), i64c(FALSE_), operands(), op(CMP[o - O_EQ]), op(SELECT);
  }
  void binop(int o, bool lnum = false, bool rnum = false) {
    if (o >= O_BAND || o == O_MOD) return slowOp(o), (void)(hasLit = false);  // integer operators and %: in rt_binop
    if (checks(lnum, rnum)) open(IF, I64, LIKELY), fast(o), op(ELSE), slowOp(o), get(R0), close();
    else fast(o);
    set(R0), hasLit = false;
  }
  void neg(bool num) {  // r0 = -r0: a negate when it is a number; else 0 - r0, in rt_binop (which raises)
    auto fneg = [&] { get(R0), op(F64_REINTERPRET), op(F64_NEG), op(I64_REINTERPRET); };
    if (num) fneg();
    else isNumber(0), open(IF, I64, LIKELY), fneg(), op(ELSE), mv(1, 0), imm(0, 0), slowOp(O_SUB), get(R0), close();
    set(R0);
  }
  void unload(u32, int) {}  // see the arm64 generator: V8 drops the dead load
  void beginFun() {}
  void notOp() { i64c(FALSE_), i64c(TRUE_), truthy(), op(SELECT), set(R0); }
  int shortBegin(bool isAnd) { truthy(); if (!isAnd) op(I32_EQZ); open(IF); return 0; }
  void shortEnd(int) { close(); }

  // ---- conditions and control flow (structured: blocks, and br to the depth they were opened at) ----
  Cond compareCond(int o, bool lnum, bool rnum) {
    auto cmp = [&] { operands(), op(CMP[o - O_EQ]); };
    if (checks(lnum, rnum)) open(IF, I32, LIKELY), cmp(), op(ELSE), slowOp(o), truthy(), close();
    else cmp();
    return hasLit = false, Cond{};
  }
  Cond truthyCond() { return truthy(), Cond{}; }
  void thenBegin(Cond&) { open(IF); }
  int elseBegin(Cond&, bool hasElse) { if (hasElse) op(ELSE); return 0; }
  void ifEnd(int) { close(); }
  // block $exit { loop $top { [exit tests] block $cont { body } step; safepoint; br $top } }
  void loopBegin(Loop& lp) { open(BLOCK), lp.depth = F->blocks, open(LOOP); }
  void loopExit(Loop& lp, Cond&) { op(I32_EQZ), br(BR_IF, lp.depth, UNLIKELY); }
  void exitIfEnd(Loop& lp) { get(R0), i64c(END), op(I64_EQ), br(BR_IF, lp.depth, UNLIKELY); }
  void bodyBegin(Loop&) { open(BLOCK); }
  void contHere(Loop&) { close(); }
  void loopBack(Loop& lp) { poll(), br(BR, lp.depth + 1); }
  void loopEnd(Loop&) { close(), close(); }
  void jump(Loop& lp, bool brk) { br(BR, brk ? lp.depth : lp.depth + 2); }
  void leaveTries(size_t) {}  // wasm try blocks are structured: leaving one needs nothing
  // block $done { block $catch (result i64) { try_table (catch $fut -> $catch) { body } br $done } e = it; reset sp; ... }
  // A frame slot keeps vm->txn at entry: the handler rolls back the transactions opened since (rt_txunwind). Four
  // more keep the effect state (vm->allowed, vm->hstack, vm->who, vm->inv), which the handler puts back.
  void effSlot(int s, u32 at, bool w32) { base(s), i32c(0), mem(w32 ? I32_LOAD : I64_LOAD, w32 ? 2 : 3, at), w32 ? op(I64_EXTEND_U) : void(), mem(I64_STORE, 3, off(s)); }
  void effBack(int s, u32 at, bool w32) { i32c(0), base(s), mem(I64_LOAD, 3, off(s)), w32 ? op(I32_WRAP) : void(), mem(w32 ? I32_STORE : I64_STORE, w32 ? 2 : 3, at); }
  TryAt tryBegin() {
    TryAt t{F->depth};
    slot(), base(t.r), txnDepth(), op(I64_EXTEND_U), mem(I64_STORE, 3, off(t.r));
    slot(), slot(), slot(), slot();
    effSlot(t.r + 1, slotOf(&vm->allowed), false), effSlot(t.r + 2, slotOf(&vm->hstack), false), effSlot(t.r + 3, slotOf(&vm->who), true);
    effSlot(t.r + 4, slotOf(&vm->inv), false);
    open(BLOCK), open(BLOCK, I64), open(TRY_TABLE), u(1), b(0), u(0), u(0);  // one catch: tag 0, label 0
    return t;
  }
  void txnDepth() { i32c(0), mem(I32_LOAD, 2, slotOf(&vm->txn)); }
  void tryCatch(TryAt& t) {
    close(), br(BR, F->blocks - 1), close(), set(R0);
    effBack(t.r + 1, slotOf(&vm->allowed), false), effBack(t.r + 2, slotOf(&vm->hstack), false), effBack(t.r + 3, slotOf(&vm->who), true);
    effBack(t.r + 4, slotOf(&vm->inv), false);
    txnDepth(), base(t.r), mem(I64_LOAD, 3, off(t.r)), op(I32_WRAP), op(I32_GT_U), open(IF, VOID, UNLIKELY);
    ld(1, t.r), helper(H_TXUNWIND), close();
  }
  void tryEnd(TryAt&) { close(); }
  void rangeTest(Loop& lp, int a, int b, int v) {  // a < b, else exit; v = a (counter, end and v are numbers)
    get(wl(a)), get(wl(b)), op(F64_LT), op(I32_EQZ), br(BR_IF, lp.depth, UNLIKELY);
    if (isFReg(v)) get(wl(a)), set(wl(v));
    else base(v), get(wl(a)), op(I64_REINTERPRET), op(LOCAL_TEE), u(wl(v)), mem(I64_STORE, 3, off(v));
  }
  void rangeStep(int a) { get(wl(a)), op(F64_CONST), f64(1), op(F64_ADD), set(wl(a)); }
  bool isFReg(int h) { return h >= 0 && h < (int)F->wnum.size() && F->wnum[h]; }
  void fpAssign(int h, int o, bool rnum) {  // local h op= r1 (or the literal): two numbers stay in the f64 local
    u32 L = wl(h);
    get(L), get(L), op(F64_EQ);  // a number (a NaN takes the slow path, which handles it too)
    if (!rnum && !hasLit) isNumber(1), op(I32_AND);
    open(IF, VOID, LIKELY), get(L), hasLit ? (op(F64_CONST), f64(lit)) : (get(R0 + 1), op(F64_REINTERPRET)), op(F64_ADD + o), set(L);
    op(ELSE), get(L), op(I64_REINTERPRET), set(R0), slowOp(o);
    get(R0), op(F64_REINTERPRET), set(L), base(h), get(R0), mem(I64_STORE, 3, off(h)), close();
    hasLit = false;
  }

  // ---- fields: an inline cache per site, in the VM block: key = the instance header, val = the slot ----
  // list[i] (r0 = the list, r1 = the index; r2 = the value to store): inline when r0 is a list and 0 <= i < len,
  // which a NaN (any non-number) fails; a fraction truncates, as in rt_geti. Else, and for stores inside a
  // transaction (which save the list first), the helper.
  void index(bool store) {
    auto idx = [&] { get(R0 + 1), op(F64_REINTERPRET); };
    open(BLOCK), open(BLOCK);  // $done, $miss
    isPtr(0, false), br(BR_IF, F->blocks, UNLIKELY);
    addr(0), mem(I32_LOAD8_U, 0, 0), i32c((int)K::List), op(I32_NE), br(BR_IF, F->blocks, UNLIKELY);
    if (store) txnDepth(), br(BR_IF, F->blocks, UNLIKELY);
    idx(), op(F64_CONST), f64(0), op(F64_GE), op(I32_EQZ), br(BR_IF, F->blocks, UNLIKELY);
    addr(0), mem(I32_LOAD, 2, LIST_V), set(T2);  // T2 = begin
    idx(), addr(0), mem(I32_LOAD, 2, LIST_V + 4);  // the end or the length
    if (!vecCount) get(T2), op(I32_SUB), i32c(3), op(I32_SHR_U);
    op(F64_CONVERT_I32_U);
    op(F64_LT), op(I32_EQZ), br(BR_IF, F->blocks, UNLIKELY);
    get(T2), idx(), op(I32_TRUNC_F64_U), i32c(3), op(I32_SHL), op(I32_ADD), set(T);
    if (store) get(T), get(R0 + 2), mem(I64_STORE, 3, 0), mv(0, 2);
    else get(T), mem(I64_LOAD, 3, 0), set(R0);
    br(BR, F->blocks - 1), close();
    helper(store ? H_SETI : H_GETI), close();
  }
  // x.push(v), x in slot base, v in r0: appended inline when x is a list with spare capacity and no transaction
  // is open (l_push saves the list first); the result is x. Else the method call.
  void listPush(int base) {
    open(BLOCK), open(BLOCK);  // $done, $miss
    ld(1, base), isPtr(1, false), br(BR_IF, F->blocks, UNLIKELY);
    addr(1), mem(I32_LOAD8_U, 0, 0), i32c((int)K::List), op(I32_NE), br(BR_IF, F->blocks, UNLIKELY);
    txnDepth(), br(BR_IF, F->blocks, UNLIKELY);
    addr(1), mem(I32_LOAD, 2, LIST_V + 4), set(T);  // the end, or the count
    get(T), addr(1), mem(I32_LOAD, 2, LIST_V + 8), op(I32_GE_U), br(BR_IF, F->blocks, UNLIKELY);  // full
    if (vecCount) addr(1), mem(I32_LOAD, 2, LIST_V), get(T), i32c(3), op(I32_SHL), op(I32_ADD);
    else get(T);
    get(R0), mem(I64_STORE, 3, 0);
    addr(1), get(T), i32c(vecCount ? 1 : 8), op(I32_ADD), mem(I32_STORE, 2, LIST_V + 4), mv(0, 1);
    br(BR, F->blocks - 1), close();
    st(0, slot()), callWindow(base, 1, S_push, false), close();
  }
  void field(int64_t sym, bool store) {  // r0 = object (r2 = value to store); result in r0
    u32 ic = slotOf(&vm->ics[newIC(store)]);
    open(BLOCK), open(BLOCK);  // $done, $miss
    isPtr(0, false), br(BR_IF, F->blocks, UNLIKELY);
    addr(0), mem(I64_LOAD, 3, 0), i32c(0), mem(I64_LOAD, 3, ic), op(I64_NE), br(BR_IF, F->blocks, UNLIKELY);
    if (store) {  // the high half: 1, a `num` field; 2, a store from outside to a class with invariants (vm->invObj)
      i32c(0), mem(I32_LOAD, 2, ic + 12), i32c(1), op(I32_AND), isNumber(2), op(I32_EQZ), op(I32_AND), br(BR_IF, F->blocks, UNLIKELY);
      i32c(0), mem(I32_LOAD, 2, ic + 12), i32c(2), op(I32_AND), open(IF, VOID, UNLIKELY), i32c(0), get(R0), mem(I64_STORE, 3, slotOf(&vm->invObj)), close();
    }
    addr(0), mem(I32_LOAD, 2, INST_SLOTS), i32c(0), mem(I32_LOAD, 2, ic + 8), i32c(3), op(I32_SHL), op(I32_ADD), set(T);  // the slot (low half)
    if (store) get(T), get(R0 + 2), mem(I64_STORE, 3, 0), mv(0, 2);
    else get(T), mem(I64_LOAD, 3, 0), op(LOCAL_TEE), u(R0 + 3), i64c(ABSENT), op(I64_EQ), br(BR_IF, F->blocks, UNLIKELY), mv(0, 3);  // ABSENT: slow
    br(BR, F->blocks - 1), close();
    karg(1, sym), karg(store ? 3 : 2, ic), helper(store ? H_SETF : H_GETF), close();
  }
  // A method of a class with invariants: link its record (slots rec, rec + 1) into vm->inv; r0 = self, and then
  // whether this is an outside call. invLeave unlinks it.
  void invEnter(int rec) { addsp(1, rec), helper(H_INVENTER); }
  void invLeave(int rec) { i32c(0), base(rec), mem(I64_LOAD, 3, off(rec)), mem(I64_STORE, 3, slotOf(&vm->inv)); }
  // After a store from outside an object: r1 = vm->invObj (rt_setf: the object, whose invariants must hold now), which
  // is cleared; nothing (0): skip to invPendingEnd.
  int invPendingBegin() {
    i32c(0), mem(I64_LOAD, 3, slotOf(&vm->invObj)), set(R0 + 1), get(R0 + 1), i64c(0), op(I64_NE), open(IF, VOID, UNLIKELY);
    i32c(0), i64c(0), mem(I64_STORE, 3, slotOf(&vm->invObj));
    return 0;
  }
  void invPendingEnd(int) { close(); }
  void typeCheck(uint64_t t, int site, bool numFast) {  // r0 must have type t (a number: no call when numFast)
    if (numFast) isNumber(0), op(I32_EQZ), open(IF, VOID, UNLIKELY);
    karg(1, (int64_t)t), karg(2, site), helper(H_TYPECHK);
    if (numFast) close();
  }
  // A function that declares effects (see gen_a64.h): the caller's vm->allowed and vm->who go to two i64 locals.
  void effectsIn(uint64_t mask, int site) {
    u32 a = F->effHome = L0 + (u32)F->k.wlocals.size(), w = a + 1;
    F->k.wlocals.push_back(I64), F->k.wlocals.push_back(I64);
    i32c(0), mem(I64_LOAD, 3, slotOf(&vm->allowed)), set(a);
    i64c((int64_t)mask), get(a), i64c(-1), op(I64_XOR), op(I64_AND), i64c(0), op(I64_NE);  // any declared effect not allowed?
    open(IF, VOID, UNLIKELY), karg(0, site), helper(H_EFFECT), close();
    i32c(0), mem(I32_LOAD, 2, slotOf(&vm->who)), op(I64_EXTEND_U), set(w);
    i32c(0), get(a), i64c(EFF_LEVEL), op(I64_SHR_U), i64c(1), op(I64_ADD), i64c(EFF_LEVEL), op(I64_SHL), i64c((int64_t)mask), op(I64_OR);
    mem(I64_STORE, 3, slotOf(&vm->allowed)), i32c(0), i32c(site), mem(I32_STORE, 2, slotOf(&vm->who));
  }
  void capAddr() { base(F->cloHome), mem(I64_LOAD, 3, off(F->cloHome)), op(I32_WRAP); }  // this closure object
  void capLoad(int i) { capAddr(), mem(I64_LOAD, 3, CLO_ENV + 8 * i), set(R0); }
  void capStore(int i) {  // inside a transaction, rt_capset saves the closure first
    txnDepth(), open(IF, VOID, UNLIKELY);
    capAddr(), op(I64_EXTEND_U), set(R0 + 1), karg(2, i), helper(H_CAPSET);
    op(ELSE), capAddr(), get(R0), mem(I64_STORE, 3, CLO_ENV + 8 * i), close();
  }

  // ---- calls: the arguments are in consecutive frame slots (the window); the callee gets its address, this frame
  // as its stack pointer, and the first NA of the n values in the window as parameters (the last may be in r0) ----
  void callArgs(int base, int n, bool lastInR0) {
    window(base), get(FP);
    for (int i = 0; i < (int)NA; i++)
      if (i >= n) i64c(0);
      else if (lastInR0 && i == n - 1) get(R0);
      else get(FP), mem(I64_LOAD, 3, off(base + i));
  }
  void callCode(int base, int n, bool lastInR0 = false) {  // T = the callee's table index
    callArgs(base, n, lastInR0), get(T), callTable(0), set(R0), F->depth = base, F->calls = true;
  }
  void prep(int base, int n, int sym, int sup, u32 ic) {  // T = rt_prep(window, n, sym, super class (a global, or -1), ic)
    if (sup >= 0) gmem(3, sup);
    else karg(3, 0);
    warg(0, base), karg(1, n), karg(2, sym), karg(4, ic), helper(H_PREP), get(R0), op(I32_WRAP), set(T);
  }
  void callWindow(int base, int n, int sym, bool super) {
    if (sym && !super) {
      u32 ic = slotOf(&vm->ics[newIC()]);
      if (n) ld(0, base);  // the receiver (with no arguments, r0 still holds it: see Compiler::call)
      open(BLOCK), open(BLOCK);  // $done, $miss
      isPtr(0, false), br(BR_IF, F->blocks, UNLIKELY);
      addr(0), mem(I32_LOAD, 2, 4), i32c(0), mem(I32_LOAD, 2, ic + 4), op(I32_NE), br(BR_IF, F->blocks, UNLIKELY);
      i32c(0), mem(I32_LOAD, 2, ic + 8), set(T), br(BR, F->blocks - 1), close();  // the method (low half)
      prep(base, n, sym, -1, ic), close();
    } else prep(base, n, sym, super ? global(F->cls).slot : -1, 0);
    callCode(base, n + 1);  // (calling a plain function, rt_prep shifted the window down: one value is junk)
  }
  // C(args) for a top-level class C: allocated inline on an inline-cache hit, as in gen_a64.h.
  void ld32(u32 addr, u32 off = 0) { get(addr), mem(I32_LOAD, 2, off); }
  void constructCall(int base, int n) {
    u32 ic = slotOf(&vm->ics[newIC()]);
    open(BLOCK), open(BLOCK);  // $done, $miss
    ld(0, base), get(R0), i32c(0), mem(I64_LOAD, 3, ic), op(I64_NE), br(BR_IF, F->blocks, UNLIKELY);
    addr(0), set(T2);                                                                     // T2 = the class
    i32c((int32_t)slotOf(vm)), ld32(T2, CLS_IRUN), op(I32_ADD), set(T3);                  // T3 = its run
    ld32(T3), set(T4), get(T4), ld32(T2, CLS_IRUN + 4), op(I32_ADD), set(T);              // T4 = cur, T = cur + size
    get(T), ld32(T3, 4), op(I32_GT_U), br(BR_IF, F->blocks, UNLIKELY);                    // the run is used up
    get(T3), get(T), mem(I32_STORE, 2, 0);
    get(T4), get(T2), mem(I64_LOAD, 3, CLS_IHDR), mem(I64_STORE, 3, 0);                  // header
    get(T4), get(T2), mem(I32_STORE, 2, offsetof(Inst, cls));
    get(T4), get(T4), i32c(sizeof(Inst)), op(I32_ADD), op(LOCAL_TEE), u(T2), mem(I32_STORE, 2, INST_SLOTS);
    open(BLOCK), open(LOOP);  // every slot, and any rounding slack after them, = ABSENT
    get(T2), get(T), op(I32_GE_U), br(BR_IF, F->blocks - 1);
    get(T2), i64c(ABSENT), mem(I64_STORE, 3, 0), get(T2), i32c(8), op(I32_ADD), set(T2), br(BR, F->blocks), close(), close();
    get(T4), op(I64_EXTEND_U), i64c(PTR), op(I64_OR), set(R0), st(0, base);
    i32c(0), mem(I64_LOAD, 3, ic + 8), op(I32_WRAP), set(T), br(BR, F->blocks - 1), close();
    prep(base, n, 0, -1, ic), close();
    callCode(base, n + 1);
  }
  void rel(u32* stub, RelKind kind) {  // a field wasm.cpp fills in (see RelKind)
    auto& k = F->k;
    k.relKind.resize(k.rel.size()), k.rel.push_back({(int)k.c.size(), stub}), k.relKind.push_back(kind);
    for (int i = 0; i < (kind == REL_CALL ? 9 : 5); i++) b(0);
  }
  // A known top-level function: if its body is in this module, a direct call (V8 runs it faster than one through
  // the table, and can inline it), else through its stub. A body the stub no longer leads to sends a direct call
  // on through the stub (finish).
  void callDirect(int base, int n, u32* stub) {  // the last argument is in r0
    if (n) st(0, slot());
    callArgs(base, n, true), rel(stub, REL_CALL);
    set(R0), F->depth = base, F->calls = true;
  }
  void closure(Fn* fn, int base, int n) {
    int c = sizeClass(CLO_ENV + 8 * n);
    bool inl = c < NCLS && sizeClasses[c] < 4096;
    if (inl) {
      u32 run = slotOf(&vm->runs[c]);
      open(BLOCK), open(BLOCK);  // $done, $full
      i32c(0), mem(I32_LOAD, 2, run), op(LOCAL_TEE), u(T4), i32c(sizeClasses[c]), op(I32_ADD), op(LOCAL_TEE), u(T);
      i32c(0), mem(I32_LOAD, 2, run + 4), op(I32_GT_U), br(BR_IF, F->blocks, UNLIKELY);
      i32c(0), get(T), mem(I32_STORE, 2, run);
      get(T4), i64c((int64_t)K::Clo), mem(I64_STORE, 3, 0);
      get(T4), i32c((int32_t)slotOf(fn)), mem(I32_STORE, 2, offsetof(Clo, fn)), get(T4), i32c(n), mem(I32_STORE, 2, offsetof(Clo, n));
      for (int i = 0; i < n; i++) get(T4), this->base(base + i), mem(I64_LOAD, 3, off(base + i)), mem(I64_STORE, 3, CLO_ENV + 8 * i);
      get(T4), op(I64_EXTEND_U), i64c(PTR), op(I64_OR), set(R0), br(BR, F->blocks - 1), close();
    }
    karg(0, (int64_t)box(fn)), karg(1, n), warg(2, base), helper(H_CLOSURE), F->depth = base;
    if (inl) close();
  }
  void traceTouch(uint64_t*) {}  // fut test --changed records traces natively only
  // Each function counts its calls; at HOT, rt_hot(tier, its arguments, how many) sees which are numbers and calls tierUp.
  void hotCounter(void* tier, int first, const std::vector<int>& homes) {
    // No declared parameters, nothing to speculate on: tier 2 would only move the body to a module of its own,
    // where V8 can no longer inline it into its callers; and the counter makes it bigger.
    if ((int)homes.size() == first) return;
    u32 ic = slotOf(&vm->ics[newIC()]);
    i32c(0), i32c(0), mem(I64_LOAD, 3, ic + 8), i64c(1), op(I64_ADD), op(LOCAL_TEE), u(R0), mem(I64_STORE, 3, ic + 8);
    get(R0), i64c(HOT), op(I64_EQ), open(IF, VOID, UNLIKELY);
    get(W), i32c(8 * first), op(I32_ADD), op(I64_EXTEND_U), set(R0 + 1);
    karg(0, slotOf(tier)), karg(2, std::min((int)homes.size() - first, 7)), helper(H_HOT), close();
  }
};
