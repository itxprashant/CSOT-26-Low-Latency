// ============================================================================
//  cache_sim.hpp — CSoT'26 Low Latency Track, Week 2
//
//  THIS IS A FROZEN ABI.
//  ----------------------
//  The Week-2 judge builds your single cache_sim.cpp against this header and a
//  judge-owned main(). It constructs your simulator via create_cache_sim() and
//  times exactly the run() call below. If you change any signature or struct
//  layout, your submission will fail to build or fail correctness.
//
//  You may:
//    * subclass CacheSim however you like
//    * add any private state, helpers, SoA arrays, SIMD, prefetch hints
//    * allocate everything you need in on_init()
//    * implement run() with any internal data layout you want
//
//  You may NOT change:
//    * the layout of MemAccess or CacheStats
//    * the signature of CacheSim's virtual functions
//    * the create_cache_sim() factory entry point at the bottom
//
//  Copy this file verbatim into your project's include/ directory.
//
//  IMPORTANT:
//  ----------
//  This header is only the runtime ABI. The cache hierarchy your run() must
//  simulate (geometry, replacement, write policy, the exact counter
//  semantics) is defined in ../CACHE_SPEC.md. The competition ranks the
//  fastest CORRECT implementation of that spec — not a cleverer cache design.
// ============================================================================

#pragma once

#include <cstddef>
#include <cstdint>

namespace csot {

// ---------------------------------------------------------------------------
// MemAccess — one memory reference in the trace.
//
// The judge mmaps the trace file directly into an array of MemAccess, so the
// on-disk record layout IS this struct: 8-byte little-endian address, then a
// 1-byte read/write flag, then 7 reserved bytes that are always zero. Treat
// `_reserved` as off-limits; it exists only to make the layout a clean 16
// bytes and to keep the file format and the in-memory format identical.
// ---------------------------------------------------------------------------
struct MemAccess {
    std::uint64_t address;       // byte address of the access
    std::uint8_t  is_write;      // 0 = read (load), 1 = write (store)
    std::uint8_t  _reserved[7];  // always zero — do not use
};
static_assert(sizeof(MemAccess) == 16, "MemAccess layout is part of the ABI; do not change.");
static_assert(alignof(MemAccess) == 8, "MemAccess alignment is part of the ABI; do not change.");

// ---------------------------------------------------------------------------
// CacheStats — the result of simulating a whole trace.
//
// These seven counters are what the judge diffs against the reference
// simulator. Every field must match EXACTLY for the submission to be correct.
// Their precise meaning (when each is incremented, what counts as a writeback)
// is pinned in CACHE_SPEC.md — read it before you trust your intuition.
// ---------------------------------------------------------------------------
struct CacheStats {
    std::uint64_t reads;             // accesses with is_write == 0
    std::uint64_t writes;            // accesses with is_write == 1
    std::uint64_t l1_hits;           // accesses that hit in L1
    std::uint64_t l1_misses;         // accesses that missed in L1
    std::uint64_t l2_hits;           // L1 misses that hit in L2
    std::uint64_t l2_misses;         // L1 misses that also missed in L2
    std::uint64_t dirty_writebacks;  // dirty lines evicted from L2 to main memory
};
static_assert(sizeof(CacheStats) == 56, "CacheStats layout is part of the ABI; do not change.");

// ---------------------------------------------------------------------------
// CacheSim — the abstract base class. Implement these two methods.
//
//   on_init : called once before run(), after construction. Allocate every
//             buffer you will ever need here (tag arrays, LRU state, …). After
//             this returns, the hot path begins; perform NO heap allocations
//             inside run().
//
//   run     : the hot path. Given the whole trace as a contiguous array of
//             `n` MemAccess records, simulate the CACHE_SPEC.md hierarchy over
//             all of them and return the final CacheStats. The judge measures
//             the wall-clock latency of this single call and ranks correct
//             submissions by how fast it is (and by throughput). You own the
//             inner loop — vectorize it, prefetch it, lay it out however you
//             like, as long as the returned stats are bit-for-bit correct.
// ---------------------------------------------------------------------------
class CacheSim {
public:
    virtual ~CacheSim() = default;

    virtual void on_init() {}

    virtual CacheStats run(const MemAccess* accesses, std::size_t n) = 0;
};

}  // namespace csot

// ---------------------------------------------------------------------------
// Factory entry point.
//
// Every submission MUST export this symbol with C linkage. The judge does the
// moral equivalent of:
//
//   csot::CacheSim* sim = create_cache_sim();
//   sim->on_init();
//   csot::CacheStats got = sim->run(trace, n);   // <-- this call is timed
//
// The returned object is `delete`d by the harness at shutdown.
// ---------------------------------------------------------------------------
extern "C" csot::CacheSim* create_cache_sim();
