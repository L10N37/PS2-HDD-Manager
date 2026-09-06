#include "core/FhdbConfig.h"
#include "core/PhysicalDisk.h"
#include "core/Ps2Apa.h"
#include "core/Ps2ApaBank.h"
#include "core/Ps2HddFormat.h"
#include "core/Ps2HddLayout.h"
#include "core/OplConfig.h"

#include <algorithm>
#include <cerrno>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef __linux__
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace
{

struct Options
{
    std::string device;
    std::string smokeImage;
    std::uint64_t expectedSize = 0;
    std::string pfsshell;
    std::string payloadDirectory;
    std::string hdlDump;
    std::string installGame;
    std::string gameName;
    std::string media;
    int bank = -1;
    bool extendedBanks = false;
    bool listGames = false;
    bool listPfs = false;
    bool ensureCoverArt = false;
    std::string pfsPartition = "auto";
    std::string pfsPath = "/OPL";
    std::string copyManifest;
    Ps2::ProvisioningSelection provision;
};

[[noreturn]] void usage(const char *program)
{
    std::cerr
        << "Usage:\n"
        << "  " << program << " --device /dev/sdX --expected-size BYTES --pfsshell PATH [format options]\n"
        << "  " << program << " --smoke-image IMAGE --pfsshell PATH\n"
        << "  " << program << " --device /dev/sdX --expected-size BYTES --hdl-dump PATH "
           "--install-game IMAGE --game-name NAME --media cd|dvd\n"
        << "  " << program << " --device /dev/sdX --expected-size BYTES --hdl-dump PATH --list-games\n"
        << "  " << program << " --device /dev/sdX --expected-size BYTES --pfsshell PATH "
           "--list-pfs [--partition auto|NAME] [--pfs-path /PATH]\n"
        << "  " << program << " --device /dev/sdX --expected-size BYTES --pfsshell PATH "
           "--copy-manifest FILE [--partition auto|NAME]\n"
        << "  " << program << " --device /dev/sdX --expected-size BYTES --pfsshell PATH "
           "--ensure-cover-art [--partition auto|NAME] [--pfs-path /OPL]\n\n"
        << "Optional provisioning flags (format mode):\n"
        << "  --payload-dir PATH       Downloaded payload root.\n"
        << "  --install-opl            Install current OPNPS2LD.ELF and configure OPL HDD storage.\n"
        << "  --configure-opl          Write plug-and-play internal-HDD conf_opl.cfg defaults.\n"
        << "  --install-wle            Install current normal wLaunchELF_ISR BOOT.ELF.\n"
        << "  --install-mca            Install Memory Card Annihilator in OPL Apps.\n"
        << "  --install-fhdb           Install FreeHDBoot 1.966 HDD boot files + MBR KELF.\n"
        << "  --install-hdd-enabler    Install the dedicated FHDB HDD Boot Configuration app.\n";
    std::exit(2);
}

Options parseOptions(int argc, char **argv)
{
    Options options;
    for (int index = 1; index < argc; index++)
    {
        const std::string argument = argv[index];
        if (argument == "--help" || argument == "-h")
            usage(argv[0]);
        if (argument == "--install-opl") { options.provision.installOpl = true; continue; }
        if (argument == "--configure-opl") { options.provision.configureOplPlugAndPlay = true; continue; }
        if (argument == "--install-wle") { options.provision.installWlaunchElf = true; continue; }
        if (argument == "--install-mca") { options.provision.installMemoryCardAnnihilator = true; continue; }
        if (argument == "--install-fhdb") { options.provision.installFhdb = true; continue; }
        if (argument == "--install-hdd-enabler") { options.provision.installHddBootEnabler = true; continue; }
        if (argument == "--list-games") { options.listGames = true; continue; }
        if (argument == "--list-pfs") { options.listPfs = true; continue; }
        if (argument == "--ensure-cover-art") { options.ensureCoverArt = true; continue; }
        if (argument == "--extended-banks") { options.extendedBanks = true; continue; }
        if (index + 1 >= argc)
            usage(argv[0]);
        const std::string value = argv[++index];
        if (argument == "--device") options.device = value;
        else if (argument == "--smoke-image") options.smokeImage = value;
        else if (argument == "--expected-size") options.expectedSize = std::stoull(value);
        else if (argument == "--pfsshell") options.pfsshell = value;
        else if (argument == "--payload-dir") options.payloadDirectory = value;
        else if (argument == "--hdl-dump") options.hdlDump = value;
        else if (argument == "--install-game") options.installGame = value;
        else if (argument == "--game-name") options.gameName = value;
        else if (argument == "--media") options.media = value;
        else if (argument == "--bank") options.bank = std::stoi(value);
        else if (argument == "--partition") options.pfsPartition = value;
        else if (argument == "--pfs-path") options.pfsPath = value;
        else if (argument == "--copy-manifest") options.copyManifest = value;
        else usage(argv[0]);
    }

    const bool commonPhysical = !options.device.empty() && options.smokeImage.empty() && options.expectedSize != 0;
    const bool gameMode = commonPhysical && !options.hdlDump.empty() && !options.installGame.empty() &&
            !options.gameName.empty() && (options.media == "cd" || options.media == "dvd") &&
            options.pfsshell.empty() && !options.provision.any() && !options.listGames && !options.listPfs &&
            !options.ensureCoverArt && options.copyManifest.empty();
    const bool listGamesMode = commonPhysical && options.listGames && !options.hdlDump.empty() &&
            options.installGame.empty() && options.pfsshell.empty() && !options.provision.any() && !options.listPfs &&
            !options.ensureCoverArt && options.copyManifest.empty();
    const bool listPfsMode = commonPhysical && options.listPfs && !options.pfsshell.empty() &&
            options.hdlDump.empty() && options.installGame.empty() && !options.provision.any() &&
            !options.ensureCoverArt && options.copyManifest.empty();
    const bool copyPfsMode = commonPhysical && !options.copyManifest.empty() && !options.pfsshell.empty() &&
            options.hdlDump.empty() && options.installGame.empty() && !options.provision.any() && !options.listGames &&
            !options.listPfs && !options.ensureCoverArt;
    const bool coverArtMode = commonPhysical && options.ensureCoverArt && !options.pfsshell.empty() &&
            options.hdlDump.empty() && options.installGame.empty() && !options.provision.any() && !options.listGames &&
            !options.listPfs && options.copyManifest.empty();
    const bool physicalMode = commonPhysical && !options.pfsshell.empty() && options.installGame.empty() &&
            !options.listGames && !options.listPfs && !options.ensureCoverArt && options.copyManifest.empty();
    const bool smokeMode = options.device.empty() && !options.smokeImage.empty() &&
            options.expectedSize == 0 && !options.pfsshell.empty() && !options.provision.any() &&
            options.installGame.empty() && !options.listGames && !options.listPfs && !options.ensureCoverArt && options.copyManifest.empty();
    if (!physicalMode && !smokeMode && !gameMode && !listGamesMode && !listPfsMode && !copyPfsMode && !coverArtMode)
        usage(argv[0]);
    if (physicalMode && options.provision.any() && options.payloadDirectory.empty())
        usage(argv[0]);
    if ((listPfsMode || copyPfsMode || coverArtMode) && (options.pfsPartition.empty() || options.pfsPath.find('\n') != std::string::npos))
        usage(argv[0]);
    if (options.bank < -1 || options.bank >= static_cast<int>(Ps2::HddLayoutPlanner::MaximumBankCount))
        usage(argv[0]);
    return options;
}

bool isFirstHardwareTestDevice(const std::string &path)
{
    if (path.rfind("/dev/sd", 0) != 0 && path.rfind("/dev/hd", 0) != 0)
        return false;
    const std::string name = fs::path(path).filename().string();
    const std::size_t prefix = name.rfind("sd", 0) == 0 || name.rfind("hd", 0) == 0 ? 2 : 0;
    if (prefix == 0 || name.size() <= prefix)
        return false;
    for (std::size_t index = prefix; index < name.size(); index++)
        if (name[index] < 'a' || name[index] > 'z')
            return false;
    return true;
}

#ifdef __linux__
constexpr std::uint32_t SessionMaxArgumentBytes = 16U * 1024U * 1024U;
constexpr std::uint32_t SessionMaxArguments = 256U;
constexpr unsigned char SessionOutputFrame = 1;
constexpr unsigned char SessionDoneFrame = 2;

bool readExact(int fd, void *buffer, std::size_t length)
{
    auto *out = static_cast<unsigned char *>(buffer);
    std::size_t done = 0;
    while (done < length)
    {
        const ssize_t amount = read(fd, out + done, length - done);
        if (amount == 0)
            return false;
        if (amount < 0)
        {
            if (errno == EINTR)
                continue;
            throw std::runtime_error("Privileged session read failed: " + std::string(std::strerror(errno)));
        }
        done += static_cast<std::size_t>(amount);
    }
    return true;
}

void writeExact(int fd, const void *buffer, std::size_t length)
{
    const auto *data = static_cast<const unsigned char *>(buffer);
    std::size_t done = 0;
    while (done < length)
    {
        const ssize_t amount = write(fd, data + done, length - done);
        if (amount < 0)
        {
            if (errno == EINTR)
                continue;
            throw std::runtime_error("Privileged session write failed: " + std::string(std::strerror(errno)));
        }
        done += static_cast<std::size_t>(amount);
    }
}

std::uint32_t decodeU32(const unsigned char *bytes)
{
    return static_cast<std::uint32_t>(bytes[0]) |
            (static_cast<std::uint32_t>(bytes[1]) << 8) |
            (static_cast<std::uint32_t>(bytes[2]) << 16) |
            (static_cast<std::uint32_t>(bytes[3]) << 24);
}

std::array<unsigned char, 4> encodeU32(std::uint32_t value)
{
    return { static_cast<unsigned char>(value & 0xff),
             static_cast<unsigned char>((value >> 8) & 0xff),
             static_cast<unsigned char>((value >> 16) & 0xff),
             static_cast<unsigned char>((value >> 24) & 0xff) };
}

void sendSessionFrame(unsigned char type, const void *payload, std::uint32_t length)
{
    const auto encodedLength = encodeU32(length);
    writeExact(STDOUT_FILENO, &type, 1);
    writeExact(STDOUT_FILENO, encodedLength.data(), encodedLength.size());
    if (length != 0)
        writeExact(STDOUT_FILENO, payload, length);
}

std::string selfExecutablePath()
{
    std::array<char, 4096> buffer{};
    const ssize_t length = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (length <= 0)
        throw std::runtime_error("Unable to resolve /proc/self/exe for privileged session.");
    return std::string(buffer.data(), static_cast<std::size_t>(length));
}

int runSessionChild(const std::vector<std::string> &arguments)
{
    int outputPipe[2];
    if (pipe(outputPipe) != 0)
        throw std::runtime_error("Unable to create privileged-session child pipe: " + std::string(std::strerror(errno)));

    const pid_t child = fork();
    if (child < 0)
    {
        close(outputPipe[0]);
        close(outputPipe[1]);
        throw std::runtime_error("Unable to fork privileged-session worker: " + std::string(std::strerror(errno)));
    }
    if (child == 0)
    {
        close(outputPipe[0]);
        dup2(outputPipe[1], STDOUT_FILENO);
        dup2(outputPipe[1], STDERR_FILENO);
        close(outputPipe[1]);

        const std::string executable = selfExecutablePath();
        std::vector<char *> argv;
        argv.reserve(arguments.size() + 2);
        argv.push_back(const_cast<char *>(executable.c_str()));
        for (const std::string &argument : arguments)
            argv.push_back(const_cast<char *>(argument.c_str()));
        argv.push_back(nullptr);
        execv(executable.c_str(), argv.data());
        std::cerr << "PS2 HDD Writer session worker exec failed: " << std::strerror(errno) << "\n";
        _exit(127);
    }

    close(outputPipe[1]);
    std::array<unsigned char, 8192> buffer{};
    for (;;)
    {
        const ssize_t amount = read(outputPipe[0], buffer.data(), buffer.size());
        if (amount == 0)
            break;
        if (amount < 0)
        {
            if (errno == EINTR)
                continue;
            close(outputPipe[0]);
            throw std::runtime_error("Reading privileged-session worker output failed: " + std::string(std::strerror(errno)));
        }
        sendSessionFrame(SessionOutputFrame, buffer.data(), static_cast<std::uint32_t>(amount));
    }
    close(outputPipe[0]);

    int status = 0;
    while (waitpid(child, &status, 0) < 0)
        if (errno != EINTR)
            throw std::runtime_error("Waiting for privileged-session worker failed: " + std::string(std::strerror(errno)));
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 1);
}

