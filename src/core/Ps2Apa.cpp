#include "Ps2Apa.h"

#include "Ps2HddLayout.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#elif defined(__linux__)
#include <cerrno>
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace
{

std::uint16_t readLittleEndian16(const unsigned char *source)
{
    return static_cast<std::uint16_t>(source[0]) |
            static_cast<std::uint16_t>(source[1] << 8);
}

std::uint32_t readLittleEndian32(const unsigned char *source)
{
    return static_cast<std::uint32_t>(source[0]) |
            (static_cast<std::uint32_t>(source[1]) << 8) |
            (static_cast<std::uint32_t>(source[2]) << 16) |
            (static_cast<std::uint32_t>(source[3]) << 24);
}

std::string fixedString(const unsigned char *source, std::size_t size)
{
    const unsigned char *end = std::find(source, source + size, 0);
    return std::string(reinterpret_cast<const char*>(source),
            reinterpret_cast<const char*>(end));
}

#ifdef _WIN32
std::string windowsError(const char *operation)
{
    return std::string(operation) + " failed (Windows error " +
            std::to_string(GetLastError()) + ").";
}
#elif defined(__linux__)
std::string linuxError(const char *operation, const std::string &path)
{
    return std::string(operation) + " failed for " + path + ": " + std::strerror(errno);
}
#endif

} // namespace

