# Troubleshooting — Week 1 Quant Platform

The 50 most common ways to get stuck, and how to unstick yourself. Skim once now, come back when you hit something.

> 🆘 If your problem isn't here, drop the full error in the CSoT group along with **your OS, compiler version, and the exact command you ran**.

---

## 🏗️ Build & Toolchain

### `g++: command not found` / `cmake: command not found`

You don't have the toolchain installed yet.

```bash
sudo apt update
sudo apt install -y build-essential cmake git python3 python3-pip \
                    linux-tools-common linux-tools-generic linux-tools-$(uname -r) \
                    libbenchmark-dev valgrind
```

On Fedora: `sudo dnf install gcc-c++ cmake git perf valgrind google-benchmark-devel`.
On Arch: `sudo pacman -S base-devel cmake git perf valgrind benchmark`.

### `error: ‘std::string_view’ has not been declared`

Your compiler is too old (pre-C++17 default) or you forgot `#include <string_view>`. Check:

```bash
g++ --version              # must be >= 11
```

Make sure your CMake sets `set(CMAKE_CXX_STANDARD 20)` (the shipped `CMakeLists.txt` does).

### `static assertion failed: Tick layout is part of the ABI`

You're including a modified `strategy.hpp`. Re-copy it verbatim from `low-latency/week-1/project/include/strategy.hpp`. **Do not edit the structs.**

If the assert fires on an *unmodified* header, you may be on a 32-bit platform or a non-x86-64 architecture. Tell us in the group — we'll add a platform check.

### `fatal error: benchmark/benchmark.h: No such file or directory`

Google Benchmark isn't installed.

```bash
sudo apt install libbenchmark-dev          # Ubuntu/Debian
brew install google-benchmark              # macOS
```

Or build from source: https://github.com/google/benchmark

Then make sure your `CMakeLists.txt` has:

```cmake
find_package(benchmark REQUIRED)
target_link_libraries(quant_bench PRIVATE benchmark::benchmark)
```

### `undefined reference to 'dlopen'`

You need to link against `libdl`. With the shipped CMake template, use `${CMAKE_DL_LIBS}`:

```cmake
target_link_libraries(engine PUBLIC ${CMAKE_DL_LIBS})
```

Or with raw `g++`: add `-ldl` at the end of the link line.

### `cannot find -lbenchmark`

Same as the missing header above — `libbenchmark-dev` isn't installed, or your linker can't find it. Try the full path:

```bash
ldconfig -p | grep benchmark   # should list libbenchmark.so
```

### My `.so` won't load — `cannot open shared object file`

Either the path is wrong, or there are missing symbols. Diagnose:

```bash
file your_strategy.so                  # confirm it's actually a shared library
ldd your_strategy.so                   # show missing dependencies (any "not found"?)
nm -D your_strategy.so | grep create_  # is create_strategy actually exported?
```

If `create_strategy` isn't shown, you forgot `extern "C"` around the factory.

### My strategy exports `create_strategy` but `dlsym` returns `nullptr`

Usually one of:

1. Missing `extern "C"` — C++ name mangling turns it into something like `_Z15create_strategyv`.
2. You exported it from the *executable* not the *.so*.
3. You added `-fvisibility=hidden` and forgot `__attribute__((visibility("default")))` on the factory.

Run `nm -D --defined-only your_strategy.so | grep create` — you should see a plain `create_strategy` symbol.

### Portal upload shows `rejected` with `runner exit 132`

Exit code **132** means the sandboxed runner hit **signal 4 (`SIGILL`)** — an **illegal instruction**. Your `.so` compiled fine on *your* machine but contains CPU instructions the judge box doesn't have.

The usual cause: **`-march=native`**. The shipped `CMakeLists.txt` enables it for Release builds so local benchmarks can use AVX2/AVX-512 on your laptop. The judge runs on a fixed EC2 instance (`c7i.xlarge`, 4 vCPU / 8 GiB, Amazon Linux 2023) — not your CPU.

### `runner exit 132` on every upload (including the shipped sample)

