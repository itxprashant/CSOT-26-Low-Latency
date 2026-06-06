# 03 — Locality: Make Your Simulator's Own Data Fit Its Own Cache

> **TL;DR** — Your cache simulator is itself a program running on a real CPU with a real cache. If your tag/valid/dirty/LRU state is scattered across DRAM, *your* hot loop misses *your* L1 on every access and you lose 10×. Lay the state out as flat, contiguous **struct-of-arrays** indexed by set, touch it sequentially, and the hardware does the rest.

We just saw the cache from the inside ([`02-cache-internals.md`](./02-cache-internals.md)). The delicious irony of Week 2: the fastest cache *simulator* is the one whose *own* memory accesses are cache-friendly. This file is about your data layout, not the simulated one.

---

## 1. The Two Localities, Restated

From Week 1 ([`week-1/03-memory-hierarchy.md`](../week-1/03-memory-hierarchy.md)):

- **Spatial locality** — if you touch address `X`, you'll soon touch addresses near `X`. The hardware bets on this by fetching whole 64-byte lines and **prefetching** the next ones.
- **Temporal locality** — if you touch `X`, you'll soon touch `X` again. The cache bets on this by *keeping* recently-used lines.

Your job is to make both bets pay off for your simulator's state. A `run()` that walks its set arrays in order has spatial locality; one that re-probes the same hot sets has temporal locality. A `run()` built on `std::unordered_map<set, Node*>` has *neither* — every probe chases a pointer to a random heap node.

---

## 2. The 10× Trap: `unordered_map` Per Set

The "obvious" first simulator keeps a map or a vector-of-vectors:

```cpp
// SLOW: node-based, pointer-chasing, allocates, terrible locality
std::unordered_map<std::uint64_t, std::list<Line>> l1_;   // set → LRU list of lines
```

Every probe hashes, follows a bucket pointer to a heap-allocated node, then walks a linked list whose nodes are scattered across DRAM. Each hop is a likely cache miss (~80 ns). You built a fast cache model on top of an architecture that is itself missing the cache constantly. Measured, this is routinely **10–30× slower** than the flat version below — and it's the single most common Week-2 mistake.

---

## 3. Struct-of-Arrays, Indexed by Set

The fast layout: flat, contiguous arrays sized once in `on_init`, indexed by arithmetic, never pointer-chased.

```cpp
// FAST: flat SoA. One contiguous block per field; index = set * WAYS + way.
struct Level {
    static constexpr std::size_t WAYS = 8;
    std::size_t sets;
    std::vector<std::uint64_t> tag;    // [sets * WAYS]
    std::vector<std::uint8_t>  valid;  // [sets * WAYS]
    std::vector<std::uint8_t>  dirty;  // [sets * WAYS]
    // LRU recency packed per set (e.g. 8 nibbles in a u32, or a small order array)
    std::vector<std::uint32_t> lru;    // [sets]

    void init(std::size_t s) {
        sets = s;
        tag.assign(s * WAYS, 0);
        valid.assign(s * WAYS, 0);
        dirty.assign(s * WAYS, 0);
        lru.assign(s, 0x76543210u);    // identity recency order, packed nibbles
    }
};
```

Now a probe is pure arithmetic on contiguous memory:

```cpp
const std::size_t base = set * Level::WAYS;       // start of this set's 8 ways
for (std::size_t w = 0; w < 8; ++w) {
    if (l1.valid[base + w] && l1.tag[base + w] == t1) { /* hit */ }
}
```

The 8 tags of a set are 8 × 8 = 64 bytes — **exactly one cache line**. One miss pulls in the whole set; the 8-way scan then runs entirely in registers/L1. That's spatial locality working *for* you.

### Should valid/dirty be separate arrays or packed in?

Both work. Separate byte arrays (above) are simple and let you SIMD-scan tags without touching valid/dirty. Packing valid+dirty into a bitset per set saves memory and can be faster if memory bandwidth is your bottleneck. Measure both — it's a great stretch experiment.

---

## 4. AoS vs. SoA for the Tag Scan

Array-of-structs is the intuitive layout:

```cpp
struct Line { std::uint64_t tag; std::uint8_t valid, dirty; };   // ~16 B padded
std::vector<Line> ways;   // AoS: [tag,v,d][tag,v,d]...
```

The tag scan now strides over 16-byte structs, pulling `valid`/`dirty` into cache even though the scan only needs `tag`. Struct-of-arrays keeps all 8 tags adjacent (64 bytes, one line) so the scan reads *only* tags and a single line covers the whole set. For a hot 8-way compare, SoA both saves bandwidth and sets up SIMD ([`06-bonus-simd-and-prefetch.md`](./06-bonus-simd-and-prefetch.md)).

