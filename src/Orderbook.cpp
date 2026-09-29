#include "Orderbook.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <iterator>
#include <numeric>
#include <optional>

#include "LevelInfo.h"
#include "OrderType.h"
#include "TradeInfo.h"

void Orderbook::PruneGoodForDayOrders()
{
    using namespace std::chrono;
    const auto end = hours(16);

    while (true)
    {
        const auto now = system_clock::now();
        const auto now_c = system_clock::to_time_t(now);
        std::tm now_parts;
#ifdef _WIN32
        localtime_s(&now_parts, &now_c);
#else
        localtime_r(&now_c, &now_parts);
#endif

        if (now_parts.tm_hour >= end.count())
            now_parts.tm_mday += 1;

        now_parts.tm_hour = end.count();
        now_parts.tm_min = 0;
        now_parts.tm_sec = 0;

        auto next = system_clock::from_time_t(mktime(&now_parts));
        auto till = next - now + milliseconds(100);

        {
            std::unique_lock ordersLock{ ordersMutex_ };

            //predicate form: a spurious wakeup must not end the thread, and the
            //flag is re-checked under the lock so a shutdown can't be missed
            if (shutdownConditionVariable_.wait_for(ordersLock, till,
                [this] { return shutdown_.load(std::memory_order_acquire); }))
                return;
        }

        OrderIds orderIds;

        {
            std::scoped_lock ordersLock{ ordersMutex_ };

            for (const auto& [_, entry] : orders_)
            {
                const auto& [order, location] = entry;

                if (order->GetOrderType() != OrderType::GoodForDay)
                    continue;

                orderIds.push_back(order->GetOrderId());
            }
        }

        CancelOrders(orderIds);
    }
}

void Orderbook::CancelOrders(OrderIds orderIds)
{
    std::scoped_lock ordersLock{ ordersMutex_ };

    for (const auto& orderId : orderIds)
        CancelOrderInternal(orderId);
}

void Orderbook::CancelOrderInternal(OrderId orderId)
{
    if (!orders_.contains(orderId))
        return;

    const auto [order, iterator] = orders_.at(orderId);
    orders_.erase(orderId);

    if (order->GetSide() == Side::Sell)
    {
        auto price = order->GetPrice();
        auto& orders = asks_.at(price);
        orders.erase(iterator);
        if (orders.empty())
            asks_.erase(price);
    }
    else
    {
        auto price = order->GetPrice();
        auto& orders = bids_.at(price);
        orders.erase(iterator);
        if (orders.empty())
            bids_.erase(price);
    }

    OnOrderCancelled(order);
}

void Orderbook::OnOrderCancelled(OrderPointer order)
{
    UpdateLevelData(order->GetSide(), order->GetPrice(), order->GetRemainingQuantity(), LevelData::Action::Remove);
}

void Orderbook::OnOrderAdded(OrderPointer order)
{
    UpdateLevelData(order->GetSide(), order->GetPrice(), order->GetInitialQuantity(), LevelData::Action::Add);
}

void Orderbook::OnOrderMatched(Side side, Price price, Quantity quantity, bool isFullyFilled)
{
    UpdateLevelData(side, price, quantity, isFullyFilled ? LevelData::Action::Remove : LevelData::Action::Match);
}

void Orderbook::UpdateLevelData(Side side, Price price, Quantity quantity, LevelData::Action action)
{
    auto& levels = LevelDataFor(side);
    auto& data = levels[price];

    if (action == LevelData::Action::Add)
        data.count_ += 1;
    else if (action == LevelData::Action::Remove)
        data.count_ -= 1;

    if (action == LevelData::Action::Remove || action == LevelData::Action::Match)
        data.quantity_ -= quantity;
    else
        data.quantity_ += quantity;

    if (data.count_ == 0)
        levels.erase(price);
}

bool Orderbook::CanFullyFill(Side side, Price price, Quantity quantity) const
{
    if (!canMatch(side, price))
        return false;

    const auto& opposite = side == Side::Buy ? askData_ : bidData_;
    for (const auto& [levelPrice, levelData] : opposite)
    {
        if ((side == Side::Buy && levelPrice > price) ||
            (side == Side::Sell && levelPrice < price))
            continue;

        if (quantity <= levelData.quantity_)
            return true;

        quantity -= levelData.quantity_;
    }

    return false;
}

