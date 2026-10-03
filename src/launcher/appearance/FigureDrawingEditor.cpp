// File: src/launcher/appearance/FigureDrawingEditor.cpp
#define IMGUI_DEFINE_MATH_OPERATORS
#include "FigureDrawingEditor.hpp"
#include "launcher/LauncherConfig.hpp"
#include "launcher/ui/LauncherTheme.hpp"
#include "launcher/ui/LauncherWidgets.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace Launcher::Appearance {

    using namespace Palette;
    using namespace Widgets;

    namespace {

        using Drawing = Game::StickFigureDrawing;
        // The canvas in grid units (x from the left as seen from the front,
        // y up from the feet) and grid units per block / per mm of radius.
        constexpr float GW = static_cast<float>(Drawing::kGridWidth);
        constexpr float GH = static_cast<float>(Drawing::kGridHeight);
        constexpr float G = static_cast<float>(Drawing::kGridPerBlock);
        constexpr float kGridPerMm = static_cast<float>(Drawing::kGridPerBlock) /
                                     static_cast<float>(Drawing::kRadiusPerBlock);
        constexpr float kPi = 3.14159265f;

        constexpr size_t kUndoDepth = 64;

        // Layout (design px, the 800×500 window).
        constexpr float kPad = 18.0f;
        constexpr float kTopH = 56.0f;
        constexpr float kColumnW = 196.0f;
        constexpr float kPreviewW = 214.0f;
        constexpr float kGap = 14.0f;

        // The canvas view's zoom range (design px per block).
        constexpr float kMinZoom = 80.0f;
        constexpr float kMaxZoom = 1500.0f;

        struct ToolInfo {
            const char* name;
            const char* tip;
            const char* hint;
        };
        constexpr ToolInfo kTools[] = {
            { "Brush",  "Brush (B)",                          "Click and drag to draw. [ and ] change the size." },
            { "Eraser", "Eraser (E)",                         "Drag over strokes to erase what it covers." },
            { "Line",   "Line (L)",                           "Drag from one end to the other. Shift snaps the angle." },
            { "Rect",   "Rectangle (R)",                      "Drag from corner to corner. Shift for a square." },
            { "Circle", "Circle or ellipse (O)",              "Drag across its box. Shift for a circle." },
            { "Pick",   "Eyedropper: colour and size (I)",    "Click a stroke to take its colour and size." },
        };

        ImU32 ColorOf(Game::PlayerColorId id, int alpha = 255) {
            const auto& e = Game::LookupPlayerColor(id);
            return IM_COL32(e.r, e.g, e.b, alpha);
        }

        float Dot(const ImVec2& a, const ImVec2& b) { return a.x * b.x + a.y * b.y; }
        float Length(const ImVec2& a) { return std::sqrt(Dot(a, a)); }
        ImVec2 Lerp(const ImVec2& a, const ImVec2& b, float t) { return a + (b - a) * t; }
        ImVec2 ToVec(const Drawing::Point& p) { return ImVec2(static_cast<float>(p.x), static_cast<float>(p.y)); }

        float PointSegmentDistance(const ImVec2& p, const ImVec2& a, const ImVec2& b) {
            const ImVec2 ab = b - a;
            const float len2 = Dot(ab, ab);
            const float t = len2 > 1e-9f ? std::clamp(Dot(p - a, ab) / len2, 0.0f, 1.0f) : 0.0f;
            return Length(p - (a + ab * t));
        }

        // The part of [p, q] within `reach` of segment [a, b], as t0 .. t1.
        // The distance is convex along [p, q] (a point moving on a line,
        // measured to a convex set), so the part is one interval: find the
        // nearest point, then each edge by bisection.
        bool CoveredInterval(const ImVec2& p, const ImVec2& q, const ImVec2& a, const ImVec2& b, float reach,
                             float& t0, float& t1) {
            const auto f = [&](float t) { return PointSegmentDistance(Lerp(p, q, t), a, b); };
            float lo = 0.0f, hi = 1.0f;
            for (int i = 0; i < 40; ++i) {
                const float m1 = lo + (hi - lo) / 3.0f, m2 = hi - (hi - lo) / 3.0f;
                if (f(m1) < f(m2)) hi = m2;
                else               lo = m1;
            }
            const float tm = (lo + hi) * 0.5f;
            if (f(tm) >= reach) return false;
            t0 = 0.0f;
            if (f(0.0f) >= reach) {
                float out = 0.0f, in = tm;
                for (int i = 0; i < 28; ++i) {
                    const float m = (out + in) * 0.5f;
                    (f(m) < reach ? in : out) = m;
                }
                t0 = in;
            }
            t1 = 1.0f;
            if (f(1.0f) >= reach) {
                float in = tm, out = 1.0f;
                for (int i = 0; i < 28; ++i) {
                    const float m = (out + in) * 0.5f;
                    (f(m) < reach ? in : out) = m;
                }
                t1 = in;
            }
            return true;
        }

        // Cuts away every part of `d`'s strokes whose ink lies under the
        // eraser swept from `a` to `b` (radius `radius`, grid units). A
        // stroke is cut where its centre line comes within the eraser's
        // radius plus its own, so what is left ends, round, at the eraser's
        // edge. True when anything changed.
        bool EraseCapsule(Drawing& d, const ImVec2& a, const ImVec2& b, float radius) {
            bool changed = false;
            std::vector<Drawing::Stroke> out;
            out.reserve(d.strokes.size() + 4);
            const ImVec2 lo(std::min(a.x, b.x), std::min(a.y, b.y)), hi(std::max(a.x, b.x), std::max(a.y, b.y));
            for (Drawing::Stroke& s : d.strokes) {
                const float reach = radius + static_cast<float>(s.radius) * kGridPerMm;
                const auto outside = [&](const ImVec2& p0, const ImVec2& p1) {
                    return std::max(p0.x, p1.x) < lo.x - reach || std::min(p0.x, p1.x) > hi.x + reach ||
                           std::max(p0.y, p1.y) < lo.y - reach || std::min(p0.y, p1.y) > hi.y + reach;
                };
                if (s.points.size() == 1) {
                    const ImVec2 p = ToVec(s.points[0]);
                    if (!outside(p, p) && PointSegmentDistance(p, a, b) < reach) {
                        changed = true;
                        continue;
                    }
                    out.push_back(std::move(s));
                    continue;
                }
                std::vector<std::vector<ImVec2>> pieces;
                std::vector<ImVec2> current;
                bool touched = false;
                for (size_t i = 0; i + 1 < s.points.size(); ++i) {
                    const ImVec2 p = ToVec(s.points[i]), q = ToVec(s.points[i + 1]);
                    float t0 = 0.0f, t1 = 1.0f;
                    if (outside(p, q) || !CoveredInterval(p, q, a, b, reach, t0, t1)) {
                        if (current.empty()) current.push_back(p);
                        current.push_back(q);
                        continue;
                    }
                    touched = true;
                    if (t0 > 0.0f) {
                        if (current.empty()) current.push_back(p);
                        current.push_back(Lerp(p, q, t0));
                    }
                    if (current.size() >= 2) pieces.push_back(current);
                    current.clear();
                    if (t1 < 1.0f) {
                        current.push_back(Lerp(p, q, t1));
                        current.push_back(q);
                    }
                }
                if (current.size() >= 2) pieces.push_back(current);
                if (!touched) {
                    out.push_back(std::move(s));
                    continue;
                }
                changed = true;
                // A closed loop cut once is still one stroke: its last piece
                // runs on into its first.
                const ImVec2 first = ToVec(s.points.front()), last = ToVec(s.points.back());
                if (s.points.size() > 2 && s.points.front() == s.points.back() && pieces.size() >= 2) {
                    const ImVec2 f0 = pieces.front().front(), lN = pieces.back().back();
                    if (f0.x == first.x && f0.y == first.y && lN.x == last.x && lN.y == last.y) {
                        std::vector<ImVec2>& tail = pieces.back();
                        tail.insert(tail.end(), pieces.front().begin() + 1, pieces.front().end());
                        pieces.erase(pieces.begin());
                    }
                }
                for (const std::vector<ImVec2>& piece : pieces) {
                    float length = 0.0f;
                    for (size_t k = 1; k < piece.size(); ++k) length += Length(piece[k] - piece[k - 1]);
                    if (length < 1.0f) continue;   // a sliver between two eraser spots
                    Drawing::Stroke n;
                    n.color = s.color;
                    n.radius = s.radius;
                    for (const ImVec2& p : piece) {
                        const Drawing::Point gp = Drawing::ToPoint(p.x, p.y);
                        if (n.points.empty() || n.points.back() != gp) n.points.push_back(gp);
                    }
                    out.push_back(std::move(n));
                }
            }
            d.strokes = std::move(out);
            return changed;
        }

        bool FitsCaps(const Drawing& d) {
            return d.StrokeCount() <= Drawing::kMaxStrokes && d.PointCount() <= Drawing::kMaxPoints &&
                   d.EncodedSize() <= Drawing::kMaxEncodedBytes;
        }

        // Two passes of a (1, 2, 1) average over the mouse's path, the ends
        // kept: takes the hand's jitter out without rounding off the shape.
        std::vector<ImVec2> Smoothed(const std::vector<ImVec2>& path) {
            std::vector<ImVec2> out = path;
            if (out.size() < 3) return out;
            std::vector<ImVec2> prev;
            for (int pass = 0; pass < 2; ++pass) {
                prev = out;
                for (size_t i = 1; i + 1 < out.size(); ++i) {
                    out[i] = (prev[i - 1] + prev[i] * 2.0f + prev[i + 1]) * 0.25f;
                }
            }
            return out;
        }

        // A stroke on the canvas: a thick line per segment and a disc per
        // point — the round joints and ends the game draws.
        template <class ToScreen>
        void DrawStroke(ImDrawList* dl, const Drawing::Stroke& s, ToScreen&& toScreen, float zoom, ImU32 color) {
            const float r = std::max(0.5f, Drawing::RadiusBlocks(s.radius) * zoom);
            for (size_t i = 0; i + 1 < s.points.size(); ++i) {
                // Not AddLine: it nudges the line half a pixel, off the discs.
                dl->PathLineTo(toScreen(ToVec(s.points[i])));
                dl->PathLineTo(toScreen(ToVec(s.points[i + 1])));
                dl->PathStroke(color, ImDrawFlags_None, 2.0f * r);
            }
            for (const Drawing::Point& p : s.points) dl->AddCircleFilled(toScreen(ToVec(p)), r, color);
        }

    } // namespace

    void FigureDrawingEditor::Open(const Settings& settings, Game::PlayerColorId figureColor) {
        m_drawing = settings.drawing;
        m_saved = m_drawing;
        m_figurePaint = settings.painted ? settings.paint : Game::StickFigurePaint::Uniform(figureColor);
        // The guide: the plain figure (only its shape is drawn).
        Render::StickFigureStrokes(Game::StickFigurePaint::Uniform(Game::PlayerColorId::White), m_guide);
        m_brush = m_drawing.DominantColor(figureColor);
        m_tool = Tool::Brush;
        m_toolBeforePick = Tool::Brush;
        m_mirror = false;
        m_smooth = true;
        m_showGuide = true;
        m_undo.clear();
        m_redo.clear();
        m_zoom = 0.0f;
        m_drag = Drag::None;
        m_raw.clear();
        m_preview.yawDeg = 205.0f;
        m_preview.pitchDeg = 10.0f;
        m_preview.zoom = 1.0f;
        m_meshDirty = true;
        m_orbiting = false;
        m_status.clear();
        m_confirm = Confirm::None;
        m_confirmPending = false;
        m_open = true;
    }

    void FigureDrawingEditor::Close() {
        m_open = false;
        m_drag = Drag::None;
        m_raw.clear();
        m_undo.clear();
        m_redo.clear();
    }

    void FigureDrawingEditor::ReleaseGpu() {
        m_preview.Release();
    }

    // ── Edits ──

    void FigureDrawingEditor::SetStatus(const char* text) {
        m_status = text;
        m_statusTime = ImGui::GetTime();
    }

    void FigureDrawingEditor::BeginDrag() {
        m_dragStart = m_drawing;
        m_liveBegin = m_drawing.strokes.size();
        m_capHit = false;
    }

    void FigureDrawingEditor::EndDrag() {
        Commit(m_dragStart);
    }

    void FigureDrawingEditor::Commit(const Drawing& before) {
        m_meshDirty = true;
        if (m_drawing == before) return;
        m_undo.push_back(before);
        if (m_undo.size() > kUndoDepth) m_undo.erase(m_undo.begin());
        m_redo.clear();
    }

    void FigureDrawingEditor::Undo() {
        if (m_undo.empty()) return;
        m_redo.push_back(m_drawing);
        m_drawing = m_undo.back();
        m_undo.pop_back();
        m_meshDirty = true;
    }

    void FigureDrawingEditor::Redo() {
        if (m_redo.empty()) return;
        m_undo.push_back(m_drawing);
        m_drawing = m_redo.back();
        m_redo.pop_back();
        m_meshDirty = true;
    }

    int FigureDrawingEditor::RadiusMm() const {
        return std::clamp(static_cast<int>(std::lround(m_radius)), Drawing::kMinRadius, Drawing::kMaxRadius);
    }

    float FigureDrawingEditor::RadiusGrid() const {
        return static_cast<float>(RadiusMm()) * kGridPerMm;
    }

    Drawing::Stroke FigureDrawingEditor::MakeStroke(const std::vector<ImVec2>& path) const {
        Drawing::Stroke s;
        s.color = m_brush;
        s.radius = static_cast<uint8_t>(RadiusMm());
        // The whole stroke stays on the canvas: nothing below the feet,
        // nothing wider than the hitbox.
        const float r = RadiusGrid();
        for (const ImVec2& p : path) {
            const Drawing::Point gp = Drawing::ToPoint(std::clamp(p.x, r, GW - r), std::clamp(p.y, r, GH - r));
            if (s.points.empty() || s.points.back() != gp) s.points.push_back(gp);
        }
        return s;
    }

    void FigureDrawingEditor::AddStroke(Drawing& d, Drawing::Stroke stroke) const {
        if (stroke.points.empty()) return;
        Drawing::Stroke mirrored;
        if (m_mirror) {
            mirrored = stroke;
            for (Drawing::Point& p : mirrored.points) p.x = static_cast<uint16_t>(Drawing::MirrorX(p.x));
            // A stroke on the centre line is its own mirror image.
            std::vector<Drawing::Point> reversed(mirrored.points.rbegin(), mirrored.points.rend());
            if (mirrored.points == stroke.points || reversed == stroke.points) mirrored.points.clear();
        }
        d.strokes.push_back(std::move(stroke));
        if (!mirrored.points.empty()) d.strokes.push_back(std::move(mirrored));
    }

    void FigureDrawingEditor::RebuildBrush() {
        // Past a cap the stroke stops where it last fitted: the newest
        // mouse points come off until it fits (or, for its very first, no
        // stroke at all).
        for (;;) {
            m_drawing.strokes.resize(m_liveBegin);
            Drawing::Stroke s = MakeStroke(m_smooth ? Smoothed(m_raw) : m_raw);
            // What the hand drew, not every mouse sample: points closer than
            // half a screen pixel (at this zoom) to the line through their
            // neighbours go.
            const float tolerance = std::clamp(0.5f / std::max(m_zoom, 1.0f) * G, 0.5f, 6.0f);
            Drawing::Simplify(s.points, tolerance);
            AddStroke(m_drawing, std::move(s));
            if (FitsCaps(m_drawing)) break;
            m_capHit = true;
            SetStatus("That is as much as a drawing can hold. Erase or simplify something first.");
            if (m_raw.size() > 1) {
                m_raw.pop_back();
            } else {
                m_drawing.strokes.resize(m_liveBegin);
                break;
            }
        }
        m_meshDirty = true;
    }

    void FigureDrawingEditor::RebuildShape(const ImVec2& cursor, bool constrain) {
        m_drawing.strokes.resize(m_liveBegin);
        const ImVec2 a = m_anchor;
        ImVec2 b = cursor;
        std::vector<ImVec2> path;
        switch (m_tool) {
            case Tool::Line: {
                if (constrain) {
                    // Shift: the angle snapped to 15° steps.
                    const ImVec2 d = b - a;
                    const float len = Length(d);
                    const float step = kPi / 12.0f;
                    const float angle = std::round(std::atan2(d.y, d.x) / step) * step;
                    b = a + ImVec2(std::cos(angle), std::sin(angle)) * len;
                }
                path = { a, b };
                break;
            }
            case Tool::Rect:
            case Tool::Ellipse: {
                if (constrain) {
                    // Shift: a square box.
                    const ImVec2 d = b - a;
                    const float side = std::max(std::abs(d.x), std::abs(d.y));
                    b = a + ImVec2(d.x < 0.0f ? -side : side, d.y < 0.0f ? -side : side);
                }
                if (m_tool == Tool::Rect) {
                    path = { a, ImVec2(b.x, a.y), b, ImVec2(a.x, b.y), a };
                } else {
                    const ImVec2 c = (a + b) * 0.5f;
                    const float rx = std::abs(b.x - a.x) * 0.5f, ry = std::abs(b.y - a.y) * 0.5f;
                    // About 1.5 cm a side, 16 to 72 sides.
                    const int sides = std::clamp(static_cast<int>(2.0f * kPi * std::max(rx, ry) / 30.0f), 16, 72);
                    for (int k = 0; k <= sides; ++k) {
                        const float t = 2.0f * kPi * static_cast<float>(k) / static_cast<float>(sides);
                        path.push_back(c + ImVec2(std::cos(t) * rx, std::sin(t) * ry));
                    }
                }
                break;
            }
            default:
                return;
        }
        AddStroke(m_drawing, MakeStroke(path));
        if (!FitsCaps(m_drawing)) {
            m_drawing.strokes.resize(m_liveBegin);
            m_capHit = true;
            SetStatus("That is as much as a drawing can hold. Erase or simplify something first.");
        }
        m_meshDirty = true;
    }

    void FigureDrawingEditor::EraseAlong(const ImVec2& a, const ImVec2& b) {
        const Drawing before = m_drawing;
        const float r = RadiusGrid();
        bool changed = EraseCapsule(m_drawing, a, b, r);
        if (m_mirror) {
            const ImVec2 ma(GW - a.x, a.y), mb(GW - b.x, b.y);
            changed = EraseCapsule(m_drawing, ma, mb, r) || changed;
        }
        if (!changed) return;
        // Cutting a stroke in two adds one: at the caps, the cut waits.
        if (!FitsCaps(m_drawing)) {
            m_drawing = before;
            SetStatus("That is as much as a drawing can hold. Erase whole strokes first.");
            return;
        }
        m_meshDirty = true;
    }

    void FigureDrawingEditor::PickAt(const ImVec2& at) {
        for (auto it = m_drawing.strokes.rbegin(); it != m_drawing.strokes.rend(); ++it) {
            const float reach = static_cast<float>(it->radius) * kGridPerMm;
            bool hit = false;
            if (it->points.size() == 1) {
                hit = Length(at - ToVec(it->points[0])) <= reach;
            } else {
                for (size_t i = 0; i + 1 < it->points.size() && !hit; ++i) {
                    hit = PointSegmentDistance(at, ToVec(it->points[i]), ToVec(it->points[i + 1])) <= reach;
                }
            }
            if (!hit) continue;
            m_brush = it->color;
            m_radius = static_cast<float>(it->radius);
            m_tool = m_toolBeforePick == Tool::Pick || m_toolBeforePick == Tool::Eraser ? Tool::Brush
                                                                                       : m_toolBeforePick;
            return;
        }
        SetStatus("Nothing is drawn there: click a stroke.");
    }

    void FigureDrawingEditor::SelectTool(Tool tool) {
        if (tool == Tool::Pick && m_tool != Tool::Pick) m_toolBeforePick = m_tool;
        m_tool = tool;
    }

    void FigureDrawingEditor::HandleShortcuts() {
        if (ImGui::GetIO().WantTextInput) return;
        if (ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) return;
        const bool drawing = m_drag == Drag::Brush || m_drag == Drag::Erase || m_drag == Drag::Shape;
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z) ||
            ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Y)) {
            if (!drawing) Redo();
            return;
        }
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Z)) {
            if (!drawing) Undo();
            return;
        }
        // The size changes even mid-stroke for the next one; it is the
        // brush's, not the stroke's.
        if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket)) {
            m_radius = std::max(static_cast<float>(Drawing::kMinRadius), m_radius / 1.15f);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_RightBracket)) {
            m_radius = std::min(static_cast<float>(Drawing::kMaxRadius), m_radius * 1.15f);
        }
        if (drawing) return;
        if (ImGui::IsKeyChordPressed(ImGuiKey_B) || ImGui::IsKeyChordPressed(ImGuiKey_P)) SelectTool(Tool::Brush);
        if (ImGui::IsKeyChordPressed(ImGuiKey_E)) SelectTool(Tool::Eraser);
        if (ImGui::IsKeyChordPressed(ImGuiKey_L)) SelectTool(Tool::Line);
        if (ImGui::IsKeyChordPressed(ImGuiKey_R)) SelectTool(Tool::Rect);
        if (ImGui::IsKeyChordPressed(ImGuiKey_O) || ImGui::IsKeyChordPressed(ImGuiKey_C)) SelectTool(Tool::Ellipse);
        if (ImGui::IsKeyChordPressed(ImGuiKey_I)) SelectTool(Tool::Pick);
        if (ImGui::IsKeyChordPressed(ImGuiKey_M)) m_mirror = !m_mirror;
    }

    // ── Drawing ──

    bool FigureDrawingEditor::Draw(Settings& settings) {
        if (!m_open) return false;
        bool applied = false;

        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float W = static_cast<float>(WindowWidth), H = static_cast<float>(WindowHeight);
        dl->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), WindowBg);

        HandleShortcuts();
        DrawTopBar(settings, applied);
        if (!m_open) return applied;
        dl->AddLine(ImVec2(0, kTopH), ImVec2(W, kTopH), Border);

        const float top = kTopH + 10.0f, bottom = H - kPad;
        const float canvasX0 = kPad + kColumnW + kGap;
        const float previewX0 = W - kPad - kPreviewW;
        DrawToolColumn(ImVec2(kPad, top), kColumnW);
        DrawCanvas(ImVec2(canvasX0, top), ImVec2(previewX0 - kGap, bottom));
        DrawPreview(ImVec2(previewX0, top), ImVec2(W - kPad, bottom));
        DrawPopups();
        return applied;
    }

    void FigureDrawingEditor::DrawTopBar(Settings& settings, bool& applied) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float W = static_cast<float>(WindowWidth);
        const bool changed = m_drawing != m_saved;

        ImGui::SetCursorScreenPos(ImVec2(kPad, 19.0f));
        if (Pill("##drawingBack", "< APPEARANCE", g_fontMono10, ImVec2(2, 2), 0, 0, TextFaint, 0, 0, true, 1.6f)) {
            if (changed) {
                m_confirm = Confirm::Discard;
                m_confirmPending = true;
            } else {
                Close();
                return;
            }
        }
        TxtTracked(dl, g_fontMono10, ImVec2(140.0f, 13.0f), TextFaint, "DRAWING EDITOR", 2.0f);
        Txt(dl, g_fontH3, ImVec2(140.0f, 27.0f), TextPrimary, changed ? "Unsaved changes" : "Your drawn figure");

        const float doneW = 96.0f, stepW = 58.0f;
        float x = W - kPad - doneW;
        if (Primary("##drawingDone", "DONE", ImVec2(x, 12.0f), ImVec2(doneW, 32.0f), true, g_fontBtn14, 0.84f)) {
            applied = !settings.drawn || settings.drawing != m_drawing;
            settings.drawing = m_drawing;
            // Finished in the drawing editor: the drawing is what you wear.
            settings.drawn = true;
            Close();
            return;
        }
        x -= 10.0f + stepW;
        if (Secondary("##drawingRedo", "Redo", ImVec2(x, 13.0f), ImVec2(stepW, 30.0f), !m_redo.empty())) Redo();
        if (ImGui::IsItemHovered()) Tooltip("Redo (Ctrl+Shift+Z / Ctrl+Y)");
        x -= 6.0f + stepW;
        if (Secondary("##drawingUndo", "Undo", ImVec2(x, 13.0f), ImVec2(stepW, 30.0f), !m_undo.empty())) Undo();
        if (ImGui::IsItemHovered()) Tooltip("Undo (Ctrl+Z)");

        // How much of the detail caps the drawing uses, so nobody is surprised by them.
        const int percent = static_cast<int>(std::ceil(m_drawing.CapUse() * 100.0f));
        const int strokes = m_drawing.StrokeCount();
        char stats[64];
        std::snprintf(stats, sizeof(stats), "%d STROKE%s   DETAIL %d%%", strokes, strokes == 1 ? "" : "S", percent);
        const float sw = MeasureTracked(g_fontMono9, stats, 1.0f);
        TxtTracked(dl, g_fontMono9, ImVec2(x - 16.0f - sw, 23.0f), percent >= 90 ? Amber : TextFaint, stats, 1.0f);
    }

    void FigureDrawingEditor::DrawSizeSlider(const ImVec2& p0, float width) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImGuiIO& io = ImGui::GetIO();
        constexpr float kH = 22.0f;
        // Logarithmic: fine steps among the thin brushes, coarse among the fat.
        const float lo = std::log(static_cast<float>(Drawing::kMinRadius));
        const float hi = std::log(static_cast<float>(Drawing::kMaxRadius));
        const float x0 = p0.x + 8.0f, x1 = p0.x + width - 8.0f, cy = p0.y + kH * 0.5f;

        ImGui::SetCursorScreenPos(p0);
        ImGui::InvisibleButton("##drawingSize", ImVec2(width, kH));
        const bool hov = ImGui::IsItemHovered();
        const bool active = ImGui::IsItemActive();
        if (active) {
            const float t = std::clamp((io.MousePos.x - x0) / (x1 - x0), 0.0f, 1.0f);
            m_radius = std::exp(lo + t * (hi - lo));
        }
        if (hov && io.MouseWheel != 0.0f) {
            m_radius = std::clamp(m_radius * std::pow(1.1f, io.MouseWheel), static_cast<float>(Drawing::kMinRadius),
                                  static_cast<float>(Drawing::kMaxRadius));
        }
        const auto xOf = [&](float radius) { return x0 + (std::log(radius) - lo) / (hi - lo) * (x1 - x0); };
        const float kx = xOf(std::clamp(m_radius, static_cast<float>(Drawing::kMinRadius),
                                        static_cast<float>(Drawing::kMaxRadius)));
        dl->AddRectFilled(ImVec2(x0, cy - 2.0f), ImVec2(x1, cy + 2.0f), BgActive, 2.0f);
        dl->AddRectFilled(ImVec2(x0, cy - 2.0f), ImVec2(kx, cy + 2.0f), Accent, 2.0f);
        // The mark: the stick figure's own line.
        const float tx = xOf(static_cast<float>(Drawing::kLineRadius));
        dl->AddLine(ImVec2(tx, cy - 6.0f), ImVec2(tx, cy + 6.0f), TextFaint, 1.5f);
        dl->AddCircleFilled(ImVec2(kx, cy), active || hov ? 8.0f : 7.0f, TextPrimary);
        dl->AddCircle(ImVec2(kx, cy), active || hov ? 8.0f : 7.0f, Accent, 0, 1.5f);
        if (hov && !active) Tooltip("Brush size ([ and ]). The mark is the stick figure's own line.");
    }

    void FigureDrawingEditor::DrawToolColumn(const ImVec2& p0, float width) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        float y = p0.y;

        // ── Tools: three a row ──
        SectionLabel(dl, ImVec2(p0.x, y), "TOOLS");
        y += 16.0f;
        {
            constexpr int kCount = static_cast<int>(Tool::Count);
            const float gap = 6.0f;
            const float cellW = (width - 2.0f * gap) / 3.0f, cellH = 30.0f;
            for (int i = 0; i < kCount; ++i) {
                const ImVec2 c0(p0.x + static_cast<float>(i % 3) * (cellW + gap),
                                y + static_cast<float>(i / 3) * (cellH + gap));
                const ImVec2 c1 = c0 + ImVec2(cellW, cellH);
                ImGui::SetCursorScreenPos(c0);
                ImGui::PushID(i);
                const bool clicked = ImGui::InvisibleButton("##drawingTool", ImVec2(cellW, cellH));
                const bool hov = ImGui::IsItemHovered();
                ImGui::PopID();
                const bool on = static_cast<int>(m_tool) == i;
                dl->AddRectFilled(c0, c1, on ? BgActive : hov ? BgHover : Rail, 8.0f);
                dl->AddRect(c0, c1, on ? Accent : Border, 8.0f);
                const char* name = kTools[i].name;
                const ImVec2 ts = Measure(g_fontSmallMed, name);
                Txt(dl, g_fontSmallMed, ImVec2(c0.x + (cellW - ts.x) * 0.5f, c0.y + (cellH - ts.y) * 0.5f),
                    on ? TextPrimary : TextBody, name);
                if (hov) Tooltip(kTools[i].tip);
                if (clicked) SelectTool(static_cast<Tool>(i));
            }
            y += 2.0f * cellH + gap + 10.0f;
        }

        // ── Brush size: a continuous slider, the stroke's width shown ──
        {
            SectionLabel(dl, ImVec2(p0.x, y), "BRUSH SIZE");
            char size[32];
            std::snprintf(size, sizeof(size), "%.1f CM", static_cast<double>(RadiusMm()) * 2.0 / 10.0);
            const float sw = MeasureTracked(g_fontMono9, size, 1.0f);
            TxtTracked(dl, g_fontMono9, ImVec2(p0.x + width - sw, y), TextBody, size, 1.0f);
            y += 14.0f;
            DrawSizeSlider(ImVec2(p0.x, y), width);
            y += 22.0f + 8.0f;
        }

        // ── Options: mirror, smoothing, the guide ──
        {
            const auto toggleRow = [&](const char* id, const char* label, const char* tip, bool& value) {
                Txt(dl, g_fontSmall, ImVec2(p0.x, y + 2.0f), TextBody, label);
                ImGui::SetCursorScreenPos(ImVec2(p0.x + width - 34.0f, y));
                if (Toggle(id, value)) value = !value;
                if (ImGui::IsItemHovered()) Tooltip(tip);
                y += 25.0f;
            };
            toggleRow("##drawingMirror", "Mirror", "Repeat every stroke on the other half (M)", m_mirror);
            toggleRow("##drawingSmooth", "Smooth strokes", "Even out the hand's jitter as you draw", m_smooth);
            toggleRow("##drawingGuide", "Guide", "The player's height and the stick figure", m_showGuide);
            y += 6.0f;
        }

        // ── Colour: the stick-figure palette ──
        SectionLabel(dl, ImVec2(p0.x, y), "COLOUR");
        y += 16.0f;
        {
            const size_t count = sizeof(Game::kPlayerColorTable) / sizeof(Game::kPlayerColorTable[0]);
            constexpr int kPerRow = 5;
            const float gap = 6.0f;
            const float cell = std::min(28.0f, (width - gap * (kPerRow - 1)) / kPerRow);
            for (size_t i = 0; i < count; ++i) {
                const auto& entry = Game::kPlayerColorTable[i];
                const ImVec2 c0(p0.x + static_cast<float>(i % kPerRow) * (cell + gap),
                                y + static_cast<float>(i / kPerRow) * (cell + gap));
                ImGui::SetCursorScreenPos(c0);
                ImGui::PushID(static_cast<int>(i));
                const bool clicked = ImGui::InvisibleButton("##drawingColour", ImVec2(cell, cell));
                const bool hov = ImGui::IsItemHovered();
                ImGui::PopID();
                dl->AddRectFilled(c0, c0 + ImVec2(cell, cell), IM_COL32(entry.r, entry.g, entry.b, 255), 6.0f);
                if (entry.id == m_brush) {
                    dl->AddRect(c0 - ImVec2(2, 2), c0 + ImVec2(cell + 2, cell + 2), TextPrimary, 8.0f, 0, 2.0f);
                } else if (hov) {
                    dl->AddRect(c0 - ImVec2(2, 2), c0 + ImVec2(cell + 2, cell + 2), BorderHover, 8.0f, 0, 2.0f);
                }
                if (hov) Tooltip(entry.name);
                if (clicked) {
                    m_brush = entry.id;
                    if (m_tool == Tool::Eraser || m_tool == Tool::Pick) m_tool = Tool::Brush;
                }
            }
            const int rows = static_cast<int>((count + kPerRow - 1) / kPerRow);
            y += static_cast<float>(rows) * (cell + gap) + 6.0f;
        }

        // ── Start from the stick figure / clear ──
        {
            const float gap = 6.0f;
            const float bw0 = std::floor(width * 0.62f), bw1 = width - bw0 - gap;
            if (Secondary("##drawingFromFigure", "From stick figure", ImVec2(p0.x, y), ImVec2(bw0, 30.0f))) {
                m_confirm = Confirm::FromFigure;
                if (m_drawing.Empty()) {
                    // Nothing to lose: no question.
                    const Drawing before = m_drawing;
                    Render::StickFigureStrokes(m_figurePaint, m_drawing);
                    Commit(before);
                    m_confirm = Confirm::None;
                } else {
                    m_confirmPending = true;
                }
            }
            if (ImGui::IsItemHovered()) Tooltip("Your stick figure as strokes, ready to edit");
            if (Secondary("##drawingClear", "Clear", ImVec2(p0.x + bw0 + gap, y), ImVec2(bw1, 30.0f),
                          !m_drawing.Empty())) {
                m_confirm = Confirm::Clear;
                m_confirmPending = true;
            }
            if (ImGui::IsItemHovered()) Tooltip("Empty the canvas and draw from scratch");
            y += 30.0f + 10.0f;
        }

        // Status, or the hint for the tool.
        if (!m_status.empty() && ImGui::GetTime() - m_statusTime < 6.0) {
            Txt(dl, g_fontSmall, ImVec2(p0.x, y), AmberText, m_status.c_str(), width);
        } else {
            Txt(dl, g_fontSmall, ImVec2(p0.x, y), TextGhost, kTools[static_cast<int>(m_tool)].hint, width);
        }
    }

    void FigureDrawingEditor::FitCanvas(const ImVec2& viewSize, bool wholeCanvas) {
        // The whole 3 blocks, or the player's 1.8 and a little over it.
        const float rows = wholeCanvas ? Drawing::kHeightBlocks : Drawing::kPlayerHeightBlocks + 0.3f;
        const float zx = (viewSize.x - 48.0f) / Drawing::kWidthBlocks;
        const float zy = (viewSize.y - 28.0f) / rows;
        m_zoom = std::clamp(std::min(zx, zy), kMinZoom, kMaxZoom);
        m_pan = ImVec2(Drawing::kWidthBlocks * 0.5f, Drawing::kHeightBlocks - rows * 0.5f);
    }

    void FigureDrawingEditor::DrawCanvas(const ImVec2& p0, const ImVec2& p1) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImGuiIO& io = ImGui::GetIO();
        const ImVec2 size = p1 - p0;
        const ImVec2 centre = (p0 + p1) * 0.5f;
        if (m_zoom <= 0.0f) FitCanvas(size, false);
        const auto clampPan = [&]() {
            m_pan.x = std::clamp(m_pan.x, 0.0f, Drawing::kWidthBlocks);
            m_pan.y = std::clamp(m_pan.y, 0.0f, Drawing::kHeightBlocks);
        };
        const auto zoomAbout = [&](const ImVec2& screen, float factor) {
            const ImVec2 anchor = (screen - centre) / m_zoom + m_pan;   // the canvas point kept in place
            m_zoom = std::clamp(m_zoom * factor, kMinZoom, kMaxZoom);
            m_pan = anchor - (screen - centre) / m_zoom;
            clampPan();
        };

        dl->AddRectFilled(p0, p1, BgStripeA, 12.0f);

        // The view's controls go on a channel above the canvas, submitted
        // before the canvas's own button so they, not the canvas, take the
        // clicks over the corner they sit in.
        dl->ChannelsSplit(2);
        dl->ChannelsSetCurrent(1);
        {
            const ImVec2 pad(9, 4);
            float bx = p0.x + 10.0f;
            const float by = p0.y + 10.0f;
            const auto button = [&](const char* id, const char* label, const char* tip) {
                ImGui::SetCursorScreenPos(ImVec2(bx, by));
                const bool clicked = Pill(id, label, g_fontSmallMed, pad, BgActive, BgActiveHov, TextBody, 7.0f);
                if (ImGui::IsItemHovered()) Tooltip(tip);
                bx += PillSize(label, g_fontSmallMed, pad).x + 5.0f;
                return clicked;
            };
            if (button("##drawingFitAll", "All", "The whole canvas: 3 blocks")) FitCanvas(size, true);
            if (button("##drawingFitPlayer", "Player", "The player's 1.8 blocks")) FitCanvas(size, false);
            if (button("##drawingZoomOut", "-", "Zoom out (scroll)")) zoomAbout(centre, 1.0f / 1.4f);
            if (button("##drawingZoomIn", "+", "Zoom in (scroll)")) zoomAbout(centre, 1.4f);
        }
        dl->ChannelsSetCurrent(0);

        // Canvas ↔ screen. The view works in blocks from the canvas's top-
        // left; the drawing in grid units from its bottom-left.
        const auto toScreen = [&](const ImVec2& g) {
            const ImVec2 origin = centre - m_pan * m_zoom;
            return origin + ImVec2(g.x / G * m_zoom, (GH - g.y) / G * m_zoom);
        };
        const auto fromScreen = [&](const ImVec2& screen) {
            const ImVec2 c = (screen - centre) / m_zoom + m_pan;
            return ImVec2(c.x * G, GH - c.y * G);
        };

        // ── Input first, so this frame's canvas shows this frame's edit ──
        ImGui::SetCursorScreenPos(p0);
        ImGui::InvisibleButton("##drawingCanvas", size,
                               ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                                   ImGuiButtonFlags_MouseButtonMiddle);
        const bool hovered = ImGui::IsItemHovered();
        const bool active = ImGui::IsItemActive();
        const ImVec2 cursor = fromScreen(io.MousePos);
        const bool constrain = io.KeyShift;

        if (ImGui::IsItemActivated()) {
            const bool draw = ImGui::IsMouseDown(ImGuiMouseButton_Left) && !ImGui::IsKeyDown(ImGuiKey_Space);
            m_drag = Drag::None;
            if (!draw) {
                m_drag = Drag::Pan;
            } else {
                switch (m_tool) {
                    case Tool::Brush:
                        BeginDrag();
                        m_raw.assign(1, cursor);
                        RebuildBrush();
                        m_drag = Drag::Brush;
                        break;
                    case Tool::Eraser:
                        BeginDrag();
                        EraseAlong(cursor, cursor);
                        m_last = cursor;
                        m_drag = Drag::Erase;
                        break;
                    case Tool::Line:
                    case Tool::Rect:
                    case Tool::Ellipse:
                        BeginDrag();
                        m_anchor = cursor;
                        RebuildShape(cursor, constrain);
                        m_drag = Drag::Shape;
                        break;
                    case Tool::Pick:
                        PickAt(cursor);
                        break;
                    default:
                        break;
                }
            }
        }
        if (active) {
            switch (m_drag) {
                case Drag::Pan:
                    m_pan = m_pan - io.MouseDelta / m_zoom;
                    clampPan();
                    break;
                case Drag::Brush: {
                    // A new point once the mouse has moved a screen pixel;
                    // the stroke runs straight between samples.
                    const float step = G / m_zoom;
                    if (!m_capHit && !m_raw.empty() && Length(cursor - m_raw.back()) >= step) {
                        m_raw.push_back(cursor);
                        RebuildBrush();
                    }
                    break;
                }
                case Drag::Erase:
                    if (cursor.x != m_last.x || cursor.y != m_last.y) {
                        EraseAlong(m_last, cursor);
                        m_last = cursor;
                    }
                    break;
                case Drag::Shape:
                    // Every frame: the shape follows the cursor and Shift.
                    RebuildShape(cursor, constrain);
                    break;
                default:
                    break;
            }
        } else if (m_drag != Drag::None) {
            if (m_drag != Drag::Pan) EndDrag();
            m_drag = Drag::None;
            m_raw.clear();
        }
        if (hovered && io.MouseWheel != 0.0f) zoomAbout(io.MousePos, std::pow(1.15f, io.MouseWheel));

        // ── The canvas ──
        const float z = m_zoom;
        const ImVec2 origin = centre - m_pan * z;
        const ImVec2 extent(Drawing::kWidthBlocks * z, Drawing::kHeightBlocks * z);
        dl->PushClipRect(p0, p1, true);
        // The empty canvas as a dim checkerboard of 0.075-block squares.
        dl->AddRectFilled(origin, origin + extent, IM_COL32(0x1b, 0x1e, 0x26, 255));
        {
            constexpr float kSquare = 0.075f;
            const int cols = static_cast<int>(std::lround(Drawing::kWidthBlocks / kSquare));
            const int rows = static_cast<int>(std::lround(Drawing::kHeightBlocks / kSquare));
            for (int cy = 0; cy < rows; ++cy) {
                for (int cx = (cy & 1); cx < cols; cx += 2) {
                    const ImVec2 a = origin + ImVec2(static_cast<float>(cx) * kSquare * z,
                                                     static_cast<float>(cy) * kSquare * z);
                    dl->AddRectFilled(a, a + ImVec2(kSquare * z, kSquare * z), IM_COL32(0x21, 0x25, 0x2f, 255));
                }
            }
        }
        // The guide under the drawing: the stick figure, faint. One polyline
        // a stroke, so the overlaps of a translucent stroke do not darken.
        if (m_showGuide) {
            const ImU32 guide = IM_COL32(255, 255, 255, 30);
            std::vector<ImVec2> pts;
            for (const Drawing::Stroke& s : m_guide.strokes) {
                const float r = Drawing::RadiusBlocks(s.radius) * z;
                if (s.points.size() == 1) {
                    dl->AddCircleFilled(toScreen(ToVec(s.points[0])), r, guide);
                    continue;
                }
                const bool closed = s.points.size() > 2 && s.points.front() == s.points.back();
                pts.clear();
                for (size_t i = 0; i < s.points.size() - (closed ? 1 : 0); ++i) pts.push_back(toScreen(ToVec(s.points[i])));
                dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), guide,
                                closed ? ImDrawFlags_Closed : ImDrawFlags_None, 2.0f * r);
            }
        }
        // The strokes, in order: a later one over an earlier one.
        for (const Drawing::Stroke& s : m_drawing.strokes) DrawStroke(dl, s, toScreen, z, ColorOf(s.color));
        dl->AddRect(origin - ImVec2(1, 1), origin + extent + ImVec2(1, 1), BorderHover);
        // The guide's heights: the player's 1.8 blocks (dashed), the top and the feet.
        if (m_showGuide) {
            const ImU32 line = IM_COL32(0x4d, 0x8d, 0xff, 150);
            const float sy = origin.y + (Drawing::kHeightBlocks - Drawing::kPlayerHeightBlocks) * z;
            for (float sx = origin.x; sx < origin.x + extent.x; sx += 8.0f) {
                dl->AddLine(ImVec2(sx, sy), ImVec2(std::min(sx + 4.0f, origin.x + extent.x), sy), line, 1.5f);
            }
            const auto label = [&](float atY, const char* text) {
                const float lw = MeasureTracked(g_fontMono9, text, 0.8f);
                TxtTracked(dl, g_fontMono9, ImVec2(origin.x - 8.0f - lw, atY - 6.0f), TextFaint, text, 0.8f);
            };
            label(sy, "1.8");
            label(origin.y, "3.0");
            label(origin.y + extent.y, "0");
        }
        // The mirror's axis.
        if (m_mirror) {
            const float sx = origin.x + extent.x * 0.5f;
            dl->AddLine(ImVec2(sx, origin.y), ImVec2(sx, origin.y + extent.y), IM_COL32(0x4d, 0x8d, 0xff, 70));
        }
        // The brush under the cursor at its true size (and its mirror image).
        if ((hovered || active) && m_drag != Drag::Pan) {
            const ImVec2 at = toScreen(cursor);
            if (m_tool == Tool::Pick) {
                dl->AddCircle(at, 5.0f, IM_COL32(0xee, 0xf1, 0xf8, 235), 0, 1.5f);
            } else {
                const float r = std::max(1.0f, static_cast<float>(RadiusMm()) / static_cast<float>(Drawing::kRadiusPerBlock) * z);
                const bool eraser = m_tool == Tool::Eraser;
                const ImU32 fill = eraser ? IM_COL32(0xf0, 0x6a, 0x5e, 40) : ColorOf(m_brush, 80);
                const ImU32 ring = eraser ? IM_COL32(0xf0, 0x6a, 0x5e, 235) : IM_COL32(0xee, 0xf1, 0xf8, 220);
                const ImU32 ringMirror = eraser ? IM_COL32(0xf0, 0x6a, 0x5e, 110) : IM_COL32(0xee, 0xf1, 0xf8, 100);
                // A shape draws no ink until it is dragged: the ring alone.
                if (m_drag == Drag::None || m_drag == Drag::Erase) dl->AddCircleFilled(at, r, fill);
                dl->AddCircle(at, r, ring, 0, 1.5f);
                if (m_mirror) {
                    const ImVec2 mirrored = toScreen(ImVec2(GW - cursor.x, cursor.y));
                    if (std::abs(mirrored.x - at.x) > 1.0f) dl->AddCircle(mirrored, r, ringMirror, 0, 1.5f);
                }
            }
        }
        dl->PopClipRect();
        dl->AddRect(p0, p1, Border, 12.0f);

        // ── Caption: the height under the cursor, or how to move the view ──
        std::string caption;
        if (hovered && cursor.x >= 0.0f && cursor.x <= GW && cursor.y >= 0.0f && cursor.y <= GH) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "%.2f BLOCKS UP", static_cast<double>(cursor.y / G));
            caption = buf;
        } else {
            caption = "SCROLL TO ZOOM  -  RIGHT-DRAG TO MOVE";
        }
        TxtTracked(dl, g_fontMono9, ImVec2(p0.x + 12.0f, p1.y - 22.0f), TextGhost, caption.c_str(), 0.9f);
        dl->ChannelsMerge();
    }

    void FigureDrawingEditor::DrawPreview(const ImVec2& p0, const ImVec2& p1) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImGuiIO& io = ImGui::GetIO();
        const ImVec2 size = p1 - p0;

        if (m_meshDirty) {
            Render::BuildDrawingMesh(m_drawing, m_mesh);
            m_meshDirty = false;
        }
        // Framed to the player, or to a drawing taller than one.
        const float top = std::max(1.9f, m_mesh.top);
        m_preview.fitExtent = glm::vec2(1.0f, top + 0.2f);
        m_preview.target = glm::vec3(0.0f, top * 0.5f, 0.0f);
        m_preview.FitTo(size);

        ImGui::SetCursorScreenPos(p0);
        ImGui::InvisibleButton("##drawingPreview", size,
                               ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
        const bool hovered = ImGui::IsItemHovered();
        if (ImGui::IsItemActivated()) m_orbiting = true;
        if (ImGui::IsItemActive()) {
            if (m_orbiting) m_preview.Orbit(io.MouseDelta);
        } else {
            m_orbiting = false;
        }
        if (hovered) m_preview.Zoom(io.MouseWheel, 0.35f, 1.8f);

        // ── Scene: the ground under the hitbox, and the figure as the game draws it ──
        m_preview.Clear();
        {
            std::vector<PreviewVertex> ground;
            const auto corner = [](float x, float z) {
                PreviewVertex v{};
                v.x = x; v.y = 0.0f; v.z = z;
                v.ny = 1.0f;
                v.r = 0x2a; v.g = 0x2f; v.b = 0x3c; v.a = 255;
                return v;
            };
            const float h = Drawing::kWidthBlocks * 0.5f;
            ground.push_back(corner(-h, -h)); ground.push_back(corner(-h, h)); ground.push_back(corner(h, h));
            ground.push_back(corner(-h, -h)); ground.push_back(corner(h, h)); ground.push_back(corner(h, -h));
            m_preview.AddBatch(ground, 0, /*cull=*/false, /*lit=*/false);
        }
        // The strokes widened toward this view's eye, as the game widens
        // them toward the camera: flat colours, both sides.
        std::vector<PreviewVertex> figure;
        AppendDrawingPreview(m_mesh, m_preview.Eye(), figure);
        if (!figure.empty()) m_preview.AddBatch(figure, 0, /*cull=*/false, /*lit=*/false);
        m_preview.Draw(dl, p0, p1, BgStripeA, 12.0f);
        dl->AddRect(p0, p1, Border, 12.0f);

        TxtTracked(dl, g_fontMono9, ImVec2(p0.x + 12.0f, p0.y + 12.0f), TextFaint, "IN GAME", 1.6f);
        const char* caption = m_mesh.Empty() ? "NOTHING DRAWN YET" : "DRAG TO TURN - SCROLL TO ZOOM";
        const float cw = MeasureTracked(g_fontMono9, caption, 0.9f);
        TxtTracked(dl, g_fontMono9, ImVec2(p0.x + (size.x - cw) * 0.5f, p1.y - 22.0f), TextGhost, caption, 0.9f);
    }

    void FigureDrawingEditor::DrawPopups() {
        if (m_confirmPending) {
            ImGui::OpenPopup("##drawingConfirm");
            m_confirmPending = false;
        }
        const ImVec2 center(WindowWidth * 0.5f, WindowHeight * 0.5f);
        ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(340.0f, 128.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        if (ImGui::BeginPopupModal("##drawingConfirm", nullptr,
                                   ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                   ImGuiWindowFlags_NoMove)) {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 wp = ImGui::GetWindowPos();
            const char* title = "";
            const char* body = "";
            const char* action = "";
            switch (m_confirm) {
                case Confirm::Discard:
                    title = "Discard these changes?";
                    body = "DONE keeps them; this throws them away.";
                    action = "DISCARD";
                    break;
                case Confirm::Clear:
                    title = "Clear the drawing?";
                    body = "The canvas empties to draw from scratch. Undo brings it back.";
                    action = "CLEAR";
                    break;
                case Confirm::FromFigure:
                    title = "Start from your stick figure?";
                    body = "It replaces the drawing on the canvas. Undo brings it back.";
                    action = "REPLACE";
                    break;
                case Confirm::None:
                    break;
            }
            Txt(dl, g_fontH3, wp + ImVec2(20, 18), TextPrimary, title);
            Txt(dl, g_fontSmall, wp + ImVec2(20, 44), TextMuted, body, 300.0f);
            if (Secondary("##drawingKeep", "Keep editing", wp + ImVec2(20, 80), ImVec2(140, 32))) {
                m_confirm = Confirm::None;
                ImGui::CloseCurrentPopup();
            }
            if (Primary("##drawingConfirmBtn", action, wp + ImVec2(180, 80), ImVec2(140, 32), true, g_fontBtn14,
                        0.84f)) {
                const Drawing before = m_drawing;
                switch (m_confirm) {
                    case Confirm::Discard:
                        Close();
                        break;
                    case Confirm::Clear:
                        m_drawing.Clear();
                        Commit(before);
                        break;
                    case Confirm::FromFigure:
                        Render::StickFigureStrokes(m_figurePaint, m_drawing);
                        Commit(before);
                        break;
                    case Confirm::None:
                        break;
                }
                m_confirm = Confirm::None;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar();
    }

} // namespace Launcher::Appearance
