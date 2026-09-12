#ifndef PS2HDDFORMAT_H
#define PS2HDDFORMAT_H

#include <cstdint>
#include <string>
#include <vector>

namespace Ps2
{

struct PayloadStatus
{
    bool valid = false;
    std::string message;
    std::vector<std::string> missingFiles;
};

struct ProvisioningSelection
{
    bool installOpl = false;
    bool installWlaunchElf = false;
    bool installFhdb = false;
    bool installHddBootEnabler = false;
    bool installMemoryCardAnnihilator = false;
    bool installFceumm = false;
    bool configureOplPlugAndPlay = false;
    bool installHddIgrReturn = false;

    bool any() const
    {
        return installOpl ||
                installWlaunchElf ||
                installFhdb ||
                installHddBootEnabler ||
                installMemoryCardAnnihilator ||
                installFceumm ||
                configureOplPlugAndPlay ||
                installHddIgrReturn;
    }

    bool needsAppsPartition() const
    {
        return installOpl ||
                installWlaunchElf ||
                installHddBootEnabler ||
                installMemoryCardAnnihilator ||
                installFceumm ||
                configureOplPlugAndPlay ||
                installHddIgrReturn;
    }
};

class Ps2HddFormat
{
public:
    static constexpr std::uint64_t MbrPayloadStartSector = 0x2000ULL;

    static constexpr std::uint32_t MinimumAppsSizeMiB = 128U;
    static constexpr std::uint32_t DefaultAppsSizeMiB = 4096U;
    static constexpr std::uint32_t MaximumAppsSizeMiB = 65536U;

    static PayloadStatus ValidateProvisionPayload(
            const std::string &payloadDirectory,
            const ProvisioningSelection &selection);

    static std::string BuildStandardFormatScript(
            const std::string &devicePath);

    static std::string BuildStandardVerifyScript(
            const std::string &devicePath);

    static void VerifyStandardApaDisk(
            const std::string &targetPath,
            std::uint64_t diskSizeBytes);

    static std::string BuildProvisionScript(
            const std::string &devicePath,
            const std::string &stagingDirectory,
            const ProvisioningSelection &selection,
            std::uint32_t appsSizeMiB = DefaultAppsSizeMiB);

    static std::string BuildProvisionVerifyScript(
            const std::string &devicePath,
            const ProvisioningSelection &selection);

    static void InstallMbrPayload(
            const std::string &targetPath,
            const std::string &mbrPayloadPath);
};

} // namespace Ps2

#endif // PS2HDDFORMAT_H
