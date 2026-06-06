# 02 — Cache Internals: Lines, Sets, Ways, and the Policies That Decide

> **TL;DR** — A cache is a hardware hash table with a fixed number of buckets (**sets**), a fixed bucket depth (**ways**), and an eviction rule (**replacement policy**) for when a bucket fills. An address is split into **offset / index / tag**; the index picks the set, the tag confirms the line. Master those three fields and the replacement and write policies, and you've understood the machine you're about to simulate.

We covered the memory *hierarchy* from the outside in Week 1 ([`week-1/03-memory-hierarchy.md`](../week-1/03-memory-hierarchy.md)) — L1 is ~1 ns, DRAM ~100 ns, lines are 64 bytes. Now we open the box: *how* does a cache decide whether an address is a hit, and *where* does it live? This is the conceptual backbone of the Week-2 project — read it slowly.

---

## 1. The Cache Line Is the Atom

Caches never deal in bytes; they deal in **lines** (a.k.a. blocks) of a fixed size — **64 bytes** on every CPU you'll touch. When you load one byte, the cache loads the whole 64-byte line containing it. The low bits of an address that pick a byte *within* a line are the **offset** and the cache ignores them when deciding hit/miss.

```text
LINE_SIZE = 64  →  offset is the low 6 bits   (2^6 = 64)
block address = full address >> 6              (which 64-byte line)
```

In the project, only the **block address** `address >> 6` matters. Two accesses to addresses 0 and 63 touch the same line (same block address 0); an access to 64 touches the next line. This is why [`CACHE_SPEC.md`](./project/CACHE_SPEC.md) tells you to start every access with `b = address >> 6`.

---

## 2. Direct-Mapped: One Home Per Line

The simplest cache: each line has exactly **one** place it can live, chosen by some bits of its address — the **index**.

```text
        ┌──────────── address ────────────┐
        │   tag    │   index   │  offset   │
        └──────────┴───────────┴───────────┘
                        │           └─ byte within the 64-B line (ignored)
                        └───────────── which set (row) this line maps to
```

With `S` sets, `index = block_address & (S - 1)` (when `S` is a power of two — and it always is). The leftover high bits are the **tag**, stored alongside the line so the cache can confirm "yes, *this* line, not another one that maps to the same set".

A direct-mapped cache is fast but brittle: two hot lines that happen to share an index evict each other forever (a **conflict miss**), even if the rest of the cache is empty.

---

## 3. Set-Associative: N Homes Per Line

The fix: give each index **`W` slots** (ways) instead of one. A line maps to a *set* (by index), and within that set it can live in any of the `W` ways. This is an **`W`-way set-associative** cache. Our project uses **8-way** at both levels.

```text
set 0 :  [ way0 ][ way1 ][ way2 ][ way3 ][ way4 ][ way5 ][ way6 ][ way7 ]
set 1 :  [ way0 ][ way1 ] ...
 ...
set 63:  ...
         each way = { valid, dirty, tag }   (+ per-set LRU ordering)
```

To probe: compute the index, then compare the tag against all `W` ways of that set. A match in any way (that is `valid`) is a **hit**; no match is a **miss**. That `W`-way tag comparison is the inner loop of your simulator — and the place SIMD and branchless tricks pay off ([`06-bonus-simd-and-prefetch.md`](./06-bonus-simd-and-prefetch.md)).

### The three address fields, for our L1

L1 is 32 KiB, 8-way, 64-byte lines → `32768 / 64 / 8 = 64` sets.

```text
block address b = address >> 6
L1 index = b & 63          (6 index bits → 64 sets)
L1 tag   = b >> 6
```

L2 is 256 KiB, 8-way → 512 sets → `index = b & 511`, `tag = b >> 9`. These are the exact splits in `CACHE_SPEC.md` §3. Note the tag is just "the block address with the index bits stripped"; you can store the *whole* block address as the tag if you prefer — comparisons still work, it just uses a few more bits.

