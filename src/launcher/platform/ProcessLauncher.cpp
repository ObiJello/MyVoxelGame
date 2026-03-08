// File: src/launcher/platform/ProcessLauncher.cpp
#include "ProcessLauncher.hpp"
#include "common/core/Log.hpp"
#include <filesystem>
#include <thread>
#include <chrono>

#ifdef _WIN32
#include <Windows.h>
#else
#include <unistd.h>
#include <cstdlib>
#endif

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace Launcher {

    bool LaunchGame(const std::string& gamePath, bool useVulkan) {
        if (!std::filesystem::exists(gamePath)) {
            Log::Error("Game not found at: %s", gamePath.c_str());
            return false;
        }

        Log::Info("Launching game: %s (vulkan=%d)", gamePath.c_str(), useVulkan);

#ifdef __APPLE__
        // On macOS, use 'open' command for .app bundles
        std::string command = "open \"" + gamePath + "\"";
        if (useVulkan) {
            command += " --args --vulkan";
        }
        int result = system(command.c_str());
        if (result != 0) {
            Log::Error("Failed to launch game (exit code %d)", result);
            return false;
        }
        return true;

#elif defined(_WIN32)
        // On Windows, use ShellExecute
        HINSTANCE result = ShellExecuteA(nullptr, "open", gamePath.c_str(),
                                          nullptr, nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<intptr_t>(result) <= 32) {
            Log::Error("Failed to launch game (ShellExecute error %lld)",
                       static_cast<long long>(reinterpret_cast<intptr_t>(result)));
            return false;
        }
        return true;

#else
        // On Linux, fork and exec
        pid_t pid = fork();
        if (pid == 0) {
            execl(gamePath.c_str(), gamePath.c_str(), nullptr);
            _exit(1); // exec failed
        } else if (pid > 0) {
            return true;
        } else {
            Log::Error("Failed to fork process");
            return false;
        }
#endif
    }

    void RelaunchSelf(const std::string& launcherPath, const std::string& updaterScript) {
        Log::Info("Relaunching launcher...");

#ifdef _WIN32
        if (!updaterScript.empty()) {
            // Launch the updater batch script which will swap and relaunch
            ShellExecuteA(nullptr, "open", updaterScript.c_str(),
                          nullptr, nullptr, SW_HIDE);
        } else {
            ShellExecuteA(nullptr, "open", launcherPath.c_str(),
                          nullptr, nullptr, SW_SHOWNORMAL);
        }
#elif defined(__APPLE__)
        // On macOS, resolve to the .app bundle and use 'open'
        std::string appPath = launcherPath;
        // If path points inside the bundle, walk up to .app
        auto pos = appPath.find(".app/");
        if (pos != std::string::npos) {
            appPath = appPath.substr(0, pos + 4);
        }
        // Launch a detached shell that waits for us to die, then opens the app
        // Using system() with & to fully detach from this process
        std::string cmd = "(sleep 1 && open \"" + appPath + "\") &";
        system(cmd.c_str());
        Log::Info("Spawned relaunch shell for: %s", appPath.c_str());
#else
        pid_t pid = fork();
        if (pid == 0) {
            execl(launcherPath.c_str(), launcherPath.c_str(), nullptr);
            _exit(1);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
#endif
        // Exit the current process
        _exit(0);
    }

    std::string GetCurrentLauncherPath() {
#ifdef __APPLE__
        // Get the executable path and resolve up to the .app bundle
        char path[4096];
        uint32_t size = sizeof(path);
        if (_NSGetExecutablePath(path, &size) == 0) {
            std::string exePath(path);
            // Walk up from Contents/MacOS/binary to the .app
            auto pos = exePath.find(".app/");
            if (pos != std::string::npos) {
                return exePath.substr(0, pos + 4);
            }
            return exePath;
        }
        return "";
#elif defined(_WIN32)
        char path[MAX_PATH];
        GetModuleFileNameA(nullptr, path, MAX_PATH);
        return std::string(path);
#else
        return std::filesystem::read_symlink("/proc/self/exe").string();
#endif
    }

} // namespace Launcher
