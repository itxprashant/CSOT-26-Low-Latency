# Cache Spec — Reference Two-Level Cache Hierarchy

> **This file is a frozen competition contract.** Every submission must simulate this exact cache hierarchy and emit the exact same counters as the judge reference simulator for the same trace. The challenge is not to design a cleverer cache; it is to simulate this fixed one *correctly* and then *as fast as possible* — fewer allocations, better cache locality of your own data structures, branchless set lookup, compile-time geometry.

---

## 1. Goal

The Week-2 challenge is computer-architecture-shaped, but it is **not** a cache-design contest.

Every participant receives the same:

- `MemAccess` / `CacheStats` / `CacheSim` ABI from [`include/cache_sim.hpp`](./include/cache_sim.hpp)
- memory-trace file format from §2 below
- cache hierarchy specification in this file

Every participant must emit the same `CacheStats` as the judge reference implementation for the same input trace. If your counters differ, your submission is incorrect. Among correct submissions, the leaderboard ranks by **wall-clock time to simulate a large held-out trace** (and throughput) — the same "fastest correct implementation" game as Week 1's strategy, on a new kernel.

---

## 2. The Trace Format (frozen)

A trace is a binary file: a contiguous array of `csot::MemAccess` records, little-endian, **no header**. Each record is exactly 16 bytes and is identical on disk and in memory, so the judge `mmap`s the file straight into a `const MemAccess*`.

```text
offset 0  : uint64  address     (byte address of the access, little-endian)
offset 8  : uint8   is_write    (0 = read / load, 1 = write / store)
offset 9  : uint8   _reserved[7] (always zero)
```

Guarantees you can rely on:

- File size is always a multiple of 16. `n = filesize / 16` accesses.
- `is_write` is always `0` or `1`.
- `_reserved` bytes are always zero. Do not read them.
- Addresses are arbitrary 64-bit byte addresses; they need **not** be aligned. Only the **block address** `address >> 6` matters (the low 6 bits are the byte offset within a 64-byte line and are ignored by the model).
- Accesses are processed strictly in file order. Order is significant — LRU and dirty state depend on it.

The reference generator [`data/gen_trace.py`](./data/gen_trace.py) emits this format; `data/tiny.trace` is a small golden file and `data/tiny.stats.json` holds its exact reference counters.

---

## 3. Geometry Constants (frozen)

These constants are part of the spec. Do not change them.

| Name | Value | Meaning |
|---|---:|---|
| `LINE_SIZE` | `64` | Bytes per cache line (block). Offset = low 6 bits. |
| `L1_SIZE` | `32 KiB` | Total L1 data capacity |
| `L1_WAYS` | `8` | L1 associativity |
| `L1_SETS` | `64` | `L1_SIZE / LINE_SIZE / L1_WAYS` |
| `L2_SIZE` | `256 KiB` | Total L2 capacity |
| `L2_WAYS` | `8` | L2 associativity |
| `L2_SETS` | `512` | `L2_SIZE / LINE_SIZE / L2_WAYS` |
| Replacement | **true LRU**, per set | Least-recently-*used* victim, exact (not pseudo-LRU) |
| Write policy | **write-back + write-allocate** | At **both** levels |
| Inclusion | **non-inclusive, non-exclusive (NINE)** | A line may live in L1, L2, both, or neither |

Derived addressing (let `b = address >> 6` be the **block address**):

```text
L1 set index = b & 63           (L1_SETS - 1)
L1 tag       = b >> 6
L2 set index = b & 511          (L2_SETS - 1)
L2 tag       = b >> 9
```

Two accesses with the same block address `b` touch the same line everywhere. You can recover a resident line's block address from its set and tag (`b = (tag << index_bits) | set`), which you need when writing a dirty L1 victim back to L2.

---

## 4. State

Per cache level you maintain, per set, `WAYS` line slots and a recency order over them:

```cpp
struct Line {
    bool          valid;
    bool          dirty;
    std::uint64_t tag;
};
// Plus, per set, an exact LRU ordering of its WAYS slots.
```

You do not need this exact layout — SoA, packed bitsets, whatever is fastest — but the **behaviour** must be identical. All lines start `valid = false`.

The seven counters in `CacheStats` start at zero and are updated exactly as §5 prescribes.

---

## 5. Per-Access Algorithm

For every access `(address, is_write)` in trace order, perform exactly the following.

### 5.1 — Count the access

```text
if is_write: writes  += 1
else:        reads   += 1
```

### 5.2 — Probe L1

Compute `b`, `s1`, `t1`. Search the 8 ways of `L1[s1]` for a valid line with tag `t1`.

