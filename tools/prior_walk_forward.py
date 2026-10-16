"""Walk-forward test of flow priors over consecutive journals (e.g. hourly chunks).

    python3 tools/prior_walk_forward.py ~/rec/btc_*.odj
    python3 tools/prior_walk_forward.py ~/rec/btc_*.odj --shrink 0 300 1000 --shape

For each journal after the first, score (bits/trade vs flat, see prior_eval.py)
priors built only from journals *before* it:
  last     the previous journal alone
  pooled   all earlier journals pooled (trades / option-minutes per bucket)
  fixed    --prior FILE, if given (e.g. the committed Oct 9 prior)
last and pooled are scored at each --shrink (option-minutes at the average rate
added to every bucket, see journal_stats.pooled_rate).

--shape rescales every prior to predict the test journal's own total, scoring only
where it puts trades, not how many. That is what the allocator uses: it ranks
options against each other, so a prior that is right about the shape but from a
busier day still steers the budget correctly.
Journals are taken in the order given; pass them sorted by time.
"""

import argparse
import math
import os

from journal_stats import pooled_rate, summarise
from prior_eval import load_prior, loglik, option_counts


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("journal", nargs="+")
    ap.add_argument("--prior", help="a fixed prior to score alongside")
    ap.add_argument("--shrink", type=float, nargs="+", default=[0.0])
    ap.add_argument("--shape", action="store_true", help="match each prior's level to the test journal")
    ap.add_argument("--floor", type=float, default=1e-4)
    args = ap.parse_args()
    if len(args.journal) < 2:
        raise SystemExit("need at least two journals")

    fixed = load_prior(args.prior) if args.prior else None
    stats = [summarise(p, verbose=False) for p in args.journal]
    tag = lambda a: f"/{a:g}" if len(args.shrink) > 1 or a else ""
    names = [f"{n}{tag(a)}" for n in ("last", "pooled") for a in args.shrink] + (["fixed"] if fixed else [])
    width = max(9, max(map(len, names)) + 2)
    totals = {n: 0.0 for n in names}
    total_trades = 0
    print(f"{'test journal':<34} {'min':>5} {'trades':>6} " + "".join(f"{n:>{width}}" for n in names))
    for t in range(1, len(args.journal)):
        minutes, rows = option_counts(args.journal[t])
        trades = sum(k for *_, k in rows)
        if trades == 0:
            print(f"{os.path.basename(args.journal[t]):<34} {minutes:5.0f} {0:6d}   (no trades, skipped)")
            continue
        exposure = sum(m for _, m, _ in rows)
        flat = max(trades / exposure, args.floor)
        base = loglik(rows, lambda key: flat)
        priors = {}
        for a in args.shrink:
            priors[f"last{tag(a)}"] = pooled_rate(stats[t - 1:t], a)[0]
            priors[f"pooled{tag(a)}"] = pooled_rate(stats[:t], a)[0]
        if fixed:
            priors["fixed"] = fixed
        cells = []
        for n in names:
            r = lambda key, p=priors[n]: max(p[key[0]][key[1]], args.floor)
            scale = trades / sum(r(key) * m for key, m, _ in rows) if args.shape else 1.0
            gain = loglik(rows, lambda key: r(key) * scale) - base
            totals[n] += gain
            cells.append(gain / trades / math.log(2))
        total_trades += trades
        print(f"{os.path.basename(args.journal[t]):<34} {minutes:5.0f} {trades:6d} "
              + "".join(f"{c:+{width}.3f}" for c in cells))
    if total_trades:
        print(f"{'all (trade-weighted)':<34} {'':>5} {total_trades:6d} "
              + "".join(f"{totals[n] / total_trades / math.log(2):+{width}.3f}" for n in names))
    print("\nbits per trade vs a flat rate taken from each test journal itself; positive = better than flat"
          + ("\n(--shape: each prior rescaled to the test journal's total, so only the shape is scored)"
             if args.shape else ""))


if __name__ == "__main__":
    main()
