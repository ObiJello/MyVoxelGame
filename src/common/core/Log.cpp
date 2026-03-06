// File: src/common/core/Log.cpp
#include "Log.hpp"
#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace Log {
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
        // No-op for now; future file setup could go here.
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