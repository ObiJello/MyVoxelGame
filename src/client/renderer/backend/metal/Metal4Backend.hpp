// File: src/client/renderer/backend/metal/Metal4Backend.hpp
//
// The Metal 4 runtime path (docs/metal4.md): macOS 26 on an Apple-silicon
// GPU reporting MTLGPUFamilyMetal4, asked for with --metal4. Derives from
// MetalBackend — resources, shaders and pipelines are the base's — and
// switches its command side to Metal 4 (MetalFour.mm). Any other Mac gets
// MetalBackend itself (CreateMetal4Backend falls back), so the shipped
// 12.7.6 build runs everywhere it ran before.
#pragma once

#include "MetalBackend.hpp"

namespace Render {

    class Metal4Backend final : public MetalBackend {
    public:
        // Runtime: the OS and the device's family (false below macOS 26).
        static bool Supported(id<MTLDevice> device);

        bool Initialize(GLFWwindow* window) override;
        void Shutdown() override;
    };

    std::unique_ptr<RenderBackend> CreateMetalBackend();    // MetalBackend.mm
    std::unique_ptr<RenderBackend> CreateMetal4Backend();

} // namespace Render
