Measured on Apple M2 Max; C is clang -O2. Best of 5 runs in one process.

| Benchmark | Implementation | Best (ms) | First run (ms) | vs C |
|---|---|---:|---:|---:|
| fib | C | 5.6 | 5.6 | 1.00x |
| fib | fut | 11.9 | 11.9 | 2.13x |
| fib | fut wasm | 17.0 | 18.0 | 3.03x |
| leibniz | C | 44.1 | 45.7 | 1.00x |
| leibniz | fut | 334.7 | 346.4 | 7.60x |
| leibniz | fut wasm | 183.0 | 433.7 | 4.15x |
| nbody | C | 6.7 | 7.1 | 1.00x |
| nbody | fut | 138.6 | 140.1 | 20.76x |
| nbody | fut wasm | 141.3 | 147.4 | 21.17x |
| binarytrees | C | 307.5 | 307.5 | 1.00x |
| binarytrees | fut | 255.4 | 255.4 | 0.83x |
| binarytrees | fut wasm | 299.0 | 311.7 | 0.97x |
| sieve | C | 19.9 | 20.7 | 1.00x |
| sieve | fut | 197.2 | 219.3 | 9.92x |
| sieve | fut wasm | 322.9 | 480.2 | 16.24x |
| maps | C | 157.3 | 175.9 | 1.00x |
| maps | fut | 252.8 | 252.8 | 1.61x |
| maps | fut wasm | 337.3 | 392.1 | 2.15x |
| mandelbrot | C | 96.7 | 97.3 | 1.00x |
| mandelbrot | C x8 threads | 13.4 | 13.7 | 0.14x |
| mandelbrot | fut | 554.9 | 555.4 | 5.74x |
| mandelbrot | fut wasm | 423.8 | 437.9 | 4.38x |
