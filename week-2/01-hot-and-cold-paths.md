# 01 — Hot and Cold Paths: The Work You Time vs. the Work You Hide

> **TL;DR** — Every latency-critical program has a tiny **hot path** that runs millions of times and a **cold path** that runs once. The entire craft of low-latency engineering is moving work *out* of the hot path and *into* the cold path. In Week 2 the hot path is `CacheSim::run()`; the cold path is `on_init()`. Everything you can do once, do in `on_init()`.

Week 1 ended with a working measurement harness: you could time a hot loop and trust the numbers ([`week-1/04-benchmarking-tools.md`](../week-1/04-benchmarking-tools.md)). Now we get specific about *what* belongs in that loop and what must be evicted from it.

---

## 1. What Is a Hot Path?

A **hot path** is the code that executes on every unit of work: every tick, every packet, every memory access. If your program processes 100 million events, the hot path runs 100 million times. A cold path runs once (or rarely): startup, configuration, allocation, teardown.

The asymmetry is brutal. Shaving 1 ns off a hot path that runs 10⁸ times saves 100 ms. Shaving 1 *second* off a cold path that runs once saves... 1 second, once. So you spend your optimization budget where the multiplier lives.

```cpp
void on_init() {
    // COLD: runs once. Spend freely here.
    tags_.resize(L1_SETS * L1_WAYS);     // allocate
    std::fill(valid_.begin(), valid_.end(), false);
}

csot::CacheStats run(const csot::MemAccess* acc, std::size_t n) {
    csot::CacheStats s{};
    for (std::size_t i = 0; i < n; ++i) {   // HOT: runs n times. Every ns counts.
        // ... the per-access logic ...
    }
    return s;
}
```

The shipped ABI ([`project/include/cache_sim.hpp`](./project/include/cache_sim.hpp)) hands you this split for free: `on_init()` is your cold path, `run()` is your hot path, and the judge times *only* `run()`.

---

## 2. The Litmus Test: Does It Depend on the Input Element?

Here's the one question that classifies any line of code: **does this work have to happen for *this specific* access, or could it have happened before the loop started?**

| Work | Depends on the element? | Where it belongs |
|---|---|---|
| `malloc` of the tag arrays | No — size is known from the spec | `on_init` (cold) |
| Zeroing the valid bits | No | `on_init` (cold) |
| Computing the index mask `SETS - 1` | No — it's a constant | compile time |
| `acc[i].address >> 6` | Yes — it's this access's address | `run` (hot) |
| Probing the set for a tag | Yes | `run` (hot) |

If the answer is "no", and you're doing it inside the loop, you have a bug — a performance bug, but a bug. The classic offenders: allocating scratch space per iteration, recomputing a constant, re-deriving something you could have cached, or calling into a function the compiler can't inline.

---

## 3. Hoisting: Move It Up and Out

**Hoisting** is the act of lifting invariant work out of a loop. Compilers do a lot of it (loop-invariant code motion), but they can only hoist what they can *prove* is invariant — and a surprising amount of code defeats that proof.

```cpp
// SLOW: the compiler may not prove sets_ is loop-invariant if it can't see
// through the member access / aliasing.
for (std::size_t i = 0; i < n; ++i) {
    std::size_t set = (acc[i].address >> 6) % sets_.size();   // division every iter!
    // ...
}

// FAST: hoist the constant; use a mask instead of a modulo.
const std::uint64_t index_mask = sets_count_ - 1;             // power of two
for (std::size_t i = 0; i < n; ++i) {
    std::size_t set = (acc[i].address >> 6) & index_mask;     // one AND
    // ...
}
```

A `%` by a runtime value is a hardware division — **20–40 cycles**. A `&` with a power-of-two mask is **1 cycle**. The cache geometry is fixed and power-of-two on purpose (64 sets, 512 sets) so you can always mask. That single change can be a 2× on a tight simulator loop.

---

## 4. The Cold Path Is Not Free of Discipline

"Do it in `on_init`" is not "do it sloppily". Two cold-path rules matter:

