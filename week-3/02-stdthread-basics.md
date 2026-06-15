# 02 — std::thread Basics: Spawn, Join, and Partition Without Losing a Tick

> **TL;DR** — A `std::thread` runs a function on another core; you must `join()` it (or use `std::jthread`, which joins in its destructor) or the program calls `std::terminate`. Spawning costs ~10–50 µs, so spawn in the cold path, not per tick. The subtle bug is **partitioning**: splitting `[0, n)` into T chunks with integer division drops the remainder unless you hand the leftover to the last thread. Get the chunk math right and each thread reduces a disjoint slice into its own partial.

[`01-going-wide.md`](./01-going-wide.md) gave us the map-reduce shape. Now the concrete tools to spawn the workers and feed each one a disjoint slice of the stream.

---

## 1. The smallest thread

```cpp
#include <thread>
#include <cstdio>

void work(int id) { std::printf("hello from thread %d\n", id); }

int main() {
    std::thread t(work, 7);   // starts running work(7) on another core NOW
    t.join();                 // block until it finishes — MANDATORY
}
```

Two rules you cannot skip:

1. A `std::thread` must be **joined** (`t.join()`) or **detached** (`t.detach()`) before it is destroyed. Drop a still-joinable thread and the destructor calls `std::terminate()` — instant crash.
2. The new thread starts running *immediately*. Anything it reads must already be valid.

`join()` is how the parent waits for the child and is also a **synchronization point**: after `t.join()` returns, everything the child wrote is visible to the parent. That is exactly when you read a worker's partial table.

---

## 2. `std::jthread` — join you can't forget

C++20 added `std::jthread`: same thing, but its destructor **joins automatically**. It removes the #1 beginner crash (forgetting to join) and is what you should reach for.

```cpp
#include <thread>
#include <vector>

void reduce_chunk(const csot::AggTick* b, const csot::AggTick* e, csot::SymbolAgg* partial);

void launch(const csot::AggTick* ticks, std::size_t n, /* ... */) {
    std::vector<std::jthread> workers;
    workers.reserve(4);
    // ... spawn into workers ...
    // no explicit join needed: when `workers` is destroyed, each jthread joins.
}
```

> 💡 `std::jthread` also carries a `std::stop_token` for cooperative cancellation. You won't need cancellation for a batch reduction, but it's why `jthread` exists beyond auto-join.

---

## 3. Spawning is expensive — do it in the cold path

Creating a thread asks the kernel to allocate a stack (default ~8 MiB of address space), set up a kernel task, and schedule it. That's **~10–50 µs** per thread — an eternity on a hot path that processes ticks in nanoseconds.

| Operation | Rough cost |
|---|---|
| `std::thread` create + join | ~10–50 µs |
| function call | ~1 ns |
| one tick of work | sub-ns to a few ns |

The lesson is the Week-2 lesson again: **do expensive setup once, in the cold path.** For a single timed `run()` over the whole stream you spawn your T workers once at the start of `run()` (or reuse a pool built in `on_init` — see [`06-bonus-bandwidth-and-pools.md`](./06-bonus-bandwidth-and-pools.md)), let them sweep, then join once. You never spawn per chunk-of-ticks, and *never* per tick.

> ⚠️ How many threads? Query it, don't hardcode: `std::thread::hardware_concurrency()` returns the core count (the judge box reports 4). Spawning more workers than cores ("oversubscription") just adds context switches inside your timed region — see [`05-scheduler-and-pinning.md`](./05-scheduler-and-pinning.md).

---

## 4. Partitioning `[0, n)` without losing a tick

This is where correctness quietly breaks. You want T disjoint, contiguous chunks that cover *every* tick exactly once. Naive `chunk = n / T` drops up to `T-1` ticks (the remainder), which shows up as `Σ count != n` — a correctness failure ([`AGG_SPEC.md`](./project/AGG_SPEC.md) §8).

The clean idiom — give each thread `[lo, hi)` with the remainder spread across the first few (or all dumped on the last):

```cpp
const unsigned T = 4;                       // worker count
for (unsigned k = 0; k < T; ++k) {
    const std::size_t lo = n *  k      / T;  // multiply BEFORE divide
    const std::size_t hi = n * (k + 1) / T;  // adjacent chunks share an endpoint
    // thread k reduces ticks[lo .. hi)
}
```

