# 05 — The Scheduler & Core Pinning: Stop the OS From Moving Your Threads

> **TL;DR** — The OS scheduler time-slices your threads across cores and can pre-empt or migrate them at any moment. A migration mid-sweep abandons a warm L1/L2 cache and refills it on the new core (~thousands of cycles); a context switch steals a few microseconds. Both inject jitter into your timed `run()`. Pin each worker to a distinct core with `sched_setaffinity` so it keeps its cache and isn't shuffled. Pinning doesn't make the average faster so much as it makes the tail *predictable* — which is what a wall-clock leaderboard rewards.

[`04-false-sharing.md`](./04-false-sharing.md) made your threads stop fighting over cache lines. This file stops a different adversary: the kernel itself, which by default treats your carefully-placed threads as interchangeable and moves them around.

---

## 1. The scheduler is always there

Linux runs far more threads than you have cores. The **scheduler** (CFS, or EEVDF on newer kernels) hands each runnable thread a slice of a core, then pre-empts it to run something else — your other processes, kernel threads, that background updater. Two things it does routinely will hurt a tight reduction:

- **Context switch:** save your thread's registers, load another's. Direct cost ~1–5 µs; the *indirect* cost is the cache and TLB the other thread evicts while it runs.
- **Migration:** resume your thread on a *different* core than it last ran on. The new core's L1/L2 is cold for your data, so your first accesses after migration all miss.

For a job timed in milliseconds over a multi-GB stream, a handful of migrations is measurable jitter; for Week 4's per-tick latency work it's fatal.

---

## 2. Why migration is expensive: you abandon a warm cache

A worker that's been sweeping its chunk has its hot data — its partial table, the stream lines it's prefetching — resident in *that core's* L1/L2. Migrate it to another core and all of that is somewhere else. The new core must re-fetch from L3 or DRAM:

| Event | Rough cost |
|---|---|
| L1 hit | ~1 ns |
| L2 hit | ~4 ns |
| L3 hit (after migration) | ~15–40 ns |
| DRAM (cold after migration) | ~80–100 ns |

A migration doesn't cost one miss — it costs a *burst* of them as the working set re-warms on the new core. Multiply by however many times the scheduler decides to shuffle you, at unpredictable points, and your `run()` time develops a fat, noisy tail. The mean might be fine; the p99 is wrecked.

---

## 3. CPU affinity: pin a thread to a core

The fix is **CPU affinity**: tell the kernel "this thread may only run on core X." On Linux that's `sched_setaffinity` with a `cpu_set_t` mask. Set it from inside the worker (or right after spawning it) so each worker owns a distinct core for the whole sweep.

```cpp
#include <pthread.h>     // pthread_self
#include <sched.h>       // cpu_set_t, CPU_ZERO, CPU_SET, sched_setaffinity

// Pin the calling thread to a single logical core. Call at the top of each worker.
inline void pin_to_core(int core) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(core, &set);
    sched_setaffinity(0, sizeof(set), &set);   // 0 = the calling thread
}

void worker(int core, const csot::AggTick* b, const csot::AggTick* e, csot::SymbolAgg* partial) {
    pin_to_core(core);                          // stay on `core` for the whole sweep
    // ... reduce [b, e) into partial ...
}
```

Spawn worker `k` and pin it to core `k` (for `k` in `0..T-1`, with `T <= hardware_concurrency()`). Now each worker keeps its caches warm and the scheduler can't migrate it.

> 💡 `sched_setaffinity` returns `-1` on failure (e.g. core out of range); it's a cold-path call so checking it is free. The portable C++ standard has **no** affinity API — pinning is inherently OS-specific, so this is `<sched.h>` on Linux (the judge's platform).

---

## 4. Pin to *distinct* cores, and mind hyperthreads

Two rules turn pinning from "nice idea" into a real win:

1. **One worker per core.** Pinning two heavy workers to the same core just trades migration for time-slicing on that core — no parallelism gained. Map worker `k` → core `k`, distinct cores.
2. **Beware sibling hyperthreads.** A 4-vCPU box might be 2 physical cores × 2 SMT threads, or 4 physical cores. Two SMT siblings share execution units and L1/L2, so pinning two bandwidth-hungry workers to siblings gives far less than 2× — they contend for the same hardware. The judge `c7i.xlarge` exposes 4 vCPUs; treat them as your 4 lanes and measure, but know that "4 vCPUs" can be fewer physical cores.

