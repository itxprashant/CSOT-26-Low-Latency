# 05 — Bonus: The Compiler Is Your Best Friend (and Worst Enemy)

> **⭐ This is bonus content for Week 1.** You don't need it to do the project — but if you read it, you'll get *significantly* more out of every benchmark you run, and you'll have a head start on Week 2 (where compile-time computation is the whole point).

> **TL;DR** — The C++ source you write is just a *suggestion*. The compiler — given the right flags — will inline, vectorize, unroll, hoist, fold constants, eliminate dead code, and rearrange your program until it bears almost no resemblance to what you wrote. Knowing how to talk to it is half of low-latency engineering.

---

## 1. Optimization Levels — Pick the Right `-O`

Every benchmark you've ever read that didn't specify an optimization level was a lie. Here's what each flag means in practice:

| Flag | What it does | When to use |
|---|---|---|
| `-O0` | No optimization. Every variable on the stack, every step preserved. | Debugging step-by-step in `gdb`. |
| `-O1` | Basic local optimizations (constant folding, dead store elimination). Code is still readable in `gdb`. | Light debugging where `-O0` is too slow. |
| `-O2` | The serious default. Inlining, loop optimizations, vectorization, instruction scheduling. | **Production. Always.** |
| `-O3` | `-O2` + aggressive vectorization + more aggressive inlining + loop unrolling. | Try it, benchmark vs `-O2`. Often faster, occasionally slower (binary bloat → I-cache pressure). |
| `-Os` | Optimize for **size**. | Embedded, or when I-cache misses dominate. |
| `-Ofast` | `-O3` + `-ffast-math` (breaks IEEE 754 strictness, allows reordering FP ops). | Numerical code that doesn't need bit-exact floating-point behaviour. Avoid it here — correctness depends on reproducible decisions. |
| `-Og` | Optimize but keep debugging usable. | Daily dev builds. |

### The Trap

If you see a benchmark result that's *too good to be true* with `-O3`, the compiler probably eliminated your code entirely. This is the most common beginner mistake. Use `benchmark::DoNotOptimize` (or look at the assembly) to confirm.

```cpp
// At -O2, this whole function is replaced with `return 499999500000;`
long sum_to_n(long n) {
    long s = 0;
    for (long i = 0; i < n; ++i) s += i;
    return s;
}
// At -O3 with constant n=1'000'000, it becomes `return <literal>;` at the call site.
```

**Always benchmark with the flags you'll ship with.** Don't develop on `-O0` and ship `-O3` — your perf intuitions will be wrong.

---

## 2. Tell the Compiler About Your Hardware

By default, both GCC and Clang compile for a generic x86-64 baseline (~2003 CPU). They emit no AVX2, no FMA, no BMI2 — none of the goodies your CPU has had for a decade.

```bash
# Tell the compiler: "use everything THIS machine has"
g++ -O2 -march=native -mtune=native myprog.cpp -o myprog
```

- **`-march=native`** — emit instructions specific to the build machine's CPU (AVX2, AVX-512, etc.).
- **`-mtune=native`** — schedule instructions for this CPU's pipeline (no ISA change, just ordering).

> **Trap:** binaries built with `-march=native` may **crash with `SIGILL`** on other CPUs (illegal instruction). Fine for local benchmarks. For deployment, pick a baseline like `-march=x86-64-v3` (= ~2015 CPUs, includes AVX2).

To see what `-march=native` *actually* enables on your box:

```bash
g++ -march=native -E -v - </dev/null 2>&1 | grep -- '-march=\|cc1'
# or, for the full feature list:
g++ -march=native -Q --help=target | grep enabled
```

For low-latency work on a known production CPU, prefer an **explicit `-march=`** (e.g. `-march=skylake-avx512`) over `native` — reproducibility matters.

---

## 3. Link-Time Optimization (LTO)

Normal compilation translates each `.cpp` to a `.o` independently — the compiler can only inline functions defined in the **same translation unit**. LTO defers that final optimization step to link time, with the whole program visible.

```bash
g++ -O2 -flto myprog.cpp helper.cpp -o myprog
```

Typical win: **5-15% speedup**, especially for codebases with many small functions split across files. Cost: longer link times, occasionally surfaces latent bugs.

Variants:
- `-flto` — classic LTO.
- `-flto=thin` (Clang) — parallel, faster, slightly less optimization.
- `-flto=auto` (GCC) — uses all cores.

Pair LTO with `-fno-fat-lto-objects` for smaller intermediate files.

---

## 4. Profile-Guided Optimization (PGO)

PGO is the "give the compiler a profiler's worth of insight" trick. You build twice:

1. **Instrumented build** — records branch frequencies, hot functions, etc. while running a realistic workload.
2. **Optimized build** — re-compile using that profile. The compiler inlines hot paths, lays out branches so the common case falls through, and demotes cold functions.

```bash
# Step 1: build with instrumentation
g++ -O2 -fprofile-generate=./pgo-data myprog.cpp -o myprog
./myprog < representative_workload.dat        # run it on REAL data
# (this populates ./pgo-data/)

# Step 2: rebuild using the profile
g++ -O2 -fprofile-use=./pgo-data myprog.cpp -o myprog
```

Typical win on real codebases: **another 5-20% on top of LTO**. Used by Chrome, Firefox, MySQL, ClickHouse, every serious DB.

Clang's equivalent uses `-fprofile-instr-generate` / `-fprofile-instr-use`.

**Even better:** **AutoFDO** (GCC `-fauto-profile`, Clang `-fprofile-sample-use`) uses `perf` samples as the profile — no instrumented build needed, you just `perf record` a production binary.

---

## 5. The Sanitizers — Free Bug Finders Built into the Compiler

These add runtime checks that catch bugs Memcheck would find, but with 2-5x slowdown instead of 20x. Use them constantly in debug builds and CI.

| Sanitizer | Flag | Catches |
|---|---|---|
| **AddressSanitizer** | `-fsanitize=address` | Buffer overflows, use-after-free, double-free, leaks |
| **UndefinedBehaviorSanitizer** | `-fsanitize=undefined` | Signed overflow, null deref, OOB shifts, misaligned loads |
| **ThreadSanitizer** | `-fsanitize=thread` | Data races (Week 3!) |
| **MemorySanitizer** | `-fsanitize=memory` (Clang only) | Reads of uninitialized memory |
| **LeakSanitizer** | `-fsanitize=leak` (included in ASan) | Memory leaks at exit |

```bash
g++ -O1 -g -fsanitize=address,undefined myprog.cpp -o myprog
./myprog       # crashes loudly on the first bug, with a full stack trace
```

> **Rule of thumb:** every C++ project you write in this track should have a CMake build option to enable `-fsanitize=address,undefined` and you should run the test suite under it at least weekly. It's the cheapest insurance in software engineering.

**Do not combine ASan with `-fsanitize=thread`** — they conflict. Pick one per build.

---

## 6. Compiler Hints — When You Know Something the Compiler Doesn't

The compiler does a lot for you, but sometimes you have information it can't infer (e.g. "this branch is taken 99.9% of the time"). Use these sparingly — measure first.

### `[[likely]]` / `[[unlikely]]` (C++20)

Hints to the branch predictor / code layout.

```cpp
if (error_condition) [[unlikely]] {     // cold path
    handle_error();
    return;
}
process_normally();                     // hot path falls through
```

Effect: the compiler arranges code so the hot path is contiguous (better I-cache), and CPU branch prediction starts off in the right direction.

### `__builtin_expect` (pre-C++20 / for older compilers)

```cpp
if (__builtin_expect(error_condition, 0)) { ... }   // expect 0 = false
```

Same idea, uglier syntax.

### `__attribute__((always_inline))` and `((noinline))`

Force inlining decisions.

```cpp
[[gnu::always_inline]] inline void hot_helper() { ... }    // ALWAYS inline
[[gnu::noinline]]      void cold_helper() { ... }          // NEVER inline (great for profiling — see real call counts)
```

`always_inline` is genuinely useful in hot paths where the compiler is being too conservative. `noinline` is useful when:
- You want a clear call boundary in `perf` / Callgrind output.
- The function is huge and inlining it would bloat the caller.

### `__restrict__` — "These Pointers Don't Alias"

A massive optimizer hint for numerical code:

```cpp
void axpy(int n, float a, const float* __restrict__ x, float* __restrict__ y) {
    for (int i = 0; i < n; ++i) y[i] += a * x[i];
}
```

You're promising the compiler that `x` and `y` don't overlap. It can now reorder loads/stores aggressively (and auto-vectorize). Lie to the compiler about this and you'll get *spectacular* miscompiles. Use only when you're sure.

### `__builtin_prefetch` — Ask for Data Before You Need It

```cpp
for (size_t i = 0; i < n; ++i) {
    __builtin_prefetch(&data[i + 16]);    // start fetching 16 ahead
    process(data[i]);
}
```

For pointer-chasing or strided workloads where the hardware prefetcher can't help. Often does **nothing** — the hardware prefetcher is very good. Measure!

### `[[noreturn]]` and `assume`

