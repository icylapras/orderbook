//libFuzzer target: arbitrary bytes through the ITCH parser and on into both
//engines. Under ASan/UBSan this checks the parser never reads out of bounds
//on hostile framing, and the engines survive nonsense order flow (unknown
//references, zero sizes, extreme prices) without corrupting themselves.
//
//  cmake -DCMAKE_CXX_COMPILER=clang++ -DORDERBOOK_FUZZ=ON -DORDERBOOK_SANITIZE=address,undefined ..
//  ./fuzz_itch -max_total_time=60

#include <cstddef>
#include <cstdint>
#include <cstdlib>

#include "FastOrderbook.h"
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

    FastOrderbook fast{ 64 };
    Orderbook baseline{ false };
    for (const auto& e : collector.Events())
    {
        itch::Apply(fast, e);
        itch::Apply(baseline, e);
        if (fast.GetTopOfBook() != baseline.GetTopOfBook() || fast.Size() != baseline.Size())
            std::abort();
    }
    return 0;
}
