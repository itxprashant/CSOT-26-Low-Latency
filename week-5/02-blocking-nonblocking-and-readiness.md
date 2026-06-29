# 02 — Blocking, Non-Blocking, and the Readiness Model

> **TL;DR** — A plain `recv()` **blocks**: if no datagram is ready, the kernel parks your thread and runs something else, and you only wake up (microseconds later) when data arrives. That's fine for one quiet socket, fatal for a saturated feed where every microsecond of sleep is a microsecond of ticks piling up behind you. Mark the socket **non-blocking** (`O_NONBLOCK`) and `recv()` returns immediately with `EAGAIN` when empty — now *you* decide whether to spin, poll, or wait. That "is it ready?" question is the **readiness model**, and `epoll` ([`03`](./03-epoll-event-loop.md)) is how you answer it for many sockets at once.

In [`01`](./01-sockets-and-the-kernel-boundary.md) we saw `recv()` is a syscall. This file is about its most important hidden behavior: what happens when there's **nothing to read yet**.

---

## 1. Blocking I/O: the kernel puts you to sleep

By default a socket is **blocking**. Call `recv()` on an empty socket and your thread doesn't return — the kernel takes it off the run queue, schedules another thread, and only makes you runnable again when a datagram lands:

```cpp
char buf[2048];
ssize_t k = recv(fd, buf, sizeof(buf), 0);   // BLOCKS here until data arrives
// ... we only get here once there's something to read
```

The good: zero CPU burned while waiting. The bad, for us:

- **Wake-up latency.** Going to sleep and being woken is a scheduler round-trip — **~1–5 µs** on a loaded box. When the feed is firehosing ticks, you never actually want to sleep; there's always more coming.
- **One descriptor at a time.** A blocking `recv()` commits the thread to *that* socket. If you had two feeds, a thread blocked on one is deaf to the other.
- **It hides readiness.** You can't ask "is there data?" — you can only say "give me data, and sleep me if there isn't." That removes your control over *how* to wait.

Blocking is the right default for a casual client. It is the wrong default for a thread whose entire job is to drain a busy socket as fast as the wire delivers.

---

## 2. Non-blocking I/O: `recv()` returns `EAGAIN` instead of sleeping

Flip the socket into **non-blocking** mode and `recv()` never sleeps. If there's data, you get it; if there isn't, it returns `-1` and sets `errno` to `EAGAIN` (a.k.a. `EWOULDBLOCK`):

```cpp
#include <fcntl.h>

int flags = fcntl(fd, F_GETFL, 0);
fcntl(fd, F_SETFL, flags | O_NONBLOCK);    // make recv() non-blocking

char buf[2048];
ssize_t k = recv(fd, buf, sizeof(buf), 0);
if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
    // nothing to read RIGHT NOW — the socket is empty. We decide what to do.
} else if (k > 0) {
    // got a datagram of k bytes — process it.
}
```

Now `recv()` answers a different question: not "give me data (sleeping if needed)" but "give me data **if it's here this instant**." The choice of what to do on `EAGAIN` is yours — and that choice is the latency/CPU trade-off of the whole week.

> ⚠️ For **UDP/`SOCK_DGRAM`**, one `recv()` returns **exactly one datagram** (or `EAGAIN`). If your buffer is smaller than the datagram, the rest is **discarded** — UDP has no "read half a message". Size your receive buffer to the spec's max datagram ([`04`](./04-tcp-vs-udp-and-framing.md) §2), and read in a loop to drain everything the kernel has queued.

---

## 3. Three ways to wait

Once the socket is non-blocking and `recv()` can say "empty," your ingest loop has to decide how to spend the time until the next datagram. Same three options as the Week-4 back-pressure spin, now against the kernel instead of a ring:

```cpp
// (a) Busy-poll — call recv() in a tight loop. Lowest latency, burns 100% of a core.
while ((k = recv(fd, buf, sizeof(buf), 0)) < 0 && errno == EAGAIN) { /* spin */ }

// (b) Spin with a PAUSE hint — same, but be a good SMT citizen.
while ((k = recv(fd, buf, sizeof(buf), 0)) < 0 && errno == EAGAIN) {
    __builtin_ia32_pause();
}

// (c) Block until ready, then drain — give the core back; pay a wake-up when data comes.
//     This is what epoll_wait() gives you, for many sockets at once (see 03).
```

| Strategy | Latency to first byte | CPU while idle | When it's right |
|---|---|---|---|
| Busy-poll (a)/(b) | **lowest** (no wake-up) | a whole core, hot | a saturated feed on a pinned, dedicated core — *this week's default* |
| Block / `epoll_wait` (c) | a wake-up (~µs) | ~zero | bursty or many sockets; when you can't dedicate a core |

