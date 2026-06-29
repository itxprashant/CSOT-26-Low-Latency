# Gateway Spec — Reference Networked Feed Gateway

> **This file is a frozen competition contract.** Every submission must turn the same UDP tick feed into the same order stream, by receiving datagrams off a connected loopback socket, decoding each `WireTick`, handing it across a lock-free SPSC ring to the **unchanged Week-1 z-score strategy**, and emitting the exact reference order stream — while acknowledging datagrams so the feed stays lossless. The challenge is **not** to invent a metric, a trading rule, or a network protocol; it is to build the wire → `epoll` ingest → SPSC hand-off → strategy pipeline *correctly*, *deterministically*, and *as fast as the kernel and two cores allow*.

---

## 1. Goal

The Week-5 capstone is networked, but it is **not** a "design your own metric" contest, **not** a "design your own signal" contest, and **not** a "design your own protocol" contest.

Every participant receives the same:

- `WireTick` / `OrderRecord` / `FeedDatagramHeader` / `Gateway` ABI from [`include/gateway.hpp`](./include/gateway.hpp)
- the frozen `Tick` / `Order` / `Strategy` ABI from [`include/strategy.hpp`](./include/strategy.hpp)
- the UDP datagram wire format (§2) and frozen constants (§3)
- the decode rule (§4), the **unchanged** z-score strategy + fill model (§5, [`STRATEGY_SPEC.md`](../../week-1/project/STRATEGY_SPEC.md)), the order-stream definition (§6), and the ACK-window flow-control rule (§7)

Every participant must emit the same `OrderRecord` stream as the judge reference for the same input feed. If your stream differs, your submission is incorrect. Among correct, deterministic submissions, the leaderboard ranks by **wall-clock time to run the whole gateway** (`run()`) over a large held-out feed (and throughput) — the same "fastest correct implementation" game as Weeks 1–4, on a workload that finally puts the feed on the wire.

This is the capstone: a correct gateway *uses* every prior week — the Week-1 strategy, the Week-2 zero-alloc hot path and rolling sums, the Week-3 pinning, the Week-4 lock-free SPSC ring — behind a new Week-5 network ingest. One `gateway.cpp` is the complete platform.

---

## 2. The Wire Format (frozen)

The feed travels as **UDP datagrams** on a connected loopback socket. Each datagram is a 16-byte [`FeedDatagramHeader`](./include/gateway.hpp) followed by `count` contiguous `WireTick` records, little-endian:

```text
   ┌──────────────────────── one UDP feed datagram ────────────────────────┐
   │ FeedDatagramHeader (16 B)                │  WireTick[count]            │
   │   offset 0 : uint64  seq                  │   count × 40 B              │
   │   offset 8 : uint32  count   (1..MAX_BATCH)                            │
   │   offset 12: uint32  _pad    (zero)       │                            │
   └────────────────────────────────────────────────────────────────────────┘
```

Each `WireTick` is exactly 40 bytes, identical to Week 4 (and identical on disk and on the wire):

```text
offset 0  : uint64  timestamp_ns   (exchange wall-clock, ns since epoch, non-decreasing)
offset 8  : int64   bid_px_fp      (FIXED-POINT best bid: real_bid * PRICE_SCALE)
offset 16 : int64   ask_px_fp      (FIXED-POINT best ask: real_ask * PRICE_SCALE)
offset 24 : uint32  symbol_id       (0 .. NUM_SYMBOLS-1)
offset 28 : uint32  bid_qty         (shares at best bid; always > 0)
offset 32 : uint32  ask_qty         (shares at best ask; always > 0)
offset 36 : uint32  _reserved        (always zero)
```

Guarantees you can rely on (on loopback, with the ACK-window of §7 enforced by the sender):

