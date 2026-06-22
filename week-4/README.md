# Week 4 — Atomics, Memory Orderings & the Lock-Free Pipeline

> *"A lock around the handoff serializes the very thing you parallelized."*
>
> *"The fastest queue is the one where the producer and consumer never touch the same cache line — and never take a lock."*

Welcome to Week 4 of the Low Latency track. Week 3 made four cores reduce one big array. This week you build a **streaming pipeline**: a market-data thread decodes a tick feed and hands each tick to a strategy thread across a **lock-free single-producer/single-consumer ring buffer** — no mutex, no allocation, the right memory orderings, and the two stages overlapping so the handoff latency vanishes. The frozen **Week-1 z-score strategy returns** as the consumer; the new craft is the hand-off. Same "fastest correct implementation" game, a workload built to reward `std::atomic` done right.

By the end of this week you will have:

- ✅ Understood why a **stateful, ordered** workload is a *pipeline*, not a partition — and why overlap caps at `(D+S)/max(D,S)`
- ✅ Learned what `std::atomic` actually guarantees, why `volatile` is **not** it, and how a **release/acquire** handshake publishes data between threads
- ✅ Picked the right **memory ordering** for every line of a queue — `relaxed` for your own index, `acquire`/`release` for the handoff, never `seq_cst` by reflex
- ✅ Built a **lock-free SPSC ring buffer**: power-of-two capacity, `head`/`tail` on separate cache lines, cached indices, batching
- ✅ **Pinned** both threads, handled **back-pressure** with a spin, and **balanced** the stages with Week-2 rolling sums
- ✅ A single `pipeline.cpp` on the **live leaderboard**, ranked by wall-clock throughput over a huge feed — and **proven deterministic** across repeated runs

---

## 📖 Reading Order

Work through these in order. Each builds on the last and ends with a curated **"Further Reading"** section.

| # | File | Topic | Est. time |
|---|------|-------|-----------|
| 1 | [`01-from-batch-to-streaming.md`](./01-from-batch-to-streaming.md) | Pipeline vs. partition, overlap & the `(D+S)/max(D,S)` ceiling, why a mutex handoff loses, the SPSC special case | 20 min |
| 2 | [`02-atomics-and-the-memory-model.md`](./02-atomics-and-the-memory-model.md) | `std::atomic`, RMW, `volatile` is not atomic, the release/acquire handshake, `compare_exchange` | 25 min |
| 3 | [`03-memory-orderings.md`](./03-memory-orderings.md) | `relaxed`/`acquire`/`release`/`seq_cst`, the matched pair, exactly what SPSC needs, the cost of `seq_cst` | 25 min |
| 4 | [`04-spsc-ring-buffer.md`](./04-spsc-ring-buffer.md) | Build it: power-of-two masking, separate cache lines for head/tail, wait-free push/pop, cached indices, batching, sizing | 30 min |
| 5 | [`05-pipeline-pinning-and-backpressure.md`](./05-pipeline-pinning-and-backpressure.md) | Wire the two threads, pin to cores, spin vs. block, balance the stages, measure the overlap | 25 min |
| ⭐ | [`06-bonus-disruptor-and-beyond.md`](./06-bonus-disruptor-and-beyond.md) | **Bonus:** batching, the LMAX Disruptor, MPSC/MPMC, prefetch, the Week-5 network bridge | 25 min |

Total reading: **~2–2.5 hours** (the ⭐ bonus is optional but highly recommended). Then the real work → the [**project**](./project/README.md).

---

## 🛠️ Setup

You already have the Week-1/2/3 toolchain. Week 4 adds nothing mandatory — same compiler, CMake, `perf`, Python 3 — but you'll lean hard on `<atomic>` and the race detector. Confirm:

```bash
g++ --version          # ≥ 11 (for C++20 std::atomic, std::jthread)
cmake --version        # ≥ 3.20
perf --version         # Linux only
nproc                  # logical cores (the judge has 4); two are your hot threads
lscpu                  # check which cores are hyperthread siblings
```

Confirm your atomics are genuinely lock-free and your sanitizers link:

```bash
echo '#include <atomic>
#include <cstdio>
int main(){ std::atomic<unsigned long> x{0};
  std::printf("lock_free=%d\n", x.is_lock_free()); }' | g++ -std=c++20 -x c++ - -o /tmp/lf && /tmp/lf   # expect lock_free=1

echo 'int main(){}' | g++ -fsanitize=thread  -x c++ - -o /tmp/tsan_ok && echo "TSan OK"
echo 'int main(){}' | g++ -fsanitize=address -x c++ - -o /tmp/asan_ok && echo "ASan OK"
```

> **WSL2 / macOS users:** as in Week 3, native Linux matters. `sched_setaffinity` is Linux-only (no real thread-pinning on macOS), `perf`'s counters are Linux-only, and core topology under a VM is virtualized and noisy. The code is correct anywhere, but trustworthy pipeline numbers — and the pinning exercise — want bare-metal Linux. The judge runs Linux on 4 vCPUs; develop as close to that as you can.

---

## 🎯 Project Brief

[**→ Open `project/README.md`**](./project/README.md)

In one sentence: **implement a single `pipeline.cpp` that decodes a binary tick feed on one thread, hands each tick across your lock-free SPSC ring buffer to a thread running the frozen Week-1 z-score strategy, and emits the exact reference order stream — correctly, deterministically, and as fast as the overlap allows.**

This week's project scope:

