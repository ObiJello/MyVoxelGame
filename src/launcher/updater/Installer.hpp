// File: src/launcher/updater/Installer.hpp
#pragma once

#include <string>
#include <functional>

namespace Launcher {

    class Installer {
    public:
        using StatusCallback = std::function<void(const std::string& status)>;

        // Extract zipPath into installDir, replacing any existing game files.
        // Returns true on success.
        bool Install(const std::string& zipPath, const std::string& installDir, StatusCallback status = nullptr);

        // Install a new version of the launcher itself.
        // zipPath: downloaded launcher zip
        // currentAppPath: path to the currently running launcher (.app or .exe)
        // stagingDir: temp directory for extraction (e.g., {gameDir}/_launcher_update/)
        // Returns true on success. On Windows, writes an updater script.
        bool InstallLauncher(const std::string& zipPath, const std::string& currentAppPath,
                             const std::string& stagingDir);

        // On Windows, returns the path to the updater batch script (empty on macOS/Linux)
        std::string GetUpdaterScriptPath() const { return m_updaterScriptPath; }

    private:
        bool ExtractZip(const std::string& zipPath, const std::string& destDir, StatusCallback status);
        bool SetExecutablePermissions(const std::string& installDir);

        std::string m_updaterScriptPath;
    };

} // namespace Launcher
