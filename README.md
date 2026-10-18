# optionsdesk

An options trading system in C++20 for Linux, built in milestones against
Deribit's BTC/ETH options.

- **M1, pricing core:** Black-76 pricer and Greeks, a safeguarded implied-vol
  solver, an SVI volatility surface fitted to the chain, static-arbitrage diagnostics.
- **M2, streaming market data:** TLS WebSocket feed handler, simdjson hot-path
  parsing, a lock-free SPSC ring between threads, and a live surface that refits
  on its own thread. A **shared-memory feed bus** lets one feed daemon serve
  any number of local processes.
- **M3, position and risk engine:** portfolio Greeks for a coin-margined book in
  both USD and coin terms, vega by expiry, and spot × vol × time scenario P&L
  revalued on the fitted surface.
- **M4, market maker (paper):** vol-space quoting with inventory skew, pre-trade
  risk gate, order manager and ledger, delta hedger, simulated exchange with a
  queue-position fill model driven by real trade prints, fill mark-outs.
- **M5, testnet order gateway:** the same engine sends real orders to Deribit's
  testnet (play money) through a gateway thread connected by lock-free rings, with
  cancel-on-disconnect and pacing to the exchange's rate limit.
- **M6, record and replay:** binary market-data journal, deterministic replay of the
  strategy engine over recorded data, config comparison.

See [docs/ROADMAP.md](docs/ROADMAP.md) for what's next.

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

### Shared-memory feed bus

One process owns the exchange connection; everything else reads market data from
shared memory, so adding a consumer costs no extra connection, parsing or REST calls.

```
od_feedd ──publish──> /dev/shm/od_feed_BTC <──read── od_live
(Deribit WS,          ┌ header: geometry, write_seq, heartbeat, feed stats   <──read── od_live
 simdjson)            ├ instrument table                                     <──read── od_risk --bus
                      ├ last-value cache (latest update per instrument)
                      └ broadcast ring (65,536 slots)
```

```bash
./build/od_feedd --currency BTC --feed-cpu 2                      # terminal 1
./build/od_live --bus od_feed_BTC --busy-poll --pricer-cpu 6      # terminal 2 (and 3, ...)
./build/od_risk --positions examples/portfolio_btc.json --bus od_feed_BTC
```

- **Single writer, many readers, writer never waits.** Each reader keeps its own
  position. A reader more than a ring's length behind is *lapped* and resyncs from
  the last-value cache; since tickers carry full state, it only skips intermediate
  states, never the latest one. New readers start from that cache too, so they have
  the whole chain immediately.
- **Every slot is a seqlock.** The writer marks the slot busy, writes, then publishes
  the slot's sequence number; a reader copies between two reads of it and discards
  the copy if it changed. Payload words are relaxed `std::atomic<uint64_t>`, so this
  is well-defined C++ (a plain `memcpy` would be a data race).
- **Readers map the segment read-only and pre-faulted** (`MAP_POPULATE`), so a
  buggy reader can't corrupt the feed and the hot path never page-faults.
- **Liveness:** the daemon writes a heartbeat every 100 ms. Readers flag a stale
  producer after 2 s and reattach when a new one starts (detected by its epoch).

Measured with `od_feedd` and two `od_live --bus` processes attached for 40 s:
both received the same 39,346 updates, 0 laps, 0 drops. Cross-process hop
(parsed in `od_feedd` → read by another process) was **0.5 µs p50** with
busy-polling, against 0.4 µs for the in-process ring measured back-to-back.
The bus costs about the same as a thread-to-thread handoff. Tails (p90 tens of
µs, p99 up to ~1 ms in these runs) varied run to run on WSL2 and showed up equally
in the in-process ring, so they come from the environment, not the bus. Pre-faulting
the reader's mapping cut the bus p99 from 4.2 ms to 0.79 ms.

Tests cover ordering, independent readers, lapping and resync, a two-thread stress
test (1M writes into an 8-slot ring: no torn reads accepted, no reordering), and a
real two-process test using `fork()`.

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

