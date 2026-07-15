# Orderbook

A limit order book / matching engine in C++20, with price–time priority matching, five order types, thread-safe order expiry, a data-driven test suite, and a latency microbenchmark.

## Features

- **Price–time priority matching** — orders match best price first; at equal prices, first-come-first-served (FIFO per level)
- **Five order types:**

  | Type | Behaviour |
  |---|---|
  | `GoodTillCancel` | Rests in the book until filled or cancelled |
  | `FillAndKill` | Fills what it can immediately, remainder is cancelled |
  | `FillOrKill` | Fills completely or not at all — checked upfront in O(levels) via incremental per-level quantity aggregates |
  | `GoodForDay` | Auto-cancelled at 16:00 by a background prune thread |
  | `Market` | Converted to a `GoodTillCancel` at the worst opposite price, guaranteeing it can sweep the whole book |

- **O(1) cancel/lookup** — each order's list iterator is cached in a hash map, so cancelling never searches
- **Thread safety** — mutex-guarded public API; the `GoodForDay` prune thread coordinates shutdown via a condition variable and atomic flag
- **Modify (cancel-replace)** — atomic lookup of the existing order's type, then cancel + re-add

## Layout

```
Orderbook.h / orderbook.cpp   the matching engine (declarations / definitions)
Order.h                       an order: type, id, side, price, quantities
OrderModify.h                 cancel-replace request
Trade.h / TradeInfo.h         result of a match (bid side + ask side)
OrderbookLevelInfos.h         aggregated per-level view of the book
Usings.h / Constants.h        type aliases and sentinels
main.cpp                      demo entry point
tests/                        GoogleTest suite + scenario files
bench/                        latency microbenchmark
```

## Build & run

Requires a C++20 compiler ([MSYS2](https://www.msys2.org/) g++ on Windows) and GoogleTest for the test suite (`pacman -S mingw-w64-ucrt-x86_64-gtest`).

```sh
# app
g++ -std=c++20 -Wall orderbook.cpp main.cpp -o orderbook

# tests (run from tests/ — scenario files are resolved relative to the cwd)
g++ -std=c++20 -I. orderbook.cpp tests/test.cpp -lgtest -lgtest_main -o tests/tests
cd tests && ./tests

# benchmark
g++ -std=c++20 -O2 -I. orderbook.cpp bench/benchmark.cpp -o bench/benchmark
./bench/benchmark
```

## Tests

The suite is data-driven: each scenario is a text file of actions replayed against a fresh book, with the final expected state on the last line.

```
A B GoodTillCancel 100 10 1     add a buy: GTC, price 100, qty 10, id 1
A S GoodTillCancel 100 10 2     add a crossing sell -> they match
R 0 0 0                         expect: 0 orders, 0 bid levels, 0 ask levels
```

Scenarios cover matching for each order type, cancels, modifies, market sweeps, and regressions for the bugs below.

## Benchmark

Per-operation latency on a book pre-seeded with **100,000 resting orders across 1,000 price levels** (5,000 samples per operation, `steady_clock`, g++ `-O2`, Ryzen 5 5600H, Windows 11):

| Operation | mean | p50 | p99 | throughput |
|---|---|---|---|---|
| Limit add (resting) | 198 ns | 100 ns | 700 ns | ~5.0M ops/sec |
| Cancel | 235 ns | 200 ns | 700 ns | ~4.3M ops/sec |
| Market order (matching) | 653 ns | 600 ns | 1.9 µs | ~1.5M ops/sec |
| Modify (cancel-replace) | 901 ns | 800 ns | 1.8 µs | ~1.1M ops/sec |

Caveat: Windows' `steady_clock` ticks at ~100 ns, so single-sample p50s at that scale sit at the measurement floor; means and p99s are the trustworthy columns.

The benchmark caught a real bug: `MatchOrders()` originally called `trades.reserve(orders_.size())` — a ~3 MB allocation on *every* add against a 100k-order book, even when nothing matched. Removing it cut resting-add latency from 15.8 µs to 198 ns (~80×).

## Bugs found in the reference implementation

This project started from the design in [Tzadiko/Orderbook](https://github.com/Tzadiko/Orderbook) (built as a learning exercise). Reading it critically turned up three latent bugs, all fixed here and covered by regression tests:

1. **Deadlock** — `MatchOrders()` runs while `AddOrder` holds the mutex, and called the public `CancelOrder`, which locks the same non-recursive mutex again. Triggered the first time a `FillAndKill` order rests. Fixed by splitting cancel into a locking public wrapper and a lock-free internal (`CancelOrderInternal`).
2. **Dangling reference / UB** — the match loop held `auto&` references to the front of the order list, then `pop_front()`-ed and kept using them. Fixed by copying the `shared_ptr` (which is what shared ownership is for).
3. **Level-aggregate corruption** — `MatchOrders()` erased the per-level aggregate by price when one side's level emptied. `data_` is keyed by price alone and shared by both sides, so when a converted market order briefly rested at a price shared with the opposite side, the erase wiped the *other* side's aggregate too, making `FillOrKill` orders get wrongly rejected. Fixed by letting the count-based cleanup in `UpdateLevelData` handle removal. Regression: `tests/TestFiles/Match_FillOrKill_AfterMarket.txt`.

## Known limitations

- A market order that only partially fills leaves its remainder resting as `GoodTillCancel` at the sweep-boundary price; real venues typically cancel the remainder.
- The single mutex serializes all operations — fine at these throughputs, but a real engine would use a single-threaded core with lock-free queues at the edges.
- `GoodForDay` expiry uses the local clock's 16:00 with no exchange-calendar awareness.

## Acknowledgements

- Design based on [Tzadiko/Orderbook](https://github.com/Tzadiko/Orderbook)
- Benchmark methodology inspired by [engineswap/orderbook](https://github.com/engineswap/orderbook)
