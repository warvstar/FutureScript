// extern fn: call C functions directly (resolved with dlsym; types num int long ptr str, return also void)
extern fn hypot(num, num): num
extern fn strlen(str): long
extern fn abs(int): int
extern fn getenv(str): str
extern fn atoi(str): int
extern fn ldexp(num, int): num

test "num, int, long and str arguments and results" {
  expect hypot(3, 4) == 5
  expect strlen("hello") == 5
  expect abs(-7) == 7
  expect atoi("1234") + 1 == 1235
  expect ldexp(1.5, 4) == 24
}
test "a NULL str result is nil" { expect getenv("FUT_NO_SUCH_VAR") == nil }
test "arguments are type-checked" {
  expect throws { hypot("a", 1) } == "hypot() argument 1: expected a number, got string"
}
