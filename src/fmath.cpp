// Math that gives the same bits everywhere: sin, cos, exp, log, pow, ... written here rather than taken from C's
// libm (natively) or JavaScript's Math (wasm), whose results differ in the last bit between platforms. This file is
// compiled the same way for arm64 and wasm, with no fused multiply-add (the pragma; build.py also passes
// -ffp-contract=off everywhere), and uses only operations IEEE 754 defines exactly (+ - * / sqrt floor), so a
// program computes the same numbers on every machine. Results are within about 1 ulp of the exact value, and exact
// where that is representable (pow(10, 3), log2(8), cbrt(27)).
//
// The kernels are fdlibm's (sin, cos, exp, atan, asin, acos), with its polynomial coefficients; log is a table and a
// double-double atanh series, which pow, log2 and log10 build on so their results round correctly in practice.
// The constants that split pi/2, ln 2 and 2/pi exactly were generated with exact rational arithmetic.
#pragma clang fp contract(off)
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#ifndef FMATH_ONLY
#include "fut.h"
#endif

namespace {
const double PIO2_1 = 0x1.921fb54400000p+0, PIO2_2 = 0x1.0b4611a600000p-34, PIO2_3 = 0x1.3198a2e037073p-69;  // 33 + 33 + 53 bits of pi/2
const double INVPIO2 = 0x1.45f306dc9c883p-1;
const double PIO2_HI = 0x1.921fb54442d18p+0, PIO2_LO = 0x1.1a62633145c07p-54;
const double PI_HI = 0x1.921fb54442d18p+1, PI_LO = 0x1.1a62633145c07p-53;
const double LN2_HI32 = 0x1.62e42fee00000p-1, LN2_LO32 = 0x1.a39ef35793c76p-33;  // ln 2 in 32 bits + the rest
const double LN2_H = 0x1.62e42fefa39efp-1, LN2_L = 0x1.abc9e3b39803fp-56;
const double INVLN2_H = 0x1.71547652b82fep+0, INVLN2_L = 0x1.777d0ffda0d24p-56;
const double INVLN10_H = 0x1.bcb7b1526e50ep-2, INVLN10_L = 0x1.95355baaafad3p-57;
const double ATANHI[] = {0x1.dac670561bb4fp-2, 0x1.921fb54442d18p-1, 0x1.f730bd281f69bp-1, 0x1.921fb54442d18p+0};
const double ATANLO[] = {0x1.a2b7f222f65e2p-56, 0x1.1a62633145c07p-55, 0x1.007887af0cbbdp-56, 0x1.1a62633145c07p-54};
// 2/pi, 1280 bits after the binary point (for reducing large arguments exactly)
const uint64_t TWO_OVER_PI[] = {
    0xa2f9836e4e441529ull, 0xfc2757d1f534ddc0ull, 0xdb6295993c439041ull, 0xfe5163abdebbc561ull, 0xb7246e3a424dd2e0ull,
    0x06492eea09d1921cull, 0xfe1deb1cb129a73eull, 0xe88235f52ebb4484ull, 0xe99c7026b45f7e41ull, 0x3991d639835339f4ull,
    0x9c845f8bbdf9283bull, 0x1ff897ffde05980full, 0xef2f118b5a0a6d1full, 0x6d367ecf27cb09b7ull, 0x4f463f669e5fea2dull,
    0x7527bac7ebe5f17bull, 0x3d0739f78a5292eaull, 0x6bfb5fb11f8d5d08ull, 0x56033046fc7b6babull, 0xf0cfbc209af4361dull};
const double INF = __builtin_inf();

uint64_t bits(double x) { uint64_t u; memcpy(&u, &x, 8); return u; }
double fromBits(uint64_t u) { double x; memcpy(&x, &u, 8); return x; }
double nan() { return fromBits(0x7ff8000000000000ull); }
double scalbnd(double y, int n) {  // y * 2^n, rounded once (musl's scalbn)
  if (n > 1023) {
    y *= 0x1p1023, n -= 1023;
    if (n > 1023) y *= 0x1p1023, n = std::min(n - 1023, 1023);
  } else if (n < -1022) {
    y *= 0x1p-1022 * 0x1p53, n += 1022 - 53;
    if (n < -1022) y *= 0x1p-1022 * 0x1p53, n = std::max(n + 1022 - 53, -1022);
  }
  return y * fromBits(uint64_t(0x3ff + n) << 52);
}
// Double-double arithmetic: a value as an unevaluated sum hi + lo, exact steps without fused multiply-add.
void twoSum(double a, double b, double& s, double& e) {
  s = a + b;
  double bb = s - a;
  e = (a - (s - bb)) + (b - bb);
}
void split(double a, double& hi, double& lo) {  // Veltkamp: a = hi + lo, each 26 bits (|a| < 2^995)
  double c = 134217729.0 * a;
  hi = c - (c - a), lo = a - hi;
}
void twoProd(double a, double b, double& p, double& e) {  // Dekker: a * b = p + e exactly
  p = a * b;
  double ah, al, bh, bl;
  split(a, ah, al), split(b, bh, bl);
  e = ((ah * bh - p) + ah * bl + al * bh) + al * bl;
}

// ---- sin, cos, tan: reduce by multiples of pi/2, then fdlibm's kernels on [-pi/4, pi/4] ----
// sin(x + y) as hi + lo, before its last rounding (tan divides the unrounded values)
void ksinDD(double x, double y, bool tail, double& hi, double& lo) {
  const double S1 = -1.66666666666666324348e-01, S2 = 8.33333333332248946124e-03, S3 = -1.98412698298579493134e-04,
               S4 = 2.75573137070700676789e-06, S5 = -2.50507602534068634195e-08, S6 = 1.58969099521155010221e-10;
  double z = x * x, w = z * z, r = S2 + z * (S3 + z * S4) + z * w * (S5 + z * S6), v = z * x;
  hi = x, lo = tail ? -((z * (0.5 * y - v * r) - y) - v * S1) : v * (S1 + z * r);
}
void kcosDD(double x, double y, double& hi, double& lo) {  // cos(x + y)
  const double C1 = 4.16666666666666019037e-02, C2 = -1.38888888888741095749e-03, C3 = 2.48015872894767294178e-05,
               C4 = -2.75573143513906633035e-07, C5 = 2.08757232129817482790e-09, C6 = -1.13596475577881948265e-11;
  double z = x * x, w = z * z, r = z * (C1 + z * (C2 + z * C3)) + w * w * (C4 + z * (C5 + z * C6));
  double hz = 0.5 * z;
  hi = 1.0 - hz, lo = ((1.0 - hi) - hz) + (z * r - x * y);
}
double ksin(double x, double y, bool tail) { double h, l; return ksinDD(x, y, tail, h, l), h + l; }
double kcos(double x, double y) { double h, l; return kcosDD(x, y, h, l), h + l; }
double ktan(double x, double y, bool odd) {  // tan(x + y), or -1/tan(x + y) when odd: (sh + sl) / (ch + cl)
  double sh, sl, ch, cl, p, pe;
  ksinDD(x, y, y != 0, sh, sl), kcosDD(x, y, ch, cl);
  if (odd) std::swap(sh, ch), std::swap(sl, cl), sh = -sh, sl = -sl;
  double t = (sh + sl) / (ch + cl);
  twoProd(t, ch, p, pe);
  return t + (((sh - p) - pe) + sl - t * cl) / ch;
}
uint64_t chunk(int s) {  // bits s .. s+63 of 2/pi (bit 1 is the first after the point; bits <= 0 are 0)
  if (s <= -63) return 0;
  if (s <= 0) return chunk(1) >> (1 - s);
  int i = s - 1, w = i / 64, b = i % 64;
  return b ? TWO_OVER_PI[w] << b | TWO_OVER_PI[w + 1] >> (64 - b) : TWO_OVER_PI[w];
}
void mul64(uint64_t a, uint64_t b, uint64_t& hi, uint64_t& lo) {  // a * b = hi * 2^64 + lo
  uint64_t a0 = (uint32_t)a, a1 = a >> 32, b0 = (uint32_t)b, b1 = b >> 32;
  uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
  uint64_t mid = (p00 >> 32) + (uint32_t)p01 + (uint32_t)p10;
  lo = (mid << 32) | (uint32_t)p00, hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
}
// |x| = (n + f) pi/2 exactly enough, for any finite x: x = m 2^e, and only the bits of 2/pi from 2^(1-e) on matter
// mod 4. m times 192 of them gives n mod 4 and 128 bits of f, ample for the closest a double comes to a multiple.
int remPio2Large(double x, double& y0, double& y1) {
  uint64_t u = bits(std::fabs(x)), m = (u & 0xfffffffffffffull) | (1ull << 52);
  int e = int(u >> 52) - 1075, i0 = e - 1;
  uint64_t w0 = chunk(i0), w1 = chunk(i0 + 64), w2 = chunk(i0 + 128), h0, l0, h1, l1, h2, l2;
  mul64(m, w0, h0, l0), mul64(m, w1, h1, l1), mul64(m, w2, h2, l2);
  (void)h0;  // (bits 192 and up: multiples of 4)
  uint64_t p0 = l2, p1 = h2 + l1, p2 = h1 + l0 + (p1 < h2);
  int q = int(p2 >> 62);  // bits 190, 191: the quadrant
  uint64_t fh = p2 << 2 | p1 >> 62, fl = p1 << 2 | p0 >> 62;  // the fraction's first 128 bits
  double sign = 1;
  if (fh >> 63) fh = ~fh, fl = ~fl + 1, fh += fl == 0, q++, sign = -1;  // f >= 1/2: f - 1, the next quadrant
  int lz = 0;
  if (!fh) fh = fl, fl = 0, lz = 64;
  if (!fh) return y0 = y1 = 0, x < 0 ? -q : q;
  int z = __builtin_clzll(fh);
  if (z) fh = fh << z | fl >> (64 - z), fl <<= z;
  lz += z;
  double hi = scalbnd((double)(fh >> 11), 11 - 64 - lz), lo = scalbnd((double)((fh & 0x7ff) << 53 | fl >> 11), -53 - 64 - lz);
  double p, pe;
  twoProd(hi, PIO2_HI, p, pe);
  pe += hi * PIO2_LO + lo * PIO2_HI;
  twoSum(p, pe, y0, y1);
  if ((x < 0) != (sign < 0)) y0 = -y0, y1 = -y1;
  return x < 0 ? -q : q;
}
int remPio2(double x, double& y0, double& y1) {  // x = n pi/2 + y0 + y1, |y0| <~ pi/4; returns n (mod 4 matters)
  if (std::fabs(x) < 0x1p20) {
    double fn = std::floor(x * INVPIO2 + 0.5), t1 = x - fn * PIO2_1, w = fn * PIO2_2, t2, e;  // both products exact
    twoSum(t1, -w, t2, e);
    twoSum(t2, e - fn * PIO2_3, y0, y1);
    if (std::fabs(y0) > 0x1p-27) return (int)fn;  // else x is close to a multiple: reduce exactly
  }
  return remPio2Large(x, y0, y1);
}
}  // namespace

