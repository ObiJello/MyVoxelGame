// File: src/launcher/ui/LauncherWidgets.hpp
//
// The launcher design's drawing primitives — device-snapped text, tracked
// small-caps labels, pills, the primary button, the pill toggle, styled
// inputs and the segmented tab control — for views that live outside
// LauncherUI.cpp (the Appearance view and the skin editor). They reproduce
// the helpers in LauncherUI.cpp's anonymous namespace exactly (same metrics,
// same emboldening), so the new views read as part of the same design.
// All coordinates are design pixels (see LauncherUI.cpp's header note).
#pragma once

#include <imgui.h>

#include <cstddef>
#include <string>

namespace Launcher::Widgets {

    // ── Text ──
    ImFont* F(ImFont* f);
    float   FontPx(ImFont* f);
    ImVec2  Measure(ImFont* f, const char* text, float wrapW = 0.0f);
    ImVec2  Snap(const ImVec2& p);
    void    Txt(ImDrawList* dl, ImFont* f, const ImVec2& pos, ImU32 col,
                const char* text, float wrapW = 0.0f);
    float   MeasureTracked(ImFont* f, const char* text, float tracking);
    void    TxtTracked(ImDrawList* dl, ImFont* f, const ImVec2& pos, ImU32 col,
                       const char* text, float tracking);
    std::string Ellipsize(ImFont* f, const std::string& s, float maxW);

    // Scoped PushFont that tolerates null (theme fallback fonts).
    struct FontScope {
        bool pushed;
        explicit FontScope(ImFont* f) : pushed(f != nullptr) { if (pushed) ImGui::PushFont(f); }
        ~FontScope() { if (pushed) ImGui::PopFont(); }
        FontScope(const FontScope&) = delete;
        FontScope& operator=(const FontScope&) = delete;
    };

    // ── Controls ──

    // 34×19 pill toggle at the cursor. True when clicked (caller flips).
    bool Toggle(const char* id, bool value);

    // Text pill button sized to its label, at the cursor. True when clicked.
    bool Pill(const char* id, const char* label, ImFont* font, ImVec2 pad,
              ImU32 bg, ImU32 bgHover, ImU32 fg, float rounding,
              ImU32 border = 0, bool enabled = true, float tracking = 0.0f);
    // Pill's size for a label, to right-align or centre one.
    ImVec2 PillSize(const char* label, ImFont* font, ImVec2 pad, float tracking = 0.0f);

    // The secondary (outline) button of the design: BgActive fill, TextBody
    // label, at `pos` with an exact size. True when clicked.
    bool Secondary(const char* id, const char* label, ImVec2 pos, ImVec2 size,
                   bool enabled = true, ImFont* font = nullptr);

    // Full-width primary action button (accent fill, tracked bold label).
    bool Primary(const char* id, const char* label, ImVec2 pos, ImVec2 size,
                 bool enabled, ImFont* font = nullptr, float tracking = 1.5f);

    // Styled single-line text input of exact pixel height at the cursor.
    bool Input(const char* id, const char* hint, char* buf, size_t bufSize,
               float width, float height, ImFont* font,
               ImGuiInputTextFlags flags = 0, ImGuiInputTextCallback cb = nullptr,
               float rounding = 10.0f, float padX = 14.0f);

    // A click-to-open select: a field at `pos` (exact `size`) showing the
    // selected option's label and a chevron; a click opens a menu of the
    // `count` options (label + one-line note) under the field — or over it
    // when the window has no room below — with the selection marked. Picking
    // one closes it; a click elsewhere closes it unchanged. Returns the
    // picked index, -1 otherwise.
    int Dropdown(const char* id, const char* const* labels, const char* const* notes, int count,
                 int selected, ImVec2 pos, ImVec2 size);

    // The Settings view's tab box: a bordered rail-coloured box holding
    // pills, one active. Drawn with its TOP-RIGHT corner at `topRight` when
    // `alignRight`, else its top-left at `pos`. Returns the clicked index or
    // -1. `outSize` (optional) gets the box size.
    int Segmented(const char* id, const char* const* labels, int count, int active,
                  ImVec2 pos, bool alignRight, ImVec2* outSize = nullptr);

    // A section label: the tracked mono small-caps line the design puts over
    // groups ("NEXT SLOTS", "SETTINGS").
    void SectionLabel(ImDrawList* dl, const ImVec2& pos, const char* text);

    // A one-line hover tooltip in the design's small font.
    void Tooltip(const char* text);

} // namespace Launcher::Widgets