bool isSessionInvocation(int argc, char **argv)
{
    return argc >= 2 && std::string(argv[1]) == "--session-stdio";
}

int runPrivilegedSession(int argc, char **argv)
{
    if (geteuid() != 0)
    {
        std::cerr << "PS2 HDD Writer: session mode requires pkexec/root.\n";
        return 1;
    }
    // --client-uid is informational/auditing only. The session has no socket
    // and exposes no generic exec facility; it only runs this writer's own
    // guarded CLI operations over its inherited stdin/stdout pipes.
    if (argc != 2 && !(argc == 4 && std::string(argv[2]) == "--client-uid"))
    {
        std::cerr << "PS2 HDD Writer: invalid session arguments.\n";
        return 2;
    }

    static constexpr char Handshake[] = "PS2HDD-SESSION-1\n";
    writeExact(STDOUT_FILENO, Handshake, sizeof(Handshake) - 1);

    for (;;)
    {
        unsigned char magic[4];
        if (!readExact(STDIN_FILENO, magic, sizeof(magic)))
            return 0; // GUI closed its write pipe: end the root session.
        if (std::memcmp(magic, "P2RQ", 4) != 0)
            throw std::runtime_error("Invalid privileged-session request magic.");

        unsigned char encodedCount[4];
        if (!readExact(STDIN_FILENO, encodedCount, sizeof(encodedCount)))
            return 0;
        const std::uint32_t count = decodeU32(encodedCount);
        if (count == 0 || count > SessionMaxArguments)
            throw std::runtime_error("Invalid privileged-session argument count.");

        std::vector<std::string> arguments;
        arguments.reserve(count);
        bool nestedSession = false;
        for (std::uint32_t index = 0; index < count; ++index)
        {
            unsigned char encodedLength[4];
            if (!readExact(STDIN_FILENO, encodedLength, sizeof(encodedLength)))
                return 0;
            const std::uint32_t length = decodeU32(encodedLength);
            if (length > SessionMaxArgumentBytes)
                throw std::runtime_error("Privileged-session argument is too large.");
            std::string argument(length, '\0');
            if (length != 0 && !readExact(STDIN_FILENO, argument.data(), length))
                return 0;
            if (argument == "--session-stdio")
                nestedSession = true;
            arguments.push_back(std::move(argument));
        }

        int exitCode = 2;
        if (nestedSession)
        {
            static constexpr char Message[] = "PS2 HDD Writer: nested session request refused.\n";
            sendSessionFrame(SessionOutputFrame, Message, sizeof(Message) - 1);
        }
        else
        {
            try
            {
                exitCode = runSessionChild(arguments);
            }
            catch (const std::exception &error)
            {
                const std::string message = std::string("PS2 HDD Writer session: FAILED: ") + error.what() + "\n";
                sendSessionFrame(SessionOutputFrame, message.data(), static_cast<std::uint32_t>(message.size()));
                exitCode = 1;
            }
        }
        const auto encodedExit = encodeU32(static_cast<std::uint32_t>(exitCode));
        sendSessionFrame(SessionDoneFrame, encodedExit.data(), encodedExit.size());
    }
}

class TemporaryDirectory
{
public:
    TemporaryDirectory()
    {
        char name[] = "/tmp/ps2-hdd-writer-XXXXXX";
        char *created = mkdtemp(name);
        if (created == nullptr)
            throw std::runtime_error("Unable to create writer staging directory: " +
                    std::string(std::strerror(errno)));
        path_ = created;
    }

    ~TemporaryDirectory()
    {
        std::error_code error;
        fs::remove_all(path_, error);
    }

    const fs::path &path() const { return path_; }

private:
    fs::path path_;
};

Ps2::PhysicalDiskCandidate requeryTarget(const Options &options);

bool sameCanonicalDevice(const std::string &source, const std::string &target)
{
    if (source.rfind("/dev/", 0) != 0)
        return false;
    std::error_code sourceError;
    std::error_code targetError;
    const fs::path sourcePath = fs::canonical(source, sourceError);
    const fs::path targetPath = fs::canonical(target, targetError);
    return !sourceError && !targetError && sourcePath == targetPath;
}

bool targetIsMounted(const std::string &target)
{
    std::ifstream mountInfo("/proc/self/mountinfo");
    std::string line;
    while (std::getline(mountInfo, line))
    {
        const std::size_t separator = line.find(" - ");
        if (separator == std::string::npos)
            continue;
        std::istringstream tail(line.substr(separator + 3));
        std::string filesystemType;
        std::string source;
        if (tail >> filesystemType >> source && sameCanonicalDevice(source, target))
            return true;
    }
    return false;
}

