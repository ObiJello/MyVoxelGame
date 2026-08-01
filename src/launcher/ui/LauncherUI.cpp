// File: src/launcher/ui/LauncherUI.cpp
//
// Redesigned launcher UI (from the "ObeyCraft Launcher" design doc):
// 800×500 fixed window, 216px left rail (Play / Servers / Settings + account
// card), content views drawn with a mix of ImGui items (inputs, scroll areas)
// and draw-list decoration (cards, chips, pills). All coordinates are design
// pixels — retina is handled by the font DPI trick in LauncherTheme.
#define IMGUI_DEFINE_MATH_OPERATORS
#include "LauncherUI.hpp"
#include "LauncherTheme.hpp"
#include "launcher/LauncherConfig.hpp"
#include "platform/GameDirectory.hpp"
#include "common/entity/PlayerColors.hpp"
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <sstream>
#include <string>

namespace Launcher {

    using namespace Palette;

    namespace {

        // ── Layout constants (design px) ──
        constexpr float kRailW = 216.0f;
        constexpr float kRailPadX = 18.0f;
        constexpr float kRailPadY = 22.0f;
        constexpr float kPadX = 28.0f;   // content horizontal padding
        constexpr float kPadY = 26.0f;   // content vertical padding

        float ContentX()     { return kRailW + kPadX; }
        float ContentW()     { return static_cast<float>(WindowWidth) - kRailW - 2.0f * kPadX; }
        float ContentRight() { return static_cast<float>(WindowWidth) - kPadX; }

        // ── Font helpers ──
        // Fonts are loaded at size*dpiScale with FontGlobalScale = 1/dpiScale,
        // so FontPx() is the on-screen (design px) size for draw-list text.
        ImFont* F(ImFont* f) { return f ? f : ImGui::GetFont(); }
        float FontPx(ImFont* f) {
            return f ? f->FontSize * ImGui::GetIO().FontGlobalScale : ImGui::GetFontSize();
        }
        ImVec2 Measure(ImFont* f, const char* text, float wrapW = 0.0f) {
            return F(f)->CalcTextSizeA(FontPx(f), FLT_MAX, wrapW, text);
        }
        // Snap a position to the device-pixel grid. Fractional glyph origins
        // get smeared by bilinear filtering and read dimmer/greyer than the
        // design's colors, so every text draw goes through this.
        ImVec2 Snap(const ImVec2& p) {
            float s = ImGui::GetIO().DisplayFramebufferScale.y;
            if (s <= 0.0f) s = 1.0f;
            return ImVec2(std::round(p.x * s) / s, std::round(p.y * s) / s);
        }

        // macOS font smoothing (what the design canvas renders with) widens
        // glyph stems — coverage tricks can't reproduce that, so embolden
        // geometrically: overdraw each string offset by a fraction of a pixel
        // in x and y. 0.30px ≈ CoreText's stem darkening at UI sizes.
        constexpr float kEmbolden = 0.30f;

        void Txt(ImDrawList* dl, ImFont* f, const ImVec2& pos, ImU32 col,
                 const char* text, float wrapW = 0.0f) {
            const ImVec2 sp = Snap(pos);
            dl->AddText(F(f), FontPx(f), sp, col, text, nullptr, wrapW);
            dl->AddText(F(f), FontPx(f), ImVec2(sp.x + kEmbolden, sp.y), col,
                        text, nullptr, wrapW);
            dl->AddText(F(f), FontPx(f), ImVec2(sp.x, sp.y + kEmbolden), col,
                        text, nullptr, wrapW);
        }

        // ── Letter-spaced text (the design's tracked small-caps labels) ──
        // ImGui has no tracking, so draw per glyph. ASCII-only labels.
        float MeasureTracked(ImFont* f, const char* text, float tracking) {
            const int n = static_cast<int>(std::strlen(text));
            return Measure(f, text).x + tracking * static_cast<float>(n > 1 ? n - 1 : 0);
        }
        void TxtTracked(ImDrawList* dl, ImFont* f, const ImVec2& pos, ImU32 col,
                        const char* text, float tracking) {
            const float size = FontPx(f);
            const float y = Snap(pos).y;
            char glyph[2] = {0, 0};
            float x = pos.x;
            for (const char* c = text; *c; ++c) {
                glyph[0] = *c;
                const ImVec2 gp = Snap(ImVec2(x, y));
                dl->AddText(F(f), size, gp, col, glyph);
                dl->AddText(F(f), size, ImVec2(gp.x + kEmbolden, gp.y), col, glyph);
                dl->AddText(F(f), size, ImVec2(gp.x, gp.y + kEmbolden), col, glyph);
                x += F(f)->CalcTextSizeA(size, FLT_MAX, 0.0f, glyph).x + tracking;
            }
        }

        // Soft drop-glow behind the primary button — emulates the design's
        // `box-shadow: 0 12px 28px -14px rgba(77,141,255,0.9)`.
        //
        // A CSS box-shadow is the shadow rect (button shrunk by the -14 spread,
        // offset 12 down) convolved with a Gaussian of sigma ≈ blur/2 = 14. Its
        // coverage at signed distance d outside that rect is the blurred-step
        // profile 0.9 * (1/2)erfc(d / (sigma*sqrt2)) — 50% AT the edge, not
        // 100%. Stacked filled rects reproduce it: ring i extends to d_i and
        // carries the coverage increment C(d_{i-1}) - C(d_i), so a point at
        // distance d accumulates ≈ C(d), the same ramp the browser renders.
        void Glow(ImDrawList* dl, const ImVec2& pos, const ImVec2& size,
                  ImU32 col, float rounding) {
            constexpr int kRings = 20;
            constexpr float kSigma = 14.0f;      // blur 28 → sigma ≈ 14
            constexpr float kSpread = -14.0f, kOffsetY = 12.0f;
            constexpr float kCoreAlpha = 230.0f; // rgba(...,0.9)
            auto coverage = [](float d) {
                return 0.5f * std::erfc(d / (kSigma * 1.41421356f));
            };
            const float d0 = -2.0f * kSigma;     // full-coverage core (hidden)
            const float d1 = 2.5f * kSigma;      // glow tail ≈ zero
            float prev = coverage(d0);
            for (int i = 1; i <= kRings; ++i) {
                const float d = d0 + (d1 - d0) * static_cast<float>(i) / kRings;
                const float c = coverage(d);
                const int a = static_cast<int>(kCoreAlpha * (prev - c) + 0.5f);
                prev = c;
                if (a <= 0) continue;
                const float e = kSpread + d;     // expansion relative to the button
                if (size.x + 2.0f * e <= 0.0f || size.y + 2.0f * e <= 0.0f)
                    continue;                    // degenerate ring, fully hidden
                const ImVec2 p0(pos.x - e, pos.y + kOffsetY - e);
                const ImVec2 p1(pos.x + size.x + e, pos.y + kOffsetY + size.y + e);
                const float r = rounding + e;
                dl->AddRectFilled(p0, p1,
                                  (col & ~IM_COL32_A_MASK) |
                                      (static_cast<ImU32>(a) << IM_COL32_A_SHIFT),
                                  r > 2.0f ? r : 2.0f);
            }
        }

        // Scoped PushFont that tolerates null (theme fallback fonts).
        struct Font {
            bool pushed;
            explicit Font(ImFont* f) : pushed(f != nullptr) { if (pushed) ImGui::PushFont(f); }
            ~Font() { if (pushed) ImGui::PopFont(); }
        };

        std::string Ellipsize(ImFont* f, const std::string& s, float maxW) {
            if (Measure(f, s.c_str()).x <= maxW) return s;
            std::string out = s;
            while (!out.empty() && Measure(f, (out + "...").c_str()).x > maxW) out.pop_back();
            return out + "...";
        }

        // ── Widget helpers ──

        // 34×19 pill toggle. Returns true when clicked (caller flips the value).
        bool Toggle(const char* id, bool value) {
            ImVec2 p = ImGui::GetCursorScreenPos();
            bool pressed = ImGui::InvisibleButton(id, ImVec2(34, 19));
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(p, p + ImVec2(34, 19), value ? Accent : SwitchOff, 9.5f);
            float cx = value ? p.x + 34 - 2.5f - 7.0f : p.x + 2.5f + 7.0f;
            dl->AddCircleFilled(ImVec2(cx, p.y + 9.5f), 7.0f, Knob);
            return pressed;
        }

        // Text pill button sized to its label. Returns true when clicked.
        bool Pill(const char* id, const char* label, ImFont* font, ImVec2 pad,
                  ImU32 bg, ImU32 bgHover, ImU32 fg, float rounding,
                  ImU32 border = 0, bool enabled = true, float tracking = 0.0f) {
            ImVec2 ts(tracking > 0.0f ? MeasureTracked(font, label, tracking)
                                      : Measure(font, label).x,
                      Measure(font, label).y);
            ImVec2 size(ts.x + pad.x * 2.0f, ts.y + pad.y * 2.0f);
            ImVec2 p = ImGui::GetCursorScreenPos();
            bool pressed = ImGui::InvisibleButton(id, size) && enabled;
            bool hov = ImGui::IsItemHovered() && enabled;
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImU32 fill = hov && bgHover ? bgHover : bg;
            if ((fill & IM_COL32_A_MASK) != 0) dl->AddRectFilled(p, p + size, fill, rounding);
            if ((border & IM_COL32_A_MASK) != 0) dl->AddRect(p, p + size, border, rounding);
            if (tracking > 0.0f) TxtTracked(dl, font, p + pad, fg, label, tracking);
            else Txt(dl, font, p + pad, fg, label);
            return pressed;
        }

        // Right-aligned status chip (dot + tracked mono label), top edge at `y`.
        void Chip(const char* text, ImU32 bg, ImU32 dot, ImU32 fg, float rightX, float y) {
            constexpr float kTracking = 1.2f;   // 0.12em at 10px
            ImDrawList* dl = ImGui::GetWindowDrawList();
            float textW = MeasureTracked(g_fontMono10Med, text, kTracking);
            float h = Measure(g_fontMono10Med, text).y + 12.0f;
            float w = 11.0f + 6.0f + 7.0f + textW + 11.0f;
            ImVec2 p(rightX - w, y);
            dl->AddRectFilled(p, p + ImVec2(w, h), bg, h * 0.5f);
            dl->AddCircleFilled(ImVec2(p.x + 11.0f + 3.0f, p.y + h * 0.5f), 3.0f, dot);
            TxtTracked(dl, g_fontMono10Med, ImVec2(p.x + 11.0f + 6.0f + 7.0f, p.y + 6.0f),
                       fg, text, kTracking);
        }

