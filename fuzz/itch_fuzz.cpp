//libFuzzer target: arbitrary bytes through the ITCH parser and on into the
//book. Under ASan/UBSan this checks the parser never reads out of bounds on
//hostile framing, and the book survives nonsense order flow (unknown
//references, zero sizes, extreme prices) without corrupting itself. Two
//price-level layouts (tick ladder with a tiny band, and pure sorted vector)
//are replayed side by side and must always agree.
//
//  cmake -DCMAKE_CXX_COMPILER=clang++ -DORDERBOOK_FUZZ=ON -DORDERBOOK_SANITIZE=address,undefined ..
//  ./fuzz_itch -max_total_time=60

#include <cstddef>
#include <cstdint>
#include <cstdlib>

#include "Orderbook.h"
#include "itch/Itch.h"
#include "itch/Replay.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    itch::EventCollector collector;//all symbols
    itch::ParseStats stats;
    const auto consumed = itch::Parse(data, size, collector, stats);
    if (consumed > size)
        std::abort();

    Orderbook ladder{ 64, 100, 64 };
    Orderbook sorted{ 64, 1, 0 };
    for (const auto& e : collector.Events())
    {
        itch::Apply(ladder, e);
        itch::Apply(sorted, e);
        if (ladder.GetTopOfBook() != sorted.GetTopOfBook() || ladder.Size() != sorted.Size())
            std::abort();
    }
    return 0;
}
