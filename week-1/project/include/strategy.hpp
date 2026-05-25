// ============================================================================
//  strategy.hpp — CSoT'26 Low Latency Track, Week 1
//
//  THIS IS A FROZEN ABI.
//  ----------------------
//  The live leaderboard judge `dlopen`s your compiled .so and looks up
//  exactly the symbols defined below. If you change any signature, your
//  strategy will fail to load and the upload will be marked `incorrect`.
//
//  You may:
//    * subclass Strategy in any way you like
//    * add private state, helpers, anything
//    * implement on_init / on_tick / on_fill with any internal data layout
//    * optimize the implementation however you want
//
//  You may NOT change:
//    * the layout of Tick or Order
//    * the signature of Strategy's virtual functions
//    * the create_strategy() factory entry point at the bottom
//
//  Copy this file verbatim into your project's include/ directory.
//
//  IMPORTANT:
//  ----------
//  This header is only the runtime ABI. The actual algorithm your on_tick()
//  must implement is defined in ../STRATEGY_SPEC.md. The competition ranks
//  the fastest correct implementation of that spec, not the cleverest
//  trading-rule choice.
// ============================================================================

#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace csot {

// ---------------------------------------------------------------------------
// Tick — one snapshot of the top of book for one symbol at one instant.
// Passed by const-reference into Strategy::on_tick. Do NOT copy or store
// the string_view beyond the call; the underlying buffer is owned by the
// engine and may be reused on the next tick.
// ---------------------------------------------------------------------------
struct Tick {
    uint64_t          timestamp_ns;  // exchange wall-clock, nanoseconds since epoch
    std::string_view  symbol;        // e.g. "SYM0" — points into engine-owned storage
    double            bid_px;        // best bid price
    double            ask_px;        // best ask price
    uint32_t          bid_qty;       // quantity available at best bid
    uint32_t          ask_qty;       // quantity available at best ask
};
static_assert(sizeof(Tick) == 48, "Tick layout is part of the ABI; do not change.");

// ---------------------------------------------------------------------------
// Order — an instruction to buy or sell. Returned by Strategy::on_tick.
//
// Pricing is in the same units as Tick::bid_px / ask_px (i.e. a decimal
// price like 100.05). The engine treats `price` as a LIMIT price.
// Set price == 0.0 for a market order.
// ---------------------------------------------------------------------------
struct Order {
    enum class Side : uint8_t { BUY = 0, SELL = 1 };

    Side              side;
    std::string_view  symbol;        // must match a symbol the engine knows about
    double            price;         // limit price; 0.0 means market order
    uint32_t          qty;           // strictly positive
};
static_assert(sizeof(Order) == 40, "Order layout is part of the ABI; do not change.");

// ---------------------------------------------------------------------------
// Strategy — the abstract base class. Implement these three methods.
//
//   on_init  : called once before the first tick, after construction.
//              Allocate everything you'll ever need here. After this returns,
//              the hot path begins and you should perform NO heap allocations.
//
//   on_tick  : the hot path. Called once per market tick, in timestamp order.
//              The engine measures the latency of this call. Return any
//              orders required by STRATEGY_SPEC.md. The judge compares your
//              emitted order stream against the reference implementation, then
//              ranks correct submissions by latency and throughput.
//
//   on_fill  : called when a previously-submitted order is matched.
//              Not on the latency-critical path; use it to update position,
//              fill-dependent state, rolling exposure, etc.
// ---------------------------------------------------------------------------
class Strategy {
public:
    virtual ~Strategy() = default;

    virtual void on_init() {}

    virtual std::vector<Order> on_tick(const Tick& t) = 0;

    virtual void on_fill(const Order& o,
                         double        fill_price,
                         uint32_t      fill_qty) {
        (void)o; (void)fill_price; (void)fill_qty;
    }
};

}  // namespace csot

// ---------------------------------------------------------------------------
// Factory entry point.
//
// Every strategy .so MUST export this symbol with C linkage. The Week-5
// judge does:
//
//   void* handle = dlopen("your_strategy.so", RTLD_NOW);
//   auto  make   = (csot::Strategy*(*)())dlsym(handle, "create_strategy");
//   csot::Strategy* s = make();
//
// Returned object will be `delete`d by the engine at shutdown.
// ---------------------------------------------------------------------------
extern "C" csot::Strategy* create_strategy();
