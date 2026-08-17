// File: src/platform/CrashHandler.hpp
//
// Writes a human-readable crash report next to the log file when the process
// dies abnormally, so a player can send you a file instead of "it crashed".
//
// This does NOT replace Sentry. Sentry is the better report when it works —
// symbolicated, aggregated, automatic — but it only works when the player has
// network, when crashpad started, and when the crash is one crashpad catches.
// This is the fallback that always leaves something on disk, and it is the only
// option for a player who is offline or who hits a crash on a build with
// symbol upload disabled.
//
// ── What actually catches what ──────────────────────────────────────────────
// On macOS Sentry runs crashpad OUT OF PROCESS via Mach exception ports, which
// are delivered BEFORE POSIX signals. For a hard SIGSEGV crashpad may take the
// exception and the process can die without our signal handler ever running.
// That is why the ROLLING LOG FILE is the primary artifact here and the crash
// report is a bonus: the log is written as the game runs, so it survives no
// matter which layer wins the race. The handler still earns its keep for
// std::terminate (an uncaught C++ exception is not a Mach exception) and for
// any signal crashpad does not claim.
#pragma once

#include <string>

namespace Platform {

    // Install handlers. `crashDir` is created if needed; `version` is stamped
    // into the report so a report can be matched to a build.
    //
    // Call AFTER sentry_init: whoever installs last runs first, and this
    // deliberately chains to the previously-installed handler afterwards so
    // Sentry still gets its report.
    void InstallCrashHandler(const std::string& crashDir, const std::string& version);

    // Path this session would write a crash to. Fixed at install time so the
    // signal handler does no string formatting — see the .cpp for why.
    const char* CrashReportPath();

} // namespace Platform
