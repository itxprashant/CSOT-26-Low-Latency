# Troubleshooting — Week 4 Lock-Free Pipeline

The common ways to get stuck on the pipeline challenge, and how to unstick yourself. Skim once now, come back when you hit something. The Week-1 [`TROUBLESHOOTING.md`](../../week-1/project/TROUBLESHOOTING.md) still covers the toolchain, `perf`, Google Benchmark, and sanitizers; the Week-2 [`TROUBLESHOOTING.md`](../../week-2/project/TROUBLESHOOTING.md) covers the single-`.cpp` / judge-compile model; the Week-3 [`TROUBLESHOOTING.md`](../../week-3/project/TROUBLESHOOTING.md) covers threads, pinning, and false sharing. This file adds the Week-4 lock-free hand-off ones.

> 🆘 If your problem isn't here, drop the full error in the CSoT group along with **your OS, compiler version, `nproc`, and the exact command you ran**.

---

## 🖥️ The judge VM (not your laptop)

Ranked uploads are built and run on the cohort EC2 judge — **`c7i.xlarge`** (4 vCPU, 8 GiB, Amazon Linux 2023, `us-east-1`). The portal queues work there; your dashboard numbers come from that box.

- **Four cores.** The pipeline uses two hot threads (decode + strategy); the other two cores are pinning headroom and the OS. Tune for 4 vCPUs, not your 16-thread laptop.
- **Reproduce the toolchain locally:** `cmake -B build-judge -DCSOT_JUDGE_BUILD=ON -DCSOT_PIPE_SRC=pipeline.cpp && cmake --build build-judge -j`
- **Absolute `run_ns` will differ** between your CPU and the judge — ranking uses denominators captured on the judge silicon.
- **Between cohort sessions** the maintainer may stop the instance to save cost; submissions stay `queued` and drain when it is back (`JudgePill` on the dashboard).

---

## 🏗️ Build & Toolchain

### `fatal error: pipeline.hpp: No such file or directory`

Your include path is wrong. Build through the shipped `CMakeLists.txt` (it adds `include/` for you), or pass `-Iinclude` to a raw `g++`. Copy both `include/pipeline.hpp` and `include/strategy.hpp` verbatim from this folder — `pipeline.hpp` `#include`s `strategy.hpp`.

### `static assertion failed: WireTick layout is part of the ABI`

You edited `pipeline.hpp` (or `strategy.hpp`). Re-copy them unchanged. The `static_assert`s on `sizeof(WireTick) == 40`, `sizeof(OrderRecord) == 48`, `sizeof(Tick) == 48`, and `sizeof(Order) == 40` are the canary: the judge mmaps the feed straight into a `WireTick[]` and diffs your `OrderRecord[]` stream, so the layouts are not negotiable.

### `undefined reference to pthread_create` / `std::thread` link errors

You're building without the threading library. Use the shipped `CMakeLists.txt` (it does `find_package(Threads)` and links `Threads::Threads`); for a raw `g++` add `-pthread`. The judge always compiles with `-pthread`.

### `cmake` builds the stub, not my code

By default `CSOT_PIPE_SRC` points at `samples/pipeline.stub.cpp`. Point it at your file:

```bash
cmake -B build -DCSOT_PIPE_SRC=pipeline.cpp
cmake --build build -j
```

---

## 🧱 The single-`.cpp` rule (threads and atomics are the point)

### "Can I split my pipeline across files?"

No. The judge compiles exactly one translation unit — your `pipeline.cpp` — against its own `main()` and the two headers. Your ring buffer, thread functions, padded index types, per-symbol state, and the interned name table must all live inside that one file (an anonymous `namespace` is your friend). If your local build links extra `.cpp`s, it will pass locally and fail on upload.

### "Can I `#include <thread>` / `<atomic>` / call `sched_setaffinity`?"

**Yes — this is the week you must.** `<atomic>`, `<thread>`, `pthread`, and `sched_setaffinity` are all expected. What is still forbidden (and blocked by the sandbox) is networking, spawning processes (`fork`/`exec`/`system`/`popen`), and file I/O — you get the feed as an in-memory array and return an order stream; that's it.

### "Can I add a `main()`?"

