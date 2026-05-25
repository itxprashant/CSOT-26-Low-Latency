# 01 — Introduction: Latency, Throughput, and the Machine's Worldview

> **TL;DR** — *Latency* is how long one thing takes. *Throughput* is how many things per second. They are not the same, optimizing one can hurt the other, and the difference between them is the difference between an HFT firm and a Hadoop cluster.

---

## 1. What Is "Low Latency", Really?

When we say a system is **low latency**, we mean the time between a *request* and its *response* is short — often microseconds (µs) or even nanoseconds (ns).

Compare:

| Domain | Typical "good" latency |
|---|---|
| Web request (browser → server → browser) | 100-500 **ms** |
| Database query (over LAN) | 1-10 **ms** |
| In-memory cache lookup (Redis over network) | 0.5-1 **ms** |
| RPC call within a data center | 50-500 **µs** |
| HFT order-book update → trade decision | 1-10 **µs** |
| Single L1 cache access | ~1 **ns** |

Notice the units change. A "fast" web request and a "fast" HFT decision differ by **six orders of magnitude**. That's the difference between a heartbeat and a year.

This track focuses on the **µs and ns** regime. At that scale, the *implementation language*, the *memory layout*, the *CPU's pipeline*, the *kernel scheduler*, and even *which physical core a thread runs on* all matter.

---

## 2. Latency vs. Throughput — They Are Not the Same

This confuses everyone at first. Let's nail it down.

- **Latency** = time for *one* unit of work. Measured in seconds (or ns/µs/ms).
- **Throughput** = how many units of work per unit time. Measured in ops/sec, requests/sec, GB/s, etc.

### The Highway Analogy

Imagine a highway between two cities, 100 km apart.

- A **sports car** travelling at 200 km/h has **low latency** — one trip takes 30 minutes. But only 1 person arrives every 30 minutes → throughput = 2 people/hour.
- A **freight train** travelling at 60 km/h with 1,000 passengers has **high latency** — one trip takes ~1.7 hours per passenger. But every hour, *thousands* of passengers arrive at the destination → throughput = ~600 people/hour.

The train *moves more people* but each individual waits longer. **Throughput is amortized; latency is felt.**

### Why The Difference Matters

| System | Optimizes for |
|---|---|
| HFT trading | **Latency** — being 1 µs late = your trade is gone |
| Video streaming (Netflix CDN) | **Throughput** — serve millions of viewers in parallel |
| Online game tick loop | **Latency** + bounded jitter |
| Batch ML training | **Throughput** — total GPU-hours matter, not per-batch latency |
| Database OLTP | Both, with **tail latency** (p99) being critical |

### Can You Improve Both Simultaneously?

Sometimes — e.g. removing a real bottleneck helps both. But often they **trade off**:

- **Batching** improves throughput (amortize fixed costs) but hurts latency (you wait for the batch to fill).
- **Speculative execution** improves latency on average but wastes CPU → hurts throughput.
- **Adding threads** improves throughput on parallel work but hurts latency on the synchronization path.

You will design these trade-offs every week of this track.

### Tail Latency (the Boss Final Form)

Average latency is a lie. In a real system, the **distribution** matters. We talk about:

- **p50 (median)** — half of requests are faster.
- **p99** — 99% of requests are faster (i.e., the worst 1%).
- **p999 (or p99.9)** — the worst 1 in 1000.

A system with p50 = 1 µs and p99 = 10 ms is **broken** for HFT. The "tail" is what kills you. We will measure tail latency throughout this track.

---

## 3. Memory Is Just a Big Addressable Array

Strip away every abstraction — classes, objects, garbage collectors, virtual memory — and at the bottom there is **one** data structure: a giant, contiguous, addressable array of bytes.

```
Address:    0      1      2      3      4      5      6      7    ...   2^48-1
          ┌──────┬──────┬──────┬──────┬──────┬──────┬──────┬──────┐
RAM:      │ 0x4F │ 0x00 │ 0xAB │ 0xCD │ 0x12 │ 0x34 │ 0xDE │ 0xAD │  ...
          └──────┴──────┴──────┴──────┴──────┴──────┴──────┴──────┘
```

Every variable in your program — `int x = 42`, the `this` pointer of an object, the bytecode of a Python interpreter — lives at some byte address. A **pointer** is just a 64-bit integer that holds such an address.

```cpp
int x = 42;
int* p = &x;   // p holds the address (e.g. 0x7fff5fbff8ac) where x lives
```

### Why You Should Care

Because the hardware that fetches bytes from memory is **physical**, with physical constraints:

1. The memory bus carries a fixed number of bytes per clock cycle.
2. DRAM is on a separate chip from the CPU — electrons have to *travel*.
3. The CPU is **vastly** faster than DRAM, so it has small high-speed caches (L1, L2, L3) on the same die.
4. Caches load data in **cache lines** of 64 bytes — not one byte at a time.

Result: **the layout of your data in this big array dramatically affects performance.**

A `std::vector<int>` stores its 1,000 ints in one contiguous block. Iterating it sequentially is *blazing fast* because the CPU pre-fetches the next cache line while you're working on the current one.

A `std::list<int>` stores each int in a separate heap-allocated node, scattered across memory. Iterating it is *painfully slow* because every pointer dereference is a potential cache miss — the CPU has to wait for DRAM.