1. **Allocate once, in `on_init`, then never again.** The hot path must not touch the heap (Week 2's [`04-zero-allocation.md`](./04-zero-allocation.md) is the whole story). If you `resize` a vector inside `run()`, you may trigger a reallocation — a `malloc`, a `memcpy`, and a cache-trashing copy — at an unpredictable moment. That shows up as a p99 spike.

2. **Lay the cold-path allocations out for the hot path.** *Where* `on_init` puts your data determines how fast `run` reads it. Allocating 4096 separate little nodes (one per set) scatters them across DRAM; allocating one flat array keeps them contiguous. Same cold-path code, wildly different hot-path latency. That's the bridge to [`03-locality.md`](./03-locality.md).

---

## 5. The Same Idea, Everywhere

This hot/cold split is not a cache-sim quirk — it is *the* recurring shape of low-latency systems, and you'll meet it every week:

| Context | Cold path (once) | Hot path (timed) |
|---|---|---|
| Week 1 strategy | `on_init` builds per-symbol state | `on_tick` — judge times this |
| Week 2 cache sim | `on_init` allocates tag/LRU arrays | `run` — judge times this |
| Week 3 threads | spawn + pin threads, size queues | per-item processing on each thread |
| Week 5 networking | `socket`/`bind`/`epoll_create` | the `epoll_wait` → parse → react loop |

> 💡 When you look at *any* performance problem, the first question is "what's the hot path, and what work is in it that doesn't belong?" Answer that and you've done half the job before writing a line of optimized code.

In Week 1 the hot path returned a `std::vector<Order>` — which *allocates*. That was a deliberate simplification we flagged at the time. The cache sim's `run()` returns a single `CacheStats` by value (56 bytes, no allocation), so the ABI itself keeps you honest: there is nowhere to accidentally allocate in the return path.

---

## 6. A Worked Micro-Example

Two versions of the same per-access set probe. Same result, different hot-path cost.

```cpp
// Version A — cold work leaking into the hot path
csot::CacheStats run(const csot::MemAccess* acc, std::size_t n) {
    csot::CacheStats s{};
    for (std::size_t i = 0; i < n; ++i) {
        std::vector<std::uint64_t> set_tags(8);            // (!) allocates every iter
        std::size_t set = (acc[i].address >> 6) % 64;      // (!) modulo every iter
        load_set(set, set_tags);
        // ...
    }
    return s;
}

// Version B — hot path does only per-access work
void on_init() { tags_.assign(64 * 8, 0); valid_.assign(64 * 8, 0); }  // once

csot::CacheStats run(const csot::MemAccess* acc, std::size_t n) {
    csot::CacheStats s{};
    constexpr std::uint64_t kIndexMask = 64 - 1;
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t set = (acc[i].address >> 6) & kIndexMask;   // 1 cycle
        const std::uint64_t* set_tags = &tags_[set * 8];              // no copy
        // ...
        (void)set_tags;
    }
    return s;
}
```

Version A allocates and frees a vector `n` times and does a hardware division `n` times. On a 100M-access trace that's hundreds of millions of mallocs. Version B does neither. You will routinely see **5–20×** between these two on the leaderboard, and it's *entirely* about what lives in the hot path.

---

## 7. Warm-Up: Pay First-Touch Costs in the Cold Path

There's a subtler cold/hot leak than allocation: **first-touch page faults**. When you allocate a buffer, the OS often hands you virtual address space without backing physical pages — the page is mapped lazily, on the *first write*, via a minor page fault (~1–3 µs each). If that first write happens inside `run()`, you've smuggled microsecond stalls into the timed path.

The good news: `std::vector::assign(n, 0)` and a zero-initialized `std::array<…> x{}` both actually *write* every byte, so the pages are committed during construction / `on_init` — before the clock starts. The trap is the "I'll just `reserve()` and fill later" pattern:

```cpp
void on_init() {
    tags_.reserve(L1_SETS * WAYS);   // (!) reserves address space, touches no pages
}                                     //     → the first writes happen in run() → faults there

void on_init_correct() {
    tags_.assign(L1_SETS * WAYS, 0);  // writes every element now → pages committed in the cold path
}
```

If you ever manage raw memory (an arena, an `mmap`'d region), **pre-fault it** in `on_init` with a `std::memset` or a strided write of one byte per 4 KiB page. The principle generalizes: anything that costs the *first* time — page faults, lazy TLB fills, a cold instruction cache — should be paid during warm-up, not while the judge is timing you.

```cpp
// Pre-fault a raw region so run() never eats a minor fault.
std::memset(region, 0, region_bytes);    // commit every page in the cold path
```

> 💡 This is also why a good local harness runs `run()` once to warm up and *then* times a second run — but the judge constructs a fresh sim, so for the contest your `on_init` must do the warming. Don't rely on a throwaway first call you won't get.

---

## 8. A Back-of-the-Envelope Cost Model

Before micro-optimizing, write down where the time goes. For a trace of `n` accesses, total `run()` time is roughly:

```text
T ≈ n × ( c_load + c_index + c_scan + c_lru + missrate × c_miss )
```

- `c_load` — pulling `acc[i]` from the streaming trace. The hardware prefetcher nails the sequential read, so this is ~free per access.
- `c_index` — `>>` and `&` to derive set/tag. ~1–2 cycles (immediates, §3).
- `c_scan` — the 8-way tag compare. The fattest term; branchless/SIMD attacks it ([`06-bonus-simd-and-prefetch.md`](./06-bonus-simd-and-prefetch.md)).
- `c_lru` — the recency update on every demand access.
- `missrate × c_miss` — miss handling (L2 probe, fills, writebacks). Only paid on the *minority* of accesses.

The lesson hides in that last term: on a realistic trace most accesses **hit L1**, so `missrate` is small and the hit path dominates total time. Spend your budget making the *common* outcome — index, scan, LRU-touch, continue — as tight as possible; don't pour hours into a branch that runs 2% of the time. (This is exactly why §5 of [`06`](./06-bonus-simd-and-prefetch.md) marks the L1-hit path `[[likely]]`.) Measure your real miss rate first (it's right there in the counters: `l1_misses / (reads + writes)`), then optimize the term that actually has the multiplier.

---

## 🎯 Key Takeaways

- The **hot path** runs per-element (per access); the **cold path** runs once. Optimization budget goes where the multiplier is.
- The classifying question: *does this work depend on the current element?* If not, hoist it to `on_init` or compile time.
- A `%` by a runtime value is a 20–40 cycle division; a `&` with a power-of-two mask is 1 cycle. The geometry is power-of-two so you can always mask.
- **Allocate once in `on_init`, never in the hot path** — heap traffic in `run()` is the classic p99 spike.
- *Where* the cold path allocates determines how fast the hot path reads. Contiguous beats scattered.
- **First-touch page faults** are a hidden hot-path cost; commit/pre-fault every buffer in `on_init` (`assign`/`memset`), never `reserve`-then-write-in-`run`.
- Write down the **cost model** (`T ≈ n × (index + scan + lru + missrate × miss)`); most accesses hit L1, so optimize the common hit path, not the rare miss handler.
- The hot/cold split is universal: strategy `on_tick`, cache `run`, the Week-5 `epoll` loop — same shape.

---

## 📚 Further Reading — Hot Paths & Loop Optimization

- 🎬 [Carl Cook — "When a Microsecond Is an Eternity"](https://www.youtube.com/watch?v=NH1Tta7purM) (CppCon 2017) — the canonical talk on keeping the hot path lean in HFT; the "do nothing on the hot path" framing.
- 📰 [Agner Fog — "Optimizing software in C++"](https://www.agner.org/optimize/optimizing_cpp.pdf) §7 (loop optimizations, code motion) — what the compiler hoists and what it can't.
- 🌐 [Compiler Explorer (godbolt.org)](https://godbolt.org/) — paste both versions of §6 and watch the allocation/division vanish.

---

## ▶️ Next

[`02-cache-internals.md`](./02-cache-internals.md) — now that you know *where* to put the work, we open up the cache itself: lines, sets, ways, LRU, and write-back — the exact machine your `run()` has to simulate.
