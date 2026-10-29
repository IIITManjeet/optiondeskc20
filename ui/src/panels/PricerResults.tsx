import { iv, num, usd, volPts } from "../lib/format";
import type { PricerResult } from "../lib/pricer";
import { Empty, Panel } from "./Panel";

/** Price and Greeks in the desk's units; the exchange's mark IV if loaded from the Desk. */
export function PricerResults({ r, error, markIv }: { r: PricerResult | null; error: string | null; markIv: number }) {
  if (!r) {
    return (
      <Panel title="Price and Greeks" className="p-res">
        <Empty>{error}</Empty>
      </Panel>
    );
  }
  const rows: [string, string, string][] = [
    ["vol", iv(r.sigma), "%"],
    ["ln(K/F)", num(r.k, 4), ""],
    ["T", num(r.T, 5), "years"],
    ["price", num(r.priceCoin, 4), "coin"],
    ["price", usd(r.priceUsd, 2), "USD"],
    ["intrinsic", usd(r.intrinsicUsd, 2), "USD"],
    ["time value", usd(r.priceUsd - r.intrinsicUsd, 2), "USD"],
    ["delta", num(r.delta, 4), "USD"],
    ["delta", num(r.deltaCoin, 4), "coin, premium-adjusted"],
    ["gamma", num(r.gamma, 5), "Δ per 1% move"],
    ["vega", usd(r.vega, 2), "USD per vol pt"],
    ["theta", usd(r.theta, 2), "USD per day"],
  ];
  if (markIv > 0) {
    rows.push(["Desk mark IV", iv(markIv), "%"]);
    rows.push(["vol − mark", volPts(r.sigma - markIv), "vol pts"]);
  }
  return (
    <Panel title="Price and Greeks" right={<span className="muted">Black-76 on the forward</span>} className="p-res">
      <table className="grid kvtable">
        <tbody>
          {rows.map(([k, v, u], i) => (
            <tr key={i}>
              <td className="muted">{k}</td>
              <td className="num">{v}</td>
              <td className="muted">{u}</td>
            </tr>
          ))}
        </tbody>
      </table>
    </Panel>
  );
}
