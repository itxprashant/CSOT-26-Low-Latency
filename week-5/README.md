# Week 5 — The Network Gateway (capstone)

> *"The fastest packet is the one your strategy thread never had to wait for."*
>
> *"For five weeks the ticks lived in your address space. This week they arrive on the wire — and everything you built is what keeps them off the hot path."*

Welcome to Week 5 — the last week, and the one where the whole platform comes together. Until now the feed was an array you were *given*. This week it arrives as **UDP datagrams on a loopback socket**, and you write a single `gateway.cpp` that reads them through an **`epoll`** event loop, decodes each `WireTick`, hands it across your **Week-4 SPSC ring** to a **pinned (Week 3)** thread running the **zero-allocation (Week 2)** **Week-1 z-score strategy**, and emits the exact reference order stream. A small **ACK-window** flow-control rule keeps the feed lossless and deterministic so it can be ranked fairly. Same "fastest correct, deterministic implementation" game as Weeks 2–4 — the new craft is keeping the kernel and the wire off your strategy's critical path.

By the end of this week you will have:

- ✅ Understood the **kernel boundary**: why `recv`/`send` are syscalls (~100+ ns), and why batching + decoupling are the only ways to pay that tax less
- ✅ Learned **blocking vs. non-blocking** I/O, `EAGAIN`, and the **readiness model** that underpins every scalable server
- ✅ Built an **`epoll` event loop**: register-once/wait-many, level- vs. edge-triggered, and the **drain-until-`EAGAIN`** rule
- ✅ Understood **TCP vs. UDP**, datagram **framing**, and the **ACK-window** that makes a UDP feed lossless and deterministic on loopback
- ✅ Assembled the **end-to-end pipeline** — wire → `epoll` ingest → SPSC ring → pinned strategy → orders — with back-pressure that reaches all the way to the sender
- ✅ A single `gateway.cpp` on the **live leaderboard**, ranked by wall-clock throughput over a huge feed — and **proven deterministic** across repeated runs

---

## 📖 Reading Order

Work through these in order. Each builds on the last and ends with a curated **"Further Reading"** section.

| # | File | Topic | Est. time |
|---|------|-------|-----------|
| 1 | [`01-sockets-and-the-kernel-boundary.md`](./01-sockets-and-the-kernel-boundary.md) | Berkeley sockets recap, `recv`/`send` as syscalls, the kernel receive buffer, why the wire is the new bottleneck | 20 min |
| 2 | [`02-blocking-nonblocking-and-readiness.md`](./02-blocking-nonblocking-and-readiness.md) | Blocking vs. `O_NONBLOCK`, `EAGAIN`, busy-poll vs. block, the readiness model | 20 min |
| 3 | [`03-epoll-event-loop.md`](./03-epoll-event-loop.md) | `epoll_create1`/`ctl`/`wait`, level- vs. edge-triggered, the drain loop, one ingest loop start to finish | 25 min |
| 4 | [`04-tcp-vs-udp-and-framing.md`](./04-tcp-vs-udp-and-framing.md) | Stream vs. datagram, the datagram frame, sequence numbers, the **ACK-window** that keeps it lossless & deterministic | 25 min |
| 5 | [`05-the-network-pipeline-end-to-end.md`](./05-the-network-pipeline-end-to-end.md) | Wire → epoll → ring → pinned strategy → orders; no syscalls on the strategy core; back-pressure to the wire; measuring | 30 min |
| ⭐ | [`06-bonus-kernel-bypass-and-beyond.md`](./06-bonus-kernel-bypass-and-beyond.md) | **Bonus:** `recvmmsg`, `SO_BUSY_POLL`, `io_uring`, `AF_XDP`, DPDK/onload — and the close of the track | 25 min |

Total reading: **~2–2.5 hours** (the ⭐ bonus is optional but a fitting finale). Then the capstone → the [**project**](./project/README.md).

---

## 🛠️ Setup

You already have the Week-1–4 toolchain. Week 5 adds **no new packages** — the sockets/`epoll` APIs are in the standard Linux headers (`<sys/socket.h>`, `<sys/epoll.h>`, `<netinet/in.h>`) you already have. Confirm the basics and a couple of network sanity checks:

```bash
g++ --version          # ≥ 11 (C++20)
cmake --version        # ≥ 3.20
nproc                  # the judge has 4 vCPUs; ingest + strategy are your two hot threads
uname -s               # Linux — epoll & loopback semantics are Linux-specific
```

Confirm `epoll` and loopback UDP work on your box:

```bash
# epoll headers present and compiling
echo '#include <sys/epoll.h>
int main(){ return epoll_create1(0) < 0; }' | g++ -std=c++20 -x c++ - -o /tmp/ep && /tmp/ep && echo "epoll OK"

# loopback is up (you should see 127.0.0.1)
ip addr show lo | grep 127.0.0.1
```

> **WSL2 / macOS users:** as in Weeks 3–4, native Linux matters most here. `epoll` is **Linux-only** (macOS has `kqueue` instead, and the project uses `epoll`), `sched_setaffinity` pinning is Linux-only, and loopback buffer sizing behaves differently under a VM. The judge runs Linux on 4 vCPUs over loopback; develop as close to that as you can. On macOS the code won't compile against `<sys/epoll.h>` — use a Linux box, a container, or WSL2.

---

## 🎯 Project Brief

[**→ Open `project/README.md`**](./project/README.md)

In one sentence: **implement a single `gateway.cpp` that receives a UDP tick feed through an `epoll` loop, hands each decoded tick across your lock-free SPSC ring to a pinned thread running the frozen Week-1 z-score strategy, acknowledges datagrams to keep the feed lossless, and emits the exact reference order stream — correctly, deterministically, and as fast as the overlap allows.**

This week's project scope:

1. Read and internalize the frozen [`project/GATEWAY_SPEC.md`](./project/GATEWAY_SPEC.md) — the UDP datagram format, the ACK-window flow-control rule, the unchanged decode + strategy + fill model, and the order-stream + determinism gates.
2. Implement a **correct** gateway against the [`project/include/gateway.hpp`](./project/include/gateway.hpp) ABI; verify against [`project/data/tiny.orders.json`](./project/data/tiny.orders.json). (The shipped stub is already correct with a single-threaded blocking `recv` loop — your job is to make it a real pipeline.)
3. Build the real thing: an **`epoll` (or busy-poll) ingest** thread that drains the socket and ACKs datagrams, a **lock-free SPSC ring** hand-off, a **pinned** strategy thread running the **zero-allocation** Week-2 rolling-sum implementation of the spec, with **back-pressure that reaches the sender** and **no syscalls on the strategy core**.
4. Measure throughput + `run()` wall-clock against the single-threaded baseline (and an `epoll`-vs-busy-poll bake-off) with the shipped harness, `perf stat`, and `tools/plot_latency.py`.
5. Upload your **single `gateway.cpp`** to **[csot-low-latency.devclub.in](https://csot-low-latency.devclub.in/dashboard/)** — the judge builds it itself (fixed flags, `-pthread`), runs its own UDP feed sender against it several times, checks the order stream and that repeated runs agree, and ranks correct, deterministic gateways by speed.

> **Same as Weeks 2–4:** you submit **source** (`gateway.cpp`), not a compiled `.so`. The judge owns the compiler, flags, and the feed sender, so the board reflects your code, not your laptop. **As in Weeks 3–4,** the judge runs your submission K times and **rejects non-deterministic results** — a network race that "passes sometimes" does not rank. Details in [`platform_week_5.md`](../../platform_week_5.md).

**Build the boring correct version first** (it's the shipped blocking-`recv` stub) — then split it into ingest + ring + pinned strategy. This is the capstone: it should *use* every prior week, not replace any of them.

---

## ✅ Week 5 Checklist

> Copy this into your tracker. **If you can tick every box in Phases 0–4, you've finished the track.** Phase 5 and the stretch goals are bonus glory.

Suggested pace: **~5–7 days, ~2 hours/day.**

### Phase 0 — Environment Setup (Day 0, ~15 min)

- [ ] Week-1–4 toolchain still works (`g++ ≥ 11`, `cmake ≥ 3.20`, `python3 ≥ 3.8`, `perf` on Linux).
- [ ] `epoll` compiles and `127.0.0.1` is up on `lo` (commands above).
- [ ] TSan and ASan both link a trivial program (`-fsanitize=thread` / `-fsanitize=address`).
- [ ] Copied the ★ project files into a fresh project (or a `week-5/` folder in your repo).
- [ ] Headers compile: `g++ -std=c++20 -Iinclude -c include/gateway.hpp -o /tmp/x.o` is silent (all `static_assert`s pass).
- [ ] `cmake -B build && cmake --build build -j` builds `gateway_runner` from the stub; `diff <(./build/gateway_runner data/tiny.feed 2>/dev/null) data/tiny.orders.json` is clean.

### Phase 1 — Reading (Days 1–2, ~2–2.5 hours)

After each note, write **one sentence** about what surprised you.

- [ ] [`01-sockets-and-the-kernel-boundary.md`](./01-sockets-and-the-kernel-boundary.md) — the kernel boundary
  - [ ] I can explain why a `recv()` per tick is a ~10× tax over a memory load, and the two ways to pay it less.
- [ ] [`02-blocking-nonblocking-and-readiness.md`](./02-blocking-nonblocking-and-readiness.md) — readiness
  - [ ] I can explain what `O_NONBLOCK`/`EAGAIN` change, and when busy-poll beats blocking.
- [ ] [`03-epoll-event-loop.md`](./03-epoll-event-loop.md) — `epoll`
  - [ ] I can state the difference between level- and edge-triggered and why ET *requires* draining to `EAGAIN`.
- [ ] [`04-tcp-vs-udp-and-framing.md`](./04-tcp-vs-udp-and-framing.md) — transport & framing
  - [ ] I can explain why loss/reorder would break the determinism gate, and how the ACK-window prevents it on loopback.
- [ ] [`05-the-network-pipeline-end-to-end.md`](./05-the-network-pipeline-end-to-end.md) — the whole pipe
  - [ ] I can name which week each layer of the pipeline came from, and why ACKing before `try_push` is a bug.
- [ ] [`06-bonus-kernel-bypass-and-beyond.md`](./06-bonus-kernel-bypass-and-beyond.md) — ⭐ **bonus** *(optional)*
  - [ ] I understand how `recvmmsg` and kernel bypass push past the per-syscall floor.

### Phase 2 — Hands-on Experiments (Day 2, ~1.5 hours)

- [ ] **Echo over loopback:** write a 30-line UDP sender/receiver on `127.0.0.1`; confirm one `recv` returns one datagram, and that a too-small buffer truncates it.
- [ ] **Block vs. non-block:** make the receiver non-blocking; watch `recv` return `EAGAIN` on an empty socket. Time a blocking wake-up vs. a busy-poll first-byte latency.
- [ ] **`epoll` it:** register the socket with `EPOLLIN` (LT), then `EPOLLET`; with ET, deliberately read only one datagram per wake and watch it **stall** — then add the drain loop and watch it recover.
- [ ] **Overflow it:** disable ACKing (blast all datagrams), shrink `SO_RCVBUF`, and watch datagrams **drop** (the order stream changes / `n` is never reached). Re-enable the ACK-window; confirm zero loss.
- [ ] **Syscall count:** `strace -c ./build/gateway_runner data/tiny.feed`; note the `recvfrom`/`sendto` counts, then batch (bigger drains, ACK per K) and watch them fall.

### Phase 3 — Project (Days 3–6, ~6–8 hours)

Full brief in [`project/README.md`](./project/README.md). High-level milestones:

- [ ] **Step 1 — Skeleton:** ★ files copied; stub builds, runs, and diffs clean on `tiny.feed` (it's correct, single-threaded, blocking).
- [ ] **Step 2 — Big feed:** generate a 5M-tick feed and confirm the stub is still correct (record its throughput as the baseline to beat).
- [ ] **Step 3 — Non-blocking + `epoll`:** flip the socket non-blocking, build the `epoll` ingest loop with a drain-to-`EAGAIN`; re-`diff` — still clean.
- [ ] **Step 4 — Split the pipeline:** ingest on one thread (recv + decode + ACK), strategy on the consumer thread over your Week-4 SPSC ring; re-`diff` — still clean.
- [ ] **Step 5 — Back-pressure to the wire:** ACK *only after* `try_push` succeeds; confirm the sender throttles to your pace and nothing drops.
- [ ] **Step 6 — Pin + balance + zero-alloc:** `sched_setaffinity` both threads; Week-2 rolling sums; everything allocated in `on_init`. Headline numbers:
  - `run()` wall-clock on a 5M-tick feed: \_\_\_\_\_ ms · throughput \_\_\_\_\_ M ticks/s · speedup vs. serial \_\_\_\_\_×
- [ ] **Step 7 — Prove determinism:** run `run()` 10× in a loop; the order-stream checksum is identical every time; TSan is clean.
- [ ] **Step 8 — Project README:** build steps, hardware (`nproc`/`lscpu`), headline throughput + speedup, a `perf stat`/`strace -c` snippet, and **one thing that surprised you**.

### Phase 4 — Submission

- [ ] Code pushed to a public repo with a clear README and a `.gitignore` (no committed `*.feed` except the golden `tiny.feed`).
- [ ] Repo link shared in the CSoT group / submission form.
- [ ] Signed in to **[csot-low-latency.devclub.in](https://csot-low-latency.devclub.in/dashboard/)** with your DevClub IITD account.
- [ ] Uploaded **`gateway.cpp`**. Submission shows `correct` (and passes the determinism check). Your judge numbers:
  - `run()` wall-clock: \_\_\_\_\_ ms · throughput: \_\_\_\_\_ M ticks/s
- [ ] Found your row on the [live leaderboard](https://csot-low-latency.devclub.in/leaderboard/) (gateway challenge).

### Phase 5 — Self-Check (the "would you survive an interview" round)

If you can answer all of these without Googling, you've internalized Week 5 — and the track:

- [ ] **Q1.** Why is a `recv()` per tick a ~10× tax over a memory load? Name the three things a syscall does.
- [ ] **Q2.** What does `O_NONBLOCK` change about `recv()`? What is `EAGAIN`?
- [ ] **Q3.** Explain the readiness model. Why is it always paired with non-blocking sockets?
- [ ] **Q4.** Level- vs. edge-triggered `epoll`: what's the difference, and why does ET force a drain-to-`EAGAIN` loop?
- [ ] **Q5.** Why is market data carried over UDP multicast and not TCP? What is head-of-line blocking?
- [ ] **Q6.** On loopback, what is the *only* way a datagram is lost? How does the ACK-window prevent it?
- [ ] **Q7.** Why must you ACK *after* `try_push` succeeds, not before? What breaks if you ACK early?
- [ ] **Q8.** Why must the strategy thread never make a syscall? What does the ring buffer protect it from?
- [ ] **Q9.** Which week does each layer of the pipeline come from? (wire, ring, pinning, zero-alloc, strategy.)
- [ ] **Q10.** Your gateway is correct but no faster than the stub. Give two likely causes and their fixes.

(Answers are scattered throughout the topic files and `GATEWAY_SPEC.md` — go find them.)

### ⭐ Stretch Goals (Optional)

- [ ] Replaced the `recv`-per-datagram loop with **`recvmmsg`** (and ACKs with `sendmmsg`); measured the syscall-count and throughput change.
- [ ] **`epoll` vs. busy-poll** bake-off on the judge-like build; quantified which wins on a saturated single feed and why.
- [ ] Swept the **ACK batching factor** (ACK every 1 → every 256 datagrams); plotted throughput vs. sender stalls.
- [ ] Swept **`SO_RCVBUF`** and the effective window; found where the receive buffer becomes the bottleneck.
- [ ] Pinned ingest and strategy to **physical cores vs. hyperthread siblings**; quantified the tail-latency difference.
- [ ] Tried **`SO_BUSY_POLL`** on the receive socket; measured the first-byte latency change.
- [ ] Deliberately ACKed *before* `try_push`, shrank the buffer, and confirmed the judge's determinism check would catch the resulting drop — then fixed it.
- [ ] Built with `-DCSOT_JUDGE_BUILD=ON` vs. `-march=native`; quantified the ISA's worth on decode + strategy.

### 🧠 The Single Most Important Box

If you check **only one** box from this entire list, make it this one:

- [ ] **I took a tick off a UDP socket through an `epoll` loop, decoded it, handed it lock-free across an SPSC ring to a pinned, zero-allocating thread running the unchanged Week-1 strategy, kept the feed lossless with an ACK-window, and have the `perf`/throughput numbers and the determinism check proving one `gateway.cpp` is a correct, fast, deterministic networked trading data path — built from every week before it.**

That sentence *is* the platform. If it's true, you didn't just finish Week 5 — you built the whole thing.

---

## 🎬 Must-Watch Talks (general — applies to the whole week)

In addition to the per-file "Further Reading", these define the week:

1. **[Carl Cook — "When a Microsecond Is an Eternity"](https://www.youtube.com/watch?v=NH1Tta7purM)** (CppCon 2017, 1 h) — the whole-pipeline HFT mindset: never sleep, never syscall on the hot path, pin everything. The single best talk for this capstone.
2. **[Robert Love — "Linux Kernel & epoll"](https://www.youtube.com/results?search_query=epoll+linux+talk)** / [Julia Evans — async I/O](https://jvns.ca/blog/2017/06/03/async-io-on-linux--select--poll--and-epoll/) — the readiness model and why blocking I/O doesn't scale.
3. **[CppCon — HFT networking / kernel-bypass talks (David Gross and others)](https://www.youtube.com/results?search_query=cppcon+hft+networking)** — where this week points: real trading systems' wire ingest.

---

## 📚 Books (reference, optional)

- **"The Linux Programming Interface" (Michael Kerrisk)** — *the* book for this week. Ch. 56–61 (sockets), Ch. 63 (`epoll`/`select`/`poll`). The single best reference for Week 5.
- **"Unix Network Programming, Vol. 1" (W. Richard Stevens)** — the classic sockets text; datagram vs. stream, I/O multiplexing.
- **"TCP/IP Illustrated, Vol. 1" (Stevens)** — what's actually on the wire; UDP, and TCP's sliding-window flow control that our ACK-window mirrors.
- **"Systems Performance" (Brendan Gregg, 2nd ed.)** — Ch. 10 (Network) for measuring where the network path's time goes.

---

## 🎓 Free University Courses

- **[MIT 6.829 — Computer Networks](https://ocw.mit.edu/)** — transport, flow control, and congestion; the theory under the ACK-window.
- **[Stanford CS144 — Introduction to Computer Networking](https://cs144.github.io/)** — you build a TCP in C++; the best hands-on networking course there is.
- **[MIT 6.172 — Performance Engineering](https://ocw.mit.edu/courses/6-172-performance-engineering-of-software-systems-fall-2018/)** — ties the network path back to the cache/pipeline discipline of the whole track.

---

## 🤷 If You Only Read 5 Things This Week

1. [`03-epoll-event-loop.md`](./03-epoll-event-loop.md) + [`04-tcp-vs-udp-and-framing.md`](./04-tcp-vs-udp-and-framing.md) — the event loop and the lossless-UDP trick that define the week
2. [`man 7 epoll`](https://man7.org/linux/man-pages/man7/epoll.7.html) — especially "Level-triggered and edge-triggered" and "Possible pitfalls"
3. [Julia Evans — "Async IO on Linux: select, poll, and epoll"](https://jvns.ca/blog/2017/06/03/async-io-on-linux--select--poll--and-epoll/)
4. [`project/GATEWAY_SPEC.md`](./project/GATEWAY_SPEC.md) §7 — the determinism guarantee (why a lossless ACK-window feed is fair to rank)
5. [Cloudflare — "How to receive a million packets per second"](https://blog.cloudflare.com/how-to-receive-a-million-packets/)

---

## 💬 Communities

- **[r/cpp](https://reddit.com/r/cpp)** — current C++ networking / `io_uring` discussions.
- **[#include <C++> Discord](https://www.includecpp.org/)** — great for sockets / `epoll` questions.
- **[r/algotrading](https://reddit.com/r/algotrading)** — the quant side.
- **[Hacker News](https://news.ycombinator.com/)** — search "epoll", "io_uring", "kernel bypass", "UDP multicast market data".

---

## 🏁 Ready? (and: thank you)

Start with [`01-sockets-and-the-kernel-boundary.md`](./01-sockets-and-the-kernel-boundary.md). Then go take a tick off the wire and run it through everything you've built — without a lock, without a race, without a syscall on the hot path.

This is the last week. When your `gateway.cpp` is on the board, look back at where you started: a CSV reader and a histogram in Week 1. You built a zero-allocation hot path, made four cores reduce one array, handed a stream between threads lock-free, and now pulled the feed off the network — one platform, five layers deep. **That's a real low-latency trading data path, and you wrote it from scratch.** Ship it. ⚡
