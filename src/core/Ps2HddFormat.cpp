#include "Ps2HddFormat.h"

#include "Ps2Apa.h"
#include "Ps2HddLayout.h"
#include "OplConfig.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#elif defined(__linux__)
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace
{

void writeLittleEndian32(unsigned char *destination, std::uint32_t value)
{
    destination[0] = static_cast<unsigned char>(value);
    destination[1] = static_cast<unsigned char>(value >> 8);
    destination[2] = static_cast<unsigned char>(value >> 16);
    destination[3] = static_cast<unsigned char>(value >> 24);
}

std::vector<unsigned char> readFile(const std::string &path)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        throw std::runtime_error("Unable to open payload: " + path);
    stream.seekg(0, std::ios::end);
    const std::streamoff length = stream.tellg();
    if (length < 0)
        throw std::runtime_error("Unable to determine payload length: " + path);
    stream.seekg(0, std::ios::beg);
    std::vector<unsigned char> data(static_cast<std::size_t>(length));
    if (!data.empty() && !stream.read(reinterpret_cast<char*>(data.data()), length))
        throw std::runtime_error("Unable to read payload: " + path);
    return data;
}

void requirePfsshellPath(const std::string &value, const char *description)
{
    if (value.empty() || value.find_first_of("\n\r\t ") != std::string::npos)
        throw std::invalid_argument(std::string("Invalid ") + description + " for pfsshell.");
}

#ifdef __linux__
void writeAllAt(int descriptor, const unsigned char *data, std::size_t size, std::uint64_t offset,
        const std::string &path)
{
    std::size_t completed = 0;
    while (completed < size)
    {
        const ssize_t written = pwrite(descriptor, data + completed, size - completed,
                static_cast<off_t>(offset + completed));
        if (written < 0)
            throw std::runtime_error("Writing " + path + " failed: " + std::strerror(errno));
        if (written == 0)
            throw std::runtime_error("Writing " + path + " returned a zero-length write.");
        completed += static_cast<std::size_t>(written);
    }
}

void readAllAt(int descriptor, unsigned char *data, std::size_t size, std::uint64_t offset,
        const std::string &path)
{
    std::size_t completed = 0;
    while (completed < size)
    {
        const ssize_t received = pread(descriptor, data + completed, size - completed,
                static_cast<off_t>(offset + completed));
        if (received < 0)
            throw std::runtime_error("Reading " + path + " failed: " + std::strerror(errno));
        if (received == 0)
            throw std::runtime_error("Reading " + path + " returned an incomplete block.");
        completed += static_cast<std::size_t>(received);
    }
}
#endif

} // namespace

namespace Ps2
{

PayloadStatus Ps2HddFormat::ValidateProvisionPayload(const std::string &payloadDirectory,
        const ProvisioningSelection &selection)
{
    PayloadStatus result;
    if (!selection.any())
    {
        result.valid = true;
        result.message = "No optional payload was selected.";
        return result;
    }
    if (payloadDirectory.empty())
    {
        result.message = "Provisioning payload directory is not configured.";
        return result;
    }

    const fs::path root(payloadDirectory);
    std::error_code error;
    if (!fs::is_directory(root, error))
    {
        result.message = "Provisioning payload directory does not exist: " + payloadDirectory;
        return result;
    }

    std::vector<fs::path> required;
    if (selection.installOpl)
        required.emplace_back("opl/OPNPS2LD.ELF");
    if (selection.installWlaunchElf)
        required.emplace_back("wle/BOOT.ELF");
    if (selection.installFhdb)
    {
        for (const char *file : { "MBR.XLF", "FHDB.XLF", "ENDVDPL.XRX", "FMCB_CFG.ELF",
                "USBD.IRX", "USBHDFSD.IRX" })
            required.emplace_back(fs::path("fhdb") / file);
    }
    if (selection.installHddBootEnabler)
        required.emplace_back("fhdb-enabler/FHDB-Boot-Config.ELF");
    if (selection.installMemoryCardAnnihilator)
        required.emplace_back("mca/BOOT.ELF");

    for (const fs::path &relative : required)
    {
        const fs::path path = root / relative;
        if (!fs::is_regular_file(path, error) || fs::file_size(path, error) == 0)
            result.missingFiles.push_back(relative.generic_string());
    }
    if (!result.missingFiles.empty())
    {
        result.message = "One or more selected provisioning payloads are missing.";
        return result;
    }

    result.valid = true;
    result.message = "Selected OPL/FHDB utility payloads are ready.";
    return result;
}

std::string Ps2HddFormat::BuildStandardFormatScript(const std::string &devicePath)
{
    requirePfsshellPath(devicePath, "device path");
    return "device " + devicePath + "\ninitialize yes\nexit\n";
}

std::string Ps2HddFormat::BuildStandardVerifyScript(const std::string &devicePath)
{
    requirePfsshellPath(devicePath, "device path");

    std::string script = "device " + devicePath + "\n";
    for (const char *partition : { "__net", "__system", "__sysconf", "__common" })
    {
        script += "mount ";
        script += partition;
        script += "\nls\numount\n";
    }
    script += "exit\n";
    return script;
}

void Ps2HddFormat::VerifyStandardApaDisk(const std::string &targetPath,
        std::uint64_t diskSizeBytes)
{
    if (diskSizeBytes == 0 || (diskSizeBytes % HddLayoutPlanner::SectorSize) != 0)
        throw std::invalid_argument("Standard APA verification requires a sector-aligned disk size.");

    const std::uint64_t sectors = diskSizeBytes / HddLayoutPlanner::SectorSize;
    if (sectors > HddLayoutPlanner::BankBoundarySectors)
        throw std::runtime_error("Standard APA verification refuses disks larger than one 32-bit APA bank.");

    const std::vector<ApaPartitionProbe> chain = Apa::ReadPartitionChain(targetPath, 0, sectors, 256);
    if (chain.size() < 5)
        throw std::runtime_error("Fresh APA layout is missing required system partitions.");

    static constexpr const char *required[] = { "__mbr", "__net", "__system", "__sysconf", "__common" };
    for (const char *name : required)
    {
        const auto found = std::find_if(chain.begin(), chain.end(), [name](const ApaPartitionProbe &probe) {
            return probe.header.id == name;
        });
        if (found == chain.end())
            throw std::runtime_error(std::string("Fresh APA layout is missing required partition ") + name + ".");
    }
}

std::string Ps2HddFormat::BuildProvisionScript(const std::string &devicePath,
        const std::string &stagingDirectory, const ProvisioningSelection &selection)
{
    requirePfsshellPath(devicePath, "device path");
    requirePfsshellPath(stagingDirectory, "staging path");
    if (!selection.any())
        return "device " + devicePath + "\nexit\n";

    std::string script = "device " + devicePath + "\n";

    if (selection.needsAppsPartition())
    {
        // One 128 MiB PFS partition is both an FHDB-friendly application location and,
        // via __common/OPL/conf_hdd.cfg, OPL's writable HDD data partition.
        script += "mkpart PP.FHDB.APPS 128M PFS\n";

        script += "mount __common\nmkdir OPL\ncd OPL\nlcd " + stagingDirectory +
                "\nput conf_hdd.cfg\ncd /\numount\n";

        script += "mount PP.FHDB.APPS\nmkdir OPL\ncd OPL\n";
        for (const char *folder : { "APPS", "ART", "CFG", "CHT", "LNG", "THM", "VMC" })
        {
            script += "mkdir ";
            script += folder;
            script += "\n";
        }

        script += "lcd " + stagingDirectory + "\n";
        if (selection.installOpl)
            script += "put OPNPS2LD.ELF\n";
        if (selection.configureOplPlugAndPlay)
            script += "put conf_opl.cfg\n";

        if (selection.installWlaunchElf)
        {
            script += "cd APPS\nmkdir wLaunchELF\ncd wLaunchELF\nlcd " + stagingDirectory +
                    "\nput WLE_BOOT.ELF\nrename WLE_BOOT.ELF BOOT.ELF\n"
                    "put title_wle.cfg\nrename title_wle.cfg title.cfg\ncd /OPL\n";
        }

        if (selection.installMemoryCardAnnihilator)
        {
            script += "cd APPS\nmkdir Memory-Card-Annihilator\ncd Memory-Card-Annihilator\nlcd " + stagingDirectory +
                    "\nput MCA_BOOT.ELF\nrename MCA_BOOT.ELF BOOT.ELF\n"
                    "put title_mca.cfg\nrename title_mca.cfg title.cfg\ncd /OPL\n";
        }

        if (selection.installHddBootEnabler)
        {
            script += "cd APPS\nmkdir FHDB-HDD-Boot-Config\ncd FHDB-HDD-Boot-Config\nlcd " + stagingDirectory +
                    "\nput FHDB_BOOT_CONFIG.ELF\nrename FHDB_BOOT_CONFIG.ELF BOOT.ELF\n"
                    "put title_fhdb_enabler.cfg\nrename title_fhdb_enabler.cfg title.cfg\ncd /OPL\n";
        }
        script += "cd /\numount\n";
    }

    if (selection.installFhdb)
    {
        script += "mount __system\nmkdir osd\ncd osd\nlcd " + stagingDirectory +
                "\nput FHDB.XLF\nrename FHDB.XLF osdmain.elf\ncd /\numount\n";

        script += "mount __sysconf\nmkdir FMCB\ncd FMCB\nlcd " + stagingDirectory +
                "\nput FREEHDB.CNF\nput FMCB_CFG.ELF\nput USBD.IRX\nput USBHDFSD.IRX\n"
                "put ENDVDPL.XRX\nrename ENDVDPL.XRX endvdpl.irx\n";
        if (selection.installWlaunchElf)
            script += "put WLE_BOOT.ELF\nrename WLE_BOOT.ELF BOOT.ELF\n";
        script += "cd /\numount\n";
    }

    script += "exit\n";
    return script;
}

std::string Ps2HddFormat::BuildProvisionVerifyScript(const std::string &devicePath,
        const ProvisioningSelection &selection)
{
    requirePfsshellPath(devicePath, "device path");
    std::string script = "device " + devicePath + "\n";
    if (selection.needsAppsPartition())
    {
        script += "mount __common\ncd OPL\nls\ncd /\numount\n";
        script += "mount PP.FHDB.APPS\ncd OPL\nls\ncd APPS\nls\ncd /\numount\n";
    }
    if (selection.installFhdb)
    {
        script += "mount __system\ncd osd\nls\ncd /\numount\n";
        script += "mount __sysconf\ncd FMCB\nls\ncd /\numount\n";
    }
    script += "exit\n";
    return script;
}

void Ps2HddFormat::InstallMbrPayload(const std::string &targetPath,
        const std::string &mbrPayloadPath)
{
    const std::vector<unsigned char> payload = readFile(mbrPayloadPath);
    if (payload.empty())
        throw std::runtime_error("FHDB MBR payload is empty.");

    const std::uint64_t payloadSectors = (payload.size() + HddLayoutPlanner::SectorSize - 1) /
            HddLayoutPlanner::SectorSize;
    if (payloadSectors > std::numeric_limits<std::uint32_t>::max())
        throw std::runtime_error("FHDB MBR payload is too large.");

#ifdef __linux__
    const int descriptor = open(targetPath.c_str(), O_RDWR | O_CLOEXEC | O_SYNC);
    if (descriptor < 0)
        throw std::runtime_error("Opening " + targetPath + " for FHDB MBR installation failed: " +
                std::strerror(errno));
    try
    {
        std::array<unsigned char, Apa::HeaderSize> header = {};
        readAllAt(descriptor, header.data(), header.size(), 0, targetPath);
        const ApaHeaderInfo parsed = Apa::ParseHeader(header.data(), header.size(), true);
        if (parsed.state != ApaHeaderState::Valid)
            throw std::runtime_error("Refusing FHDB MBR installation because __mbr is invalid: " +
                    parsed.message);
        if (parsed.length <= MbrPayloadStartSector ||
                payloadSectors > parsed.length - MbrPayloadStartSector)
            throw std::runtime_error("FHDB MBR payload does not fit inside the __mbr partition.");

        std::vector<unsigned char> padded(static_cast<std::size_t>(payloadSectors) *
                HddLayoutPlanner::SectorSize, 0);
        std::copy(payload.begin(), payload.end(), padded.begin());
        const std::uint64_t byteOffset = MbrPayloadStartSector * HddLayoutPlanner::SectorSize;
        writeAllAt(descriptor, padded.data(), padded.size(), byteOffset, targetPath);

        writeLittleEndian32(header.data() + 304, static_cast<std::uint32_t>(MbrPayloadStartSector));
        writeLittleEndian32(header.data() + 308, static_cast<std::uint32_t>(payloadSectors));
        writeLittleEndian32(header.data(), 0);
        writeLittleEndian32(header.data(), Apa::CalculateChecksum(header.data(), header.size()));
        writeAllAt(descriptor, header.data(), header.size(), 0, targetPath);
        if (fsync(descriptor) != 0)
            throw std::runtime_error("fsync failed after FHDB MBR installation: " +
                    std::string(std::strerror(errno)));

        std::array<unsigned char, Apa::HeaderSize> verify = {};
        readAllAt(descriptor, verify.data(), verify.size(), 0, targetPath);
        const ApaHeaderInfo verified = Apa::ParseHeader(verify.data(), verify.size(), true);
        if (verified.state != ApaHeaderState::Valid || verified.osdStart != MbrPayloadStartSector ||
                verified.osdSize != payloadSectors)
            throw std::runtime_error("FHDB MBR verification failed after writing.");
    }
    catch (...)
    {
        close(descriptor);
        throw;
    }
    close(descriptor);
#elif defined(_WIN32)
    (void)targetPath;
    throw std::runtime_error("FHDB MBR installation is currently enabled only on Linux.");
#else
    (void)targetPath;
    throw std::runtime_error("FHDB MBR installation is not available on this platform.");
#endif
}

} // namespace Ps2