---

## 4. Replacement: Who Gets Evicted?

When a set is full (all `W` ways valid) and a new line must come in, one resident line is **evicted** to make room. The **replacement policy** chooses the victim.

| Policy | Victim | Cost to simulate | Used by |
|---|---|---|---|
| **LRU** (least-recently-used) | the line untouched for the longest | exact ordering per set | our project |
| **Pseudo-LRU** | an *approximation* of LRU (tree/bit) | cheap bits | real L2/L3 caches |
| **FIFO** | the oldest-inserted line | a counter | simple HW |
| **Random** | a random way | trivial | some HW |

Our spec uses **true LRU**: each set maintains an exact recency order over its 8 ways. On a hit or a fill, the touched line becomes **most-recently-used (MRU)**; the victim is always the **least-recently-used** line.

```text
set recency (MRU → LRU):  [w3] [w0] [w6] [w1] [w5] [w2] [w7] [w4]
                            ▲                                   ▲
                          just used                       next to be evicted
```

> ⚠️ Real hardware uses *pseudo*-LRU because exact LRU is expensive in silicon. For the contest you must simulate **exact** LRU — pseudo-LRU produces different evictions and therefore different counters, which fails the correctness gate. (Pseudo-LRU is a fine thing to *try* locally to see the divergence — see the project stretch goals.)

The trap that costs everyone a few counters: **LRU updates on demand accesses (hits and fills) only — never on a writeback.** We'll see why in §6.

---

## 5. Hit, Miss, and the Multi-Level Walk

A real CPU has L1 → L2 → L3 → DRAM. Our project models two levels. The walk:

```text
access ──► probe L1
              ├─ hit  → done (fast)
              └─ miss → probe L2
                          ├─ hit  → bring line up, done
                          └─ miss → fetch from memory, install, done
```

Each level you descend costs more (Week-1 numbers: L1 ~1 ns, L2 ~3 ns, DRAM ~80 ns). The whole point of the hierarchy is that *most* accesses hit L1, a few fall to L2, and very few reach memory. Your simulator counts exactly how the trace distributes across these outcomes: `l1_hits`, `l1_misses`, `l2_hits`, `l2_misses`.

> 📌 In our spec, **L2 is consulted only on an L1 miss**, so `l2_hits + l2_misses == l1_misses`. That identity is your first correctness check (`CACHE_SPEC.md` §7).

---

## 6. Write Policies: What Happens on a Store

Reads are easy; writes are where caches get opinionated. Two independent choices:

### Write-hit policy — write-through vs. write-back

- **Write-through:** every store writes the cache *and* the level below immediately. Simple, but floods the lower level with traffic.
- **Write-back:** a store writes only the cache line and sets a **dirty** bit. The line is written down only when it's *evicted*. Far less traffic. **Our project is write-back.**

The dirty bit is the bookkeeping that makes write-back work: a clean line can be dropped for free (a copy exists below); a **dirty** line must be written back before its slot is reused.

### Write-miss policy — write-allocate vs. write-no-allocate

- **Write-allocate:** a store that misses first *fetches* the line into the cache (like a read miss), then writes it (now dirty). **Our project is write-allocate at both levels.**
- **Write-no-allocate:** a store that misses writes straight to the level below and does not pull the line in.

So in our model, a write miss does the *same fill* as a read miss, then marks the freshly installed line dirty. Skipping that fill (write-no-allocate) is a classic correctness bug.

### Where `dirty_writebacks` comes from

When a **dirty** line is evicted from **L2** toward main memory, that's a real write to DRAM — the spec counts it in `dirty_writebacks`. An L1 dirty eviction goes *down to L2* (still on-chip), so it does **not** count as a `dirty_writeback` — it just updates L2's copy (or allocates it). And crucially, that L1→L2 writeback is **not a demand access**: it must not touch `l2_hits`/`l2_misses` and must not reorder L2's LRU. Getting this boundary right is the difference between "correct" and "off by a few thousand". `CACHE_SPEC.md` §5.5 spells out every case.

