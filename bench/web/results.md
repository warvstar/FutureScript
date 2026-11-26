Chrome, headless, 10 rounds after one of warm-up; the median per operation, from the click to the frame
that shows it (ms). × plain DOM: how many times as long as apps/vanilla.js. `python3 run.py` writes this.

| Operation | vanilla | solid | react | fut | fut-mt |
|---|---:|---:|---:|---:|---:|
| create 1,000 rows | 16.2 (1.00×) | 16.0 (0.99×) | 17.9 (1.11×) | 36.0 (2.23×) | 57.3 (3.54×) |
| replace 1,000 rows | 17.9 (1.00×) | 18.5 (1.04×) | 21.0 (1.18×) | 39.5 (2.21×) | 60.8 (3.40×) |
| update every 10th row | 6.4 (1.00×) | 7.2 (1.13×) | 6.9 (1.08×) | 6.8 (1.08×) | 7.3 (1.15×) |
| select a row | 1.1 (1.00×) | 1.2 (1.15×) | 1.5 (1.36×) | 2.2 (2.03×) | 2.7 (2.54×) |
| swap two rows | 2.7 (1.00×) | 3.2 (1.20×) | 18.0 (6.75×) | 3.7 (1.40×) | 4.0 (1.48×) |
| remove a row | 4.1 (1.00×) | 4.5 (1.08×) | 4.5 (1.09×) | 5.6 (1.36×) | 5.9 (1.42×) |
| create 10,000 rows | 172.2 (1.00×) | 159.5 (0.93×) | 316.7 (1.84×) | 404.5 (2.35×) | 625.2 (3.63×) |
| append 1,000 rows | 21.9 (1.00×) | 21.9 (1.00×) | 23.9 (1.09×) | 41.6 (1.90×) | 62.4 (2.85×) |
| clear 10,000 rows | 17.2 (1.00×) | 18.6 (1.09×) | 42.2 (2.46×) | 33.5 (1.95×) | 34.2 (1.99×) |
| × plain DOM (geometric mean) | 1.00 | 1.06 | 1.60 | 1.78 | 2.26 |
| page ready | 70 | 33 | 45 | 108 | 259 |

Apple M2 Max, Chrome stable, machine nearly idle (load average under 20). fut-mt: fut-mt.wasm, the dev server's
default (the program on a worker; its DOM calls queued and sent to the page together).
