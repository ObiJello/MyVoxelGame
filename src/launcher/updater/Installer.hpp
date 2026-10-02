// File: src/launcher/updater/Installer.hpp
#pragma once

#include <string>

namespace Launcher {

    // The launcher's own self-update. Game updates are GameUpdater's.
    class Installer {
    public:
        // Install a new version of the launcher itself.
        // zipPath: downloaded launcher zip
        // currentAppPath: path to the currently running launcher (.app or .exe)
        // stagingDir: temp directory for extraction (e.g., {gameDir}/_launcher_update/)
        // Returns true on success. On Windows, writes an updater script.
        bool InstallLauncher(const std::string& zipPath, const std::string& currentAppPath,
                             const std::string& stagingDir);

        // On Windows, returns the path to the updater batch script (empty on macOS/Linux)
        std::string GetUpdaterScriptPath() const { return m_updaterScriptPath; }

        // chmod 755 everything under Contents/MacOS/ and every .sh (POSIX; for
        // archives zipped without Unix modes).
        static bool SetExecutablePermissions(const std::string& installDir);

    private:
        std::string m_updaterScriptPath;
    };

} // namespace Launcher
