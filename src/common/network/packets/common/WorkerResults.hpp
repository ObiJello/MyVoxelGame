// File: src/common/network/packets/common/WorkerResults.hpp
//
// NOT network packets — internal worker-thread → main-thread message types.
// Lives alongside packets because they share the message-queue plumbing
// (Network::MessageQueue) and the broader "data flowing between threads"
// pattern. They never go on the wire.
//
//   ChunkGenResult       — Server worker → Server main: completed chunk gen
//   MeshBuildResult      — Client worker → Client render: completed section mesh
//   SerializedChunkData  — In-flight compressed chunk payload (helper)

#pragma once

#include "common/world/math/WorldMath.hpp"
#include "common/world/chunk/Chunk.hpp"
#include "client/renderer/culling/VisibilitySet.hpp"

#include <memory>
#include <vector>
#include <string>
#include <chrono>
#include <cstdint>

namespace Network {

    // Chunk generation results (Server worker → Server thread).
    struct ChunkGenResult {
        Game::Math::ChunkPos position;
        std::shared_ptr<Game::Chunk> chunk;
        bool success = false;
        std::chrono::steady_clock::time_point completeTime;
        std::string errorMessage;

        ChunkGenResult() = default;
        ChunkGenResult(Game::Math::ChunkPos pos, std::shared_ptr<Game::Chunk> chunkPtr, bool succeeded)
            : position(pos), chunk(std::move(chunkPtr)), success(succeeded)
            , completeTime(std::chrono::steady_clock::now()) {}
    };

    // Mesh build results (Client worker → Client render thread).
    struct MeshBuildResult {
        Game::Math::ChunkPos chunkPos;
        int sectionY;
        uint32_t generation = 0;   // Version number for staleness checking
        uint8_t  neighborMask = 0; // Neighbor presence mask (PX=1, NX=2, PZ=4, NZ=8)

        struct SectionMeshData {
            // Opaque geometry (solid blocks like stone, dirt)
            std::vector<float>    opaqueVertices;
            std::vector<uint32_t> opaqueIndices;

            // Cutout geometry (alpha-test blocks like leaves, grass)
            std::vector<float>    cutoutVertices;
            std::vector<uint32_t> cutoutIndices;

            // Translucent geometry (blended blocks like glass, water, ice)
            std::vector<float>    translucentVertices;
            std::vector<uint32_t> translucentIndices;

            // Layer counts for validation
            size_t opaqueVertexCount      = 0;
            size_t opaqueIndexCount       = 0;
            size_t cutoutVertexCount      = 0;
            size_t cutoutIndexCount       = 0;
            size_t translucentVertexCount = 0;
            size_t translucentIndexCount  = 0;

            bool IsEmpty() const {
                return opaqueVertices.empty() && cutoutVertices.empty() && translucentVertices.empty();
            }

            size_t GetTotalVertexCount() const {
                return opaqueVertexCount + cutoutVertexCount + translucentVertexCount;
            }

            size_t GetTotalIndexCount() const {
                return opaqueIndexCount + cutoutIndexCount + translucentIndexCount;
            }
        };

        SectionMeshData         meshData;
        Render::VisibilitySet   visibilitySet;  // Occlusion data: which face pairs can see through
        bool                    success = false;
        std::chrono::steady_clock::time_point completeTime;

        MeshBuildResult() = default;
        MeshBuildResult(Game::Math::ChunkPos pos, int secY)
            : chunkPos(pos), sectionY(secY), completeTime(std::chrono::steady_clock::now()) {}
    };

    // Compressed chunk data for network transmission. Helper used during
    // ChunkDataS2CPacket assembly; not itself a packet.
    struct SerializedChunkData {
        std::vector<uint8_t> blockData;     // Compressed block IDs
        std::vector<uint8_t> lightData;     // Light data (optional)
        std::vector<uint8_t> biomeData;     // Biome data (optional)
        std::vector<uint8_t> heightmapData; // Heightmap data (optional)

        uint32_t uncompressedSize = 0;
        uint32_t compressionType  = 0; // 0 = none, 1 = zlib, etc.
        bool     hasLightData     = false;
        bool     hasBiomeData     = false;
        bool     hasHeightmapData = false;

        size_t GetTotalSize() const {
            return blockData.size() + lightData.size() + biomeData.size() + heightmapData.size();
        }
    };

} // namespace Network
