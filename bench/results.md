Measured on Apple M2 Max; C is clang -O2. Best of 5 runs in one process.

| Benchmark | Implementation | Best (ms) | First run (ms) | vs C |
|---|---|---:|---:|---:|
| fib | C | 5.6 | 5.7 | 1.00x |
| fib | fut | 11.8 | 12.0 | 2.09x |
| fib | fut wasm | 16.2 | 17.3 | 2.88x |
| leibniz | C | 41.7 | 43.4 | 1.00x |
| leibniz | fut | 333.6 | 335.1 | 8.00x |
| leibniz | fut wasm | 184.3 | 438.5 | 4.42x |
| nbody | C | 7.0 | 7.0 | 1.00x |
| nbody | fut | 137.7 | 138.6 | 19.73x |
| nbody | fut wasm | 133.6 | 139.8 | 19.15x |
| binarytrees | C | 311.0 | 313.6 | 1.00x |
| binarytrees | fut | 257.5 | 259.3 | 0.83x |
| binarytrees | fut wasm | 302.5 | 316.7 | 0.97x |
| sieve | C | 20.4 | 20.7 | 1.00x |
| sieve | fut | 156.1 | 178.7 | 7.67x |
| sieve | fut wasm | 257.6 | 385.1 | 12.65x |
| maps | C | 161.2 | 168.1 | 1.00x |
| maps | fut | 261.7 | 262.1 | 1.62x |
| maps | fut wasm | 342.6 | 403.2 | 2.12x |
| mandelbrot | C | 98.3 | 102.8 | 1.00x |
| mandelbrot | C x8 threads | 13.5 | 13.5 | 0.14x |
| mandelbrot | fut | 557.3 | 563.7 | 5.67x |
| mandelbrot | fut wasm | 418.6 | 431.2 | 4.26x |
