#pragma once

//latency measurement helpers: a serialized TSC timer, a log-linear histogram
//(HdrHistogram-style: constant ~1.6% relative precision, O(1) record, fixed
//memory, so recording on the hot path never allocates), and core pinning

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>

#if defined(__x86_64__) || defined(_M_X64)
#include <x86intrin.h>
#define ORDERBOOK_HAS_TSC 1
#endif

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <pthread.h>
#include <sched.h>
#endif

namespace perf
{

//start/stop pair with the ordering fences Intel/AMD recommend: lfence before
//rdtsc stops earlier instructions leaking into the timed region; rdtscp at
//the end waits for the timed code to retire
inline std::uint64_t StartTicks()
{
#ifdef ORDERBOOK_HAS_TSC
    _mm_lfence();
    const auto t = __rdtsc();
    _mm_lfence();
    return t;
#else
    return static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

inline std::uint64_t StopTicks()
{
#ifdef ORDERBOOK_HAS_TSC
    unsigned aux;
    const auto t = __rdtscp(&aux);
    _mm_lfence();
    return t;
#else
    return static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

//ticks per nanosecond, measured against steady_clock over `window`
inline double CalibrateTicksPerNs(std::chrono::milliseconds window = std::chrono::milliseconds{ 200 })
{
    using Clock = std::chrono::steady_clock;
    const auto wallStart = Clock::now();
    const auto tickStart = StartTicks();
    while (Clock::now() - wallStart < window) { }
    const auto tickEnd = StopTicks();
    const auto wallEnd = Clock::now();

    const double ns = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(wallEnd - wallStart).count());
    return static_cast<double>(tickEnd - tickStart) / ns;
}

//values are bucketed by power of two, each power split into 2^SubBits linear
//sub-buckets; values below 2^SubBits are exact
class Histogram
{
public:
    static constexpr int SubBits = 6;
    static constexpr std::uint64_t SubCount = 1ull << SubBits;
    static constexpr std::size_t BucketCount = (64 - SubBits + 1) * SubCount;

    void Record(std::uint64_t value)
    {
        ++counts_[Index(value)];
        ++total_;
        max_ = std::max(max_, value);
        min_ = std::min(min_, value);
        sum_ += value;
    }

    void Merge(const Histogram& other)
    {
        for (std::size_t i = 0; i < BucketCount; ++i)
            counts_[i] += other.counts_[i];
        total_ += other.total_;
        max_ = std::max(max_, other.max_);
        min_ = std::min(min_, other.min_);
        sum_ += other.sum_;
    }

    std::uint64_t Count() const { return total_; }
    std::uint64_t Max() const { return total_ ? max_ : 0; }
    std::uint64_t Min() const { return total_ ? min_ : 0; }
    double Mean() const { return total_ ? static_cast<double>(sum_) / static_cast<double>(total_) : 0.0; }

    //value at quantile q in [0, 1]; returns the upper edge of the bucket the
    //quantile falls in (clamped to the true max), i.e. never under-reports
    std::uint64_t Percentile(double q) const
    {
        if (total_ == 0)
            return 0;

        const auto rank = static_cast<std::uint64_t>(q * static_cast<double>(total_ - 1)) + 1;
        std::uint64_t seen = 0;
        for (std::size_t i = 0; i < BucketCount; ++i)
        {
            seen += counts_[i];
            if (seen >= rank)
                return std::min(UpperEdge(i), max_);
        }
        return max_;
    }

    static std::size_t Index(std::uint64_t value)
    {
        if (value < SubCount)
            return static_cast<std::size_t>(value);

        const int magnitude = 63 - std::countl_zero(value);//floor(log2)
        const int shift = magnitude - SubBits;
        const std::uint64_t sub = (value >> shift) & (SubCount - 1);
        return static_cast<std::size_t>((shift + 1) * SubCount + sub);
    }

    static std::uint64_t UpperEdge(std::size_t index)
    {
        if (index < SubCount)
            return index;

        const auto shift = static_cast<int>(index / SubCount) - 1;
        const std::uint64_t sub = index % SubCount;
        const std::uint64_t lower = (SubCount | sub) << shift;
        return lower + ((1ull << shift) - 1);
    }

private:
    std::array<std::uint64_t, BucketCount> counts_{ };
    std::uint64_t total_{ };
    std::uint64_t max_{ };
    std::uint64_t min_{ UINT64_MAX };
    std::uint64_t sum_{ };
};

struct Summary
{
    std::uint64_t count_;
    double mean_, p50_, p90_, p99_, p999_, max_;
};

//summarise a histogram recorded in ticks, converting to nanoseconds
inline Summary Summarize(const Histogram& h, double ticksPerNs)
{
    auto ns = [ticksPerNs](std::uint64_t ticks) { return static_cast<double>(ticks) / ticksPerNs; };
    return Summary{
        h.Count(),
        h.Mean() / ticksPerNs,
        ns(h.Percentile(0.50)),
        ns(h.Percentile(0.90)),
        ns(h.Percentile(0.99)),
        ns(h.Percentile(0.999)),
        ns(h.Max()),
    };
}

inline bool PinCurrentThread(int cpu)
{
#ifdef _WIN32
    return SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR{ 1 } << cpu) != 0;
#else
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return pthread_setaffinity_np(pthread_self(), sizeof set, &set) == 0;
#endif
}

}
