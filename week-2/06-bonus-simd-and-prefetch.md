# 06 — ⭐ Bonus: SIMD, Branchless, and Prefetch — The Anti-Saturation Toolkit

> **⭐ This is bonus content for Week 2.** You can be correct and competitive without it. But this is the toolkit that separates the top of the leaderboard from the middle — and the reason the board won't saturate in week one. Read it after your simulator is correct and flat.

> **TL;DR** — The 8-way tag scan is your hottest inner loop. Make it **branchless** (compute a hit mask instead of breaking on match), then **SIMD** (compare all 8 tags in one instruction), and **prefetch** the set you'll touch next. Then read the assembly to confirm the compiler did what you think.

Everything so far — hot/cold split, flat SoA, zero-alloc, compile-time geometry — got you a clean, fast scalar simulator. This file is about squeezing the last few × out of the inner loop.

---

## 1. First, Earn the Right

Do **not** start here. SIMD and prefetch on top of a node-based, allocating, branchy simulator is polishing a slow thing. The order is always:

1. Correct (matches `data/tiny.stats.json` and a large trace).
2. Flat SoA layout ([`03-locality.md`](./03-locality.md)).
3. Zero allocation in `run()` ([`04-zero-allocation.md`](./04-zero-allocation.md)).
4. Compile-time geometry ([`05-compile-time-and-static-polymorphism.md`](./05-compile-time-and-static-polymorphism.md)).
5. *Then* this file.

Measure after every change. If a "clever" trick doesn't move the throughput line, revert it — complexity you can't justify with a number is a liability.

---

## 2. Branchless Tag Scan

The naive scan branches on every way:

```cpp
int find(const std::uint64_t* tags, const std::uint8_t* valid, std::uint64_t t) {
    for (int w = 0; w < 8; ++w)
        if (valid[w] && tags[w] == t) return w;   // data-dependent branch -> mispredicts
    return -1;
}
```

On a real trace the match position is unpredictable, so the branch predictor misses often (~3–10 ns each, Week-1 [`week-1/03-memory-hierarchy.md`](../week-1/03-memory-hierarchy.md)). Branchless: compute a bitmask of which ways match, then find the set bit.

```cpp
int find_branchless(const std::uint64_t* tags, const std::uint8_t* valid, std::uint64_t t) {
    unsigned hit = 0;
    for (int w = 0; w < 8; ++w)
        hit |= unsigned((tags[w] == t) & (valid[w] != 0)) << w;   // no branch, just bit ops
    return hit ? __builtin_ctz(hit) : -1;                          // lowest matching way
}
```

The 8-iteration loop has a constant trip count, so the compiler unrolls it into straight-line code with no data-dependent branches; `__builtin_ctz` is one instruction. This alone is often a solid win on the probe.

---

## 3. SIMD Tag Compare — All 8 Ways at Once

Eight 64-bit tags are 64 bytes. With AVX2 (256-bit registers = four `uint64`), you compare four tags per instruction; two loads cover the set. With AVX-512 (512-bit = eight `uint64`), **one** compare does the whole 8-way set.

```cpp
#include <immintrin.h>

// AVX2: compare 8 tags (two 256-bit lanes) against the target, build an 8-bit hit mask.
int find_avx2(const std::uint64_t* tags, std::uint64_t t) {
    __m256i key = _mm256_set1_epi64x(std::int64_t(t));
    __m256i a   = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(tags));     // ways 0..3
    __m256i b   = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(tags + 4)); // ways 4..7
    unsigned m  = unsigned(_mm256_movemask_pd(_mm256_castsi256_pd(_mm256_cmpeq_epi64(a, key))))
                | (unsigned(_mm256_movemask_pd(_mm256_castsi256_pd(_mm256_cmpeq_epi64(b, key)))) << 4);
    // ...AND with the per-set valid mask, then __builtin_ctz(m) for the way.
    return m ? __builtin_ctz(m) : -1;
}
```

Keep `valid` as an 8-bit mask per set (one bit per way) so you can `m &= valid_mask` after the compare without a second SIMD pass. Now a probe is: two loads, one broadcast, two compares, one mask — no loop, no branch.

> ⚠️ **Portability.** The judge builds with `-march=x86-64-v2` ([`platform_week_2.md`](../../platform_week_2.md)), which includes SSE4.2 but **not** AVX2/AVX-512. Hand-written AVX2 intrinsics may `SIGILL` on that baseline. Options: (a) write SSE-friendly 128-bit intrinsics (2 lanes, still fewer ops than scalar), (b) use `__builtin_cpu_supports("avx2")` to pick a path at run time, or (c) lean on the compiler — a clean branchless scalar loop (§2) often auto-vectorizes to whatever the target allows. **Confirm what the judge's baseline emits before relying on a specific width.**

