// arm64 code generator: the parser (compile.cpp) calls these to emit machine code directly.
// Registers: x0 holds the value being computed, x1/x2 the other operands; x19..x25 and d8..d15 hold locals;
// x26 = stack limit, x27 = the NaN tag, x28 = the VM block.
#pragma once
#include "compile.h"
using namespace a64;

struct Gen : CState {
  using Cond = Jumps;  // the branches taken when a condition is false
  static int fimm8(double v) {  // the 8-bit `fmov d, #imm` encoding of v, or -1: +-(16..31)/16 * 2^(-3..4)
    uint64_t b = std::bit_cast<uint64_t>(v), e = b >> 52 & 0x7ff;
    if ((b & 0xffffffffffff) || e < 0x3fc || e > 0x403) return -1;
    return int((b >> 63) << 7 | uint64_t(e < 0x400) << 6 | (e & 3) << 4 | (b >> 48 & 15));
  }

  // ---- emission ----
  int E(u32 i) {
    auto& c = F->k.c;
    if ((int)c.size() > F->label) {  // peephole, unless a branch may land between the two instructions
      u32 p = c.back();
      if ((p & 0xffe0ffe0) == mov(0, 0) && i == mov(p >> 16 & 31, p & 31)) return (int)c.size() - 1;  // mov a,b; mov b,a
      if ((p & 0xffc00000) == str(0, 0, 0) && i == (p | ldr(0, 0, 0))) return (int)c.size() - 1;  // str r,[m]; ldr r,[m]
    }
    auto& ln = F->k.lines;
    if (ln.empty() || ln.back().second != prev.line) ln.push_back({(int)F->k.c.size(), prev.line});
    return c.push_back(i), (int)c.size() - 1;
  }
  int here() { return F->label = (int)F->k.c.size(); }  // a branch target: peepholes must not cross it
  void patch(int at, int to = -1) {  // fix up a b / b.cond / cbz emitted at `at` to jump to `to` (default: here)
    u32& i = F->k.c[at];
    u32 d = u32((to < 0 ? here() : to) - at);
    i |= (i >> 26) == 5 ? d & 0x3ffffff : (d & 0x7ffff) << 5;
  }
  void helper(int h) { E(ldr(16, 28, HOFF(h))), E(blr(16)); }
  void imm(u32 r, V v) {
    if (v >= QNAN && v <= ABSENT) return (void)E(v == QNAN ? mov(r, 27) : addi(r, 27, u32(v - QNAN)));  // nil, false, true..
    bool first = true;
    for (u32 hw = 0; hw < 4; hw++)
      if (u32 part = (v >> (16 * hw)) & 0xffff) E(first ? movz(r, part, hw) : movk(r, part, hw)), first = false;
    if (first) E(movz(r, 0, 0));
  }
  void mv(u32 d, u32 s) { E(mov(d, s)); }
  void addImm(u32 r, u32 k) { E(addi(r, r, k)); }  // r += k, as a raw integer
  void gmem(u32 r, int g, bool store = false) {  // ldr/str r, globals[g]; far slots go through x16
    u32 off = GOFF(g), base = 28;
    if (off >= 32760) E(addhi(16, 28, off >> 12)), base = 16, off &= 0xfff;
    E(store ? str(r, base, off) : ldr(r, base, off));
  }
  void ld(u32 r, int s) { E(ldr(r, SP, s * 8)); }
  void st(u32 r, int s) { E(str(r, SP, s * 8)); }
  void addsp(u32 d, int s) {  // d = address of slot s
    u32 off = s * 8;
    if (off >> 12) E(addhi(d, SP, off >> 12)), E(addi(d, d, off & 0xfff));
    else E(addi(d, SP, off));
  }
  // Locals live in callee-saved x19..x25 while they last, then in slots.
  int home() {
    if (F->nreg < 7 && !F->noRegs) return F->maxreg = std::max(F->maxreg, ++F->nreg), -F->nreg;
    return slot();
  }
  // A local known to start as a number (or a range counter) may live in d8..d15 instead: home -(100 + d).
  // Arithmetic on it then stays in the FP unit, so `s += x` in a loop is one fadd on the loop-carried path.
  int fhome() {
    if (F->nfreg == 8 || F->noRegs) return home();
    F->maxfreg = std::max(F->maxfreg, F->nfreg + 1);
    return -(108 + F->nfreg++);
  }
  int numHome() { return home(); }  // a range loop's variable
  static bool isF(int h) { return h <= -100; }
  static u32 dOf(int h) { return u32(-h - 100); }
  static u32 regOf(int h) { return u32(18 - h); }
  void ldl(u32 r, int h) {
    if (isF(h)) E(fmovxd(r, dOf(h)));
    else if (h >= 0) ld(r, h);
    else if (r != regOf(h)) E(mov(r, regOf(h)));
  }
  void stl(u32 r, int h) {
    unprove(h);
    if (isF(h)) E(fmovdx(dOf(h), r));
    else if (h >= 0) st(r, h);
    else if (r != regOf(h)) E(mov(regOf(h), r));
  }
  void param(int i, int h) { i < 8 ? stl(i, h) : (E(ldr(9, 8, 8 * i)), stl(9, h)); }  // x8: the argument window
  u32 fload(int h, u32 d) { return isF(h) ? dOf(h) : (ldl(9, h), E(fmovdx(d, 9)), d); }  // the d-register holding h
  void falsy() { E(andnot1(9, 0)), E(cmp(9, 27)); }  // flags EQ <=> x0 is nil/false
  void poll() { E(ldrw(16, 28, 0)), E(cbzw(16) | 3 << 5), helper(H_POLL); }  // hot-reload safepoint
  void ret() { if (F->init) ldl(0, F->selfHome); F->rets.push_back(E(B)), F->live = false; }

