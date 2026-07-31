// File: src/client/renderer/gui/screens/TitleScreen.cpp
#include "TitleScreen.hpp"
#include "PanoramaRenderer.hpp"
#include "OptionsScreens.hpp"
#include "WorldSelectScreens.hpp"
#include "FriendsScreen.hpp"
#include "../GuiGraphics.hpp"
#include "../FontRenderer.hpp"
#include "platform/GameDirectory.hpp"
#include "common/core/Log.hpp"
#include <GLFW/glfw3.h>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <random>
#include <vector>

namespace PlatformMain { std::string GetAssetPath(const std::string& relativePath); }

namespace Render {

    // ── TitleAction plumbing ────────────────────────────────────────────────
    namespace {
        TitleAction s_titleAction;
    }
    void SetTitleAction(TitleAction action) { s_titleAction = std::move(action); }
    TitleAction ConsumeTitleAction() {
        TitleAction out = std::move(s_titleAction);
        s_titleAction = TitleAction{};
        return out;
    }

    // ── Logo / splash assets ────────────────────────────────────────────────
    namespace {
        // Logical layout constants from MC LogoRenderer: the logo is drawn
        // 256×44 out of a 256×64 logical texture (file is a 4× upscale, but
        // normalized UVs make that transparent). The "edition" banner slot
        // below the logo is intentionally unused.
        constexpr int LOGO_W = 256, LOGO_H = 44;
        constexpr float LOGO_V1 = 44.0f / 64.0f;
        constexpr int LOGO_TOP = 30;   // DEFAULT_HEIGHT_OFFSET

        TextureHandle s_logoTexture = INVALID_TEXTURE;

        // Splash pool. MC reads its splashes from a data file; we do the same
        // when the user has dropped one in (assets/texts/splashes.txt, one
        // per line), falling back to a small set of original lines.
        const char* kFallbackSplashes[] = {
            "Now in C++!",
            "Voxels ahoy!",
            "Handcrafted chunks!",
            "20 ticks per second!",
            "Greedy meshing!",
            "Also try ObeyCraft!",
            "Server authoritative!",
            "Frustum culled!",
            "Anvil compatible!",
            "Now with portals!",
        };

        std::string PickSplash() {
            std::vector<std::string> pool;
            std::ifstream in(PlatformMain::GetAssetPath("assets/texts/splashes.txt"));
            if (in.is_open()) {
                std::string line;
                while (std::getline(in, line)) {
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    if (!line.empty()) pool.push_back(line);
                }
            }
            if (pool.empty()) {
                pool.assign(std::begin(kFallbackSplashes), std::end(kFallbackSplashes));
            }
            static std::mt19937 rng(std::random_device{}());
            std::uniform_int_distribution<size_t> dist(0, pool.size() - 1);
            return pool[dist(rng)];
        }
    } // namespace

    // ── TitleScreen ─────────────────────────────────────────────────────────

    TitleScreen::TitleScreen(bool fadeIn)
        : Screen("Title Screen"), m_fading(fadeIn) {
        // MC LogoRenderer: 1-in-10,000 chance of the scrambled logo.
        static std::mt19937 rng(std::random_device{}());
        m_easterEggLogo = std::uniform_real_distribution<float>(0.0f, 1.0f)(rng) < 1.0e-4f;
        m_splash = PickSplash();
    }

    float TitleScreen::CurrentFadeAlpha() const {
        if (!m_fading) return 1.0f;
        if (m_fadeStartSeconds < 0.0) return 0.0f;
        // MC TitleScreen.render: full fade takes 2s; widgets ramp over the
        // second half (clampedMap(fade, 0.5, 1.0, 0, 1)).
        float fade = static_cast<float>((glfwGetTime() - m_fadeStartSeconds) / 2.0);
        if (fade >= 1.0f) return 1.0f;
        float widgetAlpha = (fade - 0.5f) * 2.0f;
        return widgetAlpha < 0.0f ? 0.0f : (widgetAlpha > 1.0f ? 1.0f : widgetAlpha);
    }

