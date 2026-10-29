import { useMemo } from "react";
import { EChart, theme, type EChartsOption } from "../charts/EChart";
import { strike as fmtStrike } from "../lib/format";
import type { LadderPoint, SmilePoint } from "../lib/pricer";
import { Empty, Panel } from "./Panel";

function axes(t: ReturnType<typeof theme>) {
  const axis = {
    nameTextStyle: { color: t.muted },
    axisLine: { lineStyle: { color: t.grid } },
    axisLabel: { color: t.muted },
    splitLine: { lineStyle: { color: t.grid, opacity: 0.4 } },
  };
  return {
    axis,
    base: {
      animation: false,
      textStyle: { fontFamily: t.font, color: t.text },
      legend: { top: 0, right: 0, textStyle: { color: t.muted }, itemWidth: 12, itemHeight: 8 },
      tooltip: {
        trigger: "axis",
        backgroundColor: "#11161c",
        borderColor: t.grid,
        textStyle: { color: t.text, fontFamily: t.font, fontSize: 12 },
      },
    },
  };
}

/** Value today and at expiry, and delta, across forwards (sticky strike). */
export function ValueChart({ points, F, K }: { points: LadderPoint[]; F: number; K: number }) {
  const option = useMemo((): EChartsOption | null => {
    if (!points.length) return null;
    const t = theme();
    const { axis, base } = axes(t);
    return {
      ...base,
      grid: { left: 64, right: 48, top: 28, bottom: 40 },
      tooltip: {
        ...base.tooltip,
        formatter: (ps: { seriesName: string; value: [number, number] }[]) =>
          `F ${fmtStrike(Math.round(ps[0]!.value[0]))}<br>` +
          ps.map((p) => `${p.seriesName} ${p.value[1].toFixed(p.seriesName === "delta" ? 4 : 2)}`).join("<br>"),
      },
      xAxis: {
        ...axis,
        type: "value",
        min: points[0]!.F,
        max: points[points.length - 1]!.F,
        name: "forward",
        nameLocation: "middle",
        nameGap: 26,
        axisLabel: { ...axis.axisLabel, showMinLabel: false, showMaxLabel: false, formatter: (v: number) => fmtStrike(Math.round(v)) },
      },
      yAxis: [
        { ...axis, type: "value", name: "USD", scale: true },
        { ...axis, type: "value", name: "delta", min: -1, max: 1, splitLine: { show: false } },
      ],
      series: [
        {
          name: "value",
          type: "line",
          data: points.map((p) => [p.F, p.value]),
          showSymbol: false,
          lineStyle: { color: t.accent, width: 2 },
          itemStyle: { color: t.accent },
          markLine: {
            silent: true,
            symbol: "none",
            label: { color: t.muted, formatter: "{b}" },
            lineStyle: { color: t.muted, type: "dashed", opacity: 0.6 },
            data: [
              { name: "F", xAxis: F },
              { name: "K", xAxis: K },
            ],
          },
        },
        {
          name: "at expiry",
          type: "line",
          data: points.map((p) => [p.F, p.payoff]),
          showSymbol: false,
          lineStyle: { color: t.muted, width: 1, type: "dashed" },
          itemStyle: { color: t.muted },
        },
        {
          name: "delta",
          type: "line",
          yAxisIndex: 1,
          data: points.map((p) => [p.F, p.delta]),
          showSymbol: false,
          lineStyle: { color: t.warn, width: 1 },
          itemStyle: { color: t.warn },
        },
      ],
    };
  }, [points, F, K]);
  return (
    <Panel title="Value vs forward" right={<span className="muted">sticky strike</span>} className="p-value">
      {option ? <EChart option={option} /> : <Empty>Nothing to price</Empty>}
    </Panel>
  );
}

/** The SVI smile in IV, with Durrleman's g on a second axis; g below 0 in red. */
export function SviChart({ points, k }: { points: SmilePoint[]; k: number | null }) {
  const option = useMemo((): EChartsOption => {
    const t = theme();
    const { axis, base } = axes(t);
    const pct = (v: number) => (v > 0 ? 100 * v : null);
    return {
      ...base,
      grid: { left: 48, right: 56, top: 28, bottom: 40 },
      tooltip: {
        ...base.tooltip,
        formatter: (ps: { seriesName: string; value: [number, number | null] }[]) =>
          `k ${ps[0]!.value[0].toFixed(3)}<br>` +
          ps
            .filter((p) => p.value[1] !== null)
            .map((p) => `${p.seriesName} ${(p.value[1] as number).toFixed(p.seriesName === "IV" ? 2 : 4)}`)
            .join("<br>"),
      },
      xAxis: {
        ...axis,
        type: "value",
        min: points[0]!.k,
        max: points[points.length - 1]!.k,
        name: "ln(K/F)",
        nameLocation: "middle",
        nameGap: 26,
        axisLabel: { ...axis.axisLabel, showMinLabel: false, showMaxLabel: false, formatter: (v: number) => v.toFixed(2) },
      },
      yAxis: [
        { ...axis, type: "value", name: "IV %", scale: true },
        { ...axis, type: "value", name: "g(k)", scale: true, splitLine: { show: false } },
      ],
      series: [
        {
          name: "IV",
          type: "line",
          data: points.map((p) => [p.k, pct(p.iv)]),
          showSymbol: false,
          lineStyle: { color: t.accent, width: 2 },
          itemStyle: { color: t.accent },
          markLine:
            k === null
              ? undefined
              : {
                  silent: true,
                  symbol: "none",
                  label: { color: t.muted, formatter: "K" },
                  lineStyle: { color: t.muted, type: "dashed", opacity: 0.6 },
                  data: [{ xAxis: k }],
                },
        },
        {
          name: "g",
          type: "line",
          yAxisIndex: 1,
          data: points.map((p) => [p.k, p.g]),
          showSymbol: false,
          lineStyle: { color: t.muted, width: 1 },
          itemStyle: { color: t.muted },
          markLine: {
            silent: true,
            symbol: "none",
            label: { show: false },
            lineStyle: { color: t.grid, type: "solid" },
            data: [{ yAxis: 0 }],
          },
        },
        {
          name: "g < 0",
          type: "line",
          yAxisIndex: 1,
          data: points.map((p) => [p.k, p.g < 0 ? p.g : null]),
          showSymbol: false,
          connectNulls: false,
          lineStyle: { color: t.ask, width: 2 },
          itemStyle: { color: t.ask },
          tooltip: { show: false },
        },
      ],
    };
  }, [points, k]);
  return (
    <Panel title="SVI smile" right={<span className="muted">IV and Durrleman g(k)</span>} className="p-smile">
      <EChart option={option} />
    </Panel>
  );
}
