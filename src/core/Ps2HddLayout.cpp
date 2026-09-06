#include "Ps2HddLayout.h"

#include <algorithm>

namespace Ps2
{

HddLayout HddLayoutPlanner::Plan(std::uint64_t diskSizeBytes, HddLayoutMode mode)
{
    HddLayout result;
    result.mode = mode;
    result.diskSizeBytes = diskSizeBytes;

    if (diskSizeBytes < MinimumDiskSizeBytes)
    {
        result.message = "Disk is smaller than the 8 GiB safety minimum.";
        return result;
    }
    if ((diskSizeBytes % SectorSize) != 0)
    {
        result.message = "Disk capacity is not aligned to a 512-byte sector.";
        return result;
    }

    result.diskSectorCount = diskSizeBytes / SectorSize;
    const std::uint32_t requestedBanks = mode == HddLayoutMode::StandardApa ? 1 :
            static_cast<std::uint32_t>(std::min<std::uint64_t>(MaximumBankCount,
            (result.diskSectorCount + BankBoundarySectors - 1) / BankBoundarySectors));

    std::uint64_t representedSectors = 0;
    for (std::uint32_t index = 0; index < requestedBanks; index++)
    {
        const std::uint64_t base = static_cast<std::uint64_t>(index) * BankBoundarySectors;
        if (base >= result.diskSectorCount)
            break;

        const std::uint64_t physicalSectors = std::min(BankBoundarySectors,
                result.diskSectorCount - base);
        if (index != 0 && physicalSectors < MinimumBankSizeSectors)
            break;

        ApaBankLayout bank;
        bank.index = index;
        bank.baseSector = base;
        bank.physicalSectorCount = physicalSectors;
        bank.addressableSectorCount = std::min(physicalSectors, MaximumApaSectorCount);
        bank.role = index == 0 ? ApaBankRole::BootSystemAndGames : ApaBankRole::GamesOnly;
        result.banks.push_back(bank);
        representedSectors += bank.addressableSectorCount;
    }

    if (result.banks.empty())
    {
        result.message = "No usable APA bank fits on this disk.";
        return result;
    }

    result.unaddressedSectorCount = result.diskSectorCount - representedSectors;
    result.requiresExtendedOpl = result.banks.size() > 1;
    result.valid = true;

    if (mode == HddLayoutMode::StandardApa && result.diskSectorCount > MaximumApaSectorCount)
        result.message = "Standard APA will use only the first 2 TiB address range.";
    else if (result.requiresExtendedOpl)
        result.message = "Bank 0 remains FHDB-compatible; additional banks require the matching OPL build.";
    else
        result.message = "Standard bootable APA/FHDB layout.";

    return result;
}

} // namespace Ps2
