// File: src/launcher/ui/LauncherUI.cpp
#include "LauncherUI.hpp"
#include "LauncherTheme.hpp"
#include "launcher/LauncherConfig.hpp"
#include "platform/GameDirectory.hpp"
#include <imgui.h>
#include <sstream>

namespace Launcher {

    void LauncherUI::SetLogoTexture(GLuint textureId, int width, int height) {
        m_logoTexture = textureId;
        m_logoWidth = width;
        m_logoHeight = height;
    }

    void LauncherUI::Render(LauncherUIState& state) {
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoScrollWithMouse;

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(static_cast<float>(WindowWidth), static_cast<float>(WindowHeight)));

        ImGui::Begin("##launcher", nullptr, flags);

        float winW = ImGui::GetWindowWidth();
        float winH = ImGui::GetWindowHeight();

        // ── Launcher version (top right, small grey text) ──
        {
            if (g_fontSmall) ImGui::PushFont(g_fontSmall);
            char versionLabel[32];
            snprintf(versionLabel, sizeof(versionLabel), "v%s", LauncherVersion);
            float versionW = ImGui::CalcTextSize(versionLabel).x;
            ImGui::SetCursorPos(ImVec2(winW - versionW - 15.0f, 10.0f));
            ImGui::TextDisabled("%s", versionLabel);
            if (g_fontSmall) ImGui::PopFont();
        }

        // ── Logo (top, centered) ──
        ImGui::SetCursorPosY(winH * 0.15f);
        DrawLogo();

        // ── Progress Bar (when downloading/installing) ──
        if (state.state == LauncherState::Downloading || state.state == LauncherState::Installing) {
            ImGui::SetCursorPosY(winH * 0.55f);
            DrawProgressBar(state.downloadProgress.load(), state.downloadSizeText);
        }

        // ── Status Text (centered, above button) ──
        {
            ImGui::SetCursorPosY(winH - 130.0f);
            if (g_fontSmall) ImGui::PushFont(g_fontSmall);

            std::string statusStr = state.statusText;
            if (state.launcherUpdateReady) {
                statusStr = "Launcher update ready - restart to apply";
            }

            float textWidth = ImGui::CalcTextSize(statusStr.c_str()).x;
            ImGui::SetCursorPosX((winW - textWidth) * 0.5f);

            if (state.state == LauncherState::Error && !state.launcherUpdateReady) {
                ImGui::TextColored(ImVec4(0.957f, 0.263f, 0.212f, 1.0f), "%s", statusStr.c_str());
            } else {
                ImGui::TextDisabled("%s", statusStr.c_str());
            }

            if (g_fontSmall) ImGui::PopFont();
        }

        // ── Main Button (pinned above status bar) ──
        ImGui::SetCursorPosY(winH - 105.0f);
        if (state.launcherUpdateReady) {
            DrawRestartButton();
        } else {
            DrawPlayButton(state);
        }

        // ── Join Button (below main button) ──
        DrawJoinButton(state);

        // ── Bottom Status Bar ──
        DrawStatusBar(state);

        // ── Settings Popup ──
        if (m_showSettings) {
            DrawSettingsPopup(state);
        }

        // ── Join Server Popup ──
        if (m_showJoinPopup) {
            // Seed the input buffers with the last-used values the FIRST time the popup opens
            // this session, so users don't have to retype the IP they joined last.
            if (!m_joinPopupSeeded) {
                std::snprintf(m_joinIP, sizeof(m_joinIP), "%s", state.lastJoinIP.c_str());
                if (!state.lastJoinPort.empty()) {
                    std::snprintf(m_joinPort, sizeof(m_joinPort), "%s", state.lastJoinPort.c_str());
                }
                m_joinPopupSeeded = true;
            }
            DrawJoinPopup(state);
        } else {
            // Reset so the next open re-seeds from state (which the app may have updated)
            m_joinPopupSeeded = false;
        }

