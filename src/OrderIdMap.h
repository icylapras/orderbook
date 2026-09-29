#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "Usings.h"

//open-addressing hash map OrderId -> pool index, built for the hot path:
//one flat array (no per-node allocation like std::unordered_map), linear
//probing so a lookup is usually one cache line, and backward-shift deletion
//so there are no tombstones to degrade probes over a long trading day
class OrderIdMap
{
public:
    static constexpr std::uint32_t Missing = UINT32_MAX;

    explicit OrderIdMap(std::size_t expected = 16) { Rehash(CapacityFor(expected)); }

    std::uint32_t Find(OrderId key) const
    {
        for (std::size_t i = Home(key);; i = (i + 1) & mask_)
        {
            const Slot& slot = slots_[i];
            if (slot.value_ == Missing)
                return Missing;
            if (slot.key_ == key)
                return slot.value_;
        }
    }

    bool Contains(OrderId key) const { return Find(key) != Missing; }

    //returns false (and leaves the map unchanged) if the key already exists
    bool Insert(OrderId key, std::uint32_t value)
    {
        if ((size_ + 1) * 2 > slots_.size())
            Rehash(slots_.size() * 2);

        for (std::size_t i = Home(key);; i = (i + 1) & mask_)
        {
            Slot& slot = slots_[i];
            if (slot.value_ == Missing)
            {
                slot = Slot{ key, value };
                ++size_;
                return true;
            }
            if (slot.key_ == key)
                return false;
        }
    }

    bool Erase(OrderId key)
    {
        std::size_t hole = Home(key);
        while (true)
        {
            if (slots_[hole].value_ == Missing)
                return false;
            if (slots_[hole].key_ == key)
                break;
            hole = (hole + 1) & mask_;
        }

        //pull later members of the probe chain back into the hole, so every
        //key stays reachable from its home slot without tombstones
        for (std::size_t j = (hole + 1) & mask_; slots_[j].value_ != Missing; j = (j + 1) & mask_)
        {
            const std::size_t home = Home(slots_[j].key_);
            //the entry at j may move to the hole only if its home is not
            //cyclically inside (hole, j]
            const bool homeInRange = hole <= j ? (hole < home && home <= j) : (hole < home || home <= j);
            if (!homeInRange)
            {
                slots_[hole] = slots_[j];
                hole = j;
            }
        }

        slots_[hole].value_ = Missing;
        --size_;
        return true;
    }

    std::size_t Size() const { return size_; }

    void Clear()
    {
        for (auto& slot : slots_)
            slot.value_ = Missing;
        size_ = 0;
    }

    void Reserve(std::size_t expected)
    {
        const auto capacity = CapacityFor(expected);
        if (capacity > slots_.size())
            Rehash(capacity);
    }

private:
    struct Slot
    {
        OrderId key_{ };
        std::uint32_t value_{ Missing };
    };

    static std::size_t CapacityFor(std::size_t expected)
    {
        std::size_t capacity = 16;
        while (capacity < expected * 2)
            capacity *= 2;
        return capacity;
    }

    //fibonacci hashing: multiply by 2^64/phi and keep the top bits; spreads
    //sequential ids (which exchanges hand out) evenly across the table
    std::size_t Home(OrderId key) const
    {
        return static_cast<std::size_t>((key * 0x9E3779B97F4A7C15ull) >> shift_);
    }

    void Rehash(std::size_t capacity)
    {
        std::vector<Slot> old = std::move(slots_);
        slots_.assign(capacity, Slot{ });
        mask_ = capacity - 1;
        shift_ = 64;
        for (std::size_t c = capacity; c > 1; c >>= 1)
            --shift_;
        size_ = 0;

        for (const auto& slot : old)
            if (slot.value_ != Missing)
                Insert(slot.key_, slot.value_);
    }

    std::vector<Slot> slots_;
    std::size_t mask_{ };
    int shift_{ };
    std::size_t size_{ };
};
