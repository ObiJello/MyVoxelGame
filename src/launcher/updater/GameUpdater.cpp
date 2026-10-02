// File: src/launcher/updater/GameUpdater.cpp
#include "launcher/updater/GameUpdater.hpp"
#include "launcher/updater/BinaryPatch.hpp"
#include "launcher/updater/Downloader.hpp"
#include "launcher/updater/Extractor.hpp"
#include "launcher/updater/FileOps.hpp"
#include "launcher/updater/GitHubAPI.hpp"
#include "launcher/updater/HttpSession.hpp"
#include "launcher/updater/InstallManifest.hpp"
#include "launcher/updater/Installer.hpp"
#include "launcher/LauncherConfig.hpp"
#include "common/core/Deflate.hpp"
#include "common/core/Log.hpp"
#include "common/core/ZipArchive.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Launcher {

    namespace fs = std::filesystem;
    using Entry = Core::ZipArchive::Entry;

    namespace {

        using Clock = std::chrono::steady_clock;

        double MsSince(Clock::time_point t) {
            return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
        }
        double MB(uint64_t bytes) { return static_cast<double>(bytes) / (1024.0 * 1024.0); }

        // Entries closer together than this in the zip are fetched as one range:
        // a few hundred KB of unneeded bytes cost less than another request.
        constexpr uint64_t kRangeMergeGap = 256 * 1024;
        // A release with more than this share of the zip changed downloads whole
        // (streamed), which needs no clone of the old install.
        constexpr uint64_t kDeltaMaxPercent = 60;
        // The tail fetched beyond the last directory's size, for growth and the end records.
        constexpr uint64_t kTailSlack = 256 * 1024;

        // Every file under `root` with its size, and every directory, as
        // '/'-separated relative paths.
        void ListInstall(const fs::path& root, std::unordered_map<std::string, uint64_t>& files,
                         std::vector<std::string>& dirs) {
            std::error_code ec;
            for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::recursive_directory_iterator();
                 it.increment(ec)) {
                const std::string rel = it->path().lexically_relative(root).generic_string();
                std::error_code e2;
                if (it->is_directory(e2) && !it->is_symlink(e2)) {
                    dirs.push_back(rel);
                } else {
                    files[rel] = it->file_size(e2);
                }
            }
        }

        bool ReadWhole(const fs::path& path, std::vector<uint8_t>& out) {
            std::ifstream in(path, std::ios::binary | std::ios::ate);
            if (!in) return false;
            out.resize(static_cast<size_t>(in.tellg()));
            in.seekg(0);
            return static_cast<bool>(in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size())));
        }

        // For an install made before manifests existed: the CRC-32 of every file
        // whose size matches its new entry (any other is changed whatever it holds).
        InstallManifest HashInstall(const fs::path& gameDir, const std::unordered_map<std::string, const Entry*>& newFiles,
                                    const std::unordered_map<std::string, uint64_t>& onDisk) {
            std::vector<std::string> todo;
            for (const auto& [rel, e] : newFiles) {
                const auto d = onDisk.find(rel);
                if (d != onDisk.end() && d->second == e->size) todo.push_back(rel);
            }
            InstallManifest m;
            std::mutex mutex;
            std::atomic<size_t> next{0};
            std::vector<std::thread> threads;
            const unsigned n = std::clamp(std::thread::hardware_concurrency(), 2u, 8u);
            for (unsigned t = 0; t < n; t++) {
                threads.emplace_back([&] {
                    std::vector<uint8_t> bytes;
                    for (size_t i; (i = next++) < todo.size();) {
                        const fs::path path = gameDir / todo[i];
                        if (!ReadWhole(path, bytes)) continue;
                        InstallManifest::File f;
                        f.crc32 = Core::Deflate::Crc32(bytes.data(), bytes.size());
                        f.size = bytes.size();
                        std::error_code ec;
                        f.mode = static_cast<uint32_t>(fs::status(path, ec).permissions()) & 0777;
                        std::lock_guard<std::mutex> lock(mutex);
                        m.files.emplace(todo[i], f);
                    }
                });
            }
            for (auto& t : threads) t.join();
            return m;
        }

        // Byte ranges of the zip covering a set of entries: each entry runs from
        // its local header to the next entry's (or the central directory).
        struct Span {
            uint64_t first = 0;
            uint64_t end = 0;   // exclusive
            std::vector<std::pair<std::string, const Entry*>> entries;
        };

        std::vector<Span> BuildSpans(std::vector<std::pair<std::string, const Entry*>> wanted,
                                     const std::vector<uint64_t>& boundaries) {
            std::sort(wanted.begin(), wanted.end(),
                      [](const auto& a, const auto& b) { return a.second->localHeaderOffset < b.second->localHeaderOffset; });
            std::vector<Span> spans;
            for (auto& w : wanted) {
                const uint64_t first = w.second->localHeaderOffset;
                const auto next = std::upper_bound(boundaries.begin(), boundaries.end(), first);
                const uint64_t end = next == boundaries.end() ? boundaries.back() : *next;
                if (!spans.empty() && first <= spans.back().end + kRangeMergeGap) {
                    spans.back().end = std::max(spans.back().end, end);
                } else {
                    spans.push_back(Span{first, end, {}});
                }
                spans.back().entries.push_back(std::move(w));
            }
            return spans;
        }

        // Joins a thread on every way out of a scope.
        struct JoinOnExit {
            std::thread& thread;
            ~JoinOnExit() {
                if (thread.joinable()) thread.join();
            }
        };

    } // namespace

    bool GameUpdater::Update(const ReleaseInfo& release, const std::string& installDirString, const Callbacks& given) {
        const auto start = Clock::now();
        Callbacks cb = given;
        if (!cb.status) cb.status = [](const std::string&) {};
        if (!cb.progress) cb.progress = [](uint64_t, uint64_t) {};
        if (!cb.installing) cb.installing = [] {};
        const fs::path installDir(installDirString);
        if (!release.hasPlatformAsset) {
            Log::Error("[Update] the release has no zip for this platform");
            return false;
        }
        // Old installs a previous launcher was still deleting when it quit, and
        // the leftovers of the installer before this one.
        FileOps::SweepTrash(installDir);
        FileOps::Discard(installDir / "_old");

        const char* forceFull = std::getenv("OBEY_FULL_UPDATE");
        if (!(forceFull && forceFull[0] == '1')) {
            std::string reason;
            const DeltaResult r = TryDelta(release, installDir, cb, reason);
            if (r == DeltaResult::Applied) {
                Log::Info("[Update] %s installed (delta) in %.0f ms", release.tagName.c_str(), MsSince(start));
                return true;
            }
            Log::Info("[Update] delta %s (%s); downloading the whole release",
                      r == DeltaResult::Failed ? "failed" : "not used", reason.c_str());
        }

        std::string error;
        if (!FullInstall(release, installDir, cb, error)) {
            Log::Error("[Update] install failed: %s", error.c_str());
            return false;
        }
        Log::Info("[Update] %s installed (full) in %.0f ms", release.tagName.c_str(), MsSince(start));
        return true;
    }

    GameUpdater::DeltaResult GameUpdater::TryDelta(const ReleaseInfo& release, const fs::path& installDir,
                                                   const Callbacks& cb, std::string& reason) {
        const auto start = Clock::now();
        const fs::path gameDir = installDir / GameSubdir;
        const fs::path stage = installDir / "_update_tmp";
        const uint64_t zipSize = release.platformAsset.size;
        if (!fs::is_directory(gameDir)) {
            reason = "no game installed";
            return DeltaResult::NotApplicable;
        }
        if (zipSize == 0) {
            reason = "the release does not list the zip's size";
            return DeltaResult::NotApplicable;
        }

        InstallManifest installed;
        const bool haveManifest = installed.Load(gameDir);

        // The clone and the listing of the install run while the network works.
        FileOps::Discard(stage);
        bool cloneOk = false;
        double cloneMs = 0;
        std::thread cloneThread([&] {
            const auto t = Clock::now();
            cloneOk = FileOps::CloneTree(gameDir, stage);
            cloneMs = MsSince(t);
        });
        JoinOnExit joinClone{cloneThread};
        bool keepStage = false;
        struct DiscardStage {
            std::thread& clone;
            const fs::path& stage;
            const bool& keep;
            ~DiscardStage() {
                if (clone.joinable()) clone.join();
                if (!keep) FileOps::Discard(stage);
            }
        } discardStage{cloneThread, stage, keepStage};

        std::unordered_map<std::string, uint64_t> onDisk;
        std::vector<std::string> dirsOnDisk;
        std::thread listThread([&] { ListInstall(gameDir, onDisk, dirsOnDisk); });
        JoinOnExit joinList{listThread};

        // ── Round 1: the zip's tail (its central directory) and the binary patch ──
        cb.status("Checking for changed files...");
        Http::Session session;
        const uint64_t guess = haveManifest ? installed.directoryBytes + installed.directoryBytes / 8 : 0;
        Http::Request tail;
        tail.url = release.platformAsset.downloadUrl;
        tail.ranged = true;
        tail.rangeLast = zipSize - 1;
        tail.rangeFirst = zipSize - std::min<uint64_t>(zipSize, guess + kTailSlack);
        Http::Request patchRequest;
        const bool wantPatch = release.hasPatchAsset && release.patchAsset.size > 0 &&
                               release.patchAsset.size < (64ull << 20);
        std::vector<Http::Request*> round1{&tail};
        if (wantPatch) {
            patchRequest.url = release.patchAsset.downloadUrl;
            round1.push_back(&patchRequest);
        }
        const auto netStart = Clock::now();
        session.Fetch(round1);
        uint64_t downloaded = tail.body.size() + patchRequest.body.size();
        if (!tail.Ok()) {
            reason = "range request answered HTTP " + std::to_string(tail.status) +
                     (tail.error.empty() ? "" : " (" + tail.error + ")");
            return DeltaResult::NotApplicable;
        }
        // The signed download URL the release link redirected to; the ranges
        // below go straight there instead of through the redirect each time.
        const std::string zipUrl = tail.effectiveUrl.empty() ? tail.url : tail.effectiveUrl;

        Core::ZipArchive::DirectoryInfo dirInfo;
        std::string error;
        if (!Core::ZipArchive::LocateDirectory(tail.body.data(), tail.body.size(), tail.rangeFirst, zipSize, dirInfo, error)) {
            reason = error;
            return DeltaResult::NotApplicable;
        }
        std::vector<uint8_t> dirBytes;
        if (dirInfo.offset >= tail.rangeFirst) {
            const size_t at = static_cast<size_t>(dirInfo.offset - tail.rangeFirst);
            dirBytes.assign(tail.body.begin() + at, tail.body.begin() + at + dirInfo.size);
        } else {
            // The directory outgrew the guess: fetch the part the tail missed.
            Http::Request more;
            more.url = zipUrl;
            more.ranged = true;
            more.rangeFirst = dirInfo.offset;
            more.rangeLast = tail.rangeFirst - 1;
            session.Fetch({&more});
            if (!more.Ok()) {
                reason = "central directory request answered HTTP " + std::to_string(more.status);
                return DeltaResult::NotApplicable;
            }
            downloaded += more.body.size();
            dirBytes = std::move(more.body);
            const size_t rest = static_cast<size_t>(dirInfo.offset + dirInfo.size - tail.rangeFirst);
            dirBytes.insert(dirBytes.end(), tail.body.begin(), tail.body.begin() + rest);
        }
        tail.body = {};
        std::vector<Entry> entries;
        if (!Core::ZipArchive::ParseDirectory(dirBytes.data(), dirBytes.size(), dirInfo.entryCount, entries, error)) {
            reason = error;
            return DeltaResult::NotApplicable;
        }
        dirBytes = {};
        const double directoryMs = MsSince(netStart);

        // ── What the release holds, under the install's root ──
        const std::string prefix = FileOps::ContentPrefix(entries);
        std::unordered_map<std::string, const Entry*> newFiles;
        std::unordered_set<std::string> newDirs;
        newFiles.reserve(entries.size());
        for (const Entry& e : entries) {
            const std::string rel = FileOps::StripPrefix(FileOps::RelativePath(e.name), prefix);
            if (rel.empty()) continue;
            if (e.IsDirectory() || e.name.back() == '\\') {
                newDirs.insert(rel);
                continue;
            }
            newFiles[rel] = &e;
            for (size_t slash = rel.find('/'); slash != std::string::npos; slash = rel.find('/', slash + 1)) {
                newDirs.insert(rel.substr(0, slash));
            }
        }

        listThread.join();
        if (!haveManifest) {
            cb.status("Checking installed files...");
            const auto t = Clock::now();
            installed = HashInstall(gameDir, newFiles, onDisk);
            Log::Info("[Update] no install manifest: hashed %zu installed files in %.0f ms", installed.files.size(), MsSince(t));
        }

        // ── Changed, unchanged, removed ──
        std::vector<std::pair<std::string, const Entry*>> changed;
        std::vector<std::pair<std::string, uint32_t>> modeOnly;
        uint64_t changedBytes = 0;
        for (const auto& [rel, e] : newFiles) {
            const auto disk = onDisk.find(rel);
            const auto rec = installed.files.find(rel);
            const bool same = disk != onDisk.end() && rec != installed.files.end() && disk->second == e->size &&
                              rec->second.size == e->size && rec->second.crc32 == e->crc32;
            if (!same) {
                changed.emplace_back(rel, e);
                changedBytes += e->compressedSize;
                continue;
            }
            const uint32_t mode = e->UnixMode() & 0777;
            if (mode != 0 && (rec->second.mode & 0777) != mode) modeOnly.emplace_back(rel, mode);
        }
        std::vector<std::string> removed;
        for (const auto& [rel, size] : onDisk) {
            if (!newFiles.count(rel) && rel != InstallManifest::FileName) removed.push_back(rel);
        }
        if (changedBytes * 100 > zipSize * kDeltaMaxPercent) {
            reason = "most of the release changed (" + std::to_string(changed.size()) + " files)";
            return DeltaResult::NotApplicable;
        }

        // ── Entries the binary patch rebuilds from the installed file ──
        std::vector<BinaryPatch::Entry> patches;
        std::vector<std::pair<std::string, size_t>> patched;   // rel, index into patches
        if (wantPatch && patchRequest.Ok() &&
            BinaryPatch::Read(patchRequest.body.data(), patchRequest.body.size(), patches)) {
            for (size_t i = 0; i < patches.size(); i++) {
                const BinaryPatch::Entry& p = patches[i];
                const std::string rel = FileOps::StripPrefix(FileOps::RelativePath(p.name), prefix);
                const auto nf = newFiles.find(rel);
                const auto rec = installed.files.find(rel);
                const auto disk = onDisk.find(rel);
                if (nf == newFiles.end() || rec == installed.files.end() || disk == onDisk.end()) continue;
                if (nf->second->crc32 != p.targetCrc || nf->second->size != p.targetSize) continue;
                if (rec->second.crc32 != p.baseCrc || rec->second.size != p.baseSize || disk->second != p.baseSize) continue;
                patched.emplace_back(rel, i);
            }
        }
        patchRequest.body = {};
        std::unordered_set<std::string> patchedRels;
        for (const auto& p : patched) patchedRels.insert(p.first);
        std::vector<std::pair<std::string, const Entry*>> toFetch;
        for (const auto& c : changed) {
            if (!patchedRels.count(c.first)) toFetch.push_back(c);
        }

        std::vector<uint64_t> boundaries;
        boundaries.reserve(entries.size() + 1);
        for (const Entry& e : entries) boundaries.push_back(e.localHeaderOffset);
        boundaries.push_back(dirInfo.offset);
        std::sort(boundaries.begin(), boundaries.end());
        boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());

        // ── The clone must be complete before anything is written into it ──
        cloneThread.join();
        if (!cloneOk) {
            reason = "could not copy the current install";
            return DeltaResult::Failed;
        }

        Extract::Writer writer;
        const auto submitSpan = [&](const Span& span, const Http::Request& r) {
            if (!r.Ok()) return false;
            for (const auto& [rel, e] : span.entries) {
                const size_t at = static_cast<size_t>(e->localHeaderOffset - span.first);
                Core::ZipArchive::LocalHeader local;
                if (Core::ZipArchive::ParseLocalHeader(r.body.data() + at, r.body.size() - at, local) !=
                    Core::ZipArchive::HeaderResult::Ok) {
                    return false;
                }
                const size_t dataAt = at + local.headerSize;
                if (dataAt + e->compressedSize > r.body.size()) return false;
                Extract::Writer::Job job;
                job.path = stage / rel;
                job.key = rel;
                job.name = e->name;
                job.method = e->method;
                job.crc32 = e->crc32;
                job.size = e->size;
                job.unixMode = e->UnixMode();
                job.compressed.assign(r.body.begin() + static_cast<std::ptrdiff_t>(dataAt),
                                      r.body.begin() + static_cast<std::ptrdiff_t>(dataAt + e->compressedSize));
                writer.Submit(std::move(job));
            }
            return true;
        };
        // Fetches the spans and hands each to the writer the moment it lands.
        const auto fetchSpans = [&](const std::vector<Span>& spans) {
            std::vector<Http::Request> requests(spans.size());
            std::vector<Http::Request*> pointers;
            uint64_t total = 0;
            for (size_t i = 0; i < spans.size(); i++) {
                requests[i].url = zipUrl;
                requests[i].ranged = true;
                requests[i].rangeFirst = spans[i].first;
                requests[i].rangeLast = spans[i].end - 1;
                total += spans[i].end - spans[i].first;
                pointers.push_back(&requests[i]);
            }
            bool ok = true;
            session.Fetch(
                pointers, 6,
                [&](Http::Request& r) {
                    const size_t i = static_cast<size_t>(&r - requests.data());
                    ok = submitSpan(spans[i], r) && ok;
                    downloaded += r.body.size();
                    r.body = {};
                },
                [&](uint64_t received) { cb.progress(received, total); });
            return ok;
        };

        // ── Round 2: the changed entries' ranges, while the patches apply ──
        const std::vector<Span> spans = BuildSpans(toFetch, boundaries);
        uint64_t rangeBytes = 0;
        for (const Span& s : spans) rangeBytes += s.end - s.first;
        cb.status("Downloading " + std::to_string(changed.size()) + " changed files...");

        std::vector<std::pair<std::string, const Entry*>> patchFailed;
        double patchMs = 0;
        std::thread patchThread([&] {
            const auto t = Clock::now();
            std::vector<uint8_t> base, out;
            for (const auto& [rel, index] : patched) {
                const Entry* e = newFiles.at(rel);
                const bool ok = ReadWhole(stage / rel, base) &&
                                BinaryPatch::Apply(patches[index], base.data(), base.size(), out) &&
                                FileOps::WriteFile(stage / rel, out.data(), out.size(), e->UnixMode());
                if (!ok) {
                    Log::Warning("[Update] patch for %s did not apply; fetching the file instead", rel.c_str());
                    patchFailed.emplace_back(rel, e);
                }
            }
            patchMs = MsSince(t);
        });
        JoinOnExit joinPatch{patchThread};

        const auto rangeStart = Clock::now();
        bool fetched = spans.empty() || fetchSpans(spans);
        patchThread.join();
        if (fetched && !patchFailed.empty()) fetched = fetchSpans(BuildSpans(patchFailed, boundaries));
        const double rangeMs = MsSince(rangeStart);
        if (!fetched) {
            writer.Wait();
            reason = "a range request failed";
            return DeltaResult::Failed;
        }

        cb.installing();
        cb.status("Installing...");
        const auto writeStart = Clock::now();
        writer.Wait();
        if (writer.Failures() > 0) {
            reason = writer.FirstError();
            return DeltaResult::Failed;
        }

        // ── Make the clone exactly the new release ──
        std::error_code ec;
        for (const std::string& rel : removed) fs::remove(stage / rel, ec);
        std::sort(dirsOnDisk.begin(), dirsOnDisk.end(), [](const auto& a, const auto& b) { return a.size() > b.size(); });
        for (const std::string& rel : dirsOnDisk) {
            if (!newDirs.count(rel)) fs::remove_all(stage / rel, ec);
        }
        for (const std::string& rel : newDirs) fs::create_directories(stage / rel, ec);
