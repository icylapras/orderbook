#pragma once

enum class OrderType
{
    GoodTillCancel,
    FillAndKill,
    FillOrKill,
    Market,
    GoodForDay
};