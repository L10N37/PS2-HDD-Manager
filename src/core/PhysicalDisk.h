#ifndef PHYSICALDISK_H
#define PHYSICALDISK_H

#include <cstdint>
#include <string>
#include <vector>

namespace Ps2
{

struct PhysicalDiskCandidate
{
    std::uint32_t diskNumber = 0;
    std::string devicePath;
    std::string model;
    std::string serialNumber;
    std::string busType;
    std::uint64_t size = 0;
    std::uint32_t logicalSectorSize = 0;
    std::uint32_t physicalSectorSize = 0;
    std::uint32_t partitionCount = 0;
    bool rawPartitionStyle = false;
    bool readOnly = false;
    bool systemDiskCheckAvailable = false;
    bool containsSystemVolume = false;
    bool containsBootPartition = false;
    std::string inspectionError;
};

class PhysicalDiskScanner
{
public:
    // Enumerates physical disks without opening any of them for writing.
    static std::vector<PhysicalDiskCandidate> Scan();
};

} // namespace Ps2

#endif // PHYSICALDISK_H
