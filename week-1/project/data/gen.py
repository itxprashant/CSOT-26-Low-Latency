#!/usr/bin/env python3
"""
gen.py — synthetic tick generator for the CSoT'26 Low Latency quant platform.

Produces a CSV with the FROZEN Week-1 schema:

    timestamp_ns,symbol,bid_px,ask_px,bid_qty,ask_qty

Properties guaranteed by this generator:
  * Strictly ascending timestamps (one tick every ~`dt_ns` nanoseconds + jitter).
  * Prices follow geometric Brownian motion (per-symbol, independent).
  * Spread = ask - bid is always >= 0.01.
  * Sizes are positive integers.
  * Seeded — running with the same args produces identical output across
    machines. Use this when comparing benchmark numbers with classmates.

Usage:
    python3 gen.py --rows 10000   --out synthetic_small.csv
    python3 gen.py --rows 10000000 --out synthetic_large.csv

Dependencies: only the Python 3 standard library (no numpy required).
"""

from __future__ import annotations

import argparse
import csv
import math
import random
import sys
from pathlib import Path

# --- Defaults (do not change for the cohort baseline) -----------------------
DEFAULT_SYMBOLS  = ["SYM0", "SYM1", "SYM2", "SYM3"]
DEFAULT_START_PX = [100.00, 250.00,  50.00, 1000.00]
DEFAULT_SEED     = 42
DEFAULT_DT_NS    = 1_000_000       # 1 ms between ticks (on average)
DEFAULT_SIGMA    = 0.0002          # per-tick stddev of log-return
DEFAULT_MIN_SPREAD = 0.01
DEFAULT_BASE_QTY = 500
# ----------------------------------------------------------------------------


def generate(
    rows: int,
    out_path: Path,
    *,
    symbols: list[str]   = DEFAULT_SYMBOLS,
    start_px: list[float] = DEFAULT_START_PX,
    seed: int            = DEFAULT_SEED,
    dt_ns: int           = DEFAULT_DT_NS,
    sigma: float         = DEFAULT_SIGMA,
    min_spread: float    = DEFAULT_MIN_SPREAD,
    base_qty: int        = DEFAULT_BASE_QTY,
    start_ns: int        = 1_700_000_000_000_000_000,  # ~2023-11-14T22:13:20Z
) -> None:
    if len(symbols) != len(start_px):
        sys.exit("symbols and start_px must have the same length")

    rng     = random.Random(seed)
    n_sym   = len(symbols)
    mids    = list(start_px)
    ts      = start_ns

    with out_path.open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["timestamp_ns", "symbol", "bid_px",
                    "ask_px", "bid_qty", "ask_qty"])

        for i in range(rows):
            idx = i % n_sym                   # round-robin across symbols
            sym = symbols[idx]

            # Geometric Brownian motion step on the mid-price.
            mids[idx] *= math.exp(rng.gauss(0.0, sigma))

            half_spread = max(min_spread / 2.0, mids[idx] * 0.00005)
            bid = round(mids[idx] - half_spread, 4)
            ask = round(mids[idx] + half_spread, 4)

            # Sizes: log-normal-ish around base_qty, always >= 1.
            bid_qty = max(1, int(base_qty * math.exp(rng.gauss(0.0, 0.3))))
            ask_qty = max(1, int(base_qty * math.exp(rng.gauss(0.0, 0.3))))

            w.writerow([ts, sym, f"{bid:.4f}", f"{ask:.4f}",
                        bid_qty, ask_qty])

            # Advance timestamp with small jitter so ticks are strictly monotonic.
            ts += dt_ns + rng.randint(1, max(1, dt_ns // 10))


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--rows", type=int, default=10_000,
                   help="number of ticks to generate (default: 10_000)")
    p.add_argument("--out",  type=Path, default=Path("synthetic_small.csv"),
                   help="output CSV path")
    p.add_argument("--seed", type=int, default=DEFAULT_SEED,
                   help=f"PRNG seed for reproducibility (default: {DEFAULT_SEED})")
    args = p.parse_args()

    generate(args.rows, args.out, seed=args.seed)
    print(f"wrote {args.rows} ticks to {args.out} (seed={args.seed})")


if __name__ == "__main__":
    main()
