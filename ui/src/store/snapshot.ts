// The one data connection: a WebSocket to od_web (live), or a saved fixture
// replayed every second (mock mode). Components read it with useDesk().

import { useSyncExternalStore } from "react";
import { pushHistory } from "../lib/desk";
import type { Snapshot } from "../types";

export interface HistoryPoint {
  t: number; // client receive time, ms
  updates: number;
  trades: number;
}

export interface DeskState {
  snap: Snapshot | null;
  receivedAt: number; // client clock, ms; 0 = never
  socketOpen: boolean;
  history: HistoryPoint[]; // newest last, one per snapshot
  mock: boolean;
}

const HISTORY = 180;

let state: DeskState = { snap: null, receivedAt: 0, socketOpen: false, history: [], mock: false };
const listeners = new Set<() => void>();

function set(patch: Partial<DeskState>) {
  state = { ...state, ...patch };
  listeners.forEach((l) => l());
}

function receive(snap: Snapshot) {
  const now = Date.now();
  const history =
    snap.feed !== null
      ? pushHistory(state.history, { t: now, updates: snap.feed.updates_per_s, trades: snap.feed.trades_per_s }, HISTORY)
      : state.history;
  set({ snap, receivedAt: now, history });
}

function subscribe(l: () => void) {
  listeners.add(l);
  return () => listeners.delete(l);
}

export function useDesk(): DeskState {
  return useSyncExternalStore(subscribe, () => state);
}

let started = false;

/** Starts the data source once; later calls do nothing. */
export function startDesk() {
  if (started) return;
  started = true;
  if (import.meta.env.MODE === "mock") startMock();
  else startLive();
}

function startLive() {
  let backoff = 1000;
  const connect = () => {
    const proto = location.protocol === "https:" ? "wss" : "ws";
    const ws = new WebSocket(`${proto}://${location.host}/ws`);
    ws.onopen = () => {
      backoff = 1000;
      set({ socketOpen: true });
    };
    ws.onmessage = (ev) => {
      try {
        const msg = JSON.parse(ev.data as string) as Snapshot;
        if (msg.type === "snapshot") receive(msg);
      } catch {
        // ignore a malformed frame; the next snapshot replaces it
      }
    };
    ws.onclose = () => {
      set({ socketOpen: false });
      setTimeout(connect, backoff);
      backoff = Math.min(backoff * 2, 10_000);
    };
  };
  connect();
}

async function startMock() {
  const fixture = (await import("../fixtures/snapshot.json")).default as unknown as Snapshot;
  set({ socketOpen: true, mock: true });
  const tick = () => {
    // Re-stamp so the UI treats it as fresh; jitter the rates so sparklines move.
    const snap = structuredClone(fixture);
    snap.server_ms = Date.now();
    if (snap.feed !== null) {
      snap.feed.updates_per_s *= 0.85 + 0.3 * Math.random();
      snap.feed.trades_per_s = Math.random() < 0.3 ? Math.random() * 2 : 0;
    }
    receive(snap);
  };
  tick();
  setInterval(tick, 1000);
}
