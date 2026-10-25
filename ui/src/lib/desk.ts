// Pure helpers the Desk derives from a snapshot.

import type { ChainRow, Expiry, LiveSnapshot, Snapshot } from "../types";

/** How long without a snapshot before the data counts as stale (live mode). */
export const STALE_AFTER_MS = 3000;

export type FeedState = "connecting" | "waiting" | "live" | "stale" | "disconnected" | "replay";

/**
 * One word for the header: is what's on screen live?
 *  connecting   no snapshot yet
 *  waiting      od_web is up but no feed bus attached
 *  disconnected socket down, or the exchange connection is down
 *  stale        snapshots stopped, or od_feedd's heartbeat stopped
 * (The demo shows "replay" instead: its data is recorded, never live.)
 */
export function feedState(snap: Snapshot | null, socketOpen: boolean, ageMs: number): FeedState {
  if (!snap) return "connecting";
  if (!socketOpen) return "disconnected";
  if (snap.feed === null) return "waiting";
  if (!snap.feed.connected) return "disconnected";
  if (snap.feed.stale || ageMs > STALE_AFTER_MS) return "stale";
  return "live";
}

/** Linear (USDC-settled) options are priced in USDC, not the coin. */
export function isLinear(currency: string): boolean {
  return currency.includes("_");
}

/** The expiry to show: the stored choice if it still exists, else the first fitted one. */
export function pickExpiry(snap: LiveSnapshot, wanted: string | null): Expiry | undefined {
  return (
    snap.expiries.find((e) => e.label === wanted) ??
    snap.expiries.find((e) => e.fit) ??
    snap.expiries[0]
  );
}

/** Index of the chain row whose strike is closest to the forward. */
export function atmRow(rows: ChainRow[], forward: number): number {
  let best = -1;
  let dist = Infinity;
  rows.forEach((r, i) => {
    const d = Math.abs(r.strike - forward);
    if (d < dist) {
      dist = d;
      best = i;
    }
  });
  return best;
}

/** Arbitrage flags for the term table. */
export function arbFlags(e: Expiry): string[] {
  const f: string[] = [];
  if (e.fit && e.fit.min_g < 0) f.push("BFLY");
  if (e.fit && e.fit.calendar > 1e-6) f.push("CAL");
  return f;
}

/** Rolling history for sparklines: append, keep the newest `max`. */
export function pushHistory<T>(h: readonly T[], x: T, max: number): T[] {
  const out = h.length >= max ? h.slice(h.length - max + 1) : h.slice();
  out.push(x);
  return out;
}
