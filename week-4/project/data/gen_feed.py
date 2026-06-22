#!/usr/bin/env python3
"""
gen_feed.py — synthetic wire-feed generator for the CSoT'26 Week-4 pipeline.

Produces a binary feed in the FROZEN format from PIPELINE_SPEC.md §2: a
contiguous array of 40-byte csot::WireTick records, little-endian, no header:

    offset 0  : uint64  timestamp_ns   (non-decreasing)
    offset 8  : int64   bid_px_fp      (FIXED-POINT best bid: real_bid * 10000)
    offset 16 : int64   ask_px_fp      (FIXED-POINT best ask: real_ask * 10000)
    offset 24 : uint32  symbol_id       (0 .. NUM_SYMBOLS-1)
    offset 28 : uint32  bid_qty         (> 0)
    offset 32 : uint32  ask_qty         (> 0)
    offset 36 : uint32  _reserved        (always zero)

The judge mmaps this straight into a `const WireTick*`, so the on-disk layout IS
the struct.

Properties guaranteed:
  * Seeded — identical args + unchanged source => byte-identical feed across
    machines. Use this when comparing benchmark numbers with classmates.
  * Mid-prices follow a per-symbol random walk with enough volatility that the
    z-score strategy actually enters and exits (so the order stream is not
    empty), with a skewed symbol popularity (a few hot symbols, a long tail).
  * --tiny emits a small curated feed over 4 symbols whose reference order
    stream is committed as data/tiny.orders.json.
  * --orders reads a feed and prints the REFERENCE order stream as JSON. This is
    the single-threaded answer key: it decodes each tick and runs the frozen
    Week-1 z-score strategy (STRATEGY_SPEC.md §5-§8) in stream order. It is what
    data/tiny.orders.json is made of. It is NOT fast and will NOT win the
    leaderboard; the contest is to produce the same stream through a lock-free
    pipeline, faster.

Usage:
    python3 gen_feed.py --tiny --out tiny.feed
    python3 gen_feed.py --accesses 5000000 --seed 42 --out large.feed
    python3 gen_feed.py --dump tiny.feed                 # human-readable view
    python3 gen_feed.py --orders tiny.feed               # reference order stream JSON

Dependencies: only the Python 3 standard library.

IMPORTANT — float determinism:
  The reference reproduces the strategy's double arithmetic operation-for-
  operation (same summation order over the 64-mid window, same (bid+ask)*0.5,
  same population variance, same sqrt) so its order stream is bit-identical to a
  correct C++ implementation compiled WITHOUT -ffast-math (the judge does not
  use it). Do not "simplify" the math here.
"""

from __future__ import annotations

import argparse
import json
import math
import random
import struct
import sys
from pathlib import Path

# uint64 ts, int64 bid_fp, int64 ask_fp, uint32 sym, uint32 bid_qty,
# uint32 ask_qty, uint32 reserved => 40 bytes
REC = struct.Struct("<QqqIIII")

# --- Spec constants (must match PIPELINE_SPEC.md §3 / STRATEGY_SPEC.md §3) ---
NUM_SYMBOLS = 1024
PRICE_SCALE = 10000
WINDOW = 64
ENTRY_Z = 2.0
EXIT_Z = 0.5
EPSILON_STDDEV = 1e-9
SIDE_BUY = 0
SIDE_SELL = 1

# --- Defaults (do not change for the cohort baseline) -----------------------
DEFAULT_SEED = 42
DEFAULT_ACCESSES = 1_000_000

# FNV-1a 64-bit — the canonical order-stream checksum (also computed by
# harness/main.cpp). Each order contributes, little-endian: tick_index(u64),
# timestamp_ns(u64), side(u64), price_fp(i64 as u64), qty(u64), symbol bytes.
FNV_OFFSET = 0xCBF29CE484222325
FNV_PRIME = 0x00000100000001B3
MASK64 = 0xFFFFFFFFFFFFFFFF


# ---------------------------------------------------------------------------
# Feed generation
# ---------------------------------------------------------------------------
TINY_SYMBOLS = 4
TINY_TICKS = 2400