```bash
lscpu                              # see sockets x cores x threads-per-core
cat /sys/devices/system/cpu/cpu*/topology/thread_siblings_list   # who shares a core
```

For a coarse external pin (whole process), `taskset` is the quick tool; for per-worker control you want `sched_setaffinity` inside the code.

```bash
taskset -c 0-3 ./build/agg_runner data/large.ticks   # confine the process to cores 0..3
```

---

## 5. Pinning is about *predictability* more than speed

Here's the subtle part. On an otherwise-idle box, pinning often only nudges the *mean* throughput. What it really fixes is **variance**. Unpinned, your `run()` time wobbles run-to-run as the scheduler makes different decisions; pinned, it's tight and repeatable.

That matters because the judge times `run()` and **takes the best/median of K runs** ([`AGG_SPEC.md`](./project/AGG_SPEC.md) §9). A jittery implementation occasionally posts a great time and often a bad one; a pinned one posts the same good time every run. Predictability is rankable; noise is not. This is the same lesson as Week 2's "benchmark hygiene" (`taskset`, fixed governor, no turbo), now structural: you build the stability into the program.

> ⚠️ Don't over-claim cores. Pinning all 4 workers to all 4 vCPUs while the OS, the harness's main thread, and `perf` also want time can backfire. On a dedicated judge box it's fine; on your busy laptop, leave a core for the system or expect noise.

---

## 6. Putting the week together

You now have every piece of a fast, correct, stable aggregator:

```text
on_init(num_symbols):
    allocate T padded partial tables (alignas 64)        // 04: no false sharing
    (optionally) build a thread pool                     // 06: avoid re-spawn cost

run(ticks, n, out):
    spawn/wake T workers                                 // 02: cold-path spawn
        worker k: pin_to_core(k)                         // 05: keep caches warm
                  reduce ticks[n*k/T .. n*(k+1)/T)        // 02: clean partition
                  -> partials[k]                          // 03: private, no race
    join all                                             // 03: happens-before edge
    merge partials -> out                                // 01: small serial tail
```

That structure is the deliverable. Each topic file fixed one failure mode; together they're a reduction that actually hits ~4× on the judge and holds it steady.

---

## 🎯 Key Takeaways

- The OS scheduler pre-empts and **migrates** threads by default; both inject jitter into a timed `run()`.
- **Migration abandons a warm cache** — the working set re-warms on the new core as a burst of L2/L3/DRAM misses, fattening your tail.
- **`sched_setaffinity`** (Linux) pins a thread to a core. Call `pin_to_core(k)` at the top of worker `k`; there is no portable C++ affinity API.
- Pin **one worker per distinct core**, and beware **hyperthread siblings** sharing L1/L2 — "4 vCPUs" may be fewer physical cores (`lscpu`, `thread_siblings_list`).
- Pinning's main payoff is **predictability**, not raw mean speed — and the judge ranks the best/median of repeated runs, so stable beats jittery.
- `taskset -c 0-3` is the quick external pin; `sched_setaffinity` is the per-worker, in-code control.

---

## 📚 Further Reading — Scheduling & Affinity

- 📖 [`man sched_setaffinity(2)`](https://man7.org/linux/man-pages/man2/sched_setaffinity.2.html) and [`man cpuset(7)`](https://man7.org/linux/man-pages/man7/cpuset.7.html) — the affinity API and CPU masks.
- 🎬 [CppCon — Carl Cook, "When a Microsecond Is an Eternity"](https://www.youtube.com/watch?v=NH1Tta7purM) — core pinning and isolating cores from the scheduler, from HFT.
- 📰 [Linux kernel docs — CFS / EEVDF scheduler](https://docs.kernel.org/scheduler/sched-design-CFS.html) — what the scheduler is actually doing to your threads.
- 📰 [Johnny's Software Lab — "CPU affinity"](https://johnnysswlab.com/the-price-of-thread-migration/) — measured cost of thread migration and how pinning removes it.

---

## ▶️ Next

[`06-bonus-bandwidth-and-pools.md`](./06-bonus-bandwidth-and-pools.md) — ⭐ the bonus tier: when you're memory-bandwidth-bound, NUMA first-touch, reusing threads with a pool instead of re-spawning, and a teaser for Week 4's lock-free handoff.
