#include <gtest/gtest.h>

#include <cstdio>
#include <string>
#include <vector>

#include "EngineAdapters.h"
#include "itch/ItchFile.h"
#include "itch/ItchWriter.h"
#include "itch/Replay.h"

namespace
{

//records everything it's handed
struct Recorder
{
    std::vector<itch::SystemEvent> system_;
    std::vector<itch::StockDirectory> directory_;
    std::vector<itch::AddOrder> adds_;
    std::vector<itch::OrderExecuted> executed_;
    std::vector<itch::OrderExecutedWithPrice> executedWithPrice_;
    std::vector<itch::OrderCancel> cancels_;
    std::vector<itch::OrderDelete> deletes_;
    std::vector<itch::OrderReplace> replaces_;
    std::vector<itch::Trade> trades_;
    std::vector<itch::CrossTrade> crosses_;

    void OnSystemEvent(const itch::SystemEvent& e) { system_.push_back(e); }
    void OnStockDirectory(const itch::StockDirectory& e) { directory_.push_back(e); }
    void OnAddOrder(const itch::AddOrder& e) { adds_.push_back(e); }
    void OnOrderExecuted(const itch::OrderExecuted& e) { executed_.push_back(e); }
    void OnOrderExecutedWithPrice(const itch::OrderExecutedWithPrice& e) { executedWithPrice_.push_back(e); }
    void OnOrderCancel(const itch::OrderCancel& e) { cancels_.push_back(e); }
    void OnOrderDelete(const itch::OrderDelete& e) { deletes_.push_back(e); }
    void OnOrderReplace(const itch::OrderReplace& e) { replaces_.push_back(e); }
    void OnTrade(const itch::Trade& e) { trades_.push_back(e); }
    void OnCrossTrade(const itch::CrossTrade& e) { crosses_.push_back(e); }
};

constexpr std::uint64_t Ts = 34'200'123'456'789ull;//09:30:00.123456789, needs all 6 timestamp bytes

}

TEST(ItchParser, DecodesBigEndianFields)
{
    const std::uint8_t bytes[] = { 0x12, 0x34, 0x89, 0xAB, 0xCD, 0xEF, 0x01, 0x02, 0x03, 0x04 };
    EXPECT_EQ(itch::Load16(bytes), 0x1234);
    EXPECT_EQ(itch::Load32(bytes), 0x123489ABu);
    EXPECT_EQ(itch::Load48(bytes), 0x123489ABCDEFull);
    EXPECT_EQ(itch::Load64(bytes), 0x123489ABCDEF0102ull);
}

