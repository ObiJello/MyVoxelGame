// File: src/common/core/AssetLocator.cpp
#include "common/core/AssetLocator.hpp"

#include <algorithm>
#include <filesystem>
#include <system_error>

namespace Core::Assets {

    namespace {
        ListFn   s_list   = nullptr;
        LocateFn s_locate = nullptr;

        std::vector<Entry> ListPlain(const std::string& dir, const char* extension, bool recursive) {
            std::vector<Entry> out;
            std::error_code ec;
            const std::filesystem::path root(dir);
            if (!std::filesystem::is_directory(root, ec)) return out;
            auto take = [&](const std::filesystem::directory_entry& e) {
                if (!e.is_regular_file(ec)) return;
                if (extension && e.path().extension() != extension) return;
                std::string rel = std::filesystem::relative(e.path(), root, ec).generic_string();
                out.push_back(Entry{std::move(rel), e.path().string(), 0});
            };
            if (recursive) {
                for (const auto& e : std::filesystem::recursive_directory_iterator(root, ec)) take(e);
            } else {
                for (const auto& e : std::filesystem::directory_iterator(root, ec)) take(e);
            }
            std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) { return a.relative < b.relative; });
            return out;
        }
    }

    void SetOverlayHooks(ListFn list, LocateFn locate) {
        s_list   = list;
        s_locate = locate;
    }

    std::vector<Entry> ListFiles(const std::string& vanillaAbsDir, const char* extension, bool recursive) {
        return s_list ? s_list(vanillaAbsDir, extension, recursive) : ListPlain(vanillaAbsDir, extension, recursive);
    }

    std::string Locate(const std::string& vanillaAbsPath) {
        if (s_locate) {
            std::string hit = s_locate(vanillaAbsPath, 1);
            if (!hit.empty()) return hit;
        }
        return vanillaAbsPath;
    }

    std::string LocateFromLayer(const std::string& vanillaAbsPath, int minLayer) {
        if (s_locate) return s_locate(vanillaAbsPath, minLayer);
        if (minLayer > 0) return {};
        std::error_code ec;
        return std::filesystem::exists(vanillaAbsPath, ec) ? vanillaAbsPath : std::string();
    }

} // namespace Core::Assets
