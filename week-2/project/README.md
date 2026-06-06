# Week 2 Project — Fastest Correct Cache Simulator

> **Mission:** Implement a **single `cache_sim.cpp`** that simulates a fixed two-level cache hierarchy over a huge memory trace, emits the exact reference counters, and does it **as fast as wall-clock allows**. Week 1 taught you to measure a hot loop; Week 2 is where you make one disappear into the cache. Same "fastest correct implementation" game, brand-new kernel — one with far more room to optimize: struct-of-arrays state, branchless set lookup, compile-time geometry, prefetch, SIMD tag compares.

---

## 📦 What This Folder Gives You

Copy the ★ files verbatim. They remove the tedious, error-prone scaffolding so you can focus on the one file that is *yours*: the simulator.

| File | Purpose | Status |
|---|---|---|
| [`include/cache_sim.hpp`](./include/cache_sim.hpp) | The **frozen ABI** — `MemAccess`, `CacheStats`, `CacheSim`, `create_cache_sim()`. | ★ **Do not modify.** |
| [`CACHE_SPEC.md`](./CACHE_SPEC.md) | The **frozen cache hierarchy** every submission must simulate (geometry, LRU, write-back, write-allocate, exact counter semantics). | ★ **Do not change.** Faster implementations only. |
| [`samples/cache_sim.stub.cpp`](./samples/cache_sim.stub.cpp) | A compiling skeleton wired to the ABI, with TODOs. **Not** a correct simulator. | ★ Copy to `cache_sim.cpp` and fill in. |
| [`harness/main.cpp`](./harness/main.cpp) | Local runner: loads a trace, times your `run()`, prints `CacheStats` as JSON + throughput. Mirrors the judge. | ★ Use as-is (you never upload it). |
| [`CMakeLists.txt`](./CMakeLists.txt) | Build template (Release/Debug, sanitizers, LTO, `CSOT_JUDGE_BUILD`, `CSOT_CACHE_SIM_SRC`). | ★ Use as-is. |
| [`.gitignore`](./.gitignore) | Build, trace, perf, editor noise. | ★ Use as-is. |
| [`data/gen_trace.py`](./data/gen_trace.py) | Seeded mixed-locality trace generator. `--tiny` and `--dump` modes. | ★ Default seed is the cohort baseline. |
| [`data/tiny.trace`](./data/tiny.trace) + [`data/tiny.stats.json`](./data/tiny.stats.json) | A 14-access hand-checkable golden trace and its exact reference counters. | ★ Use for unit-testing your simulator. |
| [`TROUBLESHOOTING.md`](./TROUBLESHOOTING.md) | Cache-sim and single-`.cpp` gotchas, plus a pointer back to Week 1's toolchain section. | ★ Skim once, refer back when stuck. |
| [`tools/plot_cache.py`](./tools/plot_cache.py) | Optional: bar-chart hit rates from a stats JSON. | Optional. |

Everything inside `run()` — your data layout, your tag scan, your allocation strategy — is yours to design.

---

## 🔒 The Three Contracts You MUST Respect

These are **mandatory**. Everything else in this README is "suggested".

### Contract 1 — The trace format (frozen)

A trace is a raw array of 16-byte `MemAccess` records, little-endian, no header (see [`CACHE_SPEC.md`](./CACHE_SPEC.md) §2):

```text
offset 0 : uint64  address    (byte address, little-endian)
offset 8 : uint8   is_write   (0 = read, 1 = write)
offset 9 : uint8   pad[7]     (always zero — do not read)
```

Guarantees: file size is a multiple of 16; `is_write ∈ {0,1}`; addresses are arbitrary 64-bit byte addresses (need not be aligned — only `address >> 6`, the block address, matters); accesses are processed strictly in file order.

### Contract 2 — The cache-sim ABI (frozen)

`MemAccess`, `CacheStats`, and `class CacheSim` are defined in [`include/cache_sim.hpp`](./include/cache_sim.hpp), with `static_assert`s on `sizeof(MemAccess) == 16` and `sizeof(CacheStats) == 56` as the canary. Your file must export exactly:

```cpp
extern "C" csot::CacheSim* create_cache_sim();
```

The judge does the moral equivalent of:

```cpp
csot::CacheSim* sim = create_cache_sim();
sim->on_init();
csot::CacheStats got = sim->run(trace, n);   // <-- this call is timed
```