- **Datagrams arrive in `seq` order, contiguous (0, 1, 2, …), with none lost or duplicated.** You may treat `seq` as a monotonic counter and assert it; you do **not** need a reorder buffer or retransmit logic.
- One `recv()` returns **exactly one datagram** (UDP boundary preservation). Size the buffer you `recv()` into to at least `MAX_FEED_DATAGRAM_BYTES` (= 1296 bytes).
- `count` is in `[1, MAX_BATCH]`. The total number of `WireTick`s across all datagrams equals **`n`** (the `run()` argument), so once you have processed `n` ticks the feed is over. The sender may also send a short final datagram; reaching `n` ticks is the authoritative end.
- The `WireTick` field guarantees are identical to Week 4: `symbol_id ∈ [0, NUM_SYMBOLS)`, `bid_px_fp <= ask_px_fp`, both positive, `bid_qty`/`ask_qty` strictly positive, `_reserved` zero (do not read), `timestamp_ns` non-decreasing, ticks processed **in send order** (the strategy is stateful).

The on-disk `.feed` file (what [`data/gen_feed.py`](./data/gen_feed.py) emits and what the harness reads) is still a flat array of `WireTick` records, **no header** — the datagram framing is a transport detail the judge's sender (and the local harness) apply when they put the feed on the wire. `data/tiny.feed` is a small golden file and `data/tiny.orders.json` holds its exact reference order stream.

---

## 3. Constants (frozen)

These constants are part of the spec. Do not change them. They live in [`include/gateway.hpp`](./include/gateway.hpp).

| Name | Value | Meaning |
|---|---:|---|
| `NUM_SYMBOLS` | `1024` | Number of distinct symbol ids. `symbol_id ∈ [0, 1024)`. `on_init` is called with this. |
| `PRICE_SCALE` | `10000` | Fixed-point scale: stored `*_px_fp` = real price × 10000. |
| `MAX_BATCH` | `32` | Max `WireTick`s the sender packs into one feed datagram. |
| `ACK_WINDOW` | `128` | The sender stays at most this many datagrams ahead of your highest ACK (§7). |
| `WINDOW` | `64` | Rolling mid-price window (from `STRATEGY_SPEC.md` §3). |
| `ENTRY_Z` / `EXIT_Z` | `2.0` / `0.5` | Entry / exit z-thresholds (from `STRATEGY_SPEC.md` §3). |

`MAX_FEED_DATAGRAM_BYTES` = `16 + MAX_BATCH × 40` = **1296**. Size your receive buffer to at least this.

### Symbol naming (frozen, unchanged)

Symbol id `k` interns to the exact string `"SYM<k>"` — `0 → "SYM0"`, … `1023 → "SYM1023"`. Build the table once in `on_init`, point `Tick::symbol` at the stable string. The reference order stream identifies symbols by exactly this string; any other naming fails correctness.

---

## 4. The Decode Contract (ingest side)

Identical to Week 4. For every `WireTick w` received off the wire, the ingest thread reconstructs the frozen `csot::Tick` the strategy consumes:

```text
tick.timestamp_ns = w.timestamp_ns
tick.symbol       = intern("SYM" + to_string(w.symbol_id))   // stable address, set up in on_init
tick.bid_px       = (double) w.bid_px_fp / PRICE_SCALE        // e.g. 1000500 -> 100.05
tick.ask_px       = (double) w.ask_px_fp / PRICE_SCALE
tick.bid_qty      = w.bid_qty
tick.ask_qty      = w.ask_qty
```

This decode runs on the **ingest** thread, off the strategy's critical path — alongside the `recv()` syscalls. Keep it cheap (an O(1) `names_[symbol_id]` lookup, not a per-tick string build) so the ingest stage keeps up with the wire and the strategy stage isn't starved.

> 📌 The division by `PRICE_SCALE` reconstructs a price exactly representable as a `double`, and the strategy copies it into orders verbatim (§6), so the round-trip back to fixed-point is exact and the order stream diffs as integers, never floats.

---

## 5. The Strategy (unchanged) and the Fill Model

The strategy thread drives the **frozen Week-1 strategy spec, unchanged**: per-symbol z-score mean reversion over a rolling 64-mid-price window. The full algorithm — mid-price, warm-up, population mean/standard deviation, z-score, entry at `|z| >= 2.0`, exit at `|z| <= 0.5`, at most one unit per symbol — is defined in [`STRATEGY_SPEC.md`](../../week-1/project/STRATEGY_SPEC.md) §2–§7. **Read it; it is authoritative and is not re-printed here.**

The strategy must be driven exactly as the Week-1 engine did, in send order:

```text
for each decoded tick t (in feed/send order):
    orders = strategy.on_tick(t)              // 0 or 1 order (STRATEGY_SPEC §5)
    for each order o in orders:
        record o in the output stream (tagged with t's tick index)
        strategy.on_fill(o, o.price, o.qty)   // deterministic immediate fill (STRATEGY_SPEC §8)
```

The Week-1 fill model (`STRATEGY_SPEC.md` §8) is unchanged: every emitted order fills immediately at its own `price` and `qty`, and `on_fill` updates `position` — never `on_tick`. Because `on_fill` mutates the per-symbol position the next tick reads, **each tick's fill must be applied before the next tick for that symbol is processed.** A single in-order strategy thread satisfies this automatically; this is why the hand-off must preserve order and why the strategy stays on **one** thread, never sharded.

---

## 6. The Order Stream You Emit

Identical to Week 4. Your `run()` writes an array of `OrderRecord` (see [`include/gateway.hpp`](./include/gateway.hpp)) and returns its length. Each record is `{ tick_index, order }`, where `tick_index` is the index (into the feed, in send order) of the `WireTick` that produced the order. Records must appear in **tick order** (non-decreasing `tick_index`), which an in-order pipeline produces for free.

The judge (and the local harness) serialise the stream to JSON and diff it against the reference. The canonical per-order fields are:

| Field | Source | Notes |
|---|---|---|
| `tick_index` | index of the producing `WireTick` (send order) | strictly increasing across the stream (≤ 1 order per tick) |
| `timestamp_ns` | `feed[tick_index].timestamp_ns` | reconstructed from the tick; informational |
| `symbol` | `order.symbol` | the interned `"SYM<id>"` string (§3) |
| `side` | `order.side` | `0` = BUY, `1` = SELL |
| `price_fp` | `round(order.price * PRICE_SCALE)` | **fixed-point integer** — never the raw double |
| `qty` | `order.qty` | strictly positive |

> ⚠️ **Copy, don't recompute, the price.** A BUY uses `t.ask_px` and a SELL uses `t.bid_px`, copied verbatim from the decoded tick. Because those came from `*_px_fp / PRICE_SCALE`, re-encoding `round(price * PRICE_SCALE)` recovers the original integer exactly. Recompute or round it yourself and you will diverge.

Equality means the same sequence of `(tick_index, symbol, side, price_fp, qty)` tuples as the reference, in the same order. The harness prints an FNV-1a `checksum` over the stream for a fast same/not-same check; `diff` against `data/tiny.orders.json` is authoritative. **The order stream is byte-identical to Week 4's for the same feed** — same ticks, same strategy — so `tiny.orders.json` is carried forward unchanged.

---

## 7. Flow Control: The ACK-Window (the one new mechanism)

This is the only genuinely new contract in Week 5, and it is what makes a UDP feed safe to grade.

**The sender's promise.** The judge's feed sender numbers datagrams `seq = 0, 1, 2, …` and stays at most **`ACK_WINDOW` datagrams ahead** of the highest `seq` you have acknowledged. Formally, it never sends a datagram with `seq ≥ (your last ACKed next_seq) + ACK_WINDOW`.

**Your obligation.** As you drain datagrams, send a small **ACK** back on the same connected `fd`: a bare `std::uint64_t` equal to `next_seq` — the count of datagrams you have contiguously received (equivalently, the `seq` you expect next):

```cpp
std::uint64_t ack = next_seq;          // "I have everything with seq < next_seq"
send(fd, &ack, sizeof(ack), 0);        // 8-byte ACK datagram on the same fd
```

You must send an ACK **often enough that the sender can make progress** — at least once per `ACK_WINDOW` datagrams. The simplest correct rule (what the stub does) is to ACK after every datagram; the fast rule is to **batch** ACKs (one per K datagrams, K < `ACK_WINDOW`) to cut `send()` syscalls.

**Why this is lossless on loopback.** At most `ACK_WINDOW` datagrams are ever unacknowledged, so at most `ACK_WINDOW` can be sitting unread in your kernel receive buffer. The judge (and the harness) size `SO_RCVBUF ≥ ACK_WINDOW × MAX_FEED_DATAGRAM_BYTES`, so the buffer **cannot overflow** — and on loopback an un-overflowed buffer never drops or reorders. No loss + in order ⇒ the order stream is a pure function of the feed (§8).