- **L1 hit:** `l1_hits += 1`; mark that line **MRU** in `L1[s1]`; if `is_write`, set its `dirty = true`. **Done with this access.**
- **L1 miss:** `l1_misses += 1`; continue to §5.3.

### 5.3 — Probe L2 (only on an L1 miss)

Compute `s2`, `t2`. Search the 8 ways of `L2[s2]` for a valid line with tag `t2`.

- **L2 hit:** `l2_hits += 1`; mark that line **MRU** in `L2[s2]`.
- **L2 miss:** `l2_misses += 1`; **demand-install** the line into L2 (fetch a *clean* copy from memory):
  - if `L2[s2]` has an invalid way, use it; else evict the **LRU** way — and if that victim is `dirty`, `dirty_writebacks += 1`.
  - place `{valid, dirty=false, tag=t2}` and mark it **MRU**.

### 5.4 — Fill the line into L1 (write-allocate)

After §5.3, bring the line into L1:

- if `L1[s1]` has an invalid way, use it; else evict the **LRU** way:
  - if that L1 victim is `dirty`, **write it back to L2** (§5.5).
- place `{valid, dirty = is_write, tag = t1}` and mark it **MRU** in `L1[s1]`.

(The freshly filled line is dirty iff this access was a write — write-allocate, then immediately written.)

### 5.5 — Writing a dirty L1 victim back to L2

A dirty line evicted from L1 carries its data down to L2. Recover its block address `b_v = (victim.tag << 6) | s1`, then `s2v = b_v & 511`, `t2v = b_v >> 9`, and:

- if a valid line with tag `t2v` is present in `L2[s2v]`: set its `dirty = true`.
- else (NINE — the line was not in L2): **install it dirty** into `L2[s2v]` — invalid way if available, else evict the LRU way (`dirty_writebacks += 1` if that victim was dirty), place `{valid, dirty=true, tag=t2v}` and mark **MRU**.

> ⚠️ **A writeback is not a demand access.** §5.5 must **never** touch `l2_hits` or `l2_misses`, and must **never** update LRU recency for a *hit* in L2 (only the install case sets MRU on the newly placed line). LRU recency moves only on §5.2/§5.3 demand hits and on fills. Getting this wrong drifts every later counter.

That is the entire model. Every access falls through exactly one of: L1 hit (§5.2), or L1 miss with L2 hit (§5.3), or L1 miss with L2 miss (§5.3) — and L1-miss accesses additionally do §5.4 (and maybe §5.5).

---

## 6. Reference Pseudocode

This pseudocode is the behavioural reference. If the prose and the pseudocode appear to disagree, treat this pseudocode as authoritative and report the ambiguity. It is written for clarity, **not speed** — your job is to reproduce its counters far faster.

```cpp
struct Cache {            // one level: SETS x WAYS, true LRU per set
    int sets, ways, index_bits;
    // lines[set][way] = {valid, dirty, tag}; lru[set] = ways ordered MRU..LRU
};

// returns the way index if (set, tag) is resident, else -1
int find(Cache& c, int set, uint64_t tag);

// move way to most-recently-used within its set
void touch(Cache& c, int set, int way);

// pick an invalid way if any, else the LRU way (the victim)
int victim_way(Cache& c, int set);

void simulate(const MemAccess* acc, size_t n, CacheStats& st) {
    Cache L1{64, 8, 6}, L2{512, 8, 9};

    for (size_t i = 0; i < n; ++i) {
        const uint64_t b  = acc[i].address >> 6;
        const bool     wr = acc[i].is_write != 0;
        wr ? ++st.writes : ++st.reads;

        const int      s1 = b & 63;
        const uint64_t t1 = b >> 6;

        int w1 = find(L1, s1, t1);
        if (w1 >= 0) {                      // ---- L1 hit ----
            ++st.l1_hits;
            touch(L1, s1, w1);
            if (wr) L1.dirty(s1, w1) = true;
            continue;
        }
        ++st.l1_misses;                     // ---- L1 miss ----

        const int      s2 = b & 511;
        const uint64_t t2 = b >> 9;
        int w2 = find(L2, s2, t2);
        if (w2 >= 0) {                      // L2 hit
            ++st.l2_hits;
            touch(L2, s2, w2);
        } else {                            // L2 miss: demand-install CLEAN
            ++st.l2_misses;
            int v = victim_way(L2, s2);
            if (L2.valid(s2, v) && L2.dirty(s2, v)) ++st.dirty_writebacks;
            L2.set(s2, v, /*valid*/true, /*dirty*/false, t2);
            touch(L2, s2, v);
        }

        // ---- fill into L1 (write-allocate) ----
        int v1 = victim_way(L1, s1);
        if (L1.valid(s1, v1) && L1.dirty(s1, v1)) {       // evict dirty L1 line
            const uint64_t bv  = (L1.tag(s1, v1) << 6) | s1;
            const int      s2v = bv & 511;
            const uint64_t t2v = bv >> 9;
            int wv = find(L2, s2v, t2v);
            if (wv >= 0) {
                L2.dirty(s2v, wv) = true;                 // NOT counted, NOT touched
            } else {                                      // install dirty into L2
                int vv = victim_way(L2, s2v);
                if (L2.valid(s2v, vv) && L2.dirty(s2v, vv)) ++st.dirty_writebacks;
                L2.set(s2v, vv, true, /*dirty*/true, t2v);
                touch(L2, s2v, vv);
            }
        }
        L1.set(s1, v1, true, /*dirty*/wr, t1);
        touch(L1, s1, v1);
    }
}
```

