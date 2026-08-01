// File: src/launcher/ui/LauncherUI.hpp
#pragma once

#include <string>
#include <atomic>
#include <cstdint>
#include <functional>
#include <vector>

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

    // Sentinels for SavedServer::pingMs
    inline constexpr int PingPending = -2;  // probe in flight / not yet probed
    inline constexpr int PingOffline = -1;  // probe failed

    // One saved-server entry (persisted in launcher.json; pingMs is transient).
    struct SavedServer {
        std::string name;
        std::string host;
        uint16_t port = 25565;
        int pingMs = PingPending;
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

        // ── Release metadata (set by the update-check drain) ──
        std::string publishedAt;        // ISO 8601 timestamp of the latest game release
        std::string gameAssetMeta;      // "macos-arm64 · 112 MB"
        std::string launcherNewVersion; // version staged by a completed self-update
        std::string launcherChangelog;  // release notes of the staged launcher update
        std::string launcherAssetMeta;  // "SELF-UPDATED · 14 MB"

        // Persisted across launcher runs (loaded from / saved to launcher.json by LauncherApp)
        std::string playerName;             // Empty → server auto-assigns "PlayerN"
        std::string playerColor;            // Empty / "default" → game's neon green; otherwise palette slug
        std::string lastJoinIP;             // Pre-fills the quick-connect host field
        std::string lastJoinPort = "25565"; // Pre-fills the quick-connect port field

        // ── Saved servers ──
        // UI mutates `servers` directly and raises `serversDirty`; the app
        // persists the list and refreshes pings on the next frame.
        std::vector<SavedServer> servers;
        bool serversDirty = false;

        // ── ObeyCraft account (friends service) ──
        // sessionToken empty → guest (no account features).
        std::string sessionToken;
        int64_t     accountId = 0;
        std::string accountName;
        int64_t     accountCreated = 0; // epoch seconds ("member since"); 0 = unknown

        // Transient auth/checkmark UI state (owned by LauncherApp's drains).
        enum class NameCheck { Idle, Checking, Available, Taken, Yours, Invalid };
        NameCheck   nameCheckState = NameCheck::Idle;
        std::string authStatusText;   // last login/signup/rename outcome line
        bool        authBusy = false; // an auth op is in flight
        bool        pwChangeDone = false; // set by app on password change; consumed by UI
    };

    class LauncherUI {
    public:
        using ActionCallback = std::function<void()>;
        using JoinCallback = std::function<void(const std::string& host, uint16_t port)>;
        using CredentialsCallback = std::function<void(const std::string& name,
                                                       const std::string& password)>;
        using NameCallback = std::function<void(const std::string& name)>;
        using PasswordChangeCallback = std::function<void(const std::string& current,
                                                          const std::string& newPassword)>;

        void SetOnPlayClicked(ActionCallback cb) { m_onPlay = cb; }
        void SetOnUpdateClicked(ActionCallback cb) { m_onUpdate = cb; }
        void SetOnRetryClicked(ActionCallback cb) { m_onRetry = cb; }
        void SetOnRestartClicked(ActionCallback cb) { m_onRestart = cb; }
        void SetOnJoinClicked(JoinCallback cb) { m_onJoin = cb; }
        void SetOnPingServers(ActionCallback cb) { m_onPingServers = cb; }

        // Friends-service account hooks (all dispatched to worker threads by
        // LauncherApp — the UI just fires them).
        void SetOnLogin(CredentialsCallback cb) { m_onLogin = cb; }
        void SetOnSignup(CredentialsCallback cb) { m_onSignup = cb; }
        void SetOnLogout(ActionCallback cb) { m_onLogout = cb; }
        void SetOnRename(NameCallback cb) { m_onRename = cb; }
        void SetOnCheckName(NameCallback cb) { m_onCheckName = cb; }
        void SetOnChangePassword(PasswordChangeCallback cb) { m_onChangePassword = cb; }

        void SetLogoTexture(GLuint textureId, int width, int height);

        // Render the full launcher UI. Call once per frame between ImGui::NewFrame and ImGui::Render.
        void Render(LauncherUIState& state);

    private:
        enum class View { Play, Servers, Settings };
        enum class SettingsTab { General, Character };
        enum class AccountPane { Out, SignIn, SignUp, In, ChangePw };

        // ── Views ──
        void DrawRail(LauncherUIState& state);
        void DrawPlayView(LauncherUIState& state);
        void DrawServersView(LauncherUIState& state);
        void DrawSettingsView(LauncherUIState& state);
        void DrawSettingsGeneral(LauncherUIState& state);
        void DrawSettingsCharacter(LauncherUIState& state);

        // ── Settings/General pieces ──
        void DrawAccountSection(LauncherUIState& state);
        void DrawSignInPane(LauncherUIState& state);
        void DrawSignUpPane(LauncherUIState& state);
        void DrawSignedInPane(LauncherUIState& state);
        void DrawChangePwPane(LauncherUIState& state);
        void DrawGameRows(LauncherUIState& state);
        void DrawUsernameRow(LauncherUIState& state);

        // Sync account-pane navigation with auth state changes coming from the app.
        void SyncAccountPane(LauncherUIState& state);
        void ClearPasswordBuffers();

        ActionCallback m_onPlay;
        ActionCallback m_onUpdate;
        ActionCallback m_onRetry;
        ActionCallback m_onRestart;
        JoinCallback m_onJoin;
        ActionCallback m_onPingServers;
        CredentialsCallback m_onLogin;
        CredentialsCallback m_onSignup;
        ActionCallback m_onLogout;
        NameCallback m_onRename;
        NameCallback m_onCheckName;
        PasswordChangeCallback m_onChangePassword;

        GLuint m_logoTexture = 0;
        int m_logoWidth = 0;
        int m_logoHeight = 0;

        View m_view = View::Play;
        SettingsTab m_tab = SettingsTab::General;
        AccountPane m_acctPane = AccountPane::Out;
        bool m_acctPaneInit = false;   // seed m_acctPane from login state once

        // ── Servers view ──
        bool m_quickSeeded = false;    // quick-connect fields seeded from state
        bool m_addingServer = false;   // inline add-server form open
        bool m_pingedOnOpen = false;   // one ping refresh per Servers-view visit
        char m_quickHost[64] = "";
        char m_quickPort[8] = "25565";
        char m_addName[48] = "";
        char m_addHost[64] = "";
        char m_addPort[8] = "25565";

        // ── Account forms ──
        char m_authName[32] = "";      // sign-in / sign-up username
        char m_password[64] = "";      // sign-in / sign-up password (never persisted)
        char m_pwCurrent[64] = "";     // change-password: current
        char m_pwNew[64] = "";         // change-password: new

        // ── Username row (two-way sync with state.playerName) ──
        char m_playerName[32] = "";
        std::string m_lastSyncedName;
        double m_nameEditTime = 0.0;   // >0 when an edit is pending an availability check

        // ── Release-notes cache (reparsed only when the source string changes) ──
        std::string m_notesSource;
        std::vector<std::string> m_notesBullets;
    };

} // namespace Launcher
