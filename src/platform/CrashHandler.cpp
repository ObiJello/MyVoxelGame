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
  #include <pthread.h>
  #include <sys/mman.h>
  #include <time.h>
  #include <unistd.h>
  #if defined(__APPLE__) || defined(__linux__)
    #include <execinfo.h>
  #endif
  #if defined(__APPLE__)
    #include <sys/ucontext.h>
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

        // One report per process, written by whichever thread claims it
        // first. A crash inside the handler must not loop.
        //
        // Claiming is not enough on its own: when two threads fault at once
        // (two worldgen workers reading the same freed tree, 2026-09-28), the
        // loser used to chain straight to the default action — which kills
        // the whole process while the winner is still inside open(), and the
        // report came out EMPTY. The loser now waits for g_reportDone.
        std::atomic<uint64_t> g_reportOwner{0};   // 0 = unclaimed
        std::atomic<bool>     g_reportDone{false};

        uint64_t CurrentThreadKey() {
#if defined(_WIN32)
            return static_cast<uint64_t>(::GetCurrentThreadId()) + 1;
#else
            // pthread_self is a pointer (macOS) or an integer (Linux); either
            // way it is non-zero and unique among live threads.
            static_assert(sizeof(pthread_t) <= sizeof(uint64_t), "pthread_t wider than 64 bits");
            uint64_t key = 0;
            const pthread_t self = ::pthread_self();
            std::memcpy(&key, &self, sizeof(self));
            return key == 0 ? 1 : key;
#endif
        }

        enum class Claim { Mine, AlreadyMine, Other };

        Claim ClaimReport() {
            const uint64_t self = CurrentThreadKey();
            uint64_t expected = 0;
            if (g_reportOwner.compare_exchange_strong(expected, self)) return Claim::Mine;
            return expected == self ? Claim::AlreadyMine : Claim::Other;
        }

        // Another thread is writing the report: give it time to finish (it
        // chains to the default action itself when done, which ends the
        // process). Bounded, so a writer that is itself stuck cannot hang the
        // crash forever. nanosleep / Sleep are both safe here.
        void WaitForOtherReporter() {
            for (int i = 0; i < 500 && !g_reportDone.load(std::memory_order_acquire); ++i) {
#if defined(_WIN32)
                ::Sleep(10);
#else
                struct timespec ts{0, 10 * 1000 * 1000};
                ::nanosleep(&ts, nullptr);
#endif
            }
        }

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

