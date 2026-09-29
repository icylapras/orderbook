//replay a NASDAQ TotalView-ITCH 5.0 day through both engines.
//
//  replay <file|-> [--symbol AAPL] [--all] [--pin CPU] [--warmup N] [--runs N]
//                  [--expect-open PRICE] [--expect-close PRICE]
//                  [--engine fast|baseline|both] [--no-validate]
//                  [--tick PRICE_UNITS] [--band TICKS]   (fast book layout; --band 0 = sorted vector only)
//                  [--strict]   (--all: validate after every message instead of per chunk)
//
//single-symbol mode (default AAPL):
//  1. parse the whole file, keeping that symbol's events in memory
//  2. validate: replay through both engines in lockstep, checking the book
//     against NASDAQ's own executions/prints and the two engines against
//     each other after every message
//  3. latency: warm-up runs, then per-message rdtsc timing into a histogram
//  4. throughput: untimed-per-message runs, messages/sec
//
//--all mode: every symbol in the file, streamed in chunks (the full day does
//not fit in memory), both engines, one book per symbol

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "FastOrderbook.h"
#include "Orderbook.h"
#include "itch/ItchFile.h"
#include "itch/Replay.h"
#include "perf/Latency.h"

//count heap allocations so "no allocation on the hot path" is measured, not assumed
static std::atomic<std::uint64_t> g_allocations{ 0 };

//gcc can't see that these replace the global operator new, and warns that
//free() is paired with new
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif

//ThreadSanitizer supplies its own operator new, so counting is off in TSan builds
#if defined(__SANITIZE_THREAD__)
#define ORDERBOOK_COUNT_ALLOCATIONS 0
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define ORDERBOOK_COUNT_ALLOCATIONS 0
#endif
#endif
#ifndef ORDERBOOK_COUNT_ALLOCATIONS
#define ORDERBOOK_COUNT_ALLOCATIONS 1
#endif

#if ORDERBOOK_COUNT_ALLOCATIONS
void* operator new(std::size_t size)
{
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(size ? size : 1))
        return p;
    throw std::bad_alloc{ };
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
#endif