---

## 4. Prefetch the Next Set

You're streaming an array of accesses, so the next address is known *now*. While you finish access `i`, ask the hardware to pull access `i+K`'s L1 set into cache:

```cpp
constexpr std::size_t PF = 8;                              // tune this distance
if (i + PF < n) {
    const std::uint64_t nb = acc[i + PF].address >> 6;
    __builtin_prefetch(&l1_tag_[(nb & geo::L1_INDEX_MASK) * 8], 0 /*read*/, 3 /*high locality*/);
}
```

The hardware prefetcher already handles the *sequential trace read*, so this mostly helps hide the latency of touching a **scattered simulated set** (random-region traces). Sweep `PF` ∈ {4, 8, 16, 32} and plot throughput — the sweet spot is machine- and trace-dependent, and sometimes the answer is "prefetch hurts, the HW prefetcher had it". Measure.

---

## 5. `__builtin_expect` / `[[likely]]` for the Common Outcome

Most accesses hit L1. Tell the compiler so it lays out the hot path to fall through and pushes the miss-handling code (the cold-ish path *within* the hot loop) out of line:

```cpp
if (w1 >= 0) [[likely]] {            // C++20; or __builtin_expect(w1 >= 0, 1)
    ++s.l1_hits;
    touch_l1(s1, w1, wr);
    continue;
}
// miss handling laid out after the hot fall-through path
```

This improves instruction-cache density and branch prediction on the dominant path. It's a hint — verify with `perf stat -e branch-misses` that it actually helped.

---

## 6. Pack the LRU Update

The LRU update runs on every access; a branchy "find this way, shift the others" is slow. Two fast encodings:

- **Packed nibble order:** store the 8 ways' recency as 8 × 4-bit fields in a `uint32`. "Move way `w` to MRU" becomes a few shifts/masks — no loop, branch-free. A `consteval` table ([`05-compile-time-and-static-polymorphism.md`](./05-compile-time-and-static-polymorphism.md) §2) can even precompute the transition.
- **Per-way age counters / timestamps:** bump a counter on each touch; the LRU victim is the min. Simple, but finding the min is its own 8-way scan.

The packed-nibble approach pairs naturally with the SIMD tag scan: the scan gives you the hit way, a table/shift gives you the new packed order. Both fit in registers.

---

## 7. Software Pipelining: Overlap Independent Accesses

Each access is a dependency chain: load `acc[i]` → derive set → scan tags → branch → update LRU. The chain has *latency* — the scan can't start until the index is ready, the LRU update can't start until the scan finishes. A single chain leaves the CPU's execution units idle waiting on each step.

But access `i+1` is usually **independent** of access `i` (different set, no data dependency). An out-of-order core already exploits some of this by speculating ahead, but you can widen the window by computing the *next* access's cheap, latency-tolerant work early — its index and a prefetch — while access `i`'s expensive tail (miss handling) is still draining:

```cpp
// Compute access i's set, AND kick off i+1's set fetch, so two chains overlap.
std::size_t set = (acc[i].address >> 6) & geo::L1_INDEX_MASK;
if (i + 1 < n) {
    std::size_t next = (acc[i + 1].address >> 6) & geo::L1_INDEX_MASK;
    __builtin_prefetch(&l1_tag_[next * 8]);     // i+1's line on its way while we work on i
}
// ...probe/scan/lru for set...
```

This is hand-assisted **instruction-level parallelism**: you're not running two cores, you're keeping one core's pipeline full by making sure there's always independent work ready. The wins are smaller and flakier than SoA or branchless (the OoO engine already does a lot of this), so this is genuinely last-mile — but on a miss-heavy trace, overlapping the long L2/fill latency of one access with the scan of the next is where a few more percent hides. Measure with `perf stat -e cycles,instructions` (IPC should rise) and revert if it doesn't move.

---

## 8. Don't Reconstruct What You Can Store — the Writeback Address

A spec-specific micro-opt that also kills a whole class of bugs. On an L1 dirty eviction you must find the victim line's **L2 set**, which needs the victim's *block address*. If you stored the tag as "block address with the index bits stripped" ([`02-cache-internals.md`](./02-cache-internals.md) §9), you have to rebuild it:

```cpp
// Stored a stripped tag → reconstruct the victim's block address (shift + or, every dirty evict).
std::uint64_t victim_block = (l1_tag_[base + victim] << 6) | l1_set;
```

If instead you store the **full block address** as the tag (legal — the index bits are constant within a set, so comparisons are unaffected), the victim's block address *is* the stored value:

