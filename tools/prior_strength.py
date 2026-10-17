"""Tune prior_strength_min: how the engine should weigh the flow prior against trades it sees.

    python3 tools/prior_strength.py ~/btc_session.odj ~/btc_oct14.odj ~/rec/btc_*.odj

The engine expects (MarketMaker::expected_trades_per_min) an option to trade at

    (prior x m + activity) / (m + 5 / ln 2)    trades per minute

where activity is the option's trades so far, decayed with a 5-minute half-life, and
m is prior_strength_min. This replays that forecast minute by minute over each
journal after the first, with a prior pooled from the journals before it (shrunk by
--shrink, see journal_stats.pooled_rate), and scores it on the trades of the next
minute: bits per trade against a flat rate from the test journal itself, as in
prior_eval.py. m = 0 is "only what's trading now", a huge m is "only the prior".

--shape rescales each forecast to the test journal's total, scoring only which
options it ranks busiest, which is what the allocator uses.
Journals are taken in the order given; pass them sorted by time.
"""

import argparse
import math
import os
from collections import defaultdict

from journal_stats import DAYS_EDGES, MONEYNESS_EDGES, bucket, pooled_rate, summarise

WINDOW_MIN = 5.0 / math.log(2)
STRENGTHS = [0, 1, 3, 10, 30, 100, 300, 1e9]


def forecast_rows(stats, prior):
    """[(prior rate, activity at minute start, trades in that minute)] per option-minute."""
    minutes, first_ms, expiries, strikes, forward, trades = stats
    if not first_ms or minutes < 1:
        return []
    mid_ms = first_ms + minutes * 30_000
    times = defaultdict(list)
    for i, *_, exch_ms in trades:
        times[i].append(exch_ms)
    rows = []
    for i, fwd in forward.items():
        days = (expiries[i] - mid_ms) / 86_400_000
        if days <= 0:
            continue
        p = prior[bucket(DAYS_EDGES, days)][bucket(MONEYNESS_EDGES, abs(math.log(strikes[i] / fwd)))]
        ts = sorted(times.get(i, ()))
        for b in range(int(minutes)):
            start = first_ms + b * 60_000
            act = sum(2.0 ** (-(start - t) / 300_000) for t in ts if t < start)
            n = sum(start <= t < start + 60_000 for t in ts)
            rows.append((p, act, n))
    return rows


def score(rows, m, floor, shape):
    """Poisson log-likelihood (nats) of the next-minute counts under strength m."""
    if m >= 1e8:
        lam = [max(p, floor) for p, _, _ in rows]
    else:
        lam = [max((p * m + a) / (m + WINDOW_MIN), floor) for p, a, _ in rows]
    trades = sum(n for *_, n in rows)
    if shape:
        s = trades / sum(lam)
        lam = [x * s for x in lam]
    return sum(n * math.log(x) - x for x, (_, _, n) in zip(lam, rows))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("journal", nargs="+")
    ap.add_argument("--shrink", type=float, default=1000.0)
    ap.add_argument("--floor", type=float, default=1e-4)
    ap.add_argument("--shape", action="store_true")
    args = ap.parse_args()
    if len(args.journal) < 2:
        raise SystemExit("need at least two journals")

    stats = [summarise(p, verbose=False) for p in args.journal]
    label = lambda m: "prior" if m >= 1e8 else f"m={m:g}"
    totals = defaultdict(float)
    total_trades = 0
    print(f"{'test journal':<34} {'trades':>6} " + "".join(f"{label(m):>8}" for m in STRENGTHS))
    for t in range(1, len(stats)):
        rows = forecast_rows(stats[t], pooled_rate(stats[:t], args.shrink)[0])
        trades = sum(n for *_, n in rows)
        if not trades:
            continue
        flat = max(trades / len(rows), args.floor)
        base = sum(n * math.log(flat) - flat for *_, n in rows)
        cells = []
        for m in STRENGTHS:
            gain = score(rows, m, args.floor, args.shape) - base
            totals[m] += gain
            cells.append(gain / trades / math.log(2))
        total_trades += trades
        print(f"{os.path.basename(args.journal[t]):<34} {trades:6d} " + "".join(f"{c:+8.3f}" for c in cells))
    if total_trades:
        print(f"{'all (trade-weighted)':<34} {total_trades:6d} "
              + "".join(f"{totals[m] / total_trades / math.log(2):+8.3f}" for m in STRENGTHS))
    print("\nbits per trade vs flat for the next minute's trades; m = prior_strength_min"
          + ("\n(--shape: forecasts rescaled to each test journal's total)" if args.shape else ""))


if __name__ == "__main__":
    main()
