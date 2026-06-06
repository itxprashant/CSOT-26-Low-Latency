// ============================================================================
//  main.cpp — local test harness for the Week-2 cache-sim project.
//
//  This is INFRASTRUCTURE, not the deliverable. It mirrors what the judge does
//  so you can self-test before uploading: it loads a trace, hands it to your
//  CacheSim::run(), times that one call, and prints the resulting CacheStats
//  plus throughput. Compare the printed counters against the golden file
//  data/tiny.stats.json to know whether your simulator is correct.
//
//  Usage:
//      ./cache_sim_runner <trace_file>
//
//  The judge owns its own copy of a harness like this; you never upload it.
//  Only your cache_sim.cpp is submitted.
// ============================================================================

#include "cache_sim.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <trace_file>\n", argv[0]);
        return 2;
    }

    // ---- Load the trace -----------------------------------------------------
    // The on-disk format is a raw array of csot::MemAccess records (16 bytes
    // each), little-endian, no header. See CACHE_SPEC.md §2.
    std::ifstream in(argv[1], std::ios::binary | std::ios::ate);
    if (!in) {
        std::fprintf(stderr, "error: cannot open trace '%s'\n", argv[1]);
        return 2;
    }
    const std::streamoff bytes = in.tellg();
    if (bytes < 0 || (bytes % static_cast<std::streamoff>(sizeof(csot::MemAccess))) != 0) {
        std::fprintf(stderr, "error: trace size %lld is not a multiple of %zu\n",
                     static_cast<long long>(bytes), sizeof(csot::MemAccess));
        return 2;
    }
    in.seekg(0);

    const std::size_t n = static_cast<std::size_t>(bytes) / sizeof(csot::MemAccess);
    std::vector<csot::MemAccess> trace(n);
    if (n > 0 && !in.read(reinterpret_cast<char*>(trace.data()), bytes)) {
        std::fprintf(stderr, "error: short read on trace '%s'\n", argv[1]);
        return 2;
    }

    // ---- Build and warm up the simulator ------------------------------------
    csot::CacheSim* sim = create_cache_sim();
    if (sim == nullptr) {
        std::fprintf(stderr, "error: create_cache_sim() returned nullptr\n");
        return 1;
    }
    sim->on_init();

    // ---- Time exactly run(), like the judge does ----------------------------
    const auto t0 = std::chrono::steady_clock::now();
    const csot::CacheStats s = sim->run(trace.data(), trace.size());
    const auto t1 = std::chrono::steady_clock::now();

    const double elapsed_ns =
        std::chrono::duration<double, std::nano>(t1 - t0).count();
    const double mtps =
        (elapsed_ns > 0.0) ? (static_cast<double>(n) / elapsed_ns * 1e3) : 0.0;

    // ---- Report -------------------------------------------------------------
    // Counters as JSON so you can diff against data/tiny.stats.json; timing on
    // stderr so it never pollutes a stats comparison.
    std::printf(
        "{\n"
        "  \"reads\": %llu,\n"
        "  \"writes\": %llu,\n"
        "  \"l1_hits\": %llu,\n"
        "  \"l1_misses\": %llu,\n"
        "  \"l2_hits\": %llu,\n"
        "  \"l2_misses\": %llu,\n"
        "  \"dirty_writebacks\": %llu\n"
        "}\n",
        static_cast<unsigned long long>(s.reads),
        static_cast<unsigned long long>(s.writes),
        static_cast<unsigned long long>(s.l1_hits),
        static_cast<unsigned long long>(s.l1_misses),
        static_cast<unsigned long long>(s.l2_hits),
        static_cast<unsigned long long>(s.l2_misses),
        static_cast<unsigned long long>(s.dirty_writebacks));

    std::fprintf(stderr, "accesses = %zu   run = %.0f ns   throughput = %.2f M acc/s\n",
                 n, elapsed_ns, mtps);

    delete sim;
    return 0;
}