TEST(ItchParser, RoundTripsEveryBookMessage)
{
    itch::Writer w;
    w.SystemEvent(0, Ts, 'Q');
    w.StockDirectory(13, Ts, "AAPL");
    w.AddOrder(13, Ts, 0x0102030405060708ull, 'B', 100, "AAPL", 2915200);
    w.AddOrder(13, Ts + 1, 42, 'S', 250, "AAPL", 2915300, "GSCO");
    w.OrderExecuted(13, Ts + 2, 42, 50, 777);
    w.OrderExecutedWithPrice(13, Ts + 3, 42, 25, 778, 'Y', 2915250);
    w.OrderCancel(13, Ts + 4, 42, 10);
    w.OrderReplace(13, Ts + 5, 42, 43, 300, 2915400);
    w.OrderDelete(13, Ts + 6, 43);
    w.Trade(13, Ts + 7, 0, 'B', 500, "AAPL", 2915225, 779);
    w.CrossTrade(13, Ts + 8, 1'234'567, "AAPL", 2915100, 780, 'C');

    Recorder r;
    itch::ParseStats stats;
    const auto consumed = itch::Parse(w.Bytes().data(), w.Bytes().size(), r, stats);

    EXPECT_EQ(consumed, w.Bytes().size());
    EXPECT_EQ(stats.messages_, 11u);
    EXPECT_EQ(stats.malformed_, 0u);

    ASSERT_EQ(r.system_.size(), 1u);
    EXPECT_EQ(r.system_[0].eventCode_, 'Q');
    EXPECT_EQ(r.system_[0].timestamp_, Ts);

    ASSERT_EQ(r.directory_.size(), 1u);
    EXPECT_EQ(r.directory_[0].stock_.View(), "AAPL");
    EXPECT_EQ(r.directory_[0].stockLocate_, 13);

    ASSERT_EQ(r.adds_.size(), 2u);
    EXPECT_EQ(r.adds_[0].orderReference_, 0x0102030405060708ull);
    EXPECT_EQ(r.adds_[0].side_, 'B');
    EXPECT_EQ(r.adds_[0].shares_, 100u);
    EXPECT_EQ(r.adds_[0].price_, 2915200u);
    EXPECT_EQ(r.adds_[0].stock_.View(), "AAPL");
    EXPECT_FALSE(r.adds_[0].hasAttribution_);
    EXPECT_EQ(r.adds_[0].stockLocate_, 13);
    EXPECT_EQ(r.adds_[0].timestamp_, Ts);
    EXPECT_TRUE(r.adds_[1].hasAttribution_);
    EXPECT_EQ(std::string(r.adds_[1].attribution_.data(), 4), "GSCO");
    EXPECT_EQ(r.adds_[1].side_, 'S');

    ASSERT_EQ(r.executed_.size(), 1u);
    EXPECT_EQ(r.executed_[0].orderReference_, 42u);
    EXPECT_EQ(r.executed_[0].executedShares_, 50u);
    EXPECT_EQ(r.executed_[0].matchNumber_, 777u);

    ASSERT_EQ(r.executedWithPrice_.size(), 1u);
    EXPECT_EQ(r.executedWithPrice_[0].executedShares_, 25u);
    EXPECT_EQ(r.executedWithPrice_[0].printable_, 'Y');
    EXPECT_EQ(r.executedWithPrice_[0].executionPrice_, 2915250u);

    ASSERT_EQ(r.cancels_.size(), 1u);
    EXPECT_EQ(r.cancels_[0].cancelledShares_, 10u);

    ASSERT_EQ(r.replaces_.size(), 1u);
    EXPECT_EQ(r.replaces_[0].originalOrderReference_, 42u);
    EXPECT_EQ(r.replaces_[0].newOrderReference_, 43u);
    EXPECT_EQ(r.replaces_[0].shares_, 300u);
    EXPECT_EQ(r.replaces_[0].price_, 2915400u);

    ASSERT_EQ(r.deletes_.size(), 1u);
    EXPECT_EQ(r.deletes_[0].orderReference_, 43u);

    ASSERT_EQ(r.trades_.size(), 1u);
    EXPECT_EQ(r.trades_[0].price_, 2915225u);
    EXPECT_EQ(r.trades_[0].matchNumber_, 779u);

    ASSERT_EQ(r.crosses_.size(), 1u);
    EXPECT_EQ(r.crosses_[0].shares_, 1'234'567u);
    EXPECT_EQ(r.crosses_[0].crossPrice_, 2915100u);
    EXPECT_EQ(r.crosses_[0].crossType_, 'C');
}

