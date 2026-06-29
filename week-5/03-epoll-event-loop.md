# 03 — The `epoll` Event Loop

> **TL;DR** — `epoll` is Linux's readiness API: you register descriptors once, then call `epoll_wait()` to be handed back exactly the ones that are ready, sleeping only until something is. It's O(1) in the number of *idle* sockets (unlike `select`/`poll`, which are O(N) every call), which is why every high-throughput Linux server is built on it. Two modes — **level-triggered** (re-notifies while data remains) and **edge-triggered** (notifies once per transition; you must **drain to `EAGAIN`**). For this week's single feed either works; edge-triggered + a drain loop is the low-jitter idiom worth learning.

[`02`](./02-blocking-nonblocking-and-readiness.md) gave us the readiness model in the abstract. `epoll` is the Linux implementation, and it's the backbone of nginx, Redis, and every C++ network engine you'll meet. This file is the working knowledge to build one ingest loop with it.

---

## 1. Three calls: create, control, wait

`epoll` is an in-kernel set of "descriptors I care about." Three syscalls manage it:

```cpp
#include <sys/epoll.h>

int ep = epoll_create1(0);                       // 1. make an epoll instance

epoll_event ev{};
ev.events  = EPOLLIN;                            // "tell me when fd is readable"
ev.data.fd = fd;
epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev);           // 2. register fd (do this ONCE)

epoll_event events[64];
for (;;) {
    int nready = epoll_wait(ep, events, 64, -1); // 3. block until >=1 fd is ready
    for (int i = 0; i < nready; ++i) {
        if (events[i].events & EPOLLIN) {
            // events[i].data.fd is readable — recv() it now (non-blocking).
        }
    }
}
```

- `epoll_create1(0)` returns an fd representing the set.
- `epoll_ctl(... ADD/MOD/DEL ...)` registers/updates/removes a descriptor — a **one-time** cost per socket, *not* per wait.
- `epoll_wait()` blocks until at least one registered fd is ready (or the timeout fires), and fills `events[]` with **only the ready ones**.

That "register once, wait many times, get back only the ready ones" is the efficiency: with 10,000 mostly-idle sockets, `epoll_wait` does work proportional to the *ready* few, while `select`/`poll` re-scan all 10,000 on every call.

> 📌 Allocate the `events[]` buffer **once** (in `on_init`, the Week-2 rule) and reuse it every `epoll_wait`. The hot loop must not allocate.

---

## 2. The events buffer is a batch — and batching is the win

`epoll_wait` hands you up to `maxevents` ready descriptors in **one syscall**. For many sockets that amortizes the syscall over many ready fds. For our **single** feed socket it's less about fan-out and more about pairing readiness with a tight drain loop — but the principle is the Week-4/Week-5 refrain: **one syscall, many items.** When you also batch your `recv()`s (drain everything the kernel has) and batch your ACK `send()`s, the per-tick syscall cost from [`01`](./01-sockets-and-the-kernel-boundary.md) §2 amortizes toward zero.

---

## 3. Level-triggered vs. edge-triggered (and the drain rule)

This is the one `epoll` subtlety that bites everyone. It controls *when* `epoll_wait` re-notifies you about a socket that has data.

- **Level-triggered (LT, the default):** `epoll_wait` reports the fd readable **every time you call it while any data remains**. Forgiving — read one datagram, come back to the loop, and you'll be told again that there's more. Slightly more `epoll_wait` calls.
- **Edge-triggered (ET, `EPOLLET`):** `epoll_wait` reports the fd readable **only when it transitions** from "no data" to "data." You get **one** notification per arrival edge — so you **must keep reading until `recv()` returns `EAGAIN`**, or the bytes you didn't read sit there with no further wake-up, and your gateway hangs.

```cpp
ev.events = EPOLLIN | EPOLLET;          // edge-triggered
epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev);

// ... when epoll_wait says fd is readable, DRAIN it completely:
for (;;) {
    ssize_t k = recv(fd, buf, buf_cap, 0);
    if (k > 0)        { handle_datagram(buf, k); continue; }   // got one; keep going
    if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;  // socket empty — done
    // k == 0 or other errno: handle/close
}
```

> ⚠️ **The #1 edge-triggered bug:** read exactly one datagram per `EPOLLIN`, then wait again. With ET you won't be re-notified for the data already sitting in the buffer, so your gateway stalls partway through the feed and never reaches `n` ticks. The fix is the **drain-until-`EAGAIN`** loop above — it's not optional with ET.

For this week's single saturated feed, **level-triggered is simpler and perfectly fine** (you'll loop fast enough that the extra notifications are noise). Edge-triggered shaves a few `epoll_wait` calls and is the idiom real engines use — learn it, but don't let an ET drain bug cost you correctness.

---

## 4. One ingest loop, start to finish

