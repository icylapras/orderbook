#include "FastOrderbook.h"

#include <algorithm>

#include "Constants.h"
#include "LevelInfo.h"
#include "TradeInfo.h"

FastOrderbook::FastOrderbook(std::size_t expectedOrders, Price tickSize, std::uint32_t bandTicks)
    : bids_{ Side::Buy, tickSize, bandTicks }
    , asks_{ Side::Sell, tickSize, bandTicks }
    , ids_{ expectedOrders }
{
    nodes_.reserve(expectedOrders);
    levels_.reserve(std::clamp<std::size_t>(expectedOrders / 4, 64, 1 << 16));
    trades_.reserve(64);
}

std::uint32_t FastOrderbook::AllocateNode()
{
    if (freeNode_ != Nil)
    {
        const auto index = freeNode_;
        freeNode_ = nodes_[index].next_;
        return index;
    }

    nodes_.emplace_back();
    return static_cast<std::uint32_t>(nodes_.size() - 1);
}

void FastOrderbook::FreeNode(std::uint32_t index)
{
    nodes_[index].next_ = freeNode_;
    freeNode_ = index;
}

std::uint32_t FastOrderbook::AllocateLevel(Price price)
{
    std::uint32_t slot;
    if (freeLevel_ != Nil)
    {
        slot = freeLevel_;
        freeLevel_ = levels_[slot].head_;
    }
    else
    {
        slot = static_cast<std::uint32_t>(levels_.size());
        levels_.emplace_back();
    }

    levels_[slot] = Level{ price, 0, 0, Nil, Nil };
    return slot;
}

void FastOrderbook::RemoveLevel(Side side, std::uint32_t slot)
{
    LadderFor(side).Erase(levels_[slot].price_);
    levels_[slot].head_ = freeLevel_;
    freeLevel_ = slot;
}

void FastOrderbook::PushBack(Level& level, std::uint32_t index)
{
    auto& node = nodes_[index];
    node.prev_ = level.tail_;
    node.next_ = Nil;

    if (level.tail_ != Nil)
        nodes_[level.tail_].next_ = index;
    else
        level.head_ = index;

    level.tail_ = index;
    level.quantity_ += node.remainingQuantity_;
    ++level.count_;
}

void FastOrderbook::Unlink(Level& level, std::uint32_t index)
{
    const auto& node = nodes_[index];

    if (node.prev_ != Nil)
        nodes_[node.prev_].next_ = node.next_;
    else
        level.head_ = node.next_;

    if (node.next_ != Nil)
        nodes_[node.next_].prev_ = node.prev_;
    else
        level.tail_ = node.prev_;

    level.quantity_ -= node.remainingQuantity_;
    --level.count_;
}

void FastOrderbook::RemoveOrder(std::uint32_t index)
{
    const auto& node = nodes_[index];
    auto& level = levels_[node.level_];

    Unlink(level, index);
    if (level.count_ == 0)
        RemoveLevel(node.side_, node.level_);

    ids_.Erase(node.orderId_);
    FreeNode(index);
}

bool FastOrderbook::CanMatch(Side side, Price price) const
{
    const auto best = Opposite(side).Best();
    if (!best)
        return false;
    return side == Side::Buy ? price >= best.price_ : price <= best.price_;
}

bool FastOrderbook::CanFullyFill(Side side, Price price, Quantity quantity) const
{
    if (!CanMatch(side, price))
        return false;

    std::uint64_t needed = quantity;
    bool filled = false;
    Opposite(side).ForEachFromBest([&](PriceLadder::Entry e) {
        if (side == Side::Buy ? e.price_ > price : e.price_ < price)
            return false;

        const auto available = levels_[e.slot_].quantity_;
        if (needed <= available)
        {
            filled = true;
            return false;
        }
        needed -= available;
        return true;
    });

    return filled;
}

void FastOrderbook::MatchOrders()
{
    while (true)
    {
        const auto bestBid = bids_.Best();
        const auto bestAsk = asks_.Best();
        if (!bestBid || !bestAsk || bestBid.price_ < bestAsk.price_)
            break;

        //no allocation happens in this loop, so the references stay valid
        auto& bidLevel = levels_[bestBid.slot_];
        auto& askLevel = levels_[bestAsk.slot_];

        while (bidLevel.count_ != 0 && askLevel.count_ != 0)
        {
            const auto bidIndex = bidLevel.head_;
            const auto askIndex = askLevel.head_;
            auto& bid = nodes_[bidIndex];
            auto& ask = nodes_[askIndex];

            const Quantity quantity = std::min(bid.remainingQuantity_, ask.remainingQuantity_);

            bid.remainingQuantity_ -= quantity;
            ask.remainingQuantity_ -= quantity;
            bidLevel.quantity_ -= quantity;
            askLevel.quantity_ -= quantity;

            trades_.push_back(Trade{
                TradeInfo{ bid.orderId_, bid.price_, quantity },
                TradeInfo{ ask.orderId_, ask.price_, quantity }
                });

            //a filled order has 0 remaining, so unlinking leaves the level quantity alone
            if (bid.remainingQuantity_ == 0)
            {
                Unlink(bidLevel, bidIndex);
                ids_.Erase(bid.orderId_);
                FreeNode(bidIndex);
            }

            if (ask.remainingQuantity_ == 0)
            {
                Unlink(askLevel, askIndex);
                ids_.Erase(ask.orderId_);
                FreeNode(askIndex);
            }
        }

        if (bidLevel.count_ == 0)
            RemoveLevel(Side::Buy, bestBid.slot_);

        if (askLevel.count_ == 0)
            RemoveLevel(Side::Sell, bestAsk.slot_);
    }
}

