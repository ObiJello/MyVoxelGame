// File: src/client/renderer/gui/screens/WorldOptionsScreen.cpp
#include "WorldOptionsScreen.hpp"
#include "Widgets.hpp"
#include "WorldSelectScreens.hpp"   // ConfirmScreen
#include "../GuiGraphics.hpp"
#include "../FontRenderer.hpp"
#include "client/network/ClientConnection.hpp"
#include "client/network/NetworkClient.hpp"
#include "server/IntegratedServer.hpp"
#include "server/commands/GameRuleCommand.hpp"
#include "common/world/level/GameRules.hpp"
#include "common/core/Log.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <string>

namespace Render {

    namespace {
        int s_clientGameMode = -1;

        const char* kGameModeNames[4]  = {"Survival", "Creative", "Adventure", "Spectator"};
        const char* kGameModeCmd[4]    = {"survival", "creative", "adventure", "spectator"};
        const char* kDifficultyNames[4] = {"Peaceful", "Easy", "Normal", "Hard"};
        const char* kDifficultyCmd[4]   = {"peaceful", "easy", "normal", "hard"};

        // MC's tooltips (options.worldOptions.*, editGamerule.inGame.*,
        // menu.multiplayerOptions.network.*, lanServer.*).
        const std::vector<std::string> kDefaultModeTip        = {"Changes the default game mode of the world."};
        const std::vector<std::string> kPersonalModeTip       = {"Changes your game mode in this world."};
        const std::vector<std::string> kModeOperatorTip       = {"Changing the game mode requires", "operator permissions."};
        const std::vector<std::string> kModeHardcoreTip       = {"Cannot change the game mode in a", "hardcore world."};
        const std::vector<std::string> kAllowCommandsTip      = {"Allows the use of commands in this world."};
        const std::vector<std::string> kAllowCommandsHardTip  = {"Cannot allow commands in a hardcore world."};
        const std::vector<std::string> kDifficultyOperatorTip = {"Changing the difficulty requires", "operator permissions."};
        const std::vector<std::string> kDifficultyLockedTip   = {"Difficulty is locked."};
        const std::vector<std::string> kDifficultyHardcoreTip = {"Cannot change the difficulty in a", "hardcore world."};
        const std::vector<std::string> kGameRulesOperatorTip  = {"Updating game rules requires", "operator permissions."};
        const std::vector<std::string> kGameRulesHardcoreTip  = {"Cannot change game rules in a", "hardcore world."};
        const std::vector<std::string> kRestrictionsTip       = {"Not available in this game."};
        const std::vector<std::string> kJoinableOnTip         = {"Players on the same network can join."};
        const std::vector<std::string> kJoinableOffTip        = {"Nobody can join."};
        const std::vector<std::string> kPortTip               = {"Port Number"};
        const std::vector<std::string> kPortInvalidTip        = {"Not a valid port.", "Leave the edit box empty or enter a", "number between 1024 and 65535."};
        const std::vector<std::string> kGuestAccessTip        = {"Controls whether players that join your", "world can use commands or not."};
        const std::vector<std::string> kGuestAccessScopeTip   = {"Cannot change the command access of", "players joining your world when the", "world's multiplayer scope is set to \"Off\"."};
        const std::vector<std::string> kGuestAccessCmdTip     = {"Cannot change the command access of", "players joining your world when", "commands are not allowed."};
        const std::vector<std::string> kForceOnTip            = {"Other players will be forced to play the", "world's default game mode."};
        const std::vector<std::string> kForceOffTip           = {"Other players will retain their current", "game mode regardless of the world's", "default game mode."};
        const std::vector<std::string> kForceScopeTip         = {"Cannot change this setting when the", "world's multiplayer scope is set to \"Off\"."};
        const std::vector<std::string> kForceHardcoreTip      = {"Cannot change this setting in a", "hardcore world."};
        const std::vector<std::string> kForceCommandsTip      = {"Other players can set their own game", "mode through commands."};

        // MC EqualSpacingLayout(150): the difficulty button shrunk by the
        // lock's width, the lock at the right edge. One list cell.
        class DifficultyPair : public AbstractWidget {
        public:
            DifficultyPair(CycleButton* difficulty, LockIconButton* lock)
                : AbstractWidget(0, 0, 150, 20, ""), m_difficulty(difficulty), m_lock(lock) {}
            ~DifficultyPair() override { delete m_difficulty; delete m_lock; }

