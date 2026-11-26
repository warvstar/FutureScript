// WebAssembly encoding shared by the code generator (gen_wasm.h) and the module builder (wasm.cpp).
#pragma once
#include "fut.h"

namespace wa {
enum : uint8_t {
  UNREACHABLE = 0x00, NOP = 0x01, BLOCK = 0x02, LOOP = 0x03, IF = 0x04, ELSE = 0x05, ENDOP = 0x0b, BR = 0x0c, BR_IF = 0x0d,
  CALL = 0x10, CALL_INDIRECT = 0x11, RETURN_CALL_INDIRECT = 0x13, SELECT = 0x1b, TRY_TABLE = 0x1f, LOCAL_GET = 0x20, LOCAL_SET = 0x21, LOCAL_TEE = 0x22,
  GLOBAL_GET = 0x23, GLOBAL_SET = 0x24, I32_LOAD = 0x28, I32_LOAD8_U = 0x2d, I32_LOAD16_U = 0x2f, I64_LOAD = 0x29, F64_LOAD = 0x2b, I32_STORE = 0x36,
  I64_STORE = 0x37, F64_STORE = 0x39, I32_CONST = 0x41, I64_CONST = 0x42, F64_CONST = 0x44, I32_EQZ = 0x45,
  I32_EQ = 0x46, I32_NE = 0x47, I32_LT_U = 0x49, I32_GT_U = 0x4b, I32_GE_U = 0x4f, I64_EQ = 0x51, I64_GE_S = 0x59, I64_LT_U = 0x54, I64_NE = 0x52, F64_EQ = 0x61, F64_NE = 0x62, F64_LT = 0x63,
  F64_GT = 0x64, F64_LE = 0x65, F64_GE = 0x66, I32_ADD = 0x6a, I32_SUB = 0x6b, I32_AND = 0x71, I32_OR = 0x72, I32_XOR = 0x73, I32_SHL = 0x74, I32_SHR_U = 0x76,
  I64_ADD = 0x7c, I64_AND = 0x83, I64_OR = 0x84, I64_XOR = 0x85, I64_SHL = 0x86, I64_SHR_U = 0x88, F64_NEG = 0x9a, F64_ADD = 0xa0, I32_WRAP = 0xa7, I64_EXTEND_U = 0xad, I64_REINTERPRET = 0xbd, I32_TRUNC_F64_U = 0xab, F64_CONVERT_I32_U = 0xb8,
  F64_REINTERPRET = 0xbf, VOID = 0x40, I32 = 0x7f, I64 = 0x7e, F64 = 0x7c,
};
// Every function has the same locals: the frame, 18 i64 "registers", four i32 temporaries; then one i64 or f64
// per frame slot it uses as a local's home, caching that local.
// Parameters: the argument window, the caller's stack pointer (this frame goes below it), the first NA arguments
// (also in the window, where the collector and natives see them). Then the locals.
enum : u32 { W = 0, SP = 1, A0 = 2, NA = 4, FP = A0 + NA, R0, NREG = 18, T = R0 + NREG, T2, T3, T4, L0 };  // L0..: cached locals
// Module layout: type 0 is a fut function (window, sp, a0..a3) -> V; type 1 + h is helper h; type 1 + NH is a C++
// function (V* window) -> V that an adapter calls; then imports, see wasm.cpp.
inline const char* const FUT_SIG = "iillll:l";
// Helper signatures: parameters, ':' and the result (l: i64, i: i32 pointer; none: void).
inline const char* const HSIG[NH] = {
    ":", ":", "lll:l", "lli:l", "llli:l", "ll:l", "lll:l", "illli:i", "il:l", "ll:l", "l:l", ":", "il:l", "lli:l",
    "li:", "l:l", "ii:l", "l:l", "iii:", "l:", "lil:l", "ll:l", "lll:l", "l:", "li:l", "il:l"};
inline void uleb(std::vector<uint8_t>& o, uint64_t v) {
  do o.push_back(uint8_t((v & 0x7f) | (v > 0x7f ? 0x80 : 0))), v >>= 7;
  while (v);
}
inline void sleb(std::vector<uint8_t>& o, int64_t v) {
  for (bool more = true; more;) {
    uint8_t b = v & 0x7f;
    v >>= 7;
    more = !((v == 0 && !(b & 0x40)) || (v == -1 && (b & 0x40)));
    o.push_back(more ? b | 0x80 : b);
  }
}
inline uint8_t valType(char c) { return c == 'l' ? I64 : I32; }
inline void funcType(std::vector<uint8_t>& o, const char* sig) {  // "il:l" -> (i32 i64) -> i64
  const char* colon = strchr(sig, ':');
  o.push_back(0x60), uleb(o, colon - sig);
  for (const char* p = sig; p < colon; p++) o.push_back(valType(*p));
  uleb(o, colon[1] ? 1 : 0);
  if (colon[1]) o.push_back(valType(colon[1]));
}
// What a field in code (Code::rel) gets once its stub's target is known. REL_SLOT (5 bytes): the stub's table slot.
// REL_OWN (5 bytes): the address of this body's own "replaced" cell. REL_CALL (9 bytes): a call, direct if the body
// the stub leads to is in the same module (call f; three nops), else through the stub (i32.const stub; call_indirect).
enum RelKind : uint8_t { REL_SLOT, REL_OWN, REL_CALL };
inline u32 slotOf(const void* p) { return (u32)(uintptr_t)p; }  // a function pointer or stub: its table index
}