`n*k/T … n*(k+1)/T` guarantees chunk `k`'s `hi` equals chunk `k+1`'s `lo`, the chunks are disjoint and contiguous, and they cover all of `[0, n)` even when `T` doesn't divide `n`. (Watch for overflow if `n` is enormous; `std::size_t` is 64-bit so `n*T` is fine for any real stream.)

Sanity-check it: `Σ (hi - lo)` over all chunks must equal `n`.

---

## 5. Passing data in and out, safely

A thread function gets its inputs by argument. Two gotchas:

**Reference arguments must be wrapped.** `std::thread`/`std::jthread` copy their arguments by default; to pass a reference you must use `std::ref` — otherwise you get a dangling copy or a compile error.

```cpp
std::vector<std::jthread> ws;
for (unsigned k = 0; k < T; ++k) {
    ws.emplace_back(reduce_chunk,
                    ticks + lo, ticks + hi,
                    partials[k].rows);     // pointer to THIS thread's private table
}
```

**Outputs go to per-thread storage, never a shared variable.** Each worker writes into `partials[k]` — its own table, ideally on its own cache line. The moment two threads write the same `SymbolAgg`, you have a data race ([`03-data-races.md`](./03-data-races.md)). The map-reduce shape avoids this by construction: disjoint input chunks, private output partials, and a merge that happens only *after* every `join()`.

---

## 6. Putting it together (skeleton)

```cpp
csot::SymbolAgg* out;                 // num_symbols rows, caller-owned
// partials[k]: a private, cache-line-aligned table per worker (see 04-false-sharing.md)

void run(const csot::AggTick* ticks, std::size_t n, csot::SymbolAgg* out) {
    const unsigned T = 4;
    {
        std::vector<std::jthread> ws;
        ws.reserve(T);
        for (unsigned k = 0; k < T; ++k) {
            const std::size_t lo = n *  k      / T;
            const std::size_t hi = n * (k + 1) / T;
            ws.emplace_back([=]{ reduce_chunk(ticks + lo, ticks + hi, partials[k].rows); });
        }
    }                                 // <-- all jthreads join here: partials are now complete
    merge_partials_into(out, partials, T);   // serial, cheap
}
```

The closing brace is the join point. After it, every partial is fully written and visible, so the merge is safe. That structure — spawn, sweep disjoint chunks, join, merge — is the entire parallel skeleton. Everything else this week is making it *fast* and *correct under stress*.

---

## 🎯 Key Takeaways

- A `std::thread` runs a function on another core and **must** be joined or detached before destruction, or the program terminates.
- Prefer **`std::jthread`** (C++20): it joins in its destructor, killing the most common crash.
- `join()` is a synchronization point — after it, the worker's writes are visible. That's when you read its partial.
- Spawning costs **~10–50 µs**; spawn in the cold path (once per `run()` or reuse a pool), never per tick.
- Query core count with `std::thread::hardware_concurrency()` (4 on the judge); don't oversubscribe.
- **Partition with `n*k/T … n*(k+1)/T`** so chunks are disjoint, contiguous, and cover all `n` even when T doesn't divide n. Verify `Σ(hi-lo) == n`.
- Pass references with `std::ref`; write outputs to **per-thread** storage, never a shared variable.

---

## 📚 Further Reading — Threads in C++

- 📖 [cppreference — `std::thread`](https://en.cppreference.com/w/cpp/thread/thread) and [`std::jthread`](https://en.cppreference.com/w/cpp/thread/jthread) — the exact semantics of construction, join, and the argument-copy rule.
- 📖 ["C++ Concurrency in Action" (Williams), ch. 2–3](https://www.manning.com/books/c-plus-plus-concurrency-in-action-second-edition) — managing threads and passing arguments, the canonical treatment.
- 🎬 [CppCon — Arthur O'Dwyer, "Back to Basics: Concurrency"](https://www.youtube.com/watch?v=F6Ipn7gCOsY) — threads, joins, and the std primitives from scratch.
- 📰 [cppreference — `std::thread::hardware_concurrency`](https://en.cppreference.com/w/cpp/thread/thread/hardware_concurrency) — what it returns and the caveats.

---

## ▶️ Next

[`03-data-races.md`](./03-data-races.md) — what happens the instant two threads touch the same variable: lost updates, undefined behaviour, and how ThreadSanitizer catches it before the judge does.
