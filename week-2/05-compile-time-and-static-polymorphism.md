# 05 — Compile-Time Computation & Static Polymorphism

> **TL;DR** — Work the compiler does is work the CPU doesn't. Make the cache geometry `constexpr` so index/offset masks fold into immediates, and prefer **templates / CRTP** over virtual calls inside the hot loop so the compiler can inline and specialize. The judge calls `create_cache_sim()` (one virtual, once) — but everything *inside* your `run()` should be statically resolved.

The heap is gone ([`04-zero-allocation.md`](./04-zero-allocation.md)). Now we move computation itself out of run-time. This is the topic the Week-1 bonus ([`week-1/05-bonus-compiler.md`](../week-1/05-bonus-compiler.md)) promised would "explode in Week 2".

---

## 1. `constexpr`: Compute It Before the Program Runs

A `constexpr` value is computed at compile time and baked into the binary as a literal. For the cache sim, every geometry number is known at compile time, so encode it as such:

```cpp
namespace geo {
inline constexpr std::uint64_t LINE_BITS = 6;          // 64-byte line
inline constexpr std::uint64_t L1_SETS   = 64;
inline constexpr std::uint64_t L2_SETS   = 512;
inline constexpr std::uint64_t WAYS      = 8;

inline constexpr std::uint64_t L1_INDEX_MASK = L1_SETS - 1;   // 63
inline constexpr std::uint64_t L2_INDEX_MASK = L2_SETS - 1;   // 511
}
```

Now the per-access index computation:

```cpp
const std::uint64_t b   = acc[i].address >> geo::LINE_BITS;     // shift by an immediate
const std::uint64_t s1  = b & geo::L1_INDEX_MASK;               // AND with an immediate
```

