// Display formatting. Missing values (0 IV, NaN, null) render as a dash, never as 0.

export const DASH = "–";

const has = (x: number | null | undefined): x is number =>
  typeof x === "number" && Number.isFinite(x);

/** Decimal vol (0.4231) as "42.31". */
export function iv(x: number | null | undefined, digits = 2): string {
  return has(x) && x > 0 ? (100 * x).toFixed(digits) : DASH;
}

/** Signed vol points from a decimal (0.0123 -> "+1.23"). */
export function volPts(x: number | null | undefined, digits = 2): string {
  if (!has(x)) return DASH;
  const v = 100 * x;
  return (v >= 0 ? "+" : "") + v.toFixed(digits);
}

/** Option price: inverse (coin) contracts 4 decimals, linear (USDC) 2. 0 = no quote. */
export function price(x: number | null | undefined, linear = false): string {
  return has(x) && x > 0 ? x.toFixed(linear ? 2 : 4) : DASH;
}

export function usd(x: number | null | undefined, digits = 0): string {
  if (!has(x)) return DASH;
  return x.toLocaleString("en-US", { minimumFractionDigits: digits, maximumFractionDigits: digits });
}

export function num(x: number | null | undefined, digits = 2): string {
  if (!has(x)) return DASH;
  const s = x.toFixed(digits);
  return /^-0\.?0*$/.test(s) ? s.slice(1) : s; // no "-0.00"
}

/** Large counts: 1234567 -> "1.23M". */
export function count(x: number | null | undefined): string {
  if (!has(x)) return DASH;
  const a = Math.abs(x);
  if (a >= 1e9) return (x / 1e9).toFixed(2) + "B";
  if (a >= 1e6) return (x / 1e6).toFixed(2) + "M";
  if (a >= 1e4) return (x / 1e3).toFixed(1) + "k";
  return Math.round(x).toString();
}

/** Epoch ms as HH:MM:SS UTC. */
export function utcTime(ms: number | null | undefined): string {
  if (!has(ms) || ms <= 0) return DASH;
  return new Date(ms).toISOString().slice(11, 19);
}

/** Age in ms as "0.4s", "12s", "3m 05s". */
export function age(ms: number): string {
  if (!Number.isFinite(ms) || ms < 0) return DASH;
  if (ms < 10_000) return (ms / 1000).toFixed(1) + "s";
  const s = Math.floor(ms / 1000);
  if (s < 60) return s + "s";
  const m = Math.floor(s / 60);
  if (m < 60) return `${m}m ${String(s % 60).padStart(2, "0")}s`;
  return `${Math.floor(m / 60)}h ${String(m % 60).padStart(2, "0")}m`;
}

/** Days to expiry: "0.42d" under a day as hours, e.g. "10.1h". */
export function tenor(days: number): string {
  if (!has(days)) return DASH;
  return days < 1 ? (24 * days).toFixed(1) + "h" : days.toFixed(days < 10 ? 2 : 1) + "d";
}

/** Strike: integers plain, decimals as needed (USDC-linear strikes like 0.625). */
export function strike(k: number): string {
  return Number.isInteger(k) ? k.toLocaleString("en-US") : String(k);
}