```cpp
[[noreturn]] void fatal(const char* msg);    // helps compiler eliminate dead code after calls

// C++23:
[[assume(x > 0)]];                            // tell optimizer x is always positive
```

---

## 7. Watch What the Compiler Actually Does — Godbolt

[**Compiler Explorer (godbolt.org)**](https://godbolt.org) is the single most useful website for a low-latency engineer. Paste C++, see assembly *as you type*, across every compiler and flag combination.

Concrete experiments to try right now:

### Experiment 1 — Constant folding

```cpp
int compute() {
    int x = 7;
    int y = 13;
    return x * x + y * y;
}
```

At `-O2`: the entire function compiles to `mov eax, 218; ret`. The arithmetic happened *at compile time*.

### Experiment 2 — Auto-vectorization

```cpp
void add(float* a, const float* b, int n) {
    for (int i = 0; i < n; ++i) a[i] += b[i];
}
```

Compile with `-O3 -march=native`. The hot loop will use `vaddps ymm` (AVX2, 8 floats per instruction) or `zmm` (AVX-512, 16 floats). One instruction does what 16 scalar adds would. **For free**, just by writing the natural code.

### Experiment 3 — `std::sort` vs `qsort`

```cpp
#include <algorithm>
#include <cstdlib>

void cpp_sort(int* a, int n) { std::sort(a, a+n); }

int cmp(const void* x, const void* y) { return *(int*)x - *(int*)y; }
void c_sort(int* a, int n) { qsort(a, n, sizeof(int), cmp); }
```

At `-O2`, `std::sort` inlines its comparator and becomes ~2-3x faster than `qsort` (which has to *call through a function pointer* for every comparison). This is one of the cleanest demos of why **template-based static polymorphism beats virtual functions** — a Week 2 topic.

### Experiment 4 — Virtual calls vs `final`

```cpp
struct Base { virtual int f() = 0; };
struct Derived final : Base { int f() override { return 42; } };

int call(Derived* d) { return d->f(); }    // de-virtualized at -O2 because of `final`
int call(Base* b)    { return b->f(); }    // real vtable lookup
```

The `final` keyword (on a class or method) lets the compiler prove there's only one possible implementation and inline through it. Free perf win.

### Experiment 5 — `std::vector::push_back` reveals itself

```cpp
#include <vector>
void f(std::vector<int>& v, int x) { v.push_back(x); }
```

You'll see the fast path (`size < capacity`) inlined as a few instructions, and the slow path (reallocation) hidden behind a branch. This is *why* you call `v.reserve(N)` before a known-size loop.

> **Make Godbolt a daily habit.** Every time you write a hot loop, look at its assembly. After 50 reps, you'll start *predicting* the assembly before you compile.

---

## 8. Useful Diagnostic Flags

GCC and Clang can tell you what they did (and didn't do):

```bash
# Did the loop vectorize?
g++ -O3 -march=native -fopt-info-vec-all myprog.cpp 2>&1 | grep -v "not vectorized"

# Why didn't it vectorize?
g++ -O3 -march=native -fopt-info-vec-missed myprog.cpp

# What got inlined?
g++ -O2 -fopt-info-inline-optimized myprog.cpp

# Everything (firehose):
g++ -O2 -fopt-info-all myprog.cpp 2>&1 | less
```

Clang's equivalent:

```bash
clang++ -O3 -march=native -Rpass=loop-vectorize myprog.cpp
clang++ -O3 -march=native -Rpass-missed=loop-vectorize myprog.cpp
clang++ -O3 -march=native -Rpass-analysis=loop-vectorize myprog.cpp
```

These tell you *exactly* which loops vectorized and *why others didn't*. Eye-opening on real code.

---

## 9. Warning Flags Every Project Should Have

Warnings are bugs you haven't found yet. Crank them up:

```bash
g++ -Wall -Wextra -Wpedantic \
    -Wshadow -Wnon-virtual-dtor \
    -Wold-style-cast -Wcast-align \
    -Wunused -Woverloaded-virtual \
    -Wconversion -Wsign-conversion \
    -Wnull-dereference -Wdouble-promotion \
    -Wformat=2 \
    -Werror \              # warnings → errors. yes, really.
    myprog.cpp
```

`-Werror` in CI forces you to keep the warning list at zero. It is the single highest-ROI policy you can adopt.

For really paranoid mode (great for new code): add `-Weverything` (Clang) and silence the ones you disagree with.

---

## 10. Low-Latency-Specific Build Flags

A few flags that come up in HFT codebases:

| Flag | What | Why |
|---|---|---|
| `-fno-exceptions` | No exception support | Exceptions cost binary size and inhibit some optimizations. Many HFT shops use them. |
| `-fno-rtti` | No `dynamic_cast` / `typeid` | Smaller binaries; faster vtables. |
| `-fno-plt` | No PLT indirection for shared libs | Cuts one indirect call per cross-module call. |
| `-fno-semantic-interposition` | Allow inlining across shared-library boundaries | Big win for `.so` builds. |
| `-fvisibility=hidden` | Hide symbols by default; export only with `__attribute__((visibility("default")))` | Smaller binary, more inlining opportunities. |
| `-falign-functions=64` | Align hot functions to cache lines | Prevents one function spanning two I-cache lines. |
| `-mno-vzeroupper` | (Skylake/AVX-512) avoid the implicit `vzeroupper` after AVX use | Micro-saving on tight AVX loops. |
| `-fno-stack-protector` | Disable stack canaries | A few cycles per function entry. Trade safety for speed. |
| `-fno-omit-frame-pointer` | Keep frame pointers | Costs one register, makes `perf` call-graphs much better. **Worth it.** |

> ⚠️ Many of these are **trade-offs**. `-fno-exceptions` means you can't use any STL feature that throws (`vector::at()`, `std::stoi`, etc.). `-fvisibility=hidden` can break shared library users. Read the manuals before flipping any of these in a real project. **Measure, don't cargo-cult.**

---

## 11. A Sane CMake Template

Drop this in your Week 1 project's `CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.20)
project(quant_platform CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Default to Release if not specified
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Strong warnings
add_compile_options(
    -Wall -Wextra -Wpedantic
    -Wshadow -Wnon-virtual-dtor
    -Wcast-align -Wunused
    -Woverloaded-virtual -Wnull-dereference
)

# Tuning
add_compile_options($<$<CONFIG:Release>:-O2>)
add_compile_options($<$<CONFIG:Release>:-march=native>)
add_compile_options($<$<CONFIG:Release>:-fno-omit-frame-pointer>)   # for perf

# Sanitized debug build
option(ENABLE_SANITIZERS "Build with ASan + UBSan" OFF)
if(ENABLE_SANITIZERS)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,undefined)
endif()

# Optional LTO for release
option(ENABLE_LTO "Enable link-time optimization for Release" ON)
if(ENABLE_LTO AND CMAKE_BUILD_TYPE STREQUAL "Release")
    include(CheckIPOSupported)
    check_ipo_supported(RESULT lto_ok OUTPUT lto_err)
    if(lto_ok)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
    else()
        message(WARNING "LTO not supported: ${lto_err}")
    endif()
endif()

add_executable(quant_runner src/main.cpp src/engine.cpp)
```

Usage:

```bash
cmake -B build-release -DCMAKE_BUILD_TYPE=Release
cmake -B build-debug   -DCMAKE_BUILD_TYPE=Debug -DENABLE_SANITIZERS=ON
cmake --build build-release -j
cmake --build build-debug -j
```

Run the sanitized build on every test. Benchmark the release build.

---

## 12. Reading Assembly Without Tears

You don't need to write x86 assembly. You *do* need to *read* enough to recognize:

| What you see | What it means |
|---|---|
| `mov`, `add`, `sub`, `imul` | Scalar arithmetic on registers |
| `cmp`, `je`, `jne`, `jl`, `jg` | Compare and conditional jump |
| `call`, `ret` | Function call (`call` ≠ inlined, `ret` = return) |
| `lea` | Address arithmetic, often used for fast multiply by 3/5/9 |
| `mov rax, QWORD PTR [rdi + 8]` | Load 8 bytes from memory at address `rdi+8` |
| `vaddps ymm0, ymm1, ymm2` | AVX2: add 8 floats in parallel |
| `vaddpd zmm0, zmm1, zmm2` | AVX-512: add 8 doubles in parallel |
| `xor rax, rax` | Zero out `rax` (idiom — 1 byte shorter than `mov rax, 0`) |
| `nop` / `xchg ax, ax` | No-op (often padding for alignment) |
| `lock cmpxchg` | Atomic compare-and-swap (Week 4!) |
| `mfence` / `lfence` / `sfence` | Memory barriers (Week 4) |

You can read 95% of GCC output knowing only this. The rest you Google when it shows up.

---

## 13. The Mental Model

```
       ┌────────────────────────────────────────────────────────────┐
       │  Your C++ source                                           │
       │  "Here's roughly what I want the machine to do."           │
       └────────────────────────┬───────────────────────────────────┘
                                │
                                ▼      flags: -O2 -march=native -flto ...
       ┌────────────────────────────────────────────────────────────┐
       │  The compiler                                              │
       │  Inline. Vectorize. Unroll. Constant-fold. Hoist. Reorder. │
       │  Eliminate dead code. Pick registers.                      │
       └────────────────────────┬───────────────────────────────────┘
                                │
                                ▼
       ┌────────────────────────────────────────────────────────────┐
       │  Machine code (what actually executes)                     │
       └────────────────────────────────────────────────────────────┘
```

Your job is not to outsmart the compiler. Your job is to:
1. Write code in a shape the compiler *can* optimize (no aliasing, no opaque function calls in the hot loop, no dynamic allocation).
2. **Verify** with Godbolt / `perf annotate` that it actually did.
3. Drop hints (`[[likely]]`, `__restrict__`, `final`) only when measurement says it helps.

That's it. That's compiler-aware programming.

---

## 🎯 Key Takeaways

- **`-O2 -march=native`** is your default. Always benchmark with the flags you ship.
- **LTO** is a free 5-15% on top of `-O2`. Turn it on.
- **PGO / AutoFDO** is another 5-20% if you have a realistic workload to profile. Worth it for hot binaries.
- **Sanitizers** (`ASan`, `UBSan`, `TSan`) are the cheapest bug insurance in the world. Use them in every debug build.
- **Godbolt** is mandatory equipment. Look at the assembly your hot loops compile to.
- **Compiler hints** (`[[likely]]`, `__restrict__`, `final`, `always_inline`) are spices, not main courses. Use sparingly, after measuring.
- **`-Werror`** in CI keeps the warning list at zero — the highest-ROI policy in software.
- **Frame pointers (`-fno-omit-frame-pointer`)** make `perf` and flame graphs vastly more useful. Keep them in production.

---

## 📚 Further Reading

### Interactive Tools (open these now, use them forever)

- 🌐 **[Compiler Explorer (godbolt.org)](https://godbolt.org/)** — see your C++ become assembly *live*, across every compiler and flag combo. Mandatory equipment.
- 🌐 [Quick Bench (quick-bench.com)](https://quick-bench.com/) — Google Benchmark in the browser; great for sharing snippets.
- 📖 [Intel Intrinsics Guide](https://www.intel.com/content/www/us/en/docs/intrinsics-guide/index.html) — search any SIMD intrinsic by name or by what it does.

### Compiler Internals — Talks

- 🎬 **[Matt Godbolt — "What Has My Compiler Done For Me Lately?"](https://www.youtube.com/watch?v=bSkpMdDe4g4)** (CppCon 2017) — the canonical talk by Godbolt's creator.
- 🎬 [Chandler Carruth — "Understanding Compiler Optimization"](https://www.youtube.com/watch?v=haQ2cijhvhE) (Bloomberg Lab 2019).
- 🎬 [Chandler Carruth — "There Are No Zero-Cost Abstractions"](https://www.youtube.com/watch?v=rHIkrotSwcc) (CppCon 2019) — sobering.

### Compiler Internals — Reading

- 📖 **[Agner Fog — "Optimizing software in C++"](https://www.agner.org/optimize/optimizing_cpp.pdf)** — free, 200+ pages, encyclopedic.
- 📖 [Agner Fog — full optimization manuals](https://www.agner.org/optimize/) — instruction tables, micro-architecture details, calling conventions. The reference.
- 📖 [GCC Optimization Options manual](https://gcc.gnu.org/onlinedocs/gcc/Optimize-Options.html) — every flag, what it does.
- 📰 [Clang `-Rpass=` vectorization diagnostics](https://llvm.org/docs/Vectorizers.html#diagnostics) — *why didn't my loop vectorize?*
- 📰 [Krister Walfridsson — "How LLVM Optimizes a Function"](https://kristerw.github.io/2022/10/27/llvm-optimizations/) — one example, every pass, step-by-step.
- 📰 [Sanitizers reference (Google)](https://github.com/google/sanitizers/wiki) — ASan, TSan, MSan, UBSan, LeakSan.
- 📰 [PGO with Clang — official guide](https://clang.llvm.org/docs/UsersManual.html#profile-guided-optimization).

---

## ▶️ What's Next

That's the end of Week 1 bonus content. You now have:
- A **reading-comprehension level** of compiler internals.
- A real CMake template you can paste into your project.
- The Godbolt habit — start using it daily.

These ideas will explode in **Week 2**, where we use `constexpr` / `consteval` to push entire computations to compile time, use templates for zero-overhead polymorphism, and write code that the compiler can prove allocates nothing. See you there. ⚡
