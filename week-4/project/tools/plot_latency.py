#!/usr/bin/env python3
"""
plot_latency.py — OPTIONAL: plot pipeline throughput vs. a knob you sweep.

The most instructive Week-4 experiment is to take the SAME correct pipeline and
sweep one variable, then watch the curve. Useful sweeps:

  * single-threaded baseline vs. two-thread pipeline (the overlap win, 01-)
  * ring-buffer capacity (too small => the producer stalls on a full queue;
    too big => cache-cold slots) (04-spsc-ring-buffer.md)
  * mutex queue vs. lock-free SPSC (the cost of a lock, 01-/04-)
  * pinned vs. unpinned threads (run-to-run variance, 05-pipeline-...)

This script does NOT run your binary or know your knobs. You produce a CSV (one
row per configuration) and point it here. That keeps the measurement your job
and the plotting boring.

CSV format (header required):

    config,throughput_mtps
    serial,180.0
    mutex,150.4
    spsc-cap1024,330.2
    spsc-cap65536-pinned,355.9

Usage:
    python3 tools/plot_latency.py data/latency.csv --out latency.png

Dependencies: matplotlib (optional: `pip install matplotlib`). Without it the
script prints the parsed table instead of plotting.
"""

from __future__ import annotations

import argparse
import csv
import sys
from pathlib import Path


def read_csv(path: Path) -> tuple[list[str], list[float]]:
    configs: list[str] = []
    tput: list[float] = []
    with path.open(newline="") as f:
        reader = csv.DictReader(f)
        if reader.fieldnames is None or "config" not in reader.fieldnames \
                or "throughput_mtps" not in reader.fieldnames:
            sys.exit("CSV must have a header: config,throughput_mtps")
        for row in reader:
            configs.append(str(row["config"]))
            tput.append(float(row["throughput_mtps"]))
    return configs, tput


def main() -> None:
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("csv", type=Path, help="a configuration CSV file")
    p.add_argument("--out", type=Path, default=Path("latency.png"),
                   help="output image path")
    args = p.parse_args()

    configs, tput = read_csv(args.csv)
    base = tput[0] if tput else 1.0

    try:
        import matplotlib.pyplot as plt
    except ImportError:
        print("matplotlib not installed; printing parsed table instead.\n")
        print(f"{'config':>28}  {'M ticks/s':>12}  {'vs first':>9}")
        for c, v in zip(configs, tput):
            print(f"{c:>28}  {v:>12.2f}  {v / base:>8.2f}x")
        return

    fig, ax = plt.subplots(figsize=(max(6.0, 1.1 * len(configs)), 4.5))
    ax.bar(range(len(configs)), tput)
    ax.set_xticks(range(len(configs)))
    ax.set_xticklabels(configs, rotation=30, ha="right")
    ax.set_ylabel("throughput (M ticks/s)")
    ax.set_title("Pipeline throughput by configuration")
    ax.grid(True, axis="y", alpha=0.3)
    fig.tight_layout()
    fig.savefig(args.out, dpi=120)
    print(f"wrote {args.out}")


if __name__ == "__main__":
    main()
