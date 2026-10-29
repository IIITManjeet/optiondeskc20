import { useMemo, useState } from "react";
import {
  DAYS_PER_YEAR,
  checkSvi,
  fromExpiry,
  inputError,
  ladder,
  markIv,
  price,
  smile,
  type PricerInput,
} from "../lib/pricer";
import { OptionPanel, SviPanel } from "../panels/PricerInputs";
import { ValueChart, SviChart } from "../panels/PricerCharts";
import { PricerResults } from "../panels/PricerResults";
import { useDesk } from "../store/snapshot";
import { isLive, type Expiry } from "../types";

const START: PricerInput = {
  kind: "C",
  F: 100_000,
  K: 105_000,
  days: 30,
  source: "flat",
  vol: 0.5,
  svi: { a: 0.012, b: 0.1, rho: -0.3, m: 0.02, s: 0.12 },
};

/**
 * Black-76 and SVI playground. Everything is computed in the browser from the
 * inputs; loading an expiry copies its forward, tenor and fit from the Desk once.
 */
export function Pricer() {
  const { snap } = useDesk();
  const [input, setInput] = useState<PricerInput>(START);
  const [loaded, setLoaded] = useState<Expiry | null>(null);
  const set = (patch: Partial<PricerInput>) => setInput((p) => ({ ...p, ...patch }));

  const fitted = isLive(snap) ? snap.expiries.filter((e) => e.fit) : [];
  const onLoad = (label: string) => {
    const e = fitted.find((x) => x.label === label);
    if (!e) return;
    setLoaded(e);
    set(fromExpiry(e, input.kind));
  };

  const error = inputError(input);
  const r = useMemo(() => price(input), [input]);
  const values = useMemo(() => ladder(input), [input]);

  // The smile's k range: the Desk fit's range if loaded, else about ±3 ATM
  // standard deviations, and always wide enough to show the strike.
  const T = input.days > 0 ? input.days / DAYS_PER_YEAR : 1 / DAYS_PER_YEAR;
  const kStrike = input.F > 0 && input.K > 0 ? Math.log(input.K / input.F) : null;
  const [kLo, kHi] = useMemo(() => {
    let lo: number;
    let hi: number;
    if (loaded?.fit) {
      const pad = 0.25 * (loaded.fit.k_hi - loaded.fit.k_lo) + 0.01;
      [lo, hi] = [loaded.fit.k_lo - pad, loaded.fit.k_hi + pad];
    } else {
      const atm = checkSvi(input.svi, T, -1, 1).atmIv || 0.5;
      const span = Math.min(Math.max(3 * atm * Math.sqrt(T), 0.1), 1.5);
      [lo, hi] = [-span, span];
    }
    if (kStrike !== null) [lo, hi] = [Math.min(lo, kStrike - 0.02), Math.max(hi, kStrike + 0.02)];
    return [lo, hi];
  }, [loaded, input.svi, T, kStrike]);
  const curve = useMemo(() => smile(input.svi, T, kLo, kHi), [input.svi, T, kLo, kHi]);
  const check = useMemo(() => checkSvi(input.svi, T, kLo, kHi), [input.svi, T, kLo, kHi]);

  // Compare with the exchange only while the inputs still describe the loaded expiry.
  const sameExpiry = loaded && loaded.forward === input.F && loaded.days === input.days;
  const mark = sameExpiry ? markIv(loaded, input.K, input.kind) : 0;

  return (
    <div className="pricer">
      <OptionPanel input={input} set={set} expiries={fitted} loaded={sameExpiry ? loaded.label : null} onLoad={onLoad} />
      <SviPanel svi={input.svi} onChange={(svi) => set({ svi })} check={check} />
      <PricerResults r={r} error={error} markIv={mark} />
      <ValueChart points={values} F={input.F} K={input.K} />
      <SviChart points={curve} k={kStrike} />
    </div>
  );
}