bool targetIsSwap(const std::string &target)
{
    std::ifstream swaps("/proc/swaps");
    std::string line;
    std::getline(swaps, line);
    while (std::getline(swaps, line))
    {
        std::istringstream fields(line);
        std::string source;
        if (fields >> source && sameCanonicalDevice(source, target))
            return true;
    }
    return false;
}

bool targetHasKernelHolders(const std::string &target)
{
    const std::string name = fs::path(target).filename().string();
    const fs::path holders = fs::path("/sys/class/block") / name / "holders";
    std::error_code error;
    if (!fs::is_directory(holders, error))
        return false;
    return fs::directory_iterator(holders, error) != fs::directory_iterator();
}

bool isDirectPartitionOfTarget(const std::string &source, const std::string &target)
{
    if (source.size() <= target.size() || source.compare(0, target.size(), target) != 0)
        return false;
    for (std::size_t i = target.size(); i < source.size(); ++i)
        if (source[i] < '0' || source[i] > '9')
            return false;
    return true;
}

bool sourceBelongsToTarget(const std::string &source, const std::string &target)
{
    return sameCanonicalDevice(source, target) || isDirectPartitionOfTarget(source, target);
}

std::string decodeMountInfoPath(std::string value)
{
    struct Escape { const char *encoded; char decoded; };
    static const Escape escapes[] = {
        { "\\040", ' ' }, { "\\011", '\t' }, { "\\012", '\n' }, { "\\134", '\\' }
    };
    for (const Escape &escape : escapes)
    {
        std::size_t pos = 0;
        while ((pos = value.find(escape.encoded, pos)) != std::string::npos)
        {
            value.replace(pos, 4, 1, escape.decoded);
            ++pos;
        }
    }
    return value;
}

std::vector<std::string> targetMountPoints(const std::string &target)
{
    std::vector<std::string> mounts;
    std::ifstream mountInfo("/proc/self/mountinfo");
    std::string line;
    while (std::getline(mountInfo, line))
    {
        const std::size_t separator = line.find(" - ");
        if (separator == std::string::npos)
            continue;

        std::istringstream head(line.substr(0, separator));
        std::string mountId, parentId, deviceNumber, root, mountPoint;
        if (!(head >> mountId >> parentId >> deviceNumber >> root >> mountPoint))
            continue;

        std::istringstream tail(line.substr(separator + 3));
        std::string filesystemType, source;
        if (!(tail >> filesystemType >> source))
            continue;
        source = decodeMountInfoPath(source);
        if (sourceBelongsToTarget(source, target))
            mounts.push_back(decodeMountInfoPath(mountPoint));
    }
    std::sort(mounts.begin(), mounts.end(), [](const std::string &a, const std::string &b) {
        return a.size() > b.size();
    });
    mounts.erase(std::unique(mounts.begin(), mounts.end()), mounts.end());
    return mounts;
}

bool targetTreeIsMounted(const std::string &target)
{
    return !targetMountPoints(target).empty();
}

bool targetTreeIsSwap(const std::string &target)
{
    std::ifstream swaps("/proc/swaps");
    std::string line;
    std::getline(swaps, line);
    while (std::getline(swaps, line))
    {
        std::istringstream fields(line);
        std::string source;
        if (fields >> source && sourceBelongsToTarget(source, target))
            return true;
    }
    return false;
}

bool blockNodeHasHolders(const fs::path &node)
{
    const fs::path holders = node / "holders";
    std::error_code error;
    if (!fs::is_directory(holders, error))
        return false;
    return fs::directory_iterator(holders, error) != fs::directory_iterator();
}

bool targetTreeHasKernelHolders(const std::string &target)
{
    const std::string diskName = fs::path(target).filename().string();
    const fs::path blockRoot("/sys/class/block");
    if (blockNodeHasHolders(blockRoot / diskName))
        return true;

    std::error_code error;
    for (const auto &entry : fs::directory_iterator(blockRoot, error))
    {
        const std::string name = entry.path().filename().string();
        if (name.size() <= diskName.size() || name.compare(0, diskName.size(), diskName) != 0)
            continue;
        bool numericSuffix = true;
        for (std::size_t i = diskName.size(); i < name.size(); ++i)
            if (name[i] < '0' || name[i] > '9') { numericSuffix = false; break; }
        if (numericSuffix && blockNodeHasHolders(entry.path()))
            return true;
    }
    return false;
}

bool numericProcessDirectory(const std::string &name)
{
    if (name.empty()) return false;
    return std::all_of(name.begin(), name.end(),
            [](unsigned char ch) { return ch >= '0' && ch <= '9'; });
}

bool pathWithinMount(std::string path, const std::string &mountPoint)
{
    static const std::string deletedSuffix = " (deleted)";
    if (path.size() > deletedSuffix.size() &&
            path.compare(path.size() - deletedSuffix.size(),
                         deletedSuffix.size(), deletedSuffix) == 0)
        path.resize(path.size() - deletedSuffix.size());
    return path == mountPoint ||
            (path.size() > mountPoint.size() &&
             path.compare(0, mountPoint.size(), mountPoint) == 0 &&
             path[mountPoint.size()] == '/');
}

struct BusyProcess {
    int pid = -1;
    std::string name;
};

std::vector<BusyProcess> processesUsingMount(const std::string &mountPoint)
{
    std::vector<BusyProcess> out;
    std::error_code error;
    for (const auto &entry : fs::directory_iterator("/proc", error)) {
        const std::string pidText = entry.path().filename().string();
        if (!numericProcessDirectory(pidText)) continue;

        bool holds = false;
        auto checkLink = [&](const fs::path &link) {
            std::error_code linkError;
            const fs::path target = fs::read_symlink(link, linkError);
            if (!linkError && pathWithinMount(target.string(), mountPoint))
                holds = true;
        };

        checkLink(entry.path() / "cwd");
        if (!holds) checkLink(entry.path() / "root");
        if (!holds) {
            const fs::path fdDir = entry.path() / "fd";
            std::error_code fdError;
            for (const auto &fd : fs::directory_iterator(fdDir, fdError)) {
                checkLink(fd.path());
                if (holds) break;
            }
        }
        if (!holds) continue;

        BusyProcess process;
        try { process.pid = std::stoi(pidText); }
        catch (...) { continue; }

        std::ifstream comm(entry.path() / "comm");
        std::getline(comm, process.name);
        if (process.name.empty()) process.name = "unknown";
        out.push_back(std::move(process));
    }

    std::sort(out.begin(), out.end(),
            [](const BusyProcess &a, const BusyProcess &b) { return a.pid < b.pid; });
    out.erase(std::unique(out.begin(), out.end(),
            [](const BusyProcess &a, const BusyProcess &b) { return a.pid == b.pid; }),
            out.end());
    return out;
}

void unmountTargetFilesystems(const std::string &target)
{
    const auto mounts = targetMountPoints(target);
    for (const std::string &mountPoint : mounts) {
        std::cout << "STAGE: Unmounting existing target filesystem at "
                  << mountPoint << "...\n" << std::flush;

        int lastError = 0;
        bool unmounted = false;
        for (int attempt = 0; attempt < 4; ++attempt) {
            if (umount2(mountPoint.c_str(), 0) == 0 ||
                    errno == EINVAL || errno == ENOENT) {
                unmounted = true;
                break;
            }
            lastError = errno;
            if (lastError != EBUSY) break;
            usleep(250000);
        }
        if (unmounted) continue;

        std::ostringstream message;
        if (lastError == EBUSY) {
            message << "TARGET_BUSY: " << mountPoint << " is still in use.";
            const auto holders = processesUsingMount(mountPoint);
            if (!holders.empty()) {
                message << "\nClose these process(es), then click Retry:";
                for (const BusyProcess &process : holders)
                    message << "\n  PID " << process.pid << "  " << process.name;
            } else {
                message << "\nClose Dolphin, Double Commander, or any terminal "
                           "currently browsing this drive, then click Retry.";
            }
        } else {
            message << "REFUSED: could not unmount " << mountPoint
                    << ": " << std::strerror(lastError);
        }
        throw std::runtime_error(message.str());
    }

    if (targetTreeIsMounted(target))
        throw std::runtime_error(
                "TARGET_BUSY: one or more target partitions remain mounted. "
                "Close Dolphin, Double Commander, or any terminal using the drive, then click Retry.");
}

void writeAllAt(int fd, std::uint64_t offset, const void *buffer, std::size_t length)
{
    const auto *data = static_cast<const unsigned char *>(buffer);
    std::size_t done = 0;
    while (done < length)
    {
        const ssize_t amount = pwrite(fd, data + done, length - done,
                static_cast<off_t>(offset + done));
        if (amount < 0)
        {
            if (errno == EINTR)
                continue;
            throw std::runtime_error("Clearing the old host partition table failed: " + std::string(std::strerror(errno)));
        }
        if (amount == 0)
            throw std::runtime_error("Clearing the old host partition table made no forward progress.");
        done += static_cast<std::size_t>(amount);
    }
}

void clearHostPartitionTable(const std::string &target, std::uint64_t sizeBytes)
{
    constexpr std::size_t WipeBytes = 1024U * 1024U;
    if (sizeBytes < WipeBytes * 2ULL)
        throw std::runtime_error("Target is unexpectedly too small for guarded partition-table clearing.");

    const int fd = open(target.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0)
        throw std::runtime_error("Opening target to clear the existing host partition table failed: " + std::string(std::strerror(errno)));

    try
    {
        std::vector<unsigned char> zero(WipeBytes, 0);
        writeAllAt(fd, 0, zero.data(), zero.size());
        writeAllAt(fd, sizeBytes - WipeBytes, zero.data(), zero.size());
        if (fsync(fd) != 0)
            throw std::runtime_error("Flushing cleared partition-table sectors failed: " + std::string(std::strerror(errno)));
        if (ioctl(fd, BLKRRPART) != 0)
            throw std::runtime_error("Kernel still has the old partition table busy (close Dolphin/Double Commander and retry): " + std::string(std::strerror(errno)));
    }
    catch (...)
    {
        close(fd);
        throw;
    }
    close(fd);
}

void prepareTargetForFormat(const Options &options)
{
    const Ps2::PhysicalDiskCandidate before = requeryTarget(options);
    if (before.size != options.expectedSize || !before.systemDiskCheckAvailable ||
            before.containsSystemVolume || before.containsBootPartition || before.readOnly)
        throw std::runtime_error("REFUSED: target identity or safety state changed before partition-table clearing.");
    if (targetTreeIsSwap(options.device))
        throw std::runtime_error("REFUSED: a target partition is active swap. Disable swap before formatting this disk.");
    if (targetTreeHasKernelHolders(options.device))
        throw std::runtime_error("REFUSED: target or one of its partitions is in use by a kernel block-device holder.");

    unmountTargetFilesystems(options.device);
    std::cout << "STAGE: Clearing existing MBR/GPT partition-table signatures...\n" << std::flush;
    clearHostPartitionTable(options.device, options.expectedSize);

    for (int attempt = 0; attempt < 20; ++attempt)
    {
        const Ps2::PhysicalDiskCandidate refreshed = requeryTarget(options);
        if (refreshed.partitionCount == 0)
            return;
        usleep(100000);
    }
    throw std::runtime_error("Kernel still reports host partitions after the formatter cleared the partition table.");
}

