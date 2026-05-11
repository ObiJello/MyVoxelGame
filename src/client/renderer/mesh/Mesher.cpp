// File: src/client/renderer/mesh/Mesher.cpp
#include "Mesher.hpp"
#include "../culling/VisGraph.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/level/World.hpp"
#include "common/core/Log.hpp"
#include "common/core/Config.hpp"
#include "common/core/Profiling_Tracy.hpp"
#include <chrono>
#include <algorithm>
#include <cstring>

namespace Render {

    // Thread-local block property cache and UV cache definitions
    thread_local std::array<Mesher::CachedBlockProps, Mesher::BLOCK_ID_COUNT> Mesher::s_blockPropsCache{};
    thread_local bool Mesher::s_blockPropsCacheValid = false;
    thread_local std::unordered_map<const Game::FaceDef*, glm::vec4> Mesher::s_faceUVCache;

    // Face normal vectors for each block face
    static const glm::vec3 FACE_NORMALS[] = {
        { 0.0f,  1.0f,  0.0f}, // PositiveY (Top)
        { 0.0f, -1.0f,  0.0f}, // NegativeY (Bottom)
        { 0.0f,  0.0f,  1.0f}, // PositiveZ (Front)
        { 0.0f,  0.0f, -1.0f}, // NegativeZ (Back)
        { 1.0f,  0.0f,  0.0f}, // PositiveX (Right)
        {-1.0f,  0.0f,  0.0f}  // NegativeX (Left)
    };

    // Face offset vectors for neighbor checking
    static const glm::ivec3 FACE_OFFSETS[] = {
        { 0,  1,  0}, // PositiveY
        { 0, -1,  0}, // NegativeY
        { 0,  0,  1}, // PositiveZ
        { 0,  0, -1}, // NegativeZ
        { 1,  0,  0}, // PositiveX
        {-1,  0,  0}  // NegativeX
    };

    Mesher::Mesher(const MeshConfig& config) : m_config(config), m_world(nullptr) {
        m_lastStats = {};
        m_sectionBaseWorldX = 0;
        m_sectionBaseWorldY = 0;
        m_sectionBaseWorldZ = 0;
        std::memset(m_opaqueCache, 0, sizeof(m_opaqueCache));
        m_fluidBuilder = std::make_unique<FluidMeshBuilder>();
    }

    void Mesher::SetWorld(Game::World* world) {
        m_world = world;
    }

    void Mesher::EnsureBlockPropsCache() {
        if (s_blockPropsCacheValid) return;

        for (size_t i = 0; i < BLOCK_ID_COUNT; ++i) {
            auto blockId = static_cast<Game::BlockID>(i);
            const Game::Block& block = Game::BlockRegistry::Get(blockId);
            s_blockPropsCache[i].isOpaque = block.opaque;
            switch (block.renderLayer) {
                case Game::RenderLayer::Cutout:      s_blockPropsCache[i].renderLayer = RenderLayer::Cutout; break;
                case Game::RenderLayer::Translucent:  s_blockPropsCache[i].renderLayer = RenderLayer::Translucent; break;
                default:                              s_blockPropsCache[i].renderLayer = RenderLayer::Opaque; break;
            }
        }
        s_blockPropsCacheValid = true;
    }

    void Mesher::BuildOpaqueCache(const Game::IBlockAccess& blocks, Game::Math::ChunkPos chunkPos, int sectionY) {
        PROFILE_ZONE;
        m_sectionBaseWorldX = chunkPos.x * 16;
        m_sectionBaseWorldY = sectionY * 16 + Config::MinY;
        m_sectionBaseWorldZ = chunkPos.z * 16;

        // Sample a 18x18x18 region: the section (16^3) plus a 1-block border on all sides.
        // This covers every neighbor position that AO and face culling will access.
        for (int lx = -1; lx <= 16; ++lx) {
            for (int ly = -1; ly <= 16; ++ly) {
                for (int lz = -1; lz <= 16; ++lz) {
                    int wx = m_sectionBaseWorldX + lx;
                    int wy = m_sectionBaseWorldY + ly;
                    int wz = m_sectionBaseWorldZ + lz;
                    Game::BlockID bid = blocks.GetBlock(wx, wy, wz);
                    m_opaqueCache[lx + 1][ly + 1][lz + 1] = s_blockPropsCache[static_cast<uint16_t>(bid)].isOpaque;
                }
            }
        }
    }

