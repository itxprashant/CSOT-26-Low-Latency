# Week 1 Project — Fastest Correct Strategy

> **Mission:** Build a minimal C++ trading-platform runner that executes a **judge-defined strategy spec**, replays historical market ticks against it, verifies correctness, measures end-to-end latency per decision, and prints a summary. Over the next 4 weeks we will turn this into a multi-threaded, lock-free, networked, leaderboard-ranked beast. This week, just get the correct synchronous baseline walking.

---

## 📦 What This Folder Gives You

We've shipped a set of starter files that you should **copy verbatim** into your project. These remove the parts that are tedious and error-prone without being educational, so you can focus on the engine and the strategy.

| File | Purpose | Status |
|---|---|---|
| [`include/strategy.hpp`](./include/strategy.hpp) | The **frozen ABI** — `Tick`, `Order`, `Strategy`, `create_strategy()` factory. | **Do not modify.** |
| [`include/histogram.hpp`](./include/histogram.hpp) | A minimal exponential-bucket `LatencyHistogram` for the engine. | Use as-is; swap for HdrHistogram in Week 2+ if you want more precision. |
| [`STRATEGY_SPEC.md`](./STRATEGY_SPEC.md) | The **frozen algorithm** every submission must implement. | **Do not tune.** Faster implementations only. |
| [`CMakeLists.txt`](./CMakeLists.txt) | Production-grade build template (Release/Debug, sanitizers, LTO, `compile_commands.json`). | Uncomment the `add_executable` / `add_library` lines as you write each component. |
| [`.gitignore`](./.gitignore) | Build, perf, valgrind, editor noise. | Use as-is. |
| [`data/gen.py`](./data/gen.py) | Reference tick generator. Seeded → reproducible across all students. | Customize parameters if you want, but the **default seed is the cohort baseline**. |
| [`data/tiny.csv`](./data/tiny.csv) | 20-row hand-checkable golden file for unit-testing your loader and engine. | Use as-is. |
| [`TROUBLESHOOTING.md`](./TROUBLESHOOTING.md) | Solutions to the 50 most common errors (`perf` permissions, `dlopen` issues, ASan false-positives, WSL gotchas, etc.). | Skim once, refer back when stuck. |

Everything else (engine, loader, implementation details inside `on_tick`, bench harness) is yours to design.

---

## 🔒 The Three Contracts You MUST Respect

These three contracts are **mandatory**. Everything else in this README is "suggested".

### Contract 1 — The CSV schema (frozen)

Every tick file uses this exact schema, with a header row:

```
timestamp_ns,symbol,bid_px,ask_px,bid_qty,ask_qty
1700000000000000000,SYM0,99.9900,100.0100,500,500
1700000000001000000,SYM1,249.9900,250.0100,300,300
...
```

Guarantees you can rely on:

- **Header row is always present** and is exactly the line above (no spaces).
- **Encoding:** ASCII UTF-8, Unix line endings (`\n`).
- **`timestamp_ns`** — nanoseconds since the Unix epoch. **Strictly ascending across the whole file** (no duplicates, no out-of-order ticks).
- **`symbol`** — short identifier, `[A-Z0-9_]{1,15}`, no quotes.
- **`bid_px` / `ask_px`** — decimal price, up to 4 decimal places. `bid_px <= ask_px` always.
- **`bid_qty` / `ask_qty`** — positive integers (≥ 1), `uint32_t`-safe.
- **No missing fields, no empty rows.**

### Contract 2 — The strategy ABI (frozen)

The `Tick` / `Order` structs and `class Strategy` are defined in [`include/strategy.hpp`](./include/strategy.hpp). The header includes `static_assert`s on `sizeof(Tick)` and `sizeof(Order)` so you'll notice immediately if you drift.

> ⚠️ **Why this matters from day one:**
> The live judge already `dlopen()`s your strategy `.so` and calls exactly the symbols defined in `strategy.hpp`. If your `on_tick` signature drifts — even by a `const` qualifier — your strategy will not load and the upload fails with a `dlsym` error. Lock the ABI in Week 1 and you'll never think about it again.

Your strategy `.so` must export a single C-linkage factory:

```cpp
extern "C" csot::Strategy* create_strategy();
```

The judge does:

```cpp
void* h = dlopen("your_strategy.so", RTLD_NOW);
auto  make = (csot::Strategy*(*)())dlsym(h, "create_strategy");
csot::Strategy* s = make();
```

### Contract 3 — The strategy spec (frozen)

The algorithm your `on_tick(...)` implements is defined in [`STRATEGY_SPEC.md`](./STRATEGY_SPEC.md). It is a deterministic z-score mean-reversion strategy over a per-symbol rolling window.

You may optimize **how** you implement it:

- precompute rolling sums
- use contiguous arrays instead of maps
- avoid heap allocation after `on_init`
- use better branch layout
- use SIMD or compile-time constants later in the track

You may not change **what** it does:

- thresholds are fixed
- window size is fixed
- order side, price, and quantity rules are fixed
- position update rules are fixed
- warm-up behaviour is fixed

> 🎯 **Leaderboard rule:** correctness is a hard gate. The judge compares your emitted order stream to the reference implementation. If the streams differ, the submission is incorrect. Among correct submissions, ranking is by latency and throughput only.

---

## 🎯 Learning Goals

By completing this project you will:

1. Have a **CMake / C++20** project set up with `clang-format`, sanitizers, and Google Benchmark.
2. Have run `perf stat` and Google Benchmark on **your own code**.
3. Understand what a "**hot path**" looks like (the per-tick processing loop).
4. Have an **interface boundary** (strategy API) that you'll later extend with networking and parallelism.
5. Have a real, working measurement infrastructure for **latency** (p50, p99, p999).
6. Have experienced **programming to a stable contract** — a skill every production engineer needs.
7. Have learned that **implementation quality** is measurable and rankable, separate from algorithmic cleverness.

---

## 🏗️ What You Must Build

You must deliver, at a minimum:

### 1. The Strategy ABI — **provided, copy as-is**

[`include/strategy.hpp`](./include/strategy.hpp) defines `Tick`, `Order`, and `class Strategy`. Drop it into your project's `include/` directory unchanged.

The header's design properties (in case you're curious):
- `on_tick` is the only "hot" function — its latency is what the engine measures.
- All inputs are passed by reference / view (no heap copy).
- The return type is `std::vector<Order>` for Week-1 simplicity — you'll replace this with a zero-allocation alternative in Week 2.

### 2. The Replay Engine (`engine.hpp` / `engine.cpp`) — *yours to write*

A class that:

- Loads market data from a CSV using the [frozen schema](#contract-1--the-csv-schema-frozen).
- Constructs the `Strategy` (via `dlopen` + `create_strategy()`, OR by linking it directly — your choice for Week 1).
- Loops over ticks. For each one:
  - Note the cycle count (or `steady_clock` time).
  - Call `strategy.on_tick(t)`.
  - Note the cycle count again.
  - Record the delta in a histogram.
- After the loop, prints the latency summary.
- Applies the deterministic fill model from [`STRATEGY_SPEC.md`](./STRATEGY_SPEC.md) and calls `strategy.on_fill(...)` so strategy state stays consistent.

### 3. The Spec Strategy (`strategies/spec_strategy.cpp`) — *yours to write*

A concrete implementation of [`STRATEGY_SPEC.md`](./STRATEGY_SPEC.md): rolling 64-tick z-score mean reversion per symbol.

The point is **not** to invent a better strategy. The point is to implement the fixed spec correctly, then make that implementation fast. Don't forget to define:

```cpp
extern "C" csot::Strategy* create_strategy() { return new SpecStrategy(); }
```

### 4. Sample Data Files

- [`data/tiny.csv`](./data/tiny.csv) — **provided, 20 rows**, hand-checkable. Use this for unit tests and quick sanity runs.
- `data/synthetic_small.csv` — 10 000 ticks. Generate with `python3 data/gen.py --rows 10000 --out data/synthetic_small.csv`. Useful for fast iteration.
- `data/synthetic_large.csv` — 10 000 000 ticks. Generate with `python3 data/gen.py --rows 10000000 --out data/synthetic_large.csv`. **gitignore this file.**

> 💡 The provided [`data/gen.py`](./data/gen.py) uses a fixed seed (`42`) by default. **Everyone in the cohort generates byte-identical files**, which means your benchmark numbers are directly comparable to your friends' — and to the leaderboard baseline.

### 5. The Benchmark Driver (`bench/bench.cpp`) — *yours to write*

A Google Benchmark file that benchmarks the **per-tick latency** of `engine.run_tick(...)` for the spec strategy. Output JSON for easy plotting.

### 6. A `CMakeLists.txt` — **provided, ready to use**

Copy [`CMakeLists.txt`](./CMakeLists.txt) into your project root. It's set up for:

- C++20, `-Wall -Wextra -Wpedantic`, Release defaults to `-O2 -march=native -fno-omit-frame-pointer`.
- A `Debug` build with optional `-DENABLE_SANITIZERS=ON` for ASan + UBSan.
- LTO for Release (auto-disabled if your toolchain doesn't support it).
- `compile_commands.json` for clangd / IDE integration.

The target definitions (`engine`, `quant_runner`, `spec_strategy`, `quant_bench`) are present but **commented out**. Uncomment each one as you implement the corresponding source file. Build with:

```bash
cmake -B build && cmake --build build -j
```

Suggested targets it sets up for you:
- `quant_runner` — main binary; CLI: `./quant_runner <strategy.so> <ticks.csv>`.
- `quant_bench` — Google Benchmark binary.
- `spec_strategy.so` — your implementation of the reference strategy as a shared library (so the engine can `dlopen` it, leaderboard-style).

### 7. A `README.md` inside the project folder — *yours to write*

Explaining your build steps, how to run, and your headline numbers (median + p99 per-tick latency on the `synthetic_large` dataset).

---

## 📁 Recommended Directory Layout

```
project/
├── CMakeLists.txt             ← ★ copy from this folder
├── .gitignore                 ← ★ copy from this folder
├── .clang-format              ← optional: pick LLVM or Google style
├── README.md                  ← your own writeup
├── STRATEGY_SPEC.md           ← ★ copy/read: fixed algorithm contract
├── TROUBLESHOOTING.md         ← ★ copy from this folder (for your own reference)
├── include/
│   ├── strategy.hpp           ← ★ copy from this folder unchanged
│   ├── histogram.hpp          ← ★ copy from this folder
│   └── engine.hpp             ← yours to write
├── src/
│   ├── engine.cpp             ← yours to write
│   └── main.cpp               ← the quant_runner entry point
├── strategies/
│   └── spec_strategy.cpp      ← yours; implements STRATEGY_SPEC.md and exports create_strategy()
├── bench/
│   └── bench.cpp              ← yours to write
├── data/
│   ├── gen.py                 ← ★ copy from this folder unchanged
│   ├── tiny.csv               ← ★ copy from this folder unchanged (for tests)
│   ├── synthetic_small.csv    ← generated; safe to commit (small)
│   └── synthetic_large.csv    ← gitignored, generated by gen.py
└── tools/
    └── plot_latency.py        ← optional, plot the histogram
```

Files marked ★ are shipped with the curriculum — copy them verbatim. Everything else is yours.

---

## 🪜 Suggested Implementation Order

Follow this order — each step should compile and run before moving on.

### Step 1 — Skeleton Build (Day 1, ~1 hour)

- Create the directory layout.
- **Copy `strategy.hpp` into `include/`, `gen.py` and `tiny.csv` into `data/`.** Verify `tiny.csv` opens cleanly with `head data/tiny.csv`.
- Write a `CMakeLists.txt` that compiles a "hello world" binary.
- Hook up `clang-format` and `-Wall -Wextra -Wpedantic`.
- Get `Debug` and `Release` builds both working.
- Confirm `strategy.hpp` compiles — `g++ -std=c++20 -c include/strategy.hpp -o /tmp/x.o` should be silent.

### Step 2 — Generate Data (Day 1, ~10 min)

- Run the provided generator (no Python deps beyond stdlib):
  ```bash
  python3 data/gen.py --rows 10000   --out data/synthetic_small.csv
  python3 data/gen.py --rows 10000000 --out data/synthetic_large.csv
  ```
- Spot-check the small file: `head data/synthetic_small.csv` and `wc -l data/synthetic_small.csv` (should print `10001` — 10 000 ticks + header).
- Add `data/synthetic_large.csv` to your `.gitignore`.

### Step 3 — CSV Loader (Day 2, ~1 hour)

- Write a `load_ticks(path) -> std::vector<csot::Tick>` function. Skip the header row. Parse each field according to the [frozen schema](#contract-1--the-csv-schema-frozen).
- **`Tick::symbol` is a `std::string_view`, not an owning string.** It must point at storage **you** keep alive for the whole replay — not at a reused `getline` buffer or a temporary `std::string` that goes out of scope each iteration. The usual Week-1 pattern: **intern** each distinct symbol once (e.g. a small map + `std::deque<std::string>` so addresses stay stable) and set `tick.symbol` to a view into that phonebook. There are only a handful of symbols per file; do not store a full copy of the symbol name on every row unless you know why.
- Don't worry about loader performance yet — we'll measure and improve the hot loop first.
- **Verify on `tiny.csv` first.** Print the first and last loaded ticks and check they match the file exactly (the file has trivial round-number prices on purpose). Re-read `ticks[0].symbol` after the load finishes — it must still be `"SYM0"`, not the last symbol in the file.
- Then verify on `synthetic_small.csv`.

### Step 4 — The Null Strategy (Day 2, ~20 min)

- Implement a `null_strategy.cpp` that:
  - Subclasses `csot::Strategy`.
  - Returns an empty `std::vector<csot::Order>` from `on_tick`.
  - Exports `extern "C" csot::Strategy* create_strategy()`.
- This is your "do-nothing" baseline — the engine's minimum overhead.

### Step 5 — The Engine (Day 3, ~2 hours)

- Write the per-tick loop in `engine.cpp`.
- Use `std::chrono::steady_clock` for timing (we'll upgrade to `rdtsc` in step 8).
- Compute median, p99, p999 with a simple **fixed-bucket histogram** (see `histogram.hpp` template below).
- Compile, run on `tiny.csv` with the null strategy — should print "20 ticks processed", median latency near the noise floor.
- Then run on `synthetic_small.csv` — sanity check.

### Step 6 — Implement the Strategy Spec (Day 4, ~1.5 hours)

- Write `spec_strategy.cpp` implementing [`STRATEGY_SPEC.md`](./STRATEGY_SPEC.md). Don't forget the `create_strategy()` export.
- Build it as a `.so` (or link it directly — either works for Week 1).
- Run on `tiny.csv` first — does it produce the orders the spec predicts?
- Run on `synthetic_small.csv` — sanity check.
- Run on `synthetic_large.csv` → record your **headline number** (median + p99 ns per tick).
- **Optional but recommended:** also generate `public.csv` with `python3 data/gen.py --seed 42 --rows 50000 --out data/public.csv` and run against it. This is the **exact** public dataset the [live leaderboard judge](https://csot-low-latency.devclub.in/dashboard/) uses for its correctness gate — if your engine + strategy passes locally on this file, the upload's `public` phase will pass too.

### Step 7 — Benchmarking (Day 5, ~2 hours)

- Set up Google Benchmark (`bench/bench.cpp`).
- Benchmark the spec strategy isolated.
- Run `perf stat ./quant_runner ...` and capture IPC, cache-miss%, branch-miss%.
- Compare debug vs release builds — feel the difference.

### Step 8 — Upgrade to `rdtsc` (Day 6, optional, ~1 hour)

- Replace `steady_clock` with `__rdtsc` in the engine's measurement code.
- Calibrate the TSC frequency at startup.
- Compare your new latency numbers — they should be *smaller* (less measurement overhead).

### Step 9 — Write It Up (Day 7, ~30 min)

- Fill in the project's `README.md` with:
  - Build instructions
  - Your hardware (CPU, RAM, OS, kernel)
  - The headline latency numbers (median, p99, p999) for your spec-strategy implementation
  - A note that your order stream matches the reference spec on `tiny.csv` and `synthetic_small.csv`
  - A `perf stat` output snippet
  - **One thing that surprised you.**

That last bullet is the most important. It's the start of your intuition.

---

## 📐 A Histogram Helper — provided

The replay engine needs to record per-tick latencies and report percentiles. We've shipped a minimal fixed-bucket histogram at [`include/histogram.hpp`](./include/histogram.hpp). Drop it into your project and use it like:

```cpp
#include "histogram.hpp"

csot::LatencyHistogram hist;
for (const auto& tick : ticks) {
    auto t1 = std::chrono::steady_clock::now();
    strategy.on_tick(tick);
    auto t2 = std::chrono::steady_clock::now();
    hist.record(std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t1).count());
}
hist.print(std::cout);
```

Sample output:

```
count = 10000000
p50  <= 256 ns
p90  <= 1024 ns
p99  <= 4096 ns
p999 <= 16384 ns
```

Buckets are powers of two (32 of them, covering up to ~4.3 s) — coarse but fine for a Week-1 sanity check. For production-grade precision you'd reach for [**HdrHistogram**](https://github.com/HdrHistogram/HdrHistogram_c) — feel free to swap it in as a stretch goal.

<details>
<summary><i>Click for the full implementation (in case you're curious)</i></summary>

```cpp
#pragma once
#include <array>
#include <cstdint>
#include <ostream>

namespace csot {
class LatencyHistogram {
    static constexpr std::size_t N = 32;
    std::array<std::uint64_t, N> buckets_{};
    std::uint64_t count_ = 0;
public:
    void record(std::uint64_t ns) noexcept {
        const std::size_t idx = (ns == 0) ? 0 : 63 - __builtin_clzll(ns);
        const std::size_t clamped = (idx < N) ? idx : N - 1;
        ++buckets_[clamped];
        ++count_;
    }
    std::uint64_t count() const noexcept { return count_; }
    std::uint64_t percentile(double q) const noexcept {
        if (count_ == 0) return 0;
        auto target = static_cast<std::uint64_t>(static_cast<double>(count_) * q);
        if (target == 0) target = 1;
        std::uint64_t cum = 0;
        for (std::size_t i = 0; i < N; ++i) {
            cum += buckets_[i];
            if (cum >= target) return 1ULL << (i + 1);
        }
        return 1ULL << N;
    }
    void print(std::ostream& os) const {
        os << "count = " << count_           << '\n'
           << "p50  <= " << percentile(0.50) << " ns\n"
           << "p90  <= " << percentile(0.90) << " ns\n"
           << "p99  <= " << percentile(0.99) << " ns\n"
           << "p999 <= " << percentile(0.999) << " ns\n";
    }
};
}  // namespace csot
```

</details>

---

## 🚀 Stretch Goals (Optional, for the Eager)

If you finish early, try any of these:

1. **`mmap` the CSV** instead of reading line-by-line — much faster startup.
2. Replace CSV with a custom **binary tick format** (16-32 bytes per tick, packed). Measure the load-time difference.
3. **Plot the latency histogram** with matplotlib (`tools/plot_latency.py`). Eyeballing a log-scale histogram teaches you a lot.
4. Re-implement the same strategy spec using a different state layout (e.g. `std::unordered_map` vs fixed arrays) and compare per-tick latency.
5. Run with `LD_PRELOAD=libjemalloc.so` and see if anything changes.
6. Try `perf record` + flame graph and identify your hottest function.
7. Use `taskset -c 3` to pin your benchmark to a single core. Does the variance drop?

We'll cover #5-#7 properly in later weeks, but it's never too early to play.

---

## 📚 Quant-Side Resources

You're not expected to be a finance expert — the *engineering* is what we're optimizing for. But if you want context on what real trading platforms look like, or need realistic market data:

### Production-Grade Reference Designs

- 📰 [QuantConnect "Hello World" strategy](https://www.quantconnect.com/docs/v2/writing-algorithms/getting-started) — see what a strategy API looks like in production.
- 📰 [Backtrader Quickstart](https://www.backtrader.com/docu/quickstart/quickstart/) — Python backtesting framework; the simple version of what you're building.
- 📰 [NautilusTrader](https://github.com/nautechsystems/nautilus_trader) — production-grade open-source trading platform in Rust + Cython. Worth skimming for design ideas.
- 📰 [IMC Prosperity retrospectives](https://imc-prosperity.notion.site/) — for the "leaderboard" vibe we're channelling.

### Free Historical / Real-Time Market Data

- 🌐 [Polygon.io free tier](https://polygon.io/) — US stocks (limited but enough for a sandbox).
- 🐍 [Yahoo Finance via `yfinance`](https://github.com/ranaroussi/yfinance) — Python lib, easy CSVs. Great for your Week-1 synthetic-replacement.
- 🌐 [Binance REST + WebSocket](https://binance-docs.github.io/apidocs/spot/en/) — crypto, free, real-time. Ideal for the Week 5 networking module.
- 🐍 [NSE historical data downloader](https://nsepython.readthedocs.io/) — Indian markets if you want familiar tickers.

---

## 🏆 The Live Leaderboard

**The leaderboard is live: [csot-low-latency.devclub.in](https://csot-low-latency.devclub.in/).** Originally planned for Week 5, it shipped early — so you can drop your `spec_strategy.so` the moment it passes `tiny.csv` and watch yourself appear on the public table.

### How it works

1. Sign in with your DevClub IITD identity (one-button OAuth, no separate account).
2. Open the [**dashboard**](https://csot-low-latency.devclub.in/dashboard/) and drag your `spec_strategy.so` onto the dropzone.
3. The judge picks it up within a few seconds, replays your code against **two datasets** inside a `bwrap` sandbox, and posts back a score.
4. The submission detail page shows you the histogram, percentiles, stage timeline, and (when something goes wrong) the exact tick where your order stream diverged from the reference.

Correctness is binary, and it gates everything:

- Your emitted `Order` stream is compared byte-for-byte against the reference's. **One disagreement and the submission is `incorrect` — you don't rank.**
- If both datasets pass, the judge ranks you by **latency × throughput** (see [`PLATFORM.md`](../../PLATFORM.md) §5 for the exact formula: 40 % p50, 40 % p99, 15 % throughput, 5 % compliance bonus).

### The two datasets

| Dataset | Rows | Seed | Reproducible? | Role |
|---|---:|---:|---|---|
| **Public** | 50 000 | `42` (the `gen.py` default) | ✅ Yes — `python3 data/gen.py --seed 42 --rows 50000 --out public.csv` produces the exact file the judge uses | Correctness smoke test. Fails here and you stop. |
| **Hidden** | 1 000 000 | sealed | ❌ No — held out on the judge box only | The latency ranking happens here. |

The public dataset is *intentionally* reproducible — debug your `incorrect` runs locally before bothering the judge. The hidden dataset is *intentionally* opaque — your code has to generalize.

### Reference numbers (the judge's own implementation)

These are what your code is being measured against. Captured by running [`STRATEGY_SPEC.md`](./STRATEGY_SPEC.md)'s reference implementation through the same sandbox the judge uses for your submission:

| | EC2 judge (`c7i.xlarge`, 4 vCPU / 8 GiB, Amazon Linux 2023) |
|---|---:|
| **p50** `on_tick` | 152 ns |
| **p99** `on_tick` | 176 ns |
| **p999** `on_tick` | 184 ns |
| **Throughput** | 5.38 M ticks/s |

Your laptop will produce *different* (often faster on single-core) numbers — the judge is the only ground truth.

### Submission policy

- **Cooldown + daily quota** are enforced server-side. The dashboard shows the current window; trying to spam-upload returns `429`.
- **One identity, one entry.** No teams.
- **Max upload size:** 5 MiB. `.so` only, must export `extern "C" csot::Strategy* create_strategy()`.
- **Build for the judge CPU, not yours.** Release defaults in the shipped `CMakeLists.txt` use `-march=native` — great for local benchmarks, but if your laptop has newer instructions than the judge EC2 box, the upload fails with **`rejected` / `runner exit 132`**. Rebuild with `-march=x86-64-v2` before uploading. If *every* upload fails (even the shipped [`samples/spec_strategy.so`](./samples/spec_strategy.so)), the judge runner itself needs a rebuild — see [`TROUBLESHOOTING.md`](./TROUBLESHOOTING.md).
- **The judge VM may be stopped** between cohort sessions to save cost — when it is, the dashboard shows a grey "judge offline" pill. Submissions still queue, and drain as soon as the judge is back up. (`Restart=always` means a fresh `aws ec2 start-instances` brings everything back without intervention.)

### What you do NOT have to do this week

The leaderboard is here, but Week 1 does not require you to top it. Your local engine + histogram is plenty for the learning goals. Upload when your spec strategy is **correct on `tiny.csv` and produces sane numbers on `synthetic_large.csv`** — anything beyond that is preview for Weeks 2–5 where we actually compete on the tail.

---

## ❓ FAQ

**Q: I don't know C++ that well — should I use Python?**
A: This whole track is about controlling the machine. Python interpreter overhead drowns out the things we want to measure. Push through with C++. Use Compiler Explorer (godbolt.org) liberally.

**Q: Do I need a `clang-format` config?**
A: Recommended. Use the LLVM or Google style — anything consistent.

**Q: What if my code is unsafe?**
A: This week: build with `-DENABLE_SANITIZERS=ON` (the shipped CMake exposes this flag) for ASan + UBSan. It catches 90% of common bugs.

**Q: My laptop is slow / I'm on Windows — what do I do?**
A: WSL2 will do for Week 1. A free `g4dn.xlarge` on AWS or a Colab terminal also work. By Week 3 you'll really want native Linux.

**Q: Can I write the engine in Rust / Zig / D?**
A: We won't stop you, but our examples & support are C++. You're on your own for tooling. If you do, share a writeup — others will love it.

**Q: Can I write a different strategy?**
A: Not for the ranked challenge. Every submission must implement [`STRATEGY_SPEC.md`](./STRATEGY_SPEC.md). Otherwise the contest would measure strategy choice instead of low-latency engineering. You can experiment with other strategies locally, but the leaderboard cares only about the fixed spec.

**Q: My upload was `rejected` with `runner exit 132`. What happened?**
A: That's `SIGILL` — your `.so` contains CPU instructions the judge doesn't support. Almost always caused by building with `-march=native` on a newer machine than the judge EC2 box. Rebuild with `-march=x86-64-v2` (or see the portable-build one-liner in [`TROUBLESHOOTING.md`](./TROUBLESHOOTING.md)).

**Q: I'm stuck on a build error / segfault / weird perf result.**
A: Check [`TROUBLESHOOTING.md`](./TROUBLESHOOTING.md) — it covers the most common pitfalls (`perf` permissions, `dlopen` symbol issues, portal `runner exit 132`, ASan false positives, WSL gotchas, noisy benchmarks, etc.).

---

## 📤 Submission

Week 1 has **two complementary submission tracks**. Both are expected; do them in this order.

### 1. The Git repo (for the cohort review)

A **public Git repo** (GitHub / GitLab) containing:

- Your code matching the structure above
- The project's `README.md` with the headline numbers
- A `data/gen.py` (don't commit the multi-GB CSV — gitignore it)
- A `.gitignore` for `build/`, `*.csv` (large ones), `*.so`

Drop the link in the CSoT group when you're done. We'll feature the cleanest setups as reference for everyone in Week 2.

### 2. The leaderboard (for the live ranking)

Sign in at **[csot-low-latency.devclub.in](https://csot-low-latency.devclub.in/)** with your DevClub IITD account and upload your `spec_strategy.so` from the dashboard. The judge reports back within seconds with a histogram, percentiles, and a rank. See [§ The Live Leaderboard](#-the-live-leaderboard) above for the full flow.

You can upload as often as the per-participant cooldown allows — there's no penalty for early uploads, even an `incorrect` one is useful signal.

---

## 🎉 You Made It

You now have a working measurement harness and a piece of code that runs millions of times per second. **That is genuinely the hardest part of any low-latency project — getting to the point where you can iterate quickly and trust your numbers.**

Now go forth and make it fast. 🏎️

(See you in Week 2, where we eliminate every last `malloc` from the hot path.)
