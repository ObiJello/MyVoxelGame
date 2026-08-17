// File: src/client/renderer/viewmodel/ItemMeshBuilder.cpp
//
// Moved verbatim out of HeldItemRenderer.cpp's anonymous namespace when
// ItemEntityRenderer needed the same geometry. Behaviour is unchanged; only the
// names of the two entry points and the vertex struct were made public.
#include "ItemMeshBuilder.hpp"

#include "../texture/AtlasBuilder.hpp"
#include "common/world/block/BlockModel.hpp"
#include "common/world/block/BlockRegistry.hpp"

#include <algorithm>
#include <cmath>

namespace Render {

    // Helpers below stay internal — only the two Build* entry points and the
    // vertex struct are part of the shared interface.
    namespace {

    // Append one cube face quad (4 verts + 6 indices) to the scratch
    // buffer. positions/UVs supplied in CCW order viewed from
    // outside the face so backface culling keeps the visible side.
    // Per-vertex tint colour multiplies the sampled texture in the
    // block fragment shader (`texColor * vertColor`); used for
    // grass-top biome colouring.
    void appendCubeFace(std::vector<ItemCubeVert>& verts,
                        std::vector<uint32_t>& idx,
                        const glm::vec3 p[4], const glm::vec2 uv[4],
                        uint8_t r, uint8_t g, uint8_t b) {
        uint32_t base = (uint32_t)verts.size();
        for (int i = 0; i < 4; ++i) {
            verts.push_back({ p[i].x, p[i].y, p[i].z,
                              uv[i].x, uv[i].y,
                              r, g, b, 255 });
        }
        idx.push_back(base + 0); idx.push_back(base + 1); idx.push_back(base + 2);
        idx.push_back(base + 0); idx.push_back(base + 2); idx.push_back(base + 3);
    }

    // Default biome-tint colour applied to faces with tintindex=0
    // (grass top, vine, lily pad, …). Held items have no biome
    // context so we use the vanilla "default" grass/foliage colour
    // — the value sampled from the centre of grass.png at the
    // default plains-biome coords. Without this, grass tops render
    // grey because the texture itself is greyscale.
    constexpr uint8_t kDefaultGrassR = 124;
    constexpr uint8_t kDefaultGrassG = 189;
    constexpr uint8_t kDefaultGrassB = 107;

    // Per-element resolved face texture + tint. We collect one of
    // these per (element × face direction) so multi-layer cube
    // models (grass_block has a base cube + an overlay cube)
    // emit one quad per layer at the same world position; alpha
    // blending composites them correctly.
    struct FaceLayer {
        float u0, v0, u1, v1;
        int   tintIndex;
    };

    // Walk EVERY element of the model that defines this face and
    // resolve each one to an atlas UV + tint. Returns the layers
    // in element order (base first, overlays next) so emission can
    // draw them in that sequence — the overlay's transparent
    // pixels blend over the base.
    void collectFaceLayers(const Game::BlockModel& bm, Game::FaceDir dir,
                           std::vector<FaceLayer>& out) {
        for (const auto& el : bm.elements) {
            auto faceIt = el.faces.find(dir);
            if (faceIt == el.faces.end()) continue;
            const std::string atlasKey = bm.ResolveTexture(faceIt->second.textureRef);
            if (atlasKey.empty() || atlasKey == "missingno") continue;
            AtlasUVRect rect;
            if (!g_atlasBuilder || !g_atlasBuilder->GetUVRect(atlasKey, rect)) continue;
            out.push_back({ rect.uvMin.x, rect.uvMin.y,
                            rect.uvMax.x, rect.uvMax.y,
                            faceIt->second.tintIndex });
        }
    }

    // Resolves a tintindex value to the per-vertex colour to write
    // for that face. -1 (no tint) = white; 0 (biome tint) = the
    // default grass/foliage colour above; higher indices are reserved
    // for other items (banner, dye) and aren't relevant for blocks.
    // Multiply a face's vertex colour by its directional-shade factor.
    //
    // Done in gamma space as a direct multiplier, matching both MC and this
    // engine's chunk mesher (Mesher.cpp: "All math is in gamma space — shade
    // values are direct multipliers"). Converting to linear here would make
    // items disagree with the terrain they sit on.
    void applyDirectionalShade(float shade, uint8_t& r, uint8_t& g, uint8_t& b) {
        if (shade >= 1.0f) return;
        const auto scale = [shade](uint8_t c) {
            return static_cast<uint8_t>(std::lround(static_cast<float>(c) * shade));
        };
        r = scale(r);
        g = scale(g);
        b = scale(b);
    }

