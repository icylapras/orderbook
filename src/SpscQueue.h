#pragma once

#include <atomic>
#include <cstddef>
#include <deque>
#include <mutex>
#include <type_traits>
#include <vector>

//bounded lock-free single-producer/single-consumer ring buffer.
//
// - head_ (consumer-owned) and tail_ (producer-owned) sit on separate cache
//   lines so the two threads don't false-share
// - each side keeps a cached copy of the other side's index and only reloads
//   the shared atomic when the cache says the queue looks full/empty; in
//   steady state that removes most cross-core cache-line transfers
// - acquire/release ordering is the whole synchronisation protocol: the
//   producer's release store of tail_ publishes the slot it just wrote, the
//   consumer's release store of head_ hands the slot back
template <typename T>
class SpscQueue
{
    static_assert(std::is_nothrow_move_constructible_v<T>);

public:
    //capacity is rounded up to a power of two so wrap-around is a mask
    explicit SpscQueue(std::size_t capacity)
    {
        std::size_t size = 2;
        while (size < capacity)
            size *= 2;
        slots_.resize(size);
        mask_ = size - 1;
    }

    SpscQueue(const SpscQueue&) = delete;
    SpscQueue& operator=(const SpscQueue&) = delete;

    //producer thread only
    bool TryPush(const T& value)
    {
        const auto tail = tail_.load(std::memory_order_relaxed);
        if (tail - cachedHead_ == slots_.size())
        {
            cachedHead_ = head_.load(std::memory_order_acquire);
            if (tail - cachedHead_ == slots_.size())
                return false;
        }

        slots_[tail & mask_] = value;
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    //consumer thread only
    bool TryPop(T& out)
    {
        const auto head = head_.load(std::memory_order_relaxed);
        if (head == cachedTail_)
        {
            cachedTail_ = tail_.load(std::memory_order_acquire);
            if (head == cachedTail_)
                return false;
        }

        out = std::move(slots_[head & mask_]);
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    std::size_t Capacity() const { return slots_.size(); }

private:
    static constexpr std::size_t CacheLine = 64;

    std::vector<T> slots_;
    std::size_t mask_{ };

    alignas(CacheLine) std::atomic<std::size_t> head_{ 0 };
    std::size_t cachedTail_{ 0 };//consumer's view of tail_

    alignas(CacheLine) std::atomic<std::size_t> tail_{ 0 };
    std::size_t cachedHead_{ 0 };//producer's view of head_
};

//the conventional alternative: a std::deque behind a mutex, same interface,
//used as the baseline in the queue benchmark
template <typename T>
class MutexQueue
{
public:
    explicit MutexQueue(std::size_t capacity) : capacity_{ capacity } { }

    bool TryPush(const T& value)
    {
        std::scoped_lock lock{ mutex_ };
        if (queue_.size() == capacity_)
            return false;
        queue_.push_back(value);
        return true;
    }

    bool TryPop(T& out)
    {
        std::scoped_lock lock{ mutex_ };
        if (queue_.empty())
            return false;
        out = std::move(queue_.front());
        queue_.pop_front();
        return true;
    }

    std::size_t Capacity() const { return capacity_; }

private:
    std::mutex mutex_;
    std::deque<T> queue_;
    std::size_t capacity_;
};
