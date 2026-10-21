import { useMemo } from "react";
import { EChart, theme, type EChartsOption } from "../charts/EChart";
import { tenor } from "../lib/format";
import type { Expiry } from "../types";
import { Empty, Panel } from "./Panel";

/** ATM IV (left axis) and RR25 / BF25 (right axis) against days to expiry. */
export function TermChart({ expiries, dim }: { expiries: Expiry[]; dim: boolean }) {
  const fitted = useMemo(() => expiries.filter((e) => e.fit), [expiries]);
  const option = useMemo(() => (fitted.length ? termOption(fitted) : null), [fitted]);
  return (
    <Panel title="Term structure" dim={dim} className="term">
      {option ? <EChart option={option} /> : <Empty>No fitted expiries</Empty>}
    </Panel>
  );
}

function termOption(es: Expiry[]): EChartsOption {
  const t = theme();
  const axisCommon = {
    axisLine: { lineStyle: { color: t.grid } },
    axisLabel: { color: t.muted },
    nameTextStyle: { color: t.muted },
  };
  const line = (name: string, color: string, data: (number | string)[][], yAxisIndex = 0, dashed = false) => ({
    name,
    type: "line",
    yAxisIndex,
    data,
    symbolSize: 5,
    lineStyle: { color, width: 2, type: dashed ? "dashed" : "solid" },
    itemStyle: { color },
  });
  return {
    animation: false,
    textStyle: { fontFamily: t.font, color: t.text },
    grid: { left: 48, right: 48, top: 36, bottom: 40 },
    legend: { top: 2, left: "center", textStyle: { color: t.muted }, itemWidth: 12, itemHeight: 8 },
    tooltip: {
      trigger: "axis",
      backgroundColor: "#11161c",
      borderColor: t.grid,
      textStyle: { color: t.text, fontFamily: t.font, fontSize: 12 },
      valueFormatter: (v: number) => (typeof v === "number" ? v.toFixed(2) : "–"),
    },
    xAxis: {
      type: "log",
      name: "days",
      nameLocation: "middle",
      nameGap: 26,
      ...axisCommon,
      axisLabel: { color: t.muted, formatter: (v: number) => tenor(v) },
      splitLine: { lineStyle: { color: t.grid, opacity: 0.4 } },
    },
    yAxis: [
      { type: "value", name: "ATM %", scale: true, ...axisCommon, splitLine: { lineStyle: { color: t.grid, opacity: 0.4 } } },
      { type: "value", name: "vol pts", scale: true, ...axisCommon, splitLine: { show: false } },
    ],
    series: [
      line("ATM", t.accent, es.map((e) => [e.days, 100 * e.fit!.atm_iv, e.label])),
      line("RR25", t.ask, es.map((e) => [e.days, 100 * e.fit!.rr25, e.label]), 1, true),
      line("BF25", t.warn, es.map((e) => [e.days, 100 * e.fit!.bf25, e.label]), 1, true),
    ],
  };
}
