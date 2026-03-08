// File: src/launcher/ui/LauncherUI.hpp
#pragma once

#include <string>
#include <atomic>
#include <functional>

typedef unsigned int GLuint;

namespace Launcher {

    enum class LauncherState {
        Initializing,
        CheckingForUpdates,
        ReadyToPlay,
        UpdateAvailable,
        Downloading,
        Installing,
        LaunchingGame,
        Error
    };

    // Shared state between the app logic and UI
    struct LauncherUIState {
        LauncherState state = LauncherState::Initializing;
        std::string statusText = "Initializing...";
        std::string errorText;
        std::string installedVersion = "Not installed";
        std::string latestVersion;
        std::string changelog;
        std::atomic<float> downloadProgress{0.0f};
        std::string downloadSizeText;
        bool gameInstalled = false;
        bool launcherUpdateReady = false;  // true when a launcher update has been installed
        bool useVulkan = false;            // launch game with --vulkan
    };

    class LauncherUI {
    public:
        using ActionCallback = std::function<void()>;

        void SetOnPlayClicked(ActionCallback cb) { m_onPlay = cb; }
        void SetOnUpdateClicked(ActionCallback cb) { m_onUpdate = cb; }
        void SetOnRetryClicked(ActionCallback cb) { m_onRetry = cb; }
        void SetOnRestartClicked(ActionCallback cb) { m_onRestart = cb; }

        void SetLogoTexture(GLuint textureId, int width, int height);

        // Render the full launcher UI. Call once per frame between ImGui::NewFrame and ImGui::Render.
        void Render(LauncherUIState& state);

    private:
        void DrawLogo();
        void DrawProgressBar(float progress, const std::string& sizeText);
        void DrawRestartButton();
        void DrawPlayButton(LauncherUIState& state);
        void DrawStatusBar(const LauncherUIState& state);
        void DrawSettingsPopup();

        ActionCallback m_onPlay;
        ActionCallback m_onUpdate;
        ActionCallback m_onRetry;
        ActionCallback m_onRestart;

        GLuint m_logoTexture = 0;
        int m_logoWidth = 0;
        int m_logoHeight = 0;

        bool m_showSettings = false;
    };

} // namespace Launcher