            const std::vector<std::string>* TooltipAt(double mx, double my) override {
                Place();
                if (m_lock->ContainsPoint(mx, my)) return m_lock->TooltipAt(mx, my);
                return m_difficulty->TooltipAt(mx, my);
            }
            void OnClick(double mx, double my) override {
                Place();
                AbstractWidget* hit = m_lock->IsMouseOver(mx, my) ? static_cast<AbstractWidget*>(m_lock)
                                    : m_difficulty->IsMouseOver(mx, my) ? static_cast<AbstractWidget*>(m_difficulty) : nullptr;
                if (m_focusedChild && m_focusedChild != hit) m_focusedChild->SetFocused(false);
                m_focusedChild = hit;
                if (hit) { hit->SetFocused(m_focused); hit->OnClick(mx, my); }
            }
            bool KeyPressed(int key, int mods) override {
                return m_focusedChild && m_focusedChild->KeyPressed(key, mods);
            }
            void SetFocused(bool f) override {
                AbstractWidget::SetFocused(f);
                if (!m_focusedChild) m_focusedChild = m_difficulty;
                m_focusedChild->SetFocused(f);
            }
        protected:
            void RenderWidget(GuiGraphics& g, int mouseX, int mouseY, float partialTick) override {
                Place();
                m_difficulty->Render(g, mouseX, mouseY, partialTick);
                m_lock->Render(g, mouseX, mouseY, partialTick);
            }
        private:
            void Place() {
                m_difficulty->SetPosition(m_x, m_y);
                m_lock->SetPosition(m_x + m_width - m_lock->GetWidth(), m_y);
            }
            CycleButton*    m_difficulty;
            LockIconButton* m_lock;
            AbstractWidget* m_focusedChild = nullptr;
        };

        bool HostServerRunning() {
            return Server::g_integratedServer != nullptr && Server::g_integratedServer->IsRunning();
        }
    }

    void WorldOptionsScreen::SetClientGameMode(int mode) { s_clientGameMode = mode; }

    bool WorldOptionsScreen::IsHost() const { return m_host; }

    void WorldOptionsScreen::SendCommand(const std::string& command) {
        if (!Client::g_networkClient) return;
        if (auto conn = Client::g_networkClient->GetConnection()) conn->SendChatMessage(command);
    }

    // ── Init ────────────────────────────────────────────────────────────────

