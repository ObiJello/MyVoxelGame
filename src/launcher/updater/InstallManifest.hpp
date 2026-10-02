// File: src/launcher/updater/InstallManifest.hpp
//
// What the launcher installed: every file of the game folder with the CRC-32,
// size and mode its release zip recorded. A delta update compares the next
// release's central directory against it to find the files that changed.
// It lives inside the game folder, so it is swapped into place together with
// the files it describes and can never describe a different install.
#pragma once

#include "common/core/ZipArchive.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace Launcher {

    struct InstallManifest {
        struct File {
            uint32_t crc32 = 0;
            uint64_t size = 0;
            uint32_t mode = 0;     // Unix mode bits from the zip; 0 when it carried none
        };

        static constexpr const char* FileName = ".obeycraft-install";

        // The release zip's central directory size: the next update fetches about
        // this much of the new zip's tail, to get its directory in one request.
        uint64_t directoryBytes = 0;
        std::unordered_map<std::string, File> files;   // path under the game folder, '/'-separated

        bool Load(const std::filesystem::path& gameDir);
        bool Save(const std::filesystem::path& gameDir) const;

        // From a release zip's entries, paths taken under `prefix` (the folder an
        // install unwraps, FileOps::ContentPrefix).
        static InstallManifest FromEntries(const std::vector<Core::ZipArchive::Entry>& entries,
                                           const std::string& prefix, uint64_t directoryBytes);
    };

} // namespace Launcher
