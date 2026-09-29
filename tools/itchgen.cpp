//generates a synthetic but internally consistent ITCH 5.0 session, so CI can
//run the full replay + validation pipeline without the multi-GB NASDAQ files.
//
//  itchgen <out-file> [--symbols N] [--messages N] [--seed N]
//
//each symbol's feed is produced from a small model book, so it has the
//properties of a real feed that the replay checks: the book never crosses,
//executions always hit the front order at the best price, every reference is
//live, and hidden prints / the closing cross sit inside the spread

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "itch/ItchWriter.h"

namespace
{

constexpr std::uint32_t Tick = 100;//$0.01 in ITCH price units

struct Live
{
    char side_;
    std::uint32_t price_;
    std::uint32_t shares_;
    std::size_t slot_;//index in the sampling vector
};

class SymbolModel
{
public:
    SymbolModel(std::uint16_t locate, std::string name, std::uint32_t mid, std::uint64_t& nextRef, std::uint64_t& nextMatch)
        : locate_{ locate }, name_{ std::move(name) }, mid_{ mid }, nextRef_{ nextRef }, nextMatch_{ nextMatch } { }

    void Step(itch::Writer& w, std::uint64_t ts, std::mt19937_64& rng)
    {
        std::uniform_int_distribution<int> percent{ 0, 99 };

        //random walk of the fair price, one tick at a time
        if (percent(rng) < 10)
            mid_ = percent(rng) < 50 ? mid_ + Tick : std::max(mid_ - Tick, 50 * Tick);

        const int roll = percent(rng);
        if (live_.size() < 20 || roll < 40)
            Add(w, ts, rng);
        else if (roll < 55)
            PartialCancel(w, ts, rng);
        else if (roll < 70)
            Delete(w, ts, rng);
        else if (roll < 80)
            Replace(w, ts, rng);
        else if (roll < 95)
            Execute(w, ts, rng, percent(rng) < 85);
        else
            HiddenPrint(w, ts, rng);
    }

    void Close(itch::Writer& w, std::uint64_t ts)
    {
        if (bids_.empty() || asks_.empty())
            return;
        const auto price = (bids_.rbegin()->first + asks_.begin()->first) / 2;
        w.CrossTrade(locate_, ts, 1000, name_, price, nextMatch_++, 'C');
    }

private:
    std::uint32_t PickPrice(char side, std::mt19937_64& rng) const
    {
        std::uniform_int_distribution<std::uint32_t> away{ 0, 12 };
        if (side == 'B')
        {
            std::uint32_t price = mid_ - Tick * (1 + away(rng));
            if (!asks_.empty())
                price = std::min(price, asks_.begin()->first - Tick);
            return std::max(price, Tick);
        }
        std::uint32_t price = mid_ + Tick * (1 + away(rng));
        if (!bids_.empty())
            price = std::max(price, bids_.rbegin()->first + Tick);
        return price;
    }

    void Insert(std::uint64_t ref, char side, std::uint32_t price, std::uint32_t shares)
    {
        auto& levels = side == 'B' ? bids_ : asks_;
        levels[price].push_back(ref);
        live_[ref] = Live{ side, price, shares, refs_.size() };
        refs_.push_back(ref);
    }

    void Remove(std::uint64_t ref)
    {
        const auto it = live_.find(ref);
        auto& levels = it->second.side_ == 'B' ? bids_ : asks_;
        auto& queue = levels[it->second.price_];
        queue.erase(std::find(queue.begin(), queue.end(), ref));
        if (queue.empty())
            levels.erase(it->second.price_);

        //swap-remove from the sampling vector
        const auto slot = it->second.slot_;
        refs_[slot] = refs_.back();
        live_[refs_[slot]].slot_ = slot;
        refs_.pop_back();
        live_.erase(ref);
    }

    std::uint64_t RandomRef(std::mt19937_64& rng) const
    {
        return refs_[std::uniform_int_distribution<std::size_t>{ 0, refs_.size() - 1 }(rng)];
    }

    void Add(itch::Writer& w, std::uint64_t ts, std::mt19937_64& rng)
    {
        const char side = rng() % 2 ? 'B' : 'S';
        const auto price = PickPrice(side, rng);
        const auto shares = static_cast<std::uint32_t>(100 * (1 + rng() % 10));
        const auto ref = nextRef_++;
        w.AddOrder(locate_, ts, ref, side, shares, name_, price, rng() % 4 == 0 ? "MMKR" : "");
        Insert(ref, side, price, shares);
    }

    void PartialCancel(itch::Writer& w, std::uint64_t ts, std::mt19937_64& rng)
    {
        const auto ref = RandomRef(rng);
        auto& order = live_[ref];
        if (order.shares_ < 2)
            return Delete(w, ts, rng);
        const auto cancelled = static_cast<std::uint32_t>(1 + rng() % (order.shares_ - 1));
        w.OrderCancel(locate_, ts, ref, cancelled);
        order.shares_ -= cancelled;
    }

