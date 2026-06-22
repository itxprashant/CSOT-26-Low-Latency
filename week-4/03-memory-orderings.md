# 03 — Memory Orderings: relaxed, acquire, release, seq_cst

> **TL;DR** — Atomics let you choose *how much* ordering you pay for. `relaxed` = atomic but no ordering (fine for a standalone counter). `release` (on a store) + `acquire` (on the matching load) = a one-way barrier that publishes everything written before the store to whoever reads the value after the load — this is the SPSC queue's bread and butter. `seq_cst` = the strongest and the default, a full barrier you rarely need. An SPSC ring buffer needs only release/acquire; using `seq_cst` everywhere is correct but leaves speed on the table.

[`02`](./02-atomics-and-the-memory-model.md) introduced the release/acquire handshake. This file is the precise menu of orderings, what each costs, and exactly which one goes on each line of your ring buffer.

---

## 1. Why ordering is even a question

Both the compiler and the CPU **reorder memory operations** to go faster, as long as a *single thread* can't tell. Example the hardware is allowed to do:

```text
you wrote:        slot[7] = tick;        tail = 8;
CPU may execute:  tail = 8;              slot[7] = tick;   // store-store reorder
```

Single-threaded, indistinguishable. But a second thread that sees `tail == 8` and then reads `slot[7]` gets **garbage** — the tick isn't written yet. Memory orderings are how you forbid exactly the reorderings that would break a *cross-thread* protocol, and nothing more.

x86 has a relatively strong hardware model (it won't reorder store-store or load-load on its own), but **the compiler still will**, and weaker CPUs (ARM, which real trading and mobile boxes use) reorder aggressively. Write correct orderings and it's right everywhere; rely on x86's strength and it breaks on ARM and under `-O3`.

---

## 2. The four orderings you'll meet

```cpp
std::memory_order_relaxed
std::memory_order_acquire
std::memory_order_release
std::memory_order_seq_cst   // the default if you pass nothing
```

| Ordering | Use on | Guarantee | Cost |
|---|---|---|---|
| `relaxed` | load or store | atomic value only; **no** ordering w.r.t. other memory | cheapest (plain move on x86) |
| `acquire` | **load** | nothing after this load (in program order) moves before it; you see all writes released before the value you read | cheap on x86 (free); a barrier on ARM |
| `release` | **store** | nothing before this store moves after it; pairs with an acquire load to publish prior writes | cheap on x86 (free); a barrier on ARM |
| `seq_cst` | load, store, RMW | acquire+release **plus** a single global total order across all `seq_cst` ops | most expensive (`mfence`/`lock` on x86) |

The mental model: **`release` is "publish everything I did before this"; `acquire` is "and I'll see all of it after I read this."** They are a matched pair — a release with no acquiring reader, or an acquire with no releasing writer, synchronizes with nothing.

---

## 3. relaxed: atomic, but a loner

`relaxed` guarantees the operation is indivisible and the value isn't invented — but imposes **no** ordering relative to other memory. It's correct only when the atomic carries no information about *other* data:

```cpp
std::atomic<long> dropped{0};
dropped.fetch_add(1, std::memory_order_relaxed);   // a stat counter — fine
```

If the atomic *guards* a slot, a payload, a "ready" meaning — relaxed is wrong, because the reader can see the flag without seeing the data it's supposed to announce. In the SPSC queue, the index update *does* guard the slot, so the *publishing* store must be `release` and the *observing* load `acquire`. (You may still use `relaxed` to read your **own** index — the index only that thread writes — because there's no cross-thread publication in that read; see §5.)

---

## 4. release/acquire: the handshake, precisely

```cpp
int               slot = 0;          // plain data (a queue slot)
std::atomic<int>  tail{0};           // the published index

// PRODUCER
slot = 42;                                   // (A) write payload
tail.store(1, std::memory_order_release);    // (B) publish — release

// CONSUMER
if (tail.load(std::memory_order_acquire)     // (C) observe — acquire
        == 1) {
    int x = slot;                            // (D) sees 42, guaranteed
}
```

The rule (the "synchronizes-with" relation): **if the acquire load at (C) reads the value written by the release store at (B), then everything sequenced before (B) — including the plain write (A) — happens-before everything sequenced after (C)**. So (D) is guaranteed to read `42`, never a stale or half-written value.

