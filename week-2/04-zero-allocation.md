# 04 — Zero Allocation: The Fastest `malloc` Is the One You Never Call

> **TL;DR** — `malloc`/`new` on the hot path costs 50–500 ns *and* is non-deterministic — it's the #1 source of p99 spikes. Size everything up front in `on_init`, hand out memory from fixed buffers / arenas / pools, and make your hot path's allocation count exactly **zero**. The ABI is built so a correct cache sim never needs the heap inside `run()`.

We've placed work in the cold path ([`01-hot-and-cold-paths.md`](./01-hot-and-cold-paths.md)) and laid data out for locality ([`03-locality.md`](./03-locality.md)). The last structural win is removing the heap from the hot path entirely.

---

## 1. Why `malloc` Is Poison on the Hot Path

A general-purpose allocator has to find a free block of the right size, possibly split or coalesce blocks, maybe take a lock, maybe call the kernel (`mmap`/`sbrk`) when the arena is exhausted. The *typical* cost is tens to a few hundred nanoseconds — but the *tail* is what kills you:

| Event | Rough cost |
|---|---|
| `malloc` fast path (cached free block) | ~15–50 ns |
| `malloc` slow path (coalescing, new arena) | ~200–500 ns |
| `malloc` that triggers `mmap` + page fault | ~1–10 µs |
| `free` | ~15–100 ns |

In a hot loop running 10⁸ times, even the fast path adds seconds; the occasional slow path is exactly the p99/p999 spike you'll spend Week 4 hunting. Determinism matters as much as the mean: a sim that allocates "only sometimes" has a jittery `run()` time, and the leaderboard times `run()`.

> 💡 The rule for the rest of this track: **no allocation, no syscalls, no locks on the hot path.** Cache sim, strategy, network loop — all the same rule.

---

## 2. Allocate Once, in `on_init`

The cache sim needs a known, fixed amount of state: 64 L1 sets, 512 L2 sets, 8 ways each, a tag + flags + LRU per way/set. Every size is a compile-time constant from `CACHE_SPEC.md`. So allocate it **once** and reuse it for the whole trace:

```cpp
class FastCacheSim final : public csot::CacheSim {
    static constexpr std::size_t L1_SETS = 64, L2_SETS = 512, WAYS = 8;
    std::vector<std::uint64_t> l1_tag_, l2_tag_;     // sized in on_init
    std::vector<std::uint8_t>  l1_vd_,  l2_vd_;      // valid|dirty packed
    std::vector<std::uint32_t> l1_lru_, l2_lru_;

public:
    void on_init() override {                         // COLD: the only allocation
        l1_tag_.assign(L1_SETS * WAYS, 0);
        l2_tag_.assign(L2_SETS * WAYS, 0);
        l1_vd_.assign(L1_SETS * WAYS, 0);
        l2_vd_.assign(L2_SETS * WAYS, 0);
        l1_lru_.assign(L1_SETS, 0x76543210u);
        l2_lru_.assign(L2_SETS, 0x76543210u);
    }

    csot::CacheStats run(const csot::MemAccess* acc, std::size_t n) override {
        csot::CacheStats s{};
        for (std::size_t i = 0; i < n; ++i) {
            // HOT: only reads/writes into the buffers above. Zero allocation.
        }
        return s;
    }
};
```

Because the geometry is fixed and small, you can even use `std::array` instead of `std::vector` and put the whole thing on the stack / in the object — no heap at all, ever. That's the cleanest possible answer for this project.

```cpp
std::array<std::uint64_t, L1_SETS * WAYS> l1_tag_{};   // no heap, fixed size, zero-initialized
```

---

## 3. The Allocation Toolbox (for when sizes aren't fixed)

The cache sim has fixed sizes, so fixed arrays suffice. But the general low-latency toolkit — which you'll need when sizes are dynamic (Week 3+ queues, variable order batches) — is worth knowing now.

### Bump / arena allocator

One big buffer + a cursor. Allocation is "return cursor, advance cursor". No per-object free; you reset the whole arena at a safe point (e.g. end of a batch).

```cpp
class Arena {
    std::vector<std::byte> buf_;
    std::size_t off_ = 0;
public:
    explicit Arena(std::size_t bytes) : buf_(bytes) {}
    void* alloc(std::size_t n, std::size_t align) {
        std::size_t p = (off_ + align - 1) & ~(align - 1);
        void* r = buf_.data() + p;
        off_ = p + n;
        return r;                       // O(1), no syscall, no lock
    }
    void reset() noexcept { off_ = 0; } // free everything at once
};
```

