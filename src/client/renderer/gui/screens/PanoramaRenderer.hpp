// File: src/client/renderer/gui/screens/PanoramaRenderer.hpp
//
// The slowly-spinning title-screen skybox. Mirrors MC's CubeMap +
// PanoramaRenderer pair: six 2D textures (panorama_0..5) drawn on the
// inside faces of a unit cube around the camera, rendered with an 85°
// projection, fixed 10° downward pitch, and a yaw that advances at
// 2°/sec × the accessibility "Panorama Scroll Speed" option.
//
// Every panorama Minecraft has ever shipped lives in a versioned
// subfolder: assets/textures/gui/title/background/<slug>/panorama_0..5.png
// (slugs in kSets, chronological). The active set is the "panoramaSet"
// settings key; the Accessibility screen cycles it live via LoadSet().
#pragma once

#include "../../backend/RenderTypes.hpp"

#include <string>
#include <vector>

namespace Render {

    class GuiGraphics;

    class PanoramaRenderer {
    public:
        struct SetInfo {
            const char* slug;    // asset subfolder name
            const char* label;   // display name for the options cycle button
        };

        // Settings default — the newest panorama we ship.
        static constexpr const char* kDefaultSet = "26.1";
        // Sentinel settings value: LoadSet resolves it to a random available
        // set, so a fresh one is rolled every time the title screen comes up.
        static constexpr const char* kRandomSet = "random";

        // The known sets (chronological) filtered down to the ones whose
        // asset folders actually exist on disk.
        static std::vector<SetInfo> AvailableSets();

        bool Initialize(const std::string& setSlug = kDefaultSet);
        void Shutdown();

        // Swap the skybox to another set at runtime (options screen).
        // Falls back through kDefaultSet to a gradient if files are missing.
        void LoadSet(const std::string& slug);
        const std::string& CurrentSet() const { return m_currentSet; }

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
        std::string m_currentSet;
        bool  m_texturesValid = false;
        bool  m_initialized   = false;
        float m_spin          = 0.0f;   // degrees

        void DestroyFaceTextures();
        bool TryLoadSet(const std::string& slug);

        static const char* vertexShaderSource;
        static const char* fragmentShaderSource;
    };

    extern PanoramaRenderer g_panoramaRenderer;

} // namespace Render