namespace Ps2
{

std::uint32_t Apa::CalculateChecksum(const unsigned char *header, std::size_t size)
{
    if (header == nullptr || size < HeaderSize)
        throw std::invalid_argument("An APA checksum requires a complete 1024-byte header.");

    std::uint32_t sum = 0;
    for (std::size_t offset = 4; offset < HeaderSize; offset += 4)
        sum += readLittleEndian32(header + offset);
    return sum;
}

ApaHeaderInfo Apa::ParseHeader(const unsigned char *header, std::size_t size, bool requireMbr)
{
    ApaHeaderInfo result;
    if (header == nullptr || size < HeaderSize)
    {
        result.message = "Incomplete APA header.";
        return result;
    }

    if (readLittleEndian32(header + 4) != Magic)
    {
        result.message = "APA magic is not present.";
        return result;
    }

    result.storedChecksum = readLittleEndian32(header);
    result.calculatedChecksum = CalculateChecksum(header, size);
    result.next = readLittleEndian32(header + 8);
    result.previous = readLittleEndian32(header + 12);
    result.id = fixedString(header + 16, 32);
    result.start = readLittleEndian32(header + 64);
    result.length = readLittleEndian32(header + 68);
    result.type = readLittleEndian16(header + 72);
    result.flags = readLittleEndian16(header + 74);
    result.subPartitionCount = readLittleEndian32(header + 76);
    result.modificationVersion = readLittleEndian32(header + 96);
    result.mbrVersion = readLittleEndian32(header + 288);
    result.osdStart = readLittleEndian32(header + 304);
    result.osdSize = readLittleEndian32(header + 308);

    if (result.storedChecksum != result.calculatedChecksum)
    {
        result.state = ApaHeaderState::BadChecksum;
        result.message = "APA magic was found, but the header checksum is invalid.";
        return result;
    }

    if (requireMbr)
    {
        static constexpr char SonyMbrMagic[] = "Sony Computer Entertainment Inc.";
        const bool validMbr = result.id == "__mbr" && result.start == 0 &&
                result.type == MbrPartitionType &&
                std::memcmp(header + 256, SonyMbrMagic, 32) == 0;
        if (!validMbr)
        {
            result.state = ApaHeaderState::InvalidMbr;
            result.message = "The header is APA, but it is not a valid PS2 __mbr header.";
            return result;
        }
    }

    result.state = ApaHeaderState::Valid;
    result.message = requireMbr ? "Valid PS2 APA __mbr header." : "Valid APA partition header.";
    return result;
}

std::vector<ApaBankProbe> Apa::ProbePhysicalDrive(const std::string &devicePath,
        std::uint64_t diskSizeBytes, std::uint32_t maximumBanks)
{
    std::vector<ApaBankProbe> result;
#ifdef _WIN32
    const std::wstring widePath(devicePath.begin(), devicePath.end());
    HANDLE handle = CreateFileW(widePath.c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        throw std::runtime_error(windowsError("Opening the physical disk for APA inspection"));

    try
    {
        const std::uint64_t totalSectors = diskSizeBytes / HddLayoutPlanner::SectorSize;
        for (std::uint32_t index = 0; index < maximumBanks; index++)
        {
            const std::uint64_t baseSector = static_cast<std::uint64_t>(index) *
                    HddLayoutPlanner::BankBoundarySectors;
            if (baseSector >= totalSectors ||
                    totalSectors - baseSector < HeaderSize / HddLayoutPlanner::SectorSize)
                break;

            const std::uint64_t byteOffset = baseSector * HddLayoutPlanner::SectorSize;
            if (byteOffset > static_cast<std::uint64_t>(std::numeric_limits<LONGLONG>::max()))
                break;

            LARGE_INTEGER position;
            position.QuadPart = static_cast<LONGLONG>(byteOffset);
            if (!SetFilePointerEx(handle, position, nullptr, FILE_BEGIN))
                throw std::runtime_error(windowsError("Seeking to an APA bank"));

            std::array<unsigned char, HeaderSize> header = {};
            DWORD bytesRead = 0;
            if (!ReadFile(handle, header.data(), static_cast<DWORD>(header.size()),
                    &bytesRead, nullptr) || bytesRead != header.size())
                throw std::runtime_error(windowsError("Reading an APA bank header"));

            ApaBankProbe probe;
            probe.index = index;
            probe.baseSector = baseSector;
            probe.header = ParseHeader(header.data(), header.size(), true);
            result.push_back(std::move(probe));
        }
    }
    catch (...)
    {
        CloseHandle(handle);
        throw;
    }
    CloseHandle(handle);
#elif defined(__linux__)
    const int descriptor = open(devicePath.c_str(), O_RDONLY | O_CLOEXEC);
    if (descriptor < 0)
        throw std::runtime_error(linuxError("Opening the physical disk for APA inspection",
                devicePath));

    try
    {
        const std::uint64_t totalSectors = diskSizeBytes / HddLayoutPlanner::SectorSize;
        for (std::uint32_t index = 0; index < maximumBanks; index++)
        {
            const std::uint64_t baseSector = static_cast<std::uint64_t>(index) *
                    HddLayoutPlanner::BankBoundarySectors;
            if (baseSector >= totalSectors ||
                    totalSectors - baseSector < HeaderSize / HddLayoutPlanner::SectorSize)
                break;

            const std::uint64_t byteOffset = baseSector * HddLayoutPlanner::SectorSize;
            if (byteOffset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()))
                break;

            std::array<unsigned char, HeaderSize> header = {};
            std::size_t completed = 0;
            while (completed < header.size())
            {
                const ssize_t received = pread(descriptor, header.data() + completed,
                        header.size() - completed,
                        static_cast<off_t>(byteOffset + completed));
                if (received < 0)
                    throw std::runtime_error(linuxError("Reading an APA bank header", devicePath));
                if (received == 0)
                    throw std::runtime_error("The physical disk returned an incomplete APA header.");
                completed += static_cast<std::size_t>(received);
            }

            ApaBankProbe probe;
            probe.index = index;
            probe.baseSector = baseSector;
            probe.header = ParseHeader(header.data(), header.size(), true);
            result.push_back(std::move(probe));
        }
    }
    catch (...)
    {
        close(descriptor);
        throw;
    }
    close(descriptor);
#else
    (void)devicePath;
    (void)diskSizeBytes;
    (void)maximumBanks;
#endif
    return result;
}


std::vector<ApaPartitionProbe> Apa::ReadPartitionChain(const std::string &devicePath,
        std::uint64_t bankBaseSector, std::uint64_t bankSectorCount,
        std::uint32_t maximumPartitions)
{
    if (bankSectorCount < HeaderSize / HddLayoutPlanner::SectorSize)
        throw std::invalid_argument("APA bank is too small to contain an MBR header.");
    if (maximumPartitions == 0)
        throw std::invalid_argument("APA partition-chain limit must be non-zero.");

    std::vector<ApaPartitionProbe> result;
    std::vector<std::uint32_t> visited;

#ifdef _WIN32
    const std::wstring widePath(devicePath.begin(), devicePath.end());
    HANDLE handle = CreateFileW(widePath.c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        throw std::runtime_error(windowsError("Opening the physical disk for APA chain inspection"));

    auto readHeader = [&](std::uint64_t physicalSector) {
        const std::uint64_t byteOffset = physicalSector * HddLayoutPlanner::SectorSize;
        if (byteOffset > static_cast<std::uint64_t>(std::numeric_limits<LONGLONG>::max()))
            throw std::runtime_error("APA chain offset exceeds the host file API range.");
        LARGE_INTEGER position;
        position.QuadPart = static_cast<LONGLONG>(byteOffset);
        if (!SetFilePointerEx(handle, position, nullptr, FILE_BEGIN))
            throw std::runtime_error(windowsError("Seeking to an APA partition header"));
        std::array<unsigned char, HeaderSize> header = {};
        DWORD bytesRead = 0;
        if (!ReadFile(handle, header.data(), static_cast<DWORD>(header.size()), &bytesRead, nullptr) ||
                bytesRead != header.size())
            throw std::runtime_error(windowsError("Reading an APA partition header"));
        return header;
    };

    try
    {
        std::uint32_t relativeSector = 0;
        for (std::uint32_t entry = 0; entry < maximumPartitions; entry++)
        {
            if (relativeSector >= bankSectorCount)
                throw std::runtime_error("APA partition chain points outside the selected bank.");
            if (std::find(visited.begin(), visited.end(), relativeSector) != visited.end())
                throw std::runtime_error("APA partition chain contains a loop.");
            visited.push_back(relativeSector);

            const std::uint64_t physicalSector = bankBaseSector + relativeSector;
            const auto raw = readHeader(physicalSector);
            const ApaHeaderInfo parsed = ParseHeader(raw.data(), raw.size(), entry == 0);
            if (parsed.state != ApaHeaderState::Valid)
                throw std::runtime_error("Invalid APA partition header at bank-relative sector " +
                        std::to_string(relativeSector) + ": " + parsed.message);
            if (parsed.start != relativeSector)
                throw std::runtime_error("APA partition header start field does not match its location.");

            result.push_back({ relativeSector, physicalSector, parsed });
            if (parsed.next == 0)
                return result;
            relativeSector = parsed.next;
        }
    }
    catch (...)
    {
        CloseHandle(handle);
        throw;
    }
    CloseHandle(handle);
#elif defined(__linux__)
    const int descriptor = open(devicePath.c_str(), O_RDONLY | O_CLOEXEC);
    if (descriptor < 0)
        throw std::runtime_error(linuxError("Opening the physical disk for APA chain inspection",
                devicePath));

    auto readHeader = [&](std::uint64_t physicalSector) {
        const std::uint64_t byteOffset = physicalSector * HddLayoutPlanner::SectorSize;
        if (byteOffset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()))
            throw std::runtime_error("APA chain offset exceeds the host file API range.");
        std::array<unsigned char, HeaderSize> header = {};
        std::size_t completed = 0;
        while (completed < header.size())
        {
            const ssize_t received = pread(descriptor, header.data() + completed,
                    header.size() - completed,
                    static_cast<off_t>(byteOffset + completed));
            if (received < 0)
                throw std::runtime_error(linuxError("Reading an APA partition header", devicePath));
            if (received == 0)
                throw std::runtime_error("The physical disk returned an incomplete APA partition header.");
            completed += static_cast<std::size_t>(received);
        }
        return header;
    };

    try
    {
        std::uint32_t relativeSector = 0;
        for (std::uint32_t entry = 0; entry < maximumPartitions; entry++)
        {
            if (relativeSector >= bankSectorCount)
                throw std::runtime_error("APA partition chain points outside the selected bank.");
            if (std::find(visited.begin(), visited.end(), relativeSector) != visited.end())
                throw std::runtime_error("APA partition chain contains a loop.");
            visited.push_back(relativeSector);

            const std::uint64_t physicalSector = bankBaseSector + relativeSector;
            const auto raw = readHeader(physicalSector);
            const ApaHeaderInfo parsed = ParseHeader(raw.data(), raw.size(), entry == 0);
            if (parsed.state != ApaHeaderState::Valid)
                throw std::runtime_error("Invalid APA partition header at bank-relative sector " +
                        std::to_string(relativeSector) + ": " + parsed.message);
            if (parsed.start != relativeSector)
                throw std::runtime_error("APA partition header start field does not match its location.");

            result.push_back({ relativeSector, physicalSector, parsed });
            if (parsed.next == 0)
                return result;
            relativeSector = parsed.next;
        }
    }
    catch (...)
    {
        close(descriptor);
        throw;
    }
    close(descriptor);
#else
    (void)devicePath;
    (void)bankBaseSector;
    (void)bankSectorCount;
    (void)maximumPartitions;
#endif

    if (result.size() == maximumPartitions)
        throw std::runtime_error("APA partition chain exceeded the configured safety limit.");
    return result;
}


} // namespace Ps2
