#pragma once

#include <string>

namespace Log {
    enum class Level { Debug, Info, Warning, Error };

    // Initialize logging (e.g., open log file, set log level). For now, it's a no-op.
    void Init();

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
}