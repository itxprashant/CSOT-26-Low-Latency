# 04 — False Sharing: When "Private" Data Isn't, and 4 Threads Lose to 1

> **TL;DR** — Caches move data in 64-byte lines, and coherency is tracked per *line*, not per *variable*. If two threads write to different variables that happen to live on the **same** cache line, the hardware ping-pongs that line between cores on every write — even though the threads never touch the same byte. This "false sharing" can make a 4-thread reduction *slower* than 1 thread. The fix: pad/align each thread's hot data to its own cache line with `alignas(64)`. This is the single most important lesson of Week 3.

[`03-data-races.md`](./03-data-races.md) said: give each thread its own partial table and there's no race. True — but if those tables are packed next to each other in memory, you've traded a correctness bug for a performance catastrophe. This is the bug that makes people say "threads made it slower."

---

## 1. Caches are coherent per line, not per byte

Recall from Week 2: memory moves in **64-byte cache lines**, and each core has its own L1/L2. When multiple cores cache the same line, the hardware **cache-coherency protocol** (MESI and friends) keeps them consistent: the moment one core *writes* a line, every other core's copy of that line is **invalidated**. The next time another core touches *anything* on that line, it must re-fetch it.

The crucial detail: invalidation is per **line**, not per **variable**. The hardware does not know or care that two cores are writing different `int`s — if those `int`s share a 64-byte line, each write kicks the line out of the other core's cache.

---

## 2. The classic demonstration

Four threads, four counters, each thread bumps its own counter a hundred million times. No data race — each touches only its own index. Yet layout decides whether it flies or crawls.

```cpp
#include <thread>
#include <vector>
#include <cstdint>

struct Bad  { std::int64_t c; };                 // 8 bytes — four fit in ONE 64B line
struct alignas(64) Good { std::int64_t c; };     // padded to its own 64B line

template <class Counter>
void run4(Counter* ctrs) {
    auto bump = [](std::int64_t* p){ for (int i = 0; i < 100'000'000; ++i) ++*p; };
    std::vector<std::thread> ts;
    for (int k = 0; k < 4; ++k) ts.emplace_back(bump, &ctrs[k].c);
    for (auto& t : ts) t.join();
}
```

`Bad ctrs[4]` packs all four counters into a single cache line. The four cores fight over that one line — every increment on any core invalidates the other three. `Good ctrs[4]` puts each counter on its own line; the cores never interfere.

Typical numbers on a 4-core desktop:

| Layout | Time | vs. ideal |
|---|---:|---|
| `Bad` (shared line) | ~2.4 s | **slower than single-threaded** |
| `Good` (`alignas(64)`) | ~0.25 s | ~4× speedup, as expected |

A ~10× difference from **one keyword**, with identical logic and zero data races. That is false sharing.

---

## 3. Why it's called *false* sharing

The threads aren't *actually* sharing data — each writes a distinct variable. The *sharing* is an illusion created by the **layout**: the variables co-reside on a line, so the coherency hardware treats independent writes as if they were conflicting. The CPU does a ton of invalidate-and-refetch work to keep a line consistent that never needed to be shared in the first place.

```text
        cache line (64 bytes)
        ┌───────┬───────┬───────┬───────┬─────────────────┐
 Bad:   │ c[0]  │ c[1]  │ c[2]  │ c[3]  │  ... unused ...  │   one line, 4 cores fight
        └───────┴───────┴───────┴───────┴─────────────────┘
          T0      T1      T2      T3

 Good:  [ c[0] + 56B pad ][ c[1] + 56B pad ][ c[2] ... ]        one line each, no fight
          T0                T1                T2
```

---

## 4. Where it bites the aggregator

Your worker threads each write a *partial table* of 1024 `SymbolAgg` rows. Two ways to store them:

```cpp
// (A) DANGER: tables packed back-to-back. The last rows of partial k and the
//     first rows of partial k+1 share lines -> false sharing at the boundary,
//     and worse if you index as partials[symbol][thread].
std::vector<csot::SymbolAgg> flat(T * NUM_SYMBOLS);

// (B) SAFE: each thread's whole table starts on its own cache line.
struct alignas(64) PartialTable { csot::SymbolAgg rows[NUM_SYMBOLS]; };
std::vector<PartialTable> partials(T);     // each PartialTable is 64B-aligned
```

Layout (B) aligns each *table* to a line so no two threads' tables overlap a line. Because each table is `1024 * 40 = 40 KiB` (many lines), the only risk was the boundary between tables, and `alignas(64)` removes it. The within-a-table rows are only touched by that table's single owning thread, so they're safe regardless.

