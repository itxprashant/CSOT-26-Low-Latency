#!/usr/bin/env python3
"""
gen_trace.py — synthetic memory-trace generator for the CSoT'26 Week-2 cache sim.

Produces a binary trace in the FROZEN format from CACHE_SPEC.md §2: a contiguous
array of 16-byte csot::MemAccess records, little-endian, no header:

    offset 0 : uint64  address    (byte address, little-endian)
    offset 8 : uint8   is_write   (0 = read, 1 = write)
    offset 9 : uint8   pad[7]      (always zero)

The judge mmaps this straight into a `const MemAccess*`, so the on-disk layout
IS the struct.

Properties guaranteed:
  * Seeded — identical args + unchanged source => byte-identical trace across
    machines. Use this when comparing benchmark numbers with classmates.
  * Mixed locality on purpose: sequential scans, strided walks, hot-set reuse,
    write-heavy bursts, and cold random access — so hits, misses, evictions,
    and dirty writebacks all occur and the simulator's branch behaviour is
    non-trivial.
  * --tiny emits a small, hand-checkable curated trace (see CACHE_SPEC.md §5).

Usage:
    python3 gen_trace.py --tiny --out tiny.trace
    python3 gen_trace.py --accesses 5000000 --seed 42 --out large.trace
    python3 gen_trace.py --dump tiny.trace          # human-readable view

Dependencies: only the Python 3 standard library.
"""

from __future__ import annotations

import argparse
import random
import struct
import sys
from pathlib import Path

REC = struct.Struct("<QB7x")  # uint64 address, uint8 is_write, 7 pad bytes => 16 bytes
LINE_SIZE = 64
L1_SETS = 64
L2_SETS = 512

# --- Defaults (do not change for the cohort baseline) -----------------------
DEFAULT_SEED = 42
DEFAULT_ACCESSES = 1_000_000
# ----------------------------------------------------------------------------


def _write(out_path: Path, accesses: list[tuple[int, int]]) -> None:
    with out_path.open("wb") as f:
        for addr, is_write in accesses:
            f.write(REC.pack(addr & 0xFFFFFFFFFFFFFFFF, 1 if is_write else 0))


def curated_tiny() -> list[tuple[int, int]]:
    """A 14-access trace that exercises every demand path except L2 eviction.

    All of blocks 0,64,...,512 map to L1 set 0 (block & 63 == 0) but to distinct
    L2 sets (except 0 and 512, which share L2 set 0 without overflowing it), so
    you can trace it by hand against CACHE_SPEC.md §5. It hits: L1 hit/miss,
    L2 hit (step 11) / miss, the dirty bit, and the L1->L2 writeback hit path
    (step 10). dirty_writebacks stays 0 here (no L2 set overflows); the hidden
    trace exercises that.
    """
    blk = LINE_SIZE  # address of block k is k * 64
    return [
        (0 * blk * 64, 0),    # 0  R block0     miss / L2 miss, fill clean
        (0 * blk * 64, 0),    # 1  R block0     L1 hit
        (0 * blk * 64, 1),    # 2  W block0     L1 hit -> dirty
        (1 * blk * 64, 0),    # 3  R block64    miss / L2 miss
        (2 * blk * 64, 0),    # 4  R block128   miss / L2 miss
        (3 * blk * 64, 0),    # 5  R block192   miss / L2 miss
        (4 * blk * 64, 0),    # 6  R block256   miss / L2 miss
        (5 * blk * 64, 0),    # 7  R block320   miss / L2 miss
        (6 * blk * 64, 0),    # 8  R block384   miss / L2 miss
        (7 * blk * 64, 0),    # 9  R block448   miss / L2 miss (L1 set 0 now full)
        (8 * blk * 64, 0),    # 10 R block512   miss / L2 miss; evict dirty block0 -> writeback to L2
        (0 * blk * 64, 0),    # 11 R block0     L1 miss, L2 HIT
        (1 * 64, 1),          # 12 W block1     L1 set 1: miss / L2 miss, fill dirty
        (1 * 64, 0),          # 13 R block1     L1 hit
    ]


