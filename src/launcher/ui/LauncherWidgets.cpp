// File: src/launcher/ui/LauncherWidgets.cpp
#define IMGUI_DEFINE_MATH_OPERATORS
#include "LauncherWidgets.hpp"
#include "LauncherTheme.hpp"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>

namespace Launcher::Widgets {

    using namespace Palette;

    // macOS font smoothing widens glyph stems; the design canvas renders with
    // it, so text is emboldened geometrically (LauncherUI.cpp's kEmbolden).
    constexpr float kEmbolden = 0.30f;

    ImFont* F(ImFont* f) { return f ? f : ImGui::GetFont(); }

    float FontPx(ImFont* f) {
        return f ? f->FontSize * ImGui::GetIO().FontGlobalScale : ImGui::GetFontSize();
    }

    ImVec2 Measure(ImFont* f, const char* text, float wrapW) {
        return F(f)->CalcTextSizeA(FontPx(f), FLT_MAX, wrapW, text);
    }

    ImVec2 Snap(const ImVec2& p) {
        float s = ImGui::GetIO().DisplayFramebufferScale.y;
        if (s <= 0.0f) s = 1.0f;
        return ImVec2(std::round(p.x * s) / s, std::round(p.y * s) / s);
    }

    void Txt(ImDrawList* dl, ImFont* f, const ImVec2& pos, ImU32 col, const char* text, float wrapW) {
        const ImVec2 sp = Snap(pos);
        dl->AddText(F(f), FontPx(f), sp, col, text, nullptr, wrapW);
        dl->AddText(F(f), FontPx(f), ImVec2(sp.x + kEmbolden, sp.y), col, text, nullptr, wrapW);
        dl->AddText(F(f), FontPx(f), ImVec2(sp.x, sp.y + kEmbolden), col, text, nullptr, wrapW);
    }

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

    std::string Ellipsize(ImFont* f, const std::string& s, float maxW) {
        if (Measure(f, s.c_str()).x <= maxW) return s;
        std::string out = s;
        while (!out.empty() && Measure(f, (out + "...").c_str()).x > maxW) out.pop_back();
        return out + "...";
    }