bool Orderbook::canMatch(Side side, Price price) const
{
    if (side == Side::Buy)
    {
        if (asks_.empty())
            return false;

        const auto& [bestAsk, _] = *asks_.begin();
        return price >= bestAsk;
    }
    else
    {
        if (bids_.empty())
            return false;

        const auto& [bestBid, _] = *bids_.begin();
        return price <= bestBid;
    }
}

Trades Orderbook::MatchOrders()
{
    //no reserve: sizing to orders_.size() would allocate for the whole book
    //on every add, when most adds produce zero trades
    Trades trades;

    while (true)
    {
        if (bids_.empty() || asks_.empty())
            break;

        auto& [bidPrice, bids] = *bids_.begin();
        auto& [askPrice, asks] = *asks_.begin();

        if (bidPrice < askPrice)
            break;

        while (!bids.empty() && !asks.empty())
        {
            //copies, not references: pop_front below would leave a reference dangling
            auto bid = bids.front();
            auto ask = asks.front();

            Quantity quantity = std::min(bid->GetRemainingQuantity(), ask->GetRemainingQuantity());

            bid->Fill(quantity);
            ask->Fill(quantity);

            if (bid->IsFilled())
            {
                bids.pop_front();
                orders_.erase(bid->GetOrderId());
            }

            if (ask->IsFilled())
            {
                asks.pop_front();
                orders_.erase(ask->GetOrderId());
            }

            trades.push_back(Trade{
                TradeInfo{ bid->GetOrderId(), bid->GetPrice(), quantity },
                TradeInfo{ ask->GetOrderId(), ask->GetPrice(), quantity }
                });

            OnOrderMatched(Side::Buy, bid->GetPrice(), quantity, bid->IsFilled());
            OnOrderMatched(Side::Sell, ask->GetPrice(), quantity, ask->IsFilled());
        }

        if (bids.empty())
            bids_.erase(bidPrice);

        if (asks.empty())
            asks_.erase(askPrice);
    }

    return trades;
}

Orderbook::Orderbook(bool startExpiryThread)
{
    if (startExpiryThread)
        ordersPruneThread_ = std::thread{ [this] { PruneGoodForDayOrders(); } };
}

Orderbook::~Orderbook()
{
    {
        //set under the mutex: otherwise the store + notify can land between the
        //prune thread checking the flag and blocking, and the wakeup is lost
        std::scoped_lock ordersLock{ ordersMutex_ };
        shutdown_.store(true, std::memory_order_release);
    }
    shutdownConditionVariable_.notify_one();

    if (ordersPruneThread_.joinable())
        ordersPruneThread_.join();
}

Trades Orderbook::AddOrder(OrderPointer order)
{
    std::scoped_lock ordersLock{ ordersMutex_ };
    return AddOrderInternal(order);
}

Trades Orderbook::AddOrderInternal(OrderPointer order)
{
    if (orders_.contains(order->GetOrderId()))
        return { };

    if (order->GetOrderType() == OrderType::Market)
    {
        if (order->GetSide() == Side::Buy && !asks_.empty())
        {
            const auto& [worstAsk, _] = *asks_.rbegin();
            order->ToGoodTillCancel(worstAsk);
        }
        else if (order->GetSide() == Side::Sell && !bids_.empty())
        {
            const auto& [worstBid, _] = *bids_.rbegin();
            order->ToGoodTillCancel(worstBid);
        }
        else
            return { };
    }

    if (order->GetOrderType() == OrderType::FillAndKill && !canMatch(order->GetSide(), order->GetPrice()))
        return { };

    if (order->GetOrderType() == OrderType::FillOrKill && !CanFullyFill(order->GetSide(), order->GetPrice(), order->GetInitialQuantity()))
        return { };

    RestOrder(order);
    auto trades = MatchOrders();

    //a FillAndKill never rests: cancel whatever is left of *this* order. (This
    //used to cancel a FAK only if it was at the front of the best level; once
    //the book can be crossed, an older resting order can be ahead of it, and
    //the remainder stayed in the book. Found by the crossed-book fuzz test.)
    if (order->GetOrderType() == OrderType::FillAndKill)
        CancelOrderInternal(order->GetOrderId());

    return trades;
}

