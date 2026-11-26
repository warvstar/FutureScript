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
build/fut api file.fut         # write each imported file's .api (its pub declarations); `fut test` checks them
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
  override fn area() { return self.w * self.h }   // replacing an inherited method says so
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
- **Builtins:** `print len str int clock sleep random floor sqrt fiber run` (`int` truncates toward zero; `random()` is in [0, 1)), plus graphics (below).
- **Numbers:** `12`, `1.5`, `2e3`, `0xff8800`.
- **Keywords:** `var fn class if else while for in return break continue true false nil self super import extern yield try catch throw`. `as`, `pub`, `override`, `atomic`, `effect`, `handle` and `with` are special only where they start an import alias, declaration or block, so they stay usable as names.
- **Methods:** `list.push/pop/len`, `map.has/remove/keys/values/len`, `str.len/has` (`has`: contains a substring), `fiber.resume/send/done`.
- **Closures:** a closure captures the enclosing variables by value. The closure can still change its own copy, so counters work, but it does not see later changes in the enclosing function.
- **Interpolation:** expressions inside `{...}` in a string can't contain `"`. Write `\{` for a literal brace.
- **Newline rules:** a `(` or `[` on a new line, or a leading `-`, starts a new statement rather than continuing the previous one.

## Classes

```
class Unit {
  var hp = 10                        // declared fields, with defaults: a number, string, true, false or nil
  var name
  fn init(name) { self.name = name }
  fn hit(n) { self.hp -= n }
}
class Mage : Unit {
  var mana = 5
  override fn hit(n) { self.mana -= n }
}
```

- **Fields** are created by assignment (`self.w = w`) unless the class declares them. A class that declares a field, or inherits one, is **closed**: it has exactly the declared fields. A typo such as `self.hpp = 3` is a compile error inside the class's own methods and a runtime error elsewhere, instead of silently creating a field that nothing reads. A field never assigned reads as its default. Instances that already exist get the default when hot reload adds a declared field.
- **`override`** is required on a method that replaces a superclass's method, and is an error on one that doesn't. A subclass method can't silently replace a base method that is added later, and renaming or removing a base method can't leave subclass methods quietly orphaned. An overriding method must take as many parameters as the method it replaces. `init` is exempt.

## Types

Types are optional. Code without them is as dynamic as ever. Where one is written, on a parameter, a result or a declared field, it is checked at run time at that boundary.

```
pub class Account {
  var balance: num = 0
  var note: str? = nil               // ?: nil is allowed too
  fn withdraw(n: num): num { self.balance -= n return self.balance }
}
pub fn transfer(from: Account, to: Account, n: num) { from.withdraw(n) to.deposit(n) }
fn find(items: list, f: fn): any { }
```

```
transfer(a, "bob", 5)  // -> argument 'to' of transfer() must be Account, got the string "bob"
a.balance = nil        // -> field 'balance' of Account must be num, got nil
```

- **Types:** `num str bool list map fn any`, and class names, including a module's (`geom.Shape`). A class type accepts instances of its subclasses. `T?` also accepts nil. `any` documents a value that can be anything, nil included.
- **When they are checked:** an argument when the function is entered, a result as it leaves the function, after the function's own `try` and `atomic` blocks have closed (falling off the end returns nil, which fails unless the type allows it), and a field on every store, from any code. A typed field's default must have its type. A typed field without a default starts as nil until it is set, and only stores are checked. When hot reload changes a field's type, values already stored are not checked again.
- **Why at run time:** with hot reload and many edits landing at once, a static guarantee would mean rechecking every caller whenever any function changed. Checking at the boundary keeps each change local. A caller compiled against an older version, a line typed at the REPL, or untyped code gets a clean error that names the boundary, rather than a wrong value that travels on.
- **What the compiler uses:** a `num` parameter is known to be a number throughout the function body (until it is assigned), so arithmetic on it skips type checks. A direct call to a function declared `: num` gives a known number too. Changing a function's result type recompiles its callers on reload. A `num` check is three instructions. Other types call the runtime. A store to a typed field always takes the inline cache's slow path, so it costs about 4 ns more than an untyped store (7 ns in wasm).
- **Overrides** declare exactly the parameter and result types of the method they replace. Code holding a base-class instance relies on those types.
- **Interfaces:** types are part of a `.api` file, so changing `n: num` to `n: str` on a `pub` function is an interface change that `fut test` reports.
- `num`, `str` and the other type names are ordinary names everywhere else. Locals and top-level vars can't have types yet.