        // Full-width primary action button. `progress` in [0,1] draws a fill
        // (used while downloading); pass a negative value for none. `glow`
        // adds the design's blue drop-glow (main Play button only). Default
        // typography is the main button's Bold 15 / 0.1em; the form submit
        // buttons pass Bold 14 / 0.06em per the design.
        bool Primary(const char* id, const char* label, ImVec2 pos, ImVec2 size,
                     bool enabled, float progress = -1.0f, bool glow = false,
                     ImFont* font = nullptr, float tracking = 1.5f) {
            ImFont* f = font ? font : g_fontButton;
            ImGui::SetCursorScreenPos(pos);
            bool pressed = ImGui::InvisibleButton(id, size) && enabled;
            bool hov = ImGui::IsItemHovered() && enabled;
            bool act = ImGui::IsItemActive() && enabled;
            ImDrawList* dl = ImGui::GetWindowDrawList();
            if (glow && enabled) Glow(dl, pos, size, Accent, 11.0f);
            ImU32 bg = !enabled ? BgActive : act ? AccentActive : hov ? AccentHover : Accent;
            dl->AddRectFilled(pos, pos + size, bg, 11.0f);
            if (progress >= 0.0f) {
                float w = size.x * (progress < 1.0f ? progress : 1.0f);
                if (w > 1.0f) {
                    dl->PushClipRect(pos, ImVec2(pos.x + w, pos.y + size.y), true);
                    dl->AddRectFilled(pos, pos + size, Accent, 11.0f);
                    dl->PopClipRect();
                }
            }
            ImVec2 ts(MeasureTracked(f, label, tracking), Measure(f, label).y);
            ImU32 fg = enabled ? OnAccent : (progress >= 0.0f ? TextPrimary : TextFaint);
            TxtTracked(dl, f, pos + (size - ts) * 0.5f, fg, label, tracking);
            return pressed;
        }

