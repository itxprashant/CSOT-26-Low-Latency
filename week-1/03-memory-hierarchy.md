# 03 — The Memory Hierarchy: Why DRAM Is 200x Slower Than L1

> **TL;DR** — Your CPU runs at ~3 GHz (one cycle ≈ 0.3 ns). DRAM takes ~100 ns to respond. That's **300 wasted cycles** per cache miss. Low-latency code is mostly about *not missing the cache*.

---

## 1. The Problem: CPUs Got Fast, Memory Didn't

Between 1980 and 2020, CPU clock speeds increased by **~1000x** (1 MHz → 3+ GHz, with more work per cycle on top of that). DRAM access latency improved by **~10x**. The gap between them — known as the **memory wall** — keeps growing.

If a CPU had to wait for DRAM on every load, it would spend ~99% of its cycles idle. The industry's solution: **caches**.

A modern CPU is essentially a high-frequency core attached to a *cascade of progressively larger, slower memory tiers*, with hardware doing its best to keep the hot data close to the core.

---

## 2. The Hierarchy

A typical x86 desktop CPU (e.g. Intel i7 / AMD Ryzen) looks like:

```
       ┌─────────────────────────────────────────────────────┐
       │                    CPU CORE                         │
       │  Registers (~16x 64-bit)         ~0 cycles (instant)│
       └────────────────────┬────────────────────────────────┘
                            │ ~1 ns
       ┌────────────────────▼────────────────────────────────┐
       │  L1 Cache         (per-core, split I$/D$, 32-64 KB) │
       │                                  ~4 cycles  / ~1 ns │
       └────────────────────┬────────────────────────────────┘
                            │
       ┌────────────────────▼────────────────────────────────┐
       │  L2 Cache         (per-core, 256 KB – 1 MB)         │
       │                                  ~12 cycles / ~3 ns │
       └────────────────────┬────────────────────────────────┘
                            │
       ┌────────────────────▼────────────────────────────────┐
       │  L3 Cache         (shared, 4–64 MB)                 │
       │                                  ~40 cycles / ~12 ns│
       └────────────────────┬────────────────────────────────┘
                            │
       ┌────────────────────▼────────────────────────────────┐
       │  Main Memory (DRAM, 8–128 GB)                       │
       │                                ~200 cycles / ~80 ns │
       └────────────────────┬────────────────────────────────┘
                            │
       ┌────────────────────▼────────────────────────────────┐
       │  NVMe SSD                                ~100 µs    │
       │  HDD                                     ~10 ms     │
       │  Network (cross-DC)                      ~1-10 ms   │
       │  Network (cross-continent)               ~100 ms    │
       └─────────────────────────────────────────────────────┘
```

### Latency Numbers Every Programmer Should Know (2025 edition)

Burned into your memory, you should be able to recite:

| Operation | Latency | Relative |
|---|---|---|
| 1 CPU cycle | 0.3 ns | 1x |
| L1 cache hit | 1 ns | ~3x |
| Branch mispredict | 3 ns | ~10x |
| L2 cache hit | 3 ns | ~10x |
| L3 cache hit | 12 ns | ~40x |
| Main memory (DRAM) | 80–120 ns | **~300x** |
| Mutex lock/unlock (uncontended) | ~20 ns | ~70x |
| Mutex contended (kernel wake) | ~1-10 µs | ~30,000x |
| Context switch | ~1-5 µs | ~10,000x |
| 1 KB sequential read from DRAM | ~250 ns | ~800x |
| 1 KB random read from DRAM | ~2 µs | ~7000x |
| 1 KB read from NVMe SSD | ~10 µs | ~30,000x |
| Round-trip in same DC | ~500 µs | ~1.5M x |
| Round-trip Mumbai → Singapore | ~60 ms | ~200M x |
| Round-trip Mumbai → New York | ~250 ms | ~800M x |

