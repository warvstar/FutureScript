Chrome, headless, 10 rounds after one of warm-up; the min per operation, from the click to the frame
that shows it (ms). × plain DOM: how many times as long as apps/vanilla.js. `python3 run.py` writes this.

| Operation | vanilla | solid | react | fut |
|---|---:|---:|---:|---:|
| create 1,000 rows | 16.0 (1.00×) | 15.2 (0.95×) | 18.0 (1.12×) | 39.2 (2.45×) |
| replace 1,000 rows | 17.0 (1.00×) | 17.1 (1.01×) | 22.2 (1.31×) | 42.9 (2.52×) |
| update every 10th row | 4.0 (1.00×) | 2.8 (0.70×) | 3.3 (0.82×) | 3.1 (0.78×) |
| select a row | 1.0 (1.00×) | 0.9 (0.90×) | 1.4 (1.40×) | 2.0 (2.00×) |
| swap two rows | 2.5 (1.00×) | 2.3 (0.92×) | 15.7 (6.28×) | 3.2 (1.28×) |
| remove a row | 4.2 (1.00×) | 4.0 (0.95×) | 4.4 (1.05×) | 4.7 (1.12×) |
| create 10,000 rows | 154.9 (1.00×) | 149.3 (0.96×) | 308.6 (1.99×) | 419.0 (2.70×) |
| append 1,000 rows | 18.3 (1.00×) | 17.7 (0.97×) | 21.4 (1.17×) | 41.9 (2.29×) |
| clear 10,000 rows | 17.7 (1.00×) | 16.9 (0.95×) | 26.3 (1.49×) | 27.8 (1.57×) |
| × plain DOM (geometric mean) | 1.00 | 0.92 | 1.50 | 1.72 |
| page ready | 252 | 91 | 593 | 351 |

Apple M2 Max, Chrome stable, on a busy machine (load average about 100): hence the min. Before this change fut was
about 121 ms to create 1,000 rows and 24 ms to swap two (median of 1).
