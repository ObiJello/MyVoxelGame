// File: src/common/core/Log.hpp
#pragma once

#include <string>
#include <functional>

namespace Log {
    enum class Level { Debug, Info, Warning, Error };

    // Initialize logging (e.g., open log file, set log level). For now, it's a no-op.
    void Init();

    // ── Log file ────────────────────────────────────────────────────────────
    // Mirror every line to `path`, rotating whatever was there from the last
    // run to `<dir>/<timestamp>.log` and pruning to the newest `keep` archives
    // (MC's logs/latest.log + dated archives).
    //
    // This is the artifact a player can actually send you. Until it exists the
    // game logs only to stdout, which a launcher-started build discards
    // outright — on macOS `open --args` gives the process no usable terminal,
    // so every diagnostic printed in the field went nowhere.
    //
    // Takes a path rather than deriving one so this stays free of any platform
    // dependency; PlatformMain passes the game directory in.
    bool OpenLogFile(const std::string& path, int keep = 5);

    // Flush and note a deliberate shutdown. An unterminated log is then a
    // reliable signal that the process died rather than exited, which is the
    // one question a crash report can't answer if it never got to run.
    void CloseLogFile();

    // ── Crash-report support ────────────────────────────────────────────────
    // Copy the most recent log lines into `out` (a caller-owned buffer) and
    // return the number of bytes written.
    //
    // Reads a fixed static ring buffer with no allocation, no locking and no
    // stdio, because the only caller is a signal handler — where malloc and
    // fprintf are not async-signal-safe and stdio's buffered tail is exactly
    // what a hard crash loses. Safe to call at any time; a torn read costs at
    // most one garbled line.
    size_t CopyRecentLines(char* out, size_t outSize);

    // Log an informational message (stdout).
    void Info(const char* fmt, ...);

    // Log a debug message (stdout), only if in debug builds.
    void Debug(const char* fmt, ...);

    // Log a warning message (stdout).
    void Warning(const char* fmt, ...);

    // Log an error message (stderr).
    void Error(const char* fmt, ...);

    // (Optional) Set the minimum log level (Debug/Info/Warning/Error).
    void SetLevel(Level level);

    // Callback for capturing log output (used by debug console)
    using LogCallback = std::function<void(Level level, const std::string& message)>;
    void RegisterCallback(LogCallback callback);
    void UnregisterCallback();
}