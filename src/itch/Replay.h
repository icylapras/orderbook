#pragma once

//turns decoded ITCH messages into book operations.
//
//ITCH is a market-by-order feed: it tells us about every displayed order
//resting on NASDAQ's book, and every change to it. The mapping is:
//   A / F  Add Order (with/without MPID)   -> add a resting limit order
//   E      Order Executed                  -> reduce by executed shares
//   C      Order Executed With Price       -> reduce by executed shares
//   X      Order Cancel (partial)          -> reduce by cancelled shares
//   D      Order Delete                    -> remove
//   U      Order Replace                   -> remove + add under a new reference
//                                             (same side, loses time priority)
//   H      Stock Trading Action            -> trading state (not a book change)
//Executions arrive as E/C against the *resting* order only; the aggressor
//never rests, so it never appears as an add.

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "Itch.h"
#include "Orderbook.h"

namespace itch
{

//one book-affecting message, pre-decoded into 40 bytes so a replay can run
//from memory with no parsing in the timed loop
struct Event
{
    std::uint64_t timestamp_;
    std::uint64_t reference_;
    std::uint64_t newReference_;//U only
    std::uint32_t price_;       //A/F/U; execution price for C
    std::uint32_t shares_;
    std::uint16_t locate_;
    char type_;
    char side_;                 //A/F only; 'C' events: printable flag instead
};

//true for the message types that change the displayed book
inline bool IsBookEvent(char type)
{
    return type == 'A' || type == 'F' || type == 'E' || type == 'C' || type == 'X' || type == 'D' || type == 'U';
}

inline Side ToSide(char side) { return side == 'B' ? Side::Buy : Side::Sell; }
inline Price ToPrice(std::uint32_t price) { return static_cast<Price>(price); }

//applies one event to a book. The book mirrors NASDAQ's, so orders are
//rested without matching: NASDAQ already matched everything it was going to,
//and while a symbol is halted/paused its book can legitimately be crossed
inline void Apply(Orderbook& book, const Event& e)
{
    switch (e.type_)
    {
    case 'A':
    case 'F':
        book.InsertOrder(e.reference_, ToSide(e.side_), ToPrice(e.price_), e.shares_);
        break;
    case 'E':
    case 'C':
    case 'X':
        book.ReduceOrder(e.reference_, e.shares_);
        break;
    case 'D':
        book.CancelOrder(e.reference_);
        break;
    case 'U':
        book.ReplaceOrder(e.reference_, e.newReference_, ToPrice(e.price_), e.shares_);
        break;
    default:
        break;
    }
}

struct CrossInfo
{
    std::uint64_t timestamp_;
    std::uint64_t shares_;
    std::uint32_t price_;
    char type_;
};

//parse handler that keeps the book events (for one symbol, or all) plus the
//side information needed to validate a replay: the stock directory, system
//events, and the cross/trade prints NASDAQ published
class EventCollector
{
public:
    //empty symbol = keep every symbol
    explicit EventCollector(std::string symbol = { }) : symbol_{ std::move(symbol) } { }

    void OnSystemEvent(const SystemEvent& e) { systemEvents_.push_back({ e.timestamp_, e.eventCode_ }); }

    //trading state changes ('T' trading, 'H' halted, 'P' paused, 'Q' quotation
    //only) ride in the event stream so a replay knows when crossing is legal
    void OnStockTradingAction(const StockTradingAction& e)
    {
        if (Keep(e.stockLocate_))
            events_.push_back(Event{ e.timestamp_, 0, 0, 0, 0, e.stockLocate_, 'H', e.tradingState_ });
    }

    void OnStockDirectory(const StockDirectory& e)
    {
        const std::string name{ e.stock_.View() };
        symbols_[e.stockLocate_] = name;
        if (!symbol_.empty() && name == symbol_)
            locate_ = e.stockLocate_;
    }

    void OnAddOrder(const AddOrder& e)
    {
        if (Keep(e.stockLocate_))
            events_.push_back(Event{ e.timestamp_, e.orderReference_, 0, e.price_, e.shares_, e.stockLocate_, e.hasAttribution_ ? 'F' : 'A', e.side_ });
    }

    void OnOrderExecuted(const OrderExecuted& e)
    {
        if (Keep(e.stockLocate_))
            events_.push_back(Event{ e.timestamp_, e.orderReference_, 0, 0, e.executedShares_, e.stockLocate_, 'E', 0 });
    }

    void OnOrderExecutedWithPrice(const OrderExecutedWithPrice& e)
    {
        if (Keep(e.stockLocate_))
            events_.push_back(Event{ e.timestamp_, e.orderReference_, 0, e.executionPrice_, e.executedShares_, e.stockLocate_, 'C', e.printable_ });
    }

    void OnOrderCancel(const OrderCancel& e)
    {
        if (Keep(e.stockLocate_))
            events_.push_back(Event{ e.timestamp_, e.orderReference_, 0, 0, e.cancelledShares_, e.stockLocate_, 'X', 0 });
    }

    void OnOrderDelete(const OrderDelete& e)
    {
        if (Keep(e.stockLocate_))
            events_.push_back(Event{ e.timestamp_, e.orderReference_, 0, 0, 0, e.stockLocate_, 'D', 0 });
    }

    void OnOrderReplace(const OrderReplace& e)
    {
        if (Keep(e.stockLocate_))
            events_.push_back(Event{ e.timestamp_, e.originalOrderReference_, e.newOrderReference_, e.price_, e.shares_, e.stockLocate_, 'U', 0 });
    }

    //prints that don't touch the displayed book are kept in the event stream
    //(Apply() ignores them) so a validating replay sees them in feed order
    void OnTrade(const Trade& e)
    {
        if (Keep(e.stockLocate_))
            events_.push_back(Event{ e.timestamp_, e.orderReference_, 0, e.price_, e.shares_, e.stockLocate_, 'P', e.side_ });
    }

    void OnCrossTrade(const CrossTrade& e)
    {
        if (Keep(e.stockLocate_))
        {
            crosses_.push_back({ e.timestamp_, e.shares_, e.crossPrice_, e.crossType_ });
            events_.push_back(Event{ e.timestamp_, e.matchNumber_, 0, e.crossPrice_, static_cast<std::uint32_t>(e.shares_), e.stockLocate_, 'Q', e.crossType_ });
        }
    }

    std::vector<Event>& Events() { return events_; }
    const std::vector<CrossInfo>& Crosses() const { return crosses_; }
    const std::vector<std::pair<std::uint64_t, char>>& SystemEvents() const { return systemEvents_; }
    const std::unordered_map<std::uint16_t, std::string>& Symbols() const { return symbols_; }
    bool FoundSymbol() const { return symbol_.empty() || locate_ != NoLocate; }

private:
    static constexpr std::uint32_t NoLocate = UINT32_MAX;

    //the directory is published before any order messages, so the locate is
    //known by the time the symbol's first order arrives
    bool Keep(std::uint16_t locate) const { return symbol_.empty() || locate == locate_; }

    std::string symbol_;
    std::uint32_t locate_{ NoLocate };
    std::vector<Event> events_;
    std::vector<CrossInfo> crosses_;
    std::vector<std::pair<std::uint64_t, char>> systemEvents_;
    std::unordered_map<std::uint16_t, std::string> symbols_;
};

}
