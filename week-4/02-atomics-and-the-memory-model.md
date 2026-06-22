# 02 — Atomics and the C++ Memory Model

> **TL;DR** — A plain read/write of a variable shared by two threads is a data race, which is undefined behaviour (you saw this in Week 3). `std::atomic<T>` makes a single load, store, or read-modify-write **indivisible** and gives the compiler and CPU rules about *when* one thread's writes become visible to another. `volatile` does **not** do this — it's for memory-mapped hardware, not threads. Atomics are how the producer publishes "there's a new tick" to the consumer without a lock.

[`03-data-races.md`](../week-3/03-data-races.md) showed that `count++` from two threads is undefined behaviour and loses updates. The fix there was *don't share* — give each thread its own data. But a handoff is the one place you *must* share: the producer has to tell the consumer "I put a tick in slot 7". `std::atomic` is the tool that makes that one shared signal safe.

---

## 1. What "atomic" means

An operation is **atomic** if no other thread can ever observe it half-done. A normal 64-bit store is *usually* atomic on x86 for aligned data — but the standard doesn't promise it, and the compiler is free to tear it, reorder it, cache it in a register, or assume no other thread touches it. `std::atomic<T>` removes all that freedom:

```cpp
#include <atomic>
#include <cstdint>

std::atomic<std::uint64_t> tail{0};

tail.store(8, std::memory_order_release);          // indivisible write
std::uint64_t t = tail.load(std::memory_order_acquire);  // indivisible read
```

For an `std::atomic`:

- a `load` reads a value that was *actually stored by some thread* — never a torn half-write;
- a `store` is seen whole by every other thread;
- the compiler may not invent, duplicate, or hoist the access out of a loop;
- you additionally control *ordering* (the topic of [`03`](./03-memory-orderings.md)).

`std::atomic<std::uint64_t>` is **lock-free** on every platform we target (`tail.is_lock_free()` returns `true`) — the operations compile to single CPU instructions, not a hidden mutex. That's the property a lock-free queue depends on.

---

## 2. The race, fixed with an atomic

The Week-3 counter race, made well-defined:

```cpp
#include <atomic>
#include <thread>
#include <cstdio>

std::atomic<long> counter{0};                       // atomic, not plain long

void bump() { for (int i = 0; i < 1'000'000; ++i)
                  counter.fetch_add(1, std::memory_order_relaxed); }  // RMW, no lost updates

int main() {
    std::thread a(bump), b(bump);
    a.join(); b.join();
    std::printf("%ld (expected 2000000)\n", counter.load());   // ALWAYS 2000000
}
```

`fetch_add` is a **read-modify-write** (RMW): it loads, adds, and stores as one indivisible step, so the load-add-store interleaving that lost updates in Week 3 cannot happen. The result is always exactly `2000000`.

> 📌 Note we used `memory_order_relaxed` here. The *count* is correct because the RMW is atomic; we did **not** ask for any ordering relative to other variables, because there are none. Relaxed is the cheapest atomic — perfect for a pure counter. Ordering only matters when an atomic *guards other data*, which is §4 and all of [`03`](./03-memory-orderings.md).

---

## 3. `volatile` is not atomic (a common, costly myth)

`volatile` tells the compiler "this memory can change underneath you, don't optimize the access away." That is for **memory-mapped I/O registers** and signal handlers. It does **not**:

- make the access indivisible (a `volatile` 64-bit store can still tear on some ABIs),
- prevent the *CPU* from reordering it relative to other memory,
- establish any happens-before relationship between threads.

```cpp
volatile bool ready = false;     // WRONG for thread communication
// ... another thread does: while (!ready) {}  — may spin forever or see torn state
```

Using `volatile` for inter-thread signaling is a classic bug that "works" on x86 by luck and breaks under optimization or on weaker hardware. For threads, the answer is **always** `std::atomic`. (Java's `volatile` *is* roughly C++'s `atomic`; do not carry the Java meaning over.)

---

## 4. Atomics publish data, not just numbers

The reason atomics matter for our queue isn't the atomic value itself — it's that an atomic store/load can act as a **release/acquire fence** that makes *other, non-atomic* writes visible. The canonical pattern:

```cpp
#include <atomic>

int                 payload = 0;     // plain data
std::atomic<bool>   ready{false};    // the flag

// producer thread
payload = 42;                                  // (1) write the data
ready.store(true, std::memory_order_release);  // (2) publish: "data is ready"

// consumer thread
while (!ready.load(std::memory_order_acquire)) // (3) wait for the flag
    ;
int x = payload;                               // (4) GUARANTEED to read 42
```

The `release` store at (2) and the `acquire` load at (3) form a **handshake**: once the consumer sees `ready == true`, it is guaranteed to also see *everything the producer wrote before the release* — including the non-atomic `payload = 42`. Without the right ordering, (4) could read the old `payload` even after seeing `ready == true`, because the CPU/compiler reordered (1) after (2).

This is *exactly* the SPSC queue's core move: the producer writes a tick into a slot (plain data), then `release`-stores the new `tail` index; the consumer `acquire`-loads `tail`, and is then guaranteed to see the fully-written tick. The slot is the `payload`; `tail` is the `ready` flag.

> ⚠️ The handshake only works if the data write happens **before** the release store, and the data read happens **after** the acquire load, *in program order*. Publish last; consume after observing. Get this backwards and you read a half-written tick.

---

## 5. `compare_exchange`: the building block you mostly won't need

The other RMW worth knowing is **compare-and-swap** (CAS): "if this atomic still equals `expected`, set it to `desired`, atomically; otherwise tell me what it actually is."

```cpp
std::atomic<int> v{10};
int expected = 10;
bool ok = v.compare_exchange_strong(expected, 20);   // v==10? -> v=20, ok=true
// if it failed, `expected` now holds the current value, so you can retry.
```

CAS is the foundation of *multi*-producer / *multi*-consumer lock-free structures, where several threads contend for the same index and must retry on conflict. **For SPSC you don't need CAS at all** — each index has a single writer, so a plain `store`/`load` with release/acquire suffices. Know CAS exists (it's how MPSC/MPMC queues and the Disruptor's claim work, [`06`](./06-bonus-disruptor-and-beyond.md)); reach for the simpler tool first.

---

## 🎯 Key Takeaways

- `std::atomic<T>` makes a load/store/RMW **indivisible** and removes the compiler's freedom to tear, reorder, or elide the access. On our targets `std::atomic<uint64_t>` is **lock-free** (single instructions).
- A **read-modify-write** (`fetch_add`, `compare_exchange`) does load-modify-store as one step, which is why it can't lose updates like Week 3's `count++`.
- `volatile` is for memory-mapped hardware, **not** threads. It is neither atomic nor ordered. For inter-thread signaling, always use `std::atomic`.
- The real power: an atomic **release store** + **acquire load** form a handshake that makes the producer's *non-atomic* writes visible to the consumer. That's how you publish a tick safely.
- **SPSC needs only plain `store`/`load`** with release/acquire — no CAS. CAS is for multi-writer structures; don't pay for it here.

---

## 📚 Further Reading — Atomics & the Memory Model

- 📖 [cppreference — `std::atomic`](https://en.cppreference.com/w/cpp/atomic/atomic) — the precise contract for loads, stores, and RMW.
- 🎬 [CppCon 2017 — Fedor Pikus, "C++ atomics, from basic to advanced. What do they really do?"](https://www.youtube.com/watch?v=ZQFzMfHIxng) — the clearest tour of what atomics actually compile to.
- 📰 [Jeff Preshing — "An Introduction to Lock-Free Programming"](https://preshing.com/20120612/an-introduction-to-lock-free-programming/) — atomicity, ordering, and why `volatile` isn't enough, with hardware intuition.
- 📰 [Preshing — "The Synchronizes-With Relation"](https://preshing.com/20130823/the-synchronizes-with-relation/) — the release/acquire handshake of §4, formally but readably.

---

## ▶️ Next

[`03-memory-orderings.md`](./03-memory-orderings.md) — `relaxed` / `acquire` / `release` / `seq_cst`, exactly which one each side of the queue needs, and why `seq_cst` everywhere is paying for a barrier you don't need. ⚡
