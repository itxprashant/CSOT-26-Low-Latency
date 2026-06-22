# Pipeline Spec — Reference Lock-Free Feed Pipeline

> **This file is a frozen competition contract.** Every submission must turn the same binary tick feed into the same order stream, by decoding the feed on one thread and driving the **unchanged Week-1 z-score strategy** on another, with a lock-free hand-off between them. The challenge is **not** to invent a metric or a trading rule; it is to build the decode → SPSC hand-off → strategy pipeline *correctly*, *deterministically*, and *as fast as two cores allow* — pick your ring-buffer capacity and memory orderings, keep the producer's head and the consumer's tail off each other's cache line, pin both threads, and let the two stages overlap.

---

## 1. Goal

The Week-4 challenge is pipeline-shaped, but it is **not** a "design your own metric" contest and **not** a "design your own signal" contest.

Every participant receives the same:

- `WireTick` / `OrderRecord` / `Pipeline` ABI from [`include/pipeline.hpp`](./include/pipeline.hpp)
- the frozen `Tick` / `Order` / `Strategy` ABI from [`include/strategy.hpp`](./include/strategy.hpp)
- the wire-feed file format from §2 below
- the decode rule (§4), the **unchanged** z-score strategy + fill model (§5, [`STRATEGY_SPEC.md`](../../week-1/project/STRATEGY_SPEC.md)), and the order-stream definition (§6)

Every participant must emit the same `OrderRecord` stream as the judge reference for the same input feed. If your stream differs, your submission is incorrect. Among correct submissions, the leaderboard ranks by **wall-clock time to run the whole pipeline** over a large held-out feed (and throughput) — the same "fastest correct implementation" game as Weeks 1–3, on a workload that finally rewards a good lock-free hand-off.

---

## 2. The Wire Format (frozen)

A feed is a binary file: a contiguous array of `csot::WireTick` records, little-endian, **no header**. Each record is exactly 40 bytes and is identical on disk and in memory, so the judge `mmap`s the file straight into a `const WireTick*`.

```text
offset 0  : uint64  timestamp_ns   (exchange wall-clock, ns since epoch, non-decreasing)
offset 8  : int64   bid_px_fp      (FIXED-POINT best bid: real_bid * PRICE_SCALE)
offset 16 : int64   ask_px_fp      (FIXED-POINT best ask: real_ask * PRICE_SCALE)
offset 24 : uint32  symbol_id       (0 .. NUM_SYMBOLS-1)
offset 28 : uint32  bid_qty         (shares at best bid; always > 0)
offset 32 : uint32  ask_qty         (shares at best ask; always > 0)
offset 36 : uint32  _reserved        (always zero)
```

Guarantees you can rely on:

- File size is always a multiple of 40. `n = filesize / 40` ticks.
- `symbol_id` is always in `[0, NUM_SYMBOLS)` (see §3).
- `bid_px_fp` and `ask_px_fp` are signed fixed-point integers: a real price of `100.05` is stored as `1000500`. `bid_px_fp <= ask_px_fp` always holds (a sane top of book), and both are positive on every feed the judge uses.
- `bid_qty` and `ask_qty` are strictly positive and fit in 32 bits.
- `_reserved` is always zero. Do not read it.
- `timestamp_ns` is non-decreasing. Ticks must be **processed in stream order** — unlike Week 3's aggregator, this workload is order-dependent (the strategy is stateful), so your SPSC hand-off must preserve order.

The reference generator [`data/gen_feed.py`](./data/gen_feed.py) emits this format; `data/tiny.feed` is a small golden file and `data/tiny.orders.json` holds its exact reference order stream.

---

## 3. Constants (frozen)

These constants are part of the spec. Do not change them.

| Name | Value | Meaning |
|---|---:|---|
| `NUM_SYMBOLS` | `1024` | Number of distinct symbol ids. `symbol_id ∈ [0, 1024)`. |
| `PRICE_SCALE` | `10000` | Fixed-point scale: stored `*_px_fp` = real price × 10000. |
| `WINDOW` | `64` | Rolling mid-price window (from `STRATEGY_SPEC.md` §3). |
| `ENTRY_Z` / `EXIT_Z` | `2.0` / `0.5` | Entry / exit z-thresholds (from `STRATEGY_SPEC.md` §3). |

