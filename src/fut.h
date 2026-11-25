// fut: a tiny class-based language that JIT-compiles straight to arm64 and hot-reloads by patching machine code.
#pragma once
#include <bit>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <map>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

using V = uint64_t;
using u32 = uint32_t;

// ---- NaN-boxed values: doubles are themselves; everything else lives in the quiet-NaN space ----
constexpr V QNAN = 0x7ffc000000000000, SIGN = 1ull << 63, PTR = QNAN | SIGN, ADDR = 0xffffffffffff;
constexpr V NIL = QNAN, FALSE_ = QNAN | 1, TRUE_ = QNAN | 2, END = QNAN | 3, ABSENT = QNAN | 4;  // END/ABSENT: internal
inline bool isNum(V v) { return (v & QNAN) != QNAN; }
inline bool isObj(V v) { return (v & PTR) == PTR; }
inline bool truthy(V v) { return (v & ~1ull) != QNAN; }  // nil and false are the only falsy values
inline double num(V v) { return std::bit_cast<double>(v); }
inline V box(double d) { return std::bit_cast<V>(d); }
inline V boxb(bool b) { return b ? TRUE_ : FALSE_; }

// ---- heap objects (no vtables: JIT code reads Inst fields at fixed offsets) ----
enum class K : uint8_t { Str, List, Fn, Class, Inst, Map, Clo, Free = 0xff };
struct Obj { K kind; bool mark; uint16_t pad; u32 aux; };
struct Str : Obj {
  static constexpr K KIND = K::Str;
  u32 len, hash;
  char* s() { return (char*)(this + 1); }  // bytes stored inline after the header
  std::string_view sv() { return {s(), len}; }
};
struct List : Obj { static constexpr K KIND = K::List; std::vector<V> v; };
struct Fn : Obj { static constexpr K KIND = K::Fn; void* code; int arity, name; };
struct Class : Obj {
  static constexpr K KIND = K::Class;
  int64_t name;
  Class* super;
  std::unordered_map<int, Fn*> methods;
  std::unordered_map<int, u32> layout;  // field sym -> slot index, shared by all instances
  Fn* find(int s) {
    for (Class* c = this; c; c = c->super)
      if (auto i = c->methods.find(s); i != c->methods.end()) return i->second;
    return nullptr;
  }
};
struct Inst : Obj { static constexpr K KIND = K::Inst; Class* cls; V* slots; u32 n; };
struct KeyHash { size_t operator()(V) const; };
struct KeyEq { bool operator()(V, V) const; };
struct Map : Obj { static constexpr K KIND = K::Map; std::vector<std::pair<V, V>> e; std::unordered_map<V, u32, KeyHash, KeyEq> idx; };
struct Clo : Obj { static constexpr K KIND = K::Clo; Fn* fn; u32 n; V* env() { return (V*)(this + 1); } };
constexpr int INST_CLS = 8, INST_SLOTS = 16, INST_N = 24, CLO_ENV = 24;

inline Obj* obj(V v) { return (Obj*)(v & ADDR); }
inline V box(Obj* o) { return (V)o | PTR; }
inline bool is(V v, K k) { return isObj(v) && obj(v)->kind == k; }
template <class T> T* as(V v) { return (T*)obj(v); }
void* gcAlloc(size_t bytes);
template <class T> T* make(size_t extra = 0) {
  T* o = new (gcAlloc(sizeof(T) + extra)) T();
  o->kind = T::KIND;
  return o;
}
Str* newStr(std::string_view);
void gcInit();
extern uintptr_t* stackTop;

// ---- symbols: keywords are interned first so `sym < NKW` means keyword ----
#define KEYWORDS(X) X(var) X(fn) X(class) X(if) X(else) X(while) X(for) X(in) X(return) X(break) X(continue) \
  X(true) X(false) X(nil) X(self) X(super) X(import)
#define SYM_ENUM(k) K_##k,
#define SYM_STR(k) #k,
#define EXTRA_SYMS(X) X(init) X(main) X(push) X(pop) X(len) X(has) X(remove) X(keys) X(values) X(reload)
#define SYM_S(k) S_##k,
enum { KEYWORDS(SYM_ENUM) NKW, S_FIRST = NKW - 1, EXTRA_SYMS(SYM_S) };
int intern(std::string_view);
const std::string& symName(int);

