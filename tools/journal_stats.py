"""Summarise a market-data journal written by od_record.

Where do option trades happen (expiry, moneyness, call/put), how big are they,
and which side initiates them? Used to decide what a market maker should quote.

    python3 tools/journal_stats.py ~/btc_session.odj
    python3 tools/journal_stats.py ~/btc_session.odj --profile flow_prior.json
    python3 tools/journal_stats.py ~/rec/*.odj --profile flow_prior.json

--profile writes a flow prior for the market maker's request allocator: trades
per minute *per option*, in buckets of days to expiry x distance from the forward
(|ln(K/F)|). Per option, because a bucket with many strikes isn't individually
busier than one with few. Given several journals, each bucket's rate is pooled:
all its trades over all its option-minutes, so longer sessions count for more.
--shrink N pulls each bucket toward the average rate with N option-minutes of
pseudo-observations; out of sample, ~1000 beat the raw rates (README).

Reads the binary layout from include/od/journal.hpp and market_data.hpp directly
(standard library only).
"""

import argparse
import json
import math
import mmap
import os
import struct
import sys
from collections import Counter, defaultdict

FILE_HEADER = struct.Struct("<8sII16sq")
INSTRUMENT = struct.Struct("<48sqdQ")
RECORD = struct.Struct("<HHIq")
# TickerUpdate: instrument, (pad), exch_ts, recv_ns, parsed_ns, 8 doubles
TICKER = struct.Struct("<I4xqqq8d")
# TradeUpdate: instrument, taker_buy, exch_ts, recv_ns, parsed_ns, trade_seq, 4 doubles
TRADE = struct.Struct("<IIqqqQ4d")


DAYS_EDGES = [0, 1, 2, 4, 8, 16, 32, 64, 1e9]
MONEYNESS_EDGES = [0, 0.02, 0.05, 0.10, 0.20, 1e9]


def bucket(edges, x):
    for i in range(len(edges) - 1):
        if edges[i] <= x < edges[i + 1]:
            return i
    return len(edges) - 2


def cached_summary(path: str):
    """summarise(path, verbose=False), cached in PATH.stats.json.

    The cache is keyed on the journal's size and mtime, so a journal still being
    recorded is re-read each time and a finished one is parsed once. Parsing an
    hour (~400 MB) takes ~20 s; loading the cache takes milliseconds.
    """
    st = os.stat(path)
    key = [st.st_size, st.st_mtime_ns]
    cache = path + ".stats.json"
    try:
        with open(cache) as f:
            c = json.load(f)
        if c["key"] == key:
            return (c["minutes"], c["first_ms"], c["expiries"], c["strikes"],
                    {int(i): v for i, v in c["forward"].items()}, [tuple(t) for t in c["trades"]])
    except (OSError, ValueError, KeyError):
        pass
    minutes, first_ms, expiries, strikes, forward, trades = summarise(path, verbose=False)
    try:
        with open(cache, "w") as f:
            json.dump({"key": key, "minutes": minutes, "first_ms": first_ms, "expiries": expiries,
                       "strikes": strikes, "forward": forward, "trades": trades}, f)
    except OSError:
        pass  # read-only location: just don't cache
    return minutes, first_ms, expiries, strikes, forward, trades


def summarise(path: str, verbose: bool = True):
    # mmap rather than read(): the pages are file-backed, so a 400 MB journal doesn't
    # need 400 MB of private memory.
    with open(path, "rb") as f:
        data = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
    magic, version, n_inst, ccy, start_ms = FILE_HEADER.unpack_from(data, 0)
    if magic != b"ODJRNL01":
        sys.exit(f"{path}: not a journal")
    off = FILE_HEADER.size
    names, strikes, expiries, puts = [], [], [], []
    for _ in range(n_inst):
        name, expiry, strike, is_put = INSTRUMENT.unpack_from(data, off)
        names.append(name.split(b"\0", 1)[0].decode())
        expiries.append(expiry)
        strikes.append(strike)
        puts.append(bool(is_put))
        off += INSTRUMENT.size

    forward = {}  # latest underlying per instrument, from tickers
    n_tick = 0
    first = last = None
    first_exch_ms = None
    trades = []
    while off + RECORD.size <= len(data):
        rtype, size, _, ts = RECORD.unpack_from(data, off)
        off += RECORD.size
        if off + size > len(data):
            break
        first = ts if first is None else first
        last = ts
        if rtype == 1 and size == TICKER.size:
            f = TICKER.unpack_from(data, off)
            n_tick += 1
            if f[10] > 0:
                forward[f[0]] = f[10]  # underlying
            if first_exch_ms is None and f[1] > 0:
                first_exch_ms = f[1]
        elif rtype == 2 and size == TRADE.size:
            f = TRADE.unpack_from(data, off)
            trades.append((f[0], bool(f[1]), f[6], f[7], forward.get(f[0]), f[2]))
        off += size

    minutes = (last - first) / 60e9 if first is not None else 0.0
    if not verbose:
        return minutes, first_exch_ms, expiries, strikes, forward, trades
    currency = ccy.split(b"\0", 1)[0].decode()
    print(f"{path}: {currency}, {n_inst} instruments, "
          f"{minutes:.1f} min, {n_tick} tickers, {len(trades)} trades")
    if not trades:
        return minutes, first_exch_ms, expiries, strikes, forward, trades

    label = lambda i: names[i].split("-")[1]
    by_expiry = Counter(label(i) for i, *_ in trades)
    size_by_expiry = defaultdict(float)
    buckets = Counter()
    taker_buy = 0
    otm = 0
    for i, buy, price, amount, fwd, _ in trades:
        size_by_expiry[label(i)] += amount
        taker_buy += buy
        if fwd:
            k = math.log(strikes[i] / fwd)
            b = min(int(abs(k) / 0.05), 6)
            buckets[f"{b * 5:>2}-{b * 5 + 5}%" if b < 6 else "30%+ "] += 1
            otm += (k < 0) == puts[i]

    print("\ntrades by expiry:")
    for exp, n in sorted(by_expiry.items(), key=lambda x: -x[1]):
        print(f"  {exp:<9} {n:5d} trades  {size_by_expiry[exp]:8.1f} contracts")
    print("\ntrades by |log-moneyness| (distance of strike from forward):")
    for b, n in sorted(buckets.items()):
        print(f"  {b:<7} {n:5d}")
    amounts = sorted(a for _, _, _, a, _, _ in trades)
    print(f"\nsize: median {amounts[len(amounts) // 2]:.1f}, "
          f"90th pct {amounts[int(len(amounts) * 0.9)]:.1f}, max {amounts[-1]:.1f} contracts")
    print(f"aggressor: {taker_buy} buys / {len(trades) - taker_buy} sells; "
          f"{otm} of {sum(buckets.values())} in OTM options")
    return minutes, first_exch_ms, expiries, strikes, forward, trades


