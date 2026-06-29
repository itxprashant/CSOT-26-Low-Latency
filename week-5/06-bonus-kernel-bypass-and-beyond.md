# 06 — ⭐ Bonus: Kernel Bypass and Beyond

> **TL;DR** — Once your ingest thread is a tight `epoll`/busy-poll loop and the strategy never syscalls, the **kernel itself** becomes the floor: every datagram is still copied out of a kernel buffer across the mode boundary. The next order of magnitude comes from doing *fewer, bigger* syscalls (`recvmmsg`), letting the kernel poll the NIC for you (`SO_BUSY_POLL`), batching syscalls without entering the kernel each time (`io_uring`), or removing the kernel from the data path entirely (`AF_XDP`, DPDK, Solarflare/onload). None of it is in scope for the challenge — but it's where this track points, and where real HFT lives.

You don't need any of this to win Week 5; a clean pinned pipeline ([`05`](./05-the-network-pipeline-end-to-end.md)) is the assignment. This file is the horizon — the techniques that take you from "fast on a normal socket" to "the kernel is no longer in the way."

---

## 1. `recvmmsg` — many datagrams, one syscall

The cheapest win, and almost in-scope. `recvmmsg()` receives **multiple datagrams in a single system call**, amortizing the mode-switch from [`01`](./01-sockets-and-the-kernel-boundary.md) §2 across a whole batch:

```cpp
#include <sys/socket.h>

mmsghdr msgs[64];                       // up to 64 datagrams at once
// ... point each msgs[i].msg_hdr at its own iovec/buffer ...
int got = recvmmsg(fd, msgs, 64, MSG_DONTWAIT, nullptr);
// 'got' datagrams received in ONE syscall; msgs[i].msg_len is each length.
```

Where a `recv`-per-datagram loop pays the syscall tax once per datagram, `recvmmsg` pays it once per *batch*. On a saturated feed that's the difference between syscall-bound and copy-bound. The mirror image, `sendmmsg`, batches your ACKs the same way. This is the natural "stretch" past the basic loop — same readiness model, far fewer boundary crossings.

---

## 2. `SO_BUSY_POLL` — let the kernel spin on the NIC

Even with `epoll`, there's latency between a packet hitting the NIC and your thread being woken. `SO_BUSY_POLL` (and the global `net.core.busy_poll`) tells the kernel to **busy-poll the device queue** for a few microseconds inside the socket call, skipping the interrupt-and-wakeup path:

```cpp
int usec = 50;
setsockopt(fd, SOL_SOCKET, SO_BUSY_POLL, &usec, sizeof(usec));
```

You trade CPU (the kernel spins) for lower, tighter receive latency. It's the kernel-side cousin of your user-space busy-poll loop, and it stacks with it.

---

## 3. `io_uring` — batch syscalls without the per-call boundary

`io_uring` is Linux's modern asynchronous I/O interface: you place operations on a **submission ring** in shared memory and harvest results from a **completion ring**, so many reads/writes are issued and reaped **without a syscall per operation** (often without entering the kernel at all, in polled mode). It generalizes the "register once, batch many" idea of `epoll` from *readiness* to *the operations themselves*:

```text
epoll:     kernel tells you a socket is READY; you still syscall to recv it.
io_uring:  you queue the recv itself; the kernel completes it and posts the result.
```

For a single UDP feed `io_uring` is overkill, but it's the direction the whole Linux I/O stack is moving, and worth knowing exists when one socket becomes thousands.

---

## 4. Kernel bypass — remove the kernel from the data path

The end of the road. When even one copy across the kernel boundary is too much, you take the kernel **out of the data path** and map the NIC's packet rings straight into user space:

| Technology | What it does | Trade-off |
|---|---|---|
| **`AF_XDP`** | a Linux socket type that delivers raw frames into a user-space UMEM ring, bypassing the network stack | in-tree, needs a driver with XDP support; you parse Ethernet/IP/UDP yourself |
| **DPDK** | poll-mode userspace drivers; the kernel never sees the packets | dedicates cores/NICs; you reimplement the stack you need |
| **Solarflare / onload** | a `LD_PRELOAD` that transparently accelerates the standard sockets API onto a kernel-bypass stack | vendor NIC; near-zero code change — the HFT default |

All of them chase the same number: **wire-to-application in single-digit microseconds or less**, with no per-packet syscall and no kernel copy. This is the substrate real trading systems run on. It's far outside a 5-week curriculum — but everything you built (zero-alloc, pinning, lock-free hand-off, drain-to-`EAGAIN`) is exactly the discipline these stacks demand. Kernel bypass removes the kernel; it does *not* remove the need to be mechanically sympathetic.

---

## 5. Where this leaves you

Step back and look at what one `gateway.cpp` now contains: a wire feed decoded by an ingest thread, handed lock-free to a pinned, zero-allocating strategy, throttled by end-to-end back-pressure, emitting a deterministic order stream — measured, not guessed, at every layer. That is a real low-latency trading data path in miniature. The bypass techniques above are the next rung, but they're refinements of the same three ideas this track has hammered since Week 1:

- **Measure, don't guess.** Every step here is justified by a number (`perf`, throughput, p99), never a hunch.
- **Do the boring synchronous version first.** The stub works before the pipeline; the pipeline works before you reach for `recvmmsg`.
- **Data layout > algorithms.** `WireTick` is 40 bytes for a reason; the ring is `alignas(64)` for a reason; the bypass stacks exist to control exactly *where the bytes are* and *who copies them*.

You've taken a tick from a CSV in Week 1 to a packet on the wire in Week 5, and made it fast and correct the whole way. That's the platform. The rest is depth.

---

## 🎯 Key Takeaways

- After a clean pinned pipeline, the **kernel boundary** is the floor; the next gains are fewer/bigger syscalls or no kernel at all.
- **`recvmmsg`/`sendmmsg`** receive/send many datagrams per syscall — the natural stretch past a `recv`-per-datagram loop, same readiness model.
- **`SO_BUSY_POLL`** has the kernel spin on the NIC to cut wake-up latency; **`io_uring`** batches the *operations* themselves via shared-memory rings.
- **Kernel bypass** (`AF_XDP`, DPDK, Solarflare/onload) maps the NIC into user space for single-digit-µs wire-to-app — the real HFT substrate, out of scope here.
- The bypass world demands the *exact* discipline this track taught: zero-alloc, pinning, lock-free hand-off, drain-to-`EAGAIN`. You're standing at the door.

---

## 📚 Further Reading — Kernel Bypass & the Frontier

- 📰 [`man 2 recvmmsg`](https://man7.org/linux/man-pages/man2/recvmmsg.2.html) — the batched receive; the example program is a near-drop-in for an ingest loop.
- 📰 [Cloudflare — "How to achieve low latency with 10Gbps Ethernet" / busy-polling](https://blog.cloudflare.com/how-to-achieve-low-latency/) — `SO_BUSY_POLL` and friends, measured.
- 📰 [kernel.org — `AF_XDP` documentation](https://www.kernel.org/doc/html/latest/networking/af_xdp.html) — the in-tree kernel-bypass socket, from the source.
- 🎬 [Lord of the io_uring](https://unixism.net/loti/) — a thorough, friendly guide to `io_uring`'s submission/completion model.
- 🎬 [CppCon — David Gross / various HFT talks on kernel bypass](https://www.youtube.com/results?search_query=cppcon+hft+kernel+bypass) — practitioners on onload/DPDK in production trading systems.

---

## ▶️ Next

That's the track. Head back to the [Week 5 README](./README.md) for the closing checklist and the final leaderboard, then ship your `gateway.cpp`. You built a quant platform from a CSV reader to a networked, lock-free, deterministic trading data path — in five weeks. ⚡
