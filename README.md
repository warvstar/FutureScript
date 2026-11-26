# fut

A small class-based language written in C++23 with no third-party libraries. A single-pass compiler turns source straight into arm64 machine code, with no AST and no bytecode. With `-w`, the running process is **hot-reloaded by patching its own machine code in place**; it never loads a DLL or dylib. The same compiler can instead emit **WebAssembly**, running in Node or a browser and hot-reloading by swapping function-table entries (see [WebAssembly](#webassembly)).

```sh
python3 build.py               # -> build/fut   (clang++ -std=c++23 -O2, files in parallel, only what changed; no cmake)
python3 build.py test          # language tests, tests/specs/*.fut via `fut test`, live hot-reload scenarios
python3 build.py test --debug  # same, under ASan/UBSan
python3 build.py fuzz 1000     # mutate the tests; any crash or sanitizer report fails
build/fut file.fut             # run
build/fut test file.fut        # run its `test` blocks (--json: one JSON object per line; --filter text; --timeout s)
build/fut -w file.fut          # run and hot-reload on save (file and its imports); type lines to run them inside it
build/fut --asm file.fut       # disassemble the generated code (uses clang + otool)
build/fut                      # REPL
python3 build.py wasm          # -> build/fut.wasm (compiler + runtime for WebAssembly)
python3 build.py test --wasm   # the same tests, run under Node
node web/run.mjs [-w] file.fut # run on WebAssembly in Node (same flags; no file: REPL)
node web/serve.mjs             # browser: http://localhost:8000/?file=examples/paddle.fut
```

Requires macOS on Apple silicon (arm64, `MAP_JIT`).

## Language

```
import "lib/util.fut"                // top-level only, loaded once, path relative to this file
import "lib/geom.fut" as geom        // namespaced: geom.area(1), geom.Circle(2), class C : geom.Shape
var count = 0                        // top-level var = global; keeps its value across reloads

fn fib(n) { if n < 2 { return n } return fib(n - 1) + fib(n - 2) }

class Shape {
  fn init(name) { self.name = name }  // constructor; fields are created by assignment
  fn area() { return 0 }
}
class Rect : Shape {
  fn init(w, h) { super.init("rect") self.w = w self.h = h }
  fn area() { return self.w * self.h }
}

fn counter() { var n = 0 return fn() { n += 1 return n } }   // closures

fn main() {                          // called automatically if defined
  var r = Rect(2, 3)
  print("{r.name}: {r.area()}")      // string interpolation
  var m = {name: "fut", "k": [1, 2]} // maps; m.name is m["name"]; missing keys read as nil
  for k in m { print(k) }            // keys, in insertion order
  for i in 0..3 { }                  // numeric range, end exclusive; also lists and strings
  var next = counter()
  next()
}

fn reload() { }                      // optional: runs after every hot reload (migrate state here)
```

- **Values:** numbers (f64; whole numbers are exact up to 2^53 and print without a decimal point), strings, `true`/`false`/`nil`, lists, maps, functions and closures, classes and instances. `nil` and `false` are the only falsy values.
- **Operators:** `+ - * / %`, `== != < <= > >=`, `&& ||` (short-circuit, return the operand), `!`, `= += -= *= /=`. Bitwise `& | ^ ~ << >>` work on whole numbers as 64-bit integers (`1.5 & 1` is an error), with Python's precedence: `2 + 3 & 6` is `(2 + 3) & 6`. `+` with a string on either side concatenates.
- **Builtins:** `print len str int clock sleep floor sqrt fiber run` (`int` truncates toward zero), plus graphics (below).
- **Numbers:** `12`, `1.5`, `2e3`, `0xff8800`.
- **Keywords:** `var fn class if else while for in return break continue true false nil self super import extern yield try catch throw` (`as` is special only after an import).
- **Methods:** `list.push/pop/len`, `map.has/remove/keys/values/len`, `str.len/has` (`has`: contains a substring), `fiber.resume/send/done`.
- **Closures:** a closure captures the enclosing variables by value. The closure can still change its own copy, so counters work, but it does not see later changes in the enclosing function.
- **Interpolation:** expressions inside `{...}` in a string can't contain `"`. Write `\{` for a literal brace.
- **Newline rules:** a `(` or `[` on a new line, or a leading `-`, starts a new statement rather than continuing the previous one.

## Errors

```
fn load(path) { if path == "" { throw "no path" } return path }
try { load("") } catch e { print("failed: {e}") }    // -> failed: no path
try { nil.x } catch e { print(e) }                   // runtime errors are caught too, as their message
try { throw {code: 42} } catch e { print(e.code) }   // any value can be thrown
try { risky() } catch { }                            // the name is optional
```

`throw` unwinds to the innermost `try` on the current stack, through any number of calls. `return`, `break` and `continue` leave a `try` cleanly. An uncaught error inside a fiber ends the fiber and is thrown again in its resumer, where it can be caught. An uncaught error stops the program with a stack trace (under `-w`, it waits for a fix). Entering a `try` costs a few stores: the handler record lives in the function's frame, and a function containing `try` keeps its locals in memory so a catch sees their latest values.

## Tests

```
fn total(items) { var s = 0 for i in items { s += i.price * i.qty } return s }

test "total sums price times qty" {
  expect total([{price: 2, qty: 3}, {price: 5, qty: 1}]) == 11
  expect total([]) == 0
}
test "rejects a negative quantity" {
  expect throws { total([{price: 1, qty: -1}]) } == "negative qty"
}
test "discounts" { expect discount(10, 0.1) == 9 }   // discount isn't written yet: pending
```

```
$ build/fut test cart.fut
ok      total sums price times qty
FAIL    rejects a negative quantity (cart.fut:7)
  cart.fut:8: expect throws { ... } == "negative qty"
    nothing was thrown
pending discounts (cart.fut:10): undefined name 'discount' at cart.fut:10:27
3 tests: 1 passed, 1 failed, 1 pending (0.2 ms)
```

- **`test "name" { ... }`** at the top level declares a test. `fut test file.fut` runs the top-level code once, then every test block in order, including tests in imported files. It does not call `main`. A normal run skips test blocks without compiling them.
- **`expect a op b`**, where `op` is a comparison, checks the comparison. On failure it reports the expression's source text and the value of each side that isn't a literal. **`expect x`** checks that `x` is truthy, and `expect a == b && c` checks the whole condition. Inside `expect`, `==` and `!=` compare lists and maps by their contents, ignoring key order, and a failure names the first difference (`first difference at .items[0].qty: 3 vs 4`). The language's own `==` still compares lists and maps by identity.
- **`expect throws { ... }`** passes if the block throws, and `expect throws { ... } == value` also checks the thrown value.
- **`expect prints { ... } == "text\n"`** checks exactly what `print` wrote inside the block.
- **`run(file)`** or **`run({source: "...", args: [...], timeout: s})`** runs fut on a program in a child process and returns `{code, out, err, timedOut}`. Use it to check what can't be checked in-process: uncaught errors and their stack traces, compile errors, exit codes, and whether `main` runs. A relative `file` is found next to the program, and `args` go before the file (`args: ["test", "--json"]`). In `source`, write `\{` for a literal brace, because `{...}` interpolates.
- A failed `expect` throws an error. Outside `fut test` it works as an assertion, with the usual stack trace. A test whose `expect` fails is reported as failed even if its own `try` catches that error.
- **Pending tests support writing the spec first.** A test that names a function, class or module member that isn't defined yet is reported as pending instead of failing the whole compile. Any other compile error is still an error.
- **Statuses:** `ok`, `FAIL` (an `expect` failed), `ERROR` (an uncaught runtime error, with its trace) and `pending`. Output that a failing test printed is shown under it. The exit status is 0 only when every test passed: 1 means something failed, errored or is pending, and 2 means a compile error.
- **`--json`** prints one object per line: `{"type":"test", "name", "file", "line", "status", "ms", "output"}`. A failed test adds `"expect": {"file", "line", "expr", "left": {"expr", "value"}, "right": {...}, "diff": {"at", "what"}, "reason"}`. An error adds `"message"` and `"trace"`, and a pending test adds `"reason"`. A final `{"type":"summary", "passed", "failed", "errors", "pending", "ms"}` follows. A compile error is `{"type":"compile_error", "file", "line", "col", "message", "text"}`, and an error in top-level code is `{"type":"error", "message", "trace"}`.
- **`--filter text`** runs only the tests whose name contains `text`.
- **Isolation:** natively, each test runs in its own process, forked after the top-level code has run. A test's changes to globals, a crash or a hang stay inside that test. A test that runs longer than `--timeout` seconds (default 10) is stopped and reported as an error. Under WebAssembly, tests share one process and run in order with no timeout, so tests shouldn't depend on each other's state.
- `test`, `expect`, `throws` and `prints` are reserved only in these positions, so they stay usable as ordinary names.
- **This repository's tests:** `tests/specs/*.fut` are specs, run with `fut test`; the programs they `run()` are in `tests/specs/progs/`. A few files in `tests/*.fut` still use `// expect:` comments that the runner compares with stdout: `basics`, `control`, `cond`, `classes`, `err_compile` and `expect`. They test, without using `expect`, the features `expect` itself depends on.

## Modules

`import "path.fut"` merges the file's top-level names into the program. `import "path.fut" as name` keeps them in their own namespace: the file's functions, classes and vars are reached as `name.x`, the module's own code still uses their short names, and two modules may define the same name. A module's `main` is not run. A wrong `name.x` is a compile error. Using `name` alone as a value is an error too, since a module is not a value. Hot reload patches module functions like any other (`patched geom.area`).

## Fibers

A fiber runs a function on a stack of its own. `yield` suspends it from any depth of calls, not just its top function, and hands a value to whoever resumed it.

```
fn walk(node) {                      // yields from inside a recursion
  if node == nil { return }
  walk(node.left)  yield node.value  walk(node.right)
}
var it = fiber(fn() { walk(tree) })
for v in it { print(v) }             // for-in resumes until the fiber returns

var f = fiber(fn(first) { var total = first while true { total += yield total } })
f.send(10)  f.send(5)                // -> 10, 15: send(v) resumes and becomes yield's value
f.resume()  f.done()                 // resume() sends nil
```

An error inside a fiber ends it and is raised again in its resumer. Switching costs about as much as a function call: callee-saved registers are saved on one stack, and the stack pointer moves to the other. Hot reload sees suspended fibers' frames, so code they will return into is kept.

## C functions

```
extern fn hypot(num, num): num       // any C function in the process (libc, libm, system frameworks)
extern fn getenv(str): str
extern "libsqlite3.dylib" fn sqlite3_libversion(): str   // or from a library
print(hypot(3, 4))
```

Types: `num` (double), `int` (32-bit), `long` (64-bit), `ptr` (a pointer, as a number), `str` (a `const char*`; a returned one is copied, NULL becomes nil). The return type defaults to `void` (nil). Up to 7 parameters; variadic C functions such as `printf` are not supported. A call is a thunk that branches to one generic marshaller: integer and floating-point arguments travel in separate registers (`x0`–`x7` and `d0`–`d7`), so passing all of them suits every signature. Arguments are type-checked, but what the C function does with them is not.

## Graphics

One window with a pixel buffer; colors are `0xRRGGBB`. AppKit is loaded only when `window()` is first called.

```
window(320, 240)                     // open (or resize) the window; coordinates are pixels, top-left origin
while present() {                    // show the frame, handle input, wait for the next 1/60 s; false once closed
  clear(0x202040)
  rect(x, y, w, h, 0x40a040)
  circle(x, y, r, 0xffcc00)
  if key("left") { }                 // held keys: "a".."z", "0".."9", "space", "enter", "escape", "tab", arrows
  if mousedown() { print("{mousex()},{mousey()}") }
}
```

`build/fut -w examples/paddle.fut` is a small game to live-code.

## Hot reload (`-w`)

- **Live REPL.** Lines typed into the terminal run inside the running program at its next safe point, with full access to its globals (`player.hp = 99`, `enemies.len()`, or a new `fn`). Functions defined this way survive file reloads. After a runtime error the program waits for a fix, and the REPL still works, so you can inspect what went wrong.

- **Safe points.** A watcher thread only sets a flag. Compiled loops check it at each back-edge, so a reload always happens on the main thread at a safe point.
- **Stubs.** Every function and method is called through a one-instruction `b body` stub. A reload installs new bodies, then rewrites each stub with a single aligned 4-byte store and an icache flush.
- **Incremental.** Every function and method is hashed by its source text and by the set of top-level declarations. Unchanged ones are skipped; a function that only moved has its line table shifted instead.
- **Compile errors** are reported with file:line:col and a caret, and nothing is patched. **Runtime errors** print a stack trace; under `-w`, fut then waits for a fix and reruns `main`.
- **State survives.** Globals keep their values. A `var`'s initializer runs again only if the var is new or its declaration line was edited, so changing `var gravity = 0.15` to `0.3` takes effect at once while untouched state is kept. Class method tables are rebuilt in place, so existing instances see new, changed and removed methods. After each patch, `reload()` runs if it is defined.
- **Running frames.** Frames already running finish in their old code. If `main` itself changed, it is unwound and restarted with globals intact.
- **Changed parameter counts.** The function gets a new stub, and the old stub is pointed at a trap. Old code that is still running and calls the function gets a clean "stale call" error rather than reading garbage arguments.
- **Memory reclamation.** Replaced bodies are freed once no frame's return address points into them, found by walking the frame-pointer chain. Each reload prints the live code size.

## WebAssembly

`python3 build.py wasm` compiles the compiler and runtime themselves to `build/fut.wasm` (Homebrew LLVM: `brew install llvm lld wasi-libc wasi-runtimes`). Node 25+ (tested on 26) or Chrome 137+ is needed for JSPI and wasm exceptions. Hosts:

- `node web/run.mjs [-w | -v] file.fut`: the same CLI as `build/fut`, including `-w` hot reload and the live REPL on stdin.
- `node web/serve.mjs [port]`, then open `http://localhost:8000/?file=path/to/file.fut`: a canvas for `window()`/`present()`, keys and mouse, a REPL box, and hot reload of the file and its imports whenever they are saved.

How it maps:
- **Code.** Each compile becomes one wasm module, with one function per fut function. It shares the runtime's memory, function table and stack pointer. A stub is a table slot, so patching it is a single `table.set`, and a direct call is a `call_indirect` through the slot. Retired slots are cleared and reused; V8 frees a module once nothing references it.
- **Values and frames.** Values are NaN-boxed exactly as on arm64. Each local's home is a slot in a frame on the C++ shadow stack, which the GC scans. A wasm local caches it, and reads use that cache. Stores write both, except when the value is known to be a number (range counters, `x op= y` on a local that started as a number): those stay in an f64 wasm local, so a loop-carried `s += x` never leaves the FP registers. Temporaries live only in wasm locals, so a collection runs only at safe points (function entry and loop back-edges).
- **Branch hints.** Type checks, inline-cache hits and safe-point polls carry wasm branch hints (likely / unlikely). Without them V8 treats both arms of an `if` alike, and the helper call on the slow path makes it spill the loop's values around the fast path too. The hints alone take a 20M-iteration float loop from 67 ms to 17 ms.
- **Tier 2** works as on arm64. A failed entry guard continues in the tier-1 body with a tail call (`return_call_indirect`), which leaves no extra frame. A tier-2 body's recursive calls are direct `call`s (reloading drops the body, so they never need repointing). V8 runs these faster than calls through the table: `fib(32)` 26 → 22 ms. Typed function references (`call_ref` through a global) measured no faster than `call_indirect`.
- **Inline allocation** works as on arm64. `C(args)` and closures bump their size class's run in the VM block, with no call into the runtime.
- **try/throw** use wasm exceptions. Uncaught errors print a stack trace mapped from V8's module offsets to fut lines.
- **Fibers** each get their own shadow stack. The host suspends and resumes their wasm stacks with JSPI (`WebAssembly.Suspending` / `promising`).
- **`extern fn`** calls JavaScript instead of C. With no library it looks in a small libc shim (`strlen`, `atoi`, `getenv`, ...), then `Math`, then globals. `extern "console" fn log(str): void` names an object.

Performance, as best of 5 runs after warm-up (wasm vs arm64): float loop 17 vs 17 ms, `fib(32)` 22 vs 12 ms, 5M method calls 24 vs 15 ms, 1M allocations 7 vs 4 ms. The first call of a function runs in V8's baseline compiler, and V8 has no on-stack replacement for wasm, so a long loop in a function called only once (the first run of the float loop: 77 ms) stays at baseline speed. Functions called repeatedly, such as the ones a game loop calls each frame, are optimized after a few calls. In Node, `node --no-liftoff web/run.mjs` optimizes everything up front, at about 200 ms of extra startup.

Other differences from arm64: file changes and typed lines are delivered only while the program waits in `sleep()`, `present()` or a fiber switch, because JS events cannot interrupt running wasm. Compile errors unwind with `setjmp`/`longjmp`, since WASI libc++ has no C++ exceptions.

## Implementation

| File | What |
|---|---|
| `fut.h` | NaN-boxed values, object layouts, VM block, arm64 encoders (all checked against the system assembler) |
| `compile.h` | Lexer and the parser state shared by both code generators |
| `compile.cpp` | Single-pass Pratt compiler, generic over the code generator; token pre-pass for names, arities and spans; imports; incremental commit; tier 2 |
| `gen_a64.h` / `gen_wasm.h` | Code generators: arm64 machine code / WebAssembly function bodies, behind one interface |
| `wasm.h` | WebAssembly opcodes, encodings, helper signatures |
| `rt.cpp` | Portable runtime: helpers, natives, objects, inline caches |
| `jit.cpp` | arm64 platform: executable memory (free list, reclamation, `--asm`), stack traces and try, fibers, C calls |
| `wasm.cpp` | WebAssembly platform: module building and table patching, JS calls, JSPI fibers, the exports the JS host drives |
| `web/` | `fut.js` (JS host: instantiation, installs, traces, fibers, driver), `run.mjs` (Node CLI), `serve.mjs` + `index.html` (browser) |
| `gc.cpp` | Allocator using 64 KB pages with size classes (O(1) "is this a heap pointer?"); mark bitmap, no sweep, conservative over stack and registers |
| `gfx.cpp` | The window: AppKit through the Objective-C runtime, loaded on first use (a canvas in wasm) |
| `main.cpp` | Driver: watcher, safepoint reload, `main` restart, REPL, `--asm`, `fut test` |
| `test.cpp` | `expect`'s check (deep equality, failure details) and the test runner's reporting, text and JSON; shared by both platforms |

**Codegen:**
- Locals live in callee-saved `x19`–`x25`, then in sp-relative slots. A local whose initial value is a number, and every `for` range counter, lives in `d8`–`d15` instead. Arithmetic on it then stays in the FP unit, so `s += x` in a loop is one `fadd` on the loop-carried path.
- `x28` holds the VM block (helper table, globals and inline caches), `x27` the NaN tag, and `x26` the stack limit.
- Arguments go in `x0`–`x7`, with `x8` pointing at the full argument window, so calls take any number of arguments.
- A direct call to a top-level function is a `bl` straight to its body. Every such call site is recorded; after each reload it is re-pointed to wherever the function's stub now leads, so it behaves exactly like a call through the stub.

**Tier 2:**
- Each function and method counts its calls. At 1,000 its source is compiled again, with three speculations:
  - Arguments that were numbers on that call are assumed to stay numbers. An entry guard checks them, and branches to the tier-1 body (with the arguments untouched) if one is not.
  - A recursive call is assumed to return a number. This is kept only if every `return` in the new code is then proven to be a number, which makes the assumption true by induction. Otherwise it compiles again without the assumption.
  - Proven facts survive loop heads for locals the loop never assigns.
- The function's stub then points at the new body, and direct call sites are relinked. Reloading the function drops its tier-2 body, and it tiers up again. `fib(32)` goes from 14 ms to 11 ms (unoptimized C++: 13 ms). `-v` logs tier-ups, as `-w` does.
- Not handled: lambdas, and loops in code that is entered only once (no on-stack replacement).

**Fast paths:**
- Arithmetic and comparisons on two numbers run inline. Type checks are skipped for number literals (loaded with `fmov d, #imm` when encodable), for range counters, and for locals already proven to be numbers. A local is proven when an earlier `-`, `*`, `/`, `%`, or a comparison with a number, ran on it, since those raise otherwise. The proof holds until the local is assigned or a loop head is reached, and merges across `if`/`else`.
- An `if`/`while` condition of the form `a < b` compiles to a compare and a branch, without building a boolean.
- The prologue is generated after the body, so it saves only the registers the function used, reserves the exact frame size, and checks the stack only when the function makes calls. All returns share one epilogue.
- Instances keep their fields inline, sized to the class's field count at construction. Their header holds the class id and slot count, so a field or method inline cache hit is one 64-bit (or 32-bit) compare and a load. The caches are cleared on every reload.
- `C(args)` for a top-level class allocates inline. The call site caches the class and its `init`. On a hit, it bumps the free run of the instance's size class, writes the header and fills the slots, then calls `init`, with no call into the runtime. Closures are allocated the same way.

**Memory:**
- Small objects are rounded up to one of 17 size classes, from 16 B to 16 KB, and each 64 KB page holds one class. Bigger objects get pages of their own.
- Mark bits live in a side bitmap, one bit per 16 bytes. When a collection ends, the bitmap becomes the map of live objects. There is no sweep: allocation bumps through the runs of free cells between live objects, so a collection never touches a dead object. A page with nothing live goes back to a pool.
- Lists, maps, classes, fibers, and instances whose fields moved out of line own `malloc`'d memory. They are registered when created and finalized when found dead.
- Roots are the globals, constants, and every stack (the current one from its real `sp`, plus each suspended fiber's) and registers, scanned conservatively. Heap objects are traced precisely.
- A collection runs when the heap would grow past twice the pages in use after the last one (minimum 8 MB). Free pages beyond that are returned to the OS with `madvise`.
