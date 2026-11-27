# fut spec

Everything needed to write fut. ```fut blocks run as tests (a top-level `expect` asserts).

```
fut f.fut                      run: top level, then main() if defined
fut test f.fut                 test blocks + contract property tests + .api check. exit 0 pass, 1 fail/pending, 2 compile error
  --json                       one JSON object per line: each test, each compile error, then a summary
  --filter text  --changed     only tests named text / only tests whose inputs changed since the last run
fut check f.fut                native: check the entry and its imports without running top-level code
fut refs f.fut C.field [--writes]  native: resolved field uses and possible same-name dynamic uses
fut map f.fut [--pub] [pkg]    where everything is: packages, files, declarations, line ranges
  --json                       one declaration per line, with a hash of its text
fut explain [code]             an error code's rule and fix (no code: all codes)
fut --record log f.fut         run, logging what clock/random/key/readFile/env/exec/fetch returned; --replay log: rerun from it (sleeps skipped, prints checked, [replay] where it differs)
fut init [dir]                 new package in dir (default .): fut.pkg, <name>.fut, .api, .gitignore
fut api f.fut                  rewrite the .api files after an interface change
fut -w f.fut                   run, hot-reload on save; stdin lines run inside the program
node web/run.mjs ...           the same CLI on WebAssembly
```

## Core

```fut
var a = 1; var b = 2               // ; optional. A line starting ( [ - begins a new statement
expect "{a + b}\{\n" == "3\{\n"     // "{expr}" interpolates (strings inside too: "{xs.join(", ")}"), so a literal { is \{ (JSON: "\{\"k\": 1}"); escapes \n \t \r \0 \" \\ \{ \} \u{hex} (others are errors)
expect "{3.14159:.2}|{7:>3}|{255:x}|{a=}" == "3.14|  7|ff|a = 1"  // {expr[=][:spec]}, spec: [fill][< > ^][+][0][width][,][.precision][f e x X b o]
expect 0 && ""                     // only nil, false are falsy: "" || "x" is "", so test s != "" for text
expect (nil || 2) == 2             // && || return an operand
expect (1 > 0 ? "y" : "n") == "y" && 2 ** 3 == 8 && nil?.x == nil && "abc"[1:] == "bc"
expect -7 % 3 == 2 && 7 / 2 == 3.5 // all numbers f64
expect 2 + 3 & 6 == 4              // Python precedence; bitwise needs whole numbers
expect "a" + 1 == "a1"             // + with a str concatenates
var same = [1] == [1]
expect !same                       // == on list/map is identity (expect's top-level == is deep)
expect [1] + [2] == [1, 2]
var m = {k: 1, "s": [2]}           // m.k is m["k"]; missing key: nil; insertion order
expect m.zz == nil && m.remove("k") == 1 && m.has("s")
expect "abc"[1] == "b" && "abc".has("bc") && len("abc") == 3
var l = [3]
expect l.push(4).len() == 2 && l.pop() == 4
expect throws { l[1] }             // index 0..len-1 only, read or write
var s = 0
for i in 0..4 { s += i }           // also: list, map keys, str chars, fiber
expect s == 6
```

## Builtins

```
print(a, ...)            one or more values joined by spaces, then a newline
len(v)                   of a str, list or map
str(v)                   v as text
int(n)  floor(n)  sqrt(n)   int truncates toward 0 (numbers only)
clock()                  seconds (monotonic)
sleep(s)                 seconds
random()                 in [0, 1)
seed(n)                  random() then repeats a fixed sequence (whole n; per isolate)
budget(n, f)             f() with at most n steps (loop iterations + calls), else [budget]; stepCount()
fiber(fn(x) { ... })     f.resume()  f.send(v)  f.done()
channel(n)  cores()      see Isolates
run(path)  run({source, args, timeout})   fut in a child process -> {code, out, err, timedOut}
isa(v, C)                is v an instance of class C (by name: one made before a reload counts)
typeOf(v)                "nil" "bool" "number" "string" "list" "map" "function" "class" "instance" "js value" ...
todo()                   placeholder body: throws "f() is not implemented yet"
time()                   seconds since 1970
json(v)  parseJson(s)    data <-> JSON text (nil is null)
env(name) -> str | nil
readFile(path) -> str | nil    writeFile(path, text)   (makes parent dirs)
exec([argv...])  exec({cmd: [argv...], cwd, input, timeout}) -> {code, out, err, timedOut}
fetch(url)  fetch({url, method, headers, body, timeout, idleTimeout}) -> {status, body, headers}; failure: {status: 0, error}, no throw
                         timeout: s for the whole request (default 60); idleTimeout: fail after s with no bytes moving
                         (a server that answers only when done is idle while it works); native: TCP keepalive drops dead peers
