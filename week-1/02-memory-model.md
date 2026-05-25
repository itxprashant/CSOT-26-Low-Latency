# 02 — The Memory Model: Containers, Stack vs Heap, and Custom Allocators

> **TL;DR** — Two C++ programs with identical big-O can differ by 100x in speed because of *where* their data lives. This note teaches you how to control that.

We covered "memory is a big array" in [`01-introduction.md`](./01-introduction.md). Now we get specific: **how does C++ actually lay your data out, and how can you change it?**

---

## 1. Contiguous vs. Scattered Containers

### `std::vector<T>` — Contiguous Heap Block

A `std::vector<int>` of size 1,000 stores its 1,000 ints in **one contiguous heap allocation of 4,000 bytes**.

```
&v[0]    &v[1]    &v[2]    &v[3]    ...    &v[999]
  ↓        ↓        ↓        ↓                ↓
[ int  | int  | int  | int  | ... | int  ]    ← one big heap block
 4B     4B     4B     4B            4B
```

Iterating it is **cache-friendly**:
- The first read of `v[0]` triggers a cache miss → CPU loads 64 bytes (one cache line) into L1.
- Those 64 bytes contain `v[0]` through `v[15]` (since `sizeof(int) == 4`).
- The next 15 reads are **L1 hits** — ~1 ns each.
- Meanwhile, the CPU's **hardware prefetcher** notices the sequential pattern and starts fetching the next cache line *before* you ask for it.

Net effect: you process roughly **one int per CPU cycle** even though DRAM is hundreds of cycles away.

### `std::array<T, N>` — Contiguous Stack Block (usually)

Identical layout to `std::vector` but with two key differences:

1. The size `N` is **fixed at compile time**.
2. The storage is **inline** in the object, not heap-allocated.

```cpp
std::array<int, 1000> arr;   // 4000 bytes lives wherever `arr` lives
                             // (stack if `arr` is a local; data segment if global)
```

Use `std::array` when:
- Size is known at compile time
- You want to avoid any heap allocation
- You're nesting it inside another struct (no extra indirection)

### `std::list<T>` — Linked Scattered Nodes

A `std::list<int>` allocates **one heap node per element**. Each node holds the int *plus* two pointers (prev / next), so ~24 bytes on a 64-bit system.

```
[node A]──→[node B]──→[node C]──→ ...
   ↑  ↓        ↑  ↓        ↑  ↓
   addr 0x1000  addr 0x40A0  addr 0x7320      ← scattered all over the heap
```

Iterating it is **cache-hostile**:
- Each `node->next` is a pointer chase to an unpredictable address.
- The hardware prefetcher can't help — there's no pattern.
- Every node access risks a cache miss → ~100 ns per element.

**`std::list` should be your last resort, not your first.** It exists for cases where O(1) splice / insert-in-middle matters more than iteration speed. Those cases are rare.

### `std::unordered_map<K,V>` — Scattered with Indirection

A hash table with chaining. Each bucket is a pointer to a linked list of nodes. Each node is separately allocated.

- Best case (load factor low, key in first node): one cache miss for the bucket array + one for the node ≈ 200 ns.
- Worst case (long chain): a cache miss *per node* in the chain.

For low-latency code, prefer **open-addressing hash tables** (like `absl::flat_hash_map` or `boost::unordered_flat_map`) or **sorted vectors with binary search** for small sizes.

### Visual Summary

| Container | Layout | Iteration | Insert at end | Random access |
|---|---|---|---|---|
| `std::vector<T>` | one contiguous block | ⚡ fast | ⚡ amortized O(1) | ⚡ O(1) |
| `std::array<T,N>` | one contiguous inline block | ⚡ fast | n/a (fixed) | ⚡ O(1) |
| `std::deque<T>` | chunks of arrays | 🟡 medium | ⚡ O(1) | 🟡 O(1) with extra deref |
| `std::list<T>` | scattered nodes | 🐌 slow | ⚡ O(1) | ❌ O(N) |
| `std::map<T>` | scattered tree nodes | 🐌 slow | 🟡 O(log N) | 🟡 O(log N) |
| `std::unordered_map<T>` | scattered chains | 🐌 medium | 🟡 O(1) avg | 🟡 O(1) avg |

**Rule of thumb for low-latency:** start with `std::vector` or `std::array`. Only deviate if you have measurements proving you need something else.

---

## 2. Stack vs. Heap — The Most Important Free Lunch

Every C/C++ programmer learns this eventually, but few internalize it.

### The Stack

