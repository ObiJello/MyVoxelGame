// File: src/launcher/ui/LauncherTheme.hpp
#pragma once

#include <string>
#include <imgui.h>

struct GLFWwindow;

namespace Launcher {

    // Fonts (set by LoadLauncherFonts). The design uses Archivo (UI) and
    // JetBrains Mono (labels/addresses), vendored in ext/fonts/.
    //
    // Sizes are CSS px from the design doc. stb_truetype sizes fonts by
    // (ascent - descent) while CSS sizes by em square, so the loader
    // multiplies each size by the family's metric ratio (see kArchivoEm /
    // kMonoEm in LauncherTheme.cpp) — without this, Archivo renders 8.8%
    // and JetBrains Mono 32% smaller than the design.
    //
    // ── Archivo (UI family) ──
    extern ImFont* g_fontDisplay;    // SemiBold 42 — big version number
    extern ImFont* g_fontH2;         // SemiBold 22 — view titles
    extern ImFont* g_fontH3;         // SemiBold 17 — card titles
    extern ImFont* g_fontName15;     // SemiBold 15 — signed-in account name
    extern ImFont* g_fontButton;     // Bold 15 — primary button label
    extern ImFont* g_fontBtn14;      // Bold 14 — form submit buttons
    extern ImFont* g_fontBody;       // Regular 13.5 — notes / body copy
    extern ImFont* g_fontBodyMed;    // Medium 13.5 — nav labels, input text
    extern ImFont* g_fontBodySemi;   // SemiBold 13.5 — server names
    extern ImFont* g_fontName13;     // SemiBold 13 — rail account-card name
    extern ImFont* g_fontInput13;    // Regular 13 — username row input
    extern ImFont* g_fontSmall;      // Regular 12.5 — secondary copy, row labels
    extern ImFont* g_fontSmallMed;   // Medium 12.5 — tabs, outline buttons
    extern ImFont* g_fontSmallSemi;  // SemiBold 12.5 — Done
    extern ImFont* g_fontSmallBold;  // Bold 12.5 — OBEYCRAFT wordmark, CONNECT
    extern ImFont* g_fontLabel12;    // Regular 12 — form field labels, Sign out
    extern ImFont* g_fontWordmark;   // Bold 12 — OBEYCRAFT rail wordmark
    extern ImFont* g_fontJoin;       // SemiBold 11.5 — JOIN pills
    // ── JetBrains Mono (label family) ──
    extern ImFont* g_fontMono12;     // Regular 12 — quick-connect inputs
    extern ImFont* g_fontMono105;    // Regular 10.5 — addresses, ping, member-since
    extern ImFont* g_fontMono10;     // Regular 10 — section labels, subs, hints, footer
    extern ImFont* g_fontMono10Med;  // Medium 10 — status chips
    extern ImFont* g_fontMono95;     // Regular 9.5 — rail sub, COPY, NEXT SLOTS, SOON
    extern ImFont* g_fontMono9;      // Regular 9 — preview caption

    // Design palette — single source of truth for the redesigned launcher.
    // Values mirror the "ObeyCraft Launcher" design doc (800×500 rail layout).
    namespace Palette {
        constexpr ImU32 WindowBg     = IM_COL32(0x0f, 0x10, 0x14, 255); // content background
        constexpr ImU32 Rail         = IM_COL32(0x14, 0x16, 0x1c, 255); // rail / cards / inputs
        constexpr ImU32 Border       = IM_COL32(0x1e, 0x21, 0x2a, 255); // standard 1px border
        constexpr ImU32 BorderSoft   = IM_COL32(0x1a, 0x1d, 0x25, 255); // settings row divider
        constexpr ImU32 BorderHover  = IM_COL32(0x2c, 0x32, 0x44, 255); // server row hover border
        constexpr ImU32 BorderBtn    = IM_COL32(0x26, 0x2b, 0x38, 255); // outline button border
        constexpr ImU32 BorderDashed = IM_COL32(0x23, 0x26, 0x2f, 255); // "SOON" placeholder rows
        constexpr ImU32 BgActive     = IM_COL32(0x1c, 0x20, 0x30, 255); // active nav / pills
        constexpr ImU32 BgActiveHov  = IM_COL32(0x23, 0x28, 0x39, 255); // secondary button hover
        constexpr ImU32 BgHover      = IM_COL32(0x19, 0x1c, 0x24, 255); // nav hover
        constexpr ImU32 BgOffline    = IM_COL32(0x17, 0x1a, 0x21, 255); // disabled JOIN pill
        constexpr ImU32 BgStripeA    = IM_COL32(0x13, 0x15, 0x19, 255); // preview stripes
        constexpr ImU32 BgStripeB    = IM_COL32(0x17, 0x1a, 0x21, 255);