`on_init(num_symbols)` is called with `NUM_SYMBOLS` (1024) before `run()`. Use the argument — do not hard-code `1024` in a way that breaks if a later season changes it.

### Symbol naming (frozen)

The strategy keys on `Tick::symbol`, a string. Symbol id `k` from the wire feed **interns to the exact string `"SYM<k>"`** — `0 → "SYM0"`, `1 → "SYM1"`, …, `1023 → "SYM1023"`. Your producer must map each `symbol_id` to a stable `"SYM<k>"` string (allocated once in `on_init`) and point `Tick::symbol` at it. The reference order stream identifies symbols by exactly this string, so any other naming fails correctness.

---

## 4. The Decode Contract (producer side)

For every `WireTick w` in the feed, the producer reconstructs the frozen `csot::Tick` the strategy consumes:

```text
tick.timestamp_ns = w.timestamp_ns
tick.symbol       = intern("SYM" + to_string(w.symbol_id))   // stable address, set up in on_init
tick.bid_px       = (double) w.bid_px_fp / PRICE_SCALE        // e.g. 1000500 -> 100.05
tick.ask_px       = (double) w.ask_px_fp / PRICE_SCALE
tick.bid_qty      = w.bid_qty
tick.ask_qty      = w.ask_qty
```

This decode is **real per-tick work** (two integer→double conversions plus the symbol hand-off) and it runs on the producer thread, off the consumer's critical path — that is exactly why a lock-free hand-off pays: the decode and the strategy run on two cores at once instead of one after the other.

> 📌 The division by `PRICE_SCALE` reconstructs a price that is *exactly representable* as a `double` for every feed the judge uses, and the strategy copies it into orders verbatim (§6) — so the round-trip back to fixed-point is exact and the order stream diffs as integers, never floats.

---

## 5. The Strategy (unchanged) and the Fill Model

The consumer drives the **frozen Week-1 strategy spec, unchanged**: per-symbol z-score mean reversion over a rolling 64-mid-price window. The full algorithm — mid-price, warm-up, population mean/standard deviation, z-score, entry at `|z| >= 2.0`, exit at `|z| <= 0.5`, at most one unit per symbol — is defined in [`STRATEGY_SPEC.md`](../../week-1/project/STRATEGY_SPEC.md) §2–§7. **Read it; it is authoritative and is not re-printed here.**

The consumer must drive the strategy exactly as the Week-1 engine did, in stream order:

```text
for each decoded tick t (in feed order):
    orders = strategy.on_tick(t)              // 0 or 1 order (STRATEGY_SPEC §5)
    for each order o in orders:
        record o in the output stream (tagged with t's tick index)
        strategy.on_fill(o, o.price, o.qty)   // deterministic immediate fill (STRATEGY_SPEC §8)
```

The Week-1 fill model (`STRATEGY_SPEC.md` §8) is unchanged: every emitted order fills immediately at its own `price` and `qty`, and `on_fill` is what updates `position` — never `on_tick`. Because `on_fill` mutates the per-symbol position that the next tick's logic reads, **the consumer must apply each tick's fill before processing the next tick for that symbol.** A single in-order consumer thread satisfies this automatically; this is why the hand-off must preserve order (§2) and why the strategy stays on **one** consumer thread, not sharded across many.

---

## 6. The Order Stream You Emit

Your `run()` writes an array of `OrderRecord` (see [`include/pipeline.hpp`](./include/pipeline.hpp)) and returns its length. Each record is `{ tick_index, order }`, where `tick_index` is the index (into the input feed) of the `WireTick` that produced the order. Records must appear in **tick order** (non-decreasing `tick_index`), which an in-order pipeline produces for free.

The judge (and the local harness) serialise the stream to JSON and diff it against the reference. The canonical per-order fields are:

| Field | Source | Notes |
|---|---|---|
| `tick_index` | index of the producing `WireTick` | strictly increasing across the stream (≤ 1 order per tick) |
| `timestamp_ns` | `in[tick_index].timestamp_ns` | reconstructed from the tick; informational |
| `symbol` | `order.symbol` | the interned `"SYM<id>"` string (§3) |
| `side` | `order.side` | `0` = BUY, `1` = SELL |
| `price_fp` | `round(order.price * PRICE_SCALE)` | **fixed-point integer** — never the raw double |
| `qty` | `order.qty` | strictly positive |

> ⚠️ **Copy, don't recompute, the price.** As in `STRATEGY_SPEC.md` §7, a BUY uses `t.ask_px` and a SELL uses `t.bid_px`, copied verbatim from the tick. Because those came from `*_px_fp / PRICE_SCALE`, re-encoding `round(price * PRICE_SCALE)` recovers the original integer exactly. If you recompute or round the price yourself, you will diverge.

Equality means the same sequence of `(tick_index, symbol, side, price_fp, qty)` tuples as the reference, in the same order. The harness also prints an FNV-1a `checksum` over the stream for a fast same/not-same check; `diff` against `data/tiny.orders.json` is the authoritative comparison.

---

## 7. The Determinism Guarantee (the whole point)

A correct pipeline is **deterministic**: the strategy is a pure function of the in-order tick sequence, and an SPSC ring buffer is an *order-preserving* channel. Therefore, no matter how you size the buffer, how the producer and consumer interleave, or how the OS schedules them, the consumer sees **exactly the feed order** and emits **exactly one** order stream.

This is what makes the contest fair and cheat-detectable:

1. **The judge computes the answer key in stream order.** The reference decodes and runs the strategy single-threaded; your two-thread pipeline must match it byte-for-byte.
2. **A hand-off bug is detectable.** If your ring buffer drops a tick (overwrites an unconsumed slot), duplicates one, or reorders them — the classic symptoms of wrong `head`/`tail` memory orderings or an off-by-one on a full/empty check — the emitted stream changes, and it often changes *differently from run to run*. The judge runs your `run()` **K times** and rejects any submission whose order stream is not identical across all K runs (a data race) — see §8.
3. **You may not parallelise the strategy itself.** The strategy is stateful and order-dependent (§5); only the *decode* stage and the *hand-off* are parallel with the strategy. Sharding ticks across multiple strategy threads breaks order and fails determinism.

> ⚠️ The subtle bugs here are not lost integer updates (Week 3) but **lost or reordered messages**: a `head`/`tail` published with the wrong memory ordering lets the consumer read a slot the producer hasn't finished writing, or miss a slot entirely. §8 lists the symptoms.

---

## 8. Correctness & Determinism Rules

A submission is **correct** if, for the same feed, every `OrderRecord` equals the judge reference exactly (`tick_index`, `symbol`, `side`, `price_fp`, `qty`), in the same order, with the same total count. The judge reports the first differing record as feedback, e.g. `order 412 (tick 90183) side: got BUY want SELL`.

A submission must also be **deterministic**: the judge runs your `run()` K times (see [`README.md`](./README.md) → leaderboard) and requires **identical** order streams across all K runs. A submission that produces different streams on different runs is flagged non-deterministic (a data race in the hand-off) and is **not ranked**, even if one run happened to match.

Sanity identities that hold for any feed:

```text
records are in non-decreasing tick_index order, ≤ 1 record per tick
every order's price_fp == in[tick_index].(bid_px_fp if SELL else ask_px_fp)
no order is emitted while a symbol is in warm-up (< 64 mids)   (STRATEGY_SPEC §5)
|position| never exceeds 1 for any symbol                        (STRATEGY_SPEC §3)
```

Things that make a submission **incorrect** or **rejected**:

- A ring-buffer bug that **drops, duplicates, or reorders** ticks (wrong order stream, usually non-deterministic → rejected).
- A data race on `head`/`tail` (wrong memory ordering → the consumer reads a half-written slot → garbage tick → wrong/varying orders).
- Sharding the stateful strategy across threads (breaks order; positions update out of sequence).
- Recomputing or rounding the order price instead of copying the tick field (§6).
- Interning symbols as anything other than `"SYM<id>"` (§3).
- Any deviation from the frozen z-score / fill rules in `STRATEGY_SPEC.md`.

