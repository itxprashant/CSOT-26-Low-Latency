// ============================================================================
//  pipeline.hpp — CSoT'26 Low Latency Track, Week 4
//
//  THIS IS A FROZEN ABI.
//  ----------------------
//  The Week-4 judge builds your single pipeline.cpp against this header and a
//  judge-owned main(). It constructs your pipeline via create_pipeline(),
//  calls on_init(num_symbols) once, then times exactly the run() call below. If
//  you change any signature or struct layout, your submission will fail to
//  build or fail correctness.
//
//  WHAT YOU BUILD
//  --------------
//  A two-stage pipeline inside run():
//
//        WireTick stream ──► [ producer / decode thread ]
//                                       │  your lock-free
//                                       │  SPSC ring buffer
//                                       ▼
//                            [ consumer / strategy thread ]
//                                       │  drives the frozen
//                                       ▼  csot::Strategy (z-score)
//                            OrderRecord stream (out[])
//
//  The producer decodes each 40-byte WireTick into a frozen csot::Tick
//  (fixed-point -> double, symbol interning), pushes it across YOUR lock-free
//  single-producer/single-consumer ring buffer, and the consumer pops ticks in
//  order, drives the Week-1 strategy spec (on_tick + the deterministic fill
//  model), and appends every emitted order to `out`. See ../PIPELINE_SPEC.md.
//
//  You may:
//    * design any lock-free SPSC ring buffer you like (capacity, padding,
//      memory orderings, batching are all yours)
//    * spawn and pin your producer / consumer std::threads inside run()
//    * allocate every buffer you need in on_init()
//    * devirtualize / inline the strategy however you can (single TU)
//
//  You may NOT change:
//    * the layout of WireTick or OrderRecord
//    * the signature of Pipeline's virtual functions
//    * the create_pipeline() factory entry point at the bottom
//    * the csot::Tick / csot::Order / csot::Strategy ABI in strategy.hpp
//
//  Copy this file (and strategy.hpp) verbatim into your project's include/.
//
//  IMPORTANT:
//  ----------
//  This header is only the runtime ABI. WHAT your consumer must compute (the
//  z-score mean-reversion rule, the fill model, the empty-warmup window) is the
//  unchanged Week-1 spec in ../STRATEGY_SPEC.md; HOW the feed is framed on the
//  wire and what order stream you must emit is in ../PIPELINE_SPEC.md. The
//  competition ranks the fastest CORRECT, DETERMINISTIC pipeline — the answer
//  (the order stream) is fixed; only how fast you decode + hand off + strategize
//  is ranked.
// ============================================================================

#pragma once

#include <cstddef>
#include <cstdint>

#include "strategy.hpp"

namespace csot {

// ---------------------------------------------------------------------------
// PRICE_SCALE — fixed-point scale for prices on the wire.
//
// A WireTick carries integer prices: a real price of 100.0500 is stored as
// 1000500 (= 100.05 * 10000). The producer decodes `bid_px_fp / PRICE_SCALE`
// into the floating-point csot::Tick::bid_px the strategy spec consumes, and
// the judge re-encodes each emitted order's price back to this fixed-point
// integer so the order stream diffs as exact integers, never fragile floats.
// ---------------------------------------------------------------------------
inline constexpr std::int64_t PRICE_SCALE = 10000;

// ---------------------------------------------------------------------------
// WireTick — one market tick as it arrives on the feed (the DECODE input).
//
// The judge mmaps the feed file directly into an array of WireTick, so the
// on-disk record layout IS this struct: little-endian, 40 bytes, 8-byte
// aligned. Prices are FIXED-POINT integers (real price * PRICE_SCALE); your
// producer's job is to turn this compact integer record into the frozen
// csot::Tick (doubles + an interned symbol string_view) the strategy expects.
// `_reserved` is off-limits; it pads the record to a clean 40 bytes and keeps
// the file format and the in-memory format identical. See PIPELINE_SPEC.md §2.
// ---------------------------------------------------------------------------
struct WireTick {
    std::uint64_t timestamp_ns;  // exchange wall-clock, ns since epoch (non-decreasing)
    std::int64_t  bid_px_fp;     // FIXED-POINT best bid: real_bid * PRICE_SCALE
    std::int64_t  ask_px_fp;     // FIXED-POINT best ask: real_ask * PRICE_SCALE
    std::uint32_t symbol_id;     // 0 .. num_symbols-1 (intern to "SYM<id>")
    std::uint32_t bid_qty;       // quantity at best bid (> 0)
    std::uint32_t ask_qty;       // quantity at best ask (> 0)
    std::uint32_t _reserved;     // always zero — do not use
};
static_assert(sizeof(WireTick) == 40, "WireTick layout is part of the ABI; do not change.");
static_assert(alignof(WireTick) == 8, "WireTick alignment is part of the ABI; do not change.");

// ---------------------------------------------------------------------------
// OrderRecord — one entry in the order stream your run() produces.
//
// Every order the strategy emits is recorded here, tagged with the index of
// the WireTick that produced it (so the judge can diff your stream against the
// reference tick-by-tick and reconstruct each order's timestamp). Orders MUST
// be appended in tick order (the order in which the consumer processed ticks),
// which for a correct in-order SPSC handoff is automatic. See PIPELINE_SPEC.md
// §6 for the equality rule.
// ---------------------------------------------------------------------------
struct OrderRecord {
    std::uint64_t tick_index;  // index into the WireTick input that produced this order
    csot::Order   order;       // the emitted order (frozen csot::Order layout)
};
static_assert(sizeof(OrderRecord) == 48, "OrderRecord layout is part of the ABI; do not change.");

// ---------------------------------------------------------------------------
// Pipeline — the abstract base class. Implement these two methods.
//
//   on_init : called once before run(), after construction, with the fixed
//             number of distinct symbol ids in the feed. Allocate every buffer
//             you will ever need here (your ring buffer storage, per-symbol
//             strategy state, the interned symbol-name table, thread
//             bookkeeping). After this returns, the hot path begins; perform
//             NO heap allocations inside run().
//
//   run     : the hot path. Given the whole feed as a contiguous array of `n`
//             WireTick records, run your producer/consumer pipeline to decode
//             every tick, drive the Week-1 strategy spec over the decoded
//             stream, and write each emitted order (in tick order) into `out`.
//             Returns the number of OrderRecords written. The judge measures
//             the wall-clock latency of this single call and ranks correct,
//             deterministic submissions by how fast it is (throughput).
//
//             `out` points to caller-owned storage for at least `n`
//             OrderRecords (the spec emits at most one order per tick). You own
//             the threads, the handoff, and the decode; the only thing fixed is
//             that the OrderRecords you write must equal the reference stream.
// ---------------------------------------------------------------------------
class Pipeline {
public:
    virtual ~Pipeline() = default;

    virtual void on_init(std::uint32_t num_symbols) { (void)num_symbols; }

    virtual std::size_t run(const WireTick* in, std::size_t n, OrderRecord* out) = 0;
};

}  // namespace csot

// ---------------------------------------------------------------------------
// Factory entry point.
//
// Every submission MUST export this symbol with C linkage. The judge does the
// moral equivalent of:
//
//   csot::Pipeline* p = create_pipeline();
//   p->on_init(num_symbols);
//   std::size_t num_orders = p->run(in, n, out);   // <-- this call is timed
//
// The returned object is `delete`d by the harness at shutdown.
// ---------------------------------------------------------------------------
extern "C" csot::Pipeline* create_pipeline();
