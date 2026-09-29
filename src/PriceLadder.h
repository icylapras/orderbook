#pragma once

#include <algorithm>
#include <bit>
#include <cstdint>
#include <vector>

#include "Side.h"
#include "Usings.h"

//one side of a book: price -> level slot, kept in price order.
//
//Prices on the tick grid within a band around the market live in a flat
//array indexed by tick, with a two-level occupancy bitmap on top; lookup,
//insert and erase are O(1), and finding the next price level is a couple of
//count-leading/trailing-zero instructions. Everything else (off-tick prices,
//stub quotes far from the market) goes to a sorted overflow vector, so any
//price is accepted.
//
//Why not only the sorted vector: on a real NASDAQ day (AAPL, 2019-12-30) a
//third of all level creations/deletions happen 256-1023 levels away from the
//touch, and every one of them shifts that many entries in a sorted vector.
class PriceLadder
{
public:
    static constexpr std::uint32_t Nil = UINT32_MAX;

    struct Entry
    {
        Price price_;
        std::uint32_t slot_;//Nil if there is no such level

        explicit operator bool() const { return slot_ != Nil; }
    };

    //bandTicks == 0 disables the band: everything lives in the sorted vector
    PriceLadder(Side side, Price tickSize, std::uint32_t bandTicks)
        : side_{ side }
        , tick_{ std::max<Price>(tickSize, 1) }
        , ticks_{ (bandTicks + 63) / 64 * 64 }
        , slots_(ticks_, Nil)
        , bits_(ticks_ / 64, 0)
        , summary_((ticks_ / 64 + 63) / 64, 0)
    {
        //floor(offset / tick) as a multiply and shift; exact while
        //offset < 2^40 / tick, which holds for every in-band offset when
        //ticks * tick^2 < 2^40 (checked here, else fall back to division)
        magic_ = ((std::uint64_t{ 1 } << 40) + static_cast<std::uint64_t>(tick_) - 1) / static_cast<std::uint64_t>(tick_);
        useMagic_ = static_cast<double>(ticks_) * tick_ * tick_ < static_cast<double>(std::uint64_t{ 1 } << 40);
    }

    bool HasBand() const { return ticks_ != 0 && banded_; }

    //fixes the band so it is centred on `price`; called once, on the first order
    void CentreBand(Price price)
    {
        if (ticks_ == 0 || banded_)
            return;
        base_ = static_cast<std::int64_t>(price) - static_cast<std::int64_t>(ticks_ / 2) * tick_;
        banded_ = true;
    }

    std::uint32_t Find(Price price) const
    {
        std::uint32_t index;
        if (BandIndex(price, index))
            return slots_[index];

        const auto i = OverflowLowerBound(price);
        return i < overflow_.size() && overflow_[i].price_ == price ? overflow_[i].slot_ : Nil;
    }

    //price must not already be present
    void Insert(Price price, std::uint32_t slot)
    {
        std::uint32_t index;
        if (BandIndex(price, index))
        {
            slots_[index] = slot;
            SetBit(index);
            return;
        }

        const auto i = OverflowLowerBound(price);
        overflow_.insert(overflow_.begin() + static_cast<std::ptrdiff_t>(i), Entry{ price, slot });
    }

    void Erase(Price price)
    {
        std::uint32_t index;
        if (BandIndex(price, index))
        {
            slots_[index] = Nil;
            ClearBit(index);
            return;
        }

        const auto i = OverflowLowerBound(price);
        overflow_.erase(overflow_.begin() + static_cast<std::ptrdiff_t>(i));
    }

    Entry Best() const
    {
        const auto band = BandEntry(side_ == Side::Buy ? LastUpTo(Last()) : FirstFrom(0));
        const Entry over = overflow_.empty() ? Entry{ 0, Nil } : overflow_.back();
        return Better(band, over);
    }

    Entry Worst() const
    {
        const auto band = BandEntry(side_ == Side::Buy ? FirstFrom(0) : LastUpTo(Last()));
        const Entry over = overflow_.empty() ? Entry{ 0, Nil } : overflow_.front();
        if (!band)
            return over;
        if (!over)
            return band;
        return IsWorse(band.price_, over.price_) ? band : over;
    }

    //visits levels best to worst until f returns false
    template <typename F>
    void ForEachFromBest(F&& f) const
    {
        std::int64_t band = side_ == Side::Buy ? LastUpTo(Last()) : FirstFrom(0);
        std::size_t over = overflow_.size();

        while (band >= 0 || over > 0)
        {
            Entry next;
            const bool takeBand = band >= 0 &&
                (over == 0 || !IsWorse(BandPrice(band), overflow_[over - 1].price_));
            if (takeBand)
            {
                next = Entry{ BandPrice(band), slots_[static_cast<std::size_t>(band)] };
                band = side_ == Side::Buy ? LastUpTo(band - 1) : FirstFrom(band + 1);
            }
            else
                next = overflow_[--over];

            if (!f(next))
                return;
        }
    }

    bool Empty() const { return overflow_.empty() && !AnyBits(); }

    void Clear()
    {
        for (std::size_t w = 0; w < bits_.size(); ++w)
        {
            for (auto word = bits_[w]; word; word &= word - 1)
                slots_[w * 64 + static_cast<std::size_t>(std::countr_zero(word))] = Nil;
            bits_[w] = 0;
        }
        std::fill(summary_.begin(), summary_.end(), 0);
        overflow_.clear();
        banded_ = false;
    }