Two consequences you must respect in code:

- **Producer: write the payload, then release-store the index.** Publish *last*.
- **Consumer: acquire-load the index, then read the payload.** Consume *after* observing.

Flip either and the barrier protects nothing. This single pattern, applied to a ring buffer's `head` and `tail`, is the entire correctness argument for SPSC.

---

## 5. What the SPSC ring buffer actually needs

A single-producer/single-consumer ring buffer has two indices, each with **one** writer:

- `tail` — written only by the producer (where the next push goes), read by both.
- `head` — written only by the consumer (where the next pop comes from), read by both.

The orderings that are necessary and sufficient:

| Operation | Who | Ordering | Why |
|---|---|---|---|
| read **own** index (producer reads `tail`, consumer reads `head`) | owner | `relaxed` | single writer; no cross-thread publication in reading your own value |
| read **other's** index (producer reads `head`, consumer reads `tail`) | observer | `acquire` | must see the slot writes the other side published |
| publish **own** index after touching the slot | owner | `release` | makes the slot read/write visible to the other side |

So: producer does `slot[t] = x; tail.store(t+1, release);` and reads `head.load(acquire)` to check for space. Consumer does `x = slot[h]; head.store(h+1, release);` and reads `tail.load(acquire)` to check for data. **No `seq_cst` anywhere.** That's the canonical, optimal SPSC.

---

## 6. seq_cst: correct, default, and usually overkill

`seq_cst` adds a single global total order over all sequentially-consistent operations — easy to reason about, which is why it's the **default** when you write `tail.store(8)` with no ordering argument. But that global order costs a full memory barrier (`mfence` or a `lock`-prefixed op on x86) on the **store** side, which an SPSC queue does on *every push*. Replacing `seq_cst` with `release`/`acquire` removes those barriers and is one of the easiest measurable wins this week.

```cpp
tail.store(t + 1);                                   // seq_cst (default) — emits a barrier
tail.store(t + 1, std::memory_order_release);        // release — no barrier on x86
```

> 💡 Rule of thumb: start with `seq_cst` to get it *correct*, confirm with ThreadSanitizer and a 10× determinism check, then **relax to release/acquire** and re-measure. Don't relax to `relaxed` on a publishing store — that's the one that silently breaks. Correct first, fast second (the Week-1 thesis, applied to orderings).

---

## 🎯 Key Takeaways

- Compiler **and** CPU reorder memory; orderings forbid exactly the cross-thread reorderings that break a protocol. x86 is strong but the compiler still reorders, and ARM doesn't — write correct orderings.
- `relaxed`: atomic, no ordering — only for standalone values (stat counters, your own index).
- `release` (store) + `acquire` (load) are a **matched pair**: observing a released value means you see everything written before it. Producer publishes last; consumer consumes after observing.
- An **SPSC** queue needs only: `relaxed` to read your own index, `acquire` to read the other's, `release` to publish your own. No CAS, no `seq_cst`.
- `seq_cst` is the safe default but adds a full barrier per store. **Get it correct with `seq_cst`, then relax to release/acquire and measure** — never relax a publishing store to `relaxed`.

---

## 📚 Further Reading — Memory Orderings

- 📖 [cppreference — `std::memory_order`](https://en.cppreference.com/w/cpp/atomic/memory_order) — the normative definitions, with the release/acquire example.
- 🎬 [CppCon 2016 — Hans Boehm, "Using weakly ordered C++ atomics correctly"](https://www.youtube.com/watch?v=M15UKpNlpeM) — by an author of the C++ memory model; exactly what acquire/release buy you.
- 📰 [Jeff Preshing — "Acquire and Release Semantics"](https://preshing.com/20120913/acquire-and-release-semantics/) — the clearest picture of the one-way barriers.
- 📰 [Preshing — "This Is Why They Call It a Weakly-Ordered CPU"](https://preshing.com/20121019/this-is-why-they-call-it-a-weakly-ordered-cpu/) — proof that "it works on x86" is not "it's correct".

---

## ▶️ Next

[`04-spsc-ring-buffer.md`](./04-spsc-ring-buffer.md) — assemble these orderings into a real lock-free ring buffer: power-of-two capacity, head/tail on separate cache lines, wait-free push and pop. ⚡