def curated_tiny() -> list[tuple[int, int, int, int, int, int]]:
    """A small feed over symbols 0..3 (TINY_TICKS ticks, round-robin).

    Each symbol's mid follows a random walk — the same process as the large
    feed, just restricted to 4 symbols — so it wanders far enough from its
    trailing 64-mid mean to reach the +/-2 entry band and the +/-0.5 exit band.
    (A pure sinusoid cannot: its z-score is capped near sqrt(2) < 2.) The feed
    is long enough past warm-up to emit a handful of orders. Returns
    (timestamp_ns, bid_fp, ask_fp, symbol_id, bid_qty, ask_qty) tuples in stream
    order; the exact reference order stream is data/tiny.orders.json.
    """
    rng = random.Random(123)
    mid = [1_000_000 + s * 50_000 for s in range(TINY_SYMBOLS)]
    ticks: list[tuple[int, int, int, int, int, int]] = []
    ts = 1_000_000_000
    for k in range(TINY_TICKS):
        s = k % TINY_SYMBOLS
        step = rng.randint(-400, 400)
        mid[s] = max(1_000, mid[s] + step)
        spread = rng.randint(20, 200)
        bid = mid[s] - spread // 2
        ask = bid + spread
        ts += rng.randint(1, 50)
        ticks.append((ts, bid, ask, s, rng.randint(1, 1000), rng.randint(1, 1000)))
    return ticks


_STREAM_CHUNK = 8192  # records buffered before each write() — flat RAM on huge feeds


def _walk_state(seed: int):
    rng = random.Random(seed)
    hot = list(range(0, 16))
    warm = list(range(16, 256))
    mid = [rng.randint(500_000, 5_000_000) for _ in range(256)]  # fixed-point mids
    return rng, hot, warm, mid


def _next_tick(rng, hot, warm, mid, ts):
    if rng.random() < 0.80:
        s = rng.choice(hot)
    else:
        s = rng.choice(warm)
    step = rng.randint(-400, 400)
    mid[s] = max(1_000, mid[s] + step)
    spread = rng.randint(20, 200)
    bid = mid[s] - spread // 2
    ask = bid + spread
    bid_qty = rng.randint(1, 1000)
    ask_qty = rng.randint(1, 1000)
    ts += rng.randint(1, 50)
    return (ts, bid, ask, s, bid_qty, ask_qty), ts


def write_generated_feed(out_path: Path, accesses: int, seed: int) -> int:
    """Stream skewed ticks straight to disk without materialising the full feed."""
    rng, hot, warm, mid = _walk_state(seed)
    ts = 1_000_000_000
    buf = bytearray(_STREAM_CHUNK * REC.size)
    off = 0
    with out_path.open("wb") as f:
        for _ in range(accesses):
            (t_ts, bid, ask, s, bq, aq), ts = _next_tick(rng, hot, warm, mid, ts)
            REC.pack_into(buf, off, t_ts & MASK64, bid, ask,
                          s & 0xFFFFFFFF, bq & 0xFFFFFFFF, aq & 0xFFFFFFFF, 0)
            off += REC.size
            if off >= len(buf):
                f.write(buf)
                off = 0
        if off:
            f.write(buf[:off])
    return accesses


def write_feed(out_path: Path, ticks: list[tuple[int, int, int, int, int, int]]) -> None:
    buf = bytearray(len(ticks) * REC.size)
    off = 0
    for ts, bid, ask, sym, bq, aq in ticks:
        REC.pack_into(buf, off, ts & MASK64, bid, ask,
                      sym & 0xFFFFFFFF, bq & 0xFFFFFFFF, aq & 0xFFFFFFFF, 0)
        off += REC.size
    out_path.write_bytes(buf)


# ---------------------------------------------------------------------------
# Reference strategy (the answer key) — mirrors STRATEGY_SPEC.md §6 and the
# stub's double arithmetic operation-for-operation.
# ---------------------------------------------------------------------------
class _SymbolState:
    __slots__ = ("mids", "count", "head", "position")

    def __init__(self):
        self.mids = [0.0] * WINDOW
        self.count = 0
        self.head = 0
        self.position = 0


