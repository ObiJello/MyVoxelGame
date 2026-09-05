// File: src/client/renderer/gui/screens/WorldSelectScreens.cpp
#include "WorldSelectScreens.hpp"
#include <filesystem>
#include <unordered_set>
#include "server/world/storage/anvil/WorldFolder.hpp"
#include "server/world/storage/anvil/WorldSidecar.hpp"
#include "TitleScreen.hpp"   // TitleAction
#include "../GuiGraphics.hpp"
#include "../FontRenderer.hpp"
#include "platform/GameDirectory.hpp"
#include "common/core/Log.hpp"
#include "common/entity/Item.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "server/world/storage/SectionDataUnpacker.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "world/level/block/Blocks.h"   // terrain library (layer validity + picker filter)
#include <cmath>
#include <array>
#include <set>
#include <sstream>
#include <nlohmann/json.hpp>
#include <GLFW/glfw3.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <random>

namespace Render {

    // ═══════════════════════════ WorldList store ════════════════════════════

    namespace WorldList {

        static std::string FilePath() {
            return Platform::g_gameDirectory.GetGameDirectory() + "/worlds.json";
        }

        std::vector<WorldEntry> Load() {
            std::vector<WorldEntry> out;
            std::ifstream f(FilePath());
            if (!f.is_open()) return out;
            nlohmann::json j;
            try {
                f >> j;
            } catch (const std::exception& e) {
                Log::Warning("worlds.json parse failed: %s", e.what());
                return out;
            }
            for (const auto& w : j.value("worlds", nlohmann::json::array())) {
                WorldEntry e;
                e.name       = w.value("name", "Unnamed");
                e.seedText   = w.value("seedText", "");
                e.seed       = w.value("seed", 0);
                e.gameMode   = w.value("gameMode", 1);
                e.created    = w.value("created", 0LL);
                e.lastPlayed = w.value("lastPlayed", 0LL);
                // Creation options (absent in pre-tab worlds.json → defaults).
                e.difficulty         = w.value("difficulty", 2);
                e.allowCommands      = w.value("allowCommands", true);
                e.worldType          = w.value("worldType", 0);
                e.flatPreset         = w.value("flatPreset", std::string());
                e.flatLayers         = w.value("flatLayers", std::string());
                e.singleBiome        = w.value("singleBiome", std::string("minecraft:plains"));
                e.worldgenTweaks     = w.value("worldgenTweaks", std::string());
                e.generateStructures = w.value("generateStructures", true);
                e.bonusChest         = w.value("bonusChest", false);
                e.worldWrap          = w.value("worldWrap", 0);
                e.dimensionStack     = w.value("dimensionStack", false);
                e.dayTime            = w.value("dayTime", 6000LL);
                e.doDaylightCycle    = w.value("doDaylightCycle", false);
                e.skybox             = w.value("skybox", std::string("vanilla"));
                e.skyboxMode         = w.value("skyboxMode", 2);
                e.babyModels         = w.value("babyModels", std::string("new"));
                out.push_back(std::move(e));
            }
            // MC sorts by last-played, newest first.
            std::sort(out.begin(), out.end(), [](const WorldEntry& a, const WorldEntry& b) {
                return a.lastPlayed > b.lastPlayed;
            });
            return out;
        }

        void Save(const std::vector<WorldEntry>& worlds) {
            nlohmann::json j;
            j["worlds"] = nlohmann::json::array();
            for (const auto& e : worlds) {
                if (e.isMinecraftSave) continue;   // synthetic entry, never persisted
                j["worlds"].push_back({
                    {"name", e.name},
                    {"seedText", e.seedText},
                    {"seed", e.seed},
                    {"gameMode", e.gameMode},
                    {"created", e.created},
                    {"lastPlayed", e.lastPlayed},
                    {"difficulty", e.difficulty},
                    {"allowCommands", e.allowCommands},
                    {"worldType", e.worldType},
                    {"flatPreset", e.flatPreset},
                    {"flatLayers", e.flatLayers},
                    {"singleBiome", e.singleBiome},
                    {"worldgenTweaks", e.worldgenTweaks},
                    {"generateStructures", e.generateStructures},
                    {"bonusChest", e.bonusChest},
                    {"worldWrap", e.worldWrap},
                    {"dimensionStack", e.dimensionStack},
                    {"dayTime", e.dayTime},
                    {"doDaylightCycle", e.doDaylightCycle},
                    {"skybox", e.skybox},
                    {"skyboxMode", e.skyboxMode},
                    {"babyModels", e.babyModels},
                });
            }
            std::ofstream f(FilePath());
            if (!f.is_open()) {
                Log::Warning("Could not write %s", FilePath().c_str());
                return;
            }
            f << j.dump(2);
        }

        int64_t ResolveSeed(const std::string& text) {
            // Trim whitespace.
            std::string s = text;
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))  s.pop_back();