        // Styled single-line text input of exact pixel height.
        bool Input(const char* id, const char* hint, char* buf, size_t bufSize,
                   float width, float height, ImFont* font,
                   ImGuiInputTextFlags flags = 0, ImGuiInputTextCallback cb = nullptr,
                   float rounding = 10.0f, float padX = 14.0f) {
            Font f(font);
            float textH = ImGui::GetTextLineHeight();
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                                ImVec2(padX, (height - textH) * 0.5f));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, rounding);
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  ImGui::ColorConvertU32ToFloat4(TextPrimary));
            ImGui::PushStyleColor(ImGuiCol_TextDisabled,
                                  ImGui::ColorConvertU32ToFloat4(TextGhost));
            ImGui::SetNextItemWidth(width);
            bool changed = ImGui::InputTextWithHint(id, hint, buf, bufSize, flags, cb);
            ImGui::PopStyleColor(2);
            ImGui::PopStyleVar(2);
            return changed;
        }

        // Account-name charset filter for InputText — [A-Za-z0-9_] only, the
        // friends service's rule (also keeps the unquoted launch args safe).
        int UsernameCharFilter(ImGuiInputTextCallbackData* data) {
            const ImWchar c = data->EventChar;
            const bool allowed = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                 (c >= '0' && c <= '9') || c == '_';
            return allowed ? 0 : 1;
        }

        const Game::PlayerColorEntry& ColorBySlug(const std::string& slug) {
            if (!slug.empty()) {
                for (const auto& entry : Game::kPlayerColorTable) {
                    if (slug == entry.slug) return entry;
                }
            }
            return Game::kPlayerColorTable[0];
        }

        ImU32 PlayerColorU32(const std::string& slug) {
            const auto& c = ColorBySlug(slug);
            return IM_COL32(c.r, c.g, c.b, 255);
        }

        // ── Date helpers (Howard Hinnant civil-date algorithms) ──
        long DaysFromCivil(int y, unsigned m, unsigned d) {
            y -= m <= 2;
            const long era = (y >= 0 ? y : y - 399) / 400;
            const unsigned yoe = static_cast<unsigned>(y - era * 400);
            const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
            const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
            return era * 146097 + static_cast<long>(doe) - 719468;
        }

        void CivilFromDays(long z, int& y, unsigned& m, unsigned& d) {
            z += 719468;
            const long era = (z >= 0 ? z : z - 146096) / 146097;
            const unsigned doe = static_cast<unsigned>(z - era * 146097);
            const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
            y = static_cast<int>(yoe) + static_cast<int>(era) * 400;
            const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
            const unsigned mp = (5 * doy + 2) / 153;
            d = doy - (153 * mp + 2) / 5 + 1;
            m = mp + (mp < 10 ? 3 : -9);
            y += m <= 2;
        }

        // "2026-07-25T12:00:00Z" → "today" / "1 day ago" / "6 days ago"
        std::string RelativeDate(const std::string& iso) {
            int y = 0, mo = 0, d = 0;
            if (std::sscanf(iso.c_str(), "%d-%d-%d", &y, &mo, &d) != 3) return "";
            long then = DaysFromCivil(y, static_cast<unsigned>(mo), static_cast<unsigned>(d));
            long now = static_cast<long>(std::time(nullptr) / 86400);
            long days = now - then;
            if (days <= 0) return "today";
            if (days == 1) return "1 day ago";
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%ld days ago", days);
            return buf;
        }

        // epoch seconds → "JUN 2026"
        std::string MonthYear(int64_t epoch) {
            static const char* kMonths[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                            "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
            int y = 0; unsigned m = 1, d = 1;
            CivilFromDays(static_cast<long>(epoch / 86400), y, m, d);
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%s %d", kMonths[(m - 1) % 12], y);
            return buf;
        }

        // Release-notes body → bullet lines (strips markdown headers/markers).
        void ParseNotes(const std::string& src, std::vector<std::string>& out) {
            out.clear();
            std::istringstream ss(src);
            std::string line;
            while (std::getline(ss, line)) {
                while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                    line.pop_back();
                size_t i = line.find_first_not_of(" \t");
                if (i == std::string::npos) continue;
                line = line.substr(i);
                if (line[0] == '#') continue;
                if ((line[0] == '-' || line[0] == '*' || line[0] == '+') &&
                    line.size() > 1 && line[1] == ' ')
                    line = line.substr(2);
                if (!line.empty()) out.push_back(line);
            }
        }

        bool CanLaunch(const LauncherUIState& state) {
            return state.gameInstalled && !state.launcherUpdateReady &&
                   (state.state == LauncherState::ReadyToPlay ||
                    state.state == LauncherState::UpdateAvailable);
        }

        // Validate a port text field → 0 on failure.
        uint16_t ParsePort(const char* text) {
            char* end = nullptr;
            long v = std::strtol(text, &end, 10);
            if (end == text || *end != '\0' || v < 1 || v > 65535) return 0;
            return static_cast<uint16_t>(v);
        }

    } // namespace

    void LauncherUI::SetLogoTexture(GLuint textureId, int width, int height) {
        m_logoTexture = textureId;
        m_logoWidth = width;
        m_logoHeight = height;
    }

    void LauncherUI::ClearPasswordBuffers() {
        std::memset(m_password, 0, sizeof(m_password));
        std::memset(m_pwCurrent, 0, sizeof(m_pwCurrent));
        std::memset(m_pwNew, 0, sizeof(m_pwNew));
    }

    void LauncherUI::SyncAccountPane(LauncherUIState& state) {
        const bool loggedIn = !state.sessionToken.empty();
        if (!m_acctPaneInit) {
            m_acctPane = loggedIn ? AccountPane::In : AccountPane::Out;
            m_acctPaneInit = true;
        }
        // Login/signup completed in the app → land on the account card.
        if (loggedIn && (m_acctPane == AccountPane::SignIn || m_acctPane == AccountPane::SignUp)) {
            m_acctPane = AccountPane::In;
            ClearPasswordBuffers();
        }
        // Session gone (logout) → back to the signed-out card.
        if (!loggedIn && (m_acctPane == AccountPane::In || m_acctPane == AccountPane::ChangePw)) {
            m_acctPane = AccountPane::Out;
        }
        if (state.pwChangeDone) {
            state.pwChangeDone = false;
            if (m_acctPane == AccountPane::ChangePw) {
                m_acctPane = AccountPane::In;
                ClearPasswordBuffers();
            }
        }
    }

    void LauncherUI::Render(LauncherUIState& state) {
        SyncAccountPane(state);

        ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoScrollWithMouse;

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(static_cast<float>(WindowWidth),
                                        static_cast<float>(WindowHeight)));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::Begin("##launcher", nullptr, flags);
        ImGui::PopStyleVar();

        DrawRail(state);

        switch (m_view) {
            case View::Play:     DrawPlayView(state); break;
            case View::Servers:  DrawServersView(state); break;
            case View::Settings: DrawSettingsView(state); break;
        }

        // One ping refresh per visit to the Servers view.
        if (m_view == View::Servers) {
            if (!m_pingedOnOpen) {
                m_pingedOnOpen = true;
                if (m_onPingServers) m_onPingServers();
            }
        } else {
            m_pingedOnOpen = false;
        }

        ImGui::End();
    }

    // ═════════════════════════════════ Rail ═════════════════════════════════

    void LauncherUI::DrawRail(LauncherUIState& state) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float h = static_cast<float>(WindowHeight);

        dl->AddRectFilled(ImVec2(0, 0), ImVec2(kRailW, h), Rail);
        dl->AddLine(ImVec2(kRailW - 0.5f, 0), ImVec2(kRailW - 0.5f, h), Border);

        // ── Logo + wordmark ──
        {
            // Text column (12px bold line + 3 gap + 9.5px mono line = 28.6px)
            // is taller than the 26px logo, so the logo centers against it.
            // Snapped to the device grid — half-pixel image rects smear.
            ImVec2 p = Snap(ImVec2(kRailPadX, kRailPadY + 1.3f));
            if (m_logoTexture != 0) {
                dl->AddImageRounded(static_cast<ImTextureID>(static_cast<uintptr_t>(m_logoTexture)),
                                    p, p + ImVec2(26, 26), ImVec2(0, 0), ImVec2(1, 1),
                                    IM_COL32_WHITE, 7.0f);
            } else {
                dl->AddRectFilled(p, p + ImVec2(26, 26), BgActive, 7.0f);
                Txt(dl, g_fontBody, p + ImVec2(8, 5), TextPrimary, "O");
            }
            TxtTracked(dl, g_fontWordmark, ImVec2(kRailPadX + 36, kRailPadY),
                       TextPrimary, "OBEYCRAFT", 1.92f);
            char sub[48];
            std::snprintf(sub, sizeof(sub), "LAUNCHER %s", LauncherVersion);
            TxtTracked(dl, g_fontMono95, ImVec2(kRailPadX + 36, kRailPadY + 16.1f),
                       TextFaint, sub, 0.95f);
        }

        // ── Nav items ──
        const float navX = kRailPadX;
        const float navW = kRailW - 2.0f * kRailPadX;
        // Logo row is 28.6px tall (12px title line + 3 gap + 9.5px sub line),
        // nav starts 26px below it per the design.
        float navY = kRailPadY + 28.6f + 26.0f;

        struct NavDef { const char* label; View view; };
        const NavDef items[] = {
            { "Play",     View::Play },
            { "Servers",  View::Servers },
            { "Settings", View::Settings },
        };

        for (const NavDef& item : items) {
            ImVec2 p(navX, navY);
            ImVec2 size(navW, 38);
            ImGui::SetCursorScreenPos(p);
            ImGui::PushID(item.label);
            bool clicked = ImGui::InvisibleButton("nav", size);
            bool hovered = ImGui::IsItemHovered();
            ImGui::PopID();

            const bool active = (m_view == item.view);
            if (active) {
                dl->AddRectFilled(p, p + size, BgActive, 9.0f);
                dl->AddRectFilled(ImVec2(navX, navY + 11), ImVec2(navX + 3, navY + 27),
                                  Accent, 3.0f, ImDrawFlags_RoundCornersRight);
            } else if (hovered) {
                dl->AddRectFilled(p, p + size, BgHover, 9.0f);
            }

            float textH = Measure(g_fontBodyMed, item.label).y;
            Txt(dl, g_fontBodyMed, ImVec2(navX + 12, navY + (38 - textH) * 0.5f),
                TextBody, item.label);

            if (item.view == View::Play && state.launcherUpdateReady) {
                // Amber dot: a launcher restart is pending.
                dl->AddCircleFilled(ImVec2(navX + navW - 12 - 3, navY + 19), 3.0f, Amber);
            }
            if (item.view == View::Servers && !state.servers.empty()) {
                char badge[16];
                std::snprintf(badge, sizeof(badge), "%d",
                              static_cast<int>(state.servers.size()));
                ImVec2 bs = Measure(g_fontMono10, badge);
                Txt(dl, g_fontMono10,
                    ImVec2(navX + navW - 12 - bs.x, navY + (38 - bs.y) * 0.5f),
                    TextFaint, badge);
            }

            if (clicked) m_view = item.view;
            navY += 38 + 3;
        }

        // ── Account card (bottom) — click opens Settings / General ──
        {
            const float cardH = 52.0f;
            ImVec2 p(kRailPadX, h - kRailPadY - cardH);
            ImVec2 size(navW, cardH);
            ImGui::SetCursorScreenPos(p);
            bool clicked = ImGui::InvisibleButton("##railAccount", size);
            bool hovered = ImGui::IsItemHovered();
            if (hovered) dl->AddRectFilled(p, p + size, BgHover, 11.0f);
            dl->AddRect(p, p + size, Border, 11.0f);

            dl->AddRectFilled(p + ImVec2(10, 10), p + ImVec2(10 + 32, 10 + 32),
                              PlayerColorU32(state.playerColor), 9.0f);

            const bool loggedIn = !state.sessionToken.empty();
            std::string name = !state.playerName.empty() ? state.playerName
                             : (loggedIn ? state.accountName : std::string("guest"));
            name = Ellipsize(g_fontName13, name, navW - 52 - 10);
            Txt(dl, g_fontName13, p + ImVec2(52, 11), TextPrimary, name.c_str());
            TxtTracked(dl, g_fontMono95, p + ImVec2(52, 28),
                       loggedIn ? TextSynced : TextFaint,
                       loggedIn ? "SYNCED" : "LOCAL PROFILE", 0.76f);

            if (clicked) {
                m_view = View::Settings;
                m_tab = SettingsTab::General;
            }
        }
    }

    // ═══════════════════════════════ Play view ═══════════════════════════════

    void LauncherUI::DrawPlayView(LauncherUIState& state) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float x0 = ContentX();
        const float x1 = ContentRight();
        const bool restart = state.launcherUpdateReady;

        // ── Header: build label + big version, status chip on the right ──
        const char* verLabel = restart ? "LAUNCHER UPDATE"
                             : state.gameInstalled ? "INSTALLED BUILD" : "LATEST BUILD";
        std::string verBig = restart ? state.launcherNewVersion
                           : state.gameInstalled ? state.installedVersion
                           : (!state.latestVersion.empty() ? state.latestVersion
                                                           : std::string("--"));
        TxtTracked(dl, g_fontMono10, ImVec2(x0, kPadY), TextFaint, verLabel, 2.0f);
        // Display number: the design sets line-height:1, so pull the glyph box up
        // by half the extra line height to land the glyphs where the doc has them.
        TxtTracked(dl, g_fontDisplay,
            ImVec2(x0 - 2.0f, kPadY + 19.2f - (Measure(g_fontDisplay, "0").y - 42.0f) * 0.5f),
            TextPrimary, verBig.c_str(), -0.84f);

        {
            std::string chipText;
            ImU32 chipBg = GreyChipBg, chipDot = TextMuted, chipFg = TextMuted;
            switch (state.state) {
                case LauncherState::ReadyToPlay:
                    chipText = "READY"; chipBg = GreenBg; chipDot = Green; chipFg = GreenFg;
                    break;
                case LauncherState::UpdateAvailable:
                    chipText = "UPDATE " + state.latestVersion;
                    chipBg = BlueChipBg; chipDot = Accent; chipFg = AccentSoft;
                    break;
                case LauncherState::Downloading:
                    chipText = "DOWNLOADING"; chipBg = BlueChipBg; chipDot = Accent; chipFg = AccentSoft;
                    break;
                case LauncherState::Installing:
                    chipText = "INSTALLING"; chipBg = BlueChipBg; chipDot = Accent; chipFg = AccentSoft;
                    break;
                case LauncherState::LaunchingGame:
                    chipText = "LAUNCHING"; chipBg = GreenBg; chipDot = Green; chipFg = GreenFg;
                    break;
                case LauncherState::Error:
                    chipText = "ERROR"; chipBg = RedBg; chipDot = Red; chipFg = RedFg;
                    break;
                case LauncherState::Initializing:
                case LauncherState::CheckingForUpdates:
                    chipText = "CHECKING";
                    break;
            }
            if (restart) {
                chipText = "RESTART NEEDED";
                chipBg = AmberChipBg; chipDot = Amber; chipFg = AmberChipFg;
            }
            Chip(chipText.c_str(), chipBg, chipDot, chipFg, x1, kPadY + 4);
        }

        float y = kPadY + 61.2f;   // header block: 10px label line + 6 gap + 42 display

        // ── Banner slot: launcher-restart notice, or error detail ──
        if (restart) {
            char meta[64];
            std::snprintf(meta, sizeof(meta), "%s -> %s",
                          LauncherVersion, state.launcherNewVersion.c_str());
            float metaW = Measure(g_fontMono10, meta).x;
            char msg[160];
            std::snprintf(msg, sizeof(msg),
                          "Launcher %s is installed and waiting. Restarting takes a "
                          "second and keeps your settings.",
                          state.launcherNewVersion.c_str());
            float textW = (x1 - x0) - 14 - 6 - 11 - 11 - metaW - 14;
            float textH = Measure(g_fontSmall, msg, textW).y;
            float bh = textH + 24.0f;
            y += 18.0f;
            dl->AddRectFilled(ImVec2(x0, y), ImVec2(x1, y + bh), AmberBg, 10.0f);
            dl->AddRect(ImVec2(x0, y), ImVec2(x1, y + bh), AmberBorder, 10.0f);
            dl->AddCircleFilled(ImVec2(x0 + 14 + 3, y + bh * 0.5f), 3.0f, Amber);
            Txt(dl, g_fontSmall, ImVec2(x0 + 14 + 6 + 11, y + 12), AmberText, msg, textW);
            Txt(dl, g_fontMono10, ImVec2(x1 - 14 - metaW, y + (bh - 12) * 0.5f),
                AmberMeta, meta);
            y += bh;
        } else if (state.state == LauncherState::Error) {
            const std::string& msg = !state.errorText.empty() ? state.errorText
                                                              : state.statusText;
            float textW = (x1 - x0) - 14 - 6 - 11 - 14;
            float textH = Measure(g_fontSmall, msg.c_str(), textW).y;
            float bh = textH + 24.0f;
            y += 18.0f;
            dl->AddRectFilled(ImVec2(x0, y), ImVec2(x1, y + bh), RedBg, 10.0f);
            dl->AddCircleFilled(ImVec2(x0 + 14 + 3, y + bh * 0.5f), 3.0f, Red);
            Txt(dl, g_fontSmall, ImVec2(x0 + 14 + 6 + 11, y + 12), RedFg,
                msg.c_str(), textW);
            y += bh;
        }

        // ── Divider + notes header ──
        y += 20.0f;
        dl->AddLine(ImVec2(x0, y), ImVec2(x1, y), Border);
        y += 20.0f;

        TxtTracked(dl, g_fontMono10, ImVec2(x0, y), TextFaint, "RELEASE NOTES", 2.0f);
        {
            std::string meta;
            if (restart) {
                meta = "LAUNCHER " + state.launcherNewVersion;
            } else if (!state.latestVersion.empty()) {
                meta = state.latestVersion;
                std::string rel = RelativeDate(state.publishedAt);
                if (!rel.empty()) meta += " - " + rel;
            }
            if (!meta.empty()) {
                float mw = Measure(g_fontMono10, meta.c_str()).x;
                Txt(dl, g_fontMono10, ImVec2(x1 - mw, y), TextFaint, meta.c_str());
            }
        }
        y += 13.2f + 14.0f;

        // ── Bottom chrome geometry (fixed) ──
        const float btnH = 54.0f;
        const float btnY = static_cast<float>(WindowHeight) - kPadY - btnH;
        const float ctrlH = 19.0f;
        const float ctrlY = btnY - 12.0f - ctrlH;
        const float notesBottom = ctrlY - 16.0f;

        // ── Release notes (scrollable) ──
        {
            const std::string& src = restart ? state.launcherChangelog : state.changelog;
            if (src != m_notesSource) {
                m_notesSource = src;
                ParseNotes(src, m_notesBullets);
            }

            ImGui::SetCursorScreenPos(ImVec2(x0, y));
            ImGui::BeginChild("##notes", ImVec2(x1 - x0, notesBottom - y), ImGuiChildFlags_None,
                              ImGuiWindowFlags_NoBackground);
            ImDrawList* cdl = ImGui::GetWindowDrawList();
            const ImU32 bulletCol = restart ? Amber : Accent;
            if (m_notesBullets.empty()) {
                const char* placeholder =
                    (state.state == LauncherState::CheckingForUpdates ||
                     state.state == LauncherState::Initializing)
                        ? "Checking for updates..."
                        : "No release notes for this build.";
                ImVec2 p = ImGui::GetCursorScreenPos();
                Txt(cdl, g_fontSmall, p, TextFaint, placeholder);
                ImGui::Dummy(ImVec2(0, 20));
            } else {
                const float wrapW = (x1 - x0) - 15.0f - 8.0f;
                for (size_t i = 0; i < m_notesBullets.size(); ++i) {
                    ImVec2 p = ImGui::GetCursorScreenPos();
                    float th = Measure(g_fontBody, m_notesBullets[i].c_str(), wrapW).y;
                    cdl->AddCircleFilled(ImVec2(p.x + 2, p.y + 9), 2.0f, bulletCol);
                    Txt(cdl, g_fontBody, ImVec2(p.x + 15, p.y), TextNotes,
                        m_notesBullets[i].c_str(), wrapW);
                    ImGui::Dummy(ImVec2(x1 - x0 - 8, th));
                    if (i + 1 < m_notesBullets.size()) ImGui::Dummy(ImVec2(0, 3));
                }
            }
            ImGui::EndChild();
        }

        // ── Controls row: Vulkan toggle (or restart note) + asset meta ──
        if (restart) {
            char keep[64];
            std::snprintf(keep, sizeof(keep), "Game %s stays installed.",
                          state.installedVersion.c_str());
            Txt(dl, g_fontSmall, ImVec2(x0, ctrlY + 2), TextMuted, keep);
        } else {
            ImGui::SetCursorScreenPos(ImVec2(x0, ctrlY));
            if (Toggle("##vulkanPlay", state.useVulkan)) state.useVulkan = !state.useVulkan;
            Txt(dl, g_fontSmall, ImVec2(x0 + 34 + 9, ctrlY + 2), TextMuted, "Vulkan renderer");
        }
        {
            const std::string& meta = restart ? state.launcherAssetMeta : state.gameAssetMeta;
            if (!meta.empty()) {
                float mw = Measure(g_fontMono10, meta.c_str()).x;
                Txt(dl, g_fontMono10, ImVec2(x1 - mw, ctrlY + 4), TextFaint, meta.c_str());
            }
        }

        // ── Primary button ──
        {
            std::string label = "PLAY";
            bool enabled = true;
            float progress = -1.0f;
            ActionCallback* action = &m_onPlay;

            if (restart) {
                label = "RESTART LAUNCHER";
                action = &m_onRestart;
            } else switch (state.state) {
                case LauncherState::Initializing:
                case LauncherState::CheckingForUpdates:
                    label = "CHECKING..."; enabled = false; break;
                case LauncherState::ReadyToPlay:
                    label = "PLAY"; action = &m_onPlay; break;
                case LauncherState::UpdateAvailable:
                    label = (state.gameInstalled ? "UPDATE " : "INSTALL ") + state.latestVersion;
                    action = &m_onUpdate; break;
                case LauncherState::Downloading: {
                    enabled = false;
                    progress = state.downloadProgress.load();
                    char buf[64];
                    std::snprintf(buf, sizeof(buf), "DOWNLOADING %d%%",
                                  static_cast<int>(progress * 100.0f));
                    label = buf;
                    break;
                }
                case LauncherState::Installing:
                    label = "INSTALLING..."; enabled = false; progress = 1.0f; break;
                case LauncherState::LaunchingGame:
                    label = "LAUNCHING..."; enabled = false; break;
                case LauncherState::Error:
                    label = "RETRY"; action = &m_onRetry; break;
            }

            if (Primary("##primary", label.c_str(), ImVec2(x0, btnY),
                        ImVec2(x1 - x0, btnH), enabled, progress, /*glow=*/true)) {
                if (*action) (*action)();
            }
        }
    }

    // ══════════════════════════════ Servers view ══════════════════════════════

    void LauncherUI::DrawServersView(LauncherUIState& state) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float x0 = ContentX();
        const float x1 = ContentRight();
        const bool canJoin = CanLaunch(state);

        // Seed quick-connect fields from the last join (once per session).
        if (!m_quickSeeded) {
            std::snprintf(m_quickHost, sizeof(m_quickHost), "%s", state.lastJoinIP.c_str());
            if (!state.lastJoinPort.empty()) {
                std::snprintf(m_quickPort, sizeof(m_quickPort), "%s", state.lastJoinPort.c_str());
            }
            m_quickSeeded = true;
        }

        // ── Header ──
        // Header column: 10px label line (13.2) + 6px gap + 22px title.
        TxtTracked(dl, g_fontMono10, ImVec2(x0, kPadY), TextFaint, "SAVED SERVERS", 2.0f);
        Txt(dl, g_fontH2, ImVec2(x0, kPadY + 19.2f), TextPrimary, "Where to");

        {
            // CSS content-box: 1px border + 9/14 padding → text inset (15,10),
            // total height 33.6; centered against the 43.1px header column.
            ImVec2 ts = Measure(g_fontSmall, "Add server");
            ImVec2 size(ts.x + 30, ts.y + 20);
            ImGui::SetCursorScreenPos(ImVec2(x1 - size.x, kPadY + 4.8f));
            if (Pill("##addServer", "Add server", g_fontSmall, ImVec2(15, 10),
                     0, BgActive, TextBody, 9.0f, BorderBtn)) {
                m_addingServer = !m_addingServer;
                if (m_addingServer) {
                    // Seed the form from the quick-connect fields for convenience.
                    if (m_addHost[0] == '\0' && m_quickHost[0] != '\0') {
                        std::snprintf(m_addHost, sizeof(m_addHost), "%s", m_quickHost);
                        std::snprintf(m_addPort, sizeof(m_addPort), "%s", m_quickPort);
                    }
                }
            }
        }

        float listY = kPadY + 43.1f + 20.0f;   // header column + 20px margin

        // ── Inline add-server form ──
        if (m_addingServer) {
            const float rowH = 44.0f;
            float xx = x0;
            ImGui::SetCursorScreenPos(ImVec2(xx, listY));
            Input("##addName", "name", m_addName, sizeof(m_addName), 150, rowH, g_fontSmall);
            xx += 150 + 8;
            ImGui::SetCursorScreenPos(ImVec2(xx, listY));
            Input("##addHost", "host or ip", m_addHost, sizeof(m_addHost),
                  (x1 - x0) - 150 - 8 - 78 - 8 - 74 - 8 - 74 - 8, rowH, g_fontMono12);
            xx = x1 - 78 - 8 - 74 - 8 - 74;
            ImGui::SetCursorScreenPos(ImVec2(xx, listY));
            Input("##addPort", "port", m_addPort, sizeof(m_addPort), 78, rowH, g_fontMono12);
            xx += 78 + 8;

            const uint16_t port = ParsePort(m_addPort);
            const bool valid = m_addHost[0] != '\0' && port != 0;
            ImGui::SetCursorScreenPos(ImVec2(xx, listY + 5));
            if (Pill("##addSave", "SAVE", g_fontSmall, ImVec2(20, 8),
                     valid ? Accent : BgActive, valid ? AccentHover : 0,
                     valid ? OnAccent : TextFaint, 9.0f, 0, valid) && valid) {
                SavedServer entry;
                entry.name = m_addName[0] != '\0' ? m_addName : m_addHost;
                entry.host = m_addHost;
                entry.port = port;
                state.servers.push_back(std::move(entry));
                state.serversDirty = true;
                m_addingServer = false;
                m_addName[0] = m_addHost[0] = '\0';
            }
            xx += Measure(g_fontSmall, "SAVE").x + 40 + 8;
            ImGui::SetCursorScreenPos(ImVec2(xx, listY + 5));
            if (Pill("##addCancel", "CANCEL", g_fontSmall, ImVec2(12, 8),
                     0, BgActive, TextMuted, 9.0f)) {
                m_addingServer = false;
            }
            listY += rowH + 10;
        }

        // ── Quick-connect geometry (fixed at the bottom) ──
        // The host box is CSS content-box (44 + 2 border = 46 outer); the port
        // box and CONNECT are 44 and center against it.
        const float qcH = 46.0f;
        const float qcY = static_cast<float>(WindowHeight) - kPadY - qcH;
        const float listBottom = qcY - 16.0f;

        // ── Server list ──
        ImGui::SetCursorScreenPos(ImVec2(x0, listY));
        ImGui::BeginChild("##serverList", ImVec2(x1 - x0, listBottom - listY),
                          ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
        {
            ImDrawList* cdl = ImGui::GetWindowDrawList();
            int removeIndex = -1;
            const float rowW = ImGui::GetContentRegionAvail().x;

            if (state.servers.empty()) {
                ImGui::Dummy(ImVec2(0, 24));
                ImVec2 p = ImGui::GetCursorScreenPos();
                const char* line1 = "No saved servers yet.";
                const char* line2 = "Use Add server, or connect below to get going.";
                float w1 = Measure(g_fontSmall, line1).x;
                float w2 = Measure(g_fontSmall, line2).x;
                Txt(cdl, g_fontSmall, ImVec2(p.x + (rowW - w1) * 0.5f, p.y), TextMuted, line1);
                Txt(cdl, g_fontSmall, ImVec2(p.x + (rowW - w2) * 0.5f, p.y + 20), TextFaint, line2);
                ImGui::Dummy(ImVec2(0, 44));
            }

            for (size_t i = 0; i < state.servers.size(); ++i) {
                SavedServer& sv = state.servers[i];
                const bool online = sv.pingMs >= 0;
                const bool pending = sv.pingMs == PingPending;
                const bool joinable = canJoin && !pending && online;

                ImGui::PushID(static_cast<int>(i));
                ImVec2 p = ImGui::GetCursorScreenPos();
                // CSS content-box: 58px + 1px border each side = 60px outer.
                ImVec2 size(rowW, 60);
                ImGui::SetNextItemAllowOverlap();
                bool rowClicked = ImGui::InvisibleButton("##row", size);
                bool rowHovered = ImGui::IsItemHovered();

                cdl->AddRectFilled(p, p + size, Rail, 11.0f);
                cdl->AddRect(p, p + size, rowHovered && joinable ? BorderHover : Border, 11.0f);

                // Status dot (content inset = border 1 + padding 14 = 15)
                cdl->AddCircleFilled(ImVec2(p.x + 15 + 3.5f, p.y + 30), 3.5f,
                                     online ? Green : pending ? TextFaint : TextGhost);

                // Name + address column (14.7 + 4 gap + 13.9, centered in 60)
                float tx = p.x + 15 + 7 + 14;
                std::string name = Ellipsize(g_fontBodySemi, sv.name, rowW - 260);
                Txt(cdl, g_fontBodySemi, ImVec2(tx, p.y + 13.7f),
                    online || pending ? TextPrimary : TextMuted, name.c_str());
                char addr[96];
                std::snprintf(addr, sizeof(addr), "%s:%u", sv.host.c_str(),
                              static_cast<unsigned>(sv.port));
                Txt(cdl, g_fontMono105, ImVec2(tx, p.y + 32.4f), TextFaint, addr);

                // JOIN pill (right)
                constexpr float kJoinTracking = 0.92f;   // 0.08em at 11.5px
                ImVec2 joinTs(MeasureTracked(g_fontJoin, "JOIN", kJoinTracking),
                              Measure(g_fontJoin, "JOIN").y);
                ImVec2 joinSize(joinTs.x + 24, joinTs.y + 14);
                ImVec2 joinPos(p.x + size.x - 15 - joinSize.x,
                               p.y + (size.y - joinSize.y) * 0.5f);
                ImGui::SetCursorScreenPos(joinPos);
                bool joinClicked = ImGui::InvisibleButton("##join", joinSize) && joinable;
                bool joinHovered = ImGui::IsItemHovered() && joinable;
                cdl->AddRectFilled(joinPos, joinPos + joinSize,
                                   !joinable ? BgOffline : joinHovered ? BgActiveHov : BgActive,
                                   8.0f);
                TxtTracked(cdl, g_fontJoin, joinPos + ImVec2(12, 7),
                           joinable ? AccentSoft : TextGhost, "JOIN", kJoinTracking);

                // Ping (left of JOIN)
                {
                    char ping[24];
                    if (online) std::snprintf(ping, sizeof(ping), "%d ms", sv.pingMs);
                    else std::snprintf(ping, sizeof(ping), "%s", pending ? "..." : "offline");
                    ImVec2 ps = Measure(g_fontMono105, ping);
                    Txt(cdl, g_fontMono105,
                        ImVec2(joinPos.x - 14 - ps.x, p.y + (size.y - ps.y) * 0.5f),
                        online ? TextMuted : TextFaint, ping);
                }

                // Remove cross — only while the row is hovered.
                if (rowHovered) {
                    ImVec2 rmSize(20, 20);
                    ImVec2 rmPos(joinPos.x - 14 - 60 - 14 - rmSize.x,
                                 p.y + (size.y - rmSize.y) * 0.5f);
                    ImGui::SetCursorScreenPos(rmPos);
                    if (ImGui::InvisibleButton("##remove", rmSize)) removeIndex = static_cast<int>(i);
                    ImU32 rmCol = ImGui::IsItemHovered() ? TextBody : TextGhost;
                    ImVec2 c = rmPos + ImVec2(10, 10);
                    cdl->AddLine(c + ImVec2(-4, -4), c + ImVec2(4, 4), rmCol, 1.5f);
                    cdl->AddLine(c + ImVec2(4, -4), c + ImVec2(-4, 4), rmCol, 1.5f);
                    if (ImGui::IsItemHovered() && ImGui::BeginTooltip()) {
                        {
                            // Pop the font BEFORE EndTooltip — End() asserts on
                            // fonts still pushed within the window.
                            Font f(g_fontSmall);
                            ImGui::TextUnformatted("Remove");
                        }
                        ImGui::EndTooltip();
                    }
                }

                if ((rowClicked || joinClicked) && joinable && m_onJoin) {
                    state.lastJoinIP = sv.host;
                    state.lastJoinPort = std::to_string(sv.port);
                    m_onJoin(sv.host, sv.port);
                }

                ImGui::PopID();
                ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + size.y + 8));
            }

            if (removeIndex >= 0) {
                state.servers.erase(state.servers.begin() + removeIndex);
                state.serversDirty = true;
            }
            ImGui::Dummy(ImVec2(0, 1));
        }
        ImGui::EndChild();

        // ── Quick connect ──
        {
            const float connectW = 110.0f;
            const float portW = 92.0f;
            const float hostW = (x1 - x0) - portW - connectW - 20.0f;

            ImGui::SetCursorScreenPos(ImVec2(x0, qcY));
            Input("##qcHost", "host or ip", m_quickHost, sizeof(m_quickHost),
                  hostW, 46.0f, g_fontMono12);
            ImGui::SetCursorScreenPos(ImVec2(x0 + hostW + 10, qcY + 1));
            Input("##qcPort", "25565", m_quickPort, sizeof(m_quickPort),
                  portW, 44.0f, g_fontMono12);

            const uint16_t port = ParsePort(m_quickPort);
            const bool valid = canJoin && m_quickHost[0] != '\0' && port != 0;
            ImVec2 cpos(x1 - connectW, qcY + 1);
            ImGui::SetCursorScreenPos(cpos);
            bool pressed = ImGui::InvisibleButton("##qcConnect", ImVec2(connectW, 44.0f)) && valid;
            bool hov = ImGui::IsItemHovered() && valid;
            dl->AddRectFilled(cpos, cpos + ImVec2(connectW, 44.0f),
                              !valid ? BgActive : hov ? AccentHover : Accent, 10.0f);
            constexpr float kConnTracking = 1.25f;   // 0.1em at 12.5px
            ImVec2 ts(MeasureTracked(g_fontSmallBold, "CONNECT", kConnTracking),
                      Measure(g_fontSmallBold, "CONNECT").y);
            TxtTracked(dl, g_fontSmallBold, cpos + (ImVec2(connectW, 44.0f) - ts) * 0.5f,
                       valid ? OnAccent : TextFaint, "CONNECT", kConnTracking);

            if (pressed && m_onJoin) {
                state.lastJoinIP = m_quickHost;
                state.lastJoinPort = m_quickPort;
                m_onJoin(std::string(m_quickHost), port);
            }
        }
    }

    // ══════════════════════════════ Settings view ══════════════════════════════

    void LauncherUI::DrawSettingsView(LauncherUIState& state) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float x0 = ContentX();
        const float x1 = ContentRight();

        // ── Header + tab pills ──
        // Header column: 10px label line (13.2) + 6px gap + 22px title.
        TxtTracked(dl, g_fontMono10, ImVec2(x0, kPadY), TextFaint, "SETTINGS", 2.0f);
        Txt(dl, g_fontH2, ImVec2(x0, kPadY + 19.2f), TextPrimary, "Your setup");

        {
            struct TabDef { const char* label; SettingsTab tab; };
            const TabDef tabs[] = {
                { "General",   SettingsTab::General },
                { "Character", SettingsTab::Character },
            };
            // Tabs: 7/14 padding; container: 3px padding + 1px border (CSS
            // content-box) with a 2px gap, bottom-aligned with the title.
            const float tabH = Measure(g_fontSmallMed, "G").y + 14.0f;
            float tabsW = 0;
            for (const TabDef& t : tabs) tabsW += Measure(g_fontSmallMed, t.label).x + 28;
            tabsW += 2.0f;   // gap between the two tabs
            ImVec2 boxSize(tabsW + 8, tabH + 8);
            ImVec2 boxPos(x1 - boxSize.x, kPadY + 43.1f - boxSize.y);
            dl->AddRectFilled(boxPos, boxPos + boxSize, Rail, 10.0f);
            dl->AddRect(boxPos, boxPos + boxSize, Border, 10.0f);

            float tx = boxPos.x + 4;
            for (const TabDef& t : tabs) {
                ImVec2 ts = Measure(g_fontSmallMed, t.label);
                ImVec2 size(ts.x + 28, tabH);
                ImGui::SetCursorScreenPos(ImVec2(tx, boxPos.y + 4));
                ImGui::PushID(t.label);
                bool clicked = ImGui::InvisibleButton("##tab", size);
                bool hovered = ImGui::IsItemHovered();
                ImGui::PopID();
                if (m_tab == t.tab) {
                    dl->AddRectFilled(ImVec2(tx, boxPos.y + 4),
                                      ImVec2(tx + size.x, boxPos.y + 4 + size.y),
                                      BgActive, 7.0f);
                } else if (hovered) {
                    dl->AddRectFilled(ImVec2(tx, boxPos.y + 4),
                                      ImVec2(tx + size.x, boxPos.y + 4 + size.y),
                                      BgHover, 7.0f);
                }
                Txt(dl, g_fontSmallMed, ImVec2(tx + 14, boxPos.y + 4 + (size.y - ts.y) * 0.5f),
                    TextBody, t.label);
                if (clicked) m_tab = t.tab;
                tx += size.x + 2.0f;
            }
        }

        // ── Footer geometry ──
        const float doneH = 34.0f;   // 10px padding × 2 + 12.5px label line
        const float footerY = static_cast<float>(WindowHeight) - kPadY - doneH;
        const float footerLineY = footerY - 14.0f;
        const float scrollTop = kPadY + 43.1f + 18.0f;   // header column + 18px margin
        const float scrollBottom = footerLineY - 14.0f;

        // ── Scrollable tab content ──
        ImGui::SetCursorScreenPos(ImVec2(x0, scrollTop));
        ImGui::BeginChild("##settingsScroll", ImVec2(x1 - x0, scrollBottom - scrollTop),
                          ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
        if (m_tab == SettingsTab::General) {
            DrawSettingsGeneral(state);
        } else {
            DrawSettingsCharacter(state);
        }
        ImGui::EndChild();

        // ── Footer ──
        dl->AddLine(ImVec2(x0, footerLineY), ImVec2(x1, footerLineY), Border);
        {
            char meta[96];
            std::snprintf(meta, sizeof(meta), "LAUNCHER %s - GAME %s",
                          LauncherVersion,
                          state.gameInstalled ? state.installedVersion.c_str() : "NOT INSTALLED");
            Txt(dl, g_fontMono10, ImVec2(x0, footerY + 10), TextFaint, meta);

            ImVec2 ts = Measure(g_fontSmallSemi, "Done");
            ImVec2 size(ts.x + 36, doneH);
            ImGui::SetCursorScreenPos(ImVec2(x1 - size.x, footerY));
            bool pressed = ImGui::InvisibleButton("##done", size);
            bool hov = ImGui::IsItemHovered();
            dl->AddRectFilled(ImVec2(x1 - size.x, footerY), ImVec2(x1, footerY + doneH),
                              hov ? BgActiveHov : BgActive, 9.0f);
            Txt(dl, g_fontSmallSemi, ImVec2(x1 - size.x + 18, footerY + (doneH - ts.y) * 0.5f),
                TextBody, "Done");
            if (pressed) m_view = View::Play;
        }
    }

    // ── Settings / General ──────────────────────────────────────────────────

    void LauncherUI::DrawSettingsGeneral(LauncherUIState& state) {
        DrawAccountSection(state);
        // Forms take over the pane; the game rows show on the two "card" states.
        if (m_acctPane == AccountPane::Out || m_acctPane == AccountPane::In) {
            DrawGameRows(state);
        }
    }

    void LauncherUI::DrawAccountSection(LauncherUIState& state) {
        switch (m_acctPane) {
            case AccountPane::Out: {
                // CSS content-box card: 1px border + 16px padding → 17px inset.
                // Column: title (18.5) + 8 + body (14.1) + 8 + 6 + buttons (42).
                ImDrawList* dl = ImGui::GetWindowDrawList();
                ImVec2 p = ImGui::GetCursorScreenPos();
                float w = ImGui::GetContentRegionAvail().x;
                float cardH = 130.6f;
                dl->AddRect(p, p + ImVec2(w, cardH), Border, 12.0f);
                Txt(dl, g_fontH3, p + ImVec2(17, 17), TextPrimary, "Sign in to carry your setup");
                Txt(dl, g_fontBody, p + ImVec2(17, 43.5f), TextMuted,
                    "Your servers, colour and username follow the account.");
                ImGui::SetCursorScreenPos(p + ImVec2(17, 71.6f));
                // Accent button: 42px fixed height, 22px side padding.
                if (Pill("##goSignin", "Sign in", g_fontBodySemi, ImVec2(22, 13.65f),
                         Accent, AccentHover, OnAccent, 10.0f)) {
                    m_acctPane = AccountPane::SignIn;
                    std::snprintf(m_authName, sizeof(m_authName), "%s", state.playerName.c_str());
                    state.authStatusText.clear();
                }
                ImGui::SameLine(0, 10);
                // Outline button: content-box → 44px outer, inset 23/14.65.
                if (Pill("##goSignup", "Create account", g_fontBodyMed, ImVec2(23, 14.65f),
                         0, BgActive, TextBody, 10.0f, BorderBtn)) {
                    m_acctPane = AccountPane::SignUp;
                    std::snprintf(m_authName, sizeof(m_authName), "%s", state.playerName.c_str());
                    state.authStatusText.clear();
                }
                ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + cardH));
                ImGui::Dummy(ImVec2(0, 0));
                break;
            }
            case AccountPane::SignIn:   DrawSignInPane(state); break;
            case AccountPane::SignUp:   DrawSignUpPane(state); break;
            case AccountPane::In:       DrawSignedInPane(state); break;
            case AccountPane::ChangePw: DrawChangePwPane(state); break;
        }
    }

    void LauncherUI::DrawSignInPane(LauncherUIState& state) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        float w = ImGui::GetContentRegionAvail().x;
        ImVec2 p = ImGui::GetCursorScreenPos();

        if (Pill("##backOut", "< BACK", g_fontMono10, ImVec2(2, 2), 0, 0, TextFaint, 0, 0, true, 1.6f)) {
            m_acctPane = AccountPane::Out;
            state.authStatusText.clear();
        }
        ImGui::Dummy(ImVec2(0, 4));
        ImVec2 tp = ImGui::GetCursorScreenPos();
        Txt(dl, g_fontH3, tp, TextPrimary, "Welcome back");
        ImGui::Dummy(ImVec2(0, 26));

        Txt(dl, g_fontLabel12, ImGui::GetCursorScreenPos(), TextMuted, "Username");
        ImGui::Dummy(ImVec2(0, 18));
        Input("##siName", "your username", m_authName, sizeof(m_authName), w, 44,
              g_fontBody, ImGuiInputTextFlags_CallbackCharFilter, UsernameCharFilter);

        ImGui::Dummy(ImVec2(0, 8));
        Txt(dl, g_fontLabel12, ImGui::GetCursorScreenPos(), TextMuted, "Password");
        ImGui::Dummy(ImVec2(0, 18));
        Input("##siPw", "password", m_password, sizeof(m_password), w, 44,
              g_fontBody, ImGuiInputTextFlags_Password);

        if (!state.authStatusText.empty() && !state.authBusy) {
            ImGui::Dummy(ImVec2(0, 6));
            Txt(dl, g_fontSmall, ImGui::GetCursorScreenPos(), RedFg,
                state.authStatusText.c_str(), w);
            ImGui::Dummy(ImVec2(0, Measure(g_fontSmall, state.authStatusText.c_str(), w).y));
        }

        ImGui::Dummy(ImVec2(0, 12));
        {
            const bool ready = m_authName[0] != '\0' && m_password[0] != '\0' && !state.authBusy;
            ImVec2 bp = ImGui::GetCursorScreenPos();
            if (Primary("##doSignin", state.authBusy ? "WORKING..." : "Sign in",
                        bp, ImVec2(w, 46), ready, -1.0f, false,
                        g_fontBtn14, 0.84f) && m_onLogin) {
                state.authStatusText.clear();
                m_onLogin(m_authName, m_password);
            }
        }

        ImGui::Dummy(ImVec2(0, 12));
        {
            const char* q = "No account yet?";
            const char* link = "Create one";
            float total = Measure(g_fontSmall, q).x + 6 + Measure(g_fontSmall, link).x;
            ImVec2 cp = ImGui::GetCursorScreenPos();
            float cx = cp.x + (w - total) * 0.5f;
            Txt(dl, g_fontSmall, ImVec2(cx, cp.y), TextMuted, q);
            ImGui::SetCursorScreenPos(ImVec2(cx + Measure(g_fontSmall, q).x + 6, cp.y));
            if (Pill("##toSignup", link, g_fontSmall, ImVec2(1, 1), 0, 0, Link, 0)) {
                m_acctPane = AccountPane::SignUp;
                state.authStatusText.clear();
            }
        }
        (void)p;
    }

    void LauncherUI::DrawSignUpPane(LauncherUIState& state) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        float w = ImGui::GetContentRegionAvail().x;

        if (Pill("##backOut2", "< BACK", g_fontMono10, ImVec2(2, 2), 0, 0, TextFaint, 0, 0, true, 1.6f)) {
            m_acctPane = AccountPane::Out;
            state.authStatusText.clear();
        }
        ImGui::Dummy(ImVec2(0, 4));
        Txt(dl, g_fontH3, ImGui::GetCursorScreenPos(), TextPrimary, "Create your account");
        ImGui::Dummy(ImVec2(0, 26));

        Txt(dl, g_fontLabel12, ImGui::GetCursorScreenPos(), TextMuted, "Username");
        ImGui::Dummy(ImVec2(0, 18));
        Input("##suName", "pick a name", m_authName, sizeof(m_authName), w, 44,
              g_fontBody, ImGuiInputTextFlags_CallbackCharFilter, UsernameCharFilter);
        Txt(dl, g_fontMono10, ImGui::GetCursorScreenPos() + ImVec2(1, 3), TextFaint,
            "this is what other players see");
        ImGui::Dummy(ImVec2(0, 16));

        Txt(dl, g_fontLabel12, ImGui::GetCursorScreenPos(), TextMuted, "Password");
        ImGui::Dummy(ImVec2(0, 18));
        Input("##suPw", "4 characters or more", m_password, sizeof(m_password), w, 44,
              g_fontBody, ImGuiInputTextFlags_Password);

        if (!state.authStatusText.empty() && !state.authBusy) {
            ImGui::Dummy(ImVec2(0, 6));
            Txt(dl, g_fontSmall, ImGui::GetCursorScreenPos(), RedFg,
                state.authStatusText.c_str(), w);
            ImGui::Dummy(ImVec2(0, Measure(g_fontSmall, state.authStatusText.c_str(), w).y));
        }

        ImGui::Dummy(ImVec2(0, 12));
        {
            const bool ready = m_authName[0] != '\0' && m_password[0] != '\0' && !state.authBusy;
            ImVec2 bp = ImGui::GetCursorScreenPos();
            if (Primary("##doSignup", state.authBusy ? "WORKING..." : "Create account",
                        bp, ImVec2(w, 46), ready, -1.0f, false,
                        g_fontBtn14, 0.84f) && m_onSignup) {
                state.authStatusText.clear();
                m_onSignup(m_authName, m_password);
            }
        }

        ImGui::Dummy(ImVec2(0, 12));
        {
            const char* q = "Already have one?";
            const char* link = "Sign in";
            float total = Measure(g_fontSmall, q).x + 6 + Measure(g_fontSmall, link).x;
            ImVec2 cp = ImGui::GetCursorScreenPos();
            float cx = cp.x + (w - total) * 0.5f;
            Txt(dl, g_fontSmall, ImVec2(cx, cp.y), TextMuted, q);
            ImGui::SetCursorScreenPos(ImVec2(cx + Measure(g_fontSmall, q).x + 6, cp.y));
            if (Pill("##toSignin", link, g_fontSmall, ImVec2(1, 1), 0, 0, Link, 0)) {
                m_acctPane = AccountPane::SignIn;
                state.authStatusText.clear();
            }
        }
    }

    void LauncherUI::DrawSignedInPane(LauncherUIState& state) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 p = ImGui::GetCursorScreenPos();
        float w = ImGui::GetContentRegionAvail().x;
        const float cardH = 74.0f;   // content-box: 40 avatar + 16px padding + border

        dl->AddRect(p, p + ImVec2(w, cardH), Border, 12.0f);
        dl->AddRectFilled(p + ImVec2(17, 17), p + ImVec2(17 + 40, 17 + 40),
                          PlayerColorU32(state.playerColor), 11.0f);

        Txt(dl, g_fontName15, p + ImVec2(70, 19.9f), TextPrimary, state.accountName.c_str());
        std::string since = state.accountCreated > 0
            ? "MEMBER SINCE " + MonthYear(state.accountCreated)
            : std::string("SIGNED IN");
        Txt(dl, g_fontMono105, p + ImVec2(70, 38.2f), TextFaint, since.c_str());

        // SYNCED chip + sign-out link, right-aligned
        {
            constexpr float kTracking = 1.2f;
            float textW = MeasureTracked(g_fontMono10Med, "SYNCED", kTracking);
            float chipW = 10 + 6 + 7 + textW + 10;
            float chipH = Measure(g_fontMono10Med, "SYNCED").y + 10;
            ImVec2 cp(p.x + w - 17 - chipW, p.y + 15.3f);
            dl->AddRectFilled(cp, cp + ImVec2(chipW, chipH), GreenBg, chipH * 0.5f);
            dl->AddCircleFilled(ImVec2(cp.x + 10 + 3, cp.y + chipH * 0.5f), 3.0f, Green);
            TxtTracked(dl, g_fontMono10Med, ImVec2(cp.x + 10 + 6 + 7, cp.y + 5),
                       GreenFg, "SYNCED", kTracking);

            ImVec2 so = Measure(g_fontLabel12, "Sign out");
            ImGui::SetCursorScreenPos(ImVec2(p.x + w - 17 - so.x - 2, cp.y + chipH + 7));
            if (Pill("##signOut", "Sign out", g_fontLabel12, ImVec2(1, 1), 0, 0,
                     TextMuted, 0, 0, !state.authBusy) && m_onLogout) {
                m_onLogout();
            }
        }

        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + cardH));
        ImGui::Dummy(ImVec2(0, 4));
        Txt(dl, g_fontBody, ImGui::GetCursorScreenPos(), TextMuted,
            "Your username and friends list sync with this account.", w);
        ImGui::Dummy(ImVec2(0, Measure(g_fontBody, "A").y));
        if (!state.authStatusText.empty()) {
            Txt(dl, g_fontSmall, ImGui::GetCursorScreenPos(), TextMuted,
                state.authStatusText.c_str(), w);
            ImGui::Dummy(ImVec2(0, Measure(g_fontSmall, state.authStatusText.c_str(), w).y));
        }
        ImGui::Dummy(ImVec2(0, 0));
    }

    void LauncherUI::DrawChangePwPane(LauncherUIState& state) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        float w = ImGui::GetContentRegionAvail().x;

        if (Pill("##backAcct", "< ACCOUNT", g_fontMono10, ImVec2(2, 2), 0, 0, TextFaint, 0, 0, true, 1.6f)) {
            m_acctPane = AccountPane::In;
            ClearPasswordBuffers();
            state.authStatusText.clear();
        }
        ImGui::Dummy(ImVec2(0, 4));
        Txt(dl, g_fontH3, ImGui::GetCursorScreenPos(), TextPrimary, "Change password");
        ImGui::Dummy(ImVec2(0, 26));

        Txt(dl, g_fontLabel12, ImGui::GetCursorScreenPos(), TextMuted, "Current password");
        ImGui::Dummy(ImVec2(0, 18));
        Input("##cpCur", "current password", m_pwCurrent, sizeof(m_pwCurrent), w, 44,
              g_fontBody, ImGuiInputTextFlags_Password);

        ImGui::Dummy(ImVec2(0, 8));
        Txt(dl, g_fontLabel12, ImGui::GetCursorScreenPos(), TextMuted, "New password");
        ImGui::Dummy(ImVec2(0, 18));
        Input("##cpNew", "4 characters or more", m_pwNew, sizeof(m_pwNew), w, 44,
              g_fontBody, ImGuiInputTextFlags_Password);

        if (!state.authStatusText.empty() && !state.authBusy) {
            ImGui::Dummy(ImVec2(0, 6));
            Txt(dl, g_fontSmall, ImGui::GetCursorScreenPos(), RedFg,
                state.authStatusText.c_str(), w);
            ImGui::Dummy(ImVec2(0, Measure(g_fontSmall, state.authStatusText.c_str(), w).y));
        }

        ImGui::Dummy(ImVec2(0, 12));
        {
            const bool ready = m_pwCurrent[0] != '\0' && m_pwNew[0] != '\0' && !state.authBusy;
            ImVec2 bp = ImGui::GetCursorScreenPos();
            float cancelW = Measure(g_fontSmall, "Cancel").x + 42;
            if (Primary("##doChangePw", state.authBusy ? "WORKING..." : "Update password",
                        bp, ImVec2(w - cancelW - 10, 46), ready, -1.0f, false,
                        g_fontBtn14, 0.84f) && m_onChangePassword) {
                state.authStatusText.clear();
                m_onChangePassword(m_pwCurrent, m_pwNew);
            }
            ImGui::SetCursorScreenPos(ImVec2(bp.x + w - cancelW, bp.y));
            if (Pill("##cpCancel", "Cancel", g_fontSmall, ImVec2(21, 17.2f),
                     0, BgActive, TextBody, 10.0f, BorderBtn)) {
                m_acctPane = AccountPane::In;
                ClearPasswordBuffers();
                state.authStatusText.clear();
            }
        }
    }

    // ── Settings rows (username / password / vulkan / game dir) ──

    void LauncherUI::DrawGameRows(LauncherUIState& state) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        float w = ImGui::GetContentRegionAvail().x;
        const bool loggedIn = !state.sessionToken.empty();

        ImGui::Dummy(ImVec2(0, 16));
        {
            ImVec2 p = ImGui::GetCursorScreenPos();
            dl->AddLine(p, p + ImVec2(w, 0), Border);
        }
        ImGui::Dummy(ImVec2(0, 4));

        // Password row (signed in): entry point into the change-password pane.
        if (loggedIn) {
            ImVec2 p = ImGui::GetCursorScreenPos();
            float rowH = 52.0f;
            Txt(dl, g_fontSmall, p + ImVec2(0, 10), TextBody, "Password");
            char sub[48];
            std::snprintf(sub, sizeof(sub), "ACCOUNT #%lld",
                          static_cast<long long>(state.accountId));
            Txt(dl, g_fontMono10, p + ImVec2(0, 29), TextFaint, sub);

            ImVec2 ts = Measure(g_fontSmall, "Change >");
            ImGui::SetCursorScreenPos(ImVec2(p.x + w - ts.x - 4, p.y + (rowH - ts.y) * 0.5f));
            if (Pill("##goChangePw", "Change >", g_fontSmall, ImVec2(2, 2), 0, 0,
                     AccentSoft, 0)) {
                m_acctPane = AccountPane::ChangePw;
                state.authStatusText.clear();
            }
            ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + rowH));
            dl->AddLine(ImVec2(p.x, p.y + rowH), ImVec2(p.x + w, p.y + rowH), BorderSoft);
            ImGui::Dummy(ImVec2(0, 0));
        }

        DrawUsernameRow(state);

        // Vulkan row
        {
            ImVec2 p = ImGui::GetCursorScreenPos();
            float rowH = 52.0f;
            Txt(dl, g_fontSmall, p + ImVec2(0, 10), TextBody, "Vulkan renderer");
            Txt(dl, g_fontMono10, p + ImVec2(0, 29), TextFaint, "--vulkan");
            ImGui::SetCursorScreenPos(ImVec2(p.x + w - 34, p.y + (rowH - 19) * 0.5f));
            if (Toggle("##vulkanRow", state.useVulkan)) state.useVulkan = !state.useVulkan;
            ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + rowH));
            dl->AddLine(ImVec2(p.x, p.y + rowH), ImVec2(p.x + w, p.y + rowH), BorderSoft);
            ImGui::Dummy(ImVec2(0, 0));
        }

        // Game directory row
        {
            ImVec2 p = ImGui::GetCursorScreenPos();
            float rowH = 46.0f;
            Txt(dl, g_fontSmall, p + ImVec2(0, (rowH - 15) * 0.5f), TextBody, "Game directory");

            static double s_copiedAt = -10.0;
            const bool justCopied = ImGui::GetTime() - s_copiedAt < 1.2;
            const char* copyLabel = justCopied ? "COPIED" : "COPY";
            ImVec2 cts(MeasureTracked(g_fontMono95, copyLabel, 0.95f),
                       Measure(g_fontMono95, copyLabel).y);
            ImVec2 copySize(cts.x + 20, cts.y + 12);
            ImVec2 copyPos(p.x + w - copySize.x, p.y + (rowH - copySize.y) * 0.5f);

            std::string dir = Platform::g_gameDirectory.GetGameDirectory();
            float labelW = Measure(g_fontSmall, "Game directory").x;
            float pathMax = w - labelW - copySize.x - 30;
            std::string shown = Ellipsize(g_fontMono105, dir, pathMax);
            ImVec2 pts = Measure(g_fontMono105, shown.c_str());
            Txt(dl, g_fontMono105,
                ImVec2(copyPos.x - 10 - pts.x, p.y + (rowH - pts.y) * 0.5f),
                TextMuted, shown.c_str());

            ImGui::SetCursorScreenPos(copyPos);
            bool pressed = ImGui::InvisibleButton("##copyDir", copySize);
            bool hov = ImGui::IsItemHovered();
            dl->AddRectFilled(copyPos, copyPos + copySize,
                              hov ? BgActiveHov : BgActive, 7.0f);
            TxtTracked(dl, g_fontMono95, copyPos + ImVec2(10, 6),
                       justCopied ? GreenFg : AccentSoft, copyLabel, 0.95f);
            if (pressed) {
                ImGui::SetClipboardText(dir.c_str());
                s_copiedAt = ImGui::GetTime();
            }
            ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + rowH));
            ImGui::Dummy(ImVec2(0, 0));
        }
    }

    void LauncherUI::DrawUsernameRow(LauncherUIState& state) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        float w = ImGui::GetContentRegionAvail().x;
        const bool loggedIn = !state.sessionToken.empty();

        // Two-way sync: whenever state.playerName changed elsewhere (config
        // load, login, rename) re-seed the buffer — unless mid-edit.
        if (state.playerName != m_lastSyncedName) {
            std::snprintf(m_playerName, sizeof(m_playerName), "%s", state.playerName.c_str());
            m_lastSyncedName = state.playerName;
        }

        ImVec2 p = ImGui::GetCursorScreenPos();
        const float rowH = 56.0f;
        Txt(dl, g_fontSmall, p + ImVec2(0, 10), TextBody, "Username");
        Txt(dl, g_fontMono10, p + ImVec2(0, 29), TextFaint,
            loggedIn ? "YOUR ACCOUNT NAME - RENAMES SYNC"
                     : "BLANK -> PLAYER1, PLAYER2 ...");

        // Rename commit pill: logged in, name changed, service said Available.
        const bool canRename = loggedIn && state.playerName != state.accountName &&
            state.nameCheckState == LauncherUIState::NameCheck::Available && !state.authBusy;

        const float inputW = 180.0f;
        if (canRename) {
            ImVec2 ts = Measure(g_fontMono95, "RENAME");
            ImVec2 size(ts.x + 20, ts.y + 12);
            ImGui::SetCursorScreenPos(ImVec2(p.x + w - inputW - 10 - size.x,
                                             p.y + (36 - size.y) * 0.5f + 2));
            if (Pill("##rename", "RENAME", g_fontMono95, ImVec2(10, 6),
                     Accent, AccentHover, OnAccent, 7.0f) && m_onRename) {
                m_onRename(state.playerName);
            }
        }

        ImGui::SetCursorScreenPos(ImVec2(p.x + w - inputW, p.y + 2));
        if (Input("##username", loggedIn ? "" : "pick a name", m_playerName,
                  sizeof(m_playerName), inputW, 36, g_fontInput13,
                  ImGuiInputTextFlags_CallbackCharFilter, UsernameCharFilter, 9.0f, 13.0f)) {
            if (std::strlen(m_playerName) > 16) m_playerName[16] = '\0';
            state.playerName = m_playerName;
            m_lastSyncedName = state.playerName;
            m_nameEditTime = ImGui::GetTime();
            state.nameCheckState = LauncherUIState::NameCheck::Idle;
        }
        // Debounced availability check (0.5s after the last keystroke).
        if (m_nameEditTime > 0.0 && ImGui::GetTime() - m_nameEditTime > 0.5) {
            m_nameEditTime = 0.0;
            if (m_playerName[0] != '\0' && m_onCheckName) {
                state.nameCheckState = LauncherUIState::NameCheck::Checking;
                m_onCheckName(m_playerName);
            }
        }

        // Availability status under the input, right-aligned.
        {
            using NC = LauncherUIState::NameCheck;
            const char* text = nullptr;
            ImU32 col = TextFaint;
            switch (state.nameCheckState) {
                case NC::Checking:  text = "checking..."; break;
                case NC::Available: text = "available"; col = GreenFg; break;
                case NC::Taken:     text = "taken"; col = RedFg; break;
                case NC::Yours:     text = "this is you"; break;
                case NC::Invalid:   text = "3-16 letters, numbers, _"; col = RedFg; break;
                case NC::Idle: break;
            }
            if (text) {
                ImVec2 ts = Measure(g_fontMono10, text);
                Txt(dl, g_fontMono10, ImVec2(p.x + w - ts.x, p.y + 41), col, text);
            }
        }

        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + rowH));
        dl->AddLine(ImVec2(p.x, p.y + rowH), ImVec2(p.x + w, p.y + rowH), BorderSoft);
        ImGui::Dummy(ImVec2(0, 0));
    }

    // ── Settings / Character ────────────────────────────────────────────────

    void LauncherUI::DrawSettingsCharacter(LauncherUIState& state) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 origin = ImGui::GetCursorScreenPos();
        float w = ImGui::GetContentRegionAvail().x;

        const auto& selected = ColorBySlug(state.playerColor);
        const ImU32 col = IM_COL32(selected.r, selected.g, selected.b, 255);

        // ── Preview panel (left) ──
        {
            const ImVec2 p = origin;
            const ImVec2 size(132, 188);
            // Diagonal stripe fill, clipped to the rounded panel.
            dl->AddRectFilled(p, p + size, BgStripeA, 12.0f);
            dl->PushClipRect(p + ImVec2(1, 1), p + size - ImVec2(1, 1), true);
            for (float s = -size.y; s < size.x + size.y; s += 14.0f) {
                dl->AddLine(ImVec2(p.x + s, p.y + size.y), ImVec2(p.x + s + size.y, p.y),
                            BgStripeB, 7.0f);
            }
            // Stick figure in the selected colour (matches the in-game player).
            {
                ImVec2 c(p.x + size.x * 0.5f, p.y + 74);
                const float t = 3.0f;
                dl->AddCircle(ImVec2(c.x, c.y - 26), 9.0f, col, 0, t);      // head
                dl->AddLine(ImVec2(c.x, c.y - 17), ImVec2(c.x, c.y + 14), col, t);   // torso
                dl->AddLine(ImVec2(c.x, c.y - 10), ImVec2(c.x - 14, c.y + 2), col, t); // arms
                dl->AddLine(ImVec2(c.x, c.y - 10), ImVec2(c.x + 14, c.y + 2), col, t);
                dl->AddLine(ImVec2(c.x, c.y + 14), ImVec2(c.x - 11, c.y + 34), col, t); // legs
                dl->AddLine(ImVec2(c.x, c.y + 14), ImVec2(c.x + 11, c.y + 34), col, t);
            }
            {
                constexpr float kTracking = 1.26f;   // 0.14em at 9px
                const char* cap1 = "PLAYER";
                const char* cap2 = "PREVIEW";
                float w1 = MeasureTracked(g_fontMono9, cap1, kTracking);
                float w2 = MeasureTracked(g_fontMono9, cap2, kTracking);
                TxtTracked(dl, g_fontMono9, ImVec2(p.x + (size.x - w1) * 0.5f, p.y + 138),
                           TextGhost, cap1, kTracking);
                TxtTracked(dl, g_fontMono9, ImVec2(p.x + (size.x - w2) * 0.5f, p.y + 152),
                           TextGhost, cap2, kTracking);
            }
            dl->PopClipRect();
            dl->AddRect(p, p + size, Border, 12.0f);
        }

        // ── Colour picker (right) ──
        const float colX = origin.x + 132 + 22;
        const float colW = w - 132 - 22;
        float y = origin.y;

        Txt(dl, g_fontLabel12, ImVec2(colX, y), TextMuted, "Colour");
        {
            std::string hint = "--color ";
            hint += state.playerColor.empty() ? "default" : state.playerColor;
            ImVec2 ts = Measure(g_fontMono10, hint.c_str());
            Txt(dl, g_fontMono10, ImVec2(colX + colW - ts.x, y + 2), TextFaint, hint.c_str());
        }
        y += 26;

        // Swatch grid: 5 columns, 34px cells, 9px gap.
        {
            constexpr float kCell = 34.0f, kGap = 9.0f;
            const size_t count = sizeof(Game::kPlayerColorTable) / sizeof(Game::kPlayerColorTable[0]);
            for (size_t i = 0; i < count; ++i) {
                const auto& entry = Game::kPlayerColorTable[i];
                const size_t row = i / 5, colIdx = i % 5;
                const float cx = colX + static_cast<float>(colIdx) * (kCell + kGap);
                const float cy = y + static_cast<float>(row) * (kCell + kGap);
                ImGui::SetCursorScreenPos(ImVec2(cx, cy));
                ImGui::PushID(entry.slug);
                bool clicked = ImGui::InvisibleButton("##swatch", ImVec2(kCell, kCell));
                bool hovered = ImGui::IsItemHovered();
                ImGui::PopID();

                ImU32 c = IM_COL32(entry.r, entry.g, entry.b, 255);
                dl->AddRectFilled(ImVec2(cx, cy), ImVec2(cx + kCell, cy + kCell), c, 9.0f);
                const bool isSelected = (state.playerColor == entry.slug) ||
                    (state.playerColor.empty() && entry.id == Game::PlayerColorId::Default);
                if (isSelected) {
                    dl->AddRect(ImVec2(cx - 2, cy - 2), ImVec2(cx + kCell + 2, cy + kCell + 2),
                                TextPrimary, 11.0f, 0, 2.0f);
                } else if (hovered) {
                    dl->AddRect(ImVec2(cx - 2, cy - 2), ImVec2(cx + kCell + 2, cy + kCell + 2),
                                BorderHover, 11.0f, 0, 2.0f);
                }
                if (hovered && ImGui::BeginTooltip()) {
                    {
                        // Pop the font BEFORE EndTooltip — End() asserts on
                        // fonts still pushed within the window.
                        Font f(g_fontSmall);
                        ImGui::TextUnformatted(entry.name);
                    }
                    ImGui::EndTooltip();
                }
                if (clicked) {
                    // Default entry → store empty (lets a future renumber not break configs).
                    state.playerColor = (entry.id == Game::PlayerColorId::Default)
                                        ? std::string() : entry.slug;
                }
            }
            y += 2.0f * kCell + kGap + 12.0f;
        }

        Txt(dl, g_fontSmall, ImVec2(colX, y), TextBody, selected.name);
        y += 30;

        // ── Next slots (placeholders) ──
        TxtTracked(dl, g_fontMono95, ImVec2(colX, y), TextGhost, "NEXT SLOTS", 1.9f);
        y += 20;
        const char* slots[] = { "Capes", "Accessories" };
        for (const char* slot : slots) {
            dl->AddRect(ImVec2(colX, y), ImVec2(colX + colW, y + 40), BorderDashed, 10.0f);
            Txt(dl, g_fontSmall, ImVec2(colX + 13, y + 12), TextDim, slot);
            float soonW = MeasureTracked(g_fontMono95, "SOON", 1.33f);
            TxtTracked(dl, g_fontMono95, ImVec2(colX + colW - 13 - soonW, y + 14),
                       TextGhost, "SOON", 1.33f);
            y += 40 + 10;
        }

        // Reserve the drawn height so the scroll child sizes correctly.
        float bottom = std::max(origin.y + 188.0f + 12.0f, y);
        ImGui::SetCursorScreenPos(ImVec2(origin.x, bottom));
        ImGui::Dummy(ImVec2(0, 0));
    }

} // namespace Launcher
