// File: src/launcher/appearance/Preview3D.hpp
//
// A small OpenGL 3.3 scene drawn into its own multisampled framebuffer and
// shown in the ImGui layout as an image — the launcher's 3D previews (the
// Appearance view's model, the stick-figure painter, the skin editor).
//
// The owner rebuilds the scene's triangle batches every frame (they are a few
// hundred triangles), orbits the camera from mouse drags, and calls Draw from
// inside the ImGui frame: the GL work happens right there, before ImGui's own
// draw data is rendered, and every piece of GL state it touches is restored
// so the ImGui backend and the launcher's frame loop see what they left.
#pragma once

#include "PlayerMesh.hpp"

#include <imgui.h>
#include <glm/glm.hpp>

#include <cstddef>
#include <vector>

typedef unsigned int GLuint;

namespace Launcher::Appearance {

    class Preview3D {
    public:
        Preview3D() = default;
        ~Preview3D();
        Preview3D(const Preview3D&) = delete;
        Preview3D& operator=(const Preview3D&) = delete;

        // ── Scene ──
        void Clear();
        // `texture` 0 draws vertex colour alone. `cull`: back faces hidden
        // (counter-clockwise front). `lit`: the two-light entity shading.
        void AddBatch(const std::vector<PreviewVertex>& verts, GLuint texture, bool cull, bool lit);

        // ── Camera: an orbit around `target` ──
        float yawDeg = 205.0f;     // 180 looks at the front (the model faces +Z)
        float pitchDeg = 10.0f;
        float distance = 4.6f;     // set by FitTo each frame (fit × zoom)
        float fovDeg = 30.0f;      // vertical
        glm::vec3 target{0.0f, 1.0f, 0.0f};
        // What FitTo frames: the subject's height and its widest horizontal
        // extent at any yaw (blocks), and the share of the panel it fills.
        glm::vec2 fitExtent{1.2f, 2.1f};
        float fitFill = 0.86f;
        float zoom = 1.0f;         // mouse-wheel multiplier on the fitted distance

        // Places the camera so `fitExtent` fills `fitFill` of the panel on
        // whichever axis is tighter, then applies `zoom`. Call once a frame
        // with the panel's size, before Eye/Ray/Draw.
        void FitTo(const ImVec2& size);

        glm::vec3 Eye() const;
        glm::mat4 View() const;
        glm::mat4 Projection(float aspect) const;
        // The world ray under a point of the panel (`local` from its top-left,
        // `size` its size, both in design pixels).
        void Ray(const ImVec2& local, const ImVec2& size, glm::vec3& origin, glm::vec3& dir) const;
        // Turns the camera by a mouse drag (design pixels).
        void Orbit(const ImVec2& delta);
        // Mouse-wheel zoom: scales `zoom`, clamped to [minZoom, maxZoom]
        // (1 = fitted; below 1 is closer).
        void Zoom(float wheel, float minZoom, float maxZoom);

        // Renders the scene at the panel's device resolution and draws it
        // into `dl` over [p0, p1]. `background` fills the frame (the image is
        // opaque — a transparent clear would fringe the antialiased edges).
        void Draw(ImDrawList* dl, const ImVec2& p0, const ImVec2& p1, ImU32 background, float rounding);

        // Frees every GL object (the shared program when the last preview
        // lets go). Must run while the GL context is current — the launcher
        // calls it before tearing the window down; the destructor then has
        // nothing left to free.
        void Release();

    private:
        struct Batch {
            GLuint texture = 0;
            bool cull = false;
            bool lit = true;
            size_t first = 0;
            size_t count = 0;
        };
        std::vector<PreviewVertex> m_verts;
        std::vector<Batch> m_batches;

        bool EnsureTargets(int w, int h);
        void ReleaseTargets();

        GLuint m_msFbo = 0, m_msColor = 0, m_msDepth = 0;   // multisampled
        GLuint m_fbo = 0, m_color = 0;                      // resolved texture
        int m_w = 0, m_h = 0, m_samples = 0;
        GLuint m_vao = 0, m_vbo = 0;
        size_t m_vboBytes = 0;
    };

} // namespace Launcher::Appearance
