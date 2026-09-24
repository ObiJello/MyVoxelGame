// File: src/client/shader/ShaderPacks.cpp
#include "client/shader/ShaderPacks.hpp"

#include "common/core/Log.hpp"
#include "platform/GameDirectory.hpp"
#include "unzip.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace Shaders {

    namespace {

        constexpr const char* kSettingKey = "shaderPack";
        constexpr const char* kExtractedDir = ".extracted";

        std::string s_packsDir;

        const std::string& EnsurePacksDir() {
            if (s_packsDir.empty()) {
                s_packsDir = Platform::g_gameDirectory.GetShaderPacksDirectory();
                std::error_code ec;
                fs::create_directories(s_packsDir, ec);
            }
            return s_packsDir;
        }

        // Does this folder hold shader programs directly (a pack unpacked
        // without its shaders/ folder)?
        bool HasPrograms(const fs::path& dir) {
            std::error_code ec;
            for (const auto& e : fs::directory_iterator(dir, ec)) {
                const std::string ext = e.path().extension().string();
                if (e.is_regular_file(ec) && (ext == ".fsh" || ext == ".vsh")) return true;
            }
            return false;
        }

        // The pack's shaders directory: root/shaders, the root itself when
        // it holds the programs, or the same one folder down (a zip made
        // from a pack folder keeps that folder).
        bool FindShadersDir(const fs::path& root, fs::path& out) {
            std::error_code ec;
            if (fs::is_directory(root / "shaders", ec)) { out = root / "shaders"; return true; }
            if (HasPrograms(root)) { out = root; return true; }
            for (const auto& e : fs::directory_iterator(root, ec)) {
                if (!e.is_directory(ec)) continue;
                if (fs::is_directory(e.path() / "shaders", ec)) { out = e.path() / "shaders"; return true; }
                if (HasPrograms(e.path())) { out = e.path(); return true; }
            }
            return false;
        }

        std::string ToLower(std::string s) {
            for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        }

        bool SafeRelativeEntry(const std::string& entry) {
            if (entry.empty() || entry[0] == '/' || entry[0] == '\\') return false;
            if (entry.find("..") != std::string::npos) return false;
            if (entry.size() > 1 && entry[1] == ':') return false;
            return true;
        }

        bool ReadZipEntry(unzFile zf, std::string& out) {
            if (unzOpenCurrentFile(zf) != UNZ_OK) return false;
            char buf[65536];
            int n;
            out.clear();
            while ((n = unzReadCurrentFile(zf, buf, sizeof buf)) > 0) out.append(buf, static_cast<size_t>(n));
            unzCloseCurrentFile(zf);
            return n == 0;
        }

        bool ExtractZip(const fs::path& zip, const fs::path& destDir) {
            unzFile zf = unzOpen(zip.string().c_str());
            if (!zf) return false;
            std::error_code ec;
            fs::remove_all(destDir, ec);
            fs::create_directories(destDir, ec);
            bool ok = true;
            size_t files = 0;
            if (unzGoToFirstFile(zf) == UNZ_OK) {
                do {
                    char name[1024];
                    unz_file_info info{};
                    if (unzGetCurrentFileInfo(zf, &info, name, sizeof name, nullptr, 0, nullptr, 0) != UNZ_OK) { ok = false; break; }
                    const std::string entry(name);
                    if (entry.empty() || entry.back() == '/') continue;
                    if (!SafeRelativeEntry(entry)) { Log::Warning("[ShaderPacks] skipping unsafe zip entry %s", entry.c_str()); continue; }
                    // Only shader files and the pack's own metadata, wherever
                    // the zip keeps them.
                    const std::string ext = fs::path(entry).extension().string();
                    const bool shaderFile = ext == ".fsh" || ext == ".vsh" || ext == ".gsh" || ext == ".csh" ||
                                            ext == ".glsl" || ext == ".inc" || ext == ".properties" ||
                                            ext == ".lang" || ext == ".json" || ext == ".png" || ext == ".settings";
                    if (!shaderFile && entry.find("pack.") == std::string::npos &&
                        entry.find("LICENSE") == std::string::npos && entry.find("README") == std::string::npos) continue;
                    std::string bytes;
                    if (!ReadZipEntry(zf, bytes)) { ok = false; break; }
                    const fs::path dest = destDir / entry;
                    fs::create_directories(dest.parent_path(), ec);
                    std::ofstream out(dest, std::ios::binary);
                    if (!out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()))) { ok = false; break; }
                    ++files;
                } while (unzGoToNextFile(zf) == UNZ_OK);
            }
            unzClose(zf);
            Log::Info("[ShaderPacks] extracted %zu file(s) from %s", files, zip.filename().string().c_str());
            return ok;
        }

        // "<size>:<mtime>" of a zip, the extraction's freshness key.
        std::string ZipStamp(const fs::path& zip) {
            std::error_code ec;
            const auto size = fs::file_size(zip, ec);
            const auto time = fs::last_write_time(zip, ec).time_since_epoch().count();
            return std::to_string(size) + ":" + std::to_string(static_cast<long long>(time));
        }

    } // namespace

    const std::string& PacksDirectory() { return EnsurePacksDir(); }

    std::vector<PackInfo> Discover() {
        std::vector<PackInfo> out;
        const fs::path dir = EnsurePacksDir();
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            const fs::path p = e.path();
            const std::string name = p.filename().string();
            if (name.empty() || name[0] == '.') continue;
            PackInfo info;
            info.name       = name;
            info.id         = "file/" + name;
            info.sourcePath = p.string();
            if (e.is_directory(ec)) {
                fs::path shadersDir;
                if (!FindShadersDir(p, shadersDir)) continue;
                out.push_back(std::move(info));
            } else if (e.is_regular_file(ec) && ToLower(p.extension().string()) == ".zip") {
                info.isZip = true;
                out.push_back(std::move(info));
            }
        }
        std::sort(out.begin(), out.end(), [](const PackInfo& a, const PackInfo& b) {
            return ToLower(a.name) < ToLower(b.name);
        });
        return out;
    }

    std::string Selected() {
        return Platform::g_gameSettings.GetString(kSettingKey, "");
    }

    void SetSelected(const std::string& id) {
        Platform::g_gameSettings.SetString(kSettingKey, id);
        Platform::g_gameSettings.Save();
    }

    bool Find(const std::string& id, PackInfo& out) {
        for (PackInfo& p : Discover()) {
            if (p.id == id) { out = std::move(p); return true; }
        }
        return false;
    }

    bool Prepare(const PackInfo& pack, std::string& shadersDirOut, std::string& error) {
        std::error_code ec;
        fs::path base = pack.sourcePath;
        if (pack.isZip) {
            const fs::path extracted = fs::path(EnsurePacksDir()) / kExtractedDir / pack.name;
            const fs::path stampFile = extracted / ".zipstamp";
            const std::string stamp = ZipStamp(base);
            std::string have;
            if (std::ifstream in(stampFile); in) std::getline(in, have);
            if (have != stamp || !fs::is_directory(extracted, ec)) {
                if (!ExtractZip(base, extracted)) {
                    error = "could not extract " + pack.name;
                    return false;
                }
                std::ofstream(stampFile) << stamp;
            }
            base = extracted;
        }
        fs::path shadersDir;
        if (!FindShadersDir(base, shadersDir)) {
            error = pack.name + " has no shaders/ folder and no shader programs";
            return false;
        }
        shadersDirOut = shadersDir.string();
        return true;
    }

} // namespace Shaders