For a single firehose feed pinned to its own core, **busy-poll wins** — there's always another datagram coming, so sleeping only adds latency. For one-of-many sockets or a bursty feed, you want to **block until something is ready** without burning a core — and you want to do it for *all* your sockets in one call. That is exactly `epoll`.

---

## 4. The readiness model: "tell me when I can act without blocking"

Both `epoll` and its older cousins (`select`, `poll`) implement the **readiness model**: instead of *attempting* an operation and maybe blocking, you ask the kernel **"which of these descriptors can I read/write right now without blocking?"** and it answers (sleeping you only until *something* is ready):

```text
blocking model:    "recv() this socket"           → kernel: sleeps you on THIS socket
readiness model:   "which of {fd1,fd2,...} ready?" → kernel: sleeps you until ANY is ready,
                                                       then names them; you recv() those
```

The readiness model is what lets one thread service many sockets, and what lets you sleep efficiently (no wasted wake-ups on still-empty sockets) without committing to a single descriptor. It is **always paired with non-blocking sockets**: the kernel says "fd is readable," you `recv()` — and because the socket is non-blocking, even if the readiness was stale (rare races), you get `EAGAIN` instead of an accidental block.

> 💡 **Rule of thumb that survives the whole week:** readiness from `epoll` + non-blocking `recv()` in a **drain loop** (read until `EAGAIN`). Readiness tells you *there is data*; the drain loop reads *all* of it; `EAGAIN` tells you *you're done for now*. Get this pairing wrong and you either miss data or block by accident — [`03`](./03-epoll-event-loop.md) §3 shows exactly how.

---

## 5. Why we care for the gateway

This week's gateway reads a single UDP feed as fast as the judge's sender pushes it. Two valid designs, both built on a **non-blocking** socket:

1. **Busy-poll** the socket on a pinned ingest core, decode each datagram, push to the ring. Lowest latency, simplest loop, burns the core — fine, the box has spare cores and the feed is saturated.
2. **`epoll`-driven**: `epoll_wait()` for readiness, then drain the socket to `EAGAIN`. Gives the core back between bursts; the canonical structure that generalizes to many feeds.

Either is acceptable for the challenge — but you must understand readiness to build the `epoll` version, and `epoll` is the technique the curriculum is actually teaching (the busy-poll loop is a special case of "I never sleep"). The next file builds the event loop.

---

## 🎯 Key Takeaways

- A **blocking** `recv()` parks your thread until data arrives — ~µs wake-up latency and one socket at a time. Wrong for a saturated feed.
- **Non-blocking** (`O_NONBLOCK`) makes `recv()` return `EAGAIN`/`EWOULDBLOCK` instead of sleeping, handing *you* the decision of how to wait.
- One UDP `recv()` returns **exactly one datagram**; size the buffer to the max datagram and **loop to drain**.
- Three ways to wait: **busy-poll** (lowest latency, burns a core — this week's default on a pinned ingest core), or **block via `epoll`** (gives the core back, many sockets, a wake-up cost).
- The **readiness model** ("which fds can I act on now?") + non-blocking sockets + a **drain-to-`EAGAIN`** loop is the pattern the rest of the week is built on.

---

## 📚 Further Reading — Blocking, Non-Blocking, Readiness

- 📰 [Julia Evans — "Async IO on Linux: select, poll, and epoll"](https://jvns.ca/blog/2017/06/03/async-io-on-linux--select--poll--and-epoll/) — the clearest short tour of the readiness model and why blocking I/O doesn't scale.
- 📖 [Beej's Guide — "Blocking" and `fcntl`/`O_NONBLOCK`](https://beej.us/guide/bgnet/html/#blocking) — the minimal recipe to flip a socket non-blocking.
- 📰 [`man 2 recv`](https://man7.org/linux/man-pages/man2/recv.2.html) — read the `EAGAIN`, `MSG_DONTWAIT`, and `SOCK_DGRAM` semantics straight from the source.
- 🎬 [CppCon 2017 — Carl Cook, "When a Microsecond Is an Eternity"](https://www.youtube.com/watch?v=NH1Tta7purM) — why HFT ingest threads busy-poll instead of sleeping.

---

## ▶️ Next

[`03-epoll-event-loop.md`](./03-epoll-event-loop.md) — the Linux readiness API in anger: `epoll_create1`/`ctl`/`wait`, level- vs. edge-triggered, and the drain-until-`EAGAIN` loop that is the heart of every scalable server. ⚡
