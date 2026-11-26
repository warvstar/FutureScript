// An interpreter for the WebAssembly fut emits (wasm build, fut_init flag 4). A host that can't compile code (a
// Cloudflare Worker) runs fut.wasm, which it can, and this runs each compiled function's body instead of installing
// it as a module: one static Worker compiles and runs any fut source, with no deployment per program.
//  - Table entries: C++ calls a fut function through its table slot, so each interpreted body gets one of NTRAMP
//    static trampolines (wasm functions with the fut signature); jitCollect copies the trampoline into the body's
//    slot, and the trampoline runs body k. The interpreter calls everything (helpers, natives, other bodies) through
//    the table, as the compiled code does.
//  - Frames: the interpreter's C++ frames are on the shadow stack too, so the interpreted body's frame (SP - size, as
//    its prologue computes it) gets a region of its own in the interpreter's frame, and the body's global.set of the
//    stack pointer (before it calls C++) is ignored: the C++ it calls goes below the interpreter's frames anyway.
//    Nothing here raises the stack pointer, which could put a callee's frame over a live one.
//  - try_table: C++ here can't catch a wasm exception, so the body of a try_table runs inside try_call, a tiny static
//    module the host instantiates (fut.js tryCallModule) that catches the fut tag and hands back its payload.
//  - Suspension (JSPI): the interpreter is wasm, so fibers, sleep() and fetch() suspend through it as through
//    compiled code.
// Only the instructions gen_wasm.h and wasm.cpp emit are handled (plus the rest of the integer and f64 arithmetic);
// anything else stops with a message.
#include "wasm.h"
#include <alloca.h>
#include <cmath>
#include <map>
#include <memory>
#include <utility>