## Market maker (M4, paper)

```bash
./build/od_feedd --currency BTC                                              # terminal 1
./build/od_mm --bus od_feed_BTC --config examples/mm_btc.json --show-quotes  # terminal 2
```

`od_mm` is one more reader on the feed bus. On its strategy thread:

1. **Theo** for each quoted option: the latest SVI smile (refit every second on a
   separate thread) evaluated at the option's *current* forward, so theo tracks the
   underlying between refits.
2. **Quote in vol space:** theo vol ± half-spread, shifted against inventory by
   portfolio vega (long vega → both quotes lower: sell more eagerly, buy less).
   Convert to coin prices, round outward to Deribit's tick ladder, stay post-only,
   drop a side at the position limit.
3. **Pre-trade risk gate** on every new order and amend: max order size, worst-case
   position, portfolio vega and **per-expiry vega** limits (worst case: our other
   resting orders pushing the same way all fill), fat-finger band in vol terms,
   order-rate token bucket, kill switch (`kill -USR1`). Orders that reduce the
   limited risk always pass, so the gate can't trap the book. Vega is limited per
   expiry because it doesn't net across expiries in practice: long daily vega and
   short December vega are two different risks, not zero. Risk is recomputed after
   every fill, so limits never act on stale positions.
4. **Order manager** builds positions only from execution reports: premium cash,
   fees, inverse-perp harmonic entry and realised P&L.
5. **Delta hedge** with the perpetual each second when USD delta leaves ±0.25 BTC,
   valued on the M3 risk engine. By default the coin balance's own delta is left
   alone (`hedge_collateral`).

The strategy is a `MarketMaker` engine (`src/mm_engine.cpp`) that never reads a
clock or a socket: drivers feed it market data and the time. `od_mm` drives it live;
`od_replay` drives it from a recorded journal (see below). The exchange is simulated
(`SimGateway`); the engine only sees the `Gateway` interface, so the Deribit testnet
gateway can replace it.

**Fill model.** `od_feedd` also publishes every public option trade on a second bus
topic (`od_trades_BTC`). The simulator keeps a queue estimate per resting order:
joining the best price puts us behind the displayed size, improving it puts us first,
repricing loses priority, and displayed size shrinking at our level moves us up. A
trade at our price eats the queue ahead before reaching us (partial fills happen);
a trade through our price fills us. Our orders aren't in the real book, so this is
an estimate, not a replay of what would have happened.

**What paper trading showed.** `--show-quotes` puts our quotes next to the market's,
in price and in vol (live BTC data, quoting ±0.4 vol around theo):

```
  instrument              mkt_bid  mkt_ask |  our_bid  our_ask |    theo |  mb_iv  ob_iv  oa_iv  ma_iv
  BTC-30OCT26-81000-P      0.0210   0.0220 |   0.0210   0.0220 |  0.0214 |  32.20  32.20  33.31  33.31
  BTC-27NOV26-80000-P      0.0360   0.0365 |   0.0355   0.0370 |  0.0361 |  36.63  36.27  37.36  37.00
  BTC-27NOV26-88000-C      0.0290   0.0295 |   0.0285   0.0300 |  0.0293 |  35.84  35.47  36.58  36.21
```

The listed market is one or two ticks wide, and one tick (0.0005 BTC) is worth
0.35–0.6 vol points here. Theo sits inside the spread, but after rounding to the
tick our quotes can only *join* the best bid/offer or sit a tick behind: there is
no price inside the spread to improve to. Fills then depend on queue position at
the touch, which is what the queue model estimates.

The second constraint is how little trades. In one 10-minute live window there were
14 BTC option trades across all ~950 options (Deribit's own trade history agrees:
14), against ~900 ticker updates per second. Over the next hour there were 324.

### Record and replay

```bash
./build/od_record --out ~/btc.odj --duration 1800        # with od_feedd running
./build/od_replay --journal ~/btc.odj --config a.json --config b.json --quiet
python3 tools/journal_stats.py ~/btc.odj                 # where did trades happen?
```

