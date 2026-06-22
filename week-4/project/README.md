# Week 4 Project — Fastest Correct Lock-Free Pipeline

> **Mission:** Implement a **single `pipeline.cpp`** that decodes a binary tick feed on a **producer** thread, hands each tick across **your own lock-free SPSC ring buffer** to a **consumer** thread running the **frozen Week-1 z-score strategy**, and emits the exact reference order stream — **correctly**, **deterministically**, and **as fast as overlapping two cores allows**. Week 1 taught you to measure one core; Week 2 to feed its cache; Week 3 to drive four cores over an array. Week 4 is where two cores hand work to each other through a queue that never locks and never false-shares. Same "fastest correct implementation" game, a workload built to reward `std::atomic` done right.

---

## 📦 What This Folder Gives You

Copy the ★ files verbatim. They remove the tedious, error-prone scaffolding so you can focus on the one file that is *yours*: the pipeline.

| File | Purpose | Status |
|---|---|---|
| [`include/pipeline.hpp`](./include/pipeline.hpp) | The **frozen pipeline ABI** — `WireTick`, `OrderRecord`, `Pipeline`, `create_pipeline()`, `PRICE_SCALE`. | ★ **Do not modify.** |
| [`include/strategy.hpp`](./include/strategy.hpp) | The **frozen Week-1 ABI** — `Tick`, `Order`, `Strategy`. Carried forward unchanged (the spec returns). | ★ **Do not modify.** |
| [`PIPELINE_SPEC.md`](./PIPELINE_SPEC.md) | The **frozen spec** every submission must satisfy (wire format, decode rule, the unchanged strategy + fill model, order-stream equality, determinism). | ★ **Do not change.** Faster implementations only. |
| [`samples/pipeline.stub.cpp`](./samples/pipeline.stub.cpp) | A compiling, **correct single-threaded** skeleton wired to the ABI. Passes `tiny.feed` out of the box; ranks near the bottom. | ★ Copy to `pipeline.cpp` and split it into a pipeline. |
| [`harness/main.cpp`](./harness/main.cpp) | Local runner: loads a feed, times your `run()`, prints the order stream as JSON + checksum + throughput. Mirrors the judge. | ★ Use as-is (you never upload it). |
| [`CMakeLists.txt`](./CMakeLists.txt) | Build template (Release/Debug, `-pthread`, TSan, ASan, LTO, `CSOT_JUDGE_BUILD`, `CSOT_PIPE_SRC`). | ★ Use as-is. |
| [`.gitignore`](./.gitignore) | Build, feed, perf, sanitizer noise. | ★ Use as-is. |
| [`data/gen_feed.py`](./data/gen_feed.py) | Seeded wire-feed generator. `--tiny`, `--dump`, `--orders` modes. | ★ Default seed is the cohort baseline. |
| [`data/tiny.feed`](./data/tiny.feed) + [`data/tiny.orders.json`](./data/tiny.orders.json) | A small golden feed and its exact reference order stream. | ★ Use for unit-testing your pipeline. |
| [`TROUBLESHOOTING.md`](./TROUBLESHOOTING.md) | Atomics, ring-buffer, determinism, pinning, and back-pressure gotchas, plus pointers back to Weeks 1–3. | ★ Skim once, refer back when stuck. |
| [`tools/plot_latency.py`](./tools/plot_latency.py) | Optional: plot throughput across configurations. | Optional. |

Everything inside `run()` — your ring buffer, your thread layout, your pinning, your back-pressure, your strategy implementation — is yours to design.

---

## 🔒 The Four Contracts You MUST Respect

These are **mandatory**. Everything else in this README is "suggested".

### Contract 1 — The wire format (frozen)

A feed is a raw array of 40-byte `WireTick` records, little-endian, no header (see [`PIPELINE_SPEC.md`](./PIPELINE_SPEC.md) §2):