> **This is the single most important idea in low-latency programming.** We will explore it in depth in [`02-memory-model.md`](./02-memory-model.md) and [`03-memory-hierarchy.md`](./03-memory-hierarchy.md).

### A Quick Demo (try this!)

```cpp
#include <vector>
#include <list>
#include <chrono>
#include <numeric>
#include <iostream>

int main() {
    constexpr size_t N = 10'000'000;

    std::vector<int> v(N, 1);
    std::list<int>   l(N, 1);

    auto t1 = std::chrono::steady_clock::now();
    long sv = std::accumulate(v.begin(), v.end(), 0L);
    auto t2 = std::chrono::steady_clock::now();
    long sl = std::accumulate(l.begin(), l.end(), 0L);
    auto t3 = std::chrono::steady_clock::now();

    std::cout << "vector sum=" << sv << " took "
              << std::chrono::duration_cast<std::chrono::milliseconds>(t2-t1).count() << " ms\n";
    std::cout << "list   sum=" << sl << " took "
              << std::chrono::duration_cast<std::chrono::milliseconds>(t3-t2).count() << " ms\n";
}
```

Compile with `g++ -O2 demo.cpp -o demo` and run. On a typical laptop you'll see the `list` is **20-100x slower** despite doing the same number of additions. Same operations, same big-O, completely different runtime. **Welcome to systems engineering.**

---

## 4. Benchmarking — How We Will Measure Everything

Intuition is useless in low-latency work. **Always measure.** This subsection is a teaser; full deep-dive in [`04-benchmarking-tools.md`](./04-benchmarking-tools.md).

The three tools you'll use this week:

1. **`std::chrono::steady_clock`** — easy, portable, ~50 ns resolution. Good enough for ms-scale work.
2. **`rdtsc`** (Read Time-Stamp Counter) — a CPU instruction that returns the cycle counter. Sub-nanosecond resolution. Tricky but essential.
3. **Google Benchmark** — a C++ library that runs your code many times, warms up caches, accounts for noise, and gives you statistically meaningful numbers.
4. **`perf`** — a Linux tool that uses CPU performance counters to tell you *why* your code is slow (cache misses, branch mispredicts, etc.).

### Naive Benchmark = Misleading Benchmark

A common beginner mistake:

```cpp
auto t1 = clock();
my_function();
auto t2 = clock();
std::cout << (t2 - t1);   // ← garbage number
```

This is wrong because:
- A single call is dominated by noise (interrupts, cache state, branch predictor state).
- The compiler may eliminate the call entirely if the result is unused.
- The CPU's frequency scaling (turbo boost) may have shifted between `t1` and `t2`.

The right way: run it **millions of times**, discard warm-up iterations, *use the result so the compiler can't optimize it away*, and report the **distribution** (min, median, p99) rather than the mean.

Google Benchmark handles all of this. We'll set it up in `04-benchmarking-tools.md`.

---

## 5. The Mental Model Going Forward

For the rest of this track, internalize this picture:

```
┌─────────────────────────────────────────────────────────────────────┐
│                    Your code (C++)                                  │
│                                                                     │
│  high-level: vector<Order>, classes, virtual functions, etc.        │
└─────────────────────────────────────────────────────────────────────┘
                              ↓ compiled to
┌─────────────────────────────────────────────────────────────────────┐
│                    Machine code (x86-64)                            │
│   mov, add, cmp, jne, call, ret, ...                                │
└─────────────────────────────────────────────────────────────────────┘
                              ↓ executed by
┌─────────────────────────────────────────────────────────────────────┐
│  CPU core   →   L1 cache   →   L2   →   L3   →   DRAM   →   Disk    │
│  ~1 cycle      ~4 cycles    ~12     ~40     ~200      millions      │
└─────────────────────────────────────────────────────────────────────┘
                              ↑ mediated by
┌─────────────────────────────────────────────────────────────────────┐
│             OS (Linux): scheduler, virtual memory, syscalls         │
│   ← adds context-switch jitter, page faults, syscall overhead       │
└─────────────────────────────────────────────────────────────────────┘
```

Every line in your C++ program is doing something at every layer. Low-latency engineering is about **knowing which layer is currently your bottleneck** and either changing your code to dodge it, or changing your system so the bottleneck moves elsewhere.

---

## 🎯 Key Takeaways

- **Latency = per-operation time. Throughput = operations per second.** Don't conflate them.
- **Tail latency (p99/p999) matters more than averages** in real-time systems.
- **Memory is one giant byte array.** Where your data lives in that array dictates how fast you can access it.
- **Contiguous data beats scattered data**, often by 10-100x, regardless of big-O.
- **Always benchmark.** Never trust your gut. Numbers > opinions.

---

## 📚 Further Reading — Mental Models

- 📰 **[Mechanical Sympathy blog (Martin Thompson)](https://mechanical-sympathy.blogspot.com/)** — coined the term "mechanical sympathy". Read the early posts.
- 🎬 [Martin Thompson — "Mechanical Sympathy" (talk)](https://www.youtube.com/watch?v=MC1EKLQ2Wmg)
- 📰 [Dan Luu — "Computer latency: 1977-2017"](https://danluu.com/input-lag/) — humbling and informative; how much have we actually improved?

---

## ▶️ Next

[`02-memory-model.md`](./02-memory-model.md) — let's get into the specifics of how C++ data structures lay themselves out in memory, and how to take control of where they live.
