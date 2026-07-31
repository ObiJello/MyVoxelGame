// File: src/launcher/LauncherApp.cpp
#include "LauncherApp.hpp"
#include "LauncherConfig.hpp"
#include "ui/LauncherUI.hpp"
#include "ui/LauncherTheme.hpp"
#include "updater/VersionInfo.hpp"
#include "updater/GitHubAPI.hpp"
#include "updater/Downloader.hpp"
#include "updater/Installer.hpp"
#include "platform/ProcessLauncher.hpp"
#include "platform/GameDirectory.hpp"
#include "net/FriendsServiceClient.hpp"
#include "common/core/FriendsServiceConfig.hpp"
#include "common/core/Log.hpp"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <nlohmann/json.hpp>
#include <stb_image.h>

#include <thread>
#include <mutex>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#endif

namespace Launcher {

    // ── Launcher Config (stored in launcher.json) ──
    struct LauncherPersistentConfig {
        std::string installedVersion;
        std::string launcherVersion;
        bool autoUpdate = true;
        bool useVulkan = false;
        std::string playerName;             // Empty → server auto-assigns "PlayerN"
        std::string playerColor;            // "" or "default" → neon green; otherwise palette slug ("pink", "blue"...)
        std::string lastJoinIP;             // Pre-fill the Join Server dialog
        std::string lastJoinPort = "25565"; // Pre-fill the Join Server dialog

        // ── ObeyCraft account (friends service) ──
        // Empty sessionToken → guest (no account, friends features off).
        std::string sessionToken;
        int64_t     accountId = 0;
        std::string accountName;
        // Friends-service override "host" or "host:port"; empty → defaults
        // from FriendsServiceConfig.hpp. The HOSTING machine should set
        // "127.0.0.1" (routers rarely hairpin their own public IP).
        std::string friendsService;

        void Load(const std::string& path) {
            try {
                std::ifstream file(path);
                if (!file.is_open()) return;
                auto json = nlohmann::json::parse(file);
                installedVersion = json.value("installed_version", "");
                launcherVersion = json.value("launcher_version", "");
                autoUpdate = json.value("auto_update", true);
                useVulkan = json.value("use_vulkan", false);
                playerName = json.value("player_name", "");
                playerColor = json.value("player_color", "");
                lastJoinIP = json.value("last_join_ip", "");
                lastJoinPort = json.value("last_join_port", std::string("25565"));
                sessionToken = json.value("session_token", "");
                accountId = json.value("account_id", static_cast<int64_t>(0));
                accountName = json.value("account_name", "");
                friendsService = json.value("friends_service", "");
            } catch (...) {
                Log::Warning("Failed to load launcher config");
            }
        }

        void Save(const std::string& path) {
            try {
                nlohmann::json json;
                json["installed_version"] = installedVersion;
                json["launcher_version"] = launcherVersion;
                json["auto_update"] = autoUpdate;
                json["use_vulkan"] = useVulkan;
                json["player_name"] = playerName;
                json["player_color"] = playerColor;
                json["last_join_ip"] = lastJoinIP;
                json["last_join_port"] = lastJoinPort;
                json["session_token"] = sessionToken;
                json["account_id"] = accountId;
                json["account_name"] = accountName;
                json["friends_service"] = friendsService;
                std::ofstream file(path);
                file << json.dump(2);
            } catch (...) {
                Log::Warning("Failed to save launcher config");
            }
        }
    };

    // Parse a "host" / "host:port" friends-service override, falling back to
    // the shared defaults.
    static void ResolveFriendsService(const std::string& override_,
                                      std::string& outHost, uint16_t& outPort) {
        outHost = Friends::kDefaultServiceHost;
        outPort = Friends::kDefaultServicePort;
        if (override_.empty()) return;
        auto colon = override_.rfind(':');
        if (colon != std::string::npos && colon + 1 < override_.size()) {
            outHost = override_.substr(0, colon);
            int p = std::atoi(override_.c_str() + colon + 1);
            if (p > 0 && p <= 65535) outPort = static_cast<uint16_t>(p);
        } else {
            outHost = override_;
        }
    }