    std::size_t OverflowSize() const { return overflow_.size(); }

private:
    bool IsWorse(Price a, Price b) const { return side_ == Side::Buy ? a < b : a > b; }

    Entry Better(Entry a, Entry b) const
    {
        if (!a)
            return b;
        if (!b)
            return a;
        return IsWorse(a.price_, b.price_) ? b : a;
    }

    bool BandIndex(Price price, std::uint32_t& index) const
    {
        if (!banded_)
            return false;
        const std::int64_t offset = static_cast<std::int64_t>(price) - base_;
        if (offset < 0 || offset >= static_cast<std::int64_t>(ticks_) * tick_)
            return false;

        const auto off = static_cast<std::uint64_t>(offset);
        const std::uint64_t q = useMagic_ ? (off * magic_) >> 40 : off / static_cast<std::uint64_t>(tick_);
        if (off != q * static_cast<std::uint64_t>(tick_))
            return false;//off the tick grid

        index = static_cast<std::uint32_t>(q);
        return true;
    }

    Price BandPrice(std::int64_t index) const { return static_cast<Price>(base_ + index * tick_); }

    Entry BandEntry(std::int64_t index) const
    {
        return index < 0 ? Entry{ 0, Nil } : Entry{ BandPrice(index), slots_[static_cast<std::size_t>(index)] };
    }

    //overflow is sorted worst -> best; first entry whose price is not worse than `price`
    std::size_t OverflowLowerBound(Price price) const
    {
        const auto it = std::partition_point(overflow_.begin(), overflow_.end(),
            [this, price](const Entry& e) { return IsWorse(e.price_, price); });
        return static_cast<std::size_t>(it - overflow_.begin());
    }

    // ---- bitmap: bits_ has one bit per tick, summary_ one bit per bits_ word

    std::int64_t Last() const { return static_cast<std::int64_t>(ticks_) - 1; }

    bool AnyBits() const
    {
        return std::any_of(summary_.begin(), summary_.end(), [](std::uint64_t w) { return w != 0; });
    }

    void SetBit(std::uint32_t i)
    {
        bits_[i >> 6] |= std::uint64_t{ 1 } << (i & 63);
        summary_[i >> 12] |= std::uint64_t{ 1 } << ((i >> 6) & 63);
    }

    void ClearBit(std::uint32_t i)
    {
        auto& word = bits_[i >> 6];
        word &= ~(std::uint64_t{ 1 } << (i & 63));
        if (word == 0)
            summary_[i >> 12] &= ~(std::uint64_t{ 1 } << ((i >> 6) & 63));
    }

    //lowest set index >= i, or -1
    std::int64_t FirstFrom(std::int64_t i) const
    {
        if (i < 0)
            i = 0;
        if (i >= static_cast<std::int64_t>(ticks_))
            return -1;

        auto w = i >> 6;
        const auto word = bits_[static_cast<std::size_t>(w)] & (~std::uint64_t{ 0 } << (i & 63));
        if (word)
            return (w << 6) + std::countr_zero(word);

        if (++w >= static_cast<std::int64_t>(bits_.size()))
            return -1;
        auto s = w >> 6;
        auto summary = summary_[static_cast<std::size_t>(s)] & (~std::uint64_t{ 0 } << (w & 63));
        while (!summary)
        {
            if (++s >= static_cast<std::int64_t>(summary_.size()))
                return -1;
            summary = summary_[static_cast<std::size_t>(s)];
        }
        w = (s << 6) + std::countr_zero(summary);
        return (w << 6) + std::countr_zero(bits_[static_cast<std::size_t>(w)]);
    }

    //highest set index <= i, or -1
    std::int64_t LastUpTo(std::int64_t i) const
    {
        if (i < 0)
            return -1;
        i = std::min(i, Last());

        auto w = i >> 6;
        const auto keep = (i & 63) == 63 ? ~std::uint64_t{ 0 } : (std::uint64_t{ 1 } << ((i & 63) + 1)) - 1;
        const auto word = bits_[static_cast<std::size_t>(w)] & keep;
        if (word)
            return (w << 6) + 63 - std::countl_zero(word);

        if (--w < 0)
            return -1;
        auto s = w >> 6;
        const auto keepWords = (w & 63) == 63 ? ~std::uint64_t{ 0 } : (std::uint64_t{ 1 } << ((w & 63) + 1)) - 1;
        auto summary = summary_[static_cast<std::size_t>(s)] & keepWords;
        while (!summary)
        {
            if (--s < 0)
                return -1;
            summary = summary_[static_cast<std::size_t>(s)];
        }
        w = (s << 6) + 63 - std::countl_zero(summary);
        return (w << 6) + 63 - std::countl_zero(bits_[static_cast<std::size_t>(w)]);
    }

    Side side_;
    Price tick_;
    std::uint32_t ticks_;
    std::int64_t base_{ };
    bool banded_{ false };
    std::uint64_t magic_{ };
    bool useMagic_{ };

    std::vector<std::uint32_t> slots_;  //tick index -> level slot
    std::vector<std::uint64_t> bits_;
    std::vector<std::uint64_t> summary_;
    std::vector<Entry> overflow_;       //sorted worst -> best
};
