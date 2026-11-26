# An optimizing tier for fut

Status: design, not started. This is the plan for closing the gap to C on numeric code. Read this before changing
`tierUp`, the inline caches, or the frame layout.

## Why

`bench/run.py`, best of 5, M2 Max, against clang -O2:

| Benchmark   | C (ms) | fut (ms) | vs C  | What the time goes to |
|-------------|-------:|---------:|------:|-----------------------|
| fib         |    6.1 |     12.1 |  2.0x | call overhead, boxed returns |
| leibniz     |   45   |     92   |  2.0x | number checks on each operation, loop-carried values in and out of registers |
| nbody       |    7.1 |    136   |   19x | each field access is a full inline-cache check and two dependent loads; every value is reboxed between operations |
| sieve       |   21.7 |    119   |  5.5x | double -> integer conversion and a bounds check per access; the list's header is loaded again on each access |
| mandelbrot  |  103   |    367   |  3.6x | number checks; locals are proven numbers only until the next loop head |
| maps        |    165 |    263   |  1.6x | runtime map, mostly fine |
| binarytrees |    588 |    262   |  0.4x | bump allocation beats malloc |

The single-pass compiler has reached the limits of its design:

- It emits each operation as soon as it is parsed, so it can't move a check out of a loop. Its "facts" (locals
  proven to be numbers) are dropped at every loop head, because the back-edge hasn't been seen yet.
- Each operation is compiled alone, so a value is checked again at every use.
- It has no register allocator: temporaries go through x0/x1 and stack slots.
- It doesn't inline, so a small method still costs a full call through an inline cache.

Tier 2 today re-parses a hot function with its number arguments proven, behind an entry guard. That helps fib
(14 -> 11 ms), but measured no help on leibniz (347 ms cold, 342 ms after tier 2), because the loop's own values
are what need proving. On-stack replacement wouldn't help either, for the same reason.

## Goals

- nbody, mandelbrot, leibniz and sieve within 1.5-2x of C, fib within 1.3x, both natively and in wasm.
- No semantic change. Every program behaves exactly as under tier 1, including errors, their messages and their
  stack traces.
- Hot reload, `atomic`, effects, fibers and the conservative GC keep working unchanged.
- Small. Roughly 5k lines (the compiler is 5.8k today), in files of their own.
- Compile time stays small: optimizing a 200-line function should take well under a millisecond.

## Non-goals

- Escape analysis and allocation sinking. Bump allocation already makes binarytrees faster than C.
- Integer types in the language. Numbers stay doubles. The tier may represent values it can prove are int32s as
  integers internally (see "Representations").
- Threads. They are a separate design (isolates); this tier must not make them harder.

## Architecture

```
          source text
              |
    compile.cpp (the one parser)
     /        |          \
gen_a64.h  gen_wasm.h   gen_ir.h          <- new: a third code generator
 tier 1     tier 1         |
 (+ feedback, resume points)  SSA IR + frame states
                           |
                    opt passes (opt.cpp)
                     /          \
            lower_a64.cpp    lower_wasm.cpp
           (linear scan)    (typed wasm locals; V8 allocates registers)
```

### 1. Build the IR from the same parser

`compile.cpp` is generic over its code generator: `gen_a64.h` and `gen_wasm.h` implement the same interface
(`binop`, `field`, `index`, `ldl`/`stl`, `loopBegin`, `callWindow`, ...). The optimizing tier adds a third generator,
`gen_ir.h`, that builds SSA IR instead of emitting code.

- **One parser.** The language has one definition. Writing a second parser that builds an AST would duplicate every
  rule, and the two would drift apart.
- **SSA in one pass.** The interface already describes control flow in structured form (blocks, loops, if/else),
  because wasm needs it. That is exactly what single-pass SSA construction needs (Braun et al., "Simple and
  Efficient Construction of SSA Form", 2013). Each tier-1 storage location becomes an SSA variable: x0, x1, x2, the
  frame slots and each local's home. A loop head gets incomplete phis that are sealed at the back-edge.
- **Frame states for free.** This is the main reason. Tier 1 and the IR generator see the same sequence of
  generator calls. At every call boundary, tier 1's machine state is known exactly (what is in x0, x1, x2, the slots
  and the locals), and so are the SSA values the IR generator assigned to those same locations. A guard placed
  before an operation can therefore resume tier 1 at that same operation, with the same state rebuilt. See
  "Deoptimization".

The parser's own decisions (such as `simpleOperand` and the facts) must be the same in both runs, so that the
sequences of calls match. `tiering` currently changes a few of them. Those decisions move into the generator, or
become identical in both runs, and a debug check compares the sequences.

### 2. Feedback from tier 1

The optimizer speculates from what tier 1 has seen. Most of this is already in the inline caches:

| Site | Feedback | Today |
|------|----------|-------|
| field get/set | the class header seen (class id + slot count), the slot | in the IC (`field`) |
| method call | the receiver class, the target | in the IC (`callWindow`) |
| constructor | the class, the init code | in the IC (`constructCall`) |
| arithmetic, compare | operand kinds seen: number / string / other | **new**: 1 byte per site, set on the slow path only (the fast path already implies "number") |
| index get/set | list / map / string / other; index ever not an int32 | **new**: set on the slow path |
| function | which arguments were numbers at tier-up | `rt_hot` (already) |

Feedback is keyed by source offset. Tier 1 records, for each feedback site, the offset of the token it was compiled
from, and the IR generator looks the site up when it compiles the same token. Recording only on slow paths keeps
tier 1's fast paths as they are.

### 3. The IR

- A control-flow graph of basic blocks holding SSA instructions. A vector of instructions per function, with
  operands as indices: no pointers, no per-node allocation.
- **Types** form a lattice over representations and kinds: `any` (a boxed value); `f64` (a raw double: under NaN
  boxing it is the same bits, so unboxing a number is only a check); `i32` (proven integral and in range); `bool`;
  `obj<C>` (an instance of class C with a known slot count); `list`, `str`, `map`.
- **Operations:**
  - arithmetic and compares on `f64`/`i32`, and their generic forms (which call `rt_binop`);
  - `LoadSlot` / `StoreSlot` (instance slot k), `ListLen`, `ListData`, `LoadElem` / `StoreElem`;
  - `Call` (generic, through the IC), `CallDirect` (through the stub), `CallNative`;
  - `Box` / `Unbox`, `Poll`, `Return`, `Throw`.
- **Guards** fail into deoptimization. Each carries a frame state (below):
  - `CheckNum`, `CheckInt32`, `CheckClass(obj, header)`, `CheckList`, `CheckBounds(i, len)`;
  - `CheckNotAbsent` (a slot that was never assigned);
  - `CheckEpoch` (after calls and polls; see "Invalidation").
- **Effects.** Every instruction is pure, reads memory, writes memory or is a call. Passes may move only pure
  instructions and reads across instructions that write no memory. That is enough to hoist `ListData(l)` and
  `CheckClass(bi)` out of a loop that makes no calls and doesn't write those objects' headers.

### 4. Passes, in order

Each pass is short, and each is tested against tier 1 by running the specs.

1. **Speculation.** Replace generic operations with typed ones behind guards, from feedback. A `+` site that only
   ever saw numbers becomes `CheckNum(a)`, `CheckNum(b)`, `FAdd`.
2. **Inlining.** Small top-level functions and monomorphic methods, under a size budget, up to depth 3. Never
   functions with `try`, declared effects or `atomic` in phase 1; those come later.
3. **Type propagation and check elimination.** Forward dataflow over the lattice, through phis. This removes a
   guard whose input is already proven, and a check made earlier on every path to it. It is where loop-carried
   values become `f64`: once `s = 0.0` and `s = s + f64` are proven, the phi is `f64`.
4. **Loop-invariant code motion** for pure operations and guards. A guard hoisted out of a loop deoptimizes at the
   loop's entry, with the frame state from there.
5. **Representation selection.** Choose a register class for each value: `f64` in d-registers, `i32` in
   w-registers, `any` in x-registers. Box only at uses that need `any` (stores, generic calls, deoptimization).
6. **Global value numbering and dead-code elimination.**
7. **Bounds-check elimination** for `for i in 0..len(l)` loops that don't change `l`'s length, plus a range check
   when the loop starts. This is a later phase (sieve).

### 5. Lowering

- **arm64:** linear-scan register allocation over the CFG (Poletto and Sarkar, with lifetime intervals). It uses
  x19-x25 and d8-d15 across calls, and x9-x15 and d16-d31 as scratch. The calling convention stays tier 1's (argument
  window, x0-x7, x28 = VM, x27 = tag), so tier 1 and tier 3 code call each other freely and stubs keep working.
- **wasm:** emit wasm with typed locals (`f64`, `i32`, `i64`), and let V8 allocate registers. The wins are the same:
  fewer checks, inlining and unboxed loops. On wasm, values that are boxed pointers must be in the shadow stack
  (where the GC looks) at every safepoint and call. Unboxed `f64`/`i32` values need not be, which is most of the
  win.

## Deoptimization

A guard that fails rebuilds tier 1's state at that operation and continues in tier-1 code.

- **Resume points.** When tier 1 compiles a function, it records the pc of each speculated operation (each
  operation that has a feedback site), keyed by that operation's index in the sequence of generator calls. The IR
  generator numbers its guards the same way.
- **Frame state.** Each guard keeps, for its resume point, the SSA value of every live tier-1 location: x0, x1, x2,
  the slots below the current depth, and each local. Frame states are compressed (dead locations are dropped) and
  stored as a side table, in the style of V8's translation arrays. Keeping these values alive raises register
  pressure; liveness analysis keeps that to what the tier-1 code at the resume point still reads.
- **The deopt stub.** Each guard branches to a small exit that saves registers and calls `rt_deopt(id)`. That call
  boxes each value, builds tier 1's frame in place, sets x0/x1/x2 and jumps to the resume pc. Resizing the frame is
  allowed because the frame being rebuilt is the topmost one.
