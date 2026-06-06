# Troubleshooting — Week 2 Cache Simulator

The common ways to get stuck on the cache-sim challenge, and how to unstick yourself. Skim once now, come back when you hit something. The Week-1 [`TROUBLESHOOTING.md`](../../week-1/project/TROUBLESHOOTING.md) still covers the toolchain, `perf`, Google Benchmark, sanitizers, and platform gotchas — this file adds the Week-2-specific ones.

> 🆘 If your problem isn't here, drop the full error in the CSoT group along with **your OS, compiler version, and the exact command you ran**.

---

## 🖥️ The judge VM (not your laptop)

Ranked uploads are built and run on the cohort EC2 judge — **`c7i.xlarge`** (4 vCPU, 8 GiB, Amazon Linux 2023, `us-east-1`). The portal queues work there; your dashboard numbers come from that box.

- **Reproduce the toolchain locally:** `cmake -B build-judge -DCSOT_JUDGE_BUILD=ON -DCSOT_CACHE_SIM_SRC=cache_sim.cpp && cmake --build build-judge -j`
- **Absolute `run_ns` will differ** between your CPU and the judge — ranking uses denominators captured on the judge silicon.
- **Between cohort sessions** the maintainer may stop the instance to save cost; submissions stay `queued` and drain when it is back (`JudgePill` on the dashboard).

---

## 🏗️ Build & Toolchain

### `fatal error: cache_sim.hpp: No such file or directory`

Your include path is wrong. Build through the shipped `CMakeLists.txt` (it adds `include/` for you), or pass `-Iinclude` to a raw `g++`. Copy `include/cache_sim.hpp` verbatim from this folder — don't move it.

### `static assertion failed: MemAccess layout is part of the ABI`

You edited `cache_sim.hpp`. Re-copy it unchanged. The `static_assert`s on `sizeof(MemAccess) == 16` and `sizeof(CacheStats) == 56` are the canary: the judge mmaps the trace straight into a `MemAccess[]`, so the layout is not negotiable.

### `cmake` builds the stub, not my code

By default `CSOT_CACHE_SIM_SRC` points at `samples/cache_sim.stub.cpp`. Point it at your file:

```bash
cmake -B build -DCSOT_CACHE_SIM_SRC=cache_sim.cpp
cmake --build build -j
```

### My upload `rejected` with `runner exit 132`

Same story as Week 1: signal 4 (`SIGILL`) — your binary used CPU instructions the judge box doesn't have, almost always from `-march=native`. The judge builds your `.cpp` itself with a portable baseline (`-march=x86-64-v2`), so this should not happen on the official path — but if you self-host a build or compare locally, reproduce the judge with:

```bash
cmake -B build-judge -DCSOT_JUDGE_BUILD=ON -DCSOT_CACHE_SIM_SRC=cache_sim.cpp
cmake --build build-judge -j
```

See [`../../week-1/05-bonus-compiler.md`](../../week-1/05-bonus-compiler.md) §2.

---

## 🧱 The single-`.cpp` rule

### "Can I split my simulator across files?"

No. The judge compiles exactly one translation unit — your `cache_sim.cpp` — against its own `main()` and `cache_sim.hpp`. Helpers, templates, and tables must all live inside that one file (an anonymous `namespace` is your friend). If your local build links extra `.cpp`s, it will pass locally and fail on upload.

### "Can I add a `main()`?"

