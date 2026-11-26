// WebAssembly encoding shared by the code generator (gen_wasm.h) and the module builder (wasm.cpp).
#pragma once
#include "fut.h"

namespace wa {
enum : uint8_t {
  UNREACHABLE = 0x00, BLOCK = 0x02, LOOP = 0x03, IF = 0x04, ELSE = 0x05, ENDOP = 0x0b, BR = 0x0c, BR_IF = 0x0d,
  CALL_INDIRECT = 0x11, RETURN_CALL_INDIRECT = 0x13, SELECT = 0x1b, TRY_TABLE = 0x1f, LOCAL_GET = 0x20, LOCAL_SET = 0x21, LOCAL_TEE = 0x22,
  GLOBAL_GET = 0x23, GLOBAL_SET = 0x24, I32_LOAD = 0x28, I64_LOAD = 0x29, F64_LOAD = 0x2b, I32_STORE = 0x36,
  I64_STORE = 0x37, F64_STORE = 0x39, I32_CONST = 0x41, I64_CONST = 0x42, F64_CONST = 0x44, I32_EQZ = 0x45,
  I32_EQ = 0x46, I32_LT_U = 0x49, I64_EQ = 0x51, I64_GE_S = 0x59, I64_NE = 0x52, F64_EQ = 0x61, F64_NE = 0x62, F64_LT = 0x63,
  F64_GT = 0x64, F64_LE = 0x65, F64_GE = 0x66, I32_ADD = 0x6a, I32_SUB = 0x6b, I32_AND = 0x71, I32_SHL = 0x74,
  I64_ADD = 0x7c, I64_AND = 0x83, F64_ADD = 0xa0, I32_WRAP = 0xa7, I64_EXTEND_U = 0xad, I64_REINTERPRET = 0xbd,
  F64_REINTERPRET = 0xbf, VOID = 0x40, I32 = 0x7f, I64 = 0x7e, F64 = 0x7c,
};
// Every function has the same locals: the argument window, the frame, 18 i64 "registers", two i32 temporaries;
// then one i64 per frame slot it uses as a local's home, caching that local.
enum : u32 { W = 0, FP = 1, R0 = 2, NREG = 18, T = R0 + NREG, T2 = T + 1, L0 = T2 + 1 };  // L0..: locals' cached values
// Module layout: type 0 is a fut function (V* window) -> V; type 1 + h is helper h; then imports, see wasm.cpp.
// Helper signatures: parameters, ':' and the result (l: i64, i: i32 pointer; none: void).
inline const char* const HSIG[NH] = {
    ":", ":", "lll:l", "lli:l", "llli:l", "ll:l", "lll:l", "illli:i", "il:l", "ll:l", "l:l", ":", "il:l", "lli:l",
    "li:", "l:l", "ii:l", "l:l", "iii:", "l:"};
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
inline u32 slotOf(const void* p) { return (u32)(uintptr_t)p; }  // a function pointer or stub: its table index
}  // namespace wa
