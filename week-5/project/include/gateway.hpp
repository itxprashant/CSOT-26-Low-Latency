// ============================================================================
//  gateway.hpp — CSoT'26 Low Latency Track, Week 5 (the capstone)
//
//  THIS IS A FROZEN ABI.
//  ----------------------
//  The Week-5 judge builds your single gateway.cpp against this header and a
//  judge-owned main(). It constructs your gateway via create_gateway(), calls
//  on_init(num_symbols) once, opens a CONNECTED UDP loopback socket, spawns its
//  own feed sender on the other end, and then times exactly the run() call
//  below. If you change any signature or struct layout, your submission will
//  fail to build or fail correctness.
//
//  WHAT YOU BUILD
//  --------------
//  A networked, lock-free pipeline inside run() — the whole track in one file:
//
//        UDP datagrams (seq + count + WireTick[count])
//                 │     (judge's feed sender, ACK-window throttled)
//                 ▼
//        [ INGEST thread ]  epoll/recv → decode WireTick → push    (Week 5 + 2)
//                 │  your lock-free SPSC ring buffer                (Week 4)
//                 ▼
//        [ STRATEGY thread ] pop tick → frozen z-score → emit       (Week 3 pin,
//                 │                                                   Week 1 spec)
//                 ▼
//        OrderRecord stream (out[])
//
//  The ingest thread receives datagrams off `fd`, decodes each 40-byte WireTick
//  into a frozen csot::Tick (fixed-point -> double, symbol interning), pushes it
//  across YOUR lock-free single-producer/single-consumer ring buffer, and sends
//  small ACK datagrams back on the SAME fd so the sender never overruns your
//  receive buffer (the ACK-window, GATEWAY_SPEC.md §7). The strategy thread pops
//  ticks in order, drives the Week-1 strategy spec (on_tick + the deterministic
//  fill model), and appends every emitted order to `out`. See GATEWAY_SPEC.md.
//
//  You may:
//    * design any lock-free SPSC ring buffer you like (Week 4)
//    * use epoll OR a busy-poll recv loop; level- or edge-triggered
//    * spawn and pin your ingest / strategy std::threads inside run()
//    * batch recv()s (drain to EAGAIN) and ACK sends (one per K datagrams)
//    * allocate every buffer you need in on_init() (ring, recv buffer,
//      epoll events[], per-symbol state, interned names) — zero heap in run()
//    * devirtualize / inline the strategy however you can (single TU)
//
//  You may NOT change:
//    * the layout of WireTick, OrderRecord, or FeedDatagramHeader
//    * the signature of Gateway's virtual functions
//    * the create_gateway() factory entry point at the bottom
//    * the csot::Tick / csot::Order / csot::Strategy ABI in strategy.hpp
//    * the frozen constants (NUM_SYMBOLS, PRICE_SCALE, MAX_BATCH, ACK_WINDOW)
//
//  Copy this file (and strategy.hpp) verbatim into your project's include/.
//
//  IMPORTANT:
//  ----------
//  This header is only the runtime ABI. WHAT your strategy must compute (the
//  z-score mean-reversion rule, the fill model, the empty-warmup window) is the
//  unchanged Week-1 spec in ../../week-1/project/STRATEGY_SPEC.md; HOW the feed
//  is framed on the wire, how the ACK-window keeps it lossless, and what order
//  stream you must emit is in ../GATEWAY_SPEC.md. The competition ranks the
//  fastest CORRECT, DETERMINISTIC gateway — the answer (the order stream) is
//  fixed; only how fast you ingest + hand off + strategize is ranked.
// ============================================================================

#pragma once

#include <cstddef>
#include <cstdint>

#include "strategy.hpp"