// ---- the VM block pinned in x28; JIT code reaches helpers and globals with one ldr ----
enum Helper { H_POLL, H_STACK, H_BINOP, H_GETF, H_SETF, H_GETI, H_SETI, H_PREP, H_LIST, H_ITER, H_NUMCHK, H_UNDEF,
              H_MAP, H_CLOSURE, H_STALE, H_ECHO, NH };
enum Op { O_ADD, O_SUB, O_MUL, O_DIV, O_MOD, O_EQ, O_NE, O_LT, O_LE, O_GT, O_GE };
constexpr int MAXG = 1 << 16;
struct VM { u32 poll, pad; void* limit; void* helpers[NH]; V globals[MAXG]; };
extern VM vm;
constexpr int HOFF(int h) { return 16 + 8 * h; }
constexpr int GOFF(int g) { return 16 + 8 * NH + 8 * g; }
struct IC { Class* cls; uint64_t idx; void* code; };  // per-site inline cache (fields use idx, methods use code)
IC* newIC();
void clearICs();

struct Global { int slot = 0; char kind = 'v'; u32* stub = nullptr; Obj* o = nullptr; bool inited = false; };  // kind: v f c n(ative)
extern std::unordered_map<int, Global> globals;
extern std::map<std::pair<int, int>, Fn*> methodFns, builtinMethods;  // (class sym | kind, method sym) -> Fn
extern std::vector<V> consts;
Global& global(int sym);

// ---- JIT ----
struct Code { std::vector<u32> c; std::vector<std::pair<int, void*>> rel; std::vector<std::pair<int, int>> lines; std::string name, file; };
u32* jitInstall(Code&);
struct JitBatch { JitBatch(); ~JitBatch(); };  // hold code writable across many writes; one icache flush at the end
void jitRetire(u32* body);     // body was replaced; free it once no frame returns into it
void jitCollect();
size_t jitLive();  // bytes of machine code currently allocated
void jitShiftLines(u32* body, int delta);
u32* newStub();
void patchStub(u32* stub, void* target);
u32* staleThunk(int sym);
V vmCall(void* code, std::initializer_list<V> args = {});
extern std::string* asmListing;

struct Compiled { void* chunk = nullptr; bool mainChanged = false; std::string log; std::vector<std::string> files; };
struct CompileError : std::runtime_error {  // atEnd: the source ended too early (the REPL reads more)
  bool atEnd;
  CompileError(const std::string& m, bool end) : std::runtime_error(m), atEnd(end) {}
};
Compiled compile(const std::string& path, bool reload, bool echo, const std::string* text = nullptr);  // throws
bool readFile(const std::string& path, std::string& out);
void rtInit();
std::string toStr(V, bool quote = false);
extern void (*pollHook)();
extern jmp_buf* errJmp;
[[noreturn]] void rtError(void* ra, const char* fmt, ...);
#define ERR(...) rtError(__builtin_return_address(0), __VA_ARGS__)

