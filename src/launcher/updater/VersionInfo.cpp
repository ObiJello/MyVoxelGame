// File: src/launcher/updater/VersionInfo.cpp
#include "VersionInfo.hpp"
#include <sstream>

namespace Launcher {

    Version Version::Parse(const std::string& versionStr) {
        Version v;
        std::string s = versionStr;

        // Strip leading 'v' or 'V'
        if (!s.empty() && (s[0] == 'v' || s[0] == 'V')) {
            s = s.substr(1);
        }

        // Parse major.minor.patch
        std::istringstream stream(s);
        char dot;
        if (stream >> v.major) {
            if (stream >> dot && dot == '.') {
                if (stream >> v.minor) {
                    if (stream >> dot && dot == '.') {
                        stream >> v.patch;
                    }
                }
            }
        }

        return v;
    }

    bool Version::operator>(const Version& other) const {
        if (major != other.major) return major > other.major;
        if (minor != other.minor) return minor > other.minor;
        return patch > other.patch;
    }

    bool Version::operator==(const Version& other) const {
        return major == other.major && minor == other.minor && patch == other.patch;
    }

    bool Version::operator!=(const Version& other) const {
        return !(*this == other);
    }

    bool Version::operator<(const Version& other) const {
        return other > *this;
    }

    std::string Version::ToString() const {
        return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
    }

    bool Version::IsValid() const {
        return major > 0 || minor > 0 || patch > 0;
    }

} // namespace Launcher