You may replace this with flat SoA arrays, branchless or SIMD tag scans, compile-time-folded masks, prefetching — anything — as long as the seven returned counters are bit-for-bit identical.

---

## 7. Correctness Rules

A submission is correct if, for the same trace, it emits a `CacheStats` whose **all seven fields** equal the judge reference exactly. These identities must hold for any valid trace and are the first things to check:

```text
reads + writes            == l1_hits + l1_misses        (every access probes L1 once)
l1_misses                 == l2_hits + l2_misses         (L2 probed only on L1 miss)
```

`dirty_writebacks` counts **only** dirty lines evicted from **L2 to main memory** (§5.3 demand install and §5.5 writeback install). It does **not** count L1→L2 writebacks (those stay on-chip).

Things that make a submission incorrect:

- Updating LRU recency on a writeback (§5.5) instead of only on demand hits/fills.
- Counting an L1→L2 writeback as an `l2_hit` / `l2_miss`.
- Write-no-allocate (skipping the fill on a write miss). Both levels are write-allocate.
- Sample/approximate LRU (e.g. clock, NRU, pseudo-LRU) that disagrees with exact LRU on any set.
- Using a different inclusion policy (inclusive/exclusive) that changes which lines are resident.
- Reading the `_reserved` bytes, or assuming addresses are aligned.

---

## 8. Performance Rules

Correctness is binary. Performance is ranked among correct submissions only.

The leaderboard ranks by:

1. lower **wall-clock time** for `run()` over the hidden trace
2. higher **throughput** (accesses/second) on the same trace
3. (shown, may enter the formula in later seasons) per-segment timing stability

Exact weighting is defined in the platform docs. Cache-design quality is not part of the score — the geometry is fixed; only your simulator's speed is ranked.

### Hot-path expectations

| Week | Expected implementation focus for a competitive `run()` |
|---|---|
| 2 | Zero heap allocation after `on_init`; flat SoA set arrays (no `unordered_map`); compile-time geometry masks; branch-lean tag scan |
| 3 | (cache challenge retires from the board) layout that avoids false sharing if you later parallelize replays |
| 4 | spec-strategy returns to the board with new metrics; cache sim becomes a hall-of-fame / stretch artifact |

The anti-saturation premise: the correct counters are a fixed target, but simulating hundreds of millions of accesses *fast* has a high ceiling. Expect the board to keep moving long after correctness is solved.

---

## 9. Common Pitfalls

- **Touching LRU on a writeback.** §5.5 never reorders recency on an L2 hit. This is the single most common off-by-a-few-counts bug.
- **Counting writebacks as L2 accesses.** Only §5.2/§5.3 demand probes increment `l1_*` / `l2_*`.
- **Write-no-allocate.** A write that misses must fetch + fill, then be dirty.
- **`dirty_writebacks` over/under-count.** Only dirty lines leaving **L2** for memory count — both on demand-miss eviction (§5.3) and writeback-install eviction (§5.5).
- **Sample stddev of LRU.** Pseudo-LRU is *not* allowed; the reference is exact LRU. (Pseudo-LRU is a great *speed* idea — but it changes counters, so it fails correctness.)
- **Recovering the victim's address wrong.** `b_v = (tag << index_bits) | set`, using the **L1** index bits (6) when evicting from L1.
- **Assuming aligned addresses.** Always work with the block address `address >> 6`.
- **Heap inside `run()`.** Allocate in `on_init()`; the hot path must not `malloc`.

---

## 10. Why This Is Still the Same Track

The workload is computer-architecture-shaped — lines, sets, ways, replacement, write-back — exactly the COL216 material. But the contest objective is identical to Week 1: everyone simulates the *same* fixed hierarchy, and the leaderboard measures **systems-engineering speed**, not cache cleverness. The architecture supplies the workload; the leaderboard measures how fast you can drive a hot loop over a huge array without missing your own cache.

That distinction is the whole challenge.