        constexpr ImU32 Accent       = IM_COL32(0x4d, 0x8d, 0xff, 255); // primary blue
        constexpr ImU32 AccentHover  = IM_COL32(0x6b, 0xa0, 0xff, 255);
        constexpr ImU32 AccentActive = IM_COL32(0x3d, 0x7c, 0xe8, 255);
        constexpr ImU32 AccentSoft   = IM_COL32(0xa8, 0xc4, 0xff, 255); // JOIN / COPY pill text
        constexpr ImU32 Link         = IM_COL32(0x7f, 0x9c, 0xff, 255); // inline links
        constexpr ImU32 OnAccent     = IM_COL32(0x07, 0x10, 0x18, 255); // text on accent fills

        constexpr ImU32 TextPrimary  = IM_COL32(0xee, 0xf1, 0xf8, 255); // headings, values
        constexpr ImU32 TextBody     = IM_COL32(0xd5, 0xda, 0xe6, 255); // labels, nav
        constexpr ImU32 TextNotes    = IM_COL32(0xb9, 0xbe, 0xc9, 255); // release-note lines
        constexpr ImU32 TextMuted    = IM_COL32(0x8b, 0x90, 0xa0, 255); // secondary copy
        constexpr ImU32 TextDim      = IM_COL32(0x6f, 0x74, 0x84, 255); // tertiary copy
        constexpr ImU32 TextFaint    = IM_COL32(0x5c, 0x60, 0x70, 255); // mono labels
        constexpr ImU32 TextGhost    = IM_COL32(0x4a, 0x4f, 0x5e, 255); // placeholders, offline
        constexpr ImU32 TextSynced   = IM_COL32(0x6f, 0x8f, 0xbf, 255); // rail "SYNCED"

        constexpr ImU32 Green        = IM_COL32(0x46, 0xc4, 0x6a, 255); // online / ready
        constexpr ImU32 GreenBg      = IM_COL32(0x46, 0xc4, 0x6a, 31);  // 12% chip fill
        constexpr ImU32 GreenFg      = IM_COL32(0x8f, 0xe0, 0xa6, 255);
        constexpr ImU32 Amber        = IM_COL32(0xe0, 0xb0, 0x4a, 255); // restart pending
        constexpr ImU32 AmberBg      = IM_COL32(0xe0, 0xb0, 0x4a, 23);  // 9% banner fill
        constexpr ImU32 AmberChipBg  = IM_COL32(0xe0, 0xb0, 0x4a, 36);  // 14% chip fill
        constexpr ImU32 AmberBorder  = IM_COL32(0xe0, 0xb0, 0x4a, 56);  // 22% banner border
        constexpr ImU32 AmberText    = IM_COL32(0xe5, 0xcf, 0x9e, 255); // banner body
        constexpr ImU32 AmberChipFg  = IM_COL32(0xe5, 0xc6, 0x8a, 255);
        constexpr ImU32 AmberMeta    = IM_COL32(0xa9, 0x92, 0x5f, 255); // "1.0.37 → 1.0.38"
        constexpr ImU32 BlueChipBg   = IM_COL32(0x4d, 0x8d, 0xff, 36);  // 14% chip fill
        constexpr ImU32 Red          = IM_COL32(0xe0, 0x56, 0x4a, 255); // errors
        constexpr ImU32 RedBg        = IM_COL32(0xe0, 0x56, 0x4a, 26);
        constexpr ImU32 RedFg        = IM_COL32(0xf0, 0x9a, 0x92, 255);
        constexpr ImU32 GreyChipBg   = IM_COL32(0x8b, 0x90, 0xa0, 26);  // busy/neutral chip

        constexpr ImU32 SwitchOff    = IM_COL32(0x2a, 0x2f, 0x3c, 255); // toggle track (off)
        constexpr ImU32 Knob         = IM_COL32(0xee, 0xf1, 0xf8, 255); // toggle knob
    }

    // Apply the dark launcher theme (ImGui style side of the palette above).
    void ApplyLauncherTheme();

    // Load fonts for the launcher. Call after ImGui context is created.
    // fontDir should hold Roboto-Medium.ttf (UI) and Cousine-Regular.ttf (mono).
    void LoadLauncherFonts(GLFWwindow* window, const std::string& fontDir);

} // namespace Launcher