def reference_orders(path: Path) -> tuple[int, list[dict]]:
    """Decode the feed and run the frozen z-score strategy in stream order.

    Returns (num_ticks, orders) where each order is the dict the JSON emits.
    """
    data = path.read_bytes()
    if len(data) % REC.size != 0:
        sys.exit(f"feed size {len(data)} is not a multiple of {REC.size}")
    n = len(data) // REC.size

    state = [_SymbolState() for _ in range(NUM_SYMBOLS)]
    orders: list[dict] = []

    for i in range(n):
        ts, bid_fp, ask_fp, sym, _bq, _aq, _res = REC.unpack_from(data, i * REC.size)

        # ---- Decode (PIPELINE_SPEC.md §4) ----
        bid_px = bid_fp / 10000.0
        ask_px = ask_fp / 10000.0

        st = state[sym]
        mid = (bid_px + ask_px) * 0.5
        st.mids[st.head] = mid
        st.head = (st.head + 1) & (WINDOW - 1)
        if st.count < WINDOW:
            st.count += 1
        if st.count < WINDOW:
            continue

        s = 0.0
        for x in st.mids:
            s += x
        mean = s / 64.0

        sq = 0.0
        for x in st.mids:
            d = x - mean
            sq += d * d
        variance = sq / 64.0
        stddev = math.sqrt(variance)
        if stddev < EPSILON_STDDEV:
            continue

        z = (mid - mean) / stddev
        abs_z = abs(z)

        side = None
        price_fp = None
        qty = None
        if st.position == 0:
            if z >= ENTRY_Z:
                side, price_fp, qty = SIDE_SELL, bid_fp, 1
                st.position -= 1
            elif z <= -ENTRY_Z:
                side, price_fp, qty = SIDE_BUY, ask_fp, 1
                st.position += 1
        elif st.position > 0 and abs_z <= EXIT_Z:
            side, price_fp, qty = SIDE_SELL, bid_fp, st.position
            st.position = 0
        elif st.position < 0 and abs_z <= EXIT_Z:
            side, price_fp, qty = SIDE_BUY, ask_fp, -st.position
            st.position = 0

        if side is not None:
            orders.append({
                "tick_index": i,
                "timestamp_ns": ts,
                "symbol": f"SYM{sym}",
                "side": side,
                "price_fp": price_fp,
                "qty": qty,
            })

    return n, orders


def order_stream_checksum(orders: list[dict]) -> int:
    h = FNV_OFFSET

    def u64(v: int):
        nonlocal h
        for b in struct.pack("<Q", v & MASK64):
            h = ((h ^ b) * FNV_PRIME) & MASK64

    for o in orders:
        u64(o["tick_index"])
        u64(o["timestamp_ns"])
        u64(o["side"])
        u64(o["price_fp"] & MASK64)
        u64(o["qty"])
        for b in o["symbol"].encode("ascii"):
            h = ((h ^ b) * FNV_PRIME) & MASK64
    return h


def orders_json(path: Path) -> None:
    n, orders = reference_orders(path)
    out = {
        "num_symbols": NUM_SYMBOLS,
        "n": n,
        "num_orders": len(orders),
        "checksum": order_stream_checksum(orders),
        "orders": orders,
    }
    print(json.dumps(out, indent=2))


def dump(path: Path, limit: int) -> None:
    data = path.read_bytes()
    if len(data) % REC.size != 0:
        sys.exit(f"feed size {len(data)} is not a multiple of {REC.size}")
    n = len(data) // REC.size
    print(f"# {path}: {n} ticks")
    print(f"# {'idx':>6}  {'timestamp_ns':>14}  {'bid_fp':>10}  {'ask_fp':>10}  "
          f"{'sym':>5}  {'bid_qty':>7}  {'ask_qty':>7}")
    shown = n if limit <= 0 else min(limit, n)
    for i in range(shown):
        ts, bid, ask, sym, bq, aq, _res = REC.unpack_from(data, i * REC.size)
        print(f"  {i:>6}  {ts:>14}  {bid:>10}  {ask:>10}  {sym:>5}  {bq:>7}  {aq:>7}")
    if shown < n:
        print(f"# ... {n - shown} more")


def main() -> None:
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--accesses", type=int, default=DEFAULT_ACCESSES,
                   help=f"number of ticks to generate (default: {DEFAULT_ACCESSES})")
    p.add_argument("--seed", type=int, default=DEFAULT_SEED,
                   help=f"PRNG seed for reproducibility (default: {DEFAULT_SEED})")
    p.add_argument("--out", type=Path, default=Path("feed.feed"),
                   help="output feed path")
    p.add_argument("--tiny", action="store_true",
                   help="emit the curated hand-checkable tiny feed instead")
    p.add_argument("--dump", type=Path, default=None,
                   help="read a .feed file and print it human-readably, then exit")
    p.add_argument("--dump-limit", type=int, default=0,
                   help="max records to print with --dump (0 = all)")
    p.add_argument("--orders", type=Path, default=None,
                   help="read a .feed file, print the reference order stream JSON, exit")
    args = p.parse_args()

    if args.dump is not None:
        dump(args.dump, args.dump_limit)
        return

    if args.orders is not None:
        orders_json(args.orders)
        return

    if args.tiny:
        write_feed(args.out, curated_tiny())
        n = TINY_TICKS
    else:
        n = write_generated_feed(args.out, args.accesses, args.seed)

    print(f"wrote {n} ticks to {args.out}"
          + ("" if args.tiny else f" (seed={args.seed})"))


if __name__ == "__main__":
    main()
