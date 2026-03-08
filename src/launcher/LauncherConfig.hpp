// File: src/launcher/LauncherConfig.hpp
#pragma once

#include <string>

namespace Launcher {

    // Window
    inline constexpr int WindowWidth = 800;
    inline constexpr int WindowHeight = 500;
    inline constexpr const char* WindowTitle = "ObeyCraft Launcher";

    // GitHub
    inline constexpr const char* GitHubOwner = "ObiJello";
    inline constexpr const char* GitHubRepo = "MyVoxelGame-Download";
    inline constexpr const char* GitHubAPIBase = "https://api.github.com";
    inline constexpr const char* UserAgent = "ObeyCraftLauncher/1.0";

    // Launcher version
    inline constexpr const char* LauncherVersion = "1.0.0";

    // File names
    inline constexpr const char* LauncherConfigFile = "launcher.json";
    inline constexpr const char* GameSubdir = "game";

#ifdef __APPLE__
    inline constexpr const char* GameBinaryName = "MyVoxelGame.app";
#elif _WIN32
    inline constexpr const char* GameBinaryName = "MyVoxelGame.exe";
#else
    inline constexpr const char* GameBinaryName = "MyVoxelGame";
#endif

    // Platform detection for asset matching
    inline std::string GetPlatformAssetTag() {
#ifdef __APPLE__
    #ifdef __arm64__
        return "macos-arm64";
    #elif defined(__x86_64__)
        return "macos-x86_64";
    #else
        return "macos";
    #endif
#elif defined(_WIN32)
    #ifdef _M_ARM64
        return "windows-arm64";
    #elif defined(_M_X64) || defined(__x86_64__)
        return "windows-x64";
    #else
        return "windows-x86";
    #endif
#else
        return "linux-x86_64";
#endif
    }

    inline std::string GetPlatformFallbackTag() {
#ifdef __APPLE__
        return "macos";
#elif defined(_WIN32)
        return "windows";
#else
        return "linux";
#endif
    }

} // namespace Launcher
