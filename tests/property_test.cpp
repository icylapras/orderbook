//property / differential fuzz tests: random order streams applied in lockstep
//to an engine and to ReferenceBook, checking after every operation that
//  - the trades produced are identical, trade for trade
//  - the book (every level, both sides) and the order count are identical
//  - the book is never crossed
//  - quantity is conserved: each operation changes resting quantity by
//    exactly what its trades and its order type allow
//The "passive" profiles add market-data operations (insert/replace without
//matching), which can leave the book crossed the way a halted symbol's book
//is; the next matching order must then clear it identically everywhere.

#include <gtest/gtest.h>

#include <random>
#include <string>
#include <vector>

#include "EngineAdapters.h"
#include "ReferenceBook.h"

namespace
{

std::uint64_t Total(const OrderbookLevelInfos& levels)
{
    std::uint64_t total = 0;
    for (const auto& l : levels.GetBids())
        total += l.quantity_;
    for (const auto& l : levels.GetAsks())
        total += l.quantity_;
    return total;
}

std::uint64_t Traded(const Trades& trades)
{
    std::uint64_t total = 0;
    for (const auto& t : trades)
        total += t.GetBidTrade().quantity_;
    return total;
}

void ExpectSameTrades(const Trades& actual, const Trades& expected, const std::string& context)
{
    ASSERT_EQ(actual.size(), expected.size()) << context;
    for (std::size_t i = 0; i < actual.size(); ++i)
    {
        const auto& a = actual[i];
        const auto& e = expected[i];
        ASSERT_EQ(a.GetBidTrade().orderId_, e.GetBidTrade().orderId_) << context << " trade " << i;
        ASSERT_EQ(a.GetAskTrade().orderId_, e.GetAskTrade().orderId_) << context << " trade " << i;
        ASSERT_EQ(a.GetBidTrade().price_, e.GetBidTrade().price_) << context << " trade " << i;
        ASSERT_EQ(a.GetAskTrade().price_, e.GetAskTrade().price_) << context << " trade " << i;
        ASSERT_EQ(a.GetBidTrade().quantity_, e.GetBidTrade().quantity_) << context << " trade " << i;
        ASSERT_EQ(a.GetAskTrade().quantity_, e.GetAskTrade().quantity_) << context << " trade " << i;
        //a trade only happens when the bid is at or above the ask
        ASSERT_GE(a.GetBidTrade().price_, a.GetAskTrade().price_) << context;
    }
}

void ExpectSameLevels(const LevelInfos& actual, const LevelInfos& expected, const std::string& context)
{
    ASSERT_EQ(actual.size(), expected.size()) << context;
    for (std::size_t i = 0; i < actual.size(); ++i)
    {
        ASSERT_EQ(actual[i].price_, expected[i].price_) << context << " level " << i;
        ASSERT_EQ(actual[i].quantity_, expected[i].quantity_) << context << " level " << i;
    }
}

struct Profile
{
    Price low_, high_;          //price band; narrow = lots of matching
    Quantity maxQuantity_;
    int operations_;
    bool passive_{ false };     //include InsertOrder / ReplaceOrder (no matching)
};

template <typename Engine>
void Fuzz(std::uint32_t seed, Profile profile)
{
    std::mt19937 rng{ seed };
    Engine engine;
    ReferenceBook reference;

    std::vector<OrderId> ids;//ids handed out so far, live or not
    OrderId nextId = 1;

    std::uniform_int_distribution<Price> price{ profile.low_, profile.high_ };
    std::uniform_int_distribution<Quantity> quantity{ 1, profile.maxQuantity_ };
    std::uniform_int_distribution<int> percent{ 0, 99 };

    auto anyId = [&]() -> OrderId
    {
        if (ids.empty() || percent(rng) < 5)
            return nextId + 1000;//never issued: exercises "unknown id" paths
        return ids[std::uniform_int_distribution<std::size_t>{ 0, ids.size() - 1 }(rng)];
    };

    for (int step = 0; step < profile.operations_; ++step)
    {
        const std::string context = std::string{ Engine::Name } + " seed " + std::to_string(seed) + " step " + std::to_string(step);
        const auto before = Total(engine.Levels());
        const int roll = percent(rng);

        Trades actual, expected;
        std::int64_t delta = 0;

        if (roll < 60)
        {
            //add: mostly GTC, some of every other type, occasionally a duplicate id
            static constexpr OrderType Types[] = {
                OrderType::GoodTillCancel, OrderType::GoodTillCancel, OrderType::GoodTillCancel, OrderType::GoodTillCancel,
                OrderType::GoodForDay, OrderType::FillAndKill, OrderType::FillOrKill, OrderType::Market };
            const auto type = Types[std::uniform_int_distribution<int>{ 0, 7 }(rng)];
            const auto side = percent(rng) < 50 ? Side::Buy : Side::Sell;
            const bool duplicate = !ids.empty() && percent(rng) < 3;
            const OrderId id = duplicate ? anyId() : nextId++;
            const auto p = price(rng);
            const auto q = quantity(rng);
            if (!duplicate)
                ids.push_back(id);

            actual = engine.Add(type, id, side, p, q);
            expected = reference.Add(type, id, side, p, q);
            ASSERT_NO_FATAL_FAILURE(ExpectSameTrades(actual, expected, context));

            //conservation, from the engine's own output only: every trade
            //removes its quantity from both sides
            delta = static_cast<std::int64_t>(Total(engine.Levels())) - static_cast<std::int64_t>(before);
            const auto traded = static_cast<std::int64_t>(Traded(actual));
            const bool rejected = delta == 0 && traded == 0;
            switch (type)
            {
            case OrderType::FillAndKill:
                //with a crossed book, resting orders also trade among themselves,
                //so the exact FAK identity only holds on an uncrossed book
                if (!profile.passive_)
                {
                    ASSERT_EQ(delta, -traded) << context << " (FAK remainder must not rest)";
                }
                break;
            case OrderType::FillOrKill:
                if (!profile.passive_)
                {
                    ASSERT_TRUE(rejected || (traded == q && delta == -static_cast<std::int64_t>(q))) << context << " (FOK is all or nothing)";
                }
                break;
            default:
                ASSERT_TRUE(rejected || delta == static_cast<std::int64_t>(q) - 2 * traded) << context;
                break;
            }
        }
        else if (profile.passive_ && roll < 66)
        {
            const auto side = percent(rng) < 50 ? Side::Buy : Side::Sell;
            const bool duplicate = !ids.empty() && percent(rng) < 3;
            const OrderId id = duplicate ? anyId() : nextId++;
            const auto p = price(rng);
            const auto q = quantity(rng);
            if (!duplicate)
                ids.push_back(id);
            engine.Insert(id, side, p, q);
            reference.Insert(id, side, p, q);
            const auto after = Total(engine.Levels());
            ASSERT_TRUE(after == before || after == before + q) << context << " (insert never matches)";
        }
        else if (roll < 75)
        {
            const auto id = anyId();
            engine.Cancel(id);
            reference.Cancel(id);
            ASSERT_LE(Total(engine.Levels()), before) << context;
        }
        else if (roll < 85)
        {
            const auto id = anyId();
            const auto side = percent(rng) < 50 ? Side::Buy : Side::Sell;
            const OrderModify modify{ id, side, price(rng), quantity(rng) };
            actual = engine.Modify(modify);
            expected = reference.Modify(modify);
            ASSERT_NO_FATAL_FAILURE(ExpectSameTrades(actual, expected, context));
        }
        else if (roll < 93)
        {
            const auto id = anyId();
            const auto q = quantity(rng);
            engine.Reduce(id, q);
            reference.Reduce(id, q);
            const auto after = Total(engine.Levels());
            ASSERT_LE(after, before) << context;
            ASSERT_LE(before - after, q) << context;
        }
        else if (profile.passive_)
        {
            const auto id = anyId();
            const OrderId newId = nextId++;
            ids.push_back(newId);
            const auto p = price(rng);
            const auto q = quantity(rng);
            engine.Replace(id, newId, p, q);
            reference.Replace(id, newId, p, q);
        }
        else
        {
            const auto id = anyId();
            engine.Cancel(id);
            reference.Cancel(id);
        }

        const auto levels = engine.Levels();
        ASSERT_NO_FATAL_FAILURE(ExpectSameLevels(levels.GetBids(), reference.Levels(Side::Buy), context + " bids"));
        ASSERT_NO_FATAL_FAILURE(ExpectSameLevels(levels.GetAsks(), reference.Levels(Side::Sell), context + " asks"));
        ASSERT_EQ(engine.Size(), reference.Size()) << context;
        ASSERT_EQ(Total(levels), reference.TotalQuantity()) << context;

        const auto top = engine.Top();
        if (!profile.passive_)
        {
            ASSERT_FALSE(top.IsCrossed()) << context;
        }
        ASSERT_EQ(top.HasBid(), !levels.GetBids().empty()) << context;
        if (top.HasBid())
        {
            ASSERT_EQ(top.bidPrice_, levels.GetBids().front().price_) << context;
            ASSERT_EQ(top.bidQuantity_, levels.GetBids().front().quantity_) << context;
        }
        if (top.HasAsk())
        {
            ASSERT_EQ(top.askPrice_, levels.GetAsks().front().price_) << context;
            ASSERT_EQ(top.askQuantity_, levels.GetAsks().front().quantity_) << context;
        }
    }
}

template <typename Engine>
class PropertyTest : public ::testing::Test { };

using Engines = ::testing::Types<BaselineEngine, FastEngine, FastSortedEngine, FastMixedEngine>;
TYPED_TEST_SUITE(PropertyTest, Engines);

//tight band: nearly every order interacts with the other side
TYPED_TEST(PropertyTest, MatchesReferenceUnderHeavyCrossing)
{
    for (std::uint32_t seed = 1; seed <= 25; ++seed)
        ASSERT_NO_FATAL_FAILURE(Fuzz<TypeParam>(seed, Profile{ 98, 102, 20, 2'000 }));
}

//wide band: deep books with many levels, exercising level insert/erase in
//the middle of the sorted level vector
TYPED_TEST(PropertyTest, MatchesReferenceWithDeepBooks)
{
    for (std::uint32_t seed = 100; seed < 110; ++seed)
        ASSERT_NO_FATAL_FAILURE(Fuzz<TypeParam>(seed, Profile{ 1, 400, 100, 3'000 }));
}

//market-data inserts/replaces leave crossed books behind; matching orders
//arriving afterwards must clear them the same way in every engine
TYPED_TEST(PropertyTest, MatchesReferenceWithPassiveInsertsAndCrossedBooks)
{
    for (std::uint32_t seed = 200; seed < 225; ++seed)
        ASSERT_NO_FATAL_FAILURE(Fuzz<TypeParam>(seed, Profile{ 98, 102, 20, 2'000, true }));
    for (std::uint32_t seed = 300; seed < 305; ++seed)
        ASSERT_NO_FATAL_FAILURE(Fuzz<TypeParam>(seed, Profile{ 1, 400, 100, 3'000, true }));
}

//the two engines against each other on a much longer stream (the reference
//model is too slow for this); grows the id map and pool through many rehashes
TEST(DifferentialTest, FastMatchesBaselineOnLongStream)
{
    std::mt19937 rng{ 7 };
    BaselineEngine baseline;
    FastEngine fast;
    std::vector<OrderId> live;
    OrderId nextId = 1;
    std::uniform_int_distribution<Price> price{ 900, 1100 };
    std::uniform_int_distribution<Quantity> quantity{ 1, 500 };
    std::uniform_int_distribution<int> percent{ 0, 99 };

    for (int step = 0; step < 200'000; ++step)
    {
        const int roll = percent(rng);
        if (roll < 55 || live.empty())
        {
            const auto type = percent(rng) < 90 ? OrderType::GoodTillCancel : OrderType::FillAndKill;
            const auto side = percent(rng) < 50 ? Side::Buy : Side::Sell;
            const auto p = price(rng);
            const auto q = quantity(rng);
            const auto a = baseline.Add(type, nextId, side, p, q);
            const auto b = fast.Add(type, nextId, side, p, q);
            ASSERT_NO_FATAL_FAILURE(ExpectSameTrades(b, a, "step " + std::to_string(step)));
            live.push_back(nextId++);
        }
        else
        {
            const auto index = std::uniform_int_distribution<std::size_t>{ 0, live.size() - 1 }(rng);
            const auto id = live[index];
            if (roll < 85)
            {
                baseline.Cancel(id);
                fast.Cancel(id);
                live[index] = live.back();
                live.pop_back();
            }
            else
            {
                const auto q = quantity(rng);
                baseline.Reduce(id, q);
                fast.Reduce(id, q);
            }
        }

        ASSERT_EQ(fast.Top(), baseline.Top()) << "step " << step;
        ASSERT_EQ(fast.Size(), baseline.Size()) << "step " << step;
    }

    const auto a = baseline.Levels();
    const auto b = fast.Levels();
    ExpectSameLevels(b.GetBids(), a.GetBids(), "final bids");
    ExpectSameLevels(b.GetAsks(), a.GetAsks(), "final asks");
}

}
