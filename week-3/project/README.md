# Week 3 Project — Fastest Correct Parallel Tick Aggregator

> **Mission:** Implement a **single `aggregator.cpp`** that reduces a huge binary tick stream into a fixed per-symbol table of integer aggregates — `count`, `sum_price`, `min_price`, `max_price`, `sum_qty` — **correctly**, **deterministically**, and **as fast as four cores allow**. Week 1 taught you to measure one core; Week 2 taught you to feed its cache. Week 3 is where you use *all* the cores — and learn that threads only pay off if you stop them from racing, false-sharing, and getting shuffled by the kernel. Same "fastest correct implementation" game, a workload built to reward going wide.

---

## 📦 What This Folder Gives You

Copy the ★ files verbatim. They remove the tedious, error-prone scaffolding so you can focus on the one file that is *yours*: the aggregator.

| File | Purpose | Status |
|---|---|---|
| [`include/aggregate.hpp`](./include/aggregate.hpp) | The **frozen ABI** — `AggTick`, `SymbolAgg`, `Aggregator`, `create_aggregator()`. | ★ **Do not modify.** |
| [`AGG_SPEC.md`](./AGG_SPEC.md) | The **frozen aggregate spec** every submission must compute (record format, the five integer aggregates, empty-symbol rule, determinism guarantee). | ★ **Do not change.** Faster implementations only. |
| [`samples/aggregator.stub.cpp`](./samples/aggregator.stub.cpp) | A compiling, **correct single-threaded** skeleton wired to the ABI. Passes `tiny.ticks` out of the box; ranks near the bottom. | ★ Copy to `aggregator.cpp` and make it go wide. |
| [`harness/main.cpp`](./harness/main.cpp) | Local runner: loads a stream, times your `run()`, prints the table as JSON + checksum + throughput. Mirrors the judge. | ★ Use as-is (you never upload it). |
| [`CMakeLists.txt`](./CMakeLists.txt) | Build template (Release/Debug, `-pthread`, TSan, ASan, LTO, `CSOT_JUDGE_BUILD`, `CSOT_AGG_SRC`). | ★ Use as-is. |
| [`.gitignore`](./.gitignore) | Build, stream, perf, sanitizer noise. | ★ Use as-is. |
| [`data/gen_ticks.py`](./data/gen_ticks.py) | Seeded tick-stream generator. `--tiny`, `--dump`, `--stats` modes. | ★ Default seed is the cohort baseline. |
| [`data/tiny.ticks`](./data/tiny.ticks) + [`data/tiny.agg.json`](./data/tiny.agg.json) | A 10-tick hand-checkable golden stream and its exact reference table. | ★ Use for unit-testing your aggregator. |
| [`TROUBLESHOOTING.md`](./TROUBLESHOOTING.md) | Threading, race, false-sharing, and pinning gotchas, plus pointers back to Weeks 1–2. | ★ Skim once, refer back when stuck. |
| [`tools/plot_scaling.py`](./tools/plot_scaling.py) | Optional: plot throughput + speedup vs. thread count. | Optional. |

Everything inside `run()` — your partitioning, your per-thread layout, your pinning, your merge — is yours to design.

---

## 🔒 The Four Contracts You MUST Respect

These are **mandatory**. Everything else in this README is "suggested".

### Contract 1 — The stream format (frozen)

A stream is a raw array of 32-byte `AggTick` records, little-endian, no header (see [`AGG_SPEC.md`](./AGG_SPEC.md) §2):

```text
offset 0  : uint64  timestamp_ns   (non-decreasing)
offset 8  : int64   price          (FIXED-POINT: real price * 10000)
offset 16 : uint32  symbol_id       (0 .. 1023)
offset 20 : uint32  qty             (> 0)
offset 24 : uint64  _reserved        (always zero — do not read)
```

Guarantees: file size is a multiple of 32; `symbol_id ∈ [0, 1024)`; `price` is a signed integer you aggregate directly (never float); sums fit in 64 bits. The block of work is processed in **any order you like** — the aggregates don't depend on it.

### Contract 2 — The aggregator ABI (frozen)

`AggTick`, `SymbolAgg`, and `class Aggregator` are defined in [`include/aggregate.hpp`](./include/aggregate.hpp), with `static_assert`s on `sizeof(AggTick) == 32` and `sizeof(SymbolAgg) == 40` as the canary. Your file must export exactly:

```cpp
extern "C" csot::Aggregator* create_aggregator();
```

The judge does the moral equivalent of:

```cpp
csot::Aggregator* agg = create_aggregator();
agg->on_init(num_symbols);                  // num_symbols == 1024
agg->run(ticks, n, out);                    // <-- this call is timed; you fill out[0..1024)
```

> ⚠️ **Why this matters from day one:** the judge builds your `aggregator.cpp` against *its own* `main()` and this header. If your `run()` signature drifts — even a `const` — your submission won't link and the upload fails. Lock the ABI now.

### Contract 3 — The aggregate spec (frozen)

The table your `run()` computes is defined in [`AGG_SPEC.md`](./AGG_SPEC.md): one `SymbolAgg` row per symbol id `0..1023`, each holding the five integer aggregates, with absent symbols written as canonical zeros.

You may optimize **how** you compute it:

- partition the stream across threads, one disjoint chunk each
- per-thread partial tables on their own cache lines (`alignas(64)` — no false sharing)
- pin threads to distinct cores (`sched_setaffinity`)
- zero heap allocation after `on_init`
- prefetch, SIMD min/max, a thread pool

You may not change **what** it computes:

- the five aggregates and their integer semantics are fixed
- `min`/`max` use the empty-symbol rule (canonical `0` when `count == 0`)
- the result has exactly `num_symbols` rows, including absent symbols

> 🎯 **Leaderboard rule:** correctness is a hard gate, and so is **determinism**. The judge compares your table to the reference and **runs your `run()` several times**; if the table differs from the reference, or differs *between your own runs* (a data race), the submission does not rank. Among correct, deterministic submissions, ranking is by wall-clock speed and throughput only.

### Contract 4 — The judge machine (frozen for the season)

Ranked submissions are **not** graded on your laptop. They run on the cohort's dedicated AWS EC2 judge:

| | |
|---|---|
| Instance | **`c7i.xlarge`** — **4 vCPU**, 8 GiB, Intel Sapphire Rapids |
| OS | Amazon Linux 2023 |
| Region | `us-east-1` (maintainer start/stop via `./judge-vm.sh` in the repo root) |

The judge downloads your `aggregator.cpp`, compiles it with **fixed flags** (`-std=c++20 -O3 -march=x86-64-v2 -pthread`, see [`CMakeLists.txt`](./CMakeLists.txt) `CSOT_JUDGE_BUILD`), runs it inside **bubblewrap** against the public then hidden streams, diffs your table against the reference, and checks repeated runs agree. **Four cores** is your scaling target — tune for that, not your 16-thread laptop.

> 📌 **Laptop numbers are for debugging only.** Reproduce the judge locally with `cmake -B build-judge -DCSOT_JUDGE_BUILD=ON` — but expect different absolute `run_ns` and a different core topology. The leaderboard denominators are captured on the `c7i.xlarge` box (`/etc/csot-judge/agg-reference.json`).

---

## 🎯 Learning Goals

By completing this project you will:

1. Have turned an embarrassingly-parallel reduction into a real **map-reduce** across cores — partition, local-reduce, merge.
2. Have a `run()` that performs **zero heap allocation** after `on_init` and spawns its threads off the hot path.
3. Have laid out per-thread state so it **does not false-share** — and have the `perf` numbers proving it.
4. Have **pinned** threads to cores and measured how pinning tightens your run-to-run variance.
5. Have written, observed, and **fixed a data race**, and verified determinism under ThreadSanitizer and a repeat-run checksum.
6. Have driven a **single reduction over hundreds of millions of records** on four cores and ranked on its wall-clock speed.
7. Have practised **programming to a frozen contract** again — a different ABI, the same discipline.

---

## 🏗️ What You Must Build

You deliver exactly **one file**: `aggregator.cpp`. Everything else is provided.

### 1. The aggregator ABI — **provided, copy as-is**

[`include/aggregate.hpp`](./include/aggregate.hpp). Drop it into `include/` unchanged.

### 2. Your aggregator (`aggregator.cpp`) — *yours to write*

A concrete `csot::Aggregator` that implements [`AGG_SPEC.md`](./AGG_SPEC.md):

- allocate all state in `on_init(num_symbols)` (per-thread partial tables, thread bookkeeping)
- in `run()`, partition `[0, n)` across threads, reduce each chunk into its private partial, pin the threads, and merge into `out`
- export `extern "C" csot::Aggregator* create_aggregator() { return new YourAgg(); }`

The point is **not** to invent a metric. The point is to compute the fixed table correctly, then make `run()` fast across four cores. Start from the stub — it's already correct single-threaded:

```bash
cp samples/aggregator.stub.cpp aggregator.cpp
```

