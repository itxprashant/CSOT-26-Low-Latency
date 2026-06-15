// ============================================================================
//  main.cpp — local test harness for the Week-3 parallel-aggregator project.
//
//  This is INFRASTRUCTURE, not the deliverable. It mirrors what the judge does
//  so you can self-test before uploading: it loads a stream, hands it to your
//  Aggregator::run(), times that one call, and prints the resulting table as
//  JSON (num_symbols, n, a FNV-1a checksum over the whole table, and the
//  non-empty rows) plus throughput on stderr. The JSON is byte-compatible with
//  `python3 data/gen_ticks.py --stats <file>`, so:
//
//      diff <(./agg_runner data/tiny.ticks 2>/dev/null) data/tiny.agg.json
//
//  should be clean when your table is correct.
//
//  Usage:
//      ./agg_runner <stream_file>
//
//  The judge owns its own copy of a harness like this; you never upload it.
//  Only your aggregator.cpp is submitted.
// ============================================================================

#include "aggregate.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <vector>

// The fixed number of symbol ids in the dataset (AGG_SPEC.md §3). The stream
// file does not carry it; the spec pins it for the whole season.
namespace {
constexpr std::uint32_t NUM_SYMBOLS = 1024;

// FNV-1a 64 over the raw bytes of the result table. Identical to the checksum
// gen_ticks.py computes, so the two JSON outputs match exactly.
std::uint64_t table_checksum(const csot::SymbolAgg* out, std::uint32_t num_symbols) {
    std::uint64_t h = 0xCBF29CE484222325ull;
    const auto* bytes = reinterpret_cast<const unsigned char*>(out);
    const std::size_t total = static_cast<std::size_t>(num_symbols) * sizeof(csot::SymbolAgg);
    for (std::size_t i = 0; i < total; ++i) {
        h ^= bytes[i];
        h *= 0x00000100000001B3ull;
    }
    return h;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <stream_file>\n", argv[0]);
        return 2;
    }

    // ---- Load the stream ----------------------------------------------------
    // The on-disk format is a raw array of csot::AggTick records (32 bytes
    // each), little-endian, no header. See AGG_SPEC.md §2.
    std::ifstream in(argv[1], std::ios::binary | std::ios::ate);
    if (!in) {
        std::fprintf(stderr, "error: cannot open stream '%s'\n", argv[1]);
        return 2;
    }
    const std::streamoff bytes = in.tellg();
    if (bytes < 0 || (bytes % static_cast<std::streamoff>(sizeof(csot::AggTick))) != 0) {
        std::fprintf(stderr, "error: stream size %lld is not a multiple of %zu\n",
                     static_cast<long long>(bytes), sizeof(csot::AggTick));
        return 2;
    }
    in.seekg(0);

    const std::size_t n = static_cast<std::size_t>(bytes) / sizeof(csot::AggTick);
    std::vector<csot::AggTick> stream(n);
    if (n > 0 && !in.read(reinterpret_cast<char*>(stream.data()), bytes)) {
        std::fprintf(stderr, "error: short read on stream '%s'\n", argv[1]);
        return 2;
    }

    // ---- Build and initialize the aggregator --------------------------------
    csot::Aggregator* agg = create_aggregator();
    if (agg == nullptr) {
        std::fprintf(stderr, "error: create_aggregator() returned nullptr\n");
        return 1;
    }
    agg->on_init(NUM_SYMBOLS);

    std::vector<csot::SymbolAgg> out(NUM_SYMBOLS);

    // ---- Time exactly run(), like the judge does ----------------------------
    const auto t0 = std::chrono::steady_clock::now();
    agg->run(stream.data(), stream.size(), out.data());
    const auto t1 = std::chrono::steady_clock::now();

    const double elapsed_ns =
        std::chrono::duration<double, std::nano>(t1 - t0).count();
    const double mtps =
        (elapsed_ns > 0.0) ? (static_cast<double>(n) / elapsed_ns * 1e3) : 0.0;

    // ---- Report -------------------------------------------------------------
    // JSON to stdout (diff against data/tiny.agg.json); timing to stderr so it
    // never pollutes the comparison. Format matches gen_ticks.py --stats
    // (json.dumps indent=2) byte-for-byte.
    std::printf("{\n");
    std::printf("  \"num_symbols\": %u,\n", NUM_SYMBOLS);
    std::printf("  \"n\": %zu,\n", n);
    std::printf("  \"checksum\": %llu,\n",
                static_cast<unsigned long long>(table_checksum(out.data(), NUM_SYMBOLS)));

    std::printf("  \"nonempty\": [");
    bool first = true;
    for (std::uint32_t s = 0; s < NUM_SYMBOLS; ++s) {
        if (out[s].count == 0) continue;
        std::printf("%s\n    {\n", first ? "" : ",");
        first = false;
        std::printf("      \"symbol\": %u,\n", s);
        std::printf("      \"count\": %llu,\n",
                    static_cast<unsigned long long>(out[s].count));
        std::printf("      \"sum_price\": %lld,\n",
                    static_cast<long long>(out[s].sum_price));
        std::printf("      \"min_price\": %lld,\n",
                    static_cast<long long>(out[s].min_price));
        std::printf("      \"max_price\": %lld,\n",
                    static_cast<long long>(out[s].max_price));
        std::printf("      \"sum_qty\": %llu\n",
                    static_cast<unsigned long long>(out[s].sum_qty));
        std::printf("    }");
    }
    if (first) {
        // No non-empty rows: matches json.dumps of an empty list, "[]".
        std::printf("]\n");
    } else {
        std::printf("\n  ]\n");
    }
    std::printf("}\n");

    std::fprintf(stderr, "ticks = %zu   run = %.0f ns   throughput = %.2f M ticks/s\n",
                 n, elapsed_ns, mtps);

    delete agg;
    return 0;
}