This is the exact AoS-vs-SoA lesson from Week 1 ([`week-1/03-memory-hierarchy.md`](../week-1/03-memory-hierarchy.md) §4) — now applied to the thing *you* control.

---

## 5. The Working-Set Question

How big is your simulator's hot state? L1 model = 64 sets × 8 ways × (8 B tag + 2 B flags) ≈ 5 KiB. L2 model = 512 × 8 × 10 B ≈ 40 KiB. Plus LRU arrays. Total well under 64 KiB — it fits comfortably in your CPU's real **L1/L2**. That's the goal: keep the simulator's entire state resident so the only DRAM traffic is *streaming the trace in*.

The trace itself is huge (hundreds of MB) and read exactly once, sequentially — perfect for the hardware prefetcher. So the ideal `run()` looks like: stream the trace (prefetched, ~free) while keeping all cache state in real L1/L2 (resident, ~free). Anything that breaks either property is your bottleneck.

---

## 6. A Measured Experiment — Layout vs. Throughput

You can feel the difference without the judge. Build the *same* correct simulator two ways and compare throughput on a large trace:

```bash
python3 data/gen_trace.py --accesses 20000000 --seed 42 --out data/large.trace

# Build A: your unordered_map / list version  → cache_sim_map.cpp
# Build B: your flat SoA version              → cache_sim.cpp
cmake -B build -DCSOT_CACHE_SIM_SRC=cache_sim.cpp && cmake --build build -j
./build/cache_sim_runner data/large.trace          # note "throughput = X M acc/s" on stderr
```

Then confirm *where* the time goes with `perf` (Week 1 [`week-1/04-benchmarking-tools.md`](../week-1/04-benchmarking-tools.md)):

```bash
perf stat -e cycles,instructions,L1-dcache-load-misses,LLC-load-misses \
    ./build/cache_sim_runner data/large.trace
```

The node-based build shows a high `L1-dcache-load-misses` and `LLC-load-misses` rate — *your* loads missing *your* cache. The flat build's misses collapse and IPC roughly doubles. Same algorithm, same counters, very different machine behaviour. **This experiment is the heart of the week** — run it before you optimize anything else.

---

## 7. Prefetching the Next Access (a teaser)

Even with perfect layout, there's one stall left: the set you're about to touch for access `i+1` may not be in your L1 yet. Because the trace is an array, you *know* the next address now. You can ask the hardware to start fetching its set:

```cpp
const std::size_t next_set = (acc[i + 8].address >> 6) & index_mask;
__builtin_prefetch(&l1.tag[next_set * 8]);     // start the fetch ~8 iters ahead
```

Often the hardware prefetcher already nailed the sequential trace read, so this does little for the *trace* — but it can help hide the latency of touching a *random* simulated set. Like all prefetch: measure, don't cargo-cult. We go deeper in [`06-bonus-simd-and-prefetch.md`](./06-bonus-simd-and-prefetch.md).

---

## 8. Align the Set So One Probe Touches One Line

A set's 8 tags are 64 bytes — exactly one cache line — but only if they *start* on a line boundary. If your tag array begins at an address that isn't a multiple of 64, a given set's 8 tags **straddle two lines**, and every probe of that set pays two cache-line fetches instead of one. Half your sets, silently doubled.

`std::vector`'s buffer is only guaranteed to be aligned for its element type (8 bytes for `uint64_t`), not for a cache line. Force 64-byte alignment so set `k`'s tags live in exactly one line:

```cpp
// Each set's 8 tags occupy one aligned 64-byte line; set k starts at a line boundary.
struct alignas(64) TagSet { std::uint64_t tag[8]; };
std::vector<TagSet> l1_tags_;   // l1_tags_[set].tag[way]  — one line per set

// or, for a flat array, align the allocation itself:
alignas(64) std::array<std::uint64_t, L1_SETS * 8> l1_tag_{};
```

Because `8 ways × 8 bytes == 64`, consecutive sets then tile the cache perfectly: set 0 is line 0, set 1 is line 1, no straddling anywhere. (This same `alignas(64)` shows up again in Week 3 — there it stops two *threads* from fighting over a shared line. Same number, different enemy.)

> ⚠️ Don't over-align the *small* fields. If you `alignas(64)` each individual `valid`/`dirty` byte you waste 63 bytes per entry and blow up your working set. Align the thing you scan as a unit (the 8 tags); keep the flag arrays dense.

