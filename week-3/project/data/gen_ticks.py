#!/usr/bin/env python3
"""
gen_ticks.py — synthetic tick-stream generator for the CSoT'26 Week-3 aggregator.

Produces a binary stream in the FROZEN format from AGG_SPEC.md §2: a contiguous
array of 32-byte csot::AggTick records, little-endian, no header:

    offset 0  : uint64  timestamp_ns   (non-decreasing)
    offset 8  : int64   price          (FIXED-POINT: real price * 10000)
    offset 16 : uint32  symbol_id       (0 .. NUM_SYMBOLS-1)
    offset 20 : uint32  qty             (> 0)
    offset 24 : uint64  _reserved        (always zero)

The judge mmaps this straight into a `const AggTick*`, so the on-disk layout IS
the struct.

Properties guaranteed:
  * Seeded — identical args + unchanged source => byte-identical stream across
    machines. Use this when comparing benchmark numbers with classmates.
  * Skewed symbol popularity (a few hot symbols, a long tail, and some symbols
    that never appear) so your per-symbol table is non-trivial and the
    empty-symbol rule (AGG_SPEC.md §3) actually bites.
  * --tiny emits a small, hand-checkable curated stream (see AGG_SPEC.md §5).
  * --stats reads a stream and prints the REFERENCE aggregate table as JSON.
    This is the clarity-first single-threaded reference of AGG_SPEC.md §6 — it
    is what data/tiny.agg.json is made of. It is NOT fast and will NOT win the
    leaderboard; the contest is to compute the same table in parallel, faster.

Usage:
    python3 gen_ticks.py --tiny --out tiny.ticks
    python3 gen_ticks.py --accesses 5000000 --seed 42 --out large.ticks
    python3 gen_ticks.py --dump tiny.ticks               # human-readable view
    python3 gen_ticks.py --stats tiny.ticks              # reference table JSON

Dependencies: only the Python 3 standard library.
"""

from __future__ import annotations

import argparse
import json
import random
import struct
import sys
from pathlib import Path

# uint64 ts, int64 price, uint32 symbol_id, uint32 qty, uint64 reserved => 32 bytes
REC = struct.Struct("<QqIIQ")
# uint64 count, int64 sum_price, int64 min_price, int64 max_price, uint64 sum_qty => 40 bytes
ROW = struct.Struct("<QqqqQ")

# --- Spec constants (must match AGG_SPEC.md §3) -----------------------------
NUM_SYMBOLS = 1024
PRICE_SCALE = 10000

# --- Defaults (do not change for the cohort baseline) -----------------------
DEFAULT_SEED = 42
DEFAULT_ACCESSES = 1_000_000

# FNV-1a 64-bit — the canonical table checksum (also computed by harness/main.cpp).
FNV_OFFSET = 0xCBF29CE484222325
FNV_PRIME = 0x00000100000001B3
MASK64 = 0xFFFFFFFFFFFFFFFF


def curated_tiny() -> list[tuple[int, int, int, int]]:
    """A 10-tick stream over symbols 0..3 (out of 1024), hand-checkable.

    Returns (timestamp_ns, price, symbol_id, qty) tuples in stream order.
    Order is intentionally interleaved to show that the aggregates do not
    depend on tick order (AGG_SPEC.md §7). Expected table (data/tiny.agg.json):

      sym 0: count 3  sum_price 3000000  min 999500   max 1000500  sum_qty 35
      sym 1: count 2  sum_price 1000000  min 500000   max 500000   sum_qty 101
      sym 2: count 1  sum_price 2500000  min 2500000  max 2500000  sum_qty 7
      sym 3: count 4  sum_price 800      min 100      max 300      sum_qty 10
      all other symbols: canonical zeros.
    """
    return [
        (1000, 1000000, 0, 10),
        (1100,     300, 3,  1),
        (1200,  500000, 1, 100),
        (1300, 1000500, 0, 20),
        (1400, 2500000, 2,  7),
        (1500,     100, 3,  2),
        (1600,  999500, 0,  5),
        (1700,     200, 3,  3),
        (1800,  500000, 1,  1),
        (1900,     200, 3,  4),
    ]


def generate(accesses: int, seed: int) -> list[tuple[int, int, int, int]]:
    """Skewed, reproducible synthetic stream (in-memory; use for tiny / small tests)."""
    rng = random.Random(seed)
    hot = list(range(0, 16))
    warm = list(range(16, 384))
    cur = [rng.randint(50_000, 5_000_000) for _ in range(384)]

    out: list[tuple[int, int, int, int]] = []
    ts = 1_000_000_000
    for _ in range(accesses):
        if rng.random() < 0.80:
            s = rng.choice(hot)
        else:
            s = rng.choice(warm)
        step = rng.randint(-500, 500)
        cur[s] = max(1, cur[s] + step)
        px = cur[s]
        qty = rng.randint(1, 1000)
        ts += rng.randint(1, 50)
        out.append((ts, px, s, qty))
    return out


# Records buffered before each write() — keeps RAM flat on 200M-tick streams.
_STREAM_CHUNK = 8192


