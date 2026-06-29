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

📂 [`week-3/`](./week-3/) — **available now**

- Horizontal scaling & embarrassingly parallel workloads, the map-reduce shape, Amdahl's law
- `std::thread` / `std::jthread` basics and partitioning work
- Data races (deliberately introduced — watch them break things), caught with ThreadSanitizer
- False sharing & `alignas(64)`
- OS context switches and scheduler jitter
- Pinning threads to cores with `sched_setaffinity`

**Project:** A new **ranked challenge** — a **parallel tick aggregator**. You implement a single `aggregator.cpp` against a frozen [`AGG_SPEC.md`](./week-3/project/AGG_SPEC.md) that reduces a huge tick stream into a fixed per-symbol table of integer aggregates; the judge builds it itself (fixed flags, `-pthread`), runs it several times, and ranks **correct and deterministic** aggregators by **wall-clock speed** over a huge hidden stream. The integer aggregates are partition-independent, so the answer is the same at 1 or 8 threads — which is exactly what makes the leaderboard fair and a data race detectable.

---

### **Week 4 — Lock-Free Concurrency**
Mutexes put threads to sleep. In low-latency land, sleep is death.

📂 [`week-4/`](./week-4/) — **available now**

- Pipeline vs. partition; the overlap ceiling `(D+S)/max(D,S)`; why a mutex handoff loses
- `std::atomic`, read-modify-write, why `volatile` is not atomic, the release/acquire handshake
- Memory orderings (`relaxed`, `acquire`, `release`, `seq_cst`) and exactly which ones SPSC needs
- Building a lock-free **Single-Producer / Single-Consumer (SPSC) ring buffer**: power-of-two masking, `head`/`tail` on separate cache lines, cached indices, batching
- Pinning both threads, spin-based back-pressure, balancing the stages
- ⭐ Bonus: batching, the LMAX Disruptor, MPSC/MPMC, the bridge to network ingest

**Project:** A new **ranked challenge** — a **lock-free pipeline**. You implement a single `pipeline.cpp` against a frozen [`PIPELINE_SPEC.md`](./week-4/project/PIPELINE_SPEC.md): a producer thread decodes a binary tick feed into `csot::Tick`s and hands them across **your lock-free SPSC ring buffer** to a consumer thread running the **unchanged Week-1 z-score strategy**, emitting an order stream. The judge builds it itself (fixed flags, `-pthread`), runs it several times, and ranks **correct and deterministic** pipelines by **wall-clock throughput** over a huge hidden feed. The strategy is a pure function of the in-order tick sequence, so the answer is identical no matter how the two threads interleave — which is exactly what makes the leaderboard fair and a handoff race detectable.

---

### **Week 5 — The Network Gateway (capstone)**
The market data comes in over the wire. The wire is now the bottleneck — and a `recv()` is a syscall, ~100× a memory load.

📂 [`week-5/`](./week-5/) — **available now**

- Berkeley sockets recap; the kernel boundary as the new hot path (`recv`/`send` are syscalls)
- Blocking vs. non-blocking I/O, `EAGAIN`, and the readiness model
- Event loops with `epoll` (level- vs. edge-triggered, the drain-until-`EAGAIN` rule)
- TCP vs. UDP, datagram framing, and the **ACK-window** that keeps a loopback feed lossless & deterministic
- The end-to-end pipeline: wire → `epoll` ingest → SPSC ring → pinned strategy → orders; no syscalls on the strategy core
- ⭐ Bonus: kernel-bypass teaser — `recvmmsg`, `SO_BUSY_POLL`, `io_uring`, `AF_XDP`, DPDK, Solarflare/onload

**Project:** The capstone **ranked challenge** — a **network gateway**. You implement a single `gateway.cpp` against a frozen [`GATEWAY_SPEC.md`](./week-5/project/GATEWAY_SPEC.md): an `epoll` ingest loop receives a UDP tick feed off a connected loopback socket, decodes each `WireTick`, hands it across **your Week-4 SPSC ring** to a **pinned** thread running the **unchanged Week-1 z-score strategy**, and acknowledges datagrams (an ACK-window) so the feed stays lossless — emitting the exact reference order stream. The judge builds it itself (fixed flags, `-pthread`), runs its own UDP feed sender against it several times, and ranks **correct and deterministic** gateways by **wall-clock throughput** over a huge hidden feed. Because the ACK-window makes the loopback feed lossless and in-order, the order stream is identical to Week 4's — one `gateway.cpp` that unifies every prior week into the complete platform.

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
