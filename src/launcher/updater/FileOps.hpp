// File: src/launcher/updater/FileOps.hpp
//
// Filesystem pieces shared by the launcher's install paths (full, streamed,
// delta): zip entry names as safe relative paths, file writes that never
// write through a hard link, cloning an install, swapping a staged install
// into place, and deleting old installs off the critical path.
#pragma once

#include "common/core/ZipArchive.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace Launcher::FileOps {

    namespace fs = std::filesystem;

    // A zip entry name as a '/'-separated path relative to the extraction root,
    // or "" when it would land outside it (absolute, drive-qualified, or with a
    // ".." component). This is the zip-slip check, done on the text: asking the
    // filesystem (weakly_canonical) stats every component of every path and
    // cost a second per 30k-file install. Backslashes count as separators, as
    // Windows PowerShell 5.1's Compress-Archive writes them.
    std::string RelativePath(std::string_view entryName);

    // The single top-level directory every entry sits under (with its trailing
    // '/'), which an install unwraps; "" when there is none or it is a .app
    // bundle (the bundle is the content on macOS).
    std::string ContentPrefix(const std::vector<Core::ZipArchive::Entry>& entries);

    // `relative` with `prefix` removed; "" when it is not under the prefix or is the prefix itself.
    std::string StripPrefix(const std::string& relative, const std::string& prefix);

    // Replaces whatever is at `path` with `size` bytes. The old file is unlinked
    // first, so a path hard-linked into another tree never writes through to it.
    // `unixMode`'s permission bits are applied when non-zero (POSIX only).
    bool WriteFile(const fs::path& path, const uint8_t* data, size_t size, uint32_t unixMode);

    // Makes `dst` (which must not exist) a copy of the directory `src`: one APFS
    // clonefile on macOS (copy-on-write, no data copied), else a tree of hard
    // links, else plain copies.
    bool CloneTree(const fs::path& src, const fs::path& dst);

    // Moves `newContent` to `target`, the old `target` (if any) out of the way
    // and then handed to the background deleter. Rolls back on failure.
    bool SwapIntoPlace(const fs::path& newContent, const fs::path& target, std::string& error);

    // Old installs are deleted on a background thread: Discard renames the path
    // to a "_trash_*" name beside it (instant) and queues it. A launcher that
    // quits mid-delete leaves the rest, which SweepTrash queues next time.
    void Discard(const fs::path& path);
    void SweepTrash(const fs::path& dir);
    // Stops the deleter between files and joins it; call before the launcher exits.
    void ShutdownDeleter();

} // namespace Launcher::FileOps