    void TitleScreen::Init() {
        if (!m_logoResolved) {
            m_logoResolved = true;
            int w, h;
            const char* logoPath = m_easterEggLogo
                ? "assets/textures/gui/title/minceraft.png"
                : "assets/textures/gui/title/minecraft.png";
            s_logoTexture = LoadStandaloneGuiTexture(logoPath, w, h);
        }

        // MC TitleScreen.init: rows start at height/4 + 48, spaced 24.
        const int rowX  = m_width / 2 - 100;
        int y = m_height / 4 + 48;

        AddWidget(new Button(rowX, y, WidgetDims::BUTTON_WIDTH, WidgetDims::BUTTON_HEIGHT,
            "Singleplayer", [this] {
                m_manager->Push(std::make_unique<SelectWorldScreen>());
            }));
        y += 24;

        AddWidget(new Button(rowX, y, WidgetDims::BUTTON_WIDTH, WidgetDims::BUTTON_HEIGHT,
            "Multiplayer", [this] {
                m_manager->Push(std::make_unique<JoinMultiplayerScreen>());
            }));
        y += 24;

        // Vanilla's Realms slot → Friends (accounts + friends service).
        // Guests (launched without a launcher login) see it disabled.
        {
            const bool loggedIn = Client::g_friendsClient != nullptr;
            Button* friendsBtn = AddWidget(new Button(rowX, y, WidgetDims::BUTTON_WIDTH,
                WidgetDims::BUTTON_HEIGHT, "Friends",
                loggedIn ? Button::OnPress([this] {
                    m_manager->Push(std::make_unique<FriendsScreen>());
                }) : Button::OnPress(nullptr)));
            if (!loggedIn) {
                friendsBtn->active = false;
                friendsBtn->SetTooltip({"Log in from the ObeyCraft",
                                        "launcher to use friends."});
            }
        }
        y += 36;

        AddWidget(new Button(m_width / 2 - 100, y, 98, WidgetDims::BUTTON_HEIGHT,
            "Options...", [this] {
                m_manager->Push(std::make_unique<OptionsScreen>());
            }));
        AddWidget(new Button(m_width / 2 + 2, y, 98, WidgetDims::BUTTON_HEIGHT,
            "Quit Game", [] {
                TitleAction a; a.kind = TitleAction::Kind::Quit;
                SetTitleAction(std::move(a));
            }));

        // The version + copyright lines need font metrics, so they're drawn
        // directly in Render() rather than placed as widgets here.
    }

    void TitleScreen::RenderBackground(GuiGraphics& g, int, int, float) {
        // Skybox is drawn by the host loop (needs the 3D pass before GUI);
        // here we only add the vignette overlay + fade-from-black.
        g_panoramaRenderer.RenderOverlay(g, m_width, m_height, 1.0f);
        if (m_fading) {
            float fade = m_fadeStartSeconds < 0.0
                ? 0.0f
                : static_cast<float>((glfwGetTime() - m_fadeStartSeconds) / 2.0);
            if (fade < 1.0f) {
                float darkness = 1.0f - (fade < 0.0f ? 0.0f : fade);
                g.Fill(0, 0, m_width, m_height,
                       ApplyAlpha(0xFF000000, darkness));
            }
        }
    }

    void TitleScreen::RenderLogo(GuiGraphics& g, float alpha) {
        const uint32_t tint = Platform::g_gameSettings.GetBool("monochromeLogo", false)
            ? ApplyAlpha(0xFF808080, alpha)   // accessibility: monochrome logo
            : ApplyAlpha(0xFFFFFFFF, alpha);
        const int logoX = m_width / 2 - LOGO_W / 2;
        if (s_logoTexture != INVALID_TEXTURE) {
            g.Blit(s_logoTexture, logoX, LOGO_TOP, logoX + LOGO_W, LOGO_TOP + LOGO_H,
                   0.0f, 0.0f, 1.0f, LOGO_V1, tint);
        } else {
            g.DrawCenteredString("M Y V O X E L G A M E", m_width / 2, LOGO_TOP + 16,
                                 ApplyAlpha(0xFFFFFFFF, alpha));
        }
    }

    void TitleScreen::RenderSplash(GuiGraphics& g, float alpha) {
        if (m_splash.empty() || Platform::g_gameSettings.GetHideSplashTexts()) return;
        // MC SplashRenderer: anchored at (width/2 + 123, 69), rotated -20°,
        // scale pulsing at 1Hz, normalized so long lines shrink to fit.
        int textWidth = g.GetStringWidth(m_splash);
        float phase = 1.8f - std::fabs(std::sin(
            static_cast<float>(std::fmod(glfwGetTime(), 1.0)) * 2.0f * 3.14159265f) * 0.1f);
        float scale = phase * 100.0f / static_cast<float>(textWidth + 32);

        g.PushMatrix();
        g.Translate(static_cast<float>(m_width) / 2.0f + 123.0f, 69.0f);
        g.Rotate(-0.34906584f); // -20°
        g.Scale(scale, scale);
        g.DrawCenteredString(m_splash, 0, -FontRenderer::LINE_HEIGHT + 1,
                             ApplyAlpha(0xFFFFFF00, alpha));
        g.PopMatrix();
    }