namespace csot {

// ---------------------------------------------------------------------------
// PRICE_SCALE — fixed-point scale for prices on the wire (unchanged from W4).
//
// A WireTick carries integer prices: a real price of 100.0500 is stored as
// 1000500 (= 100.05 * 10000). The ingest thread decodes `bid_px_fp /
// PRICE_SCALE` into the floating-point csot::Tick::bid_px the strategy spec
// consumes, and the judge re-encodes each emitted order's price back to this
// fixed-point integer so the order stream diffs as exact integers, never
// fragile floats.
// ---------------------------------------------------------------------------
inline constexpr std::int64_t PRICE_SCALE = 10000;

// ---------------------------------------------------------------------------
// WireTick — one market tick as it travels on the feed (the DECODE input).
//
// Carried forward UNCHANGED from Week 4: little-endian, 40 bytes, 8-byte
// aligned. On the wire, WireTicks travel inside UDP datagrams (a
// FeedDatagramHeader followed by `count` of these); the .feed file on disk is
// still a flat array of these records. Prices are FIXED-POINT integers (real
// price * PRICE_SCALE); your ingest thread turns this compact integer record
// into the frozen csot::Tick (doubles + an interned symbol string_view) the
// strategy expects. `_reserved` is off-limits. See GATEWAY_SPEC.md §2, §4.
// ---------------------------------------------------------------------------
struct WireTick {
    std::uint64_t timestamp_ns;  // exchange wall-clock, ns since epoch (non-decreasing)
    std::int64_t  bid_px_fp;     // FIXED-POINT best bid: real_bid * PRICE_SCALE
    std::int64_t  ask_px_fp;     // FIXED-POINT best ask: real_ask * PRICE_SCALE
    std::uint32_t symbol_id;     // 0 .. num_symbols-1 (intern to "SYM<id>")
    std::uint32_t bid_qty;       // quantity at best bid (> 0)
    std::uint32_t ask_qty;       // quantity at best ask (> 0)
    std::uint32_t _reserved;     // always zero — do not use
};
static_assert(sizeof(WireTick) == 40, "WireTick layout is part of the ABI; do not change.");
static_assert(alignof(WireTick) == 8, "WireTick alignment is part of the ABI; do not change.");

// ---------------------------------------------------------------------------
// FeedDatagramHeader — the 16-byte header that precedes the WireTicks in each
// UDP feed datagram (NEW in Week 5; part of the wire ABI).
//
// On the wire each feed datagram is exactly:
//
//     [ FeedDatagramHeader (16 B) ][ WireTick[count] (count * 40 B) ]
//
// `seq` numbers the datagrams 0, 1, 2, ... contiguously, so the receiver can
// detect any gap and acknowledge progress. `count` is how many WireTicks
// follow (1 .. MAX_BATCH). On loopback, with the ACK-window enforced by the
// sender (§7), datagrams arrive in seq order, contiguous, with none lost — so
// you may treat `seq` as a monotonic counter and rely on in-order delivery. The
// ACK you send back is just a bare std::uint64_t (the next seq you expect); it
// does NOT carry this header. See GATEWAY_SPEC.md §2, §7.
// ---------------------------------------------------------------------------
struct FeedDatagramHeader {
    std::uint64_t seq;    // contiguous datagram index: 0, 1, 2, ...
    std::uint32_t count;  // number of WireTicks following, 1 .. MAX_BATCH
    std::uint32_t _pad;   // always zero — keeps the header 16 bytes / 8-byte aligned
};
static_assert(sizeof(FeedDatagramHeader) == 16, "FeedDatagramHeader is part of the wire ABI; do not change.");
static_assert(alignof(FeedDatagramHeader) == 8, "FeedDatagramHeader alignment is part of the wire ABI; do not change.");

// ---------------------------------------------------------------------------
// OrderRecord — one entry in the order stream your run() produces (unchanged
// from Week 4).
//
// Every order the strategy emits is recorded here, tagged with the index of
// the WireTick (in feed order) that produced it, so the judge can diff your
// stream against the reference tick-by-tick and reconstruct each order's
// timestamp. Orders MUST be appended in tick order, which a correct in-order
// pipeline produces automatically. See GATEWAY_SPEC.md §6.
// ---------------------------------------------------------------------------
struct OrderRecord {
    std::uint64_t tick_index;  // index into the feed (in send order) that produced this order
    csot::Order   order;       // the emitted order (frozen csot::Order layout)
};
static_assert(sizeof(OrderRecord) == 48, "OrderRecord layout is part of the ABI; do not change.");

// ---------------------------------------------------------------------------
// Frozen Week-5 constants (GATEWAY_SPEC.md §3). Do not change them.
//
//   NUM_SYMBOLS  : number of distinct symbol ids; on_init is called with this.
//   MAX_BATCH    : max WireTicks the sender packs into one feed datagram.
//   ACK_WINDOW   : the sender stays at most this many datagrams ahead of the
//                  highest seq you have ACKed, so at most ACK_WINDOW datagrams
//                  are ever unread in your receive buffer (the losslessness
//                  guarantee — §7). Size your recv buffer per datagram, not per
//                  window; the kernel's SO_RCVBUF is sized to hold the window.
//
// MAX_FEED_DATAGRAM_BYTES is the largest a single feed datagram can be — size
// the buffer you recv() into to at least this many bytes.
// ---------------------------------------------------------------------------
inline constexpr std::uint32_t NUM_SYMBOLS = 1024;
inline constexpr std::uint32_t MAX_BATCH   = 32;
inline constexpr std::uint64_t ACK_WINDOW  = 128;

inline constexpr std::size_t MAX_FEED_DATAGRAM_BYTES =
    sizeof(FeedDatagramHeader) +
    static_cast<std::size_t>(MAX_BATCH) * sizeof(WireTick);   // 16 + 32*40 = 1296

// ---------------------------------------------------------------------------
// Gateway — the abstract base class. Implement these two methods.
//
//   on_init : called once before run(), after construction, with the fixed
//             number of distinct symbol ids in the feed (NUM_SYMBOLS).
//             Allocate every buffer you will ever need here (your ring-buffer
//             storage, the datagram recv buffer, the epoll events[] array,
//             per-symbol strategy state, the interned "SYM<id>" name table,
//             thread bookkeeping). After this returns, the hot path begins;
//             perform NO heap allocations inside run().
//
//   run     : the hot path. You are handed:
//               fd  - a CONNECTED UDP socket (loopback). The judge's feed
//                     sender streams datagrams (FeedDatagramHeader + WireTick[])
//                     on it; you recv() ticks from it AND send() ACK datagrams
//                     (a bare uint64 "next seq expected") back on it. You may
//                     set it non-blocking (O_NONBLOCK) and/or register it with
//                     epoll; the judge hands it to you blocking by default.
//               n   - the TOTAL number of WireTicks that will be sent across
//                     the whole feed, so you know exactly when the feed ends
//                     (stop once you have processed n ticks).
//               out - caller-owned storage for at least `n` OrderRecords (the
//                     spec emits at most one order per tick). Write each emitted
//                     order, in tick order, here.
//             Run your ingest + ring + strategy pipeline to decode every tick,
//             drive the Week-1 strategy spec over the in-order stream, and
//             write each emitted order into `out`. Returns the number of
//             OrderRecords written. The judge measures the wall-clock latency
//             of this single call (wire + epoll + ring + strategy, overlapped)
//             and ranks correct, deterministic submissions by how fast it is.
//
//             You own the threads, the hand-off, the decode, and the ACK
//             cadence. The only thing fixed is that the OrderRecords you write
//             must equal the reference stream, and that you must keep the feed
//             lossless by acknowledging within the ACK-window (§7).
// ---------------------------------------------------------------------------
class Gateway {
public:
    virtual ~Gateway() = default;

    virtual void on_init(std::uint32_t num_symbols) { (void)num_symbols; }

    virtual std::size_t run(int fd, std::size_t n, OrderRecord* out) = 0;
};

}  // namespace csot

// ---------------------------------------------------------------------------
// Factory entry point.
//
// Every submission MUST export this symbol with C linkage. The judge does the
// moral equivalent of:
//
//   csot::Gateway* g = create_gateway();
//   g->on_init(num_symbols);                         // num_symbols == NUM_SYMBOLS
//   std::size_t num_orders = g->run(fd, n, out);     // <-- this call is timed
//
// The returned object is `delete`d by the harness at shutdown.
// ---------------------------------------------------------------------------
extern "C" csot::Gateway* create_gateway();