    bool Mesher::GetCachedOpaque(int worldX, int worldY, int worldZ) const {
        int lx = worldX - m_sectionBaseWorldX + 1;
        int ly = worldY - m_sectionBaseWorldY + 1;
        int lz = worldZ - m_sectionBaseWorldZ + 1;

        // Bounds check — positions outside the cached region fall back to non-opaque
        if (lx < 0 || lx >= 18 || ly < 0 || ly >= 18 || lz < 0 || lz >= 18) {
            return false;
        }
        return m_opaqueCache[lx][ly][lz];
    }

    void Mesher::BuildSectionMesh(const Game::IBlockAccess& blocks, Game::Math::ChunkPos chunkPos, int sectionY, SectionMesh& outMesh) {
        PROFILE_ZONE;
        auto startTime = std::chrono::high_resolution_clock::now();

        m_lastStats = {};

        outMesh.Clear();
        outMesh.chunkPos = chunkPos;
        outMesh.sectionY = sectionY;
        outMesh.Reserve(1024);

        // Populate per-thread block property cache (once per thread lifetime)
        EnsureBlockPropsCache();

        // Build the 18x18x18 opaque cache for this section — all subsequent
        // face culling and AO calculations index into this instead of calling
        // GetBlock + IsBlockOpaque repeatedly.
        BuildOpaqueCache(blocks, chunkPos, sectionY);

        // Build visibility graph from the opaque cache (indices offset by +1 for the border)
        VisGraph visGraph;
        for (int y = 0; y < 16; y++)
            for (int z = 0; z < 16; z++)
                for (int x = 0; x < 16; x++)
                    if (m_opaqueCache[x + 1][y + 1][z + 1])
                        visGraph.setOpaque(x, y, z);

        for (int localX = 0; localX < 16; ++localX) {
            for (int sectionLocalY = 0; sectionLocalY < 16; ++sectionLocalY) {
                for (int localZ = 0; localZ < 16; ++localZ) {
                    int worldY = sectionY * 16 + sectionLocalY + Config::MinY;
                    int worldX = chunkPos.x * 16 + localX;
                    int worldZ = chunkPos.z * 16 + localZ;

                    Game::BlockID blockId = blocks.GetBlock(worldX, worldY, worldZ);

                    if (blockId != Game::BlockID::Air) {
                        ProcessBlock(blocks, chunkPos, localX, worldY, localZ, sectionY, blockId, outMesh);
                    }
                }
            }
        }

        // Resolve visibility graph (which face pairs can see through this section)
        outMesh.visibilitySet = visGraph.resolve();

        auto endTime = std::chrono::high_resolution_clock::now();
        m_lastStats.buildTimeMs = std::chrono::duration<float, std::milli>(endTime - startTime).count();
    }

    // BuildChunkMesh removed - use BuildSectionMesh with IBlockAccess instead

    void Mesher::ProcessBlock(const Game::IBlockAccess& blocks, Game::Math::ChunkPos chunkPos,
                             int localX, int worldY, int localZ,
                             int sectionY, Game::BlockID blockId, SectionMesh& mesh) {

        int worldX = chunkPos.x * 16 + localX;
        int worldZ = chunkPos.z * 16 + localZ;
        // blockId passed from caller — avoids redundant GetBlock() virtual call

        // Handle fluid blocks separately
        if (blockId == Game::BlockID::Water || blockId == Game::BlockID::Lava) {
            if (m_fluidBuilder) {
                m_fluidBuilder->BuildFluidBlock(blocks, chunkPos, worldX, worldY, worldZ, mesh);
                m_lastStats.facesGenerated++;
            }
            return;
        }

        const Game::BlockModel& model = Game::BlockRegistry::GetBlockModel(blockId);
        glm::vec3 worldPos = LocalToWorldPos(chunkPos, localX, worldY, localZ);

        // Use cached render layer instead of registry lookup
        RenderLayer blockLayer = s_blockPropsCache[static_cast<uint16_t>(blockId)].renderLayer;

        for (const auto& element : model.elements) {
            for (const auto& [faceDir, faceDef] : element.faces) {
                BlockFace blockFace = static_cast<BlockFace>(static_cast<int>(faceDir));

                // Face culling uses the pre-built opaque cache (no GetBlock/registry calls)
                if (m_config.enableFaceCulling) {
                    if (ShouldCullFace(worldX, worldY, worldZ, blockFace)) {
                        m_lastStats.facesCulled++;
                        continue;
                    }
                }

                glm::vec3 faceNormal = GetFaceNormal(blockFace);
                AddBlockFace(blocks, model, element, faceDir, faceDef, worldPos, faceNormal, blockId, worldX, worldY, worldZ, blockLayer, mesh);
                m_lastStats.facesGenerated++;
            }
        }
    }

