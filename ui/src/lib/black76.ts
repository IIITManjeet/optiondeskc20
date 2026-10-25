// Black-76 on the forward and its implied-vol solver: a port of include/od/black76.hpp
// and src/implied_vol.cpp. Same conventions: price in USD per unit of underlying,
// vega per vol point, theta per calendar day. Checked against the C++ results in
// demo.test.ts; change both together.

export type OptionKind = "C" | "P";

const SQRT_2PI = Math.sqrt(2 * Math.PI);

export function normPdf(x: number): number {
  return Math.exp(-0.5 * x * x) / SQRT_2PI;
}

/**
 * Standard normal CDF to double precision (Hart 1968, as given by West 2005).
 * JavaScript has no erfc, and the usual 1e-7 approximations aren't good enough
 * to invert deep out-of-the-money prices.
 */
export function normCdf(x: number): number {
  const z = Math.abs(x);
  let c: number;
  if (z > 37) {
    c = 0;
  } else {
    const e = Math.exp(-0.5 * z * z);
    if (z < 7.07106781186547) {
      let n = 3.52624965998911e-2 * z + 0.700383064443688;
      n = n * z + 6.37396220353165;
      n = n * z + 33.912866078383;
      n = n * z + 112.079291497871;
      n = n * z + 221.213596169931;
      n = n * z + 220.206867912376;
      let d = 8.83883476483184e-2 * z + 1.75566716318264;
      d = d * z + 16.064177579207;
      d = d * z + 86.7807322029461;
      d = d * z + 296.564248779674;
      d = d * z + 637.333633378831;
      d = d * z + 793.826512519948;
      d = d * z + 440.413735824752;
      c = (e * n) / d;
    } else {
      let b = z + 0.65;
      b = z + 4 / b;
      b = z + 3 / b;
      b = z + 2 / b;
      b = z + 1 / b;
      c = e / b / SQRT_2PI;
    }
  }
  return x > 0 ? 1 - c : c;
}

export function black76Price(type: OptionKind, F: number, K: number, T: number, sigma: number): number {
  if (T <= 0 || sigma <= 0) return Math.max(type === "C" ? F - K : K - F, 0);
  const sd = sigma * Math.sqrt(T);
  const d1 = (Math.log(F / K) + 0.5 * sd * sd) / sd;
  const d2 = d1 - sd;
  return type === "C" ? F * normCdf(d1) - K * normCdf(d2) : K * normCdf(-d2) - F * normCdf(-d1);
}

export interface Greeks {
  price: number;
  delta: number;
  gamma: number;
  vega: number; // per vol point
  theta: number; // per calendar day
}

export function black76Greeks(type: OptionKind, F: number, K: number, T: number, sigma: number): Greeks {
  if (T <= 0 || sigma <= 0) {
    const itm = type === "C" ? F > K : K > F;
    return { price: black76Price(type, F, K, T, sigma), delta: itm ? (type === "C" ? 1 : -1) : 0, gamma: 0, vega: 0, theta: 0 };
  }
  const sqrtT = Math.sqrt(T);
  const sd = sigma * sqrtT;
  const d1 = (Math.log(F / K) + 0.5 * sd * sd) / sd;
  const d2 = d1 - sd;
  const pdf = normPdf(d1);
  const call = type === "C";
  return {
    price: call ? F * normCdf(d1) - K * normCdf(d2) : K * normCdf(-d2) - F * normCdf(-d1),
    delta: call ? normCdf(d1) : -normCdf(-d1),
    gamma: pdf / (F * sigma * sqrtT),
    vega: (F * pdf * sqrtT) / 100,
    theta: -(F * pdf * sigma) / (2 * sqrtT) / 365,
  };
}

/**
 * Implied vol from a USD price, or 0 if there is none (below intrinsic, above the
 * upper bound, no convergence). Safeguarded Newton inside a bisection bracket,
 * step for step the same as od::implied_vol.
 */
export function impliedVol(type: OptionKind, priceUsd: number, F: number, K: number, T: number): number {
  if (!(priceUsd >= 0) || !(F > 0) || !(K > 0) || !(T > 0)) return 0;
  const call = type === "C";
  const intrinsic = Math.max(call ? F - K : K - F, 0);
  const upper = call ? F : K;
  const tol = 1e-12 * F;
  if (priceUsd < intrinsic - tol || priceUsd >= upper || priceUsd - intrinsic <= tol) return 0;

  let lo = 1e-6;
  let hi = 5;
  let pHi = black76Price(type, F, K, T, hi);
  while (pHi < priceUsd && hi < 100) pHi = black76Price(type, F, K, T, (hi *= 2));
  if (pHi < priceUsd) return 0;

  const logFK = Math.log(F / K);
  const sqrtT = Math.sqrt(T);
  let s = Math.min(Math.max((Math.sqrt((2 * Math.PI) / T) * (priceUsd - intrinsic)) / F, 0.05), 3);
  for (let i = 1; i <= 100; ++i) {
    const sd = s * sqrtT;
    const d1 = logFK / sd + 0.5 * sd;
    const d2 = d1 - sd;
    const vega = F * normPdf(d1) * sqrtT;
    const f = (call ? F * normCdf(d1) - K * normCdf(d2) : K * normCdf(-d2) - F * normCdf(-d1)) - priceUsd;
    if (Math.abs(f) <= tol) return s;
    if (f > 0) hi = s;
    else lo = s;
    let next = vega > 0 ? s - f / vega : -1;
    if (!(next > lo && next < hi)) next = 0.5 * (lo + hi);
    if (Math.abs(next - s) < 1e-14) return next;
    s = next;
  }
  return 0;
}

export interface Svi {
  a: number;
  b: number;
  rho: number;
  m: number;
  s: number;
}

/** SVI total variance w(k). */
export function sviW(p: Svi, k: number): number {
  const x = k - p.m;
  return p.a + p.b * (p.rho * x + Math.sqrt(x * x + p.s * p.s));
}

/** SVI implied vol at log-moneyness k, or 0 where the total variance isn't positive. */
export function sviIv(p: Svi, k: number, T: number): number {
  const w = sviW(p, k);
  return w > 0 ? Math.sqrt(w / T) : 0;
}
