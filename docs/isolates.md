# Isolates and channels

Status: phases 1 (the runtime split per isolate), 2 (spawn, join, channels), 3 (the scheduler: isolate stacks, a thread pool with preemption, seeded runs) and 4 (hot reload across isolates) are done, and so is 5 (isolates in wasm: on one thread in fut.wasm, in parallel on workers in fut-mt.wasm). The README has the language as built; `src/isolate.cpp` has the runtime. and in a browser page (every thread a worker, the main isolate's too; JavaScript a module declares `main` runs on the page's thread). This is how fut gets parallelism: many isolates run at once, each owning its own heap,
and they share data only by sending it over channels. There is no shared mutable memory, so there are no locks,
no memory orderings and no data races to write. Read this before you add any global state to the runtime.

## The language

```
fn crunch(jobs, results) ! chan {
  for job in jobs { results.send(score(job)) }
}

fn main() {
  var jobs = channel(64)
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

- **`spawn f(args)`** starts an isolate running `f(args)` in parallel and returns a task. `f` is any function value,
  including a closure. It is a keyword rather than a function so the compiler can see it (for effects, and for
  the checks below).
- **`task.join()`** waits for the task and returns `f`'s result, copied over. If `f` raised an error, `join` raises
  it again in the joiner. The error's trace includes the isolate's own frames and the line of the `spawn`.
  `task.done()` reports whether it has finished, without waiting.
- **`channel(n)`** makes a channel holding up to `n` values. The capacity is required: an explicit bound makes
  the back-pressure a choice the code shows.
  - `send` blocks while the channel is full, and `recv` while it is empty.
  - `close()` ends a channel: `recv` on a closed, drained channel raises, and `for v in ch` ends.
  - `ch.tryRecv()` returns `nil` instead of blocking when the channel is empty.
  - A bounded default gives back-pressure. An unbounded channel is the usual way a program runs out of memory
    under load.
- **What a send copies.** Values are copied into the receiver's heap. Sharing and cycles inside a value are
  preserved: a list holding the same object twice arrives holding one copy twice.
  - Numbers, booleans and `nil` are copied as they are.
  - Strings, lists, maps, instances and closures (their captured values) are copied deeply.
  - Channels and tasks travel by reference: they are the shared things.
  - A fiber or a native handle can't be sent; trying raises "cannot send a fiber".
- **Globals belong to each isolate.** A spawned isolate starts with a copy of the spawner's globals as they were at
  `spawn`. No global is ever shared, so isolates can't see each other's writes.
- **Effects.** fut already checks declared effects. Isolates add two:
  - `spawn`, for starting isolates;
  - `chan`, for every operation on a channel or a task (`send`, `recv`, `tryRecv`, `close`, `join`, `done`).

  A function declared `! io` that blocks on a channel is then an error, so a function's signature says whether
  it can block. This is the main way fut's design differs from Go's, where any call might block.
- **Transactions.**
  - A `send` inside `atomic { }` is held until the transaction commits, and dropped if it rolls back. Other
    isolates never see a message from a transaction that didn't happen.
  - `recv` and `join` inside `atomic` are errors: blocking would hold the transaction open. They are runtime
    errors, since the compiler can't tell a channel's `recv` from any other method's. `spawn` there is an error
    too: a started isolate can't be taken back.
- **Deadlocks are errors.** If every isolate is blocked on a channel or a join that nothing can satisfy, the
  program stops. It reports each isolate, what it waits for, and on which line. This is like Go's "all goroutines
  are asleep", but with the waits named.
- **Deterministic tests.** `fut test` runs all isolates on one thread under a seeded scheduler, which can switch
  isolates at any channel operation or safepoint. A failing test prints its seed, and `--seed` replays the same
  interleaving exactly. Concurrency bugs found by one agent can then be replayed by another.

## Why isolates, not shared-memory threads

- Shared memory would make every object store, inline-cache fill and class-layout change a potential race. That
  means atomic instructions or locks throughout the runtime, and code that is slower even when single-threaded.
- The conservative GC would have to stop every thread and scan every stack: a global pause on each collection.
  With isolates, each heap is collected alone, and the others keep running.
- Users would get data races, and agents would write racy code that passes its tests. Removing the possibility
  is fut's whole approach: `atomic`, effects and privacy all exist so that code written in parallel by many agents
  stays safe.
- Bulk data that has to be shared for speed gets its own mechanism (shared buffers, phase 6). It doesn't need
  shared objects.

## The runtime today, and what has to change

fut has one mutator. These are its global states, and where each goes:

| State | Today | With isolates |
|---|---|---|
| VM block: allocation runs, globals, inline caches, `handler`, `txn`, effect state, `poll` | the global `vm`, reached by JIT code through x28 | **one per isolate.** Native JIT code needs no change: each thread's x28 points at its isolate's block. The C++ runtime reaches it through a thread-local `Isolate*` (see below). |
| Heap: pages, `live`/`marks` bitmaps, runs, free-page pool | globals in `gc.cpp` | **one region, page owners.** Each page belongs to one isolate, or to the image. An isolate allocates, marks and collects only its own pages. The bitmaps can stay shared arrays, because a page's bits never share a 64-bit word with another page's. The page pool and the bump pointer take a lock (only when a page is needed). |
| Constants, `Fn`s, classes, interned strings: everything compiled code points at | heap objects, rooted forever in `consts` | **the image**: pages owned by no isolate, never collected (as `consts` are today). The compiler allocates there. They are immutable, except during a reload, when every isolate is stopped. |
| Class layouts (field name -> slot) | grow on the first store of a new field (`rt_setf`, `reshape`) | **shared, append-only.** A field's slot never changes, so per-isolate inline caches stay valid. Growth takes the class's lock. Readers see a chunked array with an atomic length, so they never lock. `reshape` publishes the allocation word before `ihdr`, and JIT constructors read `ihdr` with acquire (`ldar`), so a constructor never sees a header without its cell size. |
| Runtime-made strings (`mapKey`, single characters) | cached in `consts` | per-isolate caches |
| Symbol table | interned by the compiler, read by the runtime | written only by the compiler (at reload: stopped). Run-time interning, if any remains, takes a lock. |
| Fibers, transaction journal (`levels`), `errJmp`, `stackTop`, the main stack's limit and handler, `outSink`/`errSink` | globals | per isolate |
| JIT code, stubs, `tiers` | global | shared. Patched only during a reload, when all isolates are stopped. `pthread_jit_write_protect_np` is per thread on macOS, so only the compiling thread sees code as writable. |
| Tier-up | `rt_hot` compiles in place | compiling takes a global compile lock. Another isolate that hits the same hot function waits or skips. Installing new code is a stub patch: one aligned 4-byte store, which every core sees atomically. |
| Hot reload | at a safepoint of the one thread | stop the world, then commit in two phases (below) |

**Reaching the current isolate from C++.** On macOS a `thread_local` costs a call through a TLV descriptor on each
access. Most helpers don't touch the VM at all; the ones that do (allocation slow paths, `touch`, effects) get the
`Isolate*` once per call. If measuring shows that is too slow, hot helpers can take the VM block as an argument:
JIT code always has it in x28. Measured after phase 1: `maps` (string keys, about five thread-local reads per loop
iteration, in `rt_geti`, `rt_seti` and `gcAlloc`) runs about 1.5% more instructions for them; `nbody`, `fib` and
`mandelbrot` don't change.

**Conservative scanning.** An isolate scans only its own stacks. A word on its stack that happens to point into
another isolate's page is ignored: `find` checks the page's owner. Otherwise one isolate would mark another's
bitmap while that one runs.

**Copying a value.** `send` serializes the value into a malloc'd message: a flat buffer with back-references for
shared objects, built with a pointer-to-index map. `recv` rebuilds it in the receiver's heap. Copying at `send`
lets the sender change its objects right away, and keeps the sender's heap out of the receiver's reach. Image
objects (`Fn`, classes, constant strings) are sent as pointers. Copying globals at `spawn` uses the same
serializer, as one message, so values shared between globals stay shared. Copying them only when the new isolate
first reads them would spare a worker that never reads a large global. But a global load is a single `ldr` today,
and that would add a check to every one. So phase 2 copies eagerly and measures first.

## Scheduling

- **Phase 2:** one OS thread per isolate. `recv`, `send` and `join` block on a condition variable. This is simple
  and correct, but thousands of isolates mean thousands of threads.
- **Phase 3:** an isolate's main stack becomes a fiber-style stack of its own, which the existing switching code
  in `jit.cpp` can switch. A pool of `cores()` threads then runs a queue of isolates.
  - An isolate that blocks gives up its thread.
  - A long-running isolate is preempted at its next poll: a timer sets its `vm.poll`, and the poll path yields.
  - The deterministic test scheduler is this same pool with one thread and seeded choices, so phase 3 builds both.
- **Fibers inside an isolate** are unchanged: an isolate's fibers run only on that isolate. A fiber that blocks on
  a channel blocks its whole isolate, as a blocking call does today.

## Hot reload across isolates

Code is shared, so a reload must reach every isolate at once:

1. Request a stop: set every isolate's `vm.poll`. A running isolate stops at its next poll. A blocked isolate
   counts as stopped, and when it wakes it checks for a stop before it returns to JIT code.
2. Compile once, then patch the stubs and the image. This is the same commit that reloads do today.
3. Each isolate runs the new top level and `reload()` inside its own transaction. This can happen in parallel,
   because each touches only its own heap.
4. If every isolate succeeded, all commit. If any failed, all roll back, and the code is reverted with the
   existing reload rollback. Then every isolate resumes.

## Wasm

- Each pool thread is a Web Worker (or a Node worker thread) instantiating the same module over one shared
  `WebAssembly.Memory` (done in step two, in Node).
- **The VM block.** Done in step one: JIT wasm code reads the running isolate's VM block from wasm global 1 once per
  frame, into a local, and reaches its fields as offsets from it.
- **JIT modules.** Each one is instantiated in every worker, because each worker has its own function table
  (shared tables aren't standardized). Done in step two: a log of every table change, which each thread replays.
- **Blocking.** `Atomics.wait` works in workers and in Node, but not on a browser's main thread. Done in step three
  as Emscripten's PROXY_TO_PTHREAD does it: the main isolate runs on a worker too, and the page only serves them.
- **Headers.** Shared memory requires a cross-origin-isolated page (COOP/COEP). `web/serve.mjs` sends them.

## Phases

Each phase lands on its own, with every suite passing natively, in wasm and under debug/ASan.

1. **Split the runtime per isolate, with no language change.**
   - Work: the `Isolate` struct; per-isolate VM blocks, heaps, fibers, journals and error state; the image; page
     owners; class-layout publication; locks on the shared tables.
   - Test: a mode that runs every spec in N isolates at once in one process, under ThreadSanitizer. Any state still
     shared shows up as a race. Done: `fut test --isolates N` and `fut --isolates N prog.fut`; `build.py test` runs
     every spec in 4 isolates, and `build.py tsan [N]` does the same under ThreadSanitizer. TSan sees only the C++
     runtime: JIT code isn't instrumented, which is why the VM block, inline caches and heaps are per isolate rather
     than locked.
2. **`spawn`, `join`, channels, copying, the `spawn`/`chan` effects, sends deferred in `atomic`, deadlock reports.**
   Done.
   - Work: one thread per isolate.
   - Test: `tests/specs/isolates.fut` (also in N isolates at once, and under TSan), and a parallel mandelbrot in
     `bench/`, against C on 8 threads.
3. **The scheduler.** Done.
   - Work: isolate stacks, the thread pool, preemption at polls, and deterministic `fut test` with `--seed`.
   - Test: thousands of isolates; more busy isolates than threads, which finish only if they are preempted; the same
     seed giving the same interleaving and other seeds others; a failed test naming its seed.
4. **Hot reload across isolates.** Done.
   - Work: stop the world, then the two-phase commit.
   - Test: the hot-reload suite with workers running (`build.py`'s `hot_isolates`).
5. **Wasm workers.** Done.
   - Step one: isolates in wasm on one thread (the VM base global; contexts switched with JSPI; seeded).
   - Step two, in Node: per-worker instantiation over shared memory (fut-mt.wasm, a threads build), the table log.
   - Step three: the browser (every thread a worker, as Emscripten's PROXY_TO_PTHREAD; COOP/COEP; opt-in proxying).
   - Step four: reloads with isolates running, in fut-mt.wasm (Node and the browser), as phase 4 does natively.
6. **Shared buffers and `parallel for`, for C-like throughput.**
   - Step one, done: buffers (`f64s`, `f32s`, `u32s`, `i32s`, `u8s`: main's packed buffers, plus f64) live outside every heap, and
     compiled code indexes them inline. Sending a buffer moves its ownership instead of copying it.
   - Step two, done: `parallel for i in a..b { }` splits the range over the pool. Its result is the same as a plain `for`'s
     on every run, machine and core count: the body can't assign variables from outside it (a compile error); a
     buffer it writes it may reach only at index `i` (checked through the handle, so also in functions it calls),
     and the others only read; values it captures arrive as frozen copies, so changing one raises; and it runs with
     no effects (no print, random, sleep or channels).
7. **Frozen values and shared state.**
   - Work: `freeze(v)` makes a deep-immutable value that is sent as a pointer, not copied.
   - Work: `shared` objects that can be changed only inside `atomic`, which becomes cross-isolate software
     transactional memory with a version clock, retried on conflict. This is for the rare state that really is
     global, such as a cache or a counter.

## Decisions

- **Globals at `spawn`:** copied, rather than rerunning the top level in each isolate. Rerunning would
  repeat the top level's side effects, such as prints and file writes, and give each isolate a different starting
  state from the spawner's.
- **Channel capacity** is required (`channel(n)`), and `channel(0)` is a rendezvous.
- **`spawn` is a contextual keyword** (`spawn f(args)`), compiled to a call of the runtime's `spawn@(f, [args])`.
  It stays usable as a name.

## Phase 2 as built

- **One lock** (`chanMu`) guards every channel's queue, every task, and the table of isolates. A message is encoded
  before it is taken and decoded after it is released, so it is held only to move a message in or out.
- **Programs.** Each isolate belongs to a program: a root (the main thread, one of `--isolates N`, a test's process)
  and everything spawned from it. A program counts its live and waiting isolates; when the last one waits, the
  deadlock report goes to each. A root's `isolatesFinish` waits for the rest when its code ends. If the code
  failed, it first stops them: they raise at their next poll (their `vm->poll` is set) or wait.
- **Failures nobody joined** are reported when the program ends. A failure caused by a deadlock or a stop is not
  reported again.
- **Handles.** Each isolate has one Chan or Task object per channel or task it holds (a per-isolate table), so the
  same channel is the same value however it arrived. Shared state is reference-counted; the GC drops a handle's
  reference when the handle dies. A channel that holds a message holding itself is never freed (a cycle of counts).
- **Printing.** A worker prints where its spawner's `print` goes outside any `expect prints { }` capture: stdout, or
  the test's output (`outBase`, appended under a lock).
- **Tests fork.** `fut test` runs each test in a forked process. The top level's isolates are finished before the
  tests fork, because a child process has only the thread that forked.
- **Speed.** `bench/mandelbrot_par.fut` on 8 isolates: 57 ms, against 453 ms on one isolate and 16 ms for C on 8
  threads. The gap to C is the single-threaded one (the optimizing tier's job); the scaling is near-linear.

## Phase 3 as built

- **Isolate-local state.** The runtime's per-isolate variables stay thread-locals, each registered with
  `ISO_LOCAL` (fut.h). Switching isolates on a thread swaps every registered variable with the isolate's saved copy,
  about 35 swaps, and switches stacks with the fibers' `fiberSwap`. A new isolate starts with each variable as it
  was before `main()`. Thread-locals that belong to the thread itself (JIT write mode, `locksHeld`) aren't registered.
- **Stacks.** A spawned isolate gets an 8 MB stack (reserved, with a guard page; the main thread's size). Stacks of
  ended isolates are kept for reuse, up to 64. ASan and TSan are told about each switch (their fiber APIs), so they
  follow an isolate's frames from stack to stack.
- **The pool.** Up to `cores()` threads (`--threads n`), started as isolates are spawned. Each spawned isolate goes to the thread
  with the fewest and **stays on it**. Moving an isolate between threads would be wrong: a C++ frame on its stack
  (say `take()`, waiting in `recv`) may have computed a thread-local's address before the switch, since the compiler
  treats that address as fixed within a function, and would go on using the old thread's. Moving isolates is
  possible later if the frames that can be on a switched-out stack read no thread-local after the switch. Until
  then, a thread whose isolates all wait sits idle while another has a queue.
- **Preemption.** A ticker thread wakes every 10 ms. A pool thread's isolate that has run a whole slice while
  another of that thread's waits gets bit 8 of `vm->poll`. `rt_poll` then puts it at the back of its thread's queue,
  unless it holds a lock (`locksHeld`: compiling, say).
- **The root keeps its thread.** `main()` may own the window (macOS wants the main thread), so a pool program's root
  never moves; its waits block its thread on a condition variable, as in phase 2.
- **Seeded.** Under `fut test` (seed 1 unless `--seed n`) or `fut --seed n`, a program's isolates all run on its
  root's thread. At every channel or task operation, `spawn` and `print`, a xorshift generator picks which runnable
  isolate goes on (the one at the point included). An isolate that waits hands the thread straight to the next
  pick. Between those points, isolates share nothing, so the picks determine everything the program can observe.
  The deadlock check is unchanged: when every isolate waits, there is nothing left to pick.
- **Switching holds no lock.** A pthread mutex belongs to a thread, and TSan tracks it per fiber, so a switch never
  carries `chanMu` across. An isolate marks itself parked or runnable under the lock, releases it, then switches.
  Whoever resumes it next waits for `onStack` to clear, which the side it switched to sets once it is off the stack.
- **Speed.** `bench/mandelbrot_par.fut` on 8 isolates is as fast as with a thread each (about 55 ms, against 372 ms
  on one). 3000 isolates spawned and joined take 0.17 s.

## Phase 4 as built

- **Who reloads.** The main thread, as before: the watcher sets its poll bit, and `hotReload` runs at its next
  poll. If `main()` is waiting on a channel or a task, the watcher also wakes it (`isolateNudge`); it reloads from
  inside the wait and then waits again. A line typed into the REPL is compiled with the others stopped (it may
  define functions they call), then runs on the main isolate only, with the others going again: it may wait on them.
- **Stop.** `isolatesStop` sets bit 16 of every other isolate's `vm->poll`, then waits until none is running. A
  running isolate stops at its next poll; one inside `atomic` stops at the first poll after the block. An isolate
  waiting on a channel is already stopped; if a send wakes it meanwhile, it stops before returning to fut code
  (`reloadPoint` runs wherever a pool isolate resumes: in `park`, `isolateYield` and `isoEntry`). Pool threads take
  no new work while a stop is on. `-w` always uses the pool: a seeded program's isolates share the main thread, so
  they could not be stopped at their own polls.
- **Patch, once.** The main thread compiles and patches the stubs as before. The compiled functions and classes
  that globals hold are copied into each stopped isolate's VM block (`isolatesSync`), since each has its own
  globals.
- **Two-phase commit.** The main thread runs the new top level (initializers of new and edited vars) and
  `reload()` in a transaction. Then `isolatesParts` has every other isolate do the same in a transaction of its
  own, on its own thread, all in parallel; each then waits. If all succeeded, `isolatesVerdict(true)` commits each
  transaction. If any failed, the main thread rolls back its own and reverts the code, then copies the old
  functions back into the isolates' globals, and `isolatesVerdict(false)` has each roll back. The first failure is
  printed as `[fut] reload failed in isolate N: ...`, with its trace.
- **Limits.** Code replaced while other isolates exist is kept until the program is down to one isolate:
  `jitCollect` looks only at the reloading thread's stacks, so it can't tell whether a stopped isolate's frame
  will return into old code. Walking the stopped isolates' stacks (and their fibers') would allow freeing it.

## Phase 5, step one as built: isolates in wasm, on one thread

- **Seeded, always.** Wasm has one thread here, so every program's isolates take turns on it as a seeded program's
  do natively (`--seed n`, default 1): at each channel or task operation, `spawn` and `print`, the generator picks
  which runnable isolate goes on. A wasm program interleaves the same way every run, in `fut test` or not. An
  isolate that never reaches such a point keeps the thread (there is no preemption on one thread).
- **Contexts.** Each isolate is a wasm stack of its own: `ctxSwap` saves its shadow stack pointer and calls the
  host's suspending `ctx_switch`, which parks the running stack under that name and, once it has suspended
  (a microtask), resumes the next, or starts it (a promising call of `fut_ctx_start`, which runs `isoEntry` on a
  fresh 256 KB shadow stack). Switching is never nested: each switch suspends one stack and resumes another from
  the event loop. ISO_LOCAL works as natively; wasm.cpp's `mainLimit` and `interpTops` and interp.cpp's
  `interpTop` are isolate-local too. `vmSwitched` tells the host the new VM block after each switch.
- **Errors.** A fut error is a wasm exception, which C++ can't catch, so `isoEntry` runs the spawned call under
  the host's `try_call` (`catchFut`; instantiated on first use, as the interpreter's already was) and asks the host
  for the error's trace. `spawn` records its site from the engine's stack (`hereWhere`), since wasm has no return
  addresses; a deadlock report names each wait, but not where it is.
- **Ending.** fut.js waits for a program's isolates when it ends, and after each test (`fut_isolates_finish`, a
  promising call), as main.cpp does natively.
- **Limits.** A file reload under `-w` waits until the isolates the program started have ended: other isolates
  share its code but have globals of their own, which nothing copies into them yet (natively, phase 4 does; in
  fut-mt.wasm, step four). A call that waits on the host (`sleep`, `fetch`) holds up every isolate.
- **Cloudflare Workers.** A Worker can't compile code, so every bundle ships `try_call` compiled (`try.wasm`), and
  `run()` there takes `--seed n` (and `--threads n`, which does nothing on one thread). `tests/specs/isolates.fut`
  passes in a bundle compiled ahead of time (`build.py`'s Worker check runs it).

## Phase 5, step two as built: isolates in parallel, on workers

- **Two builds.** fut.wasm stays as step one left it (one thread: Cloudflare Workers can't share memory). fut-mt.wasm
  is the same sources for `wasm32-wasip1-threads` with `-pthread` (`build.py wasm --threads`, `test --wasm-mt`):
  `ONE_THREAD` (fut.h) is false there, so the locks are pthread mutexes, `TLS` is `thread_local`, and isolate.cpp's
  pool, ticker and pinned root are the native ones. Programs are pooled by default, as natively, and `fut test` is
  seeded (by 1). `web/run.mjs` takes the build from `FUT_WASM`.
- **Threads.** `pthread_create` asks the host for a thread (wasi `thread-spawn`). In Node that is a worker
  (`web/thread.mjs`) with its own instance of fut-mt.wasm over the one shared memory, which runs `wasi_thread_start`
  as a promising call: a pool thread's loop (whose isolates switch with JSPI, as in step one) or the ticker. Each
  thread's host has the options `web/node.mjs` makes, which `run.mjs` uses too (files, output, child processes), so
  output from any thread goes straight to the file descriptor. The root runs on the main thread, and its waits block
  it (`Atomics.wait` is allowed in Node's main thread).
- **The table log.** Each thread has its own function table, and every table must hold the same function in the same
  slot. Every change (a module installed, the table grown, `try_call` added) is made by a thread holding `codeMu`,
  which first replays what it has yet to, and then logged with the module's bytes. Another thread replays the log
  (`codeSync`) before it could reach what was added: when it switches isolates, decodes a message (which may hold a
  function compiled since), adds `try_call`, or reaches a safepoint. Tier 2 code compiled on one thread sets bit 32
  of every other isolate's `vm->poll`, so each adopts it at its next poll. Until then the old body is reached directly
  (a call compiled before), and its replaced cell, in shared memory, sends the call through the stub: so it polls
  first, or this thread's stub slot could still hold that body, and the call would come straight back to it
  (`tests/isolates_tierup.fut`). A thread instantiates each module from the logged bytes itself; the bytes are kept
  for threads not started yet.
- **The image.** Compiled code's constants, functions and classes now go to the image in wasm too (as natively): a
  tier-2 compile on a spawned isolate had put them in that isolate's heap, which is freed when it ends. A compile
  error longjmps past `ImageScope` and `Locked`, so where it lands puts their state back (`Guard` in wasm.cpp).
- **Memory growth.** When one thread grows shared memory, V8 tells the other threads' instances (and their
  JavaScript) asynchronously; until they hear, the new memory is out of bounds to them, and a block `malloc` handed
  out just after a growth on another thread trapped. fut-mt.wasm starts with 768 MB of memory (reserved; pages are
  touched as used), and replaces the C library's `sbrk` with one that grows 64 MB ahead of use, so what `malloc`
  hands out was grown long before. Not always: a block larger than 64 MB, or the first block past the initial memory,
  is new memory, and a thread it is sent to at once can still fault on it.
- **Ending.** A worker can't end the process (in Node, `process.exit` there ends only the worker), and the C library's
  `exit` runs handlers under locks the main thread may need. So `exit` is replaced too: on a worker it asks the main
  thread to end the process (`isolatesExit`: bit 64 of every `vm->poll`, and a wake of the root's wait) and waits;
  on the main thread it flushes output and ends. A worker that ends this way (or traps) first lets go of the locks it
  holds, since the main thread may need one on the way to its exit check; the check itself takes none. Only the
  bits a safepoint handles are cleared there (`fetch_and`), so a stop or an exit another thread asks for meanwhile
  is not lost.
- **Limits.**
  - Extern functions (JavaScript's) and JavaScript values were the main thread's: an isolate on a worker that called
    one got an error saying so. Step three runs them where they are called.
  - A host call that waits (`sleep`, `fetch`) holds up the other isolates of its thread, as it does on one thread.
  - Deep recursion runs out of V8's stack (984 KB for a JSPI stack) before fut's own check, so it ends the program
    rather than failing the isolate. The same holds on fut.wasm's main isolate.
  - V8 has no on-stack replacement for wasm: a long loop entered while V8 still runs a module's baseline code stays in
    it, so timings vary between runs (up to 10x on a loop of 10^8 iterations), on one thread or many.
- **Speed.** Eight isolates each summing 10^8 numbers: 0.95 s on 8 threads, 2.1 s on 2, and 3.3 s in fut.wasm on one
  thread (natively: 0.26 s). `tests/isolates_busy.fut` (12 isolates collecting, compiling tier 2 and sending closures
  at once) runs in every build's suite.

## Phase 5, step three as built: the browser

- **Every thread a worker.** A page's own thread can't wait (`Atomics.wait` throws there, and so does wasm's
  `memory.atomic.wait32`), and fut's threads wait: on locks, channels, `join`. So, as Emscripten's PROXY_TO_PTHREAD
  does, the program's root (its main isolate, the REPL, reloads) runs on a worker too (`web/fut-worker.js`), and the
  page (`web/browser.js`) serves the workers: it shows their output, draws what they `present()`, sends keys, mouse,
  typed lines and changed files to the root, and starts the threads they ask for. It must start them: a browser
  starts a worker only once the thread that made it is free, and a thread asking for one may then wait on it.
  `web/index.html` runs fut-mt.wasm this way when the page is cross-origin isolated (`web/serve.mjs` sends COOP and
  COEP), fut.wasm on its own thread otherwise or with `?threads=0`.
- **JavaScript runs where it is called** (Emscripten's default too). Each thread loads a package's modules itself
  when its isolates first call them: fut.wasm binds an extern to an index on the thread that compiled it, and
  another thread looks the index up (`fut_extern_name`) and binds it the same. Handles (`Ref`) are the thread's:
  `jsval.cpp`'s `refs` is thread-local, and pinned fut functions are the process's, under a lock. A fut function
  JavaScript calls later (not during an extern call) runs on the main isolate, so on a pool thread that's an error.
- **Proxying, opt-in.** A module declared `main` in `fut.pkg` (`js dom ./dom.js main`, as std/ui's is), and a global
  this thread doesn't have but the page does (`document`, `alert`), run on the page's thread. The calling thread
  writes the call (its arguments as text, `any` and `fn` as their JSON) to a channel it shares with the page, rings
  it (a message), and waits on the channel's state word. The page runs it and writes the result back. If the page's
  JavaScript calls a fut function meanwhile (as `Array.map` does), the page writes that call to the channel and
  spins until the worker, still waiting there, has run it (as a callback during an extern call) and written its
  result; a call to the page from inside it is served during the spin. One the page calls later (a click's
  handler) goes to the root as a message, which also sets the root's poll bit in the shared memory, so a busy root
  reads its messages at its next safepoint. The page's own values cross as handles with ids from 2^31; it frees
  one when every thread that held it has let it go. `fut.js`'s `proxy` option (`?proxy=` on the page) picks what is
  proxied: `declared` (the default), `all` (every module and global function but the small libc and Math), `none`.
- **Order.** Events the page sends while a callback runs wait until it returns: a host poll reads the page's
  messages only when it isn't inside one. std/ui's `field` doesn't write back the value its box last reported, since
  events are handled after the typing that follows them.
- **Limits.**
  - The main isolate can't run on the page's own thread (spinning instead of waiting, suspending through JSPI for
    long waits): the C library's locks (malloc's) wait too, and Emscripten patches its C library for that.
  - A module's state is per thread; a module that keeps state across isolates must be declared `main`.
  - Keys and the mouse reach a busy root when it next yields to the page (`present()`), as on one thread. (A root
    waiting on a channel or a task looks at its poll bit every 20 ms: see step four.)
- **Speed.** Eight isolates each looping 2*10^7 times: 120 to 250 ms in Chrome on 12 threads, 710 to 780 ms in
  fut.wasm on the page's thread (natively: 75 ms).

## Phase 5, step four as built: reloads with isolates running

- **The same protocol as natively** (phase 4): fut-mt.wasm builds `isolatesStop`, `isolatesSync`, `isolatesParts`
  and `isolatesVerdict` (they were native only), and exports them (`fut_isolates_stop` and so on). `hostPoll` in
  `fut.js` does what main.cpp's `hotReload` does: stop the others (each at its next poll, bit 16, or where it
  waits), compile, copy the globals to them, run the new top level and `reload()` in a transaction on the root,
  then have each isolate run them in a transaction of its own; all commit, or all roll back with the code. A REPL
  line compiles with the others stopped too (`compileStopped`). Unseeded only: with `--seed`, or with no pool,
  every isolate is on the root's thread, and a reload waits until they have ended, as in fut.wasm.
- **A part's error.** Natively `partRun` catches it with `setjmp`; in wasm a fut error is a wasm exception, which
  `catchFut` (the host's `try_call`) catches.
- **A root that waits.** Natively a watcher thread signals a root waiting on a channel or a task
  (`isolateNudge`). In wasm the host's events can't signal a condition variable: they set the root's poll bit in
  the shared memory. So a root waits there 20 ms at a time, and when it finds bit 1 set runs its poll (`waitFor`).
  In Node the root is the main thread, and a waiting one runs no timers: `web/run.mjs`'s file watcher is a worker
  of its own that sets the bit and counts its message (`opts.pending`, as the page's are), which the root reads at
  that poll. A line typed on stdin still waits until the root next yields.
- **Code.** A retired body's table slot can be reused while another isolate is still inside it: its frame doesn't
  need the slot, and its direct calls are within its own module, so compiled code is safe. Interpreted bodies
  (`FUT_INTERP`) are kept until the program is down to one isolate, as native code is.
- **Test.** `build.py`'s `hot_isolates` runs on fut-mt.wasm too: three workers, a reload that fails in the workers
  only and rolls back everywhere, then one that gives each its own new var and `reload()` run, with main waiting in
  `recv()` all along. Checked by hand: isolates that never wait, reloaded three times in a row
  (Node and Chrome), and a REPL line redefining a function they call.

## Phase 6, step one as built: buffers

- **The object.** Main had packed buffers (`f32s`, `u32s`, `i32s`, `u8s`, for std/gpu and C: one kind, `K::Buf`,
  its element type in `Buf::et`), copied when sent. They became this phase's buffers, with `f64s` added: a handle
  (`Buf`, in an isolate's heap) on refcounted memory (`BufData`, malloc'd: in fut-mt.wasm, the shared memory). The
  handle's `p` and `n` are its view of the numbers; a dead handle's are null and 0. The collector finalizes a dead
  handle, which releases its reference (`src/buffer.cpp`).
- **Compiled code.** The inline `b[i]` of both code generators (`index()` in `gen_a64.h` and `gen_wasm.h`) checks
  the kind, the index against `n`, then branches on the element type: a load widens to a double (an f32 NaN goes to
  the helper, which boxes it), a store converts and checks the conversion was exact and in range (else the helper
  raises). Inside a transaction a store goes through the helper, which saves the buffer's bytes on its first write
  (`txn.cpp`).
- **Moving.** A message takes a reference on each buffer it carries (`Msg::bufs`); once the whole message is made,
  each sending handle goes dead (`bufSent`), so a failed encode leaves the sender as it was. `BufData::holder` names
  the isolate whose handles may use it (null while a message carries it): a task's result read by a second isolate
  raises rather than giving two isolates one buffer. A spawned isolate's copy of the globals gets dead handles for
  their buffers (`Writer::moving`). A send inside `atomic` raises: a rolled-back send would drop the buffer, its
  sender's handle already dead.
- **Speed.** Filling a million-element buffer and summing it 20 times, in a function: f64 42 ms, i32 51 ms, u8
  62 ms, a list 47 ms; C (clang -O2) 20 ms. (Measured before the merge with packed buffers; on a loaded machine.)
- **Not yet.** Passing buffers to JavaScript as typed arrays (they go as arrays of numbers).

## Phase 6, step two as built: parallel for

- **Compiling.** `parallel` is a contextual keyword before `for`. The body compiles as a lambda of `i` (named
  `parallel@<line>`), and the loop as a call of `parallel@(a, b, body, writes)` (`src/isolate.cpp`). While compiling
  the body, an assignment to a captured or global variable, a `return`, and a `break` or `continue` that would leave
  it are compile errors. Each `name[...] = ` whose `name` is a captured variable or a global is recorded, as the
  closure slot or the global slot and the name: that is `writes`.
- **Starting.** `parallel@` checks each written variable holds a buffer, encodes one message for every worker (the
  body's closure, then the globals) with buffers lent (`Writer::lending`: a reference each, the sender's handle
  stays live), marks the written ones (`BufData::writing`), and starts min(cores, n) isolates, which run `parWork`
  instead of a call (`TaskState::job`). It waits for all of them, unmarks, and raises the error, if any.
- **What a worker sees.** It decodes the message with `Reader::freeze`: every list, map and instance gets
  `FROZEN` (a bit of `Obj::typed`). The inline stores compare the kind and `typed` together, and the field cache's
  key holds `typed`, so a store to a frozen object goes to its helper, where `touch()` raises (`touchSlow`). Reads
  stay inline. A lent buffer the body doesn't write is a `FROZEN` handle on the same memory: reads inline, stores
  raise. One it writes is a handle of length 0 (`gone == AT`), so compiled `b[i]` always misses to `bufGet`/`bufSet`,
  which allow only the index `parAt` (the iteration, isolate-local); `len()` works, iterating raises.
- **Effects.** A worker narrows `vm->allowed` to nothing, with `vm->who = WHO_PARALLEL` for the message. A global
  the body (or a function it calls) assigns is found after each block, by comparing with the globals it started
  from; that error names the block's range unless the block is one iteration.
- **Work and errors.** Iterations go out in blocks of n / (workers × 8) from an atomic counter. A block runs under
  its own catch (`tryRun`: setjmp natively, `catchFut` in wasm); a failure at i records itself if it is the lowest
  yet (`ParJob::failAt`, under a lock, with the error encoded and its code), and every worker skips iterations past
  the lowest failure. So the error raised is always the lowest failing i's, whatever ran first, though some
  iterations after it may have run. A hot reload during the loop: workers run no part (they hold only frozen copies)
  and commit or roll back an empty transaction with the rest.
- **Cost.** A new isolate was 55 µs natively, most of it setting all 64K global slots of its VM to nil; `vmNew` now
  sets only those in use, and a new slot is set in every VM as it is made (`newGlobalSlot`, as `newIC` does), so
  `spawn`/`join` is 17 µs and a `parallel for` over 12 workers costs about 0.2 ms more than its work. In
  fut-mt.wasm a worker costs about 140 µs (`calloc` of the whole VM block, 8 MB).
- **Speed.** A 1200×800 mandelbrot into a `u8s`, 12 cores (a loaded machine): 525 ms plain, 65–95 ms parallel
  natively; in fut-mt.wasm under Node, 635 ms and about 200 ms.
- **Not yet.** A persistent pool of workers (each loop starts its isolates), a cheaper VM block in wasm, and
  reductions (`parallel for` that sums): for now each iteration writes a buffer element and the sum is a plain loop.

