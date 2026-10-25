import { age, utcTime } from "../lib/format";
import type { SessionInfo } from "../lib/demo";
import { SPEEDS, openSession, pause, play, seek, setSpeed } from "../store/replay";
import type { ReplayState } from "../store/snapshot";

function sessionLabel(s: SessionInfo): string {
  const d = new Date(s.start_ms).toISOString();
  return `${s.currency} · ${d.slice(0, 10)} ${d.slice(11, 16)} UTC · ${Math.round((s.end_ms - s.start_ms) / 60_000)} min`;
}

/** Session picker, play/pause, speed and scrubber for the demo's recorded sessions. */
export function ReplayBar({ replay }: { replay: ReplayState }) {
  const { sessions, session, pos, total, playing, speed, loading } = replay;
  const every = session ? session.every_s * 1000 : 0;
  return (
    <div className="replaybar">
      <span className="badge replay">RECORDED</span>
      <select
        value={session?.id ?? ""}
        onChange={(e) => void openSession(e.target.value)}
        disabled={sessions.length === 0}
        title="Recorded session"
      >
        {sessions.map((s) => (
          <option key={s.id} value={s.id}>
            {sessionLabel(s)}
          </option>
        ))}
      </select>
      <button className="play" onClick={playing ? pause : play} disabled={!session} title={playing ? "Pause" : "Play"}>
        {playing ? "❚❚" : "▶"}
      </button>
      <div className="seg" title="Playback speed">
        {SPEEDS.map((s) => (
          <button key={s} className={s === speed ? "on" : ""} onClick={() => setSpeed(s)}>
            {s}×
          </button>
        ))}
      </div>
      <input
        className="scrub"
        type="range"
        min={0}
        max={Math.max(0, total - 1)}
        value={pos}
        onChange={(e) => void seek(+e.target.value)}
        disabled={!session}
        aria-label="Position in the session"
      />
      <span className="num muted">
        {session ? `${utcTime(session.start_ms + pos * every)} · ${age(pos * every)} / ${age((total - 1) * every)}` : "–"}
      </span>
      {loading && <span className="muted">loading…</span>}
    </div>
  );
}
