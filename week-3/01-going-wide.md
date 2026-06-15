# 01 — Going Wide: Parallelism, the Map-Reduce Shape, and Amdahl's Tax

> **TL;DR** — Adding cores only helps the part of your program that can run in parallel. A reduction over a huge array (count/sum/min/max per symbol) is *embarrassingly parallel* — split the array into chunks, reduce each on its own thread, merge the partials. But the serial merge (and memory bandwidth) caps your speedup: Amdahl's law says 5% serial work limits you to 20× *no matter how many cores you buy*. This week you make the Week-1/2 aggregation go wide — and meet the friction that makes "just add threads" a lie.

Weeks 1–2 were about making a **single** core fast: measure honestly, lay data out for the cache, never `malloc` on the hot path. You squeezed one thread until it hummed. Now we go the other direction: use **all** the cores. The catch is that threads are not free speedup — they fight over caches, get pre-empted by the kernel, and corrupt each other's data. This file sets up the mental model; the rest of the week is the friction.

---

## 1. Two ways to use more hardware

There are exactly two levers once a single core is saturated:

| Lever | What it means | Example |
|---|---|---|
| **Scale up (vertical)** | a faster core: higher clock, wider SIMD, bigger cache | Week 2's SIMD tag scan |
| **Scale out (horizontal)** | more cores doing the work at once | this week |

You already pulled the scale-up lever as far as a laptop allows. Scaling out is the other half — and it is how every real low-latency system handles volume: one core ingests the wire, another runs the strategy, another logs. The art is splitting the work so the cores *don't* step on each other.

---

## 2. Embarrassingly parallel: the easiest win in computing

A workload is **embarrassingly parallel** when it splits into independent pieces that need no communication while they run. Summing a billion numbers is the canonical example: chop the array into N chunks, sum each chunk on its own thread, then add the N partial sums. The threads never talk until the very end.

Our Week-3 aggregator is exactly this shape. Computing per-symbol `count`, `sum_price`, `min_price`, `max_price`, `sum_qty` over a huge tick stream is a **reduction**: each tick contributes to exactly one symbol's row, and the contributions combine with `+`, `min`, `max` — all associative and commutative. So the order of processing, and the way we split the stream, *cannot* change the answer (see [`AGG_SPEC.md`](./project/AGG_SPEC.md) §7). That property is what makes the whole thing parallelizable and the leaderboard fair.

---

## 3. The map-reduce shape

The pattern has a name borrowed from distributed systems: **map-reduce**.

```text
          ┌── chunk 0 ──► reduce ──► partial 0 ┐
 stream ──┼── chunk 1 ──► reduce ──► partial 1 ┼──► merge ──► final table
          ├── chunk 2 ──► reduce ──► partial 2 ┤
          └── chunk 3 ──► reduce ──► partial 3 ┘
            (parallel: one thread each)         (serial)
```

- **Map / local reduce** (parallel): each thread sweeps its chunk of the stream into its *own* private partial table. No sharing, no locks, no communication.
- **Merge** (serial, or a small parallel tree): combine the N partial tables into the final one. For 1024 symbols × N threads this is tiny compared to the stream sweep.

