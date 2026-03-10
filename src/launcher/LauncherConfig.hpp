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

    // Launcher version (auto-updated by tools/bump_version.sh on Release builds)
    // Source value kept in sync by sed; build system uses generated BuildVersion.hpp
#if __has_include("BuildVersion.hpp")
    #include "BuildVersion.hpp"
    inline constexpr const char* LauncherVersion = BUILD_VERSION;
#else
    inline constexpr const char* LauncherVersion = "1.0.54";
#endif

    // Launcher self-update
#ifdef __APPLE__
    inline constexpr const char* LauncherReleaseTagPrefix = "launcher-mac-v";
#elif defined(_WIN32)
    inline constexpr const char* LauncherReleaseTagPrefix = "launcher-win-v";
#else
    inline constexpr const char* LauncherReleaseTagPrefix = "launcher-linux-v";
#endif
#ifdef __APPLE__
    inline constexpr const char* LauncherBinaryName = "ObeyCraftLauncher.app";
#elif _WIN32
    inline constexpr const char* LauncherBinaryName = "ObeyCraftLauncher.exe";
#else
    inline constexpr const char* LauncherBinaryName = "ObeyCraftLauncher";
#endif

    // Game release tag prefix (platform-specific)
#ifdef __APPLE__
    inline constexpr const char* GameReleaseTagPrefix = "game-mac-v";
#elif defined(_WIN32)
    inline constexpr const char* GameReleaseTagPrefix = "game-win-v";
#else
    inline constexpr const char* GameReleaseTagPrefix = "game-linux-v";
#endif

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
