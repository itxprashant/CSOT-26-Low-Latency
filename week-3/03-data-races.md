# 03 — Data Races: Two Threads, One Variable, Undefined Behaviour

> **TL;DR** — A data race is two threads accessing the same memory at the same time with at least one write and no synchronization. In C++ it is **undefined behaviour** — not "a wrong number", but "the compiler may do anything". `count++` from two threads loses updates because it's really load-add-store, and the steps interleave. The fix is almost never a lock on the hot path; it's **not sharing** — give each thread its own data and merge later. Prove you're race-free with ThreadSanitizer (`-fsanitize=thread`).

[`02-stdthread-basics.md`](./02-stdthread-basics.md) had every worker write to its *own* partial table. This file shows what happens the moment you break that rule — and why the textbook "just add a mutex" is the wrong instinct for a hot reduction.

---

## 1. The race, in one screen

Two threads each increment a shared counter a million times. The answer should be two million. It won't be.

```cpp
#include <thread>
#include <cstdio>

long counter = 0;                          // SHARED, unsynchronized

void bump() { for (int i = 0; i < 1'000'000; ++i) ++counter; }   // DATA RACE

int main() {
    std::thread a(bump), b(bump);
    a.join(); b.join();
    std::printf("%ld (expected 2000000)\n", counter);   // prints e.g. 1373182 — less than 2000000
}
```

Run it a few times and you'll get a *different* wrong number each run — often 1.1–1.4 million. The updates are being **lost**.

---

## 2. Why `++counter` loses updates

`++counter` is not one operation. The CPU does three:

```text
1. load  counter -> register
2. add   1 to register
3. store register -> counter
```

Two threads interleave those steps. A classic lost update:

```text
thread A: load counter (=100)
thread B: load counter (=100)
thread A: add 1 -> 101
thread B: add 1 -> 101
thread A: store 101
thread B: store 101        <-- both stored 101; one increment vanished
```

Two increments happened; the counter moved by one. Multiply by a million interleavings and you lose a third of your updates. In the aggregator this is exactly `++out[s].count` or `out[s].sum_price += px` from multiple threads on a **shared** table: wrong counts, wrong sums.

---

## 3. It's not "wrong number" — it's undefined behaviour

This is the part newcomers underestimate. The C++ standard says a data race is **undefined behaviour** (UB). The compiler is allowed to assume your program has no races, and it optimizes on that assumption. Consequences that actually happen:

- The compiler **hoists** a shared read out of a loop into a register, so your thread never sees another thread's writes at all.
- A non-atomic 64-bit store can **tear** on some platforms (two halves written separately), producing a value neither thread ever wrote.
- The behaviour changes between `-O0` and `-O2`, between compilers, between runs. "It worked on my machine" is meaningless under UB.

So a racy aggregator isn't just occasionally off by a few — it's *unpredictable*, and that's why the judge rejects non-deterministic submissions ([`AGG_SPEC.md`](./project/AGG_SPEC.md) §8): a program with UB has no trustworthy answer to rank.

> 📌 The formal rule (C++ memory model): if two evaluations access the same memory location, at least one is a write, and they are not ordered by a *happens-before* relationship, the behaviour is undefined. `join()`, mutexes, and atomics create happens-before; bare reads/writes do not.

---

## 4. The instinct that's wrong on the hot path: "just lock it"

You *can* make the counter correct with a mutex:

```cpp
#include <mutex>
std::mutex m;
void bump() {
    for (int i = 0; i < 1'000'000; ++i) {
        std::lock_guard<std::mutex> g(m);   // correct... and catastrophically slow
        ++counter;
    }
}
```

Now it prints 2,000,000 — and runs *slower than a single thread*, because every increment serializes through the lock (and an uncontended lock is ~20 ns; a contended one is far worse, with kernel futex wake-ups). You added cores and got a traffic jam. A mutex around the hot operation defeats the entire point of going wide.

