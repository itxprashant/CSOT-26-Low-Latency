# Troubleshooting — Week 5 Network Gateway (capstone)

The common ways to get stuck on the gateway challenge, and how to unstick yourself. Skim once now, come back when you hit something. The earlier weeks' troubleshooting still applies and is not repeated: Week 1 [`TROUBLESHOOTING.md`](../../week-1/project/TROUBLESHOOTING.md) (toolchain, `perf`, sanitizers), Week 2 (single-`.cpp` / judge-compile model), Week 3 (threads, pinning, false sharing), and Week 4 [`TROUBLESHOOTING.md`](../../week-4/project/TROUBLESHOOTING.md) (the lock-free SPSC hand-off, memory orderings, determinism). This file adds the Week-5 **networking** ones.

> 🆘 If your problem isn't here, drop the full error in the CSoT group along with **your OS, compiler version, `nproc`, `uname -a`, and the exact command you ran**.

---

## 🖥️ The judge VM (not your laptop)

Ranked uploads are built and run on the cohort EC2 judge — **`c7i.xlarge`** (4 vCPU, 8 GiB, Amazon Linux 2023, `us-east-1`). The portal queues work there; your dashboard numbers come from that box.

- **Four cores.** The gateway uses two hot threads (ingest + strategy); the other two cores are pinning headroom and the OS. Tune for 4 vCPUs.
- **Loopback only.** The judge runs its UDP feed sender and your gateway on the **same box over `127.0.0.1`**. There is no external network in the sandbox — only the loopback socket the judge hands your `run()`.
- **Reproduce the toolchain locally:** `cmake -B build-judge -DCSOT_JUDGE_BUILD=ON -DCSOT_GW_SRC=gateway.cpp && cmake --build build-judge -j`
- **Absolute `run_ns` will differ** between your CPU and the judge — ranking uses denominators captured on the judge silicon (`/etc/csot-judge/net-reference.json`).
- **Between cohort sessions** the maintainer may stop the instance to save cost; submissions stay `queued` and drain when it is back (`JudgePill` on the dashboard).

---

## 🏗️ Build & Toolchain

### `fatal error: gateway.hpp: No such file or directory`

Your include path is wrong. Build through the shipped `CMakeLists.txt` (it adds `include/` for you), or pass `-Iinclude` to a raw `g++`. Copy both `include/gateway.hpp` and `include/strategy.hpp` verbatim — `gateway.hpp` `#include`s `strategy.hpp`.

### `fatal error: sys/epoll.h: No such file or directory`

You're not on Linux. `epoll` is Linux-only; macOS has `kqueue` instead and will not compile this project. Use a Linux box, a container, or WSL2 (see the README setup note). The judge is Linux.

### `static assertion failed: WireTick layout is part of the ABI` (or `FeedDatagramHeader` / `OrderRecord`)

You edited `gateway.hpp` (or `strategy.hpp`). Re-copy them unchanged. The `static_assert`s on `sizeof(WireTick) == 40`, `sizeof(FeedDatagramHeader) == 16`, `sizeof(OrderRecord) == 48`, `sizeof(Tick) == 48`, and `sizeof(Order) == 40` are the canary: the judge's sender frames the feed straight from these layouts and diffs your `OrderRecord[]` stream, so they are not negotiable.

### `undefined reference to pthread_create` / `std::thread` link errors

You're building without the threading library. Use the shipped `CMakeLists.txt` (it does `find_package(Threads)` and links `Threads::Threads`); for a raw `g++` add `-pthread`. The judge always compiles with `-pthread`.

### `cmake` builds the stub, not my code

By default `CSOT_GW_SRC` points at `samples/gateway.stub.cpp`. Point it at your file:

```bash
cmake -B build -DCSOT_GW_SRC=gateway.cpp
cmake --build build -j
```

---

## 🧱 The single-`.cpp` rule (sockets, threads, and atomics are the point)

### "Can I split my gateway across files?"

No. The judge compiles exactly one translation unit — your `gateway.cpp` — against its own `main()` and the two headers. Your epoll loop, ring buffer, thread functions, per-symbol state, and the interned name table must all live inside that one file (an anonymous `namespace` is your friend). If your local build links extra `.cpp`s, it will pass locally and fail on upload.

### "Which APIs may I call this week?"

**Allowed and expected:** `<atomic>`, `<thread>`, `pthread`, `sched_setaffinity`, and the **sockets/`epoll` API on the fd the judge gives you** (`recv`, `send`, `recvmmsg`, `fcntl`/`O_NONBLOCK`, `epoll_create1`/`epoll_ctl`/`epoll_wait`, `setsockopt` on that fd). What is still forbidden (and blocked by the sandbox): **opening your own sockets** (`socket`/`bind`/`connect`/`listen` to anything but the fd you were handed), any **non-loopback** network access, spawning processes (`fork`/`exec`/`system`/`popen`), and file I/O. You receive the feed on the given fd and return an order stream; that's it.