def generate(accesses: int, seed: int) -> list[tuple[int, int]]:
    """Mixed-locality stochastic trace. Reproducible for a given (accesses, seed)."""
    rng = random.Random(seed)
    out: list[tuple[int, int]] = []

    # A few working regions of different sizes relative to the caches.
    L1_BYTES = L1_SETS * 8 * LINE_SIZE          # 32 KiB
    L2_BYTES = L2_SETS * 8 * LINE_SIZE          # 256 KiB
    hot_base = 0x10_000                         # fits in L1 — high reuse
    warm_base = 0x40_000                        # ~L2-sized working set
    cold_base = 0x1_000_000                     # large — forces evictions/writebacks

    seq_ptr = warm_base
    while len(out) < accesses:
        phase = rng.random()
        burst = rng.randint(200, 2000)

        if phase < 0.35:
            # Sequential scan over a warm region: strong spatial locality.
            for _ in range(burst):
                addr = seq_ptr
                seq_ptr += rng.choice([4, 8, 8, 8, 16])      # within / across lines
                if seq_ptr > warm_base + L2_BYTES:
                    seq_ptr = warm_base
                out.append((addr, 1 if rng.random() < 0.2 else 0))
        elif phase < 0.6:
            # Hot-set temporal reuse: small set, lots of repeats, write-heavy.
            for _ in range(burst):
                addr = hot_base + rng.randrange(0, L1_BYTES)
                out.append((addr, 1 if rng.random() < 0.45 else 0))
        elif phase < 0.8:
            # Strided walk: stride > line size => a miss most lines.
            stride = rng.choice([64, 128, 256, 512])
            base = warm_base + rng.randrange(0, L2_BYTES)
            for k in range(burst):
                out.append((base + k * stride, 1 if rng.random() < 0.25 else 0))
        else:
            # Cold random over a large space: evictions and dirty writebacks.
            for _ in range(burst):
                addr = cold_base + rng.randrange(0, 64 * L2_BYTES)
                out.append((addr, 1 if rng.random() < 0.3 else 0))

    return out[:accesses]


def dump(path: Path, limit: int) -> None:
    data = path.read_bytes()
    if len(data) % REC.size != 0:
        sys.exit(f"trace size {len(data)} is not a multiple of {REC.size}")
    n = len(data) // REC.size
    print(f"# {path}: {n} accesses")
    print(f"# {'idx':>6}  op  {'address':>18}  {'block':>12}  l1set  l2set")
    shown = n if limit <= 0 else min(limit, n)
    for i in range(shown):
        addr, is_write = REC.unpack_from(data, i * REC.size)
        block = addr >> 6
        print(f"  {i:>6}  {'W' if is_write else 'R'}   0x{addr:016x}  "
              f"{block:>12}  {block & (L1_SETS-1):>5}  {block & (L2_SETS-1):>5}")
    if shown < n:
        print(f"# ... {n - shown} more")


def main() -> None:
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--accesses", type=int, default=DEFAULT_ACCESSES,
                   help=f"number of accesses to generate (default: {DEFAULT_ACCESSES})")
    p.add_argument("--seed", type=int, default=DEFAULT_SEED,
                   help=f"PRNG seed for reproducibility (default: {DEFAULT_SEED})")
    p.add_argument("--out", type=Path, default=Path("trace.trace"),
                   help="output trace path")
    p.add_argument("--tiny", action="store_true",
                   help="emit the curated hand-checkable tiny trace instead")
    p.add_argument("--dump", type=Path, default=None,
                   help="read a .trace and print it human-readably, then exit")
    p.add_argument("--dump-limit", type=int, default=0,
                   help="max records to print with --dump (0 = all)")
    args = p.parse_args()

    if args.dump is not None:
        dump(args.dump, args.dump_limit)
        return

    if args.tiny:
        accesses = curated_tiny()
    else:
        accesses = generate(args.accesses, args.seed)

    _write(args.out, accesses)
    print(f"wrote {len(accesses)} accesses to {args.out}"
          + ("" if args.tiny else f" (seed={args.seed})"))


if __name__ == "__main__":
    main()
