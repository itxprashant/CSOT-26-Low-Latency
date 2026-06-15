# Troubleshooting — Week 3 Parallel Aggregator

The common ways to get stuck on the aggregator challenge, and how to unstick yourself. Skim once now, come back when you hit something. The Week-1 [`TROUBLESHOOTING.md`](../../week-1/project/TROUBLESHOOTING.md) still covers the toolchain, `perf`, Google Benchmark, and sanitizers; the Week-2 [`TROUBLESHOOTING.md`](../../week-2/project/TROUBLESHOOTING.md) covers the single-`.cpp` / judge-compile model. This file adds the Week-3 threading-specific ones.

> 🆘 If your problem isn't here, drop the full error in the CSoT group along with **your OS, compiler version, `nproc`, and the exact command you ran**.

---

## 🖥️ The judge VM (not your laptop)

Ranked uploads are built and run on the cohort EC2 judge — **`c7i.xlarge`** (4 vCPU, 8 GiB, Amazon Linux 2023, `us-east-1`). The portal queues work there; your dashboard numbers come from that box.

- **Four cores.** The judge box has 4 vCPUs. Your reduction is timed there, so tune your thread count and pinning for 4 cores, not your 16-thread laptop.
- **Reproduce the toolchain locally:** `cmake -B build-judge -DCSOT_JUDGE_BUILD=ON -DCSOT_AGG_SRC=aggregator.cpp && cmake --build build-judge -j`
- **Absolute `run_ns` will differ** between your CPU and the judge — ranking uses denominators captured on the judge silicon.
- **Between cohort sessions** the maintainer may stop the instance to save cost; submissions stay `queued` and drain when it is back (`JudgePill` on the dashboard).

---

## 🏗️ Build & Toolchain

### `fatal error: aggregate.hpp: No such file or directory`

Your include path is wrong. Build through the shipped `CMakeLists.txt` (it adds `include/` for you), or pass `-Iinclude` to a raw `g++`. Copy `include/aggregate.hpp` verbatim from this folder — don't move it.

### `static assertion failed: AggTick layout is part of the ABI`

You edited `aggregate.hpp`. Re-copy it unchanged. The `static_assert`s on `sizeof(AggTick) == 32` and `sizeof(SymbolAgg) == 40` are the canary: the judge mmaps the stream straight into an `AggTick[]` and diffs your `SymbolAgg[]` rows, so the layouts are not negotiable.

### `undefined reference to pthread_create` / `std::thread` link errors

You're building without the threading library. Use the shipped `CMakeLists.txt` (it does `find_package(Threads)` and links `Threads::Threads`); for a raw `g++` add `-pthread`. The judge always compiles with `-pthread`.

### `cmake` builds the stub, not my code

By default `CSOT_AGG_SRC` points at `samples/aggregator.stub.cpp`. Point it at your file:

```bash
cmake -B build -DCSOT_AGG_SRC=aggregator.cpp
cmake --build build -j
```

---

## 🧱 The single-`.cpp` rule (threads are allowed this week)

### "Can I split my aggregator across files?"

No. The judge compiles exactly one translation unit — your `aggregator.cpp` — against its own `main()` and `aggregate.hpp`. Helpers, thread functions, padded-table types, and tables must all live inside that one file (an anonymous `namespace` is your friend). If your local build links extra `.cpp`s, it will pass locally and fail on upload.

### "Can I `#include <thread>` / `<atomic>` / call `sched_setaffinity`?"

**Yes — this is the week you do.** Week 2 told you to keep `run()` single-threaded; Week 3 is the opposite. `<thread>`, `<atomic>`, `<mutex>`, `pthread`, and `sched_setaffinity` are all allowed and expected. What is still forbidden (and blocked by the sandbox) is networking, spawning processes (`fork`/`exec`/`system`/`popen`), and file I/O — you get the stream as an in-memory array and return a table; that's it.

### "Can I add a `main()`?"