    void WorldOptionsScreen::Init() {
        // Init runs again on every Resize (ClearWidgets has just deleted the
        // previous widgets), and the update helpers below fire during the
        // rebuild — the port box's SetText calls its responder, which reaches
        // UpdateApplyState and the Apply button. Every widget pointer must be
        // null until its widget exists, or that write lands in freed memory.
        // That was the heap corruption behind the Apply → No → Apply crash.
        m_list = nullptr;
        m_defaultModeButton = nullptr;
        m_personalModeButton = nullptr;
        m_allowCommandsButton = nullptr;
        m_difficultyButton = nullptr;
        m_lockButton = nullptr;
        m_gameRulesButton = nullptr;
        m_joinableButton = nullptr;
        m_portEdit = nullptr;
        m_guestAccessButton = nullptr;
        m_forceModeButton = nullptr;
        m_applyButton = nullptr;

        if (!m_stateLoaded) {
        m_stateLoaded = true;
        m_host = HostServerRunning();
        Server::IntegratedServer* server = m_host ? Server::g_integratedServer.get() : nullptr;

        // Initial state (WorldOptionsScreen.init / DifficultyButtons.create).
        if (server) {
            m_hardcore              = server->IsHardcore();
            m_initialDifficulty     = std::clamp(server->GetDifficulty(), 0, 3);
            m_initialLocked         = server->IsDifficultyLocked() || m_hardcore;
            m_initialDefaultMode    = std::clamp(server->GetWorldGameType(), 0, 3);
            m_initialAllowCommands  = server->IsAllowCommands();
            m_initialJoinable       = server->IsJoinable();
            m_initialPort           = server->GetPort();
            m_initialGuestAccess    = server->GetGuestCommandAccess();
            m_initialForceMode      = server->ForceGameMode();
        }
        m_initialPersonalMode = s_clientGameMode;
        m_wantedDifficulty    = m_initialDifficulty;
        m_wantedLocked        = m_initialLocked;
        m_wantedDefaultMode   = m_initialDefaultMode;
        m_wantedPersonalMode  = m_initialPersonalMode;
        m_wantedAllowCommands = m_initialAllowCommands;
        m_wantedJoinable      = m_initialJoinable;
        m_port                = m_initialPort;
        m_portValid           = true;
        m_wantedGuestAccess   = m_initialGuestAccess;
        m_wantedForceMode     = m_initialForceMode;
        }

        m_list = AddWidget(new OptionsList(0, HEADER_H, m_width, m_height - HEADER_H - FOOTER_H));

        // ── General ────────────────────────────────────────────────────────
        m_list->AddHeader("\xC2\xA7l\xC2\xA7nGeneral");
        const std::vector<std::string> modeNames(std::begin(kGameModeNames), std::end(kGameModeNames));

        if (m_host) {
            m_defaultModeButton = new CycleButton(0, 0, OptionsList::ROW_WIDTH, 20, "Default Game Mode",
                modeNames, m_wantedDefaultMode, [this](int i) { m_wantedDefaultMode = i; UpdateApplyState(); });
            m_list->AddBig(m_defaultModeButton);

            if (m_initialPersonalMode >= 0) {
                m_personalModeButton = new CycleButton(0, 0, OptionsList::ROW_WIDTH, 20, "Personal Game Mode",
                    modeNames, std::max(0, m_wantedPersonalMode), [this](int i) { m_wantedPersonalMode = i; UpdateApplyState(); });
                m_list->AddBig(m_personalModeButton);
            }

            m_allowCommandsButton = CycleButton::MakeOnOff(0, 0, 150, 20, "Allow Cheats", m_wantedAllowCommands,
                [this](bool on) {
                    m_wantedAllowCommands = on;
                    UpdateGuestCommandAccessButton();
                    UpdatePermissionDependentButtons();
                    UpdateApplyState();
                });
            if (m_hardcore) {
                m_allowCommandsButton->active = false;
                m_allowCommandsButton->SetTooltip(kAllowCommandsHardTip);
            } else {
                m_allowCommandsButton->SetTooltip(kAllowCommandsTip);
            }
        }

        // Difficulty + lock (DifficultyButtons).
        m_difficultyButton = new CycleButton(0, 0, 130, 20, "Difficulty",
            std::vector<std::string>(std::begin(kDifficultyNames), std::end(kDifficultyNames)),
            m_wantedDifficulty, [this](int i) { m_wantedDifficulty = i; UpdateApplyState(); });
        m_lockButton = new LockIconButton(0, 0, nullptr);
        m_lockButton->SetOnPress([this] {
            m_wantedLocked = !m_wantedLocked;
            m_lockButton->SetLocked(m_wantedLocked);
            UpdateApplyState();
        });
        m_lockButton->SetLocked(m_wantedLocked);
        auto* difficultyPair = new DifficultyPair(m_difficultyButton, m_lockButton);

        m_gameRulesButton = new Button(0, 0, 150, 20, "Edit Game Rules...", [this] {
            m_manager->Push(std::make_unique<InWorldGameRulesScreen>());
        });
        auto* restrictions = new Button(0, 0, 150, 20, "Restrictions...", nullptr);
        restrictions->active = false;
        restrictions->SetTooltip(kRestrictionsTip);

        if (m_host) {
            m_list->AddSmall(m_allowCommandsButton, difficultyPair);
            m_list->AddSmall(m_gameRulesButton, restrictions);
        } else {
            m_list->AddSmall(difficultyPair, m_gameRulesButton);
            m_list->AddSmall(restrictions, nullptr);
        }

        // ── Multiplayer (host only) ────────────────────────────────────────
        if (m_host) {
            m_list->AddHeader("\xC2\xA7l\xC2\xA7nMultiplayer");
            m_joinableButton = CycleButton::MakeOnOff(0, 0, 150, 20, "Joinable", m_wantedJoinable,
                [this](bool on) {
                    m_wantedJoinable = on;
                    m_joinableButton->SetTooltip(on ? kJoinableOnTip : kJoinableOffTip);
                    UpdateGuestCommandAccessButton();
                    UpdatePortControls();
                    UpdateApplyState();
                });
            m_joinableButton->SetTooltip(m_wantedJoinable ? kJoinableOnTip : kJoinableOffTip);

            m_portEdit = new EditBox(0, 0, 150, 20, "Port Number");
            m_portEdit->SetMaxLength(5);
            m_portEdit->SetTooltip(kPortTip);
            m_portEdit->SetResponder([this](const std::string& value) {
                // MC tryParsePort: blank keeps the current port; otherwise
                // 1024..65535.
                std::string trimmed;
                for (char c : value) if (!std::isspace(static_cast<unsigned char>(c))) trimmed += c;
                if (trimmed.empty()) {
                    m_port = m_initialPort;
                    m_portValid = true;
                } else {
                    bool digits = !trimmed.empty();
                    for (char c : trimmed) if (!std::isdigit(static_cast<unsigned char>(c))) digits = false;
                    const long parsed = digits ? std::atol(trimmed.c_str()) : -1;
                    m_portValid = digits && parsed >= 1024 && parsed <= 65535;
                    if (m_portValid) m_port = static_cast<int>(parsed);
                }
                m_portEdit->SetTooltip(m_portValid ? kPortTip : kPortInvalidTip);
                UpdateApplyState();
            });
            m_list->AddSmall(m_joinableButton, m_portEdit);

            m_guestAccessButton = CycleButton::MakeOnOff(0, 0, 150, 20, "Command Access", m_wantedGuestAccess,
                [this](bool on) {
                    m_wantedGuestAccess = on;
                    UpdateForceGameModeButton();
                    UpdateApplyState();
                });
            m_forceModeButton = CycleButton::MakeOnOff(0, 0, 150, 20, "Force Game Mode", m_wantedForceMode,
                [this](bool on) {
                    m_wantedForceMode = on;
                    m_forceModeButton->SetTooltip(on ? kForceOnTip : kForceOffTip);
                    UpdateApplyState();
                });
            m_list->AddSmall(m_guestAccessButton, m_forceModeButton);
            UpdatePortControls();
            UpdateGuestCommandAccessButton();
        }

        UpdatePermissionDependentButtons();

        // ── Footer: Apply Changes | Cancel ─────────────────────────────────
        const int footerY = m_height - FOOTER_H / 2 - 10;
        const int w = 150, gap = 8;
        const int left = m_width / 2 - (w * 2 + gap) / 2;
        m_applyButton = AddWidget(new Button(left, footerY, w, 20, "Apply Changes", [this] {
            if (m_wantedLocked != m_initialLocked) {
                // MC PopupScreen "Lock World Difficulty" / difficulty.lock.question.
                const std::string name = kDifficultyNames[std::clamp(m_wantedDifficulty, 0, 3)];
                m_manager->Push(std::make_unique<ConfirmScreen>(
                    "Lock World Difficulty",
                    std::vector<std::string>{
                        "Are you sure you want to lock the difficulty of this world?",
                        "This will set this world to always be " + name + ", and you",
                        "will never be able to change that again."},
                    "Yes", "No",
                    [this](bool yes) { if (yes) { ApplyChanges(); OnClose(); } }));
                return;
            }
            ApplyChanges();
            OnClose();
        }));
        m_applyButton->active = HasChanges();
        AddWidget(new Button(left + w + gap, footerY, w, 20, "Cancel", [this] { OnClose(); }));
    }

