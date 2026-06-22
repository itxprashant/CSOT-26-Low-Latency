# 01 — From Batch to Streaming: The Handoff Problem

> **TL;DR** — Week 3 reduced a whole array at once, in any order. Week 4 processes a *continuous stream* in *exact order*, splitting the work across a **producer** thread and a **consumer** thread that must hand data to each other. A lock around that handoff serializes the very thing you just parallelized — so the rest of the week is about handing data between two threads *without a lock*. The win is **overlap**: while the consumer strategizes tick N, the producer is already decoding tick N+1.

Week 3 ([the aggregator](../week-3/README.md)) was *embarrassingly parallel*: chop the array into four pieces, reduce each, merge. Order didn't matter; the pieces never talked. This week the workload is the opposite — a **stateful, order-dependent** strategy that must see every tick exactly once, in sequence — and the parallelism is a **pipeline**, not a partition. Two threads, one data path between them. That path is the whole problem.

---

## 1. The workload: a pipeline, not a partition

The Week-4 challenge ([`PIPELINE_SPEC.md`](./project/PIPELINE_SPEC.md)) is a two-stage assembly line:

```text
 wire feed ──►  PRODUCER (decode)  ──►  [ handoff ]  ──►  CONSUMER (strategy)  ──► orders
               int -> double,                            z-score over a rolling
               intern symbol                             64-window, emit orders
```

The producer turns each compact 40-byte `WireTick` into a `csot::Tick` (fixed-point → `double`, symbol interning). The consumer runs the **unchanged Week-1 z-score strategy** (`STRATEGY_SPEC.md`) over the decoded ticks, in order, updating per-symbol position as it fills.

You *cannot* shard the consumer the way you sharded the aggregator: tick N's logic depends on the position set by tick N-1's fill. The strategy stays on **one** thread. The only thing you can do in parallel is run the **decode** on another thread and overlap it with the strategy.

---

## 2. Why overlap is the entire payoff

Call the per-tick costs `D` (decode) and `S` (strategy). Single-threaded, each tick costs `D + S`:

```text
serial:   |D S|D S|D S|D S| ...        per tick: D + S
```

Pipelined, the producer decodes tick N+1 *while* the consumer strategizes tick N. If the handoff is free and the stages are balanced, the per-tick cost drops to `max(D, S)`:

```text
pipeline: produce |D|D|D|D|D| ...
          consume    |S|S|S|S|S| ...   per tick: max(D, S)  (+ tiny handoff)
```

The speedup ceiling is therefore:

```text
speedup = (D + S) / max(D, S)
```

- Perfectly balanced (`D == S`): **2×**.
- Lopsided (`S` ten times `D`): `11/10 ≈ 1.1×` — the fast stage just waits. This is why the spec gives the producer *real* work (decode) and why a competitive consumer keeps `S` low with Week-2 rolling sums: you want `D ≈ S` so the overlap actually buys you the full 2×.

> 💡 Two stages cap you near 2×, not 4×. That's fine — the judge box has 4 vCPUs and you use two hot threads. The skill being tested is the **handoff**, not raw core count. (Week 5 adds a third stage — network I/O — for a longer pipe.)

---

## 3. The naive handoff: a mutex-protected queue

The textbook way two threads share a queue:

```cpp
#include <mutex>
#include <queue>

std::queue<csot::Tick> q;
std::mutex             m;

// producer
void push(const csot::Tick& t) {
    std::lock_guard<std::mutex> lk(m);   // lock
    q.push(t);                           // mutate
}                                        // unlock

// consumer
bool pop(csot::Tick& out) {
    std::lock_guard<std::mutex> lk(m);
    if (q.empty()) return false;
    out = q.front();
    q.pop();
    return true;
}
```

This is *correct*. It is also *slow*, for two reasons:

1. **It serializes the threads.** Only one of producer/consumer can hold the lock at a time, so they take turns touching the queue — the opposite of overlap. Under contention a `std::mutex` can also sleep the loser in the kernel (~1–5 µs to wake), which is catastrophic when a tick is ~10 ns of work.
2. **`std::queue` allocates.** Every `push` may `malloc` a node. You spent Week 2 removing `malloc` from the hot path; don't put it back in the handoff.

