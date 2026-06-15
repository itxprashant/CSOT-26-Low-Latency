// ============================================================================
//  aggregator.stub.cpp — STARTING POINT for the Week-3 parallel-aggregator
//  challenge.
//
//  Copy this to `aggregator.cpp`, then make it FAST:
//      cp samples/aggregator.stub.cpp aggregator.cpp
//      cmake -B build -DCSOT_AGG_SRC=aggregator.cpp && cmake --build build -j
//      ./build/agg_runner data/tiny.ticks       # compare to data/tiny.agg.json
//
//  Unlike the Week-2 stub, this one is CORRECT: it is the AGG_SPEC.md §6
//  clarity reference, single-threaded. It passes data/tiny.ticks out of the
//  box. That is on purpose — Week 3's challenge is NOT the (trivial, fully
//  specified) reduction; it is making that reduction scream across four cores
//  without tripping over your own threads.
//
//  YOUR JOB: keep it correct and deterministic, and make run() fast by:
//    1. partitioning the stream across std::threads        (02-stdthread-basics.md)
//    2. giving each thread its OWN partial table on its OWN
//       cache line  (alignas(64) — no false sharing)        (04-false-sharing.md)
//    3. pinning the threads to distinct cores                (05-scheduler-and-pinning.md)
//    4. merging the partials once at the end (AGG_SPEC.md §7)
//    5. allocating every buffer in on_init() — zero heap in run()
//
//  A correct single-threaded entry (this file, unchanged) is a VALID upload;
//  it just ranks near the bottom. The board is won on speed.
//
//  Everything must live in this ONE translation unit. The judge builds exactly
//  this file against its own main(); no extra .cpp, no custom CMake. Threads,
//  <atomic>, and sched_setaffinity ARE allowed this week (they were not in
//  Week 2) — see TROUBLESHOOTING.md.
// ============================================================================

#include "aggregate.hpp"

namespace {

class StubAggregator final : public csot::Aggregator {
    std::uint32_t num_symbols_ = 0;

public:
    void on_init(std::uint32_t num_symbols) override {
        // COLD PATH: allocate everything you will ever need here. For the fast
        // version that means your per-thread partial tables (one padded table
        // per thread) and any thread bookkeeping — never inside run().
        num_symbols_ = num_symbols;
    }

    void run(const csot::AggTick* ticks, std::size_t n,
             csot::SymbolAgg* out) override {
        // Start from canonical empty rows (AGG_SPEC.md §3) so symbols that
        // never appear end up as {0,0,0,0,0}.
        for (std::uint32_t s = 0; s < num_symbols_; ++s) {
            out[s] = csot::SymbolAgg{0, 0, 0, 0, 0};
        }

        // SINGLE-THREADED reference reduction (AGG_SPEC.md §5/§6). Correct but
        // slow. TODO: split [0, n) across threads, accumulate into padded
        // per-thread partials, then merge into `out`.
        for (std::size_t i = 0; i < n; ++i) {
            const csot::AggTick& t = ticks[i];
            csot::SymbolAgg&     r = out[t.symbol_id];
            if (r.count == 0) {
                r.min_price = t.price;
                r.max_price = t.price;
            } else {
                if (t.price < r.min_price) r.min_price = t.price;
                if (t.price > r.max_price) r.max_price = t.price;
            }
            r.count += 1;
            r.sum_price += t.price;
            r.sum_qty += t.qty;
        }
    }
};

}  // namespace

extern "C" csot::Aggregator* create_aggregator() {
    return new StubAggregator();
}
