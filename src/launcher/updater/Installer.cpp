// File: src/launcher/updater/Installer.cpp
#include "Installer.hpp"
#include "common/core/Log.hpp"
#include <filesystem>
#include <fstream>
#include <cstring>

#include "unzip.h"

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace Launcher {

    bool Installer::Install(const std::string& zipPath, const std::string& installDir, StatusCallback status) {
        namespace fs = std::filesystem;

        if (status) status("Preparing installation...");
        Log::Info("Installing from: %s to: %s", zipPath.c_str(), installDir.c_str());

        std::string tempDir = installDir + "/_update_tmp";
        std::string oldDir = installDir + "/_old";

        // Clean up any previous failed update
        std::error_code ec;
        fs::remove_all(tempDir, ec);
        fs::remove_all(oldDir, ec);
        fs::create_directories(tempDir, ec);
        if (ec) {
            Log::Error("Failed to create temp directory: %s", ec.message().c_str());
            return false;
        }

        // Extract zip to temp directory
        if (status) status("Extracting files...");
        if (!ExtractZip(zipPath, tempDir, status)) {
            fs::remove_all(tempDir, ec);
            return false;
        }

        // Find the actual content root (might be nested in a folder inside the zip)
        std::string contentRoot = tempDir;
        std::vector<fs::directory_entry> topEntries;
        for (const auto& entry : fs::directory_iterator(tempDir)) {
            topEntries.push_back(entry);
        }

        // If zip contains a single non-.app directory, use that as content root
        // (Don't enter .app bundles - they ARE the content on macOS)
        if (topEntries.size() == 1 && topEntries[0].is_directory()) {
            std::string dirName = topEntries[0].path().filename().string();
            if (dirName.find(".app") == std::string::npos) {
                contentRoot = topEntries[0].path().string();
                Log::Info("Zip contains single directory, using as root: %s", contentRoot.c_str());
            }
        }

        // Swap: move existing game dir to _old, move new content to game dir
        if (status) status("Installing update...");

        std::string gameDir = installDir + "/game";

        if (fs::exists(gameDir)) {
            fs::rename(gameDir, oldDir, ec);
            if (ec) {
                Log::Error("Failed to move old game directory: %s", ec.message().c_str());
                fs::remove_all(tempDir, ec);
                return false;
            }
        }

        fs::create_directories(fs::path(gameDir).parent_path(), ec);
        fs::rename(contentRoot, gameDir, ec);
        if (ec) {
            Log::Error("Failed to move new game files: %s", ec.message().c_str());
            // Attempt rollback
            if (fs::exists(oldDir)) {
                std::error_code rollbackEc;
                fs::rename(oldDir, gameDir, rollbackEc);
            }
            fs::remove_all(tempDir, ec);
            return false;
        }

        // Clean up
        fs::remove_all(tempDir, ec);
        fs::remove_all(oldDir, ec);

        // Set executable permissions on macOS/Linux
#ifndef _WIN32
        SetExecutablePermissions(gameDir);
#endif

        // Delete the zip file
        fs::remove(zipPath, ec);

        if (status) status("Installation complete!");
        Log::Info("Installation complete");
        return true;
    }

    bool Installer::ExtractZip(const std::string& zipPath, const std::string& destDir, StatusCallback status) {
        unzFile zip = unzOpen64(zipPath.c_str());
        if (!zip) {
            Log::Error("Failed to open zip: %s", zipPath.c_str());
            return false;
        }

        unz_global_info64 globalInfo;
        if (unzGetGlobalInfo64(zip, &globalInfo) != UNZ_OK) {
            Log::Error("Failed to read zip info");
            unzClose(zip);
            return false;
        }

        Log::Info("Extracting %llu files...", globalInfo.number_entry);

        char filename[1024];
        char buffer[8192];

        for (uint64_t i = 0; i < globalInfo.number_entry; i++) {
            unz_file_info64 fileInfo;
            if (unzGetCurrentFileInfo64(zip, &fileInfo, filename, sizeof(filename),
                                         nullptr, 0, nullptr, 0) != UNZ_OK) {
                Log::Error("Failed to get file info at entry %llu", i);
                unzClose(zip);
                return false;
            }

            std::string fullPath = destDir + "/" + filename;

            // Prevent zip slip attacks
            auto canonical = std::filesystem::weakly_canonical(fullPath);
            auto destCanonical = std::filesystem::weakly_canonical(destDir);
            if (canonical.string().find(destCanonical.string()) != 0) {
                Log::Warning("Skipping suspicious zip entry: %s", filename);
                if (i + 1 < globalInfo.number_entry) unzGoToNextFile(zip);
                continue;
            }

            size_t filenameLen = strlen(filename);
            if (filenameLen > 0 && (filename[filenameLen - 1] == '/' || filename[filenameLen - 1] == '\\')) {
                // Directory entry
                std::filesystem::create_directories(fullPath);
            } else {
                // File entry
                std::filesystem::create_directories(std::filesystem::path(fullPath).parent_path());

                if (unzOpenCurrentFile(zip) != UNZ_OK) {
                    Log::Error("Failed to open zip entry: %s", filename);
                    unzClose(zip);
                    return false;
                }

                std::ofstream outFile(fullPath, std::ios::binary);
                if (!outFile.is_open()) {
                    Log::Error("Failed to create file: %s", fullPath.c_str());
                    unzCloseCurrentFile(zip);
                    unzClose(zip);
                    return false;
                }

                int bytesRead;
                while ((bytesRead = unzReadCurrentFile(zip, buffer, sizeof(buffer))) > 0) {
                    outFile.write(buffer, bytesRead);
                }

                outFile.close();
                unzCloseCurrentFile(zip);

                if (bytesRead < 0) {
                    Log::Error("Error reading zip entry: %s", filename);
                    unzClose(zip);
                    return false;
                }
            }

            if (status && i % 50 == 0) {
                status("Extracting... (" + std::to_string(i + 1) + "/" +
                       std::to_string(globalInfo.number_entry) + ")");
            }

            if (i + 1 < globalInfo.number_entry) {
                unzGoToNextFile(zip);
            }
        }

        unzClose(zip);
        Log::Info("Extraction complete");
        return true;
    }

    bool Installer::InstallLauncher(const std::string& zipPath, const std::string& currentAppPath,
                                     const std::string& stagingDir) {
        namespace fs = std::filesystem;
        std::error_code ec;

        Log::Info("Installing launcher update from: %s", zipPath.c_str());
        Log::Info("Current launcher: %s", currentAppPath.c_str());

        // Clean staging dir
        fs::remove_all(stagingDir, ec);
        fs::create_directories(stagingDir, ec);
        if (ec) {
            Log::Error("Failed to create staging dir: %s", ec.message().c_str());
            return false;
        }

        // Extract zip
        if (!ExtractZip(zipPath, stagingDir, nullptr)) {
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

        std::ofstream bat(batPath);
        bat << "@echo off\r\n";
        bat << "timeout /t 2 /nobreak >nul\r\n";
        bat << "xcopy /E /Y \"" << newLauncherPath << "\" \"" << exeDir << "\\\"\r\n";
        bat << "rmdir /S /Q \"" << stagingDir << "\"\r\n";
        bat << "start \"\" \"" << currentAppPath << "\"\r\n";
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