**Back-pressure composes end-to-end.** ACK **only after** the datagram's ticks are in your ring (`try_push` succeeded). Then if your strategy thread falls behind, your ring fills, your ingest blocks on `try_push`, stops `recv`ing, stops ACKing, the window closes, and the sender throttles itself to your true end-to-end speed. Nothing drops; the slowest stage sets the pace. ACKing *before* `try_push` breaks this: the sender races ahead, the buffer overflows, a datagram drops, determinism dies (§8, §10).

> 🎯 **You win by amortizing, not cutting corners.** Drain aggressively (read to `EAGAIN`), ACK every K datagrams, and the sender rarely stalls while your `send()` count stays low. A gateway that ACKs every single datagram is correct but pays a syscall per datagram.

---

## 8. The Determinism Guarantee (the whole point)

A correct gateway is **deterministic**: because the feed is delivered lossless and in order (§2, §7), the strategy is a pure function of the in-order tick sequence, and an SPSC ring is an order-preserving channel. Therefore, no matter how the kernel schedules your threads, how datagrams interleave with ACKs, or how you size your ring, the strategy sees **exactly the feed order** and emits **exactly one** order stream.

This is what makes the contest fair and cheat-detectable:

1. **The judge computes the answer key in send order.** The reference decodes and runs the strategy single-threaded over the feed; your networked pipeline must match it byte-for-byte.
2. **A bug is detectable.** If your ring drops/duplicates/reorders a tick, or you ACK too early and the receive buffer overflows (losing a datagram), the emitted stream changes — and often changes *run to run*. The judge runs your `run()` **K times** and rejects any submission whose order stream is not identical across all K runs.
3. **You may not parallelise the strategy.** The strategy is stateful and order-dependent (§5); only the *ingest* (recv + decode) and the *hand-off* are parallel with it. Sharding ticks across strategy threads breaks order and fails determinism.

> ⚠️ The Week-5 subtle bugs are **lost/reordered messages** (Week 4) *plus* **dropped datagrams from receive-buffer overflow** (the new one). Both manifest as a changed, often non-deterministic, order stream. §10 lists the symptoms.

---

## 9. Correctness & Determinism Rules

A submission is **correct** if, for the same feed, every `OrderRecord` equals the judge reference exactly (`tick_index`, `symbol`, `side`, `price_fp`, `qty`), in the same order, with the same total count. The judge reports the first differing record as feedback, e.g. `order 412 (tick 90183) side: got BUY want SELL`.

A submission must also be **deterministic**: the judge runs your `run()` K times (see [`README.md`](./README.md) → leaderboard) and requires **identical** order streams across all K runs. A submission that produces different streams on different runs is flagged non-deterministic and is **not ranked**, even if one run happened to match.

Sanity identities that hold for any feed (unchanged from Week 4):

```text
records are in non-decreasing tick_index order, ≤ 1 record per tick
every order's price_fp == feed[tick_index].(bid_px_fp if SELL else ask_px_fp)
no order is emitted while a symbol is in warm-up (< 64 mids)   (STRATEGY_SPEC §5)
|position| never exceeds 1 for any symbol                        (STRATEGY_SPEC §3)
total ticks processed == n                                       (no datagram lost)
```

Things that make a submission **incorrect** or **rejected**:

- **Dropping a datagram** by ACKing before `try_push` (or never ACKing) so the receive buffer overflows → missing ticks → wrong/short stream, usually non-deterministic → rejected.
- A ring-buffer bug that **drops, duplicates, or reorders** ticks (wrong order stream, usually non-deterministic → rejected).
- A data race on the ring `head`/`tail` (wrong memory ordering → half-written tick → garbage).
- **A syscall on the strategy thread** that perturbs ordering, or sharding the stateful strategy across threads (breaks order).
- Recomputing or rounding the order price instead of copying the tick field (§6).
- Interning symbols as anything other than `"SYM<id>"` (§3).
- Any deviation from the frozen z-score / fill rules in `STRATEGY_SPEC.md`.
- Building a reorder/retransmit layer that assumes loss and accidentally diverges from the in-order reference.