---

## 7. Inclusion: Can a Line Be in Both Levels?

When a line is in L1, must it also be in L2? Three policies:

- **Inclusive:** L2 always contains everything in L1. Evicting from L2 forces eviction from L1 (back-invalidation). Simplifies coherence; wastes capacity.
- **Exclusive:** a line is in at most one level. Maximizes effective capacity; complex transfers.
- **Non-inclusive, non-exclusive (NINE):** no guarantee either way. Simplest to reason about per-access without back-invalidation. **Our project is NINE.**

NINE is why `CACHE_SPEC.md` §5.5 has two cases for an L1 dirty writeback: the line might still be in L2 (mark it dirty) or might have already left (re-install it dirty). You don't have to *like* the policy — you have to simulate exactly this one.

---

## 8. Putting It Together — One Access, End to End

A write to a block that's not cached anywhere, into a full L1 set whose LRU victim is dirty:

```text
1. b = addr >> 6 ; it's a write → writes++
2. probe L1[set] for tag → MISS → l1_misses++
3. probe L2[set2] for tag → MISS → l2_misses++ ; install CLEAN line in L2 (maybe evict an L2 victim; if that victim was dirty → dirty_writebacks++)
4. fill the line into L1:
     L1[set] is full → evict its LRU way
       that victim is dirty → write it back DOWN to L2
         (L2 hit on the victim? mark dirty. L2 miss? install it dirty — maybe evict another dirty L2 line → dirty_writebacks++)
     install the new line in L1, mark it dirty (it's a write), make it MRU
```

Every clause in that walk maps to a counter or an LRU update. Trace it against `data/tiny.trace` by hand (`python3 data/gen_trace.py --dump data/tiny.trace`) until it's automatic — *then* write the code.

---

## 9. A Worked Address Decomposition

Abstract fields click once you decode a real address. Take `address = 0x1CAFE3` (decimal 1 880 035) and run it through both levels of our geometry.

```text
address      = 0x001CAFE3  = 0b 0001 1100 1010 1111 1110 0011
offset       = address & 63        = 0b100011        = 0x23  (35)   ← byte in the line, ignored
block addr b = address >> 6        = 0x72BF                          ← the unit caches track

L1 (64 sets → 6 index bits):
  L1 index   = b & 63              = 0x72BF & 0x3F   = 0x3F  (set 63)
  L1 tag     = b >> 6              = 0x72BF >> 6     = 0x1CA (458)

L2 (512 sets → 9 index bits):
  L2 index   = b & 511             = 0x72BF & 0x1FF  = 0x0BF (set 191)
  L2 tag     = b >> 9              = 0x72BF >> 9     = 0x39  (57)
```

Two takeaways fall out of the arithmetic:

1. **The same block lands in different sets at each level** (L1 set 63, L2 set 191) because the levels have different set counts. Your two `Level`s are indexed independently — never reuse the L1 index for L2.
2. **The tag is "the block address minus the index bits".** You may store the *whole* block address `b` as the tag and compare on that — the index bits are identical for every line in a set, so they don't affect the comparison. It costs a few extra bits per entry but removes a `>>` from the hot path. Either choice is correct; just be consistent so `CACHE_SPEC.md`'s counters still match.

> 📌 A neat consequence of write-back + 64-byte lines: the byte offset (`& 63`) **never** influences hits, misses, or counters. Two stores to `0x1CAFE0` and `0x1CAFE3` touch the same line and the same way — the second is always a hit on the first's fill. If your counters disagree, you're probably forgetting to shift off the offset.

---

## 10. The Three C's: Why a Miss Happened