    void tintToColour(int tintIndex, uint8_t& r, uint8_t& g, uint8_t& b) {
        if (tintIndex == 0) {
            r = kDefaultGrassR;
            g = kDefaultGrassG;
            b = kDefaultGrassB;
        } else {
            r = g = b = 255;
        }
    }

    // Fallback: try a couple of typical texture-key patterns for
    // the block's model name. Used when the BlockModelRegistry
    // doesn't have the model loaded (unloaded model, custom block,
    // etc.) — produces a single-texture cube. Returns true if any
    // pattern hits.
    bool fallbackBlockUV(const std::string& modelName,
                         float& u0, float& v0, float& u1, float& v1) {
        if (!g_atlasBuilder || modelName.empty()) return false;
        const std::string keys[] = {
            "block/" + modelName,
            modelName,
        };
        AtlasUVRect rect;
        for (const auto& k : keys) {
            if (g_atlasBuilder->GetUVRect(k, rect)) {
                u0 = rect.uvMin.x; v0 = rect.uvMin.y;
                u1 = rect.uvMax.x; v1 = rect.uvMax.y;
                return true;
            }
        }
        return false;
    }

    } // namespace


    // Build the 6-face cube mesh on the fly. Each face is UV'd
    // independently from the block model's per-face texture so
    // multi-textured blocks (grass, oak log, furnace, …) show the
    // right face on the right side. Caller owns the scratch
    // vertex/index buffers.
    //
    // Winding convention: vertices listed counter-clockwise when
    // viewed from the OUTSIDE of the face — matches our pipeline
    // state (CullMode::Back, FrontFace::CounterClockwise), so the
    // outward-facing side is what survives backface culling.
    //
    // Cube occupies [0, 1]³ in local space; the renderer recentres
    // around the cube's mid-point (0.5, 0.5, 0.5) before applying
    // the display transform so rotations pivot at the block centre.
    void BuildBlockCubeMesh(Game::BlockID b,
                            std::vector<ItemCubeVert>& verts,
                            std::vector<uint32_t>& idx) {
        verts.clear();
        idx.clear();

        const auto& blk  = Game::BlockRegistry::Get(b);
        const auto* bmPtr = &Game::BlockModelRegistry::GetModel(blk.modelName);
        const bool hasModel = bmPtr && !bmPtr->elements.empty();

        // 8 corners of the unit cube.
        const glm::vec3 v000{0,0,0}, v100{1,0,0}, v110{1,1,0}, v010{0,1,0};
        const glm::vec3 v001{0,0,1}, v101{1,0,1}, v111{1,1,1}, v011{0,1,1};

        // Per-face emit helper. For multi-layer cube models
        // (grass_block has a base cube + an overlay cube at the
        // same position), we walk EVERY element that defines this
        // face and emit one quad per layer. The block path uses
        // depth-test LessOrEqual so coplanar overlay quads aren't
        // z-rejected after the base writes depth; the alpha-blend
        // pipeline composites them in the order we emit (base
        // first, overlay second). Single-element models hit this
        // path with a layer count of 1 and behave exactly as before.
        auto emit = [&](Game::FaceDir dir,
                        const glm::vec3 q[4], const glm::vec2 uv[4]) {
            std::vector<FaceLayer> layers;
            if (hasModel) collectFaceLayers(*bmPtr, dir, layers);
            if (layers.empty()) {
                // Fallback when the model has no entry for this
                // face (or no model at all): try model-name keys,
                // failing that draw with the full atlas range so
                // the cube is at least visible.
                FaceLayer fb{0,0,1,1,-1};
                fallbackBlockUV(blk.modelName, fb.u0, fb.v0, fb.u1, fb.v1);
                layers.push_back(fb);
            }
            for (const FaceLayer& L : layers) {
                uint8_t r, g, b;
                tintToColour(L.tintIndex, r, g, b);
                // The cube fallback has no Element to read `shade` from, but it
                // only ever draws a full cube — and a full cube always shades
                // in MC, so unconditional is correct here.
                applyDirectionalShade(Game::DirectionalShade(dir), r, g, b);
                glm::vec2 rmap[4];
                for (int i = 0; i < 4; ++i) {
                    rmap[i].x = L.u0 + uv[i].x * (L.u1 - L.u0);
                    rmap[i].y = L.v0 + uv[i].y * (L.v1 - L.v0);
                }
                appendCubeFace(verts, idx, q, rmap, r, g, b);
            }
        };

        // Template UVs: corners in (s, t) ∈ [0,1] expressing where on
        // the resolved atlas rect each corner samples. The chosen
        // mapping puts the texture's top-left at the face's natural
        // top-left for each side (matches vanilla face UV defaults).

        // +Y top
        { glm::vec3 q[4] = {v010, v011, v111, v110};
          glm::vec2 uv[4] = {{0,0},{0,1},{1,1},{1,0}};
          emit(Game::FaceDir::Up, q, uv); }
        // -Y bottom
        { glm::vec3 q[4] = {v000, v100, v101, v001};
          glm::vec2 uv[4] = {{0,0},{1,0},{1,1},{0,1}};
          emit(Game::FaceDir::Down, q, uv); }
        // +Z front (south in MC convention)
        { glm::vec3 q[4] = {v001, v101, v111, v011};
          glm::vec2 uv[4] = {{0,1},{1,1},{1,0},{0,0}};
          emit(Game::FaceDir::South, q, uv); }
        // -Z back (north)
        { glm::vec3 q[4] = {v000, v010, v110, v100};
          glm::vec2 uv[4] = {{1,1},{1,0},{0,0},{0,1}};
          emit(Game::FaceDir::North, q, uv); }
        // +X right (east)
        { glm::vec3 q[4] = {v100, v110, v111, v101};
          glm::vec2 uv[4] = {{1,1},{1,0},{0,0},{0,1}};
          emit(Game::FaceDir::East, q, uv); }
        // -X left (west)
        { glm::vec3 q[4] = {v000, v001, v011, v010};
          glm::vec2 uv[4] = {{0,1},{1,1},{1,0},{0,0}};
          emit(Game::FaceDir::West, q, uv); }
    }