---

## 9. The Trace Is a One-Shot Stream — Treat It Like One

Your state is tiny and reused; the **trace is huge and read exactly once**, front to back. Those are opposite access patterns and you optimize them differently:

- **State** wants to stay resident (temporal locality) — that's §3–§8.
- **The trace** wants to stream in cheaply and *not evict your state* on the way through.

The judge owns trace I/O and hands `run()` a `const MemAccess*` already in memory, so you can't change how it's loaded — but you can avoid fighting it. A clean front-to-back walk (`for i in 0..n`) is the ideal input for the hardware prefetcher; it will have `acc[i+8]` on its way before you ask. Three things *not* to do:

- **Don't re-read the trace.** A second pass doubles your DRAM traffic for zero benefit — the counters need one in-order pass.
- **Don't copy it.** It's hundreds of MB; copying evicts your resident state and adds a full streaming pass.
- **Don't index it randomly.** Out-of-order access defeats the prefetcher and turns ~free loads into DRAM stalls.

The mental model for the perfect `run()`: a thin 16-byte record streams in (prefetched, ~free), you do a handful of register/L1 operations against resident state, and you move on. The only unavoidable DRAM traffic is the trace itself going past once. Everything in §3–§8 exists to make sure your *state* never adds to that traffic.

> 💡 Curious how much the streaming costs on its own? A trace of `n` accesses is `16·n` bytes; at ~20 GB/s of effective bandwidth, just *reading* a 100M-access (1.6 GB) trace is ~80 ms — a floor no amount of cleverness beats. Knowing that floor tells you when you're done optimizing: if `run()` is near the streaming bound, the bottleneck is memory bandwidth, not your code.

---

## 10. Hands-on experiment — BFS: pointer graph vs. CSR

§6 measures layout on the cache sim. Here you see the **same idea on a graph**: one BFS algorithm, two ways of storing neighbors. Not judge work — Phase 2 practice, like Week 1's `vector` vs. `list` demo.

