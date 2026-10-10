// File: src/client/renderer/backend/metal/Metal4Backend.mm
#include "Metal4Backend.hpp"
#include "common/core/Log.hpp"

namespace Render {

    bool Metal4Backend::Supported(id<MTLDevice> device) {
        if (@available(macOS 26.0, *)) {
            return device != nil && [device supportsFamily:MTLGPUFamilyMetal4];
        }
        return false;
    }

    std::unique_ptr<RenderBackend> CreateMetal4Backend() {
        @autoreleasepool {
            id<MTLDevice> device = MTLCreateSystemDefaultDevice();
            if (!Metal4Backend::Supported(device)) {
                Log::Info("Metal 4 requested (--metal4) but %s - the Metal 3 backend runs instead",
                          device ? "this GPU or macOS does not offer it (macOS 26 + MTLGPUFamilyMetal4)"
                                 : "no Metal device was found");
                return CreateMetalBackend();
            }
        }
        return std::make_unique<Metal4Backend>();
    }

    bool Metal4Backend::Initialize(GLFWwindow* window) {
        // Everything the Metal 3 path needs, first: device, layer, slots,
        // shaders, pipelines. Then the command side moves to Metal 4; if
        // that fails the Metal 3 path it just set up carries on.
        if (!MetalBackend::Initialize(window)) return false;
        // Not an error: every Mac before macOS 26 / Apple silicon lands here.
        if (!M4Setup()) Log::Info("Metal4Backend: this Mac offers no Metal 4 - the Metal 3 path runs");
        else            Log::Info("Metal4Backend: the Metal 4 path runs");
        return true;
    }

    void Metal4Backend::Shutdown() {
        MetalBackend::Shutdown();   // drains the Metal 4 frames first (M4Shutdown)
    }

} // namespace Render
