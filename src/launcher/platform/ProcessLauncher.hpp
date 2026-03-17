// File: src/launcher/platform/ProcessLauncher.hpp
#pragma once

#include <string>

namespace Launcher {

    // Launch the game process. Returns true if the process was started successfully.
    // extraArgs are appended to the command line (e.g., "--server 192.168.1.5:25565").
    bool LaunchGame(const std::string& gamePath, bool useVulkan = false, const std::string& extraArgs = "");

    // Relaunch the launcher itself (for self-update).
    // On macOS: uses 'open' on the .app bundle, then exits.
    // On Windows: launches the updater batch script, then exits.
    // This function does NOT return on success (calls exit).
    void RelaunchSelf(const std::string& launcherPath, const std::string& updaterScript = "");

    // Get the path to the currently running launcher .app or .exe
    std::string GetCurrentLauncherPath();

} // namespace Launcher
