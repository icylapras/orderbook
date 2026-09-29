//copies one symbol's messages (plus system events) out of a full-day ITCH
//file into a much smaller, still-valid ITCH file. Useful for profiling: a
//replay of the filtered file spends its time in the engine, not in parsing
//8 GB of other symbols.
//
//  itchfilter <in> <out> --symbol AAPL

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "itch/Itch.h"

int main(int argc, char** argv)
{
    if (argc != 5 || std::string{ argv[3] } != "--symbol")
    {
        std::fprintf(stderr, "usage: itchfilter <in> <out> --symbol AAPL\n");
        return 1;
    }

    std::FILE* in = std::fopen(argv[1], "rb");
    std::FILE* out = std::fopen(argv[2], "wb");
    if (!in || !out)
    {
        std::perror("open");
        return 1;
    }

    std::string symbol = argv[4];
    symbol.resize(8, ' ');

    constexpr std::uint32_t Unknown = UINT32_MAX;
    std::uint32_t locate = Unknown;
    std::uint64_t kept = 0, total = 0;

    std::vector<std::uint8_t> buffer(std::size_t{ 64 } << 20);
    std::size_t carried = 0;
    while (true)
    {
        const auto got = std::fread(buffer.data() + carried, 1, buffer.size() - carried, in);
        const auto available = carried + got;
        std::size_t offset = 0;

        //walk the framing only; every message has type at byte 0 and locate at bytes 1-2
        while (available - offset >= 2)
        {
            const std::size_t length = itch::Load16(buffer.data() + offset);
            if (available - offset - 2 < length)
                break;

            const std::uint8_t* m = buffer.data() + offset + 2;
            ++total;
            if (length >= 3)
            {
                const char type = static_cast<char>(m[0]);
                const auto messageLocate = itch::Load16(m + 1);
                if (type == 'R' && length >= 19 && std::memcmp(m + 11, symbol.data(), 8) == 0)
                    locate = messageLocate;

                if (type == 'S' || (locate != Unknown && messageLocate == locate))
                {
                    std::fwrite(buffer.data() + offset, 1, 2 + length, out);
                    ++kept;
                }
            }
            offset += 2 + length;
        }

        carried = available - offset;
        std::memmove(buffer.data(), buffer.data() + offset, carried);
        if (got == 0)
            break;
    }

    std::fclose(in);
    std::fclose(out);
    std::printf("kept %llu of %llu messages\n", static_cast<unsigned long long>(kept), static_cast<unsigned long long>(total));
    return locate == Unknown ? 2 : 0;
}