On [godbolt.org](https://godbolt.org) you'll see `shr rax, 6` and `and rax, 63` — the constants are *in the instruction*, not loaded from memory. Compare with reading `sets_count_` from a member variable: that's an extra load the compiler often can't prove invariant. `constexpr` removes the question.

> 💡 Because `L1_SETS` is a power of two, `b & 63` *is* `b % 64` — but the compiler emits a 1-cycle `and` instead of a multi-cycle division. Powers of two in the spec are a gift; use masks, never `%`.

---

## 2. `consteval` and Bigger Compile-Time Tables

`constexpr` *may* run at compile time; **`consteval`** (C++20) *must*. Use it for helpers that should never sneak into run-time:

```cpp
consteval std::uint64_t index_mask(std::uint64_t sets) { return sets - 1; }
inline constexpr std::uint64_t L1_MASK = index_mask(geo::L1_SETS);   // forced at compile time
```

You can also precompute whole lookup tables at compile time:

```cpp
// A compile-time table: for each packed LRU state, the way to evict.
consteval std::array<std::uint8_t, 256> make_victim_table() {
    std::array<std::uint8_t, 256> t{};
    for (int i = 0; i < 256; ++i) t[i] = /* derive LRU victim from packed bits */ 0;
    return t;
}
inline constexpr auto kVictim = make_victim_table();   // lives in .rodata, zero run-time cost
```

The table is computed once, by the compiler, and stored in read-only data. At run time it's a single indexed load. This is how you turn a branchy LRU update into a table lookup — a real bonus-tier optimization (see [`06-bonus-simd-and-prefetch.md`](./06-bonus-simd-and-prefetch.md)).

---

## 3. Templating on the Geometry

Both cache levels share the same logic — only the set count and masks differ. Instead of branching on the level at run time, **template** on the geometry so the compiler stamps out a specialized loop for each:

```cpp
template <std::uint64_t SETS, std::uint64_t WAYS>
struct Level {
    static constexpr std::uint64_t INDEX_MASK = SETS - 1;
    std::array<std::uint64_t, SETS * WAYS> tag{};
    std::array<std::uint8_t,  SETS * WAYS> vd{};       // valid|dirty
    std::array<std::uint32_t, SETS>        lru{};

    // probe is fully specialized: SETS, WAYS, MASK are all immediates here
    int find(std::uint64_t b) const {
        const std::uint64_t set = b & INDEX_MASK;
        const std::uint64_t base = set * WAYS;
        for (std::uint64_t w = 0; w < WAYS; ++w)        // WAYS is constant → unrolls
            if ((vd[base + w] & 1) && tag[base + w] == (b >> bits_for(SETS)))
                return int(w);
        return -1;
    }
};

Level<64, 8>  l1_;
Level<512, 8> l2_;
```

With `WAYS` a compile-time constant, the 8-iteration scan **unrolls** into 8 straight-line comparisons (or vectorizes). With `INDEX_MASK` constant, the set index is one `and`. The compiler specializes `Level<64,8>` and `Level<512,8>` independently — no run-time branch on "which level am I".

---

## 4. Virtual Calls vs. Static Polymorphism

A virtual call is an indirect jump through a vtable: load the vtable pointer, load the function pointer, call it. That's ~2–5 ns *and*, worse, it's an **optimization barrier** — the compiler usually can't inline through it, so it can't fold, vectorize, or hoist across the call.

```cpp
struct Base { virtual int probe(std::uint64_t) = 0; };   // every probe() = indirect call, no inlining
```

For the **hot loop**, you don't want that. Two ways to keep dispatch static:

### (a) Just don't be virtual inside `run()`

The judge calls one virtual function — `run()` — exactly once. Everything *inside* `run()` can be plain non-virtual member calls or free functions, which inline freely. This is the simplest answer and all this project needs: keep your set/LRU helpers non-virtual and let them inline into the loop.

### (b) CRTP — compile-time polymorphism when you *want* an interface

The Curiously Recurring Template Pattern lets you write "interface-like" code that's resolved at compile time:

```cpp
template <class Derived>
struct CacheBase {
    int probe(std::uint64_t b) { return static_cast<Derived*>(this)->probe_impl(b); }
};
struct L1Cache : CacheBase<L1Cache> {
    int probe_impl(std::uint64_t b) { /* ... */ return 0; }   // inlined, no vtable
};
```

`probe()` resolves to `L1Cache::probe_impl` at compile time and inlines — zero-overhead "virtual". You rarely need CRTP for *this* project (option (a) suffices), but it's the standard tool when you want polymorphism without the vtable, and it's everywhere in fast C++ (Eigen, many HFT codebases).

> 📌 The Week-1 bonus Experiment 4 showed `final` letting the compiler **devirtualize** a call. Same spirit: give the compiler enough static information and the indirect call disappears.

---

## 5. `if constexpr` — Branches That Vanish

When a branch depends only on a compile-time value, `if constexpr` discards the dead arm entirely — no run-time test:

```cpp
template <bool IsWrite>
void touch(std::uint64_t set, std::uint64_t way) {
    update_lru(set, way);
    if constexpr (IsWrite) {          // compiled away when IsWrite == false
        set_dirty(set, way);
    }
}
```

Calling `touch<true>` and `touch<false>` produces two specialized functions with no branch on write-ness. Use this when you can dispatch on a compile-time property; don't contort run-time data into template parameters (the trace's `is_write` is run-time data — that branch stays).

---

## 6. `__restrict__`: Promise the Compiler Nothing Aliases

Compile-time constants are half the codegen story; the other half is **aliasing**. When `run()` reads `acc[i]` and writes your tag arrays, the compiler must assume — unless told otherwise — that a write to `tag_[…]` *might* change `acc[i]` (they could overlap in memory). That forces it to reload values from memory after every store instead of keeping them in registers, and it blocks vectorization.

You know the trace and your state never overlap. Say so with `__restrict__` (GCC/Clang) on the pointers:

```cpp
csot::CacheStats run(const csot::MemAccess* __restrict__ acc, std::size_t n) override {
    std::uint64_t* __restrict__ tag = l1_tag_.data();   // promise: tag and acc are disjoint
    // ...the compiler may now keep tag[...] in registers across loop iterations,
    //    and is free to vectorize the scan because no store can alias the load.
}
```

`__restrict__` is a *promise*, not a check — if you lie (the regions really do overlap) you get undefined behaviour, not a diagnostic. Here the promise is trivially true: `acc` is a read-only input buffer, your state is separate storage. The payoff shows up in the asm as fewer reloads and tighter loops; confirm it on godbolt (§ below) rather than assuming.

> 💡 `const` and `__restrict__` solve different problems. `const` says *you* won't write through this pointer; `__restrict__` says *nobody* will reach this memory through a different pointer. The scan wants both: `const … * __restrict__`.

---

## 7. Controlling Inlining — When the Compiler Needs a Nudge

