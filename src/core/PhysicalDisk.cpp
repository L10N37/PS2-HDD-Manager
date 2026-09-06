#include "PhysicalDisk.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winioctl.h>
#elif defined(__linux__)
#include <system_error>
#endif

namespace
{

std::string trimAscii(std::string value)
{
    const auto notSpace = [](unsigned char character) { return character > 0x20; };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
    return value;
}

#ifdef _WIN32

class HandleGuard
{
public:
    explicit HandleGuard(HANDLE handle = INVALID_HANDLE_VALUE) : handle(handle) {}
    ~HandleGuard()
    {
        if (handle != INVALID_HANDLE_VALUE)
            CloseHandle(handle);
    }

    HandleGuard(const HandleGuard&) = delete;
    HandleGuard &operator=(const HandleGuard&) = delete;

    HANDLE get() const { return handle; }

private:
    HANDLE handle;
};

std::string windowsError(const std::string &operation, DWORD code = GetLastError())
{
    LPSTR message = nullptr;
    const DWORD length = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER |
            FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code,
            MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), reinterpret_cast<LPSTR>(&message), 0,
            nullptr);
    std::string result = operation + " failed (Windows error " + std::to_string(code) + ")";
    if (length != 0 && message != nullptr)
        result += ": " + trimAscii(std::string(message, length));
    if (message != nullptr)
        LocalFree(message);
    return result;
}

struct WindowsSystemDisks
{
    bool available = false;
    std::vector<std::uint32_t> numbers;
};

bool queryVolumeExtents(HANDLE volume, std::vector<DISK_EXTENT> &extents)
{
    std::vector<unsigned char> buffer(64 * 1024, 0);
    DWORD returned = 0;
    if (!DeviceIoControl(volume, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0,
            buffer.data(), static_cast<DWORD>(buffer.size()), &returned, nullptr))
        return false;

    const auto *volumeExtents = reinterpret_cast<const VOLUME_DISK_EXTENTS*>(buffer.data());
    const std::size_t headerSize = offsetof(VOLUME_DISK_EXTENTS, Extents);
    if (returned < headerSize + sizeof(DISK_EXTENT))
        return false;
    const std::size_t maximumExtents = (returned - headerSize) / sizeof(DISK_EXTENT);
    if (volumeExtents->NumberOfDiskExtents == 0 ||
            volumeExtents->NumberOfDiskExtents > maximumExtents)
        return false;

    extents.assign(volumeExtents->Extents,
            volumeExtents->Extents + volumeExtents->NumberOfDiskExtents);
    return true;
}