Want a fuller walkthrough (starter layout, Cachegrind prompts, report questions)? See the [*cache_graph* assignment](https://github.com/rijurekha/cache_graph) — same task, more hand-holding. What follows is the lean, build-it-yourself version.

### Why

BFS is O(V + E) either way. On a real CPU the **slow part is memory**: following `next` pointers hops to random cache lines (~100 ns each); scanning a flat `col_idx[]` slice has spatial locality (~1 ns per hit). Same algorithm, different layout — the gap is often **2–8×** on random graphs. That's the §2 `unordered_map` trap in graph form.

### The two layouts

**Pointer graph (adjacency list).** An array of vertex structs. Each vertex holds its **number** (id) and a **head pointer** to a singly linked list of outgoing edges — each list node is a heap-allocated struct with a `next` pointer (and the neighbor id, or a pointer to the destination vertex). Walking one vertex's neighbors means following that chain of pointers through DRAM; every edge is its own allocation, scattered wherever `malloc` lands. Flexible; terrible spatial locality.

```text
Vertex[0] ──► Edge(1) ──► Edge(2) ──► null
Vertex[1] ──► Edge(2) ──► null
Vertex[2] ──► null
```

**CSR (Compressed Sparse Row).** The same graph as **two contiguous `int` arrays** — no per-edge pointers:

- `row_ptr[v]` … `row_ptr[v+1]` — slice in `col_idx` holding all out-neighbors of `v`
- `col_idx` — all neighbor ids packed back-to-back

```text
row_ptr: [0,  2,  3,  3]      ← starts of each vertex's slice (+ sentinel at end)
col_idx: [1,  2,  2]          ← 0→{1,2}, 1→{2}, 2→{}
```

Neighbor scan is a tight integer loop over a dense slice — sequential reads, one cache line pulls in several neighbors. Conversion from pointer → CSR is **cold-path** work (like building flat SoA in `on_init`); the win shows up in the hot BFS traversal.

### What you build (C++20)

You write **everything** except the graph generator. Your call on file names, build system, and CLI — just deliver the pieces below.

| Piece | Minimum requirement |
|---|---|
| **Loader** | Read the graph format below into a **pointer adjacency list** (one `malloc`'d node per edge, linked per vertex — deliberately cache-unfriendly) |
| **`bfs_pointer`** | Standard BFS; `dist[v] = -1` if unreachable; return visited count |
| **`convert_to_csr`** | Build `row_ptr[n+1]` + `col_idx[m]` from the pointer graph |
| **`bfs_csr`** | Same BFS logic; neighbors via `col_idx[row_ptr[v] … row_ptr[v+1])` |
| **Driver** | Run either impl on a file; support `--repeat=N` for timing |
| **Sweep + plots** | Your script or shell loop: vary **n**, **graph kind**, **impl**, **repeat**; plot time and cache misses |

> 📌 **Only shipped file:** `project/data/gen_graph.py` — generates test graphs (stdlib only). Everything else is yours.

### Graph format (`gen_graph.py` output)

```text
n
deg v1 v2 ... v_deg
...
```

Directed out-edges. Example: `3` vertices, `0→{1,2}`, `1→{2}`, `2→{}`:

```text
3
2 1 2
1 2
0
```

Generate graphs:

```bash
python3 project/data/gen_graph.py --kind er   --n 50000 --deg 8 --seed 42 --out /tmp/g.graph
python3 project/data/gen_graph.py --kind grid --rows 224 --cols 224        --out /tmp/g.graph
python3 project/data/gen_graph.py --kind chain --n 100000                --out /tmp/g.graph
python3 project/data/gen_graph.py --kind star  --n 50000                   --out /tmp/g.graph
```

Kinds: `er` (random), `grid`, `chain`, `star`. For `grid`, choose `rows × cols` to match each target `n`.

`convert_to_csr`: two-pass build — prefix-sum degrees into `row_ptr`, then scatter each vertex's neighbors into `col_idx`.

### Measuring it

Once both BFS implementations return the same `dist[]` on a few small graphs, sweep — same algorithm, same inputs, two layouts. You write the driver, the loop, the CSV, and the plots; `gen_graph.py` only makes the inputs.

**What to vary.** Run `pointer` and `csr` on all four **kinds** (`er`, `grid`, `chain`, `star`) at sizes **n = 1000, 2000, …, 50000** (step 1000). Fix `source = 0`; for `er` graphs use `deg = 8`, `seed = 42`. For `grid`, pick `rows × cols = n`. At each point, also try **`repeat ∈ {1, 10, 50}`** — three ways to ask "how long does one BFS take?" The full grid is ~1 200 runs.

**What not to time.** Same rule as the cache sim's `run()`: inside the timer, only `bfs_*` × `repeat`, with `dist[]` reset each call. Outside: `load_graph`, `convert_to_csr`, and allocating `dist[]`. Conversion is cold-path work — like building flat SoA in `on_init`. The plots compare steady-state traversal.

**Wall-clock vs. cache counters need different repeats.** Timer noise drowns a single BFS; use `--repeat=50` (or `10`) for time and divide total µs by `repeat`. `perf` and Cachegrind inject their own overhead — use `--repeat=1` when reading counters. On native Linux, one sweep point looks like:

```bash
python3 project/data/gen_graph.py --kind er --n 10000 --deg 8 --seed 42 --out /tmp/er_10k.graph

perf stat -e L1-dcache-load-misses,L1-dcache-loads,LLC-load-misses \
    ./your_bench --impl=pointer --graph=/tmp/er_10k.graph --source=0 --repeat=1
perf stat -e L1-dcache-load-misses,L1-dcache-loads,LLC-load-misses \
    ./your_bench --impl=csr --graph=/tmp/er_10k.graph --source=0 --repeat=1

./your_bench --impl=pointer --graph=/tmp/er_10k.graph --source=0 --repeat=50
./your_bench --impl=csr     --graph=/tmp/er_10k.graph --source=0 --repeat=50
```

Log each run to a CSV — `impl`, `kind`, `n`, `repeat`, `time_us`, `l1_misses`, and `l1_hits` (hits = `loads − misses`). The pointer build should show more L1 misses per BFS on `er`/`star` graphs; CSR's miss rate should stay flatter as `n` grows. No native `perf` (WSL2 / macOS)? Still plot time; pull miss counts from Cachegrind on a subset.

**The plots: size on x, one metric on y, each line a situation.** Every figure shares **graph size `n` on the x-axis**. The y-axis is either **time per BFS** or a **cache hit/miss** count. A *situation* is a fixed `(impl, kind, repeat)` — draw the situations that belong together on the **same axes** so the gap is visible:

| Plot | Y-axis | Lines on each chart | `repeat` |
|---|---|---|---|
| Time vs. size | µs per BFS | `pointer` and `csr` | `50` |
| Misses vs. size | L1 misses per BFS | `pointer` and `csr` | `1` |
| Hits vs. size | L1 hits per BFS | `pointer` and `csr` | `1` |

Easiest layout: **four panels, one per `kind`**, two lines per panel. On the `er` time chart at `n = 20 000`, are you seeing 2–8× between layouts? On the hits chart, does the gap widen with `n`? On `chain`, do the lines nearly touch; on `star`, snap apart? That's §1's spatial locality, drawn.

> 💡 **Repeat is another situation axis.** On the time-vs-size charts you can add lines for `repeat = 1, 10, 50` (six lines per `kind` panel if you show all combos) and watch the high-repeat ones smooth out. Or fix `kind` and `n`, put **`repeat` on the x-axis**, and plot time there once — timer noise dying off, plain as day. Hit *rate* `(loads − misses) / loads` and speedup `time_pointer / time_csr` vs. `n` are also worth a line each.

> ⭐ **Bonus — shrink the cache in software.** Cachegrind simulates L1/LLC geometry without new hardware. Re-run a few graphs at `repeat = 1` while changing `--D1=bytes,assoc,line`:

```bash
valgrind --tool=cachegrind --D1=16384,4,64  ./your_bench --impl=pointer --graph=/tmp/er_10k.graph --repeat=1
valgrind --tool=cachegrind --D1=32768,8,64  ./your_bench --impl=csr     --graph=/tmp/er_10k.graph --repeat=1
```

Double the L1 size, then the associativity, then the line size — plot misses vs. size under each fake cache. Pointer graphs usually bleed misses first when the cache shrinks; CSR is the layout that survives.

### Sanity-check your results

- CSR faster on `er`/`star`, modest gap on `chain`/`grid` → plausible
- CSR slower → bug, or you timed conversion too
- Both give identical `dist[]` on the same graph

### Reflect (short notes to yourself)

1. Where does pointer BFS chase memory vs. where does CSR stream?
2. Which graph `kind` has the smallest CSR win? Why?
3. One sentence tying `col_idx` to §3's flat `tag[]`, and the linked list to §2's map trap.

~3 hours for the full sweep + four plots. Stretch: add `vector<vector<int>>` as a third layout.

---

## 🎯 Key Takeaways

- Your simulator runs on a real cache; **its own data layout decides its speed**. Scattered state = constant real cache misses = 10× slower.
- **`unordered_map`/`list` per set is the trap** — pointer-chasing, allocating, no locality. The most common Week-2 mistake.
- **Flat struct-of-arrays indexed by `set * WAYS + way`** turns every probe into arithmetic over contiguous memory.
- 8 tags = 64 bytes = **one cache line**; one miss pulls in the whole set, then the scan runs in L1.
- **SoA beats AoS** for the tag scan: reads only tags, packs them into a line, enables SIMD.
- The whole cache-model state fits in real L1/L2 — keep it resident; the only necessary DRAM traffic is streaming the trace.
- **`alignas(64)`** the unit you scan (a set's 8 tags) so one probe touches one line, never two; don't over-align the small flag arrays.
- The **trace is a one-shot stream** — walk it once, front to back, never copy or re-read it; the streaming bandwidth is your hard speed floor.
- Prove it with `perf`: watch `L1-dcache-load-misses` and IPC swing between the node-based and flat builds.
- §10: same layout lesson on graphs — linked-list adjacency chases pointers; CSR is SoA. Time the hot BFS only.

---

## 📚 Further Reading — Data Layout & Locality

- 🎬 **[Mike Acton — "Data-Oriented Design and C++"](https://www.youtube.com/watch?v=rX0ItVEVjHc)** (CppCon 2014) — the thesis this whole track is built on: think about where data is in memory, not about objects.
- 🎬 [Andrei Alexandrescu — "Speed Is Found in the Minds of People"](https://www.youtube.com/watch?v=FJJTYQYB1JQ) (CppCon 2019) — layout, branch, and the difference between the algorithm and the machine.
- 📰 [Igor Ostrovsky — "Gallery of Processor Cache Effects"](https://igoro.com/archive/gallery-of-processor-cache-effects/) — short demos; #1 and #2 are exactly the SoA/stride lessons.
- 📰 [Data-Oriented Design book (Richard Fabian), free online](https://www.dataorienteddesign.com/dodbook/) — chapter on existential processing / SoA.
- 📰 [cache_graph assignment (Rijurekha)](https://github.com/rijurekha/cache_graph) — original C BFS/CSR exercise §10 adapts; useful if you want the unabridged Cachegrind report prompts.

---

## ▶️ Next

[`04-zero-allocation.md`](./04-zero-allocation.md) — your data is laid out; now we make sure the hot path never calls `malloc`. Object pools, arenas, and sizing everything in `on_init`.