> ⚠️ **Why this matters from day one:** the judge builds your `cache_sim.cpp` against *its own* `main()` and this header. If your `run()` signature drifts — even a `const` — your submission won't link and the upload fails. Lock the ABI now.

### Contract 3 — The cache spec (frozen)

The hierarchy your `run()` simulates is defined in [`CACHE_SPEC.md`](./CACHE_SPEC.md): a two-level, set-associative, **true-LRU**, **write-back + write-allocate**, non-inclusive cache (L1 32 KiB 8-way, L2 256 KiB 8-way, 64-byte lines).

You may optimize **how** you implement it:

- flat SoA arrays instead of maps
- branchless / SIMD tag scan across the 8 ways
- compile-time-folded index/offset masks (`constexpr`)
- zero heap allocation after `on_init`
- prefetch the next access's set

You may not change **what** it computes:

- geometry, associativity, and line size are fixed
- replacement is exact LRU (no pseudo-LRU — it changes the counters)
- write policy is write-back + write-allocate at both levels
- the seven `CacheStats` counters and their semantics are fixed

> 🎯 **Leaderboard rule:** correctness is a hard gate. The judge compares your emitted `CacheStats` to the reference simulator. If any of the seven counters differ, the submission is incorrect. Among correct submissions, ranking is by wall-clock speed and throughput only.

### Contract 4 — The judge machine (frozen for the season)

Ranked submissions are **not** graded on your laptop. They run on the cohort's dedicated AWS EC2 judge:

| | |
|---|---|
| Instance | **`c7i.xlarge`** — 4 vCPU, 8 GiB, Intel Sapphire Rapids |
| OS | Amazon Linux 2023 |
| Region | `us-east-1` (maintainer start/stop via `./judge-vm.sh` in the repo root) |

The judge downloads your `cache_sim.cpp`, compiles it with **fixed flags** (`-std=c++20 -O3 -march=x86-64-v2`, see [`CMakeLists.txt`](./CMakeLists.txt) `CSOT_JUDGE_BUILD`), runs it inside **bubblewrap** against the public then hidden traces, and diffs your seven counters against the reference simulator. Wall-clock `run()` on the hidden trace is what moves rank.

> 📌 **Laptop numbers are for debugging only.** Reproduce the judge locally with `cmake -B build-judge -DCSOT_JUDGE_BUILD=ON` — but expect different absolute `run_ns` on different silicon. The leaderboard denominators are captured on the `c7i.xlarge` box (`/etc/csot-judge/cache-reference.json`).

---

## 🎯 Learning Goals

By completing this project you will:

1. Have **simulated a cache hierarchy** by hand and in code — lines, sets, ways, LRU, write-back, write-allocate (the COL216 core, applied).
2. Have a `run()` hot path that performs **zero heap allocation** after `on_init`.
3. Have laid out your simulator's own state as **struct-of-arrays** so *your* code doesn't thrash *your* cache.
4. Have folded geometry into **compile-time constants** and seen the asm shrink.
5. Have measured the difference between a `std::unordered_map` simulator and a flat-array one — and felt the 10×.
6. Have driven a **single hot loop over hundreds of millions of records** and ranked on its wall-clock speed.
7. Have practised **programming to a frozen contract** again — a different ABI, the same discipline.

---

## 🏗️ What You Must Build

You deliver exactly **one file**: `cache_sim.cpp`. Everything else is provided.

### 1. The cache-sim ABI — **provided, copy as-is**

[`include/cache_sim.hpp`](./include/cache_sim.hpp). Drop it into `include/` unchanged.

### 2. Your simulator (`cache_sim.cpp`) — *yours to write*

A concrete `csot::CacheSim` that implements [`CACHE_SPEC.md`](./CACHE_SPEC.md):

- allocate all state in `on_init()` (tag arrays, dirty/valid bits, LRU state for L1's 64 sets × 8 ways and L2's 512 sets × 8 ways)
- in `run()`, loop over the `n` accesses and update the seven counters exactly per spec
- export `extern "C" csot::CacheSim* create_cache_sim() { return new YourSim(); }`

The point is **not** to design a smarter cache. The point is to simulate the fixed one correctly, then make `run()` fast. Start from the stub:

```bash
cp samples/cache_sim.stub.cpp cache_sim.cpp
```

> 📌 **Single translation unit.** The judge compiles exactly this one `.cpp`. Helpers, templates, and tables all live inside it (use an anonymous `namespace`). No second `.cpp`, no `main()`, no custom CMake. See [`TROUBLESHOOTING.md`](./TROUBLESHOOTING.md) → "The single-`.cpp` rule".

