# fut spec (terse, for agents)

What you need to read and write fut correctly. The README has the reasons, costs and implementation. Every ```fut block below runs (via `python3 build.py test`): a top-level `expect` is an assertion, so the blocks are the spec.

## Commands

```
fut f.fut                run (calls main() if defined, after the top level)
fut test f.fut           run test blocks + property tests + .api check; --json, --filter s, --timeout s
                         exit 0 all pass, 1 fail/error/pending, 2 compile error
fut api f.fut            write .api interface files (--check: only report)
fut map f.fut [pkg|file] packages, files, declarations, line ranges; --pub, --json
fut -w f.fut             run, hot-reload on save, REPL on stdin
node web/run.mjs f.fut   same CLI on WebAssembly
```

## Syntax

- Statements end at a newline or anything that can't continue the expression; `;` optional. A line starting with `(`, `[` or `-` starts a new statement.
- `//` comments only. Blocks always `{ }`. `else if`. No ternary, no `**`, no `?.`, no slicing, no varargs, no default arguments.
- Named `fn`/`class` only at top level; inside functions use `var f = fn(x) { ... }`.
- Strings `"..."` only. Escapes `\n \t \0 \" \\ \{`; any other `\c` is `c`. `"{expr}"` interpolates (expr can't contain `"`).
- Numbers: `12 1.5 2e3 0xff`. All f64.
- Reserved: `var fn class if else while for in return break continue true false nil self super import extern yield try catch throw`.
- Contextual words (usable as names elsewhere): `as pub override atomic effect handle with test expect throws prints allocs grows like requires ensures invariant old result todo`.

```fut
var a = 1; var b = 2
var x = a
-1                                   // separate statement, not a - 1
expect x == 1
expect "t\{x}\n".len() == 5
expect "{a + b}!" == "3!"
```

## Values and operators

nil, bool, num (f64), str (immutable), list, map, fn/closure, class, instance, fiber. Only `nil` and `false` are falsy.

```fut
expect 0 && "" && true               // 0 and "" are truthy
expect (nil || "d") == "d"           // && || return an operand
expect 7 / 2 == 3.5 && -7 % 3 == 2   // % takes the divisor's sign
expect 2 + 3 & 6 == 5 & 6            // bitwise binds looser than arithmetic (Python order); whole numbers only
expect "a" + 1 == "a1" && 1 + "x" == "1x"   // + with a str on either side concatenates
expect "ab" == "ab" && "a" < "b"
var same = [1] == [1]
expect same == false                 // == on lists/maps is identity (expect's own top-level == is deep)
expect [1, 2] + [3] == [1, 2, 3]     // new list
expect throws { 1 + nil } == "bad operands for '+': number and nil"
expect throws { 1 < 2 < 3 }          // no chaining
expect 1 / 0 > 1e308
```

## Builtins and methods

`print(v)` (one argument) `len(v)` `str(v)` `int(n)` (truncates; numbers only) `floor sqrt clock sleep(s) random()` `fiber(fn)` `run(file | {source, args, timeout}) -> {code, out, err, timedOut}` `todo()`. Graphics: `window(w,h) present() clear(c) rect(x,y,w,h,c) circle(x,y,r,c) key(name) mousex() mousey() mousedown()`.

- list: `push(v)` (returns the list) `pop()` `len()`. Index must be in `0..len-1` (read or write), else error. No negative index, no sort, no slicing.
- map: `has(k) remove(k)` (returns value) `keys() values() len()`. `m.k` is `m["k"]`. Missing key reads `nil`. Iteration in insertion order. Keys may be any value.
- str: `len() has(sub)`. `s[i]` is a 1-char str. No other methods.
- `for x in` list (items), map (keys), str (chars), `a..b` (end exclusive), fiber (yields). Not a bare number.

```fut
var m = {name: "fut", "k": [1, 2]}
expect m.name == "fut" && m.zz == nil
expect m.remove("name") == "fut"
expect m.keys() == ["k"]             // deep only as expect's top-level op, not inside &&
var s = 0
for i in 2..5 { s += i }
expect s == 9
expect "abc"[1] == "b"
expect throws { [1][1] } == "index 1 out of range (len 1)"
expect throws { int("4") }
```

## Functions and closures

- Calls are arity-checked (compile error for known functions). Missing `return` gives `nil`.
- Closures capture enclosing **locals by value** (a copy the closure can mutate; the outer function doesn't see it). Globals are shared. Use a list/map to share state.
- `main()` runs after the top level. `reload()` runs after each hot reload.

```fut
fn counter() { var n = 0 return fn() { n += 1 return n } }
var c = counter()
c()
expect c() == 2
fn f() { var x = 0 var g = fn() { x += 1 } g() return x }
expect f() == 0                      // g changed its own copy
fn h() { var box = [0] var g = fn() { box[0] += 1 } g() return box[0] }
expect h() == 1
```

## Classes

```fut
class Shape {
  fn init(name) { self.name = name }   // constructor: Shape("x")
  fn area() { return 0 }
}
class Rect : Shape {
  fn init(w, h) { super.init("rect") self.w = w self.h = h }
  override fn area() { return self.w * self.h }   // override required iff replacing
}
class Unit {
  var hp: num = 10                     // declared fields: default is a literal (num/str/bool/nil)
  var name
}
expect Rect(2, 3).area() == 6
var u = Unit()
expect u.hp == 10 && u.name == nil
expect throws { u.hpp = 1 }          // declaring any field closes the class (compile error inside its methods)
```

- Without declared fields, fields appear on assignment. Instances print as `<Rect>`.
- An override takes the same parameter count, the same types, and a subset of the effects. `init` is exempt.

## Types (optional, checked at run time)

`num str bool list map fn any`, class names (incl. `mod.Class`; subclasses accepted), `T?` adds nil. On parameters, results, fields, vars. Checked: args on entry, result on exit, fields and vars on every store.

```fut
fn half(n: num): num { return n / 2 }
var level: num = 1
var note: str? = nil
expect throws { half("x") } == "argument 'n' of half() must be num, got the string \"x\""
expect throws { level = "two" }
```

## Effects

After the result type: `! io`, `! clock, random`, `! Log` (declared effect), `!` alone = none. No `!` = unchecked (inherits the caller's allowance). Built-in effects: `io` (print, run, extern, graphics), `clock` (clock, sleep), `random`, `yield`. Throwing is not an effect. A declaring function may only perform, and only call declaring functions with, a subset of its set.

```fut
effect Log { fn log(msg: str) }
fn work() ! Log { Log.log("hi") }
fn pure(): num ! { return 1 }
var lines = []
handle { work() print("x") } with Log.log(m) { lines.push(m) } with print(s) { lines.push(s) }
expect lines == ["hi", "x"]
expect throws { work() }             // no handler for Log.log
fn bad() ! { print("x") }
expect throws { bad() }
```

- `handle { } with op(params) { }`: ops are builtins with effects (`print clock sleep random run yield`, graphics) or `Effect.op`. The handler's return value is the op's result; it never resumes later. Handlers are closures (capture by value).
- A handler outside a function that declares the effect can answer it; it can't make a `!`-without-it function perform it. Answering every op of an effect allows that effect inside the block. `io` can't be answered whole.
- `pub effect` to export.

## Errors and atomic

```fut
fn load(p) { if p == "" { throw "no path" } return p }
var got = nil
try { load("") } catch e { got = e }
expect got == "no path"
try { throw {code: 42} } catch e { expect e.code == 42 }   // any value; runtime errors arrive as their message
var acct = {a: 10, b: 0}
try { atomic { acct.a -= 20 acct.b += 20 if acct.a < 0 { throw "insufficient" } } } catch { }
expect acct.a == 10 && acct.b == 0   // rolled back: fields, lists, maps, globals, captured vars (not locals)
```

- A failed `atomic` undoes visible state then rethrows; nested blocks roll back their own part. `io` inside `atomic` is an error unless a handler inside the block answers it. A fiber can't `yield` in its own atomic block.

## Contracts

```fut
class Account {
  var balance = 0
  invariant self.balance >= 0
  fn init(b: num)
    requires b >= 0
  { self.balance = b }
  fn withdraw(n: num): num !
    requires n > 0
    requires n <= self.balance
    ensures result == old(self.balance) - n
  { self.balance -= n return self.balance }
}
var acc = Account(10)
expect acc.withdraw(3) == 7
expect throws { acc.withdraw(50) }
```

- `fn close() { todo() }`: a contract-first stub; calling it throws, `fut test` reports it pending.
- One clause per line, between signature and body; brackets for a longer one. `requires` on entry (caller's fault), `ensures` on every return (`result`, `old(e)`), `invariant` after `init` and after each outside call or outside field store. Inner self-calls don't check invariants.
- Clauses run with no effects and their changes undone.
- Contracts of `pub` declarations go into the `.api` and may only use what readers see (params, `self`, `result`, builtins, `pub` names).
- `fut test` property-tests every contract whose params are all typed `num str bool list map` (or `?`); classes get stateful call sequences. `todo()` bodies are pending.

## Tests

```fut
fn total(items) { var s = 0 for i in items { s += i.price * i.qty } return s }
test "total" {
  expect total([{price: 2, qty: 3}]) == 6
  expect throws { total(nil) }
  expect prints { print("a") } == "a\n"
  expect allocs { [1, 2] } <= 1
  expect grows allocs { n -> total([]) } like "1"   // exact; time: expect grows { n -> ... }
}
```

- `expect a op b` reports both sides. When `==`/`!=` is expect's top-level operator it compares lists and maps deeply (key order ignored); elsewhere `==` is identity. `expect grows [allocs] { n -> ... } like "1" | "n" | "n log n" | "n^2"` is an upper bound.
- `fut test` runs the top level once, then each test (natively, each test is a forked process). It doesn't call `main`. A test naming an undefined function is pending, not a compile error.
- Repo layout: `tests/specs/*.fut` (fut test), `tests/specs/progs/` (programs they `run()`), `tests/*.fut` (`// expect:` stdout lines).

## Modules, pub, .api

- `import "path.fut"` (top level, relative, loaded once) makes its `pub` names global. `import "path.fut" as m` puts them under `m.` (`m.f()`, `m.C()`, `class D : m.C`). A module isn't a value. A module's `main` doesn't run.
- Non-`pub` top-level names are file-private: renaming/deleting them breaks nobody. A pub class's fields and methods are public.
- `fut api` writes `X.api` next to each imported file (or one per package) listing pub signatures, types, effects, contracts and the `//` comments above them. `fut test` fails while one is stale. **If your change edits a `.api`, you changed a public interface; run `fut api` and commit the `.api` with it.**

## Packages

```
// geom/fut.pkg
name geom
version 0.3.0          // breaking .api change: bump minor (0.x) or major (>=1.0), else fut test fails
root geom.fut          // default <name>.fut
dep units ../units
js draw ./draw.js      // foreign modules; also: wasm name ./x.wasm, c name libx.dylib
```

- `import "geom"` (needs a `dep` line) then `geom.f()`. Outside a package only its interface is visible: the root file's `pub` names plus `pub import "./x.fut"` re-exports. Inside it, files import each other by path, and `pub` means package-visible.
- No paths into or out of a package, no cycles. A package never sees the program's names.

## Fibers

```fut
var f = fiber(fn(first) { var t = first while true { t += yield t } })
expect f.send(10) == 10 && f.send(5) == 15   // send(v) resumes; v becomes yield's value
var g = fiber(fn() { yield 1 return 2 })
expect g.resume() == 1 && g.resume() == 2 && g.done()
```

`yield` works at any call depth inside the fiber. An error inside a fiber ends it and is rethrown in the resumer.

## Foreign code

```
extern fn hypot(num, num): num               // C in-process (native) / JS global (wasm)
extern "libsqlite3.dylib" fn sqlite3_libversion(): str
extern jwt fn sign(any, str): str            // module named in fut.pkg (js/wasm/c)
```

Param types: `num int long ptr str`, plus `any` (copies JSON-like data, else a handle) and `fn` (callback) for JS. Return defaults to nil. At most 7 params, no variadics. A JS Promise suspends the caller; a JS exception becomes a fut error.

## Hot reload rules that affect how you write code

- Globals keep their values across reloads. A `var`'s initializer reruns only when the var is new or its line was edited. Migrate state in `reload()`.
- A reload whose new top level or `reload()` throws is rolled back entirely.
- Running frames finish in old code. A function whose parameter count changed makes old callers fail with "stale call".