TEST(ItchParser, EveryWrittenMessageHasItsSpecLength)
{
    itch::Writer w;
    auto lastLength = [&w](std::size_t start) { return itch::Load16(w.Bytes().data() + start); };
    auto check = [&](char type, auto&& write)
    {
        const auto start = w.Bytes().size();
        write();
        EXPECT_EQ(lastLength(start), itch::MessageLength(type)) << type;
    };
    check('S', [&] { w.SystemEvent(1, Ts, 'O'); });
    check('R', [&] { w.StockDirectory(1, Ts, "MSFT"); });
    check('A', [&] { w.AddOrder(1, Ts, 1, 'B', 1, "MSFT", 1); });
    check('F', [&] { w.AddOrder(1, Ts, 1, 'B', 1, "MSFT", 1, "ABCD"); });
    check('E', [&] { w.OrderExecuted(1, Ts, 1, 1, 1); });
    check('C', [&] { w.OrderExecutedWithPrice(1, Ts, 1, 1, 1, 'Y', 1); });
    check('X', [&] { w.OrderCancel(1, Ts, 1, 1); });
    check('D', [&] { w.OrderDelete(1, Ts, 1); });
    check('U', [&] { w.OrderReplace(1, Ts, 1, 2, 1, 1); });
    check('P', [&] { w.Trade(1, Ts, 1, 'B', 1, "MSFT", 1, 1); });
    check('Q', [&] { w.CrossTrade(1, Ts, 1, "MSFT", 1, 1, 'O'); });
}

TEST(ItchParser, LeavesPartialMessageForNextBuffer)
{
    itch::Writer w;
    w.OrderDelete(1, Ts, 1);
    w.OrderDelete(1, Ts, 2);

    Recorder r;
    itch::ParseStats stats;
    //cut the second message short by 5 bytes
    const auto consumed = itch::Parse(w.Bytes().data(), w.Bytes().size() - 5, r, stats);
    EXPECT_EQ(consumed, 2u + 19u);
    ASSERT_EQ(r.deletes_.size(), 1u);
    EXPECT_EQ(r.deletes_[0].orderReference_, 1u);
}

TEST(ItchParser, SkipsMalformedAndUnknownMessagesButKeepsGoing)
{
    std::vector<std::uint8_t> bytes;
    //length says 5 but a 'D' is 19 bytes long
    bytes.insert(bytes.end(), { 0x00, 0x05, 'D', 0, 0, 0, 0 });
    //a type that doesn't exist in 5.0
    bytes.insert(bytes.end(), { 0x00, 0x03, 'z', 1, 2 });
    //zero-length frame
    bytes.insert(bytes.end(), { 0x00, 0x00 });

    itch::Writer w;
    w.OrderDelete(1, Ts, 99);
    bytes.insert(bytes.end(), w.Bytes().begin(), w.Bytes().end());

    Recorder r;
    itch::ParseStats stats;
    EXPECT_EQ(itch::Parse(bytes.data(), bytes.size(), r, stats), bytes.size());
    EXPECT_EQ(stats.malformed_, 2u);
    EXPECT_EQ(stats.unknown_, 1u);
    ASSERT_EQ(r.deletes_.size(), 1u);
    EXPECT_EQ(r.deletes_[0].orderReference_, 99u);
}

