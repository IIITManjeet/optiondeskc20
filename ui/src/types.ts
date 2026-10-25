// Mirror of od::web::snapshot_json() in src/web_snapshot.cpp. Change both together (see docs/UI.md).
// Units: IVs decimal, prices in the coin, vega USD per vol point, gamma per 1% move,
// theta USD per day, times epoch ms UTC.

export interface FeedStats {
  bus: string;
  connected: boolean;
  stale: boolean;
  subscribed: number;
  instruments: number;
  live: number;
  updates_per_s: number;
  trades_per_s: number;
  hop_p50_us: number;
  hop_p99_us: number;
  frames: number;
  tickers: number;
  trades: number;
  heartbeats: number;
  reconnects: number;
  drops: number;
  laps: number;
}

export interface SviParams {
  a: number;
  b: number;
  rho: number;
  m: number;
  s: number;
}

export interface SmileFit {
  atm_iv: number;
  rr25: number;
  bf25: number;
  rmse: number;
  n_fit: number;
  n_inside: number;
  min_g: number;
  calendar: number;
  k_lo: number;
  k_hi: number;
  svi: SviParams;
}

export interface SmilePoint {
  strike: number;
  k: number;
  type: "C" | "P";
  bid_iv: number;
  ask_iv: number;
  mid_iv: number;
  mark_iv: number;
  in_fit: boolean;
}

export interface OptionRow {
  name: string;
  bid: number;
  ask: number;
  mark: number;
  mark_iv: number;
  oi: number;
  delta: number;
  gamma: number;
  vega: number;
  theta: number;
}

export interface ChainRow {
  strike: number;
  call: OptionRow | null;
  put: OptionRow | null;
}

export interface Expiry {
  label: string;
  expiry_ms: number;
  days: number;
  forward: number;
  fit?: SmileFit;
  curve?: [number, number][];
  points?: SmilePoint[];
  chain: ChainRow[];
}

export interface Trade {
  name: string;
  ts_ms: number;
  side: "buy" | "sell";
  price: number;
  amount: number;
  iv: number;
  index: number;
}

export interface WaitingSnapshot {
  type: "snapshot";
  server_ms: number;
  feed: null;
  waiting_for: string;
}

export interface LiveSnapshot {
  type: "snapshot";
  server_ms: number;
  currency: string;
  asof_ms: number;
  index: number;
  fit_ms: number;
  feed: FeedStats;
  expiries: Expiry[];
  trades: Trade[];
}

export type Snapshot = WaitingSnapshot | LiveSnapshot;

export function isLive(s: Snapshot | null): s is LiveSnapshot {
  return s !== null && s.feed !== null;
}