For long recordings, `tools/record_loop.sh` runs `od_feedd` and records hourly
chunks to `~/rec/btc_YYYY-MM-DD_HHMM.odj` until `~/rec/STOP` exists (it finishes the
current chunk first). It restarts the feed if it exits and stops below 3 GB of free
disk; an hour is ~400 MB. The Python tools cache each journal's summary in
`<journal>.stats.json`, keyed on its size and mtime. Parsing an hour takes ~18 s and
~400 MB, rerunning from the cache ~1.6 s and ~20 MB, so re-scoring every session
as new ones arrive stays cheap.

The machine sleeping will stop a recording. While a laptop sleeps, Deribit drops the
WebSocket, but the client never hears about it, so the read used to block forever
(the feed showed "connected" at 0 tickers/s for hours). The feed now drops any
connection that has sent nothing for 3 heartbeat intervals (30 s) and reconnects.
Deribit also closes long-lived connections every few hours; the reconnect backoff now
starts again at 1 s after a session that ran for a while. It used to keep doubling,
toward 30 s of lost data per close.

`od_record` journals both bus topics (binary, append-only, starting from the full
last-value-cache state). `od_replay` runs the engine over the journal with the
recorded timestamps as its clock and synchronous surface refits, so a replay is
deterministic: replaying 19 minutes of real data twice gave byte-identical output,
and a unit test checks the same on a synthetic market. It runs ~40× faster than
real time, mostly spent refitting the surface every simulated second.

Where trades happened in an 18.7-minute recording (`journal_stats.py`): 135 trades,
concentrated in the daily expiries (10OCT26: 40, 11OCT26: 37) and 25DEC26 (39);
104 of 135 within 5% of the forward; median size 0.2 contracts; 87 seller-initiated.

Replaying that recording with different configs:

| config | trades in quoted | at our price | fills | contracts | P&L (BTC) | fees (BTC) | edge at fill | 5 s mark-out |
|---|---|---|---|---|---|---|---|---|
| 30OCT + 27NOV, ±8% (26 options), ±1.5 vol | 2 | 0 | 0 | 0 | 0 | 0 | | |
| same, ±0.4 vol | 2 | 0 | 0 | 0 | 0 | 0 | | |
| 5 expiries 16OCT–25DEC, ±15%, ±0.4 vol | 45 | 1 | 0 | 0 | 0 | 0 | | |
| dailies + 25DEC, ±6%, ±0.8 vol | 81 | 18 | 6 | 3.0 | +0.00003 | 0.00033 | +1.11 vol | +0.52 vol |
| dailies + 25DEC, ±6%, ±0.4 vol | 81 | 26 | 19 | 7.5 | −0.00137 | 0.00161 | +0.62 vol | +0.25 vol |

What it says, with the caveat that 19 minutes and 19 fills is a small sample:

- **Quote where the flow is.** The first configs quoted expiries that barely traded.
- **Tighter quotes trade more but earn less per contract**: 2.5× the volume at half
  the edge.
- **Fees decide it.** Deribit's option fee is capped at 12.5% of the premium, and
  the fills were in cheap dailies (0.0002–0.0023 BTC), so fees ran at roughly 10% of
  premium. The ±0.4 config made about +0.00024 BTC before fees and lost after them.
- **Mark-outs were positive in this sample**, so no sign of adverse selection
  yet; that needs far more fills to say anything.
- On cheap dailies one tick can be several vol points, so the quoter drops a side
  when rounding moves it more than `max_tick_vol` (5 vol) from where it wanted to be.
  Before that, the risk gate's fat-finger band was rejecting ~14,500 such orders
  per run.

**Fee-aware quoting.** Following that result, the quoter now only shows a side that
would add to the position if its edge over theo, after tick rounding, covers the
fee on a fill (`min_edge_to_fee`, default 1×). Position-reducing sides are always
shown: getting out of inventory is worth paying for. The table above was run
without it. The same recording with it on:

| config | fills | contracts | P&L (BTC) | fees (BTC) | edge at fill | 5 s mark-out |
|---|---|---|---|---|---|---|
| dailies + 25DEC, ±6%, ±0.4 vol, no fee rule | 19 | 7.5 | −0.00137 | 0.00161 | +0.62 vol | +0.25 vol |
| same, fee rule | 3 | 1.5 | −0.00010 | 0.00009 | +0.85 vol | +0.32 vol |
| dailies + 25DEC, ±6%, ±0.8 vol, fee rule | 5 | 2.5 | +0.00005 | 0.00032 | +1.16 vol | +1.02 vol |

The rule cut the tight config's loss by ~93% by declining fills whose fees
exceeded their edge, at the cost of 80% of the volume. With 3–5 fills these are
directions, not conclusions; the next step is hours of recordings across different
sessions.

## Testnet order gateway (M5)

Real orders on [Deribit testnet](https://test.deribit.com) (play money). The gateway's
host is a compile-time constant, `test.deribit.com`; there is no way to point it at
the real exchange.

```bash
# API key from test.deribit.com, scopes trade:read_write + account:read, stored outside the repo:
#   ~/.config/optionsdesk/testnet.env   (export DERIBIT_CLIENT_ID=... / DERIBIT_CLIENT_SECRET=...)
source ~/.config/optionsdesk/testnet.env
./build/od_gw_smoke                                     # end-to-end check, leaves nothing open
./build/od_feedd --currency BTC --testnet               # testnet market data -> od_feed_BTC_testnet
./build/od_mm --gateway testnet --config examples/mm_btc_testnet.json
```

```
strategy thread                            gateway thread (Boost.Asio, async TLS WebSocket)
  place / amend / cancel ──[SPSC ring]──>    pacing queue ──> JSON-RPC ──> test.deribit.com
         + io_context::post (wake-up)
  poll() <────────────────[SPSC ring]────    execution reports <── user.orders / user.trades
```

The strategy thread never touches the network: commands and execution reports cross
threads through fixed-size SPSC rings, and `post` only wakes the gateway. Protocol
logic (`DeribitOrders`) has no I/O and is unit-tested: amends and cancels issued
before the exchange acknowledges an order are held and sent once it does; the same
fill reported by the RPC response and by `user.trades` is counted once; a refused
amend reports the price still working; orders from other runs are ignored.

On every connection, before reporting ready: authenticate (refreshing the token
before expiry), enable **cancel-on-disconnect** (if the connection dies, the exchange
cancels our orders), read the account's rate limits, report existing positions
(`od_mm` refuses to trade on top of positions it didn't create, unless told to
`--adopt-positions` them into risk), cancel leftover orders, subscribe.

`od_gw_smoke` against testnet:

| step | result | round trip |
|---|---|---|
| connect, auth, cancel-on-disconnect, positions | ok, account flat | 1.9 s |
| resting option bid at the minimum price | ack | 211 ms |
| amend by one tick | ack at the new price | 215 ms |
| cancel | confirmed | 203 ms |
| post-only bid at the offer | rejected (`post_only_reject`), not traded | 203 ms |
| buy / sell $10 of the perpetual | filled, fee 6e-8 BTC each | 208 / 210 ms |

The ~200 ms is network distance to the exchange; the reported perpetual fee matches
the model in `od/fees.hpp` (0.05% × $10 / price).

**Rate limits.** The first market-making run on testnet was disconnected by the
exchange every 15–30 s. Capturing the WebSocket close frame showed why:
`over_limit`. The account allows **5 trading requests per second (burst 20) across
place, amend and cancel combined**, and quoting ~50 options both sides with
frequent requotes is far beyond that (it's what Deribit's mass-quote API exists for).
The engine's token bucket only covered new orders and amends; cancels bypassed it.
Now the gateway reads the limits from the account on connect and paces every trading
request at 80% of them through a queue (cancels first, nothing dropped), cancels
spend engine tokens too, and the testnet config quotes 14 options with 1 s requotes.
The next 3-minute run: no disconnects, ~20 quotes live throughout, and at exit
Deribit's own API reported 0 open orders and 0 positions.

The testnet options market is thin and sometimes unrealistic (one expiry was quoted
77% vol bid / 400% offered), so testnet shows that the plumbing works, not what a
strategy would earn.

### Request budget allocator

Deribit's answer to the rate limit is `private/mass_quote` (up to hundreds of quotes
per request), but the API spec says it "can only be used after approval from the
administrators", and a probe on this account confirmed it: `mass_quotes_disabled`,
and market maker protection (which mass quotes require) `MMP disabled`. So the engine
has to make 4 requests/s count.

