import { useEffect, useState } from "react";
import { feedState, isLinear, pickExpiry } from "../lib/desk";
import { usePref } from "../lib/prefs";
import { ChainTable } from "../panels/ChainTable";
import { FeedHealth } from "../panels/FeedHealth";
import { HeaderBar } from "../panels/HeaderBar";
import { ReplayBar } from "../panels/ReplayBar";
import { SmileChart, type SmileAxis } from "../panels/SmileChart";
import { TermChart } from "../panels/TermChart";
import { TermTable } from "../panels/TermTable";
import { TradeTape } from "../panels/TradeTape";
import { useDesk } from "../store/snapshot";
import { isLive } from "../types";

/** Re-render every `ms` so ages and staleness advance between snapshots. */
function useNow(ms: number) {
  const [now, setNow] = useState(Date.now());
  useEffect(() => {
    const id = setInterval(() => setNow(Date.now()), ms);
    return () => clearInterval(id);
  }, [ms]);
  return now;
}

export function Desk() {
  const { snap, receivedAt, socketOpen, history, mock, replay } = useDesk();
  const now = useNow(500);
  const ageMs = receivedAt ? Math.max(0, now - receivedAt) : 0;
  // A replay is never stale: pausing it is not the feed stopping.
  const state = replay ? "replay" : feedState(snap, socketOpen, ageMs);
  const dim = state !== "live" && state !== "replay";

  const [wanted, setWanted] = usePref("desk.expiry", null);
  const [axis, setAxis] = usePref<SmileAxis>("desk.axis", "strike");

  const live = isLive(snap) ? snap : null;
  const expiry = live ? pickExpiry(live, wanted) : undefined;
  const linear = live ? isLinear(live.currency) : false;

  return (
    <div className="desk">
      <HeaderBar snap={snap} state={state} ageMs={ageMs} mock={mock} />
      {replay && <ReplayBar replay={replay} />}
      {!live ? (
        <div className="desk-wait">
          {replay
            ? (replay.error ?? "Loading a recorded session…")
            : state === "connecting" || state === "disconnected"
            ? "Connecting to od_web… (WSL: ~/build/optionsdesk/od_web)"
            : "od_web is running but no feed is attached. Start od_feedd --currency BTC."}
        </div>
      ) : (
        <div className="desk-grid">
          <TermTable expiries={live.expiries} selected={expiry?.label} onSelect={setWanted} dim={dim} />
          <SmileChart expiry={expiry} axis={axis} onAxis={setAxis} dim={dim} />
          <ChainTable expiry={expiry} linear={linear} dim={dim} />
          <TermChart expiries={live.expiries} dim={dim} />
          <TradeTape trades={live.trades} linear={linear} dim={dim} />
          <FeedHealth feed={live.feed} fitMs={live.fit_ms} history={history} dim={dim} />
        </div>
      )}
    </div>
  );
}
