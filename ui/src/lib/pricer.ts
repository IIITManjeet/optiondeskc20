// The Pricer page's maths: Black-76 and SVI from black76.ts, in the desk's display
// units. This is a playground over user inputs, not a second source for anything
// od_web computes, so it may compute prices and Greeks itself.

import type { Expiry } from "../types";
import {
  black76Greeks,
  black76Price,
  inverseDelta,
  minDurrlemanG,
  sviG,
  sviIv,
  sviValid,
  type OptionKind,
  type Svi,
} from "./black76";

export const DAYS_PER_YEAR = 365; // T in years = days / 365, as od::year_frac

export type VolSource = "flat" | "svi";

export interface PricerInput {
  kind: OptionKind;
  F: number; // forward, USD
  K: number;
  days: number;
  source: VolSource;
  vol: number; // decimal, used when source = "flat"
  svi: Svi; // total variance at this expiry, used when source = "svi"
}

export interface PricerResult {
  T: number;
  k: number; // ln(K/F)
  sigma: number; // the vol actually used
  priceUsd: number;
  priceCoin: number;
  intrinsicUsd: number;
  delta: number; // USD delta
  deltaCoin: number; // premium-adjusted
  gamma: number; // change in delta per 1% move, as on the wire
  vega: number; // USD per vol point
  theta: number; // USD per calendar day
}

/** Why the inputs can't be priced, or null if they can. */
export function inputError(p: PricerInput): string | null {
  if (!(p.F > 0)) return "forward must be positive";
  if (!(p.K > 0)) return "strike must be positive";
  if (!(p.days > 0)) return "days to expiry must be positive";
  const s = volFor(p);
  if (!(s > 0)) return p.source === "svi" ? "SVI total variance is not positive at this strike" : "vol must be positive";
  return null;
}

export function volFor(p: PricerInput): number {
  const T = p.days / DAYS_PER_YEAR;
  return p.source === "svi" ? sviIv(p.svi, Math.log(p.K / p.F), T) : p.vol;
}

export function price(p: PricerInput): PricerResult | null {
  if (inputError(p)) return null;
  const T = p.days / DAYS_PER_YEAR;
  const sigma = volFor(p);
  const g = black76Greeks(p.kind, p.F, p.K, T, sigma);
  return {
    T,
    k: Math.log(p.K / p.F),
    sigma,
    priceUsd: g.price,
    priceCoin: g.price / p.F,
    intrinsicUsd: Math.max(p.kind === "C" ? p.F - p.K : p.K - p.F, 0),
    delta: g.delta,
    deltaCoin: inverseDelta(g.delta, g.price, p.F),
    gamma: (g.gamma * p.F) / 100,
    vega: g.vega,
    theta: g.theta,
  };
}

export interface LadderPoint {
  F: number;
  value: number; // USD, today
  payoff: number; // USD, at expiry
  delta: number;
}

/**
 * Value, expiry payoff and delta across forwards from F·(1 − span) to F·(1 + span).
 * Sticky strike: the option keeps the vol it has now.
 */
export function ladder(p: PricerInput, span = 0.3, n = 121): LadderPoint[] {
  const r = price(p);
  if (!r) return [];
  const out: LadderPoint[] = [];
  for (let i = 0; i < n; ++i) {
    const F = p.F * (1 - span + (2 * span * i) / (n - 1));
    const g = black76Greeks(p.kind, F, p.K, r.T, r.sigma);
    out.push({ F, value: g.price, payoff: black76Price(p.kind, F, p.K, 0, 0), delta: g.delta });
  }
  return out;
}

export interface SmilePoint {
  k: number;
  iv: number; // decimal; 0 where total variance isn't positive
  g: number; // Durrleman
}

export function smile(svi: Svi, T: number, kLo: number, kHi: number, n = 161): SmilePoint[] {
  const out: SmilePoint[] = [];
  for (let i = 0; i < n; ++i) {
    const k = kLo + ((kHi - kLo) * i) / (n - 1);
    out.push({ k, iv: sviIv(svi, k, T), g: sviG(svi, k) });
  }
  return out;
}

export interface SviCheck {
  valid: boolean; // the fitter's parameter constraints
  minG: number; // over the k range shown
  atmIv: number; // at k = 0
}

export function checkSvi(svi: Svi, T: number, kLo: number, kHi: number): SviCheck {
  return { valid: sviValid(svi), minG: minDurrlemanG(svi, kLo, kHi), atmIv: sviIv(svi, 0, T) };
}

/**
 * Inputs from a Desk expiry: its forward, tenor and fitted SVI, at the listed
 * strike nearest the forward.
 */
export function fromExpiry(e: Expiry, kind: OptionKind): Partial<PricerInput> {
  const strikes = e.chain.map((r) => r.strike);
  const K = strikes.length ? strikes.reduce((a, b) => (Math.abs(b - e.forward) < Math.abs(a - e.forward) ? b : a)) : e.forward;
  return {
    kind,
    F: e.forward,
    K,
    days: e.days,
    ...(e.fit ? { source: "svi" as const, svi: { ...e.fit.svi } } : {}),
  };
}

/** The exchange's mark IV for this strike and type in a Desk expiry, or 0. */
export function markIv(e: Expiry, K: number, kind: OptionKind): number {
  const row = e.chain.find((r) => r.strike === K);
  const q = row && (kind === "C" ? row.call : row.put);
  return q?.mark_iv ?? 0;
}
