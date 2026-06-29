# 05 — The Network Pipeline, End to End

> **TL;DR** — This is the capstone wiring. One file, `gateway.cpp`, runs three things at once: an **ingest** thread that `epoll`/`recv`s datagrams off the wire and pushes decoded ticks into a **Week-4 SPSC ring**, a **strategy** thread (pinned, Week 3) that pops ticks and runs the **zero-alloc (Week 2) Week-1 z-score** to emit the order stream, and the **ACK-window** flow control that ties the sender's pace to your true throughput. Every week of the track shows up in this one diagram. The new craft is keeping the syscall/kernel jitter on the ingest core so the strategy core never stalls.

You've built each piece. [`01`](./01-sockets-and-the-kernel-boundary.md)–[`04`](./04-tcp-vs-udp-and-framing.md) added the wire; Weeks 1–4 built everything behind it. This file is the assembly and the measurement.

---

## 1. The whole platform, in one picture

```text
                       gateway.cpp  (one translation unit, the judge compiles it)
   ┌───────────────────────────────────────────────────────────────────────────────┐
   │                                                                                 │
   │   INGEST thread (pinned core A)                  STRATEGY thread (pinned core B) │
   │   ─────────────────────────────                  ────────────────────────────── │
   │   epoll_wait → drain recv()  ┐                 ┌  pop tick                       │
   │   decode WireTick → Tick     │   SPSC ring     │  z-score over 64-window         │
   │   try_push(tick) ────────────┼──►[ . . . . ]──►┤  (Week-2 rolling sums)          │
   │   send ACK (every K dgrams)  ┘   (Week 4)      └  emit OrderRecord → out[]        │
   │        ▲                                                                          │
   └────────┼──────────────────────────────────────────────────────────────────────┘
            │  UDP datagrams (seq+count+WireTick[]), ACK-window throttled
   ┌────────┴─────────────┐
   │  judge feed SENDER    │   ← lives in the judge (locally: harness/main.cpp)
   └──────────────────────┘
```

| Layer | Week it came from | What it does here |
|---|---|---|
| z-score strategy + fill | **1** | the unchanged ranked algorithm ([`STRATEGY_SPEC.md`](../week-1/project/STRATEGY_SPEC.md)) |
| zero-alloc hot path, rolling sums | **2** | strategy stage cheap; nothing `malloc`s after `on_init` |
| pinned threads, `sched_setaffinity` | **3** | ingest on core A, strategy on core B, distinct physical cores |
| lock-free SPSC ring, release/acquire | **4** | the hand-off that decouples ingest jitter from the strategy |
| `epoll`/`recv`/`send`, ACK-window | **5** | the wire ingest and flow control — this week |

This is what "unify everything into a complete quant platform" means concretely: not five projects glued together, but **one data path** where each week is a layer the tick passes through, wire to order.

---

## 2. `run()` owns the threads; `on_init()` owns the memory

The Week-4 structure carries over verbatim — spawn the ingest thread, run the strategy on the calling thread, allocate everything up front:

```cpp
std::size_t run(int fd, std::size_t n, csot::OrderRecord* out) override {
    make_nonblocking(fd);                 // we drive readiness ourselves (02/03)
    std::atomic<std::size_t> produced{0}; // ticks pushed by ingest
    std::size_t consumed = 0, num_orders = 0;

    std::thread ingest([&]{
        pin_to(core_a_);
        // epoll_wait → drain recv → decode → ring.try_push (spin on full) → ACK every K
        // stop after n ticks ingested
    });

    pin_to(core_b_);                       // strategy runs on THIS thread
    while (consumed < n) {
        csot::Tick t;
        while (!ring_.try_pop(t)) { __builtin_ia32_pause(); }   // back-pressure (Week 4)
        num_orders += strategize(t, out + num_orders);          // Week-1 spec, Week-2 fast
        ++consumed;
    }
    ingest.join();
    return num_orders;
}
```

Everything that allocates — the ring storage, per-symbol z-score state, the interned `"SYM<k>"` name table, the `epoll` `events[]` buffer, the datagram receive buffer — is built in `on_init(num_symbols)`. After that, `run()` is heap-free. The Week-2 rule didn't relax; the wire just added two more buffers to pre-allocate.

> 💡 The judge times **exactly `run()`** — wire receive + `epoll` + ring + strategy, overlapped. Your score is wall-clock `run_ns` over the whole feed plus throughput. So shaving a syscall or balancing the stages shows up directly on the board.

---

## 3. Keep the syscalls off the strategy core

The single most important Week-5 discipline: **the strategy thread must never make a syscall.** No `recv`, no `send`, no `epoll_wait`, no logging. All of that lives on the ingest thread. Why it matters:

- A syscall is ~100+ ns of mode switch + a scheduling opportunity ([`01`](./01-sockets-and-the-kernel-boundary.md)). If the strategy thread does one per tick, you've reintroduced the exact tax the pipeline exists to hide.
- The kernel can **preempt** a thread at a syscall boundary. A strategy thread that never enters the kernel is far less likely to be descheduled mid-window, which tightens your tail latency (and the leaderboard rewards a tight distribution).

