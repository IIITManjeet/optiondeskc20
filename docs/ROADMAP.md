# Roadmap

The end state is a small but realistic options desk running on Linux, paper
trading on Deribit testnet:

```
                ┌──────────────┐  SPSC ring   ┌───────────┐     ┌───────────┐
Deribit WS ───> │ feed handler │ ───────────> │ pricer +  │ ──> │ strategy  │
(book, ticker,  │ (book build, │              │ vol surf. │     │ (MM/hedge)│
 trades)        │  seq checks) │              └───────────┘     └─────┬─────┘
                └──────┬───────┘                                      │ orders
                       │ record                                ┌──────▼──────┐
                ┌──────▼───────┐                               │ pre-trade   │
                │ journal      │ ── replay ──> backtester      │ risk gate   │
                └──────────────┘                               └──────┬──────┘
                                                                      │
          positions / Greeks / PnL  <──  fills  <── order gateway <───┘
                 (risk engine)                    (Deribit testnet, JSON-RPC)
```

## M1: Pricing core ✅
Black-76 + Greeks, IV solver, SVI per expiry, arbitrage diagnostics, REST
snapshots with offline replay, tests and benchmarks.

## M2: Streaming market data ✅
- Boost.Beast TLS WebSocket on a dedicated, optionally pinned feed thread;
  `ticker.{instrument}.100ms` for every option, heartbeats, reconnect with backoff,
  `shutdown(2)`-based interrupt for prompt Ctrl-C.
- simdjson On-Demand parser on the hot path (nlohmann kept for rare control messages).
- Lock-free SPSC ring (cache-line padded, cached indices, acquire/release) into the
  pricer thread; drops counted, never blocks the feed.
- Surface fits on a third thread fed by a latest-wins mailbox, so a 20–30 ms refit
  never stalls the ring drain.
- Log-linear latency histograms (parse, hop, snapshot, fit); busy-poll vs sleep-poll.
- Shared-memory feed bus: `od_feedd` publishes into `/dev/shm`; any number of
  reader processes (`od_live --bus`, `od_risk --bus`) attach. Seqlock broadcast ring,
  last-value cache for attach/lap resync, producer heartbeat and reattach.

Deferred: `book.*` channels with `change_id` / `prev_change_id` gap recovery (needed
once we quote, M4); refitting only expiries that changed.

## M3: Position and risk engine ✅
- Options, inverse perpetual and coin balance; equity and P&L in both USD and coin.
- Per-position analytic Greeks (incl. premium-adjusted delta); portfolio delta and
  gamma by bump-and-reprice; vega bucketed by expiry.
- Scenario grid over spot × vol × days forward, revalued on the SVI surface,
  sticky-moneyness or sticky-strike.

Deferred: live risk in `od_live` (lands with the strategy in M4), perp funding/basis,
term-structure-weighted vol shocks.

## M4: Strategy (paper) ✅
- Vol-space quoting around the SVI theo at the live forward, inventory skew by
  portfolio vega, Deribit tick ladder, post-only, per-side position limits, and
  no quote where the tick is too coarse in vol terms.
- Pre-trade risk gate (size, worst-case position, vega, fat-finger band, rate limit,
  kill switch), order manager and ledger, perpetual delta hedger.
- Simulated exchange with a queue-position fill model driven by public trade prints;
  mark-outs at 1/5/30 s.
- Strategy extracted into an engine that drivers feed with data and time.

Next: more data before trusting any parameter (hours of recordings across sessions),
a fee-aware quoting rule (don't quote where expected edge < fees), and vega limits
per expiry for the dailies.

## M5: Order gateway + pre-trade risk
- Authenticated JSON-RPC over WebSocket to **test.deribit.com** (testnet API keys
  read from the environment, never committed).
- Risk gate: max order size, position and vega limits, price bands vs theo,
  message throttle, kill switch (signal or admin socket).
- Learn: order state machines, idempotency, cancel-on-disconnect.

## M6: Record, replay, backtest ✅ (ahead of M5: it doesn't need exchange keys)
- `od_record` journals both bus topics to a binary, append-only file; the reader
  maps it read-only and tolerates a truncated tail.
- `od_replay` drives the same engine from a journal with recorded timestamps and
  synchronous refits: deterministic, ~40x real time, compares configs side by side.
- `tools/journal_stats.py` shows where trades happen.

## M7: Run it like production
- systemd units with `CPUAffinity=`, watchdog, restart policy.
- Prometheus metrics (feed lag, fit RMSE, Greeks, PnL) and a Grafana dashboard.
- Latency lab: `isolcpus`, IRQ affinity, busy-polling; before/after histograms.
  Needs bare-metal Linux for meaningful numbers.
