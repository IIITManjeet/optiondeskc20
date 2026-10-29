// The demo's landing page. Numbers are copied from README.md, where each one is
// explained with how it was measured; update both together.

import { Link } from "react-router-dom";

const REPO = "https://github.com/IIITManjeet/optiondeskc20";

const MILESTONES: [string, string][] = [
  ["M1 pricing core", "Black-76 and Greeks, a safeguarded implied-vol solver, SVI smiles fitted per expiry, static-arbitrage checks"],
  ["M2 market data", "TLS WebSocket feed, simdjson parsing, lock-free SPSC rings, a shared-memory feed bus for any number of local readers"],
  ["M3 risk", "Portfolio Greeks for a coin-margined book in USD and coin terms, vega by expiry, spot × vol × time scenarios on the fitted surface"],
  ["M4 market maker", "Vol-space quoting with inventory skew, pre-trade risk gate, delta hedger, simulated exchange with a queue-position fill model"],
  ["M5 testnet gateway", "The same engine sending real orders to Deribit's testnet, with cancel-on-disconnect and pacing to the rate limit"],
  ["M6 record and replay", "A binary market-data journal and deterministic replay of the engine over it, to compare configs"],
];

const LATENCY: [string, string, string, string][] = [
  ["parse (frame read → update pushed)", "2.6 µs", "5.4 µs", "10.8 µs"],
  ["hop (pushed → popped by pricer)", "0.3 µs", "0.7 µs", "111 µs"],
  ["book snapshot (pricer thread)", "172 µs", "188 µs", "254 µs"],
];

const PRIORS: [string, string, string, string, string][] = [
  ["last session", "−0.02", "+0.47", "+0.54", "+0.41"],
  ["all earlier sessions pooled", "+0.18", "+0.41", "+0.48", "+0.50"],
  ["last session, shape only", "+0.19", "+0.67", "+0.75", "+0.62"],
  ["pooled, shape only", "+0.56", "+0.77", "+0.82", "+0.81"],
];

const ALLOCATOR: [string, string, string, string][] = [
  ["fixed order", "67.2%", "8.1%", "79.1%"],
  ["allocator", "91.0%", "2.8%", "42.2%"],
  ["allocator + raw Oct 9 prior", "85.0%", "4.1%", "64.6%"],
  ["allocator + shrunk Oct 9 + 14 prior", "89.2%", "3.3%", "54.7%"],
];