    void Mesher::AddBlockFace(const Game::IBlockAccess& blocks,
                             const Game::BlockModel& model, const Game::Element& element,
                             Game::FaceDir faceDir, const Game::FaceDef& faceDef,
                             glm::vec3 blockPos, glm::vec3 faceNormal, Game::BlockID blockId,
                             int worldX, int worldY, int worldZ, RenderLayer layer, SectionMesh& mesh) {

        // Lookup UV rect from thread-local cache (persists across Mesher instances,
        // so texture resolution only happens once per FaceDef per thread lifetime)
        glm::vec4 uvRect;
        auto cacheIt = s_faceUVCache.find(&faceDef);
        if (cacheIt != s_faceUVCache.end()) {
            uvRect = cacheIt->second;
        } else {
            std::string texturePath = model.ResolveTexture(faceDef.textureRef);
            if (!GetTextureUV(texturePath, uvRect)) {
                GetTextureUV("missingno", uvRect);
            }
            s_faceUVCache[&faceDef] = uvRect;
        }

        // **FIXED**: Calculate biome tint based on tintIndex
        glm::vec4 tintColor(1.0f, 1.0f, 1.0f, 1.0f); // Default white (no tint)

        if (m_config.enableBiomeTinting && faceDef.tintIndex >= 0) {
            if (faceDef.tintIndex == 0) {
                // Tint index 0 - usually foliage color (leaves, vines)
                tintColor = CalculateFoliageTint(blockId, worldX, worldY, worldZ);
            } else if (faceDef.tintIndex == 1) {
                // Tint index 1 - usually grass color (grass blocks)
                tintColor = CalculateGrassTint(blockId, worldX, worldY, worldZ);
            } else {
                // Other tint indices - you can add more specific tinting here
                tintColor = CalculateBiomeTint(blockId, worldX, worldY, worldZ);
            }
        }

        // Convert face direction to our BlockFace enum
        BlockFace blockFace = static_cast<BlockFace>(static_cast<int>(faceDir));

        // Create face vertices (stack-allocated, no heap alloc)
        std::array<Vertex, 4> faceVerts = CreateFaceVertices(blockPos, blockFace, uvRect, tintColor);

        // Bake AO and directional shading into vertex colors (Minecraft-style)
        // All math is in gamma space — shade values are direct multipliers, matching Minecraft.
        float directionalShade = GetDirectionalShade(blockFace);
        for (int v = 0; v < 4; ++v) {
            float aoShade = CalculateVertexAO(blocks, worldX, worldY, worldZ, blockFace, v);
            float finalShade = aoShade * directionalShade;
            glm::vec4 c = faceVerts[v].GetColor();
            c.r *= finalShade;
            c.g *= finalShade;
            c.b *= finalShade;
            faceVerts[v].SetColor(c);
        }

        // Add to appropriate mesh layer
        switch (layer) {
            case RenderLayer::Opaque:
                GenerateQuad(faceVerts, mesh.opaqueVerts, mesh.opaqueIdxs);
                break;
            case RenderLayer::Cutout:
                GenerateQuad(faceVerts, mesh.cutoutVerts, mesh.cutoutIdxs);
                break;
            case RenderLayer::Translucent:
                GenerateQuad(faceVerts, mesh.translucentVerts, mesh.translucentIdxs);
                break;
        }

        m_lastStats.quadsGenerated++;
    }

    // **NEW**: Grass-specific tinting (tint index 1)
    glm::vec4 Mesher::CalculateGrassTint(Game::BlockID blockId, int worldX, int worldY, int worldZ) {
        // For now, return a simple green tint for grass
        // In a full implementation, this would sample from a colormap based on biome/temperature/humidity

        // Different blocks might have different base grass colors
        switch (blockId) {
            case Game::BlockID::Grass:
                return glm::vec4(0.5f, 0.7f, 0.3f, 1.0f); // Bright grass green
            default:
                return glm::vec4(0.5f, 0.7f, 0.3f, 1.0f); // Default grass green
        }
    }

