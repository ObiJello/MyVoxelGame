// File: src/client/renderer/gui/screens/WorldSelectScreens.hpp
//
// The Singleplayer flow — C++ counterparts of MC's SelectWorldScreen and
// CreateWorldScreen (classic layout), plus a reusable ConfirmScreen.
//
// World PERSISTENCE model (current stage): only world METADATA is saved —
// name, seed, game mode, timestamps — in {gameDir}/worlds.json. Joining a
// world regenerates its terrain from the stored seed every time (block
// edits are not persisted yet). The auto-detected Minecraft Anvil save
// (saves/world), when present, appears as a special list entry that keeps
// today's load-from-disk behavior.
//
// Seed resolution mirrors MC's WorldOptions.parseSeed():
//   empty        → random
//   parses as integer → that value (truncated to the engine's int32 seed)
//   anything else → Java String.hashCode() of the text
#pragma once

#include "Screen.hpp"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace Render {

    // ── World metadata store ────────────────────────────────────────────────
    struct WorldEntry {
        std::string name;
        std::string seedText;          // what the user typed ("" = random)
        int32_t     seed     = 0;      // resolved generator seed
        int         gameMode = 1;      // 0 = Survival, 1 = Creative, 2 = Hardcore
        long long   created    = 0;    // epoch seconds
        long long   lastPlayed = 0;    // epoch seconds
        bool isMinecraftSave = false;  // the detected Anvil save (not in json)

        // Creation options (MC CreateWorldScreen tabs). Persisted with the
        // world; the engine applies what it supports (world type Default is
        // the only generator today) and carries the rest until the systems
        // that consume them exist.
        int  difficulty         = 2;     // 0 Peaceful, 1 Easy, 2 Normal, 3 Hard
        bool allowCommands      = true;
        int  worldType          = 0;     // 0 Default, 1 Superflat, 2 Large Biomes,
                                         // 3 AMPLIFIED, 4 Single Biome
        bool generateStructures = true;
        bool bonusChest         = false;
    };

    namespace WorldList {
        // Load created worlds from worlds.json, newest-played first. Does NOT
        // include the Minecraft-save entry (SelectWorldScreen adds that).
        std::vector<WorldEntry> Load();
        void Save(const std::vector<WorldEntry>& worlds);
        // MC seed convention (see file header).
        int32_t ResolveSeed(const std::string& text);
    }

    // ── Generic yes/no dialog (MC ConfirmScreen) ────────────────────────────
    class ConfirmScreen : public Screen {
    public:
        ConfirmScreen(std::string title, std::vector<std::string> lines,
                      std::string yesLabel, std::string noLabel,
                      std::function<void(bool)> callback)
            : Screen(std::move(title)), m_lines(std::move(lines)),
              m_yesLabel(std::move(yesLabel)), m_noLabel(std::move(noLabel)),
              m_callback(std::move(callback)) {}

        void Init() override;
        void Render(GuiGraphics& g, int mouseX, int mouseY, float partialTick) override;
        void OnClose() override;   // ESC = "no"

    private:
        void Finish(bool yes);
        std::vector<std::string> m_lines;
        std::string m_yesLabel, m_noLabel;
        std::function<void(bool)> m_callback;
        bool m_finished = false;
    };

    // ── Scrollable selectable world list (MC WorldSelectionList) ────────────
    class WorldListWidget : public AbstractWidget {
    public:
        static constexpr int ROW_H  = 36;   // MC world row height
        static constexpr int ROW_W  = 270;  // MC world row width

        WorldListWidget(int x, int y, int width, int height)
            : AbstractWidget(x, y, width, height, "") {}

        void SetEntries(std::vector<WorldEntry> entries) { m_entries = std::move(entries); }
        const std::vector<WorldEntry>& Entries() const { return m_entries; }

        int  SelectedIndex() const { return m_selected; }
        const WorldEntry* Selected() const {
            return (m_selected >= 0 && m_selected < static_cast<int>(m_entries.size()))
                ? &m_entries[m_selected] : nullptr;
        }

        // Fired on selection change / double-click (join).
        std::function<void()>    onSelectionChanged;
        std::function<void()>    onDoubleClick;

        void OnClick(double mouseX, double mouseY) override;
        bool OnScroll(double deltaY) override;

    protected:
        void RenderWidget(GuiGraphics& g, int mouseX, int mouseY, float partialTick) override;

    private:
        int RowAt(double mouseX, double mouseY) const;
        double MaxScroll() const;

        std::vector<WorldEntry> m_entries;
        int    m_selected = -1;
        double m_scroll   = 0.0;
        long long m_lastClickMs  = 0;
        int       m_lastClickRow = -1;
    };

    // ── Select World ────────────────────────────────────────────────────────
    class SelectWorldScreen : public Screen {
    public:
        SelectWorldScreen() : Screen("Select World") {}
        void Init() override;
        void Render(GuiGraphics& g, int mouseX, int mouseY, float partialTick) override;

    private:
        void PlaySelected();
        void DeleteSelected();
        void RecreateSelected();
        void UpdateButtonStates();

        WorldListWidget* m_list = nullptr;
        Button* m_playButton     = nullptr;
        Button* m_deleteButton   = nullptr;
        Button* m_recreateButton = nullptr;
    };

    // ── Create New World (modern MC three-tab layout) ───────────────────────
    // Tabs mirror MC CreateWorldScreen:
    //   Game  — world name, game mode (Survival/Hardcore/Creative),
    //           difficulty, allow commands
    //   World — world type + customize, seed, generate structures, bonus chest
    //   More  — game rules / experiments / data packs (placeholders)
    // Hardcore couples options like vanilla: forces Hard difficulty and
    // disables difficulty/commands/bonus-chest.
    class CreateWorldScreen : public Screen {
    public:
        CreateWorldScreen() : Screen("Create New World") {
            m_draft.gameMode = 1;   // engine default: Creative
        }
        // Re-Create flow — all options pre-entered from the existing world.
        explicit CreateWorldScreen(const WorldEntry& prefill)
            : Screen("Create New World"), m_draft(prefill) {}

        void Init() override;
        void Render(GuiGraphics& g, int mouseX, int mouseY, float partialTick) override;

    private:
        enum Tab { TAB_GAME = 0, TAB_WORLD = 1, TAB_MORE = 2 };

        void CreateAndJoin();
        void InitGameTab(int contentTop);
        void InitWorldTab(int contentTop);
        void InitMoreTab(int contentTop);
        // Vanilla's hardcore coupling (difficulty locked to Hard, commands and
        // bonus chest off + disabled).
        void ApplyHardcoreCoupling();

        // The world being drafted — survives tab switches; name/seedText kept
        // in sync by the edit-box responders.
        WorldEntry m_draft;

        int  m_tab = TAB_GAME;
        int  m_pendingTab = -1;   // tab switch deferred to Render (safe rebuild)

        EditBox*     m_nameBox          = nullptr;
        EditBox*     m_seedBox          = nullptr;
        CycleButton* m_difficultyButton = nullptr;
        CycleButton* m_commandsButton   = nullptr;
        CycleButton* m_bonusChestButton = nullptr;
    };

} // namespace Render