### Object pool / free list

Pre-allocate `N` objects; hand out and return them via a free list. Constant-time, no fragmentation, perfect when you allocate and free objects of one type repeatedly (e.g. order nodes, ring slots).

### Fixed-capacity containers

A `std::array` + a size counter is a stack-allocated, zero-heap "vector" when you know the max length. This is exactly how you'd fix Week 1's `std::vector<Order>` return path (see §5).

---

## 4. Spotting Hidden Allocations

The sneaky part: allocations you didn't write. Watch for these in a hot path:

- `std::vector` / `std::string` constructed (or `push_back` past capacity) inside the loop.
- `std::unordered_map::operator[]` inserting a new key (allocates a node) — the Week-1 `state_[std::string(t.symbol)]` did exactly this, and constructing a `std::string` from a `string_view` allocates too.
- Returning a container by value where the callee builds it fresh each call.
- `std::function`, `std::any`, lambdas with large captures.
- Exceptions paths (`vector::at`, `std::stoi`).

Confirm with tooling rather than guessing:

```bash
# Count allocations during a run with valgrind (slow but exact):
valgrind --tool=massif ./build/cache_sim_runner data/large.trace

# Or watch page faults / minor faults stay flat after warm-up:
perf stat -e page-faults,minor-faults ./build/cache_sim_runner data/large.trace
```

If `page-faults` keeps climbing during `run()`, you're still allocating. A correct Week-2 sim shows faults during `on_init` and load, then **flat** through `run()`.

---

## 5. Back to Week 1: the `std::vector<Order>` Return Path

Week 1's `on_tick` returned `std::vector<csot::Order>` — convenient, but it allocates on every tick that emits an order. We flagged this as a deliberate Week-1 simplification. The zero-alloc fix (which you can apply to your Week-1 strategy as a Week-2 exercise) is a **fixed-capacity** return:

```cpp
// A strategy emits at most a handful of orders per tick. Cap it, no heap.
struct OrderBatch {
    std::array<csot::Order, 4> orders;
    std::uint8_t count = 0;
    void push(const csot::Order& o) { if (count < orders.size()) orders[count++] = o; }
};
```

Same idea as the cache sim: a known upper bound + a fixed array = zero allocation. (The frozen Week-1 ABI still returns a vector, so this is an internal refactor / measurement exercise, not an ABI change — `AGENTS.md` keeps `strategy.hpp` frozen.)

The cache sim's ABI sidesteps the problem entirely: `run()` returns one 56-byte `CacheStats` by value. There's nowhere to allocate.

---

## 6. The Discipline, Stated

1. Compute every size in `on_init` (or at compile time).
2. Allocate every buffer once, there.
3. In the hot path: only **read and write** those buffers. Never `new`, `resize`, `push_back`-past-capacity, insert into a node-based map, or throw.
4. Verify with `perf stat -e page-faults` that faults are flat during `run()`.

Do this and your `run()` time becomes smooth and predictable — which is exactly what a wall-clock leaderboard rewards.

---

## 7. Where the Fixed Arrays Live: the Stack-Size Trap

"Use `std::array` instead of `std::vector`" is great advice — with one footgun. *Where* the array lives matters. A big `std::array` declared as a **local** inside `run()` lands on the stack, and the default thread stack is ~8 MiB. Our L1+L2 model is only ~45 KiB so it fits, but the instinct to "just put it on the stack" breaks the moment a future challenge needs a few MiB of tables:

```cpp
csot::CacheStats run(const csot::MemAccess* acc, std::size_t n) override {
    std::array<std::uint64_t, 1u << 20> scratch{};   // (!) 8 MiB local → stack overflow / re-zeroed every call
    // ...
}
```

Two things are wrong: an 8 MiB local can blow the stack, and as a *local* it gets constructed (zeroed) on **every** `run()` entry — re-paying a cost that belongs in the cold path. The fix is to make large fixed state a **member of the object** (or `static`), sized once, living for the object's lifetime:

```cpp
class FastCacheSim final : public csot::CacheSim {
    alignas(64) std::array<std::uint64_t, L1_SETS * WAYS> l1_tag_{};  // member: built once, lives in the object
    // ... run() only reads/writes l1_tag_, never re-declares it ...
};
```