double fsin(double x) {
  double a = std::fabs(x), y0, y1;
  if (a < 0.7853981633974483) return a < 0x1p-26 ? x : ksin(x, 0, false);
  if (!(a < INF)) return x - x;
  switch (remPio2(x, y0, y1) & 3) {
    case 0: return ksin(y0, y1, true);
    case 1: return kcos(y0, y1);
    case 2: return -ksin(y0, y1, true);
    default: return -kcos(y0, y1);
  }
}
double fcos(double x) {
  double a = std::fabs(x), y0, y1;
  if (a < 0.7853981633974483) return a < 0x1p-27 ? 1.0 : kcos(x, 0);
  if (!(a < INF)) return x - x;
  switch (remPio2(x, y0, y1) & 3) {
    case 0: return kcos(y0, y1);
    case 1: return -ksin(y0, y1, true);
    case 2: return -kcos(y0, y1);
    default: return ksin(y0, y1, true);
  }
}
double ftan(double x) {
  double a = std::fabs(x), y0, y1;
  if (a < 0.7853981633974483) return a < 0x1p-27 ? x : ktan(x, 0, false);
  if (!(a < INF)) return x - x;
  int n = remPio2(x, y0, y1);
  return ktan(y0, y1, n & 1);
}

// ---- exp: fdlibm's; expdd takes a tail (e^(x + xl)) for pow ----
namespace {
double expdd(double x, double xl) {
  const double P1 = 1.66666666666666019037e-01, P2 = -2.77777777770155933842e-03, P3 = 6.61375632143793436117e-05,
               P4 = -1.65339022054652515390e-06, P5 = 4.13813679705723846039e-08;
  double hi, lo;
  int k = 0;
  if (std::fabs(x) > 0.5 * LN2_H) {
    k = (int)std::floor(x * INVLN2_H + 0.5);
    hi = x - k * LN2_HI32, lo = k * LN2_LO32 - xl;  // k * LN2_HI32 is exact
  } else if (std::fabs(x) > 0x1p-28 || xl != 0) hi = x, lo = -xl;
  else return 1.0 + x;
  double r = hi - lo, rr = r * r, c = r - rr * (P1 + rr * (P2 + rr * (P3 + rr * (P4 + rr * P5))));
  double y = 1.0 + (r * c / (2.0 - c) - lo + hi);
  return k ? scalbnd(y, k) : y;
}
// ---- log as a double-double: x = 2^k m, m in [1, 2) near c = 1 + (j + 1/2)/128, log m = log c + 2 atanh(s) with
// s = (m - c)/(m + c), |s| < 2^-8, so the series' rounding is far below what pow multiplies by y ----
const double LOGC_H[128] = {
    0x1.ff00aa2b10bc0p-9, 0x1.7dc475f810a77p-7, 0x1.3cea44346a575p-6, 0x1.b9fc027af9198p-6,
    0x1.1b0d98923d980p-5, 0x1.58a5bafc8e4d5p-5, 0x1.95c830ec8e3ebp-5, 0x1.d276b8adb0b52p-5,
    0x1.075983598e471p-4, 0x1.253f62f0a1417p-4, 0x1.42edcbea646f0p-4, 0x1.60658a93750c4p-4,
    0x1.7da766d7b12cdp-4, 0x1.9ab42462033adp-4, 0x1.b78c82bb0eda1p-4, 0x1.d4313d66cb35dp-4,
    0x1.f0a30c01162a6p-4, 0x1.0671512ca596ep-3, 0x1.14785846742acp-3, 0x1.2266f190a5acbp-3,
    0x1.303d718e47fd3p-3, 0x1.3dfc2b0ecc62ap-3, 0x1.4ba36f39a55e5p-3, 0x1.59338d9982086p-3,
    0x1.66acd4272ad51p-3, 0x1.740f8f54037a5p-3, 0x1.815c0a14357ebp-3, 0x1.8e928de886d41p-3,
    0x1.9bb362e7dfb83p-3, 0x1.a8becfc882f19p-3, 0x1.b5b519e8fb5a4p-3, 0x1.c2968558c18c1p-3,
    0x1.cf6354e09c5dcp-3, 0x1.dc1bca0abec7dp-3, 0x1.e8c0252aa5a60p-3, 0x1.f550a564b7b37p-3,
    0x1.00e6c45ad501dp-2, 0x1.071b85fcd590dp-2, 0x1.0d46b579ab74bp-2, 0x1.136870293a8b0p-2,
    0x1.1980d2dd4236fp-2, 0x1.1f8ff9e48a2f3p-2, 0x1.2596010df763ap-2, 0x1.2b9303ab89d25p-2,
    0x1.31871c9544185p-2, 0x1.3772662bfd85bp-2, 0x1.3d54fa5c1f710p-2, 0x1.432ef2a04e814p-2,
    0x1.49006804009d1p-2, 0x1.4ec973260026ap-2, 0x1.548a2c3add263p-2, 0x1.5a42ab0f4cfe2p-2,
    0x1.5ff3070a793d4p-2, 0x1.659b57303e1f3p-2, 0x1.6b3bb2235943ep-2, 0x1.70d42e2789236p-2,
    0x1.7664e1239dbcfp-2, 0x1.7bede0a37afc0p-2, 0x1.816f41da0d496p-2, 0x1.86e919a330ba0p-2,
    0x1.8c5b7c858b48bp-2, 0x1.91c67eb45a83ep-2, 0x1.972a341135158p-2, 0x1.9c86b02dc0863p-2,
    0x1.a1dc064d5b995p-2, 0x1.a72a4966bd9eap-2, 0x1.ac718c258b0e4p-2, 0x1.b1b1e0ebdfc5bp-2,
    0x1.b6eb59d3cf35ep-2, 0x1.bc1e08b0dad0ap-2, 0x1.c149ff115f027p-2, 0x1.c66f4e3ff6ff8p-2,
    0x1.cb8e0744d7acap-2, 0x1.d0a63ae721e64p-2, 0x1.d5b7f9ae2c684p-2, 0x1.dac353e2c5954p-2,
    0x1.dfc859906d5b5p-2, 0x1.e4c71a8687704p-2, 0x1.e9bfa659861f5p-2, 0x1.eeb20c640ddf4p-2,
    0x1.f39e5bc811e5cp-2, 0x1.f884a36fe9ec2p-2, 0x1.fd64f20f61572p-2, 0x1.011fab125ff8ap-1,
    0x1.0389eefce633bp-1, 0x1.05f14bd26459cp-1, 0x1.0855c884b450ep-1, 0x1.0ab76bece14d2p-1,
    0x1.0d163ccb9d6b8p-1, 0x1.0f7241c9b497dp-1, 0x1.11cb81787ccf8p-1, 0x1.1422025243d45p-1,
    0x1.1675cababa60ep-1, 0x1.18c6e0ff5cf06p-1, 0x1.1b154b57da29fp-1, 0x1.1d610fe677003p-1,
    0x1.1faa34b87094cp-1, 0x1.21f0bfc65beecp-1, 0x1.2434b6f483934p-1, 0x1.26762013430e0p-1,
    0x1.28b500df60783p-1, 0x1.2af15f02640adp-1, 0x1.2d2b4012edc9ep-1, 0x1.2f62a99509546p-1,
    0x1.3197a0fa7fe6ap-1, 0x1.33ca2ba328995p-1, 0x1.35fa4edd36ea0p-1, 0x1.38280fe58797fp-1,
    0x1.3a5373e7ebdfap-1, 0x1.3c7c7fff73206p-1, 0x1.3ea33936b2f5cp-1, 0x1.40c7a4880dce9p-1,
    0x1.42e9c6ddf80bfp-1, 0x1.4509a5133bb0ap-1, 0x1.472743f33aaadp-1, 0x1.4942a83a2fc07p-1,
    0x1.4b5bd6956e274p-1, 0x1.4d72d3a39fd00p-1, 0x1.4f87a3f5026e9p-1, 0x1.519a4c0ba3446p-1,
    0x1.53aad05b99b7dp-1, 0x1.55b9354b40bcdp-1, 0x1.57c57f336f191p-1, 0x1.59cfb25fae87ep-1,
    0x1.5bd7d30e71c73p-1, 0x1.5ddde57149923p-1, 0x1.5fe1edad18919p-1, 0x1.61e3efda46467p-1,
};
const double LOGC_L[128] = {
    0x1.2821ad5a6d353p-63, -0x1.16d7687d3df21p-62, -0x1.0cb5a902b3a1cp-62, -0x1.0ae69229dc868p-64,
    -0x1.e9ae889bac481p-60, -0x1.ce55c2b4e2b72p-59, 0x1.f5a0e80520bf2p-59, 0x1.1e3c53257fd47p-61,
    0x1.80da5333c45b8p-59, -0x1.c125963fc4cfdp-62, 0x1.ddd4f935996c9p-59, -0x1.388458ec21b6ap-58,
    -0x1.eeedfcdd94131p-58, -0x1.2099e1c184e8ep-59, 0x1.0878cf0327e21p-61, 0x1.790dd951d90fap-58,
    0x1.85f325c5bbacdp-58, 0x1.50c647eb86499p-58, 0x1.a28813e3a7f07p-57, 0x1.f547bf1809e88p-57,
    -0x1.6b9c7d96091fap-63, -0x1.ab3a8e7d81017p-58, 0x1.68981bcc36756p-57, -0x1.65d22aa8ad7cfp-58,
    -0x1.0900e4e1ea8b2p-58, -0x1.b264062a84cdbp-58, -0x1.4be48073a0564p-58, -0x1.569d851a56770p-57,
    0x1.575e31f003e0cp-57, -0x1.e8c37918c39ebp-58, 0x1.ba27fdc19e1a0p-57, -0x1.73dee38a3fb6bp-57,
    0x1.239a07d55b695p-57, 0x1.834c51998b6fcp-57, -0x1.6e03a39bfc89bp-59, 0x1.c5f6dfd018c37p-61,
    -0x1.cb9568ff6feadp-57, 0x1.d1707f97bde80p-58, 0x1.03ec81c3cbd92p-57, 0x1.7b66298edd24ap-56,
    0x1.9d3d1b0e4d147p-56, -0x1.c9fdf9a0c4b07p-56, -0x1.0f76c57075e9ep-58, -0x1.896b5fd852ad4p-56,
    -0x1.51acc4c09b379p-60, -0x1.b5629d8117de7p-59, -0x1.e3265c6a1c98dp-56, -0x1.29931715ac903p-56,
    -0x1.9ffc341f177dcp-57, -0x1.42a87d977dc5ep-56, -0x1.819cf7e308ddbp-57, -0x1.8ebcb7dee9a3dp-56,
    -0x1.bc60efafc6f6ep-57, -0x1.f893d41c411f1p-56, -0x1.da856ccd987b3p-56, -0x1.52cc811d78d59p-57,
    -0x1.f6d5d64f5daf8p-57, -0x1.8783cb9801a5cp-56, -0x1.2923ca04b701cp-56, 0x1.3f9b16feb7dd8p-59,
    -0x1.e0ab4fdfa0595p-56, -0x1.e0e0ae234ae11p-56, 0x1.a5c09d24b70d9p-56, -0x1.917eeb69dd421p-56,
    0x1.90128698ba0b8p-56, 0x1.6a76b1a7d87c3p-58, 0x1.8163d6f46f714p-59, 0x1.a4479608a2c55p-56,
    -0x1.8adbccd326a3cp-56, 0x1.09e8707055996p-56, -0x1.4cbcb90c06305p-56, -0x1.82947258b688bp-58,
    -0x1.48879a214a2afp-61, 0x1.2acce112c40f2p-57, -0x1.a7be7f84ac06ap-57, 0x1.18734b81a1bf8p-57,
    0x1.01e1399f96398p-56, 0x1.667923e1f5a8ep-57, 0x1.91bafc7dbe130p-56, 0x1.ac371d7c8f7f5p-57,
    -0x1.97fc777bb19e5p-57, 0x1.6315c9e010800p-57, -0x1.adb0ac2cead1bp-57, 0x1.810dd40845ddep-57,
    0x1.e155c53483748p-56, 0x1.535b8ee4f9efep-58, 0x1.705826e49f318p-55, -0x1.fd6c935453f66p-56,
    -0x1.f7b9a9a8bc30fp-57, 0x1.3a8443b9db19dp-55, 0x1.02387ab1fcc90p-55, -0x1.ad0e24adb489ep-58,
    0x1.ce63eab883717p-60, 0x1.765142c2c671fp-58, -0x1.011eb47db6a99p-57, 0x1.09d58d91e58f2p-58,
    0x1.817b8f7a193b0p-58, -0x1.e24f0c9187c92p-57, -0x1.debb8cf0f6d11p-57, -0x1.96a95781c6727p-56,
    -0x1.43f60605aaab3p-55, 0x1.cb064524aceb0p-57, -0x1.51162c99b1cabp-55, 0x1.6c686739ffd99p-56,
    0x1.d6348fb97128fp-57, -0x1.bf28b3205ede1p-56, 0x1.27d4680964362p-60, -0x1.015bd362a6e5dp-55,
    -0x1.cd8f775b8f76ep-55, -0x1.be80db7025bedp-56, -0x1.f099168a1360bp-55, 0x1.14f22de7fc9e1p-56,
    0x1.657dc7a65061dp-56, 0x1.40fe2852d7b5ap-55, 0x1.8d6cf012a2948p-56, 0x1.ed0c544652b5ap-55,
    -0x1.c87a06beea773p-55, 0x1.1cd4d414e008dp-55, -0x1.e8ca8b1bcea9dp-55, 0x1.9b32128e4a77fp-55,
    -0x1.55c8b052e2539p-55, 0x1.e4197a357cb37p-56, -0x1.e953a3bc88192p-55, -0x1.172904559c6b6p-58,
    0x1.bf8da6db2b45cp-57, 0x1.dcfa37d75ef28p-55, -0x1.ca8b610e18dbfp-55, -0x1.a1b727edefae3p-55,
};
void logdd(double x, double& H, double& L) {  // x > 0, finite
  int k = 0;
  uint64_t u = bits(x);
  if (u < (1ull << 52)) x *= 0x1p54, u = bits(x), k = -54;  // subnormal
  k += int(u >> 52) - 1023;
  int j = int(u >> 45) & 127;
  double m = fromBits((u & 0xfffffffffffffull) | 0x3ff0000000000000ull), c = 1 + (j + 0.5) / 128, ch = LOGC_H[j], cl = LOGC_L[j];
  if (x > 0.9921875 && x < 1.0078125) m = x, c = 1, ch = cl = 0, k = 0;  // near 1: no cancellation with log c or k ln 2
  double f = m - c, d, dl, p, pe;  // f exact
  twoSum(m, c, d, dl);
  double s = f / d;
  twoProd(s, d, p, pe);
  double sl = ((f - p) - pe - s * dl) / d, t = s * s;  // s + sl = (m - c)/(m + c)
  double R = s * t * (2.0 / 3 + t * (2.0 / 5 + t * (2.0 / 7 + t * (2.0 / 9))));
  double kh, kl, s1, e1, s2, e2;
  twoProd((double)k, LN2_H, kh, kl);
  twoSum(kh, ch, s1, e1);
  twoSum(s1, 2 * s, s2, e2);
  twoSum(s2, e1 + e2 + (kl + k * LN2_L) + cl + (2 * sl + R), H, L);
}
double logSpecial(double x, bool& done) {  // log of 0, negatives, inf, nan
  done = true;
  if (x != x) return x;
  if (x < 0) return nan();
  if (x == 0) return -INF;
  if (x == INF) return x;
  return done = false, 0;
}
double scaled(double x, double h, double l) {  // log(x) * (h + l)
  bool done;
  double r = logSpecial(x, done), H, L, p, pe;
  if (done) return r;
  logdd(x, H, L);
  twoProd(H, h, p, pe);
  return p + (pe + H * l + L * h);
}
}  // namespace