def write_generated_stream(out_path: Path, accesses: int, seed: int) -> int:
    """Stream skewed ticks straight to disk without materialising the full stream."""
    rng = random.Random(seed)
    hot = list(range(0, 16))
    warm = list(range(16, 384))
    cur = [rng.randint(50_000, 5_000_000) for _ in range(384)]

    ts = 1_000_000_000
    buf = bytearray(_STREAM_CHUNK * REC.size)
    off = 0

    with out_path.open("wb") as f:
        for _ in range(accesses):
            if rng.random() < 0.80:
                s = rng.choice(hot)
            else:
                s = rng.choice(warm)
            step = rng.randint(-500, 500)
            cur[s] = max(1, cur[s] + step)
            px = cur[s]
            qty = rng.randint(1, 1000)
            ts += rng.randint(1, 50)

            REC.pack_into(buf, off, ts & MASK64, px, s & 0xFFFFFFFF, qty & 0xFFFFFFFF, 0)
            off += REC.size
            if off >= len(buf):
                f.write(buf)
                off = 0

        if off:
            f.write(buf[:off])

    return accesses


def write_stream(out_path: Path, ticks: list[tuple[int, int, int, int]]) -> None:
    buf = bytearray(len(ticks) * REC.size)
    off = 0
    for ts, px, sym, qty in ticks:
        REC.pack_into(buf, off, ts & MASK64, px, sym & 0xFFFFFFFF, qty & 0xFFFFFFFF, 0)
        off += REC.size
    out_path.write_bytes(buf)


def reference_table(path: Path) -> list[tuple[int, int, int, int, int]]:
    """The AGG_SPEC.md §6 clarity reference. Returns NUM_SYMBOLS rows of
    (count, sum_price, min_price, max_price, sum_qty)."""
    data = path.read_bytes()
    if len(data) % REC.size != 0:
        sys.exit(f"stream size {len(data)} is not a multiple of {REC.size}")
    n = len(data) // REC.size

    count = [0] * NUM_SYMBOLS
    sum_price = [0] * NUM_SYMBOLS
    min_price = [0] * NUM_SYMBOLS
    max_price = [0] * NUM_SYMBOLS
    sum_qty = [0] * NUM_SYMBOLS

    for i in range(n):
        ts, px, sym, qty, _res = REC.unpack_from(data, i * REC.size)
        if count[sym] == 0:
            min_price[sym] = px
            max_price[sym] = px
        else:
            if px < min_price[sym]:
                min_price[sym] = px
            if px > max_price[sym]:
                max_price[sym] = px
        count[sym] += 1
        sum_price[sym] += px
        sum_qty[sym] += qty

    return [
        (count[s], sum_price[s], min_price[s], max_price[s], sum_qty[s])
        for s in range(NUM_SYMBOLS)
    ]


def table_checksum(table: list[tuple[int, int, int, int, int]]) -> int:
    """FNV-1a 64 over the canonical SymbolAgg byte layout (all NUM_SYMBOLS rows).
    Identical to the checksum harness/main.cpp computes over the result array."""
    h = FNV_OFFSET
    for count, sp, mn, mx, sq in table:
        for byte in ROW.pack(count & MASK64, sp, mn, mx, sq & MASK64):
            h = ((h ^ byte) * FNV_PRIME) & MASK64
    return h


def stats(path: Path) -> None:
    table = reference_table(path)
    n = sum(row[0] for row in table)
    nonempty = [
        {
            "symbol": s,
            "count": table[s][0],
            "sum_price": table[s][1],
            "min_price": table[s][2],
            "max_price": table[s][3],
            "sum_qty": table[s][4],
        }
        for s in range(NUM_SYMBOLS)
        if table[s][0] > 0
    ]
    out = {
        "num_symbols": NUM_SYMBOLS,
        "n": n,
        "checksum": table_checksum(table),
        "nonempty": nonempty,
    }
    print(json.dumps(out, indent=2))


def dump(path: Path, limit: int) -> None:
    data = path.read_bytes()
    if len(data) % REC.size != 0:
        sys.exit(f"stream size {len(data)} is not a multiple of {REC.size}")
    n = len(data) // REC.size
    print(f"# {path}: {n} ticks")
    print(f"# {'idx':>6}  {'timestamp_ns':>14}  {'price':>12}  {'sym':>5}  {'qty':>6}")
    shown = n if limit <= 0 else min(limit, n)
    for i in range(shown):
        ts, px, sym, qty, _res = REC.unpack_from(data, i * REC.size)
        print(f"  {i:>6}  {ts:>14}  {px:>12}  {sym:>5}  {qty:>6}")
    if shown < n:
        print(f"# ... {n - shown} more")


def main() -> None:
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--accesses", type=int, default=DEFAULT_ACCESSES,
                   help=f"number of ticks to generate (default: {DEFAULT_ACCESSES})")
    p.add_argument("--seed", type=int, default=DEFAULT_SEED,
                   help=f"PRNG seed for reproducibility (default: {DEFAULT_SEED})")
    p.add_argument("--out", type=Path, default=Path("stream.ticks"),
                   help="output stream path")
    p.add_argument("--tiny", action="store_true",
                   help="emit the curated hand-checkable tiny stream instead")
    p.add_argument("--dump", type=Path, default=None,
                   help="read a .ticks file and print it human-readably, then exit")
    p.add_argument("--dump-limit", type=int, default=0,
                   help="max records to print with --dump (0 = all)")
    p.add_argument("--stats", type=Path, default=None,
                   help="read a .ticks file, print the reference aggregate JSON, then exit")
    args = p.parse_args()

    if args.dump is not None:
        dump(args.dump, args.dump_limit)
        return

    if args.stats is not None:
        stats(args.stats)
        return

    if args.tiny:
        write_stream(args.out, curated_tiny())
        n = 10
    else:
        n = write_generated_stream(args.out, args.accesses, args.seed)

    print(f"wrote {n} ticks to {args.out}"
          + ("" if args.tiny else f" (seed={args.seed})"))


if __name__ == "__main__":
    main()
