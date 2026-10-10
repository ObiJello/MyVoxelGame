// File: src/client/renderer/blockentity/SkyBlockRenderer.cpp
// See the header for what this draws and why it is depth-only.

#include "SkyBlockRenderer.hpp"

#include "../backend/RenderBackend.hpp"
#include "../core/RenderOrigin.hpp"
#include "../mesh/ChunkRenderer.hpp"
#include "../../world/ClientChunkManager.hpp"
#include "common/core/Config.hpp"
#include "common/core/Log.hpp"
#include "common/core/Profiling_Tracy.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/block/entity/BlockEntityTypes.hpp"
#include "common/world/block/entity/SkyFacesBlockEntity.hpp"
#include "common/world/block/BlockModel.hpp"
#include "common/world/chunk/Chunk.hpp"
#include "common/world/math/WorldMath.hpp"
#include "common/world/block/BlockState.hpp"

#include <algorithm>
#include <string>
#include <utility>

namespace PlatformMain { std::string GetAssetPath(const std::string&); }

namespace Render {

    SkyBlockRenderer g_skyBlockRenderer;

    namespace {
        // GL fallback for a build that cannot find shaders/sky_block.{vert,
        // frag}; must track those files (same contract as EndPortalRenderer).
        const char* kVertSource = R"(
#version 330 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec2 aTexCoord;
layout (location = 2) in vec4 aColor;
uniform mat4 uMVP;
uniform vec4 uPortalClipPlane;
void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
    gl_ClipDistance[0] = (any(notEqual(uPortalClipPlane.xyz, vec3(0.0))))
        ? dot(uPortalClipPlane.xyz, aPos) + uPortalClipPlane.w
        : 1.0;
}
)";
        const char* kFragSource = R"(