```text
offset 0  : uint64  timestamp_ns   (non-decreasing)
offset 8  : int64   bid_px_fp      (FIXED-POINT: real bid * 10000)
offset 16 : int64   ask_px_fp      (FIXED-POINT: real ask * 10000)
offset 24 : uint32  symbol_id       (0 .. 1023)
offset 28 : uint32  bid_qty         (> 0)
offset 32 : uint32  ask_qty         (> 0)
offset 36 : uint32  _reserved        (always zero — do not read)
```

Your producer **decodes** each record into a `csot::Tick`: `bid_px = bid_px_fp / 10000.0`, symbol id `k` interns to the string `"SYM<k>"`. Ticks are processed **in stream order** — the strategy is stateful, so your handoff must preserve order.

### Contract 2 — The pipeline ABI (frozen)

`WireTick`, `OrderRecord`, and `class Pipeline` are defined in [`include/pipeline.hpp`](./include/pipeline.hpp), with `static_assert`s on `sizeof(WireTick) == 40` and `sizeof(OrderRecord) == 48` (and the Week-1 `Tick`/`Order` canaries) as the canary. Your file must export exactly:

```cpp
extern "C" csot::Pipeline* create_pipeline();
```

The judge does the moral equivalent of:

```cpp
csot::Pipeline* p = create_pipeline();
p->on_init(num_symbols);                          // num_symbols == 1024
std::size_t k = p->run(in, n, out);               // <-- this call is timed; you fill out[0..k)
```

> ⚠️ **Why this matters from day one:** the judge builds your `pipeline.cpp` against *its own* `main()` and these headers. If your `run()` signature drifts — even a `const` — your submission won't link and the upload fails. Lock the ABI now.

### Contract 3 — The spec (frozen)

The order stream your `run()` produces is defined by [`PIPELINE_SPEC.md`](./PIPELINE_SPEC.md), which reuses the **unchanged** Week-1 z-score strategy + fill model ([`STRATEGY_SPEC.md`](../../week-1/project/STRATEGY_SPEC.md)).

You may optimize **how** you produce it:

- decode on a producer thread, strategize on a consumer thread
- a lock-free SPSC ring buffer (`alignas(64)` head/tail, release/acquire — no lock)
- pin both threads (`sched_setaffinity`), spin under back-pressure
- zero heap allocation after `on_init`
- Week-2 rolling sums to make the strategy stage cheap

You may not change **what** it produces:

- the decode rule, the z-score logic, the fill model, the empty-warmup window are fixed
- orders are emitted in tick order, at most one per tick
- the price is **copied** from the tick (BUY → ask, SELL → bid), never recomputed

> 🎯 **Leaderboard rule:** correctness is a hard gate, and so is **determinism**. The judge compares your order stream to the reference and **runs your `run()` several times**; if the stream differs from the reference, or differs *between your own runs* (a handoff race), the submission does not rank. Among correct, deterministic submissions, ranking is by wall-clock speed and throughput only.

### Contract 4 — The judge machine (frozen for the season)

Ranked submissions are **not** graded on your laptop. They run on the cohort's dedicated AWS EC2 judge:

| | |
|---|---|
| Instance | **`c7i.xlarge`** — **4 vCPU**, 8 GiB, Intel Sapphire Rapids |
| OS | Amazon Linux 2023 |
| Region | `us-east-1` (maintainer start/stop via `./judge-vm.sh` in the repo root) |

The judge downloads your `pipeline.cpp`, compiles it with **fixed flags** (`-std=c++20 -O3 -march=x86-64-v2 -pthread`, see [`CMakeLists.txt`](./CMakeLists.txt) `CSOT_JUDGE_BUILD`), runs it inside **bubblewrap** against the public then hidden feeds, diffs your order stream against the reference, and checks repeated runs agree. **Two hot threads on four cores** is your target — tune for that, not your 16-thread laptop.

> 📌 **Laptop numbers are for debugging only.** Reproduce the judge locally with `cmake -B build-judge -DCSOT_JUDGE_BUILD=ON` — but expect different absolute `run_ns` and a different core topology. The leaderboard denominators are captured on the `c7i.xlarge` box (`/etc/csot-judge/feed-reference.json`).

