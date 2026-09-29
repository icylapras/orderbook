#pragma once

//encodes ITCH 5.0 messages (with the 2-byte length framing); used to build
//parser tests and synthetic feeds for CI, where the real multi-GB NASDAQ
//files aren't available

#include <cstdint>
#include <string_view>
#include <vector>

#include "Itch.h"

namespace itch
{

class Writer
{
public:
    void SystemEvent(std::uint16_t locate, std::uint64_t timestamp, char code)
    {
        Begin('S', locate, timestamp);
        Put8(code);
        End();
    }

    void StockDirectory(std::uint16_t locate, std::uint64_t timestamp, std::string_view stock)
    {
        Begin('R', locate, timestamp);
        PutSymbol(stock);
        Put8('Q');//market category: NASDAQ Global Select
        Put8('N');//financial status
        Put32(100);//round lot size
        Put8('N');//round lots only
        Put8('C');//issue classification
        Put8('Z'); Put8(' ');//issue sub-type
        Put8('P');//authenticity
        Put8('N');//short sale threshold
        Put8('N');//IPO flag
        Put8('1');//LULD tier
        Put8('N');//ETP flag
        Put32(0);//ETP leverage
        Put8('N');//inverse
        End();
    }

    void AddOrder(std::uint16_t locate, std::uint64_t timestamp, std::uint64_t reference, char side,
        std::uint32_t shares, std::string_view stock, std::uint32_t price, std::string_view mpid = { })
    {
        Begin(mpid.empty() ? 'A' : 'F', locate, timestamp);
        Put64(reference);
        Put8(side);
        Put32(shares);
        PutSymbol(stock);
        Put32(price);
        if (!mpid.empty())
            PutPadded(mpid, 4);
        End();
    }

    void OrderExecuted(std::uint16_t locate, std::uint64_t timestamp, std::uint64_t reference,
        std::uint32_t shares, std::uint64_t match)
    {
        Begin('E', locate, timestamp);
        Put64(reference);
        Put32(shares);
        Put64(match);
        End();
    }

    void OrderExecutedWithPrice(std::uint16_t locate, std::uint64_t timestamp, std::uint64_t reference,
        std::uint32_t shares, std::uint64_t match, char printable, std::uint32_t price)
    {
        Begin('C', locate, timestamp);
        Put64(reference);
        Put32(shares);
        Put64(match);
        Put8(printable);
        Put32(price);
        End();
    }

    void OrderCancel(std::uint16_t locate, std::uint64_t timestamp, std::uint64_t reference, std::uint32_t shares)
    {
        Begin('X', locate, timestamp);
        Put64(reference);
        Put32(shares);
        End();
    }

    void OrderDelete(std::uint16_t locate, std::uint64_t timestamp, std::uint64_t reference)
    {
        Begin('D', locate, timestamp);
        Put64(reference);
        End();
    }

    void OrderReplace(std::uint16_t locate, std::uint64_t timestamp, std::uint64_t original,
        std::uint64_t replacement, std::uint32_t shares, std::uint32_t price)
    {
        Begin('U', locate, timestamp);
        Put64(original);
        Put64(replacement);
        Put32(shares);
        Put32(price);
        End();
    }

    void Trade(std::uint16_t locate, std::uint64_t timestamp, std::uint64_t reference, char side,
        std::uint32_t shares, std::string_view stock, std::uint32_t price, std::uint64_t match)
    {
        Begin('P', locate, timestamp);
        Put64(reference);
        Put8(side);
        Put32(shares);
        PutSymbol(stock);
        Put32(price);
        Put64(match);
        End();
    }

    void CrossTrade(std::uint16_t locate, std::uint64_t timestamp, std::uint64_t shares,
        std::string_view stock, std::uint32_t price, std::uint64_t match, char crossType)
    {
        Begin('Q', locate, timestamp);
        Put64(shares);
        PutSymbol(stock);
        Put32(price);
        Put64(match);
        Put8(crossType);
        End();
    }

    const std::vector<std::uint8_t>& Bytes() const { return bytes_; }
    std::vector<std::uint8_t>& Bytes() { return bytes_; }

private:
    void Begin(char type, std::uint16_t locate, std::uint64_t timestamp)
    {
        start_ = bytes_.size();
        Put16(0);//length, patched in End()
        Put8(type);
        Put16(locate);
        Put16(0);//tracking number
        Put16(static_cast<std::uint16_t>(timestamp >> 32));
        Put32(static_cast<std::uint32_t>(timestamp));
    }

    void End()
    {
        const auto length = static_cast<std::uint16_t>(bytes_.size() - start_ - 2);
        bytes_[start_] = static_cast<std::uint8_t>(length >> 8);
        bytes_[start_ + 1] = static_cast<std::uint8_t>(length);
    }

    void Put8(char c) { bytes_.push_back(static_cast<std::uint8_t>(c)); }
    void Put16(std::uint16_t v) { for (int s = 8; s >= 0; s -= 8) bytes_.push_back(static_cast<std::uint8_t>(v >> s)); }
    void Put32(std::uint32_t v) { for (int s = 24; s >= 0; s -= 8) bytes_.push_back(static_cast<std::uint8_t>(v >> s)); }
    void Put64(std::uint64_t v) { for (int s = 56; s >= 0; s -= 8) bytes_.push_back(static_cast<std::uint8_t>(v >> s)); }

    void PutPadded(std::string_view s, std::size_t width)
    {
        for (std::size_t i = 0; i < width; ++i)
            Put8(i < s.size() ? s[i] : ' ');
    }
    void PutSymbol(std::string_view s) { PutPadded(s, 8); }

    std::vector<std::uint8_t> bytes_;
    std::size_t start_{ };
};

}