export function About() {
  return (
    <div className="about">
      <header className="about-head">
        <h1>
          options<b>desk</b>
        </h1>
        <p>
          A C++20 options desk for Deribit's BTC options: a live feed and shared-memory bus, an SVI volatility
          surface, risk, a market maker that runs on paper or on Deribit's testnet, and record and replay. This site is
          the read-only half of its web UI, playing back a recorded session.
        </p>
        <nav className="about-links">
          <Link to="/desk">Desk replay →</Link>
          <Link to="/pricer">Pricer →</Link>
          <a href={REPO} target="_blank" rel="noreferrer">
            Source on GitHub ↗
          </a>
        </nav>
      </header>

      <section>
        <h2>Architecture</h2>
        <Architecture />
        <p className="muted">
          One process owns the exchange connection. Everything else reads market data from shared memory, so a new
          reader costs no extra connection, parsing or REST calls. Orders go only to Deribit's testnet: the gateway's
          host is a compile-time constant.
        </p>
      </section>

      <section>
        <h2>Milestones</h2>
        <table className="grid kvtable">
          <tbody>
            {MILESTONES.map(([m, d]) => (
              <tr key={m}>
                <td className="accent">{m}</td>
                <td className="wrap">{d}</td>
              </tr>
            ))}
          </tbody>
        </table>
      </section>

      <section>
        <h2>Latency</h2>
        <p className="muted">
          Live BTC chain (956 options, about 1,000 updates/s), 60 s, busy-polling with pinned threads on WSL2. Through
          the shared-memory bus to another process, the hop was 0.5 µs p50, about the same as a thread-to-thread
          handoff.
        </p>
        <table className="grid">
          <thead>
            <tr>
              <th>stage</th>
              <th className="r">p50</th>
              <th className="r">p90</th>
              <th className="r">p99</th>
            </tr>
          </thead>
          <tbody>
            {LATENCY.map(([s, ...v]) => (
              <tr key={s}>
                <td>{s}</td>
                {v.map((x, i) => (
                  <td key={i} className="num">
                    {x}
                  </td>
                ))}
              </tr>
            ))}
          </tbody>
        </table>
        <p className="muted">
          Moving from a nlohmann DOM to simdjson On-Demand cut parse p50 from 23.6 µs to 2.6 µs. The hop's p99 tail is the
          pricer pausing for its book snapshot.
        </p>
      </section>

      <section>
        <h2>Where will options trade?</h2>
        <p className="muted">
          Deribit's rate limit means the market maker can't refresh every quote, so a budget allocator spends requests
          where a quote is most wrong and most likely to trade. "Likely to trade" comes from a flow prior: trades per
          minute by days to expiry × moneyness, learned from recorded sessions. Scored out of sample over eighteen
          sessions (964 minutes, 1923 test trades), in bits per trade against a flat rate; positive means the prior
          knows where trades go:
        </p>
        <table className="grid">
          <thead>
            <tr>
              <th>prior</th>
              <th className="r">raw</th>
              <th className="r">shrink 300</th>
              <th className="r">shrink 1000</th>
              <th className="r">shrink 3000</th>
            </tr>
          </thead>
          <tbody>
            {PRIORS.map(([p, ...v]) => (
              <tr key={p}>
                <td>{p}</td>
                {v.map((x, i) => (
                  <td key={i} className={`num ${x.startsWith("−") ? "ask" : ""}`}>
                    {x}
                  </td>
                ))}
              </tr>
            ))}
          </tbody>
        </table>
        <p className="muted">
          A raw prior is overconfident (a bucket that didn't trade gets a rate near zero), and the level of activity moves
          from day to day. Shrinking towards the average and scoring the shape fixes both. Replaying one hour at
          2 requests/s:
        </p>
        <table className="grid">
          <thead>
            <tr>
              <th>Oct 15, 2 requests/s</th>
              <th className="r">quotes up</th>
              <th className="r">too generous</th>
              <th className="r">requests to options &lt; 2 days</th>
            </tr>
          </thead>
          <tbody>
            {ALLOCATOR.map(([p, ...v]) => (
              <tr key={p}>
                <td>{p}</td>
                {v.map((x, i) => (
                  <td key={i} className="num">
                    {x}
                  </td>
                ))}
              </tr>
            ))}
          </tbody>
        </table>
        <p className="muted">
          Fills are still too few to compare on P&amp;L, so the bits-per-trade score is the measure used for priors.
        </p>
      </section>
    </div>
  );
}

/** Data flow between the processes, drawn in the page's colours. */
function Architecture() {
  const box = (x: number, y: number, w: number, h: number, title: string, sub: string[], accent = false) => (
    <g key={title}>
      <rect x={x} y={y} width={w} height={h} rx={3} className={accent ? "arch-box accent" : "arch-box"} />
      <text x={x + w / 2} y={y + 17} className="arch-title">
        {title}
      </text>
      {sub.map((s, i) => (
        <text key={s} x={x + w / 2} y={y + 33 + 13 * i} className="arch-sub">
          {s}
        </text>
      ))}
    </g>
  );
  const arrow = (x1: number, y1: number, x2: number, y2: number) => (
    <line key={`${x1},${y1},${x2},${y2}`} x1={x1} y1={y1} x2={x2} y2={y2} className="arch-edge" markerEnd="url(#arch-arrow)" />
  );
  const readers: [string, string, number][] = [
    ["od_web", "surface, snapshots", 20],
    ["od_mm", "quoter, risk gate", 82],
    ["od_risk", "Greeks, scenarios", 144],
    ["od_record", "binary journal", 206],
    ["od_live", "terminal surface", 268],
  ];
  return (
    <svg className="arch" viewBox="0 0 900 320" role="img" aria-label="Deribit feeds od_feedd, which publishes to a shared-memory bus read by od_web, od_mm, od_risk, od_record and od_live">
      <defs>
        <marker id="arch-arrow" viewBox="0 0 8 8" refX="7" refY="4" markerWidth="7" markerHeight="7" orient="auto">
          <path d="M0,0 L8,4 L0,8 z" className="arch-head" />
        </marker>
      </defs>
      {box(10, 135, 120, 50, "Deribit", ["public WebSocket"])}
      {box(170, 135, 130, 50, "od_feedd", ["TLS WS · simdjson"], true)}
      {box(345, 95, 140, 130, "feed bus", ["shared memory", "instrument table", "last-value cache", "update ring"], true)}
      {readers.map(([n, s, y]) => box(540, y, 140, 42, n, [s]))}
      {box(735, 20, 150, 42, "browser", ["this UI"])}
      {box(735, 82, 150, 42, "Deribit testnet", ["orders via gateway"])}
      {box(735, 206, 150, 42, "od_replay", ["engine over journal"])}
      {box(735, 268, 150, 42, "od_snapshots", ["demo replay files"])}
      {arrow(130, 160, 168, 160)}
      {arrow(300, 160, 343, 160)}
      {readers.map(([, , y]) => arrow(485, 160, 538, y + 21))}
      {arrow(680, 41, 733, 41)}
      {arrow(680, 103, 733, 103)}
      {arrow(680, 227, 733, 227)}
      {arrow(680, 227, 733, 289)}
    </svg>
  );
}