    void Delete(itch::Writer& w, std::uint64_t ts, std::mt19937_64& rng)
    {
        const auto ref = RandomRef(rng);
        w.OrderDelete(locate_, ts, ref);
        Remove(ref);
    }

    void Replace(itch::Writer& w, std::uint64_t ts, std::mt19937_64& rng)
    {
        const auto ref = RandomRef(rng);
        const char side = live_[ref].side_;
        Remove(ref);
        const auto price = PickPrice(side, rng);
        const auto shares = static_cast<std::uint32_t>(100 * (1 + rng() % 10));
        const auto newRef = nextRef_++;
        w.OrderReplace(locate_, ts, ref, newRef, shares, price);
        Insert(newRef, side, price, shares);
    }

    //an incoming marketable order hits the front of the best level
    void Execute(itch::Writer& w, std::uint64_t ts, std::mt19937_64& rng, bool plain)
    {
        const bool hitBid = rng() % 2;
        auto& levels = hitBid ? bids_ : asks_;
        if (levels.empty())
            return;

        const auto ref = hitBid ? levels.rbegin()->second.front() : levels.begin()->second.front();
        auto& order = live_[ref];
        const auto shares = static_cast<std::uint32_t>(1 + rng() % order.shares_);
        if (plain)
            w.OrderExecuted(locate_, ts, ref, shares, nextMatch_++);
        else
            w.OrderExecutedWithPrice(locate_, ts, ref, shares, nextMatch_++, 'Y', order.price_);

        if (shares == order.shares_)
            Remove(ref);
        else
            order.shares_ -= shares;
    }

    void HiddenPrint(itch::Writer& w, std::uint64_t ts, std::mt19937_64& rng)
    {
        if (bids_.empty() || asks_.empty())
            return;
        const auto bid = bids_.rbegin()->first;
        const auto ask = asks_.begin()->first;
        const auto price = bid + static_cast<std::uint32_t>(rng() % (ask - bid + 1));
        w.Trade(locate_, ts, 0, 'B', 100, name_, price, nextMatch_++);
    }

    std::uint16_t locate_;
    std::string name_;
    std::uint32_t mid_;
    std::uint64_t& nextRef_;
    std::uint64_t& nextMatch_;
    std::map<std::uint32_t, std::deque<std::uint64_t>> bids_, asks_;
    std::unordered_map<std::uint64_t, Live> live_;
    std::vector<std::uint64_t> refs_;
};

}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: itchgen <out-file> [--symbols N] [--messages N] [--seed N]\n");
        return 1;
    }

    const std::string out = argv[1];
    int symbols = 4;
    std::uint64_t messages = 1'000'000;
    std::uint64_t seed = 1;
    for (int i = 2; i + 1 < argc; i += 2)
    {
        const std::string arg = argv[i];
        if (arg == "--symbols") symbols = std::max(1, std::atoi(argv[i + 1]));
        else if (arg == "--messages") messages = std::strtoull(argv[i + 1], nullptr, 10);
        else if (arg == "--seed") seed = std::strtoull(argv[i + 1], nullptr, 10);
    }

    std::mt19937_64 rng{ seed };
    std::uint64_t nextRef = 1, nextMatch = 1;
    itch::Writer w;

    constexpr std::uint64_t Hour = 3'600'000'000'000ull;
    const std::uint64_t open = 4 * Hour, close = 16 * Hour;
    w.SystemEvent(0, open - 1, 'O');

    std::vector<SymbolModel> models;
    for (int s = 0; s < symbols; ++s)
    {
        const auto locate = static_cast<std::uint16_t>(s + 1);
        const auto name = "SYN" + std::to_string(s);
        w.StockDirectory(locate, open, name);
        models.emplace_back(locate, name, static_cast<std::uint32_t>(100 + 50 * s) * 10000, nextRef, nextMatch);
    }
    w.SystemEvent(0, open, 'S');

    std::uniform_int_distribution<int> pick{ 0, symbols - 1 };
    for (std::uint64_t i = 0; i < messages; ++i)
    {
        //divide first: (close - open) * i overflows 64 bits for long sessions
        const auto ts = open + (close - open) / (messages + 1) * i;
        models[static_cast<std::size_t>(pick(rng))].Step(w, ts, rng);
    }

    for (auto& m : models)
        m.Close(w, close);
    w.SystemEvent(0, close, 'M');
    w.SystemEvent(0, close + 1, 'C');

    std::FILE* f = std::fopen(out.c_str(), "wb");
    if (!f)
    {
        std::perror(out.c_str());
        return 1;
    }
    std::fwrite(w.Bytes().data(), 1, w.Bytes().size(), f);
    std::fclose(f);

    std::printf("wrote %s: %zu bytes, %d symbols\n", out.c_str(), w.Bytes().size(), symbols);
    return 0;
}
