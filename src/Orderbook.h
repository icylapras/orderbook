#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "OrderIdMap.h"
#include "OrderModify.h"
#include "OrderType.h"
#include "OrderbookLevelInfos.h"
#include "PriceLadder.h"
#include "Side.h"
#include "TopOfBook.h"
#include "Trade.h"
#include "Usings.h"

//limit order book and matching engine, built for latency:
// - price levels are found through a tick-indexed ladder with an occupancy
//   bitmap (PriceLadder.h): O(1) level lookup/insert/erase and O(1) "next
//   best price", with a sorted overflow for off-grid or far-away prices
// - orders and levels live in pre-allocated pools and are addressed by
//   32-bit index; each level's FIFO queue is an intrusive doubly-linked list
//   threaded through the order pool, and each order knows its level, so a
//   cancel is: hash lookup, unlink, done - no search, no allocation
// - id lookup is an open-addressing hash map into the order pool
//
//not thread-safe by design: one thread owns the book (see SpscQueue.h for
//handing it work from another thread). GoodForDay expiry is an explicit call
//driven by the session clock/feed rather than a background thread.
class Orderbook
{
public:
    //tickSize: the price grid the band is indexed on (e.g. 100 = $0.01 in
    //ITCH's 4-decimal prices); bandTicks: ticks covered by the band around the
    //first order's price, 0 = no band (every level in the sorted overflow)
    explicit Orderbook(std::size_t expectedOrders = 1024, Price tickSize = 1, std::uint32_t bandTicks = 4096);

    //the returned trades stay valid until the next call on this book
    const Trades& AddOrder(OrderType type, OrderId orderId, Side side, Price price, Quantity quantity);
    const Trades& AddMarketOrder(OrderId orderId, Side side, Quantity quantity);
    void CancelOrder(OrderId orderId);
    const Trades& ModifyOrder(const OrderModify& modify);

    //market-data operations (ITCH): they mirror a venue's book and never
    //match (the venue already did; a halted symbol's book can be crossed)
    void InsertOrder(OrderId orderId, Side side, Price price, Quantity quantity);
    void ReduceOrder(OrderId orderId, Quantity quantity);
    void ReplaceOrder(OrderId orderId, OrderId newOrderId, Price price, Quantity quantity);

    void CancelGoodForDayOrders();

    std::size_t Size() const { return ids_.Size(); }
    OrderbookLevelInfos GetLevelInfos() const;
    TopOfBook GetTopOfBook() const;

    //empties the book but keeps every allocation, so a warmed-up book can be reused
    void Clear();

private:
    static constexpr std::uint32_t Nil = UINT32_MAX;

    //32 bytes: two orders per cache line
    struct OrderNode
    {
        OrderId orderId_;
        Price price_;
        Quantity remainingQuantity_;
        std::uint32_t prev_;
        std::uint32_t next_;//doubles as the free-list link while the node is unused
        std::uint32_t level_;
        Side side_;
        OrderType orderType_;
    };

    struct Level
    {
        Price price_;
        std::uint32_t count_;
        std::uint64_t quantity_;
        std::uint32_t head_;//doubles as the free-list link while the level is unused
        std::uint32_t tail_;
    };

    PriceLadder& LadderFor(Side side) { return side == Side::Buy ? bids_ : asks_; }
    const PriceLadder& LadderFor(Side side) const { return side == Side::Buy ? bids_ : asks_; }
    const PriceLadder& Opposite(Side side) const { return side == Side::Buy ? asks_ : bids_; }

    std::uint32_t AllocateNode();
    void FreeNode(std::uint32_t index);
    std::uint32_t AllocateLevel(Price price);
    void RemoveLevel(Side side, std::uint32_t slot);

    void PushBack(Level& level, std::uint32_t index);
    void Unlink(Level& level, std::uint32_t index);

    //links a new order into its level without matching; false if the id exists
    bool RestOrder(OrderType type, OrderId orderId, Side side, Price price, Quantity quantity);
    void RemoveOrder(std::uint32_t index);
    bool CanMatch(Side side, Price price) const;
    bool CanFullyFill(Side side, Price price, Quantity quantity) const;
    void MatchOrders();

    PriceLadder bids_;
    PriceLadder asks_;
    std::vector<Level> levels_;
    std::uint32_t freeLevel_{ Nil };
    std::vector<OrderNode> nodes_;
    std::uint32_t freeNode_{ Nil };
    OrderIdMap ids_;
    Trades trades_;
};