A lock around a handoff that happens *once per tick* turns your two-core pipeline back into one core that also pays locking overhead. You'll measure it: a mutex pipeline is usually **slower than the single-threaded baseline**.

---

## 4. The SPSC special case makes locks unnecessary

The general "many threads share a queue" problem is genuinely hard. But our case is the *easy* one:

- **S**ingle **P**roducer — exactly one thread ever pushes.
- **S**ingle **C**onsumer — exactly one thread ever pops.

With one writer of the "tail" and one writer of the "head", there is **no contention to lock against**: the producer owns the tail, the consumer owns the head, and they only need to *publish* their index to each other safely. That publication is a single atomic store/load with the right memory ordering — not a lock. The result is a **lock-free SPSC ring buffer**: a fixed array plus two indices, where push and pop are a handful of instructions and never block each other.

That data structure is the heart of this week ([`04-spsc-ring-buffer.md`](./04-spsc-ring-buffer.md)). To build it correctly you first need to know what an atomic *is* ([`02`](./02-atomics-and-the-memory-model.md)) and exactly how much ordering it must guarantee ([`03`](./03-memory-orderings.md)).

---

## 5. Latency vs. throughput, and Little's law

Two numbers describe a pipeline, and they are not the same:

- **Latency** — how long *one* tick takes to travel producer → queue → consumer → order.
- **Throughput** — how many ticks per second the pipeline sustains.

The leaderboard ranks **throughput** (wall-clock of the whole `run()` over a huge feed — judge-measured, so it can't be gamed). But latency still matters: if the queue is too small the producer stalls waiting for space (back-pressure), and if it's too big you waste cache. **Little's law** ties them together:

```text
items_in_flight = throughput × latency
```

A deeper queue lets more ticks be "in flight", smoothing bursts so the consumer never starves — up to the point where the working set falls out of cache. You'll tune the capacity and feel this trade-off directly ([`05`](./05-pipeline-pinning-and-backpressure.md)).

---

## 🎯 Key Takeaways

- Week 4's workload is a **stateful, ordered pipeline**, not a partition: the strategy must see every tick once, in order, on **one** consumer thread.
- The only parallelism available is **overlap**: decode tick N+1 while strategizing tick N. Speedup ceiling is `(D+S)/max(D,S)` — **2× when balanced**, ~1× when lopsided.
- A **mutex** (or `std::queue`) around the handoff serializes the threads and allocates — it's typically *slower* than single-threaded. Don't lock the hot path.
- The handoff is the easy **SPSC** case: one producer, one consumer, no contention — so it can be **lock-free**, built from an array and two atomic indices.
- **Throughput** is ranked; **latency** and queue depth still matter (Little's law). Balance the stages and size the queue.

---

## 📚 Further Reading — Pipelines & Handoffs

- 🎬 [CppCon 2017 — Carl Cook, "When a Microsecond Is an Eternity"](https://www.youtube.com/watch?v=NH1Tta7purM) — doing nothing on the hot path, from the HFT trenches; the mindset behind a lock-free handoff.
- 📰 [Martin Thompson — Mechanical Sympathy: "Single Writer Principle"](https://mechanical-sympathy.blogspot.com/2011/09/single-writer-principle.html) — why one-writer-per-datum is the secret that makes SPSC lock-free.
- 📖 ["C++ Concurrency in Action" (2nd ed., Anthony Williams), ch. 8](https://www.manning.com/books/c-plus-plus-concurrency-in-action-second-edition) — designing concurrent code: pipelines, partitioning, and where each fits.
- 📰 [Wikipedia — Little's law](https://en.wikipedia.org/wiki/Little%27s_law) — the queueing identity behind buffer-depth tuning.

---

## ▶️ Next

[`02-atomics-and-the-memory-model.md`](./02-atomics-and-the-memory-model.md) — what `std::atomic` actually guarantees, why `volatile` is not it, and the C++ memory model that makes a lock-free handoff possible. ⚡
