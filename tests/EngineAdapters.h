#pragma once

//one interface over the engine's three price-level layouts, so every test
//runs against the tick-ladder band, the sorted overflow, and off-grid prices

#include <cstdint>

#include "Orderbook.h"

template <Price Tick, std::uint32_t Band>
struct EngineT
{
    static constexpr const char* Name = Band == 0 ? "Orderbook(no band)" : Tick == 1 ? "Orderbook" : "Orderbook(tick 3, band 64)";

    Orderbook book_{ 1024, Tick, Band };

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

//default: tick ladder band around the market
using LadderEngine = EngineT<1, 4096>;
//no band: every level in the sorted overflow vector
using SortedEngine = EngineT<1, 0>;
//tick 3 and a 64-tick band: most test prices are off-grid or out of band
using MixedEngine = EngineT<3, 64>;
