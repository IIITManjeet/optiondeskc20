# optionsdesk

An options trading system in C++20 for Linux, built in milestones against
Deribit's BTC/ETH options.

- **M1, pricing core:** Black-76 pricer and Greeks, a safeguarded implied-vol
  solver, an SVI volatility surface fitted to the chain, static-arbitrage diagnostics.
- **M2, streaming market data:** TLS WebSocket feed handler, simdjson hot-path
  parsing, a lock-free SPSC ring between threads, and a live surface that refits
  on its own thread.
- **M3, position and risk engine:** portfolio Greeks for a coin-margined book in
  both USD and coin terms, vega by expiry, and spot × vol × time scenario P&L
  revalued on the fitted surface.

See [docs/ROADMAP.md](docs/ROADMAP.md) for what's next (market-making strategy,
order gateway on Deribit testnet).

```
Deribit REST ──> Snapshot ──> OptionChain ──> per-expiry IVs ──> SVI fit ──> smile metrics
 (libcurl)       (save/load     (grouped by     (bid/ask/mid,      (quasi-     ATM, RR25, BF25,
                  for replay)    expiry, fwd)    Black-76 inverse)  explicit)   arb checks
```

## Build (Linux)

Requires GCC 12+ or Clang 15+, CMake 3.20+, libcurl, OpenSSL and Boost (1.74+,
headers only). nlohmann/json, simdjson and GoogleTest are fetched by CMake.

```bash
sudo apt install build-essential cmake libcurl4-openssl-dev libssl-dev libboost-dev
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

## Live streaming (M2)

```
              feed thread                     pricer thread               surface thread
wss://deribit ──> read frame ──> simdjson ──> SPSC ring ──> LiveBook ──snapshot──> SVI fit ──> dashboard
                  (Beast/TLS)    On-Demand    (lock-free)   (latest per   (latest-   (every
                                                             instrument)   wins)      --refresh-ms)
```

```bash
./build/od_live --currency BTC                                   # live dashboard
./build/od_live --busy-poll --feed-cpu 2 --pricer-cpu 4          # pinned, spinning consumer
./build/od_live --duration 60 --plain                            # log lines + latency summary
```

Measured on the live BTC chain (956 options, ~1,000 updates/s), 60 s run,
`--busy-poll --feed-cpu 2 --pricer-cpu 4`, WSL2 Ubuntu 22.04:

| Stage | p50 | p90 | p99 |
|---|---|---|---|
| parse (frame read → update pushed) | 2.6 µs | 5.4 µs | 10.8 µs |
| hop (pushed → popped by pricer) | 0.3 µs | 0.7 µs | 111 µs |
| book snapshot (pricer thread) | 172 µs | 188 µs | 254 µs |
| surface fit (surface thread) | 18.9 ms | 32.5 ms | 36.4 ms |

0 drops and 0 reconnects over 60,921 updates. How the design got there:

| Change | Effect (measured) |
|---|---|
| nlohmann DOM → simdjson On-Demand | parse p50 23.6 µs → 2.6 µs live; 23.3 µs → 1.7 µs on a fixed frame (`od_bench`) |
| sleep-poll (100 µs) → busy-poll + pinning | hop p50 110 µs → 0.3 µs |
| refit on pricer thread → separate surface thread | hop max 34 ms → under 1 ms |

The hop p99 tail is the pricer pausing for the ~170 µs book snapshot, plus WSL2
scheduling noise. The `exch->pricer` figure the app prints includes the local clock's
offset from Deribit, so it is not a network latency measurement.

## Risk (M3)

```bash
./build/od_risk --positions examples/portfolio_btc.json --snapshot data/btc_snapshot.json
./build/od_risk --positions my.json --currency BTC --days 7 --sticky-strike
```

Portfolio file: options in contracts (1 = 1 coin) with entry price in coin, the
inverse perpetual in USD notional, plus the coin balance (format in
`include/od/portfolio_io.hpp`). Options are valued with IVs from the fitted SVI
smile for their expiry, falling back to the exchange mark IV.

On a coin-margined exchange the account is held in BTC, so **USD P&L and BTC P&L
are different questions** and a book can be flat in one while exposed in the other.
The example (hypothetical) book is short a Nov strangle, long a Dec straddle, holds
3 BTC and is hedged with a short perpetual. Run on the recorded snapshot:

```
equity           : 3.2299 BTC  ($262794)
delta, USD view  : -0.0016 BTC   (USD P&L for +1% spot: $-1)
delta, coin view : -3.2315 BTC   (BTC P&L for +1% spot: -0.03232)
gamma, USD view  : -0.1549 BTC of delta per 1% move
vega             : $-486 per vol point
theta            : $+370 per day

scenario P&L in USD  (sticky-moneyness, 0 days forward)
vol \ spot       -20%       -10%        -5%        +0%        +5%       +10%       +20%
   -5.0 pt     -27174      -4223       +110      +2054      +1256      -3731     -28029
   +0.0 pt     -29423      -6120      -1492         +0      -1718      -7407     -31399
   +5.0 pt     -31720      -8528      -3863      -2750      -5090     -11234     -34949
```

Delta-neutral in USD, short gamma (loses on large moves either way), short vega,
collecting theta. The same book measured in BTC has −3.2 BTC of delta: the perp
that flattens USD exposure is exactly what makes the coin balance shrink when
BTC rallies.

- **Portfolio delta/gamma** are bump-and-reprice on the same valuation as the grid,
  so they can't disagree with it. Per-position Greeks are analytic Black-76.
- **Sticky-moneyness** (default): after a spot move, each strike is priced off the
  smile at its new moneyness. `--sticky-strike` keeps each strike's vol fixed instead.
  With a put skew the two give materially different P&L for the same move.
- The perpetual is marked at the index; funding and the perp/index basis are ignored.

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
| `src/deribit_feed.cpp` | WebSocket feed handler: subscribe, heartbeats, reconnect, push to ring |
| `src/ticker_parser.cpp` | simdjson On-Demand parser for ticker notifications |
| `include/od/spsc_ring.hpp` | Lock-free single-producer/single-consumer ring buffer |
| `include/od/histogram.hpp` | Fixed-memory log-linear latency histogram |
| `apps/od_live.cpp` | Three-thread live surface: feed, pricer, surface worker |
| `src/risk.cpp` | Market view, portfolio valuation, Greeks, scenario grid |
| `src/portfolio_io.cpp` | Portfolio JSON loader |
| `apps/od_risk.cpp` | Risk report CLI |

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
| Ticker parse, 782-byte real frame: nlohmann / simdjson | ~23 µs / ~1.7 µs |

## Known limitations

- SVI's wings are asymptotically linear in total variance, so it can't follow a far
  wing that keeps curving up (see the 25DEC26 call wing). SSVI / eSSVI calibrated
  across expiries would also rule out calendar arbitrage by construction.
- The IV solver is readable rather than fastest; Jäckel's "Let's Be Rational"
  is the production-grade replacement.
- The feed uses ticker channels (top of book + mark). Full order books with
  sequence-gap recovery come with quoting (M4).
- The instrument list is loaded once at startup; new listings are counted as
  `unknown` until restart.
