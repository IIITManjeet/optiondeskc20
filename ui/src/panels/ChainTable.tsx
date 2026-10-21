import { useEffect, useRef } from "react";
import { atmRow } from "../lib/desk";
import { count, iv, num, price, strike as fmtStrike, usd } from "../lib/format";
import type { Expiry, OptionRow } from "../types";
import { Empty, Panel } from "./Panel";

/** Calls | strike | puts for one expiry. ITM sides shaded, the row nearest the forward marked. */
export function ChainTable({ expiry, linear, dim }: { expiry: Expiry | undefined; linear: boolean; dim: boolean }) {
  const atmRef = useRef<HTMLTableRowElement>(null);
  const atm = expiry ? atmRow(expiry.chain, expiry.forward) : -1;
  const label = expiry?.label;

  // Centre the ATM row when the expiry changes (not on every tick).
  useEffect(() => {
    atmRef.current?.scrollIntoView({ block: "center" });
  }, [label]);

  const title = expiry ? (
    <>
      Chain <span className="accent">{expiry.label}</span>
      <span className="muted"> · fwd {usd(expiry.forward, 0)}</span>
    </>
  ) : (
    "Chain"
  );
  return (
    <Panel title={title} right={<span className="muted">{linear ? "prices in USDC" : "prices in coin · hover for USD"}</span>} dim={dim} className="chain">
      {!expiry || expiry.chain.length === 0 ? (
        <Empty>No quotes</Empty>
      ) : (
        <div className="scroll">
          <table className="grid chain-table">
            <thead>
              <tr>
                <th className="r">OI</th>
                <th className="r">Δ</th>
                <th className="r">IV</th>
                <th className="r bid">bid</th>
                <th className="r ask">ask</th>
                <th className="c strike">strike</th>
                <th className="r bid">bid</th>
                <th className="r ask">ask</th>
                <th className="r">IV</th>
                <th className="r">Δ</th>
                <th className="r">OI</th>
              </tr>
            </thead>
            <tbody>
              {expiry.chain.map((r, i) => {
                const callItm = r.strike < expiry.forward;
                return (
                  <tr key={r.strike} ref={i === atm ? atmRef : undefined} className={i === atm ? "atm" : undefined}>
                    <Side o={r.call} itm={callItm} F={expiry.forward} linear={linear} order="call" />
                    <td className="c strike num">{fmtStrike(r.strike)}</td>
                    <Side o={r.put} itm={!callItm} F={expiry.forward} linear={linear} order="put" />
                  </tr>
                );
              })}
            </tbody>
          </table>
        </div>
      )}
    </Panel>
  );
}

function Side({ o, itm, F, linear, order }: { o: OptionRow | null; itm: boolean; F: number; linear: boolean; order: "call" | "put" }) {
  const cls = itm ? "itm" : "";
  if (!o) {
    return (
      <>
        {Array.from({ length: 5 }, (_, i) => (
          <td key={i} className={`num ${cls}`}>
            –
          </td>
        ))}
      </>
    );
  }
  const tip = (p: number) => (linear || p <= 0 ? undefined : `$${usd(p * F, 2)}`);
  const cells = [
    <td key="oi" className={`num ${cls} muted`}>{count(o.oi)}</td>,
    <td key="d" className={`num ${cls}`}>{num(o.delta, 2)}</td>,
    <td key="iv" className={`num ${cls}`} title={`${o.name}`}>{iv(o.mark_iv)}</td>,
    <td key="b" className={`num ${cls} bid`} title={tip(o.bid)}>{price(o.bid, linear)}</td>,
    <td key="a" className={`num ${cls} ask`} title={tip(o.ask)}>{price(o.ask, linear)}</td>,
  ];
  // Puts mirror the calls: bid, ask, IV, delta, OI reading outward from the strike.
  return <>{order === "call" ? cells : [cells[3], cells[4], cells[2], cells[1], cells[0]]}</>;
}