using namespace wa;
bool interpMode;
void* interpTop;  // the innermost interpreted frame (a Frame), for stack traces; each fiber keeps its own (wasm.cpp)
namespace {
using L = uint64_t;
using I = uint32_t;
void setsp(void* p) { asm volatile("local.get %0\n\tglobal.set __stack_pointer" : : "r"(p)); }
void* getsp() { void* p; asm volatile("global.get __stack_pointer\n\tlocal.set %0" : "=r"(p)); return p; }

struct IFn {
  std::vector<uint8_t> code;            // locals declaration, then the instructions
  std::shared_ptr<std::vector<IFn*>> mod;  // the batch's bodies, by function index (`call` runs the body itself)
  u32 start = 0, nlocals = 0, frame = 0;  // where the instructions start; parameters + locals; its frame's size
  std::vector<u32> side;                // by pc of block / loop / if / else / try_table: the pc after its end
  std::vector<u32> elseAt;              // by pc of if: the pc after its else (0: none)
  u32 slot = 0;                         // its table slot (line tables, fut_where)
  bool ready = false;
};
constexpr int NTRAMP = 4096;  // the whole test suite needs under 1024 at once (retired bodies give theirs back)
std::vector<IFn*> fns(NTRAMP);  // by trampoline. A retired body is kept: it may be running, or called directly by a
                                // body of its batch (which then goes on through the stub, as compiled code does)
std::vector<int> freeK;
std::map<u32, int> kOfSlot;  // body slot -> trampoline
u32 tryCallSlot;

[[noreturn]] void bad(const char* what, unsigned x) { fprintf(stderr, "fut interpreter: %s 0x%x\n", what, x), fflush(stderr), __builtin_trap(); }
inline u32 uleb(const uint8_t* c, u32& pc) {
  u32 v = 0;
  for (int s = 0;; s += 7) { uint8_t b = c[pc++]; v |= u32(b & 0x7f) << s; if (!(b & 0x80)) return v; }
}
inline int64_t sleb(const uint8_t* c, u32& pc) {
  int64_t v = 0;
  int s = 0;
  uint8_t b;
  do b = c[pc++], v |= int64_t(b & 0x7f) << s, s += 7; while (b & 0x80);
  if (s < 64 && (b & 0x40)) v |= -(int64_t(1) << s);
  return v;
}
void skipImm(const uint8_t* c, u32& pc, uint8_t op) {  // past op's immediates
  switch (op) {
    case BLOCK: case LOOP: case IF: pc++; break;
    case TRY_TABLE: {
      pc++;
      for (u32 n = uleb(c, pc); n--;) { uint8_t k = c[pc++]; if (k < 2) uleb(c, pc); uleb(c, pc); }
      break;
    }
    case BR: case BR_IF: case CALL: case LOCAL_GET: case LOCAL_SET: case LOCAL_TEE: case GLOBAL_GET: case GLOBAL_SET: uleb(c, pc); break;
    case CALL_INDIRECT: case RETURN_CALL_INDIRECT: uleb(c, pc), uleb(c, pc); break;
    case I32_CONST: case I64_CONST: sleb(c, pc); break;
    case F64_CONST: pc += 8; break;
    default:
      if (op >= 0x28 && op <= 0x3e) uleb(c, pc), uleb(c, pc);  // loads and stores: align, offset
  }
}
void prepare(IFn& f) {  // the locals' count, and where each block ends
  const uint8_t* c = f.code.data();
  u32 pc = 0;
  f.nlocals = A0 + NA;
  for (u32 runs = uleb(c, pc); runs--;) f.nlocals += uleb(c, pc), pc++;
  f.start = pc;
  f.side.assign(f.code.size(), 0), f.elseAt.assign(f.code.size(), 0);
  std::vector<u32> open;
  int64_t lastConst = 0;
  uint8_t prev = 0;
  bool framed = false;
  while (pc < f.code.size()) {
    u32 at = pc;
    uint8_t op = c[pc++];
    if (op == I32_CONST) { u32 q = pc; lastConst = sleb(c, q); }
    if ((op == LOCAL_SET || op == LOCAL_TEE) && !framed && c[pc] == FP) framed = true, f.frame = prev == I32_SUB ? (u32)lastConst : 0;  // FP = SP - size
    if (op != I32_CONST) prev = op;
    skipImm(c, pc, op);
    if (op == BLOCK || op == LOOP || op == IF || op == TRY_TABLE) {
      open.push_back(at);
      if (open.size() >= 90) bad("blocks nested too deeply in one function:", (unsigned)open.size());
    }
    else if (op == ELSE) f.elseAt[open.back()] = pc, open.push_back(at);  // the else's own entry: its end
    else if (op == ENDOP) {
      if (open.empty()) break;  // the function's end
      u32 b = open.back();
      open.pop_back();
      f.side[b] = pc;
      if (f.code[b] == ELSE) f.side[open.back()] = pc, open.pop_back();  // the if, too
    }
  }
  f.ready = true;
}

// ---- calls through the table, by the callee's type ----
using FutFn = L (*)(V*, void*, L, L, L, L);
template <class R, class... P, size_t... N> L invoke(u32 slot, const L* a, std::index_sequence<N...>) {
  auto f = (R(*)(P...))(uintptr_t)slot;
  if constexpr (std::is_void_v<R>) return f((P)a[N]...), 0;
  else return (L)f((P)a[N]...);
}
template <class R, class... P> L call(u32 slot, const L* a) { return invoke<R, P...>(slot, a, std::index_sequence_for<P...>{}); }
struct Sig { const char* s; L (*f)(u32, const L*); };
const Sig sigs[] = {
    {":", call<void>}, {"l:", call<void, L>}, {"li:", call<void, L, I>}, {"iii:", call<void, I, I, I>},
    {"i:l", call<L, I>}, {"l:l", call<L, L>}, {"ll:l", call<L, L, L>}, {"il:l", call<L, I, L>}, {"li:l", call<L, L, I>},
    {"ii:l", call<L, I, I>}, {"lll:l", call<L, L, L, L>}, {"lli:l", call<L, L, L, I>}, {"lil:l", call<L, L, I, L>},
    {"llli:l", call<L, L, L, L, I>}, {"illli:i", call<I, I, L, L, L, I>}, {"iillll:l", call<L, I, I, L, L, L, L>},
};
struct TypeCall { L (*f)(u32, const L*); int n; bool ret; };
TypeCall types[2 + NH];
void callTypesInit() {
  auto find = [](const char* s) {
    for (auto& g : sigs) if (!strcmp(g.s, s)) return TypeCall{g.f, int(strchr(s, ':') - s), strchr(s, ':')[1] != 0};
    bad("no call for the signature of type", (unsigned)s[0]);
  };
  types[0] = find(FUT_SIG);
  for (int h = 0; h < NH; h++) types[1 + h] = find(HSIG[h]);
  types[1 + NH] = find("i:l");
}

// ---- running a body ----
struct Ctl { u32 cont; int height; uint8_t arity; bool loop; };
constexpr int NSTK = 96, NCTL = 96;  // operand stack (shallow: gen_wasm keeps values in locals) and block nesting (prepare checks it)
struct Frame { IFn* f; L* loc; L stk[NSTK]; int s; Ctl ctl[NCTL]; int nctl; L ret; Frame* prev; u32 pc; };
struct TryCtx { L payload; Frame* fr; u32 pc; int base; };  // payload first: try_call stores it at the context
int run(Frame& fr, u32 pc, int base);
L enter(IFn& f, V* w, L a0, L a1, L a2, L a3);
int tryBody(TryCtx* t) { int r = run(*t->fr, t->pc, t->base); return r == -1 ? -3 : r; }  // -1 from try_call: caught
using TryCall = int (*)(u32 cb, TryCtx* ctx);

inline uint8_t arityOf(uint8_t bt) { return bt == VOID ? 0 : 1; }
template <class T> inline T ld(I a) { T v; memcpy(&v, (void*)(uintptr_t)a, sizeof v); return v; }
template <class T> inline void st(I a, T v) { memcpy((void*)(uintptr_t)a, &v, sizeof v); }
inline double fd(L x) { return std::bit_cast<double>(x); }
inline L df(double d) { return std::bit_cast<L>(d); }

// Runs from pc until the function returns (-1), execution leaves the control entry `base` (falls out of its end or
// branches to it: 0), or a branch goes past it (1 + how many entries further out).
int run(Frame& fr, u32 pc, int base) {
  IFn& f = *fr.f;
  const uint8_t* c = f.code.data();
  L* loc = fr.loc;
  L* stk = fr.stk;
  int s = fr.s;
#define POP() stk[--s]
#define PUSH(v) (stk[s++] = (L)(v))
#define TOP stk[s - 1]
  auto branch = [&](u32 n) -> int {  // to the n-th enclosing label: -2 to go on at pc, else run's result
    int t = fr.nctl - 1 - (int)n;
    if (t < base) return fr.s = s, 1 + (base - 1 - t);
    if (t < 0) return fr.ret = stk[s - 1], fr.s = s, -1;  // the function's own label: return
    Ctl& k = fr.ctl[t];
    if (k.loop) { s = k.height, fr.nctl = t + 1, pc = k.cont; return -2; }
    if (k.arity) stk[k.height] = stk[s - 1];
    s = k.height + k.arity, fr.nctl = t, pc = k.cont;
    if (t == base) return fr.s = s, 0;
    return -2;
  };
  for (;;) {
    uint8_t op = c[pc++];
    switch (op) {
      case UNREACHABLE: __builtin_trap();
      case NOP: break;
      case BLOCK: case LOOP: {
        u32 at = pc - 1;
        uint8_t bt = c[pc++];
        fr.ctl[fr.nctl++] = {op == LOOP ? at : f.side[at], s, op == LOOP ? (uint8_t)0 : arityOf(bt), op == LOOP};
        if (op == LOOP) fr.ctl[fr.nctl - 1].cont = pc;  // a branch to a loop starts its body again
        break;
      }
      case IF: {
        u32 at = pc - 1;
        uint8_t bt = c[pc++];
        I cond = (I)POP();
        fr.ctl[fr.nctl++] = {f.side[at], s, arityOf(bt), false};
        if (!cond) {
          if (f.elseAt[at]) pc = f.elseAt[at];
          else pc = f.side[at], fr.nctl--;
        }
        break;
      }
      case ELSE: {  // the then-arm ended: skip the else-arm
        pc = fr.ctl[--fr.nctl].cont;
        break;
      }
      case ENDOP:
        if (fr.nctl == 0) return fr.ret = TOP, fr.s = s, -1;  // the function's end: its result
        if (--fr.nctl == base) return fr.s = s, 0;
        break;
      case BR: { int r = branch(uleb(c, pc)); if (r != -2) return r; break; }
      case BR_IF: { u32 n = uleb(c, pc); if ((I)POP()) { int r = branch(n); if (r != -2) return r; } break; }
      case TRY_TABLE: {
        u32 at = pc - 1;
        uint8_t bt = c[pc++];
        u32 n = uleb(c, pc), label = 0;
        bool caught = false;
        for (u32 i = 0; i < n; i++) {
          uint8_t k = c[pc++];
          if (k < 2) uleb(c, pc);
          u32 l = uleb(c, pc);
          if (k == 0) caught = true, label = l;
          else bad("unsupported catch kind", k);
        }
        if (!caught) bad("try_table without a catch", 0);
        int height = s, here = fr.nctl;
        fr.ctl[fr.nctl++] = {f.side[at], s, arityOf(bt), false};
        fr.s = s;
        TryCtx t{0, &fr, pc, here};
        int r = ((TryCall)(uintptr_t)tryCallSlot)(wa::slotOf((void*)tryBody), &t);
        s = fr.s;
        if (r == -3) return -1;  // the function returned (fr.ret)
        fr.nctl = here, interpTop = &fr;  // (an exception unwound the frames above without popping them)
        if (r == -1) {  // caught: the payload goes to the catch's label (counted from outside the try_table)
          s = height, PUSH(t.payload);
          int b = branch(label);
          if (b != -2) return b;
          break;
        }
        if (r == 0) { pc = f.side[at]; break; }  // out through the end (or a branch to the try_table)
        int b = branch(r - 1);                   // further out, from outside the try_table
        if (b != -2) return b;
        break;
      }
      case 0x0f: return fr.ret = TOP, fr.s = s, -1;  // return
      case CALL: case CALL_INDIRECT: case RETURN_CALL_INDIRECT: {
        u32 type = 0, slot;
        fr.pc = pc - 1;  // where a stack trace sees this frame: the call
        if (op == CALL) {  // a body of this batch: (window, sp, a0..a3)
          IFn& g = *(*f.mod)[uleb(c, pc)];
          s -= A0 + NA;
          L r = enter(g, (V*)(uintptr_t)(I)stk[s + W], stk[s + A0], stk[s + A0 + 1], stk[s + A0 + 2], stk[s + A0 + 3]);
          PUSH(r);
          break;
        }
        type = uleb(c, pc), uleb(c, pc), slot = (u32)POP();
        if (type >= 2 + NH) bad("call of type", type);
        TypeCall& tc = types[type];
        s -= tc.n;
        L r = tc.f(slot, stk + s);
        if (tc.ret) PUSH(r);
        if (op == RETURN_CALL_INDIRECT) return fr.ret = r, fr.s = s, -1;
        break;
      }
      case 0x1a: s--; break;  // drop
      case SELECT: { I k = (I)POP(); L b = POP(), a = POP(); PUSH(k ? a : b); break; }
      case LOCAL_GET: PUSH(loc[uleb(c, pc)]); break;
      case LOCAL_SET: loc[uleb(c, pc)] = POP(); break;
      case LOCAL_TEE: loc[uleb(c, pc)] = TOP; break;
      case GLOBAL_GET: uleb(c, pc), PUSH((I)(uintptr_t)getsp()); break;  // global 0: the stack pointer
      case GLOBAL_SET: uleb(c, pc), s--; break;  // ignored (see the top)
      case I32_CONST: PUSH((I)(int32_t)sleb(c, pc)); break;
      case I64_CONST: PUSH(sleb(c, pc)); break;
      case F64_CONST: { L v; memcpy(&v, c + pc, 8), pc += 8, PUSH(v); break; }
      default:
        if (op >= 0x28 && op <= 0x3e) {  // memory
          uleb(c, pc);
          u32 off = uleb(c, pc);
          if (op >= 0x36) {
            L v = POP();
            I a = (I)POP() + off;
            switch (op) {
              case 0x36: st<I>(a, (I)v); break;
              case 0x37: case 0x39: st<L>(a, v); break;
              case 0x3a: case 0x3c: st<uint8_t>(a, (uint8_t)v); break;
              case 0x3b: case 0x3d: st<uint16_t>(a, (uint16_t)v); break;
              case 0x3e: st<I>(a, (I)v); break;
              default: bad("store", op);
            }
          } else {
            I a = (I)POP() + off;
            switch (op) {
              case 0x28: PUSH(ld<I>(a)); break;
              case 0x29: case 0x2b: PUSH(ld<L>(a)); break;
              case 0x2c: PUSH((I)(int32_t)ld<int8_t>(a)); break;
              case 0x2d: PUSH(ld<uint8_t>(a)); break;
              case 0x2e: PUSH((I)(int32_t)ld<int16_t>(a)); break;
              case 0x2f: PUSH(ld<uint16_t>(a)); break;
              case 0x30: PUSH((int64_t)ld<int8_t>(a)); break;
              case 0x31: PUSH(ld<uint8_t>(a)); break;
              case 0x32: PUSH((int64_t)ld<int16_t>(a)); break;
              case 0x33: PUSH(ld<uint16_t>(a)); break;
              case 0x34: PUSH((int64_t)ld<int32_t>(a)); break;
              case 0x35: PUSH(ld<I>(a)); break;
              default: bad("load", op);
            }
          }
          break;
        }
        if (op >= 0x45 && op <= 0x4f) {  // i32 comparisons
          if (op == I32_EQZ) { TOP = (I)TOP == 0; break; }
          I b = (I)POP(), a = (I)POP();
          int32_t x = (int32_t)a, y = (int32_t)b;
          bool r = op == 0x46 ? a == b : op == 0x47 ? a != b : op == 0x48 ? x < y : op == 0x49 ? a < b : op == 0x4a ? x > y
                 : op == 0x4b ? a > b : op == 0x4c ? x <= y : op == 0x4d ? a <= b : op == 0x4e ? x >= y : a >= b;
          PUSH(r);
          break;
        }
        if (op >= 0x50 && op <= 0x5a) {  // i64 comparisons
          if (op == 0x50) { TOP = TOP == 0; break; }
          L b = POP(), a = POP();
          int64_t x = (int64_t)a, y = (int64_t)b;
          bool r = op == 0x51 ? a == b : op == 0x52 ? a != b : op == 0x53 ? x < y : op == 0x54 ? a < b : op == 0x55 ? x > y
                 : op == 0x56 ? a > b : op == 0x57 ? x <= y : op == 0x58 ? a <= b : op == 0x59 ? x >= y : a >= b;
          PUSH(r);
          break;
        }
        if (op >= 0x61 && op <= 0x66) {  // f64 comparisons
          double b = fd(POP()), a = fd(POP());
          bool r = op == 0x61 ? a == b : op == 0x62 ? a != b : op == 0x63 ? a < b : op == 0x64 ? a > b : op == 0x65 ? a <= b : a >= b;
          PUSH(r);
          break;
        }
        if (op >= 0x67 && op <= 0x78) {  // i32 arithmetic
          if (op <= 0x69) { I a = (I)TOP; TOP = op == 0x67 ? (a ? __builtin_clz(a) : 32) : op == 0x68 ? (a ? __builtin_ctz(a) : 32) : __builtin_popcount(a); break; }
          I b = (I)POP(), a = (I)POP(), r;
          switch (op) {
            case 0x6a: r = a + b; break;
            case 0x6b: r = a - b; break;
            case 0x6c: r = a * b; break;
            case 0x6d: if (!b) __builtin_trap(); r = (I)((int32_t)a / (int32_t)b); break;
            case 0x6e: if (!b) __builtin_trap(); r = a / b; break;
            case 0x6f: if (!b) __builtin_trap(); r = b == (I)-1 ? 0 : (I)((int32_t)a % (int32_t)b); break;
            case 0x70: if (!b) __builtin_trap(); r = a % b; break;
            case 0x71: r = a & b; break;
            case 0x72: r = a | b; break;
            case 0x73: r = a ^ b; break;
            case 0x74: r = a << (b & 31); break;
            case 0x75: r = (I)((int32_t)a >> (b & 31)); break;
            case 0x76: r = a >> (b & 31); break;
            case 0x77: r = (a << (b & 31)) | (a >> ((32 - (b & 31)) & 31)); break;
            default: r = (a >> (b & 31)) | (a << ((32 - (b & 31)) & 31)); break;
          }
          PUSH(r);
          break;
        }
        if (op >= 0x79 && op <= 0x8a) {  // i64 arithmetic
          if (op <= 0x7b) { L a = TOP; TOP = op == 0x79 ? (a ? __builtin_clzll(a) : 64) : op == 0x7a ? (a ? __builtin_ctzll(a) : 64) : __builtin_popcountll(a); break; }
          L b = POP(), a = POP(), r;
          switch (op) {
            case 0x7c: r = a + b; break;
            case 0x7d: r = a - b; break;
            case 0x7e: r = a * b; break;
            case 0x7f: if (!b) __builtin_trap(); r = (L)((int64_t)a / (int64_t)b); break;
            case 0x80: if (!b) __builtin_trap(); r = a / b; break;
            case 0x81: if (!b) __builtin_trap(); r = b == (L)-1 ? 0 : (L)((int64_t)a % (int64_t)b); break;
            case 0x82: if (!b) __builtin_trap(); r = a % b; break;
            case 0x83: r = a & b; break;
            case 0x84: r = a | b; break;
            case 0x85: r = a ^ b; break;
            case 0x86: r = a << (b & 63); break;
            case 0x87: r = (L)((int64_t)a >> (b & 63)); break;
            case 0x88: r = a >> (b & 63); break;
            case 0x89: r = (a << (b & 63)) | (a >> ((64 - (b & 63)) & 63)); break;
            default: r = (a >> (b & 63)) | (a << ((64 - (b & 63)) & 63)); break;
          }
          PUSH(r);
          break;
        }
        if (op >= 0x99 && op <= 0x9f) {  // f64 unary
          double a = fd(TOP);
          TOP = df(op == 0x99 ? std::fabs(a) : op == 0x9a ? -a : op == 0x9b ? std::ceil(a) : op == 0x9c ? std::floor(a)
                   : op == 0x9d ? std::trunc(a) : op == 0x9e ? std::nearbyint(a) : std::sqrt(a));
          break;
        }
        if (op >= 0xa0 && op <= 0xa6) {  // f64 binary
          double b = fd(POP()), a = fd(POP());
          PUSH(df(op == 0xa0 ? a + b : op == 0xa1 ? a - b : op == 0xa2 ? a * b : op == 0xa3 ? a / b
                  : op == 0xa4 ? std::fmin(a, b) : op == 0xa5 ? std::fmax(a, b) : std::copysign(a, b)));
          break;
        }
        switch (op) {  // conversions
          case 0xa7: TOP = (I)TOP; break;                                  // i32.wrap_i64
          case 0xaa: TOP = (I)(int32_t)fd(TOP); break;                     // i32.trunc_f64_s
          case 0xab: TOP = (I)fd(TOP); break;                              // i32.trunc_f64_u
          case 0xac: TOP = (L)(int64_t)(int32_t)(I)TOP; break;             // i64.extend_i32_s
          case 0xad: TOP = (L)(I)TOP; break;                               // i64.extend_i32_u
          case 0xb0: TOP = (L)(int64_t)fd(TOP); break;                     // i64.trunc_f64_s
          case 0xb1: TOP = (L)fd(TOP); break;                              // i64.trunc_f64_u
          case 0xb7: TOP = df((double)(int32_t)(I)TOP); break;             // f64.convert_i32_s
          case 0xb8: TOP = df((double)(I)TOP); break;                      // f64.convert_i32_u
          case 0xb9: TOP = df((double)(int64_t)TOP); break;                // f64.convert_i64_s
          case 0xba: TOP = df((double)TOP); break;                         // f64.convert_i64_u
          case 0xbd: case 0xbf: break;                                     // reinterpret: the same bits
          default: bad("instruction", op);
        }
    }
  }
#undef POP
#undef PUSH
#undef TOP
}

__attribute__((noinline)) L enter(IFn& f, V* w, L a0, L a1, L a2, L a3) {
  if (!f.ready) prepare(f);
  L* loc = (L*)alloca(f.nlocals * sizeof(L));
  uintptr_t region = (uintptr_t)alloca(f.frame + 16);  // the body's frame: [SP - frame, SP)
  memset(loc, 0, f.nlocals * sizeof(L));
  Frame fr;
  fr.f = &f, fr.loc = loc, fr.s = 0, fr.nctl = 0, fr.ret = 0, fr.prev = (Frame*)interpTop, fr.pc = f.start;
  interpTop = &fr;
  loc[W] = (I)(uintptr_t)w, loc[A0] = a0, loc[A0 + 1] = a1, loc[A0 + 2] = a2, loc[A0 + 3] = a3;
  loc[SP] = (I)((region + f.frame + 15) & ~(uintptr_t)15);
  run(fr, f.start, -1);
  interpTop = fr.prev;
  return fr.ret;
}
__attribute__((noinline)) L entry(int k, V* w, void* sp, L a0, L a1, L a2, L a3) {
  if (sp < getsp()) setsp(sp);  // (lower only: an interpreted caller passes its frame, which is above us)
  return enter(*fns[k], w, a0, a1, a2, a3);
}
template <int K> L tramp(V* w, void* sp, L a0, L a1, L a2, L a3) { return entry(K, w, sp, a0, a1, a2, a3); }  // small: there are NTRAMP
template <size_t... K> std::vector<u32> trampSlots(std::index_sequence<K...>) { return {wa::slotOf((void*)&tramp<(int)K>)...}; }
std::vector<u32> tramps;
}  // namespace

