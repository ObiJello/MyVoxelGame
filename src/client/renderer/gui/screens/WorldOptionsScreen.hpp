// File: src/client/renderer/gui/screens/WorldOptionsScreen.hpp
//
// MC 26.3 client/gui/screens/WorldOptionsScreen — the pause menu's "World
// Options..." (it replaced "Open to LAN"): General (default and personal game
// mode, Allow Cheats, difficulty + lock, Edit Game Rules, Restrictions) and,
// on the host, Multiplayer (joinable, port, guest command access, force game
// mode). Nothing is applied until "Apply Changes"; "Cancel" discards.
//
// Every change is sent as a command (/difficulty, /gamemode,
// /defaultgamemode, /worldoptions …) because the host is a TCP client of its
// own server here — the same route vanilla's packets take. The initial values
// are read straight from the integrated server when this client is the host;
// a remote client gets vanilla's non-host view (difficulty and game rules
// greyed out with the "requires operator permissions" tooltip).
//
// Deliberate differences from vanilla: the LAN switch is labelled
// "Joinable" and defaults ON (this server always listens); "Restrictions..."
// is greyed out — there is no chat-restriction system to configure.
#pragma once

#include "Screen.hpp"
#include "OptionsScreens.hpp"   // OptionsSubScreen (game rules screen)

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace Render {

    class Button;
    class CycleButton;
    class EditBox;
    class LockIconButton;
    class OptionsList;

    class WorldOptionsScreen : public Screen {
    public:
        WorldOptionsScreen() : Screen("World Options") {}

        void Init() override;
        void Render(GuiGraphics& g, int mouseX, int mouseY, float partialTick) override;

        // The local player's game mode (-1 = not known yet), pushed by the
        // frame loop; what "Personal Game Mode" starts on.
        static void SetClientGameMode(int mode);

    private:
        static constexpr int HEADER_H = 33;
        static constexpr int FOOTER_H = 33;

        bool IsHost() const;
        void UpdateApplyState();
        void UpdatePermissionDependentButtons();
        void UpdateGuestCommandAccessButton();
        void UpdateForceGameModeButton();
        void UpdatePortControls();
        bool HasChanges() const;
        bool PortRequired() const { return m_wantedJoinable; }
        void ApplyChanges();
        void SendCommand(const std::string& command);

        // ── State (MC's initial*/wanted* pairs) ────────────────────────────
        // Read once: Init also runs on every Resize — and the manager
        // resizes a screen whenever it becomes the top again (back from the
        // lock confirmation's "No", or from Edit Game Rules) — and vanilla
        // keeps its pending choices across that.
        bool m_stateLoaded = false;
        bool m_host = false;
        bool m_hardcore = false;
        int  m_initialDifficulty = 2,  m_wantedDifficulty = 2;
        bool m_initialLocked = false,  m_wantedLocked = false;
        int  m_initialDefaultMode = 0, m_wantedDefaultMode = 0;
        int  m_initialPersonalMode = -1, m_wantedPersonalMode = -1;
        bool m_initialAllowCommands = true, m_wantedAllowCommands = true;
        bool m_initialJoinable = true,  m_wantedJoinable = true;
        int  m_initialPort = 25565,     m_port = 25565;
        bool m_portValid = true;
        bool m_initialGuestAccess = false, m_wantedGuestAccess = false;
        bool m_initialForceMode = true,    m_wantedForceMode = true;

        // ── Widgets (owned by the list / screen) ───────────────────────────
        OptionsList*    m_list = nullptr;
        CycleButton*    m_defaultModeButton = nullptr;
        CycleButton*    m_personalModeButton = nullptr;
        CycleButton*    m_allowCommandsButton = nullptr;
        CycleButton*    m_difficultyButton = nullptr;
        LockIconButton* m_lockButton = nullptr;
        Button*         m_gameRulesButton = nullptr;
        CycleButton*    m_joinableButton = nullptr;
        EditBox*        m_portEdit = nullptr;
        CycleButton*    m_guestAccessButton = nullptr;
        CycleButton*    m_forceModeButton = nullptr;
        Button*         m_applyButton = nullptr;
    };

    // MC InWorldGameRulesScreen / EditGameRulesScreen, reduced to the rules
    // this engine has: one row per rule (label + Yes/No or a number box),
    // grouped under vanilla's category headers. Done sends a /gamerule per
    // changed rule; Cancel discards.
    class InWorldGameRulesScreen : public Screen {
    public:
        InWorldGameRulesScreen() : Screen("Edit Game Rules") {}
        void Init() override;
        void Render(GuiGraphics& g, int mouseX, int mouseY, float partialTick) override;
        // Escape / Cancel with pending changes asks "apply these changes?"
        // (GameRuleChangesScreen) instead of dropping them; Yes applies, and
        // either answer returns to the pause menu. Done applies at once.
        void OnClose() override;

    private:
        struct Change { std::string label, from, to; };
        std::vector<Change> PendingChanges() const;
        void ApplyPending();
        static constexpr int HEADER_H = 33;
        static constexpr int FOOTER_H = 33;
        // Width of the greyed "Not implemented yet" control (the value
        // controls are MC's 44 px).
        static constexpr int NOT_IMPLEMENTED_W = 110;

        struct Pending {
            std::string id;
            std::string label;
            bool        isInt = false;
            int         minValue = 0, maxValue = 0;
            std::string initial;
            std::string wanted;
            bool        valid = true;   // MC markInvalid: Done waits for a good number
        };
        std::vector<Pending> m_rules;
        // Read once: Init re-runs when this screen becomes the top again
        // (back from the apply question via Escape), and the edits must
        // survive that.
        bool         m_rulesLoaded = false;
        OptionsList* m_list = nullptr;
        Button*      m_doneButton = nullptr;
        void UpdateDoneState();
    };

    // "Are you sure you want to apply these changes?" with one line per
    // changed rule ("<rule>: <was> -> <now>") and Yes / No. Pops itself first,
    // then hands the answer to the callback (which pops the screens beneath).
    class GameRuleChangesScreen : public Screen {
    public:
        GameRuleChangesScreen(std::vector<std::string> lines, std::function<void(bool)> callback)
            : Screen("Apply Game Rule Changes?"), m_lines(std::move(lines)), m_callback(std::move(callback)) {}
        void Init() override;
        void Render(GuiGraphics& g, int mouseX, int mouseY, float partialTick) override;
        // Escape backs out to the game rules screen (no callback); the No
        // button is the one that discards and leaves.
        void OnClose() override;
    private:
        void Finish(bool yes);
        std::vector<std::string>  m_lines;
        std::function<void(bool)> m_callback;
        bool m_finished = false;
        // MC PopupScreen: a vertical layout (spacing 12) of bold title,
        // message, button row, centred in the screen inside popup/background.
        int m_contentX = 0, m_contentY = 0, m_contentW = 250, m_contentH = 0;
    };

} // namespace Render
