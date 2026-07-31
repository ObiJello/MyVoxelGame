// File: src/client/renderer/blockentity/BlockEntityRenderers.hpp
//
// Stage 2 / 6 / 8 / 9 / 10 BE renderer registration. Called once from
// PlatformMain after the render backend + atlas are ready. Mirrors MC
// `BlockEntityRenderers.java` which builds a Map<Type, Renderer> at boot.
#pragma once

namespace Render {

    // Construct & register every renderer. Idempotent.
    void RegisterAllBlockEntityRenderers();

} // namespace Render
