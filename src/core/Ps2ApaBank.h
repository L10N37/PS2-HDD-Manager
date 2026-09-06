#ifndef PS2APABANK_H
#define PS2APABANK_H

#include <cstdint>
#include <string>

namespace Ps2
{
class ApaBank
{
public:
    // Initializes a Bank 1+ as an ordinary, independent APA chain containing
    // only a conventional __mbr. hdl-dump then allocates HDL game partitions
    // relative to this bank. Bank 0 is deliberately refused here.
    static void InitializeGamesOnly(const std::string &targetPath,
            std::uint64_t diskSizeBytes, std::uint32_t bankIndex,
            bool overwriteExisting = false);
};
}
#endif
