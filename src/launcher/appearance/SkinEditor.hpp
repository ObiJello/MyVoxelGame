// File: src/launcher/appearance/SkinEditor.hpp
//
// The launcher's skin editor: a full-window overlay with the player model in
// 3D (drag to turn, paint by clicking the model itself), the flat 64x64
// sheet beside it, and the tools — pencil, eraser, fill, eyedropper, a colour
// picker with swatches and recent colours, undo/redo, the classic/slim arms
// and the outer-layer toggle. It starts from Steve, Alex, a fetched Java
// profile's skin, the current skin or a blank sheet, and saves a 64x64 PNG
// into {obeycraft}/skins/.
//
// The 3D model is Game::PlayerLayout's, picked with the game's own UV
// assignment (PlayerMesh.hpp), so a click on the model paints the texel the
// game will draw there.
#pragma once

#include "AppearanceSettings.hpp"
#include "PlayerMesh.hpp"
#include "Preview3D.hpp"
#include "SkinIO.hpp"

#include <imgui.h>

#include <deque>
#include <string>
#include <vector>

namespace Launcher::Appearance {

    // What the editor can start from (null / empty when unavailable).
    struct EditorTemplates {
        const Game::SkinImage* steve = nullptr;
        const Game::SkinImage* alex = nullptr;
        const Game::SkinImage* profile = nullptr;
        std::string profileName;
        Game::SkinModel profileModel = Game::SkinModel::Classic;
        const Game::SkinImage* current = nullptr;
        Game::SkinModel currentModel = Game::SkinModel::Classic;
    };

    class SkinEditor {
    public:
        struct SaveResult {
            std::string fileName;    // in skins/
            Game::SkinModel model = Game::SkinModel::Classic;
        };

        // `name` pre-fills the save name; `editingFile` (in skins/) is the
        // file it came from, if any.
        void Open(const Game::SkinImage& start, Game::SkinModel model, const std::string& name);
        bool IsOpen() const { return m_open; }
        void Close();

        // Draws the overlay over the whole launcher window. True once, the
        // frame a skin was saved (LastSave() has it).
        bool Draw(const Paths& paths, const EditorTemplates& templates);
        const SaveResult& LastSave() const { return m_lastSave; }

        void ReleaseGpu();

    private:
        enum class Tool { Pencil, Eraser, Fill, Eyedropper };

        void DrawTopBar(const Paths& paths);
        void Draw3D(const ImVec2& p0, const ImVec2& p1);
        void Draw2D(const ImVec2& p0, float scale);
        void DrawToolColumn(const ImVec2& p0, float width);
        void DrawBottomBar(const ImVec2& p0, float width, const EditorTemplates& templates);
        void DrawDiscardPopup();

        // Edits. A stroke snapshots once, at its start.
        void BeginStroke();
        void ApplyTool(int x, int y, const int* faceRect);
        void ApplyLine(int x0, int y0, int x1, int y1, const int* faceRect);
        void SetTexel(int x, int y, const uint8_t rgba[4]);
        void FloodFill(int x, int y, const int* bounds);
        void PickColor(int x, int y);
        void Undo();
        void Redo();
        void Replace(const Game::SkinImage& image, Game::SkinModel model);
        void NoteRecent(ImU32 color);
        bool Save(const Paths& paths);
        void HandleShortcuts();

        bool m_open = false;
        Game::SkinImage m_image;            // what is saved (raw, unprocessed)
        Game::SkinModel m_model = Game::SkinModel::Classic;
        bool m_showOuter = true;
        bool m_dirty = false;               // unsaved changes
        bool m_gpuDirty = true;
        GlImage m_sheetTex;                 // m_image as is (the 2D sheet)
        GlImage m_modelTex;                 // m_image as the game draws it (ProcessLegacySkin)

        std::vector<Game::SkinImage> m_undo;
        std::vector<Game::SkinImage> m_redo;

        Tool m_tool = Tool::Pencil;
        Tool m_toolBeforePick = Tool::Pencil;
        ImVec4 m_color{0.30f, 0.55f, 1.0f, 1.0f};
        std::deque<ImU32> m_recent;

        Preview3D m_preview;
        std::vector<MeshTri> m_tris;
        std::vector<FaceRect> m_faceRects;
        Game::SkinModel m_faceRectsModel = Game::SkinModel::Classic;
        bool m_faceRectsBuilt = false;

        // The stroke in progress.
        enum class StrokeSurface { None, Sheet, Model, Orbit };
        StrokeSurface m_stroke = StrokeSurface::None;
        int m_lastX = -1, m_lastY = -1;
        int m_hoverX = -1, m_hoverY = -1;   // texel under the mouse, either view

        char m_name[48] = "";
        std::string m_status;
        double m_statusTime = 0.0;
        bool m_confirmDiscard = false;

        SaveResult m_lastSave;
    };

} // namespace Launcher::Appearance
