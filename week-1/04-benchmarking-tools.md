# 04 — Benchmarking Tools: How to Measure Without Lying to Yourself

> **TL;DR** — A benchmark with a wrong methodology is worse than no benchmark, because it gives you *false confidence*. This note teaches you to time code correctly with `std::chrono`, `rdtsc`, Google Benchmark, and `perf`.

---

## 1. Why Benchmarking Is Surprisingly Hard

Modern computers actively try to make your code look fast:

- **Compilers** eliminate "dead" code if they prove the result is unused.
- **CPUs** speculatively execute, reorder instructions, and predict branches.
- **OS schedulers** preempt your thread at arbitrary moments.
- **Frequency scaling** (Intel Turbo, AMD Precision Boost) changes clock speed dynamically.
- **Caches** make the second call to a function ~100x faster than the first.

If you don't account for these, your "benchmark" is measuring noise.

The four classic mistakes:

1. **Running once.** A single measurement is noise. You need *many*.
2. **Letting the compiler eliminate the work.** Always *consume* the result.
3. **Ignoring warm-up.** The first iteration loads caches, JITs branch predictors, faults pages — its number is meaningless.
4. **Reporting only the mean.** The mean hides the tail. Report median, p99, and stddev too.

We will use tools that handle these for us.

---

## 2. The Tools You'll Use This Week

| Tool | What it measures | When to use |
|---|---|---|
| `std::chrono::steady_clock` | wall-clock ns (~50 ns res) | Coarse timing (ms-scale work), portable |
| `rdtsc` (TSC counter) | CPU cycles (sub-ns) | Fine timing inside hot loops, single-threaded |
| **Google Benchmark** | per-iteration time, throughput | Microbenchmarks of small functions — your default |
| **`perf`** | real hardware counters (PMCs) | Diagnosing *why* code is slow on a real CPU |
| **Valgrind / Memcheck** | leaks, UB, invalid reads/writes | *Correctness* of memory use (not speed) |
| **Cachegrind** | simulated cache hits/misses, branch mispredicts | Repeatable, deterministic cache analysis |
| **Callgrind + KCachegrind** | function-level call counts & costs | Visual profiling, no kernel privileges needed |

