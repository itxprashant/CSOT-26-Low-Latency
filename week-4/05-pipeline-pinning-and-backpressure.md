# 05 — Wiring the Pipeline: Pinning, Back-Pressure, Balance

> **TL;DR** — A correct ring buffer is half the job; the other half is running the two threads well. Pin the producer and consumer to **distinct physical cores** (Week 3's `sched_setaffinity`) so they stop migrating and so their private indices stay cache-hot. Decide how a thread waits when the queue is full/empty — **spin** for lowest latency, **back off** to be polite. Balance the two stages so neither starves the other. Then measure the overlap you built, against the single-threaded baseline.

The ring buffer ([`04`](./04-spsc-ring-buffer.md)) is the channel. This file is everything around it that turns "two threads that share a queue" into "two cores that stay busy."

---

## 1. Spawn cold, run hot — and account for the spawn

Creating a `std::thread` costs ~10–50 µs ([Week 3](../week-3/02-stdthread-basics.md)). For a single timed `run()` over a huge feed, spawning the producer and consumer once at the top of `run()` is a fixed cost amortized over millions of ticks — negligible. The structure:

```cpp
std::size_t run(const csot::WireTick* in, std::size_t n, csot::OrderRecord* out) override {
    std::atomic<bool> done{false};
    std::size_t num_orders = 0;

    std::thread producer([&] { /* pin; decode in[0..n); push; set done */ });
    // consumer runs on THIS thread (no extra spawn) — pin it too
    /* pin this thread; pop -> strategize -> append to out; stop when drained && done */

    producer.join();
    return num_orders;
}
```

Running the consumer on the calling thread saves one spawn and one join. Allocate the ring storage, per-symbol state, and interned names in `on_init()` — `run()` stays heap-free (the Week-2 rule still holds).

> 💡 The thread *creation* is cold-path, but the work each thread does is the hot path. Don't allocate, log, or branch on rare conditions inside the push/pop loop.

---

## 2. Pin both threads to distinct cores

Without pinning, the scheduler migrates your producer and consumer across cores mid-run. Each migration drops the thread's cache (including its private `head`/`tail` line) on the floor and refills it on the new core — jitter you'll see as run-to-run variance. The fix is [Week 3's](../week-3/05-scheduler-and-pinning.md) `sched_setaffinity`, applied to *both* pipeline threads, on *different* cores:

```cpp
#include <pthread.h>
#include <sched.h>

void pin_to(int core) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(core, &set);
    sched_setaffinity(0, sizeof(set), &set);   // 0 = the calling thread
}
// producer: pin_to(2);   consumer: pin_to(3);   (two distinct cores)
```

Two subtleties on the 4-vCPU judge box:

- **Distinct physical cores beat hyperthread siblings.** Two SMT siblings share L1/L2 and execution ports; pinning producer and consumer to *sibling* hyperthreads makes them fight for the same cache the ring buffer lives in. Prefer two different physical cores (`lscpu`, `thread_siblings_list`).
- **Leave a core for the OS.** You have two hot threads on a 4-vCPU box; that's comfortable. Don't pin both to core 0 (where interrupts land) and don't oversubscribe.

Pinning typically does little to the *mean* throughput but tightens the *variance* dramatically — and the leaderboard's best/median-of-K rewards a tight distribution.

---

## 3. Back-pressure: what to do when the queue is full or empty

`try_push` returns `false` when full; `try_pop` returns `false` when empty. The thread must decide how to wait. Three options, fastest-but-busiest first:

```cpp
// (a) Pure spin — lowest latency, burns a core. Right for a saturated feed.
while (!ring.try_push(t)) { /* nothing */ }

// (b) Spin then relax — hint the CPU you're spinning (frees SMT resources, saves power).
while (!ring.try_push(t)) {
    __builtin_ia32_pause();            // x86 PAUSE; or std::this_thread::yield()
}

// (c) Spin then sleep — polite under contention, adds wake-up latency (~µs). Rarely worth it here.
```

For this challenge — a producer and consumer both pinned, both saturated, racing to drain a huge feed — **pure spin or spin+`pause` is the right call**: there's always more work coming, so sleeping just adds latency. The `pause` variant (`_mm_pause()` / `__builtin_ia32_pause()`) is a one-instruction "I'm in a spin loop" hint that improves an SMT sibling's throughput and cuts power; sprinkle it into the empty/full wait. Avoid a mutex/condition-variable wait — that's the lock you removed in [`01`](./01-from-batch-to-streaming.md).

> ⚠️ Never *drop* a tick under back-pressure. Full means *wait*, not *overwrite*. Dropping or overwriting an unconsumed slot changes the order stream (PIPELINE_SPEC.md §8) and fails correctness.

---

## 4. Balance the stages

From [`01`](./01-from-batch-to-streaming.md): the pipeline runs at the speed of its *slowest* stage, `max(D, S)`. If one stage dominates, the other idles and your second core is wasted. So make both cheap and comparable:

- **Decode (`D`)** is already light — two integer→double conversions and a symbol lookup. Keep the symbol interning O(1): index a pre-built `names_[symbol_id]` table (built in `on_init`), don't hash a string per tick.
- **Strategy (`S`)** is the heavier stage if you implement it naively (it recomputes a 64-element mean and variance every tick — that's the [STRATEGY_SPEC.md](../week-1/project/STRATEGY_SPEC.md) §6 *clarity* reference, not a fast one). Bring back **Week-2 rolling sums**: keep a running `sum` and `sum_of_squares`, update them in O(1) as the window slides, and you drop `S` by ~30×. Now `D` and `S` are comparable and the overlap actually delivers near-2×.

A lopsided pipeline is the most common reason a correct two-thread submission barely beats one thread. Profile both stages (`perf record` per thread) and shrink the bigger one.

---

## 5. Measure the overlap you built

Prove the pipeline works with three numbers:

1. **Serial baseline.** The shipped stub (decode + strategy on one thread). Record its throughput: `\_\_\_\_\_` M ticks/s.
2. **Your pipeline.** Two threads, lock-free ring, pinned. Record: `\_\_\_\_\_` M ticks/s and the speedup vs. baseline.
3. **A mutex queue, for contrast.** Swap your ring for a `std::mutex` + `std::queue` and watch it fall *below* the baseline. That's the lesson of the week in one measurement.

```bash
# throughput is printed on stderr by the harness
./build/pipeline_runner data/large.feed
perf stat -e cache-misses,context-switches,instructions ./build/pipeline_runner data/large.feed
```

A `head`/`tail` false-sharing bug shows up as high `cache-misses`; unpinned threads show high `context-switches`. A clean, pinned, lock-free pipeline shows neither and a speedup approaching `(D+S)/max(D,S)`.

> 📌 Determinism is non-negotiable (PIPELINE_SPEC.md §7). Run `run()` 10× and confirm the order-stream checksum is identical every time, and that a TSan build is clean — *before* you chase the last few percent of throughput.

---

## 🎯 Key Takeaways

- Spawn the producer once at the top of `run()` (run the consumer on the calling thread); allocate everything in `on_init()`. Thread creation is cold-path; the push/pop loop is hot.
- **Pin both threads** to distinct **physical** cores with `sched_setaffinity` — it mostly tightens variance, which best/median-of-K rewards. Avoid hyperthread siblings; leave a core for the OS.
- Under back-pressure, **spin (with `pause`)** — there's always more work; never sleep on a lock and never drop a tick.
- **Balance the stages.** Use Week-2 rolling sums to shrink the strategy stage so `D ≈ S` and the overlap delivers near-2×. A lopsided pipeline wastes the second core.
- **Measure**: serial baseline vs. lock-free pipeline vs. mutex queue. Confirm determinism (10× checksum + TSan) before optimizing throughput.

---

## 📚 Further Reading — Pinning, Spinning, Pipelines

- 🎬 [CppCon 2017 — Carl Cook, "When a Microsecond Is an Eternity"](https://www.youtube.com/watch?v=NH1Tta7purM) — pinning, busy-waiting, and never sleeping on the hot path in real HFT.
- 📰 [Intel — `_mm_pause` / the PAUSE instruction in spin-wait loops](https://www.intel.com/content/www/us/en/docs/intrinsics-guide/index.html#text=_mm_pause) — why a spin loop should hint the CPU.
- 📰 [Rigtorp — "Pinning threads to CPU cores"](https://rigtorp.se/low-latency-guide/) — a practical low-latency tuning checklist (affinity, isolation, governors).
- 📖 [cppreference — `std::this_thread::yield`](https://en.cppreference.com/w/cpp/thread/yield) — the polite alternative to a pure spin, and when it helps.

---

## ▶️ Next

[`06-bonus-disruptor-and-beyond.md`](./06-bonus-disruptor-and-beyond.md) — ⭐ bonus: batching and the LMAX Disruptor, multi-producer/consumer queues, and the bridge to Week 5's network ingest. ⚡
