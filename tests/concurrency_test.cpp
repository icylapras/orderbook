//the baseline Orderbook is shared between caller threads and its own
//GoodForDay expiry thread; these tests are aimed at ThreadSanitizer (CI runs
//them in a -fsanitize=thread build) and at the shutdown handshake

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#include "Order.h"
#include "Orderbook.h"

//regression for the lost-wakeup bug: the destructor used to set the shutdown
//flag without the mutex, so a notify could slip between the expiry thread's
//check and its wait, leaving join() blocked until 16:00. Construct/destroy
//repeatedly so the thread is caught at every point of its startup.
TEST(OrderbookConcurrency, DestructionNeverWaitsForTheExpiryTimer)
{
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 2'000; ++i)
    {
        Orderbook book;
        if (i % 2)
            std::this_thread::yield();
    }
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds{ 30 });
}

TEST(OrderbookConcurrency, ConcurrentCallersKeepTheBookConsistent)
{
    Orderbook book;//expiry thread running alongside
    constexpr int Threads = 4;
    constexpr int PerThread = 5'000;

    std::vector<std::thread> threads;
    for (int t = 0; t < Threads; ++t)
    {
        threads.emplace_back([&book, t] {
            for (int i = 0; i < PerThread; ++i)
            {
                const OrderId id = static_cast<OrderId>(t) * PerThread + i + 1;
                const auto side = (i + t) % 2 ? Side::Buy : Side::Sell;
                //buys below 100, sells at or above: never cross, so every order rests
                const Price price = side == Side::Buy ? 90 + i % 10 : 100 + i % 10;
                book.AddOrder(std::make_shared<Order>(OrderType::GoodForDay, id, side, price, 10));
                if (i % 3 == 0)
                    book.CancelOrder(id);
                else if (i % 3 == 1)
                    book.ReduceOrder(id, 4);
                (void)book.GetTopOfBook();
            }
        });
    }
    for (auto& t : threads)
        t.join();

    //per thread: a third cancelled, the rest resting
    std::size_t expected = 0;
    for (int i = 0; i < PerThread; ++i)
        expected += i % 3 != 0;
    EXPECT_EQ(book.Size(), expected * Threads);

    const auto top = book.GetTopOfBook();
    EXPECT_FALSE(top.IsCrossed());
}

TEST(OrderbookConcurrency, ConcurrentMatchingConservesQuantity)
{
    Orderbook book;
    constexpr int PerThread = 5'000;
    std::atomic<std::uint64_t> traded{ 0 };

    auto trader = [&](Side side, OrderId firstId) {
        for (int i = 0; i < PerThread; ++i)
        {
            const auto trades = book.AddOrder(std::make_shared<Order>(OrderType::GoodTillCancel, firstId + i, side, 100, 7));
            for (const auto& t : trades)
                traded += t.GetBidTrade().quantity_;
        }
    };

    std::thread buyer{ trader, Side::Buy, 1 };
    std::thread seller{ trader, Side::Sell, 1'000'000 };
    buyer.join();
    seller.join();

    //equal volume both sides at one price: everything matches
    EXPECT_EQ(traded.load(), 7ull * PerThread);
    EXPECT_EQ(book.Size(), 0u);
}
