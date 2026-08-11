// File: src/client/renderer/gui/screens/WorldSelectScreens.cpp
#include "WorldSelectScreens.hpp"
#include "TitleScreen.hpp"   // TitleAction
#include "../GuiGraphics.hpp"
#include "../FontRenderer.hpp"
#include "platform/GameDirectory.hpp"
#include "common/core/Log.hpp"
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
                e.generateStructures = w.value("generateStructures", true);
                e.bonusChest         = w.value("bonusChest", false);
                e.dayTime            = w.value("dayTime", 6000LL);
                e.doDaylightCycle    = w.value("doDaylightCycle", false);
                e.skybox             = w.value("skybox", std::string("vanilla"));
                e.skyboxMode         = w.value("skyboxMode", 2);
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
                    {"generateStructures", e.generateStructures},
                    {"bonusChest", e.bonusChest},
                    {"dayTime", e.dayTime},
                    {"doDaylightCycle", e.doDaylightCycle},
                    {"skybox", e.skybox},
                    {"skyboxMode", e.skyboxMode},
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

        // Fill the pending TitleAction with a world and hand it to the host loop.
        void LaunchWorld(const WorldEntry& e) {
            TitleAction a;
            a.kind = TitleAction::Kind::Singleplayer;
            a.useMinecraftSave = e.isMinecraftSave;
            a.worldName = e.name;
            a.seed      = e.seed;
            a.gameMode  = e.gameMode;
            a.dayTime         = e.dayTime;
            a.doDaylightCycle = e.doDaylightCycle;
            a.skybox          = e.skybox;
            a.skyboxMode      = e.skyboxMode;
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
        return (idx >= 0 && idx < static_cast<int>(m_entries.size())) ? idx : -1;
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

            // MC selection chrome: dark plate with a light border.
            if (static_cast<int>(i) == m_selected) {
                g.Fill(rowX - 2, top - 2, rowX + ROW_W + 2, top + ROW_H - 2, 0xFF808080);
                g.Fill(rowX - 1, top - 1, rowX + ROW_W + 1, top + ROW_H - 3, 0xFF000000);
            } else if (RowAt(mouseX, mouseY) == static_cast<int>(i)) {
                g.Fill(rowX - 1, top - 1, rowX + ROW_W + 1, top + ROW_H - 3, 0x30FFFFFF);
            }

            g.DrawString(e.name, rowX + 3, top + 1, 0xFFFFFFFF);
            std::string line2, line3;
            if (e.isMinecraftSave) {
                line2 = "Minecraft save (saves/world)";
                line3 = "Loaded from disk - block changes persist";
            } else {
                line2 = std::string(GameModeName(e.gameMode)) + ", Seed: " + std::to_string(e.seed);
                line3 = "Created " + FormatDate(e.created) + " - regenerates on join";
            }
            g.DrawString(line2, rowX + 3, top + 1 + FontRenderer::LINE_HEIGHT + 2, 0xFF808080);
            g.DrawString(line3, rowX + 3, top + 1 + 2 * (FontRenderer::LINE_HEIGHT + 2), 0xFF808080);
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
        // The auto-detected Anvil save keeps today's behavior, listed first.
        if (Platform::g_gameDirectory.HasDefaultSaveWorld()) {
            WorldEntry mc;
            mc.name = "world";
            mc.isMinecraftSave = true;
            entries.push_back(std::move(mc));
        }
        for (auto& e : WorldList::Load()) entries.push_back(std::move(e));
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
        // Row 2 (MC): Edit | Delete | Re-Create | Cancel
        Button* edit = AddWidget(new Button(cx - 154, m_height - 28, 72, 20, "Edit", nullptr));
        edit->active = false;
        edit->SetTooltip({"Not available yet."});
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
                // The parent SelectWorldScreen re-inits (and reloads the
                // list) when the confirm screen pops back to it.
            }));
    }

    void SelectWorldScreen::RecreateSelected() {
        const WorldEntry* sel = m_list ? m_list->Selected() : nullptr;
        if (!sel || sel->isMinecraftSave) return;
        m_manager->Push(std::make_unique<CreateWorldScreen>(*sel));
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
                                        "(Not enforced by the engine yet.)"});
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
        auto* worldType = AddWidget(new CycleButton(cx - 155, y, 150, 20, "World Type",
            {kWorldTypeNames[0], kWorldTypeNames[1], kWorldTypeNames[2],
             kWorldTypeNames[3], kWorldTypeNames[4]},
            (m_draft.worldType >= 0 && m_draft.worldType <= 4) ? m_draft.worldType : 0,
            [this](int idx) { m_draft.worldType = idx; }));
        worldType->SetTooltip({"Only Default generates terrain right now;",
                               "other types are saved with the world for",
                               "when their generators exist."});
        Button* customize = AddWidget(new Button(cx + 5, y, 150, 20, "Customize", nullptr));
        customize->active = false;
        customize->SetTooltip({"No customizable world types yet."});
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
            ->SetTooltip({"Villages, dungeons etc.",
                          "(Structure generation is not",
                          "implemented yet.)"});
        y += 24;

        m_bonusChestButton = AddWidget(CycleButton::MakeOnOff(cx - 155, y, 310, 20,
            "Bonus Chest", m_draft.bonusChest,
            [this](bool on) { m_draft.bonusChest = on; }));
        m_bonusChestButton->SetTooltip({"A chest with starter items near spawn.",
                                        "(Not implemented yet.)"});

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
        std::string base = e.name;
        int n = 2;
        auto taken = [&](const std::string& candidate) {
            for (const auto& w : worlds) if (w.name == candidate) return true;
            return false;
        };
        while (taken(e.name)) e.name = base + " (" + std::to_string(n++) + ")";

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
            g.DrawCenteredString("Worlds regenerate from their seed each join - block changes are not saved yet.",
                                 cx, m_height - 46, 0xFF606060);
        }
    }

} // namespace Render
