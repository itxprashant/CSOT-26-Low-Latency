# Week 1 — Introduction & Memory

> *"You can't improve what you can't measure."* — Peter Drucker
>
> *"You can't measure what you don't understand."* — every systems engineer ever

Welcome to Week 1 of the Low Latency track. This week is about **building the mental model**: understanding what "low latency" actually means, how memory is physically organized, and how to measure the performance of a piece of code without lying to yourself.

By the end of this week you will have:

- ✅ A working C++/CMake/perf/Google-Benchmark toolchain on Linux
- ✅ An intuitive grasp of the memory hierarchy (and the brutal latency cliff between L1 and DRAM)
- ✅ Your first reproducible microbenchmark
- ✅ The skeleton of the **low-latency quant platform** project that you'll grow over the next 5 weeks

---

## 📖 Reading Order

Work through these in order. Each note is short (~10-15 min). Don't skip — they build on each other. Each topic file ends with a **"Further Reading"** section curated for that topic.

| # | File | Topic | Est. time |
|---|------|-------|-----------|
| 1 | [`01-introduction.md`](./01-introduction.md) | Latency vs. throughput, memory as a contiguous array | 15 min |
| 2 | [`02-memory-model.md`](./02-memory-model.md) | `std::vector` / `std::array`, stack vs. heap, custom allocators | 25 min |
| 3 | [`03-memory-hierarchy.md`](./03-memory-hierarchy.md) | L1/L2/L3/DRAM, latency numbers every programmer should know | 20 min |
| 4 | [`04-benchmarking-tools.md`](./04-benchmarking-tools.md) | `chrono`, `rdtsc`, Google Benchmark, `perf`, Valgrind, Cachegrind | 35 min |
| ⭐ | [`05-bonus-compiler.md`](./05-bonus-compiler.md) | **Bonus:** compiler flags, sanitizers, LTO/PGO, hints, reading assembly | 30 min |

Total reading: **~1.5–2 hours** (the ⭐ bonus is optional but highly recommended). Then the real work starts → the [**project**](./project/README.md).

---

## 🛠️ Setup

Before you start, make sure you have:

```bash
g++ --version          # ≥ 11
cmake --version        # ≥ 3.20
perf --version         # any (Linux only)
python3 --version      # ≥ 3.8
git --version          # any
```

Install missing pieces:

```bash
sudo apt update
sudo apt install -y build-essential cmake git python3 python3-pip \
                    linux-tools-common linux-tools-generic linux-tools-$(uname -r) \
                    libbenchmark-dev valgrind kcachegrind
```

> **WSL2 users:** `perf` and CPU-pinning behave oddly under WSL. You'll get *correct* code but *noisy* numbers. That's acceptable for Week 1, but consider dual-booting before Week 3.

> **macOS users:** Replace `perf` with **Instruments** (comes with Xcode). Install Google Benchmark via `brew install google-benchmark`. You'll need a Linux VM by Week 5.

---

## 🎯 Project Brief

[**→ Open `project/README.md`**](./project/README.md)

In one sentence: **build a platform that loads your C++ implementation of a fixed strategy spec, replays simulated market ticks against it, verifies correctness, measures latency, and submits to the live latency leaderboard.**

This week's project scope is intentionally small:

1. Use the **strategy API** and frozen [`project/STRATEGY_SPEC.md`](./project/STRATEGY_SPEC.md) contract.
2. Write a **replay engine** that reads a CSV of historical ticks and calls the strategy in a tight loop.
3. Time the strategy with `std::chrono::steady_clock` and `rdtsc`.
4. Print a per-strategy summary: ticks processed, correctness status, mean / p50 / p99 / p999 latency, throughput.
5. Run the standard benchmarking tools (`perf stat`, Google Benchmark) on your platform.
6. Upload your `spec_strategy.so` to **[csot-low-latency.devclub.in](https://csot-low-latency.devclub.in/dashboard/)** and confirm it scores. The judge replays your code against the same `gen.py` data on an EC2 box (reference: p50 ≈ **152 ns**, p99 ≈ **176 ns**, **5.38 M ticks/s**) inside a `bwrap` sandbox.

Networking, multi-threading, and lock-free queues come in later weeks. **Build the boring synchronous version first.** It will be your baseline forever.

---

## ✅ Week 1 Checklist

> Copy this into your Notion / GitHub project / a sticky note. **If you can tick every box in Phases 0–4, you're done with Week 1.** Phase 5 and the stretch goals are bonus glory.

Suggested pace: **~5–7 days, ~2 hours/day.** You can compress the reading into one weekend and spread the project across the next.

### Phase 0 — Environment Setup (Day 0, ~30 min)

- [ ] OS: native Linux, dual-boot, or WSL2 (note: WSL2 has limited `perf`).
- [ ] `g++ --version` ≥ 11 *(or `clang++` ≥ 14)*
- [ ] `cmake --version` ≥ 3.20
- [ ] `python3 --version` ≥ 3.8
- [ ] `git --version` works
- [ ] `perf --version` works (Linux only)
- [ ] `valgrind --version` works
- [ ] Google Benchmark installed (`apt install libbenchmark-dev` or built from source)
- [ ] `kcachegrind` installed (optional but nice)
- [ ] Allowed `perf` for non-root use:
  ```bash
  echo 'kernel.perf_event_paranoid = 1' | sudo tee /etc/sysctl.d/99-perf.conf
  sudo sysctl --system
  ```
- [ ] Created a fresh Git repo for the project (`csot-quant-platform` or similar) and pushed to GitHub/GitLab.
- [ ] Added a `.gitignore` for `build/`, `*.csv` (large data files), `*.so`, `*.o`, `cachegrind.out.*`, `callgrind.out.*`, `perf.data*`.

### Phase 1 — Reading (Days 1–2, ~1.5–2 hours)

After each note, write **one sentence** in your own words about what surprised you. Trust me — it cements the idea.

- [ ] [`01-introduction.md`](./01-introduction.md) — latency vs. throughput, memory as a contiguous array
  - [ ] I can state the difference between p50, p99, and p999 latency.
- [ ] [`02-memory-model.md`](./02-memory-model.md) — containers, stack vs. heap, allocators
  - [ ] I know why `std::list` is slower than `std::vector` to iterate.
  - [ ] I can describe what an arena allocator does and why it's fast.
- [ ] [`03-memory-hierarchy.md`](./03-memory-hierarchy.md) — L1/L2/L3/DRAM, cache lines
  - [ ] I have memorized ~3 latency numbers (L1 hit, DRAM access, network RTT).
  - [ ] I know what a cache line is and how big it is.
- [ ] [`04-benchmarking-tools.md`](./04-benchmarking-tools.md) — chrono, rdtsc, Google Benchmark, perf, Valgrind, Cachegrind
  - [ ] I understand why `DoNotOptimize` / `ClobberMemory` exist.
  - [ ] I can name one situation where Cachegrind beats `perf` and vice-versa.
- [ ] [`05-bonus-compiler.md`](./05-bonus-compiler.md) — ⭐ **bonus** *(optional)*
  - [ ] I know what `-O2`, `-march=native`, and `-flto` do.
  - [ ] I have opened Godbolt at least once.

### Phase 2 — Hands-on Experiments (Day 2, ~1 hour)

Don't just read — *touch* the ideas. Each of these is < 50 lines of code.

- [ ] **`vector` vs `list` sum demo** from `01-introduction.md` — compile at `-O2`, run, observe the ~20–100x gap. Note your numbers:
  - vector sum: \_\_\_\_\_ ms
  - list   sum: \_\_\_\_\_ ms
- [ ] **Cache cliff plot** from `03-memory-hierarchy.md` — run for buffer sizes from 8 KB → 64 MB. Eyeball the three plateaus (L1 / L2 / L3) and the cliffs between them. Optional: plot with matplotlib.
- [ ] Ran **Google Benchmark** on at least one of your own functions and saw `Time` / `Iterations` / `items/s` output.
- [ ] Ran **`perf stat -d ./your_program`** and identified your **IPC**, **L1 miss rate**, and **branch miss rate**.
- [ ] Ran **`valgrind --leak-check=full`** on a deliberately-buggy 20-line program (e.g. `int* p = new int; return 0;`) and watched it report the leak.
- [ ] Ran **`valgrind --tool=cachegrind`** on something and looked at the output with `cg_annotate`.
- [ ] (Bonus) Opened **Godbolt** with a 10-line function, watched the assembly change as you toggle `-O0` → `-O2` → `-O3 -march=native`.

### Phase 3 — Project (Days 3–6, ~6–8 hours)

The full brief lives in [`project/README.md`](./project/README.md). High-level milestones:

- [ ] **Step 1 — Skeleton:** directory layout in place, `CMakeLists.txt` builds Debug + Release with `-Wall -Wextra -Wpedantic`, `clang-format` config in place.
- [ ] **Step 2 — Data Generator:** `data/gen.py` produces a CSV with `timestamp_ns,symbol,bid,ask,bid_size,ask_size`. Generated both `synthetic_small.csv` (10 k rows) and `synthetic_large.csv` (10 M rows).
- [ ] **Step 3 — CSV Loader:** `load_ticks(path)` returns `std::vector<Tick>`; first & last ticks print sane values.
- [ ] **Step 4 — Strategy API:** `strategy.hpp` copied unchanged; a `null_strategy.cpp` returns no orders and compiles.
- [ ] **Step 5 — Replay Engine:** `engine.cpp` runs the per-tick loop, times each `on_tick`, and prints p50 / p90 / p99 / p999 via the `LatencyHistogram` helper.
- [ ] **Step 6 — Implement the Strategy Spec:** your `spec_strategy.cpp` implements [`project/STRATEGY_SPEC.md`](./project/STRATEGY_SPEC.md), matches the expected order stream on `tiny.csv`, and runs end-to-end on `synthetic_large.csv`. Headline numbers captured:
  - Median per-tick latency: \_\_\_\_\_ ns
  - p99 per-tick latency:    \_\_\_\_\_ ns
  - p999 per-tick latency:   \_\_\_\_\_ ns
  - Ticks/sec throughput:    \_\_\_\_\_ M ticks/s
- [ ] **Step 7 — Benchmarking:** `bench/bench.cpp` has at least one Google Benchmark; compared `-O0` vs `-O2` runtimes; ran `perf stat -d ./quant_runner ...` and recorded IPC, cache-miss%, branch-miss% in the project README. *(Optional: sanitized build with `-fsanitize=address,undefined` runs clean.)*
- [ ] **Step 8 — Project README:** accurate build instructions, hardware documented (CPU/RAM/OS/kernel), headline latency numbers, `perf stat` snippet, **one paragraph about what surprised you**.

### Phase 4 — Submission

- [ ] Code pushed to a public repo (GitHub / GitLab).
- [ ] Repo has a clear README at the root.
- [ ] Large data files (`synthetic_large.csv`) are gitignored, not committed.
- [ ] Repo link shared in the CSoT group / submission form.
- [ ] You can explain your project to a friend in < 2 minutes.
- [ ] Signed in to **[csot-low-latency.devclub.in](https://csot-low-latency.devclub.in/dashboard/)** with your DevClub IITD account.
- [ ] Uploaded `spec_strategy.so` to the dashboard. Submission shows status `correct` on the public dataset. Your numbers on the EC2 judge:
  - p50 \_\_\_\_\_ ns *(reference: 152 ns)*
  - p99 \_\_\_\_\_ ns *(reference: 176 ns)*
  - throughput \_\_\_\_\_ M ticks/s *(reference: 5.38)*
- [ ] You found your row on the [live leaderboard](https://csot-low-latency.devclub.in/leaderboard/).

### Phase 5 — Self-Check (the "would you survive an interview" round)

If you can answer all of these without Googling, you've internalized Week 1:

- [ ] **Q1.** Difference between latency and throughput? Can a system have one but not the other?
- [ ] **Q2.** Why is `std::vector<int>` typically faster to iterate than `std::list<int>`, even though both are O(N)?
- [ ] **Q3.** Approximate latency of: L1 hit, L3 hit, DRAM access, disk read, transatlantic network round-trip?
- [ ] **Q4.** Why is calling `malloc()` in a hot loop bad? What two patterns replace it?
- [ ] **Q5.** First call takes 5 µs, next 1 000 calls take 50 ns each — what's going on?
- [ ] **Q6.** Why is `std::chrono::system_clock` a bad choice for benchmarking?
- [ ] **Q7.** What's a cache line? How big is it on x86? Why does its size matter?
- [ ] **Q8.** What does `benchmark::DoNotOptimize(x)` actually do?
- [ ] **Q9.** When would you reach for Cachegrind instead of `perf`? Vice versa?
- [ ] **Q10.** What does `perf stat` mean by "instructions per cycle"? What's a "good" value?

(Answers are scattered throughout the topic files — go find them.)

### ⭐ Stretch Goals (Optional)

- [ ] Replaced CSV with a custom **binary tick format** (16–32 B per tick). Measured load-time difference.
- [ ] **`mmap`-ed the input file** instead of `std::ifstream`-ing it.
- [ ] **Linked against jemalloc** (`-ljemalloc`) and compared:
  - default malloc: \_\_\_\_\_ ns/tick
  - jemalloc:       \_\_\_\_\_ ns/tick
- [ ] Replaced `std::chrono::steady_clock` with **calibrated `rdtsc`** and measured the timing-overhead reduction.
- [ ] **Plotted the latency histogram** (matplotlib / gnuplot). Log-scale x-axis is illuminating.
- [ ] Generated a **flame graph** of your runner and identified the hottest function.
- [ ] Used **`taskset -c 3 ./quant_runner`** to pin to one core — did the variance / p99 drop?
- [ ] Ran with **`cpupower frequency-set -g performance`** + turbo disabled — confirmed numbers got more stable.
- [ ] Reimplemented the same strategy spec with a different state layout and compared per-tick latency.
- [ ] Ran **Cachegrind** on your runner and identified the source line with the most L1 misses.
- [ ] Profiled with **Callgrind + KCachegrind** — found one inefficiency you wouldn't have spotted otherwise.

### 🧠 The Single Most Important Box

If you check **only one** box from this entire list, make it this one:

- [ ] **I built something that runs in a tight loop, and I have *measured numbers* I trust — not estimates, not gut feelings.**

That single skill is what the next 4 weeks are built on. Everything else is optimization.

---

## 🎬 Must-Watch Talks (general — applies to the whole week)

In addition to the topic-specific "Further Reading" sections inside each file, these four talks define the field. Watch them in order.

1. **[Mike Acton — "Data-Oriented Design and C++"](https://www.youtube.com/watch?v=rX0ItVEVjHc)** (CppCon 2014, 1 h) — *The* talk that changed how a generation of engineers thinks about performance.
2. **[Chandler Carruth — "Efficiency with Algorithms, Performance with Data Structures"](https://www.youtube.com/watch?v=fHNmRkzxHWs)** (CppCon 2014, 1 h) — Why `std::vector` beats everything else, with measurements.
3. **[Scott Meyers — "CPU Caches and Why You Care"](https://www.youtube.com/watch?v=WDIkqP4JbkE)** (1 h) — The clearest single explanation of memory hierarchy ever recorded.
4. **[Carl Cook — "When a Microsecond Is an Eternity: HFT in C++"](https://www.youtube.com/watch?v=NH1Tta7purM)** (CppCon 2017, 1 h) — A real HFT practitioner shows what production low-latency code looks like.

---

## 📚 Books (reference, optional)

- **"Computer Systems: A Programmer's Perspective"** (Bryant & O'Hallaron) — the canonical undergrad textbook. Chapters 5, 6, 9. Often called CS:APP or "the bible".
- **"Systems Performance"** (Brendan Gregg) — covers Linux performance end-to-end. Reference book.
- **"Optimized C++"** (Kurt Guntheroth) — practical C++ perf cookbook.
- **"What Every Programmer Should Know About Memory"** ([Drepper, free PDF](https://people.freebsd.org/~lstewart/articles/cpumemory.pdf)) — book-length paper, the gold standard.

---

## 🎓 Free University Courses

- **[CMU 15-418 / 15-618 — Parallel Computer Architecture and Programming](https://www.cs.cmu.edu/~418/)** — best-in-class. Lectures on YouTube. Relevant from Week 3 onwards.
- **[MIT 6.172 — Performance Engineering of Software Systems](https://ocw.mit.edu/courses/6-172-performance-engineering-of-software-systems-fall-2018/)** — full course on OCW. The Week 1-3 lectures align perfectly with this track.
- **[IIT Delhi COL216 — Computer Architecture](https://www.cse.iitd.ac.in/~rijurekha/col216_2022.html)** — your in-house course; assignment 1 mirrors our Week 2 project.

---

## 🤷 If You Only Read 5 Things This Week

1. [Latency Numbers Every Programmer Should Know](https://colin-scott.github.io/personal_website/research/interactive_latency.html) — interactive
2. [Gallery of Processor Cache Effects](https://igoro.com/archive/gallery-of-processor-cache-effects/) — short, mind-bending
3. [Mike Acton — Data-Oriented Design (video)](https://www.youtube.com/watch?v=rX0ItVEVjHc)
4. [Google Benchmark User Guide](https://github.com/google/benchmark/blob/main/docs/user_guide.md)
5. [Carl Cook — When a Microsecond Is an Eternity (video)](https://www.youtube.com/watch?v=NH1Tta7purM)

These five alone will rewire your brain.

---

## 💬 Communities

- **[r/cpp](https://reddit.com/r/cpp)** — current discussions, talks.
- **[r/algotrading](https://reddit.com/r/algotrading)** — quant side.
- **[#include <C++> Discord](https://www.includecpp.org/)** — great place to ask C++ questions.
- **[Hacker News](https://news.ycombinator.com/)** — search for "performance", "latency", "HFT".

---

## 🚀 Ready?

Start with [`01-introduction.md`](./01-introduction.md). See you on the other side. ⚡