## Effects

A function can declare what it may do outside itself, after its result type. Like types, the declaration is optional and is checked at run time, where it is written.

```
fn save(p: Player): bool ! io { ... }     // may print, write files, run programs, call C
fn tick() ! clock, random { ... }
fn score(hits: list): num ! { ... }       // `!` alone: performs nothing
fn helper(x) { ... }                      // no `!`: unchecked, as before
```

```
fn report() ! clock { save(p) }   // -> save() declares io, but it is called from inside report(), which does not (it declares ! clock)
fn total() ! { print(n) }         // -> print() performs io, which total() does not declare (it declares no effects)
```

- **Effects:** `io` (print, `run()`, `extern` calls, the window and its input), `clock` (`clock`, `sleep`), `random` (`random()`, a number in [0, 1)), `yield` (a fiber's `yield`), and effects a program declares (below). Builtins perform theirs. Throwing is not an effect.
- **How it is checked:** while a function that declares effects runs, only those are allowed. Entering it checks that the code calling it is allowed all of them, so a declared function can't be reached from one that declares less. Performing an operation checks it at the place it happens. Functions that declare nothing run under whatever their caller allows, so `helper()` above may print when called from `main` but not when called from `total`. What is allowed comes back when the function returns or a throw leaves it, and each fiber keeps its own.
- **Why:** when many people or agents change code at once, a signature should say what a function may touch. `fn score(hits: list): num !` promises that changing `score` won't start writing files, and the promise is kept by the program, not by review. As with types, checking at run time keeps every change local: a reload, or a caller compiled against an older version, gets a clean error naming the function and the effect, instead of a static check that would have to revisit every caller.

### Handlers

`handle` runs a block and answers the operations it names, in place:

```
var out = []
handle {
  save(p)                            // its print goes to out
  print(clock())                     // and so does this: 1000
} with print(s) { out.push(s) } with clock() { return 1000 }
```

While the block runs, an operation it names calls the nearest handler for it on the current stack, as an ordinary function call. The handler's return value is the operation's result, and the code that performed it carries on. A handler never resumes anything later, or twice. One that throws ends the operation like any throw. Tests use handlers to replace the outside world (output, time, randomness, an effect of the program's own) for just the code they run, with no global mocks to set up and take down.

- **Which operations:** `print`, `clock`, `sleep`, `random`, `run`, `yield`, the graphics builtins, and the operations of declared effects. A handler's parameters are the operation's. A handler is a closure, so it captures the variables around it by value: collect into a list or map.
- **Where a handler runs:** with the effects and handlers that were in force where it was installed. A handler for `print` can itself `print`, which reaches the real output or a handler further out.
- **Handlers and declarations.** A handled operation doesn't count against a declaration in the same function: `fn quiet() ! { handle { print("x") } with print(s) { ... } }` is allowed, since nothing leaves `quiet`. Code inside a function that declares the effect is reached by a handler installed outside it, which is how a test can fake `save`'s output. Code inside a function that does not declare the effect is not: a handler can't make `total()` above print. A `handle` that answers every operation of an effect also allows that effect inside its block, so `fn run() ! { handle { work() } with Log.log(m) { } }` may call `fn work() ! Log`. Partly answering an effect doesn't allow it, and `io` can't be answered whole, since C calls can't be handled.
- **Fibers:** each fiber has its own handlers, and starts with the effects allowed where it was created.
- `return`, `break` and `continue` leave a `handle` block cleanly, and a throw removes the handlers it unwinds past.

### Declared effects

```
pub effect Log {
  fn log(msg: str)
  fn level(): num
}
fn work() ! Log { if Log.level() > 1 { Log.log("detail") } }
handle { work() } with Log.log(m) { lines.push(m) } with Log.level() { return 2 }
```

An `effect` declares operations without bodies, at the top level (`pub` makes it usable from other files: `audit.Audit.record(x)`). Code performs one by calling it through the effect's name, `Log.log("x")`, and the nearest handler answers. With no handler, performing it is an error. The effect's name, written at every place it is performed, makes the code easy to search, reads as the call it is, and tells the compiler the arity, which it checks. A keyword such as `perform log("x")` would add a reserved word and hide which effect is meant. Operations may have types, checked when one is performed and when its handler returns.

- **Atomic blocks:** performing `io` inside `atomic { }` is an error, because printed output or a written file can't be rolled back. A function that declares `io` can't be called there. A handler installed inside the block may answer an `io` operation, since what it does (such as pushing to a list) is rolled back with the block. Other effects are allowed.
- **Overrides** declare a subset of the effects of the method they replace, often the same set. Doing less breaks no caller. If the replaced method declares effects, the override must declare some too, since an unchecked override could do anything.
- **Interfaces:** effects are part of a `.api` file (`pub fn save(p): bool ! io`), so adding `io` to a `pub` function is an interface change that `fut test` reports. `pub effect` declarations are listed with their operations.
- **Hot reload:** changing a function's declared effects takes effect at its next call, from any caller, like a changed result type. Its callers are not recompiled, since the check is in the function itself. Frames already running keep their old declaration until they return.
- **Cost:** none for functions that declare nothing: they contain no extra code. A declaring function checks and narrows the allowed set on entry with about a dozen instructions and no call, and puts it back in its epilogue with four. 5M calls of `fn f(x) ! { return x + 1 }` take 11 ms against 8 ms undeclared (wasm: 20 vs 18). The set is a bitmask in the VM block (44 effects, the rest of the word counts nested narrowings). A builtin that performs an effect is checked when it is called, inside the runtime call that already resolves it. Entering a `try` saves three more words. A handled operation is a list walk and a call through the runtime.
- `effect`, `handle` and `with` are special only where they start a declaration or a block, so they stay usable as names. `io`, `clock`, `random` and `yield` are effect names only after `!`.

## Errors

```
fn load(path) { if path == "" { throw "no path" } return path }
try { load("") } catch e { print("failed: {e}") }    // -> failed: no path
try { nil.x } catch e { print(e) }                   // runtime errors are caught too, as their message
try { throw {code: 42} } catch e { print(e.code) }   // any value can be thrown
try { risky() } catch { }                            // the name is optional
```

`throw` unwinds to the innermost `try` on the current stack, through any number of calls. `return`, `break` and `continue` leave a `try` cleanly. An uncaught error inside a fiber ends the fiber and is thrown again in its resumer, where it can be caught. An uncaught error stops the program with a stack trace (under `-w`, it waits for a fix). Entering a `try` costs a few stores: the handler record lives in the function's frame, and a function containing `try` keeps its locals in memory so a catch sees their latest values.

## Atomic blocks

```
fn transfer(from, to, n) {
  atomic {
    from.balance -= n
    to.balance += n
    if from.balance < 0 { throw "insufficient funds" }   // neither balance changes
  }
}
```

If an error leaves an `atomic` block, everything the block changed that other code can see is put back before the error continues. That covers instance fields, lists, maps, globals and closures' captured variables. The error can be a `throw` or a runtime error, from any depth of calls. Whatever catches it then sees the state as it was before the block. If the block finishes, or leaves by `return`, `break` or `continue`, its changes stay. Blocks nest: an inner block that fails rolls back only its own changes, and an outer block that fails also rolls back what its inner blocks did.

- **Nothing to declare.** Any code can run inside a block, including calls into code that knows nothing about it. The runtime saves each object just before the block first writes to it.
- **Not rolled back:** local variables of the running functions, which no other code can see. The outside world (printed output, files, C calls) can't be rolled back either, so performing `io` inside a block is an error unless a handler installed inside the block answers it (see [Effects](#effects)).
- **Fibers:** a fiber may not `yield` inside its own atomic block, because other code would then see the block half done. A `yield` written directly in the block is a compile error; one reached through a call is a runtime error. A fiber resumed from inside a block may yield, and what it changed belongs to that block.
- **Cost:** a block copies the globals when it starts, and each object the first time it writes to it. Field stores inside a block skip their inline caches. Hot reloads and typed lines wait until open blocks finish.

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
- **Interfaces:** `fut test` also fails when a file's `.api` file is missing or out of date (see [Modules](#modules-and-visibility)).
- **Statuses:** `ok`, `FAIL` (an `expect` failed), `ERROR` (an uncaught runtime error, with its trace) and `pending`. Output that a failing test printed is shown under it. The exit status is 0 only when every test passed: 1 means something failed, errored or is pending, and 2 means a compile error.
- **`--json`** prints one object per line: `{"type":"test", "name", "file", "line", "status", "ms", "output"}`. A failed test adds `"expect": {"file", "line", "expr", "left": {"expr", "value"}, "right": {...}, "diff": {"at", "what"}, "reason"}`. An error adds `"message"` and `"trace"`, and a pending test adds `"reason"`. A final `{"type":"summary", "passed", "failed", "errors", "pending", "ms"}` follows. A compile error is `{"type":"compile_error", "file", "line", "col", "message", "text"}`, and an error in top-level code is `{"type":"error", "message", "trace"}`.
- **`--filter text`** runs only the tests whose name contains `text`.
- **Isolation:** natively, each test runs in its own process, forked after the top-level code has run. A test's changes to globals, a crash or a hang stay inside that test. A test that runs longer than `--timeout` seconds (default 10) is stopped and reported as an error. Under WebAssembly, tests share one process and run in order with no timeout, so tests shouldn't depend on each other's state.
- `test`, `expect`, `throws` and `prints` are reserved only in these positions, so they stay usable as ordinary names.
- **This repository's tests:** `tests/specs/*.fut` are specs, run with `fut test`; the programs they `run()` are in `tests/specs/progs/`. A few files in `tests/*.fut` still use `// expect:` comments that the runner compares with stdout: `basics`, `control`, `cond`, `classes`, `err_compile` and `expect`. They test, without using `expect`, the features `expect` itself depends on.

## Modules and visibility

A top-level name in an imported file is private to that file unless it is marked `pub`:

```
// lib/limits.fut
fn clamp(x, lo, hi) { ... }          // private: only this file can use it
pub fn limit(x) { return clamp(x, 0, 10) }
pub class Range { ... }              // a pub class's fields and methods are public
pub var calls = 0
```

`import "path.fut"` makes the file's `pub` names global in the importing program. `import "path.fut" as name` keeps them in a namespace instead: `name.limit(3)`, `name.Range()`, `class C : name.Range`. Either way, a private name used from another file is a compile error that says how to fix it (`'clamp' is private to lib/limits.fut (mark it pub there to use it here)`), and two files may use the same private name. The file you run needs no `pub`. A module's `main` is not run. A wrong `name.x` is a compile error. Using `name` alone as a value is also an error, since a module is not a value. Hot reload patches module functions like any other (`patched geom.area`).

Only the `pub` names of a file are what other files can depend on. Everything else can be renamed, given new parameters or deleted without breaking anyone, which matters when many people or agents change a codebase at once.

### Interface files

`fut api main.fut` writes a `.api` file next to each compiled file that has `pub` declarations. It lists only the signatures, sorted by name, each with the `//` comments right above it, and includes a class's fields and methods:

```
// The public interface of geom.fut, written by `fut api`. `fut test` fails while it is out of date.

pub class Vec {
  var x = 0
  fn add(o)
  fn init(x, y)
}

// Where every shape starts.
pub var origin
```

`fut test` checks these files. When a file's public interface no longer matches its `.api` file, the run fails and shows what changed:

```
API     lib/geom.fut: its public interface changed; `fut api main.fut` updates geom.api
  - Vec: fn add(o)
  + Vec: fn add(o, scale)
```

With `--json` this is `{"type":"api", "file", "interface", "status": "missing" | "changed" | "stale", "removed": [...], "added": [...]}`. So a change to what other files depend on never happens by accident: it appears as its own diff, in a small file that is easy to review. Two changes to the same interface conflict there, at merge time, rather than at run time. Edits to private code and to function bodies don't touch the `.api` file. A reader, human or agent, can learn a file's contract from its `.api` without opening the source. `fut api --check main.fut` only reports.

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

An error inside a fiber ends it and is raised again in its resumer. Each fiber has its own effect handlers and allowed effects (see [Effects](#effects)). Switching costs about as much as a function call: callee-saved registers are saved on one stack, and the stack pointer moves to the other. Hot reload sees suspended fibers' frames, so code they will return into is kept.

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
- **A failed reload is undone.** The new top level (initializers of new and edited vars) and `reload()` run as one transaction, as if inside `atomic`. If either throws, the data they changed is rolled back. The previous code is then compiled again from the sources it was built from, without being run. The program carries on as though the file had not been saved, after printing the error and `[fut] reload rolled back`. The next save tries again.
- **State survives.** Globals keep their values. A `var`'s initializer runs again only if the var is new or its declaration line was edited, so changing `var gravity = 0.15` to `0.3` takes effect at once while untouched state is kept. Class method tables are rebuilt in place, so existing instances see new, changed and removed methods. After each patch, `reload()` runs if it is defined.
- **Running frames.** Frames already running finish in their old code. If `main` itself changed, it is unwound and restarted with globals intact.
- **Changed parameter counts.** The function gets a new stub, and the old stub is pointed at a trap. Old code that is still running and calls the function gets a clean "stale call" error rather than reading garbage arguments.
- **Memory reclamation.** Replaced bodies are freed once no frame's return address points into them, found by walking the frame-pointer chain. Each reload prints the live code size.

## WebAssembly

`python3 build.py wasm` compiles the compiler and runtime themselves to `build/fut.wasm` (Homebrew LLVM: `brew install llvm lld wasi-libc wasi-runtimes`). Node 25+ (tested on 26) or Chrome 137+ is needed for JSPI and wasm exceptions. Hosts:

- `node web/run.mjs [-w | -v] file.fut`: the same CLI as `build/fut`, including `-w` hot reload and the live REPL on stdin.
- `node web/serve.mjs [port]`, then open `http://localhost:8000/?file=path/to/file.fut`: a canvas for `window()`/`present()`, keys and mouse, a REPL box, and hot reload of the file and its imports whenever they are saved.

How it maps:
- **Code.** Each compile becomes one wasm module, with one function per fut function. It shares the runtime's memory, function table and stack pointer. A stub is a table slot, so patching it is a single `table.set`. Retired slots are cleared and reused; V8 frees a module once nothing references it.
- **Calls.** A fut function is a wasm function `(window, sp, a0..a3) -> value`: the caller's argument window (in memory, where the GC sees it), the stack pointer, and the first four arguments again as wasm parameters. Parameters stay in the caller's window, so a leaf that needs no slots of its own has no frame. The stack pointer is a parameter and a local, not the C++ `__stack_pointer` global: it is written to the global only on the way into C++ (helpers; natives go through a generated adapter). A call to a known top-level function whose body is in the same module is a direct `call`; otherwise it is a `call_indirect` through its stub. A body has a "replaced" byte that wasm.cpp sets once its stub leads elsewhere (reload, tier 2, a changed arity); a body that finds it set on entry passes the call on through the stub with a tail call, which is how a direct caller reaches new code without being patched.
- **Values and frames.** Values are NaN-boxed exactly as on arm64. Each local's home is a slot in a frame on the C++ shadow stack, which the GC scans. A wasm local caches it, and reads use that cache. Stores write both, except when the value is known to be a number (range counters, `x op= y` on a local that started as a number): those stay in an f64 wasm local, so a loop-carried `s += x` never leaves the FP registers. Temporaries live only in wasm locals, so a collection runs only at safe points (function entry and loop back-edges). On entry the safe point shares the stack check: the VM block keeps the stack limit and the poll flag side by side, so one 64-bit load and compare catches both.
- **Branch hints.** Type checks, inline-cache hits and safe-point polls carry wasm branch hints (likely / unlikely). Without them V8 treats both arms of an `if` alike, and the helper call on the slow path makes it spill the loop's values around the fast path too. The hints alone take a 20M-iteration float loop from 67 ms to 17 ms.
- **Tier 2** works as on arm64. A failed entry guard continues in the tier-1 body with a tail call (`return_call_indirect`), which leaves no extra frame. Its deopt tail call marks the window pointer (low bit) so that the tier-1 body, whose replaced byte is set, runs instead of passing the call back. Direct calls run faster than calls through the table, and typed function references (`call_ref` through a global) measured no faster than `call_indirect`.
- **Inline allocation** works as on arm64. `C(args)` and closures bump their size class's run in the VM block, with no call into the runtime.
- **Code size.** V8 inlines a wasm callee only while the caller stays within about 1.1× its size (600 bytes below that), so the emitter keeps bodies small. Tag tests use shifts with one- or two-byte immediates instead of 10-byte NaN-box constants. Helper arguments that are constants go straight onto the stack. `local.set x; local.get x` becomes `local.tee x`. Known-number range bounds skip the runtime check and the frame store.
- **try/throw** use wasm exceptions. Uncaught errors print a stack trace mapped from V8's module offsets to fut lines.
- **Fibers** each get their own shadow stack. The host suspends and resumes their wasm stacks with JSPI (`WebAssembly.Suspending` / `promising`).
- **`extern fn`** calls JavaScript instead of C. With no library it looks in a small libc shim (`strlen`, `atoi`, `getenv`, ...), then `Math`, then globals. `extern "console" fn log(str): void` names an object.

Performance, as best of 5 runs after warm-up (wasm vs arm64): float loop 18 vs 18 ms, `fib(32)` 16 vs 12 ms, 5M method calls 20 vs 15 ms, 1M allocations 6.8 vs 4.4 ms. The first call of a function runs in V8's baseline compiler, and V8 has no on-stack replacement for wasm, so a long loop in a function called only once (the first run of the float loop: 77 ms) stays at baseline speed. Functions called repeatedly, such as the ones a game loop calls each frame, are optimized after a few calls. In Node, `node --no-liftoff web/run.mjs` optimizes everything up front, at about 200 ms of extra startup.

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
| `main.cpp` | Driver: watcher, safepoint reload (as a transaction), `main` restart, REPL, `--asm`, `fut test`, `fut api` |
| `txn.cpp` | Transactions (`atomic`, reloads): an object is copied before its first write, globals at the start; rollback, nesting |
| `types.cpp` | Optional types: what each check names, the check compiled code calls, and the one field stores make |
| `effects.cpp` | Effects: declared sets and their checks (entry, perform site), operations and their ids, handler records, `handle`, `random()` |
| `api.cpp` | Public interfaces: a file's `pub` declarations as `.api` text, the check `fut test` runs, `fut api` |
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
- Instances keep their fields inline, sized to the class's field count at construction. Their header holds the class id and slot count, so a field or method inline cache hit is one 64-bit (or 32-bit) compare and a load. The caches are cleared on every reload, and field-store caches also when a transaction begins (they stay empty until it ends, so each store saves its instance first).
- `C(args)` for a top-level class allocates inline. The call site caches the class and its `init`. On a hit, it bumps the free run of the instance's size class, writes the header and fills the slots, then calls `init`, with no call into the runtime. Closures are allocated the same way.

**Memory:**
- Small objects are rounded up to one of 17 size classes, from 16 B to 16 KB, and each 64 KB page holds one class. Bigger objects get pages of their own.
- Mark bits live in a side bitmap, one bit per 16 bytes. When a collection ends, the bitmap becomes the map of live objects. There is no sweep: allocation bumps through the runs of free cells between live objects, so a collection never touches a dead object. A page with nothing live goes back to a pool.
- Lists, maps, classes, fibers, and instances whose fields moved out of line own `malloc`'d memory. They are registered when created and finalized when found dead.
- Roots are the globals, constants, and every stack (the current one from its real `sp`, plus each suspended fiber's) and registers, scanned conservatively. Heap objects are traced precisely.
- A collection runs when the heap would grow past twice the pages in use after the last one (minimum 8 MB). Free pages beyond that are returned to the OS with `madvise`.