> ⚠️ The truly nasty version is `partials[symbol][thread]` (symbol-major). Then thread 0's `count` for symbol 7 sits one `int64` away from thread 1's `count` for symbol 7 — adjacent in memory, written by different cores, *every* update false-shares. Always make the **thread** the outer dimension and align per thread.

The smallest, most general guard for a single hot per-thread value (a running accumulator, a flag) is to wrap it:

```cpp
struct alignas(64) PaddedCounter {
    std::int64_t value = 0;
    char _pad[64 - sizeof(std::int64_t)];   // fill the rest of the line
};
```

C++17 even gives you the magic number: `std::hardware_destructive_interference_size` (typically 64) is the "align to this to avoid false sharing" constant.

---

## 5. Seeing it with `perf`

You don't have to guess. False sharing shows up as a storm of cache-coherency traffic:

```bash
# Build both layouts, then compare. The bad one shows huge cache-miss / HITM counts.
perf stat -e cache-misses,L1-dcache-load-misses,context-switches ./bad
perf stat -e cache-misses,L1-dcache-load-misses,context-switches ./good
```

The `Bad` build reports orders of magnitude more `cache-misses` despite doing identical arithmetic. On Intel, the smoking gun is `mem_load_l3_hit_retired.xsnp_hitm` ("a load hit a line another core had modified") — pure coherency ping-pong. If `perf stat` shows your 4-thread aggregator missing cache far more than your 1-thread one, you have false sharing.

> 💡 Plot it: run your aggregator at 1–4 threads with the packed vs. padded partials and feed the throughput to [`tools/plot_scaling.py`](./project/tools/plot_scaling.py). The padded curve climbs toward 4×; the packed one flatlines or dips. Seeing the two curves side by side is the lesson made visible.

---

## 6. The rule

1. Any data written by **one** thread and read/written by **another** must not share a cache line with the first thread's hot data.
2. Give each thread's hot, frequently-written state **its own cache line(s)**: `alignas(64)` the per-thread struct (or pad it to 64 bytes).
3. Make the **thread index the outer dimension** of any `[thread][...]` array, never interleave per-thread values of the same field.
4. Verify with `perf stat -e cache-misses` (and, on Intel, the HITM event) — the padded version misses far less.

Do this and your map-reduce finally scales the way [`01-going-wide.md`](./01-going-wide.md) promised. Skip it and you'll "prove" that threads are useless — a famous, recurring, self-inflicted wound.

---

## 🎯 Key Takeaways

- Cache coherency is tracked per **64-byte line**, not per variable. One core writing a line **invalidates** every other core's copy of that whole line.
- **False sharing:** independent variables on the same line cause cores to ping-pong it on every write, with no actual data sharing. It can make N threads slower than 1.
- One `alignas(64)` can be a ~10× swing with identical logic and zero races.
- In the aggregator, store partials **thread-major** and align each thread's table to a cache line; never interleave per-thread values of the same field (symbol-major is the trap).
- Use `alignas(64)` / `std::hardware_destructive_interference_size`; pad hot single counters to a full line.
- **Diagnose with `perf`:** false sharing = a spike in `cache-misses` and (Intel) HITM events. Plot throughput vs. threads to see the flatline.

---

## 📚 Further Reading — False Sharing

- 🎬 [CppCon — Timur Doumler, "C++ atomics, from basic to advanced"](https://www.youtube.com/watch?v=ZQFzMfHIxng) — includes a crisp false-sharing segment and the alignment fix.
- 📰 [Mechanical Sympathy — "False Sharing" (Martin Thompson)](https://mechanical-sympathy.blogspot.com/2011/07/false-sharing.html) — the canonical write-up, with cache-line padding numbers.
- 📰 [Intel — "Avoiding and Identifying False Sharing Among Threads"](https://www.intel.com/content/www/us/en/developer/articles/technical/avoiding-and-identifying-false-sharing-among-threads.html) — how to spot it in VTune/perf and pad it away.
- 📖 [cppreference — `std::hardware_destructive_interference_size`](https://en.cppreference.com/w/cpp/thread/hardware_destructive_interference_size) — the standard's "align to this" constant.

---

## ▶️ Next

[`05-scheduler-and-pinning.md`](./05-scheduler-and-pinning.md) — your tables are padded and your threads fly... until the OS migrates one to a different core mid-sweep. Pinning threads to cores to kill scheduler jitter.
