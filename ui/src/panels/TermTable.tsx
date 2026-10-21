import { arbFlags } from "../lib/desk";
import { iv, num, tenor, usd, volPts } from "../lib/format";
import type { Expiry } from "../types";
import { Empty, Panel } from "./Panel";

export function TermTable({
  expiries,
  selected,
  onSelect,
  dim,
}: {
  expiries: Expiry[];
  selected: string | undefined;
  onSelect: (label: string) => void;
  dim: boolean;
}) {
  return (
    <Panel title="Term structure" right={<span className="muted">click a row to select</span>} dim={dim}>
      {expiries.length === 0 ? (
        <Empty>No expiries yet</Empty>
      ) : (
        <table className="grid clickable">
          <thead>
            <tr>
              <th>expiry</th>
              <th className="r">tenor</th>
              <th className="r">forward</th>
              <th className="r">ATM</th>
              <th className="r">RR25</th>
              <th className="r">BF25</th>
              <th className="r">rmse</th>
              <th className="r">inside</th>
              <th>flags</th>
            </tr>
          </thead>
          <tbody>
            {expiries.map((e) => {
              const f = e.fit;
              const flags = arbFlags(e);
              return (
                <tr
                  key={e.label}
                  className={e.label === selected ? "sel" : undefined}
                  onClick={() => onSelect(e.label)}
                >
                  <td>{e.label}</td>
                  <td className="num">{tenor(e.days)}</td>
                  <td className="num">{usd(e.forward, 0)}</td>
                  <td className="num">{iv(f?.atm_iv)}</td>
                  <td className="num">{f ? volPts(f.rr25) : "–"}</td>
                  <td className="num">{f ? volPts(f.bf25) : "–"}</td>
                  <td className="num">{f ? num(100 * f.rmse, 2) : "–"}</td>
                  <td className="num">{f ? `${f.n_inside}/${f.n_fit}` : "–"}</td>
                  <td>
                    {!f && <span className="muted">no fit</span>}
                    {flags.map((x) => (
                      <span key={x} className="flag">
                        {x}
                      </span>
                    ))}
                  </td>
                </tr>
              );
            })}
          </tbody>
        </table>
      )}
    </Panel>
  );
}