    void TitleScreen::Render(GuiGraphics& g, int mouseX, int mouseY, float partialTick) {
        if (m_fading && m_fadeStartSeconds < 0.0) m_fadeStartSeconds = glfwGetTime();
        const float alpha = CurrentFadeAlpha();

        // Push the fade into every widget before the base class renders them.
        for (auto& w : m_widgets) w->SetAlpha(alpha);

        // Screen::Render calls our RenderBackground override, then widgets +
        // tooltips.
        Screen::Render(g, mouseX, mouseY, partialTick);
        RenderLogo(g, alpha);
        RenderSplash(g, alpha);

        // Version (bottom-left) and copyright (bottom-right) — MC layout.
        const std::string& version = m_manager ? m_manager->GetVersionString() : std::string();
        if (!version.empty()) {
            g.DrawString(version, 2, m_height - 10, ApplyAlpha(0xFFFFFFFF, alpha));
        }
        const std::string copyright = "Copyright Mojang AB. Do not distribute!";
        int cw = g.GetStringWidth(copyright);
        g.DrawString(copyright, m_width - cw - 2, m_height - 10,
                     ApplyAlpha(0xFFFFFFFF, alpha));
    }

    // ── JoinMultiplayerScreen (direct connect) ──────────────────────────────

    JoinMultiplayerScreen::JoinMultiplayerScreen()
        : Screen("Direct Connection") {
        m_initialAddress = Platform::g_gameSettings.GetLastServer();
    }

    void JoinMultiplayerScreen::Init() {
        // MC DirectJoinServerScreen: address box centered at height/4 area,
        // join/cancel buttons stacked below.
        const int cx = m_width / 2;

        m_addressBox = AddWidget(new EditBox(cx - 100, m_height / 4 + 24,
                                             WidgetDims::BUTTON_WIDTH, WidgetDims::BUTTON_HEIGHT,
                                             "Server Address"));
        m_addressBox->SetMaxLength(128);
        m_addressBox->SetHint("Server Address");
        m_addressBox->SetText(m_initialAddress);
        m_addressBox->SetResponder([this](const std::string& text) {
            if (m_joinButton) m_joinButton->active = !text.empty();
        });
        SetFocus(m_addressBox);

        m_joinButton = AddWidget(new Button(cx - 100, m_height / 4 + 72,
            WidgetDims::BUTTON_WIDTH, WidgetDims::BUTTON_HEIGHT,
            "Join Server", [this] { Join(); }));
        m_joinButton->active = !m_addressBox->GetText().empty();

        AddWidget(new Button(cx - 100, m_height / 4 + 96,
            WidgetDims::BUTTON_WIDTH, WidgetDims::BUTTON_HEIGHT,
            "Cancel", [this] { OnClose(); }));
    }

    void JoinMultiplayerScreen::Join() {
        std::string address = m_addressBox ? m_addressBox->GetText() : std::string();
        if (address.empty()) return;

        Platform::g_gameSettings.SetLastServer(address);
        Platform::g_gameSettings.Save();

        // host[:port] — same parse as the --server CLI flag.
        TitleAction a;
        a.kind = TitleAction::Kind::Multiplayer;
        auto colon = address.rfind(':');
        if (colon != std::string::npos && colon + 1 < address.size()) {
            a.host = address.substr(0, colon);
            int port = std::atoi(address.c_str() + colon + 1);
            a.port = (port > 0 && port <= 65535) ? static_cast<uint16_t>(port) : 25565;
        } else {
            a.host = address;
        }
        if (a.host.empty()) return;
        SetTitleAction(std::move(a));
    }

    bool JoinMultiplayerScreen::KeyPressed(int glfwKey, int glfwMods) {
        // Enter joins from anywhere on the screen (MC DirectJoinServerScreen).
        if (glfwKey == GLFW_KEY_ENTER || glfwKey == GLFW_KEY_KP_ENTER) {
            Join();
            return true;
        }
        return Screen::KeyPressed(glfwKey, glfwMods);
    }

    void JoinMultiplayerScreen::Render(GuiGraphics& g, int mouseX, int mouseY, float partialTick) {
        Screen::Render(g, mouseX, mouseY, partialTick);
        g.DrawCenteredString(m_title, m_width / 2, 20, 0xFFFFFFFF);
        g.DrawString("Server Address", m_width / 2 - 100, m_height / 4 + 12, 0xFFA0A0A0);
    }

} // namespace Render
