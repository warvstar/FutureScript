// Compares fmath.cpp (the math fut uses) with the platform's libm: the largest difference in ulps per function,
// over random arguments in each range. c++ -O2 -ffp-contract=off -DFMATH_ONLY -std=c++23 tools/fmathcheck.cpp src/fmath.cpp
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <cstdlib>
double fsin(double), fcos(double), ftan(double), fasin(double), facos(double), fatan(double), fatan2(double, double),
    fexp(double), flog(double), flog2(double), flog10(double), fpow(double, double), fcbrt(double), fhypot(double, double);
static int64_t ord(double x) { int64_t i; memcpy(&i, &x, 8); return i < 0 ? INT64_MIN - i : i; }
static double ulps(double a, double b) {
  if (a != a && b != b) return 0;
  if (a != a || b != b) return 1e9;
  if (a == b) return 0;
  return (double)llabs(ord(a) - ord(b));
}
int main(int argc, char** argv) {
  long n = argc > 1 ? atol(argv[1]) : 2000000;  // arguments per function
  std::mt19937_64 g(1);
  auto uni = [&](double lo, double hi) { return std::uniform_real_distribution<double>(lo, hi)(g); };
  auto logu = [&](double lo, double hi) { return std::exp(uni(std::log(lo), std::log(hi))); };
  struct C { const char* name; double (*f)(double); double (*ref)(double); double lo, hi; bool lg; };
  C cs[] = {{"sin", fsin, ::sin, -10, 10}, {"sin big", fsin, ::sin, 1e6, 1e300, true}, {"cos", fcos, ::cos, -10, 10},
            {"cos big", fcos, ::cos, 1e6, 1e300, true}, {"tan", ftan, ::tan, -10, 10}, {"asin", fasin, ::asin, -1, 1},
            {"acos", facos, ::acos, -1, 1}, {"atan", fatan, ::atan, -50, 50}, {"atan wide", fatan, ::atan, 1e-300, 1e300, true},
            {"exp", fexp, ::exp, -745, 709.7}, {"exp small", fexp, ::exp, -1, 1}, {"log", flog, ::log, 1e-310, 1e308, true},
            {"log near 1", flog, ::log, 0.5, 2}, {"log at 1", flog, ::log, 1 - 1e-6, 1 + 1e-6}, {"log2", flog2, ::log2, 1e-300, 1e300, true}, {"log10", flog10, ::log10, 1e-300, 1e300, true},
            {"cbrt", fcbrt, ::cbrt, 1e-300, 1e300, true}};
  int bad = 0;
  for (auto& c : cs) {
    double worst = 0, at = 0;
    long same = 0;
    for (long i = 0; i < n; i++) {
      double x = c.lg ? logu(c.lo, c.hi) : uni(c.lo, c.hi);
      double u = ulps(c.f(x), c.ref(x));
      same += u == 0;
      if (u > worst) worst = u, at = x;
    }
    printf("%-11s max %4.0f ulp (at %.17g), %5.1f%% identical to libm\n", c.name, worst, at, 100.0 * same / n);
    bad += worst > (c.f == ftan ? 3 : 1);  // macOS's tan is itself up to 3 ulp off (checked against exact values)
  }
  struct C2 { const char* name; double (*f)(double, double); double (*ref)(double, double); };
  for (auto c : {C2{"pow", fpow, ::pow}, C2{"atan2", fatan2, ::atan2}, C2{"hypot", fhypot, ::hypot}}) {
    double worst = 0, wx = 0, wy = 0;
    long same = 0;
    for (long i = 0; i < n; i++) {
      double x = c.f == fpow ? logu(1e-3, 1e3) : uni(-100, 100), y = c.f == fpow ? uni(-100, 100) : uni(-100, 100);
      double u = ulps(c.f(x, y), c.ref(x, y));
      same += u == 0;
      if (u > worst) worst = u, wx = x, wy = y;
    }
    printf("%-11s max %4.0f ulp (at %.17g, %.17g), %5.1f%% identical to libm\n", c.name, worst, wx, wy, 100.0 * same / n);
    bad += worst > 1;
  }
  // exact cases
  int ex = 0;
  for (int k = 0; k <= 22; k++) ex += fpow(10, k) != std::pow(10, k) || flog10(std::pow(10, k)) != k;
  for (int k = -1074; k <= 1023; k++) ex += flog2(std::ldexp(1, k)) != k || fpow(2, k) != std::ldexp(1, k);
  for (int k = 1; k < 1000; k++) ex += fcbrt((double)k * k * k) != k || fpow(k, 2) != (double)k * k || fhypot(3 * k, 4 * k) != 5 * k;
  printf("exact cases wrong: %d\n", ex);
  return bad || ex;
}