---

## 10. Performance Rules

Correctness (and determinism) is binary. Performance is ranked among correct, deterministic submissions only.

The leaderboard ranks by:

1. lower **wall-clock time** for `run()` over the hidden feed
2. higher **throughput** (ticks/second) on the same feed
3. (tie-break) earlier valid submission

The judge box has **4 vCPUs** (`c7i.xlarge`). The gateway naturally uses **two** hot threads (one ingest, one strategy); the spare cores are pinning headroom and the OS. The win is keeping the kernel/wire off the strategy core while the two stages overlap through a hand-off that never stalls.

### Hot-path expectations

| Week | Expected implementation focus for a competitive `run()` |
|---|---|
| 5 | A **non-blocking** UDP socket drained by an **`epoll`** (or pinned busy-poll) ingest loop that **batches** `recv()`s (drain to `EAGAIN`) and **batches ACKs** (one per K datagrams); each decoded tick handed across a lock-free **SPSC ring buffer** (Week 4: power-of-two, `alignas(64)` head/tail, acquire/release) to a **pinned** (Week 3) strategy thread running the **zero-alloc** (Week 2) rolling-sum z-score; **no syscalls on the strategy thread**; everything allocated in `on_init`; ACK after `try_push` so back-pressure reaches the wire. |

The anti-saturation premise: a correct single-threaded blocking-`recv` gateway is easy (it's the stub), but absorbing the kernel boundary on a dedicated ingest core — amortizing syscalls, never stalling the strategy, keeping the receive buffer drained — has real headroom over that baseline. For a single saturated feed, a pinned busy-poll ingest can beat `epoll`; measure it on the judge-like build. Expect the board to keep moving after the first correct upload.

---

## 11. Common Pitfalls

- **ACKing before `try_push` succeeds.** Decouples flow control from real progress; the sender races ahead, your receive buffer overflows, a datagram drops, and determinism dies. ACK *after* the ticks are in the ring (§7).
- **A syscall on the strategy thread.** `recv`/`send`/`epoll_wait`/logging on the strategy core reintroduces the ~100+ ns kernel tax and a preemption point per tick. Keep all syscalls on the ingest thread; the ring is the firewall ([`05`](../05-the-network-pipeline-end-to-end.md) §3).
- **Edge-triggered `epoll` without a drain loop.** With `EPOLLET` you are notified once per arrival; read only one datagram and the rest sit unread with no further wake-up → the gateway hangs short of `n`. Drain to `EAGAIN` ([`03`](../03-epoll-event-loop.md) §3).
- **A receive buffer smaller than one datagram.** One UDP `recv()` returns one datagram; a buffer < `MAX_FEED_DATAGRAM_BYTES` truncates it and you lose ticks. Size it to `MAX_FEED_DATAGRAM_BYTES`.
- **A mutex (or `std::queue`) in the hand-off.** A lock serialises ingest and strategy — the very thing you split. Use the lock-free SPSC ring (Week 4).
- **`head`/`tail` on the same cache line; wrong publish ordering; `seq_cst` everywhere; off-by-one full/empty.** All the Week-4 ring pitfalls still apply ([`PIPELINE_SPEC.md`](../../week-4/project/PIPELINE_SPEC.md) §10).
- **Heap inside `run()`.** Allocate the ring, recv buffer, `epoll` events[], per-symbol state, and interned names in `on_init()`.
- **Unpinned threads.** Without `sched_setaffinity`, ingest and strategy migrate, trash caches, and add jitter to your timed `run()`.
- **Recomputing the price** or interning symbols wrong (§6, §3).

---

## 12. Why This Is Still the Same Track

The workload is networked — receive datagrams on one thread, strategize on another, hand off without a lock, acknowledge to stay lossless. But the objective is identical to Weeks 1–4: everyone computes the *same* fixed answer (the Week-1 order stream), and the leaderboard measures **systems-engineering speed**, not metric, signal, or protocol cleverness. The wire supplies a jittery, syscall-bound input; the leaderboard measures how well you keep it off the strategy's hot path while two cores stay busy through a correct, race-free, lossless, lock-free pipeline.

That distinction — and assembling every prior week behind it — is the whole capstone.
