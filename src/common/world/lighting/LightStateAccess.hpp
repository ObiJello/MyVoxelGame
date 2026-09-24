// File: src/common/world/lighting/LightStateAccess.hpp
//
// The light code's block reads: one section lookup and one container read,
// no validation logging (Chunk::StateAt warns on an out-of-range Y, and the
// light range deliberately reaches a section past each end of the world,
// where MC reads VOID_AIR).
#pragma once

#include "common/core/Config.hpp"
#include "common/world/chunk/Chunk.hpp"

#include <cstdint>

namespace Game::Lighting {

    // Raw global state id at chunk-local (x, z) and world y; air outside the
    // world's height.
    inline uint32_t StateIdAt(const Chunk& chunk, int localX, int worldY, int localZ) {
        if (worldY < Config::MinY || worldY > Config::MaxY) return 0;
        const int sectionIndex = (worldY - Config::MinY) >> 4;
        return chunk.GetSection(sectionIndex)->States().Get(
            static_cast<size_t>(((worldY & 15) << 8) | (localZ << 4) | localX));
    }

} // namespace Game::Lighting
