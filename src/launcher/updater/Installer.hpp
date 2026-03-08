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

    private:
        bool ExtractZip(const std::string& zipPath, const std::string& destDir, StatusCallback status);
        bool SetExecutablePermissions(const std::string& installDir);
    };

} // namespace Launcher
