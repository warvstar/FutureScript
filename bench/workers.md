# fut tests on Cloudflare Workers: compiled ahead of time vs interpreted

Measured with `node bench/workers.mjs` in workerd (`wrangler dev`, local) on darwin/arm64, 2026-11-26.
Latencies are end to end (HTTP request to last byte) on a warm isolate unless marked cold; medians of 3. Load average
at the start: 23.1 25.8 22.2.

- **AOT**: `web/worker.mjs build --tests` compiles each test file ahead of time (under Node) into a bundle of
  precompiled wasm modules. A new code version needs a build and a deploy.
- **interp**: one static Worker (`web/worker.mjs build --interp`): each request sends the sources, fut.wasm (which
  V8 compiles once, as it is uploaded) compiles them, and src/interp.cpp interprets the code. Nothing is deployed per
  version.

## Pipeline per code version

| Step | AOT | interp |
|---|---:|---:|
| build bundle: tests/specs (30 files, 161 tests) | 1987 ms | none (static bundle, built once: 120 ms) |
| bundle size | 1278 KB (407 KB gzipped) | 866 KB, fixed |
| wrangler deploy --dry-run (bundling only) | 4913 ms | none |
| upload + activate | not measured (needs an account); typically seconds | none |
| request payload | a file list | the sources (tests/: 92 KB) |

## Latency

| Run | AOT (ms) | interp (ms) | interp / AOT |
|---|---:|---:|---:|
| first request (cold isolate), specs/bits.fut | 50.9 | 32.3 | 0.6x |
| one spec file, median over the 30 files | 11.1 | 12.9 | 1.2x |
| all 30 spec files, one request each, summed | 2284 | 5499 | 2.4x |
| all 30 spec files in one request | 2138 | 9132 | 4.3x |
| examples/store (14 tests, 4 packages) | 412 | 1292 | 3.1x |

Largest interp/AOT ratios per file: specs/gc.fut 160 vs 3050 ms; specs/lists.fut 20 vs 62 ms; specs/fpregs.fut 70 vs 202 ms; specs/contracts.fut 37 vs 90 ms; specs/tier.fut 10 vs 23 ms.

## CPU-bound kernels (bench/*.fut, as one test each)

| Kernel | AOT (ms) | interp (ms) | interp / AOT |
|---|---:|---:|---:|
| fib | 56 | 1753 | 31.0x |
| sieve | 375 | 15112 | 40.3x |
| nbody | 276 | 24505 | 88.9x |
| binarytrees | 4266 | 48478 | 11.4x |
| maps | 438 | 5951 | 13.6x |
| mandelbrot | 472 | 55453 | 117.5x |
| leibniz | 2588 | 30552 | 11.8x |

## Throughput

24 requests of specs/maps.fut, 4 at a time, one local isolate: AOT 141.0 req/s, interp 130.8 req/s.

Tests that failed in some run: failures say how it grows, with the measurements. Load average at the end: 40.0 31.3 25.3.

## Per file

| File | AOT (ms) | interp (ms) |
|---|---:|---:|
| specs/bigframe.fut | 12.1 | 9.3 |
| specs/bits.fut | 6.6 | 7.3 |
| specs/closures.fut | 7.2 | 8.0 |
| specs/collections.fut | 7.0 | 7.5 |
| specs/contracts.fut | 36.6 | 90.4 |
| specs/effects.fut | 88.7 | 101.2 |
| specs/err_arity.fut | 11.4 | 11.7 |
| specs/err_field.fut | 11.1 | 11.7 |
| specs/err_runtime.fut | 10.7 | 10.9 |
| specs/err_stack.fut | 11.5 | 12.9 |
| specs/extern.fut | 8.7 | 8.7 |
| specs/facts.fut | 8.1 | 14.4 |
| specs/fiber.fut | 16.4 | 32.4 |
| specs/fields.fut | 9.7 | 9.6 |
| specs/floats.fut | 9.4 | 9.3 |
| specs/fpregs.fut | 70.4 | 202.2 |
| specs/gc.fut | 160.0 | 3049.7 |
| specs/imports.fut | 8.3 | 10.6 |
| specs/indexing.fut | 9.3 | 13.1 |
| specs/interp.fut | 9.0 | 8.7 |
| specs/lists.fut | 20.0 | 61.6 |
| specs/manyargs.fut | 9.2 | 9.7 |
| specs/maps.fut | 8.0 | 8.7 |
| specs/maps2.fut | 8.7 | 9.8 |
| specs/maps3.fut | 25.5 | 32.7 |
| specs/measure.fut | 1648.0 | 1677.5 |
| specs/modules.fut | 14.3 | 16.6 |
| specs/modules_err.fut | 13.9 | 16.0 |
| specs/tier.fut | 10.0 | 22.7 |
| specs/try.fut | 14.5 | 13.7 |