Two different things can produce the same dashboard error. Check which one you have:

1. **Judge-side (organizer fix):** the sandboxed **runner binary** was built with `-march=native` on a CPU that has AVX-512, then the judge VM moved to an instance without it (common on AWS). The runner SIGILLs before your `.so` runs — *every* upload fails, even [`samples/spec_strategy.so`](./samples/spec_strategy.so). Rebuild the judge with `-march=x86-64-v2` (see [`judge/CMakeLists.txt`](../../../judge/CMakeLists.txt)) and restart `csot-judge`.

2. **Student-side (your fix):** your **`spec_strategy.so`** was built with `-march=native` on a newer laptop than the judge EC2 box. Only *your* uploads fail; the shipped sample and other participants' portable builds work. Rebuild with a portable baseline (below).

**Student fix:** rebuild `spec_strategy.so` with a portable baseline before uploading:

```bash
# One-off Release build without -march=native (override the shipped default):
cmake -B build-portable -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_FLAGS_RELEASE="-O2 -march=x86-64-v2 -fno-omit-frame-pointer"
cmake --build build-portable -j --target spec_strategy
```

`-march=x86-64-v2` is conservative (~2009 CPUs, SSE4.2). If you want AVX2 locally *and* on the judge, use `-march=x86-64-v3` instead — still safer than `native`.

Sanity-check the binary before uploading:

```bash
file build-portable/spec_strategy.so    # must say "ELF 64-bit LSB shared object, x86-64"
```

On the dashboard, a failed public gate shows **Public correctness → `runner exit 132`**. That's a load/crash problem, not a strategy divergence — fix the build flags, not the algorithm.

See also [`05-bonus-compiler.md`](../05-bonus-compiler.md) §2 for why `-march=native` and deployment don't mix.

---

## 📊 perf

### `perf: No permission to enable cpu-cycles event`

Kernel paranoia level is too high. Lower it:

```bash
# Temporary (lost on reboot)
sudo sysctl kernel.perf_event_paranoid=1

# Permanent
echo 'kernel.perf_event_paranoid = 1' | sudo tee /etc/sysctl.d/99-perf.conf
sudo sysctl --system
```

Or just run perf with `sudo`.

### `WARNING: perf not found for kernel X.Y.Z`

`perf` is shipped per kernel version. Install the matching package:

```bash
sudo apt install linux-tools-$(uname -r)
```

If that package doesn't exist (common on rolling-release distros), try `linux-tools-generic`.

### `perf stat` shows `<not supported>` for every hardware counter

You're in a VM, container, or WSL2 that doesn't expose PMCs. You can still use:

- `perf stat -e task-clock,context-switches,page-faults` (software counters work everywhere)
- **Cachegrind** for cache stats (simulated, but works anywhere — see `04-benchmarking-tools.md` §8)
- **Callgrind + KCachegrind** for call-graph profiling without PMCs

For real hardware counters on Week 3+, you'll want bare-metal Linux.

### `perf report` shows nothing useful, just hex addresses