    // **NEW**: Foliage-specific tinting (tint index 0)
    glm::vec4 Mesher::CalculateFoliageTint(Game::BlockID blockId, int worldX, int worldY, int worldZ) {
        // For now, return a simple green tint for foliage
        switch (blockId) {
            case Game::BlockID::OakLeaves:
                return glm::vec4(0.4f, 0.7f, 0.2f, 1.0f); // Oak leaves green
            case Game::BlockID::BirchLeaves:
                return glm::vec4(0.6f, 0.9f, 0.5f, 1.0f); // Brighter cherry green
            default:
                return glm::vec4(0.4f, 0.6f, 0.2f, 1.0f); // Default foliage green
        }
    }

    // **UPDATED**: General biome tinting
    glm::vec4 Mesher::CalculateBiomeTint(Game::BlockID blockId, int worldX, int worldY, int worldZ) {
        // This is where you'd implement full biome-based tinting
        // For now, return a neutral tint
        return glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
    }

    void Mesher::GenerateQuad(const std::array<Vertex, 4>& quadVerts,
                             std::vector<Vertex>& outVerts, std::vector<uint32_t>& outIndices) {
        uint32_t baseIndex = static_cast<uint32_t>(outVerts.size());

        // Add vertices
        outVerts.insert(outVerts.end(), quadVerts.begin(), quadVerts.end());

        // **FIXED**: Correct triangle winding for counter-clockwise faces (viewed from outside)
        // Vertices are ordered: 0=bottom-left, 1=bottom-right, 2=top-right, 3=top-left
        outIndices.insert(outIndices.end(), {
            baseIndex + 0, baseIndex + 1, baseIndex + 2,  // First triangle: 0->1->2
            baseIndex + 0, baseIndex + 2, baseIndex + 3   // Second triangle: 0->2->3
        });
    }

    bool Mesher::ShouldCullFace(int worldX, int worldY, int worldZ, BlockFace face) {
        // Use the pre-built opaque cache: neighbor is opaque → cull this face.
        // The offset table gives us the neighbor position directly.
        const glm::ivec3& offset = FACE_OFFSETS[static_cast<int>(face)];
        return GetCachedOpaque(worldX + offset.x, worldY + offset.y, worldZ + offset.z);
    }

    // **UPDATED**: Now supports cross-chunk neighbor lookup via IBlockAccess
    Game::BlockID Mesher::GetNeighborBlock(const Game::IBlockAccess& blocks, int worldX, int worldY, int worldZ,
                                          BlockFace face) {
        glm::ivec3 offset = FACE_OFFSETS[static_cast<int>(face)];

        // Calculate neighbor position in world coordinates
        int neighborX = worldX + offset.x;
        int neighborY = worldY + offset.y;
        int neighborZ = worldZ + offset.z;

        // IBlockAccess handles cross-chunk boundaries automatically
        return blocks.GetBlock(neighborX, neighborY, neighborZ);
    }

    bool Mesher::GetTextureUV(const std::string& texturePath, glm::vec4& uvRect) {
        if (g_atlasBuilder) {
            AtlasUVRect atlasUV;
            if (g_atlasBuilder->GetUVRect(texturePath, atlasUV)) {
                uvRect = glm::vec4(atlasUV.uvMin.x, atlasUV.uvMin.y,
                                  atlasUV.uvMax.x, atlasUV.uvMax.y);
                return true;
            }
        }

        // Fallback to error texture
        uvRect = glm::vec4(0.0f, 0.0f, 1.0f, 1.0f);
        return false;
    }