  // The prologue is prepended once the frame size and the callee-saved registers used are known:
  //   stp x29,x30 / mov x29,sp / sub sp / stp x19..x26 pairs below x29 / stack check (only if it calls)
  Code finish() {
    int end = (int)F->k.c.size();
    if (F->rets.empty() || F->rets.back() != end - 1 || F->label >= end) F->init ? ldl(0, F->selfHome) : (void)E(mov(0, 27));
    else F->k.c.pop_back(), F->rets.pop_back();  // ends in an unbranched-to `return`: fall into the epilogue
    if (F->maxd > 4000) fail("function too large");
    if (F->noRegs) F->maxreg = 7, F->maxfreg = 8;
    int pairs = (F->maxreg + 1) / 2, fpairs = (F->maxfreg + 1) / 2;  // x19.. then d8.. pairs, just below x29
    u32 bytes = ((F->maxd * 8 + 15) & ~15) + 16 * (pairs + fpairs);
    for (int r : F->rets) patch(r);
    for (int k = 0; k < pairs; k++) E(ldp_off(19 + 2 * k, 20 + 2 * k, 29, -16 * (k + 1)));
    for (int k = 0; k < fpairs; k++) E(ldpd(8 + 2 * k, 9 + 2 * k, 29, -16 * (pairs + k + 1)));
    E(addi(SP, 29, 0)), E(ldp_post(29, 30)), E(RET);
    std::vector<u32> pro{stp_pre(29, 30), addi(29, SP, 0)};
    if (bytes & 0xfff) pro.push_back(subi(SP, SP, bytes & 0xfff));
    if (bytes >> 12) pro.push_back(subhi(SP, SP, bytes >> 12));
    for (int k = 0; k < pairs; k++) pro.push_back(stp_off(19 + 2 * k, 20 + 2 * k, 29, -16 * (k + 1)));
    for (int k = 0; k < fpairs; k++) pro.push_back(stpd(8 + 2 * k, 9 + 2 * k, 29, -16 * (pairs + k + 1)));
    if (F->calls) pro.insert(pro.end(), {cmpsp(26), bcond(HS) | 3 << 5, ldr(16, 28, HOFF(H_STACK)), blr(16)});
    for (size_t i = F->guard.size(); i-- > 0;) pro.insert(pro.begin(), {mvn(9, F->guard[i]), tstq(9), bcond(EQ)});  // not a number: deopt
    Code& k = F->k;
    int n = (int)pro.size();
    k.c.insert(k.c.begin(), pro.begin(), pro.end());
    for (auto& r : k.rel) r.first += n;
    for (auto& l : k.lines) l.first += n;
    k.lines.insert(k.lines.begin(), {0, k.lines.empty() ? prev.line : k.lines[0].second});
    if (!F->guard.empty()) {  // the deopt branch (to the tier-1 body) goes last; tierUp fills in its target
      for (size_t i = 0; i < F->guard.size(); i++) k.c[3 * i + 2] |= u32((k.c.size() - (3 * i + 2)) & 0x7ffff) << 5;
      k.c.push_back(B);
    }
    return std::move(k);
  }

