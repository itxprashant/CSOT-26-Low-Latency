// ============================================================================
//  aggregate.hpp — CSoT'26 Low Latency Track, Week 3
//
//  THIS IS A FROZEN ABI.
//  ----------------------
//  The Week-3 judge builds your single aggregator.cpp against this header and a
//  judge-owned main(). It constructs your aggregator via create_aggregator(),
//  calls on_init(num_symbols) once, then times exactly the run() call below. If
//  you change any signature or struct layout, your submission will fail to
//  build or fail correctness.
//
//  You may:
//    * subclass Aggregator however you like
//    * add any private state, helpers, padded per-thread tables, SIMD, prefetch
//    * spawn and pin your own std::threads inside run()
//    * allocate everything you need in on_init()
//
//  You may NOT change:
//    * the layout of AggTick or SymbolAgg
//    * the signature of Aggregator's virtual functions
//    * the create_aggregator() factory entry point at the bottom
//
//  Copy this file verbatim into your project's include/ directory.
//
//  IMPORTANT:
//  ----------
//  This header is only the runtime ABI. The exact aggregates your run() must
//  compute (which fields, integer semantics, the empty-symbol rule, the
//  determinism guarantee) are defined in ../AGG_SPEC.md. The competition ranks
//  the fastest CORRECT implementation of that spec — the answer is fixed; only
//  how fast you reduce the stream over many cores is ranked.
// ============================================================================

#pragma once

#include <cstddef>
#include <cstdint>

namespace csot {

// ---------------------------------------------------------------------------
// AggTick — one market tick in the stream.
//
// The judge mmaps the dataset file directly into an array of AggTick, so the
// on-disk record layout IS this struct: little-endian, 32 bytes, 8-byte
// aligned. `price` is a FIXED-POINT integer (real price * 10000), so every
// aggregate is exact integer arithmetic — see AGG_SPEC.md §2. Treat
// `_reserved` as off-limits; it pads the record to a clean 32 bytes and keeps
// the file format and the in-memory format identical.
// ---------------------------------------------------------------------------
struct AggTick {
    std::uint64_t timestamp_ns;  // exchange wall-clock, ns since epoch (non-decreasing)
    std::int64_t  price;         // FIXED-POINT price: real_price * 10000
    std::uint32_t symbol_id;     // 0 .. num_symbols-1
    std::uint32_t qty;           // shares at this tick (> 0)
    std::uint64_t _reserved;     // always zero — do not use
};
static_assert(sizeof(AggTick) == 32, "AggTick layout is part of the ABI; do not change.");
static_assert(alignof(AggTick) == 8, "AggTick alignment is part of the ABI; do not change.");

// ---------------------------------------------------------------------------
// SymbolAgg — one row of the result table, one per symbol id.
//
// These five integer counters are what the judge diffs against the reference
// for every symbol. Because they are pure integer add / min / max, the merged
// result is IDENTICAL no matter how you split the stream across threads — that
// is the property that makes the correctness gate fair for a 1-thread or an
// 8-thread submission. Their precise semantics (and the canonical value for a
// symbol that never appears) are pinned in AGG_SPEC.md — read it before you
// trust your intuition.
// ---------------------------------------------------------------------------
struct SymbolAgg {
    std::uint64_t count;      // number of ticks with this symbol_id
    std::int64_t  sum_price;  // sum of price over those ticks
    std::int64_t  min_price;  // min price over those ticks (0 if count == 0)
    std::int64_t  max_price;  // max price over those ticks (0 if count == 0)
    std::uint64_t sum_qty;    // sum of qty over those ticks
};
static_assert(sizeof(SymbolAgg) == 40, "SymbolAgg layout is part of the ABI; do not change.");

// ---------------------------------------------------------------------------
// Aggregator — the abstract base class. Implement these two methods.
//
//   on_init : called once before run(), after construction, with the fixed
//             number of distinct symbol ids in the dataset. Allocate every
//             buffer you will ever need here (per-thread partial tables, the
//             final table, anything). After this returns, the hot path begins;
//             perform NO heap allocations inside run().
//
//   run     : the hot path. Given the whole stream as a contiguous array of
//             `n` AggTick records, compute the per-symbol aggregates of
//             AGG_SPEC.md and write them into `out` — exactly `num_symbols`
//             SymbolAgg rows, indexed by symbol_id. The judge measures the
//             wall-clock latency of this single call and ranks correct
//             submissions by how fast it is (and by throughput). You own the
//             reduction: split it across threads, pin them to cores, pad your
//             partials against false sharing — anything — as long as the
//             `num_symbols` rows you write are bit-for-bit correct.
//
//             `out` points to storage for num_symbols SymbolAgg rows owned by
//             the caller. Its contents on entry are unspecified; you must write
//             every row (use the §3 empty-symbol value for ids that never
//             appear).
// ---------------------------------------------------------------------------
class Aggregator {
public:
    virtual ~Aggregator() = default;

    virtual void on_init(std::uint32_t num_symbols) { (void)num_symbols; }

    virtual void run(const AggTick* ticks, std::size_t n, SymbolAgg* out) = 0;
};

}  // namespace csot

// ---------------------------------------------------------------------------
// Factory entry point.
//
// Every submission MUST export this symbol with C linkage. The judge does the
// moral equivalent of:
//
//   csot::Aggregator* agg = create_aggregator();
//   agg->on_init(num_symbols);
//   agg->run(ticks, n, out);          // <-- this call is timed
//
// The returned object is `delete`d by the harness at shutdown.
// ---------------------------------------------------------------------------
extern "C" csot::Aggregator* create_aggregator();