### "Can I add a `main()`?"

No. `main()` is judge-owned (locally it's `harness/main.cpp`, which also runs the UDP feed sender). Your file provides `create_gateway()` and nothing with external linkage besides that factory.

---

## 🌐 Sockets, epoll & the wire

### "My gateway hangs and never finishes / `run()` never returns"

The most common Week-5 hang. Usual causes, in order:

1. **Edge-triggered `epoll` without a drain loop.** With `EPOLLET` you get *one* notification per arrival; if you read a single datagram per `EPOLLIN` and go back to `epoll_wait`, the data already in the socket sits there with no further wake-up, and you never reach `n` ticks. **Drain to `EAGAIN`** on every wake ([`03-epoll-event-loop.md`](../03-epoll-event-loop.md) §3).
2. **You stopped ACKing.** The sender stays within `ACK_WINDOW` of your last ACK; if you stop acknowledging (e.g. your ingest blocked on a full ring and you ACK *after* the loop), the sender stalls, your socket goes quiet, and both sides wait forever. ACK as you drain, at least once per `ACK_WINDOW` datagrams ([`04-...`](../04-tcp-vs-udp-and-framing.md) §4, GATEWAY_SPEC.md §7).
3. **You miscounted `n`.** `n` is the total number of *ticks*, not datagrams. Sum `header.count`, not datagram arrivals. Stop when ticks processed `== n`.

### "I get `EAGAIN`/`EWOULDBLOCK` from `recv` and treat it as an error"

`EAGAIN` on a non-blocking socket means *"nothing to read right now"*, not a failure. It is the normal signal to stop draining and (for busy-poll) spin or (for epoll) go back to `epoll_wait`. Only treat `recv() == 0` or other `errno` as real conditions. See [`02-blocking-nonblocking-and-readiness.md`](../02-blocking-nonblocking-and-readiness.md) §2.

### "My datagram is truncated / I read garbage WireTicks"

One UDP `recv()` returns **exactly one datagram**, and a buffer smaller than the datagram **discards the overflow** — UDP has no partial read. Size your receive buffer to `csot::MAX_FEED_DATAGRAM_BYTES` (1296 bytes), allocated in `on_init`. Parse `count` from the header and only read `count` WireTicks.

### "Some ticks are missing and the count is short (`processed < n` at the end)"

A **dropped datagram** — the Week-5-specific correctness bug. On loopback the only way to lose one is **receive-buffer overflow**, which means you let the sender get ahead of you:

- You **ACKed before `try_push`** succeeded, so the window opened while your ring was full; the sender raced ahead and the kernel dropped the overflow. ACK *after* the ticks are in the ring (GATEWAY_SPEC.md §7, [`05-...`](../05-the-network-pipeline-end-to-end.md) §4).
- Your `SO_RCVBUF` is smaller than `ACK_WINDOW × MAX_FEED_DATAGRAM_BYTES`. The judge raises `net.core.rmem_max`; locally, the harness requests a large buffer best-effort. If you're testing a slow gateway on a box with a tiny `rmem_max`, either raise it (`sudo sysctl -w net.core.rmem_max=8388608`) or ACK more eagerly so the buffer never fills.

---

## 🔗 Correctness & determinism

### "I pass `tiny.feed` single-threaded, then my pipelined version disagrees with itself"

Same lesson as Week 4 — a **data race in the hand-off** — now with one extra suspect: **a dropped datagram from ACKing too early** (above). First rule out the network drop (`processed == n`?), then the ring race (publish with `release`, observe with `acquire`; correct full/empty check). Confirm with ThreadSanitizer:

```bash
cmake -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DENABLE_TSAN=ON -DCSOT_GW_SRC=gateway.cpp
cmake --build build-tsan -j
./build-tsan/gateway_runner data/tiny.feed      # TSan prints racing accesses
```

The judge runs your submission several times and **rejects it if the order stream changes between runs** — a race or a drop that "passes sometimes" still fails (GATEWAY_SPEC.md §8-§9).

### "My orders are right but in the wrong order / a few are missing"

Either your ring isn't order-preserving (Week-4 bug) or you dropped a datagram (Week-5 bug). The feed arrives in `seq` order on loopback — assert `header.seq == expected` to catch a drop immediately. Under back-pressure the ingest must **wait** on a full ring, never skip a tick or stop ACKing.

### "My positions go wrong after a while"

You parallelised the **strategy**, not just the ingest. The strategy is stateful and order-dependent: each tick's fill updates the position the next tick reads (GATEWAY_SPEC.md §5). It must run on a **single** strategy thread, in order. Only the ingest (recv + decode) and the hand-off are parallel with it.

### "My prices are off by a rounding"

You recomputed the price instead of copying the tick field. A BUY uses `ask_px`, a SELL uses `bid_px`, copied verbatim from the decoded tick (GATEWAY_SPEC.md §6). The harness re-encodes `round(price * 10000)`; if you derived the price some other way it won't round-trip.

### "How do I hand-check `tiny.feed`?"

The expected order stream is the **same as Week 4** (same feed, same strategy) and is in `data/tiny.orders.json`:

```bash
python3 data/gen_feed.py --dump data/tiny.feed
python3 data/gen_feed.py --orders data/tiny.feed     # the reference order stream JSON
diff <(./build/gateway_runner data/tiny.feed 2>/dev/null) data/tiny.orders.json
```

A clean `diff` means your order stream (and its checksum) matches.

---

## ⚡ Performance

### "My gateway is no faster than the stub — or slower!"

Usual suspects, in order:

1. **Syscalls on the strategy thread.** If your strategy thread calls `recv`/`send`/`epoll_wait` (or you never split ingest from strategy), every tick pays the ~100+ ns kernel tax and a preemption point. Move *all* syscalls to the ingest thread; the ring is the firewall ([`05-...`](../05-the-network-pipeline-end-to-end.md) §3).
2. **One `recv` and one ACK per tick.** You're syscall-bound. Drain many datagrams per wake and **batch ACKs** (one per K datagrams). Consider `recvmmsg`/`sendmmsg` ([`06-...`](../06-bonus-kernel-bypass-and-beyond.md) §1).
3. **A mutex / `std::queue` in the hand-off.** A lock serialises ingest and strategy. Use the lock-free SPSC ring (Week 4).
4. **`head`/`tail` false-share, or unbalanced stages.** All the Week-4 performance suspects still apply.

### "Throughput swings wildly between runs"

1. **Unpinned threads.** Pin ingest and strategy to distinct physical cores with `sched_setaffinity` (Week 3, [`05-...`](../05-the-network-pipeline-end-to-end.md) §3).
2. **`epoll` wake-up jitter on a saturated feed.** For a single firehose feed, a pinned busy-poll `recv` loop can be steadier than `epoll`; bake them off ([`02-...`](../02-blocking-nonblocking-and-readiness.md) §3, [`05-...`](../05-the-network-pipeline-end-to-end.md) §5).
3. **Benchmark hygiene** (same as Week 1): lock the frequency governor, disable turbo, close background apps.

### "Where is my time going?"

Count syscalls and watch context switches:

```bash
strace -c ./build/gateway_runner data/large.feed         # recvfrom/sendto counts — batch to shrink them
perf stat -e context-switches,cache-misses ./build/gateway_runner data/large.feed
```

High `recvfrom`/`sendto` counts ⇒ not batching. High `context-switches` ⇒ unpinned threads or a syscalling strategy thread. High `cache-misses` ⇒ `head`/`tail` false sharing or a too-deep ring (Week 4).

---

## 🐍 Feed generator (`gen_feed.py`)

### "Two students get different feeds from the same seed"

The seed only guarantees identical output for **identical arguments and unchanged source**. Check `--accesses`, `--seed`, and that nobody edited `gen_feed.py` (`diff` against the upstream copy). It's the **unchanged Week-4 generator** — the on-disk feed format is the same flat `WireTick[]`; only the transport (datagrams) is new, and that's owned by the harness/judge sender, not the file.

### "`gateway_runner` says the feed size isn't a multiple of 40"

The feed is a raw array of 40-byte `WireTick` records. A truncated or text-edited file breaks that invariant. Regenerate it; never open `.feed` files in a text editor and save.

---

## 🆘 When Nothing Works

Start from the golden file, single-threaded (the shipped stub):

```bash
diff <(./build/gateway_runner data/tiny.feed 2>/dev/null) data/tiny.orders.json
```

If even the small `tiny.feed` produces a wrong stream with the **stub**, regenerate your golden files (`gen_feed.py --tiny` then `--orders`). If your own gateway fails but the stub passes, the bug is in your ingest/hand-off, not the strategy. Then the standard escalation:

1. Reproduce on the smallest input (`tiny.feed`), single-threaded blocking `recv` (the stub).
2. Assert `header.seq == expected` and `processed == n` at the end — catches dropped datagrams immediately.
3. Re-run under ThreadSanitizer (`-DENABLE_TSAN=ON`) — it names racing lines in your ring.
4. Run your `run()` 10 times in a loop; if the checksum changes, you have a race or an intermittent drop even if TSan is quiet on a tiny input.
5. Re-run under `-fsanitize=address,undefined` (`-DENABLE_SANITIZERS=ON`) for out-of-bounds ring/buffer indexing.
6. `strace -c` to confirm you're actually batching syscalls once it's correct.
7. If still stuck, post the full command, full output, your OS, `nproc`, `uname -a`, and the smallest reproducing feed to the CSoT group.

You'll get unstuck. Everyone does — and this is the last one. 🚀
