# Web benchmarks

Run the keyed table operations in headless Chrome against plain DOM, Solid, React, and both fut WebAssembly builds. Use the same macOS WASI toolchain as `build.py`, with Node, Chrome, and Python Playwright installed.

```sh
python3 build.py wasm
python3 build.py wasm --threads
npm ci --prefix bench/web
python3 bench/web/run.py --threads --memory --rounds 7 --output build/web-benchmark.md
```

Each operation runs once to warm up and then reports the median from click through paint. Memory uses a separate Chrome launch per app. The report gives total resident memory (RSS) after the empty table and after rendering 10,000 rows, along with the increase between them. These are snapshots in MiB, and shared memory can appear in several Chrome processes' RSS.

For a local comparison, give `--baseline /path/to/base-checkout` after building both Wasm targets and installing npm dependencies in that checkout too. The same driver runs both revisions sequentially. With all four apps and no explicit output or baseline, the default report path is `bench/web/results.md`.

The Web benchmark GitHub Actions workflow publishes the report in the run summary and as an artifact. For pull requests it measures the base and the proposed merge on the same runner and shows changes per operation and memory snapshot. The job fails if the benchmark itself fails. Timing and memory deltas are reported without thresholds because hosted runners can vary between runs.
