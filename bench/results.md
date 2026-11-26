Measured on Apple M2 Max; C is clang -O2. Best of 5 runs in one process.

| Benchmark | Implementation | Best (ms) | First run (ms) | vs C |
|---|---|---:|---:|---:|
| fib | C | 6.1 | 6.1 | 1.00x |
| fib | fut | 12.1 | 12.3 | 1.99x |
| fib | fut wasm | 16.4 | 17.6 | 2.70x |
| leibniz | C | 41.8 | 41.8 | 1.00x |
| leibniz | fut | 335.7 | 344.6 | 8.04x |
| leibniz | fut wasm | 190.0 | 437.8 | 4.55x |
| nbody | C | 7.0 | 7.0 | 1.00x |
| nbody | fut | 141.5 | 141.7 | 20.32x |
| nbody | fut wasm | 133.7 | 142.0 | 19.20x |
| binarytrees | C | 315.0 | 315.0 | 1.00x |
| binarytrees | fut | 259.5 | 259.5 | 0.82x |
| binarytrees | fut wasm | 302.4 | 313.3 | 0.96x |
| sieve | C | 20.6 | 21.8 | 1.00x |
| sieve | fut | 120.5 | 133.3 | 5.85x |
| sieve | fut wasm | 172.4 | 269.0 | 8.37x |
| maps | C | 163.5 | 190.0 | 1.00x |
| maps | fut | 258.5 | 258.5 | 1.58x |
| maps | fut wasm | 360.1 | 420.4 | 2.20x |
| mandelbrot | C | 108.5 | 108.5 | 1.00x |
| mandelbrot | C x8 threads | 20.5 | 21.5 | 0.19x |
| mandelbrot | fut | 590.1 | 671.5 | 5.44x |
| mandelbrot | fut wasm | 446.7 | 457.1 | 4.12x |
