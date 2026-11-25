# fut

A small class-based language written in C++23 with no third-party libraries. A single-pass compiler turns source straight into arm64 machine code, with no AST and no bytecode. With `-w`, the running process is **hot-reloaded by patching its own machine code in place**; it never loads a DLL or dylib.

```sh
python3 build.py               # -> build/fut   (clang++ -std=c++23 -O2, no cmake)
python3 build.py test          # language tests + live hot-reload scenarios
python3 build.py test --debug  # same, under ASan/UBSan
python3 build.py fuzz 1000     # mutate the tests; any crash or sanitizer report fails
build/fut file.fut             # run
build/fut -w file.fut          # run and hot-reload on save (file and its imports)
build/fut --asm file.fut       # disassemble the generated code (uses clang + otool)
build/fut                      # REPL
```

Requires macOS on Apple silicon (arm64, `MAP_JIT`).

## Language

```
import "lib/geom.fut"                // top-level only, loaded once, path relative to this file
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

- **Values:** numbers (f64), strings, `true`/`false`/`nil`, lists, maps, functions and closures, classes and instances. `nil` and `false` are the only falsy values.
- **Operators:** `+ - * / %`, `== != < <= > >=`, `&& ||` (short-circuit, return the operand), `!`, `= += -= *= /=`. `+` with a string on either side concatenates.
- **Builtins:** `print len str clock sleep floor sqrt`.
- **Methods:** `list.push/pop/len`, `map.has/remove/keys/values/len`, `str.len`.
- **Closures:** a closure captures the enclosing variables by value. The closure can still change its own copy, so counters work, but it does not see later changes in the enclosing function.
- **Interpolation:** expressions inside `{...}` in a string can't contain `"`. Write `\{` for a literal brace.
- **Newline rules:** a `(` or `[` on a new line, or a leading `-`, starts a new statement rather than continuing the previous one.

## Hot reload (`-w`)

- **Safe points.** A watcher thread only sets a flag. Compiled loops check it at each back-edge, so a reload always happens on the main thread at a safe point.
- **Stubs.** Every function and method is called through a one-instruction `b body` stub. A reload installs new bodies, then rewrites each stub with a single aligned 4-byte store and an icache flush.
- **Incremental.** Every function and method is hashed by its source text and by the set of top-level declarations. Unchanged ones are skipped; a function that only moved has its line table shifted instead.
- **Compile errors** are reported with file:line:col and a caret, and nothing is patched. **Runtime errors** print a stack trace; under `-w`, fut then waits for a fix and reruns `main`.
- **State survives.** Globals keep their values, and only newly added `var`s get their initializers run. Class method tables are rebuilt in place, so existing instances see new, changed and removed methods. After each patch, `reload()` runs if it is defined.
- **Running frames.** Frames already running finish in their old code. If `main` itself changed, it is unwound and restarted with globals intact.
- **Changed parameter counts.** The function gets a new stub, and the old stub is pointed at a trap. Old code that is still running and calls the function gets a clean "stale call" error rather than reading garbage arguments.
- **Memory reclamation.** Replaced bodies are freed once no frame's return address points into them, found by walking the frame-pointer chain. Each reload prints the live code size.

## Implementation

| File | What |
|---|---|
| `fut.h` | NaN-boxed values, object layouts, VM block, arm64 encoders (all checked against the system assembler) |
| `compile.cpp` | Lexer and single-pass Pratt compiler emitting machine code; token pre-pass for names, arities and spans; imports; incremental commit |
| `rt.cpp` | Runtime helpers, natives, errors and stack traces, executable memory (free list, reclamation, `--asm` listing) |
| `gc.cpp` | Allocator using 64 KB pages with size classes (O(1) "is this a heap pointer?"); mark-sweep, conservative over stack and registers |
| `main.cpp` | Driver: watcher, safepoint reload, `main` restart, REPL, `--asm` |

**Codegen:**
- Locals live in callee-saved `x19`–`x25`, and in sp-relative slots after those run out.
- `x28` holds the VM block (helper table and globals), `x27` the NaN tag, and `x26` the stack limit.
- Arguments go in `x0`–`x7`, with `x8` pointing at the full argument window, so calls take any number of arguments.

**Fast paths:**
- Arithmetic and comparisons on two numbers run inline. A literal right operand skips its type check.
- An `if`/`while` condition of the form `a < b` compiles to a compare and a branch, without building a boolean.
- The prologue is generated after the body, so it saves only the callee-saved registers the function used, reserves the exact frame size, and checks the stack only when the function makes calls. All returns share one epilogue.
- Field reads and writes, and method calls, use per-call-site inline caches keyed on the instance's class. Each class has a fixed field layout, so a cache hit is a few loads. The caches are cleared on every reload.