§4 keeps the hot loop non-virtual so helpers *can* inline. Usually `-O3` inlines small hot-path helpers automatically. Occasionally it won't — a helper is just over the inliner's size threshold, or sits in a different translation unit — and an un-inlined call in the inner loop is a real `call`/`ret` plus a lost optimization boundary. You can force the issue:

```cpp
[[gnu::always_inline]] inline int probe(std::uint64_t b) { /* hot helper */ return 0; }
```

Conversely, mark the **rare** miss-handler `[[gnu::noinline]]` (or `[[unlikely]]`, see [`06`](./06-bonus-simd-and-prefetch.md) §5) so it doesn't bloat the hot loop's instruction footprint — keeping the common path dense in the instruction cache:

```cpp
[[gnu::noinline]] void handle_miss(std::uint64_t b, csot::CacheStats& s);  // kept out of line
```

Because you submit a **single translation unit** ([`project/README.md`](./project/README.md)), everything is in one file and the compiler can already see and inline all of it — so you rarely need `always_inline` here. But the knobs matter the moment a helper is fat: inline the small frequent thing, keep the big rare thing out of line. As always, the rule is *measure* — a forced inline that bloats the loop past the I-cache can be slower than the call it replaced.

---

## 8. The Balance: Don't Over-Template

Compile-time tricks are spices, not the meal:

- **Templating on geometry** (a handful of fixed constants): great, do it.
- **Templating on the trace contents**: impossible — the data is run-time.
- **`constexpr` masks and small tables**: free wins, do them.
- **A `consteval` 64 KiB table that takes 3 minutes to compile**: probably not worth it; measure compile time too.
- **CRTP everywhere**: only where you genuinely need an interface in the hot path.

The judge build uses fixed flags ([`platform_week_2.md`](../../platform_week_2.md)); verify your compile-time work actually folded by reading the asm on godbolt with those same flags, not just trusting that `constexpr` "should" help.

---

## 🎯 Key Takeaways

- `constexpr` geometry → index/offset masks become **instruction immediates**, not memory loads. Powers of two → `&` mask, never `%`.
- `consteval` forces compile-time evaluation and can bake whole **lookup tables** into `.rodata` (e.g. an LRU-victim table).
- **Template on the geometry** (`Level<64,8>`, `Level<512,8>`): the `WAYS` scan unrolls/vectorizes and the level branch disappears.
- **Virtual calls are optimization barriers.** The judge needs exactly one (`run`) — keep everything *inside* the loop non-virtual so it inlines.
- **CRTP** gives zero-overhead polymorphism when you actually want an interface in the hot path; `final` lets the compiler devirtualize.
- `if constexpr` removes branches that depend on compile-time values; it can't remove branches on run-time data like `is_write`.
- **`__restrict__`** promises the trace and your state don't alias, letting the compiler keep state in registers and vectorize the scan — true here since `acc` is a separate read-only buffer.
- Control inlining deliberately: `always_inline` the small frequent helper, `noinline` the rare miss-handler to keep the hot loop dense in the I-cache (one TU already lets the compiler see everything).
- Don't over-template — verify on godbolt with the **judge's flags** that the folding really happened.

---

## 📚 Further Reading — Compile-Time C++ & Zero-Overhead Dispatch

- 🎬 **[Jason Turner — "constexpr ALL the things!"](https://www.youtube.com/watch?v=PJwd4JLYJJY)** (CppCon 2017) — how far compile-time evaluation goes, with live demos.
- 🎬 [Matt Godbolt — "What Has My Compiler Done For Me Lately?"](https://www.youtube.com/watch?v=bSkpMdDe4g4) (CppCon 2017) — watch constants fold and calls devirtualize.
- 📰 [CRTP — cppreference / "Curiously Recurring Template Pattern"](https://en.cppreference.com/w/cpp/language/crtp) — the canonical static-polymorphism pattern.
- 📰 [Fluent C++ — "The CRTP, episode by episode"](https://www.fluentcpp.com/2017/05/12/curiously-recurring-template-pattern/) — a gentle, practical CRTP walkthrough.
- 📖 [cppreference — `constexpr`, `consteval`, `if constexpr`](https://en.cppreference.com/w/cpp/language/constexpr) — the precise rules.

---

## ▶️ Next

[`06-bonus-simd-and-prefetch.md`](./06-bonus-simd-and-prefetch.md) — ⭐ the bonus tier: branchless and SIMD tag scans, prefetching the next set, reading your hot loop's assembly. This is the toolkit that keeps the leaderboard from saturating.
