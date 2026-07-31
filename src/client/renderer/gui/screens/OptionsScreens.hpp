// File: src/client/renderer/gui/screens/OptionsScreens.hpp
//
// The complete vanilla options tree:
//
//   OptionsScreen                    (menu.options — FOV + category grid)
//   ├── SkinCustomizationScreen      (model-part toggles, persisted only)
//   ├── SoundOptionsScreen           (volume sliders — persisted; no audio
//   │                                 engine yet, values apply when one lands)
//   ├── VideoSettingsScreen          (display + quality + preferences)
//   ├── ControlsScreen
//   │   ├── MouseSettingsScreen
//   │   └── KeyBindsScreen           (lists current fixed binds; rebinding
//   │                                 needs the input-mapping refactor)
//   ├── LanguageSelectScreen
//   ├── ChatOptionsScreen
//   ├── AccessibilityOptionsScreen
//   └── CreditsScreen
//
// Every option is backed by Platform::g_gameSettings (options.txt with MC
// key names), saved when a screen closes. Options the engine can act on
// immediately (FOV, sensitivity, render distance, VSync, fullscreen, GUI
// scale, max FPS, raw mouse input, invert Y, panorama speed, splash/logo
// accessibility toggles) are applied live — some directly, some via
// ScreenManager's applied-settings bits that PlatformMain drains.
#pragma once

#include "Screen.hpp"

namespace Render {

    // ── Main options screen ────────────────────────────────────────────────
    class OptionsScreen : public Screen {
    public:
        OptionsScreen() : Screen("Options") {}
        void Init() override;
        void Render(GuiGraphics& g, int mouseX, int mouseY, float partialTick) override;
        void OnClose() override;   // saves settings
    };

    // ── Shared sub-screen scaffolding (MC OptionsSubScreen) ────────────────
    // Title header (33px), scrolling OptionsList content, Done footer (33px).
    class OptionsSubScreen : public Screen {
    public:
        explicit OptionsSubScreen(std::string title) : Screen(std::move(title)) {}
        void Init() final;
        void Render(GuiGraphics& g, int mouseX, int mouseY, float partialTick) override;
        void OnClose() override;   // saves settings

    protected:
        static constexpr int HEADER_H = 33;
        static constexpr int FOOTER_H = 33;

        // Subclasses populate m_list here (MC addOptions()).
        virtual void AddOptions() = 0;

        // ── Option-row factories (all 150×20, positioned by the list) ──────
        AbstractWidget* OnOff(const std::string& caption,
                              bool initial, std::function<void(bool)> apply);
        AbstractWidget* PercentSlider(const std::string& caption,
                                      float initial01, std::function<void(float)> apply);
        // Generic value slider over [min,max]; fmt renders the label suffix.
        AbstractWidget* ValueSlider(const std::string& caption,
                                    double initial, double min, double max, double step,
                                    std::function<std::string(double)> fmt,
                                    std::function<void(double)> apply);
        AbstractWidget* Cycle(const std::string& caption,
                              std::vector<std::string> values, int initial,
                              std::function<void(int)> apply);
        AbstractWidget* NavButton(const std::string& label,
                                  std::function<std::unique_ptr<Screen>()> makeScreen);

        OptionsList* m_list = nullptr;
    };

    // ── Sub-screens ────────────────────────────────────────────────────────
    class VideoSettingsScreen : public OptionsSubScreen {
    public:
        VideoSettingsScreen() : OptionsSubScreen("Video Settings") {}
    protected:
        void AddOptions() override;
    };

    class SoundOptionsScreen : public OptionsSubScreen {
    public:
        SoundOptionsScreen() : OptionsSubScreen("Music & Sound Options") {}
    protected:
        void AddOptions() override;
    private:
        AbstractWidget* VolumeSlider(const std::string& caption, const char* settingsKey,
                                     float defaultValue = 1.0f);
    };

    class ControlsScreen : public OptionsSubScreen {
    public:
        ControlsScreen() : OptionsSubScreen("Controls") {}
    protected:
        void AddOptions() override;
    };

    class MouseSettingsScreen : public OptionsSubScreen {
    public:
        MouseSettingsScreen() : OptionsSubScreen("Mouse Settings") {}
    protected:
        void AddOptions() override;
    };

    class KeyBindsScreen : public OptionsSubScreen {
    public:
        KeyBindsScreen() : OptionsSubScreen("Key Binds") {}
    protected:
        void AddOptions() override;
    };

    class ChatOptionsScreen : public OptionsSubScreen {
    public:
        ChatOptionsScreen() : OptionsSubScreen("Chat Settings") {}
    protected:
        void AddOptions() override;
    };

    class SkinCustomizationScreen : public OptionsSubScreen {
    public:
        SkinCustomizationScreen() : OptionsSubScreen("Skin Customization") {}
    protected:
        void AddOptions() override;
    };

    class AccessibilityOptionsScreen : public OptionsSubScreen {
    public:
        AccessibilityOptionsScreen() : OptionsSubScreen("Accessibility Settings") {}
    protected:
        void AddOptions() override;
    };

    class LanguageSelectScreen : public OptionsSubScreen {
    public:
        LanguageSelectScreen() : OptionsSubScreen("Language") {}
        void Render(GuiGraphics& g, int mouseX, int mouseY, float partialTick) override;
    protected:
        void AddOptions() override;
    };

    class CreditsScreen : public Screen {
    public:
        CreditsScreen() : Screen("Credits & Attribution") {}
        void Init() override;
        void Render(GuiGraphics& g, int mouseX, int mouseY, float partialTick) override;
    };

} // namespace Render
