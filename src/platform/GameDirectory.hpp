// File: src/platform/GameDirectory.hpp
#pragma once

#include <string>
#include <unordered_map>
#include <filesystem>
#include <optional>

namespace Platform {

    // Game settings manager - handles options.txt like Minecraft
    class GameSettings {
    public:
        GameSettings();
        ~GameSettings() = default;

        // Initialize and load settings from options.txt
        bool Initialize();

        // Save current settings to options.txt
        bool Save();

        // Load settings from options.txt
        bool Load();

        // Get setting values with type safety
        int GetInt(const std::string& key, int defaultValue = 0) const;
        float GetFloat(const std::string& key, float defaultValue = 0.0f) const;
        bool GetBool(const std::string& key, bool defaultValue = false) const;
        std::string GetString(const std::string& key, const std::string& defaultValue = "") const;

        // Set setting values
        void SetInt(const std::string& key, int value);
        void SetFloat(const std::string& key, float value);
        void SetBool(const std::string& key, bool value);
        void SetString(const std::string& key, const std::string& value);

        // Common game settings (convenience accessors)
        int GetRenderDistance() const { return GetInt("renderDistance", 8); }
        void SetRenderDistance(int distance) { SetInt("renderDistance", distance); }

        bool GetVSync() const { return GetBool("vsync", true); }
        void SetVSync(bool enabled) { SetBool("vsync", enabled); }

        float GetFOV() const { return GetFloat("fov", 70.0f); }
        void SetFOV(float fov) { SetFloat("fov", fov); }

        int GetResolutionWidth() const { return GetInt("resWidth", 1280); }
        int GetResolutionHeight() const { return GetInt("resHeight", 720); }
        void SetResolution(int width, int height) {
            SetInt("resWidth", width);
            SetInt("resHeight", height);
        }

        bool GetFullscreen() const { return GetBool("fullscreen", false); }
        void SetFullscreen(bool enabled) { SetBool("fullscreen", enabled); }

        float GetMouseSensitivity() const { return GetFloat("mouseSensitivity", 0.1f); }
        void SetMouseSensitivity(float sensitivity) { SetFloat("mouseSensitivity", sensitivity); }

        bool GetShowDebugInfo() const { return GetBool("showDebugInfo", false); }
        void SetShowDebugInfo(bool show) { SetBool("showDebugInfo", show); }

        // Get the options.txt file path
        std::string GetOptionsFilePath() const;

    private:
        std::unordered_map<std::string, std::string> m_settings;
        bool m_initialized = false;

        // Create default settings
        void CreateDefaults();

        // Parse a line from options.txt
        bool ParseLine(const std::string& line);

        // Convert value to appropriate type
        std::optional<int> ParseInt(const std::string& value) const;
        std::optional<float> ParseFloat(const std::string& value) const;
        std::optional<bool> ParseBool(const std::string& value) const;
    };

    // Game directory manager - creates and manages obeycraft directory structure
    class GameDirectory {
    public:
        GameDirectory();
        ~GameDirectory() = default;

        // Initialize the game directory structure
        bool Initialize();

        // Get various directory paths
        std::string GetGameDirectory() const { return m_gameDirectory; }
        std::string GetSavesDirectory() const { return m_savesDirectory; }
        std::string GetResourcePacksDirectory() const { return m_resourcePacksDirectory; }
        std::string GetAssetsDirectory() const { return m_assetsDirectory; }
        std::string GetLogsDirectory() const { return m_logsDirectory; }
        std::string GetScreenshotsDirectory() const { return m_screenshotsDirectory; }

        // Check if directories exist
        bool IsInitialized() const { return m_initialized; }

        // Create a new save directory
        std::string CreateSaveDirectory(const std::string& worldName);

        // List all existing save directories
        std::vector<std::string> GetSaveDirectories() const;

        // Get platform-specific default game directory
        static std::string GetDefaultGameDirectory();

    private:
        std::string m_gameDirectory;
        std::string m_savesDirectory;
        std::string m_resourcePacksDirectory;
        std::string m_assetsDirectory;
        std::string m_logsDirectory;
        std::string m_screenshotsDirectory;
        bool m_initialized = false;

        // Create directory structure
        bool CreateDirectories();

        // Platform-specific directory detection
        static std::string GetUserDataDirectory();
    };

    // Global instances for easy access
    extern GameDirectory g_gameDirectory;
    extern GameSettings g_gameSettings;

    // Utility functions
    bool InitializeGameDirectorySystem();
    void ShutdownGameDirectorySystem();

} // namespace Platform