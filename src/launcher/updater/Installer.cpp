// File: src/launcher/updater/Installer.cpp
#include "Installer.hpp"
#include "Extractor.hpp"
#include "FileOps.hpp"
#include "common/core/Log.hpp"
#include "common/core/ZipArchive.hpp"
#include <filesystem>
#include <fstream>
#include <cstdint>
#include <cstring>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace Launcher {

    bool Installer::InstallLauncher(const std::string& zipPath, const std::string& currentAppPath,
                                     const std::string& stagingDir) {
        namespace fs = std::filesystem;
        std::error_code ec;

        Log::Info("Installing launcher update from: %s", zipPath.c_str());
        Log::Info("Current launcher: %s", currentAppPath.c_str());

        // Clean staging dir
        FileOps::Discard(stagingDir);
        fs::create_directories(stagingDir, ec);
        if (ec) {
            Log::Error("Failed to create staging dir: %s", ec.message().c_str());
            return false;
        }

        // Extract zip
        std::vector<Core::ZipArchive::Entry> entries;
        std::string extractError;
        if (!Extract::ExtractAll(zipPath, stagingDir, entries, extractError)) {
            Log::Error("Failed to extract launcher update: %s", extractError.c_str());
            fs::remove_all(stagingDir, ec);
            return false;
        }

        // Find the launcher binary in extracted content
        std::string newLauncherPath;
        for (const auto& entry : fs::directory_iterator(stagingDir)) {
            std::string name = entry.path().filename().string();
#ifdef __APPLE__
            // On macOS, .app bundles are directories
            if (name.find(".app") != std::string::npos && entry.is_directory()) {
                newLauncherPath = entry.path().string();
                break;
            }
#elif defined(_WIN32)
            if (name.find(".exe") != std::string::npos && entry.is_regular_file()) {
                newLauncherPath = entry.path().string();
                break;
            }
#else
            if (entry.is_regular_file()) {
                newLauncherPath = entry.path().string();
                break;
            }
#endif
        }

        // If not found at top level, check one level deeper (nested directory in zip)
        if (newLauncherPath.empty()) {
            for (const auto& topEntry : fs::directory_iterator(stagingDir)) {
                if (!topEntry.is_directory()) continue;
                // Skip .app bundles (already checked above on macOS)
                if (topEntry.path().filename().string().find(".app") != std::string::npos) continue;
                for (const auto& entry : fs::directory_iterator(topEntry.path())) {
                    std::string name = entry.path().filename().string();
#ifdef __APPLE__
                    if (name.find(".app") != std::string::npos && entry.is_directory()) {
                        newLauncherPath = entry.path().string();
                        break;
                    }
#elif defined(_WIN32)
                    if (name.find(".exe") != std::string::npos && entry.is_regular_file()) {
                        newLauncherPath = entry.path().string();
                        break;
                    }
#else
                    if (entry.is_regular_file()) {
                        newLauncherPath = entry.path().string();
                        break;
                    }
#endif
                }
                if (!newLauncherPath.empty()) break;
            }
        }

        if (newLauncherPath.empty()) {
            Log::Error("Could not find launcher binary in extracted content");
            fs::remove_all(stagingDir, ec);
            return false;
        }

        Log::Info("Found new launcher: %s", newLauncherPath.c_str());

#ifdef _WIN32
        // Windows: can't replace running exe. Write a batch script to do the swap.
        std::string exeDir = fs::path(currentAppPath).parent_path().string();
        std::string batPath = stagingDir + "/update_launcher.bat";

        // Build helper paths for the rename trick
        std::string currentFilename = fs::path(currentAppPath).filename().string();
        std::string oldFilename = currentFilename + ".old";
        std::string oldFullPath = fs::path(currentAppPath).parent_path().string() + "\\" + oldFilename;

        std::ofstream bat(batPath);
        bat << "@echo off\r\n";
        bat << "timeout /t 2 /nobreak >nul\r\n";
        // Rename old exe first — renaming always succeeds even if the file is still briefly
        // held by the OS (e.g. Defender), because rename is a directory-entry operation.
        // Then copy to the now-vacant path so there is nothing to lock.
        bat << "ren \"" << currentAppPath << "\" \"" << oldFilename << "\"\r\n";
        bat << "copy /Y \"" << newLauncherPath << "\" \"" << currentAppPath << "\"\r\n";
        bat << "start \"\" \"" << currentAppPath << "\"\r\n";
        bat << "del /F /Q \"" << oldFullPath << "\"\r\n";
        bat << "rmdir /S /Q \"" << stagingDir << "\"\r\n";
        bat << "del \"%~f0\"\r\n";
        bat.close();

        m_updaterScriptPath = batPath;
        Log::Info("Wrote Windows updater script: %s", batPath.c_str());
#else
        // macOS/Linux: can replace .app bundle while running
        if (fs::exists(currentAppPath)) {
            fs::remove_all(currentAppPath, ec);
            if (ec) {
                Log::Error("Failed to remove old launcher: %s", ec.message().c_str());
                fs::remove_all(stagingDir, ec);
                return false;
            }
        }

        fs::rename(newLauncherPath, currentAppPath, ec);
        if (ec) {
            Log::Error("Failed to move new launcher: %s", ec.message().c_str());
            fs::remove_all(stagingDir, ec);
            return false;
        }

        SetExecutablePermissions(currentAppPath);

        // Clear quarantine and ad-hoc sign on macOS
#ifdef __APPLE__
        std::string clearCmd = "xattr -cr \"" + currentAppPath + "\" 2>/dev/null";
        system(clearCmd.c_str());
        std::string signCmd = "codesign --force --deep --sign - \"" + currentAppPath + "\" 2>/dev/null";
        system(signCmd.c_str());
        Log::Info("Cleared quarantine and ad-hoc signed new launcher");
#endif

        // Clean up
        fs::remove_all(stagingDir, ec);
        fs::remove(zipPath, ec);
#endif

        Log::Info("Launcher update installed successfully");
        return true;
    }

    bool Installer::SetExecutablePermissions(const std::string& installDir) {
#ifndef _WIN32
        namespace fs = std::filesystem;

        // Find and chmod +x any binary files in the game directory
        // On macOS, look inside .app/Contents/MacOS/
        for (const auto& entry : fs::recursive_directory_iterator(installDir)) {
            if (!entry.is_regular_file()) continue;

            std::string path = entry.path().string();

            // Mark executables in Contents/MacOS/ and any .sh scripts
            if (path.find("Contents/MacOS/") != std::string::npos ||
                entry.path().extension() == ".sh") {
                chmod(path.c_str(), 0755);
                Log::Info("Set executable permission: %s", path.c_str());
            }
        }
#endif
        return true;
    }

} // namespace Launcher