No. `main()` is judge-owned (locally it's `harness/main.cpp`). Your file provides `create_cache_sim()` and nothing with external linkage besides that factory. A stray `main` will collide at link time.

### "Can I `#include <thread>` / open files / call `system()`?"

The hot path is a single-threaded `run()` over an in-memory array; you do not need threads, I/O, or syscalls, and the compile sandbox forbids many of them. Keep it to standard headers and computation.

---

## 🧮 Correctness (the seven counters)

### "`l1_hits + l1_misses` doesn't equal `reads + writes`"

Every access probes L1 exactly once, so those must be equal. If they aren't, you're double-counting (e.g. incrementing a miss counter and then falling through into the hit path) or skipping the warm-up of the first accesses. Re-read [`CACHE_SPEC.md`](./CACHE_SPEC.md) §5.

### "`l2_hits + l2_misses` is larger than `l1_misses`"

L2 is consulted **only on an L1 miss**, and **writebacks do not count as L2 accesses**. If your L1→L2 dirty writeback is bumping `l2_hits`/`l2_misses`, that's the bug — a writeback updates L2's dirty bit (or allocates silently), but it is not a demand access. See `CACHE_SPEC.md` §5.4 and §6.

### "My counts are close but off by a handful"

Three classic culprits, in order of likelihood:

1. **LRU update on the wrong events.** LRU recency is updated on **demand hits and demand fills only** — never on a writeback. Updating it on a writeback changes which line is evicted next and drifts every later count.
2. **Write-allocate vs no-allocate.** A write miss must **allocate** the line (fetch then mark dirty), not skip the cache. Both levels are write-allocate.
3. **Dirty-bit propagation.** A line is dirty if it was ever written while resident. On eviction, a dirty L1 victim writes back to L2; only a dirty **L2** victim leaving for memory increments `dirty_writebacks`.

### "I pass `tiny.trace` but fail the hidden trace"

`tiny.trace` has four symbols' worth of locality and only exercises a few sets. The hidden trace touches many sets, forces real evictions, and mixes sequential / strided / pointer-chase / random regions. Bugs that only bite under eviction pressure (LRU tie-breaks, writeback-allocate causing an L2 eviction) won't show on the tiny file. Generate a bigger local trace and stress it:

```bash
python3 data/gen_trace.py --accesses 5000000 --seed 42 --out data/large.trace
./build/cache_sim_runner data/large.trace
```

### "How do I hand-check `tiny.trace`?"

`data/tiny.trace` is small on purpose. Dump it as text and walk it against `CACHE_SPEC.md` §5 by hand; the expected counters are in `data/tiny.stats.json`:

```bash
python3 data/gen_trace.py --dump data/tiny.trace
cat data/tiny.stats.json
```

---

## ⚡ Performance

### "My `run()` is correct but slow"

Expected — correctness first, speed second. The headroom is large and most of it is in this list:

- **Layout.** `std::unordered_map`-of-sets is the slow trap. Use flat, contiguous SoA arrays indexed by set (Week-1 lesson, [`03-locality.md`](../03-locality.md)).
- **Allocation.** Zero heap in `run()`; size everything in `on_init()` ([`04-zero-allocation.md`](./../04-zero-allocation.md)).
- **Compile-time geometry.** Fold the index/offset masks and way count into `constexpr` so the compiler can specialize the loop ([`05-compile-time-and-static-polymorphism.md`](../05-compile-time-and-static-polymorphism.md)).
- **Branches and tags.** A branchless / SIMD tag scan over 8 ways, and a prefetch of the next access's set, are the bonus-tier wins ([`06-bonus-simd-and-prefetch.md`](../06-bonus-simd-and-prefetch.md)).

### "Throughput swings 20–50% between runs"

Benchmark hygiene, same as Week 1: pin a core (`taskset -c 3`), lock the frequency governor, disable turbo, and close background apps. See the Week-1 [`TROUBLESHOOTING.md`](../../week-1/project/TROUBLESHOOTING.md) → Google Benchmark section.

### "perf shows most time in `run()` but no detail"

Build with frame pointers (the shipped CMake keeps `-fno-omit-frame-pointer`) and record with DWARF call graphs:

```bash
perf record -F 999 -g --call-graph dwarf ./build/cache_sim_runner data/large.trace
perf report
```

---

## 🐍 Trace generator (`gen_trace.py`)

### "Two students get different traces from the same seed"

The seed only guarantees identical output for **identical arguments and unchanged source**. Check `--accesses`, `--seed`, and that nobody edited `gen_trace.py` (`diff` against the upstream copy).

### "`cache_sim_runner` says the trace size isn't a multiple of 16"

The trace is a raw array of 16-byte `MemAccess` records. A truncated or text-edited file breaks that invariant. Regenerate it; never open `.trace` files in a text editor and save.

---

## 🆘 When Nothing Works

Start from the golden file:

```bash
./build/cache_sim_runner data/tiny.trace
diff <(./build/cache_sim_runner data/tiny.trace 2>/dev/null) data/tiny.stats.json
```

If even the 20-ish-access `tiny.trace` produces wrong counters, the bug is in your per-access logic, not your performance work. Then the standard escalation:

1. Reproduce on the smallest input (`tiny.trace`).
2. Re-run under `-fsanitize=address,undefined` (`-DENABLE_SANITIZERS=ON`).
3. Add a debug mode that prints `(access, l1 hit/miss, l2 hit/miss, evicted tag, dirty?)` per access and diff against your hand-trace of `CACHE_SPEC.md` §5.
4. If still stuck, post the full command, full output, your OS, and the smallest reproducing trace to the CSoT group.

You'll get unstuck. Everyone does. 🚀
