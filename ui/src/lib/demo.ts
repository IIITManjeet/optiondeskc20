// Demo replay files (written by apps/od_snapshots.cpp) back into live snapshots.
//
// A session is <id>/index.json plus gzipped NDJSON chunks. Each chunk starts with a
// compact keyframe, then holds patches against the previous snapshot. "Compact"
// means the fields below were dropped to fit GitHub Pages; expand() rebuilds them
// exactly as od::web::snapshot_json() and od::build_surface() compute them:
//   - every quote's delta, gamma, vega, theta (Black-76 on the mark IV);
//   - each fitted expiry's `curve` (the SVI smile) and `points` (bid/ask IVs per quote).

import type { ChainRow, Expiry, LiveSnapshot, OptionRow, SmilePoint } from "../types";
import { black76Greeks, impliedVol, sviIv, type OptionKind } from "./black76";

export const DEMO_FORMAT = 1;

export interface ChunkInfo {
  file: string;
  from_ms: number;
  to_ms: number;
  snapshots: number;
  bytes: number;
}

export interface SessionInfo {
  format: number;
  id: string;
  currency: string;
  journal: string;
  start_ms: number;
  end_ms: number;
  every_s: number;
  snapshots: number;
  bytes: number;
}

export interface Session extends SessionInfo {
  chunks: ChunkInfo[];
}

export type CompactQuote = Omit<OptionRow, "delta" | "gamma" | "vega" | "theta">;
export interface CompactRow {
  strike: number;
  call: CompactQuote | null;
  put: CompactQuote | null;
}
export type CompactExpiry = Omit<Expiry, "curve" | "points" | "chain"> & { chain: CompactRow[] };
export type CompactSnapshot = Omit<LiveSnapshot, "expiries"> & { expiries: CompactExpiry[] };

type Json = null | boolean | number | string | Json[] | { [k: string]: Json };

/** Applies one patch node (see `diff` in apps/od_snapshots.cpp) to `a`, without mutating it. */
export function applyPatch(a: Json | undefined, p: Json): Json {
  if (p === null || typeof p !== "object" || Array.isArray(p)) return p;
  if ("=" in p) return p["="] as Json;
  if ("o" in p) {
    const out: { [k: string]: Json } = { ...(a as { [k: string]: Json }) };
    for (const [k, v] of Object.entries(p.o as { [k: string]: Json })) out[k] = applyPatch(out[k], v);
    for (const k of (p.d as string[] | undefined) ?? []) delete out[k];
    return out;
  }
  const out = (a as Json[]).slice();
  for (const [i, v] of Object.entries(p.a as { [k: string]: Json })) out[+i] = applyPatch(out[+i], v);
  return out;
}

/** Every snapshot of one chunk's NDJSON text, still compact. */
export function decodeChunk(text: string): CompactSnapshot[] {
  const out: CompactSnapshot[] = [];
  let prev: Json | undefined;
  for (const line of text.split("\n")) {
    if (!line) continue;
    const msg = JSON.parse(line) as { [k: string]: Json };
    const cur = msg.type === "patch" ? applyPatch(prev, msg.patch as Json) : msg;
    out.push(cur as unknown as CompactSnapshot);
    prev = cur;
  }
  return out;
}

const MS_PER_YEAR = 365 * 24 * 3600 * 1000;
const MAX_SPREAD_VOL = 0.15; // od::SurfaceOptions::max_spread_vol

function quoteRow(q: CompactQuote, kind: OptionKind, K: number, F: number, T: number): OptionRow {
  const g = q.mark_iv > 0 ? black76Greeks(kind, F, K, T, q.mark_iv) : { delta: 0, gamma: 0, vega: 0, theta: 0 };
  return { ...q, delta: g.delta, gamma: (g.gamma * F) / 100, vega: g.vega, theta: g.theta };
}

function point(q: CompactQuote, kind: OptionKind, K: number, F: number, T: number): SmilePoint {
  const solve = (px: number) => (px > 0 ? impliedVol(kind, px * F, F, K, T) : 0);
  const bid_iv = solve(q.bid);
  const ask_iv = solve(q.ask);
  const mid_iv = bid_iv > 0 && ask_iv > 0 ? 0.5 * (bid_iv + ask_iv) : 0;
  const otm = kind === "P" ? K < F : K >= F;
  const spread = ask_iv - bid_iv;
  return {
    strike: K,
    k: Math.log(K / F),
    type: kind,
    bid_iv,
    ask_iv,
    mid_iv,
    mark_iv: q.mark_iv,
    in_fit: otm && mid_iv > 0 && spread >= 0 && spread <= MAX_SPREAD_VOL,
  };
}

function expandExpiry(e: CompactExpiry, asofMs: number): Expiry {
  const F = e.forward;
  const T = (e.expiry_ms - asofMs) / MS_PER_YEAR;
  const chain: ChainRow[] = e.chain.map((r) => ({
    strike: r.strike,
    call: r.call && quoteRow(r.call, "C", r.strike, F, T),
    put: r.put && quoteRow(r.put, "P", r.strike, F, T),
  }));
  const out: Expiry = { ...e, chain };
  if (!e.fit) return out;

  const { k_lo, k_hi, svi } = e.fit;
  const pad = 0.15 * (k_hi - k_lo) + 0.01;
  const curve: [number, number][] = [];
  for (let i = 0; i <= 80; ++i) {
    const k = k_lo - pad + ((k_hi - k_lo + 2 * pad) * i) / 80;
    curve.push([k, sviIv(svi, k, T)]);
  }
  const points: SmilePoint[] = [];
  for (const r of e.chain) {
    if (r.call) points.push(point(r.call, "C", r.strike, F, T));
    if (r.put) points.push(point(r.put, "P", r.strike, F, T));
  }
  return { ...out, curve, points };
}

/** A compact snapshot back to the full live shape. */
export function expand(s: CompactSnapshot): LiveSnapshot {
  return { ...s, expiries: s.expiries.map((e) => expandExpiry(e, s.asof_ms)) };
}

/** Fetches a URL that holds gzip bytes (GitHub Pages serves .gz without Content-Encoding). */
export async function fetchGzipText(url: string): Promise<string> {
  const res = await fetch(url);
  if (!res.ok || !res.body) throw new Error(`${url}: HTTP ${res.status}`);
  const bytes = new Uint8Array(await res.arrayBuffer());
  // A dev server may have already decoded it; gzip starts with 1f 8b.
  if (bytes[0] !== 0x1f || bytes[1] !== 0x8b) return new TextDecoder().decode(bytes);
  const stream = new Blob([bytes]).stream().pipeThrough(new DecompressionStream("gzip"));
  return await new Response(stream).text();
}
