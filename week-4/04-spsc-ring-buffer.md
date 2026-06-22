# 04 — Building a Lock-Free SPSC Ring Buffer

> **TL;DR** — A single-producer/single-consumer queue is just a fixed array plus two indices: `tail` (where the producer writes) and `head` (where the consumer reads). Power-of-two capacity turns the wrap-around into a bitmask. Put `head` and `tail` on **separate cache lines** or they false-share and you've recreated Week 3's bug in the queue itself. The producer writes the slot then `release`-stores `tail`; the consumer `acquire`-loads `tail`, reads the slot, then `release`-stores `head`. No lock, no allocation, a handful of instructions per push/pop.

You now have the pieces: atomics ([`02`](./02-atomics-and-the-memory-model.md)) and the release/acquire orderings ([`03`](./03-memory-orderings.md)). This file assembles them into *the* data structure of the week.

---

## 1. The shape: an array and two indices

A ring buffer is a fixed array used circularly. Two monotonically-increasing counters track positions; the *slot* is the counter masked to the array size:

```text
capacity = 8  (a power of two)        mask = 7

  index:   0   1   2   3   4   5   6   7
         [   ][ t ][ t ][ t ][   ][   ][   ][   ]
               ^head=1                 ^tail=4
         consumer reads from head    producer writes at tail
         count = tail - head = 3 items waiting
```

- **`tail`** — total number of items ever *pushed*. The producer increments it. Next write slot = `tail & mask`.
- **`head`** — total number of items ever *popped*. The consumer increments it. Next read slot = `head & mask`.
- **empty** ⇔ `head == tail`. **full** ⇔ `tail - head == capacity`.

Using *monotonic* counters (never reset, just `& mask` to index) makes full/empty unambiguous and kills the classic off-by-one of "wrap the index directly". Capacity **must** be a power of two so `& mask` replaces a `% capacity` (a division — slow, and you do it every push/pop).

---

## 2. False sharing strikes again: separate the indices

`head` is written by the consumer; `tail` is written by the producer. If they live in the same 64-byte cache line, every producer write to `tail` invalidates the consumer's cached copy of the whole line (which holds `head`), and vice-versa — the line ping-pongs between the two cores on *every single push and pop*. This is exactly the [Week-3 false-sharing](../week-3/04-false-sharing.md) trap, now in the queue.

The fix is the same: `alignas(64)` each index onto its own line.

```cpp
#include <atomic>
#include <new>

constexpr std::size_t kLine = 64;   // cache-line size (std::hardware_destructive_interference_size)

struct alignas(kLine) Index {
    std::atomic<std::size_t> v{0};
    char pad[kLine - sizeof(std::atomic<std::size_t>)];   // fill the rest of the line
};
```

Now the producer hammers its `tail` line and the consumer hammers its `head` line with no coherence traffic between them — the whole point of going lock-free.

---

## 3. A complete SPSC ring buffer

Here is the full structure — fixed capacity, lock-free, zero allocation after construction:

```cpp
#include <atomic>
#include <cstddef>

template <typename T, std::size_t Capacity>   // Capacity MUST be a power of two
class SpscRing {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");
    static constexpr std::size_t kMask = Capacity - 1;
    static constexpr std::size_t kLine = 64;

    T slots_[Capacity];

    alignas(kLine) std::atomic<std::size_t> tail_{0};   // producer writes
    alignas(kLine) std::atomic<std::size_t> head_{0};   // consumer writes

public:
    // PRODUCER ONLY. Returns false if the queue is full (back-pressure).
    bool try_push(const T& item) {
        const std::size_t t = tail_.load(std::memory_order_relaxed);   // own index: relaxed
        if (t - head_.load(std::memory_order_acquire) == Capacity)     // other's: acquire
            return false;                                              // full
        slots_[t & kMask] = item;                                      // write payload
        tail_.store(t + 1, std::memory_order_release);                 // publish (release)
        return true;
    }

    // CONSUMER ONLY. Returns false if the queue is empty.
    bool try_pop(T& out) {
        const std::size_t h = head_.load(std::memory_order_relaxed);   // own index: relaxed
        if (h == tail_.load(std::memory_order_acquire))                // other's: acquire
            return false;                                              // empty
        out = slots_[h & kMask];                                       // read payload
        head_.store(h + 1, std::memory_order_release);                 // publish (release)
        return true;
    }
};
```

Every ordering is the one [`03`](./03-memory-orderings.md) §5 prescribed: read your own index `relaxed`, read the other's `acquire`, publish yours `release`. Note the asymmetry that makes it correct: the producer writes `slots_[t & kMask]` *before* the `release` store of `tail_`, and the consumer reads it *after* its `acquire` load of `tail_` — so the slot write happens-before the slot read.

---

## 4. Driving it: the two threads

