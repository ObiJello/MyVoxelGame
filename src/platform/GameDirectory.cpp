// File: src/platform/GameDirectory.cpp
#include "GameDirectory.hpp"
#include "../core/Log.hpp"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstdlib>

#ifdef _WIN32
#include <Windows.h>
#include <ShlObj.h>
#elif __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <unistd.h>
#elif __linux__
#include <unistd.h>
#include <pwd.h>
#endif

namespace Platform {

    // Global instances
    GameDirectory g_gameDirectory;
    GameSettings g_gameSettings;

    // GameSettings Implementation
    GameSettings::GameSettings() {
    }

    bool GameSettings::Initialize() {
        if (m_initialized) {
            return true;
        }

        Log::Info("Initializing GameSettings...");

        // Create default settings first
        CreateDefaults();

        // Try to load from options.txt
        if (!Load()) {
            Log::Warning("Could not load options.txt, using defaults");
            // Save defaults to create the file
            Save();
        }

        m_initialized = true;
        Log::Info("GameSettings initialized successfully");
        return true;
    }

    bool GameSettings::Save() {
        std::string optionsPath = GetOptionsFilePath();

        std::ofstream file(optionsPath);
        if (!file.is_open()) {
            Log::Error("Failed to open options.txt for writing: %s", optionsPath.c_str());
            return false;
        }

        Log::Info("Saving settings to: %s", optionsPath.c_str());

        // Write all settings in key:value format
        for (const auto& [key, value] : m_settings) {
            file << key << ":" << value << "\n";
        }

        file.close();

        Log::Info("Settings saved successfully (%zu entries)", m_settings.size());
        return true;
    }

    bool GameSettings::Load() {
        std::string optionsPath = GetOptionsFilePath();

        std::ifstream file(optionsPath);
        if (!file.is_open()) {
            Log::Info("options.txt not found at: %s", optionsPath.c_str());
            return false;
        }

        Log::Info("Loading settings from: %s", optionsPath.c_str());

        m_settings.clear();
        std::string line;
        int lineNumber = 0;
        int successfulParsed = 0;

        while (std::getline(file, line)) {
            lineNumber++;

            // Skip empty lines and comments
            if (line.empty() || line[0] == '#') {
                continue;
            }

            if (ParseLine(line)) {
                successfulParsed++;
            } else {
                Log::Warning("Failed to parse line %d in options.txt: %s", lineNumber, line.c_str());
            }
        }

        file.close();

        Log::Info("Loaded %d settings from options.txt (parsed %d/%d lines)",
                 successfulParsed, successfulParsed, lineNumber);

        // Log some key settings for debugging
        Log::Info("Key settings loaded: renderDistance=%d, fov=%.1f, vsync=%s",
                 GetRenderDistance(), GetFOV(), GetVSync() ? "true" : "false");

        return true;
    }

    void GameSettings::CreateDefaults() {
        Log::Info("Creating default game settings...");

        // Graphics settings
        SetInt("renderDistance", 8);
        SetFloat("fov", 70.0f);
        SetBool("vsync", true);
        SetBool("fullscreen", false);
        SetInt("resWidth", 1280);
        SetInt("resHeight", 720);
        SetString("graphics", "fancy");

        // Input settings
        SetFloat("mouseSensitivity", 0.1f);

        // Debug settings
        SetBool("showDebugInfo", false);
        SetBool("wireframeMode", false);

        // Performance settings
        SetInt("maxFPS", 60);
        SetBool("limitFramerate", true);

        // Audio settings (for future use)
        SetFloat("masterVolume", 1.0f);
        SetFloat("musicVolume", 0.5f);
        SetFloat("soundVolume", 1.0f);

        Log::Info("Created %zu default settings", m_settings.size());
    }

