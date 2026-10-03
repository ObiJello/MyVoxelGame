// File: src/launcher/appearance/FigureDrawingEditor.hpp
//
// The launcher's drawing editor: a full-window overlay (like the skin
// editor) for drawing the figure a player can wear instead of the stick
// figure (Game::StickFigureDrawing, docs/player-appearance.md) — round
// strokes, drawn in game the way the stick figure draws its own limbs.
//
//   left     the tools: brush, eraser (cuts away the parts of strokes it
//            passes over), line, rectangle and circle (outlines),
//            eyedropper (a stroke's colour and size); the brush size, a
//            continuous slider ([ / ] too); mirror (every stroke repeated on
//            the other half), smoothing and the guide; the palette — the
//            preset stick-figure colours; "From stick figure" (your plain or
//            painted figure as editable strokes) and "Clear" (an empty
//            canvas, to draw from scratch).
//   middle   the canvas, 0.6 × 3.0 blocks, with a faint guide: the player's
//            1.8-block height and the stick figure. The brush's circle
//            follows the cursor at its true size. Scroll to zoom about the
//            cursor, right- / middle-drag or Space-drag to pan.
//   right    the result in 3D as every client draws it, turning and
//            zooming, updated live.
//
// Undo / redo (Ctrl+Z, Ctrl+Shift+Z / Ctrl+Y). It edits a copy; DONE writes
// it back to the settings (and wears it), the back button asks before
// throwing changes away.
#pragma once

#include "AppearanceSettings.hpp"
#include "PlayerMesh.hpp"
#include "Preview3D.hpp"
#include "client/renderer/entity/StickFigureGeometry.hpp"

#include <imgui.h>

#include <cstddef>
#include <string>
#include <vector>

namespace Launcher::Appearance {

    class FigureDrawingEditor {
    public:
        // `figureColor`: the plain figure's colour (--color) — what "From
        // stick figure" draws an unpainted figure in, and the first brush.
        void Open(const Settings& settings, Game::PlayerColorId figureColor);
        bool IsOpen() const { return m_open; }
        void Close();

        // Draws the overlay over the whole launcher window. True the frame
        // DONE wrote the drawing into `settings`.
        bool Draw(Settings& settings);

        void ReleaseGpu();

    private:
        using Drawing = Game::StickFigureDrawing;
        enum class Tool { Brush, Eraser, Line, Rect, Ellipse, Pick, Count };
        enum class Drag { None, Brush, Erase, Shape, Pan };
        enum class Confirm { None, Discard, Clear, FromFigure };

        // ── Edits ──
        // A drag snapshots once, at its start; its end makes one undo step.
        void BeginDrag();
        void EndDrag();
        // One edit outside a drag (clear, start from the figure).
        void Commit(const Drawing& before);
        void Undo();
        void Redo();
        void SetStatus(const char* text);

        // The brush radius in whole millimetres (the drawing's unit), and in
        // grid units.
        int   RadiusMm() const;
        float RadiusGrid() const;
        // A path in grid units (x from the left, y up) as a stroke in the
        // brush's colour and size: every point kept inside the canvas by the
        // radius, snapped to the grid, repeats dropped.
        Drawing::Stroke MakeStroke(const std::vector<ImVec2>& path) const;
        // Appends `stroke`, and its mirror image while mirroring.
        void AddStroke(Drawing& d, Drawing::Stroke stroke) const;
        // The live stroke (brush) or shape, redrawn over the drag's start.
        void RebuildBrush();
        void RebuildShape(const ImVec2& cursor, bool constrain);
        // The eraser swept from `a` to `b` (grid units), and its mirror.
        void EraseAlong(const ImVec2& a, const ImVec2& b);
        // The eyedropper: the topmost stroke under `at`.
        void PickAt(const ImVec2& at);

        void HandleShortcuts();
        void SelectTool(Tool tool);
        void DrawTopBar(Settings& settings, bool& applied);
        void DrawToolColumn(const ImVec2& p0, float width);
        void DrawSizeSlider(const ImVec2& p0, float width);
        void DrawCanvas(const ImVec2& p0, const ImVec2& p1);
        void DrawPreview(const ImVec2& p0, const ImVec2& p1);
        void DrawPopups();
        // The view's zoom to the whole canvas, and to the player's height.
        void FitCanvas(const ImVec2& viewSize, bool wholeCanvas);

        bool m_open = false;
        Drawing m_drawing;
        Drawing m_saved;                // what the settings hold
        Drawing m_dragStart;
        Drawing m_guide;                // the stick figure, as strokes
        Game::StickFigurePaint m_figurePaint;
        std::vector<Drawing> m_undo;
        std::vector<Drawing> m_redo;

        Tool m_tool = Tool::Brush;
        Tool m_toolBeforePick = Tool::Brush;
        Game::PlayerColorId m_brush = Game::PlayerColorId::Default;
        // The brush radius in mm, continuous on the slider (kept between
        // openings); strokes take it rounded to the drawing's millimetres.
        float m_radius = static_cast<float>(Drawing::kLineRadius);
        bool m_mirror = false;
        bool m_smooth = true;
        bool m_showGuide = true;

        // The canvas view: design px per block (0 = fit on the next frame),
        // and the canvas point (blocks from its top-left) at the view's centre.
        float  m_zoom = 0.0f;
        ImVec2 m_pan{0.0f, 0.0f};
        Drag   m_drag = Drag::None;
        // The drag's points, in grid units (x from the left, y up): the
        // brush's mouse path, a shape's first corner, the eraser's last spot.
        std::vector<ImVec2> m_raw;
        ImVec2 m_anchor{0.0f, 0.0f};
        ImVec2 m_last{0.0f, 0.0f};
        // Where the live stroke(s) start in m_drawing.strokes.
        size_t m_liveBegin = 0;
        bool   m_capHit = false;

        Preview3D m_preview;
        Render::DrawingMesh m_mesh;
        bool m_meshDirty = true;
        bool m_orbiting = false;

        std::string m_status;
        double m_statusTime = 0.0;
        // The question the confirm popup asks; `pending` opens it next frame.
        Confirm m_confirm = Confirm::None;
        bool m_confirmPending = false;
    };

} // namespace Launcher::Appearance