    std::array<Vertex, 4> Mesher::CreateFaceVertices(glm::vec3 blockPos, BlockFace face,
                                                  const glm::vec4& uvRect, const glm::vec4& tint) {
        std::array<Vertex, 4> vertices;
        glm::vec3 normal = GetFaceNormal(face);

        // Exact block boundary positions (matches Minecraft — no expansion)
        switch (face) {
            case BlockFace::PositiveY: // Top face (+Y)
                // MC FaceBakery convention (FaceInfo.UP + BlockElementFace.getU/getV):
                //   minZ (north/back) → vMin (top of texture);
                //   maxZ (south/front) → vMax (bottom of texture).
                // i.e. "north is up" when looking down at the texture. Sides (NSEW) match
                // MC already; only top/bottom were V-flipped before. Without this, blocks
                // with directional top textures (beacon glass, stripped logs, sandstone)
                // render with their top rotated 180° from MC.
                vertices[0] = Vertex(blockPos + glm::vec3(0, 1, 1), normal, glm::vec2(uvRect.x, uvRect.w), tint);
                vertices[1] = Vertex(blockPos + glm::vec3(1, 1, 1), normal, glm::vec2(uvRect.z, uvRect.w), tint);
                vertices[2] = Vertex(blockPos + glm::vec3(1, 1, 0), normal, glm::vec2(uvRect.z, uvRect.y), tint);
                vertices[3] = Vertex(blockPos + glm::vec3(0, 1, 0), normal, glm::vec2(uvRect.x, uvRect.y), tint);
                break;

            case BlockFace::NegativeY: // Bottom face (-Y)
                // MC FaceBakery (FaceInfo.DOWN): maxZ → vMin, minZ → vMax. Inverse of UP.
                vertices[0] = Vertex(blockPos + glm::vec3(0, 0, 0), normal, glm::vec2(uvRect.x, uvRect.w), tint);
                vertices[1] = Vertex(blockPos + glm::vec3(1, 0, 0), normal, glm::vec2(uvRect.z, uvRect.w), tint);
                vertices[2] = Vertex(blockPos + glm::vec3(1, 0, 1), normal, glm::vec2(uvRect.z, uvRect.y), tint);
                vertices[3] = Vertex(blockPos + glm::vec3(0, 0, 1), normal, glm::vec2(uvRect.x, uvRect.y), tint);
                break;

            case BlockFace::PositiveZ: // Front face (+Z)
                vertices[0] = Vertex(blockPos + glm::vec3(0, 0, 1), normal, glm::vec2(uvRect.x, uvRect.w), tint);
                vertices[1] = Vertex(blockPos + glm::vec3(1, 0, 1), normal, glm::vec2(uvRect.z, uvRect.w), tint);
                vertices[2] = Vertex(blockPos + glm::vec3(1, 1, 1), normal, glm::vec2(uvRect.z, uvRect.y), tint);
                vertices[3] = Vertex(blockPos + glm::vec3(0, 1, 1), normal, glm::vec2(uvRect.x, uvRect.y), tint);
                break;

            case BlockFace::NegativeZ: // Back face (-Z)
                vertices[0] = Vertex(blockPos + glm::vec3(1, 0, 0), normal, glm::vec2(uvRect.x, uvRect.w), tint);
                vertices[1] = Vertex(blockPos + glm::vec3(0, 0, 0), normal, glm::vec2(uvRect.z, uvRect.w), tint);
                vertices[2] = Vertex(blockPos + glm::vec3(0, 1, 0), normal, glm::vec2(uvRect.z, uvRect.y), tint);
                vertices[3] = Vertex(blockPos + glm::vec3(1, 1, 0), normal, glm::vec2(uvRect.x, uvRect.y), tint);
                break;

            case BlockFace::PositiveX: // Right face (+X)
                vertices[0] = Vertex(blockPos + glm::vec3(1, 0, 1), normal, glm::vec2(uvRect.x, uvRect.w), tint);
                vertices[1] = Vertex(blockPos + glm::vec3(1, 0, 0), normal, glm::vec2(uvRect.z, uvRect.w), tint);
                vertices[2] = Vertex(blockPos + glm::vec3(1, 1, 0), normal, glm::vec2(uvRect.z, uvRect.y), tint);
                vertices[3] = Vertex(blockPos + glm::vec3(1, 1, 1), normal, glm::vec2(uvRect.x, uvRect.y), tint);
                break;

            case BlockFace::NegativeX: // Left face (-X)
                vertices[0] = Vertex(blockPos + glm::vec3(0, 0, 0), normal, glm::vec2(uvRect.x, uvRect.w), tint);
                vertices[1] = Vertex(blockPos + glm::vec3(0, 0, 1), normal, glm::vec2(uvRect.z, uvRect.w), tint);
                vertices[2] = Vertex(blockPos + glm::vec3(0, 1, 1), normal, glm::vec2(uvRect.z, uvRect.y), tint);
                vertices[3] = Vertex(blockPos + glm::vec3(0, 1, 0), normal, glm::vec2(uvRect.x, uvRect.y), tint);
                break;
        }

        return vertices;
    }