Every miss is one of three kinds (Mark Hill's classic taxonomy). You don't report them separately, but the vocabulary explains *why* your `l1_misses` is the number it is — and where the speed ceiling comes from.

| Kind | Cause | Would more associativity help? | Would more capacity help? |
|---|---|---|---|
| **Compulsory** (cold) | First-ever touch of a line — nothing could've cached it | No | No |
| **Capacity** | Working set bigger than the cache | No | Yes |
| **Conflict** | Too many hot lines map to one set | Yes | Sometimes |

Our geometry is *fixed*, so you can't reduce the miss *count* — that's the whole point, the counters are a correctness gate, not a tuning knob. But understanding the mix tells you about the **trace's** behaviour, which shapes your hot path: a trace dominated by compulsory misses (a long sequential scan) spends most accesses in the fill/writeback path, so miss-handling speed matters more; a trace with high L1-hit rate (tight loops, pointer chases over a small set) spends most time in the scan+LRU path, so the "optimize the common hit path" advice from the cost model ([`01-hot-and-cold-paths.md`](./01-hot-and-cold-paths.md) §8) dominates. The generator ([`data/gen_trace.py`](./project/data/gen_trace.py)) deliberately mixes sequential, strided, and random regions so no single regime wins — which is exactly what keeps the leaderboard from collapsing to one trick.

---

## 🎯 Key Takeaways

- A cache is buckets (**sets**) × depth (**ways**) + a **replacement policy**. An address splits into **offset / index / tag**.
- Offset = low 6 bits (64-byte line); index picks the set (`b & (SETS-1)`); tag confirms the line.
- **Set-associative** beats direct-mapped by giving each line `W` candidate slots — at the cost of a `W`-way tag compare per probe (your hot loop).
- **True LRU**: MRU on demand hit/fill, evict the LRU way. Pseudo-LRU is cheaper but changes the counters — not allowed for the contest.
- **Write-back + write-allocate**: stores set a dirty bit; write misses fetch-then-write; dirty victims are written back on eviction.
- `dirty_writebacks` counts only dirty lines leaving **L2** for memory; L1→L2 writebacks are on-chip and are **not** demand accesses (no LRU touch, no hit/miss count).
- **NINE inclusion**: a line may be in L1, L2, both, or neither — which is why writebacks have a "hit L2 / miss L2" split.
- **Decode a real address** to internalize the fields: offset = `& 63` (ignored), index = `& (SETS-1)`, tag = the rest. The same block maps to *different* sets at L1 vs. L2.
- The **three C's** (compulsory/capacity/conflict) explain the miss *count* you can't change; the mix tells you whether the hit path or the miss path dominates your time.

---

## 📚 Further Reading — Caches From the Inside

- 📖 **Ulrich Drepper — ["What Every Programmer Should Know About Memory"](https://people.freebsd.org/~lstewart/articles/cpumemory.pdf)** §3 (CPU caches) — sets, ways, associativity, replacement, the canonical deep-dive.
- 📖 Hennessy & Patterson — *Computer Architecture: A Quantitative Approach*, Appendix B (memory hierarchy) — the textbook treatment of the exact model you're simulating.
- 🎬 [Scott Meyers — "CPU Caches and Why You Care"](https://www.youtube.com/watch?v=WDIkqP4JbkE) — intuition for why associativity and lines exist.
- 🌐 [IIT Delhi COL216 (Computer Architecture)](https://www.cse.iitd.ac.in/~rijurekha/col216.html) — if you've taken it, the cache lab is the closest cousin to this project; if you haven't, the lecture notes on set-associative caches are the right level.
- 🌐 [An interactive cache simulator (CacheLab-style visualizers)](https://courses.cs.washington.edu/courses/cse351/cachesim/) — poke a set-associative LRU cache and watch hits/misses/evictions.

---

## ▶️ Next

[`03-locality.md`](./03-locality.md) — you understand the cache; now we make sure *your simulator's own data* lives in it. Spatial vs. temporal locality, AoS vs. SoA, and the layout choice that's worth a 10× on this project.
