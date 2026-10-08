"""Plot the smiles written by `od_surface --csv FILE`.

One panel per expiry: market bid/ask IV as gray ranges, the fitted SVI curve in
blue. Quotes left out of the fit (ITM, or spread too wide) are drawn fainter.

    python tools/plot_surface.py out/btc_surface.csv -o out/btc_surface.png
"""

import argparse
import math

import matplotlib.pyplot as plt
import pandas as pd

SURFACE = "#fcfcfb"
INK = "#0b0b0b"
INK_2 = "#52514e"
GRID = "#e4e3df"
MARKET = "#8a8984"
SVI = "#2a78d6"


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("csv")
    ap.add_argument("-o", "--out", default=None)
    ap.add_argument("--kmax", type=float, default=0.6, help="max |log-moneyness| shown")
    args = ap.parse_args()

    df = pd.read_csv(args.csv)
    df = df[(df.mid_iv > 0) & (df.k.abs() <= args.kmax)]
    expiries = list(dict.fromkeys(df.expiry))  # keeps file (expiry) order

    cols = 4
    rows = math.ceil(len(expiries) / cols)
    fig, axes = plt.subplots(rows, cols, figsize=(4 * cols, 3 * rows), squeeze=False)
    fig.patch.set_facecolor(SURFACE)

    for ax, exp in zip(axes.flat, expiries):
        d = df[df.expiry == exp].sort_values("k")
        ax.set_facecolor(SURFACE)
        for in_fit, alpha in ((False, 0.3), (True, 1.0)):
            q = d[d.in_fit == int(in_fit)]
            ax.vlines(q.k, 100 * q.bid_iv, 100 * q.ask_iv, color=MARKET, lw=2, alpha=alpha)
        ax.plot(d.k, 100 * d.svi_iv, color=SVI, lw=2)

        days = d["T"].iloc[0] * 365
        ax.set_title(f"{exp}  ({days:.1f}d)", color=INK, fontsize=10, loc="left")
        ax.grid(color=GRID, lw=0.6)
        ax.tick_params(colors=INK_2, labelsize=8)
        for s in ax.spines.values():
            s.set_visible(False)

    for ax in axes.flat[len(expiries):]:
        ax.set_visible(False)
    for ax in axes[-1]:
        ax.set_xlabel("log-moneyness  ln(K/F)", color=INK_2, fontsize=8)
    for ax in axes[:, 0]:
        ax.set_ylabel("implied vol (%)", color=INK_2, fontsize=8)

    handles = [
        plt.Line2D([], [], color=MARKET, lw=2, label="market bid/ask IV"),
        plt.Line2D([], [], color=MARKET, lw=2, alpha=0.3, label="not used in fit"),
        plt.Line2D([], [], color=SVI, lw=2, label="SVI fit"),
    ]
    fig.legend(handles=handles, loc="upper right", frameon=False, fontsize=9, labelcolor=INK)
    fig.suptitle("Implied volatility smiles by expiry", x=0.01, ha="left", color=INK)
    fig.tight_layout(rect=(0, 0, 1, 0.95))

    if args.out:
        fig.savefig(args.out, dpi=130, facecolor=SURFACE)
        print(f"wrote {args.out}")
    else:
        plt.show()


if __name__ == "__main__":
    main()
