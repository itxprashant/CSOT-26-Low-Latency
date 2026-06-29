// ============================================================================
//  main.cpp — local test harness for the Week-5 network-gateway capstone.
//
//  This is INFRASTRUCTURE, not the deliverable. It mirrors what the judge does
//  so you can self-test before uploading:
//
//    1. Loads a binary wire feed (a flat array of csot::WireTick, like Week 4).
//    2. Opens a CONNECTED UDP socket pair on 127.0.0.1 (loopback).
//    3. Spawns a SENDER thread that frames the feed into datagrams
//       (FeedDatagramHeader + WireTick[count]) and transmits them using the
//       ACK-WINDOW flow-control protocol (GATEWAY_SPEC.md §7): it stays at most
//       ACK_WINDOW datagrams ahead of the gateway's last ACK, so the receive
//       buffer never overflows and no datagram is ever dropped.
//    4. Calls your Gateway::run(fd, n, out) on the OTHER end of the socket,
//       times exactly that call, and prints the resulting order stream as JSON
//       (num_symbols, n, num_orders, an FNV-1a checksum, every order) plus
//       throughput on stderr.
//
//  The JSON is byte-compatible with `python3 data/gen_feed.py --orders <feed>`
//  (the SAME order stream as Week 4 — same feed, same strategy), so:
//
//      diff <(./gateway_runner data/tiny.feed 2>/dev/null) data/tiny.orders.json
//
//  should be clean when your gateway is correct.
//
//  Usage:
//      ./gateway_runner <feed_file>
//
//  The judge owns its own copy of a harness like this (with its own UDP feed
//  sender); you never upload it. Only your gateway.cpp is submitted.
// ============================================================================

#include "gateway.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string_view>
#include <thread>
#include <vector>

namespace {

// The fixed number of symbol ids in the feed (GATEWAY_SPEC.md §3). The feed
// file does not carry it; the spec pins it for the whole season.
constexpr std::uint32_t NUM_SYMBOLS = csot::NUM_SYMBOLS;

// FNV-1a 64 over a canonical serialisation of the order stream. Identical to
// the checksum gen_feed.py computes, so the two JSON outputs match exactly.
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

// Best-effort: ask the kernel for a receive/send buffer big enough to hold the
// whole ACK-window of datagrams in flight, so loopback never drops. The judge
// raises net.core.rmem_max so the request is honoured; locally the kernel may
// cap it, which is fine because the stub ACKs eagerly and never lets the
// buffer fill.
void size_socket_buffers(int fd) {
    int want = 8 * 1024 * 1024;  // 8 MiB
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &want, sizeof(want));
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &want, sizeof(want));
}

// Open a connected UDP socket pair on 127.0.0.1. Returns {sender_fd,
// gateway_fd}; each is connect()'d to the other, so plain recv()/send() work.
bool make_loopback_udp_pair(int& sender_fd, int& gateway_fd) {
    sender_fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    gateway_fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (sender_fd < 0 || gateway_fd < 0) return false;

    auto bind_loopback = [](int fd, sockaddr_in& out) -> bool {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = 0;  // ephemeral
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) return false;
        socklen_t len = sizeof(out);
        return ::getsockname(fd, reinterpret_cast<sockaddr*>(&out), &len) == 0;
    };

    sockaddr_in addr_sender{}, addr_gateway{};
    if (!bind_loopback(sender_fd, addr_sender)) return false;
    if (!bind_loopback(gateway_fd, addr_gateway)) return false;

    size_socket_buffers(sender_fd);
    size_socket_buffers(gateway_fd);

    if (::connect(sender_fd, reinterpret_cast<sockaddr*>(&addr_gateway), sizeof(addr_gateway)) != 0)
        return false;
    if (::connect(gateway_fd, reinterpret_cast<sockaddr*>(&addr_sender), sizeof(addr_sender)) != 0)
        return false;
    return true;
}

