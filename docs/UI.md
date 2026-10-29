# Web UI: plan and requirements

The browser front end for optionsdesk. The C++ processes stay the source of truth:
the UI displays what they publish and sends them commands. Rules for working on
the UI are in [`ui/CLAUDE.md`](../ui/CLAUDE.md).

## Decisions (2026-10-22)

| Topic | Decision |
|---|---|
| Who and where | A **public portfolio demo** and a private **console** for the owner |
| Hosting | Demo: **Vercel**, static, built from `ui/` on every push to `main` (`ui/vercel.json`). Console: **localhost only**, served by `od_web` in WSL |
| Live vs demo data | Console: live from `od_feedd`. Demo: **replays of recorded sessions** (pre-rendered snapshot files), so it never looks dead and needs no server |
| Access | Demo is read-only, with no controls compiled in. Console has full control and needs no login, because it binds 127.0.0.1 and is never exposed |
| Market maker control | Everything: monitor, kill switch, start/stop with a config, **live parameter tuning**, **manual orders** |
| Manual trading (testnet only) | Click the chain for an order ticket; blotter with cancel/amend; flatten a position or delta-hedge; safety limits (max size, max notional, price band against theo, confirm step) |
| Flows | Watch the market, run and monitor the market maker, research and backtests, operate the system |
| Markets | **Any Deribit currency** (BTC, ETH, USDC-linear such as SOL/XRP): currency switcher, one `od_feedd` per currency |
| History | **SQLite** at `D:\optionsdesk-rec\desk.db`: market maker sessions and P&L, vol-surface history, replay results, alerts and audit log |
| Config editor | Quoting, risk limits, flow prior and allocator, **named presets** with diff |
| Alerts | In-app toasts, browser notifications, sound, **Telegram bot** |
| Demo extras | About / project story, session picker with play/pause/speed/scrubber, read-only research results, interactive Black-76/SVI pricer |
| Layout | **Sidebar navigation with one page at a time** |
| Style | **Pro trading terminal**: dark, dense, monospace tabular numbers |
| Build order | Live Desk → **public demo** → market maker console → manual trading → history and alerts → ops and multi-currency |

## Architecture

```
                      ┌──────────────────────── this PC (WSL) ──────────────────────────┐
Deribit ──WS──> od_feedd (per currency) ──shm──> od_web ──WS /ws, REST /api──> console UI (localhost)
                         └──> record_loop ──> D:\optionsdesk-rec\*.odj       │  ├─ spawns / commands od_mm
                                                                             │  ├─ runs od_replay
                         od_mm <──status bus + command socket──>             │  ├─ SQLite desk.db
                                                                             │  └─ alerts → Telegram
                      └──────────────────────────────────────────────────────────────────┘
journals ──od_snapshots (export)──> ui/public/sessions/*  ──Vercel build──> demo site
```

**One UI, two builds**, selected by a Vite mode:
- `console`: data from `od_web` over WebSocket and REST, with every page.
- `demo`: data from static files, read-only pages only. Control code is not
  compiled in, so nothing on the internet can send a command.

**Server pieces**

| Piece | Status | Role |
|---|---|---|
| `od_web` | ✅ live snapshots | Attaches to the buses, refits the surface, pushes snapshots, serves `ui/dist`. Will grow: several currencies, the market maker proxy, the replay runner, SQLite, alerts |
| `src/web_snapshot.cpp` | ✅ | `od::web::snapshot_json()`, moved out of `od_web` so the exporter uses exactly the same format |
| `od_snapshots` | ✅ | Journal → compact demo files (see *Demo files* below): about 1.2 MB per recorded hour at a snapshot every 5 s. Kept small so the demo stays a cheap static site |
| `od_mm` status and commands | to do | A status bus with quotes, positions, P&L, Greeks, fills, budget and rejects. A command socket for kill, set parameters, manual place/cancel/amend, flatten and hedge. Manual orders go through `od_mm`'s risk gate, never around it |

## Pages

The sidebar shows the console pages; the demo build shows only the pages marked demo.

