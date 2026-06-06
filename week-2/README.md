# Week 2 — Caches, Locality & Zero-Allocation

> *"The fastest allocation is the one you never make."*
>
> *"The fastest cache miss is the one your data layout never causes."*

Welcome to Week 2 of the Low Latency track. Week 1 built the **mental model and the measurement harness**; this week you put them to work on a new ranked challenge — a **cache simulator** — and learn the four disciplines that make a hot loop fast: hot/cold separation, locality, zero allocation, and compile-time computation.

By the end of this week you will have:

- ✅ Simulated a two-level set-associative cache hierarchy by hand and in code
- ✅ A `run()` hot path with **zero heap allocation** and a flat, cache-friendly data layout
- ✅ Folded cache geometry into **compile-time constants** and seen the assembly shrink
- ✅ A single `cache_sim.cpp` on the **live leaderboard**, ranked by wall-clock speed
- ✅ The bonus toolkit (branchless / SIMD tag scan, prefetch) that keeps the board from saturating

---

## 📖 Reading Order

Work through these in order. Each builds on the last and ends with a curated **"Further Reading"** section.

| # | File | Topic | Est. time |
|---|------|-------|-----------|
| 1 | [`01-hot-and-cold-paths.md`](./01-hot-and-cold-paths.md) | Hot vs. cold path, `on_init` vs. the timed `run()`, hoisting, masks vs. modulo | 20 min |
| 2 | [`02-cache-internals.md`](./02-cache-internals.md) | Lines, index/offset/tag, sets, ways, LRU, write-back, write-allocate, inclusion | 30 min |
| 3 | [`03-locality.md`](./03-locality.md) | Spatial/temporal locality, AoS vs. SoA, the 10× `unordered_map` trap | 25 min |
| 4 | [`04-zero-allocation.md`](./04-zero-allocation.md) | Why hot-path `malloc` spikes p99; arenas, pools, fixed buffers, sizing in `on_init` | 25 min |
| 5 | [`05-compile-time-and-static-polymorphism.md`](./05-compile-time-and-static-polymorphism.md) | `constexpr`/`consteval` masks, templating on geometry, virtual vs. CRTP | 30 min |
| ⭐ | [`06-bonus-simd-and-prefetch.md`](./06-bonus-simd-and-prefetch.md) | **Bonus:** branchless / SIMD tag scan, prefetch, packed LRU, reading the asm | 30 min |

Total reading: **~2–2.5 hours** (the ⭐ bonus is optional but highly recommended). Then the real work → the [**project**](./project/README.md).

---

## 🛠️ Setup

You already have the Week-1 toolchain. Week 2 adds nothing mandatory — same compiler, CMake, `perf`, Python 3. Confirm:

```bash
g++ --version          # ≥ 11 (for C++20 constexpr/consteval, [[likely]])
cmake --version        # ≥ 3.20
perf --version         # Linux only
python3 --version      # ≥ 3.8 (stdlib only; no extra deps for gen_trace.py)
```

Optional, only for the SIMD bonus: confirm what your CPU and the judge baseline support.

```bash
g++ -march=x86-64-v2 -Q --help=target | grep -E 'sse4|avx' | head   # the judge's baseline
g++ -march=native    -Q --help=target | grep -E 'avx2|avx512'       # what YOUR laptop has
```

> **WSL2 / macOS users:** same caveats as Week 1 — `perf` is limited under WSL2 and absent on macOS. Code is correct either way; numbers are noisy. The cache sim is CPU-only and single-threaded, so you can do almost all of Week 2 anywhere; you'll want native Linux for trustworthy `perf` from Week 3.

---

## 🎯 Project Brief

[**→ Open `project/README.md`**](./project/README.md)

In one sentence: **implement a single `cache_sim.cpp` that simulates a fixed two-level cache over a huge memory trace, emits the exact reference counters, and runs as fast as wall-clock allows.**

This week's project scope:

1. Read and internalize the frozen [`project/CACHE_SPEC.md`](./project/CACHE_SPEC.md) — geometry, LRU, write-back, write-allocate, the seven counters.
2. Implement a **correct** simulator against the [`project/include/cache_sim.hpp`](./project/include/cache_sim.hpp) ABI; verify against [`project/data/tiny.stats.json`](./project/data/tiny.stats.json).
3. Make it **flat** (struct-of-arrays), **zero-allocation** (`run()` never touches the heap), and **compile-time** (constexpr geometry).
4. Measure throughput + `run()` wall-clock with the shipped harness and `perf stat`.
5. Upload your **single `cache_sim.cpp`** to **[csot-low-latency.devclub.in](https://csot-low-latency.devclub.in/dashboard/)** — the judge builds it itself (fixed flags) and ranks correct sims by speed.

> **New this week:** you submit **source** (`cache_sim.cpp`), not a compiled `.so`. The judge owns the compiler and flags, so the board reflects your code, not your laptop's CPU. Details in [`platform_week_2.md`](../../platform_week_2.md).

Threads, false sharing, and core pinning come in Week 3. **Build the boring correct single-threaded version first** — then make it fast.

---

## ✅ Week 2 Checklist

> Copy this into your tracker. **If you can tick every box in Phases 0–4, you're done with Week 2.** Phase 5 and the stretch goals are bonus glory.

Suggested pace: **~5–7 days, ~2 hours/day.**

### Phase 0 — Environment Setup (Day 0, ~15 min)

- [ ] Week-1 toolchain still works (`g++ ≥ 11`, `cmake ≥ 3.20`, `python3 ≥ 3.8`, `perf` on Linux).
- [ ] Copied the ★ project files into a fresh project (or a `week-2/` folder in your repo).
- [ ] Header compiles: `g++ -std=c++20 -Iinclude -c include/cache_sim.hpp -o /tmp/x.o` is silent (both `static_assert`s pass).
- [ ] `cmake -B build && cmake --build build -j` builds `cache_sim_runner` from the stub.
- [ ] `.gitignore` ignores `build/` and `*.trace` (but keeps `data/tiny.trace` + `data/tiny.stats.json`).

### Phase 1 — Reading (Days 1–2, ~2–2.5 hours)

After each note, write **one sentence** about what surprised you.

- [ ] [`01-hot-and-cold-paths.md`](./01-hot-and-cold-paths.md) — hot vs. cold path
  - [ ] I can say which work belongs in `on_init` vs. `run()`, and why `& (SETS-1)` beats `% SETS`.
- [ ] [`02-cache-internals.md`](./02-cache-internals.md) — lines, sets, ways, LRU, write policies
  - [ ] I can split an address into offset / index / tag for both L1 and L2.
  - [ ] I can explain why an L1→L2 writeback is **not** counted as an L2 access.
- [ ] [`03-locality.md`](./03-locality.md) — locality, AoS vs. SoA
  - [ ] I know why a `std::unordered_map`-per-set simulator is ~10× too slow.
  - [ ] I skimmed §10 (cache-graph BFS) — pointer graph vs. CSR is the same layout trap in graph form.
- [ ] [`04-zero-allocation.md`](./04-zero-allocation.md) — zero-allocation hot path
  - [ ] I can name three things that secretly allocate inside a loop.
- [ ] [`05-compile-time-and-static-polymorphism.md`](./05-compile-time-and-static-polymorphism.md) — compile-time + static dispatch
  - [ ] I know why a virtual call inside the hot loop blocks inlining, and how to avoid it.
- [ ] [`06-bonus-simd-and-prefetch.md`](./06-bonus-simd-and-prefetch.md) — ⭐ **bonus** *(optional)*
  - [ ] I understand why a hand-rolled AVX2 path can be *slower* on the judge's `-march=x86-64-v2` baseline.

### Phase 2 — Hands-on Experiments (Day 2, ~1 hour)

- [ ] **Hand-trace `tiny.trace`:** `python3 data/gen_trace.py --dump data/tiny.trace`, walk all 14 accesses against `CACHE_SPEC.md` §5, and confirm you reach `data/tiny.stats.json` with pen and paper.
- [ ] **Mask vs. modulo:** on godbolt, compare `b % 64` and `b & 63` at `-O3` — confirm one becomes a division, the other an `and`.
- [ ] **The 10× layout demo:** build the *same* correct sim two ways (node-based vs. flat SoA) and record throughput on a 20M-access trace:
  - unordered_map / list: \_\_\_\_\_ M acc/s
  - flat SoA:             \_\_\_\_\_ M acc/s
- [ ] **Cache-graph BFS** ([`03-locality.md`](./03-locality.md) §10): same locality lesson on graphs — pointer adjacency list vs. CSR, sweep `er`/`grid`/`chain`/`star` graphs, plot time and L1 misses. Not judge work (~3 h for the full sweep). Fuller walkthrough: [cache_graph assignment](https://github.com/rijurekha/cache_graph).
- [ ] **`perf` the layouts:** `perf stat -e cycles,instructions,L1-dcache-load-misses` on both builds; note how IPC and miss rate move.
- [ ] **Allocation check:** `perf stat -e page-faults ./build/cache_sim_runner data/large.trace` — confirm faults are flat during `run()`.

### Phase 3 — Project (Days 3–6, ~6–8 hours)

Full brief in [`project/README.md`](./project/README.md). High-level milestones:

- [ ] **Step 1 — Skeleton:** ★ files copied; stub builds and runs on `tiny.trace`.
- [ ] **Step 2 — Read the spec by hand:** reproduced `tiny.stats.json` on paper.
- [ ] **Step 3 — Correct naive sim:** `cache_sim.cpp` matches `tiny.stats.json` (`diff` is clean) and a 5M-access trace runs without crashing.
- [ ] **Step 4 — Flat SoA:** node-based containers replaced with contiguous set-indexed arrays; counters unchanged.
- [ ] **Step 5 — Zero allocation:** all state allocated in `on_init`; `page-faults` flat during `run()`.
- [ ] **Step 6 — Compile-time geometry:** masks/way-counts are `constexpr`; asm shows immediates. Headline numbers captured:
  - `run()` wall-clock on `large.trace`: \_\_\_\_\_ ms
  - Throughput: \_\_\_\_\_ M acc/s
- [ ] **Step 7 — Measure & squeeze:** `perf stat` IPC / cache-miss% / branch-miss% recorded; (bonus) branchless or SIMD tag scan tried and measured.
- [ ] **Step 8 — Project README:** build steps, hardware, headline throughput, `perf stat` snippet, **one thing that surprised you**.

### Phase 4 — Submission

- [ ] Code pushed to a public repo with a clear README and a `.gitignore` (no committed `*.trace` except the golden tiny pair).
- [ ] Repo link shared in the CSoT group / submission form.
- [ ] Signed in to **[csot-low-latency.devclub.in](https://csot-low-latency.devclub.in/dashboard/)** with your DevClub IITD account.
- [ ] Uploaded **`cache_sim.cpp`**. Submission shows `correct` on the public trace. Your judge numbers:
  - `run()` wall-clock: \_\_\_\_\_ ms
  - Throughput: \_\_\_\_\_ M acc/s
- [ ] Found your row on the [live leaderboard](https://csot-low-latency.devclub.in/leaderboard/) (cache challenge).

### Phase 5 — Self-Check (the "would you survive an interview" round)

If you can answer all of these without Googling, you've internalized Week 2:

- [ ] **Q1.** Split a 64-bit address into offset / index / tag for a 32 KiB, 8-way, 64-B-line cache. How many index bits?
- [ ] **Q2.** Direct-mapped vs. set-associative — what failure does associativity fix?
- [ ] **Q3.** Write-back vs. write-through; write-allocate vs. write-no-allocate. Which does this project use, and what does the dirty bit track?
- [ ] **Q4.** Why does an L1→L2 writeback **not** increment `l2_hits`/`l2_misses` or touch L2's LRU?
- [ ] **Q5.** When exactly is `dirty_writebacks` incremented?
- [ ] **Q6.** Why is a `std::unordered_map`-per-set simulator ~10× slower than a flat array one?
- [ ] **Q7.** Why is `& (SETS-1)` faster than `% SETS`, and why is it always valid here?
- [ ] **Q8.** Name three things that secretly allocate inside a C++ loop.
- [ ] **Q9.** Why is a virtual call inside the hot loop an optimization barrier? Two ways to avoid it.
- [ ] **Q10.** Why might a hand-written AVX2 tag scan lose to a branchless scalar one on the judge?

(Answers are scattered throughout the topic files and `CACHE_SPEC.md` — go find them.)

### ⭐ Stretch Goals (Optional)

- [ ] Branchless 8-way tag scan; measured the throughput delta.
- [ ] SIMD tag compare (SSE/AVX2 with a run-time guard); measured vs. scalar on the judge baseline.
- [ ] Packed valid+dirty bitset per set; compared to byte-per-way.
- [ ] Packed-nibble LRU order in a `uint32` with a `consteval` transition table.
- [ ] Prefetch `acc[i+K]`'s set; swept `K`, plotted throughput.
- [ ] Built `-DCSOT_JUDGE_BUILD=ON` vs. `-march=native`; quantified the ISA's worth.
- [ ] Tried pseudo-LRU, confirmed it *breaks* correctness, and explained exactly why.
- [ ] Plotted L1/L2 hit rates across working-set sizes (`tools/plot_cache.py`).

### 🧠 The Single Most Important Box

If you check **only one** box from this entire list, make it this one:

- [ ] **I took the same correct simulator and made it measurably faster by changing only its data layout — and I have the `perf` numbers to prove the cache misses dropped.**

That single experience — speed coming from layout, not cleverness — is the Mike Acton thesis made real. It's what the rest of the track is built on.

---

## 🎬 Must-Watch Talks (general — applies to the whole week)

In addition to the per-file "Further Reading", these define the week:

1. **[Mike Acton — "Data-Oriented Design and C++"](https://www.youtube.com/watch?v=rX0ItVEVjHc)** (CppCon 2014, 1 h) — where data lives in memory *is* the design.
2. **[Scott Meyers — "CPU Caches and Why You Care"](https://www.youtube.com/watch?v=WDIkqP4JbkE)** (1 h) — the clearest explanation of the machine you're simulating.
3. **[Carl Cook — "When a Microsecond Is an Eternity"](https://www.youtube.com/watch?v=NH1Tta7purM)** (CppCon 2017, 1 h) — "do nothing on the hot path", from the HFT trenches.
4. **[Jason Turner — "constexpr ALL the things!"](https://www.youtube.com/watch?v=PJwd4JLYJJY)** (CppCon 2017) — how far compile-time computation reaches.

---

## 📚 Books (reference, optional)

- **"Computer Systems: A Programmer's Perspective"** (Bryant & O'Hallaron) — Ch. 6 (the memory hierarchy & cache organization) is *exactly* this project. The single best reading for Week 2.
- **"Computer Architecture: A Quantitative Approach"** (Hennessy & Patterson) — Appendix B for the formal cache model.
- **"What Every Programmer Should Know About Memory"** ([Drepper, free PDF](https://people.freebsd.org/~lstewart/articles/cpumemory.pdf)) — §3 on caches.
- **"Algorithms for Modern Hardware"** ([Algorithmica, free online](https://en.algorithmica.org/hpc/)) — SIMD, prefetch, cache-aware code; the spirit of the bonus file.

---

## 🎓 Free University Courses

- **[IIT Delhi COL216 — Computer Architecture](https://www.cse.iitd.ac.in/~rijurekha/col216_2022.html)** — your in-house course; its cache lab is the direct cousin of this week's project. If you've done it, this will feel familiar; if not, the set-associative-cache lectures are the right primer.
- **[MIT 6.172 — Performance Engineering of Software Systems](https://ocw.mit.edu/courses/6-172-performance-engineering-of-software-systems-fall-2018/)** — the caching and cache-efficient-algorithms lectures align tightly with Week 2.
- **[CMU 15-418 — Parallel Computer Architecture](https://www.cs.cmu.edu/~418/)** — relevant from Week 3, but the memory-system lectures are great background now.

---

## 🤷 If You Only Read 5 Things This Week

1. [Gallery of Processor Cache Effects](https://igoro.com/archive/gallery-of-processor-cache-effects/) — short, mind-bending, exactly on topic
2. [`02-cache-internals.md`](./02-cache-internals.md) + [`project/CACHE_SPEC.md`](./project/CACHE_SPEC.md) — the machine you must simulate
3. [Mike Acton — Data-Oriented Design (video)](https://www.youtube.com/watch?v=rX0ItVEVjHc)
4. CS:APP Chapter 6 (memory hierarchy & caches)
5. [Algorithmica — Algorithms for Modern Hardware](https://en.algorithmica.org/hpc/) (SIMD + cache chapters)

---

## 💬 Communities

- **[r/cpp](https://reddit.com/r/cpp)** — current C++ performance discussions.
- **[#include <C++> Discord](https://www.includecpp.org/)** — great for C++ questions.
- **[r/algotrading](https://reddit.com/r/algotrading)** — the quant side.
- **[Hacker News](https://news.ycombinator.com/)** — search "cache", "SIMD", "data-oriented".

---

## 🚀 Ready?

Start with [`01-hot-and-cold-paths.md`](./01-hot-and-cold-paths.md). Then go build a cache from first principles and make it scream. See you in Week 3. ⚡