TEST(ItchParser, StreamsAFileAcrossChunkBoundaries)
{
    itch::Writer w;
    for (std::uint64_t i = 0; i < 5'000; ++i)
        w.AddOrder(1, Ts + i, i, i % 2 ? 'B' : 'S', static_cast<std::uint32_t>(i), "TEST", static_cast<std::uint32_t>(i * 7), i % 3 ? "" : "MPID");

    std::FILE* f = std::tmpfile();
    ASSERT_NE(f, nullptr);
    std::fwrite(w.Bytes().data(), 1, w.Bytes().size(), f);
    std::rewind(f);

    //a 61-byte buffer: prime-sized, so messages straddle every boundary
    Recorder r;
    std::uint64_t bytes = 0;
    const auto stats = itch::ParseFile(f, r, &bytes, 61);
    std::fclose(f);

    EXPECT_EQ(bytes, w.Bytes().size());
    EXPECT_EQ(stats.malformed_, 0u);
    ASSERT_EQ(r.adds_.size(), 5'000u);
    for (std::uint64_t i = 0; i < 5'000; ++i)
    {
        ASSERT_EQ(r.adds_[i].orderReference_, i);
        ASSERT_EQ(r.adds_[i].price_, i * 7);
        ASSERT_EQ(r.adds_[i].hasAttribution_, i % 3 == 0);
    }
}

TEST(ItchParser, FlagsTruncatedFinalMessage)
{
    itch::Writer w;
    w.OrderDelete(1, Ts, 1);
    std::FILE* f = std::tmpfile();
    ASSERT_NE(f, nullptr);
    std::fwrite(w.Bytes().data(), 1, w.Bytes().size() - 1, f);
    std::rewind(f);

    Recorder r;
    const auto stats = itch::ParseFile(f, r);
    std::fclose(f);
    EXPECT_EQ(stats.malformed_, 1u);
    EXPECT_TRUE(r.deletes_.empty());
}

//a tiny hand-written session, replayed through the book
template <typename Engine>
class ItchReplay : public ::testing::Test { };

using Engines = ::testing::Types<LadderEngine, MixedEngine>;
TYPED_TEST_SUITE(ItchReplay, Engines);

namespace
{

template <Price Tick, std::uint32_t Band>
TopOfBook ReplayInto(EngineT<Tick, Band>& e, const std::vector<itch::Event>& events)
{
    for (const auto& ev : events)
        itch::Apply(e.book_, ev);
    return e.Top();
}

}

TYPED_TEST(ItchReplay, AppliesEveryOrderMessage)
{
    itch::Writer w;
    w.StockDirectory(7, Ts, "AAPL");
    w.StockDirectory(8, Ts, "MSFT");
    w.AddOrder(7, Ts, 1, 'B', 100, "AAPL", 1000000);//$100.00
    w.AddOrder(7, Ts, 2, 'B', 200, "AAPL", 1000000);
    w.AddOrder(7, Ts, 3, 'B', 300, "AAPL", 999900); //$99.99
    w.AddOrder(7, Ts, 4, 'S', 400, "AAPL", 1000100, "GSCO");
    w.AddOrder(8, Ts, 5, 'S', 999, "MSFT", 1);      //other symbol: filtered out
    w.OrderExecuted(7, Ts, 1, 40, 1);                //1: 100 -> 60
    w.OrderCancel(7, Ts, 2, 50);                     //2: 200 -> 150
    w.OrderExecutedWithPrice(7, Ts, 4, 100, 2, 'Y', 1000100);//4: 400 -> 300
    w.OrderReplace(7, Ts, 3, 30, 500, 999800);       //3 -> 30 @ $99.98 x 500
    w.OrderExecuted(7, Ts, 1, 60, 3);                //1 fully executed, removed
    w.Trade(7, Ts, 0, 'B', 10, "AAPL", 1000050, 4);  //hidden print: book unaffected
    w.OrderDelete(8, Ts, 5);

    itch::EventCollector collector{ "AAPL" };
    itch::ParseStats stats;
    itch::Parse(w.Bytes().data(), w.Bytes().size(), collector, stats);
    ASSERT_TRUE(collector.FoundSymbol());

    TypeParam engine;
    const auto top = ReplayInto(engine, collector.Events());

    EXPECT_EQ(top.bidPrice_, 1000000);
    EXPECT_EQ(top.bidQuantity_, 150u);
    EXPECT_EQ(top.askPrice_, 1000100);
    EXPECT_EQ(top.askQuantity_, 300u);
    EXPECT_EQ(engine.Size(), 3u);//2, 4, 30

    const auto levels = engine.Levels();
    ASSERT_EQ(levels.GetBids().size(), 2u);
    EXPECT_EQ(levels.GetBids()[1].price_, 999800);
    EXPECT_EQ(levels.GetBids()[1].quantity_, 500u);
}

TEST(ItchReplay, UnknownSymbolIsReported)
{
    itch::Writer w;
    w.StockDirectory(7, Ts, "AAPL");
    itch::EventCollector collector{ "NOPE" };
    itch::ParseStats stats;
    itch::Parse(w.Bytes().data(), w.Bytes().size(), collector, stats);
    EXPECT_FALSE(collector.FoundSymbol());
}
