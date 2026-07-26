#include <algorithm>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>

#include "Order.h"
#include "OrderType.h"
#include "Orderbook.h"
#include "Side.h"
#include "Trade.h"
#include "Usings.h"

namespace
{

//ladder view: asks stacked above bids, widest bar = deepest level
void PrintBook(const Orderbook& orderbook)
{
    const auto infos = orderbook.GetLevelInfos();
    const auto& bids = infos.GetBids();
    const auto& asks = infos.GetAsks();

    Quantity maxQuantity = 1;
    for (const auto& level : bids)
        maxQuantity = std::max(maxQuantity, level.quantity_);
    for (const auto& level : asks)
        maxQuantity = std::max(maxQuantity, level.quantity_);

    constexpr std::size_t BarWidth = 20;
    auto Bar = [&](Quantity quantity)
    {
        const auto width = std::max<std::size_t>(1, quantity * BarWidth / maxQuantity);
        return std::string(width, '#');
    };

    std::cout << "        price     qty\n";

    //asks print high-to-low so the best (lowest) ask sits against the spread
    for (auto level = asks.rbegin(); level != asks.rend(); ++level)
        std::cout << "  ASK " << std::setw(7) << level->price_
                  << std::setw(8) << level->quantity_ << "  " << Bar(level->quantity_) << '\n';

    if (!bids.empty() && !asks.empty())
        std::cout << "      ------- spread " << (asks.front().price_ - bids.front().price_) << " -------\n";

    for (const auto& level : bids)
        std::cout << "  BID " << std::setw(7) << level.price_
                  << std::setw(8) << level.quantity_ << "  " << Bar(level.quantity_) << '\n';

    std::cout << "  (" << orderbook.Size() << " resting orders)\n";
}

void PrintTrades(const Trades& trades)
{
    if (trades.empty())
    {
        std::cout << "  no trades\n";
        return;
    }

    for (const auto& trade : trades)
        std::cout << "  filled " << std::setw(4) << trade.GetAskTrade().quantity_
                  << " @ " << trade.GetAskTrade().price_
                  << "   (buy #" << trade.GetBidTrade().orderId_
                  << " x sell #" << trade.GetAskTrade().orderId_ << ")\n";
}

void Heading(const std::string& text)
{
    std::cout << "\n=== " << text << " ===\n";
}

}

int main()
{
    Orderbook orderbook;
    OrderId nextId = 1;

    auto AddLimit = [&](Side side, Price price, Quantity quantity)
    {
        return orderbook.AddOrder(std::make_shared<Order>(
            OrderType::GoodTillCancel, nextId++, side, price, quantity));
    };

    Heading("Seeded book");
    AddLimit(Side::Buy, 99, 80);
    AddLimit(Side::Buy, 100, 35);
    AddLimit(Side::Buy, 101, 60);
    AddLimit(Side::Sell, 103, 20);
    AddLimit(Side::Sell, 104, 45);
    AddLimit(Side::Sell, 105, 30);
    PrintBook(orderbook);

    Heading("Market buy 50 sweeps the ask side");
    const auto marketTrades = orderbook.AddOrder(
        std::make_shared<Order>(nextId++, Side::Buy, 50));
    PrintTrades(marketTrades);
    PrintBook(orderbook);

    Heading("FillOrKill buy 200 @ 105 (more than the book holds)");
    const auto killedTrades = orderbook.AddOrder(std::make_shared<Order>(
        OrderType::FillOrKill, nextId++, Side::Buy, 105, 200));
    PrintTrades(killedTrades);
    std::cout << "  rejected, book untouched: " << orderbook.Size() << " resting orders\n";

    Heading("Limit sell 101 x 40 crosses the best bid");
    const auto crossTrades = AddLimit(Side::Sell, 101, 40);
    PrintTrades(crossTrades);
    PrintBook(orderbook);

    Heading("Cancel the 99 bid");
    orderbook.CancelOrder(1);
    PrintBook(orderbook);

    return 0;
}