#version 330 core
out vec4 FragColor;
void main() {
    FragColor = vec4(0.0);
}
)";

        // The six faces of the unit cell, each wound counter-clockwise seen
        // from OUTSIDE the block (back-face culling keeps only the faces the
        // camera looks at), in the order of kNeighbour below.
        constexpr int kFaceCount = 6;
        const glm::ivec3 kNeighbour[kFaceCount] = {
            { 0, -1,  0},   // down
            { 0,  1,  0},   // up
            { 0,  0, -1},   // north
            { 0,  0,  1},   // south
            {-1,  0,  0},   // west
            { 1,  0,  0},   // east
        };
        const glm::vec3 kCorners[kFaceCount][4] = {
            {{0, 0, 0}, {1, 0, 0}, {1, 0, 1}, {0, 0, 1}},   // down
            {{0, 1, 1}, {1, 1, 1}, {1, 1, 0}, {0, 1, 0}},   // up
            {{1, 0, 0}, {0, 0, 0}, {0, 1, 0}, {1, 1, 0}},   // north
            {{0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}},   // south
            {{0, 0, 0}, {0, 0, 1}, {0, 1, 1}, {0, 1, 0}},   // west
            {{1, 0, 1}, {1, 0, 0}, {1, 1, 0}, {1, 1, 1}},   // east
        };

        constexpr int kWorldTop = Config::MinY + Game::Math::SECTIONS_PER_CHUNK * Game::Math::SECTION_HEIGHT;

        // Does the block in the neighbouring cell hide this face — the
        // chunk mesher's rule (Mesher::EnsureBlockPropsCache: an opaque
        // full cube that is not drawn by a block-entity renderer), plus
        // another sky block: the face between two of them is inside the
        // window and needs no depth. Leaves, glass, slabs and the rest keep
        // the face, so the sky shows through and around them.
        bool HidesFace(Game::BlockID id, Game::BlockStateIndex stateIndex) {
            if (id == Game::BlockID::SkyBlock) return true;
            if (id == Game::BlockID::Air) return false;
            if (!Game::BlockRegistry::Get(id).opaque) return false;
            const bool brushable = id == Game::BlockID::SuspiciousSand ||
                                   id == Game::BlockID::SuspiciousGravel ||
                                   id == Game::BlockID::ChiseledBookshelf;
            if (Game::BlockEntityTypes::HasBlockEntity(id) && !brushable) return false;
            return Game::BlockRegistry::IsOcclusionFullCube(Game::BlockStates::FromIndex(id, stateIndex));
        }

        // A model face key (Game::FaceDir: up, down, north, south, west,
        // east) as an MC Direction — the order of kNeighbour / kCorners.
        Game::Direction FaceDirToDirection(Game::FaceDir dir) {
            switch (dir) {
                case Game::FaceDir::Up:    return Game::Direction::Up;
                case Game::FaceDir::Down:  return Game::Direction::Down;
                case Game::FaceDir::North: return Game::Direction::North;
                case Game::FaceDir::South: return Game::Direction::South;
                case Game::FaceDir::West:  return Game::Direction::West;
                case Game::FaceDir::East:  return Game::Direction::East;
            }
            return Game::Direction::Up;
        }
    } // namespace

    SkyBlockRenderer::~SkyBlockRenderer() { Shutdown(); }

    bool SkyBlockRenderer::Initialize() {
        if (!g_renderBackend) return false;
        // Vulkan maps the names onto shaders/sky_block_vk.{vert,frag}.spv;
        // the default pipeline layout's push constants carry uMVP and the
        // clip plane, which is all the pass needs.
        m_shader = g_renderBackend->CreateShaderFromFiles(
            PlatformMain::GetAssetPath("shaders/sky_block.vert"),
            PlatformMain::GetAssetPath("shaders/sky_block.frag"));
        if (m_shader == INVALID_SHADER && !g_renderBackend->UsesVkShaders()) {
            m_shader = g_renderBackend->CreateShader(kVertSource, kFragSource);
        }
        if (m_shader == INVALID_SHADER) {
            Log::Warning("[SkyBlockRenderer] failed to create shader - sky blocks will not open onto the sky");
            return false;
        }
        {
            const unsigned char white[4] = {255, 255, 255, 255};
            m_dummyTexture = g_renderBackend->CreateTexture2D(1, 1, TextureFormat::RGBA8, white);
            if (m_dummyTexture == INVALID_TEXTURE) {
                Log::Warning("[SkyBlockRenderer] failed to create its texture - disabled");
                Shutdown();
                return false;
            }
        }
        // 36 verts per fully exposed block; grown on demand.
        for (int slot = 0; slot < EntityFrame::Slots(); ++slot) {
            FrameBuffers& fb = m_frames[slot];
            fb.capacityVerts = 36 * 64;
            fb.usedVerts = 0;
            fb.vb = g_renderBackend->CreateBuffer(BufferUsage::Vertex, fb.capacityVerts * sizeof(Vert),
                                                  nullptr, BufferAccess::Streaming);
            fb.mesh = g_renderBackend->CreateMesh(fb.vb, INVALID_BUFFER, GetBlockVertexLayout());
            if (fb.vb == INVALID_BUFFER || fb.mesh == INVALID_MESH) {
                Log::Warning("[SkyBlockRenderer] failed to create its vertex buffers - disabled");
                Shutdown();
                return false;
            }
        }
        m_initialized = true;
        Log::Info("[SkyBlockRenderer] initialized");
        return true;
    }

    void SkyBlockRenderer::Shutdown() {
        if (!g_renderBackend) return;
        for (FrameBuffers& fb : m_frames) {
            if (fb.mesh != INVALID_MESH)   { g_renderBackend->DestroyMesh(fb.mesh); fb.mesh = INVALID_MESH; }
            if (fb.vb   != INVALID_BUFFER) { g_renderBackend->DestroyBuffer(fb.vb); fb.vb = INVALID_BUFFER; }
            fb.capacityVerts = 0;
            fb.usedVerts = 0;
        }
        if (m_shader != INVALID_SHADER) { g_renderBackend->DestroyShader(m_shader); m_shader = INVALID_SHADER; }
        if (m_dummyTexture != INVALID_TEXTURE) { g_renderBackend->DestroyTexture(m_dummyTexture); m_dummyTexture = INVALID_TEXTURE; }
        m_verts.clear();
        m_verts.shrink_to_fit();
        m_initialized = false;
    }

    bool SkyBlockRenderer::RenderWindows(const ChunkRenderer& sections, const glm::mat4& mvp,
                                         const glm::vec4& portalClipPlane) {
        if (!m_initialized || !g_renderBackend) return false;
        // The level this view draws (rebound for a portal's far side).
        const Client::ClientChunkManager* chunkMgr = Client::g_clientChunkManager;
        if (!chunkMgr) return false;
        const auto& skyChunks = chunkMgr->SkyBlockChunks();
        const auto& skyFaces  = chunkMgr->SkyFaces();
        if (skyChunks.empty() && skyFaces.empty()) return false;   // the common case: nothing at all

        PROFILE_ZONE_N("SkyBlockWindows");
        m_verts.clear();

        // One face of the box [from, to] (block units, render space), wound
        // counter-clockwise seen from outside — kCorners scaled onto it.
        auto pushFace = [&](const glm::vec3& from, const glm::vec3& to, int face) {
            static const int kOrder[6] = {0, 1, 2, 0, 2, 3};
            for (int i : kOrder) {
                const glm::vec3 v = from + kCorners[face][i] * (to - from);
                m_verts.push_back({v.x, v.y, v.z, 0.0f, 0.0f, 255, 255, 255, 255});
            }
        };

        for (const Game::Math::ChunkPos& pos : skyChunks) {
            const Client::ClientChunk* chunk = chunkMgr->GetChunk(pos);
            if (!chunk || !chunk->chunkData) continue;
            const int chunkMinX = pos.x * Game::Math::CHUNK_SIZE_X;
            const int chunkMinZ = pos.z * Game::Math::CHUNK_SIZE_Z;

            // The neighbour's block: read straight from this chunk's sections
            // when it lies inside the chunk (the usual case), through the
            // manager across a chunk border. Outside the world: air.
            auto blockAt = [&](const glm::ivec3& p) -> std::pair<Game::BlockID, Game::BlockStateIndex> {
                if (p.y < Config::MinY || p.y >= kWorldTop) return {Game::BlockID::Air, 0};
                const int lx = p.x - chunkMinX;
                const int lz = p.z - chunkMinZ;
                if (lx >= 0 && lx < Game::Math::CHUNK_SIZE_X && lz >= 0 && lz < Game::Math::CHUNK_SIZE_Z) {
                    const Game::ChunkSection* section = chunk->chunkData->GetSection((p.y - Config::MinY) >> 4);
                    if (!section) return {Game::BlockID::Air, 0};
                    const int ly = (p.y - Config::MinY) & 15;
                    return {section->GetBlockID(lx, ly, lz), section->GetState(lx, ly, lz)};
                }
                return chunkMgr->GetBlockAndStateAt(p);
            };

            for (const glm::ivec3& block : chunk->skyBlocks) {
                // Only the sections this view draws — MC's visible-section
                // walk, and the portal views' own lists.
                if (!sections.IsSectionVisible(pos, (block.y - Config::MinY) >> 4)) continue;
                // Render space (camera-relative, RenderOrigin.hpp): integer
                // cell minus integer origin, exact.
                const glm::vec3 base = Render::ToRender(glm::dvec3(block));
                for (int f = 0; f < kFaceCount; ++f) {
                    const auto [nid, nstate] = blockAt(block + kNeighbour[f]);
                    if (HidesFace(nid, nstate)) continue;
                    pushFace(base, base + glm::vec3(1.0f), f);
                }
            }
        }

        // Doors' sky faces (SkyFacesBlockEntity): the faces of the door's
        // model the mask names, which the mesher left out. The model's face
        // keys are world directions (pre-rotated by the blockstate); the
        // mask is in the door's frame. A face that declares a cullface is
        // dropped where the mesher would cull it.
        for (const auto& [pos, mask] : skyFaces) {
            const Game::Math::ChunkPos chunkPos{pos.x >> 4, pos.z >> 4};
            if (!sections.IsSectionVisible(chunkPos, (pos.y - Config::MinY) >> 4)) continue;
            const auto [id, stateIndex] = chunkMgr->GetBlockAndStateAt(pos);
            if (!Game::SkyFaces::AppliesTo(id)) continue;   // a predicted break, not yet confirmed
            const Game::BlockState state = Game::BlockStates::FromIndex(id, stateIndex);
            const Game::BlockModel& model = Game::BlockRegistry::GetBlockModel(state);
            const glm::vec3 base = Render::ToRender(glm::dvec3(pos));
            for (const auto& element : model.elements) {
                if (!element.rotation.IsIdentity()) continue;   // not an axis-aligned box
                const glm::vec3 from = base + element.from / 16.0f;
                const glm::vec3 to   = base + element.to / 16.0f;
                for (const auto& [faceDir, faceDef] : element.faces) {
                    const Game::Direction dir = FaceDirToDirection(faceDir);
                    if (!(mask & Game::SkyFaces::Bit(Game::SkyFaces::ToDoorFrame(state, dir)))) continue;
                    if (faceDef.cullfaceDir >= 0) {
                        const Game::Direction cull =
                            FaceDirToDirection(static_cast<Game::FaceDir>(faceDef.cullfaceDir));
                        const auto [nid, nstate] =
                            chunkMgr->GetBlockAndStateAt(pos + kNeighbour[static_cast<int>(cull)]);
                        if (HidesFace(nid, nstate)) continue;
                    }
                    pushFace(from, to, static_cast<int>(dir));
                }
            }
        }
        if (m_verts.empty()) return false;

        // A new frame restarts the set's cursor; a later view of the same
        // frame appends after what the earlier views drew (see the header).
        if (m_frameCursor.Advance()) m_frames[m_frameCursor.slot].usedVerts = 0;
        FrameBuffers& fb = m_frames[m_frameCursor.slot];
        if (fb.usedVerts + m_verts.size() > fb.capacityVerts) {
            size_t newCap = std::max<size_t>(fb.capacityVerts, 36);
            while (newCap < m_verts.size() * 2) newCap *= 2;
            // Deferred: this frame's earlier views (and the previous frame
            // on Vulkan) may still draw from the old buffer.
            g_renderBackend->DeferredDestroyMesh(fb.mesh);
            g_renderBackend->DeferredDestroyBuffer(fb.vb);
            fb.vb = g_renderBackend->CreateBuffer(BufferUsage::Vertex, newCap * sizeof(Vert),
                                                  nullptr, BufferAccess::Streaming);
            fb.mesh = g_renderBackend->CreateMesh(fb.vb, INVALID_BUFFER, GetBlockVertexLayout());
            fb.capacityVerts = newCap;
            fb.usedVerts = 0;
            if (fb.vb == INVALID_BUFFER || fb.mesh == INVALID_MESH) {
                m_initialized = false;   // stop rather than draw garbage
                Log::Warning("[SkyBlockRenderer] vertex buffer growth failed - disabled");
                return false;
            }
        }
        const size_t first = fb.usedVerts;
        g_renderBackend->UpdateBuffer(fb.vb, first * sizeof(Vert), m_verts.size() * sizeof(Vert), m_verts.data());
        fb.usedVerts += m_verts.size();

        // Depth only: the colour buffer keeps the sky this view just drew.
        // Depth tested and written, back faces culled; the stencil of a
        // portal view's mask comes from the backend's override, as for the
        // terrain this precedes.
        PipelineState state;
        state.depthTestEnabled  = true;
        state.depthWriteEnabled = true;
        state.depthCompareOp    = CompareOp::LessEqual;
        state.blendEnabled      = false;
        state.cullMode          = CullMode::Back;
        state.frontFace         = FrontFace::CounterClockwise;
        state.primitiveType     = PrimitiveType::Triangles;
        state.colorWriteEnabled = false;
        g_renderBackend->SetPipelineState(state);

        g_renderBackend->BindShader(m_shader);
        g_renderBackend->BindTexture(m_dummyTexture, 0);
        g_renderBackend->SetUniformMat4(m_shader, "uMVP", mvp);
        g_renderBackend->SetUniformVec4(m_shader, "uPortalClipPlane", portalClipPlane);
        g_renderBackend->DrawArrays(fb.mesh, static_cast<uint32_t>(m_verts.size()), static_cast<uint32_t>(first));
        g_renderBackend->UnbindMesh();

        // Back to the default pipeline (colour writes on), as every
        // standalone pass leaves it.
        PipelineState defaultState;
        defaultState.depthTestEnabled  = true;
        defaultState.depthWriteEnabled = true;
        defaultState.blendEnabled      = false;
        defaultState.cullMode          = CullMode::Back;
        g_renderBackend->SetPipelineState(defaultState);
        return true;
    }

} // namespace Render