Each requote cycle now plans every wanted change and spends the budget on the most
valuable ones (`allocate_requests`, on by default), scored in USD:

- **cancel** a quote we no longer want: always first.
- **amend**: |resting price − wanted price| × size × index, ×3 if the resting order
  has become too generous (bid too high / offer too low, the side that gets picked off).
- **place**: the edge it would earn, |theo − wanted price| × size × index.
- every score × (1 + recent trades in that option, decayed with a 5-minute half-life).

Quote quality is sampled every cycle over every side we want to show: the share with a
quote up, how far resting prices are from target, and the share resting on the
generous side. Replaying the 18.7-minute recording, quoting options 0.3–25 days out
within 10% of the forward, against the old behaviour (update instruments in a fixed
order until the rate limit refuses):

| budget | approach | quotes up | off target | too generous | fills |
|---|---|---|---|---|---|
| 2/s | fixed order | 41.2% | 0.10 vol | 2.5% | 6 |
| 2/s | allocator | **58.4%** | 0.07 vol | **0.9%** | 0 |
| 4/s | fixed order | 81.8% | 0.07 vol | 3.0% | 7 |
| 4/s | allocator | **86.8%** | 0.06 vol | **1.1%** | 4 |
| 8/s | fixed order | 94.4% | 0.03 vol | 1.2% | 8 |
| 8/s | allocator | **95.7%** | 0.03 vol | **0.7%** | 6 |

The allocator keeps more quotes up and cuts the dangerous stale ones by 40–65%, most
when the budget is tight. It fills less, though. Its USD scores favour high-vega,
longer-dated options, while the fixed order happens to start with the dailies (the
exchange lists instruments by expiry), which is where the trades are. The activity
weight should correct for that, but with ~65 trades in the quoted options over 19
minutes it barely moves. A flow prior by expiry and moneyness (from `journal_stats.py`)
is the next refinement; fills this few can't separate the two approaches on P&L.

### Flow prior, and an out-of-sample test

Weighting edge by (1 + trades) couldn't overcome the edge gap between cheap
short-dated options and expensive long-dated ones, so the allocator now scores a
quote by **expected profit per minute: edge × expected trades per minute** in that
option. Expected trades blend a flow prior (trades per minute per option, by days to
expiry × |ln K/F|, worth `prior_strength_min` minutes of observation) with the trades
actually seen, with a small floor so options without history still get budget.

```bash
python3 tools/journal_stats.py ~/btc.odj --profile data/flow/btc_2026-10-09.json
# then in the config: "flow_prior": "../data/flow/btc_pooled_2026-10-18.json"
```

The prior in `data/flow/` was built from the 18.7-minute Oct 9 recording, where
near-the-money options under 2 days traded ~0.14–0.15 times a minute each, 30–50×
more than most other buckets. It was then tested on a **30-minute recording from
Oct 14 that it had never seen**:

| 2 requests/s | quotes up | share of requests to options < 2 days | fills |
|---|---|---|---|
| fixed order | 25.0% | 83.1% | 3 |
| allocator | 52.6% | 21.3% | 0 |
| allocator + Oct 9 prior | 52.1% | 44.6% | 0 |

