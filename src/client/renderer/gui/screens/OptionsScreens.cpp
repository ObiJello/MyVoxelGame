// File: src/client/renderer/gui/screens/OptionsScreens.cpp
#include "OptionsScreens.hpp"
#include <GLFW/glfw3.h>
#include "PanoramaRenderer.hpp"
#include "WorldSelectScreens.hpp"
#include "../GuiGraphics.hpp"
#include "../FontRenderer.hpp"
#include "../../environment/SkyRenderer.hpp"
#include "platform/GameDirectory.hpp"
#include "common/core/Log.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>

namespace Render {

    namespace {
        Platform::GameSettings& Settings() { return Platform::g_gameSettings; }

        std::string FormatInt(const std::string& caption, int v, const char* suffix = "") {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "%s: %d%s", caption.c_str(), v, suffix);
            return buf;
        }
        std::string FormatPercent(const std::string& caption, double v01) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "%s: %d%%", caption.c_str(),
                          static_cast<int>(std::lround(v01 * 100.0)));
            return buf;
        }
    } // namespace

    // ═══════════════════════════════ OptionsScreen ══════════════════════════

    void OptionsScreen::Init() {
        const int cx = m_width / 2;
        const int leftX  = cx - 155;
        const int rightX = cx + 5;

        // ── Header row: FOV slider + Online (vanilla title-screen variant) ──
        int y = m_height / 6 - 12;
        {
            // FOV 30..110, "Normal" at 70, "Quake Pro" at 110 (MC labels).
            const double fov = Settings().GetFOV();
            auto fmt = [](double norm) -> std::string {
                int v = 30 + static_cast<int>(std::lround(norm * 80.0));
                if (v == 70)  return "FOV: Normal";
                if (v == 110) return "FOV: Quake Pro";
                return FormatInt("FOV", v);
            };
            AddWidget(new SliderButton(leftX, y, 150, 20, (fov - 30.0) / 80.0,
                fmt,
                [](double norm) {
                    int v = 30 + static_cast<int>(std::lround(norm * 80.0));
                    Settings().SetFOV(static_cast<float>(v));
                },
                1.0 / 80.0));
        }
        Button* online = AddWidget(new Button(rightX, y, 150, 20, "Online...", nullptr));
        online->active = false;
        online->SetTooltip({"Not available."});

        // ── Category grid (2 columns × 5 rows, vanilla order) ───────────────
        struct Entry {
            const char* label;
            std::function<std::unique_ptr<Screen>()> make;
            const char* disabledReason; // non-null → greyed out
        };
        const bool worldActive = WorldSettingsContext::Active();
        const Entry entries[] = {
            {"Skin Customization...",     [] { return std::make_unique<SkinCustomizationScreen>(); },  nullptr},
            {"Music & Sounds...",         [] { return std::make_unique<SoundOptionsScreen>(); },       nullptr},
            {"Video Settings...",         [] { return std::make_unique<VideoSettingsScreen>(); },      nullptr},
            {"Controls...",               [] { return std::make_unique<ControlsScreen>(); },           nullptr},
            {"Language...",               [] { return std::make_unique<LanguageSelectScreen>(); },     nullptr},
            {"Chat Settings...",          [] { return std::make_unique<ChatOptionsScreen>(); },        nullptr},
            {"Resource Packs...",         nullptr,                                                     "Not available."},
            {"Accessibility Settings...", [] { return std::make_unique<AccessibilityOptionsScreen>(); }, nullptr},
            {"Telemetry Data...",         nullptr,                                                     "Telemetry is not collected."},
            {"Credits & Attribution...",  [] { return std::make_unique<CreditsScreen>(); },            nullptr},
            {"World Settings...",
             worldActive ? std::function<std::unique_ptr<Screen>()>(
                               [] { return std::make_unique<WorldSettingsScreen>(); })
                         : nullptr,
             worldActive ? nullptr : "Join a world first."},
        };

        y = m_height / 6 + 18;
        int col = 0;
        for (const auto& e : entries) {
            const int x = (col == 0) ? leftX : rightX;
            if (e.make) {
                auto makeFn = e.make;
                AddWidget(new Button(x, y, 150, 20, e.label, [this, makeFn] {
                    m_manager->Push(makeFn());
                }));
            } else {
                Button* b = AddWidget(new Button(x, y, 150, 20, e.label, nullptr));
                b->active = false;
                if (e.disabledReason) b->SetTooltip({e.disabledReason});
            }
            if (++col == 2) { col = 0; y += 24; }
        }

        // ── Done ────────────────────────────────────────────────────────────
        AddWidget(new Button(cx - 100, m_height / 6 + 168, 200, 20, "Done",
                             [this] { OnClose(); }));
    }

    void OptionsScreen::Render(GuiGraphics& g, int mouseX, int mouseY, float partialTick) {
        Screen::Render(g, mouseX, mouseY, partialTick);
        g.DrawCenteredString(m_title, m_width / 2, 15, 0xFFFFFFFF);
    }

    void OptionsScreen::OnClose() {
        Settings().Save();
        Screen::OnClose();
    }

    // ═══════════════════════════ OptionsSubScreen ═══════════════════════════

    void OptionsSubScreen::Init() {
        m_footerExtraLabel.clear();
        m_footerExtraAction = nullptr;

        m_list = AddWidget(new OptionsList(0, HEADER_H, m_width,
                                           m_height - HEADER_H - FOOTER_H));
        AddOptions();

        const int footerY = m_height - FOOTER_H / 2 - 10;
        if (m_footerExtraLabel.empty()) {
            AddWidget(new Button(m_width / 2 - 100, footerY, 200, 20, "Done",
                                 [this] { OnClose(); }));
        } else {
            // Two 150-wide buttons with MC's 4px gap, centred as a pair. The
            // extra button must be positioned HERE rather than by the subclass:
            // AddOptions runs before Done exists, so a subclass placing its own
            // button could only guess where Done would land — and did, landing
            // on top of it.
            const int w = 150, gap = 4;
            const int left = m_width / 2 - (w * 2 + gap) / 2;
            auto action = m_footerExtraAction;
            AddWidget(new Button(left, footerY, w, 20, m_footerExtraLabel,
                                 [action] { if (action) action(); }));
            AddWidget(new Button(left + w + gap, footerY, w, 20, "Done",
                                 [this] { OnClose(); }));
        }
    }

    void OptionsSubScreen::Render(GuiGraphics& g, int mouseX, int mouseY, float partialTick) {
        Screen::Render(g, mouseX, mouseY, partialTick);
        g.DrawCenteredString(m_title, m_width / 2,
                             (HEADER_H - FontRenderer::LINE_HEIGHT) / 2, 0xFFFFFFFF);
        RenderMenuSeparators(g, m_width, HEADER_H - 2, m_height - FOOTER_H);
    }

    void OptionsSubScreen::OnClose() {
        Settings().Save();
        Screen::OnClose();
    }

    AbstractWidget* OptionsSubScreen::OnOff(const std::string& caption,
                                            bool initial, std::function<void(bool)> apply) {
        return CycleButton::MakeOnOff(0, 0, 150, 20, caption, initial, std::move(apply));
    }

    AbstractWidget* OptionsSubScreen::PercentSlider(const std::string& caption,
                                                    float initial01,
                                                    std::function<void(float)> apply) {
        return new SliderButton(0, 0, 150, 20, initial01,
            [caption](double v) { return FormatPercent(caption, v); },
            [fn = std::move(apply)](double v) { if (fn) fn(static_cast<float>(v)); },
            0.01);
    }

    AbstractWidget* OptionsSubScreen::ValueSlider(const std::string& caption,
                                                  double initial, double min, double max,
                                                  double step,
                                                  std::function<std::string(double)> fmt,
                                                  std::function<void(double)> apply) {
        const double range = max - min;
        auto denorm = [min, range, step](double norm) {
            double v = min + norm * range;
            if (step > 0.0) v = std::round(v / step) * step;
            return v;
        };
        return new SliderButton(0, 0, 150, 20, (initial - min) / range,
            [caption, fmt, denorm](double norm) {
                double v = denorm(norm);
                return caption + ": " + (fmt ? fmt(v) : std::to_string(v));
            },
            [fn = std::move(apply), denorm](double norm) { if (fn) fn(denorm(norm)); },
            step > 0.0 ? step / range : 0.0);
    }

    AbstractWidget* OptionsSubScreen::Cycle(const std::string& caption,
                                            std::vector<std::string> values, int initial,
                                            std::function<void(int)> apply) {
        return new CycleButton(0, 0, 150, 20, caption, std::move(values), initial,
                               std::move(apply));
    }

    AbstractWidget* OptionsSubScreen::NavButton(const std::string& label,
                                                std::function<std::unique_ptr<Screen>()> makeScreen) {
        return new Button(0, 0, 150, 20, label,
            [this, makeScreen = std::move(makeScreen)] {
                if (makeScreen) m_manager->Push(makeScreen());
            });
    }

    // ═══════════════════════════ VideoSettingsScreen ════════════════════════

    void VideoSettingsScreen::AddOptions() {
        auto& s = Settings();
        auto& mgr = GetScreenManager();

        m_list->AddHeader("Display");

        {
            // Resolution switching isn't supported yet; show vanilla's
            // "Current" state, disabled.
            auto* res = Cycle("Fullscreen Resolution", {"Current"}, 0, nullptr);
            res->active = false;
            m_list->AddBig(res);
        }

        m_list->AddSmall(
            ValueSlider("Max Framerate", s.GetMaxFPS(), 10, 260, 10,
                [](double v) -> std::string {
                    if (v >= 260.0) return "Unlimited";
                    return std::to_string(static_cast<int>(v)) + " fps";
                },
                [&mgr](double v) {
                    Settings().SetMaxFPS(static_cast<int>(v));
                    mgr.MarkSettingApplied(ScreenManager::APPLY_MAX_FPS);
                }),
            OnOff("VSync", s.GetVSync(), [&mgr](bool on) {
                Settings().SetVSync(on);
                mgr.MarkSettingApplied(ScreenManager::APPLY_VSYNC);
            }));

        m_list->AddSmall(
            Cycle("Reduce FPS When", {"Minimized", "AFK"},
                  s.GetInactivityFPSLimit() == "minimized" ? 0 : 1,
                  [](int i) { Settings().SetInactivityFPSLimit(i == 0 ? "minimized" : "afk"); }),
            // Values count in framebuffer pixels (MC semantics) — retina
            // displays legitimately reach 8. Settings larger than the window
            // can fit are clamped by ComputeGuiScale, matching MC.
            Cycle("GUI Scale", {"Auto", "1", "2", "3", "4", "5", "6", "7", "8"},
                  [&s] { int g = s.GetGuiScale(); return (g >= 0 && g <= 8) ? g : 0; }(),
                  [](int i) { Settings().SetGuiScale(i); }));

        m_list->AddSmall(
            OnOff("Fullscreen", s.GetFullscreen(), [&mgr](bool on) {
                Settings().SetFullscreen(on);
                mgr.MarkSettingApplied(ScreenManager::APPLY_FULLSCREEN);
            }),
            ValueSlider("Brightness", s.GetGamma(), 0.0, 1.0, 0.01,
                [](double v) -> std::string {
                    if (v <= 0.0) return "Moody";
                    if (v >= 1.0) return "Bright";
                    return std::to_string(static_cast<int>(std::lround(v * 100.0))) + "%";
                },
                [](double v) { Settings().SetGamma(static_cast<float>(v)); }));

        m_list->AddHeader("Graphics Quality");

        m_list->AddSmall(
            ValueSlider("Render Distance", s.GetRenderDistance(), 2, 32, 1,
                [](double v) { return std::to_string(static_cast<int>(v)) + " chunks"; },
                [&mgr](double v) {
                    Settings().SetRenderDistance(static_cast<int>(v));
                    mgr.MarkSettingApplied(ScreenManager::APPLY_RENDER_DISTANCE);
                }),
            ValueSlider("Simulation Distance", s.GetSimulationDistance(), 5, 32, 1,
                [](double v) { return std::to_string(static_cast<int>(v)) + " chunks"; },
                [](double v) { Settings().SetSimulationDistance(static_cast<int>(v)); }));

        m_list->AddSmall(
            ValueSlider("Biome Blend", s.GetBiomeBlendRadius(), 0, 7, 1,
                [](double v) -> std::string {
                    int r = static_cast<int>(v);
                    if (r == 0) return "OFF";
                    int d = r * 2 + 1;
                    return std::to_string(d) + "x" + std::to_string(d);
                },
                [](double v) { Settings().SetBiomeBlendRadius(static_cast<int>(v)); }),
            Cycle("Graphics", {"Fast", "Fancy"},
                  s.GetGraphicsMode() == 0 ? 0 : 1,
                  [](int i) { Settings().SetGraphicsMode(i); }));

        {
            auto* ao = OnOff("Smooth Lighting", s.GetAO(),
                             [](bool on) { Settings().SetAO(on); });
            ao->SetTooltip({"Applies to newly meshed chunks;", "fully applies after restart."});
            auto* clouds = Cycle("Clouds", {"Fancy", "Fast", "OFF"},
                [&s] {
                    const std::string c = s.GetRenderClouds();
                    if (c == "fast")  return 1;
                    if (c == "false") return 2;
                    return 0;
                }(),
                [](int i) {
                    Settings().SetRenderClouds(i == 0 ? "true" : (i == 1 ? "fast" : "false"));
                });
            m_list->AddSmall(ao, clouds);
        }

        {
            auto* fog = OnOff("Fog", s.GetFogEnabled(),
                              [](bool on) { Settings().SetFogEnabled(on); });
            fog->SetTooltip({"Fades distant terrain into the sky", "like Minecraft. Applies instantly."});
            m_list->AddSmall(fog,
                Cycle("Particles", {"All", "Decreased", "Minimal"},
                      [&s] { int p = s.GetParticles(); return (p >= 0 && p <= 2) ? p : 1; }(),
                      [](int i) { Settings().SetParticles(i); }));
        }

        m_list->AddSmall(
            ValueSlider("Mipmap Levels", s.GetMipmapLevels(), 0, 4, 1,
                [](double v) -> std::string {
                    int m = static_cast<int>(v);
                    return m == 0 ? "OFF" : std::to_string(m);
                },
                [](double v) { Settings().SetMipmapLevels(static_cast<int>(v)); }),
            nullptr);

        m_list->AddSmall(
            OnOff("Entity Shadows", s.GetEntityShadows(),
                  [](bool on) { Settings().SetEntityShadows(on); }),
            ValueSlider("Entity Distance", s.GetEntityDistanceScaling() * 100.0, 50, 500, 25,
                [](double v) { return std::to_string(static_cast<int>(v)) + "%"; },
                [](double v) { Settings().SetEntityDistanceScaling(static_cast<float>(v / 100.0)); }));

        m_list->AddSmall(
            Cycle("Chunk Builder", {"Threaded", "Semi Blocking", "Fully Blocking"},
                  [&s] { int p = s.GetPrioritizeChunkUpdates(); return (p >= 0 && p <= 2) ? p : 0; }(),
                  [](int i) { Settings().SetPrioritizeChunkUpdates(i); }),
            ValueSlider("Cloud Range", s.GetCloudRange(), 32, 512, 32,
                [](double v) { return std::to_string(static_cast<int>(v)) + " blocks"; },
                [](double v) { Settings().SetCloudRange(static_cast<int>(v)); }));

        m_list->AddHeader("Preferences");

        m_list->AddSmall(
            OnOff("Autosave Indicator", s.GetBool("showAutosaveIndicator", true),
                  [](bool on) { Settings().SetBool("showAutosaveIndicator", on); }),
            OnOff("Vignette", s.GetBool("vignette", true),
                  [](bool on) { Settings().SetBool("vignette", on); }));

        m_list->AddSmall(
            Cycle("Attack Indicator", {"OFF", "Crosshair", "Hotbar"},
                  [&s] { int a = s.GetInt("attackIndicator", 1); return (a >= 0 && a <= 2) ? a : 1; }(),
                  [](int i) { Settings().SetInt("attackIndicator", i); }),
            ValueSlider("Menu Background Blur", s.GetInt("menuBackgroundBlurriness", 5), 0, 10, 1,
                [](double v) -> std::string {
                    int b = static_cast<int>(v);
                    return b == 0 ? "OFF" : std::to_string(b);
                },
                [](double v) { Settings().SetInt("menuBackgroundBlurriness", static_cast<int>(v)); }));
    }

    // ═══════════════════════════ WorldSettingsScreen ════════════════════════

    namespace WorldSettingsContext {
        namespace {
            std::string s_worldName;
            bool s_active = false;
            bool s_canPersist = false;
        }
        void Set(const std::string& worldName, bool canPersist) {
            s_worldName = worldName;
            s_active = true;
            s_canPersist = canPersist && !worldName.empty();
        }
        void Clear() {
            s_worldName.clear();
            s_active = false;
            s_canPersist = false;
        }
        bool Active() { return s_active; }
        const std::string& WorldName() { return s_worldName; }
        bool CanPersist() { return s_canPersist; }
    }

    namespace {
        // Write the active world's sky choice back to worlds.json (no-op for
        // multiplayer sessions / worlds not tracked there).
        void PersistWorldSky(const std::string& skybox, int mode) {
            if (!WorldSettingsContext::CanPersist()) return;
            auto worlds = WorldList::Load();
            bool found = false;
            for (auto& entry : worlds) {
                if (entry.name == WorldSettingsContext::WorldName()) {
                    entry.skybox = skybox;
                    entry.skyboxMode = mode;
                    found = true;
                    break;
                }
            }
            if (found) WorldList::Save(worlds);
        }

        void ApplyWorldSky(const std::string& skybox, int mode) {
            g_skyRenderer.SetSkybox(skybox, mode);
            // SetSkybox falls back to vanilla when the set is missing —
            // persist what actually stuck.
            PersistWorldSky(g_skyRenderer.CurrentSkybox(), mode);
        }
    } // namespace

    void WorldSettingsScreen::AddOptions() {
        m_list->AddHeader("Sky");

        // Current skybox's display label for the picker button.
        std::string currentLabel = g_skyRenderer.CurrentSkybox();
        for (const auto& info : DiscoverSkyboxes()) {
            if (info.id == g_skyRenderer.CurrentSkybox()) {
                currentLabel = info.label;
                break;
            }
        }

        auto* skyboxPick = new Button(0, 0, 150, 20, "Skybox: " + currentLabel,
            [this] { m_manager->Push(std::make_unique<SkyboxSelectScreen>()); });
        skyboxPick->SetTooltip({"Custom skyboxes: drop 6 faces named",
                                "panorama_0..5.png into assets/textures/",
                                "environment/skyboxes/<name>/"});

        auto* modeCycle = Cycle("Sky Behavior",
            {"Static", "Darken at Night", "Darken + Sun & Moon"},
            std::clamp(g_skyRenderer.CurrentSkyboxMode(), 0, 2),
            [](int i) {
                ApplyWorldSky(g_skyRenderer.CurrentSkybox(), i);
            });
        modeCycle->SetTooltip({"How a skybox reacts to the day/night",
                               "cycle. The Vanilla sky always uses the",
                               "full cycle."});

        m_list->AddSmall(skyboxPick, modeCycle);
    }

    void SkyboxSelectScreen::AddOptions() {
        const auto skyboxes = DiscoverSkyboxes();
        const std::string current = g_skyRenderer.CurrentSkybox();

        std::vector<AbstractWidget*> row;
        for (const auto& info : skyboxes) {
            const bool selected = info.id == current;
            const std::string label =
                selected ? "> " + info.label + " <" : info.label;
            const std::string id = info.id;
            auto* button = new Button(0, 0, 150, 20, label, [this, id] {
                ApplyWorldSky(id, g_skyRenderer.CurrentSkyboxMode());
                m_manager->Pop();
            });
            row.push_back(button);
            if (row.size() == 2) {
                m_list->AddSmall(row[0], row[1]);
                row.clear();
            }
        }
        if (!row.empty()) {
            m_list->AddSmall(row[0], nullptr);
        }
    }

    // ═══════════════════════════ SoundOptionsScreen ═════════════════════════

    AbstractWidget* SoundOptionsScreen::VolumeSlider(const std::string& caption,
                                                     const char* settingsKey,
                                                     float defaultValue) {
        float initial = Settings().GetFloat(settingsKey, defaultValue);
        std::string key = settingsKey;
        return new SliderButton(0, 0, 150, 20, initial,
            [caption](double v) -> std::string {
                if (v <= 0.0) return caption + ": OFF";
                return FormatPercent(caption, v);
            },
            [key](double v) { Settings().SetFloat(key, static_cast<float>(v)); },
            0.01);
    }

    void SoundOptionsScreen::AddOptions() {
        auto& s = Settings();

        // Master gets vanilla's full-width row, categories pair up below.
        m_list->AddBig(VolumeSlider("Master Volume", "soundCategory_master", 1.0f));
        m_list->AddSmall(VolumeSlider("Music", "soundCategory_music", 1.0f),
                         VolumeSlider("Jukebox/Note Blocks", "soundCategory_record", 1.0f));
        m_list->AddSmall(VolumeSlider("Weather", "soundCategory_weather", 1.0f),
                         VolumeSlider("Blocks", "soundCategory_block", 1.0f));
        m_list->AddSmall(VolumeSlider("Hostile Creatures", "soundCategory_hostile", 1.0f),
                         VolumeSlider("Friendly Creatures", "soundCategory_neutral", 1.0f));
        m_list->AddSmall(VolumeSlider("Players", "soundCategory_player", 1.0f),
                         VolumeSlider("Ambient/Environment", "soundCategory_ambient", 1.0f));
        m_list->AddSmall(VolumeSlider("Voice/Speech", "soundCategory_voice", 1.0f),
                         VolumeSlider("UI", "soundCategory_ui", 1.0f));

        m_list->AddSmall(
            OnOff("Show Subtitles", s.GetShowSubtitles(),
                  [](bool on) { Settings().SetShowSubtitles(on); }),
            OnOff("Directional Audio", s.GetDirectionalAudio(),
                  [](bool on) { Settings().SetDirectionalAudio(on); }));
    }

    // ═══════════════════════════ ControlsScreen ═════════════════════════════

    void ControlsScreen::AddOptions() {
        auto& s = Settings();

        m_list->AddSmall(
            NavButton("Mouse Settings...", [] {
                return std::make_unique<MouseSettingsScreen>();
            }),
            NavButton("Key Binds...", [] {
                return std::make_unique<KeyBindsScreen>();
            }));

        m_list->AddSmall(
            Cycle("Sneak", {"Hold", "Toggle"}, s.GetToggleCrouch() ? 1 : 0,
                  [](int i) { Settings().SetToggleCrouch(i == 1); }),
            Cycle("Sprint", {"Hold", "Toggle"}, s.GetToggleSprint() ? 1 : 0,
                  [](int i) { Settings().SetToggleSprint(i == 1); }));

        m_list->AddSmall(
            OnOff("Auto-Jump", s.GetAutoJump(),
                  [](bool on) { Settings().SetAutoJump(on); }),
            OnOff("Operator Items Tab", s.GetOperatorItemsTab(),
                  [](bool on) { Settings().SetOperatorItemsTab(on); }));
    }

    // ═══════════════════════════ MouseSettingsScreen ════════════════════════

    void MouseSettingsScreen::AddOptions() {
        auto& s = Settings();
        auto& mgr = GetScreenManager();

        // Sensitivity. The stored value is a multiplier on the engine's
        // baseline 0.1°/px (PlatformMain), so the historical feel is 1.0 —
        // shown here as 50%, the middle of a 0..200% scale rather than its
        // top. That leaves 4x headroom above the old maximum (200% = 0.4°/px,
        // roughly MC's 165% setting) for high-DPI/low-DPI extremes, while a
        // 1% step is 0.002°/px, fine enough for low-sens players down at the
        // bottom of the range. Extremes get MC's flavour labels
        // (options.sensitivity.min/max in assets/lang/en_us.json).
        m_list->AddSmall(
            ValueSlider("Sensitivity", s.GetMouseSensitivity() * 50.0, 0.0, 200.0, 1.0,
                [](double pct) -> std::string {
                    if (pct <= 0.0)   return "*yawn*";
                    if (pct >= 200.0) return "HYPERSPEED!!!";
                    char buf[16];
                    std::snprintf(buf, sizeof(buf), "%d%%",
                                  static_cast<int>(std::lround(pct)));
                    return buf;
                },
                [](double pct) {
                    Settings().SetMouseSensitivity(static_cast<float>(pct / 50.0));
                }),
            OnOff("Invert Mouse", s.GetInvertYMouse(),
                  [](bool on) { Settings().SetInvertYMouse(on); }));

        m_list->AddSmall(
            ValueSlider("Scroll Sensitivity", s.GetFloat("mouseWheelSensitivity", 1.0f),
                        0.1, 10.0, 0.1,
                [](double v) {
                    char buf[32];
                    std::snprintf(buf, sizeof(buf), "%.2f", v);
                    return std::string(buf);
                },
                [](double v) { Settings().SetFloat("mouseWheelSensitivity", static_cast<float>(v)); }),
            OnOff("Discrete Scrolling", s.GetDiscreteMouseScroll(),
                  [](bool on) { Settings().SetDiscreteMouseScroll(on); }));

        m_list->AddSmall(
            OnOff("Touchscreen Mode", s.GetTouchscreen(),
                  [](bool on) { Settings().SetTouchscreen(on); }),
            OnOff("Raw Input", s.GetBool("rawMouseInput", false), [&mgr](bool on) {
                Settings().SetBool("rawMouseInput", on);
                mgr.MarkSettingApplied(ScreenManager::APPLY_RAW_MOUSE);
            }));
    }

    // ═══════════════════════════ KeyBindsScreen ═════════════════════════════

    void KeyBindsScreen::AddOptions() {
        m_rows.clear();

        // Grouped by category in registration order, which is the order MC
        // lists them in (Movement, Gameplay, Inventory, Multiplayer, Misc).
        std::string currentCategory;
        for (Input::KeyMapping* m : Input::AllKeyMappings()) {
            if (m->category != currentCategory) {
                currentCategory = m->category;
                m_list->AddHeader(currentCategory);
            }

            auto* label = new StringWidget(0, 0, 150, 20, m->label);
            auto* bind  = new Button(0, 0, 150, 20, "", nullptr);
            // Arm this row for capture. The button's own OnPress fires on
            // mouse-up inside it, which is exactly when we want to start
            // listening — the press that armed it must not also be captured.
            bind->SetOnPress([this, m, bind] {
                m_selected = m;
                RefreshLabels();
            });
            m_list->AddSmall(label, bind);
            m_rows.emplace_back(m, bind);
        }

        // Second footer button beside Done (MC KeyBindsScreen). Init lays the
        // pair out; see OptionsSubScreen::Init.
        m_footerExtraLabel  = "Reset Keys";
        m_footerExtraAction = [this] {
            Input::ResetAllKeyBindings();
            m_selected = nullptr;
            RefreshLabels();
        };

        RefreshLabels();
    }

    void KeyBindsScreen::RefreshLabels() {
        // MC's section sign, as UTF-8. Kept as its own constant because the
        // format code that follows is a hex digit, and writing it inline as
        // "\xC2\xA7f" makes the compiler read \xA7f as one (out-of-range) escape.
        static const std::string SS = "\xC2\xA7";
        for (auto& [mapping, button] : m_rows) {
            const std::string name = mapping->key.DisplayName();
            if (mapping == m_selected) {
                // MC renders the armed row as "> key <" in yellow.
                button->SetMessage(SS + "f> " + SS + "e" + name + " " + SS + "f<");
            } else if (Input::HasBindingConflict(*mapping)) {
                // MC paints a conflicting binding red.
                button->SetMessage(SS + "c" + name);
            } else {
                button->SetMessage(name);
            }
        }
    }

    bool KeyBindsScreen::KeyPressed(int glfwKey, int glfwMods) {
        if (!m_selected) return OptionsSubScreen::KeyPressed(glfwKey, glfwMods);

        // ESC clears the binding rather than assigning ESC — vanilla behaviour,
        // and ESC is not rebindable in MC either.
        Input::SetKeyBinding(*m_selected,
                             glfwKey == GLFW_KEY_ESCAPE
                                 ? Input::BoundKey::Unbound()
                                 : Input::BoundKey::Keyboard(glfwKey));
        m_selected = nullptr;
        RefreshLabels();
        return true;   // swallow, so ESC doesn't also close the screen
    }

    bool KeyBindsScreen::MouseClicked(double mx, double my, int button) {
        if (m_selected) {
            Input::SetKeyBinding(*m_selected, Input::BoundKey::Mouse(button));
            m_selected = nullptr;
            RefreshLabels();
            return true;   // swallow, or this click would also hit a widget
        }
        return OptionsSubScreen::MouseClicked(mx, my, button);
    }

    void KeyBindsScreen::Render(GuiGraphics& g, int mouseX, int mouseY, float partialTick) {
        OptionsSubScreen::Render(g, mouseX, mouseY, partialTick);
        if (m_selected) {
            g.DrawCenteredString("Press a key or mouse button, or Esc to clear",
                                 m_width / 2, HEADER_H - FontRenderer::LINE_HEIGHT - 1,
                                 0xFFFFFF55);
        }
    }

    void KeyBindsScreen::OnClose() {
        // Persist before the base class writes options.txt.
        Input::SaveKeyBindings();
        OptionsSubScreen::OnClose();
    }

    // ═══════════════════════════ ChatOptionsScreen ══════════════════════════

    void ChatOptionsScreen::AddOptions() {
        auto& s = Settings();

        m_list->AddSmall(
            Cycle("Chat", {"Shown", "Commands Only", "Hidden"},
                  [&s] { int v = s.GetChatVisibility(); return (v >= 0 && v <= 2) ? v : 0; }(),
                  [](int i) { Settings().SetChatVisibility(i); }),
            OnOff("Colors", s.GetChatColors(),
                  [](bool on) { Settings().SetChatColors(on); }));

        m_list->AddSmall(
            OnOff("Web Links", s.GetChatLinks(),
                  [](bool on) { Settings().SetChatLinks(on); }),
            OnOff("Prompt on Links", s.GetChatLinksPrompt(),
                  [](bool on) { Settings().SetChatLinksPrompt(on); }));

        m_list->AddSmall(
            ValueSlider("Chat Text Opacity", s.GetChatOpacity(), 0.1, 1.0, 0.01,
                [](double v) { return std::to_string(static_cast<int>(std::lround(v * 100))) + "%"; },
                [](double v) { Settings().SetChatOpacity(static_cast<float>(v)); }),
            ValueSlider("Text Background Opacity", s.GetTextBackgroundOpacity(), 0.0, 1.0, 0.01,
                [](double v) { return std::to_string(static_cast<int>(std::lround(v * 100))) + "%"; },
                [](double v) { Settings().SetTextBackgroundOpacity(static_cast<float>(v)); }));

        m_list->AddSmall(
            ValueSlider("Chat Text Size", s.GetChatScale(), 0.0, 1.0, 0.01,
                [](double v) { return std::to_string(static_cast<int>(std::lround(v * 100))) + "%"; },
                [](double v) { Settings().SetChatScale(static_cast<float>(v)); }),
            ValueSlider("Line Spacing", s.GetChatLineSpacing(), 0.0, 1.0, 0.01,
                [](double v) { return std::to_string(static_cast<int>(std::lround(v * 100))) + "%"; },
                [](double v) { Settings().SetChatLineSpacing(static_cast<float>(v)); }));

        m_list->AddSmall(
            ValueSlider("Chat Delay", s.GetChatDelay(), 0.0, 6.0, 0.1,
                [](double v) -> std::string {
                    if (v <= 0.0) return "None";
                    char buf[32];
                    std::snprintf(buf, sizeof(buf), "%.1f seconds", v);
                    return buf;
                },
                [](double v) { Settings().SetChatDelay(static_cast<float>(v)); }),
            // MC maps the normalized width onto 40..320 px for display.
            ValueSlider("Chat Width", s.GetChatWidth(), 0.0, 1.0, 0.01,
                [](double v) { return std::to_string(static_cast<int>(std::lround(40.0 + v * 280.0))) + "px"; },
                [](double v) { Settings().SetChatWidth(static_cast<float>(v)); }));

        m_list->AddSmall(
            // Heights map onto 20..180 px (MC ChatComponent formulas).
            ValueSlider("Focused Height", s.GetChatHeightFocused(), 0.0, 1.0, 0.01,
                [](double v) { return std::to_string(static_cast<int>(std::lround(20.0 + v * 160.0))) + "px"; },
                [](double v) { Settings().SetChatHeightFocused(static_cast<float>(v)); }),
            ValueSlider("Unfocused Height", s.GetChatHeightUnfocused(), 0.0, 1.0, 0.01,
                [](double v) { return std::to_string(static_cast<int>(std::lround(20.0 + v * 160.0))) + "px"; },
                [](double v) { Settings().SetChatHeightUnfocused(static_cast<float>(v)); }));

        m_list->AddSmall(
            OnOff("Command Suggestions", s.GetAutoSuggestions(),
                  [](bool on) { Settings().SetAutoSuggestions(on); }),
            OnOff("Hide Matched Names", s.GetBool("hideMatchedNames", true),
                  [](bool on) { Settings().SetBool("hideMatchedNames", on); }));

        {
            auto* narrator = Cycle("Narrator", {"OFF"}, 0, nullptr);
            narrator->active = false;
            m_list->AddSmall(
                OnOff("Reduced Debug Info", s.GetReducedDebugInfo(),
                      [](bool on) { Settings().SetReducedDebugInfo(on); }),
                narrator);
        }
    }

    // ═══════════════════════════ SkinCustomizationScreen ════════════════════

    void SkinCustomizationScreen::AddOptions() {
        auto& s = Settings();

        // Model-part toggles are persisted with MC's key names. The player
        // model is currently a stick figure, so they take effect when a
        // full player model lands.
        struct Part { const char* label; const char* key; };
        const Part parts[] = {
            {"Cape",            "modelPart_cape"},
            {"Jacket",          "modelPart_jacket"},
            {"Left Sleeve",     "modelPart_left_sleeve"},
            {"Right Sleeve",    "modelPart_right_sleeve"},
            {"Left Pants Leg",  "modelPart_left_pants_leg"},
            {"Right Pants Leg", "modelPart_right_pants_leg"},
            {"Hat",             "modelPart_hat"},
        };

        std::vector<AbstractWidget*> row;
        for (const auto& p : parts) {
            std::string key = p.key;
            row.push_back(OnOff(p.label, s.GetBool(key, true),
                [key](bool on) { Settings().SetBool(key, on); }));
            if (row.size() == 2) {
                m_list->AddSmall(row[0], row[1]);
                row.clear();
            }
        }

        auto* mainHand = Cycle("Main Hand", {"Left", "Right"},
                               s.GetString("mainHand", "right") == "left" ? 0 : 1,
                               [](int i) { Settings().SetString("mainHand", i == 0 ? "left" : "right"); });
        if (row.empty()) m_list->AddSmall(mainHand, nullptr);
        else             m_list->AddSmall(row[0], mainHand);
    }

    // ═══════════════════════════ AccessibilityOptionsScreen ═════════════════

    void AccessibilityOptionsScreen::AddOptions() {
        auto& s = Settings();

        {
            auto* narrator = Cycle("Narrator", {"OFF"}, 0, nullptr);
            narrator->active = false;
            m_list->AddSmall(
                narrator,
                OnOff("Show Subtitles", s.GetShowSubtitles(),
                      [](bool on) { Settings().SetShowSubtitles(on); }));
        }

        m_list->AddSmall(
            OnOff("High Contrast", s.GetHighContrast(),
                  [](bool on) { Settings().SetHighContrast(on); }),
            OnOff("High Contrast Block Outline", s.GetHighContrastBlockOutline(),
                  [](bool on) { Settings().SetHighContrastBlockOutline(on); }));

        m_list->AddSmall(
            OnOff("Auto-Jump", s.GetAutoJump(),
                  [](bool on) { Settings().SetAutoJump(on); }),
            OnOff("Hide Lightning Flashes", s.GetHideLightningFlashes(),
                  [](bool on) { Settings().SetHideLightningFlashes(on); }));

        m_list->AddSmall(
            Cycle("Sneak", {"Hold", "Toggle"}, s.GetToggleCrouch() ? 1 : 0,
                  [](int i) { Settings().SetToggleCrouch(i == 1); }),
            Cycle("Sprint", {"Hold", "Toggle"}, s.GetToggleSprint() ? 1 : 0,
                  [](int i) { Settings().SetToggleSprint(i == 1); }));

        m_list->AddSmall(
            PercentSlider("Distortion Effects", s.GetScreenEffectScale(),
                          [](float v) { Settings().SetScreenEffectScale(v); }),
            PercentSlider("FOV Effects", s.GetFOVEffectScale(),
                          [](float v) { Settings().SetFOVEffectScale(v); }));

        m_list->AddSmall(
            PercentSlider("Darkness Pulsing", s.GetDarknessEffectScale(),
                          [](float v) { Settings().SetDarknessEffectScale(v); }),
            PercentSlider("Damage Tilt", s.GetDamageTiltStrength(),
                          [](float v) { Settings().SetDamageTiltStrength(v); }));

        m_list->AddSmall(
            PercentSlider("Glint Speed", s.GetGlintSpeed(),
                          [](float v) { Settings().SetGlintSpeed(v); }),
            PercentSlider("Glint Strength", s.GetGlintStrength(),
                          [](float v) { Settings().SetGlintStrength(v); }));

        m_list->AddSmall(
            OnOff("Monochrome Logo", s.GetBool("monochromeLogo", false),
                  [](bool on) { Settings().SetBool("monochromeLogo", on); }),
            OnOff("Hide Splash Texts", s.GetHideSplashTexts(),
                  [](bool on) { Settings().SetHideSplashTexts(on); }));

        m_list->AddSmall(
            ValueSlider("Panorama Scroll Speed", s.GetFloat("panoramaScrollSpeed", 1.0f),
                        0.0, 2.0, 0.1,
                [](double v) {
                    char buf[32];
                    std::snprintf(buf, sizeof(buf), "%.1fx", v);
                    return std::string(buf);
                },
                [](double v) { Settings().SetFloat("panoramaScrollSpeed", static_cast<float>(v)); }),
            ValueSlider("Notification Time", s.GetNotificationDisplayTime(), 0.5, 10.0, 0.5,
                [](double v) {
                    char buf[32];
                    std::snprintf(buf, sizeof(buf), "%.1fx", v);
                    return std::string(buf);
                },
                [](double v) { Settings().SetNotificationDisplayTime(static_cast<float>(v)); }));

        // Panorama version picker — every title-screen panorama MC has
        // shipped, plus "Random" (re-rolled each time the title comes up).
        // Applies live so the change is visible behind this screen.
        {
            auto sets = PanoramaRenderer::AvailableSets();
            if (!sets.empty()) {
                std::vector<std::string> labels;
                labels.emplace_back("Random");
                int current = 0;
                const std::string cur = s.GetString(
                    "panoramaSet", PanoramaRenderer::kDefaultSet);
                for (size_t i = 0; i < sets.size(); ++i) {
                    labels.push_back(sets[i].label);
                    if (cur == sets[i].slug) current = static_cast<int>(i) + 1;
                }
                m_list->AddSmall(
                    Cycle("Panorama", std::move(labels), current,
                          [sets](int i) {
                              const char* slug = (i == 0)
                                  ? Render::PanoramaRenderer::kRandomSet
                                  : sets[i - 1].slug;
                              Settings().SetString("panoramaSet", slug);
                              g_panoramaRenderer.LoadSet(slug);
                          }),
                    nullptr);
            }
        }
    }

    // ═══════════════════════════ LanguageSelectScreen ═══════════════════════

    void LanguageSelectScreen::AddOptions() {
        // Only en_US ships right now (assets/lang/en_us.json). The row shows
        // the vanilla "selected" chevron treatment.
        auto* english = new Button(0, 0, 150, 20, "> English (US) <", nullptr);
        english->active = false;
        m_list->AddBig(english);

        m_list->AddSmall(
            OnOff("Force Unicode Font", Settings().GetForceUnicodeFont(),
                  [](bool on) { Settings().SetForceUnicodeFont(on); }),
            nullptr);
    }

    void LanguageSelectScreen::Render(GuiGraphics& g, int mouseX, int mouseY, float partialTick) {
        OptionsSubScreen::Render(g, mouseX, mouseY, partialTick);
        g.DrawCenteredString("Additional languages require their lang files in assets/lang/.",
                             m_width / 2, m_height - FOOTER_H - 12, 0xFF808080);
    }

    // ═══════════════════════════ CreditsScreen ══════════════════════════════

    void CreditsScreen::Init() {
        AddWidget(new Button(m_width / 2 - 100, m_height - 33 / 2 - 10, 200, 20,
                             "Done", [this] { OnClose(); }));
    }

    void CreditsScreen::Render(GuiGraphics& g, int mouseX, int mouseY, float partialTick) {
        Screen::Render(g, mouseX, mouseY, partialTick);
        g.DrawCenteredString(m_title, m_width / 2, 15, 0xFFFFFFFF);

        const char* lines[] = {
            "MyVoxelGame / ObeyCraft",
            "",
            "A Minecraft-compatible voxel engine written in C++.",
            "",
            "Game assets (textures, models, language files) are",
            "from Minecraft, property of Mojang AB / Microsoft.",
            "For private use only - do not redistribute.",
            "",
            "Built with GLFW, GLM, ImGui, OpenAL, zlib,",
            "stb_image, nlohmann/json, and Boost.Asio.",
        };
        int y = 50;
        for (const char* line : lines) {
            g.DrawCenteredString(line, m_width / 2, y, 0xFFFFFFFF);
            y += FontRenderer::LINE_HEIGHT + 3;
        }
    }

} // namespace Render
