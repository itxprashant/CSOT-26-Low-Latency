// ============================================================================
//  histogram.hpp — CSoT'26 Low Latency Track, Week 1
//
//  Minimal fixed-bucket latency histogram for the replay engine.
//
//  Buckets are exponential: bucket i covers [2^i ns, 2^(i+1) ns).
//  With N=32 buckets we cover 0 ns ... ~4.3 seconds, which is plenty for
//  per-tick latencies on any sane strategy. Each record() is ~3 instructions
//  (clz + array bump), so this is itself safe to put on the hot path.
//
//  In production you'd reach for HdrHistogram (https://hdrhistogram.org) for
//  better precision in the tails. For Week 1 this is plenty.
// ============================================================================

#pragma once

#include <array>
#include <cstdint>
#include <ostream>

namespace csot {

class LatencyHistogram {
    static constexpr std::size_t N = 32;
    std::array<std::uint64_t, N> buckets_{};
    std::uint64_t                count_ = 0;

public:
    // Record one latency sample, in nanoseconds. O(1), allocation-free.
    void record(std::uint64_t ns) noexcept {
        const std::size_t idx =
            (ns == 0) ? 0 : 63 - __builtin_clzll(ns);
        const std::size_t clamped = (idx < N) ? idx : N - 1;
        ++buckets_[clamped];
        ++count_;
    }

    std::uint64_t count() const noexcept { return count_; }

    // Upper bound (in ns) of the bucket containing the q-th percentile,
    // q in [0.0, 1.0]. Coarse — buckets are powers of two — but fine for
    // a Week-1 sanity check.
    std::uint64_t percentile(double q) const noexcept {
        if (count_ == 0) {
            return 0;
        }
        auto target = static_cast<std::uint64_t>(static_cast<double>(count_) * q);
        if (target == 0) {
            target = 1;
        }
        std::uint64_t cum = 0;
        for (std::size_t i = 0; i < N; ++i) {
            cum += buckets_[i];
            if (cum >= target) {
                return 1ULL << (i + 1);
            }
        }
        return 1ULL << N;
    }

    void print(std::ostream& os) const {
        os << "count = " << count_           << '\n'
           << "p50  <= " << percentile(0.50) << " ns\n"
           << "p90  <= " << percentile(0.90) << " ns\n"
           << "p99  <= " << percentile(0.99) << " ns\n"
           << "p999 <= " << percentile(0.999) << " ns\n";
    }
};

}  // namespace csot
