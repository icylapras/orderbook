#pragma once

#include "Constants.h"
#include "Usings.h"

//best bid/ask and the total quantity resting at each; an empty side has
//price InvalidPrice and quantity 0
struct TopOfBook
{
    Price bidPrice_{ Constants::InvalidPrice };
    Quantity bidQuantity_{ };
    Price askPrice_{ Constants::InvalidPrice };
    Quantity askQuantity_{ };

    bool HasBid() const { return bidQuantity_ != 0; }
    bool HasAsk() const { return askQuantity_ != 0; }
    bool IsCrossed() const { return HasBid() && HasAsk() && bidPrice_ >= askPrice_; }

    bool operator==(const TopOfBook&) const = default;
};
