#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "Itch.h"

namespace itch
{

//streams a whole ITCH file (or stdin, e.g. `pigz -dc day.gz | replay -`)
//through Parse() in large chunks; a message split across two chunks is
//carried over to the front of the buffer
template <typename Handler>
ParseStats ParseFile(std::FILE* file, Handler& handler, std::uint64_t* bytesRead = nullptr,
    std::size_t bufferSize = std::size_t{ 64 } << 20)
{
    ParseStats stats;
    std::vector<std::uint8_t> buffer(bufferSize);
    std::size_t carried = 0;
    std::uint64_t total = 0;

    while (true)
    {
        const std::size_t got = std::fread(buffer.data() + carried, 1, buffer.size() - carried, file);
        total += got;
        const std::size_t available = carried + got;

        const std::size_t consumed = Parse(buffer.data(), available, handler, stats);
        carried = available - consumed;
        std::memmove(buffer.data(), buffer.data() + consumed, carried);

        if (got == 0)
            break;
    }

    //a non-empty tail at EOF is a truncated final message
    if (carried != 0)
        ++stats.malformed_;

    if (bytesRead)
        *bytesRead = total;
    return stats;
}

}