    // ── State rules (ports of MC's update* methods) ─────────────────────────

    void WorldOptionsScreen::UpdatePermissionDependentButtons() {
        // MC updateButton: active = !hardcore && (wantedAllowCommands, or the
        // player's gamemaster permission when there is no such switch — a
        // remote client here has neither).
        const bool permitted = m_host && m_wantedAllowCommands;
        auto apply = [&](AbstractWidget* w, const std::vector<std::string>& tip,
                         const std::vector<std::string>& disabledTip,
                         const std::vector<std::string>& hardcoreTip) {
            if (!w) return;
            w->active = !m_hardcore && permitted;
            w->SetTooltip(m_hardcore ? hardcoreTip : (permitted ? tip : disabledTip));
        };
        if (!permitted) {
            // MC: with commands off the game-mode choices snap back.
            if (m_defaultModeButton) { m_wantedDefaultMode = m_initialDefaultMode; m_defaultModeButton->SetIndex(m_wantedDefaultMode); }
            if (m_personalModeButton && m_initialPersonalMode >= 0) {
                m_wantedPersonalMode = m_wantedDefaultMode;
                m_personalModeButton->SetIndex(m_wantedPersonalMode);
            }
        } else if (m_personalModeButton && m_wantedPersonalMode >= 0) {
            m_personalModeButton->SetIndex(m_wantedPersonalMode);
        }
        apply(m_gameRulesButton, {}, kGameRulesOperatorTip, kGameRulesHardcoreTip);
        apply(m_defaultModeButton, kDefaultModeTip, kModeOperatorTip, kModeHardcoreTip);
        apply(m_personalModeButton, kPersonalModeTip, kModeOperatorTip, kModeHardcoreTip);

        // DifficultyButtons.updateDifficultyButtonsState: operator (here: the
        // host) → locked → hardcore → free.
        bool difficultyActive = true;
        std::vector<std::string> difficultyTip;
        if (!m_host)                  { difficultyActive = false; difficultyTip = kDifficultyOperatorTip; }
        else if (m_initialLocked && !m_hardcore) { difficultyActive = false; difficultyTip = kDifficultyLockedTip; }
        else if (m_hardcore)          { difficultyActive = false; difficultyTip = kDifficultyHardcoreTip; }
        if (m_difficultyButton) { m_difficultyButton->active = difficultyActive; m_difficultyButton->SetTooltip(difficultyTip); }
        if (m_lockButton)       { m_lockButton->active = difficultyActive; }
    }

    void WorldOptionsScreen::UpdateGuestCommandAccessButton() {
        if (!m_guestAccessButton) return;
        const bool lan = m_wantedJoinable;
        const bool allowCommands = m_wantedAllowCommands;
        if (!lan) {
            m_wantedGuestAccess = false;
            m_guestAccessButton->SetTooltip(kGuestAccessScopeTip);
        } else if (!allowCommands) {
            m_wantedGuestAccess = false;
            m_guestAccessButton->SetTooltip(kGuestAccessCmdTip);
        } else {
            m_wantedGuestAccess = m_initialGuestAccess;
            m_guestAccessButton->SetTooltip(kGuestAccessTip);
        }
        m_guestAccessButton->SetIndex(m_wantedGuestAccess ? 0 : 1);
        m_guestAccessButton->active = lan && allowCommands;
        UpdateForceGameModeButton();
    }