(Based on Jeff Dean's original list, updated for modern hardware. The canonical sources are in *Further Reading* at the bottom of this page.)

**The single most important number on this list:** L1 hit ≈ **1 ns**, DRAM ≈ **100 ns**. A **100x gap** for accessing the same kind of thing.

---

## 3. Cache Lines — The Unit of Memory Transfer

Caches don't store individual bytes — they store **cache lines**, which are **64 bytes** on virtually all modern x86 and ARM CPUs.

When you read 1 byte from DRAM:
1. The CPU loads the **entire 64-byte cache line** containing that byte into L1.
2. The next 63 nearby bytes are now "free" — accessing them is a cache hit (~1 ns).

When you read 1 byte that's not in cache and is far from anything else cached:
1. The CPU evicts some cache line from L1 to make room.
2. The chosen victim line gets pushed down to L2 (and maybe L3).
3. The requested line is fetched from L2 / L3 / DRAM (depending on where it is).
4. You wait, possibly hundreds of cycles.

This is why **64-byte alignment** and **cache-line-sized structures** matter — and why "false sharing" (Week 3) is a thing.

### Inspecting Your Caches

```bash
# Check your CPU's cache sizes
lscpu | grep cache

# Or more detail:
getconf -a | grep CACHE

# Or the gory details:
cat /sys/devices/system/cpu/cpu0/cache/index*/size
cat /sys/devices/system/cpu/cpu0/cache/index*/coherency_line_size  # always 64 on x86
```

Example output from a Ryzen 7 5800X:
```
L1d cache:   32 KiB (8 instances)        ← 32 KB data cache per core
L1i cache:   32 KiB (8 instances)        ← 32 KB instruction cache per core
L2 cache:    512 KiB (8 instances)       ← 512 KB per core
L3 cache:    32 MiB (1 instance)         ← 32 MB shared across all 8 cores
```

---

## 4. Spatial and Temporal Locality

Caches are a *bet*: the hardware bets that the bytes you'll need next are either *near* the bytes you just touched (**spatial locality**) or *the same* bytes you touched recently (**temporal locality**).

Your job as a programmer: **make that bet a winning one**.

### Spatial Locality — Iterate Sequentially

```cpp
// Good: walk a vector in order. Every cache line is fully consumed before eviction.
for (int x : vec) sum += x;

// Bad: skipping through with stride 16 = essentially one cache miss per element
for (size_t i = 0; i < vec.size(); i += 16) sum += vec[i];
```

The "bad" version reads only 4 bytes out of every 64-byte line — burning 15/16 of the memory bandwidth.

### Temporal Locality — Reuse While Hot

```cpp
// Good: process each chunk fully before moving on
for (auto& chunk : chunks) {
    auto sum = std::accumulate(chunk.begin(), chunk.end(), 0);
    save(sum);
}

// Bad: pass over all chunks, then pass over them all again. The first chunk
// has long since been evicted from cache by the time the second loop starts.
for (auto& chunk : chunks) prepare(chunk);
for (auto& chunk : chunks) process(chunk);
```

For huge datasets, the "bad" version triggers a full cache reload on the second loop.

### Array-of-Structs (AoS) vs. Struct-of-Arrays (SoA)

This is one of the most important layout decisions in performance work.

**AoS** — the obvious layout:

```cpp
struct Particle { float x, y, z; float vx, vy, vz; float mass; };
std::vector<Particle> particles;       // [p0 | p1 | p2 | ...]
```

**SoA** — flip the matrix:

```cpp
struct Particles {
    std::vector<float> x, y, z;
    std::vector<float> vx, vy, vz;
    std::vector<float> mass;
};
```

If a function only reads `x`, `y`, `z` (e.g. compute center of mass), SoA reads **only the data it needs**, fully utilizing every cache line. AoS pulls in `vx, vy, vz, mass` too — wasted bandwidth.

**SoA also enables SIMD vectorization** much more easily, because the data the CPU's vector unit wants to load is already laid out as `[x0, x1, x2, x3, ...]`.

Most low-latency trading systems use SoA for hot data.

---

## 5. Cache Coherency, Briefly

When multiple cores share data, the hardware maintains a **coherency protocol** (typically MESI: Modified, Exclusive, Shared, Invalid). Whenever a core writes to a cache line, all other cores' copies of that line are invalidated.

**Why you care:** if two threads on two cores are constantly writing to *different* variables that happen to live on the *same cache line*, the cache line bounces back and forth between cores, taking dozens of ns per invalidation. This is **false sharing** and we'll exploit / avoid it in Week 3.

---

## 6. TLB & Page Walks (a brief note)

Virtual memory means the CPU translates every address through a page table. The **TLB** (translation lookaside buffer) caches recent translations. A TLB miss costs ~10-100 ns extra. For huge working sets, use **huge pages** (2 MB instead of 4 KB) to reduce TLB pressure. We'll touch this in Week 5.

---

## 7. Putting It Together: The Latency-Aware Mental Model

When you write a loop, ask:

1. **What's the size of my working set?** Does it fit in L1 (32 KB)? L2 (512 KB)? L3 (16-32 MB)? Or am I streaming from DRAM?
2. **Am I accessing memory sequentially?** If yes, the hardware prefetcher will hide most of the cost. If no (random / pointer-chasing), every access could be a miss.
3. **Are my hot fields packed together?** If a struct is 200 bytes but you only read 8 of them, you're wasting 75% of the cache line.
4. **Am I writing data that other threads will read?** That cache line might bounce between cores.

You will return to these four questions for the rest of your engineering life.

---

## 8. A Demo: The Cache Cliff

Try this experiment (we'll do a polished version in `04-benchmarking-tools.md`):

```cpp
#include <vector>
#include <chrono>
#include <iostream>

void measure(size_t N) {
    std::vector<int> v(N, 1);
    long sum = 0;
    auto t1 = std::chrono::steady_clock::now();
    for (int rep = 0; rep < 1000; ++rep) {
        for (size_t i = 0; i < N; ++i) sum += v[i];
    }
    auto t2 = std::chrono::steady_clock::now();
    double ns_per_elem = std::chrono::duration<double, std::nano>(t2-t1).count() / (1000.0 * N);
    std::cout << "N=" << N*4 << " bytes  " << ns_per_elem << " ns/elem  sum=" << sum << "\n";
}

int main() {
    for (size_t N = 1<<10; N <= 1<<24; N <<= 1) measure(N);
}
```

Plot the result with N on x-axis (log scale) and ns/elem on y-axis. You will see **three distinct plateaus** corresponding to data fitting in L1, L2, and L3, with sharp "cliffs" between them.

That graph is the **shape of your machine**. Once you've seen it, you can never un-see it.

---

## 🎯 Key Takeaways

- The memory hierarchy spans **6+ orders of magnitude** from registers to network.
- L1 hit ≈ 1 ns; DRAM ≈ 100 ns. A 100x cliff that dominates the cost of memory-bound code.
- **Cache lines are 64 bytes.** All memory moves in 64-byte chunks.
- Optimize for **spatial locality** (sequential access) and **temporal locality** (reuse hot data).
- **Struct-of-Arrays** often beats Array-of-Structs for vector-style workloads.
- Different cores fighting over the same cache line = **false sharing** (more in Week 3).

---

## 📚 Further Reading — Latency Numbers, Caches, Memory

### Latency Numbers

- 📌 **[Latency Numbers Every Programmer Should Know — Jeff Dean (canonical list)](https://gist.github.com/jboner/2841832)**
- 🎯 **[Interactive version, by year (1990–2020)](https://colin-scott.github.io/personal_website/research/interactive_latency.html)** — slide the year and watch the numbers change. Best link in this whole curriculum.
- 📄 [Brendan Gregg — "Linux Performance Tools" (FOSDEM)](http://www.brendangregg.com/Articles/Methodology_LinuxPerformanceTools_FOSDEM2017.pdf) — slides covering the full hierarchy from caches to disk.

### Caches & Memory Hierarchy

- 📖 **Ulrich Drepper — ["What Every Programmer Should Know About Memory"](https://people.freebsd.org/~lstewart/articles/cpumemory.pdf)** (PDF, ~120 pages). The single best resource on CPU memory. Dense but worth it — at least skim Sections 2-3.
- 📰 **[Igor Ostrovsky — "Gallery of Processor Cache Effects"](https://igoro.com/archive/gallery-of-processor-cache-effects/)** — 7 short demos showing cache behaviour in 50 lines of C# each. **Read all of them.** They will recalibrate your intuition.
- 🎬 [Scott Meyers — "CPU Caches and Why You Care"](https://www.youtube.com/watch?v=WDIkqP4JbkE) (1h) — the clearest single explanation of memory hierarchy ever recorded.

---

## ▶️ Next

[`04-benchmarking-tools.md`](./04-benchmarking-tools.md) — now let's actually *measure* this stuff properly with Google Benchmark, `perf`, and `rdtsc`.