    // ── Asset path helper ──
    static std::string GetAssetPath(const std::string& relativePath) {
#ifdef __APPLE__
        CFBundleRef mainBundle = CFBundleGetMainBundle();
        if (mainBundle) {
            CFURLRef resourcesURL = CFBundleCopyResourcesDirectoryURL(mainBundle);
            if (resourcesURL) {
                char path[4096];
                if (CFURLGetFileSystemRepresentation(resourcesURL, TRUE, (UInt8*)path, sizeof(path))) {
                    CFRelease(resourcesURL);
                    std::string fullPath = std::string(path) + "/" + relativePath;
                    if (std::filesystem::exists(fullPath)) {
                        return fullPath;
                    }
                } else {
                    CFRelease(resourcesURL);
                }
            }
        }
#endif
        return relativePath;
    }

    // ── GLFW error callback ──
    static void GlfwErrorCallback(int error, const char* description) {
        Log::Error("GLFW Error %d: %s", error, description);
    }

    // ── Main entry point ──
    int LauncherApp::Run(int /*argc*/, char* /*argv*/[]) {
        Log::Init();
        Log::Info("ObeyCraft Launcher v%s starting", LauncherVersion);

        // Initialize game directory system (creates obeycraft/ dir)
        if (!Platform::InitializeGameDirectorySystem()) {
            Log::Error("Failed to initialize game directory");
            return -1;
        }

        std::string gameDir = Platform::g_gameDirectory.GetGameDirectory();
        std::string configPath = gameDir + "/" + LauncherConfigFile;
        std::string installDir = gameDir;
        std::string gamePath = installDir + "/" + GameSubdir;

        // Load persistent config
        LauncherPersistentConfig config;
        config.Load(configPath);

        // ── GLFW + OpenGL Init ──
        glfwSetErrorCallback(GlfwErrorCallback);
        if (!glfwInit()) {
            Log::Error("Failed to initialize GLFW");
            return -1;
        }

        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
        glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
        glfwWindowHint(GLFW_COCOA_RETINA_FRAMEBUFFER, GLFW_TRUE);
#endif
        glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);

        GLFWwindow* window = glfwCreateWindow(WindowWidth, WindowHeight, WindowTitle, nullptr, nullptr);
        if (!window) {
            Log::Error("Failed to create window");
            glfwTerminate();
            return -1;
        }

        // Set window icon (taskbar + title bar)
        {
            std::string iconPath = GetAssetPath("launcher/logo.png");
            int iconW, iconH, iconChannels;
            unsigned char* iconData = stbi_load(iconPath.c_str(), &iconW, &iconH, &iconChannels, 4);
            if (iconData) {
                GLFWimage glfwIcon{ iconW, iconH, iconData };
                glfwSetWindowIcon(window, 1, &glfwIcon);
                stbi_image_free(iconData);
            }
        }

        glfwMakeContextCurrent(window);
        glfwSwapInterval(1); // VSync

        if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
            Log::Error("Failed to initialize GLAD");
            glfwDestroyWindow(window);
            glfwTerminate();
            return -1;
        }

        // ── ImGui Init ──
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr; // Don't save imgui.ini for the launcher

        ImGui_ImplGlfw_InitForOpenGL(window, true);
        ImGui_ImplOpenGL3_Init("#version 330 core");

        ApplyLauncherTheme();

        // Load fonts
        std::string fontDir = GetAssetPath("fonts");
        if (!std::filesystem::exists(fontDir)) {
            // Fallback: try relative to executable
            fontDir = "ext/imgui/misc/fonts";
        }
        LoadLauncherFonts(window, fontDir);

        // Load logo texture (if available)
        GLuint logoTexture = 0;
        int logoW = 0, logoH = 0;
        {
            std::string logoPath = GetAssetPath("launcher/logo.png");
            if (std::filesystem::exists(logoPath)) {
                int channels;
                unsigned char* data = stbi_load(logoPath.c_str(), &logoW, &logoH, &channels, 4);
                if (data) {
                    glGenTextures(1, &logoTexture);
                    glBindTexture(GL_TEXTURE_2D, logoTexture);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, logoW, logoH, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
                    stbi_image_free(data);
                    Log::Info("Loaded logo texture: %dx%d", logoW, logoH);
                }
            }
        }

        // ── UI State ──
        LauncherUIState uiState;
        LauncherUI ui;

        // Check if game is already installed
        std::string gameExePath;
#ifdef __APPLE__
        gameExePath = gamePath + "/" + GameBinaryName;
#else
        gameExePath = gamePath + "/" + GameBinaryName;
