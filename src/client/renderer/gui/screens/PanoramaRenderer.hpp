// File: src/client/renderer/gui/screens/PanoramaRenderer.hpp
//
// The slowly-spinning title-screen skybox. Mirrors MC's CubeMap +
// PanoramaRenderer pair: six 2D textures (panorama_0..5) drawn on the
// inside faces of a unit cube around the camera, rendered with an 85°
// projection, fixed 10° downward pitch, and a yaw that advances at
// 2°/sec × the accessibility "Panorama Scroll Speed" option.
//
// NOTE: the current asset dump ships 1×1 placeholder PNGs for the six
// panorama images (they aren't part of the standard MC jar item/block
// dump). The renderer detects that and falls back to a static gradient
// so the title screen is still presentable; dropping real panorama_N.png
// files into assets/textures/gui/title/background/ lights it up.
#pragma once

#include "../../backend/RenderTypes.hpp"

namespace Render {

    class GuiGraphics;

    class PanoramaRenderer {
    public:
        bool Initialize();
        void Shutdown();

        // Advance the spin and draw the skybox to the current framebuffer.
        // Call before the GUI pass. `speed` is the panorama scroll-speed
        // option (1.0 = vanilla).
        void Render(int fbWidth, int fbHeight, float deltaSeconds, float speed);

        // Full-screen vignette overlay (panorama_overlay.png) — drawn in GUI
        // space by the title screen AFTER the skybox, with fade alpha.
        void RenderOverlay(GuiGraphics& g, int guiWidth, int guiHeight, float alpha);

        bool HasPanoramaTextures() const { return m_texturesValid; }

    private:
        ShaderHandle  m_shader = INVALID_SHADER;
        BufferHandle  m_vb     = INVALID_BUFFER;
        BufferHandle  m_ib     = INVALID_BUFFER;
        MeshHandle    m_mesh   = INVALID_MESH;
        TextureHandle m_faces[6] = {INVALID_TEXTURE, INVALID_TEXTURE, INVALID_TEXTURE,
                                    INVALID_TEXTURE, INVALID_TEXTURE, INVALID_TEXTURE};
        TextureHandle m_overlay = INVALID_TEXTURE;
        bool  m_texturesValid = false;
        bool  m_initialized   = false;
        float m_spin          = 0.0f;   // degrees

        static const char* vertexShaderSource;
        static const char* fragmentShaderSource;
    };

    extern PanoramaRenderer g_panoramaRenderer;

} // namespace Render