#ifndef _WIN32
        for (const auto& [rel, mode] : modeOnly) {
            fs::permissions(stage / rel, static_cast<fs::perms>(mode), fs::perm_options::replace, ec);
        }
#endif
        if (!InstallManifest::FromEntries(entries, prefix, dirInfo.size).Save(stage)) {
            reason = "could not write the install manifest";
            return DeltaResult::Failed;
        }
        if (!FileOps::SwapIntoPlace(stage, gameDir, error)) {
            reason = error;
            return DeltaResult::Failed;
        }
        keepStage = true;   // it is the install now

        Log::Info("[Update] delta: %zu of %zu files changed (%zu patched, %zu fetched in %zu ranges, %zu removed), "
                  "%.1f MB downloaded instead of %.1f MB",
                  changed.size(), newFiles.size(), patched.size() - patchFailed.size(), toFetch.size() + patchFailed.size(),
                  spans.size(), removed.size(), MB(downloaded), MB(zipSize));
        Log::Info("[Update] delta timings: directory %.0f ms, clone %.0f ms (overlapped), ranges %.0f ms (%.1f MB), "
                  "patch %.0f ms, finish %.0f ms, total %.0f ms",
                  directoryMs, cloneMs, rangeMs, MB(rangeBytes), patchMs, MsSince(writeStart), MsSince(start));
        return DeltaResult::Applied;
    }

    bool GameUpdater::FullInstall(const ReleaseInfo& release, const fs::path& installDir, const Callbacks& cb,
                                  std::string& error) {
        const auto start = Clock::now();
        const fs::path gameDir = installDir / GameSubdir;
        const fs::path temp = installDir / "_update_tmp";
        const fs::path zipPath = installDir / release.platformAsset.name;
        FileOps::Discard(temp);
        std::error_code ec;
        fs::create_directories(temp, ec);
        if (ec) {
            error = "cannot create " + temp.string() + ": " + ec.message();
            return false;
        }

        cb.status("Downloading update...");
        Extract::StreamingExtractor extractor(temp);
        Downloader downloader;
        const bool downloaded = downloader.Download(
            release.platformAsset.downloadUrl, zipPath.string(),
            [&](size_t done, size_t total) { cb.progress(done, total); },
            [&](const uint8_t* data, size_t size) { extractor.Feed(data, size); });
        const double downloadMs = MsSince(start);
        if (!downloaded) {
            FileOps::Discard(temp);
            error = "download failed";
            return false;
        }

        cb.installing();
        cb.status("Installing...");
        const auto finishStart = Clock::now();
        std::vector<Entry> entries;
        if (!extractor.Finish(zipPath, entries, error)) {
            FileOps::Discard(temp);
            return false;
        }
        const std::string prefix = FileOps::ContentPrefix(entries);
        const fs::path contentRoot = prefix.empty() ? temp : temp / prefix.substr(0, prefix.size() - 1);
        if (!InstallManifest::FromEntries(entries, prefix, extractor.DirectoryBytes()).Save(contentRoot)) {
            error = "could not write the install manifest";
            FileOps::Discard(temp);
            return false;
        }
        if (!FileOps::SwapIntoPlace(contentRoot, gameDir, error)) {
            FileOps::Discard(temp);
            return false;
        }
        if (contentRoot != temp) FileOps::Discard(temp);
#ifndef _WIN32
        Installer::SetExecutablePermissions(gameDir.string());
#endif
        fs::remove(zipPath, ec);
        Log::Info("[Update] full: download + streamed extraction %.0f ms (%zu files during the download), "
                  "finish %.0f ms", downloadMs, extractor.StreamedFiles(), MsSince(finishStart));
        return true;
    }

} // namespace Launcher
