#ifndef PS2APA_H
#define PS2APA_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Ps2
{

enum class ApaHeaderState
{
    NotPresent,
    Valid,
    BadChecksum,
    InvalidMbr
};

struct ApaHeaderInfo
{
    ApaHeaderState state = ApaHeaderState::NotPresent;
    std::uint32_t storedChecksum = 0;
    std::uint32_t calculatedChecksum = 0;
    std::uint32_t next = 0;
    std::uint32_t previous = 0;
    std::uint32_t start = 0;
    std::uint32_t length = 0;
    std::uint16_t type = 0;
    std::uint16_t flags = 0;
    std::uint32_t subPartitionCount = 0;
    std::uint32_t modificationVersion = 0;
    std::uint32_t mbrVersion = 0;
    std::uint32_t osdStart = 0;
    std::uint32_t osdSize = 0;
    std::string id;
    std::string message;
};

struct ApaBankProbe
{
    std::uint32_t index = 0;
    std::uint64_t baseSector = 0;
    ApaHeaderInfo header;
};

struct ApaPartitionProbe
{
    std::uint32_t relativeSector = 0;
    std::uint64_t physicalSector = 0;
    ApaHeaderInfo header;
};

struct ApaBankSpaceInfo
{
    std::uint32_t totalChunks = 0;
    std::uint32_t usedChunks = 0;
    std::uint32_t freeChunks = 0;
    std::uint32_t largestFreeRun = 0;
};

struct ApaHdlAllocationEstimate
{
    bool fits = false;
    std::uint32_t requiredChunks = 0;
    std::uint32_t partitionRuns = 0;
    std::uint32_t totalChunks = 0;
    std::uint32_t usedChunks = 0;
    std::uint32_t freeChunks = 0;
    std::uint32_t largestFreeRun = 0;
};

class Apa
{
public:
    static constexpr std::size_t HeaderSize = 1024;
    static constexpr std::uint32_t Magic = 0x00415041U;
    static constexpr std::uint16_t MbrPartitionType = 0x0001;
    static constexpr std::uint32_t AllocationChunkSectors = 0x40000U; // 128 MiB

    static std::uint32_t CalculateChecksum(const unsigned char *header, std::size_t size);
    static ApaHeaderInfo ParseHeader(const unsigned char *header, std::size_t size,
            bool requireMbr = false);

    static std::vector<ApaBankProbe> ProbePhysicalDrive(const std::string &devicePath,
            std::uint64_t diskSizeBytes, std::uint32_t maximumBanks = 8);

    // maximumPartitions==0 selects a large geometry-aware corruption guard.
    // Loop detection is independent and O(1) average case.
    static std::vector<ApaPartitionProbe> ReadPartitionChain(const std::string &devicePath,
            std::uint64_t bankBaseSector, std::uint64_t bankSectorCount,
            std::uint32_t maximumPartitions = 0);

    // Read-only native bank accounting used by AUTO destination selection.
    static ApaBankSpaceInfo MeasureBankSpace(const std::string &devicePath,
            std::uint64_t bankBaseSector, std::uint64_t bankSectorCount);

    // Mirrors the 128 MiB APA chunk/HDL overhead rules closely enough to decide
    // whether hdl_dump can allocate an image in this bank before attempting it.
    static ApaHdlAllocationEstimate EstimateHdlAllocation(const std::string &devicePath,
            std::uint64_t bankBaseSector, std::uint64_t bankSectorCount,
            std::uint64_t imageBytes);
};

} // namespace Ps2

#endif // PS2APA_H