No. `main()` is judge-owned (locally it's `harness/main.cpp`). Your file provides `create_aggregator()` and nothing with external linkage besides that factory.

---

## 🧮 Correctness & determinism

### "I pass `tiny.ticks`, then my parallel version disagrees with itself"

That is a **data race** — the lesson of the week, caught in the act. Two threads are updating shared state without synchronization (almost always a single shared result table). The fix is structural, not a lock: give each thread its **own** partial table, then merge once at the end (AGG_SPEC.md §7). Confirm with ThreadSanitizer:

```bash
cmake -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DENABLE_TSAN=ON -DCSOT_AGG_SRC=aggregator.cpp
cmake --build build-tsan -j
./build-tsan/agg_runner data/tiny.ticks      # TSan prints the racing accesses
```

The judge runs your submission several times and **rejects it if the table changes between runs** — a race that "passes sometimes" still fails. See [`03-data-races.md`](../03-data-races.md).

### "My counts are right but min/max is 0 for some symbols"

You merged an **empty partial** into a real one. A partition that saw no ticks for a symbol has `count == 0` and a canonical `min_price/max_price` of `0` — if you `min(0, real_price)` you clamp to 0. Guard the merge on `count` (AGG_SPEC.md §7), or seed accumulators with `INT64_MAX`/`INT64_MIN` and collapse still-empty rows to canonical zeros only at the very end.

### "`Σ count` doesn't equal n"

You either dropped ticks at a partition boundary (off-by-one when splitting `[0, n)` into chunks) or double-counted an overlap. Each tick must land in exactly one thread's chunk. Print `Σ_s out[s].count` and compare to `n`.

### "How do I hand-check `tiny.ticks`?"

It's 10 ticks over symbols 0–3. Dump it and check against AGG_SPEC.md §5; the expected table is in `data/tiny.agg.json`:

```bash
python3 data/gen_ticks.py --dump data/tiny.ticks
python3 data/gen_ticks.py --stats data/tiny.ticks      # the reference table JSON
diff <(./build/agg_runner data/tiny.ticks 2>/dev/null) data/tiny.agg.json
```

A clean `diff` means your table (and its checksum over all 1024 rows) matches.

---

## ⚡ Performance

### "4 threads is barely faster than 1 — or slower!"

The classic Week-3 trap, and it's almost always **false sharing**. If your per-thread partial tables sit next to each other in one array (`partials[thread_id][symbol]`), neighbouring threads share 64-byte cache lines and ping-pong them on every update. Pad/align each thread's table to a cache line:

```cpp
struct alignas(64) PaddedTable { csot::SymbolAgg rows[NUM_SYMBOLS]; };
```

Re-measure with the scaling sweep and you should see the curve turn from flat to climbing. See [`04-false-sharing.md`](../04-false-sharing.md) and `tools/plot_scaling.py`.

### "Throughput swings wildly between runs"

Two causes stack this week:

1. **Unpinned threads.** The scheduler migrates threads across cores mid-reduction, trashing per-core caches. Pin each worker to a distinct core with `sched_setaffinity` ([`05-scheduler-and-pinning.md`](../05-scheduler-and-pinning.md)).
2. **Benchmark hygiene** (same as Week 1): lock the frequency governor, disable turbo, close background apps, and don't oversubscribe the 4 cores.

### "I scaled to 4× on my laptop but the board barely moved"

You're probably **memory-bandwidth-bound**: the reduction reads the whole stream once and does little arithmetic per byte, so beyond a couple of threads you're waiting on DRAM, not compute. That's the real ceiling — the wins shift to streaming the data efficiently (prefetch, fewer passes, compact access) rather than adding threads. See [`06-bonus-bandwidth-and-pools.md`](../06-bonus-bandwidth-and-pools.md).

### "perf shows time in `run()` but no thread detail"

Record with call graphs and per-thread view:

```bash
perf record -F 999 -g --call-graph dwarf ./build/agg_runner data/large.ticks
perf report
perf stat -e cache-misses,LLC-load-misses,context-switches ./build/agg_runner data/large.ticks
```

A false-sharing implementation lights up `cache-misses` and `context-switches`; a clean one does not.

---

## 🐍 Stream generator (`gen_ticks.py`)

### "Two students get different streams from the same seed"

The seed only guarantees identical output for **identical arguments and unchanged source**. Check `--accesses`, `--seed`, and that nobody edited `gen_ticks.py` (`diff` against the upstream copy).

### "`agg_runner` says the stream size isn't a multiple of 32"

The stream is a raw array of 32-byte `AggTick` records. A truncated or text-edited file breaks that invariant. Regenerate it; never open `.ticks` files in a text editor and save.

---

## 🆘 When Nothing Works

Start from the golden file, single-threaded:

```bash
diff <(./build/agg_runner data/tiny.ticks 2>/dev/null) data/tiny.agg.json
```

If even the 10-tick `tiny.ticks` produces a wrong table, the bug is in your per-symbol logic, not your threading. Then the standard escalation:

1. Reproduce on the smallest input (`tiny.ticks`), single-threaded.
2. Re-run under ThreadSanitizer (`-DENABLE_TSAN=ON`) — it names the racing lines.
3. Run your parallel `run()` 10 times in a loop; if the checksum changes, you have a race even if TSan is quiet on a tiny input.
4. Re-run under `-fsanitize=address,undefined` (`-DENABLE_SANITIZERS=ON`) for out-of-bounds partial-table indexing.
5. If still stuck, post the full command, full output, your OS, `nproc`, and the smallest reproducing stream to the CSoT group.

You'll get unstuck. Everyone does. 🚀
