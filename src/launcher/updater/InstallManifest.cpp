// File: src/launcher/updater/InstallManifest.cpp
//
// Text, one file per line after a two-line header:
//   obeycraft-install 1
//   directory <bytes>
//   <crc32 hex> <size> <mode octal> <path>
#include "launcher/updater/InstallManifest.hpp"
#include "launcher/updater/FileOps.hpp"
#include "common/core/Log.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string_view>
#include <system_error>

namespace Launcher {

    namespace {
        constexpr const char* kHeader = "obeycraft-install 1";
    }

    bool InstallManifest::Load(const std::filesystem::path& gameDir) {
        files.clear();
        directoryBytes = 0;
        std::ifstream in(gameDir / FileName, std::ios::binary);
        if (!in) return false;
        std::stringstream ss;
        ss << in.rdbuf();
        const std::string text = ss.str();

        size_t p = 0;
        auto nextLine = [&](std::string_view& line) {
            if (p >= text.size()) return false;
            size_t e = text.find('\n', p);
            if (e == std::string::npos) e = text.size();
            line = std::string_view(text).substr(p, e - p);
            p = e + 1;
            return true;
        };
        std::string_view line;
        if (!nextLine(line) || line != kHeader) return false;
        if (!nextLine(line) || line.rfind("directory ", 0) != 0) return false;
        directoryBytes = std::strtoull(std::string(line.substr(10)).c_str(), nullptr, 10);

        files.reserve(text.size() / 48);
        while (nextLine(line)) {
            if (line.empty()) continue;
            // crc size mode path — the path is the rest of the line (it may hold spaces).
            const size_t a = line.find(' ');
            const size_t b = a == std::string_view::npos ? a : line.find(' ', a + 1);
            const size_t c = b == std::string_view::npos ? b : line.find(' ', b + 1);
            if (c == std::string_view::npos) {
                files.clear();
                return false;
            }
            File f;
            f.crc32 = static_cast<uint32_t>(std::strtoul(std::string(line.substr(0, a)).c_str(), nullptr, 16));
            f.size = std::strtoull(std::string(line.substr(a + 1, b - a - 1)).c_str(), nullptr, 10);
            f.mode = static_cast<uint32_t>(std::strtoul(std::string(line.substr(b + 1, c - b - 1)).c_str(), nullptr, 8));
            files.emplace(std::string(line.substr(c + 1)), f);
        }
        return true;
    }

    bool InstallManifest::Save(const std::filesystem::path& gameDir) const {
        std::string text;
        text.reserve(files.size() * 96 + 64);
        text += kHeader;
        text += "\ndirectory " + std::to_string(directoryBytes) + "\n";
        char buf[64];
        for (const auto& [path, f] : files) {
            std::snprintf(buf, sizeof buf, "%08x %llu %o ", f.crc32, static_cast<unsigned long long>(f.size), f.mode);
            text += buf;
            text += path;
            text += '\n';
        }
        const std::filesystem::path tmp = gameDir / (std::string(FileName) + ".tmp");
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out.write(text.data(), static_cast<std::streamsize>(text.size()))) {
                Log::Error("[Update] cannot write %s", tmp.string().c_str());
                return false;
            }
        }
        std::error_code ec;
        std::filesystem::rename(tmp, gameDir / FileName, ec);
        if (ec) {
            Log::Error("[Update] cannot write the install manifest: %s", ec.message().c_str());
            return false;
        }
        return true;
    }

    InstallManifest InstallManifest::FromEntries(const std::vector<Core::ZipArchive::Entry>& entries,
                                                 const std::string& prefix, uint64_t directoryBytes) {
        InstallManifest m;
        m.directoryBytes = directoryBytes;
        m.files.reserve(entries.size());
        for (const auto& e : entries) {
            if (e.IsDirectory()) continue;
            const std::string rel = FileOps::StripPrefix(FileOps::RelativePath(e.name), prefix);
            if (rel.empty() || rel.find('\n') != std::string::npos) continue;
            m.files[rel] = File{e.crc32, e.size, e.UnixMode() & 07777};
        }
        return m;
    }

} // namespace Launcher