#if defined(__APPLE__) && (defined(__aarch64__) || defined(__x86_64__))
        // The faulting thread's own frames, walked from the interrupted
        // register state rather than from the handler. backtrace() starts in
        // the handler, which runs on the alternate signal stack, and has to
        // find its way back through _sigtramp; the saved pc/fp lead straight
        // to the fault. For a stack overflow the chain is thousands of frames
        // of the same recursion, so the first kHead and the last kTail
        // (where the recursion was entered) are kept.
        bool WriteContextBacktrace(int fd, void* context) {
            if (!context) return false;
            const ucontext_t* uc = static_cast<const ucontext_t*>(context);
            if (!uc->uc_mcontext) return false;
  #if defined(__aarch64__)
            const auto& ss = uc->uc_mcontext->__ss;
    #if defined(__darwin_arm_thread_state64_get_pc)
            uintptr_t pc = (uintptr_t)(__darwin_arm_thread_state64_get_pc(ss));
            uintptr_t fp = (uintptr_t)(__darwin_arm_thread_state64_get_fp(ss));
    #else
            uintptr_t pc = static_cast<uintptr_t>(ss.__pc);
            uintptr_t fp = static_cast<uintptr_t>(ss.__fp);
    #endif
            constexpr uintptr_t kPacMask = 0x0000000FFFFFFFFFull;   // strip pointer-auth bits
  #else
            const auto& ss = uc->uc_mcontext->__ss;
            uintptr_t pc = static_cast<uintptr_t>(ss.__rip);
            uintptr_t fp = static_cast<uintptr_t>(ss.__rbp);
            constexpr uintptr_t kPacMask = ~uintptr_t{0};
  #endif
            // The thread's stack bounds: a frame pointer outside them ends
            // the walk instead of faulting inside the handler.
            const pthread_t self = ::pthread_self();
            const uintptr_t stackTop = reinterpret_cast<uintptr_t>(::pthread_get_stackaddr_np(self));
            const uintptr_t stackBottom = stackTop - ::pthread_get_stacksize_np(self);

            constexpr int kHead = 48, kTail = 16;
            void* head[kHead];
            void* tail[kTail];
            int headCount = 0;
            long long tailCount = 0;
            auto push = [&](uintptr_t addr) {
                void* frame = reinterpret_cast<void*>(addr & kPacMask);
                if (headCount < kHead) head[headCount++] = frame;
                else tail[(tailCount++) % kTail] = frame;
            };
            push(pc);
            for (int steps = 0; steps < 1000000; ++steps) {
                if (fp == 0 || (fp & 0x7) != 0 || fp < stackBottom || fp + 16 > stackTop) break;
                const uintptr_t* record = reinterpret_cast<const uintptr_t*>(fp);
                const uintptr_t next = record[0];
                const uintptr_t ret = record[1];
                if (ret == 0) break;
                push(ret);
                if (next <= fp) break;   // frames must move toward the stack top
                fp = next;
            }
            ::backtrace_symbols_fd(head, headCount, fd);
            if (tailCount > 0) {
                const long long skipped = tailCount > kTail ? tailCount - kTail : 0;
                if (skipped > 0) {
                    WriteStr(fd, "... ");
                    WriteUnsigned(fd, static_cast<unsigned long long>(skipped));
                    WriteStr(fd, " frames omitted (deep recursion?) ...\n");
                }
                const int n = static_cast<int>(tailCount < kTail ? tailCount : kTail);
                void* ordered[kTail];
                for (int i = 0; i < n; ++i) {
                    ordered[i] = tail[(tailCount - n + i) % kTail];
                }
                ::backtrace_symbols_fd(ordered, n, fd);
            }
            return true;
        }
#else
        bool WriteContextBacktrace(int, void*) { return false; }