// ---- arm64 encoders (checked against the system assembler) ----
namespace a64 {
constexpr u32 SP = 31, EQ = 0, NE = 1, HS = 2, MI = 4, PL = 5, LS = 9, GE = 10, GT = 12;
constexpr u32 ldr(u32 t, u32 n, u32 off) { return 0xf9400000 | (off / 8) << 10 | n << 5 | t; }
constexpr u32 str(u32 t, u32 n, u32 off) { return 0xf9000000 | (off / 8) << 10 | n << 5 | t; }
constexpr u32 ldrw(u32 t, u32 n, u32 off) { return 0xb9400000 | (off / 4) << 10 | n << 5 | t; }
constexpr u32 ldrb(u32 t, u32 n) { return 0x39400000 | n << 5 | t; }
constexpr u32 ldrreg(u32 t, u32 n, u32 m) { return 0xf8607800 | m << 16 | n << 5 | t; }  // ldr t, [n, m, lsl #3]
constexpr u32 strreg(u32 t, u32 n, u32 m) { return 0xf8207800 | m << 16 | n << 5 | t; }
constexpr u32 stp_off(u32 a, u32 b, u32 n, int off) { return 0xa9000000 | u32((off / 8) & 0x7f) << 15 | b << 10 | n << 5 | a; }
constexpr u32 ldp_off(u32 a, u32 b, u32 n, int off) { return 0xa9400000 | u32((off / 8) & 0x7f) << 15 | b << 10 | n << 5 | a; }
constexpr u32 mov(u32 d, u32 m) { return 0xaa0003e0 | m << 16 | d; }
constexpr u32 addi(u32 d, u32 n, u32 imm) { return 0x91000000 | imm << 10 | n << 5 | d; }
constexpr u32 addhi(u32 d, u32 n, u32 imm) { return 0x91400000 | imm << 10 | n << 5 | d; }  // add d, n, #imm, lsl #12
constexpr u32 subi(u32 d, u32 n, u32 imm) { return 0xd1000000 | imm << 10 | n << 5 | d; }
constexpr u32 subhi(u32 d, u32 n, u32 imm) { return 0xd1400000 | imm << 10 | n << 5 | d; }
constexpr u32 movz(u32 d, u32 imm, u32 hw) { return 0xd2800000 | hw << 21 | imm << 5 | d; }
constexpr u32 movk(u32 d, u32 imm, u32 hw) { return 0xf2800000 | hw << 21 | imm << 5 | d; }
constexpr u32 stp_pre(u32 a, u32 b) { return 0xa9bf0000 | b << 10 | SP << 5 | a; }   // stp a, b, [sp, #-16]!
constexpr u32 ldp_post(u32 a, u32 b) { return 0xa8c10000 | b << 10 | SP << 5 | a; }  // ldp a, b, [sp], #16
constexpr u32 blr(u32 n) { return 0xd63f0000 | n << 5; }
constexpr u32 RET = 0xd65f03c0, B = 0x14000000, BL = 0x94000000, BRK = 0xd4200000;
constexpr u32 bcond(u32 c) { return 0x54000000 | c; }
constexpr u32 cbzw(u32 t) { return 0x34000000 | t; }
constexpr u32 cmp(u32 n, u32 m) { return 0xeb00001f | m << 16 | n << 5; }
constexpr u32 cmpw(u32 n, u32 m) { return 0x6b00001f | m << 16 | n << 5; }
constexpr u32 cmpwi(u32 n, u32 imm) { return 0x7100001f | imm << 10 | n << 5; }
constexpr u32 cmpsp(u32 m) { return 0xeb2063ff | m << 16; }
constexpr u32 andnot1(u32 d, u32 n) { return 0x927ff800 | n << 5 | d; }  // and d, n, #~1
constexpr u32 andaddr(u32 d, u32 n) { return 0x9240bc00 | n << 5 | d; } // and d, n, #0xffffffffffff
constexpr u32 mvn(u32 d, u32 m) { return 0xaa2003e0 | m << 16 | d; }
constexpr u32 tstq(u32 n) { return 0xf24e301f | n << 5; }    // tst n, #QNAN
constexpr u32 tstptr(u32 n) { return 0xf24e341f | n << 5; }  // tst n, #PTR
constexpr u32 cinc(u32 d, u32 n, u32 c) { return 0x9a800400 | n << 16 | (c ^ 1) << 12 | n << 5 | d; }
constexpr u32 fmovdx(u32 d, u32 n) { return 0x9e670000 | n << 5 | d; }
constexpr u32 fmovxd(u32 d, u32 n) { return 0x9e660000 | n << 5 | d; }
constexpr u32 fcmp(u32 n, u32 m) { return 0x1e602000 | m << 16 | n << 5; }
constexpr u32 FADD = 0x1e612800, FSUB = 0x1e613800, FMUL = 0x1e610800, FDIV = 0x1e611800;  // d0 = d0 op d1
constexpr u32 FMOV1 = 0x1e6e1001;  // fmov d1, #1.0
}  // namespace a64