---

## 🎯 Learning Goals

By completing this project you will:

1. Have turned a serial decode-then-strategize into a real **two-stage pipeline** — producer, lock-free handoff, consumer — that overlaps two cores.
2. Have built a **lock-free SPSC ring buffer** from scratch: power-of-two capacity, cache-line-separated cursors, release/acquire publication, cached indices.
3. Have a `run()` that performs **zero heap allocation** after `on_init` and spawns its threads off the hot path.
4. Have **pinned** both threads, handled **back-pressure** with a spin, and **balanced** the stages so neither starves.
5. Have written, observed, and **fixed a handoff race**, and verified determinism under ThreadSanitizer and a repeat-run checksum.
6. Have driven a **stateful strategy over a huge feed** through a non-blocking channel and ranked on its wall-clock speed.
7. Have practised **programming to a frozen contract** again — a new ABI on top of the Week-1 one, the same discipline.

---

## 🏗️ What You Must Build

You deliver exactly **one file**: `pipeline.cpp`. Everything else is provided.

### 1. The pipeline ABI — **provided, copy as-is**

[`include/pipeline.hpp`](./include/pipeline.hpp) and [`include/strategy.hpp`](./include/strategy.hpp). Drop them into `include/` unchanged.

### 2. Your pipeline (`pipeline.cpp`) — *yours to write*

A concrete `csot::Pipeline` that implements [`PIPELINE_SPEC.md`](./PIPELINE_SPEC.md):

- allocate all state in `on_init(num_symbols)` (ring storage, per-symbol strategy state, interned `"SYM<id>"` names, thread bookkeeping)
- in `run()`, spawn a producer thread that decodes `in[0..n)` and pushes `csot::Tick`s across your SPSC ring; on the consumer, pop in order, run the z-score + fill model, and append each order to `out`
- export `extern "C" csot::Pipeline* create_pipeline() { return new YourPipeline(); }`

The point is **not** to invent a metric or a signal. The point is to produce the fixed order stream correctly, then make `run()` fast by overlapping the two stages. Start from the stub — it's already correct single-threaded:

```bash
cp samples/pipeline.stub.cpp pipeline.cpp
```

> 📌 **Single translation unit.** The judge compiles exactly this one `.cpp`. Your ring buffer, thread functions, padded cursor types, per-symbol state, and the interned name table all live inside it (use an anonymous `namespace`). No second `.cpp`, no `main()`, no custom CMake. **Threads, `<atomic>`, and `sched_setaffinity` ARE the point this week.** See [`TROUBLESHOOTING.md`](./TROUBLESHOOTING.md) → "The single-`.cpp` rule".

### 3. Sample data files

- [`data/tiny.feed`](./data/tiny.feed) — **provided**, small feed; expected order stream in [`data/tiny.orders.json`](./data/tiny.orders.json). Use for unit tests.
- `data/large.feed` — generate with `python3 data/gen_feed.py --accesses 5000000 --seed 42 --out data/large.feed`. Fast iteration + real overlap signal. **gitignored.**

> 💡 [`data/gen_feed.py`](./data/gen_feed.py) uses seed `42` by default — **everyone generates byte-identical feeds**, so your numbers compare directly to classmates' and the baseline.

### 4. A `README.md` for your project — *yours to write*

Build steps, your hardware (`nproc` / `lscpu`), and your headline number: **throughput (M ticks/s)** and `run()` wall-clock on `data/large.feed`, the **speedup vs. the single-threaded baseline**, plus confirmation that your order stream matches `data/tiny.orders.json` and is deterministic across runs.

---

## 📁 Recommended Directory Layout

