# UI rules (ui/)

The plan and full requirements are in `docs/UI.md`. Follow these rules when
working in this folder.

## Two builds
- `console` (localhost via `od_web`) has every page and control.
- `demo` (Vercel, static replay files) has About, Desk with the replay
  player, Pricer, and read-only Research.
- Control code (market maker commands, orders, configs, system) lives under
  `src/console/`. It is imported only behind `import.meta.env.MODE === "console"`
  checks, so the demo bundle never contains it.
- Pages are reached from a sidebar, one page at a time. The style is a pro
  trading terminal: dark and dense.

## Data
- The snapshot contract is owned by `od::web::snapshot_json()` in `src/web_snapshot.cpp`.
  `src/types.ts` mirrors it. Change both in the same commit and update the
  contract section in `docs/UI.md`.
- Wire units:
  - IV is decimal and shown as %, 2 decimals;
  - prices are in the coin and shown with 4 decimals, USD on hover (× forward or index);
  - vega is USD per vol point;
  - gamma is per 1% move;
  - times are epoch ms and shown in UTC.
- The UI computes nothing financial that the server already computes (fits, Greeks,
  IVs). Derived display values (%, USD conversions, sparkline history) live in
  `src/lib`, with Vitest tests.
- The one exception is demo replay files, which leave out the Greeks and the smile
  points and curve to keep the static site small. `src/lib/demo.ts` rebuilds them with
  `src/lib/black76.ts`, a port of the C++ pricer, and `demo.test.ts` checks the
  result against a C++ snapshot. If you change the pricing or surface code in C++,
  change the port too and regenerate the fixture (`od_snapshots --fixture`).
  The Pricer page (`src/lib/pricer.ts`) also prices with the port, but only the
  user's own inputs; `pricer.test.ts` checks it against the C++ test cases.
- An IV of 0 or a missing `fit` means "no value". Show `–`, never 0%.

## Structure
- `src/store/`: the one WebSocket connection (reconnect with backoff) and snapshot
  store, read with `useSyncExternalStore`; `replay.ts` feeds the same store in the demo.
- `src/panels/`: one component per panel. Each handles its loading, empty and stale states.
- `src/charts/`: ECharts wrappers. Update with `setOption`, don't remount every tick.
- `src/lib/`: formatting and pure helpers.
- `src/pages/`: Desk `/` (`/desk` in the demo, where About is `/`), Pricer `/pricer`,
  later `/mm`, `/research`, `/system`.

## Conventions
- TypeScript strict. No `any` on snapshot data.
- Plain CSS with the variables in `src/styles.css`:
  - dark theme by default;
  - numbers in `.num` (monospace, tabular, right-aligned);
  - green is bid/buy, red is ask/sell.
- Persist UI preferences only (selected expiry, axis mode, theme) in
  `localStorage`, inside try/catch.
- Don't add dependencies beyond React, ECharts and Vitest without a reason in
  the commit message.

## Safety
- Never show old data as live: dim panels when the socket is down, the feed is
  stale or disconnected, or the snapshot is older than 3 s.
- Manual orders:
  - testnet only, in the console build only;
  - they go through `od_mm` (its risk gate), never directly to the exchange;
  - each one passes the safety limits (size, notional, price band against
    theo) and a confirmation step.
- Every control action is a POST to `od_web` and is written to the audit log.
- No secrets in the frontend. The Telegram token stays in
  `~/.config/optionsdesk/telegram.env`, read by `od_web`.

## Running
- WSL:
  - `~/build/optionsdesk/od_feedd --currency BTC`
  - `~/build/optionsdesk/od_web`
- Windows, in `ui/`:
  - `npm run dev` serves on http://localhost:5173 and proxies `/ws` and `/api` to :8090;
  - `npm run mock` uses the saved fixture (`src/fixtures/snapshot.json`) instead of a live feed;
  - `npm run demo` is the demo build's player over `public/sessions` (exported with
    `od_snapshots`), and `npm run build:demo` writes it to `dist-demo/`.
- Before committing: `npm run typecheck && npm run test && npm run build`.
