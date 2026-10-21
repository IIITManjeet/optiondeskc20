import { count, num } from "../lib/format";
import type { HistoryPoint } from "../store/snapshot";
import type { FeedStats } from "../types";
import { Panel } from "./Panel";

export function FeedHealth({
  feed,
  fitMs,
  history,
  dim,
}: {
  feed: FeedStats | null;
  fitMs: number | null;
  history: HistoryPoint[];
  dim: boolean;
}) {
  if (!feed) {
    return (
      <Panel title="Feed health" dim={dim} className="health">
        <div className="empty">No feed attached</div>
      </Panel>
    );
  }
  const stats: [string, string, boolean?][] = [
    ["bus", feed.bus],
    ["exchange", feed.connected ? "connected" : "DOWN", !feed.connected],
    ["live / instruments", `${feed.live} / ${feed.instruments}`, feed.live < feed.instruments],
    ["subscribed", count(feed.subscribed)],
    ["bus hop p50 / p99", `${num(feed.hop_p50_us, 0)} / ${num(feed.hop_p99_us, 0)} µs`],
    ["surface fit", fitMs === null ? "–" : `${num(fitMs, 1)} ms`],
    ["reconnects", count(feed.reconnects), feed.reconnects > 0],
    ["drops / laps", `${count(feed.drops)} / ${count(feed.laps)}`, feed.drops + feed.laps > 0],
    ["frames", count(feed.frames)],
    ["heartbeats", count(feed.heartbeats)],
  ];
  return (
    <Panel title="Feed health" dim={dim} className="health">
      <Spark label="updates/s" value={feed.updates_per_s} values={history.map((h) => h.updates)} />
      <Spark label="trades/s" value={feed.trades_per_s} values={history.map((h) => h.trades)} digits={2} />
      <dl className="stats">
        {stats.map(([k, v, warn]) => (
          <div key={k}>
            <dt>{k}</dt>
            <dd className={`num ${warn ? "warn" : ""}`}>{v}</dd>
          </div>
        ))}
      </dl>
    </Panel>
  );
}

/** Inline SVG sparkline: cheaper than a chart instance for a strip this small. */
function Spark({ label, value, values, digits = 0 }: { label: string; value: number; values: number[]; digits?: number }) {
  const w = 220;
  const h = 32;
  const max = Math.max(1e-9, ...values);
  const pts = values.map((v, i) => `${values.length < 2 ? 0 : (i / (values.length - 1)) * w},${h - (v / max) * (h - 2) - 1}`);
  return (
    <div className="spark">
      <div className="spark-head">
        <span className="muted">{label}</span>
        <b className="num">{num(value, digits)}</b>
      </div>
      <svg viewBox={`0 0 ${w} ${h}`} preserveAspectRatio="none">
        {pts.length > 1 && <polyline points={pts.join(" ")} />}
      </svg>
    </div>
  );
}
