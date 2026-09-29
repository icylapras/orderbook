#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "Order.h"
#include "OrderModify.h"
#include "OrderbookLevelInfos.h"
#include "Side.h"
#include "TopOfBook.h"
#include "Trade.h"
#include "Usings.h"

class Orderbook
{
private:
    struct OrderEntry
    {
        OrderPointer order_{ nullptr };
        OrderPointers::iterator location_;
    };

    //for CanFullyFill so no walk needed
    struct LevelData
    {
        Quantity quantity_{ };
        Quantity count_{ };

        enum class Action
        {
            Add,
            Remove,
            Match,
        };
    };

    //per-level aggregates, one map per side: keyed by price alone, a bid and
    //an ask level at the same price (a crossed/locked book, which a passive
    //market-data book can hold during a halt) would be merged
    std::unordered_map<Price, LevelData> bidData_;
    std::unordered_map<Price, LevelData> askData_;
    std::map<Price, OrderPointers, std::greater<Price>> bids_;
    std::map<Price, OrderPointers, std::less<Price>> asks_;
    std::unordered_map<OrderId, OrderEntry> orders_;//for o(1) lookup by id
    mutable std::mutex ordersMutex_;
    std::thread ordersPruneThread_;
    std::condition_variable shutdownConditionVariable_;
    std::atomic<bool> shutdown_{ false };

    void PruneGoodForDayOrders();

    void CancelOrders(OrderIds orderIds);
    void CancelOrderInternal(OrderId orderId);
    Trades AddOrderInternal(OrderPointer order);
    bool InsertOrderInternal(OrderPointer order);
    void RestOrder(OrderPointer order);

    void OnOrderCancelled(OrderPointer order);
    void OnOrderAdded(OrderPointer order);
    void OnOrderMatched(Side side, Price price, Quantity quantity, bool isFullyFilled);
    void UpdateLevelData(Side side, Price price, Quantity quantity, LevelData::Action action);
    std::unordered_map<Price, LevelData>& LevelDataFor(Side side) { return side == Side::Buy ? bidData_ : askData_; }

    bool CanFullyFill(Side side, Price price, Quantity quantity) const;
    bool canMatch(Side side, Price price) const;
    Trades MatchOrders();

public:
    //the GoodForDay expiry thread can be switched off for replay/benchmarks,
    //where thousands of books would otherwise mean thousands of idle threads
    explicit Orderbook(bool startExpiryThread = true);
    Orderbook(const Orderbook&) = delete;
    void operator=(const Orderbook&) = delete;
    Orderbook(Orderbook&&) = delete;
    void operator=(Orderbook&&) = delete;
    ~Orderbook();

    Trades AddOrder(OrderPointer order);
    void CancelOrder(OrderId orderId);
    Trades MatchOrder(OrderModify order);

    //market-data operations (ITCH). These mirror a venue's book and never
    //match: the venue already did the matching, and during a halt its book
    //can legitimately be crossed.
    //rest an order without matching (ignored if the id exists)
    void InsertOrder(OrderPointer order);
    //shrink an order in place, keeping its queue position; removes it if the
    //reduction reaches its remaining size
    void ReduceOrder(OrderId orderId, Quantity quantity);
    //remove + rest under a new id, same side and type, back of the queue
    void ReplaceOrder(OrderId orderId, OrderId newOrderId, Price price, Quantity quantity);
    TopOfBook GetTopOfBook() const;

    std::size_t Size() const;
    OrderbookLevelInfos GetLevelInfos() const;
};
