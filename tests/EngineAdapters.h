#pragma once

//gives both engines one interface so every test runs against both

#include <memory>

#include "FastOrderbook.h"
#include "Order.h"
#include "Orderbook.h"

struct BaselineEngine
{
    static constexpr const char* Name = "Orderbook";

    Orderbook book_;

    Trades Add(OrderType type, OrderId id, Side side, Price price, Quantity quantity)
    {
        if (type == OrderType::Market)
            return book_.AddOrder(std::make_shared<Order>(id, side, quantity));
        return book_.AddOrder(std::make_shared<Order>(type, id, side, price, quantity));
    }
    void Cancel(OrderId id) { book_.CancelOrder(id); }
    Trades Modify(const OrderModify& modify) { return book_.MatchOrder(modify); }
    void Insert(OrderId id, Side side, Price price, Quantity quantity)
    {
        book_.InsertOrder(std::make_shared<Order>(OrderType::GoodTillCancel, id, side, price, quantity));
    }
    void Reduce(OrderId id, Quantity quantity) { book_.ReduceOrder(id, quantity); }
    void Replace(OrderId id, OrderId newId, Price price, Quantity quantity) { book_.ReplaceOrder(id, newId, price, quantity); }

    std::size_t Size() const { return book_.Size(); }
    OrderbookLevelInfos Levels() const { return book_.GetLevelInfos(); }
    TopOfBook Top() const { return book_.GetTopOfBook(); }
};

//Tick/Band pick the FastOrderbook configuration, so the same tests cover the
//tick-ladder band, the sorted overflow, and off-grid prices
template <Price Tick, std::uint32_t Band>
struct FastEngineT
{
    static constexpr const char* Name = Band == 0 ? "FastOrderbook(no band)" : Tick == 1 ? "FastOrderbook" : "FastOrderbook(tick 3, band 64)";

    FastOrderbook book_{ 1024, Tick, Band };

    Trades Add(OrderType type, OrderId id, Side side, Price price, Quantity quantity)
    {
        return book_.AddOrder(type, id, side, price, quantity);
    }
    void Cancel(OrderId id) { book_.CancelOrder(id); }
    Trades Modify(const OrderModify& modify) { return book_.ModifyOrder(modify); }
    void Insert(OrderId id, Side side, Price price, Quantity quantity) { book_.InsertOrder(id, side, price, quantity); }
    void Reduce(OrderId id, Quantity quantity) { book_.ReduceOrder(id, quantity); }
    void Replace(OrderId id, OrderId newId, Price price, Quantity quantity) { book_.ReplaceOrder(id, newId, price, quantity); }

    std::size_t Size() const { return book_.Size(); }
    OrderbookLevelInfos Levels() const { return book_.GetLevelInfos(); }
    TopOfBook Top() const { return book_.GetTopOfBook(); }
};

using FastEngine = FastEngineT<1, 4096>;
//no band: every level in the sorted vector (the previous design)
using FastSortedEngine = FastEngineT<1, 0>;
//tick 3 and a 64-tick band: most test prices are off-grid or out of band
using FastMixedEngine = FastEngineT<3, 64>;