```cpp
// Stored the full block address as the tag → it IS the block address. No reconstruction.
std::uint64_t victim_block = l1_tag_[base + victim];
```

You trade a few extra tag bits (free — you're already in a 64-bit word) for removing a shift+or from the eviction path *and* removing the single most common writeback bug: getting the reconstruction shift wrong and writing the dirty line back to the wrong L2 set. The counters in `CACHE_SPEC.md` are unforgiving about exactly which L2 set a writeback lands in, so the version that can't get the address wrong is both faster and safer. Storing what you need beats recomputing it — a theme you'll see again with the packed LRU and the compile-time tables.

---

## 9. Read the Assembly — Trust Nothing

Every optimization here is a *hypothesis* until you've seen the asm and the number. Two habits:

```bash
# 1. Disassemble the hot loop and look for: vector compares, no call into malloc,
#    masks as immediates, the 8-way scan unrolled.
objdump -d --no-show-raw-insn build/cache_sim_runner | less    # find your run()

# 2. Annotate by samples — where does run() actually spend cycles?
perf record -F 2999 -g ./build/cache_sim_runner data/large.trace
perf annotate                                                   # hot instructions highlighted
```

And paste the inner loop into [godbolt.org](https://godbolt.org) with **`-O3 -march=x86-64-v2`** (the judge's flags, [`week-1/05-bonus-compiler.md`](../week-1/05-bonus-compiler.md)) — not `-march=native`, or you'll be admiring AVX-512 the judge will never run.

> 💡 The most common bonus-tier disappointment: a beautiful AVX2 routine that's *slower* than the scalar branchless loop on the judge, because the judge's baseline doesn't have AVX2 and the dispatch overhead ate the win. Optimize for the machine that scores you.

---

## 🎯 Key Takeaways

- **Earn it:** SIMD/prefetch only after correct + flat + zero-alloc + compile-time geometry. Measure every change.
- **Branchless tag scan:** build a hit mask with bit ops, `__builtin_ctz` the result — kills the data-dependent branch the predictor hates.
- **SIMD compare:** one (AVX-512) or two (AVX2/SSE) compares cover an 8-way set; keep `valid` as a bitmask to AND in cheaply.
- **Mind the baseline:** the judge is `-march=x86-64-v2` (no AVX2/512). Prefer SSE-width, run-time dispatch, or compiler auto-vectorization; a hand-rolled AVX2 path can `SIGILL` or lose.
- **Prefetch** the set for access `i+K`; helps scattered simulated sets, often not the sequential trace read. Sweep `K`, measure.
- **`[[likely]]`** the L1-hit path for I-cache density and prediction; **pack the LRU** order into a `uint32` for branch-free updates.
- **Software-pipeline** independent accesses (compute/prefetch `i+1` while `i`'s miss drains) to keep the pipeline full — last-mile, IPC-measured, revert if flat.
- **Store, don't reconstruct:** keeping the full block address as the tag makes the writeback's L2 set free to compute and impossible to mis-derive — faster *and* fewer counter bugs.
- **Read the asm** (`perf annotate`, godbolt with judge flags). Every trick is a hypothesis until the number agrees.

---

## 📚 Further Reading — SIMD, Branchless, Microarchitecture

- 🌐 **[Intel Intrinsics Guide](https://www.intel.com/content/www/us/en/docs/intrinsics-guide/index.html)** — search any `_mm`/`_mm256` intrinsic; indispensable for §3.
- 🎬 [Matt Godbolt — "Memory and Caches" / branchless talks](https://www.youtube.com/watch?v=bSkpMdDe4g4) — how the compiler vectorizes and when it won't.
- 📰 [Daniel Lemire's blog](https://lemire.me/blog/) — a goldmine of branchless / SIMD micro-optimization with real numbers (e.g. SIMD set membership, `ctz` tricks).
- 📖 [Agner Fog — optimization manuals](https://www.agner.org/optimize/) — instruction tables and the cost of each intrinsic on each microarchitecture.
- 📰 [Algorithmica — "Algorithms for Modern Hardware" (HPC book, free)](https://en.algorithmica.org/hpc/) — chapters on SIMD, prefetching, and cache-aware code; closest in spirit to this whole week.

---

## ▶️ Next

That's the end of Week 2's reading. You have the full toolkit: hot/cold discipline, the cache machine, locality, zero allocation, compile-time computation, and the SIMD/prefetch bonus tier. Go make [the project](./project/README.md) fast — and watch the leaderboard.

Week 3 is coming soon: we take this layout discipline and add **threads** — ingest on one core, simulation on another, `alignas(64)` to dodge false sharing, and core pinning. The flat, allocation-free state you just built is exactly what survives the jump to parallelism. ⚡