namespace
{

using Clock = std::chrono::steady_clock;

struct Options
{
    std::string path_;
    std::string symbol_{ "AAPL" };
    bool all_{ false };
    int pin_{ -1 };
    int warmup_{ 2 };
    int runs_{ 5 };
    double expectOpen_{ 0 };
    double expectClose_{ 0 };
    std::string engine_{ "both" };//fast | baseline | both
    bool strict_{ false };        //--all: check every message (slower; timings not comparable)
    Price tick_{ 100 };           //ITCH prices are 1/10000 $: 100 = one cent
    std::uint32_t band_{ 4096 };  //ticks in the fast book's price ladder band
    bool validate_{ true };
};

double Seconds(Clock::duration d) { return std::chrono::duration<double>(d).count(); }
double Dollars(std::uint32_t price) { return price / 10000.0; }

std::string TimeOfDay(std::uint64_t ns)
{
    const auto ms = ns / 1'000'000;
    char buf[32];
    std::snprintf(buf, sizeof buf, "%02" PRIu64 ":%02" PRIu64 ":%02" PRIu64 ".%03" PRIu64,
        ms / 3'600'000, ms / 60'000 % 60, ms / 1000 % 60, ms % 1000);
    return buf;
}

constexpr std::uint64_t Hms(int h, int m, int s) { return ((h * 60ull + m) * 60ull + s) * 1'000'000'000ull; }

//NASDAQ publishes "trading resumed" just before the reopening cross's
//executions, so a halted symbol's crossed book legitimately stays crossed for
//a moment after resuming (measured on 2019-12-30: at most 1.2ms after the
//resume, lasting at most 0.2ms). Crossing is legal while halted ('H'),
//paused ('P') or quotation-only ('Q'), or within this grace after resuming.
constexpr std::uint64_t ResumeGraceNs = 5'000'000;

bool CrossingAllowed(char state, std::uint64_t stateSince, std::uint64_t now)
{
    if (state == 'H' || state == 'P' || state == 'Q')
        return true;
    return state == 'T' && stateSince != 0 && now - stateSince <= ResumeGraceNs;
}

std::string FormatTop(const TopOfBook& top)
{
    char buf[128];
    std::snprintf(buf, sizeof buf, "%8u @ %-10.4f | %-10.4f @ %-8u",
        top.bidQuantity_, top.HasBid() ? top.bidPrice_ / 10000.0 : 0.0,
        top.HasAsk() ? top.askPrice_ / 10000.0 : 0.0, top.askQuantity_);
    return buf;
}

std::FILE* Open(const std::string& path)
{
    if (path == "-")
        return stdin;
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f)
    {
        std::perror(path.c_str());
        std::exit(1);
    }
    return f;
}

//---------------------------------------------------------------- validation

struct ShadowOrder
{
    std::uint32_t price_;
    std::uint32_t shares_;
    char side_;
};

struct ValidationResult
{
    bool passed_{ true };
};


//replays both engines in lockstep and checks the reconstructed book against
//what NASDAQ itself reported
ValidationResult Validate(const std::vector<itch::Event>& events, const Options& options)
{
    std::printf("\n== Validation (both engines in lockstep, checked after every message) ==\n");

    FastOrderbook fast{ 1 << 16, options.tick_, options.band_ };
    Orderbook baseline{ false };

    //shadow copy of every live order, only to know an order's price/side
    //when NASDAQ executes it (the engines don't need or expose this)
    std::unordered_map<std::uint64_t, ShadowOrder> shadow;

    std::uint64_t topMismatches = 0, crossedSamples = 0, crossedWhileTrading = 0, unknownReferences = 0;
    char tradingState = '?';
    std::uint64_t tradingStateSince = 0;
    std::uint64_t executions = 0, executionsAtTouch = 0, executionsAwayFromOrderPrice = 0;
    std::uint64_t hiddenPrints = 0, hiddenPrintsInsideSpread = 0;
    std::uint64_t firstMismatchIndex = UINT64_MAX;
    std::uint64_t depthComparisons = 0, depthMismatches = 0;
    std::size_t peakLive = 0;

    auto sameLevels = [](const LevelInfos& a, const LevelInfos& b)
    {
        return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(),
            [](const LevelInfo& x, const LevelInfo& y) { return x.price_ == y.price_ && x.quantity_ == y.quantity_; });
    };
    std::size_t maxBidLevels = 0, maxAskLevels = 0;
    auto sameDepth = [&](const Orderbook& a, const FastOrderbook& b)
    {
        const auto x = a.GetLevelInfos();
        const auto y = b.GetLevelInfos();
        maxBidLevels = std::max(maxBidLevels, y.GetBids().size());
        maxAskLevels = std::max(maxAskLevels, y.GetAsks().size());
        return sameLevels(x.GetBids(), y.GetBids()) && sameLevels(x.GetAsks(), y.GetAsks());
    };

    std::uint32_t regularHigh = 0, regularLow = UINT32_MAX;
    std::uint32_t openCross = 0, closeCross = 0;
    TopOfBook topBeforeClose;

    const std::uint64_t snapshotTimes[] = { Hms(9, 30, 0), Hms(10, 0, 0), Hms(12, 0, 0), Hms(15, 0, 0), Hms(15, 59, 59), Hms(16, 0, 0) };
    std::size_t nextSnapshot = 0;
    std::vector<std::string> snapshots;

    auto trackPrint = [&](std::uint64_t ts, std::uint32_t price)
    {
        if (ts >= Hms(9, 30, 0) && ts <= Hms(16, 0, 0))
        {
            regularHigh = std::max(regularHigh, price);
            regularLow = std::min(regularLow, price);
        }
    };

    for (std::size_t i = 0; i < events.size(); ++i)
    {
        const auto& e = events[i];

        while (nextSnapshot < std::size(snapshotTimes) && e.timestamp_ >= snapshotTimes[nextSnapshot])
        {
            snapshots.push_back(TimeOfDay(snapshotTimes[nextSnapshot]) + "  " + FormatTop(fast.GetTopOfBook()));
            ++nextSnapshot;
        }

        const TopOfBook top = fast.GetTopOfBook();

        switch (e.type_)
        {
        case 'H':
            if (e.side_ != tradingState)
                tradingStateSince = e.timestamp_;
            tradingState = e.side_;
            continue;
        case 'Q':
            if (e.side_ == 'O')
                openCross = e.price_;
            if (e.side_ == 'C')
            {
                closeCross = e.price_;
                topBeforeClose = top;
            }
            trackPrint(e.timestamp_, e.price_);
            continue;
        case 'P':
        {
            //non-displayed (hidden/midpoint) orders trade at or inside the displayed spread
            ++hiddenPrints;
            if (top.HasBid() && top.HasAsk() &&
                static_cast<Price>(e.price_) >= top.bidPrice_ && static_cast<Price>(e.price_) <= top.askPrice_)
                ++hiddenPrintsInsideSpread;
            trackPrint(e.timestamp_, e.price_);
            continue;
        }
        case 'E':
        case 'C':
        {
            const auto it = shadow.find(e.reference_);
            if (it == shadow.end())
            {
                ++unknownReferences;
                break;
            }

            //an incoming order always executes against the best price first,
            //so the order NASDAQ reports as executed must sit at our touch
            const Price best = it->second.side_ == 'B' ? top.bidPrice_ : top.askPrice_;
            ++executions;
            executionsAtTouch += static_cast<Price>(it->second.price_) == best;
            //'C' at a price other than the order's own: a cross fill or price improvement
            executionsAwayFromOrderPrice += e.type_ == 'C' && e.price_ != it->second.price_;

            if (e.type_ == 'E' || e.side_ == 'Y')//'C' prints only if printable
                trackPrint(e.timestamp_, e.type_ == 'E' ? it->second.price_ : e.price_);
            break;
        }
        case 'X':
        case 'D':
        case 'U':
            if (!shadow.contains(e.reference_))
                ++unknownReferences;
            break;
        default:
            break;
        }

        //shadow bookkeeping
        switch (e.type_)
        {
        case 'A':
        case 'F':
            shadow[e.reference_] = ShadowOrder{ e.price_, e.shares_, e.side_ };
            break;
        case 'E':
        case 'C':
        case 'X':
            if (auto it = shadow.find(e.reference_); it != shadow.end())
            {
                if (e.shares_ >= it->second.shares_)
                    shadow.erase(it);
                else
                    it->second.shares_ -= e.shares_;
            }
            break;
        case 'D':
            shadow.erase(e.reference_);
            break;
        case 'U':
            if (auto it = shadow.find(e.reference_); it != shadow.end())
            {
                const char side = it->second.side_;
                shadow.erase(it);
                shadow[e.newReference_] = ShadowOrder{ e.price_, e.shares_, side };
            }
            break;
        default:
            break;
        }

        itch::Apply(fast, e);
        itch::Apply(baseline, e);

        const auto fastTop = fast.GetTopOfBook();
        if (fastTop != baseline.GetTopOfBook())
        {
            ++topMismatches;
            firstMismatchIndex = std::min<std::uint64_t>(firstMismatchIndex, i);
        }
        if (fastTop.IsCrossed())
        {
            ++crossedSamples;
            crossedWhileTrading += !CrossingAllowed(tradingState, tradingStateSince, e.timestamp_);
        }
        peakLive = std::max(peakLive, shadow.size());

        //full depth (every level, both sides), not just the top, every 10k messages
        if (i % 10'000 == 0)
        {
            ++depthComparisons;
            depthMismatches += !sameDepth(baseline, fast);
        }
    }
    ++depthComparisons;
    depthMismatches += !sameDepth(baseline, fast);

    std::printf("Top of book through the day (FastOrderbook):\n     time          bid size @ bid        | ask        @ ask size\n");
    for (const auto& s : snapshots)
        std::printf("  %s\n", s.c_str());

    auto pct = [](std::uint64_t a, std::uint64_t b) { return b ? 100.0 * static_cast<double>(a) / static_cast<double>(b) : 100.0; };

    ValidationResult result;
    auto check = [&result](bool ok, const char* what, const std::string& detail)
    {
        std::printf("  [%s] %-58s %s\n", ok ? "PASS" : "FAIL", what, detail.c_str());
        result.passed_ = result.passed_ && ok;
    };
    auto str = [](const char* fmt, auto... args)
    {
        char buf[256];
        std::snprintf(buf, sizeof buf, fmt, args...);
        return std::string{ buf };
    };

    std::printf("\nChecks:\n");
    check(unknownReferences == 0, "every E/C/X/D/U references a live order", str("%" PRIu64 " unknown", unknownReferences));
    check(crossedWhileTrading == 0, "book crossed only while halted/paused (or re-opening)",
        str("%" PRIu64 " crossed messages, %" PRIu64 " not explained", crossedSamples, crossedWhileTrading));
    check(topMismatches == 0, "baseline == fast top-of-book after every message",
        topMismatches ? str("%" PRIu64 " mismatches, first at #%" PRIu64, topMismatches, firstMismatchIndex) : str("%zu messages", events.size()));
    check(depthMismatches == 0, "baseline == fast full depth (sampled every 10k messages)",
        str("%" PRIu64 "/%" PRIu64 " snapshots equal", depthComparisons - depthMismatches, depthComparisons));
    check(fast.Size() == baseline.Size() && fast.Size() == shadow.size(), "resting order counts agree (fast/baseline/shadow)",
        str("%zu / %zu / %zu", fast.Size(), baseline.Size(), shadow.size()));
    check(executionsAtTouch == executions, "every execution hit the order at our best bid/ask",
        str("%" PRIu64 "/%" PRIu64 " (%.4f%%)", executionsAtTouch, executions, pct(executionsAtTouch, executions)));
    std::printf("  [info] executions at a price other than the order's (cross fills): %" PRIu64 "\n", executionsAwayFromOrderPrice);
    std::printf("  [info] peak resting orders: %zu; most price levels seen: %zu bid / %zu ask\n", peakLive, maxBidLevels, maxAskLevels);
    std::printf("  [info] hidden-order prints inside our spread:     %" PRIu64 "/%" PRIu64 " (%.2f%%)\n",
        hiddenPrintsInsideSpread, hiddenPrints, pct(hiddenPrintsInsideSpread, hiddenPrints));

    if (closeCross)
    {
        const bool bracketed = topBeforeClose.HasBid() && topBeforeClose.HasAsk() &&
            static_cast<Price>(closeCross) >= topBeforeClose.bidPrice_ - 10000 &&
            static_cast<Price>(closeCross) <= topBeforeClose.askPrice_ + 10000;
        check(bracketed, "closing cross price within $0.01 of our book at the cross",
            str("cross %.4f vs book %.4f / %.4f", Dollars(closeCross), topBeforeClose.bidPrice_ / 10000.0, topBeforeClose.askPrice_ / 10000.0));
    }

    std::printf("\nNASDAQ prints, regular session: open cross %.4f, high %.4f, low %.4f, close cross %.4f\n",
        Dollars(openCross), Dollars(regularHigh == 0 ? 0 : regularHigh), Dollars(regularLow == UINT32_MAX ? 0 : regularLow), Dollars(closeCross));

    //compare against an independent source (e.g. published daily OHLC)
    auto sameCents = [](double a, double b) { return std::abs(a - b) < 0.005; };
    if (options.expectOpen_ > 0)
        check(sameCents(Dollars(openCross), options.expectOpen_), "opening cross == expected official open",
            str("%.4f vs %.4f", Dollars(openCross), options.expectOpen_));
    if (options.expectClose_ > 0)
        check(sameCents(Dollars(closeCross), options.expectClose_), "closing cross == expected official close",
            str("%.4f vs %.4f", Dollars(closeCross), options.expectClose_));

    std::printf("\nValidation %s\n", result.passed_ ? "PASSED" : "FAILED");
    return result;
}

//---------------------------------------------------------------- latency

struct LatencyResult
{
    perf::Histogram all_;
    std::unordered_map<char, perf::Histogram> byType_;
    std::uint64_t allocations_{ };
    double throughput_{ };//messages per second, median of runs
};

template <typename Book, typename MakeBook, typename ResetBook>
LatencyResult Measure(const std::vector<itch::Event>& events, const Options& options, MakeBook makeBook, ResetBook resetBook)
{
    LatencyResult result;
    std::unique_ptr<Book> book = makeBook();

    //warm-up: trains branch predictors, faults in pages, grows every pool
    //and vector to its high-water mark
    for (int w = 0; w < options.warmup_; ++w)
    {
        for (const auto& e : events)
            itch::Apply(*book, e);
        resetBook(book);
    }

    //per-message latency; timing brackets only the engine call
    std::vector<perf::Histogram> byType(128);
    const auto allocationsBefore = g_allocations.load(std::memory_order_relaxed);
    for (const auto& e : events)
    {
        const auto start = perf::StartTicks();
        itch::Apply(*book, e);
        const auto stop = perf::StopTicks();
        byType[static_cast<unsigned char>(e.type_)].Record(stop - start);
    }
    result.allocations_ = g_allocations.load(std::memory_order_relaxed) - allocationsBefore;

    for (int t = 0; t < 128; ++t)
    {
        if (byType[t].Count() == 0)
            continue;
        result.all_.Merge(byType[t]);
        result.byType_[static_cast<char>(t)] = byType[t];
    }

    //throughput: the same replay with no per-message timing
    std::vector<double> rates;
    for (int r = 0; r < options.runs_; ++r)
    {
        resetBook(book);
        const auto start = Clock::now();
        for (const auto& e : events)
            itch::Apply(*book, e);
        const auto elapsed = Seconds(Clock::now() - start);
        rates.push_back(static_cast<double>(events.size()) / elapsed);
    }
    std::sort(rates.begin(), rates.end());
    result.throughput_ = rates[rates.size() / 2];

    return result;
}

void PrintLatency(const char* name, const LatencyResult& r, double ticksPerNs, std::size_t events)
{
    std::printf("\n%s\n", name);
    std::printf("  %-22s %10s %8s %8s %8s %8s %9s %10s\n", "message", "count", "mean", "p50", "p99", "p99.9", "max", "");
    auto row = [&](const char* label, const perf::Histogram& h)
    {
        const auto s = perf::Summarize(h, ticksPerNs);
        std::printf("  %-22s %10" PRIu64 " %6.0fns %6.0fns %6.0fns %6.0fns %7.1fus\n",
            label, s.count_, s.mean_, s.p50_, s.p99_, s.p999_, s.max_ / 1000.0);
    };
    row("all book messages", r.all_);
    const std::pair<char, const char*> labels[] = {
        { 'A', "A add" }, { 'F', "F add (MPID)" }, { 'E', "E executed" }, { 'C', "C executed w/ price" },
        { 'X', "X cancel (partial)" }, { 'D', "D delete" }, { 'U', "U replace" } };
    for (const auto& [type, label] : labels)
        if (auto it = r.byType_.find(type); it != r.byType_.end())
            row(label, it->second);
    std::printf("  throughput: %.2f M msgs/sec (median of runs, no per-message timing)\n", r.throughput_ / 1e6);
    std::printf("  heap allocations during measured replay: %" PRIu64 " (%.3f per message)\n",
        r.allocations_, static_cast<double>(r.allocations_) / static_cast<double>(events));
}

//---------------------------------------------------------------- modes

int RunSingleSymbol(const Options& options)
{
    std::printf("Parsing %s for %s ...\n", options.path_.c_str(), options.symbol_.c_str());

    itch::EventCollector collector{ options.symbol_ };
    std::FILE* file = Open(options.path_);
    std::uint64_t bytes = 0;
    const auto parseStart = Clock::now();
    const auto stats = itch::ParseFile(file, collector, &bytes);
    const auto parseSeconds = Seconds(Clock::now() - parseStart);
    if (file != stdin)
        std::fclose(file);

    std::printf("  %.2f GB, %" PRIu64 " messages in %.2fs  (%.2f GB/s, %.1f M msgs/sec, incl. I/O)\n",
        static_cast<double>(bytes) / 1e9, stats.messages_, parseSeconds,
        static_cast<double>(bytes) / 1e9 / parseSeconds, static_cast<double>(stats.messages_) / 1e6 / parseSeconds);
    std::printf("  malformed: %" PRIu64 ", unknown type: %" PRIu64 "\n", stats.malformed_, stats.unknown_);
    std::printf("  whole-feed counts: A %" PRIu64 "  F %" PRIu64 "  E %" PRIu64 "  C %" PRIu64 "  X %" PRIu64 "  D %" PRIu64 "  U %" PRIu64 "  P %" PRIu64 "\n",
        stats.byType_['A'], stats.byType_['F'], stats.byType_['E'], stats.byType_['C'],
        stats.byType_['X'], stats.byType_['D'], stats.byType_['U'], stats.byType_['P']);

    if (!collector.FoundSymbol())
    {
        std::fprintf(stderr, "symbol %s not in the stock directory\n", options.symbol_.c_str());
        return 1;
    }

    auto& events = collector.Events();
    std::vector<itch::Event> bookEvents;
    bookEvents.reserve(events.size());
    for (const auto& e : events)
        if (itch::IsBookEvent(e.type_))
            bookEvents.push_back(e);
    std::printf("  %s: %zu book messages\n", options.symbol_.c_str(), bookEvents.size());

    ValidationResult validation;
    if (options.validate_)
        validation = Validate(events, options);

    //size the fast book's pool and id map for the peak number of *live*
    //orders: sizing for every add of the day would spread lookups over a
    //table far bigger than the cache
    std::size_t capacity = 1024;
    {
        std::unordered_map<std::uint64_t, bool> live;
        for (const auto& e : bookEvents)
        {
            if (e.type_ == 'A' || e.type_ == 'F')
                live[e.reference_] = true;
            else if (e.type_ == 'D')
                live.erase(e.reference_);
            else if (e.type_ == 'U')
            {
                live.erase(e.reference_);
                live[e.newReference_] = true;
            }
            capacity = std::max(capacity, live.size());
        }
    }

    if (options.pin_ >= 0)
        std::printf("\nPinned to CPU %d: %s\n", options.pin_, perf::PinCurrentThread(options.pin_) ? "ok" : "FAILED");

    const double ticksPerNs = perf::CalibrateTicksPerNs();
    //cost of the timer itself, so the table can be read net of it
    perf::Histogram overhead;
    for (int i = 0; i < 1'000'000; ++i)
    {
        const auto a = perf::StartTicks();
        const auto b = perf::StopTicks();
        overhead.Record(b - a);
    }
    std::printf("\n== Latency: %zu %s book messages, %d warm-up + %d throughput runs ==\n",
        bookEvents.size(), options.symbol_.c_str(), options.warmup_, options.runs_);
    std::printf("  TSC %.3f GHz; timer overhead p50 %.0f ns (included in the numbers below)\n",
        ticksPerNs, perf::Summarize(overhead, ticksPerNs).p50_);

    std::unique_ptr<LatencyResult> fast, base;
    if (options.engine_ != "baseline")
    {
        fast = std::make_unique<LatencyResult>(Measure<FastOrderbook>(bookEvents, options,
            [&] { return std::make_unique<FastOrderbook>(capacity, options.tick_, options.band_); },
            [](std::unique_ptr<FastOrderbook>& b) { b->Clear(); }));
        PrintLatency("FastOrderbook (tick-ladder levels + pool + intrusive list + open-addressing map)", *fast, ticksPerNs, bookEvents.size());
    }

    if (options.engine_ != "fast")
    {
        base = std::make_unique<LatencyResult>(Measure<Orderbook>(bookEvents, options,
            [] { return std::make_unique<Orderbook>(false); },
            [](std::unique_ptr<Orderbook>& b) { b = std::make_unique<Orderbook>(false); }));
        PrintLatency("Orderbook baseline (std::map levels + std::list + shared_ptr + mutex)", *base, ticksPerNs, bookEvents.size());
    }

    if (fast && base)
    {
        const auto f = perf::Summarize(fast->all_, ticksPerNs);
        const auto b = perf::Summarize(base->all_, ticksPerNs);
        std::printf("\n== Summary (%s, all book messages) ==\n", options.symbol_.c_str());
        std::printf("  %-10s %8s %8s %8s %9s %14s\n", "engine", "p50", "p99", "p99.9", "max", "throughput");
        std::printf("  %-10s %6.0fns %6.0fns %6.0fns %7.1fus %8.2f M/s\n", "baseline", b.p50_, b.p99_, b.p999_, b.max_ / 1000, base->throughput_ / 1e6);
        std::printf("  %-10s %6.0fns %6.0fns %6.0fns %7.1fus %8.2f M/s\n", "fast", f.p50_, f.p99_, f.p999_, f.max_ / 1000, fast->throughput_ / 1e6);
        std::printf("  speed-up   %7.1fx %7.1fx %7.1fx %8.1fx %9.1fx\n",
            b.p50_ / f.p50_, b.p99_ / f.p99_, b.p999_ / f.p999_, b.max_ / f.max_, fast->throughput_ / base->throughput_);
    }

    return validation.passed_ ? 0 : 2;
}

//streams every symbol; events are decoded into a chunk, then the chunk is
//applied to each engine with only the apply loop timed. After every chunk
//(untimed) both engines are compared symbol by symbol, and any crossed book is
//checked against the trading state NASDAQ reported for that symbol.
class AllSymbols
{
public:
    AllSymbols(bool baseline, std::uint32_t band, bool strict)
        : baseline_{ baseline }, strict_{ strict }, band_{ band }, fast_(65536), base_(65536), state_(65536, '?'), stateSince_(65536, 0), names_(65536)
    {
        chunk_.reserve(ChunkSize);
    }

    void OnStockDirectory(const itch::StockDirectory& e) { names_[e.stockLocate_] = std::string{ e.stock_.View() }; }
    void OnStockTradingAction(const itch::StockTradingAction& e) { Push({ e.timestamp_, 0, 0, 0, 0, e.stockLocate_, 'H', e.tradingState_ }); }
    void OnAddOrder(const itch::AddOrder& e) { Push({ e.timestamp_, e.orderReference_, 0, e.price_, e.shares_, e.stockLocate_, 'A', e.side_ }); }
    void OnOrderExecuted(const itch::OrderExecuted& e) { Push({ e.timestamp_, e.orderReference_, 0, 0, e.executedShares_, e.stockLocate_, 'E', 0 }); }
    void OnOrderExecutedWithPrice(const itch::OrderExecutedWithPrice& e) { Push({ e.timestamp_, e.orderReference_, 0, e.executionPrice_, e.executedShares_, e.stockLocate_, 'C', 0 }); }
    void OnOrderCancel(const itch::OrderCancel& e) { Push({ e.timestamp_, e.orderReference_, 0, 0, e.cancelledShares_, e.stockLocate_, 'X', 0 }); }
    void OnOrderDelete(const itch::OrderDelete& e) { Push({ e.timestamp_, e.orderReference_, 0, 0, 0, e.stockLocate_, 'D', 0 }); }
    void OnOrderReplace(const itch::OrderReplace& e) { Push({ e.timestamp_, e.originalOrderReference_, e.newOrderReference_, e.price_, e.shares_, e.stockLocate_, 'U', 0 }); }

    void Flush()
    {
        if (chunk_.empty())
            return;

        //books are created up front (untimed): a symbol's first price picks its tick grid
        for (const auto& e : chunk_)
        {
            if (e.type_ == 'H' || fast_[e.locate_])
                continue;
            //Reg NMS: sub-penny prices are only allowed below $1.00, so a
            //penny grid suits anything that first trades above $1
            fast_[e.locate_] = std::make_unique<FastOrderbook>(256, e.price_ >= 10'000 ? 100 : 1, band_);
            if (baseline_)
                base_[e.locate_] = std::make_unique<Orderbook>(false);
        }

        auto start = Clock::now();
        if (strict_)
        {
            //per-message check: a crossed book is only legal while NASDAQ has
            //the symbol halted ('H'), paused ('P') or quotation-only ('Q')
            for (const auto& e : chunk_)
            {
                if (e.type_ == 'H')
                {
                    SetState(e);
                    continue;
                }
                auto& book = *fast_[e.locate_];
                itch::Apply(book, e);
                if (book.GetTopOfBook().IsCrossed())
                {
                    ++crossedMessages_;
                    if (!CrossingAllowed(state_[e.locate_], stateSince_[e.locate_], e.timestamp_))
                    {
                        ++crossedWhileTrading_;
                        crossedSymbols_.insert(names_[e.locate_] + "(UNEXPLAINED)");
                    }
                    else
                        crossedSymbols_.insert(names_[e.locate_]);
                }
            }
        }
        else
        {
            for (const auto& e : chunk_)
                if (e.type_ != 'H')
                    itch::Apply(*fast_[e.locate_], e);
        }
        fastTime_ += Clock::now() - start;

        if (baseline_)
        {
            start = Clock::now();
            for (const auto& e : chunk_)
                if (e.type_ != 'H')
                    itch::Apply(*base_[e.locate_], e);
            baseTime_ += Clock::now() - start;
        }

        std::uint64_t now = 0;
        for (const auto& e : chunk_)
        {
            now = e.timestamp_;
            if (e.type_ == 'H')
            {
                if (!strict_)//strict mode already tracked it in order
                    SetState(e);
            }
            else
                ++events_;
        }
        chunk_.clear();
        Checkpoint(now);
    }

    void Report(double parseSeconds) const
    {
        std::size_t books = 0;
        for (const auto& b : fast_)
            books += b != nullptr;

        auto rate = [this](Clock::duration t) { return static_cast<double>(events_) / 1e6 / Seconds(t); };
        std::printf("\n== Full-day replay, all symbols ==\n");
        std::printf("  %" PRIu64 " book messages across %zu symbols (end-to-end incl. parse/I-O: %.1fs)\n", events_, books, parseSeconds);
        std::printf("  fast:     %6.2fs engine time  -> %6.2f M msgs/sec%s\n", Seconds(fastTime_), rate(fastTime_),
            strict_ ? "  (includes per-message checks)" : "");
        if (baseline_)
        {
            std::printf("  baseline: %6.2fs engine time  -> %6.2f M msgs/sec\n", Seconds(baseTime_), rate(baseTime_));
            if (!strict_)
                std::printf("  speed-up: %.1fx\n", Seconds(baseTime_) / Seconds(fastTime_));
            std::printf("  [%s] engines agree on every symbol's top of book and order count at all %" PRIu64 " checkpoints (%" PRIu64 " mismatches)\n",
                mismatches_ == 0 ? "PASS" : "FAIL", checkpoints_, mismatches_);
        }
        std::printf("  [%s] every book crossed at a checkpoint was halted/paused/re-opening (%" PRIu64 " crossed, %" PRIu64 " unexplained)\n",
            checkpointCrossedWhileTrading_ == 0 ? "PASS" : "FAIL", checkpointCrossed_, checkpointCrossedWhileTrading_);
        if (strict_)
        {
            std::printf("  [%s] per message: book crossed only while halted/paused/re-opening (%" PRIu64 " crossed messages, %" PRIu64 " unexplained)\n",
                crossedWhileTrading_ == 0 ? "PASS" : "FAIL", crossedMessages_, crossedWhileTrading_);
            std::printf("  symbols whose book was ever crossed:");
            for (const auto& s : crossedSymbols_)
                std::printf(" %s", s.c_str());
            std::printf("\n");
        }
    }

    bool Passed() const
    {
        return mismatches_ == 0 && checkpointCrossedWhileTrading_ == 0 && crossedWhileTrading_ == 0;
    }

private:
    static constexpr std::size_t ChunkSize = 1 << 22;

    void Push(const itch::Event& e)
    {
        chunk_.push_back(e);
        if (chunk_.size() == ChunkSize)
            Flush();
    }

    void SetState(const itch::Event& e)
    {
        if (state_[e.locate_] != e.side_)
            stateSince_[e.locate_] = e.timestamp_;
        state_[e.locate_] = e.side_;
    }

    void Checkpoint(std::uint64_t now)
    {
        ++checkpoints_;
        for (std::size_t i = 0; i < fast_.size(); ++i)
        {
            if (!fast_[i])
                continue;
            const auto top = fast_[i]->GetTopOfBook();
            if (baseline_ && (top != base_[i]->GetTopOfBook() || fast_[i]->Size() != base_[i]->Size()))
                ++mismatches_;
            if (top.IsCrossed())
            {
                ++checkpointCrossed_;
                checkpointCrossedWhileTrading_ += !CrossingAllowed(state_[i], stateSince_[i], now);
            }
        }
    }

    bool baseline_;
    bool strict_;
    std::uint32_t band_;
    std::vector<itch::Event> chunk_;
    std::vector<std::unique_ptr<FastOrderbook>> fast_;
    std::vector<std::unique_ptr<Orderbook>> base_;
    std::vector<char> state_;
    std::vector<std::uint64_t> stateSince_;
    std::vector<std::string> names_;
    Clock::duration fastTime_{ }, baseTime_{ };
    std::uint64_t events_{ };
    std::uint64_t checkpoints_{ }, mismatches_{ }, checkpointCrossed_{ }, checkpointCrossedWhileTrading_{ };
    std::uint64_t crossedMessages_{ }, crossedWhileTrading_{ };
    std::set<std::string> crossedSymbols_;
};

int RunAllSymbols(const Options& options)
{
    if (options.pin_ >= 0)
        perf::PinCurrentThread(options.pin_);

    std::printf("Replaying every symbol in %s ...\n", options.path_.c_str());
    auto replay = std::make_unique<AllSymbols>(options.engine_ != "fast", options.band_, options.strict_);
    std::FILE* file = Open(options.path_);
    const auto start = Clock::now();
    const auto stats = itch::ParseFile(file, *replay);
    replay->Flush();
    const auto seconds = Seconds(Clock::now() - start);
    if (file != stdin)
        std::fclose(file);

    std::printf("  %" PRIu64 " messages parsed (malformed %" PRIu64 ", unknown %" PRIu64 ")\n", stats.messages_, stats.malformed_, stats.unknown_);
    replay->Report(seconds);
    return replay->Passed() ? 0 : 2;
}

}

int main(int argc, char** argv)
{
    Options options;
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        auto next = [&]() -> std::string
        {
            if (i + 1 >= argc)
            {
                std::fprintf(stderr, "%s needs a value\n", arg.c_str());
                std::exit(1);
            }
            return argv[++i];
        };

        if (arg == "--symbol") options.symbol_ = next();
        else if (arg == "--all") options.all_ = true;
        else if (arg == "--pin") options.pin_ = std::stoi(next());
        else if (arg == "--warmup") options.warmup_ = std::stoi(next());
        else if (arg == "--runs") options.runs_ = std::max(1, std::stoi(next()));
        else if (arg == "--expect-open") options.expectOpen_ = std::stod(next());
        else if (arg == "--expect-close") options.expectClose_ = std::stod(next());
        else if (arg == "--engine") options.engine_ = next();
        else if (arg == "--skip-baseline") options.engine_ = "fast";
        else if (arg == "--no-validate") options.validate_ = false;
        else if (arg == "--strict") options.strict_ = true;
        else if (arg == "--tick") options.tick_ = std::stoi(next());
        else if (arg == "--band") options.band_ = static_cast<std::uint32_t>(std::stoul(next()));
        else if (options.path_.empty()) options.path_ = arg;
        else
        {
            std::fprintf(stderr, "unknown argument %s\n", arg.c_str());
            return 1;
        }
    }

    if (options.path_.empty())
    {
        std::fprintf(stderr,
            "usage: replay <itch-file|-> [--symbol AAPL] [--all] [--pin CPU] [--warmup N] [--runs N]\n"
            "              [--expect-open PRICE] [--expect-close PRICE] [--engine fast|baseline|both] [--no-validate]\n"
            "              [--tick PRICE_UNITS] [--band TICKS] [--strict]\n");
        return 1;
    }

    return options.all_ ? RunAllSymbols(options) : RunSingleSymbol(options);
}
