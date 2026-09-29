//"network" thread -> matching thread handoff: lock-free SPSC ring vs a
//mutex-guarded deque.
//
//  bench_queue [itch-file --symbol AAPL] [--producer-cpu N] [--consumer-cpu N]
//
//The producer plays the feed handler: it stamps each ITCH event with the TSC
//and pushes it; the consumer owns a FastOrderbook, pops, applies, and records
//stamp-to-applied latency. Two modes:
//  - saturated: producer pushes as fast as it can  -> throughput
//  - paced:     one message every ~1us             -> handoff latency without
//                                                     queueing delay
//Without a file, a synthetic add/delete stream is used.

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <immintrin.h>

#include "FastOrderbook.h"
#include "SpscQueue.h"
#include "itch/ItchFile.h"
#include "itch/Replay.h"
#include "perf/Latency.h"

namespace
{

struct Message
{
    itch::Event event_;
    std::uint64_t stamp_;
};

std::vector<itch::Event> SyntheticEvents(std::size_t count)
{
    std::mt19937_64 rng{ 5 };
    std::vector<itch::Event> events;
    std::vector<std::uint64_t> live;
    std::uint64_t nextRef = 1;
    while (events.size() < count)
    {
        if (live.size() < 1000 || rng() % 2)
        {
            const char side = rng() % 2 ? 'B' : 'S';
            //bids below 100.00, asks above: never crosses
            const auto price = side == 'B' ? 1'000'000 - 100 * static_cast<std::uint32_t>(1 + rng() % 50)
                                           : 1'000'000 + 100 * static_cast<std::uint32_t>(rng() % 50);
            events.push_back({ 0, nextRef, 0, price, 100, 1, 'A', side });
            live.push_back(nextRef++);
        }
        else
        {
            const auto i = rng() % live.size();
            events.push_back({ 0, live[i], 0, 0, 0, 1, 'D', 0 });
            live[i] = live.back();
            live.pop_back();
        }
    }
    return events;
}

struct Result
{
    double throughput_;
    perf::Histogram latency_;
};

template <typename Queue>
Result Run(const std::vector<itch::Event>& events, int producerCpu, int consumerCpu, std::uint64_t paceTicks)
{
    Queue queue{ 4096 };
    std::atomic<bool> ready{ false };
    Result result;

    std::thread consumer{ [&] {
        if (consumerCpu >= 0)
            perf::PinCurrentThread(consumerCpu);
        FastOrderbook book{ events.size() };
        ready.store(true, std::memory_order_release);

        Message m;
        for (std::size_t received = 0; received < events.size();)
        {
            if (!queue.TryPop(m))
            {
                _mm_pause();
                continue;
            }
            itch::Apply(book, m.event_);
            result.latency_.Record(perf::StopTicks() - m.stamp_);
            ++received;
        }
    } };

    if (producerCpu >= 0)
        perf::PinCurrentThread(producerCpu);
    while (!ready.load(std::memory_order_acquire)) { }

    const auto start = std::chrono::steady_clock::now();
    auto next = perf::StartTicks();
    for (const auto& e : events)
    {
        if (paceTicks)
        {
            while (perf::StartTicks() < next)
                _mm_pause();
            next += paceTicks;
        }
        Message m{ e, perf::StartTicks() };
        while (!queue.TryPush(m))
            _mm_pause();
    }
    consumer.join();
    const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    result.throughput_ = static_cast<double>(events.size()) / seconds;
    return result;
}

void Print(const char* name, const Result& r, double ticksPerNs, bool paced)
{
    const auto s = perf::Summarize(r.latency_, ticksPerNs);
    if (paced)
        std::printf("  %-12s handoff+apply latency  p50 %6.0f ns  p99 %6.0f ns  p99.9 %7.0f ns  max %8.1f us\n",
            name, s.p50_, s.p99_, s.p999_, s.max_ / 1000);
    else
        std::printf("  %-12s %7.2f M msgs/sec\n", name, r.throughput_ / 1e6);
}

}

int main(int argc, char** argv)
{
    std::string path, symbol = "AAPL";
    int producerCpu = 2, consumerCpu = 4;
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "--symbol" && i + 1 < argc) symbol = argv[++i];
        else if (arg == "--producer-cpu" && i + 1 < argc) producerCpu = std::atoi(argv[++i]);
        else if (arg == "--consumer-cpu" && i + 1 < argc) consumerCpu = std::atoi(argv[++i]);
        else path = arg;
    }

    std::vector<itch::Event> events;
    if (path.empty())
    {
        events = SyntheticEvents(2'000'000);
        std::printf("synthetic feed: %zu events\n", events.size());
    }
    else
    {
        itch::EventCollector collector{ symbol };
        std::FILE* f = std::fopen(path.c_str(), "rb");
        if (!f)
        {
            std::perror(path.c_str());
            return 1;
        }
        itch::ParseFile(f, collector);
        std::fclose(f);
        for (const auto& e : collector.Events())
            if (itch::IsBookEvent(e.type_))
                events.push_back(e);
        std::printf("%s from %s: %zu book events\n", symbol.c_str(), path.c_str(), events.size());
    }

    const double ticksPerNs = perf::CalibrateTicksPerNs();
    std::printf("producer on CPU %d, consumer on CPU %d, queue capacity 4096\n", producerCpu, consumerCpu);

    //warm-up both paths once
    Run<SpscQueue<Message>>(events, producerCpu, consumerCpu, 0);
    Run<MutexQueue<Message>>(events, producerCpu, consumerCpu, 0);

    std::printf("\nsaturated (producer never waits):\n");
    const auto spsc = Run<SpscQueue<Message>>(events, producerCpu, consumerCpu, 0);
    const auto mutex = Run<MutexQueue<Message>>(events, producerCpu, consumerCpu, 0);
    Print("SPSC ring", spsc, ticksPerNs, false);
    Print("mutex+deque", mutex, ticksPerNs, false);
    std::printf("  speed-up: %.1fx\n", spsc.throughput_ / mutex.throughput_);

    //pace below both queues' capacity so latency is handoff cost, not backlog
    const auto pace = static_cast<std::uint64_t>(1000 * ticksPerNs);
    std::vector<itch::Event> sample(events.begin(), events.begin() + static_cast<std::ptrdiff_t>(std::min<std::size_t>(events.size(), 500'000)));
    std::printf("\npaced (1 message / us, %zu messages):\n", sample.size());
    const auto spscPaced = Run<SpscQueue<Message>>(sample, producerCpu, consumerCpu, pace);
    const auto mutexPaced = Run<MutexQueue<Message>>(sample, producerCpu, consumerCpu, pace);
    Print("SPSC ring", spscPaced, ticksPerNs, true);
    Print("mutex+deque", mutexPaced, ticksPerNs, true);
    return 0;
}