The prior moved the budget exactly as designed, and the quote-quality gain held. But
**the flow had moved**: on Oct 14 the busiest expiry was 30OCT26, 16 days out (35 of
65 trades, 153 contracts), and the dailies had 10 trades, against 77 of 135 on Oct 9.
So the prior pushed budget toward where trades *had* been. The fixed order's 3 fills
were all buys at the 0.0001 minimum price, worth essentially nothing, so no approach
earned anything that could be compared.

What this shows: the mechanism works out of sample, but **a prior from one short
session is not reliable, because where options flow concentrates changes from day to
day.** The next steps are a prior averaged over many sessions and faster adaptation
to what is trading now (a shorter `prior_strength_min`). Judging any of this on P&L
needs hours of recordings, not minutes.

### Scoring priors out of sample

To test a prior without P&L, `tools/prior_eval.py` and `tools/prior_walk_forward.py`
score how well it predicts where trades land in a journal it wasn't built from: the
Poisson log-likelihood of each live option's trade count, in **bits per trade against
a flat rate** taken from the test journal itself. Positive means the prior knows where
trades go better than "every option is equally likely".

```bash
python3 tools/prior_walk_forward.py ~/btc_session.odj ~/btc_oct14.odj ~/rec/btc_*.odj     --shrink 0 300 1000 3000 --shape
python3 tools/journal_stats.py <journals...> --profile data/flow/btc_pooled_2026-10-18.json --shrink 1000
```

Eighteen sessions, 964 minutes: Oct 9 (19 min), Oct 14 (30 min), Oct 15 (60 min), two
consecutive chunks early on Oct 17 (60 and 31 min), three hours on the evening of Oct
17, and ten hours on Oct 18 (03:50 to 13:40), all but the first five recorded
unattended. Each prior is built only from sessions before the one it's tested on,
giving 1923 test trades:

| prior (trade-weighted bits/trade vs flat) | raw | shrink 300 | shrink 1000 | shrink 3000 |
|---|---|---|---|---|
| last session | −0.02 | +0.47 | **+0.54** | +0.41 |
| all earlier sessions pooled | +0.18 | +0.41 | +0.48 | +0.50 |
| last session, shape only | +0.19 | +0.67 | +0.75 | +0.62 |
| pooled, shape only | +0.56 | +0.77 | **+0.82** | +0.81 |

Two things go wrong with a raw prior. It is **overconfident**: a bucket that didn't
trade in a short session gets a rate near zero, and when it then trades the miss is
expensive. And the **level moves**: Oct 9 traded 0.0071 times a minute per option,
Oct 14 0.0024. `--shrink N` fixes the first by adding N option-minutes at the average
rate to every bucket; `--shape` sets aside the second by rescaling each prior to the
test session's total. Shape is what matters to the allocator, which only ranks options
against each other. Shrinking by ~1000 option-minutes is best throughout.

Per session, the test gets easier as history grows. With three sessions, the most
recent session alone beat the pool (Oct 9's daily-heavy flow diluted the Oct 15
prediction); from four on, pooling won. Consecutive hours are alike: the previous hour
alone, shrunk, beat flat in every one of the ten Oct 18 hours. On level, it now edges
out the pool (+0.54 vs +0.48), while the pool still ranks options better (+0.82 vs
+0.75). The flow moves: on the evening of Oct 17 from the dailies to 30OCT26 and
20OCT26, then to 6NOV26. In the 23:29 hour the pool scored −0.51 while the previous
hour alone scored +0.85.

Within the engine's blend, which is the score that matters, I tried weighting sessions
by age (half-life 0.5 to 48 hours):

| blend (m = 10), shrink 1000 | half-life 0.5 h | 1 h | 3 h | 12 h | 48 h | equal |
|---|---|---|---|---|---|---|
| bits/trade vs flat | +1.13 | +1.18 | +1.24 | +1.34 | +1.34 | +1.33 |

