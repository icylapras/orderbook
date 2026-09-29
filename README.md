# Orderbook

[![CI](https://github.com/icylapras/orderbook/actions/workflows/ci.yml/badge.svg)](https://github.com/icylapras/orderbook/actions/workflows/ci.yml)

A C++20 limit order book and matching engine, driven by a full day of real NASDAQ TotalView-ITCH 5.0 data. It has two engines that share one set of semantics: the original `std::map`/`shared_ptr` design and a rewrite built for latency. Every speed-up claimed here was measured, and every result was checked against NASDAQ's own executions.

**Headline (AAPL, full day 2019-12-30, 1.51M book messages, median of 5 runs):**

| engine | p50 | p99 | p99.9 | throughput | heap allocs / msg |
|---|---|---|---|---|---|
| `Orderbook`: `std::map` levels + `std::list` + `shared_ptr` + mutex | 143 ns | 540 ns | 1,055 ns | 7.2 M msgs/s | 1.82 |
| `FastOrderbook`: tick-ladder levels + pool + intrusive list | **51 ns** | **223 ns** | **609 ns** | **36.1 M msgs/s** | **0** |
| speed-up | 2.8× | 2.4× | 1.7× | **5.0×** | |

**Full NASDAQ day, all 8,892 symbols (263M book messages):** 6.1M msgs/s for fast versus 1.1M for the baseline, a **5.5×** speed-up. The two engines agree on every symbol at every checkpoint.

Latencies are per message, timed with `rdtsc`, and include ~20 ns of timer overhead. Test setup: Ryzen 5 5600H, WSL2 Ubuntu 24.04, g++ 13.3, `-O3 -march=native`, pinned to one core, with warm-up runs first.

## What's here

```
src/Orderbook.*        baseline engine: std::map levels, std::list queues, shared_ptr orders, mutex,
                       background GoodForDay expiry thread
src/FastOrderbook.*    same semantics, rebuilt for latency (see "Design")
src/PriceLadder.h      tick-indexed price levels with a two-level occupancy bitmap
src/OrderIdMap.h       open-addressing hash map (linear probing, backward-shift delete)
src/SpscQueue.h        lock-free single-producer/single-consumer ring (+ mutex queue to compare)
src/itch/              ITCH 5.0 decoder, file streaming, encoder, event collector
src/perf/Latency.h     serialized rdtsc timer, log-linear histogram, core pinning
tools/replay.cpp       replays an ITCH day through both engines: validation + latency + throughput
tools/itchgen.cpp      synthetic but internally consistent ITCH feed (used by CI)
tools/itchfilter.cpp   extracts one symbol from a full-day file (for profiling)
tools/demo.cpp         small printable demo of the matching engine
bench/                 per-operation microbenchmark; SPSC-vs-mutex handoff benchmark
tests/                 GoogleTest: scenarios, property/differential fuzzing, ITCH, components, concurrency
fuzz/                  libFuzzer target: random bytes -> parser -> both engines
```

## Matching engine

- **Price-time priority.** Best price matches first; at equal prices, first in is first out.
- **Five order types:** `GoodTillCancel`, `FillAndKill`, `FillOrKill`, `GoodForDay` (expired at 16:00), and `Market` (swept against the book via a limit at the worst opposite price).
- **Market-data operations** for replaying a venue's feed: insert, reduce (partial cancel or execution, keeps queue position), replace (new id, back of the queue) and delete. These *mirror* the venue's book and never match. The venue already did the matching, and a halted symbol's book can legitimately be crossed.

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

### How correctness is checked (not just speed)

Every check below passes on the full 2019-12-30 file:

- **Every execution hit our best bid/ask: 60,543 / 60,543 for AAPL.** An incoming order always takes the best price first. So when NASDAQ reports an order as executed, that order must be sitting at our reconstructed top of book at that instant.
- **Hidden-order prints fell inside our spread: 7,178 / 7,178.**
- **No reference ever pointed at an unknown order** across the whole day, so the book never drifted from NASDAQ's.
- **The two engines agree.** Top of book matched after every one of 1.5M AAPL messages, and full depth matched at 151 sampled points. Across all symbols, top of book and order counts matched at 63 checkpoints.
- **The book is crossed only when it should be.** Across all 8,892 symbols, 8,649 messages left a book crossed. Every one happened while NASDAQ had that symbol halted, LULD-paused, or quotation-only, or within milliseconds of its reopening cross. Measured, the crossings started at most 1.2 ms after the resume and cleared within 0.2 ms.
- **AAPL's NASDAQ prints match published data.** The closing cross is $291.52, equal to our best bid at the close. The open cross ($289.44), day high ($292.69), day low ($285.23) and close all sit at the same constant ratio (4.156) to published split- and dividend-adjusted daily data, which is consistent with Apple's 4:1 split plus dividend adjustment.

## Design: what made it fast (measured, in order)

`perf stat` per message (AAPL, measured differentially: 30 replays minus 10, divided by 20 × messages):

| per message | cycles | instructions | IPC | cache-misses | L1d misses | branch-misses |
|---|---|---|---|---|---|---|
| baseline | 564 | 931 | 1.65 | 2.85 | 8.66 | 4.58 |
| fast, sorted-vector levels | 188 | 301 | 1.60 | 0.86 | 3.32 | 2.32 |
| fast, tick ladder (final) | **123** | **262** | **2.13** | **0.83** | **2.39** | **1.44** |

1. **No allocation on the hot path.** Orders and levels live in index-addressed pools with free lists. `replay` replaces global `operator new` with a counting version, so "0 allocations per message" is measured, not claimed (the baseline makes 1.82 per message).
2. **Intrusive FIFO queues.** Each level's queue is a doubly-linked list threaded through the order pool with 32-bit indices. An order is 32 bytes, so two fit per cache line. Each order knows its level, so a cancel is one hash lookup plus an unlink.
3. **An open-addressing id map** replaces `std::unordered_map`, which allocates a node per insert. It uses linear probing with backward-shift deletion, so there are no tombstones to accumulate over a trading day.
4. **Size for the working set, not the day.** The first version reserved the id map for every add of the day (~800k). That spread lookups across a 32 MB table and made the engine only 1.1× faster. AAPL's peak of *live* orders is 27,110. Sizing for that gave 2.7×.
5. **A tick-indexed price ladder instead of a sorted vector.** `perf record` showed 22% of time in `memmove` and 13% in binary search. Instrumenting the book showed that a third of all level creations and deletions happen **256–1,023 levels away from the touch**: someone continuously layers orders $2.50–$10 deep in a 4,212-level book. The "best price at the back of a sorted vector" trick assumes activity clusters at the top, and real data breaks that assumption. `PriceLadder` indexes levels by tick within a ±2,048-tick band, with a bitmap plus a summary bitmap, so finding the next best price takes two `countl_zero` instructions. Off-grid and far-away prices (stub quotes such as a $0.01 bid) fall back to the sorted vector, so any price is still valid. The band's `/ tick` uses a precomputed multiply-shift, which is proven exact over the band and tested exhaustively. Result: **31–36M msgs/s**.

**Full-market caveat.** Across all symbols, throughput is memory-bound: IPC is 0.40, with ~9 cache misses per message, because consecutive messages usually touch different books. The next step would be software prefetching of the next message's book and order, since the replay decodes messages in chunks ahead of time.

### Network thread → matching thread: SPSC ring vs mutex queue

A producer thread (the "feed handler") stamps each AAPL event and pushes it. A consumer thread owns the `FastOrderbook`, pops, applies, and records the time from stamp to applied. The two threads are pinned to different cores.

| queue | saturated throughput | paced (1 msg/µs) p50 | p99 |
|---|---|---|---|
| `std::mutex` + `std::deque` | 3.4 M msgs/s | 435 ns | 16.7 µs |
| lock-free SPSC ring | **6.0 M msgs/s** | **225 ns** | **7.4 µs** |

The ring keeps head and tail on separate cache lines, and each side caches the other's index, so a push or pop only touches shared state when the queue looks full or empty. Acquire/release ordering is the entire synchronisation protocol, and TSan checks it in CI.

### Microbenchmark (synthetic, includes matching)

The book is pre-seeded with 100k orders over 1,000 levels, with 20k samples per operation. `./build/bench_engine --pin 3`:

| operation | baseline p50 / p99 | fast p50 / p99 |
|---|---|---|
| limit add (resting) | 176 / 1011 ns | 142 / 558 ns |
| cancel | 197 / 664 ns | 76 / 405 ns |
| modify (cancel-replace) | 384 / 1138 ns | 187 / 685 ns |
| market order (sweeps levels) | 326 / 1022 ns | 229 / 790 ns |

## Testing

59 GoogleTest tests run in every CI configuration:

- **Scenario tests:** the original 8 data-driven scenario files, run against the baseline and three `FastOrderbook` layouts (ladder, sorted-vector only, and tick 3 with a tiny band, which forces off-grid and out-of-band prices).
- **Property / differential fuzzing** against `ReferenceBook`, a deliberately naive model (a flat vector with a linear scan for everything). Random streams of all order types plus cancels, modifies, reduces, market-data inserts and replaces are applied in lockstep. After *every* operation the test checks:
  - identical trades, trade by trade;
  - identical depth at every level;
  - the book is never crossed (unless passive inserts were used);
  - **quantity is conserved**: a GTC add changes resting quantity by exactly `qty − 2·traded`, a FAK remainder never rests, and FOK is all-or-nothing.

  There are 65 seeds per engine, plus a 200k-operation head-to-head between the two engines.
- **ITCH:** every message type round-trips through an encoder and the parser. Also tested: spec message lengths, big-endian decode, malformed and unknown frames, a 61-byte streaming buffer (so messages straddle every boundary), truncated files, and a mini session replayed into every engine.
- **Components:** the id map against `std::unordered_map` over 500k operations; `PriceLadder` against `std::map`; histogram error bounds against an exact sort; SPSC FIFO/wrap-around; 2M items across two real threads.
- **Concurrency (for TSan):** the expiry thread running alongside 4 caller threads, cross-thread matching with conservation checks, and 2,000 construct/destroy cycles.
- **Mutation testing:** 14 deliberately injected bugs (in matching, FOK/FAK, the id map, the ladder bitmap, tick division, and ITCH decoding) were **all caught**.

### Bugs found along the way

- **Lost wakeup in the baseline's shutdown.** The destructor set `shutdown_` and notified *without* holding the mutex. A notify landing between the expiry thread's check and its `wait_for` would leave `join()` blocked until 16:00. Fixed by setting the flag under the mutex, and a spurious wakeup no longer ends the thread (predicate `wait_for`).
- **The baseline didn't build on Linux** (`localtime_s`).
- **FillAndKill remainder could rest** in the baseline once a book can be crossed. Cleanup only looked at the front of the best level. The crossed-book fuzz profile found this.
- **Per-level totals keyed by price alone** in the baseline would merge a bid and an ask level at the same price (a crossed or locked book).
- **A matching engine can't replay a feed.** An early version re-matched ITCH adds and "traded" 1,578 times across 15 halted/paused symbols. The replay now mirrors the venue's book instead.

## CI (GitHub Actions)

| job | what it runs |
|---|---|
| build + test (g++, clang++) | Release build, all tests, synthetic-day replay with full validation, benchmark smoke runs |
| ASan + UBSan | Debug build, all tests, replays |
| TSan | all tests (expiry thread, concurrent callers, both queues across threads), queue benchmark |
| libFuzzer | 60 s of random ITCH bytes through the parser into both engines, which must agree |
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
perf stat -e cycles,instructions,cache-misses,L1-dcache-load-misses ./build/replay aapl.itch --engine fast --no-validate
```

## Known limitations

- The price-ladder band is centred on a book's first order and never moves. A stock that trades more than ±2,048 ticks away from where it started falls back to the slower sorted-vector path. That is still correct, just slower.
- A market order that only partially fills rests its remainder as `GoodTillCancel` at the sweep-boundary price. Real venues usually cancel the remainder.
- The baseline's single mutex serialises everything. `FastOrderbook` is single-threaded by design and is fed through the SPSC queue.
- `GoodForDay` expiry uses the local clock's 16:00 (baseline) or an explicit call (fast), with no exchange-calendar awareness.
