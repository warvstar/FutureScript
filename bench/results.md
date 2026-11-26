Measured on Apple M2 Max; C is clang -O2. Best of 5 runs in one process.

| Benchmark | Implementation | Best (ms) | First run (ms) | vs C |
|---|---|---:|---:|---:|
| fib | C | 7.6 | 7.6 | 1.00x |
| fib | fut | 15.2 | 15.4 | 2.01x |
| fib | fut wasm | 20.3 | 21.2 | 2.68x |
| leibniz | C | 55.8 | 56.9 | 1.00x |
| leibniz | fut | 113.3 | 115.4 | 2.03x |
| leibniz | fut wasm | 74.7 | 544.2 | 1.34x |
| nbody | C | 8.9 | 8.9 | 1.00x |
| nbody | fut | 186.0 | 186.0 | 21.00x |
| nbody | fut wasm | 188.4 | 196.4 | 21.28x |
| binarytrees | C | 391.4 | 397.7 | 1.00x |
| binarytrees | fut | 352.1 | 359.8 | 0.90x |
| binarytrees | fut wasm | 431.7 | 449.2 | 1.10x |
| sieve | C | 26.6 | 28.4 | 1.00x |
| sieve | fut | 141.6 | 162.7 | 5.32x |
| sieve | fut wasm | 214.4 | 318.4 | 8.06x |
| maps | C | 232.2 | 237.6 | 1.00x |
| maps | fut | 351.2 | 351.2 | 1.51x |
| maps | fut wasm | 447.7 | 527.2 | 1.93x |
| mandelbrot | C | 130.4 | 130.5 | 1.00x |
| mandelbrot | C x8 threads | 16.3 | 16.5 | 0.12x |
| mandelbrot | fut | 456.3 | 456.3 | 3.50x |
| mandelbrot | fut wasm | 553.4 | 572.7 | 4.24x |
| mandelbrot | fut x8 isolates | 57.2 | 57.3 | 0.44x |