> 📌 **Single translation unit.** The judge compiles exactly this one `.cpp`. Helpers, thread functions, padded-table types, and tables all live inside it (use an anonymous `namespace`). No second `.cpp`, no `main()`, no custom CMake. **Threads, `<atomic>`, and `sched_setaffinity` ARE allowed this week** (they weren't in Week 2). See [`TROUBLESHOOTING.md`](./TROUBLESHOOTING.md) → "The single-`.cpp` rule".

### 3. Sample data files

- [`data/tiny.ticks`](./data/tiny.ticks) — **provided, 10 ticks**, hand-checkable; expected table in [`data/tiny.agg.json`](./data/tiny.agg.json). Use for unit tests.
- `data/large.ticks` — generate with `python3 data/gen_ticks.py --accesses 5000000 --seed 42 --out data/large.ticks`. Fast iteration + real scaling signal. **gitignored.**

> 💡 [`data/gen_ticks.py`](./data/gen_ticks.py) uses seed `42` by default — **everyone generates byte-identical streams**, so your numbers compare directly to classmates' and the baseline.

### 4. A `README.md` for your project — *yours to write*

Build steps, your hardware (`nproc` / `lscpu`), and your headline number: **throughput (M ticks/s)** and `run()` wall-clock on `data/large.ticks`, the **speedup vs. 1 thread**, plus confirmation that your table matches `data/tiny.agg.json` and is deterministic across runs.

---

## 📁 Recommended Directory Layout

```
project/
├── CMakeLists.txt             ← ★ copy from this folder
├── .gitignore                 ← ★ copy from this folder
├── README.md                  ← your own writeup
├── AGG_SPEC.md                ← ★ copy/read: the frozen aggregate spec
├── TROUBLESHOOTING.md         ← ★ copy from this folder
├── include/
│   └── aggregate.hpp          ← ★ copy from this folder unchanged
├── harness/
│   └── main.cpp               ← ★ local runner (you never upload it)
├── aggregator.cpp             ← yours; implements AGG_SPEC.md + exports create_aggregator()
├── samples/
│   └── aggregator.stub.cpp    ← ★ correct single-threaded starting point (copy to aggregator.cpp)
├── data/
│   ├── gen_ticks.py           ← ★ copy from this folder unchanged
│   ├── tiny.ticks             ← ★ committed golden stream
│   ├── tiny.agg.json          ← ★ committed golden table
│   └── large.ticks            ← gitignored, generated by gen_ticks.py
└── tools/
    └── plot_scaling.py        ← optional
```

Files marked ★ are shipped — copy them verbatim. `aggregator.cpp` is yours.

---

## 🪜 Suggested Implementation Order

Each step should compile and run before moving on.

### Step 1 — Skeleton build (Day 1, ~30 min)

- Copy the ★ files into place. Confirm the header compiles: `g++ -std=c++20 -Iinclude -c include/aggregate.hpp -o /tmp/x.o` is silent.
- `cmake -B build && cmake --build build -j` builds `agg_runner` from the stub.
- `diff <(./build/agg_runner data/tiny.ticks 2>/dev/null) data/tiny.agg.json` is clean — the stub is correct single-threaded.

### Step 2 — A real stream (Day 1, ~30 min)

- `python3 data/gen_ticks.py --accesses 5000000 --seed 42 --out data/large.ticks`.
- Run the stub on it; record the single-threaded throughput as your **baseline** to beat: \_\_\_\_\_ M ticks/s.
- `python3 data/gen_ticks.py --dump data/tiny.ticks` and walk the 10 ticks against `AGG_SPEC.md` §5 to be sure you understand the reduction.

### Step 3 — Go parallel (Day 2–3, ~2.5 hours)

- `cp samples/aggregator.stub.cpp aggregator.cpp`. Partition `[0, n)` into T chunks (`n*k/T … n*(k+1)/T`), spawn a `std::jthread` per chunk, each reducing into its **own** partial table, then merge ([`02`](../02-stdthread-basics.md), [`03`](../03-data-races.md)).
- Build against your file: `cmake -B build -DCSOT_AGG_SRC=aggregator.cpp && cmake --build build -j`. Re-`diff` against `tiny.agg.json` — still clean.

### Step 4 — Kill false sharing (Day 3, ~1.5 hours)

- Make each thread's partial `alignas(64)` and store partials **thread-major** ([`04-false-sharing.md`](../04-false-sharing.md)). Re-measure throughput at 1–4 threads; the curve should now climb instead of flatlining.

### Step 5 — Zero-allocation hot path (Day 4, ~1 hour)

- Move every allocation (partials, thread storage) into `on_init()`. `run()` must not touch the heap ([`04-zero-allocation.md`](../../week-2/04-zero-allocation.md)). Confirm `perf stat -e page-faults` is flat during `run()`.

### Step 6 — Pin the threads (Day 5, ~1 hour)

- `sched_setaffinity` each worker to a distinct core ([`05-scheduler-and-pinning.md`](../05-scheduler-and-pinning.md)). Note how run-to-run variance drops. Capture headline numbers:
  - `run()` wall-clock on `large.ticks`: \_\_\_\_\_ ms · throughput \_\_\_\_\_ M ticks/s · speedup \_\_\_\_\_×

### Step 7 — Prove it (Day 6, ~1.5 hours)

- Run `run()` 10× in a loop; assert the table checksum is identical every time. Build under `-DENABLE_TSAN=ON` and confirm a clean report.
- `perf stat -e cache-misses,context-switches,instructions` — capture the before/after of your false-sharing fix.

### Step 8 — Write it up (Day 7, ~30 min)

- Project `README.md`: build steps, your hardware, headline throughput + scaling curve, a `perf stat` snippet, and **one thing that surprised you**.

That last bullet is the most important. It's the start of your parallel intuition.

---

## 🧪 Hands-on practice alongside the project

The ranked deliverable is **`aggregator.cpp` only**. These exercises are **not submitted** — they make the friction visceral before you hit it in the project.

### The race and the false-sharing demos

Both live in the topic files and take ten minutes each:

- The **shared-counter race** ([`03-data-races.md`](../03-data-races.md) §1): two threads, one counter, watch the sum come out wrong and varying. Then catch it with TSan, "fix" it with a mutex, and feel it crawl.
- The **`Bad` vs `Good` four-counter** demo ([`04-false-sharing.md`](../04-false-sharing.md) §2): identical logic, one `alignas(64)` apart, ~10× difference. `perf stat` both.

> 💡 Do these **before** Step 4. When you've personally watched four threads lose to one because of a shared cache line, padding your partials stops feeling optional and starts feeling inevitable.

---

## 🚀 Stretch Goals (Optional, for the Eager)

1. Build a **thread pool** in `on_init` (spawn once, wake per `run()`); measure the per-`run()` spawn saving.
2. **Parallel tree merge** instead of a serial merge; measure whether the serial tail mattered (Amdahl in practice).
3. **Prefetch** `ticks[i + K]` a few iterations ahead; sweep `K`, plot throughput.
4. Estimate your box's **memory bandwidth** (STREAM or `perf`), compute the theoretical max ticks/s, and see how close you get.
5. Pin to **physical cores vs. hyperthread siblings**; quantify the difference (`lscpu`, `thread_siblings_list`).
6. **SIMD** the min/max accumulate across a batch of ticks for one symbol-heavy chunk.
7. Build with `-DCSOT_JUDGE_BUILD=ON` and compare to your `-march=native` numbers. How much was the ISA worth?
8. Sweep threads 1→8 with [`tools/plot_scaling.py`](./tools/plot_scaling.py); find exactly where the bandwidth roof flattens your curve.

---

## 🏆 The Live Leaderboard

**The leaderboard is live: [csot-low-latency.devclub.in](https://csot-low-latency.devclub.in/).** Week 3 swaps the active ranked challenge from the Week-2 cache simulator to the **parallel tick aggregator** — a fresh board with a high speed ceiling (four cores × memory bandwidth), so the top is not saturated on day one.

### How it works (you upload source, the judge builds it)

1. Sign in with your DevClub IITD identity.
2. On the dashboard, upload a **single `aggregator.cpp`** (not a compiled `.so`).
3. The judge **builds your file itself** with a fixed, portable toolchain (`-O3 -march=x86-64-v2 -pthread`, `CSOT_JUDGE_BUILD=ON`) against its own `main()` and `aggregate.hpp`, inside a sandbox.
4. It runs your `run()` against **two streams**, diffs the table against the reference, **runs it several times to confirm determinism**, and — if correct and deterministic — ranks you by wall-clock speed.

Uploading source means everyone is measured on the **same compiler and flags**, so the board reflects *your code*, not your laptop's CPU.

### The two streams

| Stream | Ticks | Seed | Reproducible? | Role |
|---|---:|---:|---|---|
| **Public** | 5 000 000 | `42` (the `gen_ticks.py` default) | ✅ Yes — `python3 data/gen_ticks.py --accesses 5000000 --seed 42 --out public.ticks` | Correctness smoke test. Fail here and you stop. |
| **Hidden** | very large | sealed | ❌ No — held out on the judge box | The speed ranking happens here. |

Debug your `incorrect` runs locally on the reproducible public stream before bothering the judge; the hidden stream forces your aggregator to generalize (more symbols active, real eviction pressure on your partials).

### Correctness and determinism gate everything

- Every one of the 1024 rows must match the reference exactly. One disagreement → `incorrect`, no rank.
- Your `run()` must return the **same table on repeated runs**. A data race that "passes sometimes" is flagged non-deterministic → no rank. (This is the multithreading-specific gate; see [`03-data-races.md`](../03-data-races.md).)
- If both streams pass and runs agree, the judge ranks by **wall-clock `run()` time** (and throughput). Going wider than 4 threads buys nothing on a 4-vCPU box; correctness and determinism are never traded for speed.

### Submission policy

- **Single `aggregator.cpp`**, ≤ 1 MiB, must export `extern "C" csot::Aggregator* create_aggregator()`.
- One identity, one entry. No teams.
- Cooldown + daily quota enforced server-side (the dashboard shows the window; spamming returns `429`).
- The judge VM may be stopped between sessions to save cost — submissions queue and drain when it's back.

### What you do NOT have to do this week

You don't have to top the board. Upload when your aggregator is **correct on `tiny.ticks`, deterministic, and faster than the single-threaded baseline**. Beating the reference's parallel time is the game for the eager.

---

## ❓ FAQ

**Q: Why an aggregator? I came here for trading.**
A: A market-data ingest *is* a parallel reduction — fan the wire across cores, fold it into per-symbol state. The fixed strategy returns to the board in Week 4 with a threaded pipeline; this week the workload is reduction-shaped so you internalize threads, races, and false sharing by building one. The leaderboard still measures the same thing: systems-engineering speed.

**Q: Can I submit a `.so` like Week 1?**
A: No. Like Week 2, the judge takes a single `aggregator.cpp` and builds it itself, so the field is identical for everyone.

**Q: Can I `#include <thread>` / use atomics / pin with `sched_setaffinity`?**
A: **Yes — that's the whole point this week.** Week 2 forbade threads; Week 3 expects them. Networking, processes (`fork`/`exec`/`system`), and file I/O are still blocked by the sandbox.

**Q: My parallel version is correct on `tiny.ticks` but the judge says non-deterministic.**
A: You have a data race — two threads writing shared state. Give each thread its own partial and merge after `join()`. Catch it with `-DENABLE_TSAN=ON`. See `AGG_SPEC.md` §8 and [`03-data-races.md`](../03-data-races.md).

**Q: 4 threads is no faster than 1 — or slower.**
A: Almost always **false sharing** on your per-thread partials. `alignas(64)` them and store thread-major. See [`04-false-sharing.md`](../04-false-sharing.md) and `TROUBLESHOOTING.md` → Performance.

**Q: My min/max is 0 for some symbols after merging.**
A: You merged an empty partial (canonical `0`) into a real one. Guard the merge on `count` — `AGG_SPEC.md` §7.

**Q: I scaled 4× on my laptop but the board barely moved.**
A: You're memory-bandwidth-bound on 4 vCPUs. The wins shift to moving fewer bytes efficiently — [`06-bonus-bandwidth-and-pools.md`](../06-bonus-bandwidth-and-pools.md).

---

## 📤 Submission

### 1. The Git repo (for cohort review)

A public repo with your `aggregator.cpp`, project `README.md` (headline numbers + scaling curve), the shipped `gen_ticks.py`, and a `.gitignore` for `build/` and `*.ticks` (keep the golden `tiny.ticks`).

### 2. The leaderboard (for the live ranking)

Sign in at **[csot-low-latency.devclub.in](https://csot-low-latency.devclub.in/)** and upload your `aggregator.cpp` from the dashboard. The judge builds it, runs it (several times), and reports back the table check, throughput, and a rank.

---

## 🎉 You Made It

You took a reduction and made four cores share the work — without sharing a cache line, without a data race, without letting the kernel shuffle your threads. **Parallel speed that comes from layout and placement, not luck, is the skill every low-latency system is built on** — and the coordination you brushed up against (waking workers, handing off chunks) is exactly what Week 4 makes lock-free.

See you in Week 4. ⚡

(Week 4 is coming soon — `std::atomic`, memory orderings, and a lock-free SPSC ring buffer between a market-data thread and the strategy thread, built on the threading discipline you just practised.)