| Page | Build | Content |
|---|---|---|
| **About** | demo | Project story, architecture diagram, latency numbers, flow-prior results, GitHub link |
| **Desk** | both | Header (currency, index, exchange time, feed state, data age), term-structure table with arbitrage flags, smile chart (bid/ask IV bars, mid, mark, SVI curve; strike or log-moneyness axis), term-structure chart, option chain (calls │ strike │ puts), trade tape, feed health. In the demo it is driven by the **replay player**: session picker, play/pause, 1–60× speed, scrubber |
| **Pricer** | both | Black-76 and SVI playground in the browser: change F, K, T, vol or the SVI parameters and see price, Greeks and the smile. TypeScript port checked against the C++ results |
| **Research** | demo read-only, console full | Flow-prior heatmap (days × moneyness), walk-forward and `prior_strength_min` score tables, backtest comparisons. Console adds: journal list, a replay runner (journal + two presets → `od_replay` → side-by-side fills, P&L, mark-outs, quote quality), saved runs |
| **Market maker** | console | Mode badge (SIM/TESTNET), start/stop with a preset, kill switch, quotes against theo with edge and queue, positions and P&L (coin and USD, chart), Greeks against limits, fills with mark-outs at 1/5/30 s, request budget, quote quality, rejects, **live tuning** panel |
| **Orders** | console | Order ticket opened from a chain click (side, price, size, post-only), open-orders blotter (cancel, amend), flatten a position, delta-hedge with the perp, safety-limit settings. Testnet only |
| **Configs** | console | Preset editor: quoting (edge, size, requote, expiries, moneyness), risk (delta, vega per expiry, positions, hedge), flow prior (file, `prior_strength_min`, allocator). Save, name, diff, choose one when starting a run or replay |
| **History** | console | Market maker sessions (config, fills, P&L curve), vol-surface history (ATM, RR25, BF25 per expiry over days), replay results, audit log |
| **System** | console | Processes (`od_feedd` per currency, `od_web`, `od_mm`, recorder) with start/stop, recorder chunk, free disk, log tails, alert rules and Telegram setup |

## Flows

1. **Visitor (demo)**:
   - lands on About;
   - opens Desk, where the newest recorded session autoplays;
   - picks another session, scrubs and changes speed;
   - tries the Pricer;
   - reads the Research results.
2. **Watch the market (console)**:
   - start `od_feedd` for the currencies from System or the terminal;
   - Desk shows them live;
   - switch currency and expiry.
3. **Run the market maker**:
   - pick a preset in Configs;
   - start SIM or TESTNET on the Market maker page;
   - watch quotes, fills, P&L and risk, and tune parameters live;
   - stop or use the kill switch.

   Every session is saved to History.
4. **Trade by hand (testnet)**:
   - click a price in the chain to open the ticket;
   - the safety checks run (size, notional, price band against theo, confirm);
   - the order goes through `od_mm`;
   - the blotter can cancel or amend it;
   - flatten or hedge from Orders.
5. **Research**:
   - pick journals and two presets in Research and run the replay;
   - the comparison is saved to History;
   - export a session to the demo.
6. **Operate**:
   - System shows process health, recorder state and disk;
   - alerts fire as toasts, browser notifications, sounds and Telegram messages;
   - every control action is in the audit log.

## Alerts

Rules are evaluated in `od_web`, so they work without a browser open.

| Alert | Default |
|---|---|
| Feed stale or disconnected | more than 10 s |
| Snapshot or fit failing | |
| Risk limit breached | |
| Kill switch tripped | |
| `od_mm` exited unexpectedly | |
| Fill larger than a set size | |
| Recorder stopped | |
| Disk below a set space | |

Delivery:
- Toast and sound in the UI.
- A browser Notification when the tab is in the background.
- Telegram, for the urgent ones.
  - The bot token and chat id go in `~/.config/optionsdesk/telegram.env`,
    never in the repo or the browser.

## Storage (SQLite, `D:\optionsdesk-rec\desk.db`)

Tables:
- `mm_sessions` (id, start, end, mode, currency, preset json, summary);
- `fills`;
- `pnl_points`;
- `surface_points` (ts, currency, expiry, atm, rr25, bf25, forward), one row
  per minute per expiry;
- `presets` (name, json, updated);
- `replay_runs` (journals, presets, results json);
- `alerts`;
- `audit_log` (ts, action, params, result).

Only `od_web` writes the database. Back it up by copying the file.

## Data contract (snapshot, `type: "snapshot"`)

Defined by `od::web::snapshot_json()` (`include/od/web_snapshot.hpp`) and mirrored by `ui/src/types.ts`. Demo files
store a compact form of it that the browser expands back to this shape (see *Demo files*).

```
server_ms, currency, asof_ms, index, fit_ms
feed: null (waiting_for: bus name) | {bus, connected, stale, subscribed, instruments, live,
      updates_per_s, trades_per_s, hop_p50_us, hop_p99_us, frames, tickers, trades,
      heartbeats, reconnects, drops, laps}
expiries[]: {label, expiry_ms, days, forward,
      fit?: {atm_iv, rr25, bf25, rmse, n_fit, n_inside, min_g, calendar, k_lo, k_hi, svi{a,b,rho,m,s}},
      curve?: [[k, iv], ...],
      points?: [{strike, k, type C|P, bid_iv, ask_iv, mid_iv, mark_iv, in_fit}],
      chain: [{strike, call|null, put|null: {name, bid, ask, mark, mark_iv, oi, delta, gamma, vega, theta}}]}
trades[]: {name, ts_ms, side buy|sell, price, amount, iv, index}   newest first
```