  // ---- operators: x0 = left, x1 = right. Inline double fast path; everything else goes to rt_binop ----
  static constexpr u32 CC[] = {EQ, NE, MI, LS, GT, GE};  // O_EQ..O_GE after fcmp (false when unordered)
  // A number-literal right operand is not loaded into x1: the fast path materialises it straight into d1
  // (one fmov), and only the slow path boxes it into x1.
  void d1Right() {
    int k = hasLit ? fimm8(lit) : -1;
    if (hasLit && k < 0 && lit != 0) imm(1, box(lit));
    E(k >= 0 ? fmovi(1, k) : fmovdx(1, hasLit && lit == 0 ? 31 : 1));  // 31: xzr, i.e. +0.0
  }
  void numCheck(Jumps& slow, bool lnum, bool rnum) {  // branch to slow unless x0 and x1 are numbers
    for (u32 r = 0; r < 2; r++) if (!(r ? rnum : lnum)) E(mvn(9, r)), E(tstq(9)), slow.push_back(E(bcond(EQ)));
    E(fmovdx(0, 0)), d1Right();
  }
  void slowOp(const Jumps& slow, int op) {  // the rt_binop path, reached by the branches in slow
    for (int j : slow) patch(j);
    if (hasLit) imm(1, box(lit));
    imm(2, op), helper(H_BINOP);
  }
  void binop(int op, bool lnum = false, bool rnum = false) {  // lnum/rnum: x0/x1 known to be numbers
    if (op >= O_BAND) return slowOp({}, op), (void)(hasLit = false);  // integer operators: in rt_binop
    Jumps slow;
    int done = -1;
    if (op != O_MOD) {
      numCheck(slow, lnum, rnum);
      if (op <= O_DIV) E(farith(op, 0, 0, 1)), E(fmovxd(0, 0));
      else E(fcmp(0, 1)), E(addi(10, 27, 1)), E(cinc(0, 10, CC[op - O_EQ]));
      if (slow.n) done = E(B);
    }
    if (op == O_MOD || slow.n) slowOp(slow, op);
    if (done >= 0) patch(done);
    hasLit = false;
  }
  void notOp() { falsy(), E(addi(10, 27, 1)), E(cinc(0, 10, EQ)); }  // x0 = !x0
  int shortBegin(bool isAnd) { return falsy(), E(bcond(isAnd ? EQ : NE)); }  // && / ||: skip the right side
  void shortEnd(int j) { patch(j); }

  // ---- conditions and control flow ----
  Cond compareCond(int op, bool lnum, bool rnum) {  // `a < b {`: compare and branch, no boolean built
    Jumps no, slow;
    numCheck(slow, lnum, rnum);
    E(fcmp(0, 1)), no.push_back(E(bcond(CC[op - O_EQ] ^ 1)));
    if (slow.n) {
      int yes = E(B);
      slowOp(slow, op), falsy(), no.push_back(E(bcond(EQ))), patch(yes);
    }
    return hasLit = false, no;
  }
  Cond truthyCond() { Jumps no; falsy(), no.push_back(E(bcond(EQ))); return no; }
  void thenBegin(Cond&) {}
  int elseBegin(Cond& no, bool hasElse) {
    int e = hasElse ? E(B) : -1;
    for (int j : no) patch(j);
    return e;
  }
  void ifEnd(int e) { if (e >= 0) patch(e); }
  // A loop: top: [exit tests] body cont: step, safepoint, b top; exits and breaks land after it.
  void loopBegin(Loop& lp) { lp.top = here(); }
  void loopExit(Loop& lp, Cond& c) { for (int j : c) lp.exit.push_back(j); }
  void exitIfEnd(Loop& lp) { E(addi(9, 27, 3)), E(cmp(0, 9)), lp.exit.push_back(E(bcond(EQ))); }  // x0 == END
  void bodyBegin(Loop&) {}
  void contHere(Loop& lp) { lp.contAt = here(); }
  void loopBack(Loop& lp) { poll(), patch(E(B), lp.top); }
  void loopEnd(Loop& lp) {
    for (int e : lp.exit) patch(e);
    for (int b : lp.brk) patch(b);
    for (int c : lp.cont) patch(c, lp.contAt);
  }
  void jump(Loop& lp, bool brk) { (brk ? lp.brk : lp.cont).push_back(E(B)); }
  void leaveTries(size_t k) { if (F->tries.size() > k) ld(9, F->tries[k]), E(str(9, 28, VM_HANDLER)); }  // unlink tries k..
  // try { } catch e { }: the record (prev, sp, fp, x26, handler pc) lives in five frame slots and is linked
  // into vm.handler; a throw (or runtime error) unlinks it and jumps to the handler with x0 = the value.
  TryAt tryBegin() {
    if (!F->noRegs) fail("internal: try in a function not prepared for it");
    TryAt t{F->depth};
    for (int i = 0; i < 5; i++) slot();
    addsp(9, t.r), E(ldr(10, 28, VM_HANDLER)), E(str(10, 9, 0)), E(addi(10, SP, 0)), E(str(10, 9, 8));
    E(str(29, 9, 16)), E(str(26, 9, 24));
    t.pc = E(ADR | 10);
    E(str(10, 9, 32)), E(str(9, 28, VM_HANDLER));
    return t;
  }
  void tryCatch(TryAt& t) { ld(9, t.r), E(str(9, 28, VM_HANDLER)), t.done = E(B), patch(t.pc); }  // unlink; the handler
  void tryEnd(TryAt& t) { patch(t.done); }
  // for x in a..b: the counter and end live in d8..d15 while free, so the loop-carried chain is one fadd
  void rangeTest(Loop& lp, int a, int b, int v) {
    u32 c = fload(a, 0);
    E(fcmp(c, fload(b, 1))), lp.exit.push_back(E(bcond(PL)));
    v < 0 ? (void)E(fmovxd(regOf(v), c)) : (E(fmovxd(0, c)), st(0, v));
  }
  void rangeStep(int a) {
    u32 c = fload(a, 0);
    E(FMOV1), E(farith(O_ADD, c, c, 1));
    if (!isF(a)) E(fmovxd(0, c)), stl(0, a);
  }
  bool isFReg(int h) { return isF(h); }
  void fpAssign(int h, int op, bool rnum) {  // local in d-register h op= x1
    u32 d = dOf(h);
    Jumps slow;
    E(fmovxd(9, d)), E(mvn(9, 9)), E(tstq(9)), slow.push_back(E(bcond(EQ)));  // off the d -> d dependency chain
    if (!rnum) E(mvn(9, 1)), E(tstq(9)), slow.push_back(E(bcond(EQ)));
    d1Right(), E(farith(op, d, d, 1));
    int done = E(B);
    for (int j : slow) patch(j);
    E(fmovxd(0, d)), slowOp({}, op), E(fmovdx(d, 0)), patch(done);
    hasLit = false;
  }

