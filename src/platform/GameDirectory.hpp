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

        // === MINECRAFT-STYLE SETTINGS ===

        // Version and System
        int GetVersion() const { return GetInt("version", 4438); }
        void SetVersion(int version) { SetInt("version", version); }

        // Audio/Video Settings
        bool GetAO() const { return GetBool("ao", true); }
        void SetAO(bool enabled) { SetBool("ao", enabled); }

        int GetBiomeBlendRadius() const { return GetInt("biomeBlendRadius", 2); }
        void SetBiomeBlendRadius(int radius) { SetInt("biomeBlendRadius", radius); }

        bool GetVSync() const { return GetBool("enableVsync", true); }
        void SetVSync(bool enabled) { SetBool("enableVsync", enabled); }

        float GetEntityDistanceScaling() const { return GetFloat("entityDistanceScaling", 1.0f); }
        void SetEntityDistanceScaling(float scaling) { SetFloat("entityDistanceScaling", scaling); }

        bool GetEntityShadows() const { return GetBool("entityShadows", false); }
        void SetEntityShadows(bool enabled) { SetBool("entityShadows", enabled); }

        // Font and Localization
        bool GetForceUnicodeFont() const { return GetBool("forceUnicodeFont", false); }
        void SetForceUnicodeFont(bool force) { SetBool("forceUnicodeFont", force); }

        bool GetJapaneseGlyphVariants() const { return GetBool("japaneseGlyphVariants", false); }
        void SetJapaneseGlyphVariants(bool enabled) { SetBool("japaneseGlyphVariants", enabled); }

        std::string GetLanguage() const { return GetString("lang", "en_us"); }
        void SetLanguage(const std::string& lang) { SetString("lang", lang); }

        // Graphics Settings
        float GetFOV() const { return GetFloat("fov", 70.0f); }
        void SetFOV(float fov) { SetFloat("fov", fov); }

        float GetFOVEffectScale() const { return GetFloat("fovEffectScale", 1.0f); }
        void SetFOVEffectScale(float scale) { SetFloat("fovEffectScale", scale); }

        float GetDarknessEffectScale() const { return GetFloat("darknessEffectScale", 1.0f); }
        void SetDarknessEffectScale(float scale) { SetFloat("darknessEffectScale", scale); }

        float GetGlintSpeed() const { return GetFloat("glintSpeed", 0.5f); }
        void SetGlintSpeed(float speed) { SetFloat("glintSpeed", speed); }

        float GetGlintStrength() const { return GetFloat("glintStrength", 0.75f); }
        void SetGlintStrength(float strength) { SetFloat("glintStrength", strength); }

        int GetPrioritizeChunkUpdates() const { return GetInt("prioritizeChunkUpdates", 0); }
        void SetPrioritizeChunkUpdates(int priority) { SetInt("prioritizeChunkUpdates", priority); }

        bool GetFullscreen() const { return GetBool("fullscreen", false); }
        void SetFullscreen(bool enabled) { SetBool("fullscreen", enabled); }

        float GetGamma() const { return GetFloat("gamma", 1.0f); }
        void SetGamma(float gamma) { SetFloat("gamma", gamma); }

        int GetGraphicsMode() const { return GetInt("graphicsMode", 1); }
        void SetGraphicsMode(int mode) { SetInt("graphicsMode", mode); }

        int GetGuiScale() const { return GetInt("guiScale", 3); }
        void SetGuiScale(int scale) { SetInt("guiScale", scale); }

        int GetMaxFPS() const { return GetInt("maxFps", 120); }
        void SetMaxFPS(int fps) { SetInt("maxFps", fps); }

        std::string GetInactivityFPSLimit() const { return GetString("inactivityFpsLimit", "afk"); }
        void SetInactivityFPSLimit(const std::string& limit) { SetString("inactivityFpsLimit", limit); }

        int GetMipmapLevels() const { return GetInt("mipmapLevels", 4); }
        void SetMipmapLevels(int levels) { SetInt("mipmapLevels", levels); }

        int GetNarrator() const { return GetInt("narrator", 0); }
        void SetNarrator(int mode) { SetInt("narrator", mode); }

        int GetParticles() const { return GetInt("particles", 1); }
        void SetParticles(int level) { SetInt("particles", level); }

        bool GetReducedDebugInfo() const { return GetBool("reducedDebugInfo", false); }
        void SetReducedDebugInfo(bool reduced) { SetBool("reducedDebugInfo", reduced); }

        std::string GetRenderClouds() const { return GetString("renderClouds", "true"); }
        void SetRenderClouds(const std::string& mode) { SetString("renderClouds", mode); }

        int GetCloudRange() const { return GetInt("cloudRange", 128); }
        void SetCloudRange(int range) { SetInt("cloudRange", range); }

        int GetRenderDistance() const { return GetInt("renderDistance", 12); }
        void SetRenderDistance(int distance) { SetInt("renderDistance", distance); }

        int GetSimulationDistance() const { return GetInt("simulationDistance", 12); }
        void SetSimulationDistance(int distance) { SetInt("simulationDistance", distance); }

        float GetScreenEffectScale() const { return GetFloat("screenEffectScale", 1.0f); }
        void SetScreenEffectScale(float scale) { SetFloat("screenEffectScale", scale); }

        // Audio Settings
        std::string GetSoundDevice() const { return GetString("soundDevice", ""); }
        void SetSoundDevice(const std::string& device) { SetString("soundDevice", device); }

        float GetMasterVolume() const { return GetFloat("soundCategory_master", 0.20778146f); }
        void SetMasterVolume(float volume) { SetFloat("soundCategory_master", volume); }

        float GetMusicVolume() const { return GetFloat("soundCategory_music", 0.0f); }
        void SetMusicVolume(float volume) { SetFloat("soundCategory_music", volume); }

        float GetRecordVolume() const { return GetFloat("soundCategory_record", 1.0f); }
        void SetRecordVolume(float volume) { SetFloat("soundCategory_record", volume); }

        float GetWeatherVolume() const { return GetFloat("soundCategory_weather", 1.0f); }
        void SetWeatherVolume(float volume) { SetFloat("soundCategory_weather", volume); }

        float GetBlockVolume() const { return GetFloat("soundCategory_block", 1.0f); }
        void SetBlockVolume(float volume) { SetFloat("soundCategory_block", volume); }

        float GetHostileVolume() const { return GetFloat("soundCategory_hostile", 1.0f); }
        void SetHostileVolume(float volume) { SetFloat("soundCategory_hostile", volume); }

        float GetNeutralVolume() const { return GetFloat("soundCategory_neutral", 1.0f); }
        void SetNeutralVolume(float volume) { SetFloat("soundCategory_neutral", volume); }

        float GetPlayerVolume() const { return GetFloat("soundCategory_player", 1.0f); }
        void SetPlayerVolume(float volume) { SetFloat("soundCategory_player", volume); }

        float GetAmbientVolume() const { return GetFloat("soundCategory_ambient", 1.0f); }
        void SetAmbientVolume(float volume) { SetFloat("soundCategory_ambient", volume); }

        float GetVoiceVolume() const { return GetFloat("soundCategory_voice", 1.0f); }
        void SetVoiceVolume(float volume) { SetFloat("soundCategory_voice", volume); }

        float GetUIVolume() const { return GetFloat("soundCategory_ui", 1.0f); }
        void SetUIVolume(float volume) { SetFloat("soundCategory_ui", volume); }

        // Controls and Input
        bool GetAutoJump() const { return GetBool("autoJump", false); }
        void SetAutoJump(bool enabled) { SetBool("autoJump", enabled); }

        bool GetRotateWithMinecart() const { return GetBool("rotateWithMinecart", false); }
        void SetRotateWithMinecart(bool enabled) { SetBool("rotateWithMinecart", enabled); }

        bool GetOperatorItemsTab() const { return GetBool("operatorItemsTab", false); }
        void SetOperatorItemsTab(bool enabled) { SetBool("operatorItemsTab", enabled); }

        bool GetAutoSuggestions() const { return GetBool("autoSuggestions", true); }
        void SetAutoSuggestions(bool enabled) { SetBool("autoSuggestions", enabled); }

        bool GetChatColors() const { return GetBool("chatColors", true); }
        void SetChatColors(bool enabled) { SetBool("chatColors", enabled); }

        bool GetChatLinks() const { return GetBool("chatLinks", true); }
        void SetChatLinks(bool enabled) { SetBool("chatLinks", enabled); }

        bool GetChatLinksPrompt() const { return GetBool("chatLinksPrompt", true); }
        void SetChatLinksPrompt(bool enabled) { SetBool("chatLinksPrompt", enabled); }

        bool GetDiscreteMouseScroll() const { return GetBool("discrete_mouse_scroll", false); }
        void SetDiscreteMouseScroll(bool enabled) { SetBool("discrete_mouse_scroll", enabled); }

        bool GetInvertYMouse() const { return GetBool("invertYMouse", false); }
        void SetInvertYMouse(bool enabled) { SetBool("invertYMouse", enabled); }

        bool GetRealmsNotifications() const { return GetBool("realmsNotifications", true); }
        void SetRealmsNotifications(bool enabled) { SetBool("realmsNotifications", enabled); }

        bool GetShowSubtitles() const { return GetBool("showSubtitles", false); }
        void SetShowSubtitles(bool enabled) { SetBool("showSubtitles", enabled); }

        bool GetDirectionalAudio() const { return GetBool("directionalAudio", false); }
        void SetDirectionalAudio(bool enabled) { SetBool("directionalAudio", enabled); }

        bool GetTouchscreen() const { return GetBool("touchscreen", false); }
        void SetTouchscreen(bool enabled) { SetBool("touchscreen", enabled); }

        bool GetBobView() const { return GetBool("bobView", false); }
        void SetBobView(bool enabled) { SetBool("bobView", enabled); }

        bool GetToggleCrouch() const { return GetBool("toggleCrouch", false); }
        void SetToggleCrouch(bool enabled) { SetBool("toggleCrouch", enabled); }

        bool GetToggleSprint() const { return GetBool("toggleSprint", false); }
        void SetToggleSprint(bool enabled) { SetBool("toggleSprint", enabled); }

        bool GetDarkMojangStudiosBackground() const { return GetBool("darkMojangStudiosBackground", false); }
        void SetDarkMojangStudiosBackground(bool enabled) { SetBool("darkMojangStudiosBackground", enabled); }

        bool GetHideLightningFlashes() const { return GetBool("hideLightningFlashes", false); }
        void SetHideLightningFlashes(bool enabled) { SetBool("hideLightningFlashes", enabled); }

        bool GetHideSplashTexts() const { return GetBool("hideSplashTexts", false); }
        void SetHideSplashTexts(bool enabled) { SetBool("hideSplashTexts", enabled); }

        float GetMouseSensitivity() const { return GetFloat("mouseSensitivity", 0.5f); }
        void SetMouseSensitivity(float sensitivity) { SetFloat("mouseSensitivity", sensitivity); }

        float GetDamageTiltStrength() const { return GetFloat("damageTiltStrength", 1.0f); }
        void SetDamageTiltStrength(float strength) { SetFloat("damageTiltStrength", strength); }

        // Accessibility
        bool GetHighContrast() const { return GetBool("highContrast", false); }
        void SetHighContrast(bool enabled) { SetBool("highContrast", enabled); }

        bool GetHighContrastBlockOutline() const { return GetBool("highContrastBlockOutline", false); }
        void SetHighContrastBlockOutline(bool enabled) { SetBool("highContrastBlockOutline", enabled); }

        bool GetNarratorHotkey() const { return GetBool("narratorHotkey", true); }
        void SetNarratorHotkey(bool enabled) { SetBool("narratorHotkey", enabled); }

        // Multiplayer and Server Settings
        std::string GetLastServer() const { return GetString("lastServer", ""); }
        void SetLastServer(const std::string& server) { SetString("lastServer", server); }

        int GetChatVisibility() const { return GetInt("chatVisibility", 0); }
        void SetChatVisibility(int visibility) { SetInt("chatVisibility", visibility); }

        float GetChatOpacity() const { return GetFloat("chatOpacity", 1.0f); }
        void SetChatOpacity(float opacity) { SetFloat("chatOpacity", opacity); }

        float GetChatLineSpacing() const { return GetFloat("chatLineSpacing", 0.0f); }
        void SetChatLineSpacing(float spacing) { SetFloat("chatLineSpacing", spacing); }

        float GetTextBackgroundOpacity() const { return GetFloat("textBackgroundOpacity", 0.5f); }
        void SetTextBackgroundOpacity(float opacity) { SetFloat("textBackgroundOpacity", opacity); }

        bool GetBackgroundForChatOnly() const { return GetBool("backgroundForChatOnly", true); }
        void SetBackgroundForChatOnly(bool enabled) { SetBool("backgroundForChatOnly", enabled); }

        bool GetHideServerAddress() const { return GetBool("hideServerAddress", false); }
        void SetHideServerAddress(bool enabled) { SetBool("hideServerAddress", enabled); }

        bool GetAdvancedItemTooltips() const { return GetBool("advancedItemTooltips", true); }
        void SetAdvancedItemTooltips(bool enabled) { SetBool("advancedItemTooltips", enabled); }

        bool GetPauseOnLostFocus() const { return GetBool("pauseOnLostFocus", true); }
        void SetPauseOnLostFocus(bool enabled) { SetBool("pauseOnLostFocus", enabled); }

        // Screen and Resolution
        int GetOverrideWidth() const { return GetInt("overrideWidth", 0); }
        void SetOverrideWidth(int width) { SetInt("overrideWidth", width); }

        int GetOverrideHeight() const { return GetInt("overrideHeight", 0); }
        void SetOverrideHeight(int height) { SetInt("overrideHeight", height); }

        float GetChatHeightFocused() const { return GetFloat("chatHeightFocused", 1.0f); }
        void SetChatHeightFocused(float height) { SetFloat("chatHeightFocused", height); }

        float GetChatDelay() const { return GetFloat("chatDelay", 0.0f); }
        void SetChatDelay(float delay) { SetFloat("chatDelay", delay); }

        float GetChatHeightUnfocused() const { return GetFloat("chatHeightUnfocused", 0.4375f); }
        void SetChatHeightUnfocused(float height) { SetFloat("chatHeightUnfocused", height); }

        float GetChatScale() const { return GetFloat("chatScale", 1.0f); }
        void SetChatScale(float scale) { SetFloat("chatScale", scale); }

        float GetChatWidth() const { return GetFloat("chatWidth", 1.0f); }
        void SetChatWidth(float width) { SetFloat("chatWidth", width); }

        float GetNotificationDisplayTime() const { return GetFloat("notificationDisplayTime", 1.0f); }
        void SetNotificationDisplayTime(float time) { SetFloat("notificationDisplayTime", time); }

        // Advanced Settings
        bool GetUseNativeTransport() const { return GetBool("useNativeTransport", true); }
        void SetUseNativeTransport(bool enabled) { SetBool("useNativeTransport", enabled); }

        std::string GetMainHand() const { return GetString("mainHand", "right"); }
        void SetMainHand(const std::string& hand) { SetString("mainHand", hand); }

        int GetAttackIndicator() const { return GetInt("attackIndicator", 1); }
        void SetAttackIndicator(int indicator) { SetInt("attackIndicator", indicator); }

        std::string GetTutorialStep() const { return GetString("tutorialStep", "none"); }
        void SetTutorialStep(const std::string& step) { SetString("tutorialStep", step); }

        float GetMouseWheelSensitivity() const { return GetFloat("mouseWheelSensitivity", 1.0f); }
        void SetMouseWheelSensitivity(float sensitivity) { SetFloat("mouseWheelSensitivity", sensitivity); }

        bool GetRawMouseInput() const { return GetBool("rawMouseInput", true); }
        void SetRawMouseInput(bool enabled) { SetBool("rawMouseInput", enabled); }

        int GetGLDebugVerbosity() const { return GetInt("glDebugVerbosity", 1); }
        void SetGLDebugVerbosity(int verbosity) { SetInt("glDebugVerbosity", verbosity); }

        bool GetSkipMultiplayerWarning() const { return GetBool("skipMultiplayerWarning", true); }
        void SetSkipMultiplayerWarning(bool skip) { SetBool("skipMultiplayerWarning", skip); }

        bool GetHideMatchedNames() const { return GetBool("hideMatchedNames", true); }
        void SetHideMatchedNames(bool hide) { SetBool("hideMatchedNames", hide); }

        bool GetJoinedFirstServer() const { return GetBool("joinedFirstServer", false); }
        void SetJoinedFirstServer(bool joined) { SetBool("joinedFirstServer", joined); }

        bool GetSyncChunkWrites() const { return GetBool("syncChunkWrites", false); }
        void SetSyncChunkWrites(bool sync) { SetBool("syncChunkWrites", sync); }

        bool GetShowAutosaveIndicator() const { return GetBool("showAutosaveIndicator", true); }
        void SetShowAutosaveIndicator(bool show) { SetBool("showAutosaveIndicator", show); }

        bool GetAllowServerListing() const { return GetBool("allowServerListing", true); }
        void SetAllowServerListing(bool allow) { SetBool("allowServerListing", allow); }

        bool GetOnlyShowSecureChat() const { return GetBool("onlyShowSecureChat", false); }
        void SetOnlyShowSecureChat(bool only) { SetBool("onlyShowSecureChat", only); }

        float GetPanoramaScrollSpeed() const { return GetFloat("panoramaScrollSpeed", 1.0f); }
        void SetPanoramaScrollSpeed(float speed) { SetFloat("panoramaScrollSpeed", speed); }

        bool GetTelemetryOptInExtra() const { return GetBool("telemetryOptInExtra", false); }
        void SetTelemetryOptInExtra(bool opt) { SetBool("telemetryOptInExtra", opt); }

        bool GetOnboardAccessibility() const { return GetBool("onboardAccessibility", false); }
        void SetOnboardAccessibility(bool onboard) { SetBool("onboardAccessibility", onboard); }

        int GetMenuBackgroundBlurriness() const { return GetInt("menuBackgroundBlurriness", 5); }
        void SetMenuBackgroundBlurriness(int blur) { SetInt("menuBackgroundBlurriness", blur); }

        bool GetStartedCleanly() const { return GetBool("startedCleanly", true); }
        void SetStartedCleanly(bool clean) { SetBool("startedCleanly", clean); }

        bool GetShowNowPlayingToast() const { return GetBool("showNowPlayingToast", false); }
        void SetShowNowPlayingToast(bool show) { SetBool("showNowPlayingToast", show); }

        std::string GetMusicFrequency() const { return GetString("musicFrequency", "DEFAULT"); }
        void SetMusicFrequency(const std::string& frequency) { SetString("musicFrequency", frequency); }

        // Key Bindings (using string representation for now)
        std::string GetKeyBinding(const std::string& action) const {
            return GetString("key_" + action, "");
        }
        void SetKeyBinding(const std::string& action, const std::string& key) {
            SetString("key_" + action, key);
        }

        // Model Parts (for player skin customization)
        bool GetModelPart(const std::string& part) const {
            return GetBool("modelPart_" + part, true);
        }
        void SetModelPart(const std::string& part, bool enabled) {
            SetBool("modelPart_" + part, enabled);
        }

        // Resource packs (stored as arrays - simplified for now)
        std::string GetResourcePacks() const { return GetString("resourcePacks", "[]"); }
        void SetResourcePacks(const std::string& packs) { SetString("resourcePacks", packs); }

        std::string GetIncompatibleResourcePacks() const { return GetString("incompatibleResourcePacks", "[]"); }
        void SetIncompatibleResourcePacks(const std::string& packs) { SetString("incompatibleResourcePacks", packs); }

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
        
        // Get default save world path (temporary solution for loading existing saves)
        std::string GetDefaultSaveWorldPath() const;
        
        // Check if default save world exists
        bool HasDefaultSaveWorld() const;

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