1. Read and internalize the frozen [`project/PIPELINE_SPEC.md`](./project/PIPELINE_SPEC.md) — the 40-byte wire format, the decode rule, the unchanged strategy + fill model, and the order-stream + determinism gates.
2. Implement a **correct** pipeline against the [`project/include/pipeline.hpp`](./project/include/pipeline.hpp) ABI; verify against [`project/data/tiny.orders.json`](./project/data/tiny.orders.json). (The shipped stub is already correct single-threaded — your job is to overlap it.)
3. Make it **two-stage** (decode producer + strategy consumer) connected by a **lock-free SPSC ring buffer** (`alignas(64)` head/tail, release/acquire), **pinned**, **back-pressured** by a spin, **zero-allocation** in `run()`, and **balanced** with Week-2 rolling sums.
4. Measure throughput + `run()` wall-clock against the single-threaded baseline (and a mutex queue, for contrast) with the shipped harness, `perf stat`, and `tools/plot_latency.py`.
5. Upload your **single `pipeline.cpp`** to **[csot-low-latency.devclub.in](https://csot-low-latency.devclub.in/dashboard/)** — the judge builds it itself (fixed flags, `-pthread`), runs it several times, checks the order stream and that repeated runs agree, and ranks correct, deterministic pipelines by speed.

> **Same as Weeks 2–3:** you submit **source** (`pipeline.cpp`), not a compiled `.so`. The judge owns the compiler and flags, so the board reflects your code, not your laptop. **As in Week 3,** the judge runs your submission K times and **rejects non-deterministic results** — a handoff race that "passes sometimes" does not rank. Details in [`platform_week_4.md`](../../platform_week_4.md).

Network ingest and the `epoll` event loop come in Week 5. **Build the boring correct single-threaded version first** (it's the shipped stub) — then split it into a pipeline.

---

## ✅ Week 4 Checklist

> Copy this into your tracker. **If you can tick every box in Phases 0–4, you're done with Week 4.** Phase 5 and the stretch goals are bonus glory.

Suggested pace: **~5–7 days, ~2 hours/day.**

### Phase 0 — Environment Setup (Day 0, ~15 min)

- [ ] Week-1/2/3 toolchain still works (`g++ ≥ 11`, `cmake ≥ 3.20`, `python3 ≥ 3.8`, `perf` on Linux).
- [ ] `std::atomic<uint64_t>::is_lock_free()` prints `1` on your box (command above).
- [ ] TSan and ASan both link a trivial program.
- [ ] Copied the ★ project files into a fresh project (or a `week-4/` folder in your repo).
- [ ] Headers compile: `g++ -std=c++20 -Iinclude -c include/pipeline.hpp -o /tmp/x.o` is silent (all `static_assert`s pass).
- [ ] `cmake -B build && cmake --build build -j` builds `pipeline_runner` from the stub; `diff <(./build/pipeline_runner data/tiny.feed 2>/dev/null) data/tiny.orders.json` is clean.

### Phase 1 — Reading (Days 1–2, ~2–2.5 hours)

After each note, write **one sentence** about what surprised you.

- [ ] [`01-from-batch-to-streaming.md`](./01-from-batch-to-streaming.md) — pipeline vs. partition
  - [ ] I can explain why a mutex handoff is often slower than single-threaded, and compute the overlap ceiling for `D = S/3`.
- [ ] [`02-atomics-and-the-memory-model.md`](./02-atomics-and-the-memory-model.md) — atomics
  - [ ] I can explain why `volatile bool ready` is wrong for thread signaling and what a release/acquire handshake guarantees.
- [ ] [`03-memory-orderings.md`](./03-memory-orderings.md) — orderings
  - [ ] I can name the ordering for each of: reading my own index, reading the other's index, publishing my index.
- [ ] [`04-spsc-ring-buffer.md`](./04-spsc-ring-buffer.md) — the ring
  - [ ] I can state the empty and full conditions with monotonic counters, and why head/tail need separate cache lines.
- [ ] [`05-pipeline-pinning-and-backpressure.md`](./05-pipeline-pinning-and-backpressure.md) — wiring
  - [ ] I know why I spin (not sleep) under back-pressure and why a lopsided pipeline wastes the second core.
- [ ] [`06-bonus-disruptor-and-beyond.md`](./06-bonus-disruptor-and-beyond.md) — ⭐ **bonus** *(optional)*
  - [ ] I understand how batching the publish trades latency for throughput.

### Phase 2 — Hands-on Experiments (Day 2, ~1.5 hours)

- [ ] **The handshake:** write the `payload`/`ready` release-acquire demo from [`02`](./02-atomics-and-the-memory-model.md) §4. Then break it (use `relaxed` on the store) and, on a weakly-ordered box or under `-O3`, try to catch a stale read.
- [ ] **Tiny SPSC:** write the [`04`](./04-spsc-ring-buffer.md) ring for `int`, push 0..1e7 on one thread, sum on another; confirm the sum is exactly `n(n-1)/2` every run.
- [ ] **False-share the cursors:** put `head` and `tail` in one struct *without* `alignas(64)`, then with it; time the same push/pop loop. Record: packed \_\_\_\_\_ ms vs padded \_\_\_\_\_ ms.
- [ ] **`perf` the two:** `perf stat -e cache-misses,context-switches ./packed` vs `./padded`; note where cache-misses move.
- [ ] **Mutex vs lock-free:** swap the ring for `std::mutex`+`std::queue`; confirm it's *slower*. \_\_\_\_\_ M/s (mutex) vs \_\_\_\_\_ M/s (SPSC).

### Phase 3 — Project (Days 3–6, ~6–8 hours)

Full brief in [`project/README.md`](./project/README.md). High-level milestones:

- [ ] **Step 1 — Skeleton:** ★ files copied; stub builds, runs, and diffs clean on `tiny.feed` (it's correct single-threaded).
- [ ] **Step 2 — Big feed:** generate a 5M-tick feed and confirm the stub is still correct (record its throughput as the baseline to beat).
- [ ] **Step 3 — Ring buffer:** build your lock-free SPSC ring; unit-test it standalone (Phase 2) before wiring it in.
- [ ] **Step 4 — Split the pipeline:** decode on a producer thread, strategize on the consumer thread, handoff over the ring; re-`diff` against `tiny.orders.json` — still clean.
- [ ] **Step 5 — Kill false sharing + cache indices:** `alignas(64)` head/tail, cache the other side's index; re-measure.
- [ ] **Step 6 — Pin + balance:** `sched_setaffinity` both threads; bring back Week-2 rolling sums so `D ≈ S`. Headline numbers:
  - `run()` wall-clock on a 5M-tick feed: \_\_\_\_\_ ms · throughput \_\_\_\_\_ M ticks/s · speedup vs. serial \_\_\_\_\_×
- [ ] **Step 7 — Prove determinism:** run `run()` 10× in a loop; the order-stream checksum is identical every time; TSan is clean.
- [ ] **Step 8 — Project README:** build steps, hardware (`nproc`/`lscpu`), headline throughput + speedup, a `perf stat` snippet, and **one thing that surprised you**.

### Phase 4 — Submission

- [ ] Code pushed to a public repo with a clear README and a `.gitignore` (no committed `*.feed` except the golden `tiny.feed`).
- [ ] Repo link shared in the CSoT group / submission form.
- [ ] Signed in to **[csot-low-latency.devclub.in](https://csot-low-latency.devclub.in/dashboard/)** with your DevClub IITD account.
- [ ] Uploaded **`pipeline.cpp`**. Submission shows `correct` (and passes the determinism check). Your judge numbers:
  - `run()` wall-clock: \_\_\_\_\_ ms · throughput: \_\_\_\_\_ M ticks/s
- [ ] Found your row on the [live leaderboard](https://csot-low-latency.devclub.in/leaderboard/) (pipeline challenge).

### Phase 5 — Self-Check (the "would you survive an interview" round)

If you can answer all of these without Googling, you've internalized Week 4:

- [ ] **Q1.** Why can't you shard the strategy across threads the way you sharded the Week-3 aggregator?
- [ ] **Q2.** A producer and consumer share a `std::mutex`-protected queue. Give two reasons it's often slower than single-threaded.
- [ ] **Q3.** What does `std::atomic` guarantee that `volatile` does not? Name three things.
- [ ] **Q4.** Explain the release/acquire handshake. If the consumer sees the released flag, what else is it guaranteed to see?
- [ ] **Q5.** For an SPSC ring, what memory ordering goes on: reading your own index, reading the other's index, publishing your index? Why each?
- [ ] **Q6.** Why must capacity be a power of two? Write the empty and full conditions with monotonic counters.
- [ ] **Q7.** Why do `head` and `tail` need separate cache lines? What does the bug look like in `perf`?
- [ ] **Q8.** What is index caching and why is it the biggest win after correctness?
- [ ] **Q9.** Under back-pressure (full queue), why spin instead of sleep? Why must you never drop a tick?
- [ ] **Q10.** Your pipeline is correct but only 1.1× faster than serial. What's the most likely cause and the fix?

(Answers are scattered throughout the topic files and `PIPELINE_SPEC.md` — go find them.)

### ⭐ Stretch Goals (Optional)

- [ ] **Batched** the publish (one `release` per K items) and the drain; measured the throughput gain and the latency cost.
- [ ] Swept ring **capacity** (16 → 1M) and plotted throughput; found where it falls out of cache.
- [ ] Pinned to **physical cores vs. hyperthread siblings**; quantified the difference.
- [ ] Built a **mutex queue** and a **lock-free SPSC** and plotted both vs. the serial baseline with [`tools/plot_latency.py`](./project/tools/plot_latency.py).
- [ ] **Prefetched** the input feed a few records ahead of the decode; swept the distance.
- [ ] Read the **LMAX Disruptor** paper and mapped each of its ideas onto your ring.
- [ ] Deliberately introduced a publish-ordering bug (`relaxed` store), confirmed the judge's 10× determinism check would catch it, then fixed it.
- [ ] Built with `-DCSOT_JUDGE_BUILD=ON` vs. `-march=native`; quantified the ISA's worth on the decode + strategy.

### 🧠 The Single Most Important Box

If you check **only one** box from this entire list, make it this one:

- [ ] **I took the same correct decode-then-strategize, split it across two pinned threads connected by a lock-free SPSC ring buffer with release/acquire publication and cache-line-separated head/tail — and have the `perf` numbers and the determinism check proving two cores beat one without a lock and without a data race.**

Speed coming from a *correct, lock-free hand-off* under parallelism — not from a mutex, and not by luck — is the Week-4 thesis. It's the skeleton Week 5's networked feed hangs on.

---

## 🎬 Must-Watch Talks (general — applies to the whole week)

In addition to the per-file "Further Reading", these define the week:

1. **[Carl Cook — "When a Microsecond Is an Eternity"](https://www.youtube.com/watch?v=NH1Tta7purM)** (CppCon 2017, 1 h) — lock-free handoffs, busy-waiting, never sleeping on the hot path, from HFT.
2. **[Fedor Pikus — "C++ atomics, from basic to advanced"](https://www.youtube.com/watch?v=ZQFzMfHIxng)** (CppCon 2017, 1 h) — what atomics really compile to; the foundation for the queue.
3. **[Hans Boehm — "Using weakly ordered C++ atomics correctly"](https://www.youtube.com/watch?v=M15UKpNlpeM)** (CppCon 2016) — by an author of the memory model; exactly what acquire/release buy you.
4. **[Timur Doumler — "C++ atomics: from basic to advanced ... again"](https://www.youtube.com/watch?v=ZQFzMfHIxng)** / **[Mike Acton — "Data-Oriented Design"](https://www.youtube.com/watch?v=rX0ItVEVjHc)** — the layout mindset behind cache-line-aware cursors.

---

## 📚 Books (reference, optional)

- **"C++ Concurrency in Action" (2nd ed., Anthony Williams)** — *the* book for this week. Ch. 5 (the memory model & atomics), Ch. 7 (designing lock-free data structures — including an SPSC queue), Ch. 8 (designing concurrent code). The single best reading for Week 4.
- **"The Art of Multiprocessor Programming"** (Herlihy & Shavit) — the theory of lock-free / wait-free structures; ahead of where we are but canonical.
- **"What Every Programmer Should Know About Memory"** ([Drepper, free PDF](https://people.freebsd.org/~lstewart/articles/cpumemory.pdf)) — §6 (cache coherency / what makes false sharing expensive) underpins the cursor padding.
- **"Computer Systems: A Programmer's Perspective"** (Bryant & O'Hallaron) — Ch. 12 (concurrent programming) for the threading foundations.

---

## 🎓 Free University Courses

- **[MIT 6.172 — Performance Engineering of Software Systems](https://ocw.mit.edu/courses/6-172-performance-engineering-of-software-systems-fall-2018/)** — the synchronization and cache-coherence lectures map directly onto this week.
- **[CMU 15-418 — Parallel Computer Architecture and Programming](https://www.cs.cmu.edu/~418/)** — the cost-of-communication and memory-consistency material is exactly Week 4's theory.
- **[IIT Delhi COL380 — Introduction to Parallel & Distributed Programming](https://www.cse.iitd.ac.in/)** — the in-house course; its shared-memory and synchronization material is this week's backbone.

---

## 🤷 If You Only Read 5 Things This Week

1. [`03-memory-orderings.md`](./03-memory-orderings.md) + [`04-spsc-ring-buffer.md`](./04-spsc-ring-buffer.md) — the orderings and the data structure that define the week
2. [Jeff Preshing — "Acquire and Release Semantics"](https://preshing.com/20120913/acquire-and-release-semantics/)
3. [Rigtorp — "A single producer single consumer wait-free ring buffer"](https://rigtorp.se/ringbuffer/)
4. [`project/PIPELINE_SPEC.md`](./project/PIPELINE_SPEC.md) §7 — the determinism guarantee (why an SPSC handoff is fair)
5. "C++ Concurrency in Action" ch. 5 & 7 — the memory model and a lock-free SPSC queue

---

## 💬 Communities

- **[r/cpp](https://reddit.com/r/cpp)** — current C++ concurrency / lock-free discussions.
- **[#include <C++> Discord](https://www.includecpp.org/)** — great for atomics / memory-ordering questions.
- **[r/algotrading](https://reddit.com/r/algotrading)** — the quant side.
- **[Hacker News](https://news.ycombinator.com/)** — search "SPSC queue", "memory ordering", "LMAX Disruptor".

---

## 🚀 Ready?

Start with [`01-from-batch-to-streaming.md`](./01-from-batch-to-streaming.md). Then go make two cores hand a tick stream back and forth at memory speed — without a lock, without a race. See you in Week 5, where the feed comes off the wire. ⚡