const Trades& FastOrderbook::AddOrder(OrderType type, OrderId orderId, Side side, Price price, Quantity quantity)
{
    trades_.clear();

    if (type == OrderType::Market)
    {
        //same policy as Orderbook: rest as GTC at the worst opposite price so
        //the order can sweep the entire opposite side
        const auto worst = Opposite(side).Worst();
        if (!worst)
            return trades_;

        price = worst.price_;
        type = OrderType::GoodTillCancel;
    }

    if (type == OrderType::FillAndKill && !CanMatch(side, price))
        return trades_;

    if (type == OrderType::FillOrKill && !CanFullyFill(side, price, quantity))
        return trades_;

    if (!RestOrder(type, orderId, side, price, quantity))
        return trades_;

    MatchOrders();

    //a FillAndKill never rests: whatever is left after matching is cancelled
    if (type == OrderType::FillAndKill)
    {
        const auto remaining = ids_.Find(orderId);
        if (remaining != OrderIdMap::Missing)
            RemoveOrder(remaining);
    }

    return trades_;
}

bool FastOrderbook::RestOrder(OrderType type, OrderId orderId, Side side, Price price, Quantity quantity)
{
    //the duplicate-id check is folded into the insert: one hash probe per add
    const auto index = AllocateNode();
    if (!ids_.Insert(orderId, index))
    {
        FreeNode(index);
        return false;
    }

    //the band is centred on the first price the book sees
    bids_.CentreBand(price);
    asks_.CentreBand(price);

    auto& ladder = LadderFor(side);
    auto slot = ladder.Find(price);
    if (slot == Nil)
    {
        slot = AllocateLevel(price);
        ladder.Insert(price, slot);
    }

    nodes_[index] = OrderNode{ orderId, price, quantity, Nil, Nil, slot, side, type };
    PushBack(levels_[slot], index);
    return true;
}

void FastOrderbook::InsertOrder(OrderId orderId, Side side, Price price, Quantity quantity)
{
    RestOrder(OrderType::GoodTillCancel, orderId, side, price, quantity);
}

const Trades& FastOrderbook::AddMarketOrder(OrderId orderId, Side side, Quantity quantity)
{
    return AddOrder(OrderType::Market, orderId, side, Constants::InvalidPrice, quantity);
}

void FastOrderbook::CancelOrder(OrderId orderId)
{
    const auto index = ids_.Find(orderId);
    if (index != OrderIdMap::Missing)
        RemoveOrder(index);
}

const Trades& FastOrderbook::ModifyOrder(const OrderModify& modify)
{
    const auto index = ids_.Find(modify.GetOrderId());
    if (index == OrderIdMap::Missing)
    {
        trades_.clear();
        return trades_;
    }

    const auto type = nodes_[index].orderType_;
    RemoveOrder(index);
    return AddOrder(type, modify.GetOrderId(), modify.GetSide(), modify.GetPrice(), modify.GetQuantity());
}

void FastOrderbook::ReduceOrder(OrderId orderId, Quantity quantity)
{
    const auto index = ids_.Find(orderId);
    if (index == OrderIdMap::Missing)
        return;

    auto& node = nodes_[index];
    if (quantity >= node.remainingQuantity_)
    {
        RemoveOrder(index);
        return;
    }

    node.remainingQuantity_ -= quantity;
    levels_[node.level_].quantity_ -= quantity;
}

void FastOrderbook::ReplaceOrder(OrderId orderId, OrderId newOrderId, Price price, Quantity quantity)
{
    const auto index = ids_.Find(orderId);
    if (index == OrderIdMap::Missing)
        return;

    const auto side = nodes_[index].side_;
    const auto type = nodes_[index].orderType_;
    RemoveOrder(index);
    RestOrder(type, newOrderId, side, price, quantity);
}

void FastOrderbook::CancelGoodForDayOrders()
{
    OrderIds expired;
    for (const auto* ladder : { &bids_, &asks_ })
    {
        ladder->ForEachFromBest([&](PriceLadder::Entry e) {
            for (auto i = levels_[e.slot_].head_; i != Nil; i = nodes_[i].next_)
                if (nodes_[i].orderType_ == OrderType::GoodForDay)
                    expired.push_back(nodes_[i].orderId_);
            return true;
        });
    }

    for (const auto orderId : expired)
        CancelOrder(orderId);
}

OrderbookLevelInfos FastOrderbook::GetLevelInfos() const
{
    LevelInfos bidInfos, askInfos;
    auto collect = [this](LevelInfos& out) {
        return [this, &out](PriceLadder::Entry e) {
            out.push_back(LevelInfo{ e.price_, static_cast<Quantity>(levels_[e.slot_].quantity_) });
            return true;
        };
    };
    bids_.ForEachFromBest(collect(bidInfos));
    asks_.ForEachFromBest(collect(askInfos));
    return OrderbookLevelInfos{ bidInfos, askInfos };
}

TopOfBook FastOrderbook::GetTopOfBook() const
{
    TopOfBook top;
    if (const auto bid = bids_.Best())
    {
        top.bidPrice_ = bid.price_;
        top.bidQuantity_ = static_cast<Quantity>(levels_[bid.slot_].quantity_);
    }
    if (const auto ask = asks_.Best())
    {
        top.askPrice_ = ask.price_;
        top.askQuantity_ = static_cast<Quantity>(levels_[ask.slot_].quantity_);
    }
    return top;
}

void FastOrderbook::Clear()
{
    bids_.Clear();
    asks_.Clear();
    levels_.clear();
    freeLevel_ = Nil;
    nodes_.clear();
    freeNode_ = Nil;
    ids_.Clear();
    trades_.clear();
}