```cpp
// PRODUCER: decode each WireTick, push it (spin while full = back-pressure)
for (std::size_t i = 0; i < n; ++i) {
    csot::Tick tk = decode(in[i]);
    while (!ring.try_push(tk)) { /* spin: consumer is behind */ }
}
done.store(true, std::memory_order_release);   // signal end-of-stream

// CONSUMER: pop, strategize, until drained AND producer is done
csot::Tick tk;
for (;;) {
    if (ring.try_pop(tk)) { strategize(tk); }
    else if (done.load(std::memory_order_acquire)) {
        if (!ring.try_pop(tk)) break;          // drain the last items, then exit
        strategize(tk);
    }
}
```

The `done` flag is itself a release/acquire handshake so the consumer doesn't exit while items remain. (Watch the drain: check `done`, then try one more pop, because the producer may have pushed *and* set `done` between your empty check and your flag check.)

---

## 5. Two refinements that matter

**Cache the other side's index.** Every `try_push` loads `head_` with an `acquire` — a read of a line the *other* core owns, which can stall. But the producer doesn't need the consumer's *latest* head to know there's space; a slightly stale head that says "not full" is still safe. So cache it and only re-read when the cached value says full:

```cpp
std::size_t cached_head = 0;                       // producer-local, no atomics
bool try_push_cached(const T& item) {
    const std::size_t t = tail_.load(std::memory_order_relaxed);
    if (t - cached_head == Capacity) {             // maybe full — refresh
        cached_head = head_.load(std::memory_order_acquire);
        if (t - cached_head == Capacity) return false;   // really full
    }
    slots_[t & kMask] = item;
    tail_.store(t + 1, std::memory_order_release);
    return true;
}
```

This turns most pushes into zero cross-core reads — a large throughput win. The consumer caches `tail_` symmetrically. This is the single most effective SPSC optimization after getting the orderings right.

**Batch.** Publishing `tail_` once per item means one `release` store per tick. If the producer decodes a small batch (say 16) and publishes `tail_` once for the whole batch, you amortize the store and the consumer wakes to a chunk of work. Batching trades a little latency for throughput — exactly the Little's-law dial from [`01`](./01-from-batch-to-streaming.md).

---

## 6. Sizing the buffer

Capacity is a real tuning knob:

- **Too small** (e.g. 16): the producer constantly hits "full" and spins, the stages stop overlapping, throughput collapses toward serial.
- **Too big** (e.g. 16 M): the slot array blows past L2/L3; the consumer reads cache-cold slots the producer evicted long ago, so you pay DRAM latency per pop.
- **Just right**: large enough to absorb bursts (the producer can run ahead during a strategy-heavy stretch), small enough that the in-flight slots stay hot in cache. A few thousand to a few tens of thousands of `csot::Tick` (each 48 B) is a sensible starting sweep — measure it ([`05`](./05-pipeline-pinning-and-backpressure.md)).

> ⚠️ The slots hold `T` by value. For `csot::Tick` (48 B, trivially copyable) that's ideal — the copy into the slot *is* the publish. Don't store pointers into the input array and "save the copy": you'd lose the clean ownership handoff and risk the consumer reading a slot you've already overwritten.

---

## 🎯 Key Takeaways

- An SPSC ring buffer = fixed array + two monotonic counters; slot index = `counter & (capacity-1)`, so **capacity must be a power of two** (mask, not modulo).
- **empty** ⇔ `head == tail`; **full** ⇔ `tail - head == capacity`. Monotonic counters make this unambiguous and dodge the off-by-one.
- Put `head` and `tail` on **separate cache lines** (`alignas(64)`) or they false-share and the queue is slower than a lock.
- Orderings (from [`03`](./03-memory-orderings.md)): own index `relaxed`, other's index `acquire`, publish `release`. Write the slot before the release; read it after the acquire.
- **Cache the other side's index** (refresh only when it looks full/empty) — the biggest win after correctness. **Batch** publishes to trade latency for throughput.
- **Size** the buffer to absorb bursts while staying in cache: sweep it, don't guess.

---

## 📚 Further Reading — Lock-Free Queues

- 📰 [Rigtorp — "A single producer single consumer wait-free ring buffer"](https://rigtorp.se/ringbuffer/) — the index-caching trick of §5, benchmarked; the reference SPSC writeup.
- 📖 [Rigtorp/SPSCQueue (GitHub)](https://github.com/rigtorp/SPSCQueue) — a tiny, production-grade implementation to compare yours against (read it after you write your own).
- 🎬 [CppCon 2023 — "Single Producer Single Consumer Lock-free FIFO From the Ground Up"](https://www.youtube.com/watch?v=K3P_Lmq6pw0) — builds exactly this structure step by step.
- 📰 [Preshing — "The World's Simplest Lock-Free Hash Table"](https://preshing.com/20130605/the-worlds-simplest-lock-free-hash-table/) — broader lock-free intuition once SPSC clicks.

---

## ▶️ Next

[`05-pipeline-pinning-and-backpressure.md`](./05-pipeline-pinning-and-backpressure.md) — wire the producer and consumer together, pin them to cores, handle a full queue, balance the stages, and measure the overlap you built. ⚡