double fexp(double x) {
  if (x != x) return x;
  if (x > 709.782712893383973096) return INF;
  if (x < -745.1332191019412) return 0;
  return expdd(x, 0);
}
double flog(double x) {
  bool done;
  double r = logSpecial(x, done), H, L;
  if (done) return r;
  return logdd(x, H, L), H + L;
}
double flog2(double x) { return scaled(x, INVLN2_H, INVLN2_L); }
double flog10(double x) { return scaled(x, INVLN10_H, INVLN10_L); }
double fpow(double x, double y) {
  if (y == 0 || x == 1) return 1;
  if (x != x || y != y) return x + y;
  double ay = std::fabs(y), ax = std::fabs(x);
  bool yint = std::floor(y) == y, yodd = yint && ay < 0x1p53 && std::floor(y * 0.5) * 2 != y;
  if (ay == INF) return ax == 1 ? 1 : (ax > 1) == (y > 0) ? INF : 0;
  if (x == 0) return y < 0 ? (yodd ? std::copysign(INF, x) : INF) : (yodd ? x : 0.0);
  if (ax == INF) return x > 0 ? (y > 0 ? INF : 0) : y > 0 ? (yodd ? -INF : INF) : (yodd ? -0.0 : 0.0);
  if (x < 0 && !yint) return nan();
  double sign = x < 0 && yodd ? -1 : 1, H, L, p, pe, r, rl;
  logdd(ax, H, L);
  double ph = y * H;
  if (ph > 710) return sign * INF;
  if (ph < -746) return sign * 0.0;
  double ys = ay > 0x1p900 ? y * 0x1p-200 : y, Hs = ay > 0x1p900 ? H * 0x1p200 : H;  // (H is tiny then)
  twoProd(ys, Hs, p, pe);
  twoSum(p, pe + y * L, r, rl);
  return sign * expdd(r, rl);
}
double fcbrt(double x) {
  if (x == 0 || !(std::fabs(x) < INF)) return x;
  double a = std::fabs(x), H, L, t2, t2e, t3, t3e;
  logdd(a, H, L);
  double t = expdd(H / 3, 0);  // within an ulp or so; one Newton step with an exact residual rounds it
  twoProd(t, t, t2, t2e);
  twoProd(t2, t, t3, t3e);
  t3e += t2e * t;
  t += ((a - t3) - t3e) / (3 * t * t);
  return std::copysign(t, x);
}
double fhypot(double x, double y) {
  x = std::fabs(x), y = std::fabs(y);
  if (x == INF || y == INF) return INF;
  if (x != x || y != y) return x + y;
  if (x < y) std::swap(x, y);
  if (y == 0) return x;
  double sc = 1;
  if (x > 0x1p500) x *= 0x1p-600, y *= 0x1p-600, sc = 0x1p600;
  else if (x < 0x1p-500) x *= 0x1p600, y *= 0x1p600, sc = 0x1p-600;
  if (x > y * 0x1p60) return x * sc;
  double xx, xe, yy, ye, h, l, ss, se;
  twoProd(x, x, xx, xe), twoProd(y, y, yy, ye), twoSum(xx, yy, h, l);
  l += xe + ye;
  double s = std::sqrt(h);
  twoProd(s, s, ss, se);
  return (s + ((h - ss) - se + l) / (2 * s)) * sc;
}

