# Agg Spec — Reference Parallel Tick Aggregator

> **This file is a frozen competition contract.** Every submission must compute this exact per-symbol aggregate table and emit, for every symbol id, the same five integer counters as the judge reference for the same stream. The challenge is **not** to invent a cleverer aggregate; it is to compute this fixed reduction *correctly* and *as fast as possible* over many cores — partition the stream, keep your per-thread state off each other's cache lines, pin your threads, and reduce a huge array at memory-bandwidth speed.

---

## 1. Goal

The Week-3 challenge is parallelization-shaped, but it is **not** a "design your own metric" contest.

Every participant receives the same:

- `AggTick` / `SymbolAgg` / `Aggregator` ABI from [`include/aggregate.hpp`](./include/aggregate.hpp)
- tick-stream file format from §2 below
- the fixed symbol count and aggregate definitions in this file

Every participant must emit the same `SymbolAgg` table as the judge reference for the same input stream. If any symbol's row differs, your submission is incorrect. Among correct submissions, the leaderboard ranks by **wall-clock time to reduce a large held-out stream** (and throughput) — the same "fastest correct implementation" game as Weeks 1 and 2, on a workload that finally rewards going wide.

---

## 2. The Stream Format (frozen)

A stream is a binary file: a contiguous array of `csot::AggTick` records, little-endian, **no header**. Each record is exactly 32 bytes and is identical on disk and in memory, so the judge `mmap`s the file straight into a `const AggTick*`.

```text
offset 0  : uint64  timestamp_ns   (exchange wall-clock, ns since epoch, non-decreasing)
offset 8  : int64   price          (FIXED-POINT: real price * 10000)
offset 16 : uint32  symbol_id       (0 .. NUM_SYMBOLS-1)
offset 20 : uint32  qty             (shares; always > 0)
offset 24 : uint64  _reserved        (always zero)
```

Guarantees you can rely on:

- File size is always a multiple of 32. `n = filesize / 32` ticks.
- `symbol_id` is always in `[0, NUM_SYMBOLS)` (see §3). You never need to bounds-check it against the spec, but defensive code is cheap.
- `price` is a signed fixed-point integer: a real price of `100.05` is stored as `1000500`. You aggregate the **integer** `price` field directly; you never convert to floating point.
- `qty` is strictly positive and fits in 32 bits; sums of `qty` fit in 64 bits for any stream the judge uses.
- `_reserved` is always zero. Do not read it.
- `timestamp_ns` is non-decreasing, but **no order-dependent aggregate is required** — see §7. You may process ticks in any order.

The reference generator [`data/gen_ticks.py`](./data/gen_ticks.py) emits this format; `data/tiny.ticks` is a small golden file and `data/tiny.agg.json` holds its exact reference table.

---

## 3. Constants (frozen)

These constants are part of the spec. Do not change them.

| Name | Value | Meaning |
|---|---:|---|
| `NUM_SYMBOLS` | `1024` | Number of distinct symbol ids. `symbol_id ∈ [0, 1024)`. |
| `PRICE_SCALE` | `10000` | Fixed-point scale: stored `price` = real price × 10000. (Informational; you aggregate the stored integer directly.) |

`on_init(num_symbols)` is called with `NUM_SYMBOLS` (1024) before `run()`. Use the argument — do not hardcode `1024` in a way that breaks if a later season changes it. The `out` buffer handed to `run()` has exactly `num_symbols` rows.

### The empty-symbol rule

The result table has one row per symbol id `0 .. NUM_SYMBOLS-1`, **even for ids that never appear** in the stream. A symbol with zero ticks has the canonical row:

```text
count = 0, sum_price = 0, min_price = 0, max_price = 0, sum_qty = 0
```

You must write that exact row for absent symbols. (This makes the table a fixed-size, position-defined object that the judge can diff row-by-row.)

---

## 4. The Aggregates

For each symbol id, over exactly the ticks whose `symbol_id` equals it, compute:

| Field | Definition | Type |
|---|---|---|
| `count` | number of ticks | `uint64` |
| `sum_price` | Σ `price` | `int64` |
| `min_price` | minimum `price` (canonical `0` if `count == 0`) | `int64` |
| `max_price` | maximum `price` (canonical `0` if `count == 0`) | `int64` |
| `sum_qty` | Σ `qty` | `uint64` |

