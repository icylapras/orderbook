#pragma once

//NASDAQ TotalView-ITCH 5.0 decoder.
//
//File framing ("BinaryFILE"): every message is preceded by a 2-byte
//big-endian length. All integer fields are big-endian; prices are unsigned
//4-decimal fixed point (e.g. 1234500 = $123.45); timestamps are 6-byte
//nanoseconds since midnight.
//
//Decoding is zero-copy: Parse() walks a byte buffer and hands each message,
//decoded into a small host-endian struct, to a handler. A handler only
//implements the callbacks it cares about (checked with requires-expressions),
//so an unused message type costs one switch branch and nothing else.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace itch
{

inline std::uint16_t Load16(const std::uint8_t* p)
{
    std::uint16_t v;
    std::memcpy(&v, p, sizeof v);
    return __builtin_bswap16(v);
}

inline std::uint32_t Load32(const std::uint8_t* p)
{
    std::uint32_t v;
    std::memcpy(&v, p, sizeof v);
    return __builtin_bswap32(v);
}

inline std::uint64_t Load64(const std::uint8_t* p)
{
    std::uint64_t v;
    std::memcpy(&v, p, sizeof v);
    return __builtin_bswap64(v);
}

inline std::uint64_t Load48(const std::uint8_t* p)
{
    return (static_cast<std::uint64_t>(Load16(p)) << 32) | Load32(p + 2);
}

//8-char space-padded ASCII stock symbol
struct Symbol
{
    std::array<char, 8> chars_{ };

    std::string_view View() const
    {
        std::size_t n = chars_.size();
        while (n > 0 && chars_[n - 1] == ' ')
            --n;
        return { chars_.data(), n };
    }
};

inline Symbol LoadSymbol(const std::uint8_t* p)
{
    Symbol s;
    std::memcpy(s.chars_.data(), p, s.chars_.size());
    return s;
}

//every message starts with this 11-byte header after its type byte
struct Header
{
    std::uint16_t stockLocate_;
    std::uint16_t trackingNumber_;
    std::uint64_t timestamp_;//ns since midnight
};

struct SystemEvent : Header { char eventCode_; };                          //'S'
struct StockDirectory : Header { Symbol stock_; char marketCategory_; };   //'R' (fields we use)
struct StockTradingAction : Header { Symbol stock_; char tradingState_; }; //'H'

struct AddOrder : Header                                                   //'A' and 'F'
{
    std::uint64_t orderReference_;
    char side_;//'B' or 'S'
    std::uint32_t shares_;
    Symbol stock_;
    std::uint32_t price_;
    bool hasAttribution_;
    std::array<char, 4> attribution_;
};

struct OrderExecuted : Header                                              //'E'
{
    std::uint64_t orderReference_;
    std::uint32_t executedShares_;
    std::uint64_t matchNumber_;
};

struct OrderExecutedWithPrice : OrderExecuted                              //'C'
{
    char printable_;
    std::uint32_t executionPrice_;
};

struct OrderCancel : Header                                                //'X'
{
    std::uint64_t orderReference_;
    std::uint32_t cancelledShares_;
};

struct OrderDelete : Header { std::uint64_t orderReference_; };            //'D'

struct OrderReplace : Header                                               //'U'
{
    std::uint64_t originalOrderReference_;
    std::uint64_t newOrderReference_;
    std::uint32_t shares_;
    std::uint32_t price_;
};

struct Trade : Header                                                      //'P' (non-displayed order executions)
{
    std::uint64_t orderReference_;
    char side_;
    std::uint32_t shares_;
    Symbol stock_;
    std::uint32_t price_;
    std::uint64_t matchNumber_;
};

struct CrossTrade : Header                                                 //'Q' (opening/closing/IPO/halt cross)
{
    std::uint64_t shares_;
    Symbol stock_;
    std::uint32_t crossPrice_;
    std::uint64_t matchNumber_;
    char crossType_;//'O' open, 'C' close, 'H' halt/IPO, 'I' intraday
};

//exact on-wire message length (type byte included) for every ITCH 5.0 type;
//0 = unknown type
constexpr std::size_t MessageLength(char type)
{
    switch (type)
    {
    case 'S': return 12;
    case 'R': return 39;
    case 'H': return 25;
    case 'Y': return 20;
    case 'L': return 26;
    case 'V': return 35;
    case 'W': return 12;
    case 'K': return 28;
    case 'J': return 35;
    case 'h': return 21;
    case 'A': return 36;
    case 'F': return 40;
    case 'E': return 31;
    case 'C': return 36;
    case 'X': return 23;
    case 'D': return 19;
    case 'U': return 35;
    case 'P': return 44;
    case 'Q': return 40;
    case 'B': return 19;
    case 'I': return 50;
    case 'N': return 20;
    case 'O': return 48;
    default:  return 0;
    }
}

struct ParseStats
{
    std::uint64_t messages_{ };
    std::uint64_t malformed_{ };//length prefix disagrees with the type's fixed size
    std::uint64_t unknown_{ };  //type byte not in the 5.0 spec (skipped via the length prefix)
    std::array<std::uint64_t, 128> byType_{ };
};

namespace detail
{

inline Header LoadHeader(const std::uint8_t* m)
{
    return Header{ Load16(m + 1), Load16(m + 3), Load48(m + 5) };
}

//a zeroed message struct with its common header filled in
template <typename Message>
Message WithHeader(const std::uint8_t* m)
{
    Message message{ };
    static_cast<Header&>(message) = LoadHeader(m);
    return message;
}

}

//decodes one message body (no length prefix) of exactly MessageLength(type)
//bytes and dispatches it
template <typename Handler>
void Dispatch(const std::uint8_t* m, Handler& handler)
{
    const char type = static_cast<char>(m[0]);

    switch (type)
    {
    case 'S':
        if constexpr (requires(SystemEvent e) { handler.OnSystemEvent(e); })
        {
            auto e = detail::WithHeader<SystemEvent>(m);
            e.eventCode_ = static_cast<char>(m[11]);
            handler.OnSystemEvent(e);
        }
        break;
    case 'R':
        if constexpr (requires(StockDirectory e) { handler.OnStockDirectory(e); })
        {
            auto e = detail::WithHeader<StockDirectory>(m);
            e.stock_ = LoadSymbol(m + 11);
            e.marketCategory_ = static_cast<char>(m[19]);
            handler.OnStockDirectory(e);
        }
        break;
    case 'H':
        if constexpr (requires(StockTradingAction e) { handler.OnStockTradingAction(e); })
        {
            auto e = detail::WithHeader<StockTradingAction>(m);
            e.stock_ = LoadSymbol(m + 11);
            e.tradingState_ = static_cast<char>(m[19]);
            handler.OnStockTradingAction(e);
        }
        break;
    case 'A':
    case 'F':
        if constexpr (requires(AddOrder e) { handler.OnAddOrder(e); })
        {
            auto e = detail::WithHeader<AddOrder>(m);
            e.orderReference_ = Load64(m + 11);
            e.side_ = static_cast<char>(m[19]);
            e.shares_ = Load32(m + 20);
            e.stock_ = LoadSymbol(m + 24);
            e.price_ = Load32(m + 32);
            e.hasAttribution_ = type == 'F';
            e.attribution_ = { };
            if (e.hasAttribution_)
                std::memcpy(e.attribution_.data(), m + 36, 4);
            handler.OnAddOrder(e);
        }
        break;
    case 'E':
        if constexpr (requires(OrderExecuted e) { handler.OnOrderExecuted(e); })
        {
            auto e = detail::WithHeader<OrderExecuted>(m);
            e.orderReference_ = Load64(m + 11);
            e.executedShares_ = Load32(m + 19);
            e.matchNumber_ = Load64(m + 23);
            handler.OnOrderExecuted(e);
        }
        break;
    case 'C':
        if constexpr (requires(OrderExecutedWithPrice e) { handler.OnOrderExecutedWithPrice(e); })
        {
            auto e = detail::WithHeader<OrderExecutedWithPrice>(m);
            e.orderReference_ = Load64(m + 11);
            e.executedShares_ = Load32(m + 19);
            e.matchNumber_ = Load64(m + 23);
            e.printable_ = static_cast<char>(m[31]);
            e.executionPrice_ = Load32(m + 32);
            handler.OnOrderExecutedWithPrice(e);
        }
        break;
    case 'X':
        if constexpr (requires(OrderCancel e) { handler.OnOrderCancel(e); })
        {
            auto e = detail::WithHeader<OrderCancel>(m);
            e.orderReference_ = Load64(m + 11);
            e.cancelledShares_ = Load32(m + 19);
            handler.OnOrderCancel(e);
        }
        break;
    case 'D':
        if constexpr (requires(OrderDelete e) { handler.OnOrderDelete(e); })
        {
            auto e = detail::WithHeader<OrderDelete>(m);
            e.orderReference_ = Load64(m + 11);
            handler.OnOrderDelete(e);
        }
        break;
    case 'U':
        if constexpr (requires(OrderReplace e) { handler.OnOrderReplace(e); })
        {
            auto e = detail::WithHeader<OrderReplace>(m);
            e.originalOrderReference_ = Load64(m + 11);
            e.newOrderReference_ = Load64(m + 19);
            e.shares_ = Load32(m + 27);
            e.price_ = Load32(m + 31);
            handler.OnOrderReplace(e);
        }
        break;
    case 'P':
        if constexpr (requires(Trade e) { handler.OnTrade(e); })
        {
            auto e = detail::WithHeader<Trade>(m);
            e.orderReference_ = Load64(m + 11);
            e.side_ = static_cast<char>(m[19]);
            e.shares_ = Load32(m + 20);
            e.stock_ = LoadSymbol(m + 24);
            e.price_ = Load32(m + 32);
            e.matchNumber_ = Load64(m + 36);
            handler.OnTrade(e);
        }
        break;
    case 'Q':
        if constexpr (requires(CrossTrade e) { handler.OnCrossTrade(e); })
        {
            auto e = detail::WithHeader<CrossTrade>(m);
            e.shares_ = Load64(m + 11);
            e.stock_ = LoadSymbol(m + 19);
            e.crossPrice_ = Load32(m + 27);
            e.matchNumber_ = Load64(m + 31);
            e.crossType_ = static_cast<char>(m[39]);
            handler.OnCrossTrade(e);
        }
        break;
    default:
        //remaining types (Reg SHO, MWCB, NOII, ...) are valid but not needed for the book
        break;
    }
}

//parses every complete length-prefixed message in [data, data + size) and
//returns the number of bytes consumed; a trailing partial message is left for
//the caller to carry into the next buffer
template <typename Handler>
std::size_t Parse(const std::uint8_t* data, std::size_t size, Handler& handler, ParseStats& stats)
{
    std::size_t offset = 0;

    while (size - offset >= 2)
    {
        const std::size_t length = Load16(data + offset);
        if (size - offset - 2 < length)
            break;

        const std::uint8_t* message = data + offset + 2;
        offset += 2 + length;

        if (length == 0)
        {
            ++stats.malformed_;
            continue;
        }

        const char type = static_cast<char>(message[0]);
        const std::size_t expected = MessageLength(type);
        if (expected == 0)
        {
            ++stats.unknown_;
            continue;
        }
        if (length != expected)
        {
            ++stats.malformed_;
            continue;
        }

        ++stats.messages_;
        ++stats.byType_[static_cast<std::uint8_t>(type) & 0x7F];
        Dispatch(message, handler);
    }

    return offset;
}

}
