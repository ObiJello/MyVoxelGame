// File: src/client/renderer/gui/debug/DebugScreenEntries.cpp
#include "DebugScreenEntries.hpp"
#include "common/world/level/GameRules.hpp"
#include "platform/GameDirectory.hpp"
#include "common/core/Log.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace Render::DebugScreen {

    // Implemented in DebugEntries.cpp — fills the registry with the text entries.
    void RegisterTextEntries(std::map<std::string, std::unique_ptr<Entry>>& out);

    const char* StatusName(EntryStatus s) {
        switch (s) {
            case EntryStatus::AlwaysOn:  return "alwaysOn";
            case EntryStatus::InOverlay: return "inOverlay";
            case EntryStatus::Never:     return "never";
        }
        return "never";
    }

    bool StatusFromName(std::string_view name, EntryStatus& out) {
        if (name == "alwaysOn")  { out = EntryStatus::AlwaysOn;  return true; }
        if (name == "inOverlay") { out = EntryStatus::InOverlay; return true; }
        if (name == "never")     { out = EntryStatus::Never;     return true; }
        return false;
    }

    const char* ProfileName(Profile p) {
        return p == Profile::Performance ? "performance" : "default";
    }

    const char* ProfileLabel(Profile p) {
        return p == Profile::Performance ? "Performance profile" : "Default profile";
    }

    bool ProfileFromName(std::string_view name, Profile& out) {
        if (name == "default")     { out = Profile::Default;     return true; }
        if (name == "performance") { out = Profile::Performance; return true; }
        return false;
    }

    const char* CategoryLabel(Category c) {
        return c == Category::Renderer ? "Debug Renderers" : "Debug Screen Text";
    }

    // ── Registry ────────────────────────────────────────────────────────

    namespace {
        std::map<std::string, std::unique_ptr<Entry>>& Registry() {
            static std::map<std::string, std::unique_ptr<Entry>> s_entries;
            static bool s_built = false;
            if (!s_built) {
                s_built = true;
                RegisterTextEntries(s_entries);
                // MC DebugScreenEntries: the renderer toggles are DebugEntryNoop.
                for (const char* id : { Ids::EntityHitboxes, Ids::ChunkBorders, Ids::ThreeDimensionalCrosshair,
                                        Ids::ChunkSectionPaths, Ids::ChunkSectionOctree, Ids::VisualizeWaterLevels,
                                        Ids::VisualizeHeightmap, Ids::VisualizeCollisionBoxes,
                                        Ids::VisualizeEntitySupportingBlocks, Ids::VisualizeBlockLightLevels,
                                        Ids::VisualizeSkyLightLevels, Ids::VisualizeSolidFaces,
                                        Ids::VisualizeChunksOnServer, Ids::VisualizeSkyLightSections,
                                        Ids::ChunkSectionVisibility }) {
                    s_entries[id] = std::make_unique<NoopEntry>();
                }
            }
            return s_entries;
        }
    }

    const std::map<std::string, std::unique_ptr<Entry>>& AllEntries() { return Registry(); }

    Entry* GetEntry(std::string_view id) {
        auto& reg = Registry();
        auto it = reg.find(std::string(id));
        return it == reg.end() ? nullptr : it->second.get();
    }

    const std::map<std::string, EntryStatus>& ProfileStatuses(Profile p) {
        // DebugScreenEntries static initialiser, verbatim.
        static const std::map<std::string, EntryStatus> s_default = {
            { Ids::ThreeDimensionalCrosshair,   EntryStatus::InOverlay },
            { Ids::GameVersion,                 EntryStatus::InOverlay },
            { Ids::Tps,                         EntryStatus::InOverlay },
            { Ids::Fps,                         EntryStatus::InOverlay },
            { Ids::Memory,                      EntryStatus::InOverlay },
            { Ids::SystemSpecs,                 EntryStatus::InOverlay },
            { Ids::PlayerPosition,              EntryStatus::InOverlay },
            { Ids::PlayerSectionPosition,       EntryStatus::InOverlay },
            { Ids::SimplePerformanceImpactors,  EntryStatus::InOverlay },
        };
        static const std::map<std::string, EntryStatus> s_performance = {
            { Ids::Tps,                         EntryStatus::InOverlay },
            { Ids::Fps,                         EntryStatus::AlwaysOn },
            { Ids::GpuUtilization,              EntryStatus::InOverlay },
            { Ids::Memory,                      EntryStatus::InOverlay },
            { Ids::SimplePerformanceImpactors,  EntryStatus::InOverlay },
        };
        return p == Profile::Performance ? s_performance : s_default;
    }

    // ── EntryList ───────────────────────────────────────────────────────

    EntryList::EntryList() { Load(); }

    std::string EntryList::ProfileFilePath() const {
        return (std::filesystem::path(Platform::g_gameDirectory.GetGameDirectory()) / "debug-profile.json").string();
    }

    bool EntryList::ShowOnlyReducedInfo() {
        // MC DebugScreenOverlay: options.reducedDebugInfo() OR the server's
        // reduced_debug_info rule (player.isReducedDebugInfo, mirrored here
        // through WorldRulesS2C).
        return Platform::g_gameSettings.GetReducedDebugInfo() ||
               Game::Rules::GetBool(Game::Rules::Id::ReducedDebugInfo);
    }

    void EntryList::ResetToProfile(Profile p) {
        m_hasProfile = true;
        m_profile = p;
        m_statuses = ProfileStatuses(p);
    }

    void EntryList::Load() {
        const std::string path = ProfileFilePath();
        std::ifstream in(path);
        if (!in) {
            ResetToProfile(Profile::Default);
            RebuildCurrentList();
            return;
        }
        const nlohmann::json j = nlohmann::json::parse(in, nullptr, false, true);
        bool ok = j.is_object();
        if (ok && j.contains("profile") && j["profile"].is_string()) {
            Profile p;
            if (ProfileFromName(j["profile"].get<std::string>(), p)) ResetToProfile(p);
            else ok = false;
        } else if (ok) {
            m_statuses.clear();
            m_hasProfile = false;
            if (j.contains("custom") && j["custom"].is_object()) {
                for (auto it = j["custom"].begin(); it != j["custom"].end(); ++it) {
                    std::string id = it.key();
                    if (id.rfind("minecraft:", 0) == 0) id.erase(0, 10);
                    EntryStatus st;
                    if (it.value().is_string() && StatusFromName(it.value().get<std::string>(), st)) {
                        m_statuses[id] = st;
                    }
                }
            }
        }
        if (!ok) {
            Log::Error("[DebugScreen] Couldn't read debug profile file %s, resetting to default", path.c_str());
            ResetToProfile(Profile::Default);
            Save();
        }
        RebuildCurrentList();
    }

    void EntryList::Save() const {
        nlohmann::json j = nlohmann::json::object();
        if (m_hasProfile) {
            j["profile"] = ProfileName(m_profile);
        } else {
            nlohmann::json custom = nlohmann::json::object();
            for (const auto& [id, st] : m_statuses) custom["minecraft:" + id] = StatusName(st);
            j["custom"] = custom;
        }
        std::ofstream out(ProfileFilePath());
        if (!out) {
            Log::Error("[DebugScreen] Failed to save debug profile file %s", ProfileFilePath().c_str());
            return;
        }
        out << j.dump();
    }

    void EntryList::LoadProfile(Profile p) {
        ResetToProfile(p);
        RebuildCurrentList();
    }

    EntryStatus EntryList::GetStatus(std::string_view id) const {
        auto it = m_statuses.find(std::string(id));
        return it == m_statuses.end() ? EntryStatus::Never : it->second;
    }

    bool EntryList::IsCurrentlyEnabled(std::string_view id) const {
        return std::find(m_currentlyEnabled.begin(), m_currentlyEnabled.end(), id) != m_currentlyEnabled.end();
    }

    void EntryList::SetStatus(std::string_view id, EntryStatus status) {
        m_hasProfile = false;
        m_statuses[std::string(id)] = status;
        RebuildCurrentList();
        Save();
    }

    bool EntryList::ToggleStatus(std::string_view id) {
        // MC DebugScreenEntryList.toggleStatus: an unset entry becomes
        // always-on; always-on turns off; in-overlay turns off when the
        // overlay is up (it was showing) and always-on when it is not; never
        // becomes in-overlay when the overlay is up, always-on otherwise.
        auto it = m_statuses.find(std::string(id));
        if (it == m_statuses.end()) { SetStatus(id, EntryStatus::AlwaysOn); return true; }
        switch (it->second) {
            case EntryStatus::AlwaysOn:
                SetStatus(id, EntryStatus::Never);
                return false;
            case EntryStatus::InOverlay:
                if (m_overlayVisible) { SetStatus(id, EntryStatus::Never); return false; }
                SetStatus(id, EntryStatus::AlwaysOn);
                return true;
            case EntryStatus::Never:
                SetStatus(id, m_overlayVisible ? EntryStatus::InOverlay : EntryStatus::AlwaysOn);
                return true;
        }
        return false;
    }

    void EntryList::SetOverlayVisible(bool visible) {
        if (m_overlayVisible == visible) return;
        m_overlayVisible = visible;
        RebuildCurrentList();
    }

    void EntryList::RebuildCurrentList() {
        m_currentlyEnabled.clear();
        const bool reduced = ShowOnlyReducedInfo();
        for (const auto& [id, status] : m_statuses) {
            if (status == EntryStatus::AlwaysOn || (m_overlayVisible && status == EntryStatus::InOverlay)) {
                const Entry* e = GetEntry(id);
                if (e && e->IsAllowed(reduced)) m_currentlyEnabled.push_back(id);
            }
        }
        // MC sorts by Identifier (path first); every id here is minecraft:<path>.
        std::sort(m_currentlyEnabled.begin(), m_currentlyEnabled.end());
        ++m_version;
    }

    EntryList& Entries() {
        static EntryList s_list;
        return s_list;
    }

} // namespace Render::DebugScreen
