// File: src/launcher/updater/VersionInfo.hpp
#pragma once

#include <string>

namespace Launcher {

    struct Version {
        int major = 0;
        int minor = 0;
        int patch = 0;

        static Version Parse(const std::string& versionStr);
        bool operator>(const Version& other) const;
        bool operator==(const Version& other) const;
        bool operator!=(const Version& other) const;
        bool operator<(const Version& other) const;
        std::string ToString() const;
        bool IsValid() const;
    };

} // namespace Launcher
