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

## M2: Streaming market data
- Boost.Beast WebSocket client (TLS) on an `epoll`-driven `io_context`, one pinned thread.
- Subscribe to `book.{instrument}.100ms` / `ticker.{instrument}.100ms` for the chain
  plus the perpetual/futures for the forward; handle `change_id` / `prev_change_id`
  gaps by resubscribing for a fresh snapshot.
- Hand updates to the pricing thread through a lock-free SPSC ring buffer
  (cache-line padded, acquire/release atomics). Measure the hop latency.
- Re-fit only the expiries that changed; publish surface versions.
- Learn: TLS WebSockets, sequence-gap recovery, memory ordering, false sharing.

## M3: Position and risk engine
- Positions per instrument, in coin and USD; portfolio Greeks (premium-adjusted
  delta for inverse contracts), bucketed vega by expiry.
- Scenario grid: PnL under spot ±x% × vol ±y pts, re-priced on the fitted surface.
- Learn: why inverse contracts make delta hedging non-trivial; vega bucketing.

## M4: Strategy (paper)
- Quote a few liquid strikes around a theoretical value from the surface, with
  skew to inventory (Avellaneda–Stoikov-style), and delta-hedge with the perpetual.
- Learn: edge vs adverse selection, gamma/theta trade-off, hedging frequency.

## M5: Order gateway + pre-trade risk
- Authenticated JSON-RPC over WebSocket to **test.deribit.com** (testnet API keys
  read from the environment, never committed).
- Risk gate: max order size, position and vega limits, price bands vs theo,
  message throttle, kill switch (signal or admin socket).
- Learn: order state machines, idempotency, cancel-on-disconnect.

## M6: Record, replay, backtest
- Journal every inbound message with receive timestamps (binary, append-only, `mmap`).
- Deterministic replay into the same strategy binary; fill model with queue
  position and latency.
- Learn: event sourcing, why naive backtests overstate PnL.

## M7: Run it like production
- systemd units with `CPUAffinity=`, watchdog, restart policy.
- Prometheus metrics (feed lag, fit RMSE, Greeks, PnL) and a Grafana dashboard.
- Latency lab: `isolcpus`, IRQ affinity, busy-polling; before/after histograms.
  Needs bare-metal Linux for meaningful numbers.