The ring is the firewall: ingest deals with the messy, jittery kernel; the strategy sees only a clean stream of `csot::Tick` and pure user-space arithmetic. This is why the Week-4 decoupling, optional when the feed was an array, is **load-bearing** once the feed is a socket.

---

## 4. Back-pressure all the way to the wire

From [`04`](./04-tcp-vs-udp-and-framing.md) §4: the ACK-window couples the sender to your end-to-end speed. Concretely, the failure you must avoid and the one you get for free:

- **Avoid:** ingest reads datagrams and ACKs them *before* pushing to the ring, decoupling the ACK from real progress. Now if the strategy stalls, the ring fills, but you keep ACKing, the sender keeps sending, and your bounded receive buffer overflows → dropped datagram → broken determinism. **ACK only after `try_push` succeeds.**
- **Free:** ACK after the datagram's ticks are all in the ring. If the strategy is slow, `try_push` spins, you stop ACKing, the window closes, the sender waits. The pipeline self-throttles, nothing drops, and the slowest stage sets the pace — exactly Little's law ([Week 4 `01`](../week-4/01-from-batch-to-streaming.md) §5) extended across the kernel.

Balance the three stages the way Week 4 balanced two: ingest (`recv`+decode), hand-off (ring), strategy. If `recv`+decode dominates, batch harder (bigger drains, ACK less often); if the strategy dominates, bring back the Week-2 rolling sums so it isn't recomputing a 64-element variance per tick.

---

## 5. Measure wire-to-decision, and prove determinism first

Three numbers, same discipline as every week:

1. **Serial baseline.** The shipped stub: single-threaded blocking `recv` → decode → strategy inline. Record its throughput: `\_\_\_\_\_` M ticks/s. It's correct and slow — your floor.
2. **Your pipeline.** `epoll`/busy-poll ingest + ring + pinned strategy. Record throughput and speedup vs. the stub. Expect the overlap win plus the syscall amortization.
3. **A `epoll` vs. busy-poll bake-off.** Same pipeline, swap only the ingest wait strategy ([`02`](./02-blocking-nonblocking-and-readiness.md) §3). On a saturated single feed, busy-poll often edges `epoll`; measure it on the judge-like build, don't assume.

```bash
# throughput printed on stderr by the harness (it runs the UDP sender + your run())
./build/gateway_runner data/large.feed
perf stat -e context-switches,cache-misses ./build/gateway_runner data/large.feed
```

> 📌 **Determinism is the gate, not a nicety** (GATEWAY_SPEC.md §7–§8). Before chasing throughput: run `run()` 10× and confirm the order-stream checksum is identical every time, and that a **TSan** build is clean. A network race (ACKing too early, an off-by-one ring index, a torn datagram read) often "passes sometimes" — the judge runs K times and rejects any submission whose stream changes. High `context-switches` in `perf` usually means an unpinned thread or a strategy thread that's syscalling; high `cache-misses` means `head`/`tail` false sharing (Week 4) or a too-deep ring.

---

## 🎯 Key Takeaways

- The capstone is **one data path** through every week: wire (`epoll`/`recv`) → SPSC ring → pinned, zero-alloc Week-1 strategy → order stream. Not five projects; five layers.
- `run()` spawns the ingest thread and runs the strategy on the calling thread; **`on_init` allocates** the ring, per-symbol state, names, `events[]`, and recv buffer. `run()` stays heap-free.
- **No syscalls on the strategy core.** All `recv`/`send`/`epoll_wait` live on the ingest thread; the ring is the firewall that keeps kernel jitter off the strategy's tail latency.
- **ACK only after `try_push` succeeds** so back-pressure reaches the wire: slow strategy → full ring → no ACK → sender stalls → nothing drops. Balance the three stages.
- **Prove determinism first** (10× checksum + TSan), then optimize throughput. Bake off `epoll` vs. busy-poll on the judge-like build; pick the faster *correct* one.

---

## 📚 Further Reading — End-to-End Network Pipelines

- 🎬 [CppCon 2017 — Carl Cook, "When a Microsecond Is an Eternity"](https://www.youtube.com/watch?v=NH1Tta7purM) — the whole-pipeline HFT mindset: never sleep, never syscall on the hot path, pin everything.
- 📰 [Rigtorp — "Low latency tuning guide"](https://rigtorp.se/low-latency-guide/) — affinity, isolation, busy-polling, and NIC settings as one coherent checklist.
- 📰 [Martin Thompson — "Mechanical Sympathy"](https://mechanical-sympathy.blogspot.com/) — the design philosophy ("know your hardware") behind decoupling ingest from compute.
- 📖 ["Systems Performance" (Brendan Gregg), 2nd ed., ch. 10 (Network)](https://www.brendangregg.com/systems-performance-2nd-edition-book.html) — measuring where network-path time actually goes.

---

## ▶️ Next

[`06-bonus-kernel-bypass-and-beyond.md`](./06-bonus-kernel-bypass-and-beyond.md) — ⭐ bonus: `recvmmsg`, `SO_BUSY_POLL`, `io_uring`, `AF_XDP`, and DPDK/onload — where the next order of magnitude lives once the kernel itself is the bottleneck. The closing teaser of the whole track. ⚡