The **call stack** is a contiguous region of memory (typically 1-8 MB) used for function locals, parameters, and return addresses. Each function call **pushes** a frame; each return **pops** it.

```cpp
void foo() {
    int x = 42;          // x lives on the stack
    std::array<int, 100> arr;   // 400 bytes on the stack
    bar();               // bar's frame is pushed on top
}                        // foo's frame is popped — x and arr "deallocated"
```

**Key properties:**
- **Allocation cost:** essentially **zero**. It's literally `sub rsp, 408` — one instruction.
- **Deallocation cost:** zero (`add rsp, 408`).
- **Cache-friendly:** the top of the stack is *always* hot in L1 because every function call touches it.
- **Lifetime:** strictly tied to scope. No flexibility.
- **Size limit:** ~1-8 MB per thread by default. Stack overflow if you exceed.

### The Heap

The **heap** is a large region managed by `malloc` / `free` (and `new` / `delete`, which wrap them). You explicitly request and release memory.

```cpp
int* p = new int(42);    // p points to a heap allocation
delete p;                // you must remember to free it
```

**Key properties:**
- **Allocation cost:** `malloc` is a real function call that may take hundreds of nanoseconds (search free list, possibly call `mmap`, possibly lock a mutex in multi-threaded code).
- **Deallocation cost:** similar.
- **Cache behavior:** depends on where the allocator put it. Often scattered.
- **Lifetime:** as long as you want. Flexible but error-prone.
- **Size:** essentially as much RAM as the OS gives you.

### Why This Matters for Latency

Every `new`, every `std::make_unique`, every `std::vector::push_back` that triggers reallocation is a **call to the heap allocator**, which means:

1. A potential **lock acquisition** (in multi-threaded programs).
2. A traversal of the allocator's internal free list (variable time — bad for tail latency!).
3. A possible **page fault** if the OS hasn't backed that memory yet (microseconds, sometimes more).
4. A future **cache miss** because the returned block is probably cold.

For a system processing millions of market events per second, **every heap allocation is a stab wound**. The hot path of a low-latency trading system performs **zero heap allocations** after startup.

### The Rule

> **Allocate once at startup. Reuse forever. Never `new` on the hot path.**

This is the #1 rule of low-latency C++.

---

## 3. Bypassing the OS Allocator

OK so `malloc` is slow and unpredictable. What do we do?

### Strategy 1: Pre-Allocate Everything

The simplest fix. If you know the maximum number of orders you'll ever have in flight is 100,000, allocate a `std::vector<Order>(100'000)` at startup. Use indices into it instead of pointers. Mark slots as free/used with a flag.

```cpp
struct OrderPool {
    std::vector<Order> slots;         // pre-allocated at startup
    std::vector<bool>  used;
    std::vector<size_t> free_list;    // indices of free slots

    OrderPool(size_t n) : slots(n), used(n, false) {
        free_list.reserve(n);
        for (size_t i = n; i-- > 0;) free_list.push_back(i);
    }

    size_t acquire() {                // O(1), no allocation
        size_t idx = free_list.back();
        free_list.pop_back();
        used[idx] = true;
        return idx;
    }

    void release(size_t idx) {        // O(1), no deallocation
        used[idx] = false;
        free_list.push_back(idx);
    }
};
```

This is called an **object pool**. We'll build a templated one in Week 2.

### Strategy 2: Arena (Bump) Allocator

If you have a "phase" of work (e.g. processing one tick's worth of data) where you'll allocate many small things and then throw them all away at once, use an **arena**:

```cpp
class Arena {
    std::vector<std::byte> buf;
    size_t                 offset = 0;
public:
    explicit Arena(size_t cap) : buf(cap) {}

    template <typename T, typename... Args>
    T* alloc(Args&&... args) {
        // round up to alignof(T)
        size_t aligned = (offset + alignof(T) - 1) & ~(alignof(T) - 1);
        if (aligned + sizeof(T) > buf.size()) throw std::bad_alloc{};
        T* p = new (buf.data() + aligned) T(std::forward<Args>(args)...);
        offset = aligned + sizeof(T);
        return p;
    }

    void reset() { offset = 0; }   // "free" everything at once, O(1)
};
```

Allocation is **a pointer bump** (3-4 instructions). Deallocation is **setting one integer to zero**. There's no per-object free.

Used in:
- Game engines (per-frame arena)
- Compilers (per-translation-unit arena)
- Web servers (per-request arena)
- Trading systems (per-tick arena)

### Strategy 3: Slab Allocator

Like an object pool, but type-erased and chunked. Maintains free lists per object size. Used inside the Linux kernel and most production memory allocators (jemalloc, tcmalloc).

You probably won't write one this week — but you'll *use* jemalloc / tcmalloc as a drop-in replacement for the default `malloc`, which can be a 10-30% speedup essentially for free.

```bash
# Install
sudo apt install libjemalloc-dev

