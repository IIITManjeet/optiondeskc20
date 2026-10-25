// The demo's replay player: recorded sessions from static files instead of od_web.
//
//   sessions/index.json            every session, newest first
//   sessions/<id>/index.json       one session and its chunks
//   sessions/<id>/NNN.ndjson.gz    a keyframe, then patches (see lib/demo.ts)
//
// Chunks are fetched when first needed and the next one is prefetched near the end
// of the current one, so playback and scrubbing only ever wait for one file.

import {
  DEMO_FORMAT,
  decodeChunk,
  expand,
  fetchGzipText,
  type CompactSnapshot,
  type Session,
  type SessionInfo,
} from "../lib/demo";
import { getState, receive, set, type ReplayState } from "./snapshot";

const BASE = `${import.meta.env.BASE_URL}sessions/`;
export const SPEEDS = [1, 5, 15, 60];

let session: Session | null = null;
let starts: number[] = []; // first snapshot index of each chunk
let chunks = new Map<number, Promise<CompactSnapshot[]>>();
let timer: ReturnType<typeof setTimeout> | undefined;
let seq = 0; // bumped by every seek, so a late chunk can't overwrite a newer position

function update(patch: Partial<ReplayState>) {
  const r = getState().replay;
  if (r) set({ replay: { ...r, ...patch } });
}

async function getJson<T>(url: string): Promise<T> {
  const res = await fetch(url);
  if (!res.ok) throw new Error(`${url}: HTTP ${res.status}`);
  return (await res.json()) as T;
}

export async function startReplay() {
  set({
    socketOpen: true,
    replay: { sessions: [], session: null, pos: 0, total: 0, playing: false, speed: 5, loading: true, error: null },
  });
  try {
    const idx = await getJson<{ sessions: SessionInfo[] }>(`${BASE}index.json`);
    const sessions = idx.sessions.filter((s) => s.format === DEMO_FORMAT);
    update({ sessions });
    if (sessions[0]) await openSession(sessions[0].id);
    else update({ loading: false, error: "No recorded sessions in this build." });
  } catch (e) {
    update({ loading: false, error: `Could not load the recorded sessions (${String(e)}).` });
  }
}

export async function openSession(id: string) {
  pause();
  update({ loading: true, error: null });
  try {
    session = await getJson<Session>(`${BASE}${id}/index.json`);
  } catch (e) {
    update({ loading: false, error: `Could not load session ${id} (${String(e)}).` });
    return;
  }
  chunks = new Map();
  starts = [];
  let n = 0;
  for (const c of session.chunks) {
    starts.push(n);
    n += c.snapshots;
  }
  set({ history: [] });
  update({ session, total: n, pos: 0 });
  await seek(0);
  play();
}

function loadChunk(ci: number): Promise<CompactSnapshot[]> {
  let p = chunks.get(ci);
  const s = session;
  const c = s?.chunks[ci];
  if (!p && s && c) {
    p = fetchGzipText(`${BASE}${s.id}/${c.file}`).then(decodeChunk);
    p.catch(() => chunks.delete(ci)); // retry on the next request
    chunks.set(ci, p);
  }
  return p ?? Promise.resolve([]);
}

/** Shows snapshot `pos` (clamped). Resolves once it is on screen, or superseded. */
export async function seek(pos: number) {
  const r = getState().replay;
  if (!session || !r || r.total === 0) return;
  pos = Math.max(0, Math.min(r.total - 1, Math.round(pos)));
  const mine = ++seq;
  let ci = starts.length - 1;
  while (ci > 0 && starts[ci]! > pos) --ci;
  update({ pos, loading: !chunks.has(ci) });
  try {
    const snaps = await loadChunk(ci);
    if (mine !== seq) return;
    const c = snaps[pos - starts[ci]!];
    if (c) receive(expand(c));
    update({ loading: false, error: null });
    if (pos - starts[ci]! > snaps.length - 20 && ci + 1 < starts.length) void loadChunk(ci + 1);
  } catch (e) {
    if (mine === seq) update({ loading: false, error: `Could not load part of the session (${String(e)}).` });
  }
}

function schedule() {
  clearTimeout(timer);
  const r = getState().replay;
  if (!session || !r?.playing) return;
  timer = setTimeout(async () => {
    const now = getState().replay;
    if (!now?.playing) return;
    await seek(now.pos + 1 >= now.total ? 0 : now.pos + 1); // loop: the demo never goes quiet
    schedule();
  }, (session.every_s * 1000) / r.speed);
}

export function play() {
  update({ playing: true });
  schedule();
}

export function pause() {
  clearTimeout(timer);
  update({ playing: false });
}

export function setSpeed(speed: number) {
  update({ speed });
  schedule();
}