    glm::vec3 Mesher::GetFaceNormal(BlockFace face) {
        return FACE_NORMALS[static_cast<int>(face)];
    }

    // Minecraft directional face shading (from ClientLevel.java getShade())
    float Mesher::GetDirectionalShade(BlockFace face) {
        switch (face) {
            case BlockFace::PositiveY: return 1.0f;  // UP
            case BlockFace::NegativeY: return 0.5f;  // DOWN
            case BlockFace::PositiveZ: return 0.8f;  // SOUTH
            case BlockFace::NegativeZ: return 0.8f;  // NORTH
            case BlockFace::PositiveX: return 0.6f;  // EAST
            case BlockFace::NegativeX: return 0.6f;  // WEST
            default: return 1.0f;
        }
    }

    // Per-vertex AO neighbor offset tables.
    // For each face direction, for each vertex (0-3), defines:
    //   {edge1_dx, edge1_dy, edge1_dz, edge2_dx, edge2_dy, edge2_dz, corner_dx, corner_dy, corner_dz}
    // The offsets are relative to the block position, shifted by the face normal.
    // Vertex ordering matches CreateFaceVertices() winding.

    struct AOVertexNeighbors {
        glm::ivec3 edge1;
        glm::ivec3 edge2;
        glm::ivec3 corner;
    };

    // For each face, 4 vertices, each with 2 edge neighbors and 1 corner neighbor
    // All offsets include the face normal direction (we sample in the plane one step out from the face)
    static const AOVertexNeighbors AO_NEIGHBORS[6][4] = {
        // PositiveY (Top face, +Y) — vertices: 0=front-left, 1=front-right, 2=back-right, 3=back-left
        {
            { {-1, 1, 0}, { 0, 1, 1}, {-1, 1, 1} },  // v0: west + south edges, SW corner
            { { 1, 1, 0}, { 0, 1, 1}, { 1, 1, 1} },  // v1: east + south edges, SE corner
            { { 1, 1, 0}, { 0, 1,-1}, { 1, 1,-1} },  // v2: east + north edges, NE corner
            { {-1, 1, 0}, { 0, 1,-1}, {-1, 1,-1} },  // v3: west + north edges, NW corner
        },
        // NegativeY (Bottom face, -Y) — vertices: 0=back-left, 1=back-right, 2=front-right, 3=front-left
        {
            { {-1,-1, 0}, { 0,-1,-1}, {-1,-1,-1} },  // v0
            { { 1,-1, 0}, { 0,-1,-1}, { 1,-1,-1} },  // v1
            { { 1,-1, 0}, { 0,-1, 1}, { 1,-1, 1} },  // v2
            { {-1,-1, 0}, { 0,-1, 1}, {-1,-1, 1} },  // v3
        },
        // PositiveZ (Front/South face, +Z) — vertices: 0=bottom-left, 1=bottom-right, 2=top-right, 3=top-left
        {
            { {-1, 0, 1}, { 0,-1, 1}, {-1,-1, 1} },  // v0
            { { 1, 0, 1}, { 0,-1, 1}, { 1,-1, 1} },  // v1
            { { 1, 0, 1}, { 0, 1, 1}, { 1, 1, 1} },  // v2
            { {-1, 0, 1}, { 0, 1, 1}, {-1, 1, 1} },  // v3
        },
        // NegativeZ (Back/North face, -Z) — vertices: 0=bottom-right, 1=bottom-left, 2=top-left, 3=top-right
        {
            { { 1, 0,-1}, { 0,-1,-1}, { 1,-1,-1} },  // v0
            { {-1, 0,-1}, { 0,-1,-1}, {-1,-1,-1} },  // v1
            { {-1, 0,-1}, { 0, 1,-1}, {-1, 1,-1} },  // v2
            { { 1, 0,-1}, { 0, 1,-1}, { 1, 1,-1} },  // v3
        },
        // PositiveX (Right/East face, +X) — vertices: 0=bottom-front, 1=bottom-back, 2=top-back, 3=top-front
        {
            { { 1, 0, 1}, { 1,-1, 0}, { 1,-1, 1} },  // v0
            { { 1, 0,-1}, { 1,-1, 0}, { 1,-1,-1} },  // v1
            { { 1, 0,-1}, { 1, 1, 0}, { 1, 1,-1} },  // v2
            { { 1, 0, 1}, { 1, 1, 0}, { 1, 1, 1} },  // v3
        },
        // NegativeX (Left/West face, -X) — vertices: 0=bottom-back, 1=bottom-front, 2=top-front, 3=top-back
        {
            { {-1, 0,-1}, {-1,-1, 0}, {-1,-1,-1} },  // v0
            { {-1, 0, 1}, {-1,-1, 0}, {-1,-1, 1} },  // v1
            { {-1, 0, 1}, {-1, 1, 0}, {-1, 1, 1} },  // v2
            { {-1, 0,-1}, {-1, 1, 0}, {-1, 1,-1} },  // v3
        },
    };