---

## 9. Performance Rules

Correctness (and determinism) is binary. Performance is ranked among correct, deterministic submissions only.

The leaderboard ranks by:

1. lower **wall-clock time** for `run()` over the hidden feed
2. higher **throughput** (ticks/second) on the same feed
3. (tie-break) earlier valid submission

The judge box has **4 vCPUs** (`c7i.xlarge`). The pipeline naturally uses **two** hot threads (one decode, one strategy); the spare cores are for pinning headroom and the OS. The win is making the two stages overlap perfectly through a hand-off that never stalls.

### Hot-path expectations

| Week | Expected implementation focus for a competitive `run()` |
|---|---|
| 4 | A lock-free **SPSC ring buffer** (power-of-two capacity, `head`/`tail` on separate cache lines, `acquire`/`release` orderings — no `seq_cst`, no mutex) connecting a decode producer and a strategy consumer; both threads **pinned** (`sched_setaffinity`, from Week 3); zero heap in `run()` (ring storage, per-symbol state, and interned names all allocated in `on_init`); a tight decode and a rolling-sum z-score (Week-2 discipline) so neither stage starves the other. |
| 5 | the feed arrives over the network; the same hand-off decouples the I/O thread from the strategy. |

The anti-saturation premise: a correct single-threaded decode-then-strategize is easy, but overlapping the two stages so the hand-off latency vanishes — without a lock, without false-sharing `head` against `tail`, without scheduler jitter — has real headroom over the serial baseline (up to roughly the ratio of decode+strategy time to `max(decode, strategy)` time). Expect the board to keep moving after the first correct upload.

---

## 10. Common Pitfalls

- **A mutex (or `std::queue`) in the hand-off.** A lock serialises the producer and consumer — the very thing you split apart. It is correct but it caps you at (and often below) the single-threaded baseline.
- **`head` and `tail` on the same cache line.** The producer writes `tail`, the consumer writes `head`; if they share a 64-byte line they ping-pong it on every push/pop (false sharing, Week 3 again). Pad them apart with `alignas(64)`.
- **Wrong memory ordering on publish.** Publishing the index with `relaxed` lets the consumer observe the new `tail` before the slot's payload write is visible — it reads a half-written tick. Use `release` on publish, `acquire` on observe (see [`03-memory-orderings.md`](../03-memory-orderings.md)).
- **`seq_cst` everywhere.** Correct but slower than necessary; an SPSC queue needs only release/acquire. Don't pay for the full barrier.
- **Off-by-one full/empty check.** Confusing "full" and "empty" drops or duplicates ticks. A power-of-two capacity with masked indices and a one-slot-reserved (or separate count) convention avoids it.
- **Sharding the strategy.** The strategy is stateful and order-dependent; it stays on one consumer thread. Only decode + hand-off are parallel.
- **Heap inside `run()`.** Allocate the ring storage, per-symbol state, and interned `"SYM<id>"` table in `on_init()`; the hot path must not `malloc`.
- **Unpinned threads.** Without `sched_setaffinity`, the producer and consumer migrate across cores, trash their caches, and add jitter to your timed `run()` (Week 3, [`05-pipeline-pinning-and-backpressure.md`](../05-pipeline-pinning-and-backpressure.md)).
- **Recomputing the price** or interning symbols wrong (§6, §3).

---

## 11. Why This Is Still the Same Track

The workload is pipeline-shaped — decode on one thread, strategize on another, hand off between them without a lock. But the contest objective is identical to Weeks 1–3: everyone computes the *same* fixed answer (the Week-1 order stream), and the leaderboard measures **systems-engineering speed**, not metric or signal cleverness. The strategy supplies a stateful, order-dependent workload; the leaderboard measures how fast you can keep two cores busy through a correct, race-free, lock-free hand-off.

That distinction is the whole challenge.