std::string runPfsshell(const std::string &executable, const std::string &script)
{
    int inputPipe[2];
    int outputPipe[2];
    if (pipe(inputPipe) != 0 || pipe(outputPipe) != 0)
        throw std::runtime_error("Unable to create pfsshell pipes: " +
                std::string(std::strerror(errno)));

    const pid_t child = fork();
    if (child < 0)
        throw std::runtime_error("Unable to start pfsshell: " + std::string(std::strerror(errno)));
    if (child == 0)
    {
        dup2(inputPipe[0], STDIN_FILENO);
        dup2(outputPipe[1], STDOUT_FILENO);
        dup2(outputPipe[1], STDERR_FILENO);
        close(inputPipe[0]);
        close(inputPipe[1]);
        close(outputPipe[0]);
        close(outputPipe[1]);
        execl(executable.c_str(), executable.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }

    close(inputPipe[0]);
    close(outputPipe[1]);

    std::size_t completed = 0;
    while (completed < script.size())
    {
        const ssize_t written = write(inputPipe[1], script.data() + completed,
                script.size() - completed);
        if (written < 0)
        {
            close(inputPipe[1]);
            close(outputPipe[0]);
            throw std::runtime_error("Sending commands to pfsshell failed: " +
                    std::string(std::strerror(errno)));
        }
        completed += static_cast<std::size_t>(written);
    }
    close(inputPipe[1]);

    std::string log;
    char buffer[4096];
    for (;;)
    {
        const ssize_t received = read(outputPipe[0], buffer, sizeof(buffer));
        if (received > 0)
        {
            log.append(buffer, static_cast<std::size_t>(received));
            std::cout.write(buffer, received);
            std::cout.flush();
            continue;
        }
        if (received == 0)
            break;
        if (errno == EINTR)
            continue;
        close(outputPipe[0]);
        throw std::runtime_error("Reading pfsshell output failed: " +
                std::string(std::strerror(errno)));
    }
    close(outputPipe[0]);

    int status = 0;
    if (waitpid(child, &status, 0) < 0)
        throw std::runtime_error("Waiting for pfsshell failed: " +
                std::string(std::strerror(errno)));
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        throw std::runtime_error("pfsshell exited unsuccessfully.");

    // Selecting a RAW disk produces one expected diagnostic before `initialize yes` has
    // created hdd0:. Treat only that exact condition as harmless; every other pfsshell
    // `(!)` diagnostic is a command failure and must stop the transaction.
    std::istringstream logLines(log);
    std::string line;
    while (std::getline(logLines, line))
    {
        if (line.find("(!)") == std::string::npos)
            continue;
        if (line.find("Unable to open hdd0:: No such device") != std::string::npos)
            continue;
        throw std::runtime_error("pfsshell reported a command failure: " + line);
    }
    if (log.find("init_apa: failed") != std::string::npos ||
            log.find("init_pfs: failed") != std::string::npos ||
            log.find("Unable to parse command line") != std::string::npos)
        throw std::runtime_error("pfsshell reported a driver/command failure.");
    return log;
}

std::string bankedDevicePath(const Options &options, int bank)
{
    if (bank < 0) bank = 0;
    const std::uint64_t sectors = options.expectedSize / Ps2::HddLayoutPlanner::SectorSize;
    if (bank == 0 && sectors <= Ps2::HddLayoutPlanner::MaximumApaSectorCount) return options.device;
    return "bank" + std::to_string(bank) + ":" + options.device;
}

std::uint32_t plannedBankCount(const Options &options)
{
    const auto layout = Ps2::HddLayoutPlanner::Plan(options.expectedSize, Ps2::HddLayoutMode::ExtendedApaBanks);
    return layout.valid ? static_cast<std::uint32_t>(layout.banks.size()) : 0;
}

bool bankHasValidMbr(const Options &options, std::uint32_t bank)
{
    const auto probes = Ps2::Apa::ProbePhysicalDrive(options.device, options.expectedSize, bank + 1);
    return bank < probes.size() && probes[bank].header.state == Ps2::ApaHeaderState::Valid;
}

void verifyGameBank(const Options &options, int bank)
{
    if (bank < 0) bank = 0;
    if (static_cast<std::uint32_t>(bank) >= plannedBankCount(options)) throw std::runtime_error("Requested bank is outside the disk.");
    if (!bankHasValidMbr(options, static_cast<std::uint32_t>(bank))) throw std::runtime_error("Requested bank has no valid APA MBR. Format as Extended APA Banks first.");
    if (bank == 0) Ps2::Ps2HddFormat::VerifyStandardApaDisk(options.device, options.expectedSize);
}

std::string runHdlDumpInstall(const Options &options)
{
    int outputPipe[2];
    if (pipe(outputPipe) != 0)
        throw std::runtime_error("Unable to create hdl_dump output pipe: " +
                std::string(std::strerror(errno)));

    const pid_t child = fork();
    if (child < 0)
        throw std::runtime_error("Unable to start hdl_dump: " + std::string(std::strerror(errno)));
    if (child == 0)
    {
        dup2(outputPipe[1], STDOUT_FILENO);
        dup2(outputPipe[1], STDERR_FILENO);
        close(outputPipe[0]);
        close(outputPipe[1]);
        const char *command = options.media == "cd" ? "inject_cd" : "inject_dvd";
        const std::string target = bankedDevicePath(options, options.bank < 0 ? 0 : options.bank);
        execl(options.hdlDump.c_str(), options.hdlDump.c_str(), command,
                target.c_str(), options.gameName.c_str(), options.installGame.c_str(),
                static_cast<char*>(nullptr));
        _exit(127);
    }
    close(outputPipe[1]);
    std::string log;
    char buffer[4096];
    for (;;)
    {
        const ssize_t received = read(outputPipe[0], buffer, sizeof(buffer));
        if (received > 0)
        {
            log.append(buffer, static_cast<std::size_t>(received));
            std::cout.write(buffer, received);
            std::cout.flush();
            continue;
        }
        if (received == 0)
            break;
        if (errno == EINTR)
            continue;
        close(outputPipe[0]);
        throw std::runtime_error("Reading hdl_dump output failed: " +
                std::string(std::strerror(errno)));
    }
    close(outputPipe[0]);
    int status = 0;
    if (waitpid(child, &status, 0) < 0)
        throw std::runtime_error("Waiting for hdl_dump failed: " +
                std::string(std::strerror(errno)));
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        throw std::runtime_error("hdl_dump game installation failed.");
    return log;
}

std::string runHdlDumpToc(const Options &options, int bank)
{
    int outputPipe[2];
    if (pipe(outputPipe) != 0)
        throw std::runtime_error("Unable to create hdl_dump verification pipe.");
    const pid_t child = fork();
    if (child < 0)
        throw std::runtime_error("Unable to start hdl_dump verification.");
    if (child == 0)
    {
        dup2(outputPipe[1], STDOUT_FILENO);
        dup2(outputPipe[1], STDERR_FILENO);
        close(outputPipe[0]); close(outputPipe[1]);
        const std::string target = bankedDevicePath(options, bank);
        execl(options.hdlDump.c_str(), options.hdlDump.c_str(), "hdl_toc",
                target.c_str(), "--csv", static_cast<char*>(nullptr));
        _exit(127);
    }
    close(outputPipe[1]);
    std::string log;
    char buffer[4096];
    for (;;)
    {
        const ssize_t received = read(outputPipe[0], buffer, sizeof(buffer));
        if (received > 0) { log.append(buffer, static_cast<std::size_t>(received)); continue; }
        if (received == 0) break;
        if (errno == EINTR) continue;
        close(outputPipe[0]);
        throw std::runtime_error("Reading hdl_dump verification output failed.");
    }
    close(outputPipe[0]);
    int status = 0;
    if (waitpid(child, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
        throw std::runtime_error("hdl_dump could not verify the installed game list.");
    return log;
}


std::string trim(std::string value)
{
    const char *ws = " \t\r\n";
    const std::size_t first = value.find_first_not_of(ws);
    if (first == std::string::npos)
        return {};
    const std::size_t last = value.find_last_not_of(ws);
    return value.substr(first, last - first + 1);
}

std::string cleanField(std::string value)
{
    for (char &ch : value)
        if (ch == '\t' || ch == '\r' || ch == '\n')
            ch = ' ';
    return trim(value);
}

std::vector<std::string> splitCsvLimit(const std::string &line, std::size_t fieldCount)
{
    std::vector<std::string> fields;
    std::size_t begin = 0;
    while (fields.size() + 1 < fieldCount)
    {
        const std::size_t separator = line.find(';', begin);
        if (separator == std::string::npos)
            break;
        fields.push_back(trim(line.substr(begin, separator - begin)));
        begin = separator + 1;
    }
    fields.push_back(trim(line.substr(begin)));
    return fields;
}

void emitGameRecords(const std::string &toc, int bank)
{
    std::istringstream lines(toc);
    std::string line;
    while (std::getline(lines, line))
    {
        const std::vector<std::string> f = splitCsvLimit(trim(line), 6);
        if (f.size() != 6 || (f[0] != "DVD" && f[0] != "CD"))
            continue;
        std::string size = f[1];
        if (size.size() >= 2 && size.substr(size.size() - 2) == "KB")
            size.resize(size.size() - 2);
        std::cout << "GAME\t" << bank << "\t" << f[0] << "\t" << trim(size) << "\t"
                  << cleanField(f[4]) << "\t" << cleanField(f[5]) << "\n";
    }
}

std::string quotePfsshellToken(const std::string &value)
{
    if (value.find_first_of("\r\n") != std::string::npos)
        throw std::invalid_argument("pfsshell token contains a newline.");
    if (value.find('"') == std::string::npos)
        return "\"" + value + "\"";
    if (value.find('\'') == std::string::npos)
        return "'" + value + "'";
    throw std::invalid_argument("pfsshell cannot safely quote a path containing both quote styles: " + value);
}

void requireSafePfsPartitionName(const std::string &name)
{
    if (name == "auto")
        return;
    if (name.empty() || name.size() > 32)
        throw std::invalid_argument("Invalid PFS partition name.");
    for (char ch : name)
        if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '+' || ch == '_' || ch == '-' || ch == '.'))
            throw std::invalid_argument("Invalid PFS partition name.");
}

std::string resolvePfsPartition(const Options &options)
{
    requireSafePfsPartitionName(options.pfsPartition);
    if (options.pfsPartition != "auto")
        return options.pfsPartition;

    const std::string log = runPfsshell(options.pfsshell,
            "device " + bankedDevicePath(options, 0) + "\nls\nexit\n");
    if (log.find("PP.FHDB.APPS") != std::string::npos)
        return "PP.FHDB.APPS";
    if (log.find("+OPL") != std::string::npos)
        return "+OPL";
    throw std::runtime_error("No OPL PFS partition was found (looked for PP.FHDB.APPS and +OPL).");
}

std::string normalizedPfsPath(std::string path)
{
    if (path.empty())
        return "/";
    if (path[0] != '/')
        path.insert(path.begin(), '/');
    while (path.size() > 1 && path.back() == '/')
        path.pop_back();
    if (path.find("..") != std::string::npos || path.find('\n') != std::string::npos || path.find('\r') != std::string::npos)
        throw std::invalid_argument("Unsafe PFS path.");
    return path;
}

void emitPfsRecords(const Options &options)
{
    const std::string partition = resolvePfsPartition(options);
    const std::string path = options.pfsPath == "@opl"
            ? (partition == "+OPL" ? "/" : "/OPL")
            : normalizedPfsPath(options.pfsPath);
    std::string script = "device " + bankedDevicePath(options, 0) + "\nmount " + partition + "\ncd " +
            quotePfsshellToken(path) + "\nls -l\numount\nexit\n";
    const std::string log = runPfsshell(options.pfsshell, script);
    std::cout << "OPL_PARTITION\t" << partition << "\n";
    std::cout << "OPL_BASE\t" << path << "\n";

    std::istringstream lines(log);
    std::string line;
    while (std::getline(lines, line))
    {
        // pfsshell may place its prompt before the first ls row because stdout/stderr
        // are merged. Search for the Unix-like mode field instead of assuming column 0.
        std::size_t modePos = std::string::npos;
        for (std::size_t i = 0; i + 10 <= line.size(); i++)
        {
            const char kind = line[i];
            if (kind != 'd' && kind != '-' && kind != 'l')
                continue;
            bool mode = true;
            for (std::size_t j = 1; j < 10; j++)
                if (line[i + j] != 'r' && line[i + j] != 'w' && line[i + j] != 'x' && line[i + j] != '-')
                    mode = false;
            if (mode) { modePos = i; break; }
        }
        if (modePos == std::string::npos)
            continue;
        std::istringstream row(line.substr(modePos));
        std::string mode, size, date, time;
        if (!(row >> mode >> size >> date >> time))
            continue;
        std::string name;
        std::getline(row, name);
        name = trim(name);
        if (name == "." || name == "../" || name == "./" || name.empty())
            continue;
        bool isDir = !mode.empty() && mode[0] == 'd';
        if (isDir && !name.empty() && name.back() == '/')
            name.pop_back();
        std::cout << "PFS\t" << (isDir ? "D" : "F") << "\t" << size << "\t"
                  << cleanField(name) << "\n";
    }
}

struct ManifestEntry
{
    bool directory = false;
    std::string local;
    std::string remote;
};

std::vector<ManifestEntry> readCopyManifest(const std::string &path)
{
    std::ifstream in(path);
    if (!in)
        throw std::runtime_error("Unable to open PFS copy manifest: " + path);
    std::vector<ManifestEntry> result;
    std::string line;
    while (std::getline(in, line))
    {
        if (line.empty() || line[0] == '#')
            continue;
        const std::size_t first = line.find('\t');
        if (first == std::string::npos)
            throw std::runtime_error("Malformed PFS copy manifest line.");
        const std::string type = line.substr(0, first);
        if (type == "D")
        {
            result.push_back({ true, {}, normalizedPfsPath(line.substr(first + 1)) });
        }
        else if (type == "F")
        {
            const std::size_t second = line.find('\t', first + 1);
            if (second == std::string::npos)
                throw std::runtime_error("Malformed PFS file manifest line.");
            const std::string local = line.substr(first + 1, second - first - 1);
            const std::string remote = normalizedPfsPath(line.substr(second + 1));
            if (!fs::is_regular_file(local))
                throw std::runtime_error("PFS source file does not exist: " + local);
            result.push_back({ false, local, remote });
        }
        else
            throw std::runtime_error("Unknown PFS copy manifest entry type.");
    }
    if (result.empty())
        throw std::runtime_error("PFS copy manifest contains no work.");
    return result;
}

std::vector<std::string> pfsComponents(const std::string &path)
{
    std::vector<std::string> out;
    std::stringstream ss(path);
    std::string item;
    while (std::getline(ss, item, '/'))
        if (!item.empty())
            out.push_back(item);
    return out;
}

void appendEnsureDirectory(std::string &script, const std::string &remoteDirectory)
{
    script += "cd /\n";
    for (const std::string &component : pfsComponents(remoteDirectory))
    {
        script += "mkdir " + quotePfsshellToken(component) + "\n";
        script += "cd " + quotePfsshellToken(component) + "\n";
    }
}

void copyManifestToPfs(const Options &options)
{
    const std::string partition = resolvePfsPartition(options);
    const std::vector<ManifestEntry> entries = readCopyManifest(options.copyManifest);
    std::string script = "device " + bankedDevicePath(options, 0) + "\nmount " + partition + "\n";

    // pfsshell's `put` command uses one filename for both the host source and
    // PFS destination. For a manifest entry that renames while copying (for
    // example our temporary file -> conf_opl.cfg), expose the source through a
    // temporary symlink whose basename already matches the requested remote
    // filename. This lets `put` replace the final name directly and avoids a
    // rename-over-existing-file edge case.
    fs::path aliasRoot;
    auto ensureAliasRoot = [&]() -> const fs::path & {
        if (aliasRoot.empty()) {
            char pattern[] = "/tmp/ps2-hdd-pfs-alias-XXXXXX";
            char *created = mkdtemp(pattern);
            if (created == nullptr)
                throw std::runtime_error("Unable to create temporary PFS alias directory: " +
                        std::string(std::strerror(errno)));
            aliasRoot = created;
        }
        return aliasRoot;
    };

    try {
        std::size_t index = 0;
        for (const ManifestEntry &entry : entries)
        {
            if (entry.directory)
            {
                appendEnsureDirectory(script, entry.remote);
                continue;
            }
            const fs::path local(entry.local);
            const fs::path remote(entry.remote);
            const std::string remoteParent = remote.parent_path().generic_string().empty() ? "/" : remote.parent_path().generic_string();
            appendEnsureDirectory(script, remoteParent);

            fs::path effectiveLocal = local;
            if (local.filename() != remote.filename())
            {
                const fs::path aliasDir = ensureAliasRoot() / std::to_string(index++);
                fs::create_directories(aliasDir);
                effectiveLocal = aliasDir / remote.filename();
                std::error_code error;
                fs::create_symlink(fs::absolute(local), effectiveLocal, error);
                if (error)
                    throw std::runtime_error("Unable to stage PFS destination filename " +
                            remote.filename().string() + ": " + error.message());
            }

            script += "lcd " + quotePfsshellToken(effectiveLocal.parent_path().string()) + "\n";
            script += "put " + quotePfsshellToken(effectiveLocal.filename().string()) + "\n";
        }
        script += "cd /\numount\nexit\n";
        runPfsshell(options.pfsshell, script);
    }
    catch (...)
    {
        if (!aliasRoot.empty()) {
            std::error_code ignored;
            fs::remove_all(aliasRoot, ignored);
        }
        throw;
    }
    if (!aliasRoot.empty()) {
        std::error_code ignored;
        fs::remove_all(aliasRoot, ignored);
    }
    std::cout << "PFS_COPY_SUCCESS\t" << partition << "\t" << entries.size() << "\n";
}

void ensureCoverArtConfig(const Options &options)
{
    const std::string partition = resolvePfsPartition(options);
    const std::string base = normalizedPfsPath(options.pfsPath);
    TemporaryDirectory temp;
    const fs::path localConfig = temp.path() / "conf_opl.cfg";

    // Read the current config if it exists. A missing config is fine: OPL will
    // accept a minimal config containing only the key we need to set.
    try
    {
        std::string readScript = "device " + bankedDevicePath(options, 0) + "\nmount " + partition + "\n";
        readScript += "cd " + quotePfsshellToken(base) + "\n";
        readScript += "lcd " + quotePfsshellToken(temp.path().string()) + "\n";
        readScript += "get conf_opl.cfg\ncd /\numount\nexit\n";
        runPfsshell(options.pfsshell, readScript);
    }
    catch (const std::exception &)
    {
        // Treat only the absence/unreadability of this optional file as an
        // empty configuration. The subsequent write still goes through a
        // verified APA/PFS mount and will fail loudly if storage is unhealthy.
    }

    std::string text;
    if (fs::is_regular_file(localConfig))
    {
        std::ifstream input(localConfig, std::ios::binary);
        text.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }

    const std::string rebuilt = Ps2::OplConfig::SetKey(text, "enable_coverart", "1");

    {
        std::ofstream output(localConfig, std::ios::binary | std::ios::trunc);
        if (!output)
            throw std::runtime_error("Unable to stage patched conf_opl.cfg.");
        output.write(rebuilt.data(), static_cast<std::streamsize>(rebuilt.size()));
        if (!output)
            throw std::runtime_error("Writing patched conf_opl.cfg failed.");
    }

    std::string writeScript = "device " + bankedDevicePath(options, 0) + "\nmount " + partition + "\n";
    appendEnsureDirectory(writeScript, base);
    writeScript += "lcd " + quotePfsshellToken(temp.path().string()) + "\n";
    writeScript += "put conf_opl.cfg\ncd /\numount\nexit\n";
    runPfsshell(options.pfsshell, writeScript);
    std::cout << "OPL_COVER_ART_ENABLED\t" << partition << "\t" << base << "/conf_opl.cfg\n";
}

void copyRequired(const fs::path &source, const fs::path &destination)
{
    std::error_code error;
    if (!fs::is_regular_file(source, error) || fs::file_size(source, error) == 0)
        throw std::runtime_error("Required provisioning file is missing: " + source.string());
    fs::copy_file(source, destination, fs::copy_options::overwrite_existing, error);
    if (error)
        throw std::runtime_error("Unable to stage " + source.string() + ": " + error.message());
}

void writeText(const fs::path &path, const std::string &text)
{
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream || !stream.write(text.data(), static_cast<std::streamsize>(text.size())))
        throw std::runtime_error("Unable to create staging file: " + path.string());
}

void stageProvisionPayloads(const Options &options, const fs::path &staging)
{
    const fs::path root(options.payloadDirectory);
    const Ps2::PayloadStatus status = Ps2::Ps2HddFormat::ValidateProvisionPayload(
            options.payloadDirectory, options.provision);
    if (!status.valid)
    {
        std::string message = status.message;
        for (const std::string &missing : status.missingFiles)
            message += "\n  missing: " + missing;
        throw std::runtime_error(message);
    }

    if (options.provision.installOpl)
        copyRequired(root / "opl/OPNPS2LD.ELF", staging / "OPNPS2LD.ELF");
    if (options.provision.installWlaunchElf)
        copyRequired(root / "wle/BOOT.ELF", staging / "WLE_BOOT.ELF");
    if (options.provision.installHddBootEnabler)
        copyRequired(root / "fhdb-enabler/FHDB-Boot-Config.ELF", staging / "FHDB_BOOT_CONFIG.ELF");
    if (options.provision.installMemoryCardAnnihilator)
        copyRequired(root / "mca/BOOT.ELF", staging / "MCA_BOOT.ELF");
    if (options.provision.installFhdb)
    {
        for (const char *file : { "MBR.XLF", "FHDB.XLF", "ENDVDPL.XRX", "FMCB_CFG.ELF",
                "USBD.IRX", "USBHDFSD.IRX" })
            copyRequired(root / "fhdb" / file, staging / file);
    }

    if (options.provision.needsAppsPartition())
    {
        writeText(staging / "conf_hdd.cfg", "hdd_partition=PP.FHDB.APPS\n");
        if (options.provision.installWlaunchElf)
            writeText(staging / "title_wle.cfg", "title=wLaunchELF ISR\nboot=BOOT.ELF\n");
        if (options.provision.installMemoryCardAnnihilator)
            writeText(staging / "title_mca.cfg",
                    "title=Memory Card Annihilator\nboot=BOOT.ELF\n");
        if (options.provision.installHddBootEnabler)
            writeText(staging / "title_fhdb_enabler.cfg",
                    "title=FHDB HDD Boot Configuration\nboot=BOOT.ELF\n");
        if (options.provision.configureOplPlugAndPlay)
        {
            Ps2::OplPresetOptions preset;
            preset.enableApps = options.provision.installWlaunchElf ||
                    options.provision.installMemoryCardAnnihilator ||
                    options.provision.installHddBootEnabler;
            // Deliberately leave exit_path unset here. Current OPL's in-game reset
            // loader does not initialize HDD/PFS before loading a custom exit ELF,
            // so a direct hdd0:...:pfs:/OPL path is not a safe default.
            writeText(staging / "conf_opl.cfg", Ps2::OplConfig::BuildInternalHddPreset(preset));
        }
    }

    if (options.provision.installFhdb)
    {
        Ps2::FhdbConfig config = Ps2::FhdbConfig::CreateDefault();
        std::vector<Ps2::FhdbMenuItem> menu;
        if (options.provision.installOpl)
        {
            const std::string oplPath = "hdd0:PP.FHDB.APPS:pfs:/OPL/OPNPS2LD.ELF";
            config.SetValue("LK_Auto_E1", oplPath);
            config.SetValue("LK_L1_E1", oplPath);
            menu.push_back({ 1, "Open PS2 Loader", oplPath, "", "" });
        }
        else
        {
            config.SetValue("LK_Auto_E1", "OSDSYS");
            config.SetValue("LK_L1_E1", "OSDSYS");
        }
        config.SetValue("LK_Auto_E2", "OSDSYS");
        config.SetValue("LK_Auto_E3", "OSDSYS");

        if (options.provision.installWlaunchElf)
        {
            const std::string wlePath = "hdd0:__sysconf:pfs:/FMCB/BOOT.ELF";
            config.SetValue("LK_R1_E1", wlePath);
            menu.push_back({ 2, "wLaunchELF ISR", wlePath, "", "" });
        }
        if (options.provision.installHddBootEnabler)
        {
            const std::string enablePath =
                    "hdd0:PP.FHDB.APPS:pfs:/OPL/APPS/FHDB-HDD-Boot-Config/BOOT.ELF";
            menu.push_back({ 3, "FHDB HDD Boot Configuration", enablePath, "", "" });
        }
        menu.push_back({ 98, "Free HDBoot Configurator",
                "hdd0:__sysconf:pfs:/FMCB/FMCB_CFG.ELF", "", "" });
        menu.push_back({ 99, "Restart System", "OSDSYS", "", "" });
        menu.push_back({ 100, "Shutdown System", "POWEROFF", "", "" });
        config.SetMenuItems(menu);
        writeText(staging / "FREEHDB.CNF", config.Serialize());
    }
}
#endif

Ps2::PhysicalDiskCandidate requeryTarget(const Options &options)
{
    for (const Ps2::PhysicalDiskCandidate &disk : Ps2::PhysicalDiskScanner::Scan())
        if (disk.devicePath == options.device)
            return disk;
    throw std::runtime_error("Target disk disappeared before the destructive operation.");
}

void validateBackendResources(const Options &options)
{
    if (!options.pfsshell.empty())
    {
        if (!fs::is_regular_file(options.pfsshell) && !fs::is_symlink(options.pfsshell))
            throw std::runtime_error("pfsshell executable was not found: " + options.pfsshell);
#ifdef __linux__
        if (access(options.pfsshell.c_str(), X_OK) != 0)
            throw std::runtime_error("pfsshell is not executable: " + options.pfsshell);
#endif
    }
    if (!options.hdlDump.empty())
    {
        if (!fs::is_regular_file(options.hdlDump) && !fs::is_symlink(options.hdlDump))
            throw std::runtime_error("hdl_dump executable was not found: " + options.hdlDump);
#ifdef __linux__
        if (access(options.hdlDump.c_str(), X_OK) != 0)
            throw std::runtime_error("hdl_dump is not executable: " + options.hdlDump);
#endif
        if (!options.installGame.empty() && !fs::is_regular_file(options.installGame))
            throw std::runtime_error("Game image was not found: " + options.installGame);
    }
    if (!options.copyManifest.empty() && !fs::is_regular_file(options.copyManifest))
        throw std::runtime_error("PFS copy manifest was not found: " + options.copyManifest);
    if (options.provision.any())
    {
        const Ps2::PayloadStatus status = Ps2::Ps2HddFormat::ValidateProvisionPayload(
                options.payloadDirectory, options.provision);
        if (!status.valid)
        {
            std::string message = status.message;
            for (const std::string &missing : status.missingFiles)
                message += "\n  missing: " + missing;
            throw std::runtime_error(message);
        }
    }
}

void preflight(const Options &options)
{
#ifndef __linux__
    (void)options;
    throw std::runtime_error("The physical formatter is currently Fedora/Linux only.");
#else
    if (geteuid() != 0)
        throw std::runtime_error("The PS2 HDD writer must run through pkexec/root.");
    if (!isFirstHardwareTestDevice(options.device))
        throw std::runtime_error("Formatter only accepts whole /dev/sdX or /dev/hdX disks.");
    validateBackendResources(options);

    const bool formatRequest = options.installGame.empty() && !options.listGames && !options.listPfs &&
            !options.ensureCoverArt && options.copyManifest.empty() && options.smokeImage.empty();

    const Ps2::PhysicalDiskCandidate disk = requeryTarget(options);
    if (!disk.inspectionError.empty())
        throw std::runtime_error("Disk re-inspection failed: " + disk.inspectionError);
    if (disk.size != options.expectedSize)
        throw std::runtime_error("REFUSED: target disk capacity changed since confirmation.");
    if (disk.logicalSectorSize != 512)
        throw std::runtime_error("REFUSED: PS2 APA formatter requires 512-byte logical sectors.");
    if (disk.readOnly)
        throw std::runtime_error("REFUSED: target disk is read-only.");
    if (!disk.systemDiskCheckAvailable)
        throw std::runtime_error("REFUSED: system-disk safety check is unavailable.");
    if (disk.containsSystemVolume || disk.containsBootPartition)
        throw std::runtime_error("REFUSED: target is part of the running system disk.");
    if (disk.busType == "Virtual" || disk.busType == "File-backed virtual" ||
            disk.busType == "Storage Spaces")
        throw std::runtime_error("REFUSED: virtual/Storage Spaces disk target.");

    if (targetTreeIsSwap(options.device))
        throw std::runtime_error("REFUSED: target device or one of its partitions is active swap.");
    if (targetTreeHasKernelHolders(options.device))
        throw std::runtime_error("REFUSED: target device or one of its partitions is in use by a kernel block-device holder.");

    if (!formatRequest)
    {
        if (targetTreeIsMounted(options.device))
            throw std::runtime_error("REFUSED: target device or one of its partitions is currently mounted.");
        if (disk.partitionCount != 0)
            throw std::runtime_error("REFUSED: target has host-visible partitions and is not in a formatting operation.");
    }

    const std::uint64_t sectors = disk.size / Ps2::HddLayoutPlanner::SectorSize;
    if (formatRequest && sectors > Ps2::HddLayoutPlanner::MaximumApaSectorCount && !options.extendedBanks)
        throw std::runtime_error("REFUSED: disks above the standard APA limit require Extended APA Banks mode.");
#endif
}

#ifdef __linux__
void runImageSmokeTest(const Options &options)
{
    validateBackendResources(options);
    if (fs::exists(options.smokeImage))
        throw std::runtime_error("Smoke-test image path already exists; refusing to overwrite it.");

    constexpr std::uint64_t SmokeImageSize = 2000398934016ULL;
    const int descriptor = open(options.smokeImage.c_str(), O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600);
    if (descriptor < 0)
        throw std::runtime_error("Creating smoke-test image failed: " +
                std::string(std::strerror(errno)));
    if (ftruncate(descriptor, static_cast<off_t>(SmokeImageSize)) != 0)
    {
        const std::string message = std::strerror(errno);
        close(descriptor);
        throw std::runtime_error("Sizing smoke-test image failed: " + message);
    }
    close(descriptor);

    std::cout << "STAGE: Fast-initializing standard APA/PFS test image...\n" << std::flush;
    runPfsshell(options.pfsshell, Ps2::Ps2HddFormat::BuildStandardFormatScript(options.smokeImage));
    std::cout << "STAGE: Verifying complete APA chain...\n" << std::flush;
    Ps2::Ps2HddFormat::VerifyStandardApaDisk(options.smokeImage, SmokeImageSize);
    std::cout << "STAGE: Mount-verifying all four standard PFS system partitions...\n" << std::flush;
    runPfsshell(options.pfsshell, Ps2::Ps2HddFormat::BuildStandardVerifyScript(options.smokeImage));

    std::cout << "PS2 HDD Writer: 2TB IMAGE SMOKE TEST SUCCESS. "
                 "Standard APA and all four system PFS partitions verified.\n";
}
#endif

} // namespace

int main(int argc, char **argv)
{
    try
    {
#ifdef __linux__
        if (isSessionInvocation(argc, argv))
            return runPrivilegedSession(argc, argv);
#endif
        const Options options = parseOptions(argc, argv);
#ifdef __linux__
        if (options.listGames)
        {
            std::cout << "STAGE: Safely reading installed HDL game table across APA banks...\n" << std::flush;
            preflight(options);
            Ps2::Ps2HddFormat::VerifyStandardApaDisk(options.device, options.expectedSize);
            const std::uint32_t count = plannedBankCount(options);
            const int first = options.bank >= 0 ? options.bank : 0;
            const int last = options.bank >= 0 ? options.bank + 1 : static_cast<int>(count);
            for (int bank = first; bank < last; ++bank)
            {
                if (!bankHasValidMbr(options, static_cast<std::uint32_t>(bank))) continue;
                emitGameRecords(runHdlDumpToc(options, bank), bank);
            }
            std::cout << "PS2 HDD Writer: GAME LIST SUCCESS\n";
            return 0;
        }
        if (options.listPfs)
        {
            std::cout << "STAGE: Safely reading OPL PFS directory...\n" << std::flush;
            preflight(options);
            Ps2::Ps2HddFormat::VerifyStandardApaDisk(options.device, options.expectedSize);
            emitPfsRecords(options);
            std::cout << "PS2 HDD Writer: PFS LIST SUCCESS\n";
            return 0;
        }
        if (options.ensureCoverArt)
        {
            std::cout << "STAGE: Re-checking target safety before OPL configuration patch...\n" << std::flush;
            preflight(options);
            Ps2::Ps2HddFormat::VerifyStandardApaDisk(options.device, options.expectedSize);
            std::cout << "STAGE: Enabling OPL cover art while preserving existing settings...\n" << std::flush;
            ensureCoverArtConfig(options);
            std::cout << "PS2 HDD Writer: OPL COVER ART CONFIG SUCCESS\n";
            return 0;
        }
        if (!options.copyManifest.empty())
        {
            std::cout << "STAGE: Re-checking target safety for PFS file copy...\n" << std::flush;
            preflight(options);
            Ps2::Ps2HddFormat::VerifyStandardApaDisk(options.device, options.expectedSize);
            std::cout << "STAGE: Copying files into OPL PFS storage...\n" << std::flush;
            copyManifestToPfs(options);
            std::cout << "PS2 HDD Writer: PFS COPY SUCCESS\n";
            return 0;
        }
        if (!options.installGame.empty())
        {
            std::cout << "STAGE: Re-checking target identity and safety for HDL game install...\n" << std::flush;
            preflight(options);
            const int bank = options.bank < 0 ? 0 : options.bank;
            std::cout << "STAGE: Verifying APA Bank " << bank << " before HDL game install...\n" << std::flush;
            verifyGameBank(options, bank);
            std::cout << "STAGE: Installing " << options.gameName << " as "
                      << (options.media == "cd" ? "CD" : "DVD") << " HDL game into Bank " << bank << "...\n" << std::flush;
            runHdlDumpInstall(options);
            std::cout << "STAGE: Re-reading HDL game table in Bank " << bank << "...\n" << std::flush;
            const std::string toc = runHdlDumpToc(options, bank);
            if (toc.find(options.gameName) == std::string::npos)
                throw std::runtime_error("HDL install completed but the new game was not found in hdl_toc verification.");
            std::cout << "PS2 HDD Writer: GAME INSTALL SUCCESS: " << options.gameName << "\n";
            return 0;
        }
        if (!options.smokeImage.empty())
        {
            std::cout << "PS2 HDD Writer: running non-hardware image smoke test...\n";
            runImageSmokeTest(options);
            return 0;
        }
#endif
        std::cout << "STAGE: Re-checking target identity and safety...\n" << std::flush;
        preflight(options);
#ifdef __linux__
        prepareTargetForFormat(options);
        TemporaryDirectory staging;
        if (options.provision.any())
        {
            std::cout << "STAGE: Staging selected OPL/FHDB application payloads...\n" << std::flush;
            stageProvisionPayloads(options, staging.path());
        }

        const std::string bank0Device = bankedDevicePath(options, 0);
        std::cout << "STAGE: Fast-initializing conventional APA/PFS Bank 0...\n" << std::flush;
        runPfsshell(options.pfsshell, Ps2::Ps2HddFormat::BuildStandardFormatScript(bank0Device));

        const Ps2::PhysicalDiskCandidate afterFormat = requeryTarget(options);
        if (!afterFormat.systemDiskCheckAvailable || afterFormat.size != options.expectedSize ||
                afterFormat.logicalSectorSize != 512 || afterFormat.readOnly ||
                afterFormat.containsSystemVolume || afterFormat.containsBootPartition ||
                targetTreeIsMounted(options.device) || targetTreeIsSwap(options.device) ||
                targetTreeHasKernelHolders(options.device))
            throw std::runtime_error("REFUSED: target identity or safety state changed after APA formatting.");

        std::cout << "STAGE: Verifying fresh Bank-0 APA partition chain...\n" << std::flush;
        Ps2::Ps2HddFormat::VerifyStandardApaDisk(options.device, options.expectedSize);

        if (options.extendedBanks)
        {
            const auto layout = Ps2::HddLayoutPlanner::Plan(options.expectedSize, Ps2::HddLayoutMode::ExtendedApaBanks);
            if (!layout.valid || layout.banks.size() < 2) throw std::runtime_error("Extended mode has no upper APA bank.");
            for (std::size_t i = 1; i < layout.banks.size(); ++i)
            {
                std::cout << "STAGE: Initializing games-only APA Bank " << i << "...\n" << std::flush;
                Ps2::ApaBank::InitializeGamesOnly(options.device, options.expectedSize, static_cast<std::uint32_t>(i), true);
                const auto chain = Ps2::Apa::ReadPartitionChain(options.device, layout.banks[i].baseSector, layout.banks[i].addressableSectorCount, 256);
                if (chain.empty() || chain.front().header.id != "__mbr") throw std::runtime_error("Upper APA bank verification failed.");
            }
        }

        if (options.provision.any())
        {
            std::cout << "STAGE: Creating OPL/FHDB folders and copying selected applications...\n" << std::flush;
            runPfsshell(options.pfsshell,
                    Ps2::Ps2HddFormat::BuildProvisionScript(bank0Device,
                            staging.path().string(), options.provision));

            if (options.provision.installFhdb)
            {
                std::cout << "STAGE: Installing and verifying FreeHDBoot MBR KELF...\n" << std::flush;
                Ps2::Ps2HddFormat::InstallMbrPayload(options.device,
                        (staging.path() / "MBR.XLF").string());
            }

            std::cout << "STAGE: Verifying provisioned PFS folders...\n" << std::flush;
            runPfsshell(options.pfsshell,
                    Ps2::Ps2HddFormat::BuildProvisionVerifyScript(bank0Device,
                            options.provision));
        }

        std::cout << "STAGE: Final PFS mount verification...\n" << std::flush;
        runPfsshell(options.pfsshell,
                Ps2::Ps2HddFormat::BuildStandardVerifyScript(bank0Device));

        std::cout << "PS2 HDD Writer: SUCCESS. " << (options.extendedBanks ? "Extended banked APA + Bank-0 PFS format verified" : "Standard APA/PFS format verified");
        if (options.provision.any())
            std::cout << " and selected OPL/FHDB applications provisioned";
        std::cout << ".\n";
        if (options.provision.installFhdb)
            std::cout << "NOTE: FHDB files are installed, but the PS2 EEPROM HDD-boot setting must be enabled once on each console.\n";
        return 0;
#else
        throw std::runtime_error("Physical formatting is not available on this platform.");
#endif
    }
    catch (const std::exception &error)
    {
        std::cerr << "PS2 HDD Writer: FAILED: " << error.what() << "\n";
        return 1;
    }
    catch (const std::string &error)
    {
        std::cerr << "PS2 HDD Writer: FAILED: " << error << "\n";
        return 1;
    }
}
