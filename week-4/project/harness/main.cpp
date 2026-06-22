// ============================================================================
//  main.cpp — local test harness for the Week-4 lock-free pipeline project.
//
//  This is INFRASTRUCTURE, not the deliverable. It mirrors what the judge does
//  so you can self-test before uploading: it loads a binary wire feed, hands it
//  to your Pipeline::run(), times that one call, and prints the resulting order
//  stream as JSON (num_symbols, n, num_orders, an FNV-1a checksum over the
//  stream, and every order) plus throughput on stderr. The JSON is
//  byte-compatible with `python3 data/gen_feed.py --orders <feed>`, so:
//
//      diff <(./pipeline_runner data/tiny.feed 2>/dev/null) data/tiny.orders.json
//
//  should be clean when your pipeline is correct.
//
//  Usage:
//      ./pipeline_runner <feed_file>
//
//  The judge owns its own copy of a harness like this; you never upload it.
//  Only your pipeline.cpp is submitted.
// ============================================================================

#include "pipeline.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string_view>
#include <vector>

// The fixed number of symbol ids in the feed (PIPELINE_SPEC.md §3). The feed
// file does not carry it; the spec pins it for the whole season.
namespace {
constexpr std::uint32_t NUM_SYMBOLS = 1024;

// FNV-1a 64 over a canonical serialisation of the order stream. Identical to
// the checksum gen_feed.py computes, so the two JSON outputs match exactly.
// Each order contributes, in little-endian: tick_index(u64), timestamp_ns(u64),
// side(u64), price_fp(i64), qty(u64), then the raw bytes of the symbol string.
struct FnvHasher {
    std::uint64_t h = 0xCBF29CE484222325ull;
    void byte(unsigned char b) { h ^= b; h *= 0x00000100000001B3ull; }
    void u64(std::uint64_t v) {
        for (int i = 0; i < 8; ++i) byte(static_cast<unsigned char>((v >> (8 * i)) & 0xFF));
    }
    void bytes(std::string_view s) {
        for (char c : s) byte(static_cast<unsigned char>(c));
    }
};
}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <feed_file>\n", argv[0]);
        return 2;
    }

    // ---- Load the feed ------------------------------------------------------
    // The on-disk format is a raw array of csot::WireTick records (40 bytes
    // each), little-endian, no header. See PIPELINE_SPEC.md §2.
    std::ifstream in(argv[1], std::ios::binary | std::ios::ate);
    if (!in) {
        std::fprintf(stderr, "error: cannot open feed '%s'\n", argv[1]);
        return 2;
    }
    const std::streamoff bytes = in.tellg();
    if (bytes < 0 || (bytes % static_cast<std::streamoff>(sizeof(csot::WireTick))) != 0) {
        std::fprintf(stderr, "error: feed size %lld is not a multiple of %zu\n",
                     static_cast<long long>(bytes), sizeof(csot::WireTick));
        return 2;
    }
    in.seekg(0);

    const std::size_t n = static_cast<std::size_t>(bytes) / sizeof(csot::WireTick);
    std::vector<csot::WireTick> feed(n);
    if (n > 0 && !in.read(reinterpret_cast<char*>(feed.data()), bytes)) {
        std::fprintf(stderr, "error: short read on feed '%s'\n", argv[1]);
        return 2;
    }

    // ---- Build and initialise the pipeline ----------------------------------
    csot::Pipeline* pipe = create_pipeline();
    if (pipe == nullptr) {
        std::fprintf(stderr, "error: create_pipeline() returned nullptr\n");
        return 1;
    }
    pipe->on_init(NUM_SYMBOLS);

    // The spec emits at most one order per tick, so n records is always enough.
    std::vector<csot::OrderRecord> out(n);

    // ---- Time exactly run(), like the judge does ----------------------------
    const auto t0 = std::chrono::steady_clock::now();
    const std::size_t num_orders = pipe->run(feed.data(), feed.size(), out.data());
    const auto t1 = std::chrono::steady_clock::now();

    const double elapsed_ns =
        std::chrono::duration<double, std::nano>(t1 - t0).count();
    const double mtps =
        (elapsed_ns > 0.0) ? (static_cast<double>(n) / elapsed_ns * 1e3) : 0.0;

    // ---- Report -------------------------------------------------------------
    // JSON to stdout (diff against data/tiny.orders.json); timing to stderr so
    // it never pollutes the comparison. Format matches gen_feed.py --orders
    // (json.dumps indent=2) byte-for-byte.
    FnvHasher hasher;
    for (std::size_t i = 0; i < num_orders; ++i) {
        const csot::OrderRecord& rec = out[i];
        const std::uint64_t ts =
            (rec.tick_index < n) ? feed[rec.tick_index].timestamp_ns : 0;
        const long long price_fp =
            std::llround(rec.order.price * static_cast<double>(csot::PRICE_SCALE));
        hasher.u64(rec.tick_index);
        hasher.u64(ts);
        hasher.u64(static_cast<std::uint64_t>(rec.order.side));
        hasher.u64(static_cast<std::uint64_t>(price_fp));
        hasher.u64(static_cast<std::uint64_t>(rec.order.qty));
        hasher.bytes(rec.order.symbol);
    }

    std::printf("{\n");
    std::printf("  \"num_symbols\": %u,\n", NUM_SYMBOLS);
    std::printf("  \"n\": %zu,\n", n);
    std::printf("  \"num_orders\": %zu,\n", num_orders);
    std::printf("  \"checksum\": %llu,\n",
                static_cast<unsigned long long>(hasher.h));

    if (num_orders == 0) {
        std::printf("  \"orders\": []\n");
    } else {
        std::printf("  \"orders\": [");
        for (std::size_t i = 0; i < num_orders; ++i) {
            const csot::OrderRecord& rec = out[i];
            const std::uint64_t ts =
                (rec.tick_index < n) ? feed[rec.tick_index].timestamp_ns : 0;
            const long long price_fp =
                std::llround(rec.order.price * static_cast<double>(csot::PRICE_SCALE));
            std::printf("%s\n    {\n", i == 0 ? "" : ",");
            std::printf("      \"tick_index\": %llu,\n",
                        static_cast<unsigned long long>(rec.tick_index));
            std::printf("      \"timestamp_ns\": %llu,\n",
                        static_cast<unsigned long long>(ts));
            std::printf("      \"symbol\": \"%.*s\",\n",
                        static_cast<int>(rec.order.symbol.size()), rec.order.symbol.data());
            std::printf("      \"side\": %u,\n",
                        static_cast<unsigned>(rec.order.side));
            std::printf("      \"price_fp\": %lld,\n", price_fp);
            std::printf("      \"qty\": %u\n", rec.order.qty);
            std::printf("    }");
        }
        std::printf("\n  ]\n");
    }
    std::printf("}\n");

    std::fprintf(stderr,
                 "ticks = %zu   orders = %zu   run = %.0f ns   throughput = %.2f M ticks/s\n",
                 n, num_orders, elapsed_ns, mtps);

    delete pipe;
    return 0;
}
