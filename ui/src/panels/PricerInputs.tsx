import type { OptionKind, Svi } from "../lib/black76";
import { num } from "../lib/format";
import type { PricerInput, SviCheck } from "../lib/pricer";
import type { Expiry } from "../types";
import { NumField } from "./NumField";
import { Panel } from "./Panel";

type Set = (patch: Partial<PricerInput>) => void;

/** Option terms and where the vol comes from; can load an expiry from the Desk. */
export function OptionPanel({
  input,
  set,
  expiries,
  loaded,
  onLoad,
}: {
  input: PricerInput;
  set: Set;
  expiries: Expiry[]; // fitted expiries in the current snapshot
  loaded: string | null;
  onLoad: (label: string) => void;
}) {
  const kind = (k: OptionKind) => (
    <button className={input.kind === k ? "on" : ""} onClick={() => set({ kind: k })}>
      {k === "C" ? "call" : "put"}
    </button>
  );
  const load = (
    <select className="pick" value={loaded ?? ""} onChange={(e) => e.target.value && onLoad(e.target.value)} disabled={!expiries.length}>
      <option value="">{expiries.length ? "load from Desk…" : "Desk: no fitted expiries"}</option>
      {expiries.map((e) => (
        <option key={e.label} value={e.label}>
          {e.label}
        </option>
      ))}
    </select>
  );
  return (
    <Panel title="Option" right={load} className="p-opt">
      <div className="form">
        <div className="field">
          <span>type</span>
          <div className="seg">
            {kind("C")}
            {kind("P")}
          </div>
        </div>
        <NumField label="forward" value={input.F} onChange={(F) => set({ F })} step={100} digits={2} unit="USD" />
        <NumField label="strike" value={input.K} onChange={(K) => set({ K })} step={500} digits={2} unit="USD" />
        <NumField label="expiry" value={input.days} onChange={(days) => set({ days })} step={1} digits={4} unit="days" />
        <div className="field">
          <span>vol</span>
          <div className="seg">
            <button className={input.source === "flat" ? "on" : ""} onClick={() => set({ source: "flat" })}>
              flat
            </button>
            <button className={input.source === "svi" ? "on" : ""} onClick={() => set({ source: "svi" })}>
              SVI
            </button>
          </div>
        </div>
        {input.source === "flat" && (
          <NumField label="IV" value={input.vol} onChange={(vol) => set({ vol })} step={0.5} scale={100} digits={4} unit="%" />
        )}
      </div>
    </Panel>
  );
}

const PARAMS: { key: keyof Svi; min: number; max: number; step: number; note: string }[] = [
  { key: "a", min: -0.1, max: 0.3, step: 0.0005, note: "level" },
  { key: "b", min: 0, max: 1, step: 0.001, note: "wing slope" },
  { key: "rho", min: -0.999, max: 0.999, step: 0.001, note: "skew" },
  { key: "m", min: -0.5, max: 0.5, step: 0.001, note: "shift" },
  { key: "s", min: 0.001, max: 1, step: 0.001, note: "ATM curvature" },
];

/** Raw SVI parameters (total variance at this expiry) with sliders and arbitrage checks. */
export function SviPanel({ svi, onChange, check }: { svi: Svi; onChange: (svi: Svi) => void; check: SviCheck }) {
  const status = !check.valid ? (
    <span className="ask">outside SVI constraints</span>
  ) : check.minG < 0 ? (
    <span className="ask">butterfly arbitrage</span>
  ) : (
    <span className="bid">no butterfly arbitrage</span>
  );
  return (
    <Panel title={<>SVI <span className="muted">w(k) = a + b(ρ(k−m) + √((k−m)² + s²))</span></>} right={status} className="p-svi">
      <div className="form">
        {PARAMS.map((p) => (
          <div key={p.key} className="svi-row">
            <NumField label={p.key} value={svi[p.key]} onChange={(v) => onChange({ ...svi, [p.key]: v })} step={p.step} digits={5} />
            <input
              type="range"
              min={p.min}
              max={p.max}
              step={p.step}
              value={svi[p.key]}
              onChange={(e) => onChange({ ...svi, [p.key]: parseFloat(e.target.value) })}
            />
            <em className="muted">{p.note}</em>
          </div>
        ))}
        <div className="svi-stats">
          <span>
            min g(k) <b className={`num ${check.minG < 0 ? "ask" : ""}`}>{num(check.minG, 4)}</b>
          </span>
          <span className="muted">over the k range in the chart; below 0 the implied density is negative</span>
        </div>
      </div>
    </Panel>
  );
}
