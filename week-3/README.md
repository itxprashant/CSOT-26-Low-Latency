# Week 3 — Parallelization, Thread Friction & the Parallel Tick Aggregator

> *"Threads don't make code fast. They make it possible to be fast — if you stop them from fighting."*
>
> *"The fastest reduction is the one where four cores never touch the same cache line."*

Welcome to Week 3 of the Low Latency track. Weeks 1–2 made a **single** core fast — honest measurement, cache-friendly layout, zero allocation. This week you go **wide**: use every core to reduce a huge market-tick stream, and meet the three frictions that make "just add threads" a lie — data races, false sharing, and scheduler jitter. The new ranked challenge is a **parallel tick aggregator**: same "fastest correct implementation" game, a workload that finally rewards going wide.

By the end of this week you will have:

- ✅ Understood the **map-reduce** shape and **Amdahl's law** — why a 5% serial fraction caps you at 20×
- ✅ Spawned, joined, and **partitioned** work across `std::thread`s without losing a tick
- ✅ Deliberately written a **data race**, watched it lose updates, and caught it with **ThreadSanitizer**
- ✅ Made four "private" tables **false-share a cache line** — and fixed it with one `alignas(64)`
- ✅ **Pinned** threads to cores with `sched_setaffinity` to kill scheduler jitter
- ✅ A single `aggregator.cpp` on the **live leaderboard**, ranked by wall-clock speed over millions of ticks

---

## 📖 Reading Order

Work through these in order. Each builds on the last and ends with a curated **"Further Reading"** section.

| # | File | Topic | Est. time |
|---|------|-------|-----------|
| 1 | [`01-going-wide.md`](./01-going-wide.md) | Embarrassingly-parallel work, the map-reduce shape, Amdahl's law, throughput vs. latency | 20 min |
| 2 | [`02-stdthread-basics.md`](./02-stdthread-basics.md) | `std::thread`/`jthread`, join as a sync point, cold-path spawn, partitioning `[0,n)` correctly | 20 min |
| 3 | [`03-data-races.md`](./03-data-races.md) | Lost updates, data races as UB, why locks are wrong on the hot path, ThreadSanitizer | 25 min |
| 4 | [`04-false-sharing.md`](./04-false-sharing.md) | Cache-line coherency, false sharing, `alignas(64)`, thread-major layout, `perf` diagnosis | 30 min |
| 5 | [`05-scheduler-and-pinning.md`](./05-scheduler-and-pinning.md) | Context switches, migration cost, `sched_setaffinity`, hyperthreads, predictability | 25 min |
| ⭐ | [`06-bonus-bandwidth-and-pools.md`](./06-bonus-bandwidth-and-pools.md) | **Bonus:** the memory-bandwidth ceiling, prefetch, thread pools, NUMA first-touch, Week-4 bridge | 30 min |

Total reading: **~2–2.5 hours** (the ⭐ bonus is optional but highly recommended). Then the real work → the [**project**](./project/README.md).

---

## 🛠️ Setup

You already have the Week-1/2 toolchain. Week 3 adds nothing mandatory — same compiler, CMake, `perf`, Python 3 — but you'll lean on threads and a race detector. Confirm:

```bash
g++ --version          # ≥ 11 (for C++20 std::jthread; ≥ 10 works with std::thread)
cmake --version        # ≥ 3.20
perf --version         # Linux only
nproc                  # how many logical cores you have (the judge has 4)
```

Confirm your toolchain has the sanitizers you'll use to prove correctness:

```bash
echo 'int main(){}' | g++ -fsanitize=thread    -x c++ - -o /tmp/tsan_ok && echo "TSan OK"
echo 'int main(){}' | g++ -fsanitize=address    -x c++ - -o /tmp/asan_ok && echo "ASan OK"
```

> **WSL2 / macOS users:** native Linux matters more this week than ever. `sched_setaffinity` is Linux-only (macOS has no real thread-pinning API), `perf`'s cache-coherency counters are Linux-only, and core counts under a VM are virtualized and noisy. The code is correct anywhere, but trustworthy parallel numbers — and the pinning exercise — want bare-metal Linux. The judge runs Linux on 4 vCPUs; develop as close to that as you can.

---

## 🎯 Project Brief

[**→ Open `project/README.md`**](./project/README.md)

In one sentence: **implement a single `aggregator.cpp` that reduces a huge binary tick stream into a fixed per-symbol table of integer aggregates, correctly and deterministically, as fast as four cores allow.**

This week's project scope:

1. Read and internalize the frozen [`project/AGG_SPEC.md`](./project/AGG_SPEC.md) — the record format, the five integer aggregates, the empty-symbol rule, and the all-important determinism guarantee.
2. Implement a **correct** aggregator against the [`project/include/aggregate.hpp`](./project/include/aggregate.hpp) ABI; verify against [`project/data/tiny.agg.json`](./project/data/tiny.agg.json). (The shipped stub is already correct single-threaded — your job is speed.)
3. Make it **parallel** (partition the stream across threads), **false-sharing-free** (`alignas(64)` per-thread partials), **pinned** (`sched_setaffinity`), and **zero-allocation** in `run()` (size partials in `on_init`).
4. Measure throughput + `run()` wall-clock and **scaling** (throughput at 1–4 threads) with the shipped harness, `perf stat`, and `tools/plot_scaling.py`.
5. Upload your **single `aggregator.cpp`** to **[csot-low-latency.devclub.in](https://csot-low-latency.devclub.in/dashboard/)** — the judge builds it itself (fixed flags, `-pthread`), runs it several times, and ranks correct, deterministic aggregators by speed.

> **Same as Week 2:** you submit **source** (`aggregator.cpp`), not a compiled `.so`. The judge owns the compiler and flags, so the board reflects your code, not your laptop. **New this week:** the judge runs your submission K times and **rejects non-deterministic results** — a data race that "passes sometimes" does not rank. Details in [`platform_week_3.md`](../../platform_week_3.md).

Lock-free queues, atomics, and the streaming pipeline come in Week 4. **Build the boring correct single-threaded version first** (it's the shipped stub) — then make it go wide.

---

## ✅ Week 3 Checklist

> Copy this into your tracker. **If you can tick every box in Phases 0–4, you're done with Week 3.** Phase 5 and the stretch goals are bonus glory.

Suggested pace: **~5–7 days, ~2 hours/day.**

### Phase 0 — Environment Setup (Day 0, ~15 min)

- [ ] Week-1/2 toolchain still works (`g++ ≥ 11`, `cmake ≥ 3.20`, `python3 ≥ 3.8`, `perf` on Linux).
- [ ] `nproc` reports your core count; you know whether they're physical or hyperthreads (`lscpu`).
- [ ] TSan and ASan both link a trivial program (commands above).
- [ ] Copied the ★ project files into a fresh project (or a `week-3/` folder in your repo).
- [ ] Header compiles: `g++ -std=c++20 -Iinclude -c include/aggregate.hpp -o /tmp/x.o` is silent (both `static_assert`s pass).
- [ ] `cmake -B build && cmake --build build -j` builds `agg_runner` from the stub; `./build/agg_runner data/tiny.ticks` diffs clean against `data/tiny.agg.json`.

### Phase 1 — Reading (Days 1–2, ~2–2.5 hours)

After each note, write **one sentence** about what surprised you.

- [ ] [`01-going-wide.md`](./01-going-wide.md) — map-reduce + Amdahl
  - [ ] I can state Amdahl's law and compute the 4-core ceiling for a 10% serial fraction.
- [ ] [`02-stdthread-basics.md`](./02-stdthread-basics.md) — threads, join, partition
  - [ ] I can split `[0,n)` into T chunks that cover every tick exactly once, and prove `Σ(hi-lo)==n`.
- [ ] [`03-data-races.md`](./03-data-races.md) — races as UB
  - [ ] I can explain why `count++` loses updates, and why a hot-path mutex defeats the point.
- [ ] [`04-false-sharing.md`](./04-false-sharing.md) — the cache-line trap
  - [ ] I can explain why two threads writing different variables can still ping-pong a cache line.
- [ ] [`05-scheduler-and-pinning.md`](./05-scheduler-and-pinning.md) — jitter + pinning
  - [ ] I know why migration is expensive and what `sched_setaffinity(0, …)` does.
- [ ] [`06-bonus-bandwidth-and-pools.md`](./06-bonus-bandwidth-and-pools.md) — ⭐ **bonus** *(optional)*
  - [ ] I understand why a 4th thread can add almost nothing once I'm memory-bandwidth-bound.

### Phase 2 — Hands-on Experiments (Day 2, ~1.5 hours)

- [ ] **The racer:** write the §1 shared-counter race from [`03-data-races.md`](./03-data-races.md). Run it 10×; record the wrong, varying sums: \_\_\_\_\_ , \_\_\_\_\_ , \_\_\_\_\_
- [ ] **Catch it:** rebuild that racer under `-fsanitize=thread` and read the report. Then "fix" it with a mutex and time it — confirm it's *slower* than one thread: \_\_\_\_\_ ms (1 thread) vs \_\_\_\_\_ ms (mutex).
- [ ] **False sharing:** build the `Bad` vs. `Good` (`alignas(64)`) four-counter demo from [`04-false-sharing.md`](./04-false-sharing.md) §2:
  - packed (shared line): \_\_\_\_\_ ms
  - padded (`alignas(64)`): \_\_\_\_\_ ms
- [ ] **`perf` the two:** `perf stat -e cache-misses,context-switches ./bad` vs `./good`; note how cache-misses move.
- [ ] **Pinning:** run a parallel loop with and without `sched_setaffinity`; note the run-to-run variance, not just the mean.

### Phase 3 — Project (Days 3–6, ~6–8 hours)

Full brief in [`project/README.md`](./project/README.md). High-level milestones:

- [ ] **Step 1 — Skeleton:** ★ files copied; stub builds, runs, and diffs clean on `tiny.ticks` (it's correct single-threaded).
- [ ] **Step 2 — Big stream:** generate a 5M-tick stream and confirm the stub is still correct (slow).
- [ ] **Step 3 — Partition + threads:** split `[0,n)` across T threads, each reducing into its **own** partial; merge; still correct.
- [ ] **Step 4 — Kill false sharing:** make per-thread partials `alignas(64)` / thread-major; re-measure scaling.
- [ ] **Step 5 — Zero allocation:** all partials/threads sized in `on_init`; `run()` does not `malloc`.
- [ ] **Step 6 — Pin the threads:** `sched_setaffinity` per worker; variance drops. Headline numbers captured:
  - `run()` wall-clock on a 5M-tick stream: \_\_\_\_\_ ms
  - Throughput: \_\_\_\_\_ M ticks/s · speedup vs. 1 thread: \_\_\_\_\_×
- [ ] **Step 7 — Prove determinism:** run `run()` 10× in a loop; the table checksum is identical every time; TSan is clean.
- [ ] **Step 8 — Project README:** build steps, hardware (`nproc`/`lscpu`), headline throughput + scaling curve, a `perf stat` snippet, and **one thing that surprised you**.

### Phase 4 — Submission

- [ ] Code pushed to a public repo with a clear README and a `.gitignore` (no committed `*.ticks` except the golden `tiny.ticks`).
- [ ] Repo link shared in the CSoT group / submission form.
- [ ] Signed in to **[csot-low-latency.devclub.in](https://csot-low-latency.devclub.in/dashboard/)** with your DevClub IITD account.
- [ ] Uploaded **`aggregator.cpp`**. Submission shows `correct` (and passes the determinism check). Your judge numbers:
  - `run()` wall-clock: \_\_\_\_\_ ms
  - Throughput: \_\_\_\_\_ M ticks/s
- [ ] Found your row on the [live leaderboard](https://csot-low-latency.devclub.in/leaderboard/) (aggregator challenge).

### Phase 5 — Self-Check (the "would you survive an interview" round)

If you can answer all of these without Googling, you've internalized Week 3:

- [ ] **Q1.** State Amdahl's law. With a 5% serial fraction, what's the max speedup at 4 cores? At infinity?
- [ ] **Q2.** Why is the aggregator embarrassingly parallel? Which property of the aggregates makes the answer partition-independent?
- [ ] **Q3.** Why must a `std::thread` be joined or detached before destruction? What does `join()` guarantee about memory visibility?
- [ ] **Q4.** Split `[0, n)` into 4 chunks when `n = 10` using `n*k/T … n*(k+1)/T`. List the four ranges.
- [ ] **Q5.** Why does `count++` from two threads lose updates? What is a data race in C++ formally, and why is it worse than "a wrong number"?
- [ ] **Q6.** Why is a mutex around the hot increment slower than one thread? When is an atomic the right tool?
- [ ] **Q7.** Explain false sharing. Two threads write different variables and still ping-pong a cache line — how?
- [ ] **Q8.** What's the difference between `partials[thread][symbol]` and `partials[symbol][thread]` for false sharing?
- [ ] **Q9.** What does thread migration cost, and what does `sched_setaffinity(0, sizeof(set), &set)` do?
- [ ] **Q10.** Why might a 4th thread add almost no throughput even with zero false sharing and perfect pinning?

(Answers are scattered throughout the topic files and `AGG_SPEC.md` — go find them.)

### ⭐ Stretch Goals (Optional)

- [ ] Built a **thread pool** in `on_init` (spawn once, wake per `run()`); measured the startup saving.
- [ ] Swept thread count 1→8 and plotted throughput + speedup with [`tools/plot_scaling.py`](./project/tools/plot_scaling.py); identified where the bandwidth roof hits.
- [ ] Prefetched `ticks[i+K]` ahead of the reduce; swept `K`, plotted throughput.
- [ ] Estimated your box's memory bandwidth (STREAM or `perf`) and computed the theoretical max ticks/s; compared to your best.
- [ ] Tried a **parallel tree merge** instead of a serial merge; measured whether the serial tail even mattered (Amdahl in practice).
- [ ] Pinned to physical cores vs. hyperthread siblings; quantified the difference.
- [ ] Built with `-DCSOT_JUDGE_BUILD=ON` vs. `-march=native`; quantified the ISA's worth on the reduce.
- [ ] Deliberately introduced a race, confirmed the judge's determinism check (10× checksum) would catch it.

### 🧠 The Single Most Important Box

If you check **only one** box from this entire list, make it this one:

- [ ] **I took the same correct reduction, gave each thread its own cache-line-aligned partial, pinned the threads — and have the `perf` numbers and the scaling curve proving four cores beat one without a single data race.**

Speed coming from *layout and placement* under parallelism — not from cleverness, and not for free — is the Week-3 thesis. It's what Week 4's lock-free pipeline is built on.

---

## 🎬 Must-Watch Talks (general — applies to the whole week)

In addition to the per-file "Further Reading", these define the week:

1. **[Carl Cook — "When a Microsecond Is an Eternity"](https://www.youtube.com/watch?v=NH1Tta7purM)** (CppCon 2017, 1 h) — core pinning, doing nothing on the hot path, from the HFT trenches.
2. **[Fedor Pikus — "The Speed of Concurrency (Is Lock-free Faster?)"](https://www.youtube.com/watch?v=9hJkWwHDDxs)** (CppCon 2016, 1 h) — why naive sharing destroys scaling; the data behind false sharing and contention.
3. **[Hans Boehm — "Using weakly ordered C++ atomics correctly"](https://www.youtube.com/watch?v=M15UKpNlpeM)** (CppCon 2016) — by the author of the C++ memory model; what a data race actually is.
4. **[Arthur O'Dwyer — "Back to Basics: Concurrency"](https://www.youtube.com/watch?v=F6Ipn7gCOsY)** (CppCon 2020) — threads, joins, and the std primitives from the ground up.

---

## 📚 Books (reference, optional)

- **"C++ Concurrency in Action" (2nd ed., Anthony Williams)** — *the* book for this week. Ch. 2–3 (threads), Ch. 5 (the memory model & races), Ch. 8 (designing concurrent code / partitioning). The single best reading for Week 3.
- **"Computer Systems: A Programmer's Perspective"** (Bryant & O'Hallaron) — Ch. 12 (concurrent programming) and the cache-coherency material in Ch. 6.
- **"The Art of Multiprocessor Programming"** (Herlihy & Shavit) — the theory of shared-memory concurrency; ahead of where we are but the canonical reference.
- **"What Every Programmer Should Know About Memory"** ([Drepper, free PDF](https://people.freebsd.org/~lstewart/articles/cpumemory.pdf)) — §3 (caches/coherency) and §5 (NUMA) underpin false sharing and placement.

---

## 🎓 Free University Courses

- **[CMU 15-418 — Parallel Computer Architecture and Programming](https://www.cs.cmu.edu/~418/)** — the perfect companion this week: work partitioning, cache coherency, and the cost of communication.
- **[MIT 6.172 — Performance Engineering of Software Systems](https://ocw.mit.edu/courses/6-172-performance-engineering-of-software-systems-fall-2018/)** — the multicore-programming and cache-coherence lectures map directly onto Week 3.
- **[IIT Delhi COL380 — Introduction to Parallel & Distributed Programming](https://www.cse.iitd.ac.in/)** — the in-house parallel-programming course; its shared-memory material is this week's theory.

---

## 🤷 If You Only Read 5 Things This Week

1. [`03-data-races.md`](./03-data-races.md) + [`04-false-sharing.md`](./04-false-sharing.md) — the two bugs that define the week
2. [Mechanical Sympathy — False Sharing (Martin Thompson)](https://mechanical-sympathy.blogspot.com/2011/07/false-sharing.html)
3. [`project/AGG_SPEC.md`](./project/AGG_SPEC.md) §7 — the determinism guarantee (why parallel is fair)
4. [Carl Cook — When a Microsecond Is an Eternity (video)](https://www.youtube.com/watch?v=NH1Tta7purM)
5. "C++ Concurrency in Action" ch. 8 — designing/partitioning concurrent code

---

## 💬 Communities

- **[r/cpp](https://reddit.com/r/cpp)** — current C++ concurrency discussions.
- **[#include <C++> Discord](https://www.includecpp.org/)** — great for threading/atomics questions.
- **[r/algotrading](https://reddit.com/r/algotrading)** — the quant side.
- **[Hacker News](https://news.ycombinator.com/)** — search "false sharing", "NUMA", "thread affinity".

---

## 🚀 Ready?

Start with [`01-going-wide.md`](./01-going-wide.md). Then go make a reduction scream across four cores — without letting them fight. See you in Week 4. ⚡