### 3. Sample data files

- [`data/tiny.trace`](./data/tiny.trace) — **provided, 14 accesses**, hand-checkable; expected counters in [`data/tiny.stats.json`](./data/tiny.stats.json). Use for unit tests.
- `data/large.trace` — generate with `python3 data/gen_trace.py --accesses 5000000 --seed 42 --out data/large.trace`. Fast iteration + eviction/writeback coverage. **gitignored.**

> 💡 [`data/gen_trace.py`](./data/gen_trace.py) uses seed `42` by default — **everyone generates byte-identical traces**, so your numbers compare directly to classmates' and the baseline.

### 4. A `README.md` for your project — *yours to write*

Build steps, how to run, and your headline number: **throughput (M accesses/s)** and `run()` wall-clock on `data/large.trace`, plus confirmation that your counters match `data/tiny.stats.json`.

---

## 📁 Recommended Directory Layout

```
project/
├── CMakeLists.txt             ← ★ copy from this folder
├── .gitignore                 ← ★ copy from this folder
├── README.md                  ← your own writeup
├── CACHE_SPEC.md              ← ★ copy/read: the frozen hierarchy
├── TROUBLESHOOTING.md         ← ★ copy from this folder
├── include/
│   └── cache_sim.hpp          ← ★ copy from this folder unchanged
├── harness/
│   └── main.cpp               ← ★ local runner (you never upload it)
├── cache_sim.cpp              ← yours; implements CACHE_SPEC.md + exports create_cache_sim()
├── samples/
│   └── cache_sim.stub.cpp     ← ★ starting skeleton (copy to cache_sim.cpp)
├── data/
│   ├── gen_trace.py           ← ★ copy from this folder unchanged
│   ├── tiny.trace             ← ★ committed golden trace
│   ├── tiny.stats.json        ← ★ committed golden counters
│   └── large.trace            ← gitignored, generated by gen_trace.py
└── tools/
    └── plot_cache.py          ← optional
```

Files marked ★ are shipped — copy them verbatim. `cache_sim.cpp` is yours.

---

## 🪜 Suggested Implementation Order

Each step should compile and run before moving on.

### Step 1 — Skeleton build (Day 1, ~30 min)

- Copy the ★ files into place. Confirm the header compiles: `g++ -std=c++20 -Iinclude -c include/cache_sim.hpp -o /tmp/x.o` is silent.
- `cmake -B build && cmake --build build -j` builds `cache_sim_runner` from the stub.
- `./build/cache_sim_runner data/tiny.trace` prints stub counters (wrong on purpose) and a throughput line on stderr.

### Step 2 — Read the spec, by hand (Day 1, ~45 min)

- Read [`CACHE_SPEC.md`](./CACHE_SPEC.md) §3–§6 carefully.
- `python3 data/gen_trace.py --dump data/tiny.trace` and walk the 14 accesses against §5 with pen and paper. Confirm you arrive at `data/tiny.stats.json`. If you can't reproduce it by hand, you can't code it.

### Step 3 — Correct, naive simulator (Day 2, ~2 hours)

- `cp samples/cache_sim.stub.cpp cache_sim.cpp` and implement §5 the obvious way — even with `std::vector`/`unordered_map` per set. Correctness first.
- Build against your file and run on the tiny trace:
  ```bash
  cmake -B build -DCSOT_CACHE_SIM_SRC=cache_sim.cpp && cmake --build build -j
  diff <(./build/cache_sim_runner data/tiny.trace 2>/dev/null) data/tiny.stats.json
  ```
- A clean `diff` means you pass the smoke gate. Then stress a big trace (`--accesses 5000000`) to shake out eviction/writeback bugs (the tiny trace does **not** exercise L2 eviction — see `CACHE_SPEC.md` §9).

### Step 4 — Make it flat (Day 3, ~2 hours)

- Replace any node-based containers with **contiguous SoA arrays** indexed by set: parallel arrays of tags / valid / dirty / LRU state. This is the single biggest speedup ([`03-locality.md`](../03-locality.md)).
- Re-run the `diff`. Layout changes must not change counters.

### Step 5 — Zero-allocation hot path (Day 4, ~1 hour)

- Move every allocation into `on_init()`. `run()` must not touch the heap ([`04-zero-allocation.md`](./../04-zero-allocation.md)).
- Confirm with a quick check (e.g. an allocation counter or `perf stat -e ...` showing no page faults growing during `run()`).

