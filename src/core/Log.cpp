#include "Log.hpp"
#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace Log {
    static Level currentLevel = Level::Debug;
    static std::mutex logMutex;

    void Init() {
        // For now, nothing to initialize (we’re logging to console).
        // In future, we could open a file here.
    }

    void SetLevel(Level level) {
        currentLevel = level;
    }

    static void vLog(Level level, const char* prefix, const char* fmt, va_list args) {
        if (level < currentLevel) return;

        std::lock_guard<std::mutex> lock(logMutex);
        std::fprintf(level == Level::Error ? stderr : stdout, "%s: ", prefix);
        std::vfprintf(level == Level::Error ? stderr : stdout, fmt, args);
        std::fprintf(level == Level::Error ? stderr : stdout, "\n");
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