  // ---- inline caches live in the VM block: r = its 4KB page (one instruction), lo = its offset there ----
  u32 icAt(u32 r) {
    u32 off = IOFF(newIC());
    E(addhi(r, 28, off >> 12));
    return off & 0xfff;
  }
  void isObject(Jumps& miss) { E(mvn(9, 0)), E(tstptr(9)), miss.push_back(E(bcond(NE))), E(andaddr(9, 0)); }  // x9 = address
  void field(int sym, bool set) {  // x0 = object (x2 = value to store); result in x0
    Jumps miss;
    u32 ic = set ? 3 : 2, lo = icAt(ic);
    isObject(miss);
    E(ldr(10, 9, 0)), E(ldr(11, ic, lo)), E(cmp(10, 11)), miss.push_back(E(bcond(NE)));  // same class and slot count
    E(ldr(11, ic, lo + 8)), E(ldr(10, 9, INST_SLOTS));
    if (set) E(strreg(2, 10, 11)), E(mov(0, 2));
    else E(ldrreg(10, 10, 11)), E(addi(12, 27, 4)), E(cmp(10, 12)), miss.push_back(E(bcond(EQ))), E(mov(0, 10));  // ABSENT: slow
    int done = E(B);
    for (int m : miss) patch(m);
    if (lo) E(addi(ic, ic, lo));
    imm(1, sym), helper(set ? H_SETF : H_GETF), patch(done);
  }
  void cloEnv() { ldl(9, F->cloHome), E(andaddr(9, 9)); }  // x9 = this closure object
  void capLoad(int i) { cloEnv(), E(ldr(0, 9, CLO_ENV + 8 * i)); }
  void capStore(int i) { cloEnv(), E(str(0, 9, CLO_ENV + 8 * i)); }

