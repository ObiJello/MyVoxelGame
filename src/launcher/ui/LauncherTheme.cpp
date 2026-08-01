// File: src/launcher/ui/LauncherTheme.cpp
#include "LauncherTheme.hpp"
#include "common/core/Log.hpp"
#include <imgui.h>
#include <GLFW/glfw3.h>
#include <string>
#include <filesystem>

namespace Launcher {

    // Font pointers accessible to the UI
    ImFont* g_fontDisplay = nullptr;
    ImFont* g_fontH2 = nullptr;
    ImFont* g_fontH3 = nullptr;
    ImFont* g_fontName15 = nullptr;
    ImFont* g_fontButton = nullptr;
    ImFont* g_fontBtn14 = nullptr;
    ImFont* g_fontBody = nullptr;
    ImFont* g_fontBodyMed = nullptr;
    ImFont* g_fontBodySemi = nullptr;
    ImFont* g_fontName13 = nullptr;
    ImFont* g_fontInput13 = nullptr;
    ImFont* g_fontSmall = nullptr;
    ImFont* g_fontSmallMed = nullptr;
    ImFont* g_fontSmallSemi = nullptr;
    ImFont* g_fontSmallBold = nullptr;
    ImFont* g_fontLabel12 = nullptr;
    ImFont* g_fontWordmark = nullptr;
    ImFont* g_fontJoin = nullptr;
    ImFont* g_fontMono12 = nullptr;
    ImFont* g_fontMono105 = nullptr;
    ImFont* g_fontMono10 = nullptr;
    ImFont* g_fontMono10Med = nullptr;
    ImFont* g_fontMono95 = nullptr;
    ImFont* g_fontMono9 = nullptr;

    static ImVec4 U32ToVec4(ImU32 c) {
        return ImGui::ColorConvertU32ToFloat4(c);
    }

    void ApplyLauncherTheme() {
        ImGuiStyle& style = ImGui::GetStyle();

        style.WindowRounding = 0.0f;
        style.FrameRounding = 10.0f;
        style.GrabRounding = 6.0f;
        style.ScrollbarRounding = 6.0f;
        style.TabRounding = 7.0f;
        style.ChildRounding = 0.0f;
        style.PopupRounding = 10.0f;

        style.WindowPadding = ImVec2(0, 0);
        style.FramePadding = ImVec2(13, 11);
        style.ItemSpacing = ImVec2(10, 8);
        style.ItemInnerSpacing = ImVec2(8, 4);
        style.ScrollbarSize = 6.0f;
        style.IndentSpacing = 20.0f;
        style.WindowBorderSize = 0.0f;
        style.FrameBorderSize = 1.0f;
        style.ChildBorderSize = 0.0f;

        ImVec4* c = style.Colors;

        c[ImGuiCol_WindowBg]             = U32ToVec4(Palette::WindowBg);
        c[ImGuiCol_ChildBg]              = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_PopupBg]              = U32ToVec4(Palette::Rail);

        c[ImGuiCol_TitleBg]              = U32ToVec4(Palette::WindowBg);
        c[ImGuiCol_TitleBgActive]        = U32ToVec4(Palette::WindowBg);
        c[ImGuiCol_TitleBgCollapsed]     = U32ToVec4(Palette::WindowBg);

        // Inputs: card-dark fill with a hairline border (design's 42px fields)
        c[ImGuiCol_FrameBg]              = U32ToVec4(Palette::Rail);
        c[ImGuiCol_FrameBgHovered]       = U32ToVec4(Palette::BgHover);
        c[ImGuiCol_FrameBgActive]        = U32ToVec4(Palette::BgActive);
        c[ImGuiCol_Border]               = U32ToVec4(Palette::Border);

        // Stock buttons are rarely used (the UI draws its own), but keep them on-palette
        c[ImGuiCol_Button]               = U32ToVec4(Palette::BgActive);
        c[ImGuiCol_ButtonHovered]        = U32ToVec4(Palette::BgActiveHov);
        c[ImGuiCol_ButtonActive]         = U32ToVec4(Palette::BgActive);

        c[ImGuiCol_Header]               = U32ToVec4(Palette::BgActive);
        c[ImGuiCol_HeaderHovered]        = U32ToVec4(Palette::BgActiveHov);
        c[ImGuiCol_HeaderActive]         = U32ToVec4(Palette::BgActive);