Short half-lives are clearly worse: they throw away too many trades, and the blend with
live trades already handles recency. A 12–48 hour half-life gains 0.01–0.02 bits/trade
over equal weights, too little to be worth the extra parameter, so pooling stays equal.
`data/flow/btc_pooled_2026-10-18.json` is all eighteen sessions pooled (964 min, 2070
trades), shrunk by 1000.

Replaying the Oct 15 hour at 2 requests/s with a prior from Oct 9 + 14 only (shrunk by
1000) put the request budget between no prior and the raw Oct 9 prior, as intended:

| 2 requests/s, Oct 15 | quotes up | too generous | requests to options < 2 days | fills |
|---|---|---|---|---|
| fixed order | 67.2% | 8.1% | 79.1% | 11 |
| allocator | 91.0% | 2.8% | 42.2% | 4 |
| allocator + raw Oct 9 prior | 85.0% | 4.1% | 64.6% | 2 |
| allocator + shrunk Oct 9 + 14 prior | 89.2% | 3.3% | 54.7% | 4 |

Fills are still too few to compare on P&L, so the bits-per-trade score is the measure
to use for priors for now.

### How much to trust the prior: `prior_strength_min`

The engine expects an option to trade at (prior × m + activity) / (m + 7.2) per
minute, where activity is the option's trades so far, decayed with a 5-minute
half-life, and m is `prior_strength_min`. `tools/prior_strength.py` replays that
forecast minute by minute over each session, using a prior pooled only from earlier
sessions, and scores it on the next minute's trades:

| m (minutes) | 0 (now only) | 1 | 3 | **10** | 30 | 100 | 300 | prior only |
|---|---|---|---|---|---|---|---|---|
| bits/trade vs flat (1887 trades) | −1.07 | +0.52 | +1.09 | **+1.30** | +1.16 | +0.92 | +0.73 | +0.48 |
| shape only | −1.06 | +0.53 | +1.13 | **+1.42** | +1.40 | +1.22 | +1.06 | +0.82 |

The default of 10 is the best setting. Over 17 test sessions, m = 3 won only four hours,
by at most 0.05 bits/trade, and m = 30 two. Blending the two nearly triples the score of
the prior alone. Recent trades alone are worse than flat, because one trade in a quiet
option says little. The prior alone misses the drift within a session. The example
config (`examples/mm_btc.json`) now uses the pooled prior with m = 10.

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
| `src/feed_bus.cpp` | Shared-memory broadcast ring + last-value cache (writer and reader) |
| `apps/od_feedd.cpp` | Feed daemon publishing to the shared-memory bus |
| `src/quoter.cpp` | Vol-space quoting, inventory skew, Deribit tick ladder |
| `src/risk_gate.cpp` | Pre-trade checks, token bucket, kill switch |
| `src/orders.cpp` | Order state, positions, cash, fees, inverse-perp ledger |
| `src/gateway.cpp` | Gateway interface, simulated exchange, hedge sizing |
| `src/mm_engine.cpp` | Market-making engine: quoting, gate, fills, hedge, mark-outs |
| `apps/od_mm.cpp` | Live driver for the engine on the feed buses |
| `src/journal.cpp` | Binary market-data journal (writer and mmap reader) |
| `apps/od_record.cpp` | Records the feed buses to a journal |
| `apps/od_replay.cpp` | Deterministic replay of the engine over a journal |
| `tools/journal_stats.py` | Where trades happen in a journal (expiry, moneyness, size); builds flow priors |
| `tools/prior_eval.py` | Scores flow priors on journals they weren't built from |
| `tools/prior_walk_forward.py` | Walk-forward test of flow priors over consecutive journals |
| `tools/prior_strength.py` | Tunes how the engine weighs the flow prior against recent trades |
| `tools/record_loop.sh` | Records hourly journal chunks until told to stop |
| `src/deribit_orders.cpp` | Order-entry protocol: requests out, execution reports in (no I/O) |
| `src/deribit_gateway.cpp` | Testnet gateway thread: TLS WebSocket, rings, rate-limit pacing |
| `apps/od_gw_smoke.cpp` | End-to-end gateway check against testnet |

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