Putting [`01`](./01-sockets-and-the-kernel-boundary.md)–[`03`] together, here is the **shape** of the gateway's ingest thread (allocate in `on_init`, this runs in `run()`):

```cpp
// fd is the connected, non-blocking UDP socket the judge handed run().
// ring is your Week-4 SPSC ring of decoded csot::Tick; n is the total tick count.
int ep = epoll_create1(0);
epoll_event ev{}; ev.events = EPOLLIN | EPOLLET; ev.data.fd = fd;
epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev);

std::uint64_t next_seq = 0;          // datagrams contiguously received
std::size_t   ingested = 0;          // ticks pushed to the ring

while (ingested < n) {
    epoll_wait(ep, events_, kMaxEvents, -1);     // block until readable
    for (;;) {                                    // drain to EAGAIN (ET)
        ssize_t k = recv(fd, dgram_, dgram_cap_, 0);
        if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        if (k <= 0) continue;
        const auto* h = reinterpret_cast<const FeedDatagramHeader*>(dgram_);
        const auto* ticks = reinterpret_cast<const WireTick*>(dgram_ + sizeof(*h));
        for (std::uint32_t j = 0; j < h->count; ++j) {
            while (!ring.try_push(ticks[j])) { /* back-pressure: spin (Week 4) */ }
            ++ingested;
        }
        next_seq = h->seq + 1;
        send_ack(fd, next_seq);                   // amortize: ACK per datagram, or per K
    }
}
```

Notice what this loop **doesn't** do: it never runs the strategy, never allocates, never copies a tick twice. It moves bytes from the kernel into the ring and acknowledges them. The strategy thread on the other side of the ring does the thinking — exactly the Week-4 decoupling, with `recv` where the decode used to be.

---

## 5. `epoll` is a tool, not a religion

Be honest about when `epoll` earns its keep. Its superpower is **many descriptors**: thousands of connections, mostly idle, one thread. For a **single, saturated** feed socket — which is this week's challenge — a pinned **busy-poll** loop (`recv` until `EAGAIN`, `pause`, repeat) can be *lower latency* than `epoll`, because it never pays the `epoll_wait` wake-up. The reference and a top submission may well busy-poll.

So why teach `epoll`? Because it's the structure that *scales* and the one you'll see in every real engine, and because understanding readiness is what makes the busy-poll version a deliberate choice rather than an accident. Build the `epoll` loop to learn it; benchmark it against a busy-poll loop to *feel* the trade-off ([`05`](./05-the-network-pipeline-end-to-end.md) §5). The leaderboard rewards whichever is faster on the judge box — but only if it's still correct and deterministic.

---

## 🎯 Key Takeaways

- `epoll` = **register once** (`epoll_ctl`), **wait many** (`epoll_wait`), get back **only ready** fds. O(1) in idle sockets; `select`/`poll` are O(N) per call.
- `epoll_wait` fills a **reusable, pre-allocated** events buffer — one syscall, many ready fds. Batch readiness, batch `recv`, batch ACKs.
- **Level-triggered** re-notifies while data remains (forgiving). **Edge-triggered** notifies once per arrival — you **must drain to `EAGAIN`** or hang. For one feed, LT is fine; ET is the idiom.
- The ingest loop's job is **kernel → ring**: `recv`, decode, `try_push` (spin on back-pressure), ACK. No strategy, no allocation on the hot path.
- For a single saturated feed, a pinned **busy-poll** loop can beat `epoll`. Learn `epoll` for the structure and scaling; choose busy-poll if it measures faster — and stays correct.

---

## 📚 Further Reading — `epoll` & Event Loops

- 📰 [`man 7 epoll`](https://man7.org/linux/man-pages/man7/epoll.7.html) — the authority; the "Level-triggered and edge-triggered" and "Possible pitfalls" sections are required reading.
- 📰 [Cindy Sridharan — "The method to epoll's madness"](https://copyconstruct.medium.com/the-method-to-epolls-madness-d9d2d6378642) — a deep, readable walk through LT vs. ET and the drain rule.
- 📰 [Marek Majkowski (Cloudflare) — "I bet you didn't know all of this about epoll"](https://idea.popcount.org/2017-02-20-epoll-is-fundamentally-broken-12/) — the sharp edges of `epoll` once you push it hard.
- 📖 ["The Linux Programming Interface" (Kerrisk), ch. 63](https://man7.org/tlpi/) — `epoll` alongside `select`/`poll`, with the performance comparison spelled out.

---

## ▶️ Next

[`04-tcp-vs-udp-and-framing.md`](./04-tcp-vs-udp-and-framing.md) — why market data rides UDP, how a tick feed is framed into datagrams, and the sequence-number + ACK-window trick that keeps our loopback feed **lossless and deterministic** for grading. ⚡
