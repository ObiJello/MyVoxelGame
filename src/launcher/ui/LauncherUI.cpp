// File: src/launcher/ui/LauncherUI.cpp
#include "LauncherUI.hpp"
#include "LauncherTheme.hpp"
#include "launcher/LauncherConfig.hpp"
#include <imgui.h>
#include <cmath>
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

        float windowWidth = ImGui::GetWindowWidth();
        float contentWidth = windowWidth - 40.0f; // margins

        // ── Logo / Title Area ──
        ImGui::Spacing();
        DrawLogo();
        ImGui::Spacing();
        ImGui::Spacing();

        // ── Changelog Card ──
        if (!state.changelog.empty()) {
            DrawChangelog(state.changelog);
            ImGui::Spacing();
        } else {
            // Spacer when no changelog
            ImGui::Dummy(ImVec2(0, 60));
        }

        // ── Progress Bar (when downloading/installing) ──
        if (state.state == LauncherState::Downloading || state.state == LauncherState::Installing) {
            DrawProgressBar(state.downloadProgress.load(), state.downloadSizeText);
            ImGui::Spacing();
        }

        // ── Status Text ──
        {
            if (g_fontSmall) ImGui::PushFont(g_fontSmall);

            std::string statusStr = state.statusText;
            float textWidth = ImGui::CalcTextSize(statusStr.c_str()).x;
            ImGui::SetCursorPosX((windowWidth - textWidth) * 0.5f);

            // Color based on state
            if (state.state == LauncherState::Error) {
                ImGui::TextColored(ImVec4(0.957f, 0.263f, 0.212f, 1.0f), "%s", statusStr.c_str());
            } else {
                ImGui::TextDisabled("%s", statusStr.c_str());
            }

            if (g_fontSmall) ImGui::PopFont();
        }

        ImGui::Spacing();

        // ── Play / Update / Retry Button ──
        DrawPlayButton(state);

        // ── Bottom Status Bar ──
        DrawStatusBar(state);

        // ── Settings Popup ──
        if (m_showSettings) {
            DrawSettingsPopup();
        }

        ImGui::End();
    }

    void LauncherUI::DrawLogo() {
        float windowWidth = ImGui::GetWindowWidth();

        if (m_logoTexture != 0) {
            // Draw logo texture centered
            float maxWidth = 300.0f;
            float scale = maxWidth / static_cast<float>(m_logoWidth);
            if (scale > 1.0f) scale = 1.0f;
            float drawW = m_logoWidth * scale;
            float drawH = m_logoHeight * scale;

            ImGui::SetCursorPosX((windowWidth - drawW) * 0.5f);
            ImGui::Image(static_cast<ImTextureID>(static_cast<uintptr_t>(m_logoTexture)),
                         ImVec2(drawW, drawH));
        } else {
            // Text fallback
            if (g_fontTitle) ImGui::PushFont(g_fontTitle);

            const char* title = "ObeyCraft";
            float textWidth = ImGui::CalcTextSize(title).x;
            ImGui::SetCursorPosX((windowWidth - textWidth) * 0.5f);
            ImGui::Text("%s", title);

            if (g_fontTitle) ImGui::PopFont();

            // Subtitle
            if (g_fontSmall) ImGui::PushFont(g_fontSmall);
            const char* subtitle = "A Minecraft-compatible voxel engine";
            float subWidth = ImGui::CalcTextSize(subtitle).x;
            ImGui::SetCursorPosX((windowWidth - subWidth) * 0.5f);
            ImGui::TextDisabled("%s", subtitle);
            if (g_fontSmall) ImGui::PopFont();
        }
    }

    void LauncherUI::DrawChangelog(const std::string& changelog) {
        float windowWidth = ImGui::GetWindowWidth();
        float cardWidth = windowWidth - 60.0f;
        float cardX = (windowWidth - cardWidth) * 0.5f;

        ImGui::SetCursorPosX(cardX);

        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.110f, 0.110f, 0.149f, 1.00f));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f);

        ImGui::BeginChild("##changelog", ImVec2(cardWidth, 120), true);

        if (g_fontSmall) ImGui::PushFont(g_fontSmall);

        // Render changelog text (basic markdown-ish rendering)
        std::string line;
        std::istringstream stream(changelog);
        while (std::getline(stream, line)) {
            if (line.empty()) {
                ImGui::Spacing();
                continue;
            }
            // Bold headings starting with ##
            if (line.size() >= 2 && line[0] == '#' && line[1] == '#') {
                if (g_fontSmall) ImGui::PopFont();
                if (g_fontBody) ImGui::PushFont(g_fontBody);
                std::string heading = line.substr(line.find_first_not_of("# "));
                ImGui::TextColored(ImVec4(0.298f, 0.686f, 0.314f, 1.0f), "%s", heading.c_str());
                if (g_fontBody) ImGui::PopFont();
                if (g_fontSmall) ImGui::PushFont(g_fontSmall);
            } else if (line[0] == '-' || line[0] == '*') {
                ImGui::TextWrapped("  %s", line.c_str());
            } else {
                ImGui::TextWrapped("%s", line.c_str());
            }
        }

        if (g_fontSmall) ImGui::PopFont();

        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
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
    }

    void LauncherUI::DrawStatusBar(const LauncherUIState& state) {
        float windowWidth = ImGui::GetWindowWidth();
        float windowHeight = static_cast<float>(WindowHeight);

        // Position at bottom
        ImGui::SetCursorPosY(windowHeight - 40.0f);

        if (g_fontSmall) ImGui::PushFont(g_fontSmall);

        // Installed version (left side)
        ImGui::SetCursorPosX(20.0f);
        if (state.gameInstalled) {
            ImGui::TextDisabled("Installed: v%s", state.installedVersion.c_str());
        } else {
            ImGui::TextDisabled("Not installed");
        }

        // Settings gear (right side)
        ImGui::SameLine(windowWidth - 100.0f);
        if (ImGui::SmallButton("Settings")) {
            m_showSettings = !m_showSettings;
        }

        if (g_fontSmall) ImGui::PopFont();
    }

    void LauncherUI::DrawSettingsPopup() {
        ImGui::SetNextWindowSize(ImVec2(350, 200), ImGuiCond_FirstUseEver);
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

            ImGui::TextDisabled("Launcher v%s", LauncherVersion);
            ImGui::TextDisabled("Game directory:");
            ImGui::TextWrapped("  (Set automatically based on platform)");

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            if (ImGui::Button("Close", ImVec2(100, 0))) {
                m_showSettings = false;
            }

            if (g_fontSmall) ImGui::PopFont();
        }
        ImGui::End();
    }

} // namespace Launcher