            if (s.empty()) {
                static std::mt19937_64 rng(std::random_device{}());
                return static_cast<int64_t>(rng());
            }
            // MC WorldOptions.parseSeed: Long.parseLong, falling back to the
            // string's Java hashCode. The numeric branch is 64-bit — a real
            // Minecraft seed does not fit in 32 bits, and truncating it
            // generates a different world from the one that seed makes in
            // vanilla (different terrain AND different biomes at the same
            // coordinates).
            char* end = nullptr;
            long long v = std::strtoll(s.c_str(), &end, 10);
            if (end && *end == '\0') {
                return static_cast<int64_t>(v);
            }
            // Java String.hashCode(): h = 31*h + c over the characters. This
            // stays 32-bit and is then widened, exactly as Java does — the cast
            // to long happens after the hash in parseSeed.
            int32_t h = 0;
            for (unsigned char c : s) {
                h = static_cast<int32_t>(31u * static_cast<uint32_t>(h) + c);
            }
            return static_cast<int64_t>(h);
        }

    } // namespace WorldList

    namespace {
        long long NowEpoch() {
            return static_cast<long long>(
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());
        }

        std::string FormatDate(long long epochSeconds) {
            if (epochSeconds <= 0) return "?";
            std::time_t t = static_cast<std::time_t>(epochSeconds);
            std::tm tmv{};
#ifdef _WIN32
            localtime_s(&tmv, &t);
#else
            localtime_r(&t, &tmv);
#endif
            char buf[32];
            std::strftime(buf, sizeof(buf), "%m/%d/%y %H:%M", &tmv);
            return buf;
        }

        const char* GameModeName(int mode) {
            switch (mode) {
                case 0:  return "Survival";
                case 2:  return "Hardcore";
                default: return "Creative";
            }
        }

        // MC bounds every row line with StringWidget.setMaxWidth and shows the
        // full text in a tooltip. We have no per-row tooltips, so trim to fit
        // with an ellipsis — a long world name plus a snapshot version string
        // otherwise runs past the row and into the scrollbar.
        std::string Ellipsize(GuiGraphics& g, const std::string& text, int maxWidth) {
            if (text.empty() || g.GetStringWidth(text) <= maxWidth) return text;
            const int dotsW = g.GetStringWidth("...");
            if (maxWidth <= dotsW) return "...";
            std::string out;
            out.reserve(text.size());
            for (char c : text) {
                out.push_back(c);
                if (g.GetStringWidth(out) + dotsW > maxWidth) {
                    out.pop_back();
                    break;
                }
            }
            return out + "...";
        }

        // MC's GameType ordinal from level.dat's Data.GameType. NOT the same
        // numbering as GameModeName above, which uses our worlds.json ids
        // (where 2 means Hardcore, not Adventure).
        const char* McGameModeName(int gameType) {
            switch (gameType) {
                case 0:  return "Survival";
                case 1:  return "Creative";
                case 2:  return "Adventure";
                case 3:  return "Spectator";
                default: return "Survival";
            }
        }

        // Fill the pending TitleAction with a world and hand it to the host loop.
        void LaunchWorld(const WorldEntry& e) {
            TitleAction a;
            a.kind = TitleAction::Kind::Singleplayer;
            a.useMinecraftSave = e.isMinecraftSave;
            a.worldPath        = e.savePath;
            a.readOnlyWorld    = e.readOnly;
            a.worldName = e.name;
            a.seed      = e.seed;
            a.gameMode  = e.gameMode;
            a.generateStructures = e.generateStructures;
            a.worldType   = e.worldType;
            a.flatPreset  = e.flatPreset;
            a.flatLayers  = e.flatLayers;
            a.singleBiome = e.singleBiome;
            a.worldgenTweaks = e.worldgenTweaks;
            a.dayTime         = e.dayTime;
            a.doDaylightCycle = e.doDaylightCycle;
            a.difficulty      = e.difficulty;
            a.worldWrap       = e.worldWrap;
            a.dimensionStack  = e.dimensionStack;
            a.skybox          = e.skybox;
            a.skyboxMode      = e.skyboxMode;
            a.babyModels      = e.babyModels;
            SetTitleAction(std::move(a));
        }
    } // namespace

    // ═══════════════════════════ ConfirmScreen ══════════════════════════════

    void ConfirmScreen::Init() {
        const int cx = m_width / 2;
        const int y  = m_height / 6 + 96;
        AddWidget(new Button(cx - 155, y, 150, 20, m_yesLabel, [this] { Finish(true); }));
        AddWidget(new Button(cx + 5,   y, 150, 20, m_noLabel,  [this] { Finish(false); }));
    }

    void ConfirmScreen::Finish(bool yes) {
        if (m_finished) return;
        m_finished = true;
        // Pop FIRST so the callback can push a new screen on a clean stack.
        if (m_manager) m_manager->Pop();
        if (m_callback) m_callback(yes);
    }

    void ConfirmScreen::OnClose() { Finish(false); }

    void ConfirmScreen::Render(GuiGraphics& g, int mouseX, int mouseY, float partialTick) {
        Screen::Render(g, mouseX, mouseY, partialTick);
        g.DrawCenteredString(m_title, m_width / 2, 70, 0xFFFFFFFF);
        int y = 90;
        for (const auto& line : m_lines) {
            g.DrawCenteredString(line, m_width / 2, y, 0xFFA0A0A0);
            y += FontRenderer::LINE_HEIGHT + 2;
        }
    }

    // ═══════════════════════════ WorldListWidget ════════════════════════════

    double WorldListWidget::MaxScroll() const {
        double content = static_cast<double>(m_entries.size()) * ROW_H;
        double max = content - m_height + 4.0;
        return max > 0.0 ? max : 0.0;
    }

    int WorldListWidget::RowAt(double mouseX, double mouseY) const {
        if (!ContainsPoint(mouseX, mouseY)) return -1;
        const int rowX = m_x + (m_width - ROW_W) / 2;
        if (mouseX < rowX || mouseX >= rowX + ROW_W) return -1;
        int idx = static_cast<int>((mouseY - m_y - 2 + m_scroll) / ROW_H);
        if (idx < 0 || idx >= static_cast<int>(m_entries.size())) return -1;
        // Section headers occupy a row but are not worlds — clicking one must
        // not change the selection or the footer buttons would act on it.
        if (m_entries[idx].isHeader) return -1;
        return idx;
    }

    void WorldListWidget::OnClick(double mouseX, double mouseY) {
        const int row = RowAt(mouseX, mouseY);
        const long long now = static_cast<long long>(glfwGetTime() * 1000.0);
        if (row >= 0) {
            const bool doubleClick = (row == m_lastClickRow) && (now - m_lastClickMs < 250);
            m_selected = row;
            if (onSelectionChanged) onSelectionChanged();
            if (doubleClick && onDoubleClick) onDoubleClick();
        }
        m_lastClickRow = row;
        m_lastClickMs  = now;
    }

    bool WorldListWidget::OnScroll(double deltaY) {
        if (MaxScroll() <= 0.0) return false;
        m_scroll = std::clamp(m_scroll - deltaY * ROW_H, 0.0, MaxScroll());
        return true;
    }

    void WorldListWidget::RenderWidget(GuiGraphics& g, int mouseX, int mouseY, float) {
        m_scroll = std::clamp(m_scroll, 0.0, MaxScroll());
        g.Fill(m_x, m_y, m_x + m_width, m_y + m_height, 0x77000000);

        const int rowX = m_x + (m_width - ROW_W) / 2;
        g.EnableScissor(m_x, m_y, m_x + m_width, m_y + m_height);

        for (size_t i = 0; i < m_entries.size(); ++i) {
            const int top = m_y + 2 + static_cast<int>(i) * ROW_H - static_cast<int>(m_scroll);
            if (top + ROW_H < m_y || top > m_y + m_height) continue;
            const WorldEntry& e = m_entries[i];

            // Section divider. Reads as a heading, not as a third line of some
            // world's grey subtext: a full-width banner plate, bright text
            // centred in it, and rules on both sides of the label so the eye
            // parses it as a break in the list rather than another row.
            if (e.isHeader) {
                const int bandTop = top + 6;
                const int bandBot = top + ROW_H - 8;

                const int textW = g.GetStringWidth(e.name);
                const int textX = rowX + (ROW_W - textW) / 2;
                const int textY = (bandTop + bandBot) / 2 - FontRenderer::LINE_HEIGHT / 2;
                g.DrawString(e.name, textX, textY, 0xFFFFFFFF);

                const int ruleY = (bandTop + bandBot) / 2;
                if (textX - 8 > rowX + 4) {
                    g.Fill(rowX + 4, ruleY, textX - 8, ruleY + 1, 0x60FFFFFF);
                }
                if (textX + textW + 8 < rowX + ROW_W - 4) {
                    g.Fill(textX + textW + 8, ruleY, rowX + ROW_W - 4, ruleY + 1, 0x60FFFFFF);
                }
                continue;
            }

            // MC selection chrome: dark plate with a light border.
            if (static_cast<int>(i) == m_selected) {
                g.Fill(rowX - 2, top - 2, rowX + ROW_W + 2, top + ROW_H - 2, 0xFF808080);
                g.Fill(rowX - 1, top - 1, rowX + ROW_W + 1, top + ROW_H - 3, 0xFF000000);
            } else if (RowAt(mouseX, mouseY) == static_cast<int>(i)) {
                g.Fill(rowX - 1, top - 1, rowX + ROW_W + 1, top + ROW_H - 3, 0x30FFFFFF);
            }

            g.DrawString(e.name, rowX + 3, top + 1, 0xFFFFFFFF);
            std::string line2, line3;
            if (!e.infoLine1.empty() || !e.infoLine2.empty()) {
                // Anvil world summarised from level.dat — MC's own two grey
                // lines, verbatim in content and order.
                line2 = e.infoLine1;
                line3 = e.infoLine2;
            } else if (e.readOnly) {
                line2 = "Minecraft world";
                line3 = "Read-only - changes are not saved";
            } else if (e.isMinecraftSave) {
                line2 = "Minecraft save (saves/world)";
                line3 = "Loaded from disk - block changes persist";
            } else {
                line2 = std::string(GameModeName(e.gameMode)) + ", Seed: " + std::to_string(e.seed);
                line3 = "Created " + FormatDate(e.created) + " - regenerates on join";
            }
            g.DrawString(Ellipsize(g, line2, ROW_W - 6), rowX + 3,
                         top + 1 + FontRenderer::LINE_HEIGHT + 2, 0xFF808080);
            g.DrawString(Ellipsize(g, line3, ROW_W - 6), rowX + 3,
                         top + 1 + 2 * (FontRenderer::LINE_HEIGHT + 2), 0xFF808080);
        }
        g.DisableScissor();

        // Scrollbar (right of the row column, MC position).
        if (MaxScroll() > 0.0) {
            const int sx = rowX + ROW_W + 4;
            g.BlitSprite("widget/scroller_background", sx, m_y, 6, m_height);
            double thumbH = std::max(32.0, static_cast<double>(m_height) * m_height /
                                     (static_cast<double>(m_entries.size()) * ROW_H));
            double frac = m_scroll / MaxScroll();
            int thumbY = m_y + static_cast<int>(frac * (m_height - thumbH));
            g.BlitSprite("widget/scroller", sx, thumbY, 6, static_cast<int>(thumbH));
        }
    }

    // ═══════════════════════════ SelectWorldScreen ══════════════════════════

    void SelectWorldScreen::Init() {
        // List area: MC SelectWorldScreen — from below the title to above the
        // two footer button rows.
        m_list = AddWidget(new WorldListWidget(0, 48, m_width, m_height - 48 - 64));

        std::vector<WorldEntry> entries;

        // Headings only make sense when there are two sections to tell apart.
        // With no Minecraft install the list is just your worlds, and a lone
        // "Your ObeyCraft Worlds" banner over them would be noise.
        const auto mcWorlds = Platform::GameDirectory::ListMinecraftWorlds();
        const bool sectioned = !mcWorlds.empty();

        if (sectioned) {
            WorldEntry header;
            header.name     = "Your ObeyCraft Worlds";
            header.isHeader = true;
            entries.push_back(std::move(header));
        }

        // ObeyCraft's own worlds, read from the FOLDERS rather than from
        // worlds.json.
        //
        // A world folder is now self-describing: level.dat carries everything
        // vanilla has a home for (name, seed, game mode, difficulty, time,
        // gamerules) and data/obeycraft.json carries the handful it does not.
        // So a world dropped into obeycraft/saves by hand appears here, and a
        // deleted folder disappears — worlds.json no longer decides what
        // exists, it only remembers worlds that have not been given a folder
        // yet.
        std::unordered_set<std::string> foldersOnDisk;
        for (const auto& w : Platform::g_gameDirectory.ListObeyCraftWorlds()) {
            const auto sidecar = Game::Anvil::ReadWorldSidecar(w.path);

            WorldEntry e;
            e.name       = w.levelName;
            e.savePath   = w.path;
            e.seed       = w.seed;
            e.seedText   = std::to_string(w.seed);
            e.gameMode   = w.gameMode;
            e.difficulty = w.difficulty;
            e.lastPlayed = w.lastPlayed;
            e.created    = w.lastPlayed;
            e.allowCommands     = w.allowCommands;
            e.generateStructures = w.generateStructures;
            e.dayTime           = w.dayTime;
            e.doDaylightCycle   = w.doDaylightCycle;
            e.skybox         = sidecar.skybox;
            e.skyboxMode     = sidecar.skyboxMode;
            e.babyModels     = sidecar.babyModels;
            e.worldType      = sidecar.worldType;
            e.flatPreset     = sidecar.flatPreset;
            e.flatLayers     = sidecar.flatLayers;
            e.singleBiome    = sidecar.singleBiome;
            e.worldgenTweaks = sidecar.worldgenTweaks;
            e.bonusChest     = sidecar.bonusChest;
            foldersOnDisk.insert(Game::Anvil::SanitiseFolderName(w.levelName));
            entries.push_back(std::move(e));
        }

        // Worlds created before folders existed still live only in
        // worlds.json. They are listed too, and get a folder the first time
        // they are opened — same seed, so the same terrain.
        for (auto& e : WorldList::Load()) {
            if (foldersOnDisk.count(Game::Anvil::SanitiseFolderName(e.name))) continue;
            entries.push_back(std::move(e));
        }

        // Worlds from the player's real Minecraft installation, under their own
        // heading. Read-only — see WorldEntry::readOnly.
        if (sectioned) {
            WorldEntry header;
            header.name     = "Your Minecraft Worlds";
            header.isHeader = true;
            entries.push_back(std::move(header));

            for (const auto& w : mcWorlds) {
                WorldEntry e;
                e.name            = w.levelName;
                e.isMinecraftSave = true;
                e.readOnly        = true;
                e.savePath        = w.path;
                e.lastPlayed      = w.lastPlayed;

                // MC WorldSelectionList.WorldListEntry:1362-1366 — the folder
                // name, then the last-played timestamp in parentheses.
                e.infoLine1 = w.folderName;
                if (w.lastPlayed > 0) {
                    e.infoLine1 += " (" + FormatDate(w.lastPlayed) + ")";
                }

                // MC LevelSummary.createInfo():1136-1163 — game mode (Hardcore
                // replaces it outright), then cheats, then the version.
                e.infoLine2 = w.hardcore ? "Hardcore" : McGameModeName(w.gameMode);
                if (w.allowCommands) e.infoLine2 += ", Allow Cheats";
                if (!w.versionName.empty()) e.infoLine2 += ", Version: " + w.versionName;

                entries.push_back(std::move(e));
            }
            Log::Info("Select World: found %zu Minecraft world(s) in %s",
                      mcWorlds.size(),
                      Platform::GameDirectory::GetMinecraftSavesDirectory().c_str());
        }

        m_list->SetEntries(std::move(entries));
        m_list->onSelectionChanged = [this] { UpdateButtonStates(); };
        m_list->onDoubleClick      = [this] { PlaySelected(); };

        const int cx = m_width / 2;
        // Row 1 (MC): Play Selected World | Create New World
        m_playButton = AddWidget(new Button(cx - 154, m_height - 52, 150, 20,
            "Play Selected World", [this] { PlaySelected(); }));
        AddWidget(new Button(cx + 4, m_height - 52, 150, 20,
            "Create New World", [this] {
                m_manager->Push(std::make_unique<CreateWorldScreen>());
            }));
        // Row 2: Copy | Delete | Re-Create | Cancel.
        // The slot MC uses for Edit, which has never done anything here.
        m_copyButton = AddWidget(new Button(cx - 154, m_height - 28, 72, 20,
            "Copy", [this] { CopySelected(); }));
        m_copyButton->SetTooltip({"Copy a Minecraft world into ObeyCraft,",
                                  "where it can be edited and saved."});
        m_deleteButton = AddWidget(new Button(cx - 76, m_height - 28, 72, 20,
            "Delete", [this] { DeleteSelected(); }));
        m_recreateButton = AddWidget(new Button(cx + 4, m_height - 28, 72, 20,
            "Re-Create", [this] { RecreateSelected(); }));
        AddWidget(new Button(cx + 82, m_height - 28, 72, 20,
            "Cancel", [this] { OnClose(); }));

        UpdateButtonStates();
    }

    void SelectWorldScreen::UpdateButtonStates() {
        const WorldEntry* sel = m_list ? m_list->Selected() : nullptr;
        const bool has = sel != nullptr;
        if (m_playButton)     m_playButton->active     = has;
        if (m_recreateButton) m_recreateButton->active = has && !sel->isMinecraftSave;
        if (m_deleteButton)   m_deleteButton->active   = has && !sel->isMinecraftSave;
        // Copying only makes sense for an imported world — ours are already
        // where they belong.
        if (m_copyButton)     m_copyButton->active     = has && sel->isMinecraftSave && sel->readOnly;
    }

    void SelectWorldScreen::PlaySelected() {
        const WorldEntry* sel = m_list ? m_list->Selected() : nullptr;
        if (!sel) return;
        if (!sel->isMinecraftSave) {
            // Bump last-played so the list stays MC-sorted next time.
            auto worlds = WorldList::Load();
            for (auto& w : worlds) {
                if (w.name == sel->name && w.created == sel->created) {
                    w.lastPlayed = NowEpoch();
                    break;
                }
            }
            WorldList::Save(worlds);
        }
        LaunchWorld(*sel);
    }

    void SelectWorldScreen::DeleteSelected() {
        const WorldEntry* sel = m_list ? m_list->Selected() : nullptr;
        if (!sel || sel->isMinecraftSave) return;
        const std::string name = sel->name;
        const long long created = sel->created;
        m_manager->Push(std::make_unique<ConfirmScreen>(
            "Are you sure you want to delete this world?",
            std::vector<std::string>{"'" + name + "' will be removed from the list."},
            "Delete", "Cancel",
            [name, created](bool yes) {
                if (!yes) return;
                auto worlds = WorldList::Load();
                worlds.erase(std::remove_if(worlds.begin(), worlds.end(),
                    [&](const WorldEntry& w) {
                        return w.name == name && w.created == created;
                    }), worlds.end());
                WorldList::Save(worlds);

                // The folder IS the world now, so removing the json entry
                // alone would leave it listed. Routed through SaveRoot, which
                // refuses any path outside obeycraft/saves — a delete is the
                // last operation that should ever be handed a bare path.
                std::string reason;
                if (auto root = Game::Anvil::RootForWorldName(name, reason)) {
                    std::error_code ec;
                    const auto removed = std::filesystem::remove_all(root->Root(), ec);
                    if (ec) {
                        Log::Error("Could not delete '%s': %s", name.c_str(), ec.message().c_str());
                    } else if (removed > 0) {
                        Log::Info("Deleted world folder '%s' (%llu files)",
                                  name.c_str(), static_cast<unsigned long long>(removed));
                    }
                }
                // The parent SelectWorldScreen re-inits (and reloads the
                // list) when the confirm screen pops back to it.
            }));
    }

    void SelectWorldScreen::RecreateSelected() {
        const WorldEntry* sel = m_list ? m_list->Selected() : nullptr;
        if (!sel || sel->isMinecraftSave) return;
        m_manager->Push(std::make_unique<CreateWorldScreen>(*sel));
    }

    void SelectWorldScreen::CopySelected() {
        const WorldEntry* sel = m_list ? m_list->Selected() : nullptr;
        if (!sel || !sel->isMinecraftSave) return;

        // Pick a folder name that is free. MC dedups the same way.
        std::string name = sel->name;
        std::string reason;
        auto root = Game::Anvil::RootForWorldName(name, reason);
        for (int suffix = 2; root && Game::Anvil::LooksLikeWorld(*root) && suffix < 1000; ++suffix) {
            name = sel->name + " (" + std::to_string(suffix) + ")";
            root = Game::Anvil::RootForWorldName(name, reason);
        }
        if (!root) {
            Log::Error("Cannot copy '%s': %s", sel->name.c_str(), reason.c_str());
            return;
        }

        // The destination SaveRoot is resolved FIRST. The source is only ever
        // read, and it is read through std::filesystem::copy — no write API in
        // the Anvil stack accepts a bare path, so the original cannot be
        // touched even by mistake.
        Log::Info("Copying '%s' into ObeyCraft as '%s'...", sel->name.c_str(), name.c_str());

        std::error_code ec;
        std::filesystem::create_directories(root->Root(), ec);
        for (const auto& entry : std::filesystem::recursive_directory_iterator(sel->savePath, ec)) {
            if (ec) break;
            const auto relative = std::filesystem::relative(entry.path(), sel->savePath, ec);
            if (ec) continue;
            const std::string leaf = entry.path().filename().string();
            // session.lock belongs to whoever holds it, and level.dat_old is a
            // backup of a file we are about to rewrite anyway.
            if (leaf == "session.lock" || leaf == "level.dat_old") continue;

            const auto target = root->Root() / relative;
            if (entry.is_directory(ec)) {
                std::filesystem::create_directories(target, ec);
            } else {
                std::filesystem::create_directories(target.parent_path(), ec);
                std::filesystem::copy_file(entry.path(), target,
                                           std::filesystem::copy_options::overwrite_existing, ec);
            }
            if (ec) {
                Log::Error("Copy failed at %s: %s", relative.string().c_str(), ec.message().c_str());
                return;
            }
        }

        Log::Info("Copied to %s", root->Root().string().c_str());
        Init();   // rebuild the list so the copy shows up
    }

    void SelectWorldScreen::Render(GuiGraphics& g, int mouseX, int mouseY, float partialTick) {
        Screen::Render(g, mouseX, mouseY, partialTick);
        g.DrawCenteredString(m_title, m_width / 2, 16, 0xFFFFFFFF);
        RenderMenuSeparators(g, m_width, 46, m_height - 64);
        if (m_list && m_list->Entries().empty()) {
            g.DrawCenteredString("No worlds yet - create one!", m_width / 2,
                                 m_height / 2 - 4, 0xFF808080);
        }
    }

    // ═══════════════════════════ CreateWorldScreen ══════════════════════════

    namespace {
        constexpr int TAB_BAR_H = 24;   // MC TabNavigationBar height
        const char* kTabTitles[]      = {"Game", "World", "More"};
        const char* kGameModeNames[]  = {"Survival", "Hardcore", "Creative"};
        const char* kDifficultyNames[] = {"Peaceful", "Easy", "Normal", "Hard"};
        const char* kWorldTypeNames[] = {"Default", "Superflat", "Large Biomes",
                                         "AMPLIFIED", "Single Biome"};

        // Cycle-order index (Survival, Hardcore, Creative — MC order) from the
        // stored gameMode id (0 Survival, 1 Creative, 2 Hardcore) and back.
        int GameModeToCycle(int gameMode) {
            return gameMode == 0 ? 0 : (gameMode == 2 ? 1 : 2);
        }
        int CycleToGameMode(int idx) {
            return idx == 0 ? 0 : (idx == 1 ? 2 : 1);
        }
    } // namespace

    // ═══════════════════ World-type customization screens ═══════════════════
    namespace {
        // Vanilla flat presets (FlatLevelGeneratorPresets) with their
        // canonical PresetFlatWorldScreen strings ("<layers bottom-first>;
        // <biome>"). Index 0 = MC's default flat settings ("" preset id).
        // iconBlock: block id whose block-item is the preset's display item;
        // iconItem: pure-item override (vanilla water_bucket/feather/redstone).
        // Index 0 is MC's DEFAULT flat settings - it is the base for a fresh
        // flat world but is NOT a row in the vanilla preset list.
        struct FlatPresetRow {
            const char* id; const char* label; const char* layers;
            const char* iconBlock; Game::ItemID iconItem;
        };
        const FlatPresetRow kFlatPresets[] = {
            {"",                "Default",          "minecraft:bedrock,2*minecraft:dirt,minecraft:grass_block;minecraft:plains", "minecraft:grass_block", 0},
            {"classic_flat",    "Classic Flat",     "minecraft:bedrock,2*minecraft:dirt,minecraft:grass_block;minecraft:plains", "minecraft:grass_block", 0},
            {"tunnelers_dream", "Tunnelers' Dream", "minecraft:bedrock,230*minecraft:stone,5*minecraft:dirt,minecraft:grass_block;minecraft:windswept_hills", "minecraft:stone", 0},
            {"water_world",     "Water World",      "minecraft:bedrock,64*minecraft:deepslate,5*minecraft:stone,5*minecraft:dirt,5*minecraft:gravel,90*minecraft:water;minecraft:deep_ocean", nullptr, Game::Items::WaterBucket},
            {"overworld",       "Overworld",        "minecraft:bedrock,59*minecraft:stone,3*minecraft:dirt,minecraft:grass_block;minecraft:plains", "minecraft:short_grass", 0},
            {"snowy_kingdom",   "Snowy Kingdom",    "minecraft:bedrock,59*minecraft:stone,3*minecraft:dirt,minecraft:grass_block,minecraft:snow;minecraft:snowy_plains", "minecraft:snow", 0},
            {"bottomless_pit",  "Bottomless Pit",   "2*minecraft:cobblestone,3*minecraft:dirt,minecraft:grass_block;minecraft:plains", nullptr, Game::Items::Feather},
            {"desert",          "Desert",           "minecraft:bedrock,3*minecraft:stone,52*minecraft:sandstone,8*minecraft:sand;minecraft:desert", "minecraft:sand", 0},
            {"redstone_ready",  "Redstone Ready",   "minecraft:bedrock,3*minecraft:stone,116*minecraft:sandstone;minecraft:desert", nullptr, Game::Items::Redstone},
            {"the_void",        "The Void",         "minecraft:air;minecraft:the_void", "minecraft:barrier", 0},
        };
        constexpr int kFlatPresetCount = sizeof(kFlatPresets) / sizeof(kFlatPresets[0]);

        // ── Superflat customization: exact replica of MC's two screens ──
        // (CreateFlatWorldScreen + PresetFlatWorldScreen, decompiled 26.1).

        // One run of identical blocks; layersInfo is BOTTOM-first like Java.
        struct FlatLayerRowUI { int height; std::string blockId; };

        struct FlatWorkingSettings {
            std::string presetId;                 // base ("" = MC default flat)
            std::vector<FlatLayerRowUI> layers;   // bottom-first
            std::string biome = "minecraft:plains";
        };

        int FlatPresetIndexById(const std::string& id) {
            for (int i = 0; i < kFlatPresetCount; ++i) {
                if (id == kFlatPresets[i].id) return i;
            }
            return 0;
        }

        // "<layers>;<biome>" -> settings (PresetFlatWorldScreen.fromString's
        // client half; bad layer -> false, unknown biome tolerated - the
        // server validates and falls back to plains like vanilla).
        bool ParseFlatString(const std::string& text, std::vector<FlatLayerRowUI>& outLayers,
                             std::string& outBiome) {
            outLayers.clear();
            outBiome = "minecraft:plains";
            std::string layersPart = text, biomePart;
            auto semi = text.find(';');
            if (semi != std::string::npos) {
                layersPart = text.substr(0, semi);
                biomePart = text.substr(semi + 1);
                auto semi2 = biomePart.find(';');
                if (semi2 != std::string::npos) biomePart = biomePart.substr(0, semi2);
            }
            std::stringstream ss(layersPart);
            std::string spec;
            while (std::getline(ss, spec, ',')) {
                if (spec.empty()) return false;
                std::string blockId = spec;
                long long height = 1;
                auto star = spec.find('*');
                if (star != std::string::npos) {
                    try {
                        height = std::max<long long>(std::stoll(spec.substr(0, star)), 0);
                    } catch (...) { return false; }
                    blockId = spec.substr(star + 1);
                }
                if (blockId.empty()) return false;
                if (blockId.find(':') == std::string::npos) blockId = "minecraft:" + blockId;
                outLayers.push_back({static_cast<int>(std::min<long long>(height, 4064)), blockId});
            }
            if (outLayers.empty()) return false;
            if (!biomePart.empty()) {
                if (biomePart.find(':') == std::string::npos) biomePart = "minecraft:" + biomePart;
                outBiome = biomePart;
            }
            return true;
        }

        // PresetFlatWorldScreen.save(): "<h>*<id>|<id>, ... ;<biome>".
        std::string FlatSettingsToString(const FlatWorkingSettings& s) {
            std::string out;
            for (size_t i = 0; i < s.layers.size(); ++i) {
                if (i > 0) out += ",";
                if (s.layers[i].height != 1) out += std::to_string(s.layers[i].height) + "*";
                out += s.layers[i].blockId;
            }
            out += ";";
            out += s.biome;
            return out;
        }

        FlatWorkingSettings FlatSettingsFromDraft(const WorldEntry& draft) {
            FlatWorkingSettings s;
            s.presetId = draft.flatPreset;
            const FlatPresetRow& base = kFlatPresets[FlatPresetIndexById(draft.flatPreset)];
            const std::string& source =
                !draft.flatLayers.empty() ? draft.flatLayers : std::string(base.layers);
            if (!ParseFlatString(source, s.layers, s.biome)) {
                ParseFlatString(kFlatPresets[0].layers, s.layers, s.biome);
                s.presetId.clear();
            }
            return s;
        }

        // Vanilla CreateFlatWorldScreen row visuals: the item stack shown for
        // a layer (WATER -> water bucket, LAVA -> lava bucket, else the
        // block's own item) and its display name.
        Game::ItemStack FlatLayerDisplayItem(const std::string& blockId) {
            Game::ItemStack stack;
            stack.count = 1;
            if (blockId == "minecraft:water") {
                stack.itemId = Game::Items::WaterBucket;
            } else if (blockId == "minecraft:lava") {
                stack.itemId = Game::Items::LavaBucket;
            } else {
                Game::BlockStateRegistry::Initialize();
                Game::NbtBlockState st = Game::BlockStateRegistry::CreateBlockState(blockId);
                stack.itemId = Game::ItemRegistry::FromBlock(st.resolvedId);
            }
            return stack;
        }

        std::string FlatItemDisplayName(const Game::ItemStack& stack, const std::string& blockId) {
            if (blockId == "minecraft:air") return "Air";
            const Game::Item& item = Game::ItemRegistry::Get(stack.itemId);
            if (!item.name.empty()) return item.name;
            // Fallback: prettify the id (snake_case -> Title Case).
            std::string pretty = blockId.substr(blockId.find(':') + 1);
            bool up = true;
            for (char& c : pretty) {
                if (c == '_') { c = ' '; up = true; }
                else if (up) { c = static_cast<char>(std::toupper(static_cast<unsigned char>(c))); up = false; }
            }
            return pretty;
        }

        // Vanilla slot chrome: "container/slot" sprite 18x18 at (+1,+1), item
        // at (+2,+2). SLOT_BG_SIZE/SLOT_FG offsets from the decompiled screen.
        void BlitFlatSlot(GuiGraphics& g, int x, int y, const Game::ItemStack& stack) {
            g.BlitSprite("container/slot", x + 1, y + 1, 18, 18);
            if (stack.itemId != Game::Items::Air) {
                g.RenderItem(stack, x + 2, y + 2);
            }
        }

        void DrawUnderlinedString(GuiGraphics& g, const std::string& text, int x, int y,
                                  uint32_t color) {
            g.DrawString(text, x, y, color);
            g.Fill(x, y + FontRenderer::LINE_HEIGHT, x + g.GetStringWidth(text),
                   y + FontRenderer::LINE_HEIGHT + 1, color);
        }

        // ── CreateFlatWorldScreen's DetailsList ─────────────────────────────
        // ObjectSelectionList: row width 220 centered, item height 24; first
        // entry is the "Layer Material"/"Height" header (13 px), then the
        // layers TOP-first.
        class FlatLayerListWidget : public AbstractWidget {
        public:
            static constexpr int ROW_W = 220;      // ObjectSelectionList default
            static constexpr int ROW_H = 24;
            static constexpr int HEADER_H = 13;    // 9 * 1.5 (font height * 1.5)

            FlatLayerListWidget(int x, int y, int width, int height, FlatWorkingSettings* settings)
                : AbstractWidget(x, y, width, height, ""), m_settings(settings) {}

            std::function<void()> onSelectionChanged;

            // Selected LAYER index in top-first display order (-1 = none /
            // header). Vanilla selects rows, header included in children but
            // not selectable as a layer.
            int SelectedLayer() const { return m_selected; }

            void SetSelected(int row) {
                m_selected = row;
                if (onSelectionChanged) onSelectionChanged();
            }

            void Reset() {
                // Vanilla resetRows keeps the selection index when possible.
                int count = static_cast<int>(m_settings->layers.size());
                if (m_selected >= count) m_selected = count - 1;
                if (onSelectionChanged) onSelectionChanged();
            }

            void OnClick(double mouseX, double mouseY) override {
                // Height +/- mini-buttons on the selected row take priority.
                if (m_selected >= 0) {
                    int btn = HeightButtonAt(mouseX, mouseY, m_selected);
                    if (btn != 0) {
                        int n = static_cast<int>(m_settings->layers.size());
                        FlatLayerRowUI& layer =
                            m_settings->layers[static_cast<size_t>(n - 1 - m_selected)];
                        layer.height = std::clamp(layer.height + btn, 1, 384);
                        return;
                    }
                }
                int row = RowAt(mouseX, mouseY);
                if (row >= 0) {
                    m_selected = row;
                    if (onSelectionChanged) onSelectionChanged();
                    // Arm a potential drag-reorder from this row.
                    m_dragRow = row;
                    m_dragStartY = mouseY;
                    m_dragging = false;
                }
            }

            void OnDrag(double /*mouseX*/, double mouseY) override {
                if (m_dragRow < 0) return;
                if (!m_dragging && std::abs(mouseY - m_dragStartY) > 4.0) {
                    m_dragging = true;
                }
                m_dragMouseY = mouseY;
            }

            void OnRelease(double /*mouseX*/, double mouseY) override {
                if (m_dragging && m_dragRow >= 0) {
                    int n = static_cast<int>(m_settings->layers.size());
                    int target = InsertionRowAt(mouseY);
                    // Moving display row m_dragRow so it lands at display
                    // position `target` (after removal adjustment).
                    int from = m_dragRow;
                    if (target > from) target -= 1;
                    target = std::clamp(target, 0, n - 1);
                    if (target != from) {
                        // Display i <-> storage n-1-i (bottom-first storage).
                        FlatLayerRowUI moved =
                            m_settings->layers[static_cast<size_t>(n - 1 - from)];
                        m_settings->layers.erase(
                            m_settings->layers.begin() + (n - 1 - from));
                        m_settings->layers.insert(
                            m_settings->layers.begin() + (n - 1 - target), moved);
                        m_selected = target;
                        if (onSelectionChanged) onSelectionChanged();
                    }
                }
                m_dragRow = -1;
                m_dragging = false;
            }

            bool OnScroll(double deltaY) override {
                if (MaxScroll() <= 0.0) return false;
                m_scroll = std::clamp(m_scroll - deltaY * ROW_H, 0.0, MaxScroll());
                return true;
            }

        protected:
            void RenderWidget(GuiGraphics& g, int mouseX, int mouseY, float) override {
                m_scroll = std::clamp(m_scroll, 0.0, MaxScroll());
                g.Fill(m_x, m_y, m_x + m_width, m_y + m_height, 0x77000000);

                const int rowX = m_x + (m_width - ROW_W) / 2;
                g.EnableScissor(m_x, m_y, m_x + m_width, m_y + m_height);

                // Header entry (underlined column titles, right-aligned Height).
                int headerTop = m_y + 2 - static_cast<int>(m_scroll);
                DrawUnderlinedString(g, "Layer Material", rowX, headerTop, 0xFFFFFFFF);
                const char* heightTitle = "Height";
                DrawUnderlinedString(g, heightTitle,
                                     rowX + ROW_W - g.GetStringWidth(heightTitle),
                                     headerTop, 0xFFFFFFFF);

                const auto& layers = m_settings->layers;
                const int n = static_cast<int>(layers.size());
                for (int i = 0; i < n; ++i) {   // i = top-first display index
                    const int top = m_y + 2 + HEADER_H + i * ROW_H - static_cast<int>(m_scroll);
                    if (top + ROW_H < m_y || top > m_y + m_height) continue;
                    const FlatLayerRowUI& layer = layers[static_cast<size_t>(n - 1 - i)];

                    // Vanilla ObjectSelectionList selection chrome.
                    if (i == m_selected) {
                        g.Fill(rowX - 2, top - 2, rowX + ROW_W + 2, top + ROW_H - 2, 0xFF808080);
                        g.Fill(rowX - 1, top - 1, rowX + ROW_W + 1, top + ROW_H - 3, 0xFF000000);
                    } else if (RowAt(mouseX, mouseY) == i) {
                        g.Fill(rowX - 1, top - 1, rowX + ROW_W + 1, top + ROW_H - 3, 0x30FFFFFF);
                    }

                    Game::ItemStack stack = FlatLayerDisplayItem(layer.blockId);
                    BlitFlatSlot(g, rowX, top, stack);

                    const int textY = top + ROW_H / 2 - 2 - FontRenderer::LINE_HEIGHT / 2;
                    g.DrawString(FlatItemDisplayName(stack, layer.blockId),
                                 rowX + 18 + 5, textY, 0xFFFFFFFF);

                    // createWorld.customize.flat.layer(.top/.bottom):
                    // "Top - %s" / "Bottom - %s" / "%s".
                    std::string heightText = std::to_string(layer.height);
                    if (i == 0) heightText = "Top - " + heightText;
                    else if (i == n - 1) heightText = "Bottom - " + heightText;
                    // The selected row shows [-][+] height buttons; the height
                    // label shifts left to make room for them.
                    int heightRight = rowX + ROW_W;
                    if (i == m_selected) {
                        heightRight = rowX + ROW_W - 2 * (BTN + 2) - 4;
                        for (int b = 0; b < 2; ++b) {
                            int bx = rowX + ROW_W - (2 - b) * (BTN + 2);
                            int by = top + (ROW_H - 2 - BTN) / 2;
                            bool hot = mouseX >= bx && mouseX < bx + BTN &&
                                       mouseY >= by && mouseY < by + BTN;
                            g.Fill(bx, by, bx + BTN, by + BTN, hot ? 0xFFAAAAAA : 0xFF666666);
                            g.Fill(bx + 1, by + 1, bx + BTN - 1, by + BTN - 1,
                                   hot ? 0xFF3F3F5F : 0xFF202020);
                            const char* glyph = (b == 0) ? "-" : "+";
                            g.DrawString(glyph,
                                         bx + (BTN - g.GetStringWidth(glyph)) / 2 + 1,
                                         by + (BTN - FontRenderer::LINE_HEIGHT) / 2 + 1,
                                         0xFFFFFFFF);
                        }
                    }
                    g.DrawString(heightText, heightRight - g.GetStringWidth(heightText),
                                 textY, 0xFFFFFFFF);
                }

                // Drag-reorder feedback: insertion line + ghost of the row.
                if (m_dragging && m_dragRow >= 0 && m_dragRow < n) {
                    int target = InsertionRowAt(m_dragMouseY);
                    int lineY = m_y + 2 + HEADER_H + target * ROW_H
                              - static_cast<int>(m_scroll) - 2;
                    g.Fill(rowX - 2, lineY, rowX + ROW_W + 2, lineY + 2, 0xFFFFFFFF);

                    const FlatLayerRowUI& dragLayer =
                        layers[static_cast<size_t>(n - 1 - m_dragRow)];
                    int ghostTop = static_cast<int>(m_dragMouseY) - ROW_H / 2;
                    g.Fill(rowX - 1, ghostTop - 1, rowX + ROW_W + 1,
                           ghostTop + ROW_H - 3, 0x60FFFFFF);
                    Game::ItemStack ghostStack = FlatLayerDisplayItem(dragLayer.blockId);
                    BlitFlatSlot(g, rowX, ghostTop, ghostStack);
                    g.DrawString(FlatItemDisplayName(ghostStack, dragLayer.blockId),
                                 rowX + 18 + 5,
                                 ghostTop + ROW_H / 2 - 2 - FontRenderer::LINE_HEIGHT / 2,
                                 0xFFFFFFFF);
                }
                g.DisableScissor();

                if (MaxScroll() > 0.0) {
                    const int sx = rowX + ROW_W + 4;
                    g.BlitSprite("widget/scroller_background", sx, m_y, 6, m_height);
                    const double content = ContentHeight();
                    double thumbH = std::max(32.0, static_cast<double>(m_height) * m_height / content);
                    double frac = m_scroll / MaxScroll();
                    int thumbY = m_y + static_cast<int>(frac * (m_height - thumbH));
                    g.BlitSprite("widget/scroller", sx, thumbY, 6, static_cast<int>(thumbH));
                }
            }

        private:
            double ContentHeight() const {
                return 4.0 + HEADER_H +
                       static_cast<double>(m_settings->layers.size()) * ROW_H;
            }
            double MaxScroll() const {
                return std::max(0.0, ContentHeight() - m_height);
            }
            int RowAt(double mouseX, double mouseY) const {
                if (!ContainsPoint(mouseX, mouseY)) return -1;
                const int rowX = m_x + (m_width - ROW_W) / 2;
                if (mouseX < rowX - 2 || mouseX >= rowX + ROW_W + 2) return -1;
                double localY = mouseY - m_y - 2 - HEADER_H + m_scroll;
                if (localY < 0) return -1;
                int idx = static_cast<int>(localY / ROW_H);
                if (idx >= static_cast<int>(m_settings->layers.size())) return -1;
                return idx;
            }

            // Height mini-button hit test on a display row: -1 for [-],
            // +1 for [+], 0 for neither.
            int HeightButtonAt(double mouseX, double mouseY, int row) const {
                const int rowX = m_x + (m_width - ROW_W) / 2;
                const int top = m_y + 2 + HEADER_H + row * ROW_H
                              - static_cast<int>(m_scroll);
                for (int b = 0; b < 2; ++b) {
                    int bx = rowX + ROW_W - (2 - b) * (BTN + 2);
                    int by = top + (ROW_H - 2 - BTN) / 2;
                    if (mouseX >= bx && mouseX < bx + BTN &&
                        mouseY >= by && mouseY < by + BTN) {
                        return b == 0 ? -1 : +1;
                    }
                }
                return 0;
            }

            // Display insertion index (0..n) for a drop at this mouse y.
            int InsertionRowAt(double mouseY) const {
                double localY = mouseY - m_y - 2 - HEADER_H + m_scroll;
                int idx = static_cast<int>(std::floor(localY / ROW_H + 0.5));
                return std::clamp(idx, 0,
                                  static_cast<int>(m_settings->layers.size()));
            }

            static constexpr int BTN = 13;   // height +/- mini-button size

            FlatWorkingSettings* m_settings;
            int m_selected = -1;
            double m_scroll = 0.0;
            int m_dragRow = -1;
            bool m_dragging = false;
            double m_dragStartY = 0.0;
            double m_dragMouseY = 0.0;
        };

        // ── Add Layer: creative-inventory-style block picker ────────────────
        // A search bar, a scrolling 9-wide slot grid of every block the
        // terrain library can place, a height slider, and OK/Cancel.
        class FlatBlockPickerScreen : public Screen {
        public:
            // onAccept(blockId, height) fires on OK with a selection.
            FlatBlockPickerScreen(std::function<void(const std::string&, int)> onAccept)
                : Screen("Add Layer"), m_onAccept(std::move(onAccept)) {}

            void Init() override {
                const int cx = m_width / 2;

                m_search = AddWidget(new EditBox(cx - 100, 18, 200, 15, ""));
                m_search->SetMaxLength(128);
                m_search->SetHint("Search...");
                m_search->SetResponder([this](const std::string& v) {
                    if (m_grid) m_grid->Filter(v);
                });

                m_grid = AddWidget(new BlockGridWidget(0, 40, m_width, m_height - 40 - 80));
                m_grid->onSelectionChanged = [this] { UpdateButtonValidity(); };

                // Height slider: 1..384 (a layer taller than the world just
                // clips, like vanilla's preset strings).
                m_slider = AddWidget(new SliderButton(cx - 154, m_height - 56, 308, 20,
                    (m_height1 - 1) / 383.0,
                    [](double norm) {
                        return "Height: " + std::to_string(
                            1 + static_cast<int>(std::lround(norm * 383.0)));
                    },
                    [this](double norm) {
                        m_height1 = 1 + static_cast<int>(std::lround(norm * 383.0));
                    },
                    1.0 / 383.0));

                m_okButton = AddWidget(new Button(cx - 154, m_height - 28, 150, 20, "OK",
                    [this] {
                        if (m_grid && m_grid->HasSelection()) {
                            m_onAccept(m_grid->SelectedBlockId(), m_height1);
                            OnClose();
                        }
                    }));
                AddWidget(new Button(cx + 4, m_height - 28, 150, 20, "Cancel",
                                     [this] { OnClose(); }));
                UpdateButtonValidity();
            }

            void Render(GuiGraphics& g, int mouseX, int mouseY, float partialTick) override {
                Screen::Render(g, mouseX, mouseY, partialTick);
                g.DrawCenteredString(m_title, m_width / 2, 6, 0xFFFFFFFF);
                // Hovered block name, vanilla tooltip chrome. The renderer
                // batches per stratum (fills/sprites/text sort independently),
                // so hop to the next stratum or the panel lands UNDER the
                // grid's item icons - same pattern as Screen's tooltip pass.
                if (m_grid) {
                    std::string hover = m_grid->HoveredName(mouseX, mouseY);
                    if (!hover.empty()) {
                        g.NextStratum();
                        int w = g.GetStringWidth(hover);
                        int tx = mouseX + 12;
                        int ty = mouseY - 12;
                        if (tx + w + 8 > m_width) tx = m_width - w - 8;
                        if (ty < 0) ty = 0;
                        g.Fill(tx - 3, ty - 3, tx + w + 3,
                               ty + FontRenderer::LINE_HEIGHT + 3, 0xF0100010);
                        g.RenderOutline(tx - 3, ty - 3, w + 6,
                                        FontRenderer::LINE_HEIGHT + 6, 0xFF250559);
                        g.DrawString(hover, tx, ty, 0xFFFFFFFF);
                    }
                }
            }

        private:
            void UpdateButtonValidity() {
                if (m_okButton) m_okButton->active = m_grid && m_grid->HasSelection();
            }

            class BlockGridWidget : public AbstractWidget {
            public:
                static constexpr int COLS = 9;     // creative inventory grid
                static constexpr int CELL = 18;    // slot pitch

                BlockGridWidget(int x, int y, int width, int height)
                    : AbstractWidget(x, y, width, height, "") {
                    BuildEntries();
                    Filter("");
                }

                std::function<void()> onSelectionChanged;

                bool HasSelection() const { return !m_selectedBlockId.empty(); }
                const std::string& SelectedBlockId() const { return m_selectedBlockId; }

                void Filter(const std::string& filter) {
                    std::string needle = filter;
                    std::transform(needle.begin(), needle.end(), needle.begin(),
                                   [](unsigned char c) { return std::tolower(c); });
                    m_visible.clear();
                    for (size_t i = 0; i < m_entries.size(); ++i) {
                        std::string hay = m_entries[i].label;
                        std::transform(hay.begin(), hay.end(), hay.begin(),
                                       [](unsigned char c) { return std::tolower(c); });
                        if (needle.empty() || hay.find(needle) != std::string::npos) {
                            m_visible.push_back(static_cast<int>(i));
                        }
                    }
                    m_scroll = 0.0;
                }

                std::string HoveredName(double mouseX, double mouseY) const {
                    int cell = CellAt(mouseX, mouseY);
                    if (cell < 0) return "";
                    return m_entries[static_cast<size_t>(m_visible[cell])].label;
                }

                void OnClick(double mouseX, double mouseY) override {
                    int cell = CellAt(mouseX, mouseY);
                    if (cell >= 0) {
                        m_selectedBlockId =
                            m_entries[static_cast<size_t>(m_visible[cell])].blockId;
                        if (onSelectionChanged) onSelectionChanged();
                    }
                }

                bool OnScroll(double deltaY) override {
                    if (MaxScroll() <= 0.0) return false;
                    m_scroll = std::clamp(m_scroll - deltaY * CELL, 0.0, MaxScroll());
                    return true;
                }

            protected:
                void RenderWidget(GuiGraphics& g, int mouseX, int mouseY, float) override {
                    m_scroll = std::clamp(m_scroll, 0.0, MaxScroll());
                    g.Fill(m_x, m_y, m_x + m_width, m_y + m_height, 0x77000000);

                    const int gridX = GridX();
                    g.EnableScissor(m_x, m_y, m_x + m_width, m_y + m_height);
                    for (size_t v = 0; v < m_visible.size(); ++v) {
                        const int col = static_cast<int>(v) % COLS;
                        const int rowI = static_cast<int>(v) / COLS;
                        const int sx = gridX + col * CELL;
                        const int sy = m_y + 2 + rowI * CELL - static_cast<int>(m_scroll);
                        if (sy + CELL < m_y || sy > m_y + m_height) continue;

                        const Entry& e = m_entries[static_cast<size_t>(m_visible[v])];
                        g.BlitSprite("container/slot", sx, sy, 18, 18);
                        Game::ItemStack stack;
                        stack.itemId = e.itemId;
                        stack.count = 1;
                        g.RenderItem(stack, sx + 1, sy + 1);

                        const bool hovered = CellAt(mouseX, mouseY) == static_cast<int>(v);
                        if (e.blockId == m_selectedBlockId) {
                            // Selection frame.
                            g.Fill(sx - 1, sy - 1, sx + CELL + 1, sy, 0xFFFFFFFF);
                            g.Fill(sx - 1, sy + CELL, sx + CELL + 1, sy + CELL + 1, 0xFFFFFFFF);
                            g.Fill(sx - 1, sy, sx, sy + CELL, 0xFFFFFFFF);
                            g.Fill(sx + CELL, sy, sx + CELL + 1, sy + CELL, 0xFFFFFFFF);
                        } else if (hovered) {
                            g.Fill(sx + 1, sy + 1, sx + 17, sy + 17, 0x80FFFFFF);
                        }
                    }
                    g.DisableScissor();

                    if (MaxScroll() > 0.0) {
                        const int sx = gridX + COLS * CELL + 4;
                        g.BlitSprite("widget/scroller_background", sx, m_y, 6, m_height);
                        const double content = ContentHeight();
                        double thumbH = std::max(32.0,
                            static_cast<double>(m_height) * m_height / content);
                        double frac = m_scroll / MaxScroll();
                        int thumbY = m_y + static_cast<int>(frac * (m_height - thumbH));
                        g.BlitSprite("widget/scroller", sx, thumbY, 6,
                                     static_cast<int>(thumbH));
                    }
                }

            private:
                struct Entry {
                    std::string blockId;   // "minecraft:stone"
                    std::string label;     // display name
                    Game::ItemID itemId;   // block item for the icon
                };

                void BuildEntries() {
                    // Every game block (creative-inventory enumeration:
                    // BlockIDs in registry order, deduped by vanilla slug)
                    // that the TERRAIN LIBRARY can actually place as a layer
                    // - anything else would fall back to the default flat
                    // settings server-side.
                    minecraft::world::level::block::Blocks::bootstrap();
                    std::set<std::string> seen;
                    const int count = static_cast<int>(Game::BlockID::Count);
                    for (int i = 1; i < count; ++i) {
                        const auto& block = Game::BlockRegistry::Get(
                            static_cast<Game::BlockID>(i));
                        const std::string& slug = block.registrySlug;
                        if (slug.empty() || !seen.insert(slug).second) continue;
                        std::string mcId = "minecraft:" + slug;
                        if (minecraft::world::level::block::Blocks::getBlock(mcId) == nullptr) {
                            continue;
                        }
                        const auto& item = Game::ItemRegistry::Get(
                            static_cast<Game::ItemID>(i));
                        if (item.name.empty()) continue;
                        m_entries.push_back({std::move(mcId), item.name,
                                             static_cast<Game::ItemID>(i)});
                    }
                }

                int GridX() const {
                    return m_x + (m_width - COLS * CELL) / 2;
                }
                double ContentHeight() const {
                    int rows = (static_cast<int>(m_visible.size()) + COLS - 1) / COLS;
                    return 4.0 + rows * CELL;
                }
                double MaxScroll() const {
                    return std::max(0.0, ContentHeight() - m_height);
                }
                int CellAt(double mouseX, double mouseY) const {
                    if (!ContainsPoint(mouseX, mouseY)) return -1;
                    const int gridX = GridX();
                    if (mouseX < gridX || mouseX >= gridX + COLS * CELL) return -1;
                    double localY = mouseY - m_y - 2 + m_scroll;
                    if (localY < 0) return -1;
                    int col = static_cast<int>((mouseX - gridX) / CELL);
                    int row = static_cast<int>(localY / CELL);
                    int idx = row * COLS + col;
                    if (idx < 0 || idx >= static_cast<int>(m_visible.size())) return -1;
                    return idx;
                }

                std::vector<Entry> m_entries;
                std::vector<int> m_visible;
                std::string m_selectedBlockId;
                double m_scroll = 0.0;
            };

            std::function<void(const std::string&, int)> m_onAccept;
            EditBox* m_search = nullptr;
            SliderButton* m_slider = nullptr;
            Button* m_okButton = nullptr;
            BlockGridWidget* m_grid = nullptr;
            int m_height1 = 1;
        };

        // ── MC CreateFlatWorldScreen ────────────────────────────────────────
        class CreateFlatWorldScreenImpl : public Screen {
        public:
            explicit CreateFlatWorldScreenImpl(WorldEntry* draft)
                : Screen("Superflat Customization"), m_draft(draft),
                  m_settings(FlatSettingsFromDraft(*draft)) {}

            FlatWorkingSettings& Settings() { return m_settings; }

            // Vanilla setConfig - called by the preset screen on "Use Preset".
            void SetConfig(FlatWorkingSettings settings) {
                m_settings = std::move(settings);
                if (m_list) m_list->Reset();
                UpdateButtonValidity();
            }

            void Init() override {
                const int cx = m_width / 2;

                // DetailsList: y0 = 43, height = screen - 103 (decompiled ctor).
                m_list = AddWidget(new FlatLayerListWidget(0, 43, m_width, m_height - 103,
                                                           &m_settings));
                m_list->onSelectionChanged = [this] { UpdateButtonValidity(); };

                // Footer: two centered rows of 150x20 buttons, 8 px apart
                // horizontally, 4 px vertically (HeaderAndFooterLayout footer 64).
                const int rowTopY = m_height - 52;
                const int rowBotY = m_height - 28;
                AddWidget(new Button(cx - 154, rowTopY, 100, 20, "Add Layer", [this] {
                    m_manager->Push(std::make_unique<FlatBlockPickerScreen>(
                        [this](const std::string& blockId, int height) {
                            // Insert above the selected display row (top of
                            // the stack when nothing is selected). Display i
                            // <-> storage n-1-i, bottom-first storage.
                            int n = static_cast<int>(m_settings.layers.size());
                            int sel = m_list ? m_list->SelectedLayer() : -1;
                            int displayPos = (sel >= 0 && sel < n) ? sel : 0;
                            m_settings.layers.insert(
                                m_settings.layers.begin() + (n - displayPos),
                                FlatLayerRowUI{height, blockId});
                            if (m_list) {
                                m_list->Reset();
                                m_list->SetSelected(displayPos);
                            }
                            UpdateButtonValidity();
                        }));
                }));
                m_removeLayerButton = AddWidget(new Button(cx - 50, rowTopY, 100, 20,
                    "Remove Layer", [this] {
                        int sel = m_list ? m_list->SelectedLayer() : -1;
                        int n = static_cast<int>(m_settings.layers.size());
                        if (sel < 0 || sel >= n) return;
                        // Display is top-first; layersInfo is bottom-first.
                        m_settings.layers.erase(m_settings.layers.begin() + (n - 1 - sel));
                        m_list->Reset();
                        UpdateButtonValidity();
                    }));
                AddWidget(new Button(cx + 54, rowTopY, 100, 20, "Presets", [this] {
                    m_manager->Push(std::make_unique<PresetFlatWorldScreenWrapper>(this));
                }));
                AddWidget(new Button(cx - 154, rowBotY, 150, 20, "Done", [this] {
                    ApplyToDraft();
                    OnClose();
                }));
                AddWidget(new Button(cx + 4, rowBotY, 150, 20, "Cancel", [this] {
                    OnClose();   // vanilla: discard (Done is the only apply)
                }));

                UpdateButtonValidity();
            }

            void Render(GuiGraphics& g, int mouseX, int mouseY, float partialTick) override {
                Screen::Render(g, mouseX, mouseY, partialTick);
                // HeaderAndFooterLayout(33, ...) title, centered in the header.
                g.DrawCenteredString(m_title, m_width / 2,
                                     (33 - FontRenderer::LINE_HEIGHT) / 2 + 1, 0xFFFFFFFF);
            }

        private:
            void UpdateButtonValidity() {
                if (m_removeLayerButton) {
                    m_removeLayerButton->active =
                        m_list && m_list->SelectedLayer() >= 0 &&
                        m_list->SelectedLayer() < static_cast<int>(m_settings.layers.size());
                }
            }

            void ApplyToDraft() {
                m_draft->flatPreset = m_settings.presetId;
                const FlatPresetRow& base =
                    kFlatPresets[FlatPresetIndexById(m_settings.presetId)];
                std::string text = FlatSettingsToString(m_settings);
                m_draft->flatLayers = (text == base.layers) ? "" : text;
            }

            WorldEntry* m_draft;
            FlatWorkingSettings m_settings;
            FlatLayerListWidget* m_list = nullptr;
            Button* m_removeLayerButton = nullptr;

        public:
            class PresetFlatWorldScreenWrapper : public Screen {
            public:
                explicit PresetFlatWorldScreenWrapper(CreateFlatWorldScreenImpl* parent)
                    : Screen("Select a Preset"), m_parent(parent) {}

                void Init() override {
                    const int cx = m_width / 2;

                    // EditBox (50, 40, width-100, 20), max length 1230.
                    m_export = AddWidget(new EditBox(50, 40, m_width - 100, 20, "Preset"));
                    m_export->SetMaxLength(1230);
                    m_export->SetText(FlatSettingsToString(m_parent->Settings()));
                    m_export->SetResponder([this](const std::string&) {
                        UpdateButtonValidity();
                    });

                    // PresetsList: y0 = 80, height = screen - 117, rows 24.
                    m_list = AddWidget(new FlatPresetListWidget(0, 80, m_width, m_height - 117));
                    m_list->onSelect = [this](int presetIndex) {
                        // Vanilla Entry.select(): selecting fills the box with
                        // the preset's string.
                        m_selectedPreset = presetIndex;
                        if (m_export) m_export->SetText(kFlatPresets[presetIndex].layers);
                        UpdateButtonValidity();
                    };

                    // "Use Preset" (w/2-155, h-28) + "Cancel" (w/2+5, h-28).
                    m_selectButton = AddWidget(new Button(cx - 155, m_height - 28, 150, 20,
                        "Use Preset", [this] {
                            FlatWorkingSettings next;
                            const std::string text = m_export ? m_export->GetText() : "";
                            // Preset id carries when the box still holds the
                            // clicked preset's canonical string; the layers/
                            // biome always come from the box (fromString).
                            next.presetId = m_parent->Settings().presetId;
                            if (m_selectedPreset > 0 &&
                                text == kFlatPresets[m_selectedPreset].layers) {
                                next.presetId = kFlatPresets[m_selectedPreset].id;
                            }
                            if (!ParseFlatString(text, next.layers, next.biome)) {
                                // Vanilla fromString: unparseable -> default.
                                next.presetId.clear();
                                ParseFlatString(kFlatPresets[0].layers, next.layers, next.biome);
                            }
                            m_parent->SetConfig(std::move(next));
                            OnClose();
                        }));
                    AddWidget(new Button(cx + 5, m_height - 28, 150, 20, "Cancel",
                                         [this] { OnClose(); }));
                    UpdateButtonValidity();
                }

                void Render(GuiGraphics& g, int mouseX, int mouseY, float partialTick) override {
                    Screen::Render(g, mouseX, mouseY, partialTick);
                    g.DrawCenteredString(m_title, m_width / 2, 8, 0xFFFFFFFF);
                    // Grey labels at (51,30) and (51,68), color 0xFFA0A0A0
                    // (vanilla -6250336).
                    g.DrawString("Want to share your preset with someone? Use the below box!",
                                 51, 30, 0xFFA0A0A0);
                    g.DrawString("Alternatively, here's some we made earlier!",
                                 51, 68, 0xFFA0A0A0);
                }

            private:
                void UpdateButtonValidity() {
                    if (m_selectButton) {
                        m_selectButton->active =
                            m_selectedPreset > 0 ||
                            (m_export && m_export->GetText().length() > 1);
                    }
                }

                // ── MC PresetFlatWorldScreen.PresetsList ────────────────────
                class FlatPresetListWidget : public AbstractWidget {
                public:
                    static constexpr int ROW_W = 220;
                    static constexpr int ROW_H = 24;

                    FlatPresetListWidget(int x, int y, int width, int height)
                        : AbstractWidget(x, y, width, height, "") {}

                    std::function<void(int presetIndex)> onSelect;

                    void OnClick(double mouseX, double mouseY) override {
                        int row = RowAt(mouseX, mouseY);
                        if (row >= 0) {
                            m_selected = row;
                            if (onSelect) onSelect(row + 1);   // skip Default
                        }
                    }

                    bool OnScroll(double deltaY) override {
                        if (MaxScroll() <= 0.0) return false;
                        m_scroll = std::clamp(m_scroll - deltaY * ROW_H, 0.0, MaxScroll());
                        return true;
                    }

                protected:
                    void RenderWidget(GuiGraphics& g, int mouseX, int mouseY, float) override {
                        m_scroll = std::clamp(m_scroll, 0.0, MaxScroll());
                        g.Fill(m_x, m_y, m_x + m_width, m_y + m_height, 0x77000000);

                        const int rowX = m_x + (m_width - ROW_W) / 2;
                        g.EnableScissor(m_x, m_y, m_x + m_width, m_y + m_height);
                        for (int i = 0; i < kFlatPresetCount - 1; ++i) {
                            const int top = m_y + 2 + i * ROW_H - static_cast<int>(m_scroll);
                            if (top + ROW_H < m_y || top > m_y + m_height) continue;
                            const FlatPresetRow& preset = kFlatPresets[i + 1];

                            if (i == m_selected) {
                                g.Fill(rowX - 2, top - 2, rowX + ROW_W + 2, top + ROW_H - 2, 0xFF808080);
                                g.Fill(rowX - 1, top - 1, rowX + ROW_W + 1, top + ROW_H - 3, 0xFF000000);
                            } else if (RowAt(mouseX, mouseY) == i) {
                                g.Fill(rowX - 1, top - 1, rowX + ROW_W + 1, top + ROW_H - 3, 0x30FFFFFF);
                            }

                            Game::ItemStack stack;
                            stack.count = 1;
                            if (preset.iconItem != 0) {
                                stack.itemId = preset.iconItem;
                            } else {
                                Game::BlockStateRegistry::Initialize();
                                stack.itemId = Game::ItemRegistry::FromBlock(
                                    Game::BlockStateRegistry::CreateBlockState(preset.iconBlock)
                                        .resolvedId);
                            }
                            BlitFlatSlot(g, rowX, top, stack);
                            // Vanilla: name at contentX + 18 + 5, contentY + 6.
                            g.DrawString(preset.label, rowX + 18 + 5, top + 6, 0xFFFFFFFF);
                        }
                        g.DisableScissor();

                        if (MaxScroll() > 0.0) {
                            const int sx = rowX + ROW_W + 4;
                            g.BlitSprite("widget/scroller_background", sx, m_y, 6, m_height);
                            const double content = 4.0 + (kFlatPresetCount - 1) * ROW_H;
                            double thumbH = std::max(32.0,
                                static_cast<double>(m_height) * m_height / content);
                            double frac = m_scroll / MaxScroll();
                            int thumbY = m_y + static_cast<int>(frac * (m_height - thumbH));
                            g.BlitSprite("widget/scroller", sx, thumbY, 6,
                                         static_cast<int>(thumbH));
                        }
                    }

                private:
                    double MaxScroll() const {
                        return std::max(0.0,
                            4.0 + (kFlatPresetCount - 1) * ROW_H - m_height);
                    }
                    int RowAt(double mouseX, double mouseY) const {
                        if (!ContainsPoint(mouseX, mouseY)) return -1;
                        const int rowX = m_x + (m_width - ROW_W) / 2;
                        if (mouseX < rowX - 2 || mouseX >= rowX + ROW_W + 2) return -1;
                        double localY = mouseY - m_y - 2 + m_scroll;
                        if (localY < 0) return -1;
                        int idx = static_cast<int>(localY / ROW_H);
                        if (idx >= kFlatPresetCount - 1) return -1;
                        return idx;
                    }

                    int m_selected = -1;
                    double m_scroll = 0.0;
                };

                CreateFlatWorldScreenImpl* m_parent;
                EditBox* m_export = nullptr;
                Button* m_selectButton = nullptr;
                FlatPresetListWidget* m_list = nullptr;
                int m_selectedPreset = 0;   // kFlatPresets index (0 = none)
            };
        };

        // ── Single Biome customization: exact replica of MC's
        // CreateBuffetWorldScreen ("Single Biome Customization", decompiled
        // 26.1): centered title + 200x15 search box in the header, a
        // name-only biome list (15 px rows) filtered live by the search
        // text, Done/Cancel footer. Done applies the selection.

        // All 65 vanilla biomes, sorted by display name (vanilla sorts the
        // registry with a locale collator on the localized name).
        struct BuffetBiomeRow { const char* id; const char* label; };
        const BuffetBiomeRow kBuffetBiomes[] = {
            {"minecraft:badlands", "Badlands"},
            {"minecraft:bamboo_jungle", "Bamboo Jungle"},
            {"minecraft:basalt_deltas", "Basalt Deltas"},
            {"minecraft:beach", "Beach"},
            {"minecraft:birch_forest", "Birch Forest"},
            {"minecraft:cherry_grove", "Cherry Grove"},
            {"minecraft:cold_ocean", "Cold Ocean"},
            {"minecraft:crimson_forest", "Crimson Forest"},
            {"minecraft:dark_forest", "Dark Forest"},
            {"minecraft:deep_cold_ocean", "Deep Cold Ocean"},
            {"minecraft:deep_dark", "Deep Dark"},
            {"minecraft:deep_frozen_ocean", "Deep Frozen Ocean"},
            {"minecraft:deep_lukewarm_ocean", "Deep Lukewarm Ocean"},
            {"minecraft:deep_ocean", "Deep Ocean"},
            {"minecraft:desert", "Desert"},
            {"minecraft:dripstone_caves", "Dripstone Caves"},
            {"minecraft:end_barrens", "End Barrens"},
            {"minecraft:end_highlands", "End Highlands"},
            {"minecraft:end_midlands", "End Midlands"},
            {"minecraft:eroded_badlands", "Eroded Badlands"},
            {"minecraft:flower_forest", "Flower Forest"},
            {"minecraft:forest", "Forest"},
            {"minecraft:frozen_ocean", "Frozen Ocean"},
            {"minecraft:frozen_peaks", "Frozen Peaks"},
            {"minecraft:frozen_river", "Frozen River"},
            {"minecraft:grove", "Grove"},
            {"minecraft:ice_spikes", "Ice Spikes"},
            {"minecraft:jagged_peaks", "Jagged Peaks"},
            {"minecraft:jungle", "Jungle"},
            {"minecraft:lukewarm_ocean", "Lukewarm Ocean"},
            {"minecraft:lush_caves", "Lush Caves"},
            {"minecraft:mangrove_swamp", "Mangrove Swamp"},
            {"minecraft:meadow", "Meadow"},
            {"minecraft:mushroom_fields", "Mushroom Fields"},
            {"minecraft:nether_wastes", "Nether Wastes"},
            {"minecraft:ocean", "Ocean"},
            {"minecraft:old_growth_birch_forest", "Old Growth Birch Forest"},
            {"minecraft:old_growth_pine_taiga", "Old Growth Pine Taiga"},
            {"minecraft:old_growth_spruce_taiga", "Old Growth Spruce Taiga"},
            {"minecraft:pale_garden", "Pale Garden"},
            {"minecraft:plains", "Plains"},
            {"minecraft:river", "River"},
            {"minecraft:savanna", "Savanna"},
            {"minecraft:savanna_plateau", "Savanna Plateau"},
            {"minecraft:small_end_islands", "Small End Islands"},
            {"minecraft:snowy_beach", "Snowy Beach"},
            {"minecraft:snowy_plains", "Snowy Plains"},
            {"minecraft:snowy_slopes", "Snowy Slopes"},
            {"minecraft:snowy_taiga", "Snowy Taiga"},
            {"minecraft:soul_sand_valley", "Soul Sand Valley"},
            {"minecraft:sparse_jungle", "Sparse Jungle"},
            {"minecraft:stony_peaks", "Stony Peaks"},
            {"minecraft:stony_shore", "Stony Shore"},
            {"minecraft:sunflower_plains", "Sunflower Plains"},
            {"minecraft:swamp", "Swamp"},
            {"minecraft:taiga", "Taiga"},
            {"minecraft:the_end", "The End"},
            {"minecraft:the_void", "The Void"},
            {"minecraft:warm_ocean", "Warm Ocean"},
            {"minecraft:warped_forest", "Warped Forest"},
            {"minecraft:windswept_forest", "Windswept Forest"},
            {"minecraft:windswept_gravelly_hills", "Windswept Gravelly Hills"},
            {"minecraft:windswept_hills", "Windswept Hills"},
            {"minecraft:windswept_savanna", "Windswept Savanna"},
            {"minecraft:wooded_badlands", "Wooded Badlands"},
        };
        constexpr int kBuffetBiomeCount = sizeof(kBuffetBiomes) / sizeof(kBuffetBiomes[0]);


        class CreateBuffetWorldScreenImpl : public Screen {
        public:
            explicit CreateBuffetWorldScreenImpl(WorldEntry* draft)
                : Screen("Single Biome Customization"), m_draft(draft),
                  m_biome(draft->singleBiome.empty() ? "minecraft:plains"
                                                     : draft->singleBiome) {}

            void Init() override {
                const int cx = m_width / 2;

                // Header (13 + 9 + 3 + 15 = 40): title + search, centered.
                m_search = AddWidget(new EditBox(cx - 100, 18, 200, 15, ""));
                m_search->SetMaxLength(128);
                m_search->SetHint("Search...");
                m_search->SetResponder([this](const std::string& v) {
                    if (m_list) m_list->FilterEntries(v);
                });

                // BiomeList: y0 = header height (40), item height 15.
                m_list = AddWidget(new BuffetBiomeListWidget(0, 40, m_width,
                                                             m_height - 40 - 33, &m_biome));

                // Footer (33): Done + Cancel, 150x20, spacing 8, centered.
                m_doneButton = AddWidget(new Button(cx - 154, m_height - 27, 150, 20,
                    "Done", [this] {
                        m_draft->singleBiome = m_biome;
                        OnClose();
                    }));
                AddWidget(new Button(cx + 4, m_height - 27, 150, 20, "Cancel",
                                     [this] { OnClose(); }));

                m_list->onSelectionChanged = [this] {
                    if (m_doneButton) m_doneButton->active = m_list->HasSelection();
                };
                m_list->FilterEntries("");
                m_list->ScrollToBiome(m_biome);
                if (m_doneButton) m_doneButton->active = m_list->HasSelection();
            }

            void Render(GuiGraphics& g, int mouseX, int mouseY, float partialTick) override {
                Screen::Render(g, mouseX, mouseY, partialTick);
                g.DrawCenteredString(m_title, m_width / 2, 6, 0xFFFFFFFF);
            }

        private:
            // ── MC CreateBuffetWorldScreen.BiomeList ────────────────────────
            class BuffetBiomeListWidget : public AbstractWidget {
            public:
                static constexpr int ROW_W = 220;   // ObjectSelectionList default
                static constexpr int ROW_H = 15;

                BuffetBiomeListWidget(int x, int y, int width, int height,
                                      std::string* biome)
                    : AbstractWidget(x, y, width, height, ""), m_biome(biome) {}

                std::function<void()> onSelectionChanged;

                void FilterEntries(const std::string& filter) {
                    // Vanilla filterEntries: case-insensitive substring on the
                    // display name; the list is already display-name sorted.
                    std::string needle = filter;
                    std::transform(needle.begin(), needle.end(), needle.begin(),
                                   [](unsigned char c) { return std::tolower(c); });
                    m_visible.clear();
                    for (int i = 0; i < kBuffetBiomeCount; ++i) {
                        std::string hay = kBuffetBiomes[i].label;
                        std::transform(hay.begin(), hay.end(), hay.begin(),
                                       [](unsigned char c) { return std::tolower(c); });
                        if (needle.empty() || hay.find(needle) != std::string::npos) {
                            m_visible.push_back(i);
                        }
                    }
                    m_scroll = 0.0;
                    if (onSelectionChanged) onSelectionChanged();
                }

                bool HasSelection() const {
                    for (int idx : m_visible) {
                        if (*m_biome == kBuffetBiomes[idx].id) return true;
                    }
                    // Vanilla keeps the Done button usable while the selected
                    // biome exists, even when the filter hides its row.
                    for (int i = 0; i < kBuffetBiomeCount; ++i) {
                        if (*m_biome == kBuffetBiomes[i].id) return true;
                    }
                    return false;
                }

                void ScrollToBiome(const std::string& biome) {
                    for (size_t row = 0; row < m_visible.size(); ++row) {
                        if (biome == kBuffetBiomes[m_visible[row]].id) {
                            double target = static_cast<double>(row) * ROW_H
                                          - m_height / 2.0 + ROW_H / 2.0;
                            m_scroll = std::clamp(target, 0.0, MaxScroll());
                            return;
                        }
                    }
                }

                void OnClick(double mouseX, double mouseY) override {
                    int row = RowAt(mouseX, mouseY);
                    if (row >= 0) {
                        *m_biome = kBuffetBiomes[m_visible[static_cast<size_t>(row)]].id;
                        if (onSelectionChanged) onSelectionChanged();
                    }
                }

                bool OnScroll(double deltaY) override {
                    if (MaxScroll() <= 0.0) return false;
                    m_scroll = std::clamp(m_scroll - deltaY * ROW_H, 0.0, MaxScroll());
                    return true;
                }

            protected:
                void RenderWidget(GuiGraphics& g, int mouseX, int mouseY, float) override {
                    m_scroll = std::clamp(m_scroll, 0.0, MaxScroll());
                    g.Fill(m_x, m_y, m_x + m_width, m_y + m_height, 0x77000000);

                    const int rowX = m_x + (m_width - ROW_W) / 2;
                    g.EnableScissor(m_x, m_y, m_x + m_width, m_y + m_height);
                    for (size_t row = 0; row < m_visible.size(); ++row) {
                        const int top = m_y + 2 + static_cast<int>(row) * ROW_H
                                      - static_cast<int>(m_scroll);
                        if (top + ROW_H < m_y || top > m_y + m_height) continue;
                        const BuffetBiomeRow& biome = kBuffetBiomes[m_visible[row]];

                        if (*m_biome == biome.id) {
                            g.Fill(rowX - 2, top - 2, rowX + ROW_W + 2, top + ROW_H - 2, 0xFF808080);
                            g.Fill(rowX - 1, top - 1, rowX + ROW_W + 1, top + ROW_H - 3, 0xFF000000);
                        } else if (RowAt(mouseX, mouseY) == static_cast<int>(row)) {
                            g.Fill(rowX - 1, top - 1, rowX + ROW_W + 1, top + ROW_H - 3, 0x30FFFFFF);
                        }

                        // Vanilla: name at contentX + 5, contentY + 2.
                        g.DrawString(biome.label, rowX + 5, top + 2, 0xFFFFFFFF);
                    }
                    g.DisableScissor();

                    if (MaxScroll() > 0.0) {
                        const int sx = rowX + ROW_W + 4;
                        g.BlitSprite("widget/scroller_background", sx, m_y, 6, m_height);
                        const double content = 4.0 + m_visible.size() * ROW_H;
                        double thumbH = std::max(32.0,
                            static_cast<double>(m_height) * m_height / content);
                        double frac = m_scroll / MaxScroll();
                        int thumbY = m_y + static_cast<int>(frac * (m_height - thumbH));
                        g.BlitSprite("widget/scroller", sx, thumbY, 6,
                                     static_cast<int>(thumbH));
                    }
                }

            private:
                double MaxScroll() const {
                    return std::max(0.0, 4.0 + m_visible.size() * ROW_H - m_height);
                }
                int RowAt(double mouseX, double mouseY) const {
                    if (!ContainsPoint(mouseX, mouseY)) return -1;
                    const int rowX = m_x + (m_width - ROW_W) / 2;
                    if (mouseX < rowX - 2 || mouseX >= rowX + ROW_W + 2) return -1;
                    double localY = mouseY - m_y - 2 + m_scroll;
                    if (localY < 0) return -1;
                    int idx = static_cast<int>(localY / ROW_H);
                    if (idx >= static_cast<int>(m_visible.size())) return -1;
                    return idx;
                }

                std::string* m_biome;
                std::vector<int> m_visible;   // indices into kBuffetBiomes
                double m_scroll = 0.0;
            };

            WorldEntry* m_draft;
            std::string m_biome;
            EditBox* m_search = nullptr;
            Button* m_doneButton = nullptr;
            BuffetBiomeListWidget* m_list = nullptr;
        };
        // ── World Properties: sandbox worldgen knobs (NON-vanilla) ──────────
        // Everything here maps onto minecraft::levelgen::WorldGenTweaks;
        // defaults generate byte-identical vanilla worlds.
        struct TweaksUI {
            bool caves = true;
            std::array<bool, 11> steps{true, true, true, true, true, true,
                                       true, true, true, true, true};
            float featureDensity = 1.0f;
            float oreDensity = 1.0f;
            float vegetationDensity = 1.0f;
            float structureFrequency = 1.0f;
            std::set<std::string> disabledBiomes;

            bool IsDefault() const {
                if (!caves) return false;
                for (bool b : steps) if (!b) return false;
                return featureDensity == 1.0f && oreDensity == 1.0f &&
                       vegetationDensity == 1.0f && structureFrequency == 1.0f &&
                       disabledBiomes.empty();
            }

            static TweaksUI FromJson(const std::string& text) {
                TweaksUI t;
                if (text.empty()) return t;
                try {
                    nlohmann::json j = nlohmann::json::parse(text);
                    t.caves = j.value("caves", true);
                    if (j.contains("steps") && j["steps"].is_array()) {
                        for (size_t i = 0; i < t.steps.size() && i < j["steps"].size(); ++i) {
                            t.steps[i] = j["steps"][i].get<bool>();
                        }
                    }
                    t.featureDensity     = j.value("featureDensity", 1.0f);
                    t.oreDensity         = j.value("oreDensity", 1.0f);
                    t.vegetationDensity  = j.value("vegetationDensity", 1.0f);
                    t.structureFrequency = j.value("structureFrequency", 1.0f);
                    for (const auto& b : j.value("disabledBiomes", nlohmann::json::array())) {
                        t.disabledBiomes.insert(b.get<std::string>());
                    }
                } catch (...) { return TweaksUI(); }
                return t;
            }

            std::string ToJson() const {
                if (IsDefault()) return "";
                nlohmann::json j;
                j["caves"] = caves;
                j["steps"] = nlohmann::json::array();
                for (bool b : steps) j["steps"].push_back(b);
                j["featureDensity"] = featureDensity;
                j["oreDensity"] = oreDensity;
                j["vegetationDensity"] = vegetationDensity;
                j["structureFrequency"] = structureFrequency;
                j["disabledBiomes"] = nlohmann::json::array();
                for (const auto& b : disabledBiomes) j["disabledBiomes"].push_back(b);
                return j.dump();
            }
        };

        // The 11 non-overworld entries of kBuffetBiomes (nether/end/void) -
        // the biome checklist governs the OVERWORLD MultiNoise list only.
        bool IsOverworldBiomeId(const std::string& id) {
            static const std::set<std::string> kNonOverworld = {
                "minecraft:nether_wastes", "minecraft:soul_sand_valley",
                "minecraft:crimson_forest", "minecraft:warped_forest",
                "minecraft:basalt_deltas", "minecraft:the_end",
                "minecraft:end_highlands", "minecraft:end_midlands",
                "minecraft:small_end_islands", "minecraft:end_barrens",
                "minecraft:the_void"};
            return kNonOverworld.find(id) == kNonOverworld.end();
        }

        class WorldGenPropertiesScreen : public Screen {
        public:
            explicit WorldGenPropertiesScreen(WorldEntry* draft)
                : Screen("World Generation Properties"), m_draft(draft),
                  m_tweaks(TweaksUI::FromJson(draft->worldgenTweaks)) {}

            void Init() override {
                const int cx = m_width / 2;
                m_panel = AddWidget(new PropertiesPanel(0, 24, m_width, m_height - 24 - 40,
                                                        &m_tweaks, m_draft->worldType));
                AddWidget(new Button(cx - 156, m_height - 28, 100, 20, "Reset",
                    [this] { m_tweaks = TweaksUI(); }));
                AddWidget(new Button(cx - 50, m_height - 28, 100, 20, "Done",
                    [this] {
                        m_draft->worldgenTweaks = m_tweaks.ToJson();
                        OnClose();
                    }));
                AddWidget(new Button(cx + 56, m_height - 28, 100, 20, "Cancel",
                    [this] { OnClose(); }));
            }

            void Render(GuiGraphics& g, int mouseX, int mouseY, float partialTick) override {
                Screen::Render(g, mouseX, mouseY, partialTick);
                g.DrawCenteredString(m_title, m_width / 2, 8, 0xFFFFFFFF);
            }

        private:
            // One scrolling panel of mixed rows: section headers, checkboxes,
            // sliders, and the biome All/None buttons.
            class PropertiesPanel : public AbstractWidget {
            public:
                static constexpr int ROW_W = 280;
                static constexpr int ROW_H = 22;

                PropertiesPanel(int x, int y, int width, int height, TweaksUI* tweaks,
                                int worldType)
                    : AbstractWidget(x, y, width, height, ""), m_tweaks(tweaks),
                      m_worldType(worldType) {
                    BuildRows();
                }

                void OnClick(double mouseX, double mouseY) override {
                    int rowIndex = RowAt(mouseX, mouseY);
                    if (rowIndex < 0) return;
                    Row& row = m_rows[static_cast<size_t>(rowIndex)];
                    const int rowX = m_x + (m_width - ROW_W) / 2;
                    switch (row.kind) {
                        case Row::CHECK:
                            *row.value = !*row.value;
                            break;
                        case Row::BIOME: {
                            auto it = m_tweaks->disabledBiomes.find(row.biomeId);
                            if (it != m_tweaks->disabledBiomes.end()) {
                                m_tweaks->disabledBiomes.erase(it);
                            } else {
                                m_tweaks->disabledBiomes.insert(row.biomeId);
                            }
                            break;
                        }
                        case Row::ALLNONE: {
                            // Two half-width inline buttons: [All] [None].
                            bool right = mouseX >= rowX + ROW_W / 2;
                            if (!right) {
                                m_tweaks->disabledBiomes.clear();
                            } else {
                                for (int i = 0; i < kBuffetBiomeCount; ++i) {
                                    if (IsOverworldBiomeId(kBuffetBiomes[i].id)) {
                                        m_tweaks->disabledBiomes.insert(kBuffetBiomes[i].id);
                                    }
                                }
                            }
                            break;
                        }
                        case Row::SLIDER:
                            m_dragSliderRow = rowIndex;
                            ApplySliderMouse(row, mouseX);
                            break;
                        default: break;
                    }
                }

                void OnDrag(double mouseX, double /*mouseY*/) override {
                    if (m_dragSliderRow >= 0) {
                        ApplySliderMouse(m_rows[static_cast<size_t>(m_dragSliderRow)], mouseX);
                    }
                }

                void OnRelease(double, double) override { m_dragSliderRow = -1; }

                bool OnScroll(double deltaY) override {
                    if (MaxScroll() <= 0.0) return false;
                    m_scroll = std::clamp(m_scroll - deltaY * ROW_H, 0.0, MaxScroll());
                    return true;
                }

            protected:
                void RenderWidget(GuiGraphics& g, int mouseX, int mouseY, float) override {
                    m_scroll = std::clamp(m_scroll, 0.0, MaxScroll());
                    g.Fill(m_x, m_y, m_x + m_width, m_y + m_height, 0x77000000);
                    const int rowX = m_x + (m_width - ROW_W) / 2;
                    g.EnableScissor(m_x, m_y, m_x + m_width, m_y + m_height);

                    for (size_t i = 0; i < m_rows.size(); ++i) {
                        const int top = m_y + 4 + static_cast<int>(i) * ROW_H
                                      - static_cast<int>(m_scroll);
                        if (top + ROW_H < m_y || top > m_y + m_height) continue;
                        const Row& row = m_rows[i];
                        const int textY = top + (ROW_H - FontRenderer::LINE_HEIGHT) / 2;
                        const bool hover = RowAt(mouseX, mouseY) == static_cast<int>(i);

                        switch (row.kind) {
                            case Row::HEADER: {
                                int w = g.GetStringWidth(row.label);
                                int tx = rowX + (ROW_W - w) / 2;
                                g.DrawString(row.label, tx, textY, 0xFFFFFF55);
                                int ruleY = textY + FontRenderer::LINE_HEIGHT / 2;
                                if (tx - 8 > rowX) g.Fill(rowX, ruleY, tx - 8, ruleY + 1, 0x60FFFFFF);
                                if (tx + w + 8 < rowX + ROW_W)
                                    g.Fill(tx + w + 8, ruleY, rowX + ROW_W, ruleY + 1, 0x60FFFFFF);
                                break;
                            }
                            case Row::CHECK:
                            case Row::BIOME: {
                                bool on = (row.kind == Row::CHECK)
                                    ? *row.value
                                    : m_tweaks->disabledBiomes.find(row.biomeId)
                                          == m_tweaks->disabledBiomes.end();
                                if (hover) {
                                    g.Fill(rowX - 2, top, rowX + ROW_W + 2, top + ROW_H - 2,
                                           0x30FFFFFF);
                                }
                                // Checkbox chrome.
                                int bx = rowX, by = top + (ROW_H - 2 - 14) / 2;
                                g.Fill(bx, by, bx + 14, by + 14, 0xFF666666);
                                g.Fill(bx + 1, by + 1, bx + 13, by + 13, 0xFF202020);
                                if (on) {
                                    g.DrawString("x", bx + 4, by + 3, 0xFFFFFFFF);
                                }
                                g.DrawString(row.label, rowX + 20, textY,
                                             on ? 0xFFFFFFFF : 0xFF888888);
                                break;
                            }
                            case Row::SLIDER: {
                                // Vanilla-style slider: track + handle + label.
                                int sy = top + 1;
                                g.Fill(rowX, sy, rowX + ROW_W, sy + ROW_H - 4, 0xFF000000);
                                g.Fill(rowX + 1, sy + 1, rowX + ROW_W - 1, sy + ROW_H - 5,
                                       0xFF3F3F3F);
                                float norm = (*row.fvalue - row.min) / (row.max - row.min);
                                norm = std::clamp(norm, 0.0f, 1.0f);
                                int hx = rowX + 1 + static_cast<int>(norm * (ROW_W - 10));
                                g.Fill(hx, sy, hx + 8, sy + ROW_H - 4,
                                       (hover || m_dragSliderRow == static_cast<int>(i))
                                           ? 0xFFBFBFBF : 0xFF8F8F8F);
                                std::string label = row.label + ": " +
                                    std::to_string(static_cast<int>(
                                        std::lround(*row.fvalue * 100.0f))) + "%";
                                g.DrawString(label,
                                             rowX + (ROW_W - g.GetStringWidth(label)) / 2,
                                             textY, 0xFFFFFFFF);
                                break;
                            }
                            case Row::NOTE: {
                                g.DrawString(row.label,
                                             rowX + (ROW_W - g.GetStringWidth(row.label)) / 2,
                                             textY, 0xFFA0A0A0);
                                break;
                            }
                            case Row::ALLNONE: {
                                const char* labels[2] = {"All", "None"};
                                for (int b = 0; b < 2; ++b) {
                                    int bx0 = rowX + b * (ROW_W / 2 + 2);
                                    int bx1 = bx0 + ROW_W / 2 - 2;
                                    bool bh = hover && ((mouseX >= rowX + ROW_W / 2) == (b == 1));
                                    g.Fill(bx0, top, bx1, top + ROW_H - 4,
                                           bh ? 0xFF7F7F9F : 0xFF5F5F5F);
                                    g.Fill(bx0 + 1, top + 1, bx1 - 1, top + ROW_H - 5, 0xFF2A2A2A);
                                    g.DrawString(labels[b],
                                                 (bx0 + bx1 - g.GetStringWidth(labels[b])) / 2,
                                                 textY, 0xFFFFFFFF);
                                }
                                break;
                            }
                        }
                    }
                    g.DisableScissor();

                    if (MaxScroll() > 0.0) {
                        const int sx = rowX + ROW_W + 6;
                        g.BlitSprite("widget/scroller_background", sx, m_y, 6, m_height);
                        const double content = ContentHeight();
                        double thumbH = std::max(32.0,
                            static_cast<double>(m_height) * m_height / content);
                        double frac = m_scroll / MaxScroll();
                        int thumbY = m_y + static_cast<int>(frac * (m_height - thumbH));
                        g.BlitSprite("widget/scroller", sx, thumbY, 6,
                                     static_cast<int>(thumbH));
                    }
                }

            private:
                struct Row {
                    enum Kind { HEADER, CHECK, SLIDER, BIOME, ALLNONE, NOTE } kind = HEADER;
                    std::string label;
                    bool* value = nullptr;       // CHECK
                    float* fvalue = nullptr;     // SLIDER
                    float min = 0.0f, max = 1.0f;
                    std::string biomeId;         // BIOME
                };

                void BuildRows() {
                    auto header = [&](const char* t) {
                        Row r; r.kind = Row::HEADER; r.label = t; m_rows.push_back(r); };
                    auto check = [&](const char* t, bool* v) {
                        Row r; r.kind = Row::CHECK; r.label = t; r.value = v;
                        m_rows.push_back(r); };
                    auto slider = [&](const char* t, float* v, float lo, float hi) {
                        Row r; r.kind = Row::SLIDER; r.label = t; r.fvalue = v;
                        r.min = lo; r.max = hi; m_rows.push_back(r); };
                    auto note = [&](const char* t) {
                        Row r; r.kind = Row::NOTE; r.label = t; m_rows.push_back(r); };

                    // 0 Default, 1 Superflat, 2 Large Biomes, 3 AMPLIFIED,
                    // 4 Single Biome (WorldEntry::worldType).
                    const bool isFlat = (m_worldType == 1);
                    const bool fixedBiomeWorld = isFlat || (m_worldType == 4);

                    header("World");
                    if (isFlat) {
                        note("Superflat worlds have no caves.");
                    } else {
                        check("Caves & Canyons", &m_tweaks->caves);
                    }

                    header("Feature Groups");
                    check("Raw Generation",            &m_tweaks->steps[0]);
                    check("Lakes",                     &m_tweaks->steps[1]);
                    check("Icebergs & Local Changes",  &m_tweaks->steps[2]);
                    check("Dungeons & Fossils",        &m_tweaks->steps[3]);
                    check("Surface Structures",        &m_tweaks->steps[4]);
                    check("Ores",                      &m_tweaks->steps[6]);
                    check("Underground Decoration",    &m_tweaks->steps[7]);
                    check("Springs",                   &m_tweaks->steps[8]);
                    check("Trees & Vegetation",        &m_tweaks->steps[9]);
                    check("Snow & Ice Cover",          &m_tweaks->steps[10]);

                    header("Density");
                    slider("Feature Density",    &m_tweaks->featureDensity,    0.0f, 5.0f);
                    slider("Ore Density",        &m_tweaks->oreDensity,        0.0f, 5.0f);
                    slider("Vegetation Density", &m_tweaks->vegetationDensity, 0.0f, 5.0f);
                    slider("Structure Frequency", &m_tweaks->structureFrequency, 0.25f, 4.0f);

                    header("Biomes");
                    if (fixedBiomeWorld) {
                        note(isFlat
                            ? "This world's biome is set in Superflat Customization."
                            : "This world's biome is set in Single Biome Customization.");
                    } else {
                        { Row r; r.kind = Row::ALLNONE; m_rows.push_back(r); }
                        for (int i = 0; i < kBuffetBiomeCount; ++i) {
                            if (!IsOverworldBiomeId(kBuffetBiomes[i].id)) continue;
                            Row r; r.kind = Row::BIOME; r.label = kBuffetBiomes[i].label;
                            r.biomeId = kBuffetBiomes[i].id;
                            m_rows.push_back(r);
                        }
                    }
                }

                void ApplySliderMouse(Row& row, double mouseX) {
                    const int rowX = m_x + (m_width - ROW_W) / 2;
                    float norm = static_cast<float>((mouseX - rowX - 4) / (ROW_W - 8));
                    norm = std::clamp(norm, 0.0f, 1.0f);
                    // Snap to 5% steps so exact 100% (vanilla) is reachable.
                    float v = row.min + norm * (row.max - row.min);
                    v = std::round(v * 20.0f) / 20.0f;
                    *row.fvalue = std::clamp(v, row.min, row.max);
                }

                double ContentHeight() const {
                    return 8.0 + static_cast<double>(m_rows.size()) * ROW_H;
                }
                double MaxScroll() const {
                    return std::max(0.0, ContentHeight() - m_height);
                }
                int RowAt(double mouseX, double mouseY) const {
                    if (!ContainsPoint(mouseX, mouseY)) return -1;
                    const int rowX = m_x + (m_width - ROW_W) / 2;
                    if (mouseX < rowX - 2 || mouseX >= rowX + ROW_W + 2) return -1;
                    double localY = mouseY - m_y - 4 + m_scroll;
                    if (localY < 0) return -1;
                    int idx = static_cast<int>(localY / ROW_H);
                    if (idx >= static_cast<int>(m_rows.size())) return -1;
                    return idx;
                }

                TweaksUI* m_tweaks;
                int m_worldType = 0;
                std::vector<Row> m_rows;
                double m_scroll = 0.0;
                int m_dragSliderRow = -1;
            };

            WorldEntry* m_draft;
            TweaksUI m_tweaks;
            PropertiesPanel* m_panel = nullptr;
        };

    } // namespace

    void CreateWorldScreen::Init() {
        const int cx = m_width / 2;

        // Widget pointers belong to the PREVIOUS build (tab switch / resize
        // destroyed them) — null them so ApplyHardcoreCoupling never touches
        // a widget from another tab.
        m_nameBox          = nullptr;
        m_seedBox          = nullptr;
        m_difficultyButton = nullptr;
        m_commandsButton   = nullptr;
        m_bonusChestButton = nullptr;

        // ── Tab bar (MC TabNavigationBar: min(400, width), centered) ───────
        const int barW = std::min(400, m_width);
        const int tabW = barW / 3;
        const int barX = cx - barW / 2;
        for (int t = 0; t < 3; ++t) {
            auto* tab = AddWidget(new TabButton(barX + t * tabW, 0, tabW, TAB_BAR_H,
                kTabTitles[t], [this, t] {
                    // Deferred: rebuilding widgets mid-click would destroy the
                    // tab button we're inside of. Render() applies it.
                    if (t != m_tab) m_pendingTab = t;
                }));
            tab->selected = (t == m_tab);
        }

        const int contentTop = TAB_BAR_H + 16;
        switch (m_tab) {
            case TAB_GAME:  InitGameTab(contentTop);  break;
            case TAB_WORLD: InitWorldTab(contentTop); break;
            case TAB_MORE:  InitMoreTab(contentTop);  break;
        }

        // ── Footer (MC: Create / Cancel) ───────────────────────────────────
        AddWidget(new Button(cx - 155, m_height - 28, 150, 20,
            "Create New World", [this] { CreateAndJoin(); }));
        AddWidget(new Button(cx + 5, m_height - 28, 150, 20,
            "Cancel", [this] { OnClose(); }));
    }

    void CreateWorldScreen::InitGameTab(int contentTop) {
        const int cx = m_width / 2;
        int y = contentTop + 12;   // room for the "World Name" label

        // MC GameTab: single centered column; name box 208, buttons 210 wide.
        m_nameBox = AddWidget(new EditBox(cx - 104, y, 208, 20, "World Name"));
        m_nameBox->SetMaxLength(32);
        m_nameBox->SetText(m_draft.name.empty() ? "New World" : m_draft.name);
        m_draft.name = m_nameBox->GetText();
        m_nameBox->SetResponder([this](const std::string& v) { m_draft.name = v; });
        SetFocus(m_nameBox);
        y += 28;

        auto* gameMode = AddWidget(new CycleButton(cx - 105, y, 210, 20, "Game Mode",
            {kGameModeNames[0], kGameModeNames[1], kGameModeNames[2]},
            GameModeToCycle(m_draft.gameMode),
            [this](int idx) {
                m_draft.gameMode = CycleToGameMode(idx);
                ApplyHardcoreCoupling();
            }));
        gameMode->SetTooltip({"Survival: gather, craft, and survive.",
                              "Hardcore: survival at Hard difficulty,",
                              "  with only one life.",
                              "Creative: build freely with unlimited",
                              "  resources and flight."});
        y += 28;

        m_difficultyButton = AddWidget(new CycleButton(cx - 105, y, 210, 20, "Difficulty",
            {kDifficultyNames[0], kDifficultyNames[1], kDifficultyNames[2], kDifficultyNames[3]},
            (m_draft.difficulty >= 0 && m_draft.difficulty <= 3) ? m_draft.difficulty : 2,
            [this](int idx) { m_draft.difficulty = idx; }));
        m_difficultyButton->SetTooltip({"How dangerous the world is.",
                                        "Peaceful spawns no monsters and",
                                        "heals you; /difficulty changes it later."});
        y += 28;

        m_commandsButton = AddWidget(CycleButton::MakeOnOff(cx - 105, y, 210, 20,
            "Allow Commands", m_draft.allowCommands,
            [this](bool on) { m_draft.allowCommands = on; }));
        m_commandsButton->SetTooltip({"Commands like /tp are allowed.",
                                      "(Commands are currently always",
                                      "available in this engine.)"});

        ApplyHardcoreCoupling();
    }

    void CreateWorldScreen::InitWorldTab(int contentTop) {
        const int cx = m_width / 2;
        int y = contentTop;

        // MC WorldTab: two 150-wide columns with a 10px gutter (310 span).
        Button* customize = AddWidget(new Button(cx + 5, y, 150, 20, "Customize", nullptr));
        auto updateCustomize = [this, customize]() {
            // MC: only Superflat and Single Biome have customization screens.
            customize->active = (m_draft.worldType == 1 || m_draft.worldType == 4);
        };
        customize->SetOnPress([this] {
            if (m_draft.worldType == 1) {
                m_manager->Push(std::make_unique<CreateFlatWorldScreenImpl>(&m_draft));
            } else if (m_draft.worldType == 4) {
                m_manager->Push(std::make_unique<CreateBuffetWorldScreenImpl>(&m_draft));
            }
        });
        customize->SetTooltip({"Superflat: presets + layer string.",
                               "Single Biome: choose the biome."});
        auto* worldType = AddWidget(new CycleButton(cx - 155, y, 150, 20, "World Type",
            {kWorldTypeNames[0], kWorldTypeNames[1], kWorldTypeNames[2],
             kWorldTypeNames[3], kWorldTypeNames[4]},
            (m_draft.worldType >= 0 && m_draft.worldType <= 4) ? m_draft.worldType : 0,
            [this, updateCustomize](int idx) { m_draft.worldType = idx; updateCustomize(); }));
        worldType->SetTooltip({"Default, Superflat, Large Biomes,",
                               "AMPLIFIED, and Single Biome generate",
                               "exactly like Minecraft."});
        updateCustomize();
        y += 28 + 12;   // + room for the seed label

        m_seedBox = AddWidget(new EditBox(cx - 155, y, 310, 20, "Seed"));
        m_seedBox->SetMaxLength(32);
        m_seedBox->SetHint("Leave blank for a random seed");
        m_seedBox->SetText(m_draft.seedText);
        m_seedBox->SetResponder([this](const std::string& v) { m_draft.seedText = v; });
        y += 28;

        // MC SwitchGrid rows (full 310 span toggles).
        AddWidget(CycleButton::MakeOnOff(cx - 155, y, 310, 20,
            "Generate Structures", m_draft.generateStructures,
            [this](bool on) { m_draft.generateStructures = on; }))
            ->SetTooltip({"Villages, dungeons etc."});
        y += 24;

        m_bonusChestButton = AddWidget(CycleButton::MakeOnOff(cx - 155, y, 310, 20,
            "Bonus Chest", m_draft.bonusChest,
            [this](bool on) { m_draft.bonusChest = on; }));
        m_bonusChestButton->SetTooltip({"A chest with starter items near spawn.",
                                        "(Not implemented yet.)"});
        y += 24;

        // Immersive-portal world options. Default off.
        {
            static const int kWrapSizes[] = { 0, 512, 1024, 2048, 4096 };
            int wrapIdx = 0;
            for (int i = 0; i < 5; ++i) if (kWrapSizes[i] == m_draft.worldWrap) wrapIdx = i;
            auto* wrap = AddWidget(new CycleButton(cx - 155, y, 150, 20, "World Wrap",
                {"Off", "512", "1024", "2048", "4096"}, wrapIdx,
                [this](int idx) { m_draft.worldWrap = kWrapSizes[idx]; }));
            wrap->SetTooltip({"The world is this many blocks across and",
                              "its borders are portals onto the opposite",
                              "border: walk off one edge, arrive at the",
                              "other. The Nether wraps at an eighth."});
            auto* stack = AddWidget(CycleButton::MakeOnOff(cx + 5, y, 150, 20,
                "Dimension Stack", m_draft.dimensionStack,
                [this](bool on) { m_draft.dimensionStack = on; }));
            stack->SetTooltip({"The bottom of each dimension opens onto",
                               "the top of the next: Overworld over Nether",
                               "over End over Overworld. Seam bedrock",
                               "generates as stone."});
            y += 24;
        }

        AddWidget(new Button(cx - 155, y, 310, 20, "World Properties...", [this] {
            m_manager->Push(std::make_unique<WorldGenPropertiesScreen>(&m_draft));
        }))->SetTooltip({"Sandbox worldgen knobs: biome checklist,",
                         "feature groups, densities, structure",
                         "frequency. Defaults = pure vanilla."});

        ApplyHardcoreCoupling();
    }

    void CreateWorldScreen::InitMoreTab(int contentTop) {
        const int cx = m_width / 2;
        int y = contentTop;

        // MC MoreTab: single 210-wide column — Game Rules, Experiments,
        // Data Packs. None of these systems exist in the engine yet.
        const char* rows[]     = {"Game Rules...", "Experiments...", "Data Packs..."};
        const char* reasons[]  = {"Game rules are not implemented yet.",
                                  "No experimental features to toggle.",
                                  "Data packs are not supported yet."};
        for (int i = 0; i < 3; ++i) {
            Button* b = AddWidget(new Button(cx - 105, y, 210, 20, rows[i], nullptr));
            b->active = false;
            b->SetTooltip({reasons[i]});
            y += 28;
        }
    }

    void CreateWorldScreen::ApplyHardcoreCoupling() {
        const bool hardcore = (m_draft.gameMode == 2);
        if (hardcore) {
            m_draft.difficulty    = 3;       // Hard, locked (MC)
            m_draft.allowCommands = false;
            m_draft.bonusChest    = false;
        }
        // Widget pointers exist only for the currently built tab.
        if (m_difficultyButton) m_difficultyButton->active = !hardcore;
        if (m_commandsButton)   m_commandsButton->active   = !hardcore;
        if (m_bonusChestButton) m_bonusChestButton->active = !hardcore;
    }

    void CreateWorldScreen::CreateAndJoin() {
        WorldEntry e = m_draft;
        if (e.name.empty()) e.name = "New World";
        e.seed       = WorldList::ResolveSeed(e.seedText);
        e.created    = NowEpoch();
        e.lastPlayed = e.created;
        e.isMinecraftSave = false;

        auto worlds = WorldList::Load();
        // MC-style name dedup: "New World (2)", "New World (3)", …
        // Checked against BOTH worlds.json and the folders on disk, since a
        // world folder is now what really decides whether a name is taken.
        std::string base = e.name;
        int n = 2;
        auto taken = [&](const std::string& candidate) {
            for (const auto& w : worlds) if (w.name == candidate) return true;
            std::string reason;
            auto root = Game::Anvil::RootForWorldName(candidate, reason);
            return root && Game::Anvil::LooksLikeWorld(*root);
        };
        while (taken(e.name)) e.name = base + " (" + std::to_string(n++) + ")";

        // The DEDUPED name is what names the folder, so create it now rather
        // than letting the server derive a different one later.
        std::string reason;
        if (auto root = Game::Anvil::RootForWorldName(e.name, reason)) {
            e.savePath = root->Root().string();

            std::string error;
            if (!Game::Anvil::EnsureDirectories(*root, error)) {
                Log::Error("Could not create the world folder: %s", error.c_str());
            }

            // The engine-only settings, beside level.dat rather than inside
            // it — level.dat stays byte-vanilla so Minecraft and third-party
            // tools see exactly what they expect.
            Game::Anvil::WorldSidecar sidecar;
            sidecar.skybox         = e.skybox;
            sidecar.skyboxMode     = e.skyboxMode;
            sidecar.babyModels     = e.babyModels;
            sidecar.worldType      = e.worldType;
            sidecar.flatPreset     = e.flatPreset;
            sidecar.flatLayers     = e.flatLayers;
            sidecar.singleBiome    = e.singleBiome;
            sidecar.worldgenTweaks = e.worldgenTweaks;
            sidecar.bonusChest     = e.bonusChest;
            Game::Anvil::WriteWorldSidecar(e.savePath, sidecar);
        } else {
            Log::Error("Cannot create a folder for '%s': %s", e.name.c_str(), reason.c_str());
        }

        worlds.push_back(e);
        WorldList::Save(worlds);
        Log::Info("Created world '%s' (seed %d, %s, difficulty %s, type %s)",
                  e.name.c_str(), e.seed, GameModeName(e.gameMode),
                  kDifficultyNames[e.difficulty], kWorldTypeNames[e.worldType]);
        LaunchWorld(e);
    }

    void CreateWorldScreen::Render(GuiGraphics& g, int mouseX, int mouseY, float partialTick) {
        // Apply a deferred tab switch BEFORE any widget iteration this frame.
        if (m_pendingTab >= 0) {
            m_tab = m_pendingTab;
            m_pendingTab = -1;
            Resize(m_width, m_height);
        }

        Screen::Render(g, mouseX, mouseY, partialTick);

        // Header separator under the tab bar + footer separator (MC layout).
        RenderMenuSeparators(g, m_width, TAB_BAR_H - 2, m_height - 33);

        const int cx = m_width / 2;
        if (m_tab == TAB_GAME) {
            g.DrawString("World Name", cx - 104, TAB_BAR_H + 16, 0xFFA0A0A0);
        } else if (m_tab == TAB_WORLD) {
            // 12px above the seed box (box sits at contentTop + 40).
            g.DrawString("Seed for the world generator", cx - 155, TAB_BAR_H + 44,
                         0xFFA0A0A0);
            g.DrawCenteredString("Your world is saved in Minecraft's own format.",
                                 cx, m_height - 46, 0xFF606060);
        }
    }

} // namespace Render