    void WorldOptionsScreen::UpdateForceGameModeButton() {
        if (!m_forceModeButton) return;
        const bool lan = m_wantedJoinable;
        if (m_hardcore) {
            m_wantedForceMode = true;
            m_forceModeButton->SetTooltip(kForceHardcoreTip);
        } else if (!lan) {
            m_wantedForceMode = true;
            m_forceModeButton->SetTooltip(kForceScopeTip);
        } else if (m_wantedGuestAccess) {
            m_wantedForceMode = false;
            m_forceModeButton->SetTooltip(kForceCommandsTip);
        } else {
            m_wantedForceMode = m_initialForceMode;
            m_forceModeButton->SetTooltip(m_wantedForceMode ? kForceOnTip : kForceOffTip);
        }
        m_forceModeButton->SetIndex(m_wantedForceMode ? 0 : 1);
        m_forceModeButton->active = lan && !m_wantedGuestAccess && !m_hardcore;
    }

    void WorldOptionsScreen::UpdatePortControls() {
        if (!m_portEdit) return;
        const bool lan = m_wantedJoinable;
        const std::string desired = lan && m_initialJoinable ? std::to_string(m_initialPort) : std::string();
        if (m_portEdit->GetText() != desired) m_portEdit->SetText(desired);
        m_portEdit->active = lan;
        m_portEdit->SetHint(lan ? std::to_string(m_port) : std::string("Port Number"));
        if (!lan) {
            m_portEdit->SetFocused(false);
            m_portValid = true;
            m_portEdit->SetTooltip(kPortTip);
        }
    }

    bool WorldOptionsScreen::HasChanges() const {
        const bool portChanged = m_wantedJoinable && m_initialJoinable && m_port != m_initialPort;
        const bool settings =
            m_wantedDifficulty != m_initialDifficulty || m_wantedLocked != m_initialLocked ||
            m_wantedDefaultMode != m_initialDefaultMode || m_wantedPersonalMode != m_initialPersonalMode ||
            m_wantedAllowCommands != m_initialAllowCommands || m_wantedJoinable != m_initialJoinable ||
            m_wantedGuestAccess != m_initialGuestAccess || m_wantedForceMode != m_initialForceMode ||
            portChanged;
        return settings && (!PortRequired() || m_portValid);
    }

    void WorldOptionsScreen::UpdateApplyState() {
        if (m_applyButton) m_applyButton->active = HasChanges();
    }

    // ── Apply (WorldOptionsScreen.applyChanges) ─────────────────────────────

    void WorldOptionsScreen::ApplyChanges() {
        if (m_wantedDifficulty != m_initialDifficulty)
            SendCommand(std::string("/difficulty ") + kDifficultyCmd[std::clamp(m_wantedDifficulty, 0, 3)]);
        if (m_wantedLocked && !m_initialLocked)
            SendCommand("/worldoptions difficulty_lock");
        if (!m_host) return;

        if (m_wantedAllowCommands != m_initialAllowCommands)
            SendCommand(std::string("/worldoptions allow_commands ") + (m_wantedAllowCommands ? "on" : "off"));
        if (m_wantedForceMode != m_initialForceMode)
            SendCommand(std::string("/worldoptions force_game_mode ") + (m_wantedForceMode ? "on" : "off"));
        if (m_wantedDefaultMode != m_initialDefaultMode)
            SendCommand(std::string("/defaultgamemode ") + kGameModeCmd[std::clamp(m_wantedDefaultMode, 0, 3)]);
        if (m_wantedPersonalMode >= 0 && m_wantedPersonalMode != m_initialPersonalMode)
            SendCommand(std::string("/gamemode ") + kGameModeCmd[std::clamp(m_wantedPersonalMode, 0, 3)]);
        if (m_wantedGuestAccess != m_initialGuestAccess)
            SendCommand(std::string("/worldoptions guest_command_access ") + (m_wantedGuestAccess ? "on" : "off"));

        // changeMultiplayerScope: scope and/or port.
        const bool portChanged = m_wantedJoinable && m_port != m_initialPort;
        if (!m_wantedJoinable) {
            if (m_initialJoinable) SendCommand("/worldoptions joinable off");
        } else {
            if (portChanged) SendCommand("/worldoptions port " + std::to_string(m_port));
            if (!m_initialJoinable) SendCommand("/worldoptions joinable on");
        }
    }

    void WorldOptionsScreen::Render(GuiGraphics& g, int mouseX, int mouseY, float partialTick) {
        Screen::Render(g, mouseX, mouseY, partialTick);
        g.DrawCenteredString(m_title, m_width / 2, (HEADER_H - FontRenderer::LINE_HEIGHT) / 2, 0xFFFFFFFF);
        RenderMenuSeparators(g, m_width, HEADER_H - 2, m_height - FOOTER_H);
    }

    // ── InWorldGameRulesScreen ──────────────────────────────────────────────