        c[ImGuiCol_Separator]            = U32ToVec4(Palette::Border);
        c[ImGuiCol_SeparatorHovered]     = U32ToVec4(Palette::BorderHover);
        c[ImGuiCol_SeparatorActive]      = U32ToVec4(Palette::Accent);

        c[ImGuiCol_ScrollbarBg]          = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_ScrollbarGrab]        = U32ToVec4(Palette::BgActive);
        c[ImGuiCol_ScrollbarGrabHovered] = U32ToVec4(Palette::BgActiveHov);
        c[ImGuiCol_ScrollbarGrabActive]  = U32ToVec4(Palette::BorderHover);

        c[ImGuiCol_PlotHistogram]        = U32ToVec4(Palette::Accent);

        c[ImGuiCol_Text]                 = U32ToVec4(Palette::TextBody);
        c[ImGuiCol_TextDisabled]         = U32ToVec4(Palette::TextFaint);

        c[ImGuiCol_CheckMark]            = U32ToVec4(Palette::Accent);
        c[ImGuiCol_SliderGrab]           = U32ToVec4(Palette::Accent);
        c[ImGuiCol_SliderGrabActive]     = U32ToVec4(Palette::AccentHover);
        c[ImGuiCol_NavHighlight]         = ImVec4(0, 0, 0, 0);
    }

    // stb_truetype maps AddFontFromFileTTF's size to (hhea.ascent - hhea.descent),
    // while CSS font-size maps to the em square. To render the design's CSS px
    // sizes exactly, every load is multiplied by the family's metric ratio
    // (ascent - descent) / unitsPerEm, measured from the vendored files:
    //   Archivo:        (878 + 210) / 1000 = 1.088
    //   JetBrains Mono: (1020 + 300) / 1000 = 1.320
    static constexpr float kArchivoEm = 1.088f;
    static constexpr float kMonoEm    = 1.320f;

    void LoadLauncherFonts(GLFWwindow* window, const std::string& fontDir) {
        ImGuiIO& io = ImGui::GetIO();

        // Get DPI scale for retina displays
        float xscale = 1.0f, yscale = 1.0f;
        glfwGetWindowContentScale(window, &xscale, &yscale);
        float dpiScale = xscale;

        auto add = [&](const char* file, float cssSize, float emRatio) -> ImFont* {
            std::string path = fontDir + "/" + file;
            if (!std::filesystem::exists(path)) return nullptr;
            ImFontConfig cfg;
            // Slight AA-coverage boost so stems read like the browser's
            // antialiased rendering of the design canvas. Keep this gentle:
            // 1.30 made every weight look a step heavier than the design.
            cfg.RasterizerMultiply = cssSize <= 17.0f ? 1.10f
                                   : cssSize <= 22.0f ? 1.05f : 1.02f;
            return io.Fonts->AddFontFromFileTTF(path.c_str(),
                                                cssSize * emRatio * dpiScale, &cfg);
        };

        // ── UI family: Archivo, per-weight (the design's typeface) ──
        g_fontBody = add("Archivo-Regular.ttf", 13.5f, kArchivoEm);
        if (g_fontBody) {
            Log::Info("Loading Archivo from %s (DPI scale: %.1f)", fontDir.c_str(), dpiScale);
            g_fontBodyMed   = add("Archivo-Medium.ttf",   13.5f, kArchivoEm);
            g_fontBodySemi  = add("Archivo-SemiBold.ttf", 13.5f, kArchivoEm);
            g_fontName13    = add("Archivo-SemiBold.ttf", 13.0f, kArchivoEm);
            g_fontInput13   = add("Archivo-Regular.ttf",  13.0f, kArchivoEm);
            g_fontSmall     = add("Archivo-Regular.ttf",  12.5f, kArchivoEm);
            g_fontSmallMed  = add("Archivo-Medium.ttf",   12.5f, kArchivoEm);
            g_fontSmallSemi = add("Archivo-SemiBold.ttf", 12.5f, kArchivoEm);
            g_fontSmallBold = add("Archivo-Bold.ttf",     12.5f, kArchivoEm);
            g_fontLabel12   = add("Archivo-Regular.ttf",  12.0f, kArchivoEm);
            g_fontWordmark  = add("Archivo-Bold.ttf",     12.0f, kArchivoEm);
            g_fontJoin      = add("Archivo-SemiBold.ttf", 11.5f, kArchivoEm);
            g_fontName15    = add("Archivo-SemiBold.ttf", 15.0f, kArchivoEm);
            g_fontButton    = add("Archivo-Bold.ttf",     15.0f, kArchivoEm);
            g_fontBtn14     = add("Archivo-Bold.ttf",     14.0f, kArchivoEm);
            g_fontH3        = add("Archivo-SemiBold.ttf", 17.0f, kArchivoEm);
            g_fontH2        = add("Archivo-SemiBold.ttf", 22.0f, kArchivoEm);
            g_fontDisplay   = add("Archivo-SemiBold.ttf", 42.0f, kArchivoEm);
        } else {
            // Archivo missing (misconfigured install): fall back to Roboto,
            // one weight for everything, before giving up entirely.
            g_fontBody = add("Roboto-Medium.ttf", 13.5f, 1.0f);
            if (g_fontBody) {
                Log::Warning("Archivo not found in %s - falling back to Roboto", fontDir.c_str());
                g_fontSmall   = add("Roboto-Medium.ttf", 12.5f, 1.0f);
                g_fontButton  = add("Roboto-Medium.ttf", 15.0f, 1.0f);
                g_fontH3      = add("Roboto-Medium.ttf", 17.0f, 1.0f);
                g_fontH2      = add("Roboto-Medium.ttf", 22.0f, 1.0f);
                g_fontDisplay = add("Roboto-Medium.ttf", 42.0f, 1.0f);
            } else {
                Log::Warning("No launcher fonts found in %s - using ImGui default", fontDir.c_str());
                g_fontBody = io.Fonts->AddFontDefault();
                dpiScale = 1.0f;
            }
        }
        io.FontGlobalScale = 1.0f / dpiScale;

        // Any missing weight degrades to the nearest loaded one.
        if (!g_fontBodyMed)   g_fontBodyMed = g_fontBody;
        if (!g_fontBodySemi)  g_fontBodySemi = g_fontBodyMed;
        if (!g_fontName13)    g_fontName13 = g_fontBodySemi;
        if (!g_fontInput13)   g_fontInput13 = g_fontBody;
        if (!g_fontSmall)     g_fontSmall = g_fontBody;
        if (!g_fontSmallMed)  g_fontSmallMed = g_fontSmall;
        if (!g_fontSmallSemi) g_fontSmallSemi = g_fontSmallMed;
        if (!g_fontSmallBold) g_fontSmallBold = g_fontSmallSemi;
        if (!g_fontLabel12)   g_fontLabel12 = g_fontSmall;
        if (!g_fontWordmark)  g_fontWordmark = g_fontSmallBold;
        if (!g_fontJoin)      g_fontJoin = g_fontSmallSemi;
        if (!g_fontName15)    g_fontName15 = g_fontH3;
        if (!g_fontButton)    g_fontButton = g_fontBodySemi;
        if (!g_fontBtn14)     g_fontBtn14 = g_fontButton;
        if (!g_fontH3)        g_fontH3 = g_fontBodySemi;
        if (!g_fontH2)        g_fontH2 = g_fontH3;
        if (!g_fontDisplay)   g_fontDisplay = g_fontH2;

        // ── Mono family: JetBrains Mono (labels, chips, addresses) ──
        g_fontMono12    = add("JetBrainsMono-Regular.ttf", 12.0f, kMonoEm);
        g_fontMono105   = add("JetBrainsMono-Regular.ttf", 10.5f, kMonoEm);
        g_fontMono10    = add("JetBrainsMono-Regular.ttf", 10.0f, kMonoEm);
        g_fontMono10Med = add("JetBrainsMono-Medium.ttf",  10.0f, kMonoEm);
        g_fontMono95    = add("JetBrainsMono-Regular.ttf",  9.5f, kMonoEm);
        g_fontMono9     = add("JetBrainsMono-Regular.ttf",  9.0f, kMonoEm);
        if (!g_fontMono10) {
            Log::Warning("JetBrains Mono not found in %s", fontDir.c_str());
            g_fontMono10 = g_fontSmall;
        }
        if (!g_fontMono12)    g_fontMono12 = g_fontMono10;
        if (!g_fontMono105)   g_fontMono105 = g_fontMono10;
        if (!g_fontMono10Med) g_fontMono10Med = g_fontMono10;
        if (!g_fontMono95)    g_fontMono95 = g_fontMono10;
        if (!g_fontMono9)     g_fontMono9 = g_fontMono95;
    }

} // namespace Launcher
