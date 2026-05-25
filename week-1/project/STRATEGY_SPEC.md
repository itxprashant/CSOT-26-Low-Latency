# Strategy Spec — Reference Mean-Reversion Strategy

> **This file is a frozen competition contract.** Every submission must implement this exact strategy. The challenge is not to invent signals; the challenge is to implement the same logic faster, with lower tail latency, fewer allocations, better cache locality, and cleaner systems design.

---

## 1. Goal

The low-latency challenge is quant-trading-shaped, but it is **not** a strategy-selection contest.

Every participant receives the same:

- `Tick` / `Order` / `Strategy` ABI from [`include/strategy.hpp`](./include/strategy.hpp)
- CSV tick data schema from [`README.md`](./README.md)
- Algorithm specification in this file

Every participant must emit the same order stream as the judge reference implementation for the same input. If your order stream differs, your submission is incorrect. Among correct submissions, the leaderboard ranks by latency and throughput.

---

## 2. High-Level Idea

Implement a simple per-symbol z-score mean-reversion strategy:

1. For each symbol, maintain a rolling window of the last `64` mid-prices.
2. On each tick, compute the current mid-price:

```text
mid = (bid_px + ask_px) / 2
```

3. Once a symbol has at least `64` mid-prices, compute the rolling mean and population standard deviation.
4. If the current mid-price is far above the rolling mean, sell one unit.
5. If the current mid-price is far below the rolling mean, buy one unit.
6. If already holding a position and the z-score returns near zero, flatten the position.

This is intentionally simple. Its purpose is to create a deterministic, stateful, hot-path workload with real opportunities for low-latency engineering:

- per-symbol state layout
- ring-buffer implementation
- rolling statistics
- branch layout
- allocation avoidance
- order emission without dynamic memory

---

## 3. Constants

These constants are part of the spec. Do not tune them.

| Name | Value | Meaning |
|---|---:|---|
| `WINDOW` | `64` | Number of mid-prices in the rolling window |
| `ENTRY_Z` | `2.0` | Enter a position when `abs(z) >= ENTRY_Z` |
| `EXIT_Z` | `0.5` | Exit an existing position when `abs(z) <= EXIT_Z` |
| `ORDER_QTY` | `1` | Quantity for every entry order |
| `MAX_ABS_POSITION` | `1` | Maximum allowed absolute position per symbol |
| `EPSILON_STDDEV` | `1e-9` | If standard deviation is below this, emit no order |

The strategy never pyramids. It may hold at most one unit long or one unit short per symbol.

---

## 4. State

Your strategy must maintain the following state per symbol:

```cpp
struct SymbolState {
    double mids[64];      // rolling mid-price window
    uint32_t count;       // number of valid entries seen so far, capped at 64
    uint32_t head;        // index where the next mid-price will be written
    int32_t position;     // -1, 0, or +1
};
```

You do not need to use this exact struct layout, but your behaviour must be identical.

### Symbol identity

Symbols are strings in `Tick::symbol`, e.g. `"SYM0"`, `"SYM1"`.

For Week 1, the public generator emits four symbols:

```text
SYM0, SYM1, SYM2, SYM3
```

The judge may use more symbols in held-out data. Your implementation must support at least `64` distinct symbols.

### Position updates

`position` is updated only when `on_fill(...)` is called:

- BUY fill of quantity `q` increases position by `q`
- SELL fill of quantity `q` decreases position by `q`

Do not update position merely because `on_tick(...)` returns an order. The engine/judge will decide whether the order fills, then call `on_fill`.

For the Week-1 simple engine, you may assume every valid order fills immediately at its submitted price. The judge will use the same deterministic fill model for public and hidden datasets.

---

## 5. Per-Tick Algorithm

For every tick `t`, perform exactly the following logic.

### Step 1 — Compute mid-price

```text
mid = (t.bid_px + t.ask_px) / 2.0
```

Use `double`.

### Step 2 — Append to rolling window

For the symbol `s = t.symbol`:

```text
state.mids[state.head] = mid
state.head = (state.head + 1) mod WINDOW
state.count = min(state.count + 1, WINDOW)
```

### Step 3 — Warm-up period

If `state.count < WINDOW`, return zero orders.

The tick that fills the 64th entry **is eligible** for trading. In other words, append first, then compute.

### Step 4 — Compute mean and standard deviation

Compute the population mean:

```text
mean = sum(mids[i] for i in 0..63) / 64
```

Compute the population variance:

```text
variance = sum((mids[i] - mean)^2 for i in 0..63) / 64
```

Then:

```text
stddev = sqrt(variance)
```

Use population standard deviation, not sample standard deviation. The denominator is exactly `64`, not `63`.

If:

```text
stddev < EPSILON_STDDEV
```

return zero orders.

### Step 5 — Compute z-score

```text
z = (mid - mean) / stddev
```

### Step 6 — Entry logic

If `position == 0`:

- If `z >= +ENTRY_Z`, emit exactly one SELL order:

```text
side   = SELL
symbol = t.symbol
price  = t.bid_px
qty    = 1
```

- Else if `z <= -ENTRY_Z`, emit exactly one BUY order:

```text
side   = BUY
symbol = t.symbol
price  = t.ask_px
qty    = 1
```

- Else emit zero orders.

The `z >= +ENTRY_Z` condition is checked before `z <= -ENTRY_Z`, though both cannot be true for finite `z`.

### Step 7 — Exit logic

If `position > 0` and `abs(z) <= EXIT_Z`, emit exactly one SELL order to flatten:

```text
side   = SELL
symbol = t.symbol
price  = t.bid_px
qty    = abs(position)
```

If `position < 0` and `abs(z) <= EXIT_Z`, emit exactly one BUY order to flatten:

```text
side   = BUY
symbol = t.symbol
price  = t.ask_px
qty    = abs(position)
```

Otherwise emit zero orders.

Because `MAX_ABS_POSITION == 1`, `abs(position)` should be `1` in valid runs.

---

## 6. Reference Pseudocode

This pseudocode is the behavioural reference. If the prose and pseudocode appear to disagree, treat this pseudocode as authoritative and report the ambiguity.

```cpp
std::vector<csot::Order> on_tick(const csot::Tick& t) {
    auto& st = state_for(t.symbol);

    const double mid = (t.bid_px + t.ask_px) * 0.5;

    st.mids[st.head] = mid;
    st.head = (st.head + 1) & 63;       // valid because WINDOW == 64
    if (st.count < 64) {
        ++st.count;
    }

    if (st.count < 64) {
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

    if (stddev < 1e-9) {
        return {};
    }

    const double z = (mid - mean) / stddev;
    const double abs_z = std::abs(z);

    if (st.position == 0) {
        if (z >= 2.0) {
            return {csot::Order{csot::Order::Side::SELL, t.symbol, t.bid_px, 1}};
        }
        if (z <= -2.0) {
            return {csot::Order{csot::Order::Side::BUY, t.symbol, t.ask_px, 1}};
        }
        return {};
    }

    if (st.position > 0 && abs_z <= 0.5) {
        return {csot::Order{csot::Order::Side::SELL, t.symbol, t.bid_px,
                            static_cast<uint32_t>(st.position)}};
    }

    if (st.position < 0 && abs_z <= 0.5) {
        return {csot::Order{csot::Order::Side::BUY, t.symbol, t.ask_px,
                            static_cast<uint32_t>(-st.position)}};
    }

    return {};
}
```

This implementation is deliberately not optimal. It recomputes the mean and variance from scratch every tick. You may replace it with rolling sums, SIMD, a structure-of-arrays layout, or any other implementation — as long as the emitted orders are identical.

---

## 7. Correctness Rules

A submission is correct if, for the same tick file and fill model, it emits the exact same order stream as the judge reference implementation.

Order-stream equality means the same sequence of:

```text
tick_index, timestamp_ns, symbol, side, price, qty
```

with exact equality for:

- `tick_index`
- `timestamp_ns`
- `symbol`
- `side`
- `qty`

For `price`, exact equality is expected when using the tick's `bid_px` or `ask_px`. Do not recompute or round the price; copy the tick field directly.

### Invalid orders

The following are invalid and cause the submission to fail correctness:

- `qty == 0`
- `symbol` does not match `t.symbol`
- BUY order with price other than `t.ask_px`
- SELL order with price other than `t.bid_px`
- More than one order returned from a single `on_tick`
- Any order during warm-up (`count < 64`)
- Any position whose absolute value would exceed `1`

---

## 8. Fill Model

The Week-1 engine and the judge use the same simple deterministic fill model:

- Every valid order returned from `on_tick` is filled immediately.
- Fill price equals the submitted order's `price`.
- Fill quantity equals the submitted order's `qty`.
- After the fill, the engine calls:

```cpp
strategy.on_fill(order, order.price, order.qty);
```

This keeps the challenge focused on low-latency implementation rather than market microstructure.

---

## 9. Performance Rules

Correctness is binary. Performance is ranked among correct submissions only.

The leaderboard ranks by:

1. lower p50 `on_tick` latency
2. lower p99 `on_tick` latency
3. higher ticks/second throughput
4. lower p999 latency

Exact weighting is defined in [`../../PLATFORM.md`](../../PLATFORM.md). Trading performance is not part of the score.

### Hot-path expectations

By the end of Week 1, a straightforward implementation is fine. By later weeks, the same algorithm should evolve:

| Week | Expected implementation focus |
|---|---|
| 1 | Correct synchronous implementation, latency histogram, baseline measurement |
| 2 | Zero allocations after `on_init`, rolling sums, compile-time constants |
| 3 | Per-symbol state layout, false-sharing avoidance, core pinning |
| 4 | Lock-free event handoff around the strategy loop |
| 5 | Network ingest without disturbing the strategy hot path |

---

## 10. Common Pitfalls

- **Updating position inside `on_tick`:** wrong. Update only in `on_fill`.
- **Trading before 64 samples:** wrong. Append first; the 64th tick is tradable, the first 63 are not.
- **Using sample standard deviation:** wrong. Denominator is `64`, not `63`.
- **Returning a market order:** wrong for this spec. Use `t.ask_px` for BUY and `t.bid_px` for SELL.
- **Returning multiple orders:** wrong. At most one order per tick.
- **Comparing `abs(z) < 0.5` instead of `<= 0.5`:** wrong. Boundaries are inclusive.
- **Tuning the thresholds:** wrong. `2.0` and `0.5` are fixed.
- **Assuming only four symbols:** wrong. Public data has four; hidden data may have more. Support at least 64.

---

## 11. Why This Is Still a Quant Platform

The data, API, and runtime are trading-shaped:

- market ticks arrive in timestamp order
- strategies receive bid/ask snapshots
- orders are emitted and filled
- positions are tracked per symbol
- the final system eventually ingests ticks over TCP/UDP

But the contest objective is pure low latency. Everyone implements the same trading rule. The trading domain supplies the workload; the leaderboard measures systems engineering.

That distinction is the whole challenge.
