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
//   │   └── KeyBindsScreen           (rebindable; backed by Input::KeyMapping,
//   │                                 persisted as key_<id> in options.txt)
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
#include "Widgets.hpp"
#include "client/input/KeyMapping.hpp"
#include <functional>
#include <utility>
#include <vector>

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

        // An optional second footer button placed beside Done, as MC's
        // KeyBindsScreen does with "Reset Keys". Set from AddOptions; Init
        // lays the pair out centred so they can't overlap. Leaving the label
        // empty keeps the single centred Done button.
        std::string m_footerExtraLabel;
        std::function<void()> m_footerExtraAction;

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

    // ── World Settings ─────────────────────────────────────────────────────
    // Per-world options (currently the skybox + its day/night behavior).
    // Unlike the other sub-screens these persist to the CURRENT world's
    // entry in worlds.json, not to options.txt — so PlatformMain announces
    // which world is active at session start (and clears it on quit-to-title,
    // which also greys out the button on the title screen's options menu).
    namespace WorldSettingsContext {
        void Set(const std::string& worldName, bool canPersist);
        void Clear();
        bool Active();
        const std::string& WorldName();
        bool CanPersist();
    }

    class WorldSettingsScreen : public OptionsSubScreen {
    public:
        WorldSettingsScreen() : OptionsSubScreen("World Settings") {}
    protected:
        void AddOptions() override;
    };

    // Scrollable pick-one list of every discovered skybox (MC language-screen
    // pattern — the codebase has no floating dropdown widget, and a full
    // screen list scales to any number of sets). Selecting applies + pops
    // back to World Settings, whose button label refreshes on re-init.
    class SkyboxSelectScreen : public OptionsSubScreen {
    public:
        SkyboxSelectScreen() : OptionsSubScreen("Select Skybox") {}
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

    // MC KeyBindsScreen. Rows are grouped by category; clicking a row's button
    // arms it for capture and the next key or mouse press becomes its binding
    // (ESC clears it, matching vanilla). Conflicting rows render red.
    class KeyBindsScreen : public OptionsSubScreen {
    public:
        KeyBindsScreen() : OptionsSubScreen("Key Binds") {}

        // Capture hooks — only meaningful while a row is armed.
        bool KeyPressed(int glfwKey, int glfwMods) override;
        bool MouseClicked(double mx, double my, int button) override;
        // ESC is the "clear this binding" gesture while capturing, so it must
        // not also close the screen.
        bool ShouldCloseOnEsc() const override { return m_selected == nullptr; }
        void Render(GuiGraphics& g, int mouseX, int mouseY, float partialTick) override;
        void OnClose() override;

    protected:
        void AddOptions() override;

    private:
        void RefreshLabels();

        Input::KeyMapping* m_selected = nullptr;   // row armed for capture
        std::vector<std::pair<Input::KeyMapping*, Button*>> m_rows;
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