    bool Toggle(const char* id, bool value) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const bool pressed = ImGui::InvisibleButton(id, ImVec2(34, 19));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p, p + ImVec2(34, 19), value ? Accent : SwitchOff, 9.5f);
        const float cx = value ? p.x + 34 - 2.5f - 7.0f : p.x + 2.5f + 7.0f;
        dl->AddCircleFilled(ImVec2(cx, p.y + 9.5f), 7.0f, Knob);
        return pressed;
    }

    ImVec2 PillSize(const char* label, ImFont* font, ImVec2 pad, float tracking) {
        const float w = tracking > 0.0f ? MeasureTracked(font, label, tracking) : Measure(font, label).x;
        return ImVec2(w + pad.x * 2.0f, Measure(font, label).y + pad.y * 2.0f);
    }

    bool Pill(const char* id, const char* label, ImFont* font, ImVec2 pad,
              ImU32 bg, ImU32 bgHover, ImU32 fg, float rounding,
              ImU32 border, bool enabled, float tracking) {
        const ImVec2 size = PillSize(label, font, pad, tracking);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const bool pressed = ImGui::InvisibleButton(id, size) && enabled;
        const bool hov = ImGui::IsItemHovered() && enabled;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImU32 fill = hov && bgHover ? bgHover : bg;
        if ((fill & IM_COL32_A_MASK) != 0) dl->AddRectFilled(p, p + size, fill, rounding);
        if ((border & IM_COL32_A_MASK) != 0) dl->AddRect(p, p + size, border, rounding);
        if (tracking > 0.0f) TxtTracked(dl, font, p + pad, fg, label, tracking);
        else Txt(dl, font, p + pad, fg, label);
        return pressed;
    }

    bool Secondary(const char* id, const char* label, ImVec2 pos, ImVec2 size, bool enabled, ImFont* font) {
        ImFont* f = font ? font : g_fontSmallSemi;
        ImGui::SetCursorScreenPos(pos);
        const bool pressed = ImGui::InvisibleButton(id, size) && enabled;
        const bool hov = ImGui::IsItemHovered() && enabled;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(pos, pos + size, !enabled ? BgOffline : hov ? BgActiveHov : BgActive, 9.0f);
        const ImVec2 ts = Measure(f, label);
        Txt(dl, f, pos + (size - ts) * 0.5f, enabled ? TextBody : TextGhost, label);
        return pressed;
    }

    bool Primary(const char* id, const char* label, ImVec2 pos, ImVec2 size,
                 bool enabled, ImFont* font, float tracking) {
        ImFont* f = font ? font : g_fontButton;
        ImGui::SetCursorScreenPos(pos);
        const bool pressed = ImGui::InvisibleButton(id, size) && enabled;
        const bool hov = ImGui::IsItemHovered() && enabled;
        const bool act = ImGui::IsItemActive() && enabled;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImU32 bg = !enabled ? BgActive : act ? AccentActive : hov ? AccentHover : Accent;
        dl->AddRectFilled(pos, pos + size, bg, 11.0f);
        const ImVec2 ts(MeasureTracked(f, label, tracking), Measure(f, label).y);
        TxtTracked(dl, f, pos + (size - ts) * 0.5f, enabled ? OnAccent : TextFaint, label, tracking);
        return pressed;
    }

    bool Input(const char* id, const char* hint, char* buf, size_t bufSize,
               float width, float height, ImFont* font,
               ImGuiInputTextFlags flags, ImGuiInputTextCallback cb,
               float rounding, float padX, bool enabled) {
        FontScope f(font);
        const float textH = ImGui::GetTextLineHeight();
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(padX, (height - textH) * 0.5f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, rounding);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(enabled ? TextPrimary : TextGhost));
        ImGui::PushStyleColor(ImGuiCol_TextDisabled, ImGui::ColorConvertU32ToFloat4(TextGhost));
        // Disabled: ImGui dims the frame and the text and ignores the mouse
        // and keyboard for it.
        if (!enabled) ImGui::BeginDisabled(true);
        ImGui::SetNextItemWidth(width);
        const bool changed = ImGui::InputTextWithHint(id, hint, buf, bufSize, flags, cb);
        if (!enabled) ImGui::EndDisabled();
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(2);
        return changed && enabled;
    }

    int Segmented(const char* id, const char* const* labels, int count, int active,
                  ImVec2 pos, bool alignRight, ImVec2* outSize) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        // LauncherUI's Settings tabs: 7/14 padding pills, a 3px padded,
        // 1px bordered rail box, 2px between pills.
        const float tabH = Measure(g_fontSmallMed, "G").y + 14.0f;
        float tabsW = 0.0f;
        for (int i = 0; i < count; ++i) tabsW += Measure(g_fontSmallMed, labels[i]).x + 28.0f;
        tabsW += 2.0f * static_cast<float>(count > 1 ? count - 1 : 0);
        const ImVec2 boxSize(tabsW + 8.0f, tabH + 8.0f);
        const ImVec2 boxPos = alignRight ? ImVec2(pos.x - boxSize.x, pos.y) : pos;
        if (outSize) *outSize = boxSize;
        dl->AddRectFilled(boxPos, boxPos + boxSize, Rail, 10.0f);
        dl->AddRect(boxPos, boxPos + boxSize, Border, 10.0f);

        int clickedIndex = -1;
        float tx = boxPos.x + 4.0f;
        ImGui::PushID(id);
        for (int i = 0; i < count; ++i) {
            const ImVec2 ts = Measure(g_fontSmallMed, labels[i]);
            const ImVec2 size(ts.x + 28.0f, tabH);
            const ImVec2 tp(tx, boxPos.y + 4.0f);
            ImGui::SetCursorScreenPos(tp);
            ImGui::PushID(i);
            const bool clicked = ImGui::InvisibleButton("##seg", size);
            const bool hovered = ImGui::IsItemHovered();
            ImGui::PopID();
            if (i == active) dl->AddRectFilled(tp, tp + size, BgActive, 7.0f);
            else if (hovered) dl->AddRectFilled(tp, tp + size, BgHover, 7.0f);
            Txt(dl, g_fontSmallMed, ImVec2(tx + 14.0f, tp.y + (size.y - ts.y) * 0.5f),
                i == active ? TextPrimary : TextBody, labels[i]);
            if (clicked) clickedIndex = i;
            tx += size.x + 2.0f;
        }
        ImGui::PopID();
        return clickedIndex;
    }

    int Dropdown(const char* id, const char* const* labels, const char* const* notes, int count,
                 int selected, ImVec2 pos, ImVec2 size) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImGui::PushID(id);
        const bool open = ImGui::IsPopupOpen("##menu");

        // ── The field: rail fill, the selection, a chevron ──
        ImGui::SetCursorScreenPos(pos);
        if (ImGui::InvisibleButton("##field", size)) ImGui::OpenPopup("##menu");
        const bool hovered = ImGui::IsItemHovered();
        dl->AddRectFilled(pos, pos + size, hovered || open ? BgHover : Rail, 9.0f);
        dl->AddRect(pos, pos + size, open ? Accent : hovered ? BorderHover : Border, 9.0f);
        const char* current = selected >= 0 && selected < count ? labels[selected] : "";
        const ImVec2 ts = Measure(g_fontSmallMed, current);
        Txt(dl, g_fontSmallMed, ImVec2(pos.x + 12.0f, pos.y + (size.y - ts.y) * 0.5f), TextPrimary, current);
        {
            // Down when closed, up when open.
            const ImVec2 c(pos.x + size.x - 15.0f, pos.y + size.y * 0.5f);
            const float dy = open ? -2.0f : 2.0f;
            dl->AddLine(ImVec2(c.x - 4.0f, c.y - dy), ImVec2(c.x, c.y + dy), TextMuted, 1.5f);
            dl->AddLine(ImVec2(c.x, c.y + dy), ImVec2(c.x + 4.0f, c.y - dy), TextMuted, 1.5f);
        }

        // ── The menu ──
        constexpr float kPad = 5.0f;
        constexpr float kRowH = 40.0f;
        const float menuH = kPad * 2.0f + kRowH * static_cast<float>(count);
        const float below = pos.y + size.y + 4.0f;
        const bool fitsBelow = below + menuH <= ImGui::GetIO().DisplaySize.y - 4.0f;
        const float menuW = std::max(size.x, 200.0f);
        ImGui::SetNextWindowPos(ImVec2(pos.x + size.x - menuW, fitsBelow ? below : pos.y - 4.0f - menuH));
        ImGui::SetNextWindowSize(ImVec2(menuW, menuH));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(kPad, kPad));
        ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 10.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
        ImGui::PushStyleColor(ImGuiCol_PopupBg, ImGui::ColorConvertU32ToFloat4(Rail));
        ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(Border));
        int picked = -1;
        if (ImGui::BeginPopup("##menu", ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings)) {
            ImDrawList* mdl = ImGui::GetWindowDrawList();
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            const float rowW = menuW - kPad * 2.0f;
            for (int i = 0; i < count; ++i) {
                const ImVec2 rp(origin.x, origin.y + kRowH * static_cast<float>(i));
                ImGui::SetCursorScreenPos(rp);
                ImGui::PushID(i);
                const bool clicked = ImGui::InvisibleButton("##opt", ImVec2(rowW, kRowH));
                const bool rowHover = ImGui::IsItemHovered();
                ImGui::PopID();
                if (i == selected) mdl->AddRectFilled(rp, rp + ImVec2(rowW, kRowH), BgActive, 7.0f);
                else if (rowHover) mdl->AddRectFilled(rp, rp + ImVec2(rowW, kRowH), BgHover, 7.0f);
                Txt(mdl, g_fontSmallMed, rp + ImVec2(11.0f, 5.0f), i == selected ? TextPrimary : TextBody, labels[i]);
                if (notes && notes[i]) Txt(mdl, g_fontMono10, rp + ImVec2(11.0f, 23.0f), TextFaint, notes[i]);
                if (i == selected) {
                    // An accent check mark on the right.
                    const ImVec2 c(rp.x + rowW - 17.0f, rp.y + kRowH * 0.5f);
                    mdl->AddLine(ImVec2(c.x - 5.0f, c.y), ImVec2(c.x - 1.5f, c.y + 3.5f), Accent, 2.0f);
                    mdl->AddLine(ImVec2(c.x - 1.5f, c.y + 3.5f), ImVec2(c.x + 5.0f, c.y - 4.0f), Accent, 2.0f);
                }
                if (clicked) {
                    picked = i;
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::EndPopup();
        }
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(4);
        ImGui::PopID();
        return picked;
    }

    void SectionLabel(ImDrawList* dl, const ImVec2& pos, const char* text) {
        TxtTracked(dl, g_fontMono95, pos, TextFaint, text, 1.9f);
    }

    void Tooltip(const char* text) {
        if (ImGui::BeginTooltip()) {
            {
                // Pop the font BEFORE EndTooltip — End() asserts on fonts
                // still pushed within the window.
                FontScope f(g_fontSmall);
                ImGui::TextUnformatted(text);
            }
            ImGui::EndTooltip();
        }
    }

} // namespace Launcher::Widgets