The whole game this week is: make the parallel part genuinely parallel (no accidental sharing — that's [`04-false-sharing.md`](./04-false-sharing.md)), and keep the serial part small.

```cpp
// One thread's local reduce: a chunk in, a private partial table out.
// (Sketch — the real thing lives in your aggregator.cpp.)
void reduce_chunk(const csot::AggTick* begin, const csot::AggTick* end,
                  csot::SymbolAgg* partial /* num_symbols rows, private to this thread */) {
    for (const csot::AggTick* t = begin; t != end; ++t) {
        csot::SymbolAgg& r = partial[t->symbol_id];
        if (r.count == 0) { r.min_price = t->price; r.max_price = t->price; }
        else { r.min_price = std::min(r.min_price, t->price);
               r.max_price = std::max(r.max_price, t->price); }
        r.count += 1; r.sum_price += t->price; r.sum_qty += t->qty;
    }
}
```

Each thread owns its `partial`. Nobody writes to anyone else's. That's the discipline.

---

## 4. Amdahl's law: why 1000 cores won't save a serial program

Here is the rule that humbles every "just add threads" plan. If a fraction **p** of your work is parallelizable and **(1 − p)** is inherently serial, then with **N** cores your speedup is:

```text
speedup(N) = 1 / ( (1 - p) + p / N )
```

Take the limit as N → ∞ and the `p/N` term vanishes:

```text
max speedup = 1 / (1 - p)
```

So if **5%** of your runtime is serial (`p = 0.95`), you can never beat **20×**, even with infinite cores. If 50% is serial, you cap at **2×**. The serial fraction — your merge, your thread spawn, your final write-out — is the tax, and it dominates as you add cores.

| Serial fraction | Max speedup (∞ cores) | Speedup at 4 cores |
|---|---:|---:|
| 0% | ∞ | 4.0× |
| 5% | 20× | 3.48× |
| 10% | 10× | 3.08× |
| 25% | 4× | 2.29× |
| 50% | 2× | 1.60× |

The judge box has **4 vCPUs**, so your realistic ceiling is ~4×, and only if your serial fraction (spawning threads, merging 1024-row partials, allocating) is tiny. This is why you allocate partials in `on_init` (cold path), keep the merge cheap, and don't re-spawn threads you could reuse.

> 💡 Amdahl assumes a *fixed* problem size. Its cousin **Gustafson's law** observes that in practice we scale the *problem* with the cores (a bigger hidden stream), which keeps parallel efficiency high. The judge's hidden stream is large on purpose — so going wide genuinely pays.

---

## 5. Latency vs. throughput, under threads

Week 1 drew the line between **latency** (time for one operation) and **throughput** (operations per second). Threads change them differently:

- **Throughput scales** with cores on an embarrassingly-parallel job — that's the win, and it's what the aggregator leaderboard measures (ticks/second over the whole stream).
- **Latency of a single tick does not improve** by adding threads — one tick is still processed by one core. Threads help you process *more ticks at once*, not *one tick faster*.

This is why Week 3's challenge is a **throughput** challenge (reduce a giant stream fast) and Week 4's threaded strategy will care about **latency** again (one tick → one decision, fast). Know which one you're optimizing; the techniques differ.

---

## 6. The friction preview

"Split, reduce, merge" sounds trivial. It is — until real threads get involved. The next four files are the friction, in the order it bites:

| File | The friction |
|---|---|
| [`02-stdthread-basics.md`](./02-stdthread-basics.md) | spawning, joining, and partitioning work correctly (off-by-one at chunk edges loses ticks) |
| [`03-data-races.md`](./03-data-races.md) | two threads touching one variable → lost updates, undefined behaviour, non-determinism |
| [`04-false-sharing.md`](./04-false-sharing.md) | "private" partials that share a cache line → 4 threads slower than 1 |
| [`05-scheduler-and-pinning.md`](./05-scheduler-and-pinning.md) | the kernel migrating and pre-empting your threads → jitter in your timed `run()` |

Get those right and the map-reduce above turns into a real ~4× on the judge. Get them wrong and you'll watch threads make your code *slower* — a genuinely useful thing to witness once.

---

## 🎯 Key Takeaways

- Two levers when one core saturates: **scale up** (faster core) and **scale out** (more cores). Week 3 is scale-out.
- A workload is **embarrassingly parallel** when pieces run independently with no mid-flight communication. The aggregator is exactly this.
- The **map-reduce** shape: parallel local-reduce into private partials, then a small serial merge. Each thread owns its partial — no sharing.
- The aggregates are associative + commutative, so **partition and order don't change the answer** — the basis for both parallelism and a fair correctness gate.
- **Amdahl's law:** `speedup = 1/((1-p) + p/N)`; a 5% serial fraction caps you at 20× with infinite cores, ~3.5× at 4. Keep the serial part (spawn, merge, alloc) tiny.
- Threads scale **throughput**, not single-op **latency**. Week 3 is a throughput challenge; know which you're tuning.

---

## 📚 Further Reading — Parallelism Foundations

- 📖 [Amdahl's law — Wikipedia](https://en.wikipedia.org/wiki/Amdahl%27s_law) — the formula, the derivation, and Gustafson's rebuttal in one place.
- 🎬 [CppCon — Fedor Pikus, "The Speed of Concurrency (Is Lock-free Faster?)"](https://www.youtube.com/watch?v=9hJkWwHDDxs) — sets up why naive sharing kills scaling (we cash this in for false sharing).
- 📰 [Google — "MapReduce: Simplified Data Processing on Large Clusters"](https://research.google/pubs/pub62/) — the paper that named the shape; the same idea at datacenter scale.
- 📖 ["C++ Concurrency in Action" (Anthony Williams), ch. 8](https://www.manning.com/books/c-plus-plus-concurrency-in-action-second-edition) — "Designing concurrent code": partitioning data and the cost of the serial parts.

---

## ▶️ Next

[`02-stdthread-basics.md`](./02-stdthread-basics.md) — the actual tools: `std::thread`/`std::jthread`, joining, and how to split `[0, n)` into chunks without losing a single tick.
