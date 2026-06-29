# 01 — Sockets and the Kernel Boundary: The Wire Is the New Bottleneck

> **TL;DR** — For four weeks the ticks were already in your address space — an array, a file you `mmap`'d. This week they arrive on a **socket**, which means every tick crosses the **kernel boundary**: a `recv()` is a system call, and a system call is a mode switch, a copy out of a kernel buffer, and a scheduler decision. The wire and the kernel are now on the hot path. The whole of Week 5 is keeping that cost off the strategy by absorbing it on a dedicated ingest thread — the exact decoupling you built in Week 4.

Week 4 handed you a `const WireTick*` — the feed was already in memory and you `mmap`'d it for free. That was a convenient lie. Real market data does not arrive as a file; it arrives as **packets on a network interface**, and the only way to get at them is to ask the kernel, one syscall at a time. This file is the recap of how a socket works and why the syscall boundary is suddenly the most expensive thing in your pipeline.

---

## 1. A socket is a file descriptor that talks to the network

The Berkeley sockets API is the same five calls it has been since 1983. You create an endpoint, give it an address, and then read and write bytes:

```cpp
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

int fd = socket(AF_INET, SOCK_DGRAM, 0);   // AF_INET = IPv4, SOCK_DGRAM = UDP

sockaddr_in addr{};
addr.sin_family = AF_INET;
addr.sin_port   = htons(9000);                 // network byte order
inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));   // claim the address
// ... then recvfrom()/recv() to read datagrams that arrive here.
```

`fd` is just an `int` — a **file descriptor**, the same kind of handle you'd get from `open()`. That is the Unix design: the network looks like a file. But unlike a file, the bytes behind it are produced by *another machine* (or another process), asynchronously, and the kernel is the middleman that buffers them until you ask.

> 📌 In this week's challenge the judge hands your gateway an **already-connected** `fd` — you don't `socket()`/`bind()`/`connect()` yourself. Your job starts at "you have a file descriptor; drain it as fast as you can." But you need to know what that descriptor *is* to drain it well.

---

## 2. `recv()` is a system call, and a system call is not free

Reading from an in-memory array is a load: ~1 ns from L1, ~100 ns from DRAM (Week 1). Reading from a socket is a **system call**:

```cpp
char buf[2048];
ssize_t k = recv(fd, buf, sizeof(buf), 0);   // returns #bytes read, or -1
```

That one line does a surprising amount:

1. **Mode switch** — the CPU traps from user mode into kernel mode (and back). On a modern box with mitigations on, the round trip is **tens to hundreds of nanoseconds** before any work happens.
2. **A copy** — the kernel copies the datagram from its internal socket buffer into your `buf`. That's bytes moved across the boundary, polluting cache.
3. **A scheduling decision** — if no data is ready, the kernel may **block** your thread and run something else (more on that in [`02`](./02-blocking-nonblocking-and-readiness.md)).

