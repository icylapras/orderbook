# Orderbook

[![CI](https://github.com/icylapras/orderbook/actions/workflows/ci.yml/badge.svg)](https://github.com/icylapras/orderbook/actions/workflows/ci.yml)

A low-latency C++20 limit order book and matching engine, driven by a full day of real NASDAQ TotalView-ITCH 5.0 market data. The book reconstructed from the feed is checked message by message against NASDAQ's own executions.

**AAPL, full trading day (2019-12-30), 1.51M book messages:**

| p50 | p99 | p99.9 | throughput | heap allocations |
|---|---|---|---|---|
| 51 ns | 223 ns | 609 ns | **36 M msgs/sec** | **0 per message** |

**Every NASDAQ symbol, full day (263M book messages, 8,892 symbols):** 6.1M msgs/sec on one core.

Latencies are per message, timed with `rdtsc`, include ~20 ns of timer overhead, and are the median of 5 runs. Test setup: Ryzen 5 5600H, WSL2 Ubuntu 24.04, g++ 13.3, `-O3 -march=native`, pinned to one core, with warm-up runs first.

## What's here

```
src/Orderbook.*        the engine: matching + market-data operations
src/PriceLadder.h      tick-indexed price levels with a two-level occupancy bitmap
src/OrderIdMap.h       open-addressing hash map (linear probing, backward-shift delete)
src/SpscQueue.h        lock-free single-producer/single-consumer ring buffer
src/itch/              ITCH 5.0 decoder, file streaming, encoder, event collector
src/perf/Latency.h     serialized rdtsc timer, log-linear latency histogram, core pinning
tools/replay.cpp       replays an ITCH day: validation, latency histogram, throughput
tools/itchgen.cpp      synthetic but internally consistent ITCH feed (used by CI)
tools/itchfilter.cpp   extracts one symbol from a full-day file (for profiling)
tools/demo.cpp         small printable demo of the matching engine
bench/                 per-operation microbenchmark; network-thread -> matching-thread queue benchmark
tests/                 GoogleTest: scenarios, property/differential fuzzing, ITCH, components
fuzz/                  libFuzzer target: random bytes -> parser -> book
```

## Matching engine

- **Price-time priority.** Best price matches first; at equal prices, first in is first out.
- **Five order types:** `GoodTillCancel`, `FillAndKill`, `FillOrKill`, `GoodForDay`, and `Market` (swept against the book via a limit at the worst opposite price).
- **Market-data operations** for replaying a venue's feed: insert, reduce (partial cancel or execution, keeps queue position), replace (new id, back of the queue) and delete. These *mirror* the venue's book and never match. The venue already did the matching, and a halted symbol's book can legitimately be crossed.
- **Single-threaded by design.** One thread owns the book, and other threads hand it work through the SPSC queue. There are no locks on the hot path, and `GoodForDay` expiry is an explicit call driven by the session, not a background thread.

## Design

- **No allocation on the hot path.** Orders and price levels live in index-addressed pools with free lists, and freed slots are reused while still warm in cache. `replay` replaces global `operator new` with a counting version, so "0 allocations per message" is measured, not assumed.
- **Intrusive FIFO queues.** Each price level's queue is a doubly-linked list threaded through the order pool with 32-bit indices. An order is 32 bytes, so two fit per cache line. Each order knows its level, so a cancel is one hash lookup plus an unlink, with no search.
- **Open-addressing id map.** Linear probing keeps a lookup to usually one cache line. Backward-shift deletion means no tombstones accumulate over a trading day. The map is sized for the peak number of *live* orders, not the day's total, which keeps it in cache.
- **Tick-indexed price ladder.** Levels within a ±2,048-tick band around the market sit in an array indexed by tick, so lookup, insert and erase are O(1). A bitmap marks which levels exist, and a summary bitmap marks which bitmap words are non-empty. Finding the next best price is two `countl_zero`/`countr_zero` instructions, however sparse the book. The `price / tick` index uses a precomputed multiply-shift instead of a division; it is proven exact over the band and tested exhaustively.

  Why a ladder: real books are deep. AAPL reaches 4,212 bid levels, and a third of all level creations and deletions happen 256–1,023 levels away from the best price. A structure that's only fast near the top of book would pay for that constantly.

  Prices that are off the tick grid or far outside the band (stub quotes such as a $0.01 bid) fall back to a sorted vector, so any price is valid.
- **Measured with `perf`:** 123 cycles, 262 instructions (IPC 2.1), 0.83 cache misses and 1.4 branch misses per message on the AAPL replay.
- **Full-market caveat.** Across all symbols, throughput is memory-bound (IPC 0.40, ~9 cache misses per message), because consecutive messages usually touch different books. The next step would be software prefetching of the next message's book and order; the replay already decodes messages in chunks ahead of time.

## ITCH 5.0 replay

```sh
# data: https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/ (e.g. 12302019.NASDAQ_ITCH50.gz, 3.5 GB)
pigz -dk 12302019.NASDAQ_ITCH50.gz

./build/replay 12302019.NASDAQ_ITCH50 --symbol AAPL --pin 3      # validate + latency + throughput
./build/replay 12302019.NASDAQ_ITCH50 --all --strict             # every symbol, checked per message
```

The file is a sequence of length-prefixed, big-endian messages. The book is driven by these message types:

| ITCH | meaning | book operation |
|---|---|---|
| `A` / `F` | Add Order (without / with MPID) | insert resting order |
| `E` / `C` | Order Executed (without / with price) | reduce by executed shares |
| `X` | Order Cancel (partial) | reduce by cancelled shares |
| `D` | Order Delete | remove |
| `U` | Order Replace | remove, then insert under the new reference (same side) |
| `H` | Stock Trading Action | trading state (used by validation) |

The parser is zero-copy. It decodes each message into a small struct and calls only the handler callbacks that exist, checked at compile time with `requires`-expressions. Framing errors and unknown types are counted and skipped, and messages that straddle a read buffer are carried over to the next buffer.

### How correctness is checked

Every check below passes on the full 2019-12-30 file:

- **Every execution hit our best bid/ask: 60,543 / 60,543 for AAPL.** An incoming order always takes the best price first. So when NASDAQ reports an order as executed, that order must be sitting at our reconstructed top of book at that instant.
- **Hidden-order prints fell inside our spread: 7,178 / 7,178.**
- **No reference ever pointed at an unknown order** across the whole day, so the book never drifted from NASDAQ's.
- **The book agrees with an independent rebuild.** A deliberately simple mirror (a hash map of orders plus a `std::map` of levels) shares no code with the engine. Top of book matched after every one of 1.5M AAPL messages, and full depth matched at every sample. Across all symbols, every symbol's top of book and order count matched at 63 checkpoints.
- **The book is crossed only when it should be.** Across all 8,892 symbols, 8,649 messages left a book crossed. Every one happened while NASDAQ had that symbol halted, LULD-paused, or quotation-only, or within milliseconds of its reopening cross. Measured, the crossings started at most 1.2 ms after the resume and cleared within 0.2 ms.
- **AAPL's NASDAQ prints match published data.** The closing cross is $291.52, equal to our best bid at the close. The open cross ($289.44), day high ($292.69), day low ($285.23) and close all sit at the same constant ratio (4.156) to published split- and dividend-adjusted daily data, which is consistent with Apple's 4:1 split plus dividend adjustment.

## Network thread → matching thread

`bench_queue` models the handoff between a feed-handler thread and the thread that owns the book. The producer stamps each AAPL event and pushes it; the consumer pops, applies and records the time from stamp to applied. The two threads are pinned to different cores.

The SPSC ring keeps head and tail on separate cache lines, and each side caches the other's index, so a push or pop only touches shared state when the queue looks full or empty. Acquire/release ordering is the entire synchronisation protocol, and ThreadSanitizer checks it in CI. A mutex-guarded `std::deque` with the same interface is included as the conventional alternative. The ring gives 1.7× the throughput and about half the handoff latency (p50 225 ns vs 435 ns).

## Testing

- **Scenario tests:** data-driven scenario files, run against all three price-level layouts (ladder, sorted-vector only, and tick 3 with a tiny band, which forces off-grid and out-of-band prices).
- **Property / differential fuzzing** against `ReferenceBook`, a deliberately naive model (a flat vector with a linear scan for everything). Random streams of all order types plus cancels, modifies, reduces, market-data inserts and replaces are applied in lockstep. After *every* operation the test checks:
  - identical trades, trade by trade;
  - identical depth at every level;
  - the book is never crossed (unless passive inserts were used);
  - **quantity is conserved**: a GTC add changes resting quantity by exactly `qty − 2·traded`, a FAK remainder never rests, and FOK is all-or-nothing.
- **ITCH:** every message type round-trips through an encoder and the parser. Also tested: spec message lengths, big-endian decode, malformed and unknown frames, a 61-byte streaming buffer (so messages straddle every boundary), truncated files, and a mini session replayed into the book.
- **Components:** the id map against `std::unordered_map` over 500k operations; `PriceLadder` against `std::map`; histogram error bounds against an exact sort; SPSC FIFO/wrap-around; 2M items across two real threads.
- **Mutation testing:** 14 deliberately injected bugs (in matching, FOK/FAK, the id map, the ladder bitmap, tick division and ITCH decoding) were all caught.

## CI (GitHub Actions)

| job | what it runs |
|---|---|
| build + test (g++, clang++) | Release build, all tests, synthetic-day replay with full validation, benchmark smoke runs |
| ASan + UBSan | Debug build, all tests, replays |
| TSan | all tests (including cross-thread queue tests), queue benchmark |
| libFuzzer | 60 s of random ITCH bytes through the parser into two price-level layouts, which must agree |
| Windows (MSYS2 g++) | build + all tests |

CI can't download the 3.5 GB NASDAQ file, so `itchgen` produces a synthetic, internally consistent day for the replay jobs.

## Build

Requires CMake ≥ 3.20 and a C++20 compiler. GoogleTest is taken from the system if found, otherwise fetched.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release     # -O3 -march=native
cmake --build build
ctest --test-dir build --output-on-failure

# sanitizers / fuzzing
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=clang++ -DORDERBOOK_SANITIZE=address,undefined
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=clang++ -DORDERBOOK_SANITIZE=thread
cmake -S . -B build-fuzz -DCMAKE_CXX_COMPILER=clang++ -DORDERBOOK_FUZZ=ON -DORDERBOOK_SANITIZE=address,undefined

# profiling (Linux/WSL2)
./build/itchfilter 12302019.NASDAQ_ITCH50 aapl.itch --symbol AAPL
perf stat -e cycles,instructions,cache-misses,L1-dcache-load-misses ./build/replay aapl.itch --no-validate
```

## Known limitations

- The price-ladder band is centred on a book's first order and never moves. A stock that trades more than ±2,048 ticks away from where it started falls back to the slower sorted-vector path. That is still correct, just slower.
- A market order that only partially fills rests its remainder as `GoodTillCancel` at the sweep-boundary price. Real venues usually cancel the remainder.
- `GoodForDay` expiry is an explicit call, with no exchange-calendar awareness.