WindowsSystemDisks queryWindowsSystemDisks()
{
    WindowsSystemDisks result;
    std::vector<wchar_t> windowsDirectory(32768, L'\0');
    if (GetSystemWindowsDirectoryW(windowsDirectory.data(),
            static_cast<UINT>(windowsDirectory.size())) == 0)
        return result;

    std::vector<wchar_t> mountPath(32768, L'\0');
    if (!GetVolumePathNameW(windowsDirectory.data(), mountPath.data(),
            static_cast<DWORD>(mountPath.size())))
        return result;

    std::vector<wchar_t> volumeName(32768, L'\0');
    if (!GetVolumeNameForVolumeMountPointW(mountPath.data(), volumeName.data(),
            static_cast<DWORD>(volumeName.size())))
        return result;

    std::wstring path(volumeName.data());
    if (!path.empty() && path.back() == L'\\')
        path.pop_back();
    HandleGuard volume(CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (volume.get() == INVALID_HANDLE_VALUE)
        return result;

    std::vector<DISK_EXTENT> extents;
    if (!queryVolumeExtents(volume.get(), extents))
        return result;

    for (const DISK_EXTENT &extent : extents)
        if (std::find(result.numbers.begin(), result.numbers.end(), extent.DiskNumber) ==
                result.numbers.end())
            result.numbers.push_back(extent.DiskNumber);
    result.available = !result.numbers.empty();
    return result;
}

bool layoutContainsBootPartition(const DRIVE_LAYOUT_INFORMATION_EX *layout)
{
    static const GUID EfiSystemPartition = {
        0xc12a7328, 0xf81f, 0x11d2,
        { 0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b }
    };
    for (DWORD index = 0; index < layout->PartitionCount; index++)
    {
        const PARTITION_INFORMATION_EX &partition = layout->PartitionEntry[index];
        if (partition.PartitionStyle == PARTITION_STYLE_MBR && partition.Mbr.BootIndicator)
            return true;
        if (partition.PartitionStyle == PARTITION_STYLE_GPT &&
                (IsEqualGUID(partition.Gpt.PartitionType, EfiSystemPartition) ||
                 (partition.Gpt.Attributes & GPT_ATTRIBUTE_PLATFORM_REQUIRED) != 0))
            return true;
    }
    return false;
}

std::string descriptorString(const std::vector<unsigned char> &buffer, DWORD offset)
{
    if (offset == 0 || offset >= buffer.size())
        return std::string();
    const char *begin = reinterpret_cast<const char*>(buffer.data() + offset);
    const std::size_t remaining = buffer.size() - offset;
    return trimAscii(std::string(begin, strnlen(begin, remaining)));
}

std::string busTypeName(STORAGE_BUS_TYPE type)
{
    switch (type)
    {
        case BusTypeScsi: return "SCSI";
        case BusTypeAtapi: return "ATAPI";
        case BusTypeAta: return "ATA/SATA";
        case BusType1394: return "IEEE 1394";
        case BusTypeSsa: return "SSA";
        case BusTypeFibre: return "Fibre Channel";
        case BusTypeUsb: return "USB";
        case BusTypeRAID: return "RAID";
        case BusTypeiScsi: return "iSCSI";
        case BusTypeSas: return "SAS";
        case BusTypeSata: return "SATA";
        case BusTypeSd: return "SD";
        case BusTypeMmc: return "MMC";
        case BusTypeVirtual: return "Virtual";
        case BusTypeFileBackedVirtual: return "File-backed virtual";
        case BusTypeSpaces: return "Storage Spaces";
        case BusTypeNvme: return "NVMe";
        default: return "Unknown";
    }
}

Ps2::PhysicalDiskCandidate queryCandidate(HANDLE handle, std::uint32_t requestedDiskNumber,
        const std::string &path, const WindowsSystemDisks &systemDisks)
{
    Ps2::PhysicalDiskCandidate candidate;
    candidate.diskNumber = requestedDiskNumber;
    candidate.devicePath = path;

    DWORD returned = 0;
    STORAGE_DEVICE_NUMBER deviceNumber = {};
    if (!DeviceIoControl(handle, IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0, &deviceNumber,
            sizeof(deviceNumber), &returned, nullptr))
        throw std::runtime_error(windowsError("Querying the physical disk number"));
    if (deviceNumber.DeviceType != FILE_DEVICE_DISK ||
            deviceNumber.DeviceNumber != requestedDiskNumber)
        throw std::runtime_error("The physical disk number changed while it was being queried.");

    std::vector<unsigned char> geometryBuffer(sizeof(DISK_GEOMETRY_EX) + 1024, 0);
    if (!DeviceIoControl(handle, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, nullptr, 0,
            geometryBuffer.data(), static_cast<DWORD>(geometryBuffer.size()), &returned, nullptr))
        throw std::runtime_error(windowsError("Querying physical disk geometry"));
    const auto *geometry = reinterpret_cast<const DISK_GEOMETRY_EX*>(geometryBuffer.data());
    candidate.size = static_cast<std::uint64_t>(geometry->DiskSize.QuadPart);
    candidate.logicalSectorSize = geometry->Geometry.BytesPerSector;

    STORAGE_PROPERTY_QUERY propertyQuery = {};
    propertyQuery.PropertyId = StorageAccessAlignmentProperty;
    propertyQuery.QueryType = PropertyStandardQuery;
    STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR alignment = {};
    if (DeviceIoControl(handle, IOCTL_STORAGE_QUERY_PROPERTY, &propertyQuery,
            sizeof(propertyQuery), &alignment, sizeof(alignment), &returned, nullptr))
        candidate.physicalSectorSize = alignment.BytesPerPhysicalSector;
    if (candidate.physicalSectorSize == 0)
        candidate.physicalSectorSize = candidate.logicalSectorSize;

    propertyQuery.PropertyId = StorageDeviceProperty;
    std::vector<unsigned char> descriptorBuffer(64 * 1024, 0);
    if (DeviceIoControl(handle, IOCTL_STORAGE_QUERY_PROPERTY, &propertyQuery,
            sizeof(propertyQuery), descriptorBuffer.data(),
            static_cast<DWORD>(descriptorBuffer.size()), &returned, nullptr))
    {
        const auto *descriptor =
                reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(descriptorBuffer.data());
        const std::string vendor = descriptorString(descriptorBuffer, descriptor->VendorIdOffset);
        const std::string product = descriptorString(descriptorBuffer, descriptor->ProductIdOffset);
        candidate.model = trimAscii(vendor + (vendor.empty() || product.empty() ? "" : " ") + product);
        candidate.serialNumber = descriptorString(descriptorBuffer, descriptor->SerialNumberOffset);
        candidate.busType = busTypeName(descriptor->BusType);
    }
    if (candidate.model.empty())
        candidate.model = "Unknown disk";
    if (candidate.busType.empty())
        candidate.busType = "Unknown";

    std::vector<unsigned char> layoutBuffer(sizeof(DRIVE_LAYOUT_INFORMATION_EX) +
            sizeof(PARTITION_INFORMATION_EX) * 128, 0);
    if (!DeviceIoControl(handle, IOCTL_DISK_GET_DRIVE_LAYOUT_EX, nullptr, 0,
            layoutBuffer.data(), static_cast<DWORD>(layoutBuffer.size()), &returned, nullptr))
        throw std::runtime_error(windowsError("Querying the Windows partition layout"));
    const auto *layout = reinterpret_cast<const DRIVE_LAYOUT_INFORMATION_EX*>(layoutBuffer.data());
    candidate.partitionCount = layout->PartitionCount;
    candidate.rawPartitionStyle = layout->PartitionStyle == PARTITION_STYLE_RAW ||
            layout->PartitionCount == 0;
    candidate.systemDiskCheckAvailable = systemDisks.available;
    candidate.containsSystemVolume = std::find(systemDisks.numbers.begin(),
            systemDisks.numbers.end(), requestedDiskNumber) != systemDisks.numbers.end();
    candidate.containsBootPartition = layoutContainsBootPartition(layout);

    if (!DeviceIoControl(handle, IOCTL_DISK_IS_WRITABLE, nullptr, 0, nullptr, 0,
            &returned, nullptr) && GetLastError() == ERROR_WRITE_PROTECT)
        candidate.readOnly = true;
    return candidate;
}

#elif defined(__linux__)

namespace fs = std::filesystem;

std::string readTextFile(const fs::path &path)
{
    std::ifstream stream(path);
    if (!stream)
        return {};
    std::string value;
    std::getline(stream, value);
    return trimAscii(value);
}

std::uint64_t readUnsignedFile(const fs::path &path, std::uint64_t fallback = 0)
{
    const std::string value = readTextFile(path);
    if (value.empty())
        return fallback;
    try
    {
        return std::stoull(value);
    }
    catch (...)
    {
        return fallback;
    }
}

bool allDigits(const std::string &value)
{
    return !value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char character) {
        return std::isdigit(character) != 0;
    });
}

