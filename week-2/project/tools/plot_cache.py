#!/usr/bin/env python3
"""
plot_cache.py — OPTIONAL helper to visualise cache-sim stats.

Reads one or more stats JSON files (the exact shape printed by the local
harness / stored in data/tiny.stats.json) and renders L1/L2 hit-rate and
writeback bars so you can eyeball how a trace stresses the hierarchy.

This is a convenience tool, not part of the contest. It needs matplotlib:
    python3 -m pip install --user matplotlib

Usage:
    ./cache_sim_runner data/large.trace 2>/dev/null > /tmp/large.stats.json
    python3 tools/plot_cache.py /tmp/large.stats.json data/tiny.stats.json
"""

from __future__ import annotations

import json
import sys
from pathlib import Path


def hit_rates(s: dict) -> dict:
    accesses = s["reads"] + s["writes"]
    l1 = s["l1_hits"] / accesses if accesses else 0.0
    l2 = s["l2_hits"] / s["l1_misses"] if s["l1_misses"] else 0.0
    return {"L1 hit rate": l1, "L2 hit rate (of L1 misses)": l2}


def main() -> None:
    if len(sys.argv) < 2:
        sys.exit("usage: plot_cache.py <stats.json> [more.json ...]")

    try:
        import matplotlib.pyplot as plt
    except ImportError:
        sys.exit("matplotlib not installed: python3 -m pip install --user matplotlib")

    paths = [Path(p) for p in sys.argv[1:]]
    stats = [json.loads(p.read_text()) for p in paths]

    fig, ax = plt.subplots(figsize=(8, 4.5))
    labels = ["L1 hit rate", "L2 hit rate (of L1 misses)"]
    width = 0.8 / len(stats)
    for i, (p, s) in enumerate(zip(paths, stats)):
        r = hit_rates(s)
        xs = [j + i * width for j in range(len(labels))]
        ax.bar(xs, [r[l] for l in labels], width=width, label=p.name)

    ax.set_xticks([j + width * (len(stats) - 1) / 2 for j in range(len(labels))])
    ax.set_xticklabels(labels)
    ax.set_ylim(0, 1)
    ax.set_ylabel("rate")
    ax.set_title("Cache hit rates")
    ax.legend()
    fig.tight_layout()
    out = Path("cache_hit_rates.png")
    fig.savefig(out, dpi=120)
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