Units on the wire:
- IVs are decimal (0.42);
- prices are in the coin; USDC-linear options are priced in USDC;
- `vega` is USD per vol point;
- `gamma` is the delta change per 1% move;
- `theta` is USD per day;
- times are epoch ms UTC.

`fit` is missing for expiries under 2 hours or without enough quotes. An IV of 0
means it could not be solved. The market maker messages (`type: "mm"`, `"order"`,
`"alert"`) will be added here as they are built.

## Demo files

Written by `od_snapshots` into `ui/public/sessions/`, read by `ui/src/store/replay.ts`:

```
sessions/index.json          {format, sessions: [{format, id, currency, journal, start_ms, end_ms, every_s, snapshots, bytes}]}, newest first
sessions/<id>/index.json     the same, plus chunks: [{file, from_ms, to_ms, snapshots, bytes}]
sessions/<id>/NNN.ndjson.gz  10 min of recorded time per file: a compact keyframe, then {"type": "patch", "patch": ...} lines
```

The compact form is the live snapshot minus what the browser can recompute
exactly. `ui/src/lib/demo.ts` rebuilds it, and its tests check the result against
a C++ snapshot (`ui/src/fixtures/demo_pair.json`, from `od_snapshots --fixture`):
- the Greeks of every quote (Black-76 on the mark IV);
- each fitted expiry's `curve` (from the SVI parameters) and `points` (bid/ask IVs
  solved from the prices, with the same `in_fit` rule as `od::build_surface`).

Numbers are rounded to 6 significant digits, forwards to 10. A patch node is a
scalar (replace), `{"=": v}` (replace with an object or array), `{"o": {...}, "d": [...]}`
(patch or delete object keys) or `{"a": {"i": node}}` (patch elements of an array
whose length didn't change). Dropping the derived fields cut an hour from 37 MB to
1.2 MB; delta encoding alone only reached 8–23 MB. `format` is bumped whenever any
of this changes, and the player ignores sessions in other formats.

## Frontend requirements

**Stack**
- React 18, Vite, TypeScript in strict mode, React Router.
- Apache ECharts for charts.
- Plain CSS with variables.
- Vitest for tests.
- No state library at first: a WebSocket store read through
  `useSyncExternalStore`.

**Data**
- One WebSocket per tab, reconnecting with backoff from 1 to 10 s.
- REST for commands and history.
- Demo mode reads `sessions/index.json` and the chunk files with `fetch`.

**Behaviour**
- Never present old data as live: show the data age, and dim panels when the
  socket is down, the feed is stale, or the data is more than 3 s old (live only).
- Every panel has loading, empty and stale states.
- UI preferences go in `localStorage`, inside try/catch.

**Performance**
- At most one render per snapshot; panels are memoised.
- Charts are updated in place, not remounted.
- Live snapshots are about 400 KB/s. Demo files must be compact: rounded, a
  snapshot every few seconds, gzipped.

**Look**
- Pro trading terminal: dark, dense, monospace tabular numbers.
- Green for bid/buy, red for ask/sell.
- Built for 1280 px and up. Demo pages must also stay readable on a phone.

**Safety**
- Demo builds contain no control code.
- The console binds to 127.0.0.1.
- Orders are testnet only and go through `od_mm`'s risk gate.
- Manual orders need a confirmation step and pass the safety limits.
- No secrets in the browser.
- Every command is written to the audit log.

**Testing**
- Vitest for `src/lib` (formatting, the TypeScript pricer checked against the C++
  results).
- A snapshot fixture and a mock mode.
- `npm run typecheck`, `npm run test` and `npm run build` pass before a commit.
- CI builds the demo.

## Phases

1. **Live Desk** ✅:
   - `od_web` ✅
   - UI scaffold, sidebar, WebSocket store, Desk panels ✅
   - fixture and mock mode ✅

   Tested end to end against the live BTC feed: a snapshot every second, at
   most 1.05 s apart.
2. **Public demo**:
   - shared `web_snapshot.cpp` ✅
   - `od_snapshots` exporter and replay player ✅
   - About page and Pricer ✅
   - Research views (read-only, from exported JSON)
   - Vercel deploy: config ✅ (`ui/vercel.json`), project to be connected
3. **Market maker console**:
   - `od_mm` status bus and command socket
   - Market maker page with kill switch and start/stop
   - Configs and presets, live tuning
4. **Manual trading**:
   - order ticket, blotter, flatten and hedge
   - safety limits
5. **History and alerts**:
   - SQLite
   - sessions and P&L, surface history, replay runner and results
   - audit log
   - alerts: toast, browser, sound, Telegram
6. **Ops and multi-currency**:
   - System page with process and recorder control
   - several feeds and the currency switcher
   - recording other currencies
