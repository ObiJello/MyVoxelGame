// File: src/launcher/ui/LauncherTheme.cpp
#include "LauncherTheme.hpp"
#include "common/core/Log.hpp"
#include <imgui.h>
#include <GLFW/glfw3.h>
#include <string>
#include <filesystem>

namespace Launcher {

    // Font pointers accessible to the UI
    ImFont* g_fontTitle = nullptr;    // 28px
    ImFont* g_fontBody = nullptr;     // 18px
    ImFont* g_fontSmall = nullptr;    // 14px
    ImFont* g_fontButton = nullptr;   // 22px

    void ApplyLauncherTheme() {
        ImGuiStyle& style = ImGui::GetStyle();

        // Rounding for modern feel
        style.WindowRounding = 8.0f;
        style.FrameRounding = 6.0f;
        style.GrabRounding = 4.0f;
        style.ScrollbarRounding = 4.0f;
        style.TabRounding = 4.0f;
        style.ChildRounding = 6.0f;
        style.PopupRounding = 6.0f;

        // Padding and spacing
        style.WindowPadding = ImVec2(20, 20);
        style.FramePadding = ImVec2(12, 8);
        style.ItemSpacing = ImVec2(10, 8);
        style.ItemInnerSpacing = ImVec2(8, 4);
        style.ScrollbarSize = 10.0f;
        style.IndentSpacing = 20.0f;
        style.WindowBorderSize = 0.0f;
        style.FrameBorderSize = 0.0f;

        ImVec4* c = style.Colors;

        // Background colors - near black with blue tint
        c[ImGuiCol_WindowBg]             = ImVec4(0.071f, 0.071f, 0.094f, 1.00f);  // rgb(18, 18, 24)
        c[ImGuiCol_ChildBg]              = ImVec4(0.110f, 0.110f, 0.149f, 1.00f);  // rgb(28, 28, 38)
        c[ImGuiCol_PopupBg]              = ImVec4(0.090f, 0.090f, 0.120f, 0.98f);

        // Title bar
        c[ImGuiCol_TitleBg]              = ImVec4(0.055f, 0.055f, 0.075f, 1.00f);
        c[ImGuiCol_TitleBgActive]        = ImVec4(0.075f, 0.075f, 0.100f, 1.00f);
        c[ImGuiCol_TitleBgCollapsed]     = ImVec4(0.040f, 0.040f, 0.060f, 1.00f);

        // Frame/input backgrounds
        c[ImGuiCol_FrameBg]              = ImVec4(0.130f, 0.130f, 0.170f, 1.00f);
        c[ImGuiCol_FrameBgHovered]       = ImVec4(0.170f, 0.170f, 0.220f, 1.00f);
        c[ImGuiCol_FrameBgActive]        = ImVec4(0.200f, 0.200f, 0.260f, 1.00f);

        // Buttons - green accent
        c[ImGuiCol_Button]               = ImVec4(0.298f, 0.686f, 0.314f, 1.00f);  // rgb(76, 175, 80)
        c[ImGuiCol_ButtonHovered]         = ImVec4(0.400f, 0.733f, 0.416f, 1.00f);  // rgb(102, 187, 106)
        c[ImGuiCol_ButtonActive]          = ImVec4(0.239f, 0.600f, 0.255f, 1.00f);  // rgb(61, 153, 65)

        // Headers
        c[ImGuiCol_Header]               = ImVec4(0.150f, 0.150f, 0.200f, 1.00f);
        c[ImGuiCol_HeaderHovered]        = ImVec4(0.200f, 0.200f, 0.260f, 1.00f);
        c[ImGuiCol_HeaderActive]         = ImVec4(0.240f, 0.240f, 0.320f, 1.00f);

        // Separator
        c[ImGuiCol_Separator]            = ImVec4(0.180f, 0.180f, 0.240f, 0.60f);
        c[ImGuiCol_SeparatorHovered]     = ImVec4(0.298f, 0.686f, 0.314f, 0.80f);
        c[ImGuiCol_SeparatorActive]      = ImVec4(0.298f, 0.686f, 0.314f, 1.00f);

        // Scrollbar
        c[ImGuiCol_ScrollbarBg]          = ImVec4(0.060f, 0.060f, 0.080f, 0.50f);
        c[ImGuiCol_ScrollbarGrab]        = ImVec4(0.200f, 0.200f, 0.260f, 1.00f);
        c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.260f, 0.260f, 0.340f, 1.00f);
        c[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.320f, 0.320f, 0.420f, 1.00f);

        // Progress bar
        c[ImGuiCol_PlotHistogram]        = ImVec4(0.298f, 0.686f, 0.314f, 1.00f);  // green

        // Text
        c[ImGuiCol_Text]                 = ImVec4(0.902f, 0.902f, 0.902f, 1.00f);  // rgb(230, 230, 230)
        c[ImGuiCol_TextDisabled]         = ImVec4(0.627f, 0.627f, 0.667f, 1.00f);  // rgb(160, 160, 170)

        // Misc
        c[ImGuiCol_Border]               = ImVec4(0.180f, 0.180f, 0.240f, 0.40f);
        c[ImGuiCol_CheckMark]            = ImVec4(0.298f, 0.686f, 0.314f, 1.00f);
        c[ImGuiCol_SliderGrab]           = ImVec4(0.298f, 0.686f, 0.314f, 1.00f);
        c[ImGuiCol_SliderGrabActive]     = ImVec4(0.400f, 0.733f, 0.416f, 1.00f);
    }

    void LoadLauncherFonts(GLFWwindow* window, const std::string& fontDir) {
        ImGuiIO& io = ImGui::GetIO();

        // Get DPI scale for retina displays
        float xscale = 1.0f, yscale = 1.0f;
        glfwGetWindowContentScale(window, &xscale, &yscale);
        float dpiScale = xscale; // Use horizontal scale

        std::string fontPath = fontDir + "/Roboto-Medium.ttf";

        if (std::filesystem::exists(fontPath)) {
            Log::Info("Loading font: %s (DPI scale: %.1f)", fontPath.c_str(), dpiScale);

            g_fontSmall  = io.Fonts->AddFontFromFileTTF(fontPath.c_str(), 14.0f * dpiScale);
            g_fontBody   = io.Fonts->AddFontFromFileTTF(fontPath.c_str(), 18.0f * dpiScale);
            g_fontButton = io.Fonts->AddFontFromFileTTF(fontPath.c_str(), 22.0f * dpiScale);
            g_fontTitle  = io.Fonts->AddFontFromFileTTF(fontPath.c_str(), 32.0f * dpiScale);

            io.FontGlobalScale = 1.0f / dpiScale;
        } else {
            Log::Warning("Font not found: %s, using ImGui default", fontPath.c_str());
            g_fontBody = io.Fonts->AddFontDefault();
            g_fontSmall = g_fontBody;
            g_fontButton = g_fontBody;
            g_fontTitle = g_fontBody;
        }
    }

} // namespace Launcher
