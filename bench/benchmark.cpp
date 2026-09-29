//synthetic per-operation microbenchmark: the same workload against both
//engines on a book pre-seeded with 100,000 resting orders across 1,000
//price levels. (The ITCH replay tool is the real-data benchmark; this one
//isolates individual operation types, including matching, which a NASDAQ
//replay never exercises because NASDAQ's own book never crosses.)
//
//  bench_engine [--pin CPU]

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "FastOrderbook.h"
#include "Order.h"
#include "Orderbook.h"
#include "perf/Latency.h"

namespace
{

struct Baseline
{
    Orderbook book_{ false };
    void Add(OrderType t, OrderId id, Side s, Price p, Quantity q) { book_.AddOrder(std::make_shared<Order>(t, id, s, p, q)); }
    void Market(OrderId id, Side s, Quantity q) { book_.AddOrder(std::make_shared<Order>(id, s, q)); }
    void Cancel(OrderId id) { book_.CancelOrder(id); }
    void Modify(const OrderModify& m) { book_.MatchOrder(m); }
    std::size_t Size() const { return book_.Size(); }
};

struct Fast
{
    FastOrderbook book_{ 1 << 18 };
    void Add(OrderType t, OrderId id, Side s, Price p, Quantity q) { book_.AddOrder(t, id, s, p, q); }
    void Market(OrderId id, Side s, Quantity q) { book_.AddMarketOrder(id, s, q); }
    void Cancel(OrderId id) { book_.CancelOrder(id); }
    void Modify(const OrderModify& m) { book_.ModifyOrder(m); }
    std::size_t Size() const { return book_.Size(); }
};

struct Row
{
    std::string name_;
    perf::Summary summary_;
};

template <typename Engine>
std::vector<Row> Run(double ticksPerNs)
{
    constexpr int LevelsPerSide = 500;
    constexpr int OrdersPerLevel = 100;
    constexpr Price BestBid = 999;
    constexpr Price BestAsk = 1000;
    constexpr int NumOps = 20'000;

    std::mt19937 rng{ 42 };//fixed seed: both engines see the identical workload
    std::uniform_int_distribution<Quantity> qtyDist{ 100, 1000 };

    auto engine = std::make_unique<Engine>();
    OrderId nextId = 1;

    //deep orders: far from the touch, never reached by the market sweeps,
    //so cancels/modifies below always hit a live order
    std::vector<OrderId> deepBidIds;
    std::vector<Price> deepBidPrices;

    for (int level = 0; level < LevelsPerSide; ++level)
    {
        for (int i = 0; i < OrdersPerLevel; ++i)
        {
            const OrderId bidId = nextId++;
            engine->Add(OrderType::GoodTillCancel, bidId, Side::Buy, BestBid - level, qtyDist(rng));
            if (level >= LevelsPerSide / 2)
            {
                deepBidIds.push_back(bidId);
                deepBidPrices.push_back(BestBid - level);
            }
            engine->Add(OrderType::GoodTillCancel, nextId++, Side::Sell, BestAsk + level, qtyDist(rng));
        }
    }

    std::vector<std::size_t> deepIndices(deepBidIds.size());
    for (std::size_t i = 0; i < deepIndices.size(); ++i)
        deepIndices[i] = i;
    std::shuffle(deepIndices.begin(), deepIndices.end(), rng);

    std::vector<Row> rows;
    auto time = [&](const char* name, auto&& op)
    {
        perf::Histogram h;
        for (int i = 0; i < NumOps; ++i)
        {
            const auto start = perf::StartTicks();
            op(i);
            h.Record(perf::StopTicks() - start);
        }
        rows.push_back(Row{ name, perf::Summarize(h, ticksPerNs) });
    };

    std::uniform_int_distribution<Price> restingBidPrice{ BestBid - 400, BestBid - 1 };
    time("Limit add (resting)", [&](int) {
        engine->Add(OrderType::GoodTillCancel, nextId++, Side::Buy, restingBidPrice(rng), qtyDist(rng));
    });
    time("Modify (cancel-replace)", [&](int i) {
        const auto idx = deepIndices[static_cast<std::size_t>(i)];
        engine->Modify(OrderModify{ deepBidIds[idx], Side::Buy, deepBidPrices[idx], qtyDist(rng) });
    });
    time("Cancel", [&](int i) {
        engine->Cancel(deepBidIds[deepIndices[static_cast<std::size_t>(i)]]);
    });
    std::uniform_int_distribution<Quantity> marketQtyDist{ 100, 2000 };
    time("Market order (matching)", [&](int i) {
        engine->Market(nextId++, i % 2 == 0 ? Side::Buy : Side::Sell, marketQtyDist(rng));
    });

    return rows;
}

void Print(const char* engine, const std::vector<Row>& rows)
{
    std::printf("\n%s\n", engine);
    std::printf("  %-26s %8s %8s %8s %8s %9s\n", "operation", "mean", "p50", "p99", "p99.9", "max");
    for (const auto& r : rows)
        std::printf("  %-26s %6.0fns %6.0fns %6.0fns %6.0fns %7.1fus\n", r.name_.c_str(),
            r.summary_.mean_, r.summary_.p50_, r.summary_.p99_, r.summary_.p999_, r.summary_.max_ / 1000);
}

}

int main(int argc, char** argv)
{
    if (argc == 3 && std::string{ argv[1] } == "--pin")
        std::printf("pinned to CPU %s: %s\n", argv[2], perf::PinCurrentThread(std::atoi(argv[2])) ? "ok" : "FAILED");

    const double ticksPerNs = perf::CalibrateTicksPerNs();
    std::printf("Book pre-seeded with 100,000 resting orders over 1,000 levels; 20,000 samples per operation\n");

    //one untimed pass of each to warm caches, predictors and the allocator
    Run<Baseline>(ticksPerNs);
    Run<Fast>(ticksPerNs);

    Print("Orderbook (baseline: std::map + std::list + shared_ptr + mutex)", Run<Baseline>(ticksPerNs));
    Print("FastOrderbook (tick-ladder levels + pool + intrusive list)", Run<Fast>(ticksPerNs));
    return 0;
}
