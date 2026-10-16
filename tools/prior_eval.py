"""Score flow priors on journals they were not built from.

    python3 tools/prior_eval.py --prior a.json --prior b.json ~/rec/test_hour.odj

For every option live in a test journal, a prior predicts how many trades it
should see: rate(days, |ln K/F|) x minutes. The score is the Poisson
log-likelihood of the trade counts that actually happened, reported in bits per
trade against a flat baseline that gives every option the test journal's own
average rate. Positive = the prior knew where trades would land better than
"everything is equally likely"; the flat baseline uses the test data itself, so
beating it is not automatic.

Rates are floored at --floor (trades/min per option, default 1e-4) so a prior
that says "never" about a bucket that then trades isn't scored as infinitely
wrong. The market maker's own floor (fill_rate_floor_per_min, 0.005) is far
higher, but it only applies to the handful of options it quotes; over all ~900
live options it would swamp every rate here.
"""

import argparse
import json
import math
from collections import Counter

from journal_stats import DAYS_EDGES, MONEYNESS_EDGES, bucket, summarise


def load_prior(path):
    j = json.load(open(path))
    if j["days_edges"][:-1] != DAYS_EDGES[:-1] or j["moneyness_edges"][:-1] != MONEYNESS_EDGES[:-1]:
        raise SystemExit(f"{path}: bucket edges differ from journal_stats.py")
    return j["rate_per_min"]


def option_counts(path):
    """[(expected-rate lookup key, minutes, trades)] for every live option."""
    minutes, first_ms, expiries, strikes, forward, trades = summarise(path, verbose=False)
    if not first_ms or minutes <= 0:
        return minutes, []
    mid_ms = first_ms + minutes * 30_000
    n = Counter(i for i, *_ in trades)
    rows = []
    for i, fwd in forward.items():
        days = (expiries[i] - mid_ms) / 86_400_000
        if days <= 0:
            continue
        key = (bucket(DAYS_EDGES, days), bucket(MONEYNESS_EDGES, abs(math.log(strikes[i] / fwd))))
        rows.append((key, minutes, n.get(i, 0)))
    return minutes, rows


def loglik(rows, rate_of):
    return sum(k * math.log(lam) - lam - math.lgamma(k + 1)
               for key, m, k in rows for lam in [rate_of(key) * m])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("journal", nargs="+", help="test journals")
    ap.add_argument("--prior", action="append", required=True)
    ap.add_argument("--floor", type=float, default=1e-4)
    args = ap.parse_args()

    priors = [(p, load_prior(p)) for p in args.prior]
    rows, minutes = [], 0.0
    for path in args.journal:
        m, r = option_counts(path)
        minutes += m
        rows += r
    trades = sum(k for *_, k in rows)
    exposure = sum(m for _, m, _ in rows)
    print(f"test: {len(args.journal)} journal(s), {minutes:.1f} min, {trades} trades in "
          f"{len({key for key, *_ in rows})} buckets")
    if trades == 0:
        raise SystemExit("no trades to score")

    flat_rate = max(trades / exposure, args.floor)
    base = loglik(rows, lambda key: flat_rate)
    print(f"\n{'prior':<40} {'bits/trade vs flat':>19} {'predicted trades':>17}")
    for path, rate in priors:
        f = lambda key: max(rate[key[0]][key[1]], args.floor)
        ll = loglik(rows, f)
        predicted = sum(f(key) * m for key, m, _ in rows)
        print(f"{path:<40} {(ll - base) / trades / math.log(2):+19.3f} {predicted:17.1f}")
    print(f"{'(flat, from the test data itself)':<40} {0:+19.3f} {flat_rate * exposure:17.1f}")


if __name__ == "__main__":
    main()
