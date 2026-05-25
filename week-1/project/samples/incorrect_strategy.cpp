// Incorrect sample — identical to spec_strategy.cpp except ENTRY_Z = 2.5 (spec says 2.0).
// The judge diffs order streams; this should fail with status `incorrect`.
// Build: ./build-incorrect.sh

#include "strategy.hpp"

#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

inline constexpr std::size_t WINDOW = 64;
inline constexpr double ENTRY_Z = 2.5;  // BUG: spec requires 2.0
inline constexpr double EXIT_Z = 0.5;
inline constexpr double EPSILON_STDDEV = 1e-9;

struct SymbolState {
    double mids[WINDOW]{};
    std::uint32_t count = 0;
    std::uint32_t head = 0;
    std::int32_t position = 0;
};

class WrongEntryStrategy : public csot::Strategy {
public:
    std::vector<csot::Order> on_tick(const csot::Tick& t) override {
        auto& st = state_[std::string(t.symbol)];

        const double mid = (t.bid_px + t.ask_px) * 0.5;

        st.mids[st.head] = mid;
        st.head = (st.head + 1) & 63;
        if (st.count < WINDOW) {
            ++st.count;
        }

        if (st.count < WINDOW) {
            return {};
        }

        double sum = 0.0;
        for (double x : st.mids) {
            sum += x;
        }
        const double mean = sum / 64.0;

        double sq = 0.0;
        for (double x : st.mids) {
            const double d = x - mean;
            sq += d * d;
        }
        const double variance = sq / 64.0;
        const double stddev = std::sqrt(variance);

        if (stddev < EPSILON_STDDEV) {
            return {};
        }

        const double z = (mid - mean) / stddev;
        const double abs_z = std::fabs(z);

        if (st.position == 0) {
            if (z >= ENTRY_Z) {
                return {csot::Order{csot::Order::Side::SELL, t.symbol, t.bid_px, 1}};
            }
            if (z <= -ENTRY_Z) {
                return {csot::Order{csot::Order::Side::BUY, t.symbol, t.ask_px, 1}};
            }
            return {};
        }

        if (st.position > 0 && abs_z <= EXIT_Z) {
            return {csot::Order{
                csot::Order::Side::SELL, t.symbol, t.bid_px,
                static_cast<std::uint32_t>(st.position)}};
        }

        if (st.position < 0 && abs_z <= EXIT_Z) {
            return {csot::Order{
                csot::Order::Side::BUY, t.symbol, t.ask_px,
                static_cast<std::uint32_t>(-st.position)}};
        }

        return {};
    }

    void on_fill(const csot::Order& o, double, std::uint32_t fill_qty) override {
        auto& st = state_[std::string(o.symbol)];
        if (o.side == csot::Order::Side::BUY) {
            st.position += static_cast<std::int32_t>(fill_qty);
        } else {
            st.position -= static_cast<std::int32_t>(fill_qty);
        }
    }

private:
    std::unordered_map<std::string, SymbolState> state_;
};

}  // namespace

extern "C" csot::Strategy* create_strategy() {
    return new WrongEntryStrategy();
}