You forgot `-g` for call graphs and/or you stripped symbols. Rebuild with `-g` (the shipped CMake's `Debug` and sanitized configs already include it) and re-record:

```bash
perf record -g --call-graph dwarf ./quant_runner ...
perf report
```

### Flame graph is blank or just one function

Frame pointers are missing. The shipped `CMakeLists.txt` sets `-fno-omit-frame-pointer` for Release. Make sure you're building from there. Or use `--call-graph dwarf`:

```bash
perf record -F 999 -g --call-graph dwarf ./quant_runner ...
```

---

## 🧮 Google Benchmark

### My benchmark reports `1 ns` for everything

The compiler eliminated your code. You forgot `benchmark::DoNotOptimize` and/or `benchmark::ClobberMemory`. Wrap any value whose computation you want timed:

```cpp
for (auto _ : state) {
    long sum = std::accumulate(v.begin(), v.end(), 0L);
    benchmark::DoNotOptimize(sum);     // <-- mandatory
}
```

See `04-benchmarking-tools.md` §5.

### My benchmark numbers vary 20–50% between runs

Background noise. Fixes, in order of effectiveness:

```bash
# 1. Pin to a single core (single-thread benches only)
taskset -c 3 ./quant_bench

# 2. Lock the CPU frequency
sudo cpupower frequency-set -g performance

# 3. Disable Intel turbo (Intel)
echo 1 | sudo tee /sys/devices/system/cpu/intel_pstate/no_turbo

# 4. Close your browser, Slack, Spotify, Discord
# 5. Plug in your laptop (battery mode throttles aggressively)
```

For Week 1, items 4 and 5 are 80% of the win.

### Google Benchmark crashes with `Inconsistency detected by ld.so`

You linked `quant_bench` and `quant_runner` against different versions of stdlib or sanitizer runtimes. Most often: ASan + Google Benchmark in the same binary. Fix: build benchmarks without sanitizers.

### Output is just `Time` and no `items/s`

You didn't call `state.SetItemsProcessed(...)` or `state.SetBytesProcessed(...)` inside your benchmark function.

---

## 🧪 Sanitizers

### ASan fires on `_dl_open` or inside system libraries before `main()`

Common when running a build that was linked against ASan but the OS's loader / glibc has known false positives. Usually harmless. Try:

```bash
ASAN_OPTIONS=verify_asan_link_order=0:detect_leaks=0 ./your_program
```

### `LeakSanitizer: detected memory leaks` in std::vector / std::string

That's not a real leak — it's an allocation that wasn't freed before `exit()`. For long-lived globals this is benign. To silence:

```bash
ASAN_OPTIONS=detect_leaks=0 ./your_program
```

But before you do, double-check it really *is* a global. Most "false positive" leaks are real ones in disguise.

### ASan + TSan together → linker error

You can't use both simultaneously. Pick one per build. The shipped CMake has `ENABLE_SANITIZERS=ON` for ASan+UBSan; add a separate `ENABLE_TSAN` if you need it later.

---

## 🐍 Data Generator (`gen.py`)

### `python3: command not found`

`sudo apt install python3` (or use `python` on systems where it's still Python 3).

### `gen.py` runs but my CSV is empty

You probably passed `--out` to a path that's a directory or unwritable. Check:

```bash
python3 data/gen.py --rows 100 --out /tmp/test.csv && head /tmp/test.csv
```

### Two students compare numbers and they're different despite same seed

Different `--rows`, different `--out` (doesn't matter), or you edited `gen.py`. The seed only guarantees identical output for **identical arguments and unchanged source**. Run `diff gen.py upstream-gen.py` to make sure.

---

## 📄 CSV Loader

### "Every tick has the same symbol" / "Symbols are garbage after loading"

You pointed `Tick::symbol` at memory that didn't outlive the load loop. Classic mistake:

```cpp
std::string line;
while (std::getline(file, line)) {
    ticks.push_back({ .symbol = line, ... });  // all views alias one buffer
}
```

Fix: **intern symbols into loader-owned storage** that stays alive until replay finishes. Keep each distinct symbol name once (e.g. `std::deque<std::string>` + a map from name → `string_view`), then set `tick.symbol` to a view into that stable string. Re-read `ticks[0].symbol` after loading 10 000 rows — if it isn't still `"SYM0"`, your storage is wrong.

See `strategy.hpp`: the engine owns the bytes; during Week 1 that "engine" is your loader.

### "My loader segfaults at the end of the file"

Almost always a missing EOF check inside your line-parsing loop, or `std::getline` returning an empty line that you treat as a tick. Test on the 20-row `data/tiny.csv` first — segfaults there are much easier to debug than at row 9 999 999.

### "First tick's symbol is `\xef\xbb\xbftimestamp_ns`"

Your file has a UTF-8 BOM at the start. Either:

- Strip it: `sed -i '1s/^\xEF\xBB\xBF//' data/your.csv`
- Or skip the first 3 bytes when you open the file.

The shipped `gen.py` does NOT emit a BOM.

### "My loaded `bid_px` values are wrong by ~1e-10"

That's just `double` precision noise. The CSV stores 4 decimal places; the binary representation is approximate. Compare with `std::abs(got - want) < 1e-6` instead of `==`.

### "My CSV loader is the bottleneck of my whole program"

That's normal in Week 1 and totally fine — the **point** of Week 1 is the per-tick latency *inside the loop*, not the load time. If it bothers you, Week 1 stretch goal: `mmap` the file.

---

## 📈 Strategy / Engine

### "My moving-average strategy never produces any orders"

Most likely:

1. Your MA window is larger than the number of ticks per symbol you've actually seen (the generator rotates symbols, so 10 000 rows = ~2 500 per symbol).
2. Your threshold is too large for the synthetic data's volatility (`sigma = 0.0002` by default → moves of ~0.02% per tick).

Try a window of 20 and a threshold of `0.001 * price` to see *something* trigger.

### "My p50 latency is 50 ns. Is that right?"

For a no-op strategy: yes, that's the timer overhead of `std::chrono::steady_clock::now()` itself. Switch to `rdtsc` (calibrated) to push that down to ~5-10 ns. A real strategy on top of that adds 50–200 ns typically — depending on what you do.

### "My p99 is 10 000x my p50"

You're being descheduled by the kernel mid-tick. Two fixes:

1. Pin to one isolated core: `taskset -c 3 ./quant_runner`
2. Boot with `isolcpus=3` to keep all other processes off core 3.

Item 1 alone usually drops the gap by 10–100×.

---

## 🔍 Valgrind

### `valgrind: error: ‘--tool=cachegrind’ unrecognized`

Either `valgrind` isn't installed, or you have an extremely old version. `sudo apt install valgrind` and check `valgrind --version` — anything 3.18+ is fine.

### Valgrind reports leaks from `dlopen`-ed `.so` symbols

This is usually a real leak in the strategy. But if it complains about `libdl` itself, that's a known false positive — suppress with:

```bash
valgrind --suppressions=/usr/lib/valgrind/default.supp ./quant_runner ...
```

### `cg_annotate` output is mostly `???:???`

You ran Cachegrind on a binary built without `-g`. Rebuild with debug info (any of `Debug`, `RelWithDebInfo`, or just `-g`) and re-run.

---

## 🪟 Platform-Specific

### WSL2: `perf` doesn't work / shows fake numbers

Known limitation — WSL2's kernel doesn't expose PMCs. Use Cachegrind/Callgrind instead, or dual-boot Linux. By Week 3, you'll really want native Linux.

### macOS: `perf` not found

It doesn't exist on macOS. Use **Instruments.app** (comes with Xcode), or run Linux in a VM. By Week 5 (networking with `epoll`), you'll need real Linux either way.

### macOS Apple Silicon: Valgrind doesn't work

Valgrind has limited / experimental ARM64-macOS support. Use **leaks** (built-in, `leaks --atExit -- ./your_program`) or AddressSanitizer.

### Windows (native, not WSL): I can't get the project to build

Use WSL2 — it's free and 5 minutes to set up. We don't support native Windows builds in this curriculum.

---

## 🆘 When Nothing Works

Try the **golden file** first:

```bash
./quant_runner ./null_strategy.so data/tiny.csv
```

If even that 20-row file produces wrong output or crashes, the problem is in your engine/loader — not your strategy, and not your CSV.

Beyond that, the standard debugging escalation:

1. Reproduce with the smallest possible input (start with `tiny.csv`).
2. Re-run under `-fsanitize=address,undefined` (`-DENABLE_SANITIZERS=ON` in the shipped CMake).
3. If it segfaults, get a backtrace: `gdb --batch --ex run --ex bt ./your_program <args>`.
4. If it's a logic bug, add prints. Don't be ashamed — printf debugging is undefeated.
5. If still stuck, **post the full command, full error, your OS, and the smallest reproducing input** to the CSoT group.

You'll get unstuck. Everyone does. 🚀