An `std::atomic<long>` is better than a mutex (no kernel involvement) but still forces every thread's increment through the *same* cache line — the contention just moves to the hardware, and you'll measure it in [`04-false-sharing.md`](./04-false-sharing.md). Atomics are the right tool for a *handoff* (Week 4's lock-free queue), not for a *reduction* where every thread hammers the same accumulator.

---

## 5. The right fix: don't share

The reduction has a structural answer that needs **no** synchronization in the hot loop: give each thread its own private table, and combine them once, after the threads are done.

```cpp
// Each thread reduces its chunk into ITS OWN partial — zero sharing, zero locks.
void reduce_chunk(const csot::AggTick* b, const csot::AggTick* e, csot::SymbolAgg* partial);

// After all threads join (a happens-before edge!), merge serially:
void merge(csot::SymbolAgg* out, const csot::SymbolAgg* partial, std::uint32_t num_symbols) {
    for (std::uint32_t s = 0; s < num_symbols; ++s) {
        if (partial[s].count == 0) continue;            // skip empty (guards min/max — §7)
        if (out[s].count == 0) { out[s] = partial[s]; continue; }
        out[s].count     += partial[s].count;
        out[s].sum_price += partial[s].sum_price;
        out[s].sum_qty   += partial[s].sum_qty;
        out[s].min_price  = std::min(out[s].min_price, partial[s].min_price);
        out[s].max_price  = std::max(out[s].max_price, partial[s].max_price);
    }
}
```

No two threads ever touch the same memory during the sweep, so there is no race, no lock, and no contention. The only synchronization is the `join()` between the parallel sweep and the serial merge — and `join()` is exactly the happens-before edge that makes the partials safely readable. This is why the map-reduce shape is the answer, not a workaround.

---

## 6. Prove it: ThreadSanitizer

You cannot test a race away — it hides at `-O0`, on your laptop, on small inputs. **ThreadSanitizer (TSan)** instruments memory accesses and reports races the moment they *could* happen, even if the run looked fine. The shipped CMake has a TSan build:

```bash
cmake -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DENABLE_TSAN=ON -DCSOT_AGG_SRC=aggregator.cpp
cmake --build build-tsan -j
./build-tsan/agg_runner data/tiny.ticks
```

On the racy counter from §1, TSan prints something like:

```text
WARNING: ThreadSanitizer: data race (pid=12345)
  Write of size 8 at 0x... by thread T2:
    #0 bump() ...
  Previous write of size 8 at 0x... by thread T1:
    #0 bump() ...
```

It names the two racing accesses and the threads. A clean TSan run on a representative input is your evidence that the parallel reduction is correct, not just lucky. (TSan slows execution ~5–15× and roughly doubles memory — it's a debug tool, never a benchmark build, and it's incompatible with AddressSanitizer, hence the separate build dir.)

> 💡 TSan can be quiet on a *tiny* input that never actually interleaves the bug. Belt-and-suspenders: also run your parallel `run()` in a loop and assert the checksum is identical every time. The judge does exactly this (§8).

---

## 7. The hands-on you should actually do

Build the §1 racer, run it ten times, and watch the numbers differ. Then run it under TSan and read the report. Then "fix" it with a mutex and time it — feel it crawl. Finally rewrite it as per-thread partials + merge and confirm it's both correct *and* fast. That five-minute experiment teaches the whole week: **sharing is the enemy; structure beats synchronization.**

---

## 🎯 Key Takeaways

- A **data race** = concurrent access to the same memory, ≥1 write, no synchronization. In C++ it is **undefined behaviour**, not merely a wrong value.
- `count++` is load-add-store; interleaving those steps across threads **loses updates**.
- UB means the compiler may hoist reads, tear stores, and behave differently per build/run — a racy program has no answer worth ranking, which is why the judge rejects non-determinism.
- A **mutex on the hot operation** serializes it — often slower than one thread. An **atomic** moves contention to hardware (one hot cache line) — right for handoffs, wrong for reductions.
- The structural fix is **don't share**: private per-thread partials, merge once after `join()` (the happens-before edge).
- **Prove** race-freedom with ThreadSanitizer (`-DENABLE_TSAN=ON`); it names the racing accesses. Back it with a repeat-run checksum check.

---

## 📚 Further Reading — Races & the Memory Model

- 📖 [ThreadSanitizer manual (Clang docs)](https://clang.llvm.org/docs/ThreadSanitizer.html) — how to build, read the report, and its limitations.
- 🎬 [CppCon — Hans Boehm, "Using weakly ordered C++ atomics correctly"](https://www.youtube.com/watch?v=M15UKpNlpeM) — by the author of the C++ memory model; what a race *is*, formally.
- 📰 [Preshing — "This Is Why They Call It a Weakly-Ordered CPU"](https://preshing.com/20120930/weak-memory-models-in-practice/) — races and reordering made concrete with real hardware.
- 📖 [cppreference — memory model & `std::memory_order`](https://en.cppreference.com/w/cpp/atomic/memory_order) — happens-before, and what synchronizes (preview of Week 4).

---

## ▶️ Next

[`04-false-sharing.md`](./04-false-sharing.md) — you fixed the race by giving each thread its own table. Now watch those "private" tables silently share a cache line and make four threads *slower* than one.
