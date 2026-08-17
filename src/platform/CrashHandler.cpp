// File: src/platform/CrashHandler.cpp
#include "CrashHandler.hpp"
#include "common/core/Log.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <exception>
#include <filesystem>

#if defined(_WIN32)
  #include <windows.h>
#else
  #include <csignal>
  #include <fcntl.h>
  #include <unistd.h>
  #if defined(__APPLE__) || defined(__linux__)
    #include <execinfo.h>
  #endif
#endif

namespace Platform {

    namespace {

        // ── Everything the handler touches is preallocated ──────────────────
        // A signal handler may only call async-signal-safe functions. malloc,
        // std::string, snprintf and the whole of stdio are NOT on that list —
        // and a SIGSEGV inside the allocator (a real possibility, since heap
        // corruption is a common cause of the crash in the first place) would
        // deadlock or re-crash on the way to writing the report.
        //
        // So: the full path, the version banner and the scratch buffer are all
        // fixed-size statics filled during Install, and the handler itself uses
        // only open/write/close plus backtrace_symbols_fd (which is explicitly
        // documented as not calling malloc, unlike backtrace_symbols).
        constexpr size_t kPathMax    = 1024;
        constexpr size_t kBannerMax  = 256;
        constexpr size_t kLogDumpMax = 128 * 1024;

        char g_reportPath[kPathMax] = {0};
        char g_banner[kBannerMax]   = {0};
        char g_logDump[kLogDumpMax];

        // One report per process. A crash inside the handler must not loop.
        std::atomic<bool> g_handled{false};

#if !defined(_WIN32)
        // SIGTRAP belongs here even though it is not a "fault" in the usual
        // sense. On modern macOS both libmalloc (heap corruption, double free,
        // bad pointer) and libc++ hardening (out-of-range operator[], empty
        // optional dereference) raise it via __builtin_trap() rather than
        // abort(). Without it, that whole class of bug produces a bare
        // "exit code 133" and no report at all — which is exactly how it was
        // found.
        constexpr int kSignals[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP };
        constexpr size_t kSignalCount = sizeof(kSignals) / sizeof(kSignals[0]);
        struct sigaction g_previous[kSignalCount];

        // write(2) can return short. Looping is the only correct use.
        void WriteAll(int fd, const char* data, size_t len) {
            size_t off = 0;
            while (off < len) {
                const ssize_t n = ::write(fd, data + off, len - off);
                if (n <= 0) return;
                off += static_cast<size_t>(n);
            }
        }
        void WriteStr(int fd, const char* s) { WriteAll(fd, s, ::strlen(s)); }

        // Async-signal-safe unsigned -> decimal. snprintf is not on the safe
        // list, and this is the only formatting the handler needs.
        void WriteUnsigned(int fd, unsigned long long v) {
            char buf[32];
            int i = sizeof(buf);
            buf[--i] = '\0';
            if (v == 0) buf[--i] = '0';
            while (v > 0 && i > 0) { buf[--i] = static_cast<char>('0' + (v % 10)); v /= 10; }
            WriteStr(fd, buf + i);
        }

        const char* SignalName(int sig) {
            switch (sig) {
                case SIGSEGV: return "SIGSEGV (invalid memory access)";
                case SIGBUS:  return "SIGBUS (bad memory alignment or mapping)";
                case SIGILL:  return "SIGILL (illegal instruction)";
                case SIGFPE:  return "SIGFPE (arithmetic error)";
                case SIGABRT: return "SIGABRT (abort — often an uncaught C++ exception or assert)";
                case SIGTRAP: return "SIGTRAP (trap — heap corruption, or a libc++ hardening check)";
                default:      return "unknown signal";
            }
        }

        // si_addr only carries meaning for the memory/arithmetic faults; for
        // SIGABRT it is whatever was left in the struct.
        bool SignalHasFaultAddress(int sig) {
            return sig == SIGSEGV || sig == SIGBUS || sig == SIGILL || sig == SIGFPE;
        }

        void WriteReport(int fd, const char* reason, bool hasAddr, void* faultAddr) {
            WriteStr(fd, "=== ObeyCraft crash report ===\n");
            WriteStr(fd, g_banner);
            WriteStr(fd, "Unix time: ");
            WriteUnsigned(fd, static_cast<unsigned long long>(::time(nullptr)));
            WriteStr(fd, "\nReason: ");
            WriteStr(fd, reason);
            if (hasAddr) {
                // Printed even when it is 0 — a null dereference is the single
                // most common crash and "address 0" is exactly what identifies
                // it, so treating 0 as "no address" would hide the good case.
                WriteStr(fd, "\nFault address: ");
                WriteUnsigned(fd, reinterpret_cast<unsigned long long>(faultAddr));
            }
            WriteStr(fd, "\n\n--- Backtrace ---\n");

#if defined(__APPLE__) || defined(__linux__)
            void* frames[64];
            const int n = ::backtrace(frames, 64);
            // _fd variant on purpose: backtrace_symbols() allocates.
            ::backtrace_symbols_fd(frames, n, fd);
#else
            WriteStr(fd, "(not available on this platform)\n");
#endif

            WriteStr(fd, "\n--- Recent log ---\n");
            const size_t used = Log::CopyRecentLines(g_logDump, sizeof(g_logDump));
            WriteAll(fd, g_logDump, used);
            WriteStr(fd,
                "\n--- End of report ---\n"
                "The full session log is in the logs/ folder next to this file.\n");
        }

        void SignalHandler(int sig, siginfo_t* info, void* /*context*/) {
            if (!g_handled.exchange(true)) {
                const int fd = ::open(g_reportPath,
                                      O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (fd >= 0) {
                    const bool hasAddr = info && SignalHasFaultAddress(sig);
                    WriteReport(fd, SignalName(sig), hasAddr,
                                hasAddr ? info->si_addr : nullptr);
                    ::close(fd);
                }
            }

            // Hand back to whoever was installed before us — Sentry's handler
            // if it uses signals, otherwise the default action. Restoring and
            // re-raising is the portable way to do that AND to get the normal
            // termination status/core dump, rather than returning from a
            // handler whose faulting instruction would simply re-execute.
            for (size_t i = 0; i < kSignalCount; ++i) {
                if (kSignals[i] == sig) {
                    ::sigaction(sig, &g_previous[i], nullptr);
                    break;
                }
            }
            ::raise(sig);
        }
#endif // !_WIN32

        void TerminateHandler() {
            // std::terminate is a normal C++ call, not a signal, so the safety
            // rules above do not apply and stdio is fine here. This is the path
            // an uncaught exception takes — and notably NOT a Mach exception,
            // so this fires even on macOS where crashpad would otherwise win.
            if (!g_handled.exchange(true)) {
                if (std::FILE* f = std::fopen(g_reportPath, "w")) {
                    std::fprintf(f, "=== ObeyCraft crash report ===\n%s", g_banner);
                    std::fprintf(f, "Unix time: %lld\n",
                                 static_cast<long long>(std::time(nullptr)));
                    std::fprintf(f, "Reason: std::terminate — uncaught exception\n");

                    const char* what = "(no exception object)";
                    if (auto ex = std::current_exception()) {
                        try {
                            std::rethrow_exception(ex);
                        } catch (const std::exception& e) {
                            what = e.what();
                        } catch (...) {
                            what = "(non-std exception)";
                        }
                    }
                    std::fprintf(f, "Exception: %s\n\n--- Recent log ---\n", what);
                    const size_t used = Log::CopyRecentLines(g_logDump, sizeof(g_logDump));
                    std::fwrite(g_logDump, 1, used, f);
                    std::fprintf(f, "\n--- End of report ---\n");
                    std::fclose(f);
                }
            }
            std::abort();   // falls into the signal path, which is already latched
        }

#if defined(_WIN32)
        LONG WINAPI SehHandler(EXCEPTION_POINTERS* info) {
            if (!g_handled.exchange(true)) {
                if (std::FILE* f = std::fopen(g_reportPath, "w")) {
                    std::fprintf(f, "=== ObeyCraft crash report ===\n%s", g_banner);
                    std::fprintf(f, "Unix time: %lld\n",
                                 static_cast<long long>(std::time(nullptr)));
                    std::fprintf(f, "Reason: SEH exception 0x%08lX at 0x%p\n\n",
                                 info ? info->ExceptionRecord->ExceptionCode : 0UL,
                                 info ? info->ExceptionRecord->ExceptionAddress : nullptr);
                    std::fprintf(f, "--- Recent log ---\n");
                    const size_t used = Log::CopyRecentLines(g_logDump, sizeof(g_logDump));
                    std::fwrite(g_logDump, 1, used, f);
                    std::fprintf(f, "\n--- End of report ---\n");
                    std::fclose(f);
                }
            }
            return EXCEPTION_CONTINUE_SEARCH;   // let Sentry/WER also see it
        }
#endif

    } // namespace

    const char* CrashReportPath() { return g_reportPath; }

    void InstallCrashHandler(const std::string& crashDir, const std::string& version) {
        std::error_code ec;
        std::filesystem::create_directories(crashDir, ec);

        // Build the path ONCE, here, where formatting is legal. The handler
        // must not construct it (see the safety note above), and one report per
        // session is the realistic case anyway.
        std::time_t t = std::time(nullptr);
        std::tm tmv{};
#if defined(_WIN32)
        localtime_s(&tmv, &t);
#else
        localtime_r(&t, &tmv);
#endif
        char stamp[64];
        std::strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H-%M-%S", &tmv);
        std::snprintf(g_reportPath, sizeof(g_reportPath), "%s/crash-%s.txt",
                      crashDir.c_str(), stamp);
        std::snprintf(g_banner, sizeof(g_banner), "Version: %s\n", version.c_str());

#if !defined(_WIN32)
        struct sigaction sa;
        std::memset(&sa, 0, sizeof(sa));
        sa.sa_sigaction = &SignalHandler;
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK;   // ONSTACK: a stack overflow
        sigemptyset(&sa.sa_mask);                // still needs somewhere to run
        for (size_t i = 0; i < kSignalCount; ++i) {
            ::sigaction(kSignals[i], &sa, &g_previous[i]);
        }
#else
        ::SetUnhandledExceptionFilter(&SehHandler);
#endif
        std::set_terminate(&TerminateHandler);

        Log::Info("Crash handler installed (report path: %s)", g_reportPath);
    }

} // namespace Platform
