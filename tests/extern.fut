// extern fn: call C functions directly (resolved with dlsym; types num int long ptr str, return also void)
extern fn hypot(num, num): num
extern fn strlen(str): long
extern fn abs(int): int
extern fn getenv(str): str
extern fn atoi(str): int
extern fn ldexp(num, int): num
print(hypot(3, 4))                 // expect: 5
print(strlen("hello"))             // expect: 5
print(abs(-7))                     // expect: 7
print(getenv("FUT_NO_SUCH_VAR"))   // expect: nil
print(atoi("1234") + 1)            // expect: 1235
print(ldexp(1.5, 4))               // expect: 24
print(hypot("a", 1))               // error: hypot() argument 1: expected a number, got string
