import { describe, expect, it } from "vitest";
import { black76Price, minDurrlemanG, sviValid, sviW, sviW1, sviW2, type Svi } from "./black76";
import { DAYS_PER_YEAR, checkSvi, fromExpiry, inputError, ladder, price, type PricerInput } from "./pricer";
import type { Expiry } from "../types";

// The same cases as tests/test_svi.cpp and tests/test_black76.cpp.
const vogt: Svi = { a: -0.041, b: 0.1331, rho: 0.306, m: 0.3586, s: 0.4153 };
const sane: Svi = { a: 0.02, b: 0.1, rho: -0.4, m: 0.05, s: 0.2 };

describe("svi port", () => {
  it("derivatives match finite differences", () => {
    const h = 1e-4;
    for (const k of [-1, -0.3, 0, 0.2, 0.9]) {
      expect(sviW1(sane, k)).toBeCloseTo((sviW(sane, k + h) - sviW(sane, k - h)) / (2 * h), 8);
      expect(sviW2(sane, k)).toBeCloseTo((sviW(sane, k + h) - 2 * sviW(sane, k) + sviW(sane, k - h)) / (h * h), 4);
    }
  });
  it("Durrleman's condition flags Vogt's arbitrage and passes a sane smile", () => {
    expect(minDurrlemanG(vogt)).toBeLessThan(0);
    expect(minDurrlemanG(sane)).toBeGreaterThan(0);
    // Vogt's smile passes the simple constraints; only g catches it.
    expect(sviValid(vogt)).toBe(true);
    expect(sviValid({ ...sane, rho: 1 })).toBe(false);
  });
});

const base: PricerInput = {
  kind: "C",
  F: 82022.7,
  K: 88000,
  days: 0.13580176201801114 * DAYS_PER_YEAR,
  source: "flat",
  vol: 0.3722,
  svi: sane,
};

describe("pricer", () => {
  it("reproduces Deribit's published ticker (BTC-27NOV26-88000-C)", () => {
    const r = price(base)!;
    expect(Math.abs(r.priceCoin - 0.0275)).toBeLessThan(2.5e-4);
    expect(r.delta).toBeCloseTo(0.32843, 4);
    expect(r.vega).toBeCloseTo(109.25415, 1);
    expect(r.theta).toBeCloseTo(-41.01911, 1);
    expect(r.deltaCoin).toBeCloseTo(r.delta - r.priceCoin, 12);
  });
  it("uses the SVI vol at ln(K/F) when asked", () => {
    const r = price({ ...base, source: "svi" })!;
    expect(r.sigma).toBeCloseTo(Math.sqrt(sviW(sane, Math.log(88000 / 82022.7)) / r.T), 12);
  });
  it("rejects inputs it can't price", () => {
    expect(inputError(base)).toBeNull();
    expect(inputError({ ...base, days: 0 })).toMatch(/days/);
    expect(inputError({ ...base, vol: 0 })).toMatch(/vol/);
    expect(inputError({ ...base, source: "svi", svi: { ...sane, a: -1 } })).toMatch(/SVI/);
    expect(price({ ...base, F: -1 })).toBeNull();
  });
  it("ladders value above payoff, centred on the forward", () => {
    const l = ladder(base, 0.3, 61);
    expect(l).toHaveLength(61);
    expect(l[30]!.F).toBeCloseTo(base.F, 6);
    expect(l[30]!.value).toBeCloseTo(price(base)!.priceUsd, 9);
    for (const p of l) {
      expect(p.value).toBeGreaterThanOrEqual(p.payoff - 1e-9);
      expect(p.payoff).toBe(Math.max(p.F - base.K, 0));
    }
    expect(black76Price("C", l[60]!.F, base.K, 0, 0)).toBe(l[60]!.payoff);
  });
  it("loads a Desk expiry at the strike nearest the forward", () => {
    const e = {
      label: "27NOV26",
      forward: 82022.7,
      days: 49.6,
      chain: [{ strike: 80000 }, { strike: 82000 }, { strike: 84000 }],
      fit: { svi: sane },
    } as unknown as Expiry;
    expect(fromExpiry(e, "P")).toEqual({ kind: "P", F: 82022.7, K: 82000, days: 49.6, source: "svi", svi: sane });
    expect(checkSvi(sane, 0.1, -1, 1).valid).toBe(true);
  });
});