All five are **pure integer** reductions. There is deliberately **no** average, VWAP, "last price", or variance in the required set: an average would force a division (and a rounding convention), and "last price" would be order-dependent. Every required aggregate is associative **and** commutative over integers, which is what §7 leans on. (Want a mean or VWAP? Derive it *outside* the timed `run()` from `sum_price`, `sum_qty`, and `count` — it is not part of the contract.)

> 📌 **No overflow worries for the judge's streams.** `sum_price` and `sum_qty` fit in 64 bits for every stream the judge uses. You do not need saturating or wider arithmetic; plain `int64`/`uint64` accumulation is correct.

---

## 5. Per-Tick Algorithm

For every tick in the stream, do exactly the following (order across ticks does not matter — see §7):

```text
s = tick.symbol_id
row = out[s]
if row.count == 0:           # first time we see this symbol
    row.min_price = tick.price
    row.max_price = tick.price
else:
    row.min_price = min(row.min_price, tick.price)
    row.max_price = max(row.max_price, tick.price)
row.count     += 1
row.sum_price += tick.price
row.sum_qty   += tick.qty
```

The first-tick special-case for `min`/`max` is just the standard "seed min/max with the first value" trick. Equivalently, you may seed `min_price = INT64_MAX`, `max_price = INT64_MIN` for accumulators, then **collapse any still-empty row to the canonical zeros (§3) before emitting**. Both are correct; pick whichever vectorizes better.

---

## 6. Reference Pseudocode

This pseudocode is the behavioural reference. It is written for clarity, **not speed** — your job is to reproduce its table far faster, in parallel.

```cpp
void aggregate(const AggTick* t, size_t n, SymbolAgg* out, uint32_t num_symbols) {
    for (uint32_t s = 0; s < num_symbols; ++s)
        out[s] = SymbolAgg{0, 0, 0, 0, 0};       // canonical empty rows

    bool seen[NUM_SYMBOLS] = {false};            // first-tick tracking
    for (size_t i = 0; i < n; ++i) {
        const uint32_t s  = t[i].symbol_id;
        const int64_t  px = t[i].price;
        SymbolAgg&     r  = out[s];
        if (!seen[s]) { r.min_price = px; r.max_price = px; seen[s] = true; }
        else          { if (px < r.min_price) r.min_price = px;
                        if (px > r.max_price) r.max_price = px; }
        r.count     += 1;
        r.sum_price += px;
        r.sum_qty   += t[i].qty;
    }
    // rows whose count stayed 0 already hold the canonical zeros.
}
```

You may replace this with anything — per-thread partial tables merged at the end, SoA columns, SIMD min/max, prefetch — as long as the `num_symbols` rows you return are bit-for-bit identical.

---

## 7. The Determinism Guarantee (the whole point)

Every required aggregate is **associative and commutative** over integers:

```text
count, sum_price, sum_qty :  + is associative and commutative
min_price, max_price       :  min / max are associative and commutative
```

Therefore the result does **not depend on how you split the stream or in what order threads finish**. Split the `n` ticks into any partition `P₁, P₂, …, P_k` (one per thread, however you like), aggregate each partition independently into a partial table, then **merge** the partials:

```text
merge(A, B).count     = A.count + B.count
merge(A, B).sum_price = A.sum_price + B.sum_price
merge(A, B).sum_qty   = A.sum_qty + B.sum_qty
merge(A, B).min_price = (A.count==0) ? B.min_price : (B.count==0) ? A.min_price : min(A.min, B.min)
merge(A, B).max_price = (A.count==0) ? B.max_price : (B.count==0) ? A.max_price : max(A.max, B.max)
```

The merged table equals the single-threaded table **exactly**, for any partition and any thread count. This is why:

1. **The correctness gate is fair across thread counts.** A 1-thread and an 8-thread submission that are both correct produce the identical 1024-row table. Nobody is penalized or helped by how wide they go.
2. **The judge computes the answer key single-threaded.** The reference runs the §6 loop once; your multithreaded reduction must match it.
3. **A data race is detectable.** A correct parallel reduction is *deterministic*: it returns the same table every run. If your `run()` returns different tables on repeated runs (because two threads raced on a shared accumulator), the judge sees the variation and rejects it — see §8. Determinism is not optional; it is the gate.

> ⚠️ Watch the empty-partition edge case in `merge`: a partition that saw **no** ticks for a symbol has `count == 0` and its `min_price`/`max_price` are the canonical `0`, **not** a real price. If you naively `min(0, real_price)` you will wrongly clamp to 0. Merge must skip empty operands (the `count==0` guards above), or you must seed accumulators with `INT64_MAX`/`INT64_MIN` and only collapse to zero at the very end.

