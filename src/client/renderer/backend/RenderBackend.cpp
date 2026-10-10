// File: src/client/renderer/backend/RenderBackend.cpp
#include "RenderBackend.hpp"
#include "opengl/GLBackend.hpp"
#include "common/core/Log.hpp"

#include <cstdlib>
#include <cstring>

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
    // Metal 4 by default (2026-10-09): Metal4Backend takes the Metal 4
    // path where the Mac offers it and runs the Metal 3 path otherwise.
    // --metal3 (PlatformMain) or OBEY_METAL4=0 keep the Metal 3 backend.
    static bool g_metal4Requested = true;
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
            {
                const char* env = std::getenv("OBEY_METAL4");
                const bool metal4 = g_metal4Requested && !(env && std::strcmp(env, "0") == 0);
                Log::Info("Metal backend: %s", metal4 ? "Metal 4 path where this Mac offers it (--metal3 / OBEY_METAL4=0 for Metal 3)"
                                                      : "Metal 3 backend");
                return metal4 ? CreateMetal4Backend() : CreateMetalBackend();
            }
#else
                Log::Error("Metal backend not available (compiled without HAS_METAL)");
                return nullptr;
#endif
        }
        return nullptr;
    }

} // namespace Render
