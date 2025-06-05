#include "Log.hpp"
#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace Log {
    static Level currentLevel = Level::Debug;
    static std::mutex logMutex;

    void Init() {
        // No-op for now; future file setup could go here.
    }

    void SetLevel(Level level) {
        currentLevel = level;
    }

    static void vLog(Level level, const char* prefix, const char* fmt, va_list args) {
        if (level < currentLevel) return;

        // ANSI color codes
        const char* colorStart = "";
        const char* colorEnd   = "\033[0m";

        switch (level) {
            case Level::Debug:
                colorStart = "\033[34m"; // red
                break;
            case Level::Info:
                colorStart = "\033[32m"; // green
                break;
            case Level::Error:
                colorStart = "\033[31m"; // red
                break;
        }

        std::lock_guard<std::mutex> lock(logMutex);
        std::FILE* out = (level == Level::Error ? stderr : stdout);
        std::fprintf(out, "%s%s%s: ", colorStart, prefix, colorEnd);
        std::vfprintf(out, fmt, args);
        std::fprintf(out, "\n");
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

    void Error(const char* fmt, ...) {
        va_list args;
        va_start(args, fmt);
        vLog(Level::Error, "ERROR", fmt, args);
        va_end(args);
    }
}