```
project/
├── CMakeLists.txt             ← ★ copy from this folder
├── .gitignore                 ← ★ copy from this folder
├── README.md                  ← your own writeup
├── PIPELINE_SPEC.md           ← ★ copy/read: the frozen pipeline spec
├── TROUBLESHOOTING.md         ← ★ copy from this folder
├── include/
│   ├── pipeline.hpp           ← ★ copy from this folder unchanged
│   └── strategy.hpp           ← ★ copy from this folder unchanged (frozen Week-1 ABI)
├── harness/
│   └── main.cpp               ← ★ local runner (you never upload it)
├── pipeline.cpp               ← yours; implements PIPELINE_SPEC.md + exports create_pipeline()
├── samples/
│   └── pipeline.stub.cpp      ← ★ correct single-threaded starting point (copy to pipeline.cpp)
├── data/
│   ├── gen_feed.py            ← ★ copy from this folder unchanged
│   ├── tiny.feed              ← ★ committed golden feed
│   ├── tiny.orders.json       ← ★ committed golden order stream
│   └── large.feed             ← gitignored, generated by gen_feed.py
└── tools/
    └── plot_latency.py        ← optional
```

Files marked ★ are shipped — copy them verbatim. `pipeline.cpp` is yours.

---

## 🪜 Suggested Implementation Order

Each step should compile and run before moving on.

### Step 1 — Skeleton build (Day 1, ~30 min)

- Copy the ★ files into place. Confirm the headers compile: `g++ -std=c++20 -Iinclude -c include/pipeline.hpp -o /tmp/x.o` is silent.
- `cmake -B build && cmake --build build -j` builds `pipeline_runner` from the stub.
- `diff <(./build/pipeline_runner data/tiny.feed 2>/dev/null) data/tiny.orders.json` is clean — the stub is correct single-threaded.

### Step 2 — A real feed (Day 1, ~30 min)

- `python3 data/gen_feed.py --accesses 5000000 --seed 42 --out data/large.feed`.
- Run the stub on it; record the single-threaded throughput as your **baseline** to beat: \_\_\_\_\_ M ticks/s.
- `python3 data/gen_feed.py --dump data/tiny.feed` and skim it against `PIPELINE_SPEC.md` §2/§4.

### Step 3 — Build the ring (Day 2, ~2 hours)

- Write your lock-free SPSC ring buffer ([`04-spsc-ring-buffer.md`](../04-spsc-ring-buffer.md)). **Unit-test it standalone first** (push 0..1e7 on one thread, sum on another — Phase 2 of the week checklist) before wiring it into the pipeline. Get the orderings and the full/empty check right here, in isolation.

### Step 4 — Split the pipeline (Day 3, ~2 hours)

- `cp samples/pipeline.stub.cpp pipeline.cpp`. Move the decode onto a producer thread that pushes `csot::Tick`s; run the strategy on the consumer thread, popping in order ([`05-pipeline-pinning-and-backpressure.md`](../05-pipeline-pinning-and-backpressure.md)).
- Build against your file: `cmake -B build -DCSOT_PIPE_SRC=pipeline.cpp && cmake --build build -j`. Re-`diff` against `tiny.orders.json` — still clean.

### Step 5 — Kill false sharing + cache indices (Day 4, ~1.5 hours)

- `alignas(64)` your head and tail cursors; cache the other side's index and refresh only when it looks full/empty ([`04`](../04-spsc-ring-buffer.md) §2, §5). Re-measure throughput; the curve should climb past serial.

### Step 6 — Pin + balance (Day 5, ~1.5 hours)

- `sched_setaffinity` both threads to distinct cores ([`05`](../05-pipeline-pinning-and-backpressure.md)). Bring back **Week-2 rolling sums** so the strategy stage is cheap and `D ≈ S`. Capture headline numbers:
  - `run()` wall-clock on `large.feed`: \_\_\_\_\_ ms · throughput \_\_\_\_\_ M ticks/s · speedup vs. serial \_\_\_\_\_×

### Step 7 — Prove it (Day 6, ~1.5 hours)

- Run `run()` 10× in a loop; assert the order-stream checksum is identical every time. Build under `-DENABLE_TSAN=ON` and confirm a clean report.
- `perf stat -e cache-misses,context-switches,instructions` — capture the before/after of your false-sharing fix and your pinning.

### Step 8 — Write it up (Day 7, ~30 min)