Member arrays of an object you `new` (the factory returns `new FastCacheSim`) live on the heap — but allocated **once**, in the cold path, which is exactly the rule. The point isn't "stack good, heap bad"; it's "**allocate the storage once and reuse it**", and member/`static` storage is how you express that for fixed sizes without a single `run()`-time allocation.

---

## 8. Reserve-and-Reuse: Zero Allocation Without Fixed Sizes

The cache sim has fixed sizes, but the broader pattern — which you'll lean on from Week 3 — is **reserve once to a high-water mark, then reuse without ever shrinking**. The trick is to `clear()` a container (which keeps its capacity) instead of destroying it:

```cpp
class Worker {
    std::vector<Result> out_;            // reused across calls
public:
    void on_init() { out_.reserve(MAX_RESULTS); }   // one allocation, ever
    void step() {
        out_.clear();                    // size → 0, CAPACITY retained → no free, no realloc
        // ... push_back up to MAX_RESULTS: never reallocates, because capacity is held ...
    }
};
```

`clear()` is the key insight most people miss: it runs destructors and sets size to zero but **keeps the buffer**, so the next round of `push_back`s reuses the same memory — zero allocation after the first `reserve`. The only way this re-allocates is exceeding the reserved capacity, so size your reserve to the worst case you can prove. This is the same "bound it, then reuse it" idea as the fixed-capacity batch in §5, generalized to when you don't know the exact size but *do* know a ceiling.

> ⚠️ `clear()` keeps capacity; `shrink_to_fit()`, swapping with a fresh vector, or letting the vector go out of scope all *free* it — and you'll re-allocate next round. In a reuse loop, `clear()` and only `clear()`.

---

## 🎯 Key Takeaways

- Hot-path `malloc` costs 50–500 ns and spikes to microseconds — the #1 cause of p99 jitter. Determinism matters as much as the mean.
- **Allocate once in `on_init`; the hot path only reads/writes those buffers.** Zero heap traffic in `run()`.
- The cache geometry is fixed and small, so `std::array` (no heap at all, ever) is the cleanest layout for this project.
- General toolbox for dynamic sizes: **bump/arena** (O(1), reset-to-free), **object pool/free list**, **fixed-capacity containers**.
- Hunt hidden allocations: `vector`/`string` in the loop, `unordered_map[]` inserts, by-value container returns, `std::function`.
- Verify, don't assume: `perf stat -e page-faults` should be flat during `run()`; massif counts exact allocations.
- **Where** fixed arrays live matters: a big `std::array` *local* in `run()` re-zeros every call and can overflow the ~8 MiB stack — make large state a **member** (or `static`), built once.
- For dynamic-but-bounded sizes, **`reserve` once + `clear()` to reuse**: `clear()` keeps capacity (no realloc); `shrink_to_fit`/swap/scope-exit free it and force re-allocation.
- The Week-1 `std::vector<Order>` return is fixable with a fixed-capacity batch — same bounded-size trick, no ABI change.

---

## 📚 Further Reading — Allocation & the Hot Path

- 🎬 **[Carl Cook — "When a Microsecond Is an Eternity"](https://www.youtube.com/watch?v=NH1Tta7purM)** (CppCon 2017) — the "allocate nothing on the hot path" doctrine, from an HFT trenches view.
- 🎬 [John Lakos — "Local ('Arena') Memory Allocators"](https://www.youtube.com/watch?v=nZNd5FjSquk) (CppCon 2017) — arenas/pools in depth, with numbers.
- 📰 [Bloomberg `bdlma` arena allocators](https://github.com/bloomberg/bde) — production arena/pool implementations to read.
- 📖 [pmr (polymorphic memory resources), cppreference](https://en.cppreference.com/w/cpp/memory/memory_resource) — the standard-library arena/pool toolkit (`std::pmr::monotonic_buffer_resource`).
- 📰 [What a typical malloc does — glibc malloc internals](https://sourceware.org/glibc/wiki/MallocInternals) — why the slow path is slow.

---

## ▶️ Next

[`05-compile-time-and-static-polymorphism.md`](./05-compile-time-and-static-polymorphism.md) — with the heap gone, we push *computation* to compile time: `constexpr` masks, and replacing virtual dispatch with templates so the compiler can inline your whole hot loop.
