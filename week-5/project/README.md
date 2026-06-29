# Week 5 Project — Fastest Correct Network Gateway (the capstone)

> **Mission:** Implement a **single `gateway.cpp`** that receives a UDP tick feed off a connected loopback socket through an **`epoll`** (or busy-poll) ingest loop, hands each decoded tick across **your Week-4 lock-free SPSC ring buffer** to a **pinned** thread running the **frozen Week-1 z-score strategy**, **acknowledges** datagrams to keep the feed lossless, and emits the exact reference order stream — **correctly**, **deterministically**, and **as fast as the kernel and two cores allow**. Week 1 measured one core; Week 2 fed its cache; Week 3 drove four cores; Week 4 handed a stream between two cores without a lock. Week 5 puts the feed on the wire — and keeps it off the strategy's hot path. This is the whole platform in one file.

---

## 📦 What This Folder Gives You

Copy the ★ files verbatim. They remove the tedious, error-prone scaffolding so you can focus on the one file that is *yours*: the gateway.

| File | Purpose | Status |
|---|---|---|
| [`include/gateway.hpp`](./include/gateway.hpp) | The **frozen gateway ABI** — `WireTick`, `FeedDatagramHeader`, `OrderRecord`, `Gateway`, `create_gateway()`, `PRICE_SCALE`, `NUM_SYMBOLS`, `MAX_BATCH`, `ACK_WINDOW`. | ★ **Do not modify.** |
| [`include/strategy.hpp`](./include/strategy.hpp) | The **frozen Week-1 ABI** — `Tick`, `Order`, `Strategy`. Carried forward unchanged (the spec is frozen for the whole track). | ★ **Do not modify.** |
| [`GATEWAY_SPEC.md`](./GATEWAY_SPEC.md) | The **frozen spec** every submission must satisfy (UDP datagram format, decode rule, the unchanged strategy + fill model, order-stream equality, the ACK-window flow control, determinism). | ★ **Do not change.** Faster implementations only. |
| [`samples/gateway.stub.cpp`](./samples/gateway.stub.cpp) | A compiling, **correct single-threaded blocking-`recv`** skeleton wired to the ABI. Passes `tiny.feed` out of the box; ranks near the bottom. | ★ Copy to `gateway.cpp` and turn it into a pipeline. |
| [`harness/main.cpp`](./harness/main.cpp) | Local runner: opens a loopback UDP pair, runs a feed sender, times your `run()`, prints the order stream as JSON + checksum + throughput. Mirrors the judge. | ★ Use as-is (you never upload it). |
| [`CMakeLists.txt`](./CMakeLists.txt) | Build template (Release/Debug, `-pthread`, TSan, ASan, LTO, `CSOT_JUDGE_BUILD`, `CSOT_GW_SRC`). | ★ Use as-is. |
| [`.gitignore`](./.gitignore) | Build, feed, perf, sanitizer noise. | ★ Use as-is. |
| [`data/gen_feed.py`](./data/gen_feed.py) | Seeded wire-feed generator (unchanged from Week 4). `--tiny`, `--dump`, `--orders` modes. | ★ Default seed is the cohort baseline. |
| [`data/tiny.feed`](./data/tiny.feed) + [`data/tiny.orders.json`](./data/tiny.orders.json) | A small golden feed and its exact reference order stream (identical to Week 4's). | ★ Use for unit-testing your gateway. |
| [`TROUBLESHOOTING.md`](./TROUBLESHOOTING.md) | Sockets, `epoll`, `EAGAIN`, dropped-datagram, determinism, pinning gotchas, plus pointers back to Weeks 1–4. | ★ Skim once, refer back when stuck. |
| [`tools/plot_latency.py`](./tools/plot_latency.py) | Optional: plot throughput across configurations. | Optional. |

Everything inside `run()` — your epoll loop, your ring buffer, your thread layout, your pinning, your ACK cadence, your strategy implementation — is yours to design.

---

## 🔒 The Five Contracts You MUST Respect

These are **mandatory**. Everything else in this README is "suggested".

### Contract 1 — The UDP wire format (frozen)

The feed arrives as UDP datagrams: a 16-byte `FeedDatagramHeader` (`seq`, `count`) followed by up to `MAX_BATCH` (32) `WireTick` records, little-endian (see [`GATEWAY_SPEC.md`](./GATEWAY_SPEC.md) §2):

```text
[ FeedDatagramHeader: seq:u64  count:u32  _pad:u32 ][ WireTick[count] ]
```

Each 40-byte `WireTick` is unchanged from Week 4 (`timestamp_ns`, `bid_px_fp`, `ask_px_fp`, `symbol_id`, `bid_qty`, `ask_qty`, `_reserved`). On loopback with the ACK-window enforced, datagrams arrive **in `seq` order, contiguous, none lost** — you may assert `seq` and rely on in-order delivery. Decode each tick (`bid_px = bid_px_fp / 10000.0`, symbol id `k` → `"SYM<k>"`) and process in send order; the strategy is stateful.

### Contract 2 — The gateway ABI (frozen)

`WireTick`, `FeedDatagramHeader`, `OrderRecord`, and `class Gateway` are defined in [`include/gateway.hpp`](./include/gateway.hpp), with `static_assert`s on their sizes (40 / 16 / 48) and the Week-1 `Tick`/`Order` canaries. Your file must export exactly:

```cpp
extern "C" csot::Gateway* create_gateway();
```

The judge does the moral equivalent of:

```cpp
csot::Gateway* g = create_gateway();
g->on_init(num_symbols);                          // num_symbols == 1024
std::size_t k = g->run(fd, n, out);               // <-- this call is timed; fd is a connected UDP socket
```

> ⚠️ **Why this matters from day one:** the judge builds your `gateway.cpp` against *its own* `main()` (with its own UDP feed sender) and these headers. If your `run()` signature drifts — even a `const` — your submission won't link and the upload fails. Lock the ABI now.

### Contract 3 — The spec (frozen)

The order stream your `run()` produces is defined by [`GATEWAY_SPEC.md`](./GATEWAY_SPEC.md), which reuses the **unchanged** Week-1 z-score strategy + fill model ([`STRATEGY_SPEC.md`](../../week-1/project/STRATEGY_SPEC.md)). It is **byte-identical to Week 4's** for the same feed.

You may optimize **how** you produce it: epoll/busy-poll ingest, a lock-free SPSC ring, pinned threads, batched recv/ACK, zero heap after `on_init`, Week-2 rolling sums. You may not change **what** it produces: the decode rule, the z-score logic, the fill model, the warm-up window are fixed; orders are emitted in tick order, at most one per tick; the price is **copied** from the tick (BUY → ask, SELL → bid), never recomputed.

### Contract 4 — The ACK-window flow control (frozen)

This is the one new rule. The judge's sender stays at most **`ACK_WINDOW` (128) datagrams ahead** of the highest `seq` you have ACKed. As you drain datagrams you must **send a bare `uint64` ACK** (the next seq you expect) back on the same `fd`, at least once per `ACK_WINDOW` datagrams — and, crucially, **only after** the datagram's ticks are safely in your ring (`try_push` succeeded). That keeps the feed lossless *and* makes back-pressure reach the sender: a slow strategy → full ring → no ACK → the sender waits, nothing drops. See [`GATEWAY_SPEC.md`](./GATEWAY_SPEC.md) §7.

### Contract 5 — The judge machine (frozen for the season)

Ranked submissions are **not** graded on your laptop. They run on the cohort's dedicated AWS EC2 judge:

| | |
|---|---|
| Instance | **`c7i.xlarge`** — **4 vCPU**, 8 GiB, Intel Sapphire Rapids |
| OS | Amazon Linux 2023 |
| Network | **loopback only** — the judge's UDP feed sender and your gateway share `127.0.0.1` inside the sandbox; there is no external network |
| Region | `us-east-1` (maintainer start/stop via `./judge-vm.sh` in the repo root) |

The judge downloads your `gateway.cpp`, compiles it with **fixed flags** (`-std=c++20 -O3 -march=x86-64-v2 -pthread`, see [`CMakeLists.txt`](./CMakeLists.txt) `CSOT_JUDGE_BUILD`), runs it inside **bubblewrap** with a loopback socket against the public then hidden feeds, diffs your order stream against the reference, and checks repeated runs agree. **Two hot threads on four cores** is your target.

> 📌 **Laptop numbers are for debugging only.** Reproduce the judge locally with `cmake -B build-judge -DCSOT_JUDGE_BUILD=ON` — but expect different absolute `run_ns` and a different core topology. The leaderboard denominators are captured on the `c7i.xlarge` box (`/etc/csot-judge/net-reference.json`).

---

## 🎯 Learning Goals

By completing this project you will:

1. Have built a real **network ingest loop** — non-blocking socket, `epoll` readiness, drain-to-`EAGAIN` — and felt the syscall boundary as the new bottleneck.
2. Have implemented a **lossless, deterministic UDP protocol** with sequence numbers and ACK-window flow control, and understood *why* loss/reorder would break a fair leaderboard.
3. Have **decoupled the wire from the strategy** with your Week-4 SPSC ring, keeping every syscall off the strategy's hot path.
4. Have made **back-pressure reach the sender** — a slow strategy throttles the wire, nothing drops — by ACKing after the hand-off.
5. Have **pinned** both threads, kept `run()` **zero-allocation**, and balanced ingest against the strategy with Week-2 rolling sums.
6. Have assembled **every prior week into one `gateway.cpp`** and ranked it on the live board — the complete platform.
7. Have practised **programming to a frozen contract** one last time — a new ABI and a new flow-control rule on top of the Week-1 spec.

---

## 🏗️ What You Must Build

You deliver exactly **one file**: `gateway.cpp`. Everything else is provided.

### 1. The gateway ABI — **provided, copy as-is**

[`include/gateway.hpp`](./include/gateway.hpp) and [`include/strategy.hpp`](./include/strategy.hpp). Drop them into `include/` unchanged.

### 2. Your gateway (`gateway.cpp`) — *yours to write*

A concrete `csot::Gateway` that implements [`GATEWAY_SPEC.md`](./GATEWAY_SPEC.md):

- allocate all state in `on_init(num_symbols)` (ring storage, the datagram recv buffer, the epoll `events[]` array, per-symbol strategy state, interned `"SYM<id>"` names, thread bookkeeping)
- in `run(fd, n, out)`, run an **ingest** thread that drains `fd` (epoll/busy-poll, drain to `EAGAIN`), decodes each `WireTick`, pushes `csot::Tick`s across your SPSC ring, and ACKs datagrams; on a **pinned strategy** thread, pop in order, run the z-score + fill model, and append each order to `out`
- ACK **after** `try_push`; keep **no syscalls** on the strategy thread
- export `extern "C" csot::Gateway* create_gateway() { return new YourGateway(); }`

The point is **not** to invent a metric, a signal, or a protocol. The point is to produce the fixed order stream correctly, then make `run()` fast. Start from the stub — it's already correct, single-threaded, blocking:

```bash
cp samples/gateway.stub.cpp gateway.cpp
```

> 📌 **Single translation unit.** The judge compiles exactly this one `.cpp` against its own `main()` and headers. Your epoll loop, ring buffer, thread functions, per-symbol state, and the interned name table all live inside it (use an anonymous `namespace`). No second `.cpp`, no `main()`, no custom CMake. **Sockets/`epoll`, `<atomic>`, `<thread>`, and `sched_setaffinity` ARE the point this week** — but you may only use the `fd` you were handed; opening your own sockets, non-loopback access, processes, and file I/O are blocked by the sandbox. See [`TROUBLESHOOTING.md`](./TROUBLESHOOTING.md) → "The single-`.cpp` rule".

### 3. Sample data files

- [`data/tiny.feed`](./data/tiny.feed) — **provided**, small feed; expected order stream in [`data/tiny.orders.json`](./data/tiny.orders.json) (same as Week 4). Use for unit tests.
- `data/large.feed` — generate with `python3 data/gen_feed.py --accesses 5000000 --seed 42 --out data/large.feed`. Fast iteration + real overlap signal. **gitignored.**

> 💡 [`data/gen_feed.py`](./data/gen_feed.py) uses seed `42` by default — **everyone generates byte-identical feeds**, so your numbers compare directly to classmates' and the baseline. It's the unchanged Week-4 generator; the datagram framing happens in the harness/judge sender, not the file.

### 4. A `README.md` for your project — *yours to write*

Build steps, your hardware (`nproc` / `lscpu`), and your headline number: **throughput (M ticks/s)** and `run()` wall-clock on `data/large.feed`, the **speedup vs. the single-threaded stub**, a `strace -c` syscall count showing your batching, plus confirmation that your order stream matches `data/tiny.orders.json` and is deterministic across runs.

---

## 📁 Recommended Directory Layout

```
project/
├── CMakeLists.txt             ← ★ copy from this folder
├── .gitignore                 ← ★ copy from this folder
├── README.md                  ← your own writeup
├── GATEWAY_SPEC.md            ← ★ copy/read: the frozen gateway spec
├── TROUBLESHOOTING.md         ← ★ copy from this folder
├── include/
│   ├── gateway.hpp            ← ★ copy from this folder unchanged
│   └── strategy.hpp           ← ★ copy from this folder unchanged (frozen Week-1 ABI)
├── harness/
│   └── main.cpp               ← ★ local runner + UDP feed sender (you never upload it)
├── gateway.cpp                ← yours; implements GATEWAY_SPEC.md + exports create_gateway()
├── samples/
│   └── gateway.stub.cpp       ← ★ correct single-threaded starting point (copy to gateway.cpp)
├── data/
│   ├── gen_feed.py            ← ★ copy from this folder unchanged
│   ├── tiny.feed              ← ★ committed golden feed
│   ├── tiny.orders.json       ← ★ committed golden order stream
│   └── large.feed             ← gitignored, generated by gen_feed.py
└── tools/
    └── plot_latency.py        ← optional
```

Files marked ★ are shipped — copy them verbatim. `gateway.cpp` is yours.

---

## 🪜 Suggested Implementation Order

Each step should compile and run before moving on.

### Step 1 — Skeleton build (Day 1, ~30 min)

- Copy the ★ files into place. Confirm the headers compile: `g++ -std=c++20 -Iinclude -c include/gateway.hpp -o /tmp/x.o` is silent.
- `cmake -B build && cmake --build build -j` builds `gateway_runner` from the stub.
- `diff <(./build/gateway_runner data/tiny.feed 2>/dev/null) data/tiny.orders.json` is clean — the stub is correct single-threaded.

### Step 2 — A real feed (Day 1, ~30 min)

- `python3 data/gen_feed.py --accesses 5000000 --seed 42 --out data/large.feed`.
- Run the stub on it; record the single-threaded throughput as your **baseline** to beat: \_\_\_\_\_ M ticks/s.
- `strace -c ./build/gateway_runner data/large.feed` — note the `recvfrom`/`sendto` counts (one per datagram in the stub).

### Step 3 — Non-blocking + epoll ingest (Day 2, ~2 hours)

- Flip the socket non-blocking (`O_NONBLOCK`), register it with `epoll`, and drain to `EAGAIN` on each wake ([`02-`](../02-blocking-nonblocking-and-readiness.md), [`03-`](../03-epoll-event-loop.md)). Keep it single-threaded for now (decode + strategize inline). Re-`diff` — still clean.

### Step 4 — Split the pipeline (Day 3, ~2 hours)

- `cp samples/gateway.stub.cpp gateway.cpp`. Move recv + decode + ACK onto an **ingest** thread that pushes `csot::Tick`s across your **Week-4 SPSC ring**; run the strategy on the **consumer** thread, popping in order ([`05-`](../05-the-network-pipeline-end-to-end.md)).
- Build against your file: `cmake -B build -DCSOT_GW_SRC=gateway.cpp && cmake --build build -j`. Re-`diff` against `tiny.orders.json` — still clean.

### Step 5 — Back-pressure to the wire (Day 4, ~1.5 hours)

- ACK **only after** `try_push` succeeds; confirm a slow strategy throttles the sender and `processed == n` always (nothing drops). Assert `header.seq == expected` to catch any drop. Batch ACKs (one per K datagrams).

### Step 6 — Pin + balance + zero-alloc (Day 5, ~1.5 hours)

- `sched_setaffinity` both threads to distinct cores ([`05-`](../05-the-network-pipeline-end-to-end.md) §3). Bring back **Week-2 rolling sums** so the strategy stage is cheap. Confirm nothing allocates in `run()`. Capture headline numbers:
  - `run()` wall-clock on `large.feed`: \_\_\_\_\_ ms · throughput \_\_\_\_\_ M ticks/s · speedup vs. stub \_\_\_\_\_×

### Step 7 — Prove it (Day 6, ~1.5 hours)

- Run `run()` 10× in a loop; assert the order-stream checksum is identical every time. Build under `-DENABLE_TSAN=ON` and confirm a clean report.
- `strace -c` (syscalls down after batching), `perf stat -e context-switches,cache-misses` (before/after pinning).

### Step 8 — Write it up (Day 7, ~30 min)

- Project `README.md`: build steps, your hardware, headline throughput + speedup, a `strace -c`/`perf stat` snippet, and **one thing that surprised you**.

That last bullet is the most important. It's the start of your network-engineering intuition.

---

## 🧪 Hands-on practice alongside the project

The ranked deliverable is **`gateway.cpp` only**. These exercises are **not submitted** — they make the friction visceral before you hit it in the project. All live in the topic files and take ten minutes each:

- The **edge-triggered drain bug** ([`03-epoll-event-loop.md`](../03-epoll-event-loop.md) §3): register `EPOLLET`, read one datagram per wake, watch it stall; add the drain loop, watch it recover.
- The **overflow/drop demo** ([`04-tcp-vs-udp-and-framing.md`](../04-tcp-vs-udp-and-framing.md) §3): stop ACKing, shrink `SO_RCVBUF`, watch datagrams drop and the stream change; re-enable the ACK-window, confirm zero loss.

> 💡 Do these **before** Step 4. When you've personally watched an edge-triggered socket hang and a too-fast sender drop datagrams, the drain loop and "ACK after `try_push`" stop feeling like rules and start feeling inevitable.

---

## 🚀 Stretch Goals (Optional, for the Eager)

1. Replace the `recv`-per-datagram loop with **`recvmmsg`** (and ACKs with `sendmmsg`); measure the syscall-count and throughput change ([`06-`](../06-bonus-kernel-bypass-and-beyond.md)).
2. **`epoll` vs. busy-poll** bake-off on the judge-like build; which wins on a saturated single feed, and why?
3. Sweep the **ACK batching factor** (1 → 127 datagrams); plot throughput vs. sender stalls.
4. Sweep **`SO_RCVBUF`** and the effective window; find where the buffer becomes the bottleneck.
5. Pin to **physical cores vs. hyperthread siblings**; quantify the tail-latency difference.
6. Try **`SO_BUSY_POLL`** on the receive socket; measure the first-byte latency change.
7. Build with `-DCSOT_JUDGE_BUILD=ON` and compare to your `-march=native` numbers. How much was the ISA worth on decode + strategy?

---

## 🏆 The Live Leaderboard

**The leaderboard is live: [csot-low-latency.devclub.in](https://csot-low-latency.devclub.in/).** Week 5 swaps the active ranked challenge from the Week-4 pipeline to the **network gateway** — a fresh board whose ceiling is keeping the kernel/wire off the strategy while two cores stay busy, so the top is not saturated on day one.

### How it works (you upload source, the judge builds it)

1. Sign in with your DevClub IITD identity.
2. On the dashboard, upload a **single `gateway.cpp`** (not a compiled `.so`).
3. The judge **builds your file itself** with a fixed, portable toolchain (`-O3 -march=x86-64-v2 -pthread`, `CSOT_JUDGE_BUILD=ON`) against its own `main()` and headers, inside a sandbox with a loopback socket.
4. It runs your `run()` against **two feeds** (its own UDP sender feeding your gateway), diffs the order stream against the reference, **runs it several times to confirm determinism**, and — if correct and deterministic — ranks you by wall-clock speed.

Uploading source means everyone is measured on the **same compiler, flags, and feed sender**, so the board reflects *your code*, not your laptop.

### The two feeds

| Feed | Ticks | Seed | Reproducible? | Role |
|---|---:|---:|---|---|
| **Public** | 5 000 000 | `42` (the `gen_feed.py` default) | ✅ Yes — `python3 data/gen_feed.py --accesses 5000000 --seed 42 --out public.feed` | Correctness smoke test. Fail here and you stop. |
| **Hidden** | very large | sealed | ❌ No — held out on the judge box | The speed ranking happens here. |

Debug your `incorrect` runs locally on the reproducible public feed before bothering the judge; the hidden feed forces your gateway to sustain wire pressure for longer (more datagrams, more ACK round-trips).

### Correctness and determinism gate everything

- Every order must match the reference exactly, in order. One disagreement → `incorrect`, no rank.
- Your `run()` must return the **same order stream on repeated runs**. A hand-off race or a dropped datagram that "passes sometimes" is flagged non-deterministic → no rank.
- If both feeds pass and runs agree, the judge ranks by **wall-clock `run()` time** (and throughput). Correctness and determinism are never traded for speed.

### Submission policy

- **Single `gateway.cpp`**, ≤ 1 MiB, must export `extern "C" csot::Gateway* create_gateway()`.
- One identity, one entry. No teams.
- Cooldown + daily quota enforced server-side (the dashboard shows the window; spamming returns `429`).
- The judge VM may be stopped between sessions to save cost — submissions queue and drain when it's back.

### What you do NOT have to do this week

You don't have to top the board. Upload when your gateway is **correct on `tiny.feed`, deterministic, lossless (`processed == n`), and faster than the single-threaded stub**. Beating the reference's pipelined time is the game for the eager.

---

## ❓ FAQ

**Q: Why a network gateway? I came here for trading.**
A: A real market-data system *starts* at the wire — one thread takes packets off the NIC, another runs the strategy, decoupled so network jitter never stalls the logic. This week the producer is a UDP socket; the strategy is the unchanged Week-1 z-score rule. The leaderboard still measures the same thing: systems-engineering speed.

**Q: Can I submit a `.so` like Week 1?**
A: No. Like Weeks 2–4, the judge takes a single `gateway.cpp` and builds it itself, so the field is identical for everyone.

**Q: Which APIs may I use?**
A: Sockets/`epoll` on the fd you're handed, `<atomic>`, `<thread>`, and `sched_setaffinity` are the point this week. Opening your own sockets, any non-loopback network, processes (`fork`/`exec`/`system`), and file I/O are blocked by the sandbox.

**Q: My gateway hangs / never finishes.**
A: Edge-triggered `epoll` without a drain-to-`EAGAIN` loop, or you stopped ACKing (the sender stalls within `ACK_WINDOW`). See [`03-`](../03-epoll-event-loop.md) §3 and `TROUBLESHOOTING.md` → "My gateway hangs".

**Q: Some ticks are missing (`processed < n`).**
A: A dropped datagram — you ACKed before `try_push` (the sender raced ahead and the receive buffer overflowed) or your `SO_RCVBUF` is too small. ACK after the ticks are in the ring (GATEWAY_SPEC.md §7).

**Q: Correct on `tiny.feed` but the judge says non-deterministic.**
A: A hand-off race (Week-4 ordering bug) or an intermittent drop. Get the orderings right ([`03-memory-orderings`](../../week-4/03-memory-orderings.md)), ACK after `try_push`, and catch it with `-DENABLE_TSAN=ON` + a 10× checksum loop.

**Q: Two threads is no faster than one.**
A: Usually syscalls on the strategy thread, or one `recv`/ACK per tick. Move all syscalls to ingest, batch the recv (drain to `EAGAIN`) and the ACKs. See [`05-`](../05-the-network-pipeline-end-to-end.md) and `TROUBLESHOOTING.md` → Performance.

**Q: My prices are off by a rounding.**
A: You recomputed the price. Copy `bid_px`/`ask_px` from the decoded tick (GATEWAY_SPEC.md §6); the harness re-encodes the fixed-point integer.

---

## 📤 Submission

### 1. The Git repo (for cohort review)

A public repo with your `gateway.cpp`, project `README.md` (headline numbers + speedup + syscall count), the shipped `gen_feed.py`, and a `.gitignore` for `build/` and `*.feed` (keep the golden `tiny.feed`).

### 2. The leaderboard (for the live ranking)

Sign in at **[csot-low-latency.devclub.in](https://csot-low-latency.devclub.in/)** and upload your `gateway.cpp` from the dashboard. The judge builds it, runs its UDP feed against it (several times), and reports back the order-stream check, throughput, and a rank.

---

## 🎉 You Made It — and That's the Track

You took a tick off a UDP socket, through an `epoll` loop, across a lock-free ring, into a pinned, zero-allocating, unchanged Week-1 strategy — lossless, race-free, and with no syscall on the hot path. **That is a real low-latency trading data path, and you built it from a CSV reader in five weeks.**

Look back at the layers in this one `gateway.cpp`: the Week-1 strategy, the Week-2 zero-alloc rolling sums, the Week-3 pinning, the Week-4 SPSC ring, the Week-5 network ingest. Five weeks, one platform, every layer measured rather than guessed. The frontier — `recvmmsg`, `io_uring`, kernel bypass ([`06-`](../06-bonus-kernel-bypass-and-beyond.md)) — is more depth on the exact discipline you now have.

Ship it. And thank you for building the whole thing. ⚡