    bool GameSettings::ParseLine(const std::string& line) {
        size_t colonPos = line.find(':');
        if (colonPos == std::string::npos) {
            return false;
        }

        std::string key = line.substr(0, colonPos);
        std::string value = line.substr(colonPos + 1);

        // Trim whitespace
        key.erase(0, key.find_first_not_of(" \t"));
        key.erase(key.find_last_not_of(" \t") + 1);
        value.erase(0, value.find_first_not_of(" \t"));
        value.erase(value.find_last_not_of(" \t") + 1);

        if (key.empty()) {
            return false;
        }

        m_settings[key] = value;
        return true;
    }

    std::string GameSettings::GetOptionsFilePath() const {
        return g_gameDirectory.GetGameDirectory() + "/options.txt";
    }

    // Type-safe getters
    int GameSettings::GetInt(const std::string& key, int defaultValue) const {
        auto it = m_settings.find(key);
        if (it == m_settings.end()) {
            return defaultValue;
        }

        auto parsed = ParseInt(it->second);
        return parsed.value_or(defaultValue);
    }

    float GameSettings::GetFloat(const std::string& key, float defaultValue) const {
        auto it = m_settings.find(key);
        if (it == m_settings.end()) {
            return defaultValue;
        }

        auto parsed = ParseFloat(it->second);
        return parsed.value_or(defaultValue);
    }

    bool GameSettings::GetBool(const std::string& key, bool defaultValue) const {
        auto it = m_settings.find(key);
        if (it == m_settings.end()) {
            return defaultValue;
        }

        auto parsed = ParseBool(it->second);
        return parsed.value_or(defaultValue);
    }

    std::string GameSettings::GetString(const std::string& key, const std::string& defaultValue) const {
        auto it = m_settings.find(key);
        if (it == m_settings.end()) {
            return defaultValue;
        }
        return it->second;
    }

    // Type-safe setters
    void GameSettings::SetInt(const std::string& key, int value) {
        m_settings[key] = std::to_string(value);
    }

    void GameSettings::SetFloat(const std::string& key, float value) {
        m_settings[key] = std::to_string(value);
    }

    void GameSettings::SetBool(const std::string& key, bool value) {
        m_settings[key] = value ? "true" : "false";
    }

    void GameSettings::SetString(const std::string& key, const std::string& value) {
        m_settings[key] = value;
    }

    // Parsing helpers
    std::optional<int> GameSettings::ParseInt(const std::string& value) const {
        try {
            return std::stoi(value);
        } catch (...) {
            return std::nullopt;
        }
    }

    std::optional<float> GameSettings::ParseFloat(const std::string& value) const {
        try {
            return std::stof(value);
        } catch (...) {
            return std::nullopt;
        }
    }

    std::optional<bool> GameSettings::ParseBool(const std::string& value) const {
        std::string lower = value;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

        if (lower == "true" || lower == "1" || lower == "yes" || lower == "on") {
            return true;
        } else if (lower == "false" || lower == "0" || lower == "no" || lower == "off") {
            return false;
        }

        return std::nullopt;
    }

    // GameDirectory Implementation
    GameDirectory::GameDirectory() {
    }

    bool GameDirectory::Initialize() {
        if (m_initialized) {
            return true;
        }

        Log::Info("Initializing GameDirectory...");

        // Get the base game directory
        m_gameDirectory = GetDefaultGameDirectory();
        Log::Info("Game directory: %s", m_gameDirectory.c_str());

        // Set up subdirectories
        m_savesDirectory = m_gameDirectory + "/saves";
        m_resourcePacksDirectory = m_gameDirectory + "/resourcepacks";
        m_assetsDirectory = m_gameDirectory + "/assets";
        m_logsDirectory = m_gameDirectory + "/logs";
        m_screenshotsDirectory = m_gameDirectory + "/screenshots";

        // Create the directory structure
        if (!CreateDirectories()) {
            Log::Error("Failed to create game directory structure");
            return false;
        }

        m_initialized = true;
        Log::Info("GameDirectory initialized successfully");
        return true;
    }