---

## 8. Correctness & Determinism Rules

A submission is **correct** if, for the same stream, every one of the `num_symbols` rows equals the judge reference exactly (all five fields). The judge reports the first differing row as feedback, e.g. `symbol 17 sum_price: got 41280500, want 41280499`.

A submission must also be **deterministic**: the judge runs your `run()` K times (see [`README.md`](./README.md) → leaderboard) and requires **identical** tables across all K runs. A submission that produces different results on different runs is flagged as non-deterministic (a data race) and is **not ranked**, even if one of the runs happened to match.

Sanity identities that must hold for any stream:

```text
Σ_s count(s)     == n                         (every tick lands in exactly one row)
Σ_s sum_qty(s)   == Σ_i qty(i)                 (all quantity is accounted for)
count(s) == 0    ⇒  the whole row is canonical zeros   (§3)
count(s)  > 0    ⇒  min_price(s) <= max_price(s)
```

Things that make a submission **incorrect** or **rejected**:

- A data race on a shared accumulator (lost updates → wrong counts, and non-determinism → rejected).
- Merging empty partials wrong (clamping `min`/`max` to the canonical `0` — see the §7 warning).
- Forgetting to write canonical zeros for symbols that never appear.
- Floating-point accumulation of `price` (it is an integer; FP would round and could even become non-associative — never do this).
- Reading `_reserved`, or assuming the stream is sorted by symbol or by time.

---

## 9. Performance Rules

Correctness (and determinism) is binary. Performance is ranked among correct submissions only.

The leaderboard ranks by:

1. lower **wall-clock time** for `run()` over the hidden stream
2. higher **throughput** (ticks/second) on the same stream
3. (tie-break) earlier valid submission

The judge box has **4 vCPUs** (`c7i.xlarge`). You may spawn more threads than that, but there are only four cores to run them on; oversubscription usually hurts. Treat 4 as your scaling target.

### Hot-path expectations

| Week | Expected implementation focus for a competitive `run()` |
|---|---|
| 3 | Partition the stream across threads; **per-thread partial tables on separate cache lines** (`alignas(64)` / padding — no false sharing); pin threads to distinct cores; zero heap in `run()` (size partials in `on_init`); a tight, branch-lean, ideally SIMD-friendly inner accumulate; merge once at the end. |
| 4 | the spec strategy returns with a threaded pipeline; this aggregator becomes a hall-of-fame / stretch artifact. |

The anti-saturation premise: a single-threaded correct reducer is easy, but driving four cores over a multi-GB stream **without** false sharing, scheduler jitter, or memory-bandwidth waste has a high ceiling. Expect the board to keep moving long after the first correct upload.

---

## 10. Common Pitfalls

- **False sharing on the partial tables.** Two threads' accumulators sharing a 64-byte line ping-pong the cache and can make 4 threads *slower* than 1. Pad/align per-thread state to a cache line ([`04-false-sharing.md`](../04-false-sharing.md)).
- **A shared, unsynchronized table.** `++out[s].count` from many threads is a data race: lost updates *and* non-determinism. Reduce into per-thread partials, then merge.
- **Merging empty rows wrong.** `count==0` rows carry canonical `0` min/max, not real prices. Guard the merge (§7).
- **Heap inside `run()`.** Allocate per-thread tables and the thread handles' storage in `on_init()`; the hot path must not `malloc` ([`04-zero-allocation.md`](../../week-2/04-zero-allocation.md)).
- **Unpinned threads.** Without `sched_setaffinity`, the scheduler migrates threads across cores mid-reduction, trashing the per-core caches and adding jitter to your timed `run()` ([`05-scheduler-and-pinning.md`](../05-scheduler-and-pinning.md)).
- **Oversubscription.** More threads than cores means context switches inside the timed region. Match thread count to the 4 vCPUs.
- **Floating-point sums.** `price` is an integer. FP accumulation rounds and is not associative — it would even break the determinism guarantee.
- **Forgetting the canonical empty rows.** Every one of the 1024 rows must be written, including absent symbols.

---

## 11. Why This Is Still the Same Track

The workload is parallelization-shaped — partition, reduce, merge, and fight the friction (races, false sharing, scheduler jitter) that threads introduce. But the contest objective is identical to Weeks 1 and 2: everyone computes the *same* fixed answer, and the leaderboard measures **systems-engineering speed**, not metric cleverness. The aggregate supplies the workload; the leaderboard measures how fast you can drive four cores over a huge array without tripping over each other.

That distinction is the whole challenge.
