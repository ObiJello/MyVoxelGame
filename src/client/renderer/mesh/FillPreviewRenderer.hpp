// File: src/client/renderer/mesh/FillPreviewRenderer.hpp
//
// The fill tool's preview: the held block's model, drawn see-through with a
// faint blue cast in every cell of the box between the marked corner and
// the cell under the crosshair. One streaming mesh per frame; faces between
// two cells of the box are left out, so a floor is its top and bottom and
// a cube its six sides, whatever its size.
#pragma once

#include "../backend/RenderTypes.hpp"
#include "common/world/block/BlockState.hpp"

#include <glm/glm.hpp>
#include <vector>

namespace Render {

    class FillPreviewRenderer {
    public:
        bool Initialize();
        void Shutdown();

        // Draw `state` in every cell of [lo, hi] (inclusive corners).
        void Render(const glm::mat4& projection, const glm::mat4& view,
                    const glm::vec3& cameraPos, Game::BlockState state,
                    const glm::ivec3& lo, const glm::ivec3& hi);

    private:
        bool          m_initialized = false;
        ShaderHandle  m_shader = INVALID_SHADER;
        struct FrameBuffers {
            BufferHandle vb   = INVALID_BUFFER;
            BufferHandle ib   = INVALID_BUFFER;
            MeshHandle   mesh = INVALID_MESH;
        };
        FrameBuffers m_frames[2];
        int          m_parity = 0;
    };

    extern FillPreviewRenderer g_fillPreviewRenderer;

} // namespace Render