// ---- atan, atan2, asin, acos: fdlibm's ----
double fatan(double x) {
  const double aT[] = {3.33333333333329318027e-01, -1.99999999998764832476e-01, 1.42857142725034663711e-01,
                       -1.11111104054623557880e-01, 9.09088713343650656196e-02, -7.69187620504482999495e-02,
                       6.66107313738753120669e-02, -5.83357013379057348645e-02, 4.97687799461593236017e-02,
                       -3.65315727442169155270e-02, 1.62858201153657823623e-02};
  double a = std::fabs(x);
  if (x != x) return x;
  if (a >= 0x1p66) return std::copysign(ATANHI[3] + ATANLO[3], x);
  int id = -1;
  if (a < 0.4375) {
    if (a < 0x1p-27) return x;
  } else if (a < 1.1875) {
    if (a < 0.6875) id = 0, a = (2.0 * a - 1.0) / (2.0 + a);
    else id = 1, a = (a - 1.0) / (a + 1.0);
  } else if (a < 2.4375) id = 2, a = (a - 1.5) / (1.0 + 1.5 * a);
  else id = 3, a = -1.0 / a;
  double z = a * a, w = z * z;
  double s1 = z * (aT[0] + w * (aT[2] + w * (aT[4] + w * (aT[6] + w * (aT[8] + w * aT[10])))));
  double s2 = w * (aT[1] + w * (aT[3] + w * (aT[5] + w * (aT[7] + w * aT[9]))));
  if (id < 0) return x - x * (s1 + s2);
  double r = ATANHI[id] - ((a * (s1 + s2) - ATANLO[id]) - a);
  return x < 0 ? -r : r;
}
double fatan2(double y, double x) {
  if (x != x || y != y) return x + y;
  if (x == 1) return fatan(y);
  int m = (std::signbit(y) ? 1 : 0) | (std::signbit(x) ? 2 : 0);
  if (y == 0) return m < 2 ? y : m == 2 ? PI_HI : -PI_HI;
  if (x == 0) return y < 0 ? -PIO2_HI : PIO2_HI;
  if (std::fabs(x) == INF) {
    if (std::fabs(y) == INF) return (m & 1 ? -1 : 1) * (m & 2 ? 0x1.2d97c7f3321d2p+1 : PIO2_HI / 2);  // 3pi/4, pi/4
    return m == 0 ? 0.0 : m == 1 ? -0.0 : m == 2 ? PI_HI : -PI_HI;
  }
  if (std::fabs(y) == INF) return y < 0 ? -PIO2_HI : PIO2_HI;
  int k = int((bits(y) >> 52) & 0x7ff) - int((bits(x) >> 52) & 0x7ff);
  double z;
  if (k > 60) z = PIO2_HI + 0.5 * PI_LO, m &= 1;
  else if (x < 0 && k < -60) z = 0.0;
  else z = fatan(std::fabs(y / x));
  switch (m) {
    case 0: return z;
    case 1: return -z;
    case 2: return PI_HI - (z - PI_LO);
    default: return (z - PI_LO) - PI_HI;
  }
}
namespace {
double asinR(double z) {  // fdlibm's rational approximation for asin/acos
  const double pS0 = 1.66666666666666657415e-01, pS1 = -3.25565818622400915405e-01, pS2 = 2.01212532134862925881e-01,
               pS3 = -4.00555345006794114027e-02, pS4 = 7.91534994289814532176e-04, pS5 = 3.47933107596021167570e-05,
               qS1 = -2.40339491173441421878e+00, qS2 = 2.02094576023350569471e+00, qS3 = -6.88283971605453293030e-01,
               qS4 = 7.70381505559019352791e-02;
  double p = z * (pS0 + z * (pS1 + z * (pS2 + z * (pS3 + z * (pS4 + z * pS5)))));
  double q = 1.0 + z * (qS1 + z * (qS2 + z * (qS3 + z * qS4)));
  return p / q;
}
double lowCleared(double x) { return fromBits(bits(x) & 0xffffffff00000000ull); }
}  // namespace
double fasin(double x) {
  double a = std::fabs(x);
  if (x != x) return x;
  if (a >= 1) return a == 1 ? x * PIO2_HI + x * PIO2_LO : nan();
  if (a < 0.5) return a < 0x1p-26 ? x : x + x * asinR(x * x);
  double z = (1 - a) * 0.5, s = std::sqrt(z), r = asinR(z), v;
  if (a >= 0.975) v = PIO2_HI - (2 * (s + s * r) - PIO2_LO);
  else {
    double f = lowCleared(s), c = (z - f * f) / (s + f);
    v = 0.5 * PIO2_HI - (2 * s * r - (PIO2_LO - 2 * c) - (0.5 * PIO2_HI - 2 * f));
  }
  return x < 0 ? -v : v;
}
double facos(double x) {
  double a = std::fabs(x);
  if (x != x) return x;
  if (a >= 1) return a == 1 ? (x < 0 ? 2 * PIO2_HI + 2 * PIO2_LO : 0.0) : nan();
  if (a < 0.5) return a < 0x1p-57 ? PIO2_HI : PIO2_HI - (x - (PIO2_LO - x * asinR(x * x)));
  if (x < 0) {
    double z = (1.0 + x) * 0.5, s = std::sqrt(z), w = asinR(z) * s - PIO2_LO;
    return 2 * (PIO2_HI - (s + w));
  }
  double z = (1.0 - x) * 0.5, s = std::sqrt(z), df = lowCleared(s), c = (z - df * df) / (s + df);
  return 2 * (df + (asinR(z) * s + c));
}

