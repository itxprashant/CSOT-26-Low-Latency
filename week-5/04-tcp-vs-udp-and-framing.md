# 04 — TCP vs. UDP, Framing, and the ACK-Window

> **TL;DR** — TCP gives you a reliable, ordered **byte stream** at the cost of head-of-line blocking and a kernel doing retransmits; UDP gives you raw, unordered, possibly-lost **datagrams** with nothing in the way — which is why real market-data feeds are UDP multicast. This week's feed is UDP on loopback. To make a UDP feed safe to *grade* (every tick delivered, in order, every run) the protocol adds two things: a **sequence number** on every datagram and an **ACK-window** flow-control rule that keeps the sender from ever overrunning your receive buffer. Lossless + in-order ⇒ the order stream is a pure function of the feed ⇒ the Week-4 determinism gate still holds.

[`01`](./01-sockets-and-the-kernel-boundary.md)–[`03`](./03-epoll-event-loop.md) covered *how* to read a socket. This file covers *what's on it*: the transport, the frame, and the one new mechanism that makes a lossy protocol deterministic enough to rank.

---

## 1. TCP vs. UDP: stream vs. datagram

| | TCP (`SOCK_STREAM`) | UDP (`SOCK_DGRAM`) |
|---|---|---|
| Delivery | reliable — lost segments retransmitted | best-effort — lost datagrams are *gone* |
| Order | in-order guaranteed | may arrive reordered |
| Boundaries | **none** — a byte stream; you must frame | **preserved** — one `recv` = one datagram |
| Head-of-line blocking | yes — a lost segment stalls *everything* behind it | no — datagram N+1 doesn't wait on N |
| Per-message overhead | connection state, ACKs, congestion control | a thin header; you control the rest |

The HFT-relevant line is **head-of-line blocking**. On TCP, one dropped packet makes the kernel hold *all* later bytes until the retransmit arrives — a single loss adds a full round-trip of latency to everything behind it. For market data, a tick that's 2 ms late is worthless; you'd rather know you missed it and move on. So exchanges publish over **UDP multicast**: lowest latency, no retransmit stalls, and the (rare) loss is handled by a separate recovery channel — not by making everyone wait.

> 💡 "Reliable" sounds strictly better until you're latency-bound. TCP trades latency for reliability *invisibly and unconditionally*. UDP hands you the raw wire and lets you choose your own reliability/latency point. This week we choose one explicitly.

---

## 2. Framing: turning a feed into datagrams

UDP preserves message boundaries, so framing is simple: each datagram is a small **header** followed by a batch of `WireTick`s. The frozen format (see [`GATEWAY_SPEC.md`](./project/GATEWAY_SPEC.md) §2 and [`gateway.hpp`](./project/include/gateway.hpp)):

```text
   ┌──────────────── one UDP datagram ─────────────────┐
   │ FeedDatagramHeader (16 B)  │  WireTick[count]      │
   │  seq:u64  count:u32  _pad  │  count × 40 B          │
   └────────────────────────────────────────────────────┘
```

```cpp
struct FeedDatagramHeader {
    std::uint64_t seq;     // 0, 1, 2, ... — contiguous datagram index
    std::uint32_t count;   // number of WireTicks following, 1..MAX_BATCH
    std::uint32_t _pad;    // zero
};
static_assert(sizeof(FeedDatagramHeader) == 16, "datagram header is part of the wire ABI");
```

- **`seq`** numbers the datagrams 0,1,2,… so the receiver can tell exactly what it has and detect any gap.
- **`count`** lets the sender pack up to `MAX_BATCH` ticks per datagram — the syscall-amortization lever from [`01`](./01-sockets-and-the-kernel-boundary.md). One `recv`, `count` ticks.
- The on-disk `.feed` file is still a flat `WireTick[]` (Week 4, unchanged); **framing is a transport detail** the judge's sender and your harness apply. The decode of each `WireTick` is identical to [`PIPELINE_SPEC.md`](../week-4/project/PIPELINE_SPEC.md) §4.

A bigger `MAX_BATCH` means fewer datagrams and fewer syscalls (good) but larger datagrams (closer to the loopback MTU, more copy per `recv`). The constant is frozen so everyone competes on the same frame size; your job is to drain it efficiently.

---

## 3. The loss problem, and why it's a *determinism* problem

If a datagram is lost or reordered, the receiver sees a different tick sequence, the stateful z-score strategy takes a different path, and the order stream changes. Worse, it changes **run to run** — a dropped datagram on one run and not the next means two different checksums. That detonates the Week-4 determinism gate (identical order stream across K runs), which the judge reuses verbatim.

On **loopback** (`127.0.0.1`), there's no physical link to corrupt or reorder packets — the kernel hands datagrams to the receiving socket in send order. The only way to lose one is **receive-buffer overflow**: if the sender pushes datagrams faster than your gateway drains them, `SO_RCVBUF` fills and the kernel **silently drops** the overflow ([`01`](./01-sockets-and-the-kernel-boundary.md) §4). So on loopback, "lossless" reduces to one rule: **never let the receive buffer overflow.** That's what flow control buys us.

---

## 4. The ACK-window: flow control that guarantees losslessness

The protocol's one new mechanism. The sender does **not** blast all datagrams as fast as it can. It stays at most **`ACK_WINDOW` datagrams ahead** of what the gateway has acknowledged:

```text
sender keeps:   (highest seq sent) − (last ACK received)  ≤  ACK_WINDOW

  acked          in flight (≤ WINDOW)        not yet sent
  ───────────┬───────────────────────────┬──────────────────►  seq
             ▲                           ▲
        last ACK from gateway      sender's send cursor
```

