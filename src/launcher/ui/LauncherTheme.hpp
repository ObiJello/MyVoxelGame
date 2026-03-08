// File: src/launcher/ui/LauncherTheme.hpp
#pragma once

#include <string>
#include <imgui.h>

struct GLFWwindow;

namespace Launcher {

    // Font pointers (set by LoadLauncherFonts)
    extern ImFont* g_fontTitle;
    extern ImFont* g_fontBody;
    extern ImFont* g_fontSmall;
    extern ImFont* g_fontButton;

    // Apply the custom dark launcher theme to ImGui
    void ApplyLauncherTheme();

    // Load fonts for the launcher. Call after ImGui context is created.
    // fontDir should point to where Roboto-Medium.ttf lives.
    void LoadLauncherFonts(GLFWwindow* window, const std::string& fontDir);

} // namespace Launcher