#endif

        uiState.gameInstalled = std::filesystem::exists(gameExePath);
        uiState.useVulkan = config.useVulkan;
        uiState.playerName = config.playerName;
        uiState.playerColor = config.playerColor;
        uiState.lastJoinIP = config.lastJoinIP;
        uiState.lastJoinPort = config.lastJoinPort;
        uiState.sessionToken = config.sessionToken;
        uiState.accountId = config.accountId;
        uiState.accountName = config.accountName;
        // Logged in → the account name IS the username (server-canonical).
        if (!config.sessionToken.empty() && !config.accountName.empty()) {
            uiState.playerName = config.accountName;
        }
        if (!config.installedVersion.empty()) {
            uiState.installedVersion = config.installedVersion;
        }

        if (logoTexture != 0) {
            ui.SetLogoTexture(logoTexture, logoW, logoH);
        }

        // ── Background worker state ──
        std::atomic<bool> workerRunning{false};
        std::mutex resultMutex;
        ReleaseInfo latestRelease;
        std::atomic<bool> checkComplete{false};
        std::atomic<bool> checkSuccess{false};
        std::string checkError;

        // Strip the platform tag prefix (e.g. "game-win-v") or old "v" prefix before parsing
        auto ParseGameVersion = [](const std::string& tagName) -> Version {
            std::string s = tagName;
            std::string prefix(GameReleaseTagPrefix);
            if (s.find(prefix) == 0) {
                s = s.substr(prefix.length());
            } else if (!s.empty() && (s[0] == 'v' || s[0] == 'V')) {
                s = s.substr(1);
            }
            return Version::Parse(s);
        };

        std::atomic<bool> downloadComplete{false};
        std::atomic<bool> downloadSuccess{false};

        // ── Friends-service auth worker state ──
        // One auth op (login/signup/logout/rename) in flight at a time;
        // results published under authMutex, drained at the top of the frame
        // loop (same shape as the update-check worker above).
        struct AuthOutcome {
            bool        success = false;
            std::string error;          // service error code or "network"
            std::string token, name;    // login/signup/rename results
            int64_t     accountId = 0;
            bool        clearedSession = false;   // logout
            bool        renamed = false;
        };
        std::atomic<bool> authBusy{false};
        std::atomic<bool> authComplete{false};
        std::mutex authMutex;
        AuthOutcome authOutcome;

        // Username availability checks: keyed by a generation counter so
        // stale responses (older keystrokes) are discarded on arrival.
        std::atomic<uint64_t> nameCheckGeneration{0};
        std::atomic<bool> nameCheckComplete{false};
        std::mutex nameCheckMutex;
        struct { uint64_t generation = 0; std::string status; } nameCheckResult;

        std::string friendsHost;
        uint16_t friendsPort = 0;
        ResolveFriendsService(config.friendsService, friendsHost, friendsPort);

        std::atomic<bool> installComplete{false};
        std::atomic<bool> installSuccess{false};

        // Launcher self-update state
        std::atomic<bool> launcherUpdateReady{false};
        std::string currentLauncherPath = GetCurrentLauncherPath();
        std::string updaterScriptPath; // Windows only

        // ── Start version check (launcher update + game update in one thread) ──
        uiState.state = LauncherState::CheckingForUpdates;
        uiState.statusText = "Checking for updates...";
        Log::Info("[SELFUPDATE] My compiled version: %s", LauncherVersion);
        Log::Info("[SELFUPDATE] My path: %s", currentLauncherPath.c_str());

        std::thread checkThread([&]() {
            workerRunning = true;
            GitHubAPI api(GitHubOwner, GitHubRepo);

            // Phase 1: Check for launcher self-update
            {
                ReleaseInfo launcherRelease;
                if (api.FetchLatestLauncherRelease(launcherRelease)) {
                    std::string tagPrefix(LauncherReleaseTagPrefix);
                    std::string oldTagPrefix = "launcher-v";
                    std::string versionStr = launcherRelease.tagName;
                    if (versionStr.find(tagPrefix) == 0) {
                        versionStr = versionStr.substr(tagPrefix.length());
                    } else if (versionStr.find(oldTagPrefix) == 0) {
                        versionStr = versionStr.substr(oldTagPrefix.length());
                    }
                    Version latestLauncher = Version::Parse(versionStr);
                    Version currentLauncher = Version::Parse(LauncherVersion);

                    Log::Info("[SELFUPDATE] GitHub latest: %s (tag: %s)", latestLauncher.ToString().c_str(), launcherRelease.tagName.c_str());
                    Log::Info("[SELFUPDATE] Compiled: %s", currentLauncher.ToString().c_str());
                    Log::Info("[SELFUPDATE] Is newer: %d", (latestLauncher > currentLauncher) ? 1 : 0);

                    // Also check config version — if we already installed this version
                    // in a previous session, don't re-download (avoids loop if compiled
                    // version is stale due to incremental build)
                    Version configLauncher = Version::Parse(config.launcherVersion);

                    if (latestLauncher > currentLauncher && latestLauncher > configLauncher && launcherRelease.hasPlatformAsset) {
                        Log::Info("[SELFUPDATE] UPDATE NEEDED: %s -> %s",
                                  currentLauncher.ToString().c_str(), latestLauncher.ToString().c_str());

                        Downloader launcherDl;
                        std::string dlPath = installDir + "/_launcher_dl.zip";
                        if (launcherDl.Download(launcherRelease.platformAsset.downloadUrl, dlPath)) {
                            Installer launcherInstaller;
                            std::string stagingDir = installDir + "/_launcher_update";
                            if (launcherInstaller.InstallLauncher(dlPath, currentLauncherPath, stagingDir)) {
                                updaterScriptPath = launcherInstaller.GetUpdaterScriptPath();
                                config.launcherVersion = latestLauncher.ToString();
                                config.Save(configPath);
                                launcherUpdateReady = true;
                                Log::Info("Launcher update ready - restart to apply");
                            }
                        }
                    } else {
                        Log::Info("[SELFUPDATE] NO UPDATE NEEDED - compiled=%s github=%s",
                                  currentLauncher.ToString().c_str(), latestLauncher.ToString().c_str());
                    }
                }
            }

            // Phase 2: Check for game update
            ReleaseInfo info;
            bool success = api.FetchLatestRelease(info);

            std::lock_guard<std::mutex> lock(resultMutex);
            latestRelease = info;
            checkSuccess = success;
            if (!success) {
                checkError = !info.tagName.empty()
                    ? "No game download available for this platform yet"
                    : "Could not connect to update server";
            }
            checkComplete = true;
            workerRunning = false;
        });
        checkThread.detach();

        // ── UI Callbacks ──
        Downloader downloader;
        Installer installer;

        ui.SetOnRestartClicked([&]() {
            Log::Info("User requested launcher restart for self-update");
            config.Save(configPath);
            RelaunchSelf(currentLauncherPath, updaterScriptPath);
            // RelaunchSelf calls _exit(0), so we never get here
        });

        // Build "--name <X>" arg fragment if a non-empty username is set. Game's PlatformMain
        // accepts an empty/missing name and lets the server auto-assign "PlayerN".
        auto buildNameArg = [&]() -> std::string {
            if (uiState.playerName.empty()) return "";
            return " --name " + uiState.playerName;
        };
        // Build "--color <slug>" arg fragment if a non-default colour is set.
        // Empty string means the user picked "Default" → game falls back to neon green.
        auto buildColorArg = [&]() -> std::string {
            if (uiState.playerColor.empty() || uiState.playerColor == "default") return "";
            return " --color " + uiState.playerColor;
        };
        // Build the friends-service identity args when logged in. The token
        // only grants friends-service access (never sent to game servers).
        // Guests get no args → the game runs exactly as before accounts.
        auto buildSessionArgs = [&]() -> std::string {
            if (uiState.sessionToken.empty()) return "";
            std::string s = " --session " + uiState.sessionToken
                          + " --account-id " + std::to_string(uiState.accountId);
            if (!config.friendsService.empty()) {
                s += " --friends-service " + config.friendsService;
            }
            return s;
        };

        ui.SetOnPlayClicked([&]() {
            uiState.state = LauncherState::LaunchingGame;
            uiState.statusText = "Launching game...";
            // Persist any settings changes (username, colour) before launching, so a
            // crash before clean exit still keeps what the user typed/picked.
            config.playerName = uiState.playerName;
            config.playerColor = uiState.playerColor;
            config.Save(configPath);
            std::string args = buildNameArg() + buildColorArg() + buildSessionArgs();
            if (LaunchGame(gameExePath, uiState.useVulkan, args)) {
                // Close launcher after a brief delay
                glfwSetWindowShouldClose(window, GLFW_TRUE);
            } else {
                uiState.state = LauncherState::Error;
                uiState.statusText = "Failed to launch game";
                uiState.errorText = "Could not start the game executable";
            }
        });

        ui.SetOnJoinClicked([&](const std::string& host, uint16_t port) {
            uiState.state = LauncherState::LaunchingGame;
            uiState.statusText = "Joining server...";
            // Persist username, colour + IP/port so re-opening the launcher pre-fills all
            config.playerName = uiState.playerName;
            config.playerColor = uiState.playerColor;
            config.lastJoinIP = uiState.lastJoinIP;
            config.lastJoinPort = uiState.lastJoinPort;
            config.Save(configPath);
            std::string serverArg = "--server " + host + ":" + std::to_string(port)
                                  + buildNameArg() + buildColorArg() + buildSessionArgs();
            if (LaunchGame(gameExePath, uiState.useVulkan, serverArg)) {
                glfwSetWindowShouldClose(window, GLFW_TRUE);
            } else {
                uiState.state = LauncherState::Error;
                uiState.statusText = "Failed to launch game";
                uiState.errorText = "Could not start the game executable";
            }
        });

        // ── Friends-service account callbacks ──
        // Each spawns a detached worker (FriendsServiceClient blocks);
        // results land in authOutcome and are drained each frame below.
        auto runAuthOp = [&](std::function<AuthOutcome(FriendsServiceClient&)> op) {
            if (authBusy.exchange(true)) return;   // one at a time
            uiState.authBusy = true;
            std::thread t([&, op = std::move(op)]() {
                FriendsServiceClient client(friendsHost, friendsPort);
                AuthOutcome outcome = op(client);
                {
                    std::lock_guard<std::mutex> lock(authMutex);
                    authOutcome = std::move(outcome);
                }
                authComplete = true;
            });
            t.detach();
        };

        ui.SetOnLogin([&](const std::string& name, const std::string& password) {
            runAuthOp([name, password](FriendsServiceClient& client) {
                auto r = client.Login(name, password);
                AuthOutcome o;
                o.success = r.ok;
                o.error = r.error;
                if (r.ok) {
                    o.token = r.body.value("token", "");
                    o.name = r.body.value("name", "");
                    o.accountId = r.body.value("account_id", static_cast<int64_t>(0));
                }
                return o;
            });
        });

        ui.SetOnSignup([&](const std::string& name, const std::string& password) {
            runAuthOp([name, password](FriendsServiceClient& client) {
                auto r = client.Signup(name, password);
                AuthOutcome o;
                o.success = r.ok;
                o.error = r.error;
                if (r.ok) {
                    o.token = r.body.value("token", "");
                    o.name = r.body.value("name", "");
                    o.accountId = r.body.value("account_id", static_cast<int64_t>(0));
                }
                return o;
            });
        });

        ui.SetOnLogout([&]() {
            const std::string token = uiState.sessionToken;
            runAuthOp([token](FriendsServiceClient& client) {
                client.Logout(token);   // best-effort; local session clears regardless
                AuthOutcome o;
                o.success = true;
                o.clearedSession = true;
                return o;
            });
        });

        ui.SetOnRename([&](const std::string& newName) {
            const std::string token = uiState.sessionToken;
            runAuthOp([token, newName](FriendsServiceClient& client) {
                auto r = client.Rename(token, newName);
                AuthOutcome o;
                o.success = r.ok;
                o.error = r.error;
                o.renamed = r.ok;
                if (r.ok) o.name = r.body.value("name", newName);
                return o;
            });
        });

        ui.SetOnCheckName([&](const std::string& name) {
            const uint64_t generation = ++nameCheckGeneration;
            const std::string token = uiState.sessionToken;
            std::thread t([&, generation, name, token]() {
                FriendsServiceClient client(friendsHost, friendsPort);
                auto r = client.CheckName(name, token);
                const std::string status =
                    r.ok ? r.body.value("status", "invalid") : "network";
                {
                    std::lock_guard<std::mutex> lock(nameCheckMutex);
                    // Stale keystrokes lose: only the newest generation wins.
                    if (generation >= nameCheckResult.generation) {
                        nameCheckResult = {generation, status};
                        nameCheckComplete = true;
                    }
                }
            });
            t.detach();
        });

        ui.SetOnUpdateClicked([&]() {
            uiState.state = LauncherState::Downloading;
            uiState.statusText = "Downloading update...";
            uiState.downloadProgress = 0.0f;

            std::thread dlThread([&]() {
                workerRunning = true;
                std::string downloadUrl;
                std::string assetName;
                {
                    std::lock_guard<std::mutex> lock(resultMutex);
                    downloadUrl = latestRelease.platformAsset.downloadUrl;
                    assetName = latestRelease.platformAsset.name;
                }

                std::string downloadPath = installDir + "/" + assetName;

                bool success = downloader.Download(downloadUrl, downloadPath,
                    [&](size_t downloaded, size_t total) {
                        if (total > 0) {
                            uiState.downloadProgress = static_cast<float>(downloaded) / static_cast<float>(total);
                            double dlMB = static_cast<double>(downloaded) / (1024.0 * 1024.0);
                            double totalMB = static_cast<double>(total) / (1024.0 * 1024.0);
                            char buf[64];
                            snprintf(buf, sizeof(buf), "%.1f / %.1f MB", dlMB, totalMB);
                            uiState.downloadSizeText = buf;
                        }
                    });

                if (success) {
                    // Install
                    uiState.state = LauncherState::Installing;
                    uiState.statusText = "Installing...";

                    success = installer.Install(downloadPath, installDir,
                        [&](const std::string& status) {
                            uiState.statusText = status;
                        });
                }

                downloadSuccess = success;
                installSuccess = success;
                installComplete = true;
                workerRunning = false;
            });
            dlThread.detach();
        });

        ui.SetOnRetryClicked([&]() {
            uiState.state = LauncherState::CheckingForUpdates;
            uiState.statusText = "Checking for updates...";
            checkComplete = false;
            checkSuccess = false;

            std::thread retryThread([&]() {
                workerRunning = true;
                GitHubAPI api(GitHubOwner, GitHubRepo);
                ReleaseInfo info;
                bool success = api.FetchLatestRelease(info);

                std::lock_guard<std::mutex> lock(resultMutex);
                latestRelease = info;
                checkSuccess = success;
                if (!success) {
                    checkError = !info.tagName.empty()
                        ? "No game download available for this platform yet"
                        : "Could not connect to update server";
                }
                checkComplete = true;
                workerRunning = false;
            });
            retryThread.detach();
        });

        // ── Main Loop ──
        while (!glfwWindowShouldClose(window)) {
            glfwPollEvents();

            // ── Drain friends-service auth results ──
            if (authComplete.load()) {
                authComplete = false;
                authBusy = false;
                uiState.authBusy = false;
                AuthOutcome outcome;
                {
                    std::lock_guard<std::mutex> lock(authMutex);
                    outcome = authOutcome;
                }
                if (outcome.clearedSession) {
                    // Logout: drop the local session no matter what the
                    // service said (it may simply be unreachable).
                    config.sessionToken.clear();
                    config.accountId = 0;
                    config.accountName.clear();
                    uiState.sessionToken.clear();
                    uiState.accountId = 0;
                    uiState.accountName.clear();
                    uiState.authStatusText = "Logged out.";
                    config.Save(configPath);
                } else if (outcome.success) {
                    if (!outcome.token.empty()) {   // login / signup
                        config.sessionToken = outcome.token;
                        config.accountId = outcome.accountId;
                        uiState.sessionToken = outcome.token;
                        uiState.accountId = outcome.accountId;
                    }
                    if (!outcome.name.empty()) {    // login / signup / rename
                        config.accountName = outcome.name;
                        uiState.accountName = outcome.name;
                        // The account name is the canonical username.
                        config.playerName = outcome.name;
                        uiState.playerName = outcome.name;
                    }
                    uiState.authStatusText = outcome.renamed
                        ? "Renamed to " + outcome.name
                        : "Logged in as " + uiState.accountName;
                    uiState.nameCheckState = LauncherUIState::NameCheck::Idle;
                    config.Save(configPath);
                } else {
                    // Human-readable error mapping for the settings popup.
                    const std::string& e = outcome.error;
                    uiState.authStatusText =
                        e == "bad_credentials"    ? "Wrong username or password." :
                        e == "name_taken"         ? "That username is taken." :
                        e == "name_invalid"       ? "Names are 3-16 letters, numbers, _." :
                        e == "password_too_short" ? "Password must be at least 4 characters." :
                        e == "network"            ? "Can't reach the friends server." :
                                                    "Error: " + e;
                }
            }

            // ── Drain username availability results ──
            if (nameCheckComplete.load()) {
                nameCheckComplete = false;
                std::string status;
                uint64_t generation;
                {
                    std::lock_guard<std::mutex> lock(nameCheckMutex);
                    status = nameCheckResult.status;
                    generation = nameCheckResult.generation;
                }
                // Only the newest in-flight check may update the UI.
                if (generation == nameCheckGeneration.load()) {
                    using NC = LauncherUIState::NameCheck;
                    uiState.nameCheckState =
                        status == "available" ? NC::Available :
                        status == "taken"     ? NC::Taken :
                        status == "yours"     ? NC::Yours :
                        status == "invalid"   ? NC::Invalid :
                                                NC::Idle;   // network → no claim
                }
            }

            // Process background results
            if (checkComplete.load() && uiState.state == LauncherState::CheckingForUpdates) {
                if (checkSuccess.load()) {
                    std::lock_guard<std::mutex> lock(resultMutex);
                    Version latest = ParseGameVersion(latestRelease.tagName);
                    Version installed = Version::Parse(config.installedVersion);

                    uiState.latestVersion = latest.ToString();
                    uiState.changelog = latestRelease.body;

                    if (!config.installedVersion.empty() && !(latest > installed) && uiState.gameInstalled) {
                        uiState.state = LauncherState::ReadyToPlay;
                        uiState.statusText = "Ready to play - v" + latest.ToString();
                    } else {
                        uiState.state = LauncherState::UpdateAvailable;
                        if (uiState.gameInstalled) {
                            uiState.statusText = "Update available: v" + latest.ToString();
                        } else {
                            uiState.statusText = "v" + latest.ToString() + " available for download";
                        }
                    }
                } else {
                    if (uiState.gameInstalled) {
                        // Offline mode - can still play
                        uiState.state = LauncherState::ReadyToPlay;
                        uiState.statusText = "Offline mode - v" + config.installedVersion;
                    } else {
                        uiState.state = LauncherState::Error;
                        uiState.statusText = checkError;
                    }
                }
            }

            if (installComplete.load() &&
                (uiState.state == LauncherState::Installing || uiState.state == LauncherState::Downloading)) {
                installComplete = false;
                if (installSuccess.load()) {
                    // Update config
                    {
                        std::lock_guard<std::mutex> lock(resultMutex);
                        Version latest = ParseGameVersion(latestRelease.tagName);
                        config.installedVersion = latest.ToString();
                    }
                    config.Save(configPath);

                    uiState.installedVersion = config.installedVersion;
                    uiState.gameInstalled = true;
                    uiState.state = LauncherState::ReadyToPlay;
                    uiState.statusText = "Ready to play - v" + config.installedVersion;

                    // Re-check game exe path
                    gameExePath = gamePath + "/" + GameBinaryName;
                } else {
                    uiState.state = LauncherState::Error;
                    uiState.statusText = "Installation failed";
                }
            }

            // Render
            int fbWidth, fbHeight;
            glfwGetFramebufferSize(window, &fbWidth, &fbHeight);
            glViewport(0, 0, fbWidth, fbHeight);
            glClearColor(0.071f, 0.071f, 0.094f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);

            // Update launcher update flag for UI
            uiState.launcherUpdateReady = launcherUpdateReady.load();

            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();

            ui.Render(uiState);

            ImGui::Render();
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

            glfwSwapBuffers(window);
        }

        // ── Cleanup ──
        config.useVulkan = uiState.useVulkan;
        config.playerName = uiState.playerName;
        config.playerColor = uiState.playerColor;
        config.lastJoinIP = uiState.lastJoinIP;
        config.lastJoinPort = uiState.lastJoinPort;
        config.sessionToken = uiState.sessionToken;
        config.accountId = uiState.accountId;
        config.accountName = uiState.accountName;
        config.Save(configPath);

        if (logoTexture != 0) {
            glDeleteTextures(1, &logoTexture);
        }

        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();

        glfwDestroyWindow(window);
        glfwTerminate();

        Platform::ShutdownGameDirectorySystem();

        Log::Info("Launcher shut down");
        return 0;
    }

} // namespace Launcher
