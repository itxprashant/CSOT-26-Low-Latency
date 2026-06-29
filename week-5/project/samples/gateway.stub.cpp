// ============================================================================
//  gateway.stub.cpp — STARTING POINT for the Week-5 network-gateway capstone.
//
//  Copy this to `gateway.cpp`, then make it FAST:
//      cp samples/gateway.stub.cpp gateway.cpp
//      cmake -B build -DCSOT_GW_SRC=gateway.cpp && cmake --build build -j
//      ./build/gateway_runner data/tiny.feed     # compare to data/tiny.orders.json
//
//  Like every week's stub, this one is CORRECT: it receives the UDP feed with a
//  SINGLE-THREADED, BLOCKING recv loop, decodes each WireTick, runs the frozen
//  Week-1 z-score strategy spec inline (GATEWAY_SPEC.md §4-§6, STRATEGY_SPEC.md
//  §5-§8) in stream order, and ACKs every datagram so the sender keeps going
//  (the ACK-window, §7). It passes data/tiny.feed out of the box and produces
//  the exact reference order stream. That is on purpose — Week 5's challenge is
//  NOT the (fully specified) strategy; it is keeping the kernel/wire off the
//  strategy's hot path while two cores stay busy.
//
//  YOUR JOB: keep it correct and deterministic, and make run() fast by:
//    1. flipping the socket NON-BLOCKING and draining it with an epoll loop
//       (or a pinned busy-poll loop), reading to EAGAIN              (02-, 03-)
//    2. moving recv + decode + ACK onto an INGEST thread, and handing each
//       decoded tick to a STRATEGY thread across your Week-4 lock-free
//       SPSC RING BUFFER                                             (Week 4)
//    3. ACKing only AFTER try_push succeeds, so back-pressure reaches the
//       sender and nothing is ever dropped                          (04- §4, 05-)
//    4. batching: drain many datagrams per wake, ACK once per K      (01-, 03-)
//    5. pinning both threads to distinct cores (Week 3), Week-2 rolling sums
//       for the strategy, and allocating EVERYTHING in on_init()     (zero heap
//       in run(); NO syscalls on the strategy thread)                (05- §3)
//
//  A correct single-threaded entry (this file, unchanged) is a VALID upload; it
//  just ranks near the bottom because the recv syscall, the decode, and the
//  strategy all run one after another on one thread. The board is won on speed.
//
//  Everything must live in this ONE translation unit. The judge builds exactly
//  this file against its own main() (with its own UDP feed sender); no extra
//  .cpp, no custom CMake. Threads, <atomic>, sched_setaffinity, epoll, and the
//  sockets API ARE allowed and expected this week — see TROUBLESHOOTING.md.
// ============================================================================

#include "gateway.hpp"

#include <sys/socket.h>

#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstring>
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

class StubGateway final : public csot::Gateway {
    std::uint32_t            num_symbols_ = 0;
    std::vector<std::string> names_;   // names_[k] == "SYM<k>" (interned once)
    std::vector<SymbolState> state_;
    std::vector<unsigned char> dgram_;  // one datagram's worth of recv buffer

public:
    void on_init(std::uint32_t num_symbols) override {
        // COLD PATH: allocate everything you will ever need here. For the fast
        // version that means your ring-buffer storage, epoll events[] buffer,
        // and thread bookkeeping too — never inside run().
        num_symbols_ = num_symbols;
        names_.resize(num_symbols);
        for (std::uint32_t k = 0; k < num_symbols; ++k) {
            names_[k] = "SYM" + std::to_string(k);
        }
        state_.assign(num_symbols, SymbolState{});
        dgram_.assign(csot::MAX_FEED_DATAGRAM_BYTES, 0);
    }

    std::size_t run(int fd, std::size_t n, csot::OrderRecord* out) override {
        std::size_t num_orders = 0;
        std::size_t processed = 0;

        // SINGLE-THREADED, BLOCKING: recv one datagram, decode + strategize all
        // its ticks in stream order, ACK it, repeat until n ticks are done.
        // TODO: split this into an epoll/recv INGEST thread that pushes decoded
        // ticks across your SPSC ring, and a pinned STRATEGY thread that pops
        // them and runs the logic below. Keep ALL syscalls off the strategy
        // thread, and ACK only after the ticks are in the ring.
        while (processed < n) {
            const ssize_t k = ::recv(fd, dgram_.data(), dgram_.size(), 0);
            if (k < 0) {
                if (errno == EINTR) continue;
                break;  // unexpected error
            }
            if (k < static_cast<ssize_t>(sizeof(csot::FeedDatagramHeader))) {
                continue;  // runt datagram; ignore
            }

            csot::FeedDatagramHeader hdr;
            std::memcpy(&hdr, dgram_.data(), sizeof(hdr));

            const unsigned char* tick_bytes = dgram_.data() + sizeof(csot::FeedDatagramHeader);
            for (std::uint32_t j = 0; j < hdr.count && processed < n; ++j) {
                csot::WireTick w;
                std::memcpy(&w, tick_bytes + static_cast<std::size_t>(j) * sizeof(csot::WireTick),
                            sizeof(csot::WireTick));

                // ---- Decode (GATEWAY_SPEC.md §4) ----------------------------
                const std::uint32_t sym = w.symbol_id;
                const double bid_px = static_cast<double>(w.bid_px_fp) /
                                      static_cast<double>(csot::PRICE_SCALE);
                const double ask_px = static_cast<double>(w.ask_px_fp) /
                                      static_cast<double>(csot::PRICE_SCALE);

                // ---- Strategy (STRATEGY_SPEC.md §5-§7) ----------------------
                SymbolState& st = state_[sym];

                const double mid = (bid_px + ask_px) * 0.5;
                st.mids[st.head] = mid;
                st.head = (st.head + 1) & (WINDOW - 1);   // valid because WINDOW == 64
                if (st.count < WINDOW) {
                    ++st.count;
                }
                if (st.count < WINDOW) {
                    ++processed;
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
                    ++processed;
                    continue;
                }

                const double z = (mid - mean) / stddev;
                const double abs_z = std::fabs(z);

                // ---- Emit (and apply the deterministic fill, STRATEGY_SPEC §8)
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
                    out[num_orders].tick_index = static_cast<std::uint64_t>(processed);
                    out[num_orders].order = order;
                    ++num_orders;
                }

                ++processed;
            }

            // ---- ACK this datagram (GATEWAY_SPEC.md §7) ----------------------
            // "I have everything with seq < next_seq." The fast version batches
            // this (one ACK per K datagrams) and ACKs only after try_push.
            const std::uint64_t next_seq = hdr.seq + 1;
            (void)::send(fd, &next_seq, sizeof(next_seq), 0);
        }

        return num_orders;
    }
};

}  // namespace

extern "C" csot::Gateway* create_gateway() {
    return new StubGateway();
}