#ifndef FMATH_ONLY  // (tools/fmathcheck.cpp compiles this file alone, against libm)
#define NUM(v, f) (isNum(v) ? num(v) : (ERR("@argument " f "() expects a number, got %s", typeName(v)), 0.0))
// std/math binds these with `extern "fut" fn sin(num): num !` (compile.cpp: the runtime's own functions).
void fmathInit() {
  native<+[](V x) { return box(fsin(NUM(x, "sin"))); }>("fut@sin");
  native<+[](V x) { return box(fcos(NUM(x, "cos"))); }>("fut@cos");
  native<+[](V x) { return box(ftan(NUM(x, "tan"))); }>("fut@tan");
  native<+[](V x) { return box(fasin(NUM(x, "asin"))); }>("fut@asin");
  native<+[](V x) { return box(facos(NUM(x, "acos"))); }>("fut@acos");
  native<+[](V x) { return box(fatan(NUM(x, "atan"))); }>("fut@atan");
  native<+[](V y, V x) { return box(fatan2(NUM(y, "atan2"), NUM(x, "atan2"))); }>("fut@atan2");
  native<+[](V x) { return box(fexp(NUM(x, "exp"))); }>("fut@exp");
  native<+[](V x) { return box(flog(NUM(x, "log"))); }>("fut@log");
  native<+[](V x) { return box(flog2(NUM(x, "log2"))); }>("fut@log2");
  native<+[](V x) { return box(flog10(NUM(x, "log10"))); }>("fut@log10");
  native<+[](V x, V y) { return box(fpow(NUM(x, "pow"), NUM(y, "pow"))); }>("fut@pow");
  native<+[](V x) { return box(fcbrt(NUM(x, "cbrt"))); }>("fut@cbrt");
  native<+[](V x, V y) { return box(fhypot(NUM(x, "hypot"), NUM(y, "hypot"))); }>("fut@hypot");
  native<+[](V x) { return box(std::ceil(NUM(x, "ceil"))); }>("fut@ceil");
  native<+[](V x) { return box(std::trunc(NUM(x, "trunc"))); }>("fut@trunc");
}
#endif
