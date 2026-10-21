import { useMemo } from "react";
import { EChart, theme, type EChartsOption } from "../charts/EChart";
import { iv, strike as fmtStrike } from "../lib/format";
import type { Expiry } from "../types";
import { Empty, Panel } from "./Panel";

export type SmileAxis = "strike" | "k";

/** Bid–ask IV bars, mid (filled if used in the fit), Deribit mark IV, and the SVI curve. */
export function SmileChart({
  expiry,
  axis,
  onAxis,
  dim,
}: {
  expiry: Expiry | undefined;
  axis: SmileAxis;
  onAxis: (a: SmileAxis) => void;
  dim: boolean;
}) {
  const option = useMemo(() => (expiry?.fit ? smileOption(expiry, axis) : null), [expiry, axis]);
  const toggle = (
    <div className="seg">
      <button className={axis === "strike" ? "on" : ""} onClick={() => onAxis("strike")}>
        strike
      </button>
      <button className={axis === "k" ? "on" : ""} onClick={() => onAxis("k")}>
        ln(K/F)
      </button>
    </div>
  );
  const title = expiry ? (
    <>
      Smile <span className="accent">{expiry.label}</span>
      {expiry.fit && <span className="muted"> · ATM {iv(expiry.fit.atm_iv)}%</span>}
    </>
  ) : (
    "Smile"
  );
  return (
    <Panel title={title} right={toggle} dim={dim} className="smile">
      {option ? <EChart option={option} /> : <Empty>{expiry ? "No fit for this expiry (too close or too few quotes)" : "No expiry selected"}</Empty>}
    </Panel>
  );
}

function smileOption(e: Expiry, axis: SmileAxis): EChartsOption {
  const t = theme();
  const F = e.forward;
  const x = (k: number, K: number) => (axis === "k" ? k : K);
  const pts = e.points ?? [];
  const pct = (v: number) => (v > 0 ? 100 * v : null);

  const bars = pts.filter((p) => p.bid_iv > 0 && p.ask_iv > 0).map((p) => [x(p.k, p.strike), 100 * p.bid_iv, 100 * p.ask_iv, p.in_fit ? 1 : 0]);
  const midFit = pts.filter((p) => p.in_fit).map((p) => [x(p.k, p.strike), pct(p.mid_iv), p.type, p.strike]);
  const midOut = pts.filter((p) => !p.in_fit && p.mid_iv > 0).map((p) => [x(p.k, p.strike), pct(p.mid_iv), p.type, p.strike]);
  const mark = pts.filter((p) => p.mark_iv > 0).map((p) => [x(p.k, p.strike), 100 * p.mark_iv, p.type, p.strike]);
  const curve = (e.curve ?? []).map(([k, v]) => [axis === "k" ? k : F * Math.exp(k), v > 0 ? 100 * v : null]);

  // Zoom to the fitted range with some margin, so deep wings don't squash the smile.
  const fit = e.fit!;
  const pad = 0.25 * (fit.k_hi - fit.k_lo) + 0.01;
  const lo = fit.k_lo - pad;
  const hi = fit.k_hi + pad;
  const xmin = axis === "k" ? lo : F * Math.exp(lo);
  const xmax = axis === "k" ? hi : F * Math.exp(hi);
  const visible = [...midFit, ...curve].map((p) => p[1] as number | null).filter((v): v is number => v !== null);
  const ymin = Math.max(0, Math.floor(Math.min(...visible) - 3));
  const ymax = Math.ceil(Math.max(...visible) + 3);

  return {
    animation: false,
    textStyle: { fontFamily: t.font, color: t.text },
    grid: { left: 48, right: 16, top: 28, bottom: 40 },
    legend: { top: 0, right: 0, textStyle: { color: t.muted }, itemWidth: 12, itemHeight: 8 },
    tooltip: {
      trigger: "item",
      backgroundColor: "#11161c",
      borderColor: t.grid,
      textStyle: { color: t.text, fontFamily: t.font, fontSize: 12 },
      formatter: (p: { seriesName: string; value: unknown[] }) => {
        const v = p.value;
        if (p.seriesName === "bid/ask") return `bid ${(v[1] as number).toFixed(2)}%  ask ${(v[2] as number).toFixed(2)}%`;
        if (p.seriesName === "SVI") return `SVI ${(v[1] as number)?.toFixed(2)}%`;
        return `${fmtStrike(v[3] as number)} ${v[2]} · ${p.seriesName} ${(v[1] as number)?.toFixed(2)}%`;
      },
    },
    xAxis: {
      type: "value",
      min: xmin,
      max: xmax,
      name: axis === "k" ? "ln(K/F)" : "strike",
      nameLocation: "middle",
      nameGap: 26,
      nameTextStyle: { color: t.muted },
      axisLine: { lineStyle: { color: t.grid } },
      axisLabel: {
        color: t.muted,
        showMinLabel: false,
        showMaxLabel: false,
        formatter: (v: number) => (axis === "k" ? v.toFixed(2) : fmtStrike(Math.round(v))),
      },
      splitLine: { lineStyle: { color: t.grid, opacity: 0.4 } },
    },
    yAxis: {
      type: "value",
      min: ymin,
      max: ymax,
      name: "IV %",
      nameTextStyle: { color: t.muted },
      axisLine: { lineStyle: { color: t.grid } },
      axisLabel: { color: t.muted },
      splitLine: { lineStyle: { color: t.grid, opacity: 0.4 } },
    },
    series: [
      {
        name: "bid/ask",
        type: "custom",
        data: bars,
        encode: { x: 0, y: [1, 2] },
        renderItem: (_params: unknown, api: CustomApi) => {
          const lo = api.coord([api.value(0), api.value(1)]);
          const hi = api.coord([api.value(0), api.value(2)]);
          const inFit = api.value(3) === 1;
          const style = { stroke: inFit ? t.text : t.muted, lineWidth: 1, opacity: inFit ? 0.8 : 0.35 };
          return {
            type: "group",
            children: [
              { type: "line", shape: { x1: lo[0], y1: lo[1], x2: hi[0], y2: hi[1] }, style },
              { type: "line", shape: { x1: lo[0] - 3, y1: lo[1], x2: lo[0] + 3, y2: lo[1] }, style },
              { type: "line", shape: { x1: hi[0] - 3, y1: hi[1], x2: hi[0] + 3, y2: hi[1] }, style },
            ],
          };
        },
        z: 1,
      },
      { name: "mid (fit)", type: "scatter", data: midFit, symbolSize: 6, itemStyle: { color: t.accent }, z: 3 },
      { name: "mid (excluded)", type: "scatter", data: midOut, symbolSize: 5, itemStyle: { color: "transparent", borderColor: t.muted, borderWidth: 1 }, z: 2 },
      { name: "mark", type: "scatter", data: mark, symbol: "diamond", symbolSize: 5, itemStyle: { color: t.warn, opacity: 0.7 }, z: 2 },
      { name: "SVI", type: "line", data: curve, showSymbol: false, lineStyle: { color: t.accent, width: 2 }, z: 4 },
    ],
  };
}

interface CustomApi {
  value: (i: number) => number;
  coord: (v: [number, number]) => [number, number];
}
