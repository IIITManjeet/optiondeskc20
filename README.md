# optionsdesk

An options trading system in C++20 for Linux, built in milestones against
Deribit's BTC/ETH options. **Milestone 1** (this commit) is the pricing core:
Black-76 pricer and Greeks, a safeguarded implied-vol solver, an SVI volatility
surface fitted to the live chain, and static-arbitrage diagnostics.

See [docs/ROADMAP.md](docs/ROADMAP.md) for where it goes next (streaming feed,
risk engine, market-making strategy, order gateway on Deribit testnet).

```
Deribit REST ──> Snapshot ──> OptionChain ──> per-expiry IVs ──> SVI fit ──> smile metrics
 (libcurl)       (save/load     (grouped by     (bid/ask/mid,      (quasi-     ATM, RR25, BF25,
                  for replay)    expiry, fwd)    Black-76 inverse)  explicit)   arb checks
```

## Build (Linux)

Requires GCC 12+ or Clang 15+, CMake 3.20+, libcurl dev headers.
nlohmann/json and GoogleTest are fetched by CMake.

```bash
sudo apt install build-essential cmake libcurl4-openssl-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build          # or ./build/od_tests
```

## Run

```bash
./build/od_surface --currency BTC                         # live chain
./build/od_surface --currency ETH --watch 30              # refresh every 30 s
./build/od_surface --save snap.json --csv out/surface.csv # keep raw data + per-strike IVs
./build/od_surface --snapshot data/btc_snapshot.json      # offline, reproducible
python tools/plot_surface.py out/surface.csv -o out/surface.png
```

Example output from the recorded snapshot in `data/` (BTC, 2026-10-08):

```
expiry       days    forward  inside  atm_iv    rr25    bf25   rmse    min_g flags
9OCT26       0.57   81408.68   10/11  38.47%  -2.53  +1.43   1.11   0.3602
27NOV26     49.57   82043.55   48/48  36.90%  -1.15  +1.14   0.20   0.2545
25DEC26     77.57   82365.70   45/58  37.38%  -1.26  +1.17   1.43   0.0900
24SEP27    350.57   85687.34   42/42  39.05%  -0.19  +1.05   0.06   0.4683
...
pricer cross-check vs exchange marks (443 OTM options, 30 skipped for ~zero vega):
  |iv(mark) - mark_iv|  median 0.0055  p95 0.0421  max 0.1256 vol pts
```

- **inside**: fit quotes where the SVI vol lies within the market bid/ask IVs.
- **rr25 / bf25**: 25-delta risk reversal and butterfly, the standard way desks
  quote skew and smile curvature. Negative RR = puts richer than calls.
- **min_g**: Durrleman's condition over the quoted range; negative means the
  fitted smile implies a negative probability density (butterfly arbitrage).
- **CALENDAR_ARB**: a later expiry has less total variance than an earlier one
  at the same moneyness.

## What's in here

| File | What it does |
|---|---|
| `include/od/black76.hpp` | Black-76 price and Greeks, inverse (coin-settled) helpers |
| `src/implied_vol.cpp` | Newton on vega inside a bisection bracket; rejects arbitrageable prices |
| `src/svi.cpp` | Raw SVI, Zeliade quasi-explicit calibration, Durrleman `g(k)` |
| `include/od/nelder_mead.hpp` | Small derivative-free optimiser used by the SVI fit |
| `src/chain.cpp` | Instrument-name parsing, grouping quotes into expiry slices |
| `src/surface.cpp` | Per-strike IVs, OTM selection, weighted fits, smile metrics, cross-check |
| `src/deribit.cpp` | Public REST client and snapshot save/load |

### Deribit conventions (verified against live data)

- Options are **inverse**: quoted and settled in BTC/ETH. `price_coin = Black76_usd(F) / F`.
- `F` is the per-expiry `underlying_price` (the future or synthetic forward), `r = 0`.
- Greeks are reported in USD Black-76 terms: delta `N(d1)`, vega per vol point,
  theta per day on a 365-day year. `tests/test_black76.cpp` pins this against
  one captured ticker.
- `get_book_summary_by_currency` is not an atomic snapshot: the forward moves while it
  is assembled. That's why the cross-check reports a median rather than relying on the max.
- Deep-ITM marks sit closer to intrinsic than the published `mark_iv` implies, so the
  cross-check uses OTM options only.

## Performance

`./build/od_bench`, Release, `-march=native`, measured on a WSL2 Ubuntu 22.04 VM
(GCC 12.3). Treat these as rough; WSL2 adds timer and scheduling noise.

| Operation | Time |
|---|---|
| `black76_price` | ~50 ns |
| `black76_greeks` (price + 4 Greeks) | ~70 ns |
| `implied_vol` (avg 5.9 iterations) | ~0.5 µs |
| `fit_svi`, 41 quotes, 6 starts | ~0.3 ms |
| Full BTC surface (956 instruments, 12 expiries) | ~20 ms |

## Known limitations

- SVI's wings are asymptotically linear in total variance, so it can't follow a far
  wing that keeps curving up (see the 25DEC26 call wing). SSVI / eSSVI calibrated
  across expiries would also rule out calendar arbitrage by construction.
- The IV solver is readable rather than fastest; Jäckel's "Let's Be Rational"
  is the production-grade replacement.
- REST snapshots only; streaming market data is milestone 2.
