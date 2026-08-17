// File: src/common/core/Log.cpp
#include "Log.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace Log {

    namespace {
        // ── Signal-safe ring of recent lines ────────────────────────────────
        // Plain static storage, fixed stride, no allocation and no locking, so
        // a signal handler can read it. Everything richer (std::deque, a mutex,
        // std::string) would be unusable at exactly the moment it matters.
        //
        // The write index is atomic only so readers see whole increments; a
        // crash mid-write costs one garbled line, which is a fair trade for
        // being able to read this from a SIGSEGV handler at all.
        constexpr size_t kRingLines = 512;
        constexpr size_t kRingStride = 256;      // bytes per line, NUL-terminated
        char                 g_ring[kRingLines][kRingStride];
        std::atomic<uint64_t> g_ringWrites{0};

        void RingPush(const char* text) {
            const uint64_t seq = g_ringWrites.fetch_add(1, std::memory_order_acq_rel);
            char* dst = g_ring[seq % kRingLines];
            std::snprintf(dst, kRingStride, "%s", text);
        }

        std::FILE*& LogFile() {
            static std::FILE* f = nullptr;
            return f;
        }

        std::string TimestampNow(const char* fmt) {
            std::time_t t = std::time(nullptr);
            std::tm tmv{};
#if defined(_WIN32)
            localtime_s(&tmv, &t);
#else
            localtime_r(&t, &tmv);
#endif
            char buf[64];
            std::strftime(buf, sizeof(buf), fmt, &tmv);
            return buf;
        }

        const char* LevelName(Level level) {
            switch (level) {
                case Level::Debug:   return "DEBUG";
                case Level::Info:    return "INFO";
                case Level::Warning: return "WARN";
                case Level::Error:   return "ERROR";
            }
            return "?";
        }
    } // namespace

    static Level& getCurrentLevel() {
        static Level currentLevel = Level::Debug;
        return currentLevel;
    }

    static std::mutex& getLogMutex() {
        static std::mutex logMutex;
        return logMutex;
    }

    static LogCallback& getCallback() {
        static LogCallback callback;
        return callback;
    }

    static std::mutex& getCallbackMutex() {
        static std::mutex cbMutex;
        return cbMutex;
    }

    void Init() {
        // No-op; the log FILE is opened separately via OpenLogFile once the
        // game directory is known, which is later than this runs.
    }

    bool OpenLogFile(const std::string& path, int keep) {
        namespace fs = std::filesystem;
        std::lock_guard<std::mutex> lock(getLogMutex());
        if (LogFile()) return true;                 // already open

        std::error_code ec;
        const fs::path target(path);
        fs::create_directories(target.parent_path(), ec);

        // Rotate the previous run out of the way, stamped with ITS OWN mtime
        // rather than now — the archive name should say when that session ran,
        // not when the next one started.
        if (fs::exists(target, ec)) {
            std::time_t when = std::time(nullptr);
            const auto ftime = fs::last_write_time(target, ec);
            if (!ec) {
                const auto sys = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                    ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
                when = std::chrono::system_clock::to_time_t(sys);
            }
            std::tm tmv{};
#if defined(_WIN32)
            localtime_s(&tmv, &when);
#else
            localtime_r(&when, &tmv);
#endif
            char stamp[64];
            std::strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H-%M-%S", &tmv);
            fs::rename(target, target.parent_path() / (std::string(stamp) + ".log"), ec);
        }

        // Prune archives, newest `keep` retained. Names sort chronologically by
        // construction, so a lexicographic sort is a date sort.
        if (keep >= 0) {
            std::vector<fs::path> archives;
            for (const auto& e : fs::directory_iterator(target.parent_path(), ec)) {
                if (ec) break;
                if (e.path() == target) continue;
                if (e.path().extension() == ".log") archives.push_back(e.path());
            }
            std::sort(archives.begin(), archives.end());
            if (archives.size() > static_cast<size_t>(keep)) {
                for (size_t i = 0; i + static_cast<size_t>(keep) < archives.size(); ++i) {
                    fs::remove(archives[i], ec);
                }
            }
        }

        LogFile() = std::fopen(path.c_str(), "w");
        if (!LogFile()) return false;
        // Line buffering, so a crash loses at most the line being written
        // rather than everything since the last 4 KB flush. The whole point of
        // this file is to survive an abnormal exit.
        std::setvbuf(LogFile(), nullptr, _IOLBF, 4096);
        return true;
    }

    void CloseLogFile() {
        std::lock_guard<std::mutex> lock(getLogMutex());
        if (!LogFile()) return;
        std::fprintf(LogFile(), "[%s] [INFO] --- log closed cleanly ---\n",
                     TimestampNow("%H:%M:%S").c_str());
        std::fclose(LogFile());
        LogFile() = nullptr;
    }

    size_t CopyRecentLines(char* out, size_t outSize) {
        if (!out || outSize == 0) return 0;
        const uint64_t total = g_ringWrites.load(std::memory_order_acquire);
        const uint64_t count = total < kRingLines ? total : kRingLines;
        const uint64_t first = total - count;

        size_t used = 0;
        for (uint64_t i = 0; i < count; ++i) {
            const char* line = g_ring[(first + i) % kRingLines];
            const size_t len = ::strnlen(line, kRingStride);
            if (used + len + 1 >= outSize) break;
            std::memcpy(out + used, line, len);
            used += len;
            out[used++] = '\n';
        }
        out[used < outSize ? used : outSize - 1] = '\0';
        return used;
    }

    void SetLevel(Level level) {
        getCurrentLevel() = level;
    }

    void RegisterCallback(LogCallback callback) {
        std::lock_guard<std::mutex> lock(getCallbackMutex());
        getCallback() = std::move(callback);
    }

    void UnregisterCallback() {
        std::lock_guard<std::mutex> lock(getCallbackMutex());
        getCallback() = nullptr;
    }

    static void vLog(Level level, const char* prefix, const char* fmt, va_list args) {
        if (level < getCurrentLevel()) return;

        // ANSI color codes
        const char* colorStart = "";
        const char* colorEnd   = "\033[0m";

        switch (level) {
            case Level::Debug:
                colorStart = "\033[34m"; // blue
                break;
            case Level::Info:
                colorStart = "\033[32m"; // green
                break;
            case Level::Warning:
                colorStart = "\033[33m"; // yellow
                break;
            case Level::Error:
                colorStart = "\033[31m"; // red
                break;
        }

        // Format message for callback before consuming va_list
        va_list argsCopy;
        va_copy(argsCopy, args);
        char callbackBuf[2048];
        std::vsnprintf(callbackBuf, sizeof(callbackBuf), fmt, argsCopy);
        va_end(argsCopy);

        std::lock_guard<std::mutex> lock(getLogMutex());
        std::FILE* out = (level == Level::Error ? stderr : stdout);
        std::fprintf(out, "%s%s%s: ", colorStart, prefix, colorEnd);
        std::vfprintf(out, fmt, args);
        std::fprintf(out, "\n");

        // File + ring get the SAME already-formatted text, and never the ANSI
        // colour codes — those are terminal control characters that would make
        // the file unreadable in whatever the player opens it with.
        {
            char line[kRingStride];
            std::snprintf(line, sizeof(line), "[%s] [%s] %s",
                          TimestampNow("%H:%M:%S").c_str(), LevelName(level), callbackBuf);
            RingPush(line);
            if (LogFile()) {
                std::fprintf(LogFile(), "%s\n", line);
                // Errors and warnings are what a crash report is read for, so
                // pay the flush to guarantee they reach disk even if the line
                // buffer would have held them.
                if (level >= Level::Warning) std::fflush(LogFile());
            }
        }

        // Invoke callback if registered
        {
            std::lock_guard<std::mutex> cbLock(getCallbackMutex());
            if (getCallback()) {
                getCallback()(level, std::string(callbackBuf));
            }
        }
    }

    void Info(const char* fmt, ...) {
        va_list args;
        va_start(args, fmt);
        vLog(Level::Info, "INFO", fmt, args);
        va_end(args);
    }

    void Debug(const char* fmt, ...) {
#ifndef NDEBUG
        va_list args;
        va_start(args, fmt);
        vLog(Level::Debug, "DEBUG", fmt, args);
        va_end(args);
#endif
    }

    void Warning(const char* fmt, ...) {
        va_list args;
        va_start(args, fmt);
        vLog(Level::Warning, "WARNING", fmt, args);
        va_end(args);
    }

    void Error(const char* fmt, ...) {
        va_list args;
        va_start(args, fmt);
        vLog(Level::Error, "ERROR", fmt, args);
        va_end(args);
    }
}