No. `main()` is judge-owned (locally it's `harness/main.cpp`). Your file provides `create_pipeline()` and nothing with external linkage besides that factory.

---

## 🔗 Correctness & determinism

### "I pass `tiny.feed` single-threaded, then my two-thread version disagrees with itself"

That is a **data race in the hand-off** — the lesson of the week. The producer published the new `tail` (or the consumer the new `head`) with the wrong memory ordering, so the consumer reads a slot the producer hasn't finished writing, or the full/empty check is off by one and a tick is dropped or duplicated. Symptoms: missing/extra orders, or a stream that varies run to run. Fixes:

- Publish the index with `release`, observe it with `acquire` (never `relaxed` on the publish). See [`03-memory-orderings.md`](../03-memory-orderings.md).
- Get the full/empty convention right: a power-of-two capacity with masked indices and one reserved slot (or a separate size) avoids the off-by-one. See [`04-spsc-ring-buffer.md`](../04-spsc-ring-buffer.md).

Confirm with ThreadSanitizer:

```bash
cmake -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DENABLE_TSAN=ON -DCSOT_PIPE_SRC=pipeline.cpp
cmake --build build-tsan -j
./build-tsan/pipeline_runner data/tiny.feed      # TSan prints the racing accesses
```

The judge runs your submission several times and **rejects it if the order stream changes between runs** — a race that "passes sometimes" still fails (PIPELINE_SPEC.md §7-§8).

### "My orders are right but in the wrong order / a few are missing"

Your ring buffer is **not order-preserving** or it is **dropping** under back-pressure. An SPSC queue is FIFO by construction — if yours isn't, the bug is in the index arithmetic. Under back-pressure (queue full) the producer must **wait**, never overwrite an unconsumed slot. Print `num_orders` vs the reference count and `diff` to find the first divergence.

### "My positions go wrong after a while"

You parallelised the **strategy**, not just the decode. The strategy is stateful and order-dependent: each tick's fill updates the position the next tick reads (PIPELINE_SPEC.md §5). It must run on a **single** consumer thread, in order. Only the decode stage and the hand-off are parallel with it.

### "My prices are off by a rounding"

You recomputed the price instead of copying the tick field. A BUY uses `ask_px`, a SELL uses `bid_px`, copied verbatim from the decoded tick (PIPELINE_SPEC.md §6). The harness re-encodes `round(price * 10000)`; if you derived the price some other way it won't round-trip.

### "How do I hand-check `tiny.feed`?"

Dump it and check against the spec; the expected order stream is in `data/tiny.orders.json`:

```bash
python3 data/gen_feed.py --dump data/tiny.feed
python3 data/gen_feed.py --orders data/tiny.feed     # the reference order stream JSON
diff <(./build/pipeline_runner data/tiny.feed 2>/dev/null) data/tiny.orders.json
```

A clean `diff` means your order stream (and its checksum) matches.

---

## ⚡ Performance

### "Two threads are no faster than one — or slower!"

Usual suspects, in order:

1. **You used a mutex / `std::queue`.** A lock serialises the producer and consumer — exactly what you split apart. Replace it with a lock-free SPSC ring buffer ([`04-spsc-ring-buffer.md`](../04-spsc-ring-buffer.md)).
2. **`head` and `tail` share a cache line.** The producer writes `tail`, the consumer writes `head`; if they sit in the same 64-byte line they ping-pong it on every push/pop (false sharing, Week 3). Pad them apart with `alignas(64)`.
3. **The stages are unbalanced.** If the strategy dominates, the decode thread idles and you're back to one hot core. Make the strategy cheap (rolling sums, Week 2) so the two stages take comparable time.
4. **`seq_cst` everywhere.** Correct but it inserts full barriers an SPSC queue doesn't need; use release/acquire.

### "Throughput swings wildly between runs"

1. **Unpinned threads.** The scheduler migrates the producer and consumer across cores, trashing their caches. Pin both to distinct cores with `sched_setaffinity` ([`05-pipeline-pinning-and-backpressure.md`](../05-pipeline-pinning-and-backpressure.md)).
2. **Benchmark hygiene** (same as Week 1): lock the frequency governor, disable turbo, close background apps.

### "perf shows time spinning, not working"

A busy-wait consumer burns cycles when the queue is empty. That's fine for a saturated feed but shows up as spin in `perf`. Record per-thread:

```bash
perf record -F 999 -g --call-graph dwarf ./build/pipeline_runner data/large.feed
perf report
perf stat -e cache-misses,LLC-load-misses,context-switches ./build/pipeline_runner data/large.feed
```

A `head`/`tail` false-sharing bug lights up `cache-misses`; an unpinned pipeline lights up `context-switches`.

---

## 🐍 Feed generator (`gen_feed.py`)

### "Two students get different feeds from the same seed"

The seed only guarantees identical output for **identical arguments and unchanged source**. Check `--accesses`, `--seed`, and that nobody edited `gen_feed.py` (`diff` against the upstream copy).

### "`pipeline_runner` says the feed size isn't a multiple of 40"

The feed is a raw array of 40-byte `WireTick` records. A truncated or text-edited file breaks that invariant. Regenerate it; never open `.feed` files in a text editor and save.

---

## 🆘 When Nothing Works

Start from the golden file, single-threaded (the shipped stub):

```bash
diff <(./build/pipeline_runner data/tiny.feed 2>/dev/null) data/tiny.orders.json
```

If even the small `tiny.feed` produces a wrong stream with the **stub**, regenerate your golden files (`gen_feed.py --tiny` then `--orders`). If your own pipeline fails but the stub passes, the bug is in your hand-off, not the strategy. Then the standard escalation:

1. Reproduce on the smallest input (`tiny.feed`), single-threaded.
2. Re-run under ThreadSanitizer (`-DENABLE_TSAN=ON`) — it names the racing lines.
3. Run your two-thread `run()` 10 times in a loop; if the checksum changes, you have a race even if TSan is quiet on a tiny input.
4. Re-run under `-fsanitize=address,undefined` (`-DENABLE_SANITIZERS=ON`) for out-of-bounds ring indexing.
5. If still stuck, post the full command, full output, your OS, `nproc`, and the smallest reproducing feed to the CSoT group.

You'll get unstuck. Everyone does. 🚀
