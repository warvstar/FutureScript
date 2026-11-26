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
    if (ln.empty() || ln.back().second != prev.line) ln.push_back({(int)F->k.c.size(), prev.line});
    b(o);
  }
  void u(uint64_t v) { uleb(F->k.c, v); }
  void get(u32 l) { op(LOCAL_GET), u(l); }
  void set(u32 l) { op(LOCAL_SET), u(l); }
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
  void isNumber(u32 r) { get(R0 + r), i64c(QNAN), op(I64_AND), i64c(QNAN), op(I64_NE); }
  void truthy() { get(R0), i64c(~1ll), op(I64_AND), i64c(QNAN), op(I64_NE); }
  void callTable(u32 type) { op(CALL_INDIRECT), u(type), u(0); }  // the table index is on the stack
  void helper(int h) {  // arguments in r0.., result in r0
    const char* s = HSIG[h];
    for (u32 i = 0; s[i] != ':'; i++) get(R0 + i), s[i] == 'i' ? op(I32_WRAP) : void();
    i32c(slotOf(vm.helpers[h])), callTable(1 + h);
    if (char r = strchr(s, ':')[1]) r == 'i' ? (op(I64_EXTEND_U), set(R0)) : set(R0);
  }
  void imm(u32 r, V v) { i64c((int64_t)v), set(R0 + r); }
  void mv(u32 d, u32 s) { get(R0 + s), set(R0 + d); }
  void addImm(u32 r, u32 k) { get(R0 + r), i64c(k), op(I64_ADD), set(R0 + r); }
  void gmem(u32 r, int g, bool store = false) {
    i32c(0);
    store ? (get(R0 + r), mem(I64_STORE, 3, slotOf(&vm.globals[g]))) : (mem(I64_LOAD, 3, slotOf(&vm.globals[g])), set(R0 + r));
  }
  void ld(u32 r, int s) { get(FP), mem(I64_LOAD, 3, 8 * s), set(R0 + r); }
  void st(u32 r, int s) { get(FP), get(R0 + r), mem(I64_STORE, 3, 8 * s); }
  void window(int s) { get(FP), i32c(8 * s), op(I32_ADD); }  // address of slot s (an i32 on the stack)
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
  int numHome() { return fhome(); }  // a range loop's variable
  void ldl(u32 r, int h) { get(wl(h)), isFReg(h) ? op(I64_REINTERPRET) : void(), set(R0 + r); }
  void stl(u32 r, int h) {
    unprove(h);
    if (isFReg(h)) get(R0 + r), op(F64_REINTERPRET), set(wl(h)), get(FP), get(R0 + r), mem(I64_STORE, 3, 8 * h);
    else get(FP), get(R0 + r), op(LOCAL_TEE), u(wl(h)), mem(I64_STORE, 3, 8 * h);
  }
  void param(int i, int h) { unprove(h), get(FP), get(W), mem(I64_LOAD, 3, 8 * i), op(LOCAL_TEE), u(wl(h)), mem(I64_STORE, 3, 8 * h); }
  void poll() {  // safepoint: hot reload, REPL lines, garbage collection
    i32c(0), mem(I32_LOAD, 2, slotOf(&vm.poll)), open(IF, VOID, UNLIKELY), i32c(slotOf(vm.helpers[H_POLL])), callTable(1 + H_POLL), close();
  }
  void ret() { if (F->init) ldl(0, F->selfHome); br(BR, 0), F->live = false; }  // to the epilogue

  // Frame: sp -= size (the shadow stack, shared with C++), a stack check and a safepoint, then the body in a block
  // that `return` leaves, then sp += size and the result. A throw that unwinds the frame skips the epilogue: the
  // catching function's handler resets sp to its own frame.
  Code finish() {
    F->init ? ldl(0, F->selfHome) : imm(0, NIL);  // falling off the end (dead code after a return)
    if (F->maxd > 4000) fail("function too large");
    u32 size = (F->maxd * 8 + 15) & ~15u;
    op(ENDOP), get(FP), i32c(size), op(I32_ADD), op(GLOBAL_SET), u(0), get(R0), op(ENDOP);  // the body block; epilogue
    Code& k = F->k;
    std::vector<uint8_t> body = std::move(k.c);
    auto lines = std::move(k.lines);
    auto hints = std::move(k.hints);
    k.c.clear(), k.lines.clear(), k.hints.clear();
    for (u32 i : F->guard) {  // tier 2: a speculated argument that is not a number continues in the tier-1 body
      get(W), mem(I64_LOAD, 3, 8 * i), i64c(QNAN), op(I64_AND), i64c(QNAN), op(I64_EQ), open(IF, VOID, UNLIKELY);
      get(W), i32c((int32_t)slotOf(deopt)), op(RETURN_CALL_INDIRECT), u(0), u(0), close();  // a tail call: no extra frame
    }
    op(GLOBAL_GET), u(0), i32c(size), op(I32_SUB), op(LOCAL_TEE), u(FP), op(GLOBAL_SET), u(0);
    get(FP), i32c(0), mem(I32_LOAD, 2, slotOf(&vm.limit)), op(I32_LT_U);
    open(IF, VOID, UNLIKELY), i32c(slotOf(vm.helpers[H_STACK])), callTable(1 + H_STACK), close();
    poll(), b(BLOCK), b(VOID);
    int n = (int)k.c.size();
    k.c.insert(k.c.end(), body.begin(), body.end());
    k.lines = {{0, lines.empty() ? prev.line : lines[0].second}};
    for (auto [at, line] : lines) k.lines.push_back({at + n, line});
    for (auto& r : k.rel) r.first += n;
    for (auto [at, h] : hints) k.hints.push_back({at + n, h});
    return std::move(k);
  }

  // ---- operators: r0 = left, r1 = right (or the literal `lit`). Two numbers: inline; anything else: rt_binop ----
  void slowOp(int op) {
    if (hasLit) imm(1, box(lit));
    imm(2, op), helper(H_BINOP);
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
  TryAt tryBegin() {
    TryAt t{F->depth};
    open(BLOCK), open(BLOCK, I64), open(TRY_TABLE), u(1), b(0), u(0), u(0);  // one catch: tag 0, label 0
    return t;
  }
  void tryCatch(TryAt&) { close(), br(BR, F->blocks - 1), close(), set(R0), get(FP), op(GLOBAL_SET), u(0); }
  void tryEnd(TryAt&) { close(); }
  void rangeTest(Loop& lp, int a, int b, int v) {  // a < b, else exit; v = a (counter, end and v are numbers)
    get(wl(a)), get(wl(b)), op(F64_LT), op(I32_EQZ), br(BR_IF, lp.depth, UNLIKELY);
    if (isFReg(v)) get(wl(a)), set(wl(v));
    else get(FP), get(wl(a)), op(I64_REINTERPRET), op(LOCAL_TEE), u(wl(v)), mem(I64_STORE, 3, 8 * v);
  }
  void rangeStep(int a) { get(wl(a)), op(F64_CONST), f64(1), op(F64_ADD), set(wl(a)); }
  bool isFReg(int h) { return h >= 0 && h < (int)F->wnum.size() && F->wnum[h]; }
  void fpAssign(int h, int o, bool rnum) {  // local h op= r1 (or the literal): two numbers stay in the f64 local
    u32 L = wl(h);
    get(L), get(L), op(F64_EQ);  // a number (a NaN takes the slow path, which handles it too)
    if (!rnum && !hasLit) isNumber(1), op(I32_AND);
    open(IF, VOID, LIKELY), get(L), hasLit ? (op(F64_CONST), f64(lit)) : (get(R0 + 1), op(F64_REINTERPRET)), op(F64_ADD + o), set(L);
    op(ELSE), get(L), op(I64_REINTERPRET), set(R0), slowOp(o);
    get(R0), op(F64_REINTERPRET), set(L), get(FP), get(R0), mem(I64_STORE, 3, 8 * h), close();
    hasLit = false;
  }

  // ---- fields: an inline cache per site, in the VM block: key = the instance header, val = the slot ----
  void field(int sym, bool store) {  // r0 = object (r2 = value to store); result in r0
    u32 ic = slotOf(&vm.ics[newIC()]);
    open(BLOCK), open(BLOCK);  // $done, $miss
    get(R0), i64c(PTR), op(I64_AND), i64c(PTR), op(I64_NE), br(BR_IF, F->blocks, UNLIKELY);
    addr(0), mem(I64_LOAD, 3, 0), i32c(0), mem(I64_LOAD, 3, ic), op(I64_NE), br(BR_IF, F->blocks, UNLIKELY);
    addr(0), mem(I32_LOAD, 2, INST_SLOTS), i32c(0), mem(I64_LOAD, 3, ic + 8), op(I32_WRAP), i32c(3), op(I32_SHL), op(I32_ADD), set(T);
    if (store) get(T), get(R0 + 2), mem(I64_STORE, 3, 0), mv(0, 2);
    else get(T), mem(I64_LOAD, 3, 0), op(LOCAL_TEE), u(R0 + 3), i64c(ABSENT), op(I64_EQ), br(BR_IF, F->blocks, UNLIKELY), mv(0, 3);  // ABSENT: slow
    br(BR, F->blocks - 1), close();
    imm(1, sym), imm(store ? 3 : 2, ic), helper(store ? H_SETF : H_GETF), close();
  }
  void capAddr() { get(FP), mem(I64_LOAD, 3, 8 * F->cloHome), op(I32_WRAP); }  // this closure object
  void capLoad(int i) { capAddr(), mem(I64_LOAD, 3, CLO_ENV + 8 * i), set(R0); }
  void capStore(int i) { capAddr(), get(R0), mem(I64_STORE, 3, CLO_ENV + 8 * i); }

  // ---- calls: the arguments are in consecutive frame slots (the window); the callee gets its address ----
  void callCode(int base) {  // T = the callee's table index
    window(base), get(T), callTable(0), set(R0), F->depth = base, F->calls = true;
  }
  void prep(int base, int n, int sym, int sup, u32 ic) {  // T = rt_prep(window, n, sym, super class (a global, or -1), ic)
    addsp(0, base), imm(1, n), imm(2, sym);
    sup >= 0 ? gmem(3, sup) : imm(3, 0);
    imm(4, ic), helper(H_PREP), get(R0), op(I32_WRAP), set(T);
  }
  void callWindow(int base, int n, int sym, bool super) {  // window[0] = callee or receiver
    if (sym && !super) {  // method call: inline cache on the receiver's class id
      u32 ic = slotOf(&vm.ics[newIC()]);
      ld(0, base);
      get(R0), i64c(PTR), op(I64_AND), i64c(PTR), op(I64_EQ);
      open(IF, I32, LIKELY), addr(0), mem(I32_LOAD, 2, 4), i32c(0), mem(I32_LOAD, 2, ic + 4), op(I32_EQ), op(ELSE), i32c(0), close();
      open(IF, VOID, LIKELY), i32c(0), mem(I64_LOAD, 3, ic + 8), op(I32_WRAP), set(T), op(ELSE), prep(base, n, sym, -1, ic), close();
    } else prep(base, n, sym, super ? global(F->cls).slot : -1, 0);
    callCode(base);
  }
  void constructCall(int base, int n) { prep(base, n, 0, -1, 0), callCode(base); }
  void callDirect(int base, int n, u32* stub) {  // the last argument is in r0
    if (n) st(0, slot());
    op(I32_CONST), F->k.rel.push_back({(int)F->k.c.size(), stub});  // the stub's slot: wasm.cpp writes it (5 bytes)
    for (int i = 0; i < 5; i++) b(0);
    set(T), callCode(base);
  }
  void closure(Fn* fn, int base, int n) { imm(0, box(fn)), imm(1, n), addsp(2, base), helper(H_CLOSURE), F->depth = base; }
  // Each function counts its calls; at HOT, rt_hot(tier, its arguments, how many) sees which are numbers and calls tierUp.
  void hotCounter(void* tier, int first, const std::vector<int>& homes) {
    u32 ic = slotOf(&vm.ics[newIC()]);
    i32c(0), i32c(0), mem(I64_LOAD, 3, ic + 8), i64c(1), op(I64_ADD), op(LOCAL_TEE), u(R0), mem(I64_STORE, 3, ic + 8);
    get(R0), i64c(HOT), op(I64_EQ), open(IF, VOID, UNLIKELY);
    imm(0, (V)slotOf(tier)), get(W), i32c(8 * first), op(I32_ADD), op(I64_EXTEND_U), set(R0 + 1);
    imm(2, std::min((int)homes.size() - first, 7)), helper(H_HOT), close();
  }
};