    bool GameDirectory::CreateDirectories() {
        std::vector<std::string> directories = {
            m_gameDirectory,
            m_savesDirectory,
            m_resourcePacksDirectory,
            m_assetsDirectory,
            m_logsDirectory,
            m_screenshotsDirectory
        };

        for (const auto& dir : directories) {
            try {
                if (!std::filesystem::exists(dir)) {
                    std::filesystem::create_directories(dir);
                    Log::Info("Created directory: %s", dir.c_str());
                } else {
                    Log::Debug("Directory already exists: %s", dir.c_str());
                }
            } catch (const std::filesystem::filesystem_error& e) {
                Log::Error("Failed to create directory %s: %s", dir.c_str(), e.what());
                return false;
            }
        }

        return true;
    }

    std::string GameDirectory::CreateSaveDirectory(const std::string& worldName) {
        std::string saveDir = m_savesDirectory + "/" + worldName;

        try {
            if (!std::filesystem::exists(saveDir)) {
                std::filesystem::create_directories(saveDir);
                Log::Info("Created save directory: %s", saveDir.c_str());
            }
        } catch (const std::filesystem::filesystem_error& e) {
            Log::Error("Failed to create save directory %s: %s", saveDir.c_str(), e.what());
            return "";
        }

        return saveDir;
    }

    std::vector<std::string> GameDirectory::GetSaveDirectories() const {
        std::vector<std::string> saves;

        try {
            if (std::filesystem::exists(m_savesDirectory)) {
                for (const auto& entry : std::filesystem::directory_iterator(m_savesDirectory)) {
                    if (entry.is_directory()) {
                        saves.push_back(entry.path().filename().string());
                    }
                }
            }
        } catch (const std::filesystem::filesystem_error& e) {
            Log::Warning("Failed to list save directories: %s", e.what());
        }

        return saves;
    }

    std::string GameDirectory::GetDefaultGameDirectory() {
        std::string userDataDir = GetUserDataDirectory();
        return userDataDir + "/obeycraft";
    }

    std::string GameDirectory::GetUserDataDirectory() {
#ifdef _WIN32
        // Windows: %APPDATA%
        char* appData = nullptr;
        size_t len = 0;
        if (_dupenv_s(&appData, &len, "APPDATA") == 0 && appData != nullptr) {
            std::string result(appData);
            free(appData);
            return result;
        }

        // Fallback
        return "C:/Users/" + std::string(getenv("USERNAME")) + "/AppData/Roaming";

#elif __APPLE__
        // macOS: ~/Library/Application Support
        const char* home = getenv("HOME");
        if (home) {
            return std::string(home) + "/Library/Application Support";
        }

        // Fallback
        return "/tmp";

#elif __linux__
        // Linux: ~/.local/share or $XDG_DATA_HOME
        const char* xdgData = getenv("XDG_DATA_HOME");
        if (xdgData && strlen(xdgData) > 0) {
            return std::string(xdgData);
        }

        const char* home = getenv("HOME");
        if (home) {
            return std::string(home) + "/.local/share";
        }

        // Fallback
        return "/tmp";

#else
        // Unknown platform fallback
        return "./obeycraft_data";
#endif
    }

    // Utility functions
    bool InitializeGameDirectorySystem() {
        Log::Info("Initializing game directory system...");

        // Initialize game directory first
        if (!g_gameDirectory.Initialize()) {
            Log::Error("Failed to initialize game directory");
            return false;
        }

        // Then initialize settings (needs directory to exist)
        if (!g_gameSettings.Initialize()) {
            Log::Error("Failed to initialize game settings");
            return false;
        }

        Log::Info("Game directory system initialized successfully");
        Log::Info("Game directory: %s", g_gameDirectory.GetGameDirectory().c_str());
        Log::Info("Settings file: %s", g_gameSettings.GetOptionsFilePath().c_str());

        return true;
    }

    void ShutdownGameDirectorySystem() {
        Log::Info("Shutting down game directory system...");

        // Save settings before shutdown
        if (g_gameSettings.Save()) {
            Log::Info("Settings saved successfully");
        } else {
            Log::Warning("Failed to save settings");
        }

        Log::Info("Game directory system shutdown complete");
    }

} // namespace Platform