#endif

        void WriteReport(int fd, const char* reason, bool hasAddr, void* faultAddr, void* context) {
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
            WriteStr(fd, "\nThread: ");
            WriteUnsigned(fd, static_cast<unsigned long long>(CurrentThreadKey()));
            WriteStr(fd, "\n\n--- Backtrace ---\n");

            if (!WriteContextBacktrace(fd, context)) {
#if defined(__APPLE__) || defined(__linux__)
                void* frames[64];
                const int n = ::backtrace(frames, 64);
                // _fd variant on purpose: backtrace_symbols() allocates.
                ::backtrace_symbols_fd(frames, n, fd);
#else
                WriteStr(fd, "(not available on this platform)\n");
#endif
            }

            WriteStr(fd, "\n--- Recent log ---\n");
            const size_t used = Log::CopyRecentLines(g_logDump, sizeof(g_logDump));
            WriteAll(fd, g_logDump, used);
            WriteStr(fd,
                "\n--- End of report ---\n"
                "The full session log is in the logs/ folder next to this file.\n");
        }

        void SignalHandler(int sig, siginfo_t* info, void* context) {
            switch (ClaimReport()) {
                case Claim::Mine: {
                    const int fd = ::open(g_reportPath,
                                          O_WRONLY | O_CREAT | O_TRUNC, 0644);
                    if (fd >= 0) {
                        const bool hasAddr = info && SignalHasFaultAddress(sig);
                        WriteReport(fd, SignalName(sig), hasAddr,
                                    hasAddr ? info->si_addr : nullptr, context);
                        ::close(fd);
                    }
                    g_reportDone.store(true, std::memory_order_release);
                    break;
                }
                case Claim::AlreadyMine:
                    // This thread already wrote (std::terminate -> abort) or
                    // faulted again while writing: nothing more to add.
                    break;
                case Claim::Other:
                    WaitForOtherReporter();
                    break;
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
            const Claim claim = ClaimReport();
            if (claim == Claim::Other) WaitForOtherReporter();
            if (claim == Claim::Mine) {
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
                g_reportDone.store(true, std::memory_order_release);
            }
            std::abort();   // falls into the signal path, which is already latched
        }

#if defined(_WIN32)
        LONG WINAPI SehHandler(EXCEPTION_POINTERS* info) {
            const Claim claim = ClaimReport();
            if (claim == Claim::Other) WaitForOtherReporter();
            if (claim == Claim::Mine) {
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
                g_reportDone.store(true, std::memory_order_release);
            }
            return EXCEPTION_CONTINUE_SEARCH;   // let Sentry/WER also see it
        }
#endif

#if !defined(_WIN32)
        // One alternate signal stack per thread (see InstallThreadCrashStack).
        // Mapped, not malloc'd, with a PROT_NONE guard page below it so a
        // handler that overran it faults instead of scribbling on the heap.
        struct ThreadCrashStack {
            void*  mapping = nullptr;
            size_t mappingSize = 0;
            void*  stackBase = nullptr;

            void Install() {
                if (mapping) return;
                // A thread that already has an alternate stack keeps it.
                stack_t current{};
                if (::sigaltstack(nullptr, &current) == 0 && !(current.ss_flags & SS_DISABLE)) return;

                const size_t page = static_cast<size_t>(::getpagesize());
                size_t size = 128 * 1024;              // report writer + backtrace_symbols_fd
                if (size < static_cast<size_t>(SIGSTKSZ)) size = static_cast<size_t>(SIGSTKSZ);
                size = (size + page - 1) / page * page;
                void* map = ::mmap(nullptr, size + page, PROT_READ | PROT_WRITE,
                                   MAP_PRIVATE | MAP_ANON, -1, 0);
                if (map == MAP_FAILED) return;
                ::mprotect(map, page, PROT_NONE);      // guard page at the low end

                stack_t ss{};
                ss.ss_sp    = static_cast<char*>(map) + page;
                ss.ss_size  = size;
                ss.ss_flags = 0;
                if (::sigaltstack(&ss, nullptr) != 0) {
                    ::munmap(map, size + page);
                    return;
                }
                mapping = map;
                mappingSize = size + page;
                stackBase = ss.ss_sp;
            }

            ~ThreadCrashStack() {
                if (!mapping) return;
                // Unhook before unmapping, and only if it is still ours.
                stack_t current{};
                if (::sigaltstack(nullptr, &current) == 0 && current.ss_sp == stackBase &&
                    !(current.ss_flags & SS_ONSTACK)) {
                    stack_t off{};
                    off.ss_flags = SS_DISABLE;
                    ::sigaltstack(&off, nullptr);
                    ::munmap(mapping, mappingSize);
                }
                // Otherwise leak it: unmapping a stack the kernel may still
                // deliver onto is worse than 132 KB.
            }
        };
        thread_local ThreadCrashStack t_crashStack;
#endif

    } // namespace

    const char* CrashReportPath() { return g_reportPath; }

    void InstallThreadCrashStack() {
#if !defined(_WIN32)
        t_crashStack.Install();
#endif
    }

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
        // SA_ONSTACK only helps a thread that HAS an alternate stack; this one
        // covers the installing (main) thread, the others call
        // InstallThreadCrashStack from their entry points.
        InstallThreadCrashStack();
#else
        ::SetUnhandledExceptionFilter(&SehHandler);
#endif
        std::set_terminate(&TerminateHandler);

        Log::Info("Crash handler installed (report path: %s)", g_reportPath);
    }

} // namespace Platform
