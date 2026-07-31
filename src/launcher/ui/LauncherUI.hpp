// File: src/launcher/ui/LauncherUI.hpp
#pragma once

#include <string>
#include <atomic>
#include <cstdint>
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

        // Persisted across launcher runs (loaded from / saved to launcher.json by LauncherApp)
        std::string playerName;             // Empty → server auto-assigns "PlayerN"
        std::string playerColor;            // Empty / "default" → game's neon green; otherwise palette slug
        std::string lastJoinIP;             // Pre-fills the Join Server dialog IP field
        std::string lastJoinPort = "25565"; // Pre-fills the Join Server dialog port field

        // ── ObeyCraft account (friends service) ──
        // sessionToken empty → guest (no account features).
        std::string sessionToken;
        int64_t     accountId = 0;
        std::string accountName;

        // Transient auth/checkmark UI state (owned by LauncherApp's drains).
        enum class NameCheck { Idle, Checking, Available, Taken, Yours, Invalid };
        NameCheck   nameCheckState = NameCheck::Idle;
        std::string authStatusText;   // last login/signup/rename outcome line
        bool        authBusy = false; // an auth op is in flight
    };

    class LauncherUI {
    public:
        using ActionCallback = std::function<void()>;
        using JoinCallback = std::function<void(const std::string& host, uint16_t port)>;
        using CredentialsCallback = std::function<void(const std::string& name,
                                                       const std::string& password)>;
        using NameCallback = std::function<void(const std::string& name)>;

        void SetOnPlayClicked(ActionCallback cb) { m_onPlay = cb; }
        void SetOnUpdateClicked(ActionCallback cb) { m_onUpdate = cb; }
        void SetOnRetryClicked(ActionCallback cb) { m_onRetry = cb; }
        void SetOnRestartClicked(ActionCallback cb) { m_onRestart = cb; }
        void SetOnJoinClicked(JoinCallback cb) { m_onJoin = cb; }

        // Friends-service account hooks (all dispatched to worker threads by
        // LauncherApp — the UI just fires them).
        void SetOnLogin(CredentialsCallback cb) { m_onLogin = cb; }
        void SetOnSignup(CredentialsCallback cb) { m_onSignup = cb; }
        void SetOnLogout(ActionCallback cb) { m_onLogout = cb; }
        void SetOnRename(NameCallback cb) { m_onRename = cb; }
        void SetOnCheckName(NameCallback cb) { m_onCheckName = cb; }

        void SetLogoTexture(GLuint textureId, int width, int height);

        // Render the full launcher UI. Call once per frame between ImGui::NewFrame and ImGui::Render.
        void Render(LauncherUIState& state);

    private:
        void DrawLogo();
        void DrawProgressBar(float progress, const std::string& sizeText);
        void DrawRestartButton();
        void DrawPlayButton(LauncherUIState& state);
        void DrawJoinButton(LauncherUIState& state);
        void DrawJoinPopup(LauncherUIState& state);
        void DrawStatusBar(const LauncherUIState& state);
        void DrawSettingsPopup(LauncherUIState& state);

        ActionCallback m_onPlay;
        ActionCallback m_onUpdate;
        ActionCallback m_onRetry;
        ActionCallback m_onRestart;
        JoinCallback m_onJoin;
        CredentialsCallback m_onLogin;
        CredentialsCallback m_onSignup;
        ActionCallback m_onLogout;
        NameCallback m_onRename;
        NameCallback m_onCheckName;

        GLuint m_logoTexture = 0;
        int m_logoWidth = 0;
        int m_logoHeight = 0;

        bool m_showSettings = false;
        bool m_showJoinPopup = false;
        bool m_joinPopupSeeded = false; // True after pre-filling m_joinIP/m_joinPort from state
        char m_joinIP[64] = "";
        char m_joinPort[8] = "25565";
        char m_playerName[32] = "";     // Bound to LauncherUIState::playerName via Render()
        char m_password[64] = "";       // Account password entry (never persisted)

        // Two-way username sync: what the buffer last agreed with state on.
        // When state.playerName changes elsewhere (login/rename), the buffer
        // re-seeds — unless the user is mid-edit.
        std::string m_lastSyncedName;
        // Availability-check debounce: >0 when an edit is pending a check.
        double m_nameEditTime = 0.0;
    };

} // namespace Launcher
