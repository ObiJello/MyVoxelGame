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
#include <filesystem>
#include <fstream>

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#endif

namespace Launcher {

    // ── Launcher Config (stored in launcher.json) ──
    struct LauncherPersistentConfig {
        std::string installedVersion;
        bool autoUpdate = true;

        void Load(const std::string& path) {
            try {
                std::ifstream file(path);
                if (!file.is_open()) return;
                auto json = nlohmann::json::parse(file);
                installedVersion = json.value("installed_version", "");
                autoUpdate = json.value("auto_update", true);
            } catch (...) {
                Log::Warning("Failed to load launcher config");
            }
        }

        void Save(const std::string& path) {
            try {
                nlohmann::json json;
                json["installed_version"] = installedVersion;
                json["auto_update"] = autoUpdate;
                std::ofstream file(path);
                file << json.dump(2);
            } catch (...) {
                Log::Warning("Failed to save launcher config");
            }
        }
    };

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

        std::atomic<bool> downloadComplete{false};
        std::atomic<bool> downloadSuccess{false};

        std::atomic<bool> installComplete{false};
        std::atomic<bool> installSuccess{false};

        // ── Start version check ──
        uiState.state = LauncherState::CheckingForUpdates;
        uiState.statusText = "Checking for updates...";

        std::thread checkThread([&]() {
            workerRunning = true;
            GitHubAPI api(GitHubOwner, GitHubRepo);
            ReleaseInfo info;
            bool success = api.FetchLatestRelease(info);

            std::lock_guard<std::mutex> lock(resultMutex);
            latestRelease = info;
            checkSuccess = success;
            if (!success) {
                checkError = "Could not connect to update server";
            }
            checkComplete = true;
            workerRunning = false;
        });
        checkThread.detach();

        // ── UI Callbacks ──
        Downloader downloader;
        Installer installer;

        ui.SetOnPlayClicked([&]() {
            uiState.state = LauncherState::LaunchingGame;
            uiState.statusText = "Launching game...";
            if (LaunchGame(gameExePath)) {
                // Close launcher after a brief delay
                glfwSetWindowShouldClose(window, GLFW_TRUE);
            } else {
                uiState.state = LauncherState::Error;
                uiState.statusText = "Failed to launch game";
                uiState.errorText = "Could not start the game executable";
            }
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
                    checkError = "Could not connect to update server";
                }
                checkComplete = true;
                workerRunning = false;
            });
            retryThread.detach();
        });

        // ── Main Loop ──
        while (!glfwWindowShouldClose(window)) {
            glfwPollEvents();

            // Process background results
            if (checkComplete.load() && uiState.state == LauncherState::CheckingForUpdates) {
                if (checkSuccess.load()) {
                    std::lock_guard<std::mutex> lock(resultMutex);
                    Version latest = Version::Parse(latestRelease.tagName);
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
                        Version latest = Version::Parse(latestRelease.tagName);
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

            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();

            ui.Render(uiState);

            ImGui::Render();
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

            glfwSwapBuffers(window);
        }

        // ── Cleanup ──
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