        ImGui::End();
    }

    void LauncherUI::DrawLogo() {
        float windowWidth = ImGui::GetWindowWidth();

        if (m_logoTexture != 0) {
            float maxWidth = 300.0f;
            float scale = maxWidth / static_cast<float>(m_logoWidth);
            if (scale > 1.0f) scale = 1.0f;
            float drawW = m_logoWidth * scale;
            float drawH = m_logoHeight * scale;

            ImGui::SetCursorPosX((windowWidth - drawW) * 0.5f);
            ImGui::Image(static_cast<ImTextureID>(static_cast<uintptr_t>(m_logoTexture)),
                         ImVec2(drawW, drawH));
        } else {
            if (g_fontTitle) ImGui::PushFont(g_fontTitle);
            const char* title = "ObeyCraft";
            float textWidth = ImGui::CalcTextSize(title).x;
            ImGui::SetCursorPosX((windowWidth - textWidth) * 0.5f);
            ImGui::Text("%s", title);
            if (g_fontTitle) ImGui::PopFont();

            if (g_fontSmall) ImGui::PushFont(g_fontSmall);
            const char* subtitle = "A Minecraft-compatible voxel engine";
            float subWidth = ImGui::CalcTextSize(subtitle).x;
            ImGui::SetCursorPosX((windowWidth - subWidth) * 0.5f);
            ImGui::TextDisabled("%s", subtitle);
            if (g_fontSmall) ImGui::PopFont();
        }
    }

    void LauncherUI::DrawProgressBar(float progress, const std::string& sizeText) {
        float windowWidth = ImGui::GetWindowWidth();
        float barWidth = windowWidth - 80.0f;
        float barX = (windowWidth - barWidth) * 0.5f;

        ImGui::SetCursorPosX(barX);

        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.298f, 0.686f, 0.314f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.130f, 0.130f, 0.170f, 1.00f));

        char overlay[64];
        snprintf(overlay, sizeof(overlay), "%.0f%%", progress * 100.0f);
        ImGui::ProgressBar(progress, ImVec2(barWidth, 24), overlay);

        ImGui::PopStyleColor(2);

        if (!sizeText.empty()) {
            if (g_fontSmall) ImGui::PushFont(g_fontSmall);
            float textW = ImGui::CalcTextSize(sizeText.c_str()).x;
            ImGui::SetCursorPosX((windowWidth - textW) * 0.5f);
            ImGui::TextDisabled("%s", sizeText.c_str());
            if (g_fontSmall) ImGui::PopFont();
        }
    }

    void LauncherUI::DrawRestartButton() {
        float windowWidth = ImGui::GetWindowWidth();
        float buttonWidth = 240.0f;
        float buttonHeight = 50.0f;

        ImGui::SetCursorPosX((windowWidth - buttonWidth) * 0.5f);

        if (g_fontButton) ImGui::PushFont(g_fontButton);

        if (ImGui::Button("RESTART", ImVec2(buttonWidth, buttonHeight))) {
            if (m_onRestart) m_onRestart();
        }

        if (g_fontButton) ImGui::PopFont();
    }

    void LauncherUI::DrawPlayButton(LauncherUIState& state) {
        float windowWidth = ImGui::GetWindowWidth();
        float buttonWidth = 240.0f;
        float buttonHeight = 50.0f;

        const char* buttonText = "PLAY";
        bool enabled = true;

        switch (state.state) {
            case LauncherState::Initializing:
                buttonText = "LOADING...";
                enabled = false;
                break;
            case LauncherState::CheckingForUpdates:
                buttonText = "CHECKING...";
                enabled = false;
                break;
            case LauncherState::ReadyToPlay:
                buttonText = "PLAY";
                break;
            case LauncherState::UpdateAvailable:
                buttonText = state.gameInstalled ? "UPDATE" : "INSTALL";
                break;
            case LauncherState::Downloading:
                buttonText = "DOWNLOADING...";
                enabled = false;
                break;
            case LauncherState::Installing:
                buttonText = "INSTALLING...";
                enabled = false;
                break;
            case LauncherState::LaunchingGame:
                buttonText = "LAUNCHING...";
                enabled = false;
                break;
            case LauncherState::Error:
                buttonText = "RETRY";
                break;
        }

        ImGui::SetCursorPosX((windowWidth - buttonWidth) * 0.5f);

        if (g_fontButton) ImGui::PushFont(g_fontButton);

        if (!enabled) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.200f, 0.200f, 0.260f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.200f, 0.200f, 0.260f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.200f, 0.200f, 0.260f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.500f, 0.500f, 0.500f, 1.00f));
        } else if (state.state == LauncherState::Error) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.800f, 0.200f, 0.200f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.900f, 0.300f, 0.300f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.700f, 0.150f, 0.150f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
        }

        if (ImGui::Button(buttonText, ImVec2(buttonWidth, buttonHeight)) && enabled) {
            if (state.state == LauncherState::ReadyToPlay && m_onPlay) {
                m_onPlay();
            } else if (state.state == LauncherState::UpdateAvailable && m_onUpdate) {
                m_onUpdate();
            } else if (state.state == LauncherState::Error && m_onRetry) {
                m_onRetry();
            }
        }

        if (!enabled || state.state == LauncherState::Error) {
            ImGui::PopStyleColor(4);
        }

        if (g_fontButton) ImGui::PopFont();

        // Vulkan checkbox to the right of the button
        ImGui::SameLine(0, 20.0f);
        if (g_fontSmall) ImGui::PushFont(g_fontSmall);
        float checkY = ImGui::GetCursorPosY() + (buttonHeight - ImGui::GetFrameHeight()) * 0.5f;
        ImGui::SetCursorPosY(checkY);
        ImGui::PushStyleColor(ImGuiCol_CheckMark, ImVec4(0.298f, 0.686f, 0.314f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.18f, 0.18f, 0.24f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.25f, 0.25f, 0.33f, 1.0f));
        ImGui::Checkbox("Vulkan", &state.useVulkan);
        ImGui::PopStyleColor(3);
        if (g_fontSmall) ImGui::PopFont();
    }

    void LauncherUI::DrawJoinButton(LauncherUIState& state) {
        if (state.launcherUpdateReady) return;

        float windowWidth = ImGui::GetWindowWidth();
        float buttonWidth = 120.0f;
        float buttonHeight = 25.0f;

        ImGui::SetCursorPosX((windowWidth - buttonWidth) * 0.5f);

        bool enabled = state.gameInstalled &&
                       (state.state == LauncherState::ReadyToPlay || state.state == LauncherState::UpdateAvailable);

        if (g_fontSmall) ImGui::PushFont(g_fontSmall);

        if (!enabled) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.160f, 0.180f, 0.240f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.160f, 0.180f, 0.240f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.160f, 0.180f, 0.240f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.400f, 0.400f, 0.400f, 1.00f));
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.180f, 0.200f, 0.320f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.250f, 0.280f, 0.420f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.150f, 0.170f, 0.280f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.800f, 0.800f, 0.830f, 1.00f));
        }

        // Center the text within the button by computing padding
        ImVec2 textSize = ImGui::CalcTextSize("JOIN");
        float padX = (buttonWidth - textSize.x) * 0.5f;
        float padY = (buttonHeight - textSize.y) * 0.5f;
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(padX, padY));

        if (ImGui::Button("JOIN", ImVec2(buttonWidth, buttonHeight)) && enabled) {
            m_showJoinPopup = true;
        }

        ImGui::PopStyleVar();
        ImGui::PopStyleColor(4);
        if (g_fontSmall) ImGui::PopFont();
    }

    void LauncherUI::DrawJoinPopup(LauncherUIState& state) {
        ImGui::SetNextWindowSize(ImVec2(340, 220), ImGuiCond_Always);
        ImGui::SetNextWindowPos(
            ImVec2(static_cast<float>(WindowWidth) * 0.5f, static_cast<float>(WindowHeight) * 0.5f),
            ImGuiCond_Always,
            ImVec2(0.5f, 0.5f)
        );

        if (ImGui::Begin("Join Server", &m_showJoinPopup,
                          ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse)) {

            if (g_fontBody) ImGui::PushFont(g_fontBody);
            ImGui::Text("Join Server");
            ImGui::Separator();
            if (g_fontBody) ImGui::PopFont();

            ImGui::Spacing();
            if (g_fontSmall) ImGui::PushFont(g_fontSmall);

            ImGui::Text("IP Address");
            ImGui::SetNextItemWidth(200.0f);
            ImGui::InputText("##ip", m_joinIP, sizeof(m_joinIP));

            ImGui::SameLine(0, 10.0f);
            ImGui::Text("Port");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(60.0f);
            ImGui::InputText("##port", m_joinPort, sizeof(m_joinPort));

            ImGui::Spacing();
            ImGui::Spacing();

            // Validate: IP must not be empty, port must be a number 1-65535
            bool valid = false;
            uint16_t port = 0;
            if (m_joinIP[0] != '\0') {
                char* end = nullptr;
                long portVal = strtol(m_joinPort, &end, 10);
                if (end != m_joinPort && *end == '\0' && portVal >= 1 && portVal <= 65535) {
                    port = static_cast<uint16_t>(portVal);
                    valid = true;
                }
            }

            if (!valid) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.200f, 0.200f, 0.260f, 1.00f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.200f, 0.200f, 0.260f, 1.00f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.200f, 0.200f, 0.260f, 1.00f));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.500f, 0.500f, 0.500f, 1.00f));
            }

            if (ImGui::Button("CONNECT", ImVec2(120, 30)) && valid) {
                m_showJoinPopup = false;
                // Persist the IP/port so the next open of this dialog (and the next launch
                // of the launcher) pre-fills with what the user just typed.
                state.lastJoinIP = m_joinIP;
                state.lastJoinPort = m_joinPort;
                if (m_onJoin) {
                    m_onJoin(std::string(m_joinIP), port);
                }
            }

            if (!valid) {
                ImGui::PopStyleColor(4);
            }

            ImGui::SameLine(0, 10.0f);
            if (ImGui::Button("Cancel", ImVec2(80, 30))) {
                m_showJoinPopup = false;
            }

            if (g_fontSmall) ImGui::PopFont();
        }
        ImGui::End();
    }

    void LauncherUI::DrawStatusBar(const LauncherUIState& state) {
        float winW = ImGui::GetWindowWidth();
        float winH = ImGui::GetWindowHeight();

        // Consistent margin used for all edges
        const float margin = 20.0f;

        if (g_fontSmall) ImGui::PushFont(g_fontSmall);

        // Calculate button size first so we can align everything
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(16, 6));
        ImVec2 btnSize = ImGui::CalcTextSize("Settings");
        btnSize.x += 32.0f; // padding * 2
        btnSize.y += 12.0f; // padding * 2

        // Bottom Y: anchor both elements from the bottom using the same margin
        float btnY = winH - margin - btnSize.y;
        float textY = btnY + (btnSize.y - ImGui::GetTextLineHeight()) * 0.5f;

        // Installed version (left, vertically centered with button)
        ImGui::SetCursorPos(ImVec2(margin, textY));
        if (state.gameInstalled) {
            ImGui::TextDisabled("Installed: v%s", state.installedVersion.c_str());
        } else {
            ImGui::TextDisabled("Not installed");
        }

        // Settings button (right, same margin from edge)
        ImGui::SetCursorPos(ImVec2(winW - margin - btnSize.x, btnY));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.16f, 0.18f, 0.28f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.25f, 0.38f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.18f, 0.20f, 0.32f, 1.0f));
        if (ImGui::Button("Settings", btnSize)) {
            m_showSettings = !m_showSettings;
        }
        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar();

        if (g_fontSmall) ImGui::PopFont();
    }

    void LauncherUI::DrawSettingsPopup(LauncherUIState& state) {
        ImGui::SetNextWindowSize(ImVec2(400, 320), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(
            ImVec2(static_cast<float>(WindowWidth) * 0.5f, static_cast<float>(WindowHeight) * 0.5f),
            ImGuiCond_FirstUseEver,
            ImVec2(0.5f, 0.5f)
        );

        if (ImGui::Begin("Settings", &m_showSettings, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse)) {
            if (g_fontBody) ImGui::PushFont(g_fontBody);

            ImGui::Text("Launcher Settings");
            ImGui::Separator();
            ImGui::Spacing();

            if (g_fontBody) ImGui::PopFont();
            if (g_fontSmall) ImGui::PushFont(g_fontSmall);

            // ── Username (passed to the game as --name <X>; empty → server auto-assigns) ──
            // Sync the char buffer from state on first paint and whenever state was loaded.
            // We keep them in sync by writing back into state on every edit.
            if (m_playerName[0] == '\0' && !state.playerName.empty()) {
                std::snprintf(m_playerName, sizeof(m_playerName), "%s", state.playerName.c_str());
            }
            ImGui::Text("Username");
            ImGui::SetNextItemWidth(220.0f);
            if (ImGui::InputText("##username", m_playerName, sizeof(m_playerName))) {
                state.playerName = m_playerName;
            }
            ImGui::TextDisabled("Leave blank to use the default (Player1, Player2, ...)");

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::TextDisabled("Launcher v%s", LauncherVersion);
            ImGui::Spacing();
            ImGui::TextDisabled("Game directory:");
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.18f, 0.24f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.25f, 0.33f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.30f, 0.30f, 0.40f, 1.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6, 5));

            std::string gameDir = Platform::g_gameDirectory.GetGameDirectory();
            if (ImGui::SmallButton(gameDir.c_str())) {
                ImGui::SetClipboardText(gameDir.c_str());
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Click to copy");
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            if (ImGui::Button("Close", ImVec2(100, 0))) {
                m_showSettings = false;
            }

            ImGui::PopStyleVar();
            ImGui::PopStyleColor(3);

            if (g_fontSmall) ImGui::PopFont();
        }
        ImGui::End();
    }

} // namespace Launcher
