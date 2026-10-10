// File: src/launcher/ui/LauncherUI.hpp
#pragma once

#include "launcher/GameRenderer.hpp"
#include "launcher/appearance/AppearanceSettings.hpp"
#include "launcher/appearance/AppearanceView.hpp"
#include "launcher/ui/AppIconPicker.hpp"

#include <string>
#include <atomic>
#include <filesystem>
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
        GameRenderer renderer = GameRenderer::OpenGL;   // the backend the game starts with (GameRenderer.hpp)
        // Settings → Launch arguments: the user's own command-line additions,
        // used only while the toggle is on (GameRenderer.hpp LaunchArgs*).
        bool        customArgsEnabled = false;
        std::string customArgs;
        // Settings → App icon: a full block's id, "random", or the default
        // "tnt" (AppIconPicker.hpp); reaches the game as --icon.
        std::string appIcon = Render::BlockIcon::kDefaultBlock;

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

        // ── Appearance (the Appearance view) ──
        // The persisted look: stick figure (playerColor + optional paint) or
        // a Minecraft skin and cape. The UI edits it in place and raises
        // `appearanceDirty`; the app saves launcher.json and clears it.
        Appearance::Settings appearance;
        bool appearanceDirty = false;

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

        // Where the Appearance view keeps and finds its files (the obeycraft
        // directory, the bundled default skins).
        void SetAppearancePaths(const Appearance::Paths& paths) { m_appearanceView.SetPaths(paths); }

        // The installed game's assets, where the App icon picker reads blocks.
        void SetGameAssetsDir(const std::filesystem::path& dir) { m_appIconPicker.SetAssetsDir(dir); }

        // Frees the UI's GL objects (the 3D previews, skin and cape
        // textures, the block icon thumbnails). Call while the GL context is
        // still current, before the ImGui backend and the window are torn
        // down.
        void Shutdown() {
            m_appearanceView.ReleaseGpu();
            m_appIconPicker.ReleaseGpu();
        }

        // Render the full launcher UI. Call once per frame between ImGui::NewFrame and ImGui::Render.
        void Render(LauncherUIState& state);

    private:
        enum class View { Play, Servers, Appearance, Settings };
        enum class AccountPane { Out, SignIn, SignUp, In, ChangePw };

        // ── Views ──
        void DrawRail(LauncherUIState& state);
        void DrawPlayView(LauncherUIState& state);
        void DrawServersView(LauncherUIState& state);
        void DrawSettingsView(LauncherUIState& state);
        void DrawSettingsGeneral(LauncherUIState& state);

        // ── Settings/General pieces ──
        void DrawAccountSection(LauncherUIState& state);
        void DrawSignInPane(LauncherUIState& state);
        void DrawSignUpPane(LauncherUIState& state);
        void DrawSignedInPane(LauncherUIState& state);
        void DrawChangePwPane(LauncherUIState& state);
        void DrawGameRows(LauncherUIState& state);
        // The renderer select (Play view controls row and Settings row).
        void DrawRendererPicker(LauncherUIState& state, const char* id, ImVec2 pos, ImVec2 size);
        void DrawUsernameRow(LauncherUIState& state);
        void DrawLaunchArgsRow(LauncherUIState& state);

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

        // The Appearance view (stick figure, skin, capes) and its skin editor.
        Appearance::AppearanceView m_appearanceView;
        // Settings → App icon.
        AppIconPicker m_appIconPicker;

        GLuint m_logoTexture = 0;
        int m_logoWidth = 0;
        int m_logoHeight = 0;

        View m_view = View::Play;
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
        char m_customArgs[512] = "";       // the Launch arguments field's edit buffer
        std::string m_lastSyncedCustomArgs;
        double m_nameEditTime = 0.0;   // >0 when an edit is pending an availability check

        // ── Release-notes cache (reparsed only when the source string changes) ──
        std::string m_notesSource;
        std::vector<std::string> m_notesBullets;
    };

} // namespace Launcher
