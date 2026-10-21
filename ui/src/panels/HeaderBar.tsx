import type { FeedState } from "../lib/desk";
import { age, usd, utcTime } from "../lib/format";
import type { Snapshot } from "../types";

const LABEL: Record<FeedState, string> = {
  connecting: "CONNECTING",
  waiting: "WAITING FOR FEED",
  live: "LIVE",
  stale: "STALE",
  disconnected: "DISCONNECTED",
};

export function HeaderBar({
  snap,
  state,
  ageMs,
  mock,
}: {
  snap: Snapshot | null;
  state: FeedState;
  ageMs: number;
  mock: boolean;
}) {
  const live = snap && snap.feed !== null ? snap : null;
  return (
    <div className="headerbar">
      <span className="ccy">{live?.currency ?? "—"}</span>
      <span className="kv">
        <label>index</label>
        <b className="num">{live ? usd(live.index, 2) : "–"}</b>
      </span>
      <span className="kv">
        <label>exch time</label>
        <b className="num">{live ? utcTime(live.asof_ms) + " UTC" : "–"}</b>
      </span>
      <span className="spacer" />
      {mock && <span className="badge mock">MOCK DATA</span>}
      {snap && snap.feed === null && <span className="hint">od_web is up; start od_feedd ({snap.waiting_for})</span>}
      <span className="kv">
        <label>data age</label>
        <b className="num">{snap ? age(ageMs) : "–"}</b>
      </span>
      <span className={`badge state-${state}`}>
        <i className="dot" />
        {LABEL[state]}
      </span>
    </div>
  );
}