    namespace {
        // One rule: the label on the left, the control (44 px, MC's width) at
        // the right edge of the 310 px row.
        class GameRuleRow : public AbstractWidget {
        public:
            // `controlWidth` is where the control's LEFT edge sits (right
            // aligned at that width); an auto-growing EditBox may extend
            // further right from there.
            GameRuleRow(std::string label, AbstractWidget* control, int controlWidth,
                        std::vector<std::string> tooltip)
                : AbstractWidget(0, 0, OptionsList::ROW_WIDTH, 20, ""), m_label(std::move(label)),
                  m_control(control), m_controlWidth(controlWidth), m_rowTooltip(std::move(tooltip)) {}
            ~GameRuleRow() override { delete m_control; }

            // MC's rule entries carry one tooltip for the whole row: the
            // rule id (yellow), its description, "Default: x" (grey).
            const std::vector<std::string>* TooltipAt(double mx, double my) override {
                Place();
                if (!ContainsPoint(mx, my) && !m_control->ContainsPoint(mx, my)) return nullptr;
                return m_rowTooltip.empty() ? nullptr : &m_rowTooltip;
            }
            void OnClick(double mx, double my) override {
                Place();
                if (m_control->IsMouseOver(mx, my)) { m_control->SetFocused(true); m_control->OnClick(mx, my); }
                else m_control->SetFocused(false);
            }
            bool KeyPressed(int key, int mods) override { return m_control->KeyPressed(key, mods); }
            bool CharTyped(unsigned int cp) override { return m_control->CharTyped(cp); }
            void SetFocused(bool f) override { AbstractWidget::SetFocused(f); m_control->SetFocused(f); }
        protected:
            void RenderWidget(GuiGraphics& g, int mouseX, int mouseY, float partialTick) override {
                Place();
                // Word-wrap the label into the space left of the control
                // (two lines fit a 25 px row).
                const int maxW = m_width - m_controlWidth - 8;
                std::vector<std::string> lines;
                std::string line, word;
                auto flushWord = [&] {
                    if (word.empty()) return;
                    const std::string candidate = line.empty() ? word : line + " " + word;
                    if (g.GetStringWidth(candidate) <= maxW || line.empty()) line = candidate;
                    else { lines.push_back(line); line = word; }
                    word.clear();
                };
                for (char c : m_label) { if (c == ' ') flushWord(); else word += c; }
                flushWord();
                if (!line.empty()) lines.push_back(line);
                if (lines.size() > 2) { lines.resize(2); lines[1] += "..."; }
                const int textY = lines.size() == 1 ? m_y + (m_height - FontRenderer::LINE_HEIGHT) / 2 + 1 : m_y + 1;
                for (size_t i = 0; i < lines.size(); ++i) {
                    g.DrawString(lines[i], m_x, textY + static_cast<int>(i) * (FontRenderer::LINE_HEIGHT + 1), 0xFFFFFFFF);
                }
                m_control->Render(g, mouseX, mouseY, partialTick);
            }
        private:
            void Place() { m_control->SetPosition(m_x + m_width - m_controlWidth, m_y); }
            std::string     m_label;
            AbstractWidget* m_control;
            int             m_controlWidth;
            std::vector<std::string> m_rowTooltip;
        };

        // MC AbstractGameRulesScreen.addEntry's tooltip: the id in yellow,
        // the description split to ~150 px, "Default: x" in grey.
        std::vector<std::string> RuleTooltip(const Server::GameRuleCommand::RuleInfo& r) {
            std::vector<std::string> lines;
            lines.push_back("\xC2\xA7" "e" + r.id);
            std::string line, word;
            auto flush = [&] {
                if (word.empty()) return;
                const std::string candidate = line.empty() ? word : line + " " + word;
                if (candidate.size() <= 30 || line.empty()) line = candidate;
                else { lines.push_back(line); line = word; }
                word.clear();
            };
            for (char c : r.description) { if (c == ' ') flush(); else word += c; }
            flush();
            if (!line.empty()) lines.push_back(line);
            lines.push_back("\xC2\xA7" "7Default: " + r.defaultValue);
            // Vanilla's integer rules have a lower bound only (random_tick_speed
            // is 0..Integer.MAX_VALUE); say so rather than print the int limit.
            if (r.isInt && r.maxValue == std::numeric_limits<int>::max()) {
                lines.push_back("\xC2\xA7" "7Minimum: " + std::to_string(r.minValue));
            } else if (r.isInt) {
                lines.push_back("\xC2\xA7" "7Range: " + std::to_string(r.minValue) + " to " + std::to_string(r.maxValue));
            }
            if (!r.implemented) {
                lines.push_back("\xC2\xA7" "cNot implemented yet");
                lines.push_back("\xC2\xA7" "7This game has no system for");
                lines.push_back("\xC2\xA7" "7this rule; its value is kept");
                lines.push_back("\xC2\xA7" "7but does nothing.");
            }
            return lines;
        }
    }

