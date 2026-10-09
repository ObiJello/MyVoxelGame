// File: src/client/renderer/backend/RenderBackend.cpp
#include "RenderBackend.hpp"
#include "opengl/GLBackend.hpp"
#include "common/core/Log.hpp"

namespace Render {

    // Global instance
    std::unique_ptr<RenderBackend> g_renderBackend = nullptr;

    // Each optional backend's factory lives in its own translation unit,
    // compiled only when the backend is (HAS_VULKAN / HAS_METAL).
#ifdef HAS_VULKAN
    std::unique_ptr<RenderBackend> CreateVulkanBackend();
#endif
#ifdef HAS_METAL
    std::unique_ptr<RenderBackend> CreateMetalBackend();
    std::unique_ptr<RenderBackend> CreateMetal4Backend();
    static bool g_metal4Requested = false;
#endif

    // Default multi-draw: loops individual draws (GL overrides with native call)
    void RenderBackend::MultiDrawIndexedBaseVertex(const int32_t* indexCounts,
                                                    const size_t* indexByteOffsets,
                                                    const int32_t* baseVertices,
                                                    uint32_t drawCount,
                                                    IndexType indexType) {
        for (uint32_t i = 0; i < drawCount; i++) {
            if (indexCounts[i] > 0)
                DrawIndexedBaseVertex(indexCounts[i], indexByteOffsets[i], baseVertices[i], indexType);
        }
    }

    void SetMetal4Requested(bool requested) { g_metal4Requested = requested; }

    std::unique_ptr<RenderBackend> CreateRenderBackend(BackendType type) {
        switch (type) {
            case BackendType::OpenGL:
                return std::make_unique<GLBackend>();
            case BackendType::Vulkan:
#ifdef HAS_VULKAN
                return CreateVulkanBackend();
#else
                Log::Error("Vulkan backend not available (compiled without HAS_VULKAN)");
                return nullptr;
#endif
            case BackendType::Metal:
#ifdef HAS_METAL
                // --metal4: the Metal 4 runtime path where the Mac offers it,
                // the Metal 3 backend everywhere else (docs/metal4.md).
                return g_metal4Requested ? CreateMetal4Backend() : CreateMetalBackend();
#else
                Log::Error("Metal backend not available (compiled without HAS_METAL)");
                return nullptr;
#endif
        }
        return nullptr;
    }

} // namespace Render
