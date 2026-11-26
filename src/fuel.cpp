// Steps: a count of the work fut code does that is the same on every platform and every run, so a limit on it cuts a
// program off at the same point everywhere (a time limit would not). Every loop back-edge and every function entry
// is one step: JIT code takes it from vm->fuel (gen_a64.h, gen_wasm.h, fuel()) and calls rt_fuel when that goes
// below zero. Only code compiled with stepsOn counts (up to ~15% natively; a 1 ns wasm loop body, 60%): it goes on, and everything
// compiled so far recompiles, once a program names budget or stepCount, or the host asks (fut_init). Builtins count as the steps of the fut code they call back; their own work is bounded by their input.
//
// budget(n, f) runs f() with at most n steps (or what the enclosing budget has left, if less). Running out raises
// [budget] at the step that ran out; fuel stays below zero, so a try inside f that catches it raises again at its
// next step, and only code outside the budget gets past it. Leaving a budget, by returning or by an error, gives
// the enclosing count back what the budget did not use. The open budgets are a stack: try records save its depth
// (vm->budgets, next to vm->who), and a catch, a driver (effLeave) or a finished fiber unwinds it to theirs.
#include "fut.h"
#include <cmath>

namespace {
constexpr int64_t FREE = int64_t(1) << 62;  // outside every budget: never runs out (centuries of steps)
struct Budget { int64_t outer, given, n; };  // vm->fuel when it opened, what it gave (min(n, outer)), and n
TLS std::vector<Budget> open;
TLS u32 fiberBase;  // the budgets open when the running fiber was resumed: it can't yield out of one of its own

void unwindTo(u32 depth) {
  while (open.size() > depth) {
    Budget b = open.back();
    open.pop_back();
    vm->fuel = b.outer - (b.given - vm->fuel);  // what it used comes off the enclosing count (all of it, if it ran out)
  }
  vm->budgets = (u32)open.size();
}
void rt_fuel() {
  if (open.empty()) return void(vm->fuel = FREE);  // only after 2^62 steps
  size_t i = open.size() - 1;
  while (i > 0 && open[i].given < open[i].n) i--;  // the budget whose own limit this is (an inner one may ask for more)
  ERR("@budget ran out of steps: the budget was %lld (each loop iteration and each call is a step)", (long long)open[i].n);
}
void rt_budget(u32 depth) { unwindTo(depth); }  // wasm: a catch whose try opened fewer budgets than are open now

V n_budget(V nv, V f) {
  double d = isNum(nv) ? num(nv) : -1;
  if (!(d >= 0) || d != std::trunc(d) || d > 0x1p53) ERR("@argument budget() expects a whole number of steps, got %s", toStr(nv, true).c_str());
  Fn* fn = is(f, K::Clo) ? as<Clo>(f)->fn : is(f, K::Fn) ? as<Fn>(f) : nullptr;
  if (!fn || !fn->code) ERR("@argument budget() expects a function to run, got %s", typeName(f));
  if (fn->arity - is(f, K::Clo) != 0) ERR("@arity budget()'s function must take no parameters");
  if (!stepsOn) ERR("@budget budget() can't count steps: this code was compiled without step counting");
  int64_t n = (int64_t)d, given = std::min(n, vm->fuel);
  u32 depth = (u32)open.size();
  open.push_back({vm->fuel, given, n}), vm->budgets = depth + 1, vm->fuel = given;
  V args[8] = {f};
  V r = vmEnter(fn->code, is(f, K::Clo) ? args : args + 1);
  unwindTo(depth);
  return r;
}
V n_stepCount() {  // the steps taken inside the innermost budget, or since the program started
  return box(double(open.empty() ? FREE - vm->fuel : open.back().given - vm->fuel));
}
}  // namespace

bool stepsOn;
u32 budgetDepth() { return (u32)open.size(); }
void budgetUnwind(u32 depth) { unwindTo(depth); }
u32 budgetFiberEnter() { return std::exchange(fiberBase, (u32)open.size()); }
void budgetFiberLeave(u32 outer) { unwindTo(fiberBase), fiberBase = outer; }  // a fiber that failed left its budgets open
void budgetYieldCheck() {
  if (open.size() > fiberBase) ERR("@fiber yield inside budget(): the budget would go on counting the steps of whatever runs next");
}
void fuelInit() {
  vm->helpers[H_FUEL] = (void*)rt_fuel, vm->helpers[H_BUDGET] = (void*)rt_budget;
  native<n_budget>("budget"), native<n_stepCount>("stepCount");
}
