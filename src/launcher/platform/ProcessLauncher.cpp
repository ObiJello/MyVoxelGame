// File: src/launcher/platform/ProcessLauncher.cpp
#include "ProcessLauncher.hpp"
#include "common/core/Log.hpp"
#include <filesystem>

#ifdef _WIN32
#include <Windows.h>
#else
#include <unistd.h>
#include <cstdlib>
#endif

namespace Launcher {

    bool LaunchGame(const std::string& gamePath) {
        if (!std::filesystem::exists(gamePath)) {
            Log::Error("Game not found at: %s", gamePath.c_str());
            return false;
        }

        Log::Info("Launching game: %s", gamePath.c_str());

#ifdef __APPLE__
        // On macOS, use 'open' command for .app bundles
        std::string command = "open \"" + gamePath + "\"";
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

} // namespace Launcher
