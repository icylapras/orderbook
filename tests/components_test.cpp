//tests for the building blocks: id map, latency histogram, SPSC queue,
//and Orderbook-specific behaviour

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <random>
#include <thread>
#include <unordered_map>
#include <vector>

#include "Orderbook.h"
#include "OrderIdMap.h"
#include "PriceLadder.h"
#include "SpscQueue.h"
#include "perf/Latency.h"

TEST(OrderIdMap, MatchesUnorderedMapUnderRandomOperations)
{
    std::mt19937_64 rng{ 3 };
    OrderIdMap map;//starts tiny, so this also exercises many rehashes
    std::unordered_map<OrderId, std::uint32_t> model;

    //small key range forces collisions, re-inserts and long probe chains
    std::uniform_int_distribution<OrderId> key{ 0, 5'000 };
    for (int i = 0; i < 500'000; ++i)
    {
        const auto k = key(rng);
        switch (rng() % 3)
        {
        case 0:
        {
            const auto v = static_cast<std::uint32_t>(rng() % 1'000'000);
            ASSERT_EQ(map.Insert(k, v), model.emplace(k, v).second);
            break;
        }
        case 1:
            ASSERT_EQ(map.Erase(k), model.erase(k) == 1);
            break;
        default:
        {
            const auto it = model.find(k);
            ASSERT_EQ(map.Find(k), it == model.end() ? OrderIdMap::Missing : it->second);
            break;
        }
        }
        ASSERT_EQ(map.Size(), model.size());
    }

    for (const auto& [k, v] : model)
        ASSERT_EQ(map.Find(k), v);
}

TEST(OrderIdMap, BackwardShiftKeepsWrappedChainsReachable)
{
    //sequential keys in a full-ish small table wrap around the end of the
    //array; erasing from the middle of chains must not orphan anything
    OrderIdMap map{ 8 };
    for (OrderId k = 1; k <= 8; ++k)
        ASSERT_TRUE(map.Insert(k, static_cast<std::uint32_t>(k)));
    for (OrderId k = 1; k <= 8; k += 2)
        ASSERT_TRUE(map.Erase(k));
    for (OrderId k = 2; k <= 8; k += 2)
        ASSERT_EQ(map.Find(k), k);
    for (OrderId k = 1; k <= 8; k += 2)
        ASSERT_EQ(map.Find(k), OrderIdMap::Missing);
}

//the ladder against std::map as the model of "sorted set of price levels"
void LadderMatchesMap(Side side, Price tick, std::uint32_t band, Price centre, Price low, Price high, std::uint32_t seed)
{
    std::mt19937 rng{ seed };
    PriceLadder ladder{ side, tick, band };
    ladder.CentreBand(centre);
    std::map<Price, std::uint32_t> model;
    std::uniform_int_distribution<Price> price{ low, high };
    std::uint32_t nextSlot = 0;

    auto bestOf = [&](bool best) -> std::pair<Price, std::uint32_t> {
        if (model.empty())
            return { 0, PriceLadder::Nil };
        const bool highest = (side == Side::Buy) == best;
        return highest ? *model.rbegin() : *model.begin();
    };

    for (int step = 0; step < 20'000; ++step)
    {
        const auto p = price(rng);
        const auto it = model.find(p);
        ASSERT_EQ(ladder.Find(p), it == model.end() ? PriceLadder::Nil : it->second) << "step " << step;

        if (it == model.end())
        {
            ladder.Insert(p, nextSlot);
            model[p] = nextSlot++;
        }
        else if (rng() % 2)
        {
            ladder.Erase(p);
            model.erase(it);
        }

        const auto best = ladder.Best();
        const auto expectedBest = bestOf(true);
        ASSERT_EQ(best.slot_, expectedBest.second) << "step " << step;
        if (best)
        {
            ASSERT_EQ(best.price_, expectedBest.first) << "step " << step;
        }

        const auto worst = ladder.Worst();
        const auto expectedWorst = bestOf(false);
        ASSERT_EQ(worst.slot_, expectedWorst.second) << "step " << step;

        if (step % 500 == 0)
        {
            std::vector<std::pair<Price, std::uint32_t>> visited;
            ladder.ForEachFromBest([&](PriceLadder::Entry e) { visited.emplace_back(e.price_, e.slot_); return true; });
            std::vector<std::pair<Price, std::uint32_t>> expected(model.begin(), model.end());
            if (side == Side::Buy)
                std::reverse(expected.begin(), expected.end());
            ASSERT_EQ(visited, expected) << "step " << step;
        }
    }
}

TEST(PriceLadder, MatchesSortedMapInBand)
{
    for (const auto side : { Side::Buy, Side::Sell })
        LadderMatchesMap(side, 1, 4096, 1000, 0, 2000, 1);
}

TEST(PriceLadder, MatchesSortedMapAcrossBandEdgesAndOffGrid)
{
    //tick 7 and a 128-tick band around 5000: prices land in the band, off
    //the grid, and far outside it on both sides
    for (const auto side : { Side::Buy, Side::Sell })
        LadderMatchesMap(side, 7, 128, 5000, 3000, 7000, 2);
}

TEST(PriceLadder, MatchesSortedMapWithoutBand)
{
    for (const auto side : { Side::Buy, Side::Sell })
        LadderMatchesMap(side, 1, 0, 0, 0, 500, 3);
}

TEST(PriceLadder, SparseBandFindsLevelsFarApart)
{
    //levels thousands of ticks apart: next-best search has to cross many
    //empty bitmap words via the summary level
    PriceLadder ladder{ Side::Buy, 1, 1 << 16 };
    ladder.CentreBand(1 << 15);
    ladder.Insert(10, 1);
    ladder.Insert(40'000, 2);
    ladder.Insert(65'000, 3);
    EXPECT_EQ(ladder.Best().price_, 65'000);
    ladder.Erase(65'000);
    EXPECT_EQ(ladder.Best().price_, 40'000);
    ladder.Erase(40'000);
    EXPECT_EQ(ladder.Best().price_, 10);
    EXPECT_EQ(ladder.Worst().price_, 10);
    ladder.Erase(10);
    EXPECT_FALSE(ladder.Best());
    EXPECT_TRUE(ladder.Empty());
}

TEST(PriceLadder, TickDivisionIsExactAcrossTheBand)
{
    //the band index uses a multiply-shift instead of a divide; check it
    //against real division for every in-band offset at several tick sizes
    for (const Price tick : { 1, 3, 7, 100, 1000 })
    {
        PriceLadder ladder{ Side::Sell, tick, 4096 };
        ladder.CentreBand(0);
        const Price low = -2048 * tick;
        for (Price p = low; p < low + 4096 * tick; p += (tick > 100 ? 13 : 1))
        {
            const bool onGrid = (p - low) % tick == 0;
            ladder.Insert(p, 1);
            //an on-grid in-band price must be findable and not use the overflow
            ASSERT_EQ(ladder.Find(p), 1u) << tick << " " << p;
            ASSERT_EQ(ladder.OverflowSize(), onGrid ? 0u : 1u) << tick << " " << p;
            ladder.Erase(p);
        }
    }
}

TEST(Histogram, PercentilesWithinBucketPrecisionOfExactSort)
{
    std::mt19937_64 rng{ 11 };
    std::lognormal_distribution<double> latency{ 5.0, 1.0 };//long right tail, like real latencies
    perf::Histogram h;
    std::vector<std::uint64_t> exact;

    for (int i = 0; i < 200'000; ++i)
    {
        const auto v = static_cast<std::uint64_t>(latency(rng));
        h.Record(v);
        exact.push_back(v);
    }
    std::sort(exact.begin(), exact.end());

    for (const double q : { 0.5, 0.9, 0.99, 0.999 })
    {
        const auto truth = static_cast<double>(exact[static_cast<std::size_t>(q * static_cast<double>(exact.size() - 1))]);
        const auto approx = static_cast<double>(h.Percentile(q));
        //reported value is the bucket's upper edge: never below, at most 1/64 above
        EXPECT_GE(approx, truth) << q;
        EXPECT_LE(approx, truth * (1.0 + 1.0 / 64) + 1) << q;
    }
    EXPECT_EQ(h.Max(), exact.back());
    EXPECT_EQ(h.Count(), exact.size());
}

TEST(Histogram, BucketEdgesAreConsistent)
{
    const std::uint64_t values[] = { 0, 1, 63, 64, 65, 127, 128, 1000, 123456789, (1ull << 40) + 12345, UINT64_MAX };
    for (const auto v : values)
    {
        const auto index = perf::Histogram::Index(v);
        ASSERT_LT(index, perf::Histogram::BucketCount);
        ASSERT_GE(perf::Histogram::UpperEdge(index), v);
        if (index > 0)
        {
            ASSERT_LT(perf::Histogram::UpperEdge(index - 1), v);
        }
    }
}

TEST(SpscQueue, FifoWithWrapAround)
{
    SpscQueue<int> q{ 4 };
    int out = 0;
    EXPECT_FALSE(q.TryPop(out));

    for (int round = 0; round < 10; ++round)
    {
        for (int i = 0; i < 4; ++i)
            ASSERT_TRUE(q.TryPush(round * 10 + i));
        ASSERT_FALSE(q.TryPush(99)) << "full";
        for (int i = 0; i < 4; ++i)
        {
            ASSERT_TRUE(q.TryPop(out));
            ASSERT_EQ(out, round * 10 + i);
        }
        ASSERT_FALSE(q.TryPop(out)) << "empty";
    }
}

//two real threads; run under TSan in CI to prove the memory ordering
template <typename Queue>
void TransferInOrder()
{
    constexpr std::uint64_t Count = 2'000'000;
    Queue q{ 1024 };

    std::thread producer{ [&] {
        for (std::uint64_t i = 0; i < Count; ++i)
            while (!q.TryPush(i))
                std::this_thread::yield();
    } };

    std::uint64_t expected = 0, value = 0, sum = 0;
    while (expected < Count)
    {
        if (!q.TryPop(value))
        {
            std::this_thread::yield();
            continue;
        }
        ASSERT_EQ(value, expected);
        sum += value;
        ++expected;
    }
    producer.join();
    EXPECT_EQ(sum, Count * (Count - 1) / 2);
}

TEST(SpscQueue, TransfersInOrderAcrossThreads) { TransferInOrder<SpscQueue<std::uint64_t>>(); }
TEST(MutexQueue, TransfersInOrderAcrossThreads) { TransferInOrder<MutexQueue<std::uint64_t>>(); }

TEST(Orderbook, CancelGoodForDayOrdersOnlyRemovesThem)
{
    Orderbook book;
    book.AddOrder(OrderType::GoodForDay, 1, Side::Buy, 100, 10);
    book.AddOrder(OrderType::GoodTillCancel, 2, Side::Buy, 100, 10);
    book.AddOrder(OrderType::GoodForDay, 3, Side::Sell, 105, 10);
    book.AddOrder(OrderType::GoodTillCancel, 4, Side::Sell, 106, 10);

    book.CancelGoodForDayOrders();

    EXPECT_EQ(book.Size(), 2u);
    const auto top = book.GetTopOfBook();
    EXPECT_EQ(top.bidPrice_, 100);
    EXPECT_EQ(top.bidQuantity_, 10u);
    EXPECT_EQ(top.askPrice_, 106);
}

TEST(Orderbook, ClearKeepsWorking)
{
    Orderbook book{ 4 };
    for (int round = 0; round < 3; ++round)
    {
        for (OrderId id = 1; id <= 100; ++id)
            book.AddOrder(OrderType::GoodTillCancel, id, id % 2 ? Side::Buy : Side::Sell,
                id % 2 ? 100 - static_cast<Price>(id % 10) : 101 + static_cast<Price>(id % 10), 5);
        EXPECT_EQ(book.Size(), 100u);
        book.Clear();
        EXPECT_EQ(book.Size(), 0u);
        EXPECT_FALSE(book.GetTopOfBook().HasBid());
    }
}

TEST(Orderbook, TimePriorityWithinLevel)
{
    Orderbook book;
    book.AddOrder(OrderType::GoodTillCancel, 1, Side::Sell, 100, 5);
    book.AddOrder(OrderType::GoodTillCancel, 2, Side::Sell, 100, 5);
    book.AddOrder(OrderType::GoodTillCancel, 3, Side::Sell, 100, 5);
    book.CancelOrder(2);//unlink from the middle of the queue

    const auto trades = book.AddOrder(OrderType::GoodTillCancel, 4, Side::Buy, 100, 8);
    ASSERT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[0].GetAskTrade().orderId_, 1u);
    EXPECT_EQ(trades[0].GetAskTrade().quantity_, 5u);
    EXPECT_EQ(trades[1].GetAskTrade().orderId_, 3u);
    EXPECT_EQ(trades[1].GetAskTrade().quantity_, 3u);
    EXPECT_EQ(book.GetTopOfBook().askQuantity_, 2u);
}

TEST(Orderbook, ReduceKeepsQueuePosition)
{
    Orderbook book;
    book.AddOrder(OrderType::GoodTillCancel, 1, Side::Buy, 100, 10);
    book.AddOrder(OrderType::GoodTillCancel, 2, Side::Buy, 100, 10);
    book.ReduceOrder(1, 7);//still first in line, now 3 shares

    const auto trades = book.AddOrder(OrderType::GoodTillCancel, 3, Side::Sell, 100, 4);
    ASSERT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[0].GetBidTrade().orderId_, 1u);
    EXPECT_EQ(trades[0].GetBidTrade().quantity_, 3u);
    EXPECT_EQ(trades[1].GetBidTrade().orderId_, 2u);
    EXPECT_EQ(trades[1].GetBidTrade().quantity_, 1u);
}
