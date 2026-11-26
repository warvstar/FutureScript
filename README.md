# fut

A small class-based language written in C++23 with no third-party libraries. A single-pass compiler turns source straight into arm64 machine code, with no AST and no bytecode. With `-w`, the running process is **hot-reloaded by patching its own machine code in place**; it never loads a DLL or dylib. The same compiler can instead emit **WebAssembly**, running in Node or a browser and hot-reloading by swapping function-table entries (see [WebAssembly](#webassembly)).

```sh
python3 build.py               # -> build/fut   (clang++ -std=c++23 -O2, files in parallel, only what changed; no cmake)
python3 build.py test          # language tests, tests/specs/*.fut via `fut test`, live hot-reload scenarios
python3 build.py test contracts # only the tests whose name contains a word given (all run in parallel, ~7s in all)
python3 build.py test --debug  # same, under ASan/UBSan
python3 build.py fuzz 1000     # mutate the tests; any crash or sanitizer report fails
python3 bench/run.py           # fut (native and wasm) against the same kernels in C (clang -O2) -> bench/results.md
build/fut file.fut             # run
build/fut test file.fut        # run its `test` blocks (--json: one JSON object per line; --filter text; --timeout s)
build/fut test --changed f.fut # rerun only the tests an edit can affect; the rest are cached passes
build/fut -w file.fut          # run and hot-reload on save (file and its imports); type lines to run them inside it
build/fut api file.fut         # write each imported file's .api (its pub declarations); `fut test` checks them
build/fut map file.fut         # every package, file and declaration the program uses, with line ranges
build/fut explain [code]       # an error code's rule and fix ([index], [requires], ...; no code: all of them)
build/fut --asm file.fut       # disassemble the generated code (uses clang + otool)
build/fut                      # REPL
python3 build.py wasm          # -> build/fut.wasm (compiler + runtime for WebAssembly)
python3 build.py test --wasm   # the same tests, run under Node
node web/run.mjs [-w] file.fut # run on WebAssembly in Node (same flags; no file: REPL)
node web/serve.mjs             # browser: http://localhost:8000/?file=examples/paddle.fut
```

Requires macOS on Apple silicon (arm64, `MAP_JIT`).

Agents: [docs/spec.md](docs/spec.md) is the whole language in about 6 KB; every example in it runs as a test.

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

- **Values:** numbers (f64; whole numbers are exact up to 2^53), strings, `true`/`false`/`nil`, lists, maps, functions and closures, classes and instances. `nil` and `false` are the only falsy values.
- **Operators:** `+ - * / %`, `== != < <= > >=`, `&& ||` (short-circuit, return the operand), `!`, `= += -= *= /=`. Bitwise `& | ^ ~ << >>` work on whole numbers as 64-bit integers (`1.5 & 1` is an error), with Python's precedence: `2 + 3 & 6` is `(2 + 3) & 6`. `+` with a string on either side concatenates.
- **Builtins:** `print len str int clock sleep random floor sqrt fiber run channel cores` (`int` truncates toward zero; `random()` is in [0, 1)), plus graphics (below).
- **Value rendering:** `print(a, ...)` joins one or more values with single spaces and adds a newline. A `with print(s)` handler receives the joined string before the newline is added. `str(v)` and string interpolation use the same rendering. Standalone strings are raw. Inside lists, maps and instances, strings are quoted and escaped. Maps use bare identifier keys where possible, as in `{a: 1, "b c": "s", n: nil}`. Instances show their stored fields in layout order, as in `P{x: 1, y: [2]}`. Cyclic references display as `<cycle>`. Finite numbers round-trip to the same f64. Nonzero magnitudes below 1e-6 or at least 1e21 use exponent notation.
- **Host builtins** (`io`; native build except `time`, `json`, `parseJson`, `fetch` and `fetchAll`, which wasm has too, through the host's `fetch()`; `time` is `clock`, and JSON has no effect): `fetch(url)` or `fetch({url, method, headers, body, timeout})` returns `{status, body, headers}` (header names lower-case), or `{status: 0, error}` when nothing came back, without throwing. `fetchAll(reqs)` runs the requests at once and returns their results in order. `exec([argv...])` or `exec({cmd, cwd, input, timeout})` runs a process and returns `{code, out, err, timedOut}`; `execAll(cmds)` runs several at once. `readFile(path)` returns the text or `nil`; `writeFile(path, text)` makes the directories it needs. `env(name)` returns a variable or `nil`; `time()` is seconds since 1970. `json(v)` and `parseJson(text)` convert data as for JavaScript. HTTP uses the system libcurl.
- **Numbers:** `12`, `1.5`, `2e3`, `0xff8800`.
- **Keywords:** `var fn class if else while for in return break continue true false nil self super import extern yield try catch throw`. `as`, `pub`, `override`, `atomic`, `effect`, `handle`, `with` and `spawn` are special only where they start an import alias, declaration or block, so they stay usable as names.
- **Methods:** `list.push/pop/len/slice/join`, `map.has/remove/keys/values/len`, `str.len/has/find/slice/split/trim`, `fiber.resume/send/done`, `channel.send/recv/tryRecv/close`, `task.join/done`. `has` asks whether a string contains a part, and `find` gives its index or -1. `slice(from, to)` excludes `to` and clamps both to the length. `split(sep)` keeps empty parts. `join(sep)` writes strings as they are and other values as `str` does. `parseNum(s)` gives the number a string spells, or nil. Indexes count bytes, as `len` and `s[i]` do.
- **Closures:** a closure captures the enclosing variables by value. The closure can still change its own copy, so counters work, but it does not see later changes in the enclosing function.
- **Interpolation:** expressions inside `{...}` in a string can't contain `"`. Write `\{` for a literal brace.
- **Newline rules:** a `(` or `[` on a new line, or a leading `-`, starts a new statement rather than continuing the previous one.
- **Not in fut** (for those coming from other languages): negative indexes (use `l[l.len() - 1]` for the last element), varargs (pass a list), default arguments (pass an options map), and chained comparisons (write `a < b && b < c`). For sorting, `import "std/lists"` and call `lists.sort(l)`. `int` takes only numbers.

### Interpolation formats

Use `"{expr:spec}"` to format a value. A spec can contain these parts in order.

- Set a minimum width with `:10`. Numbers align right and other values align left. Use `<`, `>` or `^` before the width to align left, right or center. Put a printable ASCII fill character before the alignment marker, as in `:*>10`. Braces are reserved.
- Use `+` to show a number's sign and `,` after the width to group thousands. With default alignment, a leading zero in the width (such as `:04`) pads a number with zeros after its sign.
- Use `:.2` or `:.2f` for two fixed decimal places on numbers. Bare `.N` on a string keeps its first N bytes. Use `e` for scientific notation. Explicit `f` and `e` default to six decimal places.
- Use `x`, `X`, `b` or `o` for finite whole numbers in hexadecimal, uppercase hexadecimal, binary or octal. These forms accept width and sign options.

For example, `"{3.14159:.2}|{7:>3}|{255:x}"` gives `"3.14|  7|ff"`. Write `"{x=}"` to include the original expression text, giving `"x = 7"` for `x = 7`. A format can follow the debug marker, as in `"{x=:.2}"`. Width is limited to 65,535 bytes and precision to 1,000. Invalid specs raise a `syntax` error at compile time. A spec that cannot format its value raises an `argument` error at run time.

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

Types are optional. Code without them is as dynamic as ever. Where one is written, on a parameter, a result, a declared field or a var, it is checked at run time at that boundary.

```
pub class Account {
  var balance: num = 0
  var note: str? = nil               // ?: nil is allowed too
  fn withdraw(n: num): num { self.balance -= n return self.balance }
}
pub fn transfer(from: Account, to: Account, n: num) { from.withdraw(n) to.deposit(n) }
fn find(items: list, f: fn): any { }
var level: num = 1                   // a top-level var or a local
type Method = "GET" | "POST"
type User = {name: str, age: num, email: str?}
fn send(m: Method, to: list<User>, retries: map<str, num>) { }
```

```
transfer(a, "bob", 5)  // -> argument 'to' of transfer() must be Account, got the string "bob"
a.balance = nil        // -> field 'balance' of Account must be num, got nil
level = "two"          // -> var 'level' must be num, got the string "two"
```

- **Types:** `num str bool list map fn any`, and class names, including a module's (`geom.Shape`). A class type accepts instances of its subclasses. `T?` also accepts nil. `any` documents a value that can be anything, nil included.
- **Compound types:** `A | B` (either), literals (`"GET"`, `3`, `true`, `nil`), shapes `{name: str, age: num?}`, `list<T>`, `map<V>` and `map<K, V>`, nested as deep as needed. `type Name = <type>` names one (`pub type` for other files; it can refer to itself, as in `type Tree = {value: num, kids: list<Tree>}`). A name used as a type can't be used as a value.
- **Shapes are structural and open:** a shape accepts a map or an instance that has each listed field with its type, and may have more. A method satisfies a `fn` field, so `{name: str, greet: fn}` accepts a class instance with a `greet` method. A field typed `T?` may be missing.
- **Failures name the path:** `argument 't' of depth() must be Tree: .kids[0].value must be num, got the string "x"`. A missing field names a near spelling if the map has one (`.age is missing (it must be num); there is a 'agee': a misspelling?`).
- **Field names are checked as the code compiles:** on a variable declared with a shape or class type, `u.emial` is a compile error (`'emial' is not a field of User (u is User: name, age, email); did you mean 'email'?`). A shape also allows the map methods (`has remove keys values len`).
- **Lists and maps keep their element type:** the first `list<num>` check walks the list. Once it passes, the list keeps that type: later `list<num>` checks of it take constant time, and every store into it (`push`, `l[i] = v`) is checked, so it can't quietly stop being `list<num>`. A store that breaks it names where the list got its type: `cannot store the string "x" in this list: it has been list<num> since it passed as argument 'ids' of send()`. A checked push costs about 13 ns, against 6 ns unchecked. A list checked against a union (`list<num> | list<str>`) or a failed check keeps no type. Copies (`slice`) start untyped. Shapes are checked each time and aren't remembered. Use a class when a record's fields must stay checked.
- **No type parameters:** there is no `fn first<T>(l: list<T>): T`. A checker that ties a result to an argument has to see every call site, which hot reload and run-time checks don't do.
- **When they are checked:** an argument when the function is entered, a result as it leaves the function, after the function's own `try` and `atomic` blocks have closed (falling off the end returns nil, which fails unless the type allows it), a field on every store, from any code, and a var on every store too: from closures (which keep the type on their copy), from functions above its declaration, from other modules and from the REPL. A typed parameter is checked again when the function assigns to it. A typed var needs an initial value unless its type allows nil. A typed field's default must have its type. A typed field without a default starts as nil until it is set, and only stores are checked. When hot reload changes a field's type, values already stored are not checked again.
- **Why at run time:** with hot reload and many edits landing at once, a static guarantee would mean rechecking every caller whenever any function changed. Checking at the boundary keeps each change local. A caller compiled against an older version, a line typed at the REPL, or untyped code gets a clean error that names the boundary, rather than a wrong value that travels on.
- **What the compiler uses:** a `num` parameter is known to be a number throughout the function body (until it is assigned), so arithmetic on it skips type checks. So is a `num` var, which every store checks, and a `num` local can live in a floating-point register. A direct call to a function declared `: num` gives a known number too. Changing a function's result type recompiles its callers on reload, and changing a top-level var's type recompiles the code that stores to it (and resets the var, like any edit to its declaration). A `num` check is three instructions. Other types call the runtime. A store to a `num` field stays on the inline cache's fast path, which checks the value inline (three more instructions), so it costs the same as an untyped store. Stores to fields of other types take the slow path, about 4 ns more (7 ns in wasm).
- **Overrides** declare exactly the parameter and result types of the method they replace. Code holding a base-class instance relies on those types.
- **Interfaces:** types are part of a `.api` file, so changing `n: num` to `n: str` on a `pub` function is an interface change that `fut test` reports.
- `num`, `str` and the other type names are ordinary names everywhere else.

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

- **Effects:** `io` (print, `run()`, the host builtins, `extern` calls, the window and its input), `clock` (`clock`, `sleep`, `time`), `random` (`random()`, a number in [0, 1)), `yield` (a fiber's `yield`), `spawn` (starting an isolate), `chan` (every operation on a channel or a task: `send`, `recv`, `join`, ...), and effects a program declares (below). Builtins perform theirs. Throwing is not an effect.
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

- **Which operations:** `print`, `clock`, `sleep`, `time`, `random`, `run`, the host builtins, `yield`, the graphics builtins, and the operations of declared effects. A handler's parameters are the operation's. A handler is a closure, so it captures the variables around it by value: collect into a list or map.
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
- `effect`, `handle` and `with` are special only where they start a declaration or a block, so they stay usable as names. `io`, `clock`, `random`, `yield`, `spawn` and `chan` are effect names only after `!`.

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
- **`expect allocs { ... } <= 2000`** compares how many objects the block allocated: lists, maps, strings, instances, closures and fibers, one each however big. Numbers aren't objects. The count is exact and the same natively and in wasm, so it makes a portable performance check. Counting costs nothing: the allocator's bump pointers already record it. Objects a contract check makes (its `old()` values, a clause's calls) aren't counted, so adding a contract doesn't change a count.
- **`expect grows { n -> parse(lines(n)) } like "n"`** checks how the block's running time grows with `n`. It runs the block for doubling sizes: from the `n` at which one call takes 50 µs, five sizes, stopping early at 0.25 s per call or 3 s in all. Each time is the best of 3 batches of 2 ms or more. The check fits the times on a log-log scale and fails if, divided by the growth, they still grow by more than a tolerance. **`expect grows allocs { ... }`** fits allocation counts instead, for `n` = 64..2048, with a tight tolerance. A growth is `"1"`, `"n"`, `"n^2"` (any `n^k`), optionally followed by `log n` (`"log n"`, `"n log n"`). It is an upper bound: code that grows more slowly passes. A failure names the growth it measured and lists the measurements (`it grows like n^2, not n (n = 256: 0.081 ms, 512: 0.304 ms, ...)`). The tolerance is wide enough to tell `n` from `n^2` reliably, but not `n` from `n log n`.
- **`run(file)`** or **`run({source: "...", args: [...], timeout: s})`** runs fut on a program in a child process and returns `{code, out, err, timedOut}`. Use it to check what can't be checked in-process: uncaught errors and their stack traces, compile errors, exit codes, and whether `main` runs. A relative `file` is found next to the program, and `args` go before the file (`args: ["test", "--json"]`). In `source`, write `\{` for a literal brace, because `{...}` interpolates.
- A failed `expect` throws an error. Outside `fut test` it works as an assertion, with the usual stack trace. A test whose `expect` fails is reported as failed even if its own `try` catches that error.
- **Pending tests support writing tests first.** A test that names a function, class or module member that isn't defined yet is reported as pending instead of failing the whole compile. Any other compile error is still an error.
- **Interfaces:** `fut test` also fails when a file's `.api` file is missing or out of date (see [Modules](#modules-and-visibility)).
- **Statuses:** `ok`, `FAIL` (an `expect` failed), `ERROR` (an uncaught runtime error, with its trace) and `pending`. Output that a failing test printed is shown under it. The exit status is 0 only when every test passed: 1 means something failed, errored or is pending, and 2 means a compile error.
- **`--json`** prints one object per line: `{"type":"test", "name", "file", "line", "status", "ms", "output"}`. A failed test adds `"expect": {"file", "line", "expr", "left": {"expr", "value"}, "right": {...}, "diff": {"at", "what"}, "reason"}`. An error adds `"message"` and `"trace"`, and a pending test adds `"reason"`. A final `{"type":"summary", "passed", "failed", "errors", "pending", "ms"}` follows. A compile error is `{"type":"compile_error", "file", "line", "col", "message", "text"}`, and an error in top-level code is `{"type":"error", "message", "trace"}`.
- **`--filter text`** runs only the tests whose name contains `text`.
- **`--changed`** runs only the tests an edit can have affected and reports the rest as cached passes, so a test run costs what the change touches, not what the project holds. Each `fut test` records, per test, what the test actually used: the functions and methods it entered (a lambda counts as the function it is written in), each method lookup it made (class and name, and which method that found, so a new override in a subclass reruns the tests that called the inherited method on it), the top-level vars and classes its code names, and each file's other top-level code (imports, statements, `extern` and `effect` declarations). Each item has a hash of its tokens (comments and spacing don't count), and of the declarations of every name in them, so a new declaration that a name now resolves to counts too. A test is cached when it passed, every hash still matches and nothing it ran did io, read the clock or drew a random number (`print` is fine: a test's output is captured), and it was recorded by the same `fut` binary. Tests that use `run()`, `extern`s, `clock()`, `random()` or timed `expect grows` always run, as do failed and pending ones. A top-level change that any test may observe reruns them all. Output is one line and then what failed: `2 run, 340 cached, 1 failed (40.1 ms)`; with `--json`, only failed and pending tests are listed, and the summary adds `"run"` and `"cached"`. If every test is cached, not even the top-level code runs. The cache is `.fut-cache/<file>.tests` next to the program (ignore it in version control). Under WebAssembly, `--changed` runs every test.
- **Isolation:** natively, each test runs in its own process, forked after the top-level code has run. A test's changes to globals, a crash or a hang stay inside that test. A test that runs longer than `--timeout` seconds (default 10) is stopped and reported as an error. Under WebAssembly, tests share one process and run in order with no timeout, so tests shouldn't depend on each other's state.
- `test`, `expect`, `throws`, `prints`, `allocs`, `grows` and `like` are reserved only in these positions, so they stay usable as ordinary names.
- **This repository's tests:** `tests/specs/*.fut` are specs, run with `fut test`; the programs they `run()` are in `tests/specs/progs/`. A few files in `tests/*.fut` still use `// expect:` comments that the runner compares with stdout: `basics`, `control`, `cond`, `classes`, `err_compile` and `expect`. They test, without using `expect`, the features `expect` itself depends on.

## Contracts

```
class Account {
  var balance = 0
  invariant self.balance >= 0

  fn init(balance: num)
    requires balance >= 0
  {
    self.balance = balance
  }
  fn withdraw(n: num): num !
    requires n > 0
    requires n <= self.balance
    ensures self.balance == old(self.balance) - n
    ensures result == self.balance
  {
    self.balance -= n
    return self.balance
  }
}
```

```
$ build/fut bank.fut
runtime error: the caller broke Account.withdraw()'s contract: requires n <= self.balance (n is 50, self.balance is 10)
  at Account.withdraw (bank.fut:10)
  at <top> (bank.fut:21)
```

A function's contract sits between its signature and its body, one clause per line, so whoever edits the body sees what it must keep. A class's invariants sit in its body. Contracts are checked on every call, like types, in normal runs and in `fut test`; they are part of the package's `.api`, and `fut test` also checks each one on generated inputs.

- **`requires cond`** is checked on entry. A failure is the caller's fault, and the error says so.
- **`ensures cond`** is checked when the function returns, by any `return` or by reaching its end. A failure is the implementation's fault. In `ensures`, `result` is the value returned and `old(expr)` is `expr`'s value on entry (only those expressions are computed on entry). Parameters mean what was passed, even if the body assigns them.
- **`invariant cond`** (in a class body) is checked after `init` and whenever an outside call on an instance ends. A method that calls its own object's methods partway through a change doesn't trigger the check in those inner calls, so it may break the invariant temporarily. A call that throws is over: the next outside call checks it. A store to a field from outside the object (`acct.balance = 5`) is checked too. A subclass keeps its superclass's invariants and may add its own, and every method checks all of them, including methods it inherits.
- **A clause is one line:** its condition runs to the end of the line, or to a `{` or `}` on it (`fn f(n) requires n > 0 { ... }` works for a short one). A condition that needs more room goes in brackets. Write one `requires` per condition rather than joining them with `&&`: adding or removing a clause is then a one-line change, and the `.api` reports each one.
- **Failures name the part that failed.** A condition joined with `&&` is checked part by part, and the error shows the failing part's source and the value of each side that isn't a literal. A condition with `||` outside brackets is checked as one. A contract error is an ordinary runtime error, so `try` and `expect throws` see it.
- **Checking changes nothing.** A clause that calls code runs with no effects allowed (it can't print or read the clock), and whatever it changed is put back afterwards, as in an atomic block. Clauses without calls compile to inline comparisons. Helpers a clause calls are ordinary functions.
- **Interfaces:** each `requires`, `ensures` and `invariant` of a `pub` declaration is written into the `.api`, under its declaration, so a package's users learn its contracts without reading its source. Such a contract may use only what those readers can see: the parameters, `self` and its methods, `result`, built-ins and `pub` names. A private helper in one is a compile error (`the contract of split(), a pub function, can only use what its readers can see, and 'sum' is private`). A change to them is an interface change, classified by clause (an edited clause is one removed and one added): adding a `requires`, or removing an `ensures` or an `invariant`, is breaking, so a package's version must move (see [Packages](#packages)); adding an `ensures` or an `invariant`, or removing a `requires`, is compatible. `fut test` lists each one (`+ Stock.reserve() requires n <= self.available(sku)`).
- **Property tests:** `fut test` also tests every contract on generated inputs. A function with a contract whose parameters are all typed `num`, `str`, `bool`, `list` or `map` (optionally `?`) is called with 100 valid inputs; one with an untyped parameter gets no property test. An input that breaks a `requires` doesn't count, and the test fails if fewer than 20 of up to 1000 inputs are valid. A class with invariants or methods with contracts gets a stateful test instead: it builds an instance and calls its methods with contracts (every method, if it has only invariants) up to 20 times with generated arguments, so invariants and `ensures` are checked across sequences of calls. Inputs are the same on every run. A failure is shrunk to a small counterexample, with fewer calls first and then smaller values:

  ```
  FAIL    contract half() on random inputs (half.fut:1)
    half.fut:1: expect half() to keep its contract on random inputs
      half(101)
      half() broke its contract: ensures result * 2 == n (result * 2 is 0, n is 101)
      (random input 1, shrunk)
  ```
- **Contract first:** write a function's signature and contract with **`todo()`** as its body. Calling it throws `f() is not implemented yet`, and under `fut test` it is reported as pending (`pending Account.close() (bank.fut:14): Account.close() is not implemented yet`) and left out of property tests. A stub file of such functions is the skeleton whoever implements it fills in.
- **Cost:** clauses without calls are inline comparisons, and each `old()` takes one frame slot. A method of a class with invariants (its own, a superclass's or a subclass's) also notes whether its call is an outside one, with one runtime call that allocates nothing (about 3 ns: it links a record in the method's frame, which the method unlinks as it returns), and when it is, calls the object's hidden invariant method as it ends. A store to a field of an object other than `self` checks the object's invariants only if its class has any: the store's inline cache marks those classes, so stores to other classes cost one more test, and code elsewhere doesn't depend on whether any class declares an invariant. A function with a contract stays in tier 1, because tier 2 recompiles functions without their contracts.
- `requires`, `ensures`, `invariant`, `old`, `result` and `todo` are reserved only in these positions.

`examples/store` is a larger example: three packages (`money`, `stock`, `orders`) whose functions and classes state their contracts, plus a program and its tests that use them through `import "orders"` etc. `cd examples/store && ../../build/fut test store_test.fut` runs 7 written tests (output, effects, an atomic checkout, `allocs`, `grows`) and 7 property tests generated from the contracts, including stateful ones for the `Stock` and `Cart` classes and their invariants; each package's `.api` shows its contracts.

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

`fut api main.fut` writes a `.api` file next to each compiled file that has `pub` declarations. It lists only the signatures, sorted by name, each with the `//` comments right above it and its [contract](#contracts) below it, and includes a class's invariants, fields and methods:

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

## Packages

A directory with a `fut.pkg` file is a package. Its files are the `.fut` files below it, down to the next `fut.pkg`.

```
// geom/fut.pkg
name geom                    // how other packages import it
version 0.3.0                // major.minor.patch (default 0.0.0)
root geom.fut                // its root file (default: <name>.fut)
dep units ../units           // a package it imports: the name, and where it is
js draw ./draw.js            // foreign modules (see Foreign modules)
```

Other packages import it by name, and reach it through that name (or the one after `as`):

```
// app/fut.pkg: name app, dep geom ../geom
import "geom"
var v = geom.Vec(3, 4)
import "geom" as g           // the same package, under another name
```

A package has two levels of visibility. Inside it, `pub` makes a name visible to the package's other files, which import each other by path, as above. Outside it, only the package's interface is visible: what its root file makes `pub`, and the `pub` names of files the root re-exports with `pub import "./vec.fut"`. Anything else is a compile error that says where the boundary is:

```
error: 'sq' is pub inside package geom but not part of its interface: only what geom.fut makes pub (or re-exports with pub import) is
```

The rest follows from that:
- A package sees its own names, its dependencies' (through their names), and builtins. It never sees the program's names, so it means the same thing in every program that uses it.
- `import "name"` needs a `dep` line. A path into another package is an error (`'../geom/util.fut' is in package geom: import it by name`), and so is a path out of the package.
- Dependencies go one way. A cycle is a compile error that names it (`app -> geom -> app`).
- The program's own package, and files that are in no package, work as described in [Modules and visibility](#modules-and-visibility).

**One interface file per package.** `fut api` writes a single `.api` for a package, next to its root file. It holds the package's whole interface, re-exported files included, under the package's name and version (`// The public interface of package geom 0.3.0 (geom.fut)`). Files inside the package get no `.api` of their own, because their `pub` names are not visible outside it. When the interface loses or changes a declaration, the version must move: from 0.x.y to at least 0.(x+1).0, and from 1.0.0 on, to the next major version. Until it does, `fut test` fails (`its package's interface lost declarations, a breaking change, but its version is still 0.3.0: set 'version 0.4.0'`, status `breaking` in JSON) and `fut api` won't write the file. Additions need no bump.

**Reloads stop at package boundaries.** A function body is reused when its text and its environment are unchanged. Its environment is its own package's declarations plus the interfaces of the packages it imports. So a new private function in `geom` recompiles only `geom`, an addition to `geom`'s interface also recompiles the packages that import it, and an edit in the program recompiles no dependency. Manifests are watched too.

### The standard library

`std/` holds packages written in fut, imported as `import "std/strings"` (then `strings.upper(s)`) from any file, with no `dep` line. Each is an ordinary package with an `.api`, which is the place to read what it offers: `std/strings/strings.api`, `std/lists/lists.api`, `std/math/math.api`. A builtin is something only the runtime or the host can do; everything else belongs in a std package, where it is readable fut, its contracts are tested, and it reloads like other code.

- **`strings`:** replace, startsWith, endsWith, count, repeat, padStart, padEnd, upper, lower, lines, words, reverse.
- **`lists`:** sort (stable, of numbers or of strings), sortBy, map, filter, reduce, find, any, all, count, indexOf, contains, reverse, unique, flatten, range, sum, min, max. None changes the list it is given.
- **`math`:** libm natively and JavaScript's `Math` under wasm (pow, exp, log, log2, log10, cbrt, hypot, the trigonometric functions, ceil, trunc), all declared pure; abs, min, max, clamp, sign, round, isNaN, pi and e in fut.

The binary finds `std/` next to the directory it is in (`build/fut` uses `std/`), or at `$FUT_STD`; the WebAssembly hosts pass its location. `std/x` and a program's own package `x` can both be used. `fut test` checks the contracts of std functions only when the test file is itself in `std/`, so a program's tests don't rerun the library's. `python3 build.py test` runs `std/*/*_test.fut`.

### Error codes

Every error the compiler or runtime raises ends with a stable code naming the rule it broke: `undefined name 'x' [undefined-name]`, `index 3 out of range (len 1) [index]`, `the caller broke f()'s contract: requires n > 0 (n is 0) [requires]`. `--json` records carry it as `"code"`. A code is never renamed or reused, so tools and agents can key on it instead of the wording, which may improve. `fut explain index` prints a code's rule and fix; `fut explain` lists them all. The value a `catch` receives is the message alone, and what a program throws itself has no code. `tests/specs/codes.fut` raises every code, and the build checks that each code the source uses is registered in `src/errors.cpp` and raised there.

### fut map

`fut map main.fut` prints the layout of a program: each package, the files of it the program uses, and every top-level declaration, with line ranges, in fut syntax. It is generated from the sources each time, so it is never stale and never committed. It is for readers who want to know where things are before reading any code, such as an agent picking where to work.

```
pkg app 0.0.0  // fut.pkg
  main.fut
    import "geom"  // 1
    fn main()      // 3-9
pkg geom 0.3.0  // ../geom/fut.pkg
  js draw draw.js  // line, fill
  geom.fut  // root
    pub import "./vec.fut"  // 1
    pub fn area(r)          // 3-5
    fn hidden()             // 6
  vec.fut
    pub class Vec    // 1-4
      var x = 0      // 2
      fn len()       // 3
```

Private names are included; contracts are not (the source and the `.api` have them). The map and the `.api` come from the same reading of the source, so they always agree. `--pub` shows only what other packages can use: each package's interface, and the `pub` names of files in no package. `--json` prints one object per line (`{"type":"fn", "package", "file", "name", "in", "pub", "sig", "line", "end", "hash"}`; `hash` is of the declaration's text, so it changes when the declaration is edited, not when it moves, plus `package`, `module` and `file` records). A last argument keeps one package (`fut map main.fut geom`) or one file.

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

## Isolates and channels

An isolate runs a function in parallel with the rest of the program, on a stack and a heap of its own. Isolates share no objects, so there are no data races to write: they communicate by sending values over channels, and each value sent is copied.

```
fn crunch(jobs, results) ! chan {
  for job in jobs { results.send(score(job)) }   // for-in receives until the channel is closed and empty
}
fn main() {
  var jobs = channel(64)       // holds up to 64 values: send waits while it is full
  var results = channel(64)
  var ws = []
  for i in 0..cores() { ws.push(spawn crunch(jobs, results)) }
  for j in work { jobs.send(j) }
  jobs.close()
  var total = 0
  for i in 0..len(work) { total += results.recv() }
  for w in ws { w.join() }
}
```

- **`spawn f(args)`** starts an isolate running `f(args)` and gives its task. `f` is a function value: a name, a field, or a lambda (`spawn fn(n) { ... }(4)`). `task.join()` waits for it and gives its result; if it failed, `join` raises its error, with its frames and the line of the `spawn`. `task.done()` says whether it has finished.
- **What an isolate starts with:** a copy of the spawner's globals as they were at `spawn`, the copied arguments, and the effects the spawner may perform. What it changes stays its own.
- **`channel(n)`** holds up to `n` values. `send(v)` waits while it is full; `recv()` waits while it is empty. `channel(0)` hands each value over directly: `send` waits until a receiver has taken it. `close()` ends it: what is in it can still be received, then `recv()` raises and a `for` loop over it ends. `tryRecv()` gives nil instead of waiting.
- **Sending copies.** Strings, lists, maps, instances and closures are copied into the receiver's heap, with their sharing and cycles: a list holding one map twice arrives holding one copy twice. Functions and classes are compiled code, shared by every isolate, so they are not copied. A channel or a task is sent as itself. A fiber can't be sent.
- **Effects.** `spawn` performs `spawn`, and every channel or task operation performs `chan`, so a function's declared effects say whether it can start isolates or wait on one: `fn log(msg) ! io` can't send to a channel.
- **Atomic blocks.** A `send` inside `atomic { }` happens when the block commits, and never if it rolls back, so no isolate sees a message from a block that didn't happen. `recv`, `join` and `spawn` inside one are errors: waiting would hold the block open, and a started isolate can't be taken back.
- **Deadlocks are errors.** If every isolate of a program is waiting on the others, none can ever go on. Each raises an error that names every isolate, what it waits in and where, instead of hanging.
- **Nothing is lost.** When `main()` returns, the program waits for the isolates it started. An isolate that failed with nothing joining it fails the program, with its error. If `main()` fails, the others are stopped. `fut test` does the same at the end of each test.
- **Scheduling.** A program's isolates run on a pool of up to `cores()` threads (`--threads n` sets another size); `main()` keeps its own. An isolate that waits lets others run on its thread, and one that computes for 10 ms while another on its thread is ready gives way at its next loop iteration or call. Thousands of isolates are fine. A call that blocks its thread (`sleep`, `exec`, `fetch`) holds up the isolates that share it.
- **Tests replay.** `fut test` runs each program's isolates on one thread, switching between them at channel operations (and `print`) as a generator seeded with 1 decides. Every run interleaves them the same way, so a test that spawns passes or fails the same every time, and `--changed` can reuse its result. `--seed n` tries another interleaving; a failing test names its seed. `fut --seed n prog.fut` runs a program the same way.
- **Speed.** An isolate costs a stack (8 MB reserved, touched as used) and a VM block (its globals and inline caches); code compiled by one is used by all. `bench/mandelbrot_par.fut` on 8 isolates runs about 7 times as fast as on one. Spawning copies every global, so keep large data out of globals that workers don't need, or send it.
- **Hot reload.** Under `-w`, a reload stops every isolate at a safe point (a loop iteration or call, or where it waits), patches the code once, then has each run the new top level and `reload()` in its own transaction. If any of them fails, every isolate rolls back and the old code returns, as for a failed reload with one isolate. `main()` reloads even while it waits on a channel or a task.
- Natively only for now: in wasm, `spawn` raises (channels work within the one isolate). That is phase 5 of `docs/isolates.md`.

## C functions

```
extern fn hypot(num, num): num       // any C function in the process (libc, libm, system frameworks)
extern fn getenv(str): str
extern "libsqlite3.dylib" fn sqlite3_libversion(): str   // or from a library
print(hypot(3, 4))
```

Types: `num` (double), `int` (32-bit), `long` (64-bit), `ptr` (a pointer, as a number), `str` (a `const char*`; a returned one is copied, NULL becomes nil). The return type defaults to `void` (nil). Up to 7 parameters; variadic C functions such as `printf` are not supported. A call is a thunk that branches to one generic marshaller: integer and floating-point arguments travel in separate registers (`x0`–`x7` and `d0`–`d7`), so passing all of them suits every signature. Arguments are type-checked, but what the C function does with them is not.

A call performs `io` (see [Effects](#effects)). A bare `!` after the signature declares that the function performs nothing, so code that may not perform `io` can call it: `extern fn sin(num): num !`. Like the types, this is the declaration's promise, not something fut can check.

## Foreign modules

A package can include JavaScript, WebAssembly and C, listed in its `fut.pkg` and bound with `extern <module> fn`:

```
// fut.pkg
name auth
js jwt ./jwt.js              // a JavaScript module (wasm build)
wasm zlib ./zlib.wasm        // a WebAssembly module (wasm build)
c jwt libjwt.dylib           // a C library (native build): the same name, for the other target

// auth.fut
extern jwt fn sign(any, str): str
extern zlib fn crc32(int, int): int
pub fn token(user) { return sign({"sub": user.name, "roles": user.roles}, secret) }
```

Each build binds the version it can load: the native build uses `c`, the wasm build uses `js` or `wasm`. A module the build can't load is a compile error that says which version to add. Mistakes are compile errors too: a module the package doesn't list, a function the module doesn't export, or a type a wasm module can't take.

Into JavaScript, besides the C types:
- **`any`** copies data. Numbers, strings, bools, nil (`null`), lists (arrays) and maps with string keys (objects) are copied both ways, nested as deep as they go. A JS object that is not plain data, such as a class instance, a DOM node or a function, comes back as a handle (`<js value>`). Fut code can keep a handle, compare it, and pass it back to JavaScript, which gets the same object. When fut no longer holds it, JavaScript can collect it. A fut instance or fiber can't cross: pass a map.
- **`fn`** passes a fut function, which JavaScript gets as a function. If JavaScript calls it during the extern call, as `Array.map` does, it runs at once and returns its result. If JavaScript calls it later, from an event or a timer, it runs at the program's next safe point, and the call returns a Promise of its result. JavaScript may call it as long as it keeps it.
- **Promises.** If a JS function returns a Promise, the fut code that called it waits for the result, as `sleep()` does, without blocking the page. In a fiber, the fiber and its resumer wait.
- **Exceptions.** A JS exception becomes a fut error (`sign(): invalid key`), which `try` catches.

A wasm module's functions take and return numbers (`long` is a BigInt on the JavaScript side). A C module is a library loaded by `dlopen`, and works as described in [C functions](#c-functions).

**Hot reload.** Under `-w`, a module's file is watched with the rest of the program. When it changes, it is loaded again, and the fut functions bound to it call the new version, with no restart. If the new version doesn't load (a syntax error), the old one stays in place and the error is printed. `fut map` lists each module and the functions bound to it.

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
- **Incremental.** Every function and method is hashed by its source text and by the declarations it can see: its own package's, and the interfaces of the packages it imports (see [Packages](#packages)). Unchanged ones are skipped; a function that only moved has its line table shifted instead.
- **Compile errors** are reported with file:line:col and a caret, and nothing is patched. **Runtime errors** print a stack trace; under `-w`, fut then waits for a fix and reruns `main`.
- **A failed reload is undone.** The new top level (initializers of new and edited vars) and `reload()` run as one transaction, as if inside `atomic` (with isolates running, one in each isolate, and all of them commit or roll back together: see [Isolates and channels](#isolates-and-channels)). If either throws, the data they changed is rolled back. The previous code is then compiled again from the sources it was built from, without being run. The program carries on as though the file had not been saved, after printing the error and `[fut] reload rolled back`. The next save tries again.
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
- **`extern fn`** calls JavaScript instead of C. With no library it looks in a small libc shim (`strlen`, `atoi`, `getenv`, ...), then `Math`, then globals. `extern "console" fn log(str): void` names an object. A package's `js` and `wasm` modules (see [Foreign modules](#foreign-modules)) are loaded before the compile that binds them: the compile names the ones it is missing, the host imports them (Node: dynamic `import()`; the browser: the same, from the server), and the compile runs again. The extern call import is a `WebAssembly.Suspending` function, so a Promise result suspends the calling stack.

Performance, as best of 5 runs after warm-up (wasm vs arm64): float loop 18 vs 18 ms, `fib(32)` 16 vs 12 ms, 5M method calls 20 vs 15 ms, 1M allocations 6.8 vs 4.4 ms. The first call of a function runs in V8's baseline compiler, and V8 has no on-stack replacement for wasm, so a long loop in a function called only once (the first run of the float loop: 77 ms) stays at baseline speed. Functions called repeatedly, such as the ones a game loop calls each frame, are optimized after a few calls. In Node, `node --no-liftoff web/run.mjs` optimizes everything up front, at about 200 ms of extra startup.

Other differences from arm64: file changes and typed lines are delivered only while the program waits in `sleep()`, `present()` or a fiber switch, because JS events cannot interrupt running wasm. Compile errors unwind with `setjmp`/`longjmp`, since WASI libc++ has no C++ exceptions.

### Cloudflare Workers

A fut program can run as a Cloudflare Worker, and test files can run on Workers in shards. A Worker may instantiate only the wasm modules uploaded with it: `new WebAssembly.Module(bytes)` fails with "Wasm code generation disallowed by embedder". So `fut worker build` (`node web/worker.mjs`) runs each program's compile ahead of time, under Node, and keeps every module the compiler emits. In the Worker, `fut.js`'s `module()` hook hands back the uploaded module with the same bytes. The compile is deterministic: the same files at the same paths (`/app/...`), the same steps and the same heap give the same bytes. Each module is checked by length and hash, in order, and a mismatch fails loudly ("not one compiled ahead of time: rebuild the bundle"). Tier 2 is off (`noTier`), since it compiles while the program runs.

```
fut worker build examples/worker/hello.fut build/worker      # fn serve(req) answers each request
fut worker run build/worker /echo --method POST --body hi    # one request under Node, code generation off as in a Worker
cd build/worker && npx wrangler dev                          # workerd, locally; `npx wrangler deploy` (logged in) to deploy

fut worker build --tests --shards 4 --root tests build/wt tests/specs/*.fut   # build/wt/shard-<i>/, build/wt/manifest.json
fut worker test build/wt                                     # every shard under Node, as on Workers
curl 'localhost:8787/__fut/test?file=specs/try.fut&filter=throw'             # a shard: `fut test --json` lines
```

- **Serving.** `serve(req)` gets `{method, url, headers, body}` (header names lower-case, body text). It returns a string (text/plain), a map with `status`, `headers` and `body` (a body that isn't a string goes as JSON), other data (sent as JSON), or `nil` (204). The first request creates one instance per isolate and runs the top level, not `main()`. Requests then call `serve` one at a time, since they share one shadow stack, so a slow `fetch()` delays the requests queued behind it. `fetch` and `fetchAll` use the Worker's `fetch()` and suspend through JSPI. JSPI needs no compatibility flag; the bundle's `compatibility_date` is 2025-09-01.
- **Tests.** A shard holds any number of test files and runs each in a fresh instance (about 5 ms each beyond its tests). `GET /__fut/test` (or `POST` with `{files, filter}`) returns each file's `fut test --json` lines, then a summary of them all; `/__fut/tests` lists the tests. `manifest.json` records which files and tests are in which shard and how big each is. `--shards N` balances the files by size; otherwise `--max-kb` (default 8192) caps a shard. `run()` works: the build runs the tests of files that call it, and compiles each child program they start (by its args and its file or source). A child whose source changes from run to run can't be known ahead of time and reports so.
- **Sizes and limits.** fut.wasm (the interpreter included) and the JS come to 865 KB (299 KB gzipped). Each test file adds its own modules and sources: tests/specs averages 13 KB a file (median 8 KB), about 2.5 KB a test. Files with the same code share modules. Workers allow 64 MiB uncompressed per Worker, with no module-count limit, so size allows thousands of test files per shard. What binds is CPU time: 30 s per request by default and 5 min at most on the Paid plan, while the Free plan's 10 ms fits a request or two but not a test run. Memory is 128 MB per isolate; a 300-file shard ran in workerd at 75 MB. Shard by how long the tests take.
- **Interpreted instead: one static Worker.** `fut worker build --interp outdir` makes a Worker that compiles and runs whatever `POST /run` sends (`{files: {path: text}, tests: [path...] | main: path, filter}`; `fut worker send <url|outdir> --root dir files...` sends a directory's sources). It deploys once and never again. fut.wasm is uploaded, so V8 compiles it. The code fut compiles from the request is interpreted by src/interp.cpp, a switch over the instructions gen_wasm.h emits, which runs inside fut.wasm. Static trampolines give interpreted bodies table entries. A tiny static module (`tryCallBytes` in fut.js) catches the fut exception tag for `try_table`. JSPI suspends through the interpreter as through compiled code. The whole wasm suite passes interpreted (`FUT_INTERP=1 python3 build.py test --wasm`), including hot reload, fibers, run() and stack traces. In bench/workers.md, a typical test file takes about as long either way (compile and instance setup dominate: a 13 ms median AOT against 15 ms interpreted). Whole suites take 2–3× longer interpreted, and CPU-bound code 40–80× longer.
- **Limits.** No hot reload, no tier 2, no REPL. `exec`, files and `env` are missing; so are packages' `js`/`wasm` modules (they would need importing statically). Production Workers stop `Date.now()` during execution, so timing tests (`expect grows { }` on time) are unreliable there; `expect grows allocs` is exact.

## Implementation

| File | What |
|---|---|
| `fut.h` | NaN-boxed values, object layouts, VM block, arm64 encoders (all checked against the system assembler) |
| `compile.h` | Lexer and the parser state shared by both code generators |
| `compile.cpp` | Single-pass Pratt compiler, generic over the code generator; token pre-pass for names, arities, spans and contracts; contracts woven into functions, invariants into a hidden method per class; imports; incremental commit; tier 2 |
| `gen_a64.h` / `gen_wasm.h` | Code generators: arm64 machine code / WebAssembly function bodies, behind one interface |
| `wasm.h` | WebAssembly opcodes, encodings, helper signatures |
| `rt.cpp` | Portable runtime: helpers, natives, objects, inline caches |
| `jit.cpp` | arm64 platform: executable memory (free list, reclamation, `--asm`), stack traces and try, fibers, C calls |
| `wasm.cpp` | WebAssembly platform: module building and table patching, JS calls, JSPI fibers, the exports the JS host drives |
| `web/` | `fut.js` (JS host: instantiation, installs, traces, fibers, driver), `run.mjs` (Node CLI), `serve.mjs` + `index.html` (browser) |
| `gc.cpp` | Allocator using 64 KB pages with size classes (O(1) "is this a heap pointer?"), counting allocations from its runs; mark bitmap, no sweep, conservative over stack and registers |
| `gfx.cpp` | The window: AppKit through the Objective-C runtime, loaded on first use (a canvas in wasm) |
| `main.cpp` | Driver: watcher, safepoint reload (as a transaction), `main` restart, REPL, `--asm`, `fut test`, `fut api`, `fut map` |
| `txn.cpp` | Transactions (`atomic`, reloads, contract checks that call code): an object is copied before its first write, globals at the start; rollback, nesting |
| `types.cpp` | Optional types: what each check names, the check compiled code calls, and the one field stores make |
| `effects.cpp` | Effects: declared sets and their checks (entry, perform site), operations and their ids, handler records, `handle`, `random()`; the frame records that tell an outside call for invariants |
| `api.cpp` | Public interfaces: a file's or a package's `pub` declarations as `.api` text, the version check, the check `fut test` runs, `fut api`, `fut map` |
| `errors.cpp` | Error codes: the registry (code, rule, fix), finding a runtime error's code from its message value, `fut explain` |
| `pkg.cpp` | Packages: reading `fut.pkg` manifests, finding a file's package, where the standard library is |
| `jsval.cpp` | Values crossing into JavaScript: JSON both ways, pinned fut functions, handles (`Ref`) for JS objects |
| `testcache.cpp` | `fut test --changed`: each test's trace (functions entered, method lookups, names used) with the hashes the compiler gives, checked against the program as compiled now, and the cache file |
| `test.cpp` | `expect`'s check (deep equality, failure details), `allocs` and `grows` (sizes, timing, the log-log fit), property tests from contracts (generation, shrinking), a failed contract's message, and the test runner's reporting, text and JSON; shared by both platforms |

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