# Use at link time
g++ -O2 myprog.cpp -ljemalloc -o myprog
```

Or at runtime via `LD_PRELOAD`:

```bash
LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libjemalloc.so ./myprog
```

### Strategy 4: Custom STL Allocators

`std::vector`, `std::map`, etc. all take an `Allocator` template parameter. You can plug your own. This is advanced and we won't use it in Week 1, but be aware.

---

## 4. Putting It Together — A Cheat Sheet

| Goal | Use |
|---|---|
| A collection that grows dynamically | `std::vector<T>` (with `reserve()`!) |
| A fixed-size collection on the stack | `std::array<T, N>` |
| A small set of items (≤ 32-ish) | sorted `std::vector` + binary search |
| Many small short-lived allocations in a phase | **arena allocator** |
| Recycling fixed-type objects | **object pool** |
| Reduce `malloc` overhead with zero code change | link against **jemalloc / tcmalloc** |
| Eliminate `malloc` from hot path | **pre-allocate at startup** |

---

## 5. A Concrete Example: Order Book

Naive (heap-heavy) design:

```cpp
class OrderBook {
    std::map<double, std::list<Order*>> buys;     // tree of lists of heap-Orders
    std::map<double, std::list<Order*>> sells;
};
```

Every new order = 1 `new Order` + 1 `std::list::push_back` (which `new`s a node). Every cancel = a `delete`. The whole structure is scattered across the heap. **Brutal.**

Latency-friendly design:

```cpp
class OrderBook {
    // Pre-allocated pool of orders, addressed by 32-bit index
    std::vector<Order>                 orders;     // size = MAX_ORDERS
    std::vector<uint32_t>              free_slots;

    // Price-indexed levels (assumes integer ticks, e.g. price in 1/100 cents)
    static constexpr size_t MAX_LEVELS = 1 << 16;
    std::array<PriceLevel, MAX_LEVELS> buy_levels;
    std::array<PriceLevel, MAX_LEVELS> sell_levels;
};

struct PriceLevel {
    uint32_t head_order_idx = INVALID;   // intrusive linked list via indices
    uint32_t tail_order_idx = INVALID;
    int64_t  total_qty      = 0;
};
```

Zero heap allocations after construction. All data lives in 3 contiguous blocks. Cache lines are dense with useful data. This is what real exchanges look like.

We won't build the full thing this week, but you'll be ready by Week 4.

---

## 🎯 Key Takeaways

- **`std::vector` and `std::array` are your default containers.** Linked structures are slow because they're scattered.
- **Stack allocation is free; heap allocation costs hundreds of ns and is non-deterministic.** Avoid the heap on the hot path.
- **Pre-allocate at startup, reuse forever.** That's the #1 rule.
- **Arenas and object pools** are the two patterns that replace `malloc` in 90% of low-latency code.
- A simple drop-in win: **link against jemalloc or tcmalloc**.

---

## 📚 Further Reading — Allocators & Memory Layout

- 📰 [Howard Hinnant — "Memory allocators are silly"](https://howardhinnant.github.io/allocator_boilerplate.html) — by the author of `std::chrono` and `<memory>`.
- 🎬 [Andrei Alexandrescu — "std::allocator is to allocation what std::vector is to vexation"](https://www.youtube.com/watch?v=LIb3L4vKZ7U) (CppCon 2015) — entertaining and informative.
- 📰 [Jonathan Müller — "Adventures in Allocators"](https://www.foonathan.net/2016/02/memory-stack-allocator/) — practical, code-heavy series.
- 📰 [Ginger Bill — "Memory Allocation Strategies"](https://www.gingerbill.org/article/2019/02/08/memory-allocation-strategies-002/) — game-engine flavor; the canonical arena-allocator tutorial.
- 📰 [Aleksey Shipilëv — "Java Memory Layout"](https://shipilev.net/jvm/objects-inside-out/) — Java-focused but the layout principles apply to any language.

---

## ▶️ Next

[`03-memory-hierarchy.md`](./03-memory-hierarchy.md) — now that you control *where* your data lives, let's see *what the hardware does* with that data. L1, L2, L3, DRAM — and the brutal latency cliff between them.
