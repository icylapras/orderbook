#pragma once

//the matching rules written as plainly as possible: a flat vector of orders,
//every query a linear scan. Too slow for real use, but short enough to check
//by reading, which is the point: every price-level layout is fuzzed against it.

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

#include "LevelInfo.h"
#include "OrderModify.h"
#include "OrderType.h"
#include "Side.h"
#include "TopOfBook.h"
#include "Trade.h"

class ReferenceBook
{
public:
    struct Resting
    {
        OrderId id_;
        Side side_;
        Price price_;
        Quantity remaining_;
        OrderType type_;
        std::uint64_t sequence_;//arrival order, for time priority
    };

    Trades Add(OrderType type, OrderId id, Side side, Price price, Quantity quantity)
    {
        if (Find(id))
            return { };

        const Side opposite = side == Side::Buy ? Side::Sell : Side::Buy;

        if (type == OrderType::Market)
        {
            //rests as GTC at the worst opposite price
            std::optional<Price> worst;
            for (const auto& o : orders_)
                if (o.side_ == opposite && (!worst || (side == Side::Buy ? o.price_ > *worst : o.price_ < *worst)))
                    worst = o.price_;
            if (!worst)
                return { };
            price = *worst;
            type = OrderType::GoodTillCancel;
        }

        auto crosses = [&](Price restingPrice) { return side == Side::Buy ? restingPrice <= price : restingPrice >= price; };

        if (type == OrderType::FillAndKill)
        {
            const bool any = std::any_of(orders_.begin(), orders_.end(),
                [&](const Resting& o) { return o.side_ == opposite && crosses(o.price_); });
            if (!any)
                return { };
        }

        if (type == OrderType::FillOrKill)
        {
            std::uint64_t available = 0;
            for (const auto& o : orders_)
                if (o.side_ == opposite && crosses(o.price_))
                    available += o.remaining_;
            if (available < quantity)
                return { };
        }

        orders_.push_back(Resting{ id, side, price, quantity, type, sequence_++ });
        Trades trades = Match();

        if (type == OrderType::FillAndKill)
            Erase(id);

        return trades;
    }

    void Cancel(OrderId id) { Erase(id); }

    Trades Modify(const OrderModify& modify)
    {
        const auto* existing = Find(modify.GetOrderId());
        if (!existing)
            return { };
        const auto type = existing->type_;
        Erase(modify.GetOrderId());
        return Add(type, modify.GetOrderId(), modify.GetSide(), modify.GetPrice(), modify.GetQuantity());
    }

    //market-data insert: rests without matching, so the book may cross
    void Insert(OrderId id, Side side, Price price, Quantity quantity)
    {
        Rest(OrderType::GoodTillCancel, id, side, price, quantity);
    }

    void Reduce(OrderId id, Quantity quantity)
    {
        auto* o = Find(id);
        if (!o)
            return;
        if (quantity >= o->remaining_)
            Erase(id);
        else
            o->remaining_ -= quantity;
    }

    //market-data replace: remove + rest under the new id, no matching
    void Replace(OrderId id, OrderId newId, Price price, Quantity quantity)
    {
        const auto* existing = Find(id);
        if (!existing)
            return;
        const auto side = existing->side_;
        const auto type = existing->type_;
        Erase(id);
        Rest(type, newId, side, price, quantity);
    }

    std::size_t Size() const { return orders_.size(); }

    const Resting* Find(OrderId id) const
    {
        for (const auto& o : orders_)
            if (o.id_ == id)
                return &o;
        return nullptr;
    }

    //best first on both sides
    LevelInfos Levels(Side side) const
    {
        std::map<Price, std::uint64_t> byPrice;
        for (const auto& o : orders_)
            if (o.side_ == side)
                byPrice[o.price_] += o.remaining_;

        LevelInfos levels;
        for (const auto& [price, quantity] : byPrice)
            levels.push_back(LevelInfo{ price, static_cast<Quantity>(quantity) });
        if (side == Side::Buy)
            std::reverse(levels.begin(), levels.end());
        return levels;
    }

    std::uint64_t TotalQuantity() const
    {
        std::uint64_t total = 0;
        for (const auto& o : orders_)
            total += o.remaining_;
        return total;
    }

    const std::vector<Resting>& Orders() const { return orders_; }

private:
    void Rest(OrderType type, OrderId id, Side side, Price price, Quantity quantity)
    {
        if (!Find(id))
            orders_.push_back(Resting{ id, side, price, quantity, type, sequence_++ });
    }

    Resting* Find(OrderId id)
    {
        for (auto& o : orders_)
            if (o.id_ == id)
                return &o;
        return nullptr;
    }

    void Erase(OrderId id)
    {
        std::erase_if(orders_, [id](const Resting& o) { return o.id_ == id; });
    }

    //best = highest bid / lowest ask, ties broken by earliest arrival
    Resting* Best(Side side)
    {
        Resting* best = nullptr;
        for (auto& o : orders_)
        {
            if (o.side_ != side)
                continue;
            if (!best)
                best = &o;
            else if (o.price_ != best->price_)
            {
                if (side == Side::Buy ? o.price_ > best->price_ : o.price_ < best->price_)
                    best = &o;
            }
            else if (o.sequence_ < best->sequence_)
                best = &o;
        }
        return best;
    }

    Trades Match()
    {
        Trades trades;
        while (true)
        {
            auto* bid = Best(Side::Buy);
            auto* ask = Best(Side::Sell);
            if (!bid || !ask || bid->price_ < ask->price_)
                break;

            const Quantity quantity = std::min(bid->remaining_, ask->remaining_);
            bid->remaining_ -= quantity;
            ask->remaining_ -= quantity;
            trades.push_back(Trade{ TradeInfo{ bid->id_, bid->price_, quantity }, TradeInfo{ ask->id_, ask->price_, quantity } });

            std::erase_if(orders_, [](const Resting& o) { return o.remaining_ == 0; });
        }
        return trades;
    }

    std::vector<Resting> orders_;
    std::uint64_t sequence_{ };
};