- Project `README.md`: build steps, your hardware, headline throughput + speedup, a `perf stat` snippet, and **one thing that surprised you**.

That last bullet is the most important. It's the start of your lock-free intuition.

---

## 🧪 Hands-on practice alongside the project

The ranked deliverable is **`pipeline.cpp` only**. These exercises are **not submitted** — they make the friction visceral before you hit it in the project.

### The handshake and the false-sharing demos

Both live in the topic files and take ten minutes each:

- The **release/acquire handshake** ([`02-atomics-and-the-memory-model.md`](../02-atomics-and-the-memory-model.md) §4): publish `payload` then `ready`; break it with `relaxed` and try to catch a stale read.
- The **packed vs padded cursors** demo ([`04-spsc-ring-buffer.md`](../04-spsc-ring-buffer.md) §2): the same push/pop loop with head/tail sharing a line vs. `alignas(64)` apart. `perf stat` both.

> 💡 Do these **before** Step 4. When you've personally watched two threads crawl because their cursors shared a cache line, padding them stops feeling optional and starts feeling inevitable.

---

## 🚀 Stretch Goals (Optional, for the Eager)

1. **Batch** the publish (one `release` per K items) and the drain; measure the throughput gain and the latency cost.
2. Sweep ring **capacity** (16 → 1M); plot throughput; find where it falls out of cache.
3. Pin to **physical cores vs. hyperthread siblings**; quantify the difference (`lscpu`, `thread_siblings_list`).
4. Build a **mutex queue** and your **lock-free SPSC**; plot both vs. the serial baseline with [`tools/plot_latency.py`](./tools/plot_latency.py).
5. **Prefetch** the input feed a few records ahead of the decode; sweep the distance.
6. Read the **LMAX Disruptor** paper; map each idea onto your ring ([`06-bonus-disruptor-and-beyond.md`](../06-bonus-disruptor-and-beyond.md)).
7. Build with `-DCSOT_JUDGE_BUILD=ON` and compare to your `-march=native` numbers. How much was the ISA worth?

---

## 🏆 The Live Leaderboard

