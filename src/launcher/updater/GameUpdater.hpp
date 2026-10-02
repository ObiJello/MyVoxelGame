// File: src/launcher/updater/GameUpdater.hpp
//
// Installs a game release, fetching as few bytes as the release allows.
//
//  * Delta, when a game is installed: the new release zip's central
//    directory comes in over an HTTP range request and is compared with the
//    install's manifest (InstallManifest), and only the entries that changed
//    are fetched, as byte ranges of the zip, or rebuilt from the release's
//    binary patch (BinaryPatch) where the installed file is the patch's base.
//    The install is cloned (APFS clonefile; hard links elsewhere) while that
//    happens, the changes are written into the clone, and the clone is swapped
//    into place. A release typically changes ~100 of its ~30k files.
//  * Full, for a first install or a delta that cannot apply (no range support,
//    most of the release changed): the zip downloads with its entries
//    extracted as they arrive (Extract::StreamingExtractor).
//
// Either way the previous install is deleted in the background after the
// swap. OBEY_FULL_UPDATE=1 skips the delta, for A/B timing.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace Launcher {

    struct ReleaseInfo;

    class GameUpdater {
    public:
        struct Callbacks {
            std::function<void(const std::string& status)> status;
            std::function<void(uint64_t done, uint64_t total)> progress;   // bytes being downloaded
            std::function<void()> installing;                              // downloads done, writing files
        };

        // installDir: the obeycraft directory (the game lives in installDir/game).
        bool Update(const ReleaseInfo& release, const std::string& installDir, const Callbacks& callbacks);

    private:
        enum class DeltaResult : uint8_t { Applied, NotApplicable, Failed };

        DeltaResult TryDelta(const ReleaseInfo& release, const std::filesystem::path& installDir,
                             const Callbacks& callbacks, std::string& reason);
        bool FullInstall(const ReleaseInfo& release, const std::filesystem::path& installDir,
                         const Callbacks& callbacks, std::string& error);
    };

} // namespace Launcher