    void InWorldGameRulesScreen::Init() {
        const bool firstInit = !m_rulesLoaded;
        m_rulesLoaded = true;
        if (firstInit) m_rules.clear();
        m_list = nullptr;
        m_doneButton = nullptr;
        m_list = AddWidget(new OptionsList(0, HEADER_H, m_width, m_height - HEADER_H - FOOTER_H));

        const bool host = HostServerRunning();
        std::vector<Server::GameRuleCommand::RuleInfo> rules = host ? Server::GameRuleCommand::Rules()
                                                                    : std::vector<Server::GameRuleCommand::RuleInfo>{};
        // MC EditGameRulesScreen: rules grouped by category, categories in
        // GameRuleCategory.SORT_ORDER (Player, Mobs, Spawning, Drops, World
        // Updates, Chat, Miscellaneous), rules alphabetical within one —
        // which is the registry's own order.
        std::vector<std::string> categories;
        for (int c = 0; c <= static_cast<int>(Game::Rules::Category::Misc); ++c) {
            const std::string name = Game::Rules::CategoryName(static_cast<Game::Rules::Category>(c));
            for (const auto& r : rules) {
                if (r.category == name) { categories.push_back(name); break; }
            }
        }
        for (const auto& r : rules) {
            if (std::find(categories.begin(), categories.end(), r.category) == categories.end()) categories.push_back(r.category);
        }
        for (const std::string& category : categories) {
            m_list->AddHeader("\xC2\xA7l\xC2\xA7n" + category);
            for (const auto& r : rules) {
                if (r.category != category) continue;
                if (!r.implemented) {
                    // The engine has no system behind this rule: a greyed
                    // "Not implemented yet" where the value control would be
                    // (user request), never a pending change.
                    auto* note = new Button(0, 0, NOT_IMPLEMENTED_W, 20, "Not implemented yet", [] {});
                    note->active = false;
                    m_list->AddBig(new GameRuleRow(r.label, note, NOT_IMPLEMENTED_W, RuleTooltip(r)));
                    continue;
                }
                size_t index = 0;
                if (firstInit) {
                    Pending p;
                    p.id = r.id;
                    p.label = r.label;
                    p.isInt = r.isInt;
                    p.minValue = r.minValue;
                    p.maxValue = r.maxValue;
                    p.initial = Server::GameRuleCommand::ReadValue(r.id);
                    p.wanted = p.initial;
                    index = m_rules.size();
                    m_rules.push_back(p);
                } else {
                    // A rebuild: find the row's pending record; widgets show
                    // the WANTED value so edits survive.
                    bool found = false;
                    for (size_t k = 0; k < m_rules.size(); ++k) if (m_rules[k].id == r.id) { index = k; found = true; break; }
                    if (!found) continue;
                }
                const Pending& p = m_rules[index];

                AbstractWidget* control = nullptr;
                if (r.isInt) {
                    // MC IntegerRuleEntry: 44 px box; a value the rule cannot
                    // deserialize turns the text red (0xFF0000) and marks the
                    // entry invalid, which holds Done until it is fixed.
                    auto* box = new EditBox(0, 0, 44, 20, r.label);
                    box->SetMaxLength(11);
                    box->SetAutoGrow(true);
                    const int lo = r.minValue, hi = r.maxValue;
                    box->SetResponder([this, index, box, lo, hi](const std::string& v) {
                        char* end = nullptr;
                        const long long parsed = v.empty() ? 0 : std::strtoll(v.c_str(), &end, 10);
                        const bool ok = !v.empty() && end && *end == '\0' && parsed >= lo && parsed <= hi;
                        m_rules[index].wanted = v;
                        m_rules[index].valid = ok;
                        box->SetTextColor(ok ? 0xFFE0E0E0u : 0xFFFF0000u);
                        UpdateDoneState();
                    });
                    box->SetText(p.wanted);
                    control = box;
                } else {
                    control = new CycleButton(0, 0, 44, 20, "", {"Yes", "No"}, p.wanted == "true" ? 0 : 1,
                        [this, index](int i) { m_rules[index].wanted = i == 0 ? "true" : "false"; UpdateDoneState(); });
                }
                m_list->AddBig(new GameRuleRow(r.label, control, 44, RuleTooltip(r)));
            }
        }
        if (rules.empty()) m_list->AddHeader("Game rules can only be edited by the host.");

        const int footerY = m_height - FOOTER_H / 2 - 10;
        const int w = 150, gap = 8;
        const int left = m_width / 2 - (w * 2 + gap) / 2;
        m_doneButton = AddWidget(new Button(left, footerY, w, 20, "Done", [this] {
            ApplyPending();
            Screen::OnClose();   // straight back to World Options, no question
        }));
        AddWidget(new Button(left + w + gap, footerY, w, 20, "Cancel", [this] { OnClose(); }));
    }

    std::vector<InWorldGameRulesScreen::Change> InWorldGameRulesScreen::PendingChanges() const {
        std::vector<Change> out;
        for (const Pending& p : m_rules) {
            if (p.wanted == p.initial || !p.valid) continue;
            // Booleans read as the buttons show them.
            auto shown = [&](const std::string& v) {
                if (p.isInt) return v;
                return std::string(v == "true" ? "Yes" : "No");
            };
            out.push_back({p.label, shown(p.initial), shown(p.wanted)});
        }
        return out;
    }