bool Orderbook::InsertOrderInternal(OrderPointer order)
{
    if (orders_.contains(order->GetOrderId()))
        return false;
    RestOrder(order);
    return true;
}

void Orderbook::RestOrder(OrderPointer order)
{
    OrderPointers::iterator iterator;
    if (order->GetSide() == Side::Buy)
    {
        auto& orders = bids_[order->GetPrice()];
        orders.push_back(order);
        iterator = std::prev(orders.end());
    }
    else
    {
        auto& orders = asks_[order->GetPrice()];
        orders.push_back(order);
        iterator = std::prev(orders.end());
    }

    orders_.insert({ order->GetOrderId(), OrderEntry{ order, iterator } });

    OnOrderAdded(order);
}

void Orderbook::CancelOrder(OrderId orderId)
{
    std::scoped_lock ordersLock{ ordersMutex_ };

    CancelOrderInternal(orderId);
}

Trades Orderbook::MatchOrder(OrderModify order)
{
    OrderType orderType;

    {
        std::scoped_lock ordersLock{ ordersMutex_ };

        if (!orders_.contains(order.GetOrderId()))
            return { };

        const auto& [existingOrder, _ ] = orders_.at(order.GetOrderId());
        orderType = existingOrder->GetOrderType();
    }

    CancelOrder(order.GetOrderId());
    return AddOrder(order.ToOrderPointer(orderType));
}

void Orderbook::ReduceOrder(OrderId orderId, Quantity quantity)
{
    std::scoped_lock ordersLock{ ordersMutex_ };

    auto it = orders_.find(orderId);
    if (it == orders_.end())
        return;

    const auto& order = it->second.order_;
    if (quantity >= order->GetRemainingQuantity())
    {
        CancelOrderInternal(orderId);
        return;
    }

    order->Fill(quantity);
    UpdateLevelData(order->GetSide(), order->GetPrice(), quantity, LevelData::Action::Match);
}

void Orderbook::InsertOrder(OrderPointer order)
{
    std::scoped_lock ordersLock{ ordersMutex_ };
    InsertOrderInternal(order);
}

void Orderbook::ReplaceOrder(OrderId orderId, OrderId newOrderId, Price price, Quantity quantity)
{
    std::scoped_lock ordersLock{ ordersMutex_ };

    auto it = orders_.find(orderId);
    if (it == orders_.end())
        return;

    const auto side = it->second.order_->GetSide();
    const auto type = it->second.order_->GetOrderType();
    CancelOrderInternal(orderId);

    InsertOrderInternal(std::make_shared<Order>(type, newOrderId, side, price, quantity));
}

TopOfBook Orderbook::GetTopOfBook() const
{
    std::scoped_lock ordersLock{ ordersMutex_ };

    TopOfBook top;
    if (!bids_.empty())
    {
        top.bidPrice_ = bids_.begin()->first;
        top.bidQuantity_ = bidData_.at(top.bidPrice_).quantity_;
    }
    if (!asks_.empty())
    {
        top.askPrice_ = asks_.begin()->first;
        top.askQuantity_ = askData_.at(top.askPrice_).quantity_;
    }
    return top;
}

std::size_t Orderbook::Size() const
{
    std::scoped_lock ordersLock{ ordersMutex_ };
    return orders_.size();
}

OrderbookLevelInfos Orderbook::GetLevelInfos() const
{
    std::scoped_lock ordersLock{ ordersMutex_ };

    LevelInfos bidInfos, askInfos;
    bidInfos.reserve(orders_.size());
    askInfos.reserve(orders_.size());

    auto CreateLevelInfos = [](Price price, const OrderPointers& orders)
    {
        return LevelInfo{ price, std::accumulate(orders.begin(), orders.end(), (Quantity)0,
            [](Quantity runningSum, const OrderPointer& order)
        { return runningSum + order->GetRemainingQuantity(); }) };
    };

    for (const auto& [price, orders] : bids_)
        bidInfos.push_back(CreateLevelInfos(price, orders));
    for (const auto& [price, orders] : asks_)
        askInfos.push_back(CreateLevelInfos(price, orders));

    return OrderbookLevelInfos{ bidInfos, askInfos};

}
