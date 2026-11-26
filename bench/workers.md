# fut tests on Cloudflare Workers: compiled ahead of time vs interpreted

Measured with `node bench/workers.mjs` in workerd (`wrangler dev`, local) on darwin/arm64, 2026-11-26.
Latencies are end to end (HTTP request to last byte) on a warm isolate unless marked cold; medians of 3. The machine
was shared (load average ~30 during this run), so treat single numbers as noisy and the ratios as the result.

- **AOT**: `web/worker.mjs build --tests` compiles each test file ahead of time (under Node) into a bundle of
  precompiled wasm modules. A new code version needs a build and a deploy.
- **interp**: one static Worker (`web/worker.mjs build --interp`): each request sends the sources, fut.wasm (which
  V8 compiles once, as it is uploaded) compiles them, and src/interp.cpp interprets the code. Nothing is deployed per
  version.

## Pipeline per code version

| Step | AOT | interp |
|---|---:|---:|
| build bundle: tests/specs (30 files, 161 tests) | 12247 ms | none (static bundle, built once: 143 ms) |
| bundle size | 1572 KB (435 KB gzipped) | 1158 KB, fixed |
| wrangler deploy --dry-run (bundling only) | 5879 ms | none |
| upload + activate | not measured (needs an account); typically seconds | none |
| request payload | a file list | the sources (tests/: 92 KB) |

## Latency

| Run | AOT (ms) | interp (ms) | interp / AOT |
|---|---:|---:|---:|
| first request (cold isolate), specs/bits.fut | 49.7 | 35.1 | 0.7x |
| one spec file, median over the 30 files | 12.7 | 15.0 | 1.2x |
| all 30 spec files, one request each, summed | 2330 | 5456 | 2.3x |
| all 30 spec files in one request | 2728 | 5933 | 2.2x |
| examples/store (14 tests, 4 packages) | 427 | 1314 | 3.1x |

Largest interp/AOT ratios per file: specs/gc.fut 163 vs 3044 ms; specs/lists.fut 21 vs 66 ms; specs/fpregs.fut 67 vs 208 ms; specs/tier.fut 11 vs 22 ms; specs/facts.fut 8 vs 15 ms.

## CPU-bound kernels (bench/*.fut, as one test each)

| Kernel | AOT (ms) | interp (ms) | interp / AOT |
|---|---:|---:|---:|
| fib | 56 | 2228 | 39.5x |
| sieve | 394 | 28385 | 72.0x |
| nbody | 276 | 16950 | 61.4x |
| binarytrees | 600 | 43493 | 72.4x |
| maps | 4060 | 2292 | 0.6x |
| mandelbrot | 478 | 39145 | 81.9x |
| leibniz | 449 | 27320 | 60.9x |

## Throughput

24 requests of specs/maps.fut, 4 at a time, one local isolate: AOT 111.0 req/s, interp 71.4 req/s.

Tests that failed in some run: none. Load average at the end: 25.4 27.5 21.9.

## Per file

| File | AOT (ms) | interp (ms) |
|---|---:|---:|
| specs/bigframe.fut | 12.1 | 10.3 |
| specs/bits.fut | 8.2 | 8.9 |
| specs/closures.fut | 8.2 | 10.1 |
| specs/collections.fut | 9.0 | 10.5 |
| specs/contracts.fut | 30.5 | 49.9 |
| specs/effects.fut | 39.2 | 34.9 |
| specs/err_arity.fut | 12.7 | 13.2 |
| specs/err_field.fut | 13.0 | 14.4 |
| specs/err_runtime.fut | 13.5 | 14.8 |
| specs/err_stack.fut | 13.3 | 15.0 |
| specs/extern.fut | 8.6 | 9.4 |
| specs/facts.fut | 8.4 | 15.0 |
| specs/fiber.fut | 21.6 | 35.3 |
| specs/fields.fut | 8.3 | 9.4 |
| specs/floats.fut | 8.5 | 9.2 |
| specs/fpregs.fut | 66.7 | 208.2 |
| specs/gc.fut | 162.5 | 3044.3 |
| specs/imports.fut | 10.0 | 13.1 |
| specs/indexing.fut | 10.1 | 15.3 |
| specs/interp.fut | 9.4 | 13.9 |
| specs/lists.fut | 20.6 | 65.6 |
| specs/manyargs.fut | 9.1 | 9.8 |
| specs/maps.fut | 9.2 | 9.6 |
| specs/maps2.fut | 10.1 | 10.3 |
| specs/maps3.fut | 27.5 | 34.4 |
| specs/measure.fut | 1718.8 | 1685.8 |
| specs/modules.fut | 17.9 | 24.7 |
| specs/modules_err.fut | 16.0 | 22.7 |
| specs/tier.fut | 11.0 | 22.2 |
| specs/try.fut | 15.9 | 16.1 |
