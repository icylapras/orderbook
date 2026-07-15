#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "Orderbook.h"

using Clock = std::chrono::steady_clock;

namespace
{

struct Stats
{
    double meanNs;
    std::uint64_t p50Ns;
    std::uint64_t p99Ns;
    double opsPerSec;
};

Stats Summarize(std::vector<std::uint64_t>& timesNs)
{
    std::sort(timesNs.begin(), timesNs.end());

    std::uint64_t total = 0;
    for (auto t : timesNs)
        total += t;

    const double mean = static_cast<double>(total) / timesNs.size();
    return Stats{
        mean,
        timesNs[timesNs.size() / 2],
        timesNs[timesNs.size() * 99 / 100],
        1e9 / mean,
    };
}

void Report(const std::string& name, Stats stats)
{
    std::cout << name << ":\n"
              << "  mean " << stats.meanNs << " ns"
              << " | p50 " << stats.p50Ns << " ns"
              << " | p99 " << stats.p99Ns << " ns"
              << " | " << static_cast<std::uint64_t>(stats.opsPerSec) << " ops/sec\n";
}

//time a single call; the lambda returns nothing
template <typename F>
std::uint64_t TimeNs(F&& operation)
{
    const auto start = Clock::now();
    operation();
    const auto end = Clock::now();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
}

}

int main()
{
    constexpr int LevelsPerSide = 500;
    constexpr int OrdersPerLevel = 100;
    constexpr Price BestBid = 999;
    constexpr Price BestAsk = 1000;

    std::mt19937 rng{ 42 };//fixed seed so runs are comparable
    std::uniform_int_distribution<Quantity> qtyDist{ 100, 1000 };

    Orderbook orderbook;
    OrderId nextId = 1;

    //deep orders: far from the touch, guaranteed untouched by market sweeps,
    //so cancels/modifies below always hit a live order
    std::vector<OrderId> deepBidIds;
    std::vector<Price> deepBidPrices;

    for (int level = 0; level < LevelsPerSide; ++level)
    {
        const Price bidPrice = BestBid - level;
        const Price askPrice = BestAsk + level;

        for (int i = 0; i < OrdersPerLevel; ++i)
        {
            const OrderId bidId = nextId++;
            orderbook.AddOrder(std::make_shared<Order>(
                OrderType::GoodTillCancel, bidId, Side::Buy, bidPrice, qtyDist(rng)));

            if (level >= LevelsPerSide / 2)
            {
                deepBidIds.push_back(bidId);
                deepBidPrices.push_back(bidPrice);
            }

            orderbook.AddOrder(std::make_shared<Order>(
                OrderType::GoodTillCancel, nextId++, Side::Sell, askPrice, qtyDist(rng)));
        }
    }

    std::cout << "Seeded book: " << orderbook.Size() << " resting orders across "
              << LevelsPerSide * 2 << " price levels\n\n";

    //shuffle so cancels/modifies hit prices in random order, like real flow
    std::vector<std::size_t> deepIndices(deepBidIds.size());
    for (std::size_t i = 0; i < deepIndices.size(); ++i)
        deepIndices[i] = i;
    std::shuffle(deepIndices.begin(), deepIndices.end(), rng);

    constexpr int NumOps = 5000;
    std::vector<std::uint64_t> times;
    times.reserve(NumOps);

    // 1) Resting limit adds: inside the book, never crossing
    std::uniform_int_distribution<Price> restingBidPrice{ BestBid - 400, BestBid - 1 };
    for (int i = 0; i < NumOps; ++i)
    {
        auto order = std::make_shared<Order>(
            OrderType::GoodTillCancel, nextId++, Side::Buy, restingBidPrice(rng), qtyDist(rng));
        times.push_back(TimeNs([&] { orderbook.AddOrder(order); }));
    }
    Report("Limit add (resting)", Summarize(times));
    times.clear();

    // 2) Modifies (cancel-replace at same price, new quantity)
    for (int i = 0; i < NumOps; ++i)
    {
        const auto idx = deepIndices[i];
        OrderModify modify{ deepBidIds[idx], Side::Buy, deepBidPrices[idx], qtyDist(rng) };
        times.push_back(TimeNs([&] { orderbook.MatchOrder(modify); }));
    }
    Report("Modify (cancel-replace)", Summarize(times));
    times.clear();

    // 3) Cancels: same deep orders, guaranteed alive
    for (int i = 0; i < NumOps; ++i)
    {
        const OrderId id = deepBidIds[deepIndices[i]];
        times.push_back(TimeNs([&] { orderbook.CancelOrder(id); }));
    }
    Report("Cancel", Summarize(times));
    times.clear();

    // 4) Market orders: sweep the touch, alternating sides to keep the book balanced
    std::uniform_int_distribution<Quantity> marketQtyDist{ 100, 2000 };
    for (int i = 0; i < NumOps; ++i)
    {
        const Side side = (i % 2 == 0) ? Side::Buy : Side::Sell;
        auto order = std::make_shared<Order>(nextId++, side, marketQtyDist(rng));
        times.push_back(TimeNs([&] { orderbook.AddOrder(order); }));
    }
    Report("Market order (matching)", Summarize(times));

    std::cout << "\nBook after benchmark: " << orderbook.Size() << " resting orders\n";

    return 0;
}
