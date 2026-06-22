// ============================================================================
//  pipeline.stub.cpp — STARTING POINT for the Week-4 lock-free pipeline
//  challenge.
//
//  Copy this to `pipeline.cpp`, then make it FAST:
//      cp samples/pipeline.stub.cpp pipeline.cpp
//      cmake -B build -DCSOT_PIPE_SRC=pipeline.cpp && cmake --build build -j
//      ./build/pipeline_runner data/tiny.feed     # compare to data/tiny.orders.json
//
//  Like the Week-3 stub, this one is CORRECT: it decodes the feed and runs the
//  frozen Week-1 z-score strategy spec (PIPELINE_SPEC.md §4-§6, STRATEGY_SPEC.md
//  §5-§8) SINGLE-THREADED, in stream order. It passes data/tiny.feed out of the
//  box and produces the exact reference order stream. That is on purpose —
//  Week 4's challenge is NOT the (fully specified) strategy; it is overlapping
//  the decode and the strategy across two cores through a lock-free hand-off.
//
//  YOUR JOB: keep it correct and deterministic, and make run() fast by:
//    1. moving the decode onto its OWN producer thread            (05-pipeline-...)
//    2. handing each decoded tick to the strategy thread across a
//       lock-free SPSC RING BUFFER you build                      (04-spsc-ring-buffer.md)
//    3. publishing with release / observing with acquire          (03-memory-orderings.md)
//    4. keeping head and tail on SEPARATE cache lines (no false
//       sharing) and pinning both threads to distinct cores       (Week 3)
//    5. allocating the ring, the per-symbol state, and the interned
//       names in on_init() — zero heap in run()
//
//  A correct single-threaded entry (this file, unchanged) is a VALID upload; it
//  just ranks near the bottom because the decode and the strategy run one after
//  the other instead of at the same time. The board is won on speed.
//
//  Everything must live in this ONE translation unit. The judge builds exactly
//  this file against its own main(); no extra .cpp, no custom CMake. Threads,
//  <atomic>, and sched_setaffinity ARE allowed (this is the whole point) —
//  see TROUBLESHOOTING.md.
// ============================================================================

#include "pipeline.hpp"

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace {

// --- Frozen strategy constants (STRATEGY_SPEC.md §3) ------------------------
constexpr std::uint32_t WINDOW = 64;
constexpr double        ENTRY_Z = 2.0;
constexpr double        EXIT_Z = 0.5;
constexpr double        EPSILON_STDDEV = 1e-9;

// Per-symbol rolling state (STRATEGY_SPEC.md §4). One of these per symbol id.
struct SymbolState {
    double        mids[WINDOW] = {};
    std::uint32_t count = 0;     // valid mids seen so far, capped at WINDOW
    std::uint32_t head = 0;      // next write index into mids[]
    std::int32_t  position = 0;  // -1, 0, or +1
};

class StubPipeline final : public csot::Pipeline {
    std::uint32_t                 num_symbols_ = 0;
    std::vector<std::string>      names_;   // names_[k] == "SYM<k>" (interned once)
    std::vector<SymbolState>      state_;

public:
    void on_init(std::uint32_t num_symbols) override {
        // COLD PATH: allocate everything you will ever need here. For the fast
        // version that means your ring-buffer storage and thread bookkeeping
        // too — never inside run().
        num_symbols_ = num_symbols;
        names_.resize(num_symbols);
        for (std::uint32_t k = 0; k < num_symbols; ++k) {
            names_[k] = "SYM" + std::to_string(k);
        }
        state_.assign(num_symbols, SymbolState{});
    }

    std::size_t run(const csot::WireTick* in, std::size_t n,
                    csot::OrderRecord* out) override {
        std::size_t num_orders = 0;

        // SINGLE-THREADED: decode each tick, then strategize, in stream order.
        // TODO: split this into a producer thread (the decode below) and a
        // consumer thread (the strategy below) connected by your SPSC queue.
        for (std::size_t i = 0; i < n; ++i) {
            const csot::WireTick& w = in[i];

            // ---- Decode (PIPELINE_SPEC.md §4) -------------------------------
            const std::uint32_t sym = w.symbol_id;
            const double bid_px = static_cast<double>(w.bid_px_fp) /
                                  static_cast<double>(csot::PRICE_SCALE);
            const double ask_px = static_cast<double>(w.ask_px_fp) /
                                  static_cast<double>(csot::PRICE_SCALE);

            // ---- Strategy (STRATEGY_SPEC.md §5-§7) --------------------------
            SymbolState& st = state_[sym];

            const double mid = (bid_px + ask_px) * 0.5;
            st.mids[st.head] = mid;
            st.head = (st.head + 1) & (WINDOW - 1);   // valid because WINDOW == 64
            if (st.count < WINDOW) {
                ++st.count;
            }
            if (st.count < WINDOW) {
                continue;  // warm-up: no order
            }

            double sum = 0.0;
            for (double x : st.mids) sum += x;
            const double mean = sum / static_cast<double>(WINDOW);

            double sq = 0.0;
            for (double x : st.mids) {
                const double d = x - mean;
                sq += d * d;
            }
            const double variance = sq / static_cast<double>(WINDOW);
            const double stddev = std::sqrt(variance);
            if (stddev < EPSILON_STDDEV) {
                continue;
            }

            const double z = (mid - mean) / stddev;
            const double abs_z = std::fabs(z);

            // ---- Emit (and apply the deterministic fill, STRATEGY_SPEC §8) --
            csot::Order order{};
            bool emit = false;

            if (st.position == 0) {
                if (z >= ENTRY_Z) {
                    order = {csot::Order::Side::SELL, names_[sym], bid_px, 1};
                    st.position -= 1;
                    emit = true;
                } else if (z <= -ENTRY_Z) {
                    order = {csot::Order::Side::BUY, names_[sym], ask_px, 1};
                    st.position += 1;
                    emit = true;
                }
            } else if (st.position > 0 && abs_z <= EXIT_Z) {
                order = {csot::Order::Side::SELL, names_[sym], bid_px,
                         static_cast<std::uint32_t>(st.position)};
                st.position = 0;
                emit = true;
            } else if (st.position < 0 && abs_z <= EXIT_Z) {
                order = {csot::Order::Side::BUY, names_[sym], ask_px,
                         static_cast<std::uint32_t>(-st.position)};
                st.position = 0;
                emit = true;
            }

            if (emit) {
                out[num_orders].tick_index = static_cast<std::uint64_t>(i);
                out[num_orders].order = order;
                ++num_orders;
            }
        }

        return num_orders;
    }
};

}  // namespace

extern "C" csot::Pipeline* create_pipeline() {
    return new StubPipeline();
}
