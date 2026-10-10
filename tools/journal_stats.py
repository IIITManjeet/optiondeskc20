"""Summarise a market-data journal written by od_record.

Where do option trades happen (expiry, moneyness, call/put), how big are they,
and which side initiates them? Used to decide what a market maker should quote.

    python3 tools/journal_stats.py ~/btc_session.odj

Reads the binary layout from include/od/journal.hpp and market_data.hpp directly
(standard library only).
"""

import math
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


def main(path: str) -> None:
    data = open(path, "rb").read()
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
        elif rtype == 2 and size == TRADE.size:
            f = TRADE.unpack_from(data, off)
            trades.append((f[0], bool(f[1]), f[6], f[7], forward.get(f[0])))
        off += size

    minutes = (last - first) / 60e9 if first is not None else 0.0
    currency = ccy.split(b"\0", 1)[0].decode()
    print(f"{path}: {currency}, {n_inst} instruments, "
          f"{minutes:.1f} min, {n_tick} tickers, {len(trades)} trades")
    if not trades:
        return

    label = lambda i: names[i].split("-")[1]
    by_expiry = Counter(label(i) for i, *_ in trades)
    size_by_expiry = defaultdict(float)
    buckets = Counter()
    taker_buy = 0
    otm = 0
    for i, buy, price, amount, fwd in trades:
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
    amounts = sorted(a for _, _, _, a, _ in trades)
    print(f"\nsize: median {amounts[len(amounts) // 2]:.1f}, "
          f"90th pct {amounts[int(len(amounts) * 0.9)]:.1f}, max {amounts[-1]:.1f} contracts")
    print(f"aggressor: {taker_buy} buys / {len(trades) - taker_buy} sells; "
          f"{otm} of {sum(buckets.values())} in OTM options")


if __name__ == "__main__":
    main(sys.argv[1])