    // ── Real block-model geometry for the held item ──────────────
    //
    // BuildBlockCubeMesh above forces every block into a 1x1 cube and only
    // takes the TEXTURES from the model. That is right for a full cube and
    // wrong for everything else: a button came out as a plank cube, because
    // block/button's texture IS oak_planks and only its tiny from/to said
    // otherwise. Slabs, stairs, torches, fences, trapdoors and carpets were
    // all cubes for the same reason.
    //
    // This walks the model's ELEMENTS and emits each face at its real
    // extents, which is what the chunk mesher and the inventory icon both
    // already do. Model selection mirrors GuiGraphics::RenderBlockItemImpl
    // so the item in your hand and its inventory icon agree: prefer an
    // `_inventory` model (fences and mushroom blocks ship one whose
    // orientation differs from the world block), then any per-item model
    // override from assets/items/{slug}.json, then the block's own.
    static const Game::BlockModel* pickItemModel(Game::BlockID b,
                                          const std::string& modelOverride) {
        const auto& blk = Game::BlockRegistry::Get(b);
        const std::string& name = !modelOverride.empty() ? modelOverride : blk.modelName;
        if (name.empty()) return nullptr;
        if (Game::BlockModelRegistry::HasModel(name + "_inventory")) {
            return &Game::BlockModelRegistry::GetModel(name + "_inventory");
        }
        if (!modelOverride.empty() && Game::BlockModelRegistry::HasModel(name)) {
            return &Game::BlockModelRegistry::GetModel(name);
        }
        return &Game::BlockRegistry::GetBlockModel(b);
    }

    // Emit one element face. `uvT` is the same corner->(s,t) template the
    // cube path uses; the face's own `uv` rectangle (MC pixel space, and
    // already defaulted to the full 0..16 face) then selects the sub-region
    // of the atlas tile, so a model that samples a corner of its texture
    // gets that corner rather than the whole thing stretched.
    static void emitModelFace(std::vector<ItemCubeVert>& verts, std::vector<uint32_t>& idx,
                       const Game::BlockModel& bm, const Game::Element& el,
                       Game::FaceDir dir, const glm::vec3 q[4], const glm::vec2 uvT[4]) {
        auto it = el.faces.find(dir);
        if (it == el.faces.end()) return;
        const Game::FaceDef& face = it->second;

        const std::string atlasKey = bm.ResolveTexture(face.textureRef);
        if (atlasKey.empty() || atlasKey == "missingno") return;
        AtlasUVRect rect;
        if (!g_atlasBuilder || !g_atlasBuilder->GetUVRect(atlasKey, rect)) return;

        // Face uv is in [0,16] over the tile; fold it into the atlas rect.
        const float su0 = face.uv.x / 16.0f, sv0 = face.uv.y / 16.0f;
        const float su1 = face.uv.z / 16.0f, sv1 = face.uv.w / 16.0f;
        const float au0 = rect.uvMin.x + su0 * (rect.uvMax.x - rect.uvMin.x);
        const float av0 = rect.uvMin.y + sv0 * (rect.uvMax.y - rect.uvMin.y);
        const float au1 = rect.uvMin.x + su1 * (rect.uvMax.x - rect.uvMin.x);
        const float av1 = rect.uvMin.y + sv1 * (rect.uvMax.y - rect.uvMin.y);

        uint8_t r, g, b;
        tintToColour(face.tintIndex, r, g, b);
        // Same fake directional light the chunk mesher bakes in, honouring the
        // element's own `shade` opt-out. Without this an item's faces are all
        // equally bright and it reads as a flat sticker against the terrain
        // beside it, which shades top-vs-side.
        applyDirectionalShade(el.shade ? Game::DirectionalShade(dir) : 1.0f, r, g, b);
        glm::vec2 uv[4];
        for (int i = 0; i < 4; ++i) {
            uv[i].x = au0 + uvT[i].x * (au1 - au0);
            uv[i].y = av0 + uvT[i].y * (av1 - av0);
        }
        appendCubeFace(verts, idx, q, uv, r, g, b);
    }

