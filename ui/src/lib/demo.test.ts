import { describe, expect, it } from "vitest";
import pair from "../fixtures/demo_pair.json";
import type { LiveSnapshot } from "../types";
import { black76Greeks, black76Price, impliedVol, normCdf } from "./black76";
import { applyPatch, decodeChunk, expand, type CompactSnapshot } from "./demo";

// demo_pair.json: one snapshot exactly as od_web would send it ("full") and as
// od_snapshots stores it ("compact", rounded to 6 significant digits), trimmed to
// four expiries. Regenerate with od_snapshots --fixture.
const full = pair.full as unknown as LiveSnapshot;
const compact = pair.compact as unknown as CompactSnapshot;

describe("black76", () => {
  it("normCdf matches known values, tails included", () => {
    expect(normCdf(0)).toBe(0.5);
    expect(normCdf(1.96)).toBeCloseTo(0.9750021048517795, 14);
    // Small relative error in the tails too, where 1 - N(x) approximations fail.
    expect(normCdf(-5) / 2.866515718791939e-7).toBeCloseTo(1, 9);
    expect(normCdf(-10) / 7.619853024160527e-24).toBeCloseTo(1, 8);
  });
  it("inverts its own prices, deep OTM included", () => {
    for (const [K, T, v] of [
      [80000, 0.1, 0.5],
      [120000, 0.02, 0.8],
      [50000, 0.5, 0.35],
    ] as const) {
      for (const kind of ["C", "P"] as const) {
        const px = black76Price(kind, 80000, K, T, v);
        expect(impliedVol(kind, px, 80000, K, T)).toBeCloseTo(v, 8);
      }
    }
    expect(impliedVol("C", 0, 80000, 90000, 0.1)).toBe(0);
    expect(impliedVol("C", 80001, 80000, 90000, 0.1)).toBe(0); // above the upper bound
  });
  it("put-call parity on delta and equal vega", () => {
    const c = black76Greeks("C", 80000, 85000, 0.1, 0.5);
    const p = black76Greeks("P", 80000, 85000, 0.1, 0.5);
    expect(c.delta - p.delta).toBeCloseTo(1, 12);
    expect(c.vega).toBeCloseTo(p.vega, 9);
  });
});

describe("applyPatch", () => {
  it("patches objects and same-length arrays, replaces the rest", () => {
    const a = { x: 1, y: { z: [1, 2, 3], w: "k" }, gone: true, row: { call: { bid: 1 } } };
    const p = {
      o: {
        x: 2,
        y: { o: { z: { a: { "1": 5 } } } },
        add: { "=": [9] },
        row: { o: { call: null } },
      },
      d: ["gone"],
    };
    expect(applyPatch(a, p)).toEqual({ x: 2, y: { z: [1, 5, 3], w: "k" }, add: [9], row: { call: null } });
    expect(a.y.z).toEqual([1, 2, 3]); // not mutated
    expect(applyPatch([1, 2], { "=": [1, 2, 3] })).toEqual([1, 2, 3]);
  });
  it("decodes a chunk: keyframe then patches", () => {
    const text = [
      JSON.stringify({ type: "snapshot", server_ms: 1, index: 100 }),
      JSON.stringify({ type: "patch", patch: { o: { server_ms: 2 } } }),
      JSON.stringify({ type: "patch", patch: { o: { server_ms: 3, index: 101 } } }),
      "",
    ].join("\n");
    const s = decodeChunk(text);
    expect(s.map((x) => [x.server_ms, x.index])).toEqual([
      [1, 100],
      [2, 100],
      [3, 101],
    ]);
  });
});

describe("expand matches the C++ snapshot", () => {
  const out = expand(compact);

  it("keeps the shape", () => {
    expect(out.expiries.map((e) => e.label)).toEqual(full.expiries.map((e) => e.label));
    out.expiries.forEach((e, i) => {
      const f = full.expiries[i]!;
      expect(e.chain.length).toBe(f.chain.length);
      expect(e.fit === undefined).toBe(f.fit === undefined);
      expect(e.points?.length).toBe(f.points?.length);
      expect(e.curve?.length).toBe(f.curve?.length);
    });
  });

  it("recomputes the Greeks", () => {
    let n = 0;
    out.expiries.forEach((e, i) =>
      e.chain.forEach((r, j) => {
        for (const side of ["call", "put"] as const) {
          const a = r[side];
          const b = full.expiries[i]!.chain[j]![side];
          if (!a || !b) continue;
          const tol = (x: number) => 1e-4 * Math.max(1, Math.abs(x));
          expect(Math.abs(a.delta - b.delta)).toBeLessThan(1e-4);
          expect(Math.abs(a.gamma - b.gamma)).toBeLessThan(tol(b.gamma));
          expect(Math.abs(a.vega - b.vega)).toBeLessThan(tol(b.vega));
          expect(Math.abs(a.theta - b.theta)).toBeLessThan(tol(b.theta));
          ++n;
        }
      }),
    );
    expect(n).toBeGreaterThan(100);
  });

  it("recomputes the smile points and the fitted curve", () => {
    let n = 0;
    let flips = 0;
    out.expiries.forEach((e, i) => {
      const f = full.expiries[i]!;
      e.points?.forEach((p, j) => {
        const q = f.points![j]!;
        expect(p.strike).toBe(q.strike);
        expect(p.type).toBe(q.type);
        expect(Math.abs(p.k - q.k)).toBeLessThan(1e-5);
        expect(Math.abs(p.bid_iv - q.bid_iv)).toBeLessThan(5e-4);
        expect(Math.abs(p.ask_iv - q.ask_iv)).toBeLessThan(5e-4);
        if (p.in_fit !== q.in_fit) ++flips;
        ++n;
      });
      e.curve?.forEach(([k, v], j) => {
        const [k2, v2] = f.curve![j]!;
        expect(Math.abs(k - k2)).toBeLessThan(1e-5);
        expect(Math.abs(v - v2)).toBeLessThan(1e-4);
      });
    });
    expect(n).toBeGreaterThan(100);
    expect(flips).toBeLessThanOrEqual(1); // a quote on the 15-vol spread cutoff may round across it
  });
});
