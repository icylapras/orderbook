#pragma once

#include <limits>

#include "Usings.h"

struct Constants
{
    static constexpr Price InvalidPrice = std::numeric_limits<Price>::max();
};