// The judge-equivalent feed sender. Frames `feed` into datagrams of up to
// MAX_BATCH WireTicks and transmits them on `fd`, never running more than
// ACK_WINDOW datagrams ahead of the gateway's acknowledged seq. Reads the
// gateway's ACKs (bare uint64 = next expected seq) off the same fd to advance
// the window. Returns when the whole feed has been sent.
void feed_sender(int fd, const std::vector<csot::WireTick>& feed) {
    const std::size_t n = feed.size();
    alignas(8) unsigned char buf[csot::MAX_FEED_DATAGRAM_BYTES];

    std::uint64_t seq = 0;        // next datagram seq to send
    std::uint64_t acked = 0;      // highest "next seq" the gateway has ACKed
    std::size_t ticks_sent = 0;

    auto drain_acks = [&](bool blocking) {
        std::uint64_t ack = 0;
        for (;;) {
            const ssize_t r = ::recv(fd, &ack, sizeof(ack),
                                     blocking ? 0 : MSG_DONTWAIT);
            if (r == static_cast<ssize_t>(sizeof(ack))) {
                if (ack > acked) acked = ack;
                blocking = false;  // after one blocking read, just drain the rest
                continue;
            }
            break;  // EAGAIN (non-blocking) or short/closed read
        }
    };

    while (ticks_sent < n) {
        // Respect the ACK window: never get more than ACK_WINDOW datagrams
        // ahead of the gateway's acknowledged progress. If we're at the edge,
        // block until an ACK opens the window.
        while (seq >= acked + csot::ACK_WINDOW) {
            drain_acks(/*blocking=*/true);
        }

        const std::uint32_t count = static_cast<std::uint32_t>(
            std::min<std::size_t>(csot::MAX_BATCH, n - ticks_sent));

        auto* h = reinterpret_cast<csot::FeedDatagramHeader*>(buf);
        h->seq = seq;
        h->count = count;
        h->_pad = 0;
        std::memcpy(buf + sizeof(csot::FeedDatagramHeader),
                    feed.data() + ticks_sent,
                    static_cast<std::size_t>(count) * sizeof(csot::WireTick));

        const std::size_t bytes =
            sizeof(csot::FeedDatagramHeader) +
            static_cast<std::size_t>(count) * sizeof(csot::WireTick);

        // On loopback a send only fails transiently if the *send* buffer is
        // full; retry until it goes out so we never silently skip a datagram.
        for (;;) {
            const ssize_t s = ::send(fd, buf, bytes, 0);
            if (s == static_cast<ssize_t>(bytes)) break;
            drain_acks(/*blocking=*/false);
        }

        ticks_sent += count;
        ++seq;

        drain_acks(/*blocking=*/false);  // keep the window fresh, never block here
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <feed_file>\n", argv[0]);
        return 2;
    }

    // ---- Load the feed ------------------------------------------------------
    // The on-disk format is a raw array of csot::WireTick records (40 bytes
    // each), little-endian, no header. See GATEWAY_SPEC.md §2.
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

    // ---- Open the connected UDP loopback socket pair ------------------------
    int sender_fd = -1, gateway_fd = -1;
    if (!make_loopback_udp_pair(sender_fd, gateway_fd)) {
        std::fprintf(stderr, "error: could not set up loopback UDP socket pair\n");
        return 1;
    }

    // ---- Build and initialise the gateway -----------------------------------
    csot::Gateway* gw = create_gateway();
    if (gw == nullptr) {
        std::fprintf(stderr, "error: create_gateway() returned nullptr\n");
        return 1;
    }
    gw->on_init(NUM_SYMBOLS);

    // The spec emits at most one order per tick, so n records is always enough.
    std::vector<csot::OrderRecord> out(n);

    // ---- Start the feed sender, then time exactly run() ---------------------
    // The sender runs concurrently with run(), exactly like the judge: the wire
    // is live while the gateway drains it. We time only run() (wire + epoll +
    // ring + strategy, overlapped).
    std::thread sender;
    if (n > 0) sender = std::thread(feed_sender, sender_fd, std::cref(feed));

    const auto t0 = std::chrono::steady_clock::now();
    const std::size_t num_orders = gw->run(gateway_fd, n, out.data());
    const auto t1 = std::chrono::steady_clock::now();

    if (sender.joinable()) sender.join();

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

    delete gw;
    ::close(sender_fd);
    ::close(gateway_fd);
    return 0;
}
