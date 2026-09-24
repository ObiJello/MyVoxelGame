// File: src/client/world/ClientBiomeZoom.hpp
//
// The client's BiomeManager seed — MC ClientLevel builds its BiomeManager from
// the hashedSeed in the login and respawn packets (CommonPlayerSpawnInfo). Here
// it arrives on LoginSuccess and ChangeDimensionS2C, and every client-side
// "biome of this block" read (ClientBlockAccess::GetBiome, the mesher's tint)
// zooms with it. Read from the main thread and the mesh workers, hence atomic.
//
// It is set before any chunk of the level arrives (LoginSuccess precedes the
// chunk stream; ChangeDimensionS2C precedes the new dimension's), so no mesh
// is ever built with a stale value in practice.
#pragma once

#include <atomic>
#include <cstdint>

namespace Client {

    inline std::atomic<int64_t> g_biomeZoomSeed{0};

    inline int64_t BiomeZoomSeed() {
        return g_biomeZoomSeed.load(std::memory_order_relaxed);
    }

    inline void SetBiomeZoomSeed(int64_t hashedSeed) {
        g_biomeZoomSeed.store(hashedSeed, std::memory_order_relaxed);
    }

} // namespace Client
