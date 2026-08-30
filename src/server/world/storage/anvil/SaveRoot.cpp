// File: src/server/world/storage/anvil/SaveRoot.cpp
#include "server/world/storage/anvil/SaveRoot.hpp"

#include "common/core/Log.hpp"
#include "platform/GameDirectory.hpp"

namespace Game::Anvil {

    namespace {

        // weakly_canonical resolves symlinks and ".." without requiring the
        // path to exist, which matters because a world folder is validated
        // before it is created.
        std::filesystem::path Canonical(const std::filesystem::path& p) {
            std::error_code ec;
            auto out = std::filesystem::weakly_canonical(p, ec);
            return ec ? p.lexically_normal() : out;
        }

        // True when `child` is `base` or lives inside it. Compares whole path
        // components, so "/saves-backup" is not treated as inside "/saves".
        bool IsInside(const std::filesystem::path& child, const std::filesystem::path& base) {
            auto c = child.begin(), cEnd = child.end();
            auto b = base.begin(),  bEnd = base.end();
            for (; b != bEnd; ++b, ++c) {
                if (c == cEnd) return false;
                if (*c != *b) return false;
            }
            return true;
        }

    } // namespace

    std::optional<SaveRoot> SaveRoot::Open(const std::string& worldPath, std::string& reason) {
        if (worldPath.empty()) {
            reason = "no world path";
            return std::nullopt;
        }

        const auto root  = Canonical(std::filesystem::path(worldPath));
        const auto saves = Canonical(std::filesystem::path(Platform::g_gameDirectory.GetSavesDirectory()));

        if (saves.empty()) {
            reason = "obeycraft saves directory is not configured";
            return std::nullopt;
        }
        if (!IsInside(root, saves)) {
            reason = "world is outside " + saves.string();
            return std::nullopt;
        }
        // The world folder itself, not the saves directory.
        if (root == saves) {
            reason = "path is the saves directory, not a world";
            return std::nullopt;
        }

        // Belt and braces. IsInside already excludes anything outside the
        // obeycraft tree, but if the two ever overlap — a symlinked or
        // relocated saves directory — the Minecraft install wins and we refuse.
        const std::string mcSaves = Platform::GameDirectory::GetMinecraftSavesDirectory();
        if (!mcSaves.empty() && IsInside(root, Canonical(std::filesystem::path(mcSaves)))) {
            reason = "world is inside the Minecraft installation";
            return std::nullopt;
        }

        reason.clear();
        return SaveRoot(root);
    }

    std::filesystem::path SaveRoot::Dimension(DimensionId dim) const {
        const std::string_view sub = DimensionSaveSubdir(dim);
        return sub.empty() ? m_root : (m_root / std::filesystem::path(sub));
    }

} // namespace Game::Anvil