- **Inlined frames.** A guard inside an inlined callee rebuilds one tier-1 frame per inlining level: the caller,
  stopped at its call site's resume point, and below it the callee, whose saved lr points at the caller's tier-1
  return address. This comes in phase 3; phase 2 rebuilds single frames only.
- **Feedback after a deopt.** Each guard counts its failures. After a few, the function is recompiled without that
  speculation. After repeated recompiles, it stays at tier 1.

Precise deoptimization at the failing operation means nothing runs twice. Effects that happened before the guard
stay done, and tier 1 carries on from the exact operation.

## Invalidation

Speculation also relies on facts about other code: which function a name calls (inlined bodies) and the classes'
layouts. Those can change only at a hot reload, which happens only at a safepoint (a poll, or inside a call).

- A global **epoch** counter in the VM block is incremented by every reload that changes a function, a class or
  an arity.
- Optimized code reads the epoch after each call and each poll that could have reloaded (one `ldr`, a `cmp` and a
  `b.ne`), and deoptimizes if it changed. A frame suspended in a fiber does the same when it resumes, because
  `yield` is a call.
- The reload also repoints the stubs of invalidated functions at tier 1 (as reloads already do for tier 2), so new
  calls never enter stale code.

Compared with patching return addresses (V8's lazy deoptimization), this costs a few instructions per call. In
exchange, the code is easy to reason about, fibers need no special case, and wasm works the same way (where
return addresses can't be patched at all).

## Interactions

- **Transactions.** Inside a function, `vm.txn` changes only in its own `atomic` blocks: a callee that opens one
  closes it before returning. So one check on entry decides whether stores may go straight to memory, and inside
  `atomic` blocks stores always go through the journal. Today every inline store loads `vm.txn`.
- **Effects.** Inlining a function with declared effects keeps its `vm.allowed` save and restore, so nothing
  changes. Phase 1 doesn't inline such functions.
- **GC.** Marking is conservative over the stack and registers (d8-d15 included, through `setjmp`). Optimized code
  may therefore keep raw doubles and pointers anywhere, with no stack maps. There is one rule: a derived pointer
  (such as `ListData(l)` or a slot address) must not outlive its base. The IR keeps the base alive while a derived
  value has uses.
- **Lists that grow** move their storage. `ListData` reads memory, so a store to the list or any call
  invalidates it, and LICM hoists it only out of loops that do neither.
- **Errors and stack traces.** A runtime error raised in optimized code must report the same line as tier 1. Each
  call site and each helper call records its source line (as tier 1's `lines` table already does), and inlined
  frames add their callers to the trace.
- **Tests and tooling.** `fut --asm` prints optimized code too. `fut -v` logs optimizations, deopts with reasons,
  and invalidations.

## Phases

Each phase lands on its own and keeps every suite passing on native, wasm and debug/ASan builds.

1. **IR generator and lowering, no speculation (about 2k lines).**
   - `gen_ir.h`, the IR, SSA construction, the arm64 lowering with linear scan, and the wasm lowering.
   - Replaces today's tier 2 (its entry guard becomes `CheckNum` on parameters).
   - Test mode `--opt-all`: optimize every function at its first call, then run every spec and example and compare
     with tier 1.
   - Exit: all specs pass under `--opt-all`; fib no slower than today.
2. **Feedback, speculation, deoptimization, type propagation, LICM (about 1.5k).**
   - Exit: leibniz and mandelbrot within 1.5x of C. nbody within 4x, which needs field speculation and hoisted
     class checks.
   - Test: a deopt-stress mode where a seeded random choice fails 1 guard in N, so that every resume point is
     exercised by the specs and the fuzzer.
3. **Inlining with multi-frame deoptimization, and epoch invalidation (about 1k).**
   - Exit: nbody within 2x of C (`advance` inlines no calls, but `energy` calls `sqrt`). The hot-reload suite passes
     under `--opt-all`.
4. **Integer representation and bounds-check elimination.**
   - Exit: sieve within 2x of C.
5. **Later:**
   - store fields inline in the instance for classes with declared fields (one load instead of two);
   - polymorphic inline caches (2-4 classes) with matching speculation;
   - optimizing inside `atomic` and `try`.

## Risks

- **The two runs must emit the same sequence of generator calls.** A mismatch silently breaks deoptimization.
  Mitigation: in debug builds, a hash of the sequence is recorded at tier 1 and checked at optimization, and any
  difference fails the compile.
- **Frame states make code bigger.** V8-style compression keeps the side tables small; the guard exits themselves
  are 2 instructions each.
- **Wasm.** V8 already optimizes fut's wasm. The tier's wins there are the checks it removes and the unboxing,
  which V8 can't do because it doesn't know fut's types. If phase 2 shows no gain on wasm, the wasm lowering can be
  dropped, and wasm keeps tier 1 with V8 doing the optimizing.
- **Effort.** About 5k lines over four phases. Phases 1 and 2 hold most of the risk and most of the benefit.
