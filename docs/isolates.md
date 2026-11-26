# Isolates and channels

Status: phases 1 (the runtime split per isolate) and 2 (spawn, join, channels, one thread per isolate) are done. The README has the language as built; `src/isolate.cpp` has the runtime. Phase 3 (the scheduler) is next. This is how fut gets parallelism: many isolates run at once, each owning its own heap,
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

- Each isolate is a Web Worker (or a Node worker thread) instantiating the same module over one shared
  `WebAssembly.Memory`.
- **The VM block.** JIT wasm code reaches the VM block by absolute address today (17 sites of `slotOf(&vm...)` in
  `gen_wasm.h`). Each isolate's block lives at a different address, so those sites load the base from a per-instance
  wasm global and add an offset. This should cost little: V8 keeps such a global in a register.
- **JIT modules.** Each one is instantiated in every worker, because each worker has its own function table
  (shared tables aren't standardized). A compiled `WebAssembly.Module` can be posted to workers. Reload installs
  the new module in each worker at its stop.
- **Blocking.** `Atomics.wait` works in workers and in Node, but not on a browser's main thread. There, the main
  isolate's `recv` suspends through JSPI, as fibers already do.
- **Headers.** Shared memory requires a cross-origin-isolated page (COOP/COEP). `web/serve.mjs` will send them.

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
3. **The scheduler.**
   - Work: isolate stacks, the thread pool, preemption at polls, and deterministic `fut test` with `--seed`.
4. **Hot reload across isolates.**
   - Work: stop the world, then the two-phase commit.
   - Test: the hot-reload suite with workers running.
5. **Wasm workers.**
   - Work: the VM base global, per-worker instantiation, JSPI on the main thread, COOP/COEP.
6. **Shared buffers and `parallel for`, for C-like throughput.**
   - Work: typed buffers (`f64buffer(n)` and the like) live outside every heap, so all isolates read and write them
     directly. Sending a buffer moves its ownership instead of copying it.
   - Work: `parallel for i in a..b { }` splits the range over the pool. The compiler checks that the body writes
     shared buffers only at index `i`, and writes no globals. Any other write is a compile error, so the loop is
     race-free by construction.
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
