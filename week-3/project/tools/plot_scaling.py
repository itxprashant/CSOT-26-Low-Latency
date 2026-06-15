#!/usr/bin/env python3
"""
plot_scaling.py — OPTIONAL: plot aggregator throughput vs. thread count.

The single most instructive Week-3 experiment is to take the SAME correct
aggregator, run it at 1, 2, 3, 4, ... threads, and watch the curve. You want to
*see* three things from the topic files:

  * the linear-ish climb of an embarrassingly-parallel reduction (01-going-wide)
  * the ceiling where memory bandwidth or Amdahl's serial merge caps you
  * the cliff/regression a false-sharing implementation shows instead of a climb
    (04-false-sharing) — re-run with your padded vs. unpadded partials.

This script does NOT run your binary. You produce a CSV (one row per thread
count) and point it here. That keeps the measurement your job and the plotting
boring.

CSV format (header required):

    threads,throughput_mtps
    1,420.5
    2,790.1
    3,1100.7
    4,1380.2

Usage:
    python3 tools/plot_scaling.py data/scaling.csv --out scaling.png
    python3 tools/plot_scaling.py data/scaling_padded.csv data/scaling_unpadded.csv \
        --labels padded unpadded --out scaling.png

Dependencies: matplotlib (optional install: `pip install matplotlib`). If it is
not installed the script prints the parsed table instead of plotting.
"""

from __future__ import annotations

import argparse
import csv
import sys
from pathlib import Path


def read_csv(path: Path) -> tuple[list[float], list[float]]:
    threads: list[float] = []
    tput: list[float] = []
    with path.open(newline="") as f:
        reader = csv.DictReader(f)
        if reader.fieldnames is None or "threads" not in reader.fieldnames \
                or "throughput_mtps" not in reader.fieldnames:
            sys.exit("CSV must have a header: threads,throughput_mtps")
        for row in reader:
            threads.append(float(row["threads"]))
            tput.append(float(row["throughput_mtps"]))
    return threads, tput


def main() -> None:
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("csv", type=Path, nargs="+", help="one or more scaling CSV files")
    p.add_argument("--labels", nargs="*", default=None,
                   help="legend label per CSV (defaults to filename)")
    p.add_argument("--out", type=Path, default=Path("scaling.png"),
                   help="output image path")
    args = p.parse_args()

    series = [read_csv(c) for c in args.csv]
    labels = args.labels or [c.stem for c in args.csv]

    try:
        import matplotlib.pyplot as plt
    except ImportError:
        print("matplotlib not installed; printing parsed tables instead.\n")
        for label, (threads, tput) in zip(labels, series):
            print(f"# {label}")
            print(f"{'threads':>8}  {'M ticks/s':>12}  {'speedup':>8}")
            base = tput[0] if tput else 1.0
            for t, v in zip(threads, tput):
                print(f"{t:>8.0f}  {v:>12.2f}  {v / base:>7.2f}x")
            print()
        return

    fig, (ax_t, ax_s) = plt.subplots(1, 2, figsize=(11, 4.5))
    for label, (threads, tput) in zip(labels, series):
        ax_t.plot(threads, tput, marker="o", label=label)
        base = tput[0] if tput else 1.0
        ax_s.plot(threads, [v / base for v in tput], marker="o", label=label)

    # Ideal linear speedup reference on the speedup panel.
    if series:
        max_threads = max(max(s[0]) for s in series)
        ideal = list(range(1, int(max_threads) + 1))
        ax_s.plot(ideal, ideal, linestyle="--", label="ideal linear")

    ax_t.set_xlabel("threads")
    ax_t.set_ylabel("throughput (M ticks/s)")
    ax_t.set_title("Aggregator throughput")
    ax_t.legend()
    ax_t.grid(True, alpha=0.3)

    ax_s.set_xlabel("threads")
    ax_s.set_ylabel("speedup vs. 1 thread")
    ax_s.set_title("Parallel speedup (mind Amdahl)")
    ax_s.legend()
    ax_s.grid(True, alpha=0.3)

    fig.tight_layout()
    fig.savefig(args.out, dpi=120)
    print(f"wrote {args.out}")


if __name__ == "__main__":
    main()