  // ---- calls: arguments go to consecutive slots (the "window"); x0..x7 carry the first 8, x8 points at all ----
  void args(int base, int n) {  // x0..x7 = the first 8; x8 = the window, read only for parameters past the 8th
    if (n > 8) addsp(8, base);
    for (int i = 0; i < n && i < 8; i++) ld(i, base + i);
  }
  void callWindow(int base, int n, int sym, bool super) {  // window[0] = callee or receiver
    Jumps miss;
    int have = -1;
    if (sym && !super) {  // method call: inline cache on the receiver's class id
      u32 lo = icAt(4);
      ld(0, base), isObject(miss);
      E(ldrw(10, 9, 4)), E(ldrw(11, 4, lo + 4)), E(cmpw(10, 11)), miss.push_back(E(bcond(NE)));
      E(ldr(9, 4, lo + 8)), have = E(B);
      for (int m : miss) patch(m);
      if (lo) E(addi(4, 4, lo));
    } else imm(4, 0);
    addsp(0, base), imm(1, n), imm(2, sym);
    super ? gmem(3, global(F->cls).slot) : imm(3, 0);
    helper(H_PREP), E(mov(9, 0));
    if (have >= 0) patch(have);
    args(base, n + 1), E(blr(9)), F->depth = base, F->calls = true;
  }
  // C(args) for a top-level class C. Its inline cache holds the class value and its init's code (filled by
  // rt_prep, cleared on reload). On a hit the instance is allocated right here: bump the run of C's size
  // class (C keeps its header, run offset and cell size current), fill the slots with ABSENT, call init.
  void constructCall(int base, int n) {  // window[0] = the class
    Jumps miss;
    u32 lo = icAt(4);
    ld(0, base), E(ldr(9, 4, lo)), E(cmp(0, 9)), miss.push_back(E(bcond(NE)));
    E(andaddr(9, 0)), E(ldr(10, 9, CLS_IRUN));                   // w10: run offset; x10 >> 32: cell size
    E(addxw(11, 28, 10)), E(ldp_off(12, 13, 11, 0));             // x12, x13 = run cur, end
    E(addlsr(14, 12, 10, 32)), E(cmp(14, 13)), miss.push_back(E(bcond(HI)));
    E(str(14, 11, 0)), E(ldr(10, 9, CLS_IHDR)), E(addi(15, 12, 24));
    E(str(10, 12, 0)), E(stp_off(9, 15, 12, 8)), E(addi(16, 27, 4));  // header; cls, slots; x16 = ABSENT
    int fill = here();                                            // every slot, and any rounding slack after them
    E(strpost(16, 15, 8)), E(cmp(15, 14)), patch(E(bcond(LO)), fill);
    E(orrptr(0, 12)), st(0, base), E(ldr(9, 4, lo + 8));
    int have = E(B);
    for (int m : miss) patch(m);
    if (lo) E(addi(4, 4, lo));
    addsp(0, base), imm(1, n), imm(2, 0), imm(3, 0), helper(H_PREP), E(mov(9, 0));
    patch(have);
    args(base, n + 1), E(blr(9)), F->depth = base, F->calls = true;
  }
  void callDirect(int base, int n, u32* stub) {  // a known top-level fn: `bl` to its body (relinked on reload); last arg in x0
    if (n > 8) st(0, slot());
    else if (n > 1) E(mov(n - 1, 0));
    args(base, n > 8 ? n : n - 1), F->k.rel.push_back({here(), stub}), E(BL), F->depth = base, F->calls = true;
  }
  void closure(Fn* fn, int base, int n) {  // a closure of fn over the n values in slots base..
    int c = sizeClass(CLO_ENV + 8 * n), done = -1;
    if (c < NCLS && sizeClasses[c] < 4096) {  // allocate inline from the size class's run
      E(ldp_off(12, 13, 28, ROFF(c))), E(addi(14, 12, sizeClasses[c])), E(cmp(14, 13));
      int full = E(bcond(HI));
      E(str(14, 28, ROFF(c))), E(movz(10, (u32)K::Clo, 0)), E(str(10, 12, 0));
      imm(10, (V)fn), E(str(10, 12, 8)), imm(10, n), E(str(10, 12, 16));  // fn; n (and the padding after it)
      for (int i = 0; i < n; i++) ld(10, base + i), E(str(10, 12, CLO_ENV + 8 * i));
      E(orrptr(0, 12)), done = E(B), patch(full);
    }
    imm(0, box(fn)), imm(1, n), addsp(2, base), helper(H_CLOSURE), F->depth = base;
    if (done >= 0) patch(done);
  }
  // Each function counts its calls; at HOT, rt_hot sees which arguments are numbers and calls tierUp.
  void hotCounter(void* tier, int first, const std::vector<int>& homes) {
    u32 lo = icAt(16);
    E(ldr(17, 16, lo + 8)), E(addi(17, 17, 1)), E(str(17, 16, lo + 8)), E(cmpi(17, HOT));
    int j = E(bcond(NE));
    for (int i = first; i < (int)homes.size() && i - first < 7; i++) ldl(1 + i - first, homes[i]);
    imm(0, (V)tier), helper(H_HOT), patch(j);
  }
};
