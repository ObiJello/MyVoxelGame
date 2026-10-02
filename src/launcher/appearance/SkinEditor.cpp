// File: src/launcher/appearance/SkinEditor.cpp
#define IMGUI_DEFINE_MATH_OPERATORS
#include "SkinEditor.hpp"
#include "launcher/LauncherConfig.hpp"
#include "launcher/ui/LauncherTheme.hpp"
#include "launcher/ui/LauncherWidgets.hpp"
#include "common/core/Log.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <utility>

namespace Launcher::Appearance {

    using namespace Palette;
    using namespace Widgets;

    namespace {

        constexpr size_t kUndoDepth = 64;
        constexpr size_t kRecentCount = 12;

        // Layout (design px, the 800×500 window).
        constexpr float kPad = 18.0f;
        constexpr float kTopH = 56.0f;
        constexpr float kSheetScale = 5.0f;          // 64 texels → 320 px

        // The swatch palette: neutrals, four skin tones, the colour wheel.
        constexpr ImU32 kSwatches[] = {
            IM_COL32(0xFF, 0xFF, 0xFF, 255), IM_COL32(0xC6, 0xC6, 0xC6, 255), IM_COL32(0x7F, 0x7F, 0x7F, 255),
            IM_COL32(0x3F, 0x3F, 0x3F, 255), IM_COL32(0x14, 0x14, 0x14, 255), IM_COL32(0x7A, 0x4A, 0x2A, 255),
            IM_COL32(0xF9, 0xD3, 0xB5, 255), IM_COL32(0xE0, 0xAC, 0x7E, 255), IM_COL32(0xB5, 0x83, 0x5A, 255),
            IM_COL32(0x6B, 0x46, 0x28, 255), IM_COL32(0xC0, 0x39, 0x2B, 255), IM_COL32(0xE6, 0x7E, 0x22, 255),
            IM_COL32(0xF1, 0xC4, 0x0F, 255), IM_COL32(0x27, 0xAE, 0x60, 255), IM_COL32(0x1A, 0xBC, 0x9C, 255),
            IM_COL32(0x2E, 0x5B, 0xCC, 255), IM_COL32(0x8E, 0x44, 0xAD, 255), IM_COL32(0xE8, 0x6F, 0xA6, 255),
        };

        void ToBytes(const ImVec4& c, uint8_t out[4]) {
            out[0] = static_cast<uint8_t>(std::clamp(c.x, 0.0f, 1.0f) * 255.0f + 0.5f);
            out[1] = static_cast<uint8_t>(std::clamp(c.y, 0.0f, 1.0f) * 255.0f + 0.5f);
            out[2] = static_cast<uint8_t>(std::clamp(c.z, 0.0f, 1.0f) * 255.0f + 0.5f);
            out[3] = static_cast<uint8_t>(std::clamp(c.w, 0.0f, 1.0f) * 255.0f + 0.5f);
        }

        ImU32 ToU32(const ImVec4& c) { return ImGui::ColorConvertFloat4ToU32(c); }