**The leaderboard is live: [csot-low-latency.devclub.in](https://csot-low-latency.devclub.in/).** Week 4 swaps the active ranked challenge from the Week-3 aggregator to the **lock-free pipeline** — a fresh board whose ceiling is the overlap of two cores through a non-blocking handoff, so the top is not saturated on day one.

### How it works (you upload source, the judge builds it)

1. Sign in with your DevClub IITD identity.
2. On the dashboard, upload a **single `pipeline.cpp`** (not a compiled `.so`).
3. The judge **builds your file itself** with a fixed, portable toolchain (`-O3 -march=x86-64-v2 -pthread`, `CSOT_JUDGE_BUILD=ON`) against its own `main()` and headers, inside a sandbox.
4. It runs your `run()` against **two feeds**, diffs the order stream against the reference, **runs it several times to confirm determinism**, and — if correct and deterministic — ranks you by wall-clock speed.

Uploading source means everyone is measured on the **same compiler and flags**, so the board reflects *your code*, not your laptop's CPU.

### The two feeds

| Feed | Ticks | Seed | Reproducible? | Role |
|---|---:|---:|---|---|
| **Public** | 5 000 000 | `42` (the `gen_feed.py` default) | ✅ Yes — `python3 data/gen_feed.py --accesses 5000000 --seed 42 --out public.feed` | Correctness smoke test. Fail here and you stop. |
| **Hidden** | very large | sealed | ❌ No — held out on the judge box | The speed ranking happens here. |

Debug your `incorrect` runs locally on the reproducible public feed before bothering the judge; the hidden feed forces your pipeline to generalize (more symbols active, longer sustained pressure on your ring).

### Correctness and determinism gate everything

- Every order must match the reference exactly, in order. One disagreement → `incorrect`, no rank.
- Your `run()` must return the **same order stream on repeated runs**. A handoff race that "passes sometimes" is flagged non-deterministic → no rank. (This is the multithreading-specific gate; see [`03-data-races.md`](../../week-3/03-data-races.md) and [`03-memory-orderings.md`](../03-memory-orderings.md).)
- If both feeds pass and runs agree, the judge ranks by **wall-clock `run()` time** (and throughput). Going wider than two hot threads buys nothing here; correctness and determinism are never traded for speed.

### Submission policy

- **Single `pipeline.cpp`**, ≤ 1 MiB, must export `extern "C" csot::Pipeline* create_pipeline()`.
- One identity, one entry. No teams.
- Cooldown + daily quota enforced server-side (the dashboard shows the window; spamming returns `429`).
- The judge VM may be stopped between sessions to save cost — submissions queue and drain when it's back.

### What you do NOT have to do this week

You don't have to top the board. Upload when your pipeline is **correct on `tiny.feed`, deterministic, and faster than the single-threaded baseline**. Beating the reference's pipelined time is the game for the eager.

---

## ❓ FAQ

**Q: Why a pipeline? I came here for trading.**
A: A real market-data system *is* a pipeline — one thread takes ticks off the wire, another runs the strategy, decoupled so I/O jitter never stalls the logic. This week the producer decodes an in-memory feed; in Week 5 it reads the network. The strategy is the unchanged Week-1 z-score rule. The leaderboard still measures the same thing: systems-engineering speed.

**Q: Can I submit a `.so` like Week 1?**
A: No. Like Weeks 2–3, the judge takes a single `pipeline.cpp` and builds it itself, so the field is identical for everyone.

**Q: Can I `#include <atomic>` / `<thread>` / pin with `sched_setaffinity`?**
A: **Yes — that's the whole point this week.** Networking, processes (`fork`/`exec`/`system`), and file I/O are still blocked by the sandbox.

**Q: My pipeline is correct on `tiny.feed` but the judge says non-deterministic.**
A: You have a handoff race — usually a `relaxed` store where you needed `release`, or a full/empty off-by-one that drops a tick. Get the orderings right ([`03`](../03-memory-orderings.md)) and unit-test the ring standalone. Catch it with `-DENABLE_TSAN=ON`.

**Q: Two threads is no faster than one — or slower.**
A: Either you used a mutex (replace it with the lock-free ring), your head/tail false-share (pad them), or your stages are unbalanced (use Week-2 rolling sums to shrink the strategy stage). See [`05`](../05-pipeline-pinning-and-backpressure.md) and `TROUBLESHOOTING.md` → Performance.

**Q: My orders are right but a few are missing or out of order.**
A: Your ring isn't order-preserving or it drops under back-pressure. SPSC is FIFO by construction; full means *wait*, never overwrite. See [`04`](../04-spsc-ring-buffer.md).

**Q: My prices are off by a rounding.**
A: You recomputed the price. Copy `bid_px`/`ask_px` from the decoded tick (PIPELINE_SPEC.md §6); the harness re-encodes the fixed-point integer.

---

## 📤 Submission

### 1. The Git repo (for cohort review)

A public repo with your `pipeline.cpp`, project `README.md` (headline numbers + speedup), the shipped `gen_feed.py`, and a `.gitignore` for `build/` and `*.feed` (keep the golden `tiny.feed`).

### 2. The leaderboard (for the live ranking)

Sign in at **[csot-low-latency.devclub.in](https://csot-low-latency.devclub.in/)** and upload your `pipeline.cpp` from the dashboard. The judge builds it, runs it (several times), and reports back the order-stream check, throughput, and a rank.

---

## 🎉 You Made It

You took a serial decode-then-strategize and split it across two cores that hand work to each other through a queue with no lock, no allocation, and no data race — the exact pattern every real low-latency system uses to keep I/O off the hot path. **A correct, lock-free hand-off is the skill the whole field is built on** — and the I/O thread you just decoupled is precisely where Week 5 plugs in the network.

See you in Week 5. ⚡

(Week 5 is coming soon — a TCP/UDP feed and an `epoll` event loop feeding *this same* SPSC ring into *this same* strategy consumer, so the wire never touches the strategy's hot path.)
