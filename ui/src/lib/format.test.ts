import { describe, expect, it } from "vitest";
import { atmRow, feedState, isLinear, pickExpiry, pushHistory } from "./desk";
import { age, count, iv, num, price, strike, tenor, utcTime, volPts } from "./format";
import type { LiveSnapshot } from "../types";

describe("format", () => {
  it("shows missing IVs and prices as a dash, not zero", () => {
    expect(iv(0)).toBe("–");
    expect(iv(NaN)).toBe("–");
    expect(iv(0.4231)).toBe("42.31");
    expect(price(0)).toBe("–");
    expect(price(0.01234)).toBe("0.0123");
    expect(price(12.345, true)).toBe("12.35");
  });
  it("never prints negative zero", () => {
    expect(num(-0.001, 2)).toBe("0.00");
    expect(num(-0.4, 0)).toBe("0");
    expect(num(-0.5, 2)).toBe("-0.50");
  });
  it("signs vol points", () => {
    expect(volPts(0.0123)).toBe("+1.23");
    expect(volPts(-0.005)).toBe("-0.50");
  });
  it("formats counts, times, ages, tenors and strikes", () => {
    expect(count(1_234_567)).toBe("1.23M");
    expect(count(950)).toBe("950");
    expect(utcTime(Date.UTC(2026, 9, 21, 14, 3, 9))).toBe("14:03:09");
    expect(age(400)).toBe("0.4s");
    expect(age(185_000)).toBe("3m 05s");
    expect(tenor(0.5)).toBe("12.0h");
    expect(tenor(3.256)).toBe("3.26d");
    expect(strike(85000)).toBe("85,000");
    expect(strike(0.625)).toBe("0.625");
  });
});

describe("desk", () => {
  const snap = {
    type: "snapshot",
    server_ms: 0,
    currency: "BTC",
    asof_ms: 0,
    index: 85000,
    fit_ms: 1,
    feed: { connected: true, stale: false } as LiveSnapshot["feed"],
    expiries: [
      { label: "22OCT26", expiry_ms: 0, days: 0.05, forward: 85000, chain: [] },
      {
        label: "23OCT26",
        expiry_ms: 0,
        days: 1,
        forward: 85010,
        chain: [
          { strike: 84000, call: null, put: null },
          { strike: 85000, call: null, put: null },
          { strike: 86000, call: null, put: null },
        ],
        fit: {} as never,
      },
    ],
    trades: [],
  } satisfies LiveSnapshot;

  it("classifies feed state", () => {
    expect(feedState(null, false, 0)).toBe("connecting");
    expect(feedState(snap, false, 0)).toBe("disconnected");
    expect(feedState(snap, true, 500)).toBe("live");
    expect(feedState(snap, true, 5000)).toBe("stale");
    expect(feedState({ type: "snapshot", server_ms: 0, feed: null, waiting_for: "x" }, true, 0)).toBe("waiting");
  });
  it("picks the stored expiry, else the first fitted one", () => {
    expect(pickExpiry(snap, "22OCT26")?.label).toBe("22OCT26");
    expect(pickExpiry(snap, "gone")?.label).toBe("23OCT26");
  });
  it("finds the ATM row", () => {
    expect(atmRow(snap.expiries[1]!.chain, 85010)).toBe(1);
  });
  it("detects linear currencies", () => {
    expect(isLinear("BTC")).toBe(false);
    expect(isLinear("SOL_USDC")).toBe(true);
  });
  it("keeps bounded history", () => {
    expect(pushHistory([1, 2, 3], 4, 3)).toEqual([2, 3, 4]);
  });
});
