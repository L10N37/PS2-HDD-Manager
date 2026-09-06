#ifndef PS2HDDLAYOUT_H
#define PS2HDDLAYOUT_H

#include <cstdint>
#include <string>
#include <vector>

namespace Ps2
{

enum class HddLayoutMode
{
    StandardApa,
    ExtendedApaBanks
};

enum class ApaBankRole
{
    BootSystemAndGames,
    GamesOnly
};

struct ApaBankLayout
{
    std::uint32_t index = 0;
    std::uint64_t baseSector = 0;
    std::uint64_t physicalSectorCount = 0;
    std::uint64_t addressableSectorCount = 0;
    ApaBankRole role = ApaBankRole::GamesOnly;
};

struct HddLayout
{
    HddLayoutMode mode = HddLayoutMode::StandardApa;
    std::uint64_t diskSizeBytes = 0;
    std::uint64_t diskSectorCount = 0;
    std::uint64_t unaddressedSectorCount = 0;
    std::vector<ApaBankLayout> banks;
    bool valid = false;
    bool requiresExtendedOpl = false;
    std::string message;
};

class HddLayoutPlanner
{
public:
    static constexpr std::uint64_t SectorSize = 512;
    static constexpr std::uint64_t BankBoundarySectors = 0x100000000ULL;
    static constexpr std::uint64_t MaximumApaSectorCount = 0xFFFFFFFFULL;
    static constexpr std::uint64_t MinimumDiskSizeBytes = 8ULL * 1024ULL * 1024ULL * 1024ULL;
    static constexpr std::uint64_t MinimumBankSizeSectors = 0x40000ULL; // 128 MiB
    static constexpr std::uint32_t MaximumBankCount = 8;
    static constexpr const char *SharedOplPartitionName = "+OPL";

    static HddLayout Plan(std::uint64_t diskSizeBytes, HddLayoutMode mode);
};

} // namespace Ps2

#endif // PS2HDDLAYOUT_H