execAll([cmd, ...])  fetchAll([req, ...])   run at once, results in order
                         time/json/parseJson/fetch/fetchAll also in wasm; the other host builtins are native only
l.push(v) -> l   l.pop() -> v   l.len()   l.slice(a, b)   l.join(sep) -> str
m.has(k)   m.remove(k) -> v   m.keys()   m.values()   m.len()
s.len()    s.has(sub)   s[i] -> 1-char str   s.find(sub) -> i | -1   s.slice(a, b)   s.split(sep)   s.trim()
s.byte(i) -> 0..255   char(code) -> str (UTF-8)   parseNum(s) -> num | nil   (indexes count bytes; slice excludes b, clamps)
window(w, h)   present() -> false once closed (once per frame, waits 1/60 s)
clear(rgb)   rect(x, y, w, h, rgb)   circle(x, y, r, rgb)     rgb = 0xRRGGBB, origin top-left
key(name) -> bool   "a".."z" "0".."9" space enter escape tab up down left right
mousex()   mousey()   mousedown() -> bool
touches() -> [{id, x, y}]   fingers down, in window pixels, oldest first (a page's; [] natively)
screensize() -> [w, h]   what a window could fill: a page's viewport (CSS pixels), natively the screen's free part; window(w, h) again resizes
f64s(n | list | buf)  f32s  u32s  i32s  u8s   packed numbers (64/32-bit float, 32-bit whole, byte), fixed length, outside the heap,
                         also types: b[i] (each store checked; f32 rounds), for x in b, len, slice(a, b), bytes(), fill(x), toList().
                         An extern's ptr param gets the bytes; sending one moves it (the sender's handle dies)
vec2f(x, y)  vec3f  vec4f   an f32s of 2/3/4, also types; v.x .y .z .w (.r .g .b .a), swizzles v.xy v.zyx -> new;
                         + - * / % on f32s elementwise (with same-length f32s or a number) -> new; v.length() v.dot(w)
                         v.normalize() v.cross(w)
```

## Functions, classes

```fut
fn f() { var x = 0 var g = fn() { x += 1 } g() return x }
expect f() == 0                    // closures copy locals; share via list/map or globals
class Shape { fn init(n) { self.n = n } fn area() { return 0 } }   // undeclared fields: made by assignment
class Sq : Shape {
  var n
  var samples: list<num> = []       // fresh per instance, before init; {} creates a fresh map
  var side: num = 1                // any declared field closes the class: declare all, inherited too
  fn init(s) { super.init("sq") self.side = s }
  override fn area() { return self.side * self.side }  // override required iff replacing
}
expect Sq(3).area() == 9
expect throws { Sq(1).sid = 2 }
```

## Enums, match

```fut
enum Expr {
  Num(v: num)
  Add(a: Expr, b: Expr)
  Zero
}
fn eval(e: Expr): num {
  match e {                        // no else: an arm per variant, or a compile error [match]
    Num(v) { return v }
    Add(a, b) { return eval(a) + eval(b) }
    Zero { return 0 }
  }
}
expect eval(Expr.Add(Expr.Num(2), Expr.Zero)) == 2
expect Expr.Num(1) == Expr.Num(1)  // by value; prints as made: Expr.Num(1)
var e: Expr.Num = Expr.Num(3)      // Expr and Expr.Num are types; e.v reads a field
match e { Expr.Num(_) { } else { } }   // _ skips a field; Num alone binds none; else takes the rest
```

`match` is a statement. A value that is no variant of the enum raises [match]. Variants with no fields are single values. Adding a variant to a pub enum is a breaking change.

`class` and `enum` declarations are top level only. A nested `fn g(x) { ... }` declares a local closure that can call itself by name. `main()` runs after top level; `reload()` after each hot reload. Arity is checked. Overrides keep param count and types.

## Types, effects

```fut
fn half(n: num): num ! { return n / 2 }   // types: num str bool list map fn any Class T?
var lvl: num = 1                           // checked on every store
expect throws { half("x") }
type User = {name: str, age: num?}         // also A | B, "GET" 3 true nil, list<T>, map<K, V>; pub type
fn ages(us: list<User>): num ! { return us.len() }   // given u: User, u.nmae is a compile error
var us = [{name: "a"}]
ages(us) expect throws { us.push(3) }      // us is list<User> from now on
effect Log { fn log(m: str) }
fn work() ! Log, clock { Log.log("hi") }   // ! = may only do these; bare ! = nothing; none = unchecked
var out = []
handle { work() print("p") } with Log.log(m) { out.push(m) } with print(t) { out.push(t) }
expect out == ["hi", "p"]
fn bad() ! { print("x") }
expect throws { bad() }
```

A list or map that passes `list<T>`/`map<K, V>` keeps that type for good: any later store into it, through any reference, throws unless it fits, and later checks of it cost nothing. A union or a failed check doesn't do this; `slice()` makes an unchecked copy. A shape is checked only when the value crosses the boundary, so a map can change after it passes; use a class to keep fields checked.

Effects: `io` (print, run, extern, graphics, files, env, exec, fetch) `clock` (clock, sleep, time) `random` `yield` `spawn` `chan` (every channel/task op), declared ones; json is pure. A handler's return is the op's result, no resumption.

Determinism: numbers (`+ - * / %`, sqrt, floor, std/math) give the same bits on every platform. Code whose declared effects leave out `io`, `clock`, `spawn` and `chan` is deterministic, given `seed(n)` if it has `random`; supply the clock with `handle { } with clock() { }`. Steps count the same everywhere: `budget(n, f)` stops runaway code at the same step on every platform; a try inside f can't get past it; an inner budget gets at most what the outer has left.

## Errors, atomic, contracts

```fut
var acct = {a: 10}
try { atomic { acct.a -= 20 if acct.a < 0 { throw "low" } } } catch e { expect e == "low" }
expect acct.a == 10                // atomic rolls back heap/globals on throw; io inside is an error
class Acct {
  var bal = 0
  invariant self.bal >= 0
  fn take(n: num): num
    requires n <= self.bal
    ensures result == old(self.bal) - n
  { self.bal -= n return self.bal }
}
expect throws { Acct().take(1) }
```

Clauses: one per line, run effect-free and undone. `fut test` property-tests contracts with typed params. `todo()` body = pending.

## Tests

```fut
test "t" {
  expect [1, {a: 2}] == [1, {a: 2}]
  expect throws { nil.x } == "nil has no field 'x'"
  expect prints { print(1) } == "1\n"
  expect allocs { [1] } <= 1
  expect grows allocs { n -> [n] } like "1"   // or: expect grows { n -> ... } like "n log n"
}
```

## Fibers

```fut
var fb = fiber(fn(x) { while true { x += yield x } })   // yield from any call depth
expect fb.send(1) == 1 && fb.send(2) == 3 && !fb.done()  // send(v): v is yield's value; resume() sends nil
```

## Isolates

```
var t = spawn f(a, b)          // f (name, field or lambda) in a new isolate: own heap, copy of the globals
t.join() -> f's result         // re-raises f's error; t.done() -> bool
var c = channel(8)             // capacity required; channel(0): send waits for a receiver
c.send(v)  c.recv()  c.tryRecv() -> v | nil  c.close()  for v in c { }   // values are copied (sharing, cycles kept)
```
No shared objects. All waiting on each other: a deadlock error naming each wait. `send` in `atomic` waits for the
commit; `recv`/`join`/`spawn` there raise. Unjoined failures fail the program. Pool of `cores()` threads (`--threads n`); `fut test`
(and `--seed n`) runs them on one thread, interleaved by the seed (default 1): same seed, same run. Under `-w` a reload
stops all isolates, then each runs the new top level and `reload()`; all commit or all roll back. Wasm: fut.wasm has one
thread, always seeded (default 1); fut-mt.wasm (the threads build) has the pool, on workers, and reloads as natively;
in fut.wasm a reload waits for the isolates to end.
```
parallel for i in 0..n { out[i] = f(xs[i]) }   // the iterations on the pool's isolates; ends when all have
```
Same result as a plain `for`, every run: whole-number range; the body can't assign outside variables, `return` or
`break` (compile errors); a buffer it writes (`name[...] = ` in the body itself) it reaches only at `[i]`; what else it
reads from outside is a frozen copy (changing one raises, globals too); no effects. An error is the lowest failing
i's: "parallel for, at i = 7: ...". [parallel]
```
var cfg = freeze({sizes: [1, 2], name: "a"})   // a deep copy that never changes; isFrozen(cfg) -> true
cfg.sizes.push(3)                              // raises [frozen]; thaw(cfg) -> a copy you may change
```
Every isolate reads a frozen value in place: sending one (to `spawn`, `send`, a `parallel for`) passes a pointer,
however big. Lists, maps, instances, buffers, strings and closures freeze; a frozen value's parts already frozen are
shared, not copied. Not channels, tasks, fibers, js values, or a value that contains itself. A frozen value goes when
no isolate, message or other frozen value holds it; `frozenCount()` counts those alive (every isolate's). [frozen]
```
shared var hits = 0                 // top level: one variable for every isolate (other globals are copied)
atomic { hits += 1 }                // assigned only inside atomic; reads anywhere give the latest committed value
```
A shared var holds frozen values (assigning freezes a copy). An atomic block that uses shared vars is a transaction
across isolates: it reads one consistent snapshot, and commits only if no var it read changed meanwhile; otherwise it
is rolled back and runs again (the function's locals as they were), with no limit. Its first run sets a shared var; a
reload keeps its value. Not in a parallel for's body. [atomic]

## Modules, packages

- `import "x.fut"` (pub names global) or `as x` (`x.f`). Non-`pub` = file-private, free to change.
- `fut.pkg`: `name`, `version`, `root`, `dep <name> <dir>`, `js|wasm|c <name> <path>`, `js <name> <path> main` (in a browser it runs on the page's thread; other JS runs on the thread calling it). `import "<name>"`. Outside sees only root's `pub` + `pub import` re-exports. No cycles, no paths across packages.
- `.api` = public signatures, types, effects, contracts. Stale `.api` fails `fut test`. Editing one means you changed an interface: run `fut api`, commit it; breaking change needs a version bump.
- `extern fn hypot(num, num): num` (C natively, JS under wasm; performs io), `extern fn sin(num): num !` (bare `!`: pure), `extern mod fn f(any, fn): str` (a module in fut.pkg; one this build can't load (js/wasm natively, c in wasm) compiles to a stub whose call raises [host]). Types `num int long ptr str`, and for JS `any fn`.
- Hot reload keeps globals; a var reinitializes only if its line changed.

## Std

`import "std/<name>"` (no dep line; `as` to rename). Signatures, docs and contracts: `std/<name>/<name>.api`.

- `std/strings`: replace startsWith endsWith count repeat padStart padEnd upper lower lines words reverse isInt parseInt (an optional `-` and digits only; parseInt: nil otherwise)
- `std/lists`: sort sortable sortBy map filter reduce find any all count indexOf contains reverse unique flatten range sum min max
- `std/math`: pow exp log log2 log10 cbrt hypot sin cos tan asin acos atan atan2 ceil trunc abs min max clamp sign round isNaN pi e
- `std/reactive`: signal(v) (.get .set .update .peek) effect computed batch untrack onCleanup root — effects rerun when a signal read on their last run changes
- `std/ui` (DOM calls: wasm; natively it imports and its pure fns run, DOM calls raise [host]): el(tag, props, kids) mount(sel|node, view) each(items, key, view) field(sig, props) check(sig, label, props) button(label, f) style(css) find htmlOf trigger; a fn child/prop stays current; `on<event>` props: handlers given {type, value, checked, key, x, y (from the element), clientX, clientY, button, ctrl, shift, alt, meta}; `{class: "x"}` (a keyword works as a key and after `.`)
- `std/gpu` (native over Dawn, `python3 build.py gpu` once; in a browser over its WebGPU, both builds; not under Node): WebGPU over Dawn, WGSL shaders (in a "..." string: `\{` for `{`). `window(w, h)` then `g = gpu.open(nil)`, or `gpu.open({headless: true, width, height})`; `L = gpu.layout({pos: "vec2f", c: "f32"})` (f32 vec2f vec3f vec4f mat4x4f), `L.pack(maps, asStruct) -> f32s`; `g.vertices(buf)` `g.storage(buf)` `g.uniform(L).set(map)`; `g.render({shader, vertex: "vs", fragment: "fs", layout: L, bind: [bufs], draw: "points", depth: true})` (depth: nearer z in 0..1 wins) `g.compute({shader, entry: "main", bind, size: 64})`; `g.run(p, n)`; `while present() { g.frame(0xRRGGBB).draw(p, buf | nil, count) }`; `buf.read(nil) -> f32s` `buf.write(b)` `g.pixels() -> u8s` (headless); `.free()`. Shaders in fut: `gpu fn f(uv: vec2f, t: num): vec4f { ... }` runs on the CPU as usual and on the GPU via `g.map(f, f32s | storageBuf, [args])` (`f(x: T, ...): T`, T num/vec2f/vec4f, in place) or `g.frame(rgb).paint(f, [args])` (`f(uv: vec2f, ...): vec4f`, uv (0,0) top left to (1,1)); `wgsl(f) -> {wgsl, entry, names, types, result}`. A gpu fn's parameters (all typed) and result are num, bool, vec2f, vec3f, vec4f; its body: var, assignment, if/else, while, for i in a..b, return, break, continue, operators, `v.x`/swizzles, calls of gpu fns of its file, sqrt floor int vec2f/3f/4f, std/math's (sin cos ... clamp, math.pi), `v.length() v.dot(w) v.normalize() v.cross(w)`; no strings, lists, maps, lambdas, globals (pass as params), recursion, bitwise ops, or `v.x = ...` (`v = vec2f(...)`) [gpu]
- `state x = v` (top level) / `state f` (class field): a signal read as `x`, set by `x = v`/`x += v`. `view V(a) { ... }`: fn returning nodes; body: `row .c .on: cond attr: v { text "x{y}" button "b" on click { ... } check "l" bind: s  if c { } else { }  for t in ts key t.id { Row(t) } }`; holes stay current. `mount(V)`: into #ui or body, with the theme. Tags: any HTML or SVG, row column text field check link (`link to: "/p" "Text"`). A call on its own line is a child; `state` in a view or fn is its own (a reload keeps it: by fn and name, in the order made; the dev page lists each state live: `x`, `View.x`, `View.x#2`; std/reactive: watch(fn(name, s, gone))); `state x = e` uses e if it's a signal. std/ui: `load(url|fn)` -> data() error() loading() reload(); every(s, f) after(s, f); path() go(p) at("/x/:id"); persist(key, v); lineChart barChart sparkline svg; form(values, rules) -> at(name) submit(f) valid() values() reset() (tags field/choice/area show errors); `dialog bind: s`; notify(text); rows(items, height, view, props); `table rows: l columns: [{key label num value show}] search: q on pick { e }` (ui.table: sorts by heading); `tabs bind: s { tab "A" { } }` (ui.tabs); `menu "M" { button ... }` (ui.menu, a popover); `files bind: s accept: "image/*" multiple: true { }` (chosen or dropped: s = [{name size type file}]); readText(f) readUrl(f) upload(url, f|{k: f}) -> Task: data() error() loading() progress() stop(); preview(f); size(bytes). An attribute's value may be `fn(x) { ... }`

```fut
import "std/lists"
import "std/strings"
expect lists.sort([3, 1, 2]) == [1, 2, 3]
expect strings.replace("a-b", "-", "+") == "a+b"
```

## Working on fut code

- Find code with `fut map --pub`; read a package's `.api`, not its source, to use it.
- Type and declare effects on `pub` functions; keep `pub` small (private code is free to change).
- Contract first: signature, `requires`/`ensures` (one per line), `todo()` body; then tests; then the body.
- Errors end with a stable code (`index 3 out of range (len 1) [index]`); key on the code, not the wording. A caught error is the message alone.
- Before editing a declaration you read earlier, check its `fut map --json` hash is unchanged.
- Before finishing: `fut test` passes. If an `.api` changed, you changed an interface: `fut api`, commit the `.api`, bump the version if something was removed or changed.
