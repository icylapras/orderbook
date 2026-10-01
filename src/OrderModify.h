#pragma once

#include "Side.h"
#include "Usings.h"

//modify (cancel-replace) an order: new side, price and quantity; the order
//keeps its id and type but loses its place in the queue
class OrderModify
{
public:
    OrderModify(OrderId orderId, Side side, Price price, Quantity quantity)
        : orderId_{ orderId }
        , price_{ price }
        , side_{ side }
        , quantity_{ quantity }
    { }

    OrderId GetOrderId() const { return orderId_; }
    Price GetPrice() const { return price_; }
    Side GetSide() const { return side_; }
    Quantity GetQuantity() const { return quantity_; }

private:
    OrderId orderId_;
    Price price_;
    Side side_;
    Quantity quantity_;
};