    // Minecraft-style per-vertex AO calculation
    // Uses the exact Minecraft values: solid blocks = 0.2 shade, non-solid = 1.0
    // Formula: vertex_ao = (edge1 + edge2 + corner + center) * 0.25
    // Corner rule: if both edges are solid, corner is forced solid (prevents diagonal light leak)
    float Mesher::CalculateVertexAO(const Game::IBlockAccess& /*blocks*/, int worldX, int worldY, int worldZ,
                                    BlockFace face, int vertexIndex) {
        if (!m_config.enableAmbientOcclusion) {
            return 1.0f;
        }

        const auto& neighbors = AO_NEIGHBORS[static_cast<int>(face)][vertexIndex];

        // Use the pre-built opaque cache instead of GetBlock + IsBlockOpaque per sample.
        // This eliminates 12 block lookups + 12 registry lookups per face (3 per vertex × 4 vertices).
        bool edge1Solid = GetCachedOpaque(worldX + neighbors.edge1.x, worldY + neighbors.edge1.y, worldZ + neighbors.edge1.z);
        bool edge2Solid = GetCachedOpaque(worldX + neighbors.edge2.x, worldY + neighbors.edge2.y, worldZ + neighbors.edge2.z);

        float edge1Shade = edge1Solid ? 0.2f : 1.0f;
        float edge2Shade = edge2Solid ? 0.2f : 1.0f;

        // Corner rule: if both edges are solid, corner is forced solid (no diagonal light leak)
        float cornerShade;
        if (edge1Solid && edge2Solid) {
            cornerShade = 0.2f;
        } else {
            bool cornerSolid = GetCachedOpaque(worldX + neighbors.corner.x, worldY + neighbors.corner.y, worldZ + neighbors.corner.z);
            cornerShade = cornerSolid ? 0.2f : 1.0f;
        }

        float centerShade = 1.0f;
        return (edge1Shade + edge2Shade + cornerShade + centerShade) * 0.25f;
    }

    // Render layer classification — uses thread-local cache when available,
    // falls back to registry for initial population or external callers.
    RenderLayer ClassifyBlock(Game::BlockID blockId) {
        auto idx = static_cast<uint16_t>(blockId);
        if (Mesher::s_blockPropsCacheValid && idx < Mesher::BLOCK_ID_COUNT) {
            return Mesher::s_blockPropsCache[idx].renderLayer;
        }
        switch (Game::BlockRegistry::Get(blockId).renderLayer) {
            case Game::RenderLayer::Cutout:       return RenderLayer::Cutout;
            case Game::RenderLayer::Translucent:  return RenderLayer::Translucent;
            default:                              return RenderLayer::Opaque;
        }
    }

    bool IsBlockOpaque(Game::BlockID blockId) {
        auto idx = static_cast<uint16_t>(blockId);
        if (Mesher::s_blockPropsCacheValid && idx < Mesher::BLOCK_ID_COUNT) {
            return Mesher::s_blockPropsCache[idx].isOpaque;
        }
        return Game::BlockRegistry::Get(blockId).opaque;
    }

    bool IsBlockTranslucent(Game::BlockID blockId) {
        return ClassifyBlock(blockId) == RenderLayer::Translucent;
    }

} // namespace Render