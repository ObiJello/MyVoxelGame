// File: tools/obpatch/obpatch.cpp
//
// Release-side half of the launcher's binary patches (launcher/updater/BinaryPatch).
//
//   obpatch make   <previous.zip> <new.zip> <out.obpatch>
//   obpatch verify <previous.zip> <new.zip> <patch.obpatch>
//
// make: every file in the new release zip that also exists in the previous
// one under the same name, changed, and is at least kMinEntryBytes
// compressed, is bsdiffed; a patch is kept only when it is under half the
// entry's compressed size (otherwise fetching the entry is as cheap). Each
// kept patch is applied back before it is written. Exit 0 with the file
// written, 2 when nothing was worth patching (no file), 1 on error.
//
// verify: applies every entry of a patch to the previous zip's files and
// checks the results against the new zip.
#include "launcher/updater/BinaryPatch.hpp"
#include "common/core/ZipArchive.hpp"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {

    constexpr uint64_t kMinEntryBytes = 256 * 1024;

    double Seconds(std::chrono::steady_clock::time_point since) {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - since).count();
    }

    bool ReadFile(const char* path, std::vector<uint8_t>& out) {
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (!f) return false;
        out.resize(static_cast<size_t>(f.tellg()));
        f.seekg(0);
        return static_cast<bool>(f.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size())));
    }

    int Make(const char* oldPath, const char* newPath, const char* outPath) {
        Core::ZipArchive oldZip(oldPath), newZip(newPath);
        if (!oldZip.IsOpen() || !newZip.IsOpen()) {
            std::fprintf(stderr, "obpatch: cannot open %s\n", !oldZip.IsOpen() ? oldPath : newPath);
            return 1;
        }
        const int jobs = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
        std::vector<Launcher::BinaryPatch::Entry> kept;
        uint64_t zipBytes = 0;
        for (const auto& e : newZip.Entries()) {
            if (e.IsDirectory() || e.compressedSize < kMinEntryBytes) continue;
            const Core::ZipArchive::Entry* base = oldZip.Find(e.name);
            if (!base || base->IsDirectory() || (base->crc32 == e.crc32 && base->size == e.size)) continue;

            std::vector<uint8_t> oldBytes, newBytes;
            if (!oldZip.Read(*base, oldBytes) || !newZip.Read(e, newBytes)) {
                std::fprintf(stderr, "obpatch: cannot read %s\n", e.name.c_str());
                return 1;
            }
            const auto start = std::chrono::steady_clock::now();
            Launcher::BinaryPatch::Entry p;
            p.name = e.name;
            if (!Launcher::BinaryPatch::Create(oldBytes.data(), oldBytes.size(), newBytes.data(), newBytes.size(), p, jobs)) {
                std::fprintf(stderr, "obpatch: diff failed for %s\n", e.name.c_str());
                continue;
            }
            std::vector<uint8_t> check;
            if (!Launcher::BinaryPatch::Apply(p, oldBytes.data(), oldBytes.size(), check) || check != newBytes) {
                std::fprintf(stderr, "obpatch: %s does not round-trip; leaving it out\n", e.name.c_str());
                continue;
            }
            const bool worth = p.StoredBytes() * 2 < e.compressedSize;
            std::printf("obpatch: %-70s %8.2f MB in zip -> %7.2f MB patch (%.1fs)%s\n", e.name.c_str(),
                        e.compressedSize / 1048576.0, p.StoredBytes() / 1048576.0, Seconds(start),
                        worth ? "" : "  [not worth it]");
            if (!worth) continue;
            zipBytes += e.compressedSize;
            kept.push_back(std::move(p));
        }
        if (kept.empty()) {
            std::printf("obpatch: nothing worth patching\n");
            return 2;
        }
        std::vector<uint8_t> file;
        Launcher::BinaryPatch::Write(kept, file);
        std::ofstream out(outPath, std::ios::binary);
        if (!out.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()))) {
            std::fprintf(stderr, "obpatch: cannot write %s\n", outPath);
            return 1;
        }
        std::printf("obpatch: %zu entries, %.2f MB of zip entries -> %.2f MB patch file\n", kept.size(),
                    zipBytes / 1048576.0, file.size() / 1048576.0);
        return 0;
    }

    int Verify(const char* oldPath, const char* newPath, const char* patchPath) {
        Core::ZipArchive oldZip(oldPath), newZip(newPath);
        std::vector<uint8_t> file;
        std::vector<Launcher::BinaryPatch::Entry> entries;
        if (!oldZip.IsOpen() || !newZip.IsOpen() || !ReadFile(patchPath, file) ||
            !Launcher::BinaryPatch::Read(file.data(), file.size(), entries)) {
            std::fprintf(stderr, "obpatch: cannot open inputs\n");
            return 1;
        }
        int failures = 0;
        for (const auto& p : entries) {
            const Core::ZipArchive::Entry* base = oldZip.Find(p.name);
            const Core::ZipArchive::Entry* target = newZip.Find(p.name);
            std::vector<uint8_t> oldBytes, result;
            const auto start = std::chrono::steady_clock::now();
            const bool ok = base && target && target->crc32 == p.targetCrc && oldZip.Read(*base, oldBytes) &&
                            Launcher::BinaryPatch::Apply(p, oldBytes.data(), oldBytes.size(), result);
            std::printf("obpatch: %s %s (%.2fs)\n", ok ? "ok  " : "FAIL", p.name.c_str(), Seconds(start));
            failures += ok ? 0 : 1;
        }
        return failures == 0 ? 0 : 1;
    }

} // namespace

int main(int argc, char** argv) {
    if (argc == 5 && std::string(argv[1]) == "make") return Make(argv[2], argv[3], argv[4]);
    if (argc == 5 && std::string(argv[1]) == "verify") return Verify(argv[2], argv[3], argv[4]);
    std::fprintf(stderr,
                 "usage: obpatch make   <previous.zip> <new.zip> <out.obpatch>\n"
                 "       obpatch verify <previous.zip> <new.zip> <patch.obpatch>\n");
    return 1;
}
