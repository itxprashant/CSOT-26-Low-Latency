# 06 — ⭐ Bonus: The Bandwidth Ceiling, Thread Pools & NUMA

> **TL;DR** — Once your reduction is race-free, false-sharing-free, and pinned, you stop being compute-bound and start being **memory-bandwidth-bound**: the aggregator reads the whole stream once and does almost no arithmetic per byte, so beyond ~2–3 threads you're waiting on DRAM, not cores. The wins shift from "more threads" to "move fewer bytes, move them efficiently": streaming reads, prefetch, a compact hot record, a thread *pool* instead of re-spawning, and (on multi-socket boxes) NUMA-aware placement. This file is optional and points ahead to Week 4.

This is the ⭐ bonus tier. Everything in [`01`](./01-going-wide.md)–[`05`](./05-scheduler-and-pinning.md) gets you a correct, scaling aggregator. This file is about the *ceiling* you hit afterward and the last percentage points — useful for the top of the board, skippable for a solid submission.

---

## 1. The arithmetic intensity problem

**Arithmetic intensity** = useful FLOPs (or ops) per byte moved from memory. The aggregator's is brutally low: per 32-byte `AggTick` you do a handful of integer adds and a min/max. You touch ~32 bytes of DRAM to do ~5 cheap ops. That makes you **memory-bound**: the bottleneck is how fast the memory subsystem can stream bytes into the cores, not how fast the cores compute.

The **roofline** mental model: plot performance against arithmetic intensity and there's a sloped "memory bandwidth" roof and a flat "compute" roof. Low-intensity kernels like ours live under the *bandwidth* roof — you hit it with just 2–3 cores saturating the memory controller, and a 4th core adds little because there's no spare bandwidth to feed it. That's why your laptop's "4×" might be the judge's "2.8×": you ran out of bandwidth, not cores.

```text
throughput
  │              ┌──────────── compute roof (we never reach it)
  │             /
  │   bandwidth/  ← low-intensity reductions are capped here
  │   roof    /
  │         /
  └────────────────────────── arithmetic intensity (ops/byte)
            ▲ the aggregator lives here (far left)
```

---

## 2. Consequences: move fewer bytes, move them well

If bandwidth is the wall, the wins are all about the memory traffic, not the threads:

- **One pass, sequential.** Read the stream front-to-back once. Sequential access lets the hardware prefetcher run ahead and uses full cache lines. Random or multi-pass access wastes the scarce bandwidth.
- **Prefetch the next lines.** `__builtin_prefetch(&ticks[i + K])` a few iterations ahead can hide DRAM latency behind compute — sweep `K` and measure (same trick as Week 2's bonus).
- **Keep the hot record compact.** You can't change `AggTick` (it's frozen, 32 B), but you *can* avoid bloating your per-thread partials and touching them in a cache-friendly order. A 1024-row × 40 B partial is 40 KiB — bigger than L1; if a thread's symbol distribution is skewed, the hot rows stay in L1 and the cold ones don't, which is fine.
- **Don't write what you won't read.** Re-zeroing a 40 KiB partial every `run()` is cold-path work; do it in `on_init` and reuse (Week 2's `clear()`/reuse lesson).

> 💡 Measure the ceiling directly: a tool like `STREAM`, or `perf stat -e ... ` watching memory-controller counters, tells you your box's GB/s. Divide by 32 B/tick and you have the *theoretical* max ticks/s — if you're close, more threads won't help and you should stop.

---

## 3. Thread pools: stop paying to spawn

[`02-stdthread-basics.md`](./02-stdthread-basics.md) noted spawning a thread costs ~10–50 µs. For a single timed `run()` that's a one-off in your serial fraction (Amdahl tax). But if you ever time *many* smaller `run()`s, or want the leanest possible startup, build a **thread pool** in `on_init` and *wake* the workers in `run()` instead of creating them:

```text
on_init: spawn T workers, each parks on a condition_variable / atomic flag
run():   publish the chunk bounds, signal "go", workers sweep, signal "done"
         (no thread creation in the timed region)
```

The pool turns per-`run()` thread creation into a cheap wake-up. The synchronization to wake and collect workers (condition variables, or atomics + spinning) is a *handoff* — which is exactly the lock-free territory Week 4 opens. For Week 3's single big `run()`, a plain spawn-join is usually fine; the pool is a top-of-board refinement.

> ⚠️ A pool reintroduces shared state (the "go"/"done" signals) between the main thread and workers. That's a synchronization problem — use `std::atomic` or a `std::condition_variable` correctly, or you've recreated [`03-data-races.md`](./03-data-races.md)'s race in the coordination layer.

---

## 4. NUMA: when memory has a near side and a far side

On a single-socket box (like the judge's `c7i.xlarge`) all cores share one memory controller and this section is academic — but it's how real trading boxes are built, so know it.

On a **NUMA** (Non-Uniform Memory Access) machine with multiple sockets, each socket has its own local DRAM. Accessing your *own* socket's memory is fast; reaching *across* to another socket's memory is markedly slower. The trap is the **first-touch** policy: Linux places a physical page on the NUMA node of the thread that first *writes* it — not the thread that allocates it. So if your main thread zeroes all the partials, they all land on one node, and workers on other sockets pay the remote-access tax forever.

The fix is **first-touch from the owning thread**: have each worker initialize (zero) *its own* partial table, so each table is placed local to the core that will hammer it. Tools: `numactl --hardware` to see the topology, `numactl --cpunodebind=… --membind=…` to pin a process, and `libnuma` for fine control.

```bash
numactl --hardware                 # show NUMA nodes and inter-node distances
numactl --cpunodebind=0 --membind=0 ./build/agg_runner data/large.ticks
```

---

## 5. The bridge to Week 4

Notice what kept appearing: the moment threads must *coordinate* — wake a pool, hand off a chunk, share a "done" flag — you're back in synchronization, and a mutex would serialize the very thing you parallelized. Week 4 is the principled answer: `std::atomic`, memory orderings (`relaxed`/`acquire`/`release`), compare-and-swap, and a **lock-free single-producer/single-consumer ring buffer** for handing data between threads without a lock. The aggregator's batch reduction barely needs it; the *streaming* pipeline that comes back in Week 4 (one thread ingests, another reduces, continuously) needs it badly.

For now: get the reduction bandwidth-efficient, prove it's race-free, and enjoy that the leaderboard is finally a contest of who understands the memory system best — exactly where this track has been pointing since Week 1.

---

## 🎯 Key Takeaways

- The aggregator has **low arithmetic intensity** (few ops per byte), so it's **memory-bandwidth-bound** — you hit the roof at ~2–3 cores and a 4th adds little.
- Under the bandwidth roof, optimize **bytes moved**: one sequential pass, prefetch (`__builtin_prefetch(&ticks[i+K])`), compact/reused partials, no needless re-zeroing in the hot path.
- Estimate your ceiling: box GB/s ÷ 32 B/tick = max ticks/s. If you're near it, **stop adding threads.**
- A **thread pool** (spawn in `on_init`, wake in `run()`) removes per-`run()` spawn cost — but its wake/collect signals are a synchronization problem (Week 4 tools).
- **NUMA + first-touch:** on multi-socket boxes, have each worker initialize its **own** partial so pages land local; `numactl` controls placement. (Single-socket judge box: not a factor, but real HFT boxes are NUMA.)
- The coordination patterns here are the on-ramp to Week 4's atomics and lock-free SPSC queue.

---

## 📚 Further Reading — Bandwidth, Pools & NUMA

- 📰 [Roofline model (Berkeley) — overview](https://crd.lbl.gov/divisions/amcr/computer-science-amcr/par/research/roofline/) — arithmetic intensity vs. the bandwidth/compute roofs.
- 📖 [STREAM benchmark (McCalpin)](https://www.cs.virginia.edu/stream/) — measure your machine's real memory bandwidth in GB/s.
- 📰 ["What Every Programmer Should Know About Memory" (Drepper), §5 NUMA](https://people.freebsd.org/~lstewart/articles/cpumemory.pdf) — first-touch and NUMA placement, in depth.
- 🎬 [CppCon — David Gross, "Designing a Fast, Efficient, Cache-Friendly Hardware-Aware …"](https://www.youtube.com/watch?v=BD9cRbxWQx8) — bandwidth-bound thinking and thread coordination in practice.
- 📖 [cppreference — `std::condition_variable`](https://en.cppreference.com/w/cpp/thread/condition_variable) — the standard wait/notify primitive a pool is built on.

---

## ▶️ Next

That's the reading for Week 3. Now go build it: the [**project brief**](./project/README.md) turns these five (plus bonus) lessons into a single `aggregator.cpp` on the live leaderboard. Week 4 picks up the coordination thread with lock-free queues. ⚡