        // A swatch with the checkerboard behind translucent colours.
        void Swatch(ImDrawList* dl, const ImVec2& p0, const ImVec2& p1, ImU32 color, float rounding) {
            if (((color >> IM_COL32_A_SHIFT) & 0xFF) < 255) {
                dl->AddRectFilled(p0, p1, IM_COL32(0xD0, 0xD0, 0xD0, 255), rounding);
                const ImVec2 mid((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
                dl->AddRectFilled(p0, mid, IM_COL32(0x9A, 0x9A, 0x9A, 255), rounding, ImDrawFlags_RoundCornersTopLeft);
                dl->AddRectFilled(mid, p1, IM_COL32(0x9A, 0x9A, 0x9A, 255), rounding, ImDrawFlags_RoundCornersBottomRight);
            }
            dl->AddRectFilled(p0, p1, color, rounding);
        }

        const char* ToolName(int t) {
            static const char* kNames[] = { "Pencil", "Eraser", "Fill", "Pick" };
            return kNames[t];
        }
        const char* ToolKey(int t) {
            static const char* kKeys[] = { "B", "E", "G", "I" };
            return kKeys[t];
        }

    } // namespace

    void SkinEditor::Open(const Game::SkinImage& start, Game::SkinModel model, const std::string& name) {
        m_image = start;
        if (!m_image.Valid() || !NormalizeSkin(m_image)) m_image = BlankSkin();
        m_model = model;
        m_showOuter = true;
        m_dirty = false;
        m_gpuDirty = true;
        m_undo.clear();
        m_redo.clear();
        m_tool = Tool::Pencil;
        m_stroke = StrokeSurface::None;
        m_hoverX = m_hoverY = -1;
        std::snprintf(m_name, sizeof(m_name), "%s", name.c_str());
        m_status.clear();
        m_confirmDiscard = false;
        m_preview.yawDeg = 205.0f;
        m_preview.pitchDeg = 8.0f;
        m_preview.target = glm::vec3(0.0f, 1.0f, 0.0f);
        m_open = true;
    }

    void SkinEditor::Close() {
        m_open = false;
        m_stroke = StrokeSurface::None;
        m_undo.clear();
        m_redo.clear();
    }

    void SkinEditor::ReleaseGpu() {
        m_sheetTex.Release();
        m_modelTex.Release();
        m_preview.Release();
    }

    // ── Edits ──

    void SkinEditor::BeginStroke() {
        m_undo.push_back(m_image);
        if (m_undo.size() > kUndoDepth) m_undo.erase(m_undo.begin());
        m_redo.clear();
        if (m_tool == Tool::Pencil || m_tool == Tool::Fill) NoteRecent(ToU32(m_color));
    }

    void SkinEditor::SetTexel(int x, int y, const uint8_t rgba[4]) {
        if (x < 0 || y < 0 || x >= m_image.width || y >= m_image.height) return;
        uint8_t* p = m_image.PixelPtr(x, y);
        if (std::memcmp(p, rgba, 4) == 0) return;
        std::memcpy(p, rgba, 4);
        m_dirty = true;
        m_gpuDirty = true;
    }

    void SkinEditor::ApplyTool(int x, int y, const int* faceRect) {
        if (x < 0 || y < 0 || x >= m_image.width || y >= m_image.height) return;
        switch (m_tool) {
            case Tool::Pencil: {
                uint8_t c[4];
                ToBytes(m_color, c);
                SetTexel(x, y, c);
                break;
            }
            case Tool::Eraser: {
                const uint8_t clear[4] = { 0, 0, 0, 0 };
                SetTexel(x, y, clear);
                break;
            }
            case Tool::Fill:
                if (faceRect) FloodFill(x, y, faceRect);
                break;
            case Tool::Eyedropper:
                PickColor(x, y);
                break;
        }
    }

    void SkinEditor::ApplyLine(int x0, int y0, int x1, int y1, const int* faceRect) {
        // Bresenham, so a fast drag on the sheet leaves no gaps.
        const int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
        const int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
        int err = dx + dy;
        for (;;) {
            ApplyTool(x0, y0, faceRect);
            if (x0 == x1 && y0 == y1) break;
            const int e2 = 2 * err;
            if (e2 >= dy) { err += dy; x0 += sx; }
            if (e2 <= dx) { err += dx; y0 += sy; }
        }
    }

    void SkinEditor::FloodFill(int x, int y, const int* bounds) {
        uint8_t fill[4];
        ToBytes(m_color, fill);
        uint8_t target[4];
        std::memcpy(target, m_image.PixelPtr(x, y), 4);
        if (std::memcmp(target, fill, 4) == 0) return;
        const int bx0 = std::max(0, bounds[0]), by0 = std::max(0, bounds[1]);
        const int bx1 = std::min(m_image.width, bounds[2]), by1 = std::min(m_image.height, bounds[3]);
        std::vector<std::pair<int, int>> stack{ { x, y } };
        while (!stack.empty()) {
            const auto [cx, cy] = stack.back();
            stack.pop_back();
            if (cx < bx0 || cy < by0 || cx >= bx1 || cy >= by1) continue;
            uint8_t* p = m_image.PixelPtr(cx, cy);
            if (std::memcmp(p, target, 4) != 0) continue;
            std::memcpy(p, fill, 4);
            stack.push_back({ cx + 1, cy });
            stack.push_back({ cx - 1, cy });
            stack.push_back({ cx, cy + 1 });
            stack.push_back({ cx, cy - 1 });
        }
        m_dirty = true;
        m_gpuDirty = true;
    }

    void SkinEditor::PickColor(int x, int y) {
        if (x < 0 || y < 0 || x >= m_image.width || y >= m_image.height) return;
        const uint8_t* p = m_image.PixelPtr(x, y);
        if (p[3] == 0) return;   // nothing there to pick
        m_color = ImVec4(p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f, p[3] / 255.0f);
        if (m_tool == Tool::Eyedropper) m_tool = m_toolBeforePick;
    }

    void SkinEditor::Undo() {
        if (m_undo.empty()) return;
        m_redo.push_back(std::move(m_image));
        m_image = std::move(m_undo.back());
        m_undo.pop_back();
        m_dirty = true;
        m_gpuDirty = true;
    }

    void SkinEditor::Redo() {
        if (m_redo.empty()) return;
        m_undo.push_back(std::move(m_image));
        m_image = std::move(m_redo.back());
        m_redo.pop_back();
        m_dirty = true;
        m_gpuDirty = true;
    }

    void SkinEditor::Replace(const Game::SkinImage& image, Game::SkinModel model) {
        Game::SkinImage next = image;
        if (!next.Valid() || !NormalizeSkin(next)) return;
        m_undo.push_back(m_image);
        if (m_undo.size() > kUndoDepth) m_undo.erase(m_undo.begin());
        m_redo.clear();
        m_image = std::move(next);
        m_model = model;
        m_dirty = true;
        m_gpuDirty = true;
    }

    void SkinEditor::NoteRecent(ImU32 color) {
        m_recent.erase(std::remove(m_recent.begin(), m_recent.end(), color), m_recent.end());
        m_recent.push_front(color);
        while (m_recent.size() > kRecentCount) m_recent.pop_back();
    }

    bool SkinEditor::Save(const Paths& paths) {
        const std::string file = SanitizeSkinFileName(m_name);
        if (file.empty()) {
            m_status = "Name the skin first (letters, numbers, - and _)";
            m_statusTime = ImGui::GetTime();
            return false;
        }
        if (!SavePngFile(paths.CustomSkin(file), m_image)) {
            m_status = "Could not write " + file;
            m_statusTime = ImGui::GetTime();
            return false;
        }
        Log::Info("[SkinEditor] saved %s", paths.CustomSkin(file).c_str());
        m_dirty = false;
        m_lastSave.fileName = file;
        m_lastSave.model = m_model;
        m_status = "Saved as " + file + " and selected";
        m_statusTime = ImGui::GetTime();
        return true;
    }

    void SkinEditor::HandleShortcuts() {
        if (ImGui::GetIO().WantTextInput) return;
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z) ||
            ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Y)) {
            Redo();
        } else if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Z)) {
            Undo();
        }
        if (ImGui::IsKeyChordPressed(ImGuiKey_B)) m_tool = Tool::Pencil;
        if (ImGui::IsKeyChordPressed(ImGuiKey_E)) m_tool = Tool::Eraser;
        if (ImGui::IsKeyChordPressed(ImGuiKey_G)) m_tool = Tool::Fill;
        if (ImGui::IsKeyChordPressed(ImGuiKey_I)) {
            if (m_tool != Tool::Eyedropper) m_toolBeforePick = m_tool;
            m_tool = Tool::Eyedropper;
        }
    }

    // ── Drawing ──

    bool SkinEditor::Draw(const Paths& paths, const EditorTemplates& templates) {
        if (!m_open) return false;
        bool saved = false;

        if (m_gpuDirty) {
            m_sheetTex.Upload(m_image);
            Game::SkinImage shown = m_image;
            Game::ProcessLegacySkin(shown);   // the base layer opaque, as the game draws it
            m_modelTex.Upload(shown);
            m_gpuDirty = false;
        }
        if (!m_faceRectsBuilt || m_faceRectsModel != m_model) {
            m_faceRects = SkinFaceRects(m_model);
            m_faceRectsModel = m_model;
            m_faceRectsBuilt = true;
        }

        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float W = static_cast<float>(WindowWidth), H = static_cast<float>(WindowHeight);
        dl->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), WindowBg);

        HandleShortcuts();
        m_hoverX = m_hoverY = -1;

        // ── Top bar ──
        {
            ImGui::SetCursorScreenPos(ImVec2(kPad, 19.0f));
            if (Pill("##editorBack", "< APPEARANCE", g_fontMono10, ImVec2(2, 2), 0, 0, TextFaint, 0, 0, true, 1.6f)) {
                if (m_dirty) m_confirmDiscard = true;
                else Close();
            }
            TxtTracked(dl, g_fontMono10, ImVec2(140.0f, 13.0f), TextFaint, "SKIN EDITOR", 2.0f);
            Txt(dl, g_fontH3, ImVec2(140.0f, 27.0f), TextPrimary, m_dirty ? "Unsaved skin" : "Skin");

            const float saveW = 96.0f, nameW = 180.0f;
            const float saveX = W - kPad - saveW;
            const float nameX = saveX - 10.0f - nameW;
            ImGui::SetCursorScreenPos(ImVec2(nameX, 12.0f));
            Input("##skinName", "Skin name", m_name, sizeof(m_name), nameW, 32.0f, g_fontInput13);
            if (Primary("##saveSkin", "SAVE", ImVec2(saveX, 12.0f), ImVec2(saveW, 32.0f), true,
                        g_fontBtn14, 0.84f)) {
                saved = Save(paths);
            }
        }
        dl->AddLine(ImVec2(0, kTopH), ImVec2(W, kTopH), Border);

        // ── Panels ──
        const ImVec2 view0(kPad, kTopH + 10.0f), view1(kPad + 290.0f, H - kPad);
        Draw3D(view0, view1);
        const ImVec2 sheet0(view1.x + 16.0f, kTopH + 10.0f);
        Draw2D(sheet0, kSheetScale);
        const float sheetSize = 64.0f * kSheetScale;
        DrawBottomBar(ImVec2(sheet0.x, sheet0.y + sheetSize + 12.0f), sheetSize, templates);
        const float toolX = sheet0.x + sheetSize + 16.0f;
        DrawToolColumn(ImVec2(toolX, kTopH + 10.0f), W - kPad - toolX);

        DrawDiscardPopup();
        return saved;
    }

    void SkinEditor::Draw3D(const ImVec2& p0, const ImVec2& p1) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 size = p1 - p0;
        const ImGuiIO& io = ImGui::GetIO();

        // The scene: the model in the editing texture, arms straight down.
        PlayerPose pose;
        pose.idleSway = false;
        m_tris.clear();
        BuildPlayerMesh(m_model, m_showOuter, Game::ModelPartBits::All, pose, m_tris);
        std::vector<PreviewVertex> verts;
        AppendMeshVertices(m_tris, 64.0f, 64.0f, verts);
        m_preview.Clear();
        m_preview.AddBatch(verts, m_modelTex.Id(), /*cull=*/false, /*lit=*/true);

        m_preview.FitTo(size);
        ImGui::SetCursorScreenPos(p0);
        ImGui::InvisibleButton("##editor3d", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
        const bool hovered = ImGui::IsItemHovered();
        const bool active = ImGui::IsItemActive();

        TexelHit hit;
        if (hovered || active) {
            glm::vec3 o, d;
            m_preview.Ray(io.MousePos - p0, size, o, d);
            hit = PickTexel(m_tris, o, d, m_tool == Tool::Eyedropper ? &m_image : nullptr);
            if (hit.hit) {
                m_hoverX = hit.x;
                m_hoverY = hit.y;
            }
        }
        if (ImGui::IsItemActivated()) {
            const bool left = ImGui::IsMouseDown(ImGuiMouseButton_Left);
            if (left && hit.hit) {
                const bool alt = io.KeyAlt && m_tool == Tool::Pencil;
                if (alt) {
                    // Alt-click picks the colour you see (through clear
                    // outer-layer texels).
                    glm::vec3 o, d;
                    m_preview.Ray(io.MousePos - p0, size, o, d);
                    const TexelHit seen = PickTexel(m_tris, o, d, &m_image);
                    if (seen.hit) PickColor(seen.x, seen.y);
                    m_stroke = StrokeSurface::None;
                } else {
                    m_stroke = StrokeSurface::Model;
                    if (m_tool != Tool::Eyedropper) BeginStroke();
                    ApplyTool(hit.x, hit.y, m_tris[static_cast<size_t>(hit.tri)].rect);
                    m_lastX = hit.x;
                    m_lastY = hit.y;
                }
            } else {
                m_stroke = StrokeSurface::Orbit;
            }
        }
        if (active) {
            if (m_stroke == StrokeSurface::Model) {
                if (hit.hit && (hit.x != m_lastX || hit.y != m_lastY) &&
                    (m_tool == Tool::Pencil || m_tool == Tool::Eraser)) {
                    ApplyTool(hit.x, hit.y, m_tris[static_cast<size_t>(hit.tri)].rect);
                    m_lastX = hit.x;
                    m_lastY = hit.y;
                }
            } else if (m_stroke == StrokeSurface::Orbit) {
                m_preview.Orbit(io.MouseDelta);
            }
        } else if (m_stroke == StrokeSurface::Model || m_stroke == StrokeSurface::Orbit) {
            m_stroke = StrokeSurface::None;
        }
        if (hovered) m_preview.Zoom(io.MouseWheel, 0.35f, 1.6f);

        m_preview.Draw(dl, p0, p1, BgStripeA, 12.0f);
        dl->AddRect(p0, p1, Border, 12.0f);

        // The texel under the cursor, or the controls.
        char info[96];
        if (m_hoverX >= 0) {
            const FaceRect* face = FaceRectAt(m_faceRects, m_hoverX, m_hoverY);
            std::snprintf(info, sizeof(info), "%d, %d  %s", m_hoverX, m_hoverY, face ? face->part : "");
        } else {
            std::snprintf(info, sizeof(info), "PAINT ON THE MODEL  -  DRAG AROUND IT TO TURN");
        }
        TxtTracked(dl, g_fontMono9, ImVec2(p0.x + 12.0f, p1.y - 22.0f), TextGhost, info, 0.9f);
    }

    void SkinEditor::Draw2D(const ImVec2& p0, float scale) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float size = 64.0f * scale;
        const ImVec2 p1(p0.x + size, p0.y + size);
        const ImGuiIO& io = ImGui::GetIO();

        // Unused sheet space reads as background; each face gets the
        // transparency checkerboard (two texels a check).
        dl->AddRectFilled(p0 - ImVec2(1, 1), p1 + ImVec2(1, 1), Rail, 4.0f);
        for (const FaceRect& r : m_faceRects) {
            for (int y = r.y0; y < r.y1; y += 2) {
                for (int x = r.x0; x < r.x1; x += 2) {
                    const bool dark = ((x / 2) + (y / 2)) % 2 == 0;
                    const ImVec2 a(p0.x + x * scale, p0.y + y * scale);
                    const ImVec2 b(p0.x + std::min(x + 2, r.x1) * scale, p0.y + std::min(y + 2, r.y1) * scale);
                    dl->AddRectFilled(a, b, dark ? IM_COL32(0x2a, 0x2e, 0x38, 255) : IM_COL32(0x33, 0x38, 0x44, 255));
                }
            }
        }
        if (m_sheetTex.Valid()) {
            dl->AddImage(static_cast<ImTextureID>(static_cast<uintptr_t>(m_sheetTex.Id())), p0, p1);
        }
        for (const FaceRect& r : m_faceRects) {
            const ImVec2 a(p0.x + r.x0 * scale, p0.y + r.y0 * scale);
            const ImVec2 b(p0.x + r.x1 * scale, p0.y + r.y1 * scale);
            if (r.outer && !m_showOuter) dl->AddRectFilled(a, b, IM_COL32(0x0f, 0x10, 0x14, 150));
            dl->AddRect(a, b, IM_COL32(0x4a, 0x4f, 0x5e, 110));
        }

        ImGui::SetCursorScreenPos(p0);
        ImGui::InvisibleButton("##editorSheet", ImVec2(size, size));
        const bool hovered = ImGui::IsItemHovered();
        const bool active = ImGui::IsItemActive();
        const int tx = static_cast<int>(std::floor((io.MousePos.x - p0.x) / scale));
        const int ty = static_cast<int>(std::floor((io.MousePos.y - p0.y) / scale));
        const bool inside = tx >= 0 && ty >= 0 && tx < 64 && ty < 64;
        if ((hovered || active) && inside) {
            m_hoverX = tx;
            m_hoverY = ty;
        }
        const auto rectOf = [&](int x, int y) -> const int* {
            const FaceRect* face = FaceRectAt(m_faceRects, x, y);
            static int r[4];
            if (!face) return nullptr;
            r[0] = face->x0; r[1] = face->y0; r[2] = face->x1; r[3] = face->y1;
            return r;
        };
        if (ImGui::IsItemActivated() && inside) {
            if (io.KeyAlt && m_tool == Tool::Pencil) {
                PickColor(tx, ty);
                m_stroke = StrokeSurface::None;
            } else {
                m_stroke = StrokeSurface::Sheet;
                if (m_tool != Tool::Eyedropper) BeginStroke();
                ApplyTool(tx, ty, rectOf(tx, ty));
                m_lastX = tx;
                m_lastY = ty;
            }
        }
        if (active && m_stroke == StrokeSurface::Sheet && inside && (tx != m_lastX || ty != m_lastY) &&
            (m_tool == Tool::Pencil || m_tool == Tool::Eraser)) {
            ApplyLine(m_lastX, m_lastY, tx, ty, nullptr);
            m_lastX = tx;
            m_lastY = ty;
        }
        if (!active && m_stroke == StrokeSurface::Sheet) m_stroke = StrokeSurface::None;

        // The hovered texel (from either view).
        if (m_hoverX >= 0) {
            const ImVec2 a(p0.x + m_hoverX * scale, p0.y + m_hoverY * scale);
            dl->AddRect(a - ImVec2(1, 1), a + ImVec2(scale + 1, scale + 1), IM_COL32(0xee, 0xf1, 0xf8, 230), 0.0f, 0, 1.5f);
        }
        dl->AddRect(p0 - ImVec2(1, 1), p1 + ImVec2(1, 1), Border, 4.0f);
    }

    void SkinEditor::DrawToolColumn(const ImVec2& p0, float width) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        float y = p0.y;

        SectionLabel(dl, ImVec2(p0.x, y), "TOOLS");
        y += 18.0f;
        const float cellW = (width - 6.0f) * 0.5f, cellH = 38.0f;
        for (int i = 0; i < 4; ++i) {
            const ImVec2 c0(p0.x + (i % 2) * (cellW + 6.0f), y + (i / 2) * (cellH + 6.0f));
            const ImVec2 c1 = c0 + ImVec2(cellW, cellH);
            ImGui::SetCursorScreenPos(c0);
            ImGui::PushID(i);
            const bool clicked = ImGui::InvisibleButton("##tool", ImVec2(cellW, cellH));
            const bool hov = ImGui::IsItemHovered();
            ImGui::PopID();
            const bool on = static_cast<int>(m_tool) == i;
            dl->AddRectFilled(c0, c1, on ? BgActive : hov ? BgHover : Rail, 9.0f);
            dl->AddRect(c0, c1, on ? Accent : Border, 9.0f);
            Txt(dl, g_fontSmallMed, c0 + ImVec2(10.0f, 7.0f), on ? TextPrimary : TextBody, ToolName(i));
            Txt(dl, g_fontMono9, c0 + ImVec2(10.0f, 23.0f), TextFaint, ToolKey(i));
            if (clicked) {
                if (i == static_cast<int>(Tool::Eyedropper) && m_tool != Tool::Eyedropper) m_toolBeforePick = m_tool;
                m_tool = static_cast<Tool>(i);
            }
        }
        y += 2.0f * cellH + 6.0f + 16.0f;

        // The current colour: click for the picker.
        SectionLabel(dl, ImVec2(p0.x, y), "COLOUR");
        y += 18.0f;
        {
            const ImVec2 c0(p0.x, y), c1(p0.x + width, y + 34.0f);
            ImGui::SetCursorScreenPos(c0);
            if (ImGui::InvisibleButton("##currentColor", c1 - c0)) ImGui::OpenPopup("##skinColorPicker");
            Swatch(dl, c0, c1, ToU32(m_color), 9.0f);
            dl->AddRect(c0, c1, ImGui::IsItemHovered() ? BorderHover : Border, 9.0f, 0, 1.5f);
            char hex[16];
            uint8_t b[4];
            ToBytes(m_color, b);
            std::snprintf(hex, sizeof(hex), "#%02X%02X%02X", b[0], b[1], b[2]);
            const float lum = 0.299f * m_color.x + 0.587f * m_color.y + 0.114f * m_color.z;
            Txt(dl, g_fontMono10, c0 + ImVec2(10.0f, 11.0f),
                lum > 0.55f || m_color.w < 0.5f ? IM_COL32(0x10, 0x12, 0x18, 255) : TextPrimary, hex);
        }
        ImGui::SetNextWindowPos(ImVec2(p0.x + width, y), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
        if (ImGui::BeginPopup("##skinColorPicker")) {
            ImGui::Dummy(ImVec2(0, 4));
            ImGui::Indent(10.0f);
            ImGui::SetNextItemWidth(220.0f);
            ImGui::ColorPicker4("##picker", &m_color.x,
                                ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_NoSidePreview |
                                ImGuiColorEditFlags_PickerHueBar | ImGuiColorEditFlags_DisplayHex |
                                ImGuiColorEditFlags_NoLabel);
            ImGui::Unindent(10.0f);
            ImGui::Dummy(ImVec2(0, 4));
            ImGui::EndPopup();
        }
        y += 34.0f + 14.0f;

        // Swatches, then the colours used lately.
        const auto swatchGrid = [&](const ImU32* colors, size_t count, const char* idPrefix) {
            constexpr int kCols = 6;
            const float gap = 4.0f;
            const float cell = (width - gap * (kCols - 1)) / kCols;
            for (size_t i = 0; i < count; ++i) {
                const ImVec2 c0(p0.x + (i % kCols) * (cell + gap), y + (i / kCols) * (cell + gap));
                ImGui::SetCursorScreenPos(c0);
                ImGui::PushID(idPrefix);
                ImGui::PushID(static_cast<int>(i));
                if (ImGui::InvisibleButton("##sw", ImVec2(cell, cell))) {
                    m_color = ImGui::ColorConvertU32ToFloat4(colors[i]);
                    if (m_tool == Tool::Eraser || m_tool == Tool::Eyedropper) m_tool = Tool::Pencil;
                }
                const bool hov = ImGui::IsItemHovered();
                ImGui::PopID();
                ImGui::PopID();
                Swatch(dl, c0, c0 + ImVec2(cell, cell), colors[i], 4.0f);
                if (hov) dl->AddRect(c0 - ImVec2(1, 1), c0 + ImVec2(cell + 1, cell + 1), TextPrimary, 5.0f);
            }
            const size_t rows = (count + kCols - 1) / kCols;
            y += rows * (cell + gap);
        };
        SectionLabel(dl, ImVec2(p0.x, y), "SWATCHES");
        y += 18.0f;
        swatchGrid(kSwatches, sizeof(kSwatches) / sizeof(kSwatches[0]), "swatch");
        y += 12.0f;
        SectionLabel(dl, ImVec2(p0.x, y), "RECENT");
        y += 18.0f;
        if (m_recent.empty()) {
            Txt(dl, g_fontSmall, ImVec2(p0.x, y), TextGhost, "None yet");
            y += 20.0f;
        } else {
            const std::vector<ImU32> recent(m_recent.begin(), m_recent.end());
            swatchGrid(recent.data(), recent.size(), "recent");
        }
        y += 12.0f;

        // Undo / redo.
        const float bw = (width - 6.0f) * 0.5f;
        if (Secondary("##undo", "Undo", ImVec2(p0.x, y), ImVec2(bw, 30.0f), !m_undo.empty())) Undo();
        if (Secondary("##redo", "Redo", ImVec2(p0.x + bw + 6.0f, y), ImVec2(bw, 30.0f), !m_redo.empty())) Redo();
    }

    void SkinEditor::DrawBottomBar(const ImVec2& p0, float width, const EditorTemplates& templates) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        float y = p0.y;

        // Arms + outer layer.
        Txt(dl, g_fontLabel12, ImVec2(p0.x, y + 8.0f), TextMuted, "Arms");
        {
            const char* labels[] = { "Classic", "Slim" };
            const int active = m_model == Game::SkinModel::Slim ? 1 : 0;
            const int clicked = Segmented("##editorArms", labels, 2, active, ImVec2(p0.x + 44.0f, y), false);
            if (clicked >= 0) m_model = clicked == 1 ? Game::SkinModel::Slim : Game::SkinModel::Classic;
        }
        {
            const char* label = "Outer layer";
            const float lw = Measure(g_fontLabel12, label).x;
            Txt(dl, g_fontLabel12, ImVec2(p0.x + width - 34.0f - 10.0f - lw, y + 8.0f), TextMuted, label);
            ImGui::SetCursorScreenPos(ImVec2(p0.x + width - 34.0f, y + 6.0f));
            if (Toggle("##outerLayer", m_showOuter)) m_showOuter = !m_showOuter;
        }
        y += 40.0f;

        // Start over from a template.
        SectionLabel(dl, ImVec2(p0.x, y), "START FROM");
        y += 16.0f;
        float x = p0.x;
        const auto option = [&](const char* id, const char* label, const Game::SkinImage* image,
                                Game::SkinModel model, bool blank) {
            if (!blank && (!image || !image->Valid())) return;
            const ImVec2 sz = PillSize(label, g_fontSmallMed, ImVec2(11, 6));
            if (x + sz.x > p0.x + width) return;
            ImGui::SetCursorScreenPos(ImVec2(x, y));
            if (Pill(id, label, g_fontSmallMed, ImVec2(11, 6), BgActive, BgActiveHov, TextBody, 8.0f)) {
                Replace(blank ? BlankSkin() : *image, model);
            }
            x += sz.x + 6.0f;
        };
        option("##fromSteve", "Steve", templates.steve, Game::SkinModel::Classic, false);
        option("##fromAlex", "Alex", templates.alex, Game::SkinModel::Slim, false);
        if (!templates.profileName.empty()) {
            option("##fromProfile", templates.profileName.c_str(), templates.profile, templates.profileModel, false);
        }
        option("##fromCurrent", "Current skin", templates.current, templates.currentModel, false);
        option("##fromBlank", "Blank", nullptr, m_model, true);
        y += 32.0f;

        // Status / hints.
        if (!m_status.empty() && ImGui::GetTime() - m_statusTime < 6.0) {
            Txt(dl, g_fontSmall, ImVec2(p0.x, y), TextMuted, m_status.c_str());
        } else {
            Txt(dl, g_fontSmall, ImVec2(p0.x, y), TextGhost,
                m_showOuter ? "Hide the outer layer to paint the skin beneath it."
                            : "Alt-click picks a colour.");
        }
    }

    void SkinEditor::DrawDiscardPopup() {
        if (m_confirmDiscard) {
            ImGui::OpenPopup("##discardSkin");
            m_confirmDiscard = false;
        }
        const ImVec2 center(WindowWidth * 0.5f, WindowHeight * 0.5f);
        ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(320.0f, 128.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        if (ImGui::BeginPopupModal("##discardSkin", nullptr,
                                   ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                   ImGuiWindowFlags_NoMove)) {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 wp = ImGui::GetWindowPos();
            Txt(dl, g_fontH3, wp + ImVec2(20, 18), TextPrimary, "Discard this skin?");
            Txt(dl, g_fontSmall, wp + ImVec2(20, 44), TextMuted, "Your changes have not been saved.");
            if (Secondary("##keepEditing", "Keep editing", wp + ImVec2(20, 78), ImVec2(130, 32))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::SetCursorScreenPos(wp + ImVec2(170, 78));
            if (Primary("##discard", "DISCARD", wp + ImVec2(170, 78), ImVec2(130, 32), true, g_fontBtn14, 0.84f)) {
                ImGui::CloseCurrentPopup();
                Close();
            }
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar();
    }

} // namespace Launcher::Appearance
