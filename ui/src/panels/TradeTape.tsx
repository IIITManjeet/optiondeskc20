import { iv, num, price, utcTime } from "../lib/format";
import type { Trade } from "../types";
import { Empty, Panel } from "./Panel";

export function TradeTape({ trades, linear, dim }: { trades: Trade[]; linear: boolean; dim: boolean }) {
  return (
    <Panel title="Trades" right={<span className="muted">last {trades.length}</span>} dim={dim} className="tape">
      {trades.length === 0 ? (
        <Empty>No trades since od_web attached</Empty>
      ) : (
        <div className="scroll">
          <table className="grid">
            <thead>
              <tr>
                <th>time</th>
                <th>instrument</th>
                <th>side</th>
                <th className="r">price</th>
                <th className="r">size</th>
                <th className="r">IV</th>
              </tr>
            </thead>
            <tbody>
              {trades.map((t, i) => (
                <tr key={`${t.ts_ms}-${t.name}-${i}`}>
                  <td className="num muted">{utcTime(t.ts_ms)}</td>
                  <td title={t.name}>{t.name.slice(t.name.indexOf("-") + 1)}</td>
                  <td className={t.side === "buy" ? "bid" : "ask"}>{t.side}</td>
                  <td className="num">{price(t.price, linear)}</td>
                  <td className="num">{num(t.amount, 1)}</td>
                  <td className="num">{iv(t.iv)}</td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      )}
    </Panel>
  );
}