### Step 6 — Compile-time geometry (Day 5, ~1.5 hours)

- Express `L1_SETS`, masks, way counts as `constexpr`. Let the compiler specialize the loop and fold the index/offset masks ([`05-compile-time-and-static-polymorphism.md`](../05-compile-time-and-static-polymorphism.md)).
- Look at the asm on [godbolt.org](https://godbolt.org). Did the set-index computation become a single `and`?

### Step 7 — Measure and squeeze (Day 6, ~2 hours)

- Generate a large trace, pin a core, and read the throughput line.
- `perf stat ./build/cache_sim_runner data/large.trace` — capture IPC, cache-miss%, branch-miss%.
- Bonus tier ([`06-bonus-simd-and-prefetch.md`](../06-bonus-simd-and-prefetch.md)): branchless 8-way tag scan, SIMD compare, `__builtin_prefetch` of the next access's set.

### Step 8 — Write it up (Day 7, ~30 min)

- Project `README.md`: build steps, your hardware, headline throughput + `run()` time on `large.trace`, a `perf stat` snippet, and **one thing that surprised you**.

That last bullet is the most important. It's the start of your intuition.

---

## 🧪 Hands-on practice alongside the project

The ranked deliverable is **`cache_sim.cpp` only**. These exercises are **not submitted** — they exist to make the layout lesson stick before you flatten the simulator (Step 4).

### Cache-graph — pointer graph vs. CSR BFS

[`03-locality.md`](../03-locality.md) §10 walks through a second project in the same spirit as Week 1's `vector` vs. `list` demo:

- **Same algorithm** (BFS), **two layouts** — linked-list adjacency (pointer-chasing, heap-scattered) vs. **CSR** (contiguous `row_ptr` + `col_idx`, the graph version of flat SoA).
- You write the loader, both BFS implementations, a conversion pass, a driver with `--repeat=N`, and a sweep/plot script. Only the graph generator spec is spelled out in the topic file; everything else is yours.
- Expect **2–8×** gaps on random graphs (`er`, `star`); `chain`/`grid` are tighter. Time **only** the BFS hot path inside the timer — conversion is cold-path work, like building flat arrays in `on_init`.
- ~3 hours for the full size/kind/repeat sweep and four plot panels. Optional Cachegrind bonus: shrink the simulated L1 and watch pointer layout bleed misses first.

Want more hand-holding (starter layout, report prompts)? See the original [cache_graph assignment](https://github.com/rijurekha/cache_graph) — the Week-2 topic adapts the same task.

> 💡 Do this **before or during Step 4**. When CSR's `col_idx` slice clicks as the same idea as contiguous `tag[]` per set, the SoA refactor on `cache_sim.cpp` stops feeling like a style preference and starts feeling inevitable.

---

## 🚀 Stretch Goals (Optional, for the Eager)

1. **`mmap` the trace** in your local harness instead of reading it — feel the load-time difference (the judge already mmaps).
2. Implement the 8-way tag scan **branchlessly**, then with SIMD intrinsics. Measure each step.
3. Pack valid+dirty into a **bitset per set**; compare against byte-per-way.
4. Try a **pseudo-LRU** internally and confirm it *breaks* correctness — then understand exactly why the counters drift (great learning, never submit it).
5. Prefetch `acc[i + K]`'s L1 set a few iterations ahead. Sweep `K`; plot throughput.
6. Build with `-DCSOT_JUDGE_BUILD=ON` and compare to your `-march=native` numbers. How much was the ISA worth?
7. Plot L1/L2 hit rates across working-set sizes (`tools/plot_cache.py`) — rediscover the Week-1 cache cliff, this time in a simulator.
8. Finish the full **cache-graph** sweep from [`03-locality.md`](../03-locality.md) §10 (pointer vs. CSR on all four graph kinds, `repeat` sweep, time + L1 miss plots).

---

## 🏆 The Live Leaderboard

**The leaderboard is live: [csot-low-latency.devclub.in](https://csot-low-latency.devclub.in/).** Week 2 swaps the active ranked challenge from the Week-1 strategy to the **cache simulator** — a fresh board with a high speed ceiling, so the top is not saturated on day one.

### How it works (new this week: you upload source)

1. Sign in with your DevClub IITD identity.
2. On the dashboard, upload a **single `cache_sim.cpp`** (not a compiled `.so` — that changed this week).
3. The judge **builds your file itself** with a fixed, portable toolchain (`-O3 -march=x86-64-v2`, `CSOT_JUDGE_BUILD=ON`) against its own `main()` and `cache_sim.hpp`, inside a sandbox.
4. It runs your `run()` against **two traces**, diffs the seven counters against the reference, and — if correct — ranks you by wall-clock speed.

Uploading source instead of a binary means everyone is measured on the **same compiler and flags**, so the board reflects *your code*, not your laptop's CPU. (It also kills the Week-1 `runner exit 132` class of failures.) The exact build, scoring, and sandbox rules are specced in [`platform_week_2.md`](../../../platform_week_2.md).

### The two traces

| Trace | Accesses | Seed | Reproducible? | Role |
|---|---:|---:|---|---|
| **Public** | 5 000 000 | `42` (the `gen_trace.py` default) | ✅ Yes — `python3 data/gen_trace.py --accesses 5000000 --seed 42 --out public.trace` | Correctness smoke test. Fail here and you stop. |
| **Hidden** | very large | sealed | ❌ No — held out on the judge box | The speed ranking happens here. |

Debug your `incorrect` runs locally on the reproducible public trace before bothering the judge; the hidden trace forces your simulator to generalize.

### Correctness is binary, and it gates everything

- All **seven** `CacheStats` counters must match the reference exactly. One disagreement → `incorrect`, no rank.
- If both traces pass, the judge ranks by **wall-clock `run()` time** (and throughput). Cache-design quality is *not* scored — the geometry is fixed.

### Submission policy

- **Single `cache_sim.cpp`**, ≤ 1 MiB, must export `extern "C" csot::CacheSim* create_cache_sim()`.
- One identity, one entry. No teams.
- Cooldown + daily quota enforced server-side (the dashboard shows the window; spamming returns `429`).
- The judge VM may be stopped between sessions to save cost — submissions queue and drain when it's back.

### What you do NOT have to do this week

You don't have to top the board. Upload when your simulator is **correct on `tiny.trace` and produces sane throughput on a large trace**. Beating the reference is the game for the eager.

---

## ❓ FAQ

**Q: Why a cache simulator? I came here for trading.**
A: Low-latency trading *is* cache engineering. The fixed strategy returns to the board in Week 4 with new metrics; this week the workload is computer-architecture-shaped so you internalize the memory hierarchy by building one. The leaderboard still measures the same thing: systems-engineering speed.

**Q: Can I submit a `.so` like last week?**
A: No. From this week the judge takes a single `cache_sim.cpp` and builds it itself. This makes the playing field identical for everyone.

**Q: Can I split my simulator across multiple files / add a `main()`?**
A: No. One translation unit, no `main()`. See [`TROUBLESHOOTING.md`](./TROUBLESHOOTING.md).

**Q: Can I use a faster, approximate replacement policy (pseudo-LRU, NRU)?**
A: Not for a ranked submission — it changes the counters and fails correctness. Exact LRU only. (It's a great thing to *try* locally to understand why it diverges.)

**Q: My counters are close but off by a handful.**
A: Almost always one of: updating LRU on a writeback, counting a writeback as an L2 access, or write-no-allocate. See `CACHE_SPEC.md` §9 and `TROUBLESHOOTING.md` → "Correctness".

**Q: I pass `tiny.trace` but fail the hidden trace.**
A: `tiny.trace` doesn't force L2 eviction (so `dirty_writebacks` stays 0). Generate a large trace to exercise eviction and writeback before you trust your simulator.

**Q: I'm on Windows / a slow laptop.**
A: WSL2 is fine. By Week 3 you'll want native Linux for `perf`.

---

## 📤 Submission

### 1. The Git repo (for cohort review)

A public repo with your `cache_sim.cpp`, project `README.md` (headline numbers), the shipped `gen_trace.py`, and a `.gitignore` for `build/` and `*.trace`.

### 2. The leaderboard (for the live ranking)

Sign in at **[csot-low-latency.devclub.in](https://csot-low-latency.devclub.in/)** and upload your `cache_sim.cpp` from the dashboard. The judge builds it, runs it, and reports back counters, throughput, and a rank.

---

## 🎉 You Made It

You built a cache from first principles and then made it scream. **Understanding the memory hierarchy by simulating it is the fastest way to never fear it again** — and every optimization you just made (SoA, zero-alloc, compile-time folding) is exactly what Week 3 needs when we add threads.

See you in Week 3. ⚡

(Week 3 is coming soon — multi-threaded ingest, `alignas(64)`, and core pinning, built on the layout discipline you just practised.)