> A useful mental split:
> - **`perf`** — *what the real CPU actually did this run* (fast, real hardware, but noisy).
> - **Valgrind family (Cachegrind/Callgrind)** — *what a simulated CPU would do* (slow, deterministic, repeatable across machines).
> - **Memcheck (Valgrind's default)** — *did your program touch memory legally* (correctness, not perf).
>
> They're complementary. Real low-latency work uses all of them.

---

## 3. `std::chrono::steady_clock` — The Portable Default

Always use `steady_clock`, never `system_clock` (which can jump backward when NTP adjusts time).

```cpp
#include <chrono>

auto t1 = std::chrono::steady_clock::now();
work();
auto t2 = std::chrono::steady_clock::now();

auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t1).count();
std::cout << ns << " ns\n";
```

**Caveats:**
- Resolution on Linux is typically ~20-50 ns. Anything shorter than that is noise.
- Calling `now()` itself takes ~20-50 ns, so you can't put it inside a tight inner loop.
- It crosses kernel boundaries on some platforms (via `clock_gettime`).

Good for: timing whole functions, the full run of a benchmark, between-frame measurements.

---

## 4. `rdtsc` — The Cycle Counter

`rdtsc` (Read Time-Stamp Counter) is a CPU instruction that returns a 64-bit counter incremented at the CPU's nominal frequency (regardless of turbo / power state, on modern CPUs — this is "constant_tsc"). It's the lowest-overhead clock you can use.

```cpp
#include <x86intrin.h>     // GCC / Clang on x86

uint64_t t1 = __rdtsc();
work();
uint64_t t2 = __rdtsc();

uint64_t cycles = t2 - t1;
```

**Caveats:**
- Out-of-order CPUs can move instructions across `rdtsc`. Use `__rdtscp` (serializing variant) or surround with `_mm_lfence()` if you need precise ordering.
- The TSC is per-package; on multi-socket systems, threads on different sockets may see slightly different values.
- The TSC frequency ≠ the actual core frequency. To convert cycles → ns, you need to know the **TSC frequency** (e.g. from `lscpu` or by calibrating against `steady_clock` for ~1 sec at startup).

Use `rdtsc` when you need to measure something inside a tight loop where even 50 ns of `chrono` overhead matters.

```cpp
// Calibrate at startup
double tsc_per_ns() {
    auto t0 = std::chrono::steady_clock::now();
    uint64_t c0 = __rdtsc();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    auto t1 = std::chrono::steady_clock::now();
    uint64_t c1 = __rdtsc();
    double ns = std::chrono::duration<double, std::nano>(t1 - t0).count();
    return (c1 - c0) / ns;
}
```

---

## 5. Google Benchmark — Your Default Microbench

This is the tool you'll reach for 90% of the time. It handles warm-up, iteration count, compiler-fooling, and statistics for you.

### Install

```bash
sudo apt install libbenchmark-dev          # Ubuntu/Debian
# or
brew install google-benchmark              # macOS
```

If unavailable, build from source: <https://github.com/google/benchmark>

### Minimal example — `bench.cpp`

```cpp
#include <benchmark/benchmark.h>
#include <vector>
#include <list>
#include <numeric>

static void BM_VectorSum(benchmark::State& state) {
    std::vector<int> v(state.range(0), 1);
    for (auto _ : state) {
        long sum = std::accumulate(v.begin(), v.end(), 0L);
        benchmark::DoNotOptimize(sum);   // prevent dead-code elimination
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_VectorSum)->Range(1 << 10, 1 << 22);

static void BM_ListSum(benchmark::State& state) {
    std::list<int> l(state.range(0), 1);
    for (auto _ : state) {
        long sum = std::accumulate(l.begin(), l.end(), 0L);
        benchmark::DoNotOptimize(sum);
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_ListSum)->Range(1 << 10, 1 << 22);

BENCHMARK_MAIN();
```

### Compile & Run

```bash
g++ -O2 -std=c++20 bench.cpp -lbenchmark -lpthread -o bench
./bench
```

Output looks like:

```
-------------------------------------------------------------------
Benchmark               Time           CPU Iterations  items/second
-------------------------------------------------------------------
BM_VectorSum/1024     273 ns        273 ns    2546912    3.74G/s
BM_VectorSum/4096    1086 ns       1086 ns     645248    3.77G/s
...
BM_ListSum/1024      8412 ns       8412 ns      83136    121.7M/s
BM_ListSum/4096     54218 ns      54218 ns      12891     75.5M/s
```

The `Time` column is **per iteration** (the inner loop body, not the entire benchmark run). Google Benchmark figures out how many iterations are needed to get a statistically stable number.

### Useful Knobs

```cpp
// Multiple complexity points
BENCHMARK(BM_VectorSum)->RangeMultiplier(2)->Range(1<<10, 1<<24);

// Pass arbitrary args
BENCHMARK(BM_MyFunc)->Args({1, 100})->Args({2, 200});

// Manual timing (when you need rdtsc inside the loop)
state.PauseTiming();  do_setup();  state.ResumeTiming();

// Real-time wall clock (default is CPU time)
BENCHMARK(BM_MyFunc)->UseRealTime();

// Run on a single thread / multiple threads
BENCHMARK(BM_MyFunc)->Threads(1)->Threads(4)->Threads(8);

// JSON output for plotting
./bench --benchmark_format=json --benchmark_out=results.json
```

### The Critical Calls

- **`benchmark::DoNotOptimize(x)`** — tells the compiler "pretend `x` is used", preventing dead-code elimination of the value.
- **`benchmark::ClobberMemory()`** — tells the compiler "pretend all memory may have been written", preventing reorder of stores across this point. Use after writes you want to time.

Without these, with `-O2`, the compiler will happily delete your entire benchmark and you'll measure "1 ns/iter" for everything. Then you'll get confused. Don't be that person.

---

## 6. `perf` — The Hardware Microscope

`std::chrono` and Google Benchmark tell you *how slow* your code is. `perf` tells you *why*.

`perf` reads the CPU's built-in **performance monitoring counters** (PMCs) — hardware registers that count events like cycles, retired instructions, cache misses, branch mispredicts, page faults.

### Install

```bash
sudo apt install linux-tools-common linux-tools-generic linux-tools-$(uname -r)
```

You may need to relax kernel security to use it as a regular user:

```bash
# Temporary (lost on reboot)
sudo sysctl kernel.perf_event_paranoid=1

# Permanent
echo 'kernel.perf_event_paranoid = 1' | sudo tee /etc/sysctl.d/99-perf.conf
```

### `perf stat` — High-Level Counters

The 30-second version of "is this code memory-bound, branch-bound, or CPU-bound?"

```bash
perf stat ./bench
```

Output (truncated):

```
 Performance counter stats for './bench':

           1234.56 msec task-clock                #    0.998 CPUs utilized
                12      context-switches          #    0.010 K/sec
                 0      cpu-migrations
               456      page-faults               #    0.369 K/sec
     4,098,765,432      cycles                    #    3.319 GHz
     8,123,456,789      instructions              #    1.98  insn per cycle
       456,789,012      branches                  #  370.123 M/sec
         1,234,567      branch-misses             #    0.27% of all branches
        45,678,901      cache-references
         9,876,543      cache-misses              #   21.62% of all cache refs

       1.236 seconds time elapsed
```

What to look at:

- **insn per cycle (IPC)** — modern x86 can sustain 3-4 IPC on optimized code. Below 1.0 = stalling on memory or branches.
- **cache-misses %** — high = memory-bound. Try to reduce working-set size or improve locality.
- **branch-misses %** — > 5% is bad; look for unpredictable branches.
- **page-faults** — should be near zero on a warm hot path; non-zero means you're touching pages the OS hasn't backed.

### Selecting Specific Events

```bash
perf stat -e cache-references,cache-misses,L1-dcache-load-misses,LLC-load-misses ./bench
```

List all available events: `perf list`. There are hundreds. The ones you'll actually use:

| Event | What it tells you |
|---|---|
| `cycles` | Total CPU cycles consumed |
| `instructions` | Retired instructions (combine with cycles → **IPC**) |
| `L1-dcache-loads` / `L1-dcache-load-misses` | L1 data-cache pressure |
| `LLC-loads` / `LLC-load-misses` | Last-level (L3) misses → going to DRAM |
| `branches` / `branch-misses` | Branch predictor health |
| `dTLB-loads` / `dTLB-load-misses` | TLB pressure (huge-page candidate?) |
| `mem-loads` / `mem-stores` | Memory traffic (Intel) |
| `cpu-migrations` | Thread bounced between cores (bad for caches) |
| `page-faults` / `minor-faults` / `major-faults` | OS touching your memory |

You can also group events with `{ ... }` so they're measured against the same window:

```bash
perf stat -e '{cycles,instructions,cache-misses,branch-misses}' ./bench
```

### `perf stat -d` and `-dd` — Pre-packaged Detail Levels

Don't want to memorize event names? `perf` ships with shortcuts:

```bash
perf stat -d  ./bench    # adds L1/LLC + frontend/backend stalls
perf stat -dd ./bench    # adds dTLB/iTLB stats
perf stat -ddd ./bench   # the kitchen sink
```

These are the fastest way to get a useful first picture of a binary.

### Repeated Runs & Stable Numbers

A single `perf stat` run is noisy. Use `-r` for repetitions and `--table` for variance:

```bash
perf stat -r 10 ./bench           # 10 runs, prints mean ± stddev
perf stat --table -r 5 ./bench    # per-run breakdown
```

### `perf record` + `perf report` — Profile by Function

```bash
perf record -g ./bench    # default: samples 'cycles' at 4 kHz
perf report               # interactive TUI
```

This shows which functions / source lines are eating the most CPU. `-g` enables call-graph capture (sampled stack traces). Useful options:

```bash
perf record -F 999 -g --call-graph dwarf ./bench   # 999 Hz, DWARF unwinder
perf record -e cache-misses -c 10000 ./bench       # sample on cache misses, every 10k events
perf record --pid $(pidof my_daemon) -- sleep 10   # attach to a running process for 10s
```

> **Sampling vs counting:** `perf stat` *counts* events exactly (no overhead per event). `perf record` *samples* — it interrupts every N events and records the stack. Sampling is what lets you ask "*where* are my cache misses happening?", not just "how many?".

### `perf top` — Live System-Wide View

```bash
sudo perf top              # like 'top' but for hot functions across the whole system
perf top --pid $(pidof bench)   # scoped to one process
```

Use it to spot a runaway hot function in production.

### `perf annotate` — Down to the Assembly

```bash
perf record ./bench
perf annotate                 # pick a function, see per-instruction hit count
```

This is **the** tool for "why is this specific line slow?". You'll see each x86 instruction with the % of samples that hit it. The slow instructions are almost always loads from memory or hard-to-predict branches.

### `perf mem` and `perf c2c` — Memory & Cache Coherency (advanced)

For Week 3+ when we tackle false sharing:

```bash
perf mem record ./bench
perf mem report                # which loads were L1 hits? L3? DRAM?

perf c2c record ./bench        # "cache-to-cache" — finds false sharing
perf c2c report                # shows cache lines bouncing between cores
```

`perf c2c` is gold for hunting false sharing. We'll use it in Week 3.

### Flame Graphs (the gorgeous version)

```bash
git clone https://github.com/brendangregg/FlameGraph
perf record -F 999 -g ./bench
perf script | ./FlameGraph/stackcollapse-perf.pl | ./FlameGraph/flamegraph.pl > graph.svg
xdg-open graph.svg
```

A flame graph is the single most useful performance visualization ever invented. Make one and stare at it.

### Gotchas

- **Permissions:** if you see `No permission to enable ... events`, drop `perf_event_paranoid` (above) or run with `sudo`.
- **Frame pointers:** `-fomit-frame-pointer` (default at `-O2` on many distros) breaks `--call-graph fp`. Use `--call-graph dwarf` or rebuild with `-fno-omit-frame-pointer`.
- **Inlining:** at `-O2`/`-O3`, the function you want to profile may be inlined into its caller. Use `__attribute__((noinline))` temporarily on the function of interest.
- **VMs / containers:** PMCs are virtualized poorly. Numbers from a Docker container on a hypervisor are *suggestive* at best. Bare metal for real measurements.
- **WSL2:** no PMCs at all — `perf stat` works for software counters (`task-clock`, `context-switches`, `page-faults`) but every hardware counter shows `<not supported>`. Boot real Linux when you need them.

---

## 7. Valgrind — Correctness First, Then Speed

Valgrind is **not** a benchmarking tool. It's a *dynamic binary instrumentation* framework that runs your program on a synthetic CPU and watches every load, store, and branch. That makes it ~10-50x slower than native, but it sees **everything**.

The Valgrind project ships several "tools" (think: plugins) that share that machinery. The three you care about in this track:

| Tool | Purpose |
|---|---|
| **Memcheck** (default) | Detects memory bugs: leaks, use-after-free, uninitialized reads, out-of-bounds |
| **Cachegrind** | Simulates L1/LL caches + branch predictor; deterministic miss counts |
| **Callgrind** | Cachegrind + call-graph; visualize with KCachegrind |

### Install

```bash
sudo apt install valgrind kcachegrind          # Ubuntu/Debian
brew install valgrind                          # macOS (limited support on Apple Silicon)
```

### Memcheck — The First Thing You Run on Any New C++ Code

Forgot to free that arena? Read past the end of a `std::vector` in a tight loop? Memcheck will find it.

```bash
g++ -O1 -g myprog.cpp -o myprog                # -g for symbols, -O1 for readable traces
valgrind --leak-check=full --show-leak-kinds=all ./myprog
```

Typical output excerpt:

```
==12345== Invalid read of size 4
==12345==    at 0x4006A2: process_tick (engine.cpp:42)
==12345==    by 0x40081B: main (main.cpp:17)
==12345==  Address 0x520404c is 4 bytes after a block of size 4,000 alloc'd
==12345==    at 0x483B7F3: operator new (vg_replace_malloc.c:344)
==12345==    by 0x40067D: main (main.cpp:11)
==12345==
==12345== HEAP SUMMARY:
==12345==     in use at exit: 4,000 bytes in 1 blocks
==12345==    definitely lost: 4,000 bytes in 1 blocks
```

It tells you the **bug**, the **file:line**, and the **allocation site** of the offending block. There is no excuse for memory bugs in modern C++ — run Memcheck on every project at least once.

> **Note:** Memcheck and **AddressSanitizer (`-fsanitize=address`)** overlap a lot. ASan is ~2x slowdown vs Memcheck's ~20x, and catches most of the same things. Use ASan in CI; reach for Memcheck when ASan says "clean" but you still suspect a bug, or when you want leak detection without recompiling.

### When Not to Run Memcheck

- **Never on a benchmark.** Timing under Valgrind is meaningless — everything is 20x slower, but not uniformly.
- **Never in production.** Massive overhead.
- Use it during *development* and in *CI on small test inputs*.

---

## 8. Cachegrind — Deterministic Cache Analysis

`perf` reads real hardware counters → fast but noisy. Cachegrind *simulates* a cache → slow but **bit-for-bit deterministic**. Run it twice, get identical numbers. Run it on a colleague's machine, get identical numbers (assuming the same cache config).

### Basic Run

```bash
g++ -O2 -g myprog.cpp -o myprog
valgrind --tool=cachegrind ./myprog
```

You'll see (and a `cachegrind.out.<pid>` file gets written):

```
==12345== I   refs:      1,234,567,890
==12345== I1  misses:           12,345        ← L1 instruction-cache misses
==12345== LLi misses:            1,234        ← Last-level instruction misses
==12345== I1  miss rate:         0.00%
==12345== LLi miss rate:         0.00%
==12345==
==12345== D   refs:        456,789,012  (300M rd + 156M wr)
==12345== D1  misses:        4,567,890  (3.5M rd + 1.0M wr)   ← L1 data misses
==12345== LLd misses:          234,567  (200K rd + 34K wr)    ← LL data misses
==12345== D1  miss rate:          1.0%
==12345== LLd miss rate:          0.1%
==12345==
==12345== LL refs:           4,580,235
==12345== LL misses:           235,801
==12345== LL miss rate:           0.0%
```

By default Cachegrind auto-detects your real CPU's cache geometry. You can override it for "what-if" analysis:

```bash
valgrind --tool=cachegrind \
         --I1=32768,8,64 \         # 32 KB, 8-way, 64-byte lines
         --D1=32768,8,64 \
         --LL=8388608,16,64 \      # 8 MB LL cache, 16-way, 64-byte lines
         ./myprog
```

This is incredibly useful for asking *"how would my code behave on a CPU with only 16 KB of L1?"* without owning that CPU.

### `cg_annotate` — Per-Source-Line Miss Counts

```bash
cg_annotate cachegrind.out.12345
```

You get a per-function table:

```
Ir          I1mr  ILmr  Dr          D1mr     DLmr   Dw         D1mw  DLmw   file:function
--------------------------------------------------------------------------------------
245,678,901   12     0  98,765,432  3,456,789  234   45,678,901  ...   engine.cpp:run_tick
 12,345,678    0     0   5,432,109     12,345   0     1,234,567  ...   strategies/ma.cpp:on_tick
```

Add `--auto=yes` to get a per-source-line annotation embedded in your source code. This is **the** view that tells you "line 47 of `run_tick` accounts for 80% of your L1 misses."

### Branch Prediction (`--branch-sim`)

By default Cachegrind only simulates caches. Add branch-predictor simulation:

```bash
valgrind --tool=cachegrind --branch-sim=yes ./myprog
cg_annotate --show=Bc,Bcm cachegrind.out.<pid>     # branches / mispredicts
```

`Bcm` (conditional branch mispredicts) tells you where your unpredictable `if`s are.

### When to Reach for Cachegrind vs `perf`

| Situation | Tool |
|---|---|
| "I want a quick miss-rate ballpark on real hardware" | `perf stat` |
| "I want to know *which source line* misses most" | `perf record` + `perf annotate`, or `cachegrind` + `cg_annotate` |
| "I want numbers a colleague on a different laptop can reproduce" | **Cachegrind** |
| "I want to simulate a CPU I don't have" | **Cachegrind** (`--D1=...`, `--LL=...`) |
| "I'm in a VM / WSL where PMCs don't work" | **Cachegrind** |
| "I need true microsecond timing of a real run" | `perf` (Cachegrind distorts timing) |
| "My program is huge and I have 2 minutes" | `perf` (Cachegrind is 20-100x slower) |

---

## 9. Callgrind + KCachegrind — Visual Profiling

Callgrind is Cachegrind plus **call-graph collection**. KCachegrind is a Qt GUI that turns the output into clickable flame-graph-like views.

### Capture

```bash
valgrind --tool=callgrind ./myprog                 # writes callgrind.out.<pid>

# Optional: also collect cache events
valgrind --tool=callgrind --cache-sim=yes --branch-sim=yes ./myprog

# Toggle collection on/off around a region of interest (avoid measuring startup):
valgrind --tool=callgrind --collect-atstart=no ./myprog
# Then in code:
#   #include <valgrind/callgrind.h>
#   CALLGRIND_START_INSTRUMENTATION;
#   CALLGRIND_TOGGLE_COLLECT;
#   ...hot code...
#   CALLGRIND_TOGGLE_COLLECT;
```

### View

```bash
kcachegrind callgrind.out.<pid>
```

You get:
- A sortable function list (self cost, inclusive cost, call count)
- A **call graph** view (zoom into hot subtrees)
- A **caller / callee map** (who calls what, how often)
- Per-source-line annotation (with `-g` symbols)

It's the single most ergonomic profiling UI in open source. Especially good when:
- You're new to a codebase and want a visual map of where time goes.
- You don't have root and can't use `perf record`.
- You want to *count* something (call counts are exact under Callgrind, sampled under `perf`).

### Text-mode Alternative

If you don't have a GUI handy:

```bash
callgrind_annotate callgrind.out.<pid> | less
callgrind_annotate --tree=both callgrind.out.<pid>
```

### Callgrind vs `perf record`

| | Callgrind | `perf record` |
|---|---|---|
| Speed | 20-100x slower | ~5% overhead |
| Determinism | Fully deterministic | Sampled, noisy |
| Counts | Exact | Statistical |
| Privileges | None needed | Often needs `sudo` or sysctl tweak |
| Works in VM/WSL | Yes | Hardware events: no |
| Hardware reality | Simulated CPU | Your actual CPU |
| Best for | "What does my code *do*?" | "What is my CPU *actually struggling with*?" |

Use both. They answer different questions.

---

## 10. Best Practices for Reproducible Benchmarks

Before running a serious benchmark, **disable the noise sources**:

```bash
# Disable CPU frequency scaling (forces max frequency)
sudo cpupower frequency-set -g performance

# Disable Intel turbo (constant frequency = comparable runs)
echo 1 | sudo tee /sys/devices/system/cpu/intel_pstate/no_turbo

# Disable hyperthreading siblings on the cores you'll use (optional, advanced)
# Find sibling pairs:
cat /sys/devices/system/cpu/cpu*/topology/thread_siblings_list | sort -u

# Pin your benchmark to a specific core (Week 3 covers this)
taskset -c 3 ./bench

# Increase process priority
sudo nice -n -20 ./bench

# Run on an isolated core (boot parameter: isolcpus=3)
# Then nothing else gets scheduled there.

# Disable address-space layout randomization (for reproducible cache effects)
setarch $(uname -m) -R ./bench
```

For Week 1, just *closing your browser and running on AC power* gets you 80% of the benefit. The above is for Week 3+.

### Other Sanity Rules

- **Run each benchmark at least 5-10 times** and look at the spread.
- **Compile with `-O2` (or `-O3`) and `-DNDEBUG`** — never benchmark debug builds.
- **Pin to a single core** for any benchmark < 1 ms.
- **Beat the compiler:** use `DoNotOptimize` / `ClobberMemory`, or write the result to a `volatile`.
- **Document your hardware** — CPU model, RAM speed, kernel version. Without this, your numbers are unreproducible.

---

## 11. A Mini-Workflow

The end-to-end loop for any optimization:

1. **Make a hypothesis.** ("I think iterating this list is slow because of cache misses.")
2. **Run Memcheck / ASan first** to make sure the code is *correct* — there's no point optimizing a bug.
3. **Write a Google Benchmark** comparing the old vs. new design.
4. **Run `perf stat -d`** on each to confirm *why* one is faster (cache misses? branch misses? IPC?).
5. **Drill in with `perf record` + `perf annotate`** (or Cachegrind + `cg_annotate`) to find the exact source line responsible.
6. **Plot the data** (gnuplot, matplotlib, anything) — eyeballing a single number lies; a curve doesn't.
7. **Decide if the win is worth the code complexity.** Often it isn't.

Internalize this loop. It will become muscle memory.

---

## 12. A Complete Example: The Cache Cliff

Putting it all together — measure the L1/L2/L3/DRAM cliff with proper methodology:

```cpp
// cache_cliff.cpp
#include <benchmark/benchmark.h>
#include <vector>
#include <numeric>

static void BM_SumVec(benchmark::State& state) {
    size_t n_bytes = state.range(0);
    size_t n = n_bytes / sizeof(int);
    std::vector<int> v(n, 1);
    for (auto _ : state) {
        long s = 0;
        for (int x : v) s += x;
        benchmark::DoNotOptimize(s);
    }
    state.SetBytesProcessed(state.iterations() * n_bytes);
    state.SetLabel(std::to_string(n_bytes/1024) + " KiB");
}
BENCHMARK(BM_SumVec)->RangeMultiplier(2)->Range(8 << 10, 64 << 20);  // 8 KiB → 64 MiB

BENCHMARK_MAIN();
```

```bash
g++ -O2 -std=c++20 cache_cliff.cpp -lbenchmark -lpthread -o cache_cliff
sudo cpupower frequency-set -g performance
taskset -c 3 ./cache_cliff --benchmark_repetitions=5
```

You'll see throughput (bytes/sec) drop sharply at three points corresponding to L1, L2, L3 boundaries. **This is the graph from `03-memory-hierarchy.md`, made real.**

---

## 🎯 Key Takeaways

- **`std::chrono::steady_clock`** for portable timing of ms-scale work.
- **`rdtsc`** when you need sub-ns resolution inside hot loops.
- **Google Benchmark** for microbenchmarks of small functions — handles iteration, warm-up, statistics, and compiler-fooling.
- **`perf`** for real-CPU diagnostics: `perf stat -d` for the overview, `perf record` + `perf annotate` for line-level hotspots, `perf c2c` for false sharing (Week 3).
- **Valgrind / Memcheck** for *correctness* (leaks, UAF, OOB) — never for timing.
- **Cachegrind** for **deterministic, reproducible** cache miss counts and "what-if" cache geometry experiments.
- **Callgrind + KCachegrind** for visual call-graph profiling without needing kernel privileges or PMCs (works in VMs, WSL, restricted environments).
- **Always** use `DoNotOptimize` / `ClobberMemory` or you'll measure nothing.
- **Disable noise** (frequency scaling, turbo, hyperthreading, background tasks) for serious measurements.
- **Report distributions, not means.** Median and p99 tell the real story.

---

## 📚 Further Reading — Benchmarking, perf, Valgrind

### Benchmarking Methodology

- 📦 **[Google Benchmark — official User Guide](https://github.com/google/benchmark/blob/main/docs/user_guide.md)** — especially the "Preventing Optimization" section.
- 📦 [Quick Bench (quick-bench.com)](https://quick-bench.com/) — Google Benchmark in the browser. Great for sharing snippets.
- 📦 [`hyperfine`](https://github.com/sharkdp/hyperfine) — quick CLI tool for whole-program timing (`hyperfine './a.out' './b.out'`).
- 📰 **[Andrey Akinshin — "Pro .NET Benchmarking"](https://aakinshin.net/prodotnetbenchmarking/)** (free chapters) — .NET examples but the methodology chapters are universal.
- 📰 [Daniel Lemire's blog](https://lemire.me/blog/) — endless micro-benchmarking experiments with real code.
- 📰 [Aleksey Shipilëv — "Nanotrusting the Nanotime"](https://shipilev.net/blog/2014/nanotrusting-nanotime/) — why low-resolution clocks lie to you.
- 🎬 [Chandler Carruth — "Tuning C++: Benchmarks, and CPUs, and Compilers!"](https://www.youtube.com/watch?v=nXaxk27zwlk) (CppCon 2015) — covers `perf`, microbenches, and compiler interactions.

### `perf` & Flame Graphs

- 📰 **[Brendan Gregg — Linux `perf` examples](https://www.brendangregg.com/perf.html)** — every flag you'll ever need.
- 📰 [Brendan Gregg — Flame Graphs](https://www.brendangregg.com/flamegraphs.html) — the canonical reference for the most useful perf visualization in existence.

### Valgrind & Cachegrind

- 📖 [Valgrind official manual](https://valgrind.org/docs/manual/manual.html) — the *Quick Start* is 10 minutes.
- 📖 [Memcheck manual](https://valgrind.org/docs/manual/mc-manual.html) — what every error message actually means.
- 📖 [Cachegrind manual](https://valgrind.org/docs/manual/cg-manual.html) — including `--I1=`, `--D1=`, `--LL=` for what-if cache geometry.
- 📖 [Callgrind manual](https://valgrind.org/docs/manual/cl-manual.html) + [KCachegrind docs](https://kcachegrind.github.io/html/Documentation.html) — the visual profiling workflow.
- 📰 [Julia Evans — "Profiling with Valgrind and KCachegrind"](https://jvns.ca/blog/2017/12/02/taking-a-look-at-callgrind/) — short, friendly walkthrough.

---

## ▶️ Next

That's it for the reading! Now hit the project. Open [`project/README.md`](./project/README.md) and start building.