void interpInit() {
  callTypesInit(), tramps = trampSlots(std::make_index_sequence<NTRAMP>{});
  for (int k = NTRAMP; k-- > 0;) freeK.push_back(k);
}
void interpSetTryCall(u32 slot) { tryCallSlot = slot; }
// A batch's bodies (locals declaration + code, relocated), in function-index order: each gets a trampoline, whose
// table entry ops copies into its slot.
void interpAdd(const std::vector<u32>& slots, std::vector<std::vector<uint8_t>>& bodies, std::vector<int32_t>& ops) {
  auto mod = std::make_shared<std::vector<IFn*>>();
  for (size_t i = 0; i < slots.size(); i++) {
    if (freeK.empty()) ERR("the interpreter ran out of function slots (%d)", NTRAMP);
    int k = freeK.back();
    freeK.pop_back();
    IFn* f = new IFn;
    f->code = std::move(bodies[i]), f->mod = mod, f->slot = slots[i], mod->push_back(f);
    fns[k] = f, kOfSlot[slots[i]] = k;
    ops.push_back((int32_t)slots[i]), ops.push_back((int32_t)tramps[k]);
  }
}
void interpRetire(u32 slot) {
  auto it = kOfSlot.find(slot);
  if (it == kOfSlot.end()) return;
  freeK.push_back(it->second), kOfSlot.erase(it);  // (the body itself stays: see fns)
}
// The interpreted frames, innermost first, as fut_where describes them ("name (file:line)"), one per line.
std::string interpTrace_(const char* (*where)(int slot, int off)) {
  std::string t;
  int n = 0;
  for (Frame* f = (Frame*)interpTop; f && n < 64; f = f->prev, n++)
    if (const char* d = where((int)f->f->slot, (int)f->pc); *d) t += std::string("  at ") + d + "\n";
  return t;
}