When a tick is ~10 ns of strategy work (your Week-2/4 numbers), a per-tick syscall at ~100+ ns is a **10× tax**. The entire art of low-latency networking is paying that tax **fewer times** (batch many ticks per syscall) and **off the critical path** (on a thread that isn't your strategy).

```text
Week 1-4 cost model:      tick = decode + strategy            (~tens of ns)
Week 5 cost model:        tick = (syscall + copy)/batch + decode + strategy
                                  └────── the new, dominant term ──────┘
```

---

## 3. Send is symmetric — and this week you send too

`send()`/`sendto()` push bytes the other way, with the same boundary cost:

```cpp
std::uint64_t ack = next_seq;
send(fd, &ack, sizeof(ack), 0);   // tell the sender "I've got everything up to here"
```

Most of this week is *receiving* the feed, but the challenge's flow control (the ACK-window of [`04`](./04-tcp-vs-udp-and-framing.md)) means your gateway also **sends** small acknowledgements back to the judge's feed sender. Those sends are syscalls too — so you'll want to **batch** them (one ACK per N datagrams, not one per datagram), the same amortization idea as the Week-4 batched publish.

---

## 4. The socket buffer: a queue you don't control

Between the wire and your `recv()` sits a kernel-owned **receive buffer** (sized by `SO_RCVBUF`). Datagrams land there as they arrive; `recv()` drains them in order. This buffer is a queue with the same dynamics as your Week-4 ring:

- If you `recv()` **faster** than data arrives, the buffer stays near-empty and `recv()` blocks (or returns `EAGAIN`).
- If you `recv()` **slower** than data arrives, the buffer **fills**. For UDP, a full receive buffer means the kernel **silently drops** new datagrams — they're gone, no error to the sender.

That last point is the whole reason this week's protocol has flow control: a gateway that can't keep up would drop ticks, change its order stream, and fail correctness. You will not let the buffer overflow — you'll keep draining, and the sender will throttle itself to your pace ([`04`](./04-tcp-vs-udp-and-framing.md) §4). For now, internalize the shape: **the kernel hands you a bounded queue you must drain, and falling behind loses data.**

```text
   wire ──► [ kernel socket recv buffer (SO_RCVBUF) ] ──recv()──► your ingest thread
              fills if you're slow; UDP drops on overflow
```

---

## 5. Why this maps straight onto Week 4

Look at the shape again and you'll see you already built the answer:

```text
            the new front of the pipe                 the pipe you built in Week 4
   ┌──────────────────────────────────────┐   ┌─────────────────────────────────────┐
   wire ─► kernel recv buffer ─► INGEST ───┼─► SPSC ring ─► STRATEGY ─► order stream
                                 (recv,                       (Week-1 z-score,
                                  decode)                      one consumer thread)
```

The **ingest** thread does the expensive, jittery thing — syscalls into the kernel — and hands clean `csot::Tick`s across your lock-free ring to the strategy thread, which never touches a socket. Network jitter, a slow `recv()`, a scheduler hiccup on the ingest core: none of it stalls the strategy, because the ring decouples them. **The decoupling that was a nice-to-have in Week 4 is the entire point in Week 5** — the wire is exactly the kind of jittery, blocking input the lock-free hand-off exists to isolate.

The rest of the week fills in the ingest box: how to read without blocking ([`02`](./02-blocking-nonblocking-and-readiness.md)), how to wait for many sockets efficiently with `epoll` ([`03`](./03-epoll-event-loop.md)), how the feed is framed on UDP and kept lossless ([`04`](./04-tcp-vs-udp-and-framing.md)), and how to wire the whole thing end-to-end ([`05`](./05-the-network-pipeline-end-to-end.md)).

---

## 🎯 Key Takeaways

- A socket is a **file descriptor** backed by the network; in this week's challenge the judge hands you an already-connected one.
- `recv()`/`send()` are **system calls**: a mode switch + a copy + a possible scheduling decision — **~100+ ns**, dwarfing a ~10 ns tick. The wire and the kernel are now the hot path.
- You beat the syscall tax two ways: **batch** (many ticks per syscall) and **decouple** (do the syscalls on an ingest thread, never on the strategy thread).
- The kernel keeps a bounded **receive buffer**; drain it or, for UDP, lose datagrams. Falling behind is a correctness bug, not just a slow one.
- The pipeline is Week 4 with the wire bolted onto the front: ingest absorbs the kernel's jitter; the SPSC ring keeps it off the strategy.

---

## 📚 Further Reading — Sockets & the Syscall Boundary

- 📖 [Beej's Guide to Network Programming](https://beej.us/guide/bgnet/html/) — the friendliest sockets recap there is; read §5–§7 (socket/bind/recv) if any of §1 felt rusty.
- 📰 [Brendan Gregg — "Why syscalls are expensive (and getting more so)"](https://www.brendangregg.com/blog/2018-01-08/benchmarking-nodejs-bare-metal-cloud.html) — measured mode-switch + mitigation costs; why one syscall per item is a tax.
- 📰 [Cloudflare — "How to receive a million packets per second"](https://blog.cloudflare.com/how-to-receive-a-million-packets/) — the canonical tour of where UDP receive time actually goes.
- 📖 ["The Linux Programming Interface" (Kerrisk), ch. 56–61](https://man7.org/tlpi/) — the definitive sockets reference if you want the full picture.

---

## ▶️ Next

[`02-blocking-nonblocking-and-readiness.md`](./02-blocking-nonblocking-and-readiness.md) — when there's no data, should your thread *sleep* or *spin*? Blocking vs. non-blocking I/O, `EAGAIN`, and the readiness model that `epoll` is built on. ⚡
