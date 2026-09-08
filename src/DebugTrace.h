#ifndef PS2_HDD_DEBUG_TRACE_H
#define PS2_HDD_DEBUG_TRACE_H

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#ifdef __linux__
#include <execinfo.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace DebugTrace
{
inline std::mutex logMutex;
inline std::string activeLogPath;
inline int crashFd = -1;
inline volatile std::sig_atomic_t handlingSignal = 0;

inline long threadId()
{
#ifdef __linux__
    return static_cast<long>(::syscall(SYS_gettid));
#else
    return 0;
#endif
}

inline std::string defaultLogPath()
{
    if (const char *configured = std::getenv("PS2_HDD_DEBUG_LOG"))
        if (*configured != '\0')
            return configured;

    const char *home = std::getenv("HOME");
    return std::string(home && *home ? home : "/tmp") +
            "/Downloads/ps2-hdd-manager-debug-trace.log";
}

inline std::string timestamp()
{
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto tt = system_clock::to_time_t(now);
    const auto ms = duration_cast<milliseconds>(
            now.time_since_epoch()) % 1000;

    std::tm local{};
#ifdef __linux__
    localtime_r(&tt, &local);
#else
    local = *std::localtime(&tt);
#endif

    std::ostringstream out;
    out << std::put_time(&local, "%Y-%m-%d %H:%M:%S")
        << '.' << std::setfill('0') << std::setw(3) << ms.count();
    return out.str();
}

inline void initialize()
{
    std::lock_guard<std::mutex> lock(logMutex);

    if (!activeLogPath.empty())
        return;

    activeLogPath = defaultLogPath();

#ifdef __linux__
    crashFd = ::open(
            activeLogPath.c_str(),
            O_WRONLY | O_CREAT | O_APPEND,
            0644);
#endif

    std::ofstream file(
            activeLogPath,
            std::ios::out | std::ios::app);

    file << "\n============================================================\n"
         << "PS2 HDD Manager debug session started: "
         << timestamp() << "\n"
         << "pid=";
#ifdef __linux__
    file << ::getpid();
#else
    file << 0;
#endif
    file << " tid=" << threadId() << "\n"
         << "============================================================\n";
    file.flush();
}

inline const std::string &path()
{
    if (activeLogPath.empty())
        initialize();
    return activeLogPath;
}

inline void write(const std::string &message)
{
    if (activeLogPath.empty())
        initialize();

    std::lock_guard<std::mutex> lock(logMutex);
    std::ofstream file(
            activeLogPath,
            std::ios::out | std::ios::app);

    file << timestamp()
         << " pid=";
#ifdef __linux__
    file << ::getpid();
#else
    file << 0;
#endif
    file << " tid=" << threadId()
         << " | " << message << '\n';
    file.flush();
}

inline void dumpStackToCrashFd()
{
#ifdef __linux__
    if (crashFd < 0)
        return;

    void *frames[96];
    const int count = ::backtrace(
            frames,
            static_cast<int>(
                sizeof(frames) / sizeof(frames[0])));

    static constexpr char header[] =
            "--- native backtrace ---\n";
    ::write(crashFd, header, sizeof(header) - 1);

    if (count > 0)
        ::backtrace_symbols_fd(
                frames,
                count,
                crashFd);

    static constexpr char footer[] =
            "--- end native backtrace ---\n";
    ::write(crashFd, footer, sizeof(footer) - 1);

    ::fsync(crashFd);
#endif
}

inline const char *signalName(int signal)
{
    switch (signal)
    {
        case SIGSEGV: return "SIGSEGV";
        case SIGABRT: return "SIGABRT";
#ifdef SIGBUS
        case SIGBUS:  return "SIGBUS";
#endif
        case SIGILL:  return "SIGILL";
        case SIGFPE:  return "SIGFPE";
        default:      return "UNKNOWN";
    }
}

inline void crashSignalHandler(int signal)
{
#ifdef __linux__
    if (handlingSignal)
        _exit(128 + signal);
    handlingSignal = 1;

    if (crashFd >= 0)
    {
        static constexpr char prefix[] =
                "\n*** FATAL SIGNAL: ";
        ::write(crashFd, prefix, sizeof(prefix) - 1);

        const char *name = signalName(signal);
        ::write(crashFd, name, std::strlen(name));

        static constexpr char suffix[] =
                " ***\n";
        ::write(crashFd, suffix, sizeof(suffix) - 1);

        dumpStackToCrashFd();
    }

    struct sigaction action {};
    sigemptyset(&action.sa_mask);
    action.sa_handler = SIG_DFL;
    sigaction(signal, &action, nullptr);
    ::kill(::getpid(), signal);
    _exit(128 + signal);
#else
    std::_Exit(128 + signal);
#endif
}

inline void installCrashHandlers()
{
    initialize();

#ifdef __linux__
    struct sigaction action {};
    sigemptyset(&action.sa_mask);
    action.sa_handler = crashSignalHandler;
    action.sa_flags = SA_RESETHAND;

    sigaction(SIGSEGV, &action, nullptr);
    sigaction(SIGABRT, &action, nullptr);
#ifdef SIGBUS
    sigaction(SIGBUS, &action, nullptr);
#endif
    sigaction(SIGILL, &action, nullptr);
    sigaction(SIGFPE, &action, nullptr);
#endif

    std::set_terminate([]() {
        std::string message =
                "*** std::terminate called";

        if (std::exception_ptr current =
                    std::current_exception())
        {
            try
            {
                std::rethrow_exception(current);
            }
            catch (const std::exception &error)
            {
                message += ": ";
                message += error.what();
            }
            catch (...)
            {
                message += ": non-std exception";
            }
        }

        write(message);
        dumpStackToCrashFd();
        std::abort();
    });
}

class Scope
{
public:
    explicit Scope(std::string label)
        : label_(std::move(label))
    {
        write("ENTER " + label_);
    }

    ~Scope()
    {
        write("EXIT  " + label_);
    }

    Scope(const Scope &) = delete;
    Scope &operator=(const Scope &) = delete;

private:
    std::string label_;
};

} // namespace DebugTrace

#define PS2_TRACE_SCOPE(label) \
    DebugTrace::Scope ps2DebugScope_##__LINE__(label)

#endif
