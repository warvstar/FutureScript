Chrome, headless, 10 rounds after one of warm-up; the min per operation, from the click to the frame
that shows it (ms). × plain DOM: how many times as long as apps/vanilla.js. `python3 run.py` writes this.

| Operation | vanilla | solid | react | fut | fut-mt |
|---|---:|---:|---:|---:|---:|
| create 1,000 rows | 15.5 (1.00×) | 15.2 (0.98×) | 17.9 (1.15×) | 31.7 (2.04×) | 53.5 (3.45×) |
| replace 1,000 rows | 17.1 (1.00×) | 17.8 (1.04×) | 21.0 (1.23×) | 35.2 (2.06×) | 54.1 (3.17×) |
| update every 10th row | 3.7 (1.00×) | 3.8 (1.04×) | 3.6 (0.97×) | 2.6 (0.70×) | 3.5 (0.94×) |
| select a row | 1.0 (1.00×) | 1.1 (1.15×) | 1.2 (1.20×) | 1.6 (1.64×) | 2.0 (2.05×) |
| swap two rows | 2.4 (1.00×) | 2.6 (1.07×) | 16.3 (6.79×) | 3.0 (1.27×) | 3.4 (1.42×) |
| remove a row | 3.9 (1.00×) | 4.0 (1.02×) | 4.3 (1.09×) | 4.9 (1.24×) | 5.1 (1.31×) |
| create 10,000 rows | 155.8 (1.00×) | 155.6 (1.00×) | 320.8 (2.06×) | 362.0 (2.32×) | 547.2 (3.51×) |
| append 1,000 rows | 17.6 (1.00×) | 17.3 (0.99×) | 19.2 (1.10×) | 35.3 (2.01×) | 55.3 (3.15×) |
| clear 10,000 rows | 16.4 (1.00×) | 17.9 (1.09×) | 26.9 (1.63×) | 30.4 (1.85×) | 30.1 (1.83×) |
| × plain DOM (geometric mean) | 1.00 | 1.04 | 1.53 | 1.59 | 2.10 |
| page ready | 54 | 33 | 47 | 139 | 217 |

Apple M2 Max, Chrome stable, on a busy machine (load average about 100): hence the min. fut-mt: fut-mt.wasm, the
dev server's default (the program on a worker; its DOM calls queued and sent to the page together).
