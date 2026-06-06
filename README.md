# ⚡ Low Latency Track — CSoT'26

> **"In trading, the speed of light is a real bottleneck. So is your cache."**

Welcome to the **Low Latency** track of CSoT'26. Over 5 weeks, you'll learn how to write code that *truly* respects the machine — code that exploits the memory hierarchy, avoids stalls, dodges the kernel, and parallelizes safely. By the end, you'll have built a **mini quantitative trading platform** that pulls market-shaped data over the network, runs a judge-defined strategy spec, and competes on a real-time leaderboard ranked by latency.

This is the curriculum that high-frequency trading firms, game engine developers, database engineers, and embedded systems folks live by.

---

## 🎯 Who Is This For?

- Students comfortable with **C/C++ basics** (you've written a `for` loop, used `std::vector`, compiled a program).
- Anyone curious about **how computers *really* work** under the abstractions.
- Future Inter-IIT contingent members interested in **systems / HFT / quant** problems.

You do **NOT** need prior experience with concurrency, networking, or assembly. We start from zero.

---

## 🧰 Prerequisites & Setup

| Tool | Why | How |
|------|-----|-----|
| **Linux** (Ubuntu 22.04+ / WSL2 / Arch) | All low-level tooling assumes Linux | Native install or WSL2 |
| **g++ ≥ 11** or **clang++ ≥ 14** | C++20 support | `sudo apt install build-essential` |
| **CMake ≥ 3.20** | Build system | `sudo apt install cmake` |
| **perf** | CPU profiling | `sudo apt install linux-tools-common linux-tools-generic` |
| **Google Benchmark** | Microbenchmarks | We'll install in Week 1 |
| **Python 3** | Plotting results / market data scripts | `sudo apt install python3 python3-pip` |
| **Git** | Version control | `sudo apt install git` |

We strongly recommend **bare-metal Linux** (dual boot) over WSL or a VM — virtualization layers add noise that ruins benchmarks. macOS works for most of the C++ but `perf`, `epoll`, and `sched_setaffinity` are Linux-only.

---

## 🗺️ The 5-Week Roadmap

### **Week 1 — Introduction & Memory**
Foundations: what is latency, what is throughput, how is memory laid out, and how do we *measure* anything reliably? You'll set up the toolchain, learn to read a benchmark, and start the quant-platform project skeleton.

📂 [`week-1/`](./week-1/) — **available now**

- Latency vs. throughput
- Memory as an addressable contiguous array
- `std::vector` / `std::array` and why contiguous matters
- Stack vs. heap
- Bypassing `malloc`: arena & bump allocators
- Memory hierarchy: L1 / L2 / L3 / DRAM
- Benchmarking tools (`perf`, Google Benchmark, `rdtsc`)

**Project:** Bootstrap the quant platform. Build an API runner that loads your implementation of the fixed strategy spec (`.so` or compiled binary), feeds it a tick stream, verifies correctness, measures latency per decision, and prepares for a Prosperity-style latency leaderboard.

---

### **Week 2 — Caches, Locality & Zero-Allocation**
The fastest allocation is the one you never make. The fastest cache miss is the one your data layout never causes.

📂 [`week-2/`](./week-2/) — **available now**

- Hot path vs. cold path (`on_init` vs. the timed `run()`)
- Cache internals: lines, index/offset/tag, sets, ways, LRU, write-back, write-allocate, inclusion
- Spatial & temporal locality, AoS vs. SoA (the 10× layout experiment)
- Zero dynamic allocation patterns (object pools, arenas, fixed buffers)
- `constexpr` / `consteval` — fold cache geometry at compile time
- Static polymorphism with templates / CRTP (no vtables on the hot path!)
- ⭐ Bonus: branchless / SIMD tag scan, prefetch, packed LRU

**Project:** A new **ranked challenge** — a COL216-style two-level **cache simulator**. You implement a single `cache_sim.cpp` against a frozen [`CACHE_SPEC.md`](./week-2/project/CACHE_SPEC.md); the judge builds it itself (fixed flags) and ranks correct simulators by **wall-clock speed** over a huge hidden memory trace. New this week: you submit **source**, not a compiled `.so`.

---

### **Week 3 — Parallelization & Thread Friction**
Going wide. But threads aren't free — they fight over caches, get pre-empted by the kernel, and corrupt each other's data.

- Horizontal scaling & embarrassingly parallel workloads
- `std::thread` basics
- Data races (deliberately introduced — watch them break things)
- False sharing & `alignas(64)`
- OS context switches and scheduler jitter
- Pinning threads to cores with `sched_setaffinity`

**Project:** Embarrassingly-parallel exercises, an intentional false-sharing benchmark, and the start of parallelizing the quant platform.

---

### **Week 4 — Lock-Free Concurrency**
Mutexes put threads to sleep. In low-latency land, sleep is death.

- Mutex overhead and futex wake-ups
- `std::atomic`, memory orderings (`relaxed`, `acquire`, `release`, `seq_cst`)
- Compare-and-swap (CAS) loops
- Single-Producer / Single-Consumer (SPSC) ring buffers
- ABA problem (intro)

**Project:** Replace mutex-based queues in the quant platform with a lock-free SPSC ring buffer between the market-data thread and the strategy thread.

---

### **Week 5 — The Network Gateway**
The market data comes in over the wire. The order goes out over the wire. The wire is now the bottleneck.

- Berkeley sockets recap (`socket`, `bind`, `connect`, `send`/`recv`)
- Blocking vs. non-blocking I/O
- Raw socket polling
- Event loops with `epoll`
- TCP vs. UDP — when to use which
- (Bonus) Kernel-bypass teaser: `AF_XDP`, DPDK, Solarflare

**Project:** Connect a real-world stock-data feed to the platform over TCP/UDP. Profile end-to-end latency from wire-arrival to order-decision using network profilers (`tcpdump`, `bpftrace`).

---

## 🏆 Final Deliverable

A **fully functional low-latency quant platform** with:

- ✅ A C++ strategy-runner API
- ✅ A fastest-possible correct implementation of the reference strategy
- ✅ Zero-allocation hot path
- ✅ Multi-threaded market-data and execution pipelines
- ✅ Lock-free SPSC queues between threads
- ✅ Network ingest over TCP/UDP with `epoll`
- ✅ End-to-end latency benchmarks (tick → decision)
- ✅ A live leaderboard ranked by latency and throughput, not by trading performance

This is genuinely a **CV-worthy project**. Quant firms ask candidates to build pieces of this in their interview loops.

---

## 📐 How Each Week Is Structured

Each `week-N/` folder contains:

```
week-N/
├── README.md             ← week overview + checklist + general talks/books/courses
├── 0X-topic.md           ← bite-sized concept notes (each ends with topic-specific "Further Reading")
└── project/              ← the week's project + starter code
    └── README.md
```

**How to use it:**
1. Read `README.md` first — it tells you the *order* to read the topic files in, and contains the week's checklist.
2. Read each `0X-topic.md` in order. They are short — 10-15 min each. Each topic file has a curated "Further Reading" section at the end — skim it and pick **one** link that matches your learning style.
3. Build the project. **This is non-negotiable.** Reading without building = forgetting in a week.

---

## 💡 Philosophy

> *"Premature optimization is the root of all evil."* — Donald Knuth
>
> *"...yet we should not pass up our opportunities in that critical 3%."* — also Donald Knuth, same sentence, conveniently forgotten

Low-latency engineering is **not** about micro-optimizing every loop. It's about:

1. **Knowing what's slow** (measurement > intuition, *always*).
2. **Understanding the machine** so the slow things make sense.
3. **Designing systems** where the hot path is naturally fast.

You will write code in this track that is *slower* than `std::unordered_map`. You will also write code that is *100x faster* than `std::unordered_map`. The difference is knowing **which path your data flows through**.

---

## 📬 Stuck?

- Track-specific doubts → ping the track leads on the CSoT group.
- Bug in the docs → open an issue / PR on this repo.
- Existential dread about pointer arithmetic → that's normal, keep going.

Let's go fast. 🏎️💨