def bucket_counts(minutes, first_ms, expiries, strikes, forward, trades):
    """Option-minutes and trades per (days, moneyness) bucket for one journal.

    Days to expiry is taken at the middle of the recording for both, so an option
    near a bucket edge puts its trades and its exposure in the same bucket.
    """
    nd, nk = len(DAYS_EDGES) - 1, len(MONEYNESS_EDGES) - 1
    exposure = [[0.0] * nk for _ in range(nd)]
    counts = [[0] * nk for _ in range(nd)]
    if not first_ms:
        return exposure, counts
    mid_ms = first_ms + minutes * 30_000
    days = lambda i: (expiries[i] - mid_ms) / 86_400_000
    for i, fwd in forward.items():  # options that were live and priced during the recording
        if days(i) <= 0:
            continue
        exposure[bucket(DAYS_EDGES, days(i))][bucket(MONEYNESS_EDGES, abs(math.log(strikes[i] / fwd)))] += minutes
    for i, _, _, _, fwd, _ in trades:
        if not fwd:
            continue
        counts[bucket(DAYS_EDGES, max(days(i), 0))][bucket(MONEYNESS_EDGES, abs(math.log(strikes[i] / fwd)))] += 1
    return exposure, counts


def pooled_rate(stats, shrink=0.0):
    """Trades per minute per option by bucket, pooled over journals (summarise() tuples).

    shrink adds that many option-minutes to every bucket at the pooled average rate,
    pulling thinly observed buckets toward the average. A raw single-session rate is
    overconfident: a bucket that happened not to trade gets ~0 and one lucky bucket
    gets a large multiple, and flow moves between days (see prior_walk_forward.py).
    """
    nd, nk = len(DAYS_EDGES) - 1, len(MONEYNESS_EDGES) - 1
    exposure = [[0.0] * nk for _ in range(nd)]
    counts = [[0] * nk for _ in range(nd)]
    for s in stats:
        e, c = bucket_counts(*s)
        for d in range(nd):
            for k in range(nk):
                exposure[d][k] += e[d][k]
                counts[d][k] += c[d][k]
    total = sum(map(sum, exposure))
    avg = sum(map(sum, counts)) / total if total > 0 else 0.0
    rate = [[(counts[d][k] + shrink * avg) / (exposure[d][k] + shrink) if exposure[d][k] + shrink > 0 else 0.0
             for k in range(nk)] for d in range(nd)]
    return rate, exposure, counts


def write_profile(out, sessions, shrink=0.0):
    nd, nk = len(DAYS_EDGES) - 1, len(MONEYNESS_EDGES) - 1
    rate, exposure, counts = pooled_rate([s[1:] for s in sessions], shrink)
    json.dump({
        "source": [path for path, *_ in sessions],
        "minutes": round(sum(m for _, m, *_ in sessions), 2),
        "trades": sum(len(s[-1]) for s in sessions),
        "days_edges": DAYS_EDGES[:-1] + [None],
        "moneyness_edges": MONEYNESS_EDGES[:-1] + [None],
        "option_minutes": [[round(x, 1) for x in row] for row in exposure],
        "trades_by_bucket": counts,
        "shrink_option_minutes": shrink,
        "rate_per_min": rate,
    }, open(out, "w"), indent=1)
    print(f"\nflow prior written to {out} (trades per minute per option):")
    print("  days x |ln K/F| " + "".join(f"{f'<{e:g}' if e < 1e8 else 'rest':>9}" for e in MONEYNESS_EDGES[1:]))
    for d in range(nd):
        lo, hi = DAYS_EDGES[d], DAYS_EDGES[d + 1]
        label = f"{lo:g}-{hi:g}" if hi < 1e8 else f"{lo:g}+"
        print(f"  {label:>8}        " + "".join(f"{rate[d][k]:9.4f}" for k in range(nk)))


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("journal", nargs="+")
    ap.add_argument("--profile", help="write a flow prior JSON for the market maker")
    ap.add_argument("--shrink", type=float, default=0.0,
                    help="option-minutes at the average rate added to every bucket (e.g. 1000)")
    args = ap.parse_args()
    sessions = []
    for i, path in enumerate(args.journal):
        if i:
            print()
        sessions.append((path, *summarise(path)))
    if args.profile:
        write_profile(args.profile, sessions, args.shrink)
