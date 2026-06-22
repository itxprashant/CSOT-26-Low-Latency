# 06 — ⭐ Bonus: Batching, the Disruptor & Beyond SPSC

> **TL;DR** — Once your SPSC ring is correct, pinned, and index-cached, the remaining wins are about *amortizing* the handoff: publish in **batches** so you pay one `release` per chunk, not per tick, and read in batches so the consumer stays in a tight loop. The **LMAX Disruptor** is the famous productionization of these ideas (a ring with a published-sequence cursor and batch draining). Going past one-producer-one-consumer (MPSC/MPMC) needs CAS and is a different beast. This file is optional and points ahead to Week 5's networked feed.

Everything in [`01`](./01-from-batch-to-streaming.md)–[`05`](./05-pipeline-pinning-and-backpressure.md) gets you a correct, overlapping pipeline. This file is the ceiling above it — useful for the top of the board, skippable for a solid submission.

---

## 1. Batching: amortize the publish

Per-tick publishing means one `release` store of `tail_` and (for the consumer) one `acquire` load per item. Those are cheap on x86 but not free, and they cap how tight the inner loop gets. **Batch** them:

```text
producer: decode K ticks into K slots, THEN tail_.store(t + K, release)  // one publish per K
consumer: acquire-load tail_ once, drain everything up to it, THEN head_.store(h + m, release)
```

The producer writes `slots_[(t+0)&mask] … slots_[(t+K-1)&mask]` with plain stores, then a *single* `release` makes the whole batch visible at once. The consumer, on one `acquire` load of `tail_`, sees a run of `m = tail - head` available items and processes them in a bare loop with no per-item atomics, publishing `head_` once at the end.

Batching trades a little **latency** (an item waits until its batch is published) for **throughput** (fewer barriers, tighter loops, better prefetch) — the Little's-law dial from [`01`](./01-from-batch-to-streaming.md). For the throughput-ranked feed challenge, a modest batch (8–64) is usually a clear win. Sweep it.

---

## 2. The LMAX Disruptor: this pattern, productionized

The [LMAX Disruptor](https://lmax-exchange.github.io/disruptor/) is a famous lock-free ring buffer from a London exchange that processed millions of orders/sec on one thread of business logic. Its ideas are exactly yours, refined:

- A **power-of-two ring** of pre-allocated slots (no per-item allocation) — your `slots_`.
- A **published-sequence cursor** the producer advances with release semantics — your `tail_`.
- **Batch draining**: a consumer asks "what's the highest published sequence?" and processes everything up to it in one go — your §1 consumer.
- **Cache-line padding** of the cursors to kill false sharing — your `alignas(64)`.
- Multiple consumers arranged as a *dependency graph* over the same ring, each tracking its own sequence — beyond SPSC, but built on the same cursor idea.

Reading the Disruptor's design after building your own SPSC is the fastest way to see which of your choices were fundamental and which were incidental.

---

## 3. Beyond SPSC: MPSC, MPMC, and why they're harder

The moment you have **more than one producer** or **more than one consumer**, the "single writer per index" property that made SPSC lock-free *without CAS* is gone — two producers both want to claim the next `tail` slot. Now you need an atomic **claim**:

```cpp
// multi-producer claim: atomically grab a unique slot index
std::size_t my_slot = tail_.fetch_add(1, std::memory_order_relaxed);   // each producer gets a distinct index
// ... write slots_[my_slot & mask] ... then publish (carefully — out-of-order completion!)
```

The hard part isn't the claim; it's **publication ordering**: producer A may claim slot 5 and producer B slot 6, but B finishes writing first — the consumer must not read slot 6 until slot 5 is also published. Solutions (per-slot sequence numbers à la Dmitry Vyukov's bounded MPMC queue, or the Disruptor's availability buffer) are real engineering. **You do not need any of this for Week 4** — the spec is single-producer (decode) → single-consumer (strategy). Know the boundary so you reach for SPSC's simplicity whenever the problem allows it (it usually does — the Single Writer Principle).

---

## 4. Other top-of-board refinements

- **Prefetch the input.** The producer reads the feed sequentially; `__builtin_prefetch(&in[i + K])` a few iterations ahead hides DRAM latency behind the decode (same trick as [Week 3's bonus](../week-3/06-bonus-bandwidth-and-pools.md)). Sweep `K`.
- **Keep slots trivially copyable.** `csot::Tick` is 48 B and trivially copyable; the slot copy is a couple of cache lines. Don't bloat what you put through the ring.
- **Avoid a `done` re-check storm.** A consumer spinning on an empty queue while polling `done` can hammer the producer's `tail_` line. Cache `tail_` (consumer-side), and only re-check `done` when truly empty.
- **One allocation, in `on_init`.** Ring storage, per-symbol state, interned names — all sized once. A `run()` that touches the heap will show up in `perf` as page faults and lose to one that doesn't.

---

## 5. The bridge to Week 5

Notice the shape you built: a thread that *produces* events (decode) handed off to a thread that *consumes* them (strategy), decoupled by a lock-free ring so a slow stage never blocks a fast one. **Week 5 replaces the producer's input.** Instead of decoding an in-memory array, the producer becomes a **network ingest** thread: it reads ticks off a TCP/UDP socket with an `epoll` event loop and pushes them across *the same SPSC ring* to *the same strategy consumer*. The handoff you wrote this week is precisely what keeps network jitter and syscall latency off the strategy's hot path — the I/O thread absorbs the wire, the strategy thread never waits on a socket.

Everything you learned — release/acquire publication, false-sharing-free cursors, spin-with-pause back-pressure, pinning — carries over unchanged. Week 5 just lengthens the pipe and puts the wire at the front.

---

## 🎯 Key Takeaways

- **Batch** the publish: write K slots with plain stores, then one `release`; drain a run of items on one `acquire`. Trades a little latency for throughput — sweep the batch size.
- The **LMAX Disruptor** is this exact pattern productionized (power-of-two ring, published cursor, batch drain, padded sequences). Read it after building your own SPSC.
- **MPSC/MPMC** lose SPSC's no-CAS simplicity: multiple writers must atomically claim slots and solve out-of-order publication. Not needed for Week 4 — prefer the Single Writer Principle.
- Top-of-board polish: **prefetch** the input, keep slots trivially copyable, cache the other cursor, and keep `run()` allocation-free.
- The pipeline you built is the Week-5 skeleton: swap the decode producer for a **network ingest** thread and the same ring keeps wire latency off the strategy.

---

## 📚 Further Reading — Disruptor, Batching, MPMC

- 📰 [LMAX Disruptor — technical paper (Thompson, Farley, Barker, et al.)](https://lmax-exchange.github.io/disruptor/files/Disruptor-1.0.pdf) — the canonical writeup; batching and the sequence cursor.
- 📰 [Martin Thompson — "Single Writer Principle"](https://mechanical-sympathy.blogspot.com/2011/09/single-writer-principle.html) — why SPSC is the easy case and worth designing toward.
- 📰 [Dmitry Vyukov — "Bounded MPMC queue"](https://www.1024cores.net/home/lock-free-algorithms/queues/bounded-mpmc-queue) — the classic per-slot-sequence MPMC ring, for when you truly need many writers.
- 🎬 [QCon — Martin Thompson & Michael Barker, "The LMAX Architecture / Disruptor"](https://www.infoq.com/presentations/LMAX/) — the war story behind the data structure.

---

## ▶️ Next

That's the reading for Week 4. Now go build it: the [**project brief**](./project/README.md) turns these lessons into a single `pipeline.cpp` on the live leaderboard. Week 5 puts the feed on the wire. ⚡