The gateway, as it drains datagrams, sends a tiny **ACK datagram** back on the same connected socket carrying `next_seq` — the count of datagrams it has contiguously received (equivalently, the seq it expects next):

```cpp
std::uint64_t ack = next_seq;        // "I have everything with seq < next_seq"
send(fd, &ack, sizeof(ack), 0);      // 8-byte ACK, same connected fd
```

Why this makes loss impossible on loopback: at most `ACK_WINDOW` datagrams are ever unacknowledged and therefore at most `ACK_WINDOW` can be sitting unread in your receive buffer. Size `SO_RCVBUF ≥ ACK_WINDOW × max_datagram` (the harness and judge do) and the buffer **cannot** overflow. No overflow ⇒ no drop ⇒ in-order, complete delivery ⇒ deterministic order stream.

And here's the elegant part — **the back-pressure composes end-to-end**:

```text
sender ──(ACK-window)──► socket buffer ──(your recv)──► SPSC ring ──(try_push)──► strategy
   ▲                                                        │
   └──────────────── if the strategy is slow, the ring fills, ingest stops
                      reading, stops ACKing, the window closes, the sender stalls ┘
```

If your strategy thread falls behind, your ring fills, your ingest thread blocks on `try_push`, stops `recv`ing, stops ACKing — and the sender throttles itself to your true end-to-end speed. The same back-pressure you built in Week 4 now reaches all the way back to the wire. Nothing is dropped; the slow stage just sets the pace.

> 📌 **You win by amortizing, not by cutting corners.** A wider effective window (drain aggressively, ACK every K datagrams instead of every one) means fewer ACK syscalls and the sender rarely stalls. A gateway that ACKs every single datagram is correct but pays a `send` per datagram. Batch the ACKs the way you batched everything else.

---

## 5. What you can rely on (and what you can't)

For this challenge, on loopback, with the ACK-window enforced by the judge's sender:

- ✅ **Datagrams arrive in `seq` order, contiguous, none lost.** You may assert `header.seq == expected` and treat a violation as a bug, not a case to recover from.
- ✅ **One `recv` = one datagram** (UDP boundary preservation). Size your buffer to `MAX_FEED_DATAGRAM_BYTES`.
- ✅ **The feed ends at `n` ticks** (the `run()` argument). When you've ingested `n`, you're done; a terminal/short datagram may also signal end-of-feed.
- ❌ Don't build a general reorder buffer or retransmit logic — that's real-internet UDP, not loopback, and it's explicitly out of scope ([`06`](./06-bonus-kernel-bypass-and-beyond.md) sketches where that lives).
- ❌ Don't ignore the window and "just `recv` in a loop" assuming infinite buffering — under-draining still overflows if you stop ACKing; the window only protects you if you keep acknowledging.

This is a deliberately *tamed* slice of network reality: enough to make you write real `epoll`/`recv`/`send` code and feel batching and back-pressure across the kernel boundary, without the nondeterminism that would make a fair leaderboard impossible.

---

## 🎯 Key Takeaways

- **TCP** = reliable, ordered byte stream with head-of-line blocking; **UDP** = best-effort datagrams with boundaries preserved and no stalls — why market data is UDP multicast.
- **Framing:** each datagram is a 16-byte `FeedDatagramHeader` (`seq`, `count`) + up to `MAX_BATCH` `WireTick`s. The `.feed` file stays a flat `WireTick[]`; framing is a transport detail.
- On **loopback**, the only way to lose a datagram is **receive-buffer overflow**; loss/reorder would break the **determinism gate**, so the protocol forbids it.
- The **ACK-window**: the sender stays ≤ `ACK_WINDOW` datagrams ahead of your ACKed `next_seq`, so at most `ACK_WINDOW` datagrams are ever unread — size `SO_RCVBUF` accordingly and overflow is impossible.
- Back-pressure **composes end-to-end**: slow strategy → full ring → ingest stops ACKing → sender stalls. Lossless, in-order, deterministic. You win by **amortizing ACKs** (one per K datagrams), not by dropping work.

---

## 📚 Further Reading — Transports & Reliable UDP

- 📰 [Cloudflare — "Everything you ever wanted to know about UDP sockets but were afraid to ask"](https://blog.cloudflare.com/everything-you-ever-wanted-to-know-about-udp-sockets-but-were-afraid-to-ask-part-1/) — UDP sockets, buffers, and where datagrams quietly die.
- 📰 [Glenn Fiedler — "Reliability and Flow Control" (Gaffer On Games)](https://gafferongames.com/post/reliability_ordered_messages/) — the canonical "build just-enough reliability on UDP" series; the ACK-window idea, game-engine flavored.
- 📰 [Jane Street — "How market data feeds work"](https://signalsandthreads.com/) (Signals & Threads) — why exchanges multicast over UDP and run an A/B feed with a recovery channel.
- 📖 ["TCP/IP Illustrated, Vol. 1" (Stevens), ch. on UDP & TCP flow control](https://www.pearson.com/) — the deep reference on stream vs. datagram and sliding-window flow control (TCP's, which our ACK-window mirrors).

---

## ▶️ Next

[`05-the-network-pipeline-end-to-end.md`](./05-the-network-pipeline-end-to-end.md) — assemble it: wire → `epoll` ingest → SPSC ring → pinned strategy → order stream, with every prior week showing up in one `gateway.cpp`. ⚡