    void InWorldGameRulesScreen::ApplyPending() {
        for (const Pending& p : m_rules) {
            if (p.wanted == p.initial || !p.valid) continue;
            if (Client::g_networkClient) {
                if (auto conn = Client::g_networkClient->GetConnection()) conn->SendChatMessage("/gamerule " + p.id + " " + p.wanted);
            }
        }
    }

    void InWorldGameRulesScreen::OnClose() {
        const std::vector<Change> changes = PendingChanges();
        if (changes.empty()) { Screen::OnClose(); return; }
        std::vector<std::string> lines;
        for (const Change& c : changes) lines.push_back(c.label + ": " + c.from + " -> " + c.to);
        m_manager->Push(std::make_unique<GameRuleChangesScreen>(std::move(lines), [this](bool yes) {
            if (yes) ApplyPending();
            // Either answer lands on the pause menu: this screen and the
            // World Options screen beneath it both go (the question screen
            // has already popped itself).
            m_manager->Pop();
            m_manager->Pop();
        }));
    }

    // ── GameRuleChangesScreen ───────────────────────────────────────────────

    void GameRuleChangesScreen::Init() {
        // PopupScreen: contentWidth 250; LinearLayout.vertical().spacing(12):
        // title (bold) / message / button row; buttons min((250 - 6) / 2, 150)
        // wide with 6 between; the whole block centred in the screen.
        constexpr int kSpacing = 12;
        const int lineStep = FontRenderer::LINE_HEIGHT;
        const int listH = static_cast<int>(m_lines.size()) * lineStep;
        m_contentW = 250;
        m_contentH = lineStep                 // title
                   + kSpacing + lineStep     // question
                   + kSpacing + listH        // the changes, a block of their own
                   + kSpacing + 20;          // buttons
        m_contentX = m_width / 2 - m_contentW / 2;
        m_contentY = m_height / 2 - m_contentH / 2;
        const int buttonW = std::min((m_contentW - 6) / 2, 150);
        const int rowW = buttonW * 2 + 6;
        const int bx = m_width / 2 - rowW / 2;
        const int by = m_contentY + m_contentH - 20;
        AddWidget(new Button(bx, by, buttonW, 20, "Yes", [this] { Finish(true); }));
        AddWidget(new Button(bx + buttonW + 6, by, buttonW, 20, "No", [this] { Finish(false); }));
    }

    void GameRuleChangesScreen::Finish(bool yes) {
        if (m_finished) return;
        m_finished = true;
        if (m_manager) m_manager->Pop();   // first, so the callback pops a clean stack
        if (m_callback) m_callback(yes);
    }

    void GameRuleChangesScreen::OnClose() {
        // Escape: back to Edit Game Rules with the pending changes intact.
        if (m_finished) return;
        m_finished = true;
        if (m_manager) m_manager->Pop();
    }

    void GameRuleChangesScreen::Render(GuiGraphics& g, int mouseX, int mouseY, float partialTick) {
        RenderBackground(g, mouseX, mouseY, partialTick);
        // PopupScreen's panel: popup/background nine-slice, 18 px around the layout.
        g.BlitSprite("popup/background", m_contentX - 18, m_contentY - 18, m_contentW + 36, m_contentH + 36);
        constexpr int kSpacing = 12;
        const int lineStep = FontRenderer::LINE_HEIGHT;
        int y = m_contentY;
        g.DrawCenteredString("\xC2\xA7l" + m_title, m_width / 2, y, 0xFFFFFFFF);
        y += lineStep + kSpacing;
        g.DrawCenteredString("Are you sure you want to apply these changes?", m_width / 2, y, 0xFFFFFFFF);
        y += lineStep + kSpacing;
        for (const auto& line : m_lines) {
            g.DrawCenteredString(line, m_width / 2, y, 0xFFFFFFFF);
            y += lineStep;
        }
        for (auto& w : m_widgets) w->Render(g, mouseX, mouseY, partialTick);
    }

    void InWorldGameRulesScreen::UpdateDoneState() {
        // MC markInvalid/clearInvalid: Done is greyed while any number is bad.
        if (!m_doneButton) return;
        bool allValid = true;
        for (const Pending& p : m_rules) if (!p.valid) { allValid = false; break; }
        m_doneButton->active = allValid;
        m_doneButton->SetTooltip(allValid ? std::vector<std::string>{}
                                          : std::vector<std::string>{"Fix the values in red first."});
    }

    void InWorldGameRulesScreen::Render(GuiGraphics& g, int mouseX, int mouseY, float partialTick) {
        Screen::Render(g, mouseX, mouseY, partialTick);
        g.DrawCenteredString(m_title, m_width / 2, (HEADER_H - FontRenderer::LINE_HEIGHT) / 2, 0xFFFFFFFF);
        RenderMenuSeparators(g, m_width, HEADER_H - 2, m_height - FOOTER_H);
    }

} // namespace Render