## Deployed (Cloudflare, Paid plan), November 2026

The interpreting Worker (`fut worker build --interp`, deployed as `fut-interp` with a `FUT_TOKEN` secret) against a
swarm's fsql project: 54 test files, 945 tests, 486 KB of sources sent with each request.

| | wall time |
|---|---:|
| native, locally, 12 cores (`fut test` per file, in parallel) | 4.9 s |
| the Worker, one request per file, all at once | ~300 s (median request 41 s; several stopped at the 300 s client timeout) |

fsql's tests are CPU-heavy (B-tree and sort scale tests over thousands of keys), where interpreting costs 30-100x. For
such suites, test locally, or use bundles compiled ahead of time; the interpreter suits small, quick test files. Two
fixes this needed: the wasm heap was a fixed 256 MB reservation, over a Worker's 128 MB (now `heapMB`, 32 MB in
Workers), and one request running every file hit the default 30 s CPU limit (`cpu_ms = 300000` in the generated config).

## Deployed, compiled ahead of time vs interpreted vs local (the swarm's fsql), November 2026

The swarm's fsql main at the time: 70 test files, 1231 tests (175 of them pending), 582 KB of sources. Sent from a
laptop on a home uplink (about 1.5 Mbit/s up), load average 12-25.

| | per code version | running every file | result |
|---|---:|---:|---|
| local, native, 12 cores (`fut test` per file, in parallel) | none | 5.1 s | all pass (one timing check flaky under load) |
| AOT: one bundle (`build --tests --shards 1 --heap-mb 96`), deployed as one Worker | build 2-3 s, upload + deploy 48-59 s | 2.5-4.2 s (70 requests at once, median 1 s) | all pass but one hand-timed test (below) |
| interp: `fut-interp`, sources sent with each request | none | 85 s, and 52 of 70 requests failed (connections dropped) | the 18 that came back passed |
| interp, one file alone (types_test, 25 tests) | none | 14.8 s | pass |

- The bundle is 6.9 MB (1.3 MB gzipped): 71 compiled modules, one per test file, each carrying the project's code it
  imports. Wrangler uploads it uncompressed, so from here the upload dominates (37 s); the interpreting Worker's 1.1 MB
  took 6 s.
- A Worker's clock stands still while code runs (it moves only across I/O). Timing loops then never end: `expect grows`
  doubled its repetitions toward 2^20 calls per size and pinned the isolate until the 300 s CPU limit, and every request
  sharing that isolate queued behind it (the first deploy timed out 7-9 of 70 files, small ones included). Now the
  Worker runtime tells fut (`fut_untimed`), and a clock that doesn't move across 65536 calls counts as stopped: those
  checks pass unmeasured, and the summary says so (`"untimed": true`). A test that times itself with `clock()` still
  can't work there (fsql's parse_scale compares two zero durations): such files belong on a real clock.
- fsql's scale tests need more than the 32 MB default heap (`parse_scale` ran out of memory at 64 MB). AOT code is
  compiled for one heap size, so `--heap-mb` is a build option, kept in the bundle. A file that dies before its summary
  (out of memory) now counts as an error instead of silently dropping its tests.

So for a swarm on this machine, local tests win: 5 s, against about 50 s for AOT (almost all of it the upload) and
worse for the interpreter. AOT does scale out: 70 files took no longer than the slowest one, so with a fast uplink
(or a remote builder near Cloudflare) a version would cost roughly 2 s build + a few seconds upload + 3 s run, and a
suite far larger than one machine's cores would still finish in seconds. The interpreter suits a few small files.