bool allLowercaseLetters(const std::string &value)
{
    return !value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char character) {
        return character >= 'a' && character <= 'z';
    });
}

bool isSupportedWholeDiskName(const std::string &name)
{
    if ((name.rfind("sd", 0) == 0 || name.rfind("hd", 0) == 0) &&
            allLowercaseLetters(name.substr(2)))
        return true;
    if (name.rfind("mmcblk", 0) == 0 && allDigits(name.substr(6)))
        return true;
    if (name.rfind("nvme", 0) == 0)
    {
        const std::size_t controllerEnd = name.find('n', 4);
        return controllerEnd != std::string::npos &&
                allDigits(name.substr(4, controllerEnd - 4)) &&
                allDigits(name.substr(controllerEnd + 1));
    }
    return false;
}

fs::path canonicalPath(const fs::path &path)
{
    std::error_code error;
    const fs::path result = fs::canonical(path, error);
    return error ? fs::path() : result;
}

void resolveBackingDisks(const fs::path &blockPath, std::set<std::string> &disks,
        std::set<std::string> &visited)
{
    const fs::path resolved = canonicalPath(blockPath);
    if (resolved.empty())
        return;
    const std::string resolvedText = resolved.string();
    if (!visited.insert(resolvedText).second)
        return;

    const fs::path slaves = resolved / "slaves";
    std::error_code error;
    bool foundSlave = false;
    if (fs::is_directory(slaves, error))
    {
        for (const fs::directory_entry &entry : fs::directory_iterator(slaves, error))
        {
            foundSlave = true;
            resolveBackingDisks(entry.path(), disks, visited);
        }
    }
    if (foundSlave)
        return;

    fs::path diskPath = resolved;
    if (fs::exists(resolved / "partition", error))
        diskPath = resolved.parent_path();
    const std::string name = diskPath.filename().string();
    if (isSupportedWholeDiskName(name))
        disks.insert(name);
}

std::set<std::string> linuxSystemDisks()
{
    std::set<std::string> result;
    std::ifstream mountInfo("/proc/self/mountinfo");
    std::string line;
    while (std::getline(mountInfo, line))
    {
        std::istringstream fields(line);
        std::string mountId;
        std::string parentId;
        std::string deviceNumber;
        std::string root;
        std::string mountPoint;
        if (!(fields >> mountId >> parentId >> deviceNumber >> root >> mountPoint))
            continue;
        if (mountPoint != "/" && mountPoint != "/boot" && mountPoint != "/boot/efi" &&
                mountPoint != "/usr" && mountPoint != "/var" && mountPoint != "/home")
            continue;

        const fs::path sysDevice = fs::path("/sys/dev/block") / deviceNumber;
        std::set<std::string> visited;
        resolveBackingDisks(sysDevice, result, visited);

        // Fedora commonly uses Btrfs and/or LUKS. In those cases mountinfo's
        // major:minor can name a virtual filesystem, while the source after
        // the " - " separator still identifies /dev/mapper/* or a partition.
        const std::size_t separator = line.find(" - ");
        if (separator != std::string::npos)
        {
            std::istringstream tail(line.substr(separator + 3));
            std::string filesystemType;
            std::string source;
            if (tail >> filesystemType >> source && source.rfind("/dev/", 0) == 0)
            {
                std::error_code sourceError;
                const fs::path resolvedSource = fs::canonical(source, sourceError);
                if (!sourceError)
                {
                    std::set<std::string> sourceVisited;
                    resolveBackingDisks(fs::path("/sys/class/block") /
                            resolvedSource.filename(), result, sourceVisited);
                }
            }
        }
    }
    return result;
}

std::uint32_t linuxPartitionCount(const fs::path &diskPath)
{
    const fs::path diskResolved = canonicalPath(diskPath);
    if (diskResolved.empty())
        return 0;

    std::uint32_t count = 0;
    std::error_code error;
    for (const fs::directory_entry &entry : fs::directory_iterator("/sys/class/block", error))
    {
        const fs::path resolved = canonicalPath(entry.path());
        if (!resolved.empty() && fs::exists(resolved / "partition", error) &&
                resolved.parent_path() == diskResolved)
            count++;
    }
    return count;
}

std::string linuxBusType(const std::string &name, const fs::path &diskPath)
{
    if (name.rfind("nvme", 0) == 0)
        return "NVMe";
    if (name.rfind("mmcblk", 0) == 0)
        return "MMC";
    const std::string resolved = canonicalPath(diskPath).string();
    if (resolved.find("/usb") != std::string::npos)
        return "USB";
    if (resolved.find("/ata") != std::string::npos)
        return "ATA/SATA";
    if (resolved.find("/virtual/") != std::string::npos)
        return "Virtual";
    return "SCSI/SATA";
}

Ps2::PhysicalDiskCandidate queryLinuxCandidate(const std::string &name,
        std::uint32_t displayNumber, const std::set<std::string> &systemDisks)
{
    const fs::path diskPath = fs::path("/sys/class/block") / name;
    Ps2::PhysicalDiskCandidate candidate;
    candidate.diskNumber = displayNumber;
    candidate.devicePath = "/dev/" + name;

    // Linux reports this value in 512-byte units regardless of logical block size.
    const std::uint64_t kernelSectors = readUnsignedFile(diskPath / "size");
    candidate.size = kernelSectors * 512ULL;
    candidate.logicalSectorSize = static_cast<std::uint32_t>(
            readUnsignedFile(diskPath / "queue/logical_block_size"));
    candidate.physicalSectorSize = static_cast<std::uint32_t>(
            readUnsignedFile(diskPath / "queue/physical_block_size",
                    candidate.logicalSectorSize));
    candidate.readOnly = readUnsignedFile(diskPath / "ro") != 0;
    candidate.partitionCount = linuxPartitionCount(diskPath);
    candidate.rawPartitionStyle = candidate.partitionCount == 0;
    candidate.busType = linuxBusType(name, diskPath);

    const std::string vendor = readTextFile(diskPath / "device/vendor");
    const std::string model = readTextFile(diskPath / "device/model");
    candidate.model = trimAscii(vendor + (vendor.empty() || model.empty() ? "" : " ") + model);
    if (candidate.model.empty())
        candidate.model = name;
    candidate.serialNumber = readTextFile(diskPath / "device/serial");

    candidate.systemDiskCheckAvailable = !systemDisks.empty();
    candidate.containsSystemVolume = systemDisks.find(name) != systemDisks.end();
    candidate.containsBootPartition = candidate.containsSystemVolume;
    if (candidate.size == 0 || candidate.logicalSectorSize == 0)
        candidate.inspectionError = "Linux sysfs did not report complete disk geometry.";
    return candidate;
}

#endif // platform helpers

} // namespace

namespace Ps2
{

std::vector<PhysicalDiskCandidate> PhysicalDiskScanner::Scan()
{
#ifdef _WIN32
    std::vector<PhysicalDiskCandidate> candidates;
    const WindowsSystemDisks systemDisks = queryWindowsSystemDisks();
    bool accessDenied = false;
    for (std::uint32_t diskNumber = 0; diskNumber < 64; diskNumber++)
    {
        std::wstringstream path;
        path << L"\\\\.\\PHYSICALDRIVE" << diskNumber;
        const std::string displayPath = "\\\\.\\PHYSICALDRIVE" + std::to_string(diskNumber);
        HandleGuard handle(CreateFileW(path.str().c_str(), GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL, nullptr));
        if (handle.get() == INVALID_HANDLE_VALUE)
        {
            if (GetLastError() == ERROR_ACCESS_DENIED)
                accessDenied = true;
            continue;
        }

        try
        {
            candidates.push_back(queryCandidate(handle.get(), diskNumber, displayPath, systemDisks));
        }
        catch (const std::exception &error)
        {
            PhysicalDiskCandidate rejected;
            rejected.diskNumber = diskNumber;
            rejected.devicePath = displayPath;
            rejected.model = "Inspection failed";
            rejected.busType = "Unknown";
            rejected.inspectionError = error.what();
            candidates.push_back(std::move(rejected));
        }
    }
    if (candidates.empty() && accessDenied)
        throw std::runtime_error("Administrator access is required to inspect physical disks.");
    return candidates;
#elif defined(__linux__)
    std::vector<std::string> names;
    std::error_code error;
    for (const fs::directory_entry &entry : fs::directory_iterator("/sys/class/block", error))
    {
        const std::string name = entry.path().filename().string();
        if (isSupportedWholeDiskName(name))
            names.push_back(name);
    }
    if (error)
        throw std::runtime_error("Enumerating /sys/class/block failed: " + error.message());
    std::sort(names.begin(), names.end());

    const std::set<std::string> systemDisks = linuxSystemDisks();
    std::vector<PhysicalDiskCandidate> candidates;
    candidates.reserve(names.size());
    for (std::size_t index = 0; index < names.size(); index++)
        candidates.push_back(queryLinuxCandidate(names[index],
                static_cast<std::uint32_t>(index), systemDisks));
    return candidates;
#else
    return {};
#endif
}

} // namespace Ps2