    // Returns false when the model has no usable geometry, so the caller
    // can fall back to the textured cube (water/lava and the BEWLR blocks
    // have models with no elements at all).
    bool BuildBlockModelMesh(Game::BlockID b, const std::string& modelOverride,
                             std::vector<ItemCubeVert>& verts,
                             std::vector<uint32_t>& idx) {
        verts.clear();
        idx.clear();
        const Game::BlockModel* bm = pickItemModel(b, modelOverride);
        if (!bm || bm->elements.empty()) return false;

        for (const auto& el : bm->elements) {
            // Clamp rather than overrun the fixed streaming buffers. A
            // model this large does not exist in vanilla; dropping its tail
            // is still better than corrupting the upload.
            if (verts.size() + 24 > kItemCubeMaxVerts) break;
            const size_t elemVertStart = verts.size();

            // MC pixel space [0,16] -> block space [0,1].
            const glm::vec3 lo = el.from * (1.0f / 16.0f);
            const glm::vec3 hi = el.to   * (1.0f / 16.0f);

            const glm::vec3 v000{lo.x, lo.y, lo.z}, v100{hi.x, lo.y, lo.z};
            const glm::vec3 v110{hi.x, hi.y, lo.z}, v010{lo.x, hi.y, lo.z};
            const glm::vec3 v001{lo.x, lo.y, hi.z}, v101{hi.x, lo.y, hi.z};
            const glm::vec3 v111{hi.x, hi.y, hi.z}, v011{lo.x, hi.y, hi.z};

            // Same six corner/UV templates as the cube path.
            { const glm::vec3 q[4] = {v010, v011, v111, v110};
              const glm::vec2 t[4] = {{0,0},{0,1},{1,1},{1,0}};
              emitModelFace(verts, idx, *bm, el, Game::FaceDir::Up, q, t); }
            { const glm::vec3 q[4] = {v000, v100, v101, v001};
              const glm::vec2 t[4] = {{0,0},{1,0},{1,1},{0,1}};
              emitModelFace(verts, idx, *bm, el, Game::FaceDir::Down, q, t); }
            { const glm::vec3 q[4] = {v001, v101, v111, v011};
              const glm::vec2 t[4] = {{0,1},{1,1},{1,0},{0,0}};
              emitModelFace(verts, idx, *bm, el, Game::FaceDir::South, q, t); }
            { const glm::vec3 q[4] = {v000, v010, v110, v100};
              const glm::vec2 t[4] = {{1,1},{1,0},{0,0},{0,1}};
              emitModelFace(verts, idx, *bm, el, Game::FaceDir::North, q, t); }
            { const glm::vec3 q[4] = {v100, v110, v111, v101};
              const glm::vec2 t[4] = {{1,1},{1,0},{0,0},{0,1}};
              emitModelFace(verts, idx, *bm, el, Game::FaceDir::East, q, t); }
            { const glm::vec3 q[4] = {v000, v001, v011, v010};
              const glm::vec2 t[4] = {{0,1},{1,1},{1,0},{0,0}};
              emitModelFace(verts, idx, *bm, el, Game::FaceDir::West, q, t); }

            // MC FaceBakery.applyElementRotation — an arbitrary angle about
            // the element's own origin, so it cannot be folded into from/to
            // and has to move the vertices just emitted. Same treatment the
            // chunk mesher gives it; without this the crossed planes of a
            // torch or a flower pot sit axis-aligned instead of at 45.
            if (!el.rotation.IsIdentity()) {
                for (size_t i = elemVertStart; i < verts.size(); ++i) {
                    glm::vec3 p{verts[i].x, verts[i].y, verts[i].z};
                    p = Game::ApplyElementRotation(p, el.rotation, 1.0f / 16.0f);
                    verts[i].x = p.x; verts[i].y = p.y; verts[i].z = p.z;
                }
            }
        }
        return !verts.empty();
    }

} // namespace Render
