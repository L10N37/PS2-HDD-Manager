#include "core/Ps2Apa.h"
#include "core/Ps2ApaBank.h"
#include "core/FhdbConfig.h"
#include "core/PhysicalDisk.h"
#include "core/Ps2HddLayout.h"
#include "core/Ps2HddFormat.h"
#include "core/OplConfig.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef __linux__
#include <fcntl.h>
#include <unistd.h>
#endif

namespace
{

[[noreturn]] void failTestCheck(const char *expression, const char *file, int line)
{
    throw std::runtime_error(std::string(file) + ":" + std::to_string(line) +
            ": test check failed: " + expression);
}

#define TEST_CHECK(expression) \
    do \
    { \
        if (!(expression)) \
            failTestCheck(#expression, __FILE__, __LINE__); \
    } while (false)

#ifdef __linux__
class TemporaryProbeFile
{
public:
    TemporaryProbeFile(int descriptor, const char *path)
        : descriptor_(descriptor), path_(path)
    {
    }

    ~TemporaryProbeFile()
    {
        if (descriptor_ >= 0)
            ::close(descriptor_);
        ::unlink(path_.c_str());
    }

    int closeFile()
    {
        const int result = ::close(descriptor_);
        descriptor_ = -1;
        return result;
    }

private:
    int descriptor_;
    std::string path_;
};
#endif

void writeLe16(unsigned char *destination, std::uint16_t value)
{
    destination[0] = static_cast<unsigned char>(value);
    destination[1] = static_cast<unsigned char>(value >> 8);
}

void writeLe32(unsigned char *destination, std::uint32_t value)
{
    destination[0] = static_cast<unsigned char>(value);
    destination[1] = static_cast<unsigned char>(value >> 8);
    destination[2] = static_cast<unsigned char>(value >> 16);
    destination[3] = static_cast<unsigned char>(value >> 24);
}

std::array<unsigned char, Ps2::Apa::HeaderSize> makeMbr()
{
    std::array<unsigned char, Ps2::Apa::HeaderSize> header = {};
    writeLe32(header.data() + 4, Ps2::Apa::Magic);
    std::memcpy(header.data() + 16, "__mbr", 5);
    writeLe32(header.data() + 68, 0x40000);
    writeLe16(header.data() + 72, Ps2::Apa::MbrPartitionType);
    writeLe32(header.data() + 96, 0x201);
    std::memcpy(header.data() + 256, "Sony Computer Entertainment Inc.", 32);
    writeLe32(header.data() + 288, 2);
    writeLe32(header.data(), Ps2::Apa::CalculateChecksum(header.data(), header.size()));
    return header;
}


std::array<unsigned char, Ps2::Apa::HeaderSize> makePartition(const char *id,
        std::uint32_t start, std::uint32_t length, std::uint32_t next)
{
    std::array<unsigned char, Ps2::Apa::HeaderSize> header = {};
    writeLe32(header.data() + 4, Ps2::Apa::Magic);
    std::strncpy(reinterpret_cast<char*>(header.data() + 16), id, 31);
    writeLe32(header.data() + 8, next);
    writeLe32(header.data() + 64, start);
    writeLe32(header.data() + 68, length);
    writeLe16(header.data() + 72, 0x0100);
    writeLe32(header.data() + 96, 0x201);
    writeLe32(header.data(), Ps2::Apa::CalculateChecksum(header.data(), header.size()));
    return header;
}

void testStandardLayout()
{
    const std::uint64_t oneTiB = 1ULL << 40;
    const Ps2::HddLayout layout = Ps2::HddLayoutPlanner::Plan(oneTiB,
            Ps2::HddLayoutMode::StandardApa);
    TEST_CHECK(layout.valid);
    TEST_CHECK(layout.banks.size() == 1);
    TEST_CHECK(layout.banks[0].baseSector == 0);
    TEST_CHECK(layout.banks[0].addressableSectorCount == oneTiB / 512);
    TEST_CHECK(!layout.requiresExtendedOpl);
    TEST_CHECK(layout.unaddressedSectorCount == 0);
}

void testFourTbExtendedLayout()
{
    constexpr std::uint64_t fourTb = 4000000000000ULL;
    const Ps2::HddLayout layout = Ps2::HddLayoutPlanner::Plan(fourTb,
            Ps2::HddLayoutMode::ExtendedApaBanks);
    TEST_CHECK(layout.valid);
    TEST_CHECK(layout.banks.size() == 2);
    TEST_CHECK(layout.banks[0].baseSector == 0);
    TEST_CHECK(layout.banks[0].addressableSectorCount == 0xFFFFFFFFULL);
    TEST_CHECK(layout.banks[1].baseSector == 0x100000000ULL);
    TEST_CHECK(layout.banks[1].addressableSectorCount == 3517532704ULL);
    TEST_CHECK(layout.banks[0].role == Ps2::ApaBankRole::BootSystemAndGames);
    TEST_CHECK(layout.banks[1].role == Ps2::ApaBankRole::GamesOnly);
    TEST_CHECK(layout.requiresExtendedOpl);
    TEST_CHECK(layout.unaddressedSectorCount == 1);
}

void testFourTbStandardLayoutLeavesRemainderUnused()
{
    constexpr std::uint64_t fourTb = 4000000000000ULL;
    const Ps2::HddLayout layout = Ps2::HddLayoutPlanner::Plan(fourTb,
            Ps2::HddLayoutMode::StandardApa);
    TEST_CHECK(layout.valid);
    TEST_CHECK(layout.banks.size() == 1);
    TEST_CHECK(layout.unaddressedSectorCount == 3517532705ULL);
}

void testApaMbrParser()
{
    std::array<unsigned char, Ps2::Apa::HeaderSize> header = makeMbr();
    Ps2::ApaHeaderInfo parsed = Ps2::Apa::ParseHeader(header.data(), header.size(), true);
    TEST_CHECK(parsed.state == Ps2::ApaHeaderState::Valid);
    TEST_CHECK(parsed.id == "__mbr");
    TEST_CHECK(parsed.length == 0x40000);
    TEST_CHECK(parsed.type == Ps2::Apa::MbrPartitionType);
    TEST_CHECK(parsed.mbrVersion == 2);

    header[100] ^= 0x80;
    parsed = Ps2::Apa::ParseHeader(header.data(), header.size(), true);
    TEST_CHECK(parsed.state == Ps2::ApaHeaderState::BadChecksum);
}

void testApaMbrValidation()
{
    std::array<unsigned char, Ps2::Apa::HeaderSize> header = makeMbr();
    std::memcpy(header.data() + 16, "+OPL", 4);
    writeLe16(header.data() + 72, 0x100);
    writeLe32(header.data(), Ps2::Apa::CalculateChecksum(header.data(), header.size()));
    const Ps2::ApaHeaderInfo parsed = Ps2::Apa::ParseHeader(header.data(), header.size(), true);
    TEST_CHECK(parsed.state == Ps2::ApaHeaderState::InvalidMbr);
}

void testFhdbConfigRoundTripAndMenuEditing()
{
    const std::string original =
            "# keep this comment\r\n"
            "FastBoot = 1\r\n"
            "custom_setting = untouched\r\n"
            "name_OSDSYS_ITEM_1 = Old OPL\r\n"
            "path1_OSDSYS_ITEM_1 = mass:/OPL.ELF\r\n";
    Ps2::FhdbConfig config = Ps2::FhdbConfig::Parse(original);
    TEST_CHECK(config.Serialize() == original);
    TEST_CHECK(config.Value("custom_setting") == "untouched");

    std::vector<Ps2::FhdbMenuItem> items = config.MenuItems();
    TEST_CHECK(items.size() == 1);
    TEST_CHECK(items[0].index == 1);
    items[0].name = "Open PS2 Loader";
    items[0].path1 = "hdd0:PP.FHDB.APPS:pfs:/OPL/OPNPS2LD.ELF";
    items.push_back({ 2, "wLaunchELF", "hdd0:__sysconf:pfs:/FMCB/BOOT.ELF", "", "" });
    config.SetMenuItems(items);

    const std::string saved = config.Serialize();
    TEST_CHECK(saved.find("# keep this comment\r\n") != std::string::npos);
    TEST_CHECK(saved.find("custom_setting = untouched\r\n") != std::string::npos);
    TEST_CHECK(saved.find("name_OSDSYS_ITEM_1 = Open PS2 Loader\r\n") != std::string::npos);
    TEST_CHECK(saved.find("name_OSDSYS_ITEM_2 = wLaunchELF\r\n") != std::string::npos);

    config.SetMenuItems(items);
    const std::string savedAgain = config.Serialize();
    const std::size_t firstHeader = savedAgain.find("# FHDB menu entries");
    TEST_CHECK(firstHeader != std::string::npos);
    TEST_CHECK(savedAgain.find("# FHDB menu entries", firstHeader + 1) == std::string::npos);
}

void testPhysicalDiskEnumeration()
{
    const std::vector<Ps2::PhysicalDiskCandidate> disks = Ps2::PhysicalDiskScanner::Scan();
    std::set<std::string> paths;
    for (const Ps2::PhysicalDiskCandidate &disk : disks)
    {
#ifdef __linux__
        TEST_CHECK(disk.devicePath.rfind("/dev/", 0) == 0);
#endif
        TEST_CHECK(paths.insert(disk.devicePath).second);
        if (disk.inspectionError.empty())
        {
            TEST_CHECK(disk.size != 0);
            TEST_CHECK(disk.logicalSectorSize != 0);
        }
    }
}

void testOplPlugAndPlayPreset()
{
    Ps2::OplPresetOptions options;
    options.enableApps = true;
    const std::string cfg = Ps2::OplConfig::BuildInternalHddPreset(options);
    TEST_CHECK(cfg.find("hdd_mode=2\n") != std::string::npos);
    TEST_CHECK(cfg.find("default_device=6\n") != std::string::npos);
    TEST_CHECK(cfg.find("app_mode=2\n") != std::string::npos);
    TEST_CHECK(cfg.find("usb_mode=0\n") != std::string::npos);
    TEST_CHECK(cfg.find("eth_mode=0\n") != std::string::npos);
    TEST_CHECK(cfg.find("enable_coverart=1\n") != std::string::npos);
    TEST_CHECK(cfg.find("enable_delete_rename=1\n") != std::string::npos);
    TEST_CHECK(cfg.find("hdd_game_list_cache=1\n") != std::string::npos);
    TEST_CHECK(cfg.find("autorefresh=1\n") != std::string::npos);
    TEST_CHECK(cfg.find("autosort=1\n") != std::string::npos);
    // A direct HDD/PFS IGR target is intentionally not generated by default:
    // OPL's EE IGR loader does not initialize HDD/PFS before LoadElf().
    TEST_CHECK(cfg.find("exit_path=") == std::string::npos);

    options.exitPath = "mc0:/BOOT/BOOT.ELF";
    const std::string explicitCfg = Ps2::OplConfig::BuildInternalHddPreset(options);
    TEST_CHECK(explicitCfg.find("exit_path=mc0:/BOOT/BOOT.ELF\n") != std::string::npos);
}

void testOplConfigSetKeyPreservesOtherSettings()
{
    const std::string original =
            "# custom user config\n"
            "hdd_mode=1\n"
            "enable_coverart=0\n"
            "theme=my-theme\n"
            "enable_coverart=0\n";
    const std::string patched = Ps2::OplConfig::SetKey(original, "enable_coverart", "1");
    TEST_CHECK(patched.find("# custom user config\n") != std::string::npos);
    TEST_CHECK(patched.find("hdd_mode=1\n") != std::string::npos);
    TEST_CHECK(patched.find("theme=my-theme\n") != std::string::npos);
    TEST_CHECK(patched.find("enable_coverart=1\n") != std::string::npos);
    TEST_CHECK(patched.find("enable_coverart=0") == std::string::npos);
    TEST_CHECK(patched.find("enable_coverart=1\n") == patched.rfind("enable_coverart=1\n"));

    const std::string appended = Ps2::OplConfig::SetKey("autosort=0\n", "enable_coverart", "1");
    TEST_CHECK(appended == "autosort=0\nenable_coverart=1\n");
}

#ifdef __linux__
void testLinuxHighLbaApaProbe()
{
    char path[] = "/tmp/ps2-hdd-apa-probe-XXXXXX";
    const int descriptor = mkstemp(path);
    TEST_CHECK(descriptor >= 0);
    TemporaryProbeFile temporaryFile(descriptor, path);
    const std::uint64_t secondBankOffset = Ps2::HddLayoutPlanner::BankBoundarySectors *
            Ps2::HddLayoutPlanner::SectorSize;
    const std::uint64_t imageSize = secondBankOffset + Ps2::Apa::HeaderSize;
    const int truncateResult = ftruncate(descriptor, static_cast<off_t>(imageSize));
    TEST_CHECK(truncateResult == 0);

    const std::array<unsigned char, Ps2::Apa::HeaderSize> header = makeMbr();
    const ssize_t firstWrite = pwrite(descriptor, header.data(), header.size(), 0);
    TEST_CHECK(firstWrite == static_cast<ssize_t>(header.size()));
    const ssize_t secondWrite = pwrite(descriptor, header.data(), header.size(),
            static_cast<off_t>(secondBankOffset));
    TEST_CHECK(secondWrite == static_cast<ssize_t>(header.size()));
    const int closeResult = temporaryFile.closeFile();
    TEST_CHECK(closeResult == 0);

    const std::vector<Ps2::ApaBankProbe> probes = Ps2::Apa::ProbePhysicalDrive(path,
            imageSize, 2);
    TEST_CHECK(probes.size() == 2);
    TEST_CHECK(probes[0].baseSector == 0);
    TEST_CHECK(probes[1].baseSector == Ps2::HddLayoutPlanner::BankBoundarySectors);
    TEST_CHECK(probes[0].header.state == Ps2::ApaHeaderState::Valid);
    TEST_CHECK(probes[1].header.state == Ps2::ApaHeaderState::Valid);
}
#endif

#ifdef __linux__
void testFhdbMbrImageInstallation()
{
    char imagePath[] = "/tmp/ps2-hdd-fhdb-image-XXXXXX";
    const int imageDescriptor = mkstemp(imagePath);
    TEST_CHECK(imageDescriptor >= 0);
    TemporaryProbeFile image(imageDescriptor, imagePath);

    const std::uint64_t imageSize = (Ps2::Ps2HddFormat::MbrPayloadStartSector + 8) * 512ULL;
    TEST_CHECK(ftruncate(imageDescriptor, static_cast<off_t>(imageSize)) == 0);
    const std::array<unsigned char, Ps2::Apa::HeaderSize> header = makeMbr();
    TEST_CHECK(pwrite(imageDescriptor, header.data(), header.size(), 0) ==
            static_cast<ssize_t>(header.size()));
    TEST_CHECK(image.closeFile() == 0);

    char payloadPath[] = "/tmp/ps2-hdd-fhdb-payload-XXXXXX";
    const int payloadDescriptor = mkstemp(payloadPath);
    TEST_CHECK(payloadDescriptor >= 0);
    TemporaryProbeFile payloadFile(payloadDescriptor, payloadPath);
    std::vector<unsigned char> payload(777);
    for (std::size_t index = 0; index < payload.size(); index++)
        payload[index] = static_cast<unsigned char>((index * 13U) & 0xFFU);
    TEST_CHECK(write(payloadDescriptor, payload.data(), payload.size()) ==
            static_cast<ssize_t>(payload.size()));
    TEST_CHECK(payloadFile.closeFile() == 0);

    Ps2::Ps2HddFormat::InstallMbrPayload(imagePath, payloadPath);

    const int verifyDescriptor = open(imagePath, O_RDONLY | O_CLOEXEC);
    TEST_CHECK(verifyDescriptor >= 0);
    std::array<unsigned char, Ps2::Apa::HeaderSize> verifyHeader = {};
    TEST_CHECK(pread(verifyDescriptor, verifyHeader.data(), verifyHeader.size(), 0) ==
            static_cast<ssize_t>(verifyHeader.size()));
    const Ps2::ApaHeaderInfo parsed = Ps2::Apa::ParseHeader(verifyHeader.data(), verifyHeader.size(), true);
    TEST_CHECK(parsed.state == Ps2::ApaHeaderState::Valid);
    TEST_CHECK(parsed.osdStart == Ps2::Ps2HddFormat::MbrPayloadStartSector);
    TEST_CHECK(parsed.osdSize == 2);

    std::vector<unsigned char> verifyPayload(1024, 0xFF);
    TEST_CHECK(pread(verifyDescriptor, verifyPayload.data(), verifyPayload.size(),
            static_cast<off_t>(Ps2::Ps2HddFormat::MbrPayloadStartSector * 512ULL)) ==
            static_cast<ssize_t>(verifyPayload.size()));
    TEST_CHECK(std::equal(payload.begin(), payload.end(), verifyPayload.begin()));
    TEST_CHECK(std::all_of(verifyPayload.begin() + static_cast<std::ptrdiff_t>(payload.size()),
            verifyPayload.end(), [](unsigned char value) { return value == 0; }));
    close(verifyDescriptor);
}

void testStandardPfsshellScripts()
{
    const std::string formatScript = Ps2::Ps2HddFormat::BuildStandardFormatScript("/dev/sdz");
    TEST_CHECK(formatScript == "device /dev/sdz\ninitialize yes\nexit\n");

    const std::string verifyScript = Ps2::Ps2HddFormat::BuildStandardVerifyScript("/dev/sdz");
    TEST_CHECK(verifyScript.find("mount __net\n") != std::string::npos);
    TEST_CHECK(verifyScript.find("mount __system\n") != std::string::npos);
    TEST_CHECK(verifyScript.find("mount __sysconf\n") != std::string::npos);
    TEST_CHECK(verifyScript.find("mount __common\n") != std::string::npos);
}

void testProvisionPfsshellScripts()
{
    Ps2::ProvisioningSelection selection;
    selection.installOpl = true;
    selection.installWlaunchElf = true;
    selection.installFhdb = true;
    selection.installHddBootEnabler = true;
    selection.installMemoryCardAnnihilator = true;
    selection.configureOplPlugAndPlay = true;

    const std::string script = Ps2::Ps2HddFormat::BuildProvisionScript(
            "/dev/sdz", "/tmp/stage", selection);
    TEST_CHECK(script.find("mkpart PP.FHDB.APPS 128M PFS\n") != std::string::npos);
    TEST_CHECK(script.find("mount __common\nmkdir OPL\n") != std::string::npos);
    TEST_CHECK(script.find("put conf_hdd.cfg\n") != std::string::npos);
    TEST_CHECK(script.find("put OPNPS2LD.ELF\n") != std::string::npos);
    TEST_CHECK(script.find("mkdir wLaunchELF\n") != std::string::npos);
    TEST_CHECK(script.find("mkdir FHDB-HDD-Boot-Config\n") != std::string::npos);
    TEST_CHECK(script.find("put FHDB_BOOT_CONFIG.ELF\n") != std::string::npos);
    TEST_CHECK(script.find("mkdir Memory-Card-Annihilator\n") != std::string::npos);
    TEST_CHECK(script.find("put MCA_BOOT.ELF\n") != std::string::npos);
    TEST_CHECK(script.find("put conf_opl.cfg\n") != std::string::npos);
    TEST_CHECK(script.find("mount __system\n") != std::string::npos);
    TEST_CHECK(script.find("rename FHDB.XLF osdmain.elf\n") != std::string::npos);
    TEST_CHECK(script.find("mount __sysconf\n") != std::string::npos);
    TEST_CHECK(script.find("put FREEHDB.CNF\n") != std::string::npos);

    const std::string verify = Ps2::Ps2HddFormat::BuildProvisionVerifyScript(
            "/dev/sdz", selection);
    TEST_CHECK(verify.find("mount PP.FHDB.APPS\n") != std::string::npos);
    TEST_CHECK(verify.find("cd APPS\nls\n") != std::string::npos);
    TEST_CHECK(verify.find("mount __system\n") != std::string::npos);
    TEST_CHECK(verify.find("mount __sysconf\n") != std::string::npos);

    Ps2::ProvisioningSelection formatOnly;
    TEST_CHECK(!formatOnly.any());
    TEST_CHECK(Ps2::Ps2HddFormat::BuildProvisionScript("/dev/sdz", "/tmp/stage", formatOnly) ==
            "device /dev/sdz\nexit\n");
}


void testGamesOnlyBankInitializer()
{
    char imagePath[] = "/tmp/ps2-hdd-upper-bank-XXXXXX";
    const int descriptor = mkstemp(imagePath);
    TEST_CHECK(descriptor >= 0);
    TemporaryProbeFile image(descriptor, imagePath);

    const std::uint64_t bank1BaseBytes = Ps2::HddLayoutPlanner::BankBoundarySectors *
            Ps2::HddLayoutPlanner::SectorSize;
    const std::uint64_t imageSize = bank1BaseBytes +
            Ps2::HddLayoutPlanner::MinimumBankSizeSectors * Ps2::HddLayoutPlanner::SectorSize;
    TEST_CHECK(ftruncate(descriptor, static_cast<off_t>(imageSize)) == 0);
    TEST_CHECK(image.closeFile() == 0);

    Ps2::ApaBank::InitializeGamesOnly(imagePath, imageSize, 1, false);
    const auto probes = Ps2::Apa::ProbePhysicalDrive(imagePath, imageSize, 2);
    TEST_CHECK(probes.size() == 2);
    TEST_CHECK(probes[0].header.state == Ps2::ApaHeaderState::NotPresent);
    TEST_CHECK(probes[1].header.state == Ps2::ApaHeaderState::Valid);
    TEST_CHECK(probes[1].header.id == "__mbr");
    TEST_CHECK(probes[1].header.start == 0);
    TEST_CHECK(probes[1].header.length == 0x40000U);
    TEST_CHECK(probes[1].header.next == 0);
    TEST_CHECK(probes[1].header.previous == 0);

    bool refusedOverwrite = false;
    try { Ps2::ApaBank::InitializeGamesOnly(imagePath, imageSize, 1, false); }
    catch (const std::runtime_error &) { refusedOverwrite = true; }
    TEST_CHECK(refusedOverwrite);
}

void testApaPartitionChainReader()
{
    char imagePath[] = "/tmp/ps2-hdd-apa-chain-XXXXXX";
    const int descriptor = mkstemp(imagePath);
    TEST_CHECK(descriptor >= 0);
    TemporaryProbeFile image(descriptor, imagePath);

    constexpr std::uint32_t step = 0x40000;
    constexpr std::uint64_t sectors = static_cast<std::uint64_t>(step) * 6;
    TEST_CHECK(ftruncate(descriptor, static_cast<off_t>(sectors * 512ULL)) == 0);

    auto mbr = makeMbr();
    writeLe32(mbr.data() + 8, step);
    writeLe32(mbr.data(), 0);
    writeLe32(mbr.data(), Ps2::Apa::CalculateChecksum(mbr.data(), mbr.size()));
    TEST_CHECK(pwrite(descriptor, mbr.data(), mbr.size(), 0) == static_cast<ssize_t>(mbr.size()));

    const char *names[] = { "__net", "__system", "__sysconf", "__common" };
    for (std::uint32_t i = 0; i < 4; i++)
    {
        const std::uint32_t start = step * (i + 1);
        const std::uint32_t next = i == 3 ? 0 : step * (i + 2);
        const auto header = makePartition(names[i], start, step, next);
        TEST_CHECK(pwrite(descriptor, header.data(), header.size(),
                static_cast<off_t>(static_cast<std::uint64_t>(start) * 512ULL)) ==
                static_cast<ssize_t>(header.size()));
    }
    TEST_CHECK(image.closeFile() == 0);

    const auto chain = Ps2::Apa::ReadPartitionChain(imagePath, 0, sectors, 16);
    TEST_CHECK(chain.size() == 5);
    TEST_CHECK(chain[0].header.id == "__mbr");
    TEST_CHECK(chain[1].header.id == "__net");
    TEST_CHECK(chain[2].header.id == "__system");
    TEST_CHECK(chain[3].header.id == "__sysconf");
    TEST_CHECK(chain[4].header.id == "__common");
}

#endif

} // namespace

int main()
{
    testStandardLayout();
    testFourTbExtendedLayout();
    testFourTbStandardLayoutLeavesRemainderUnused();
    testApaMbrParser();
    testApaMbrValidation();
    testFhdbConfigRoundTripAndMenuEditing();
    testPhysicalDiskEnumeration();
    testOplPlugAndPlayPreset();
    testOplConfigSetKeyPreservesOtherSettings();
#ifdef __linux__
    testLinuxHighLbaApaProbe();
    testFhdbMbrImageInstallation();
    testStandardPfsshellScripts();
    testProvisionPfsshellScripts();
    testApaPartitionChainReader();
    testGamesOnlyBankInitializer();
#endif
    std::cout << "PS2 APA, layout, standard-format and FHDB configuration tests passed.\n";
    return 0;
}
