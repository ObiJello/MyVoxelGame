// File: src/client/renderer/entity/MobRenderer.cpp
#include "client/renderer/entity/MobRenderer.hpp"
#include "client/renderer/backend/RenderBackend.hpp"
#include "client/entity/ClientMobManager.hpp"
#include "client/renderer/viewmodel/HeldItemSpriteMesh.hpp"
#include "common/entity/mobs/Monsters.hpp"
#include "common/entity/mobs/Animals.hpp"
#include "common/entity/mobs/AnimatedMobs.hpp"
#include "common/entity/GeneratedMobDefs.hpp"
#include "client/renderer/entity/model/GeneratedEntityModels.hpp"
#include "common/core/Mth.hpp"
#include "common/core/Log.hpp"
#include "common/core/Profiling_Tracy.hpp"

// Declaration only — STB_IMAGE_IMPLEMENTATION is defined in exactly one TU
// elsewhere in the project, the same way ChestRenderer includes it.
#include "stb_image.h"

#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>
#include <filesystem>

namespace PlatformMain { std::string GetAssetPath(const std::string& relativePath); }

namespace Render {

    namespace {

        // MC EntityModel.MODEL_Y_OFFSET. In BLOCKS, applied after the 1/16
        // scale. See the header for why it is 1.501 and not 1.5.
        constexpr float kModelYOffset = -1.501f;

        // Texture paths. Note the 1.21 naming: cow/pig/chicken gained biome
        // variants and the plain `cow.png` no longer exists — the temperate
        // variant is the one that matches vanilla's default appearance.
        std::string_view TexturePathFor(Game::EntityTypeId type) {
            switch (type) {
                case Game::EntityTypeId::Zombie:   return "assets/textures/entity/zombie/zombie.png";
                case Game::EntityTypeId::Skeleton: return "assets/textures/entity/skeleton/skeleton.png";
                case Game::EntityTypeId::Creeper:  return "assets/textures/entity/creeper/creeper.png";
                case Game::EntityTypeId::Spider:   return "assets/textures/entity/spider/spider.png";
                case Game::EntityTypeId::Cow:      return "assets/textures/entity/cow/temperate_cow.png";
                case Game::EntityTypeId::Pig:      return "assets/textures/entity/pig/temperate_pig.png";
                case Game::EntityTypeId::Sheep:    return "assets/textures/entity/sheep/sheep.png";
                case Game::EntityTypeId::Chicken:  return "assets/textures/entity/chicken/temperate_chicken.png";
                default: break;
            }
            // Read off the mob's own def, which took it from the renderer class
            // in the decompile (or from the pinned default variant when that
            // renderer picks its texture from a map).
            const Game::MobDef* def = Game::FindMobDef(type);
            return def ? def->texture : std::string_view{};
        }

        std::unique_ptr<EntityModel> CreateModelFor(Game::EntityTypeId type) {
            switch (type) {
                case Game::EntityTypeId::Zombie:   return std::make_unique<ZombieModel>();
                case Game::EntityTypeId::Skeleton: return std::make_unique<SkeletonModel>();
                case Game::EntityTypeId::Creeper:  return std::make_unique<CreeperModel>();
                case Game::EntityTypeId::Spider:   return std::make_unique<SpiderModel>();
                case Game::EntityTypeId::Cow:      return std::make_unique<CowModel>();
                case Game::EntityTypeId::Pig:      return std::make_unique<PigModel>();
                case Game::EntityTypeId::Sheep:    return std::make_unique<SheepModel>(false);
                case Game::EntityTypeId::Chicken:  return std::make_unique<ChickenModel>();
                default: break;
            }
            // Everything else uses the mesh generated from MC's own
            // createBodyLayer. The eight above keep hand-written classes only
            // because each has a real setupAnim worth porting exactly.
            const std::string_view slug = Game::GetEntityTypeInfo(type).slug;
            if (FindGenModel(slug)) return std::make_unique<GeneratedModel>(slug);
            return nullptr;
        }

        // MC ColorLerper.Type.SHEEP — the wool tint per dye colour.
        //
        // NOT DyeColor's raw textureDiffuseColor: SHEEP is registered with a
        // brightness of 0.75, so every channel is floored to three quarters
        // (ColorLerper.getModifiedColor). White is the one special case and is
        // hardcoded to 0xE6E6E6 rather than scaled.
        //
        // These MULTIPLY the wool texture, exactly as MC's
        // renderColoredCutoutModel passes them as the model's vertex colour.
        // Using them as a flat replacement colour is what makes a sheep one
        // solid bright blob with no wool shading at all.
        struct SheepWoolColor { uint8_t r, g, b; };
        constexpr SheepWoolColor kSheepWoolColors[16] = {
            { 230, 230, 230 },  // white
            { 186,  96,  21 },  // orange
            { 149,  58, 141 },  // magenta
            {  43, 134, 163 },  // light_blue
            { 190, 162,  45 },  // yellow
            {  96, 149,  23 },  // lime
            { 182, 104, 127 },  // pink
            {  53,  59,  61 },  // gray
            { 117, 117, 113 },  // light_gray
            {  16, 117, 117 },  // cyan
            { 102,  37, 138 },  // purple
            {  45,  51, 127 },  // blue
            {  98,  63,  37 },  // brown
            {  70,  93,  16 },  // green
            { 132,  34,  28 },  // red
            {  21,  21,  24 },  // black
        };

    } // namespace

    MobRenderer::~MobRenderer() { Shutdown(); }

    bool MobRenderer::Initialize() {
        if (!g_renderBackend) return false;

        // CreateShaderFromFiles rewrites the path to shaders/entity_vk.*.spv on
        // Vulkan. NOT CreateShader(GLSL source) — that returns INVALID_SHADER on
        // Vulkan, which is exactly why the block-entity renderers silently do
        // not draw under --vulkan today.
        m_shader = g_renderBackend->CreateShaderFromFiles("shaders/entity.vert",
                                                          "shaders/entity.frag");
        if (m_shader == INVALID_SHADER) {
            Log::Warning("[MobRenderer] failed to load entity shader — mobs will not render");
            return false;
        }

        m_vertexBuffer = g_renderBackend->CreateBuffer(
            BufferUsage::Vertex, kMaxVertices * sizeof(ModelVertex),
            nullptr, BufferAccess::Streaming);
        m_indexBuffer = g_renderBackend->CreateBuffer(
            BufferUsage::Index, kMaxIndices * sizeof(uint32_t),
            nullptr, BufferAccess::Streaming);
        m_mesh = g_renderBackend->CreateMesh(m_vertexBuffer, m_indexBuffer,
                                             GetBlockVertexLayout());

        m_verts.reserve(65536);
        m_indices.reserve(98304);

        m_initialized = true;
        Log::Info("[MobRenderer] initialized");
        return true;
    }

    void MobRenderer::Shutdown() {
        if (!g_renderBackend) return;

        if (m_mesh != INVALID_MESH)          { g_renderBackend->DestroyMesh(m_mesh); m_mesh = INVALID_MESH; }
        if (m_vertexBuffer != INVALID_BUFFER){ g_renderBackend->DestroyBuffer(m_vertexBuffer); m_vertexBuffer = INVALID_BUFFER; }
        if (m_indexBuffer != INVALID_BUFFER) { g_renderBackend->DestroyBuffer(m_indexBuffer); m_indexBuffer = INVALID_BUFFER; }
        if (m_shader != INVALID_SHADER)      { g_renderBackend->DestroyShader(m_shader); m_shader = INVALID_SHADER; }

        for (auto& [path, tex] : m_textureCache) {
            if (tex != INVALID_TEXTURE) g_renderBackend->DestroyTexture(tex);
        }
        m_textureCache.clear();
        m_models.clear();

        // The bow texture lived in m_textureCache and has already been
        // destroyed above; only the CPU geometry and the state flag are ours.
        m_bowState = AssetState::Unloaded;
        m_bowTexture = INVALID_TEXTURE;
        m_bowVerts.clear();
        m_bowIndices.clear();

        m_initialized = false;
    }

    TextureHandle MobRenderer::LoadTexture(const std::string& relativePath) {
        const auto it = m_textureCache.find(relativePath);
        if (it != m_textureCache.end()) return it->second;

        // Same load as ChestRenderer::LoadVariantTexture — nearest filtering
        // and clamped wrap, because entity sheets are pixel art whose edges
        // must not bleed into the neighbouring part's texels.
        const std::string full = PlatformMain::GetAssetPath(relativePath);
        if (!std::filesystem::exists(full)) {
            Log::Warning("[MobRenderer] missing texture %s", relativePath.c_str());
            m_textureCache[relativePath] = INVALID_TEXTURE;
            return INVALID_TEXTURE;
        }

        int w = 0, h = 0, ch = 0;
        stbi_set_flip_vertically_on_load(0);
        unsigned char* pixels = stbi_load(full.c_str(), &w, &h, &ch, STBI_rgb_alpha);
        if (!pixels) {
            Log::Warning("[MobRenderer] failed to decode %s", relativePath.c_str());
            m_textureCache[relativePath] = INVALID_TEXTURE;
            return INVALID_TEXTURE;
        }

        TextureHandle tex = g_renderBackend->CreateTexture2D(w, h, TextureFormat::RGBA8, pixels);
        stbi_image_free(pixels);

        g_renderBackend->SetTextureFilter(tex, TextureFilter::Nearest, TextureFilter::Nearest);
        g_renderBackend->SetTextureWrap(tex, TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);

        m_textureCache[relativePath] = tex;
        return tex;
    }

    MobRenderer::ModelEntry* MobRenderer::GetModelFor(Game::EntityTypeId type) {
        const auto key = static_cast<uint16_t>(type);
        const auto it = m_models.find(key);
        if (it != m_models.end()) return &it->second;

        std::unique_ptr<EntityModel> model = CreateModelFor(type);
        if (!model) return nullptr;

        const std::string_view texturePath = TexturePathFor(type);
        if (texturePath.empty()) return nullptr;

        ModelEntry entry;
        entry.model = std::move(model);
        entry.texture = LoadTexture(std::string(texturePath));
        if (type == Game::EntityTypeId::Sheep) {
            entry.overlayModel = std::make_unique<SheepModel>(true);
        }

        return &m_models.emplace(key, std::move(entry)).first->second;
    }

    glm::mat4 MobRenderer::EntityMatrix(const glm::dvec3& renderPos,
                                        const glm::vec3& cameraPos,
                                        float bodyRot, float ageScale,
                                        float deathFlipDeg,
                                        const glm::vec3& modelScale) {
        // The MC transform chain — see the header. Camera-relative translation
        // keeps float precision usable far from the origin, which matters
        // because these vertices are baked in world space rather than being
        // transformed by a per-object matrix in the shader.
        const glm::vec3 relative(renderPos.x - cameraPos.x,
                                 renderPos.y - cameraPos.y,
                                 renderPos.z - cameraPos.z);

        glm::mat4 m = glm::translate(glm::mat4(1.0f), relative);
        m = glm::rotate(m, glm::radians(180.0f - bodyRot), glm::vec3(0.0f, 1.0f, 0.0f));
        // The topple. MC setupRotations applies it here — AFTER the body yaw
        // and BEFORE the Y flip — so a dying mob falls sideways relative to the
        // way it was facing, and the fall direction is stable while the body
        // continues to be animated underneath it.
        if (deathFlipDeg != 0.0f) {
            m = glm::rotate(m, glm::radians(deathFlipDeg), glm::vec3(0.0f, 0.0f, 1.0f));
        }
        m = glm::scale(m, glm::vec3(-ageScale, -ageScale, ageScale));
        // MC LivingEntityRenderer.submit calls the renderer's scale() hook
        // HERE — after the -1,-1,1 flip and before the model Y offset, so the
        // offset below is scaled along with the body. Identity for everything
        // except the swelling creeper.
        if (modelScale != glm::vec3(1.0f)) m = glm::scale(m, modelScale);
        m = glm::scale(m, glm::vec3(1.0f / 16.0f));

        // The Y offset is in blocks, so it is applied in the flipped, scaled
        // space: MC's translate(0, -1.501, 0) sits after scale(-1,-1,1), where
        // -Y is up. Pre-multiplying it in block units here is the same thing.
        m = glm::translate(m, glm::vec3(0.0f, kModelYOffset * 16.0f, 0.0f));
        return m;
    }

    glm::mat4 MobRenderer::AppendMob(EntityModel& model, const EntityRenderState& state,
                                     const glm::dvec3& renderPos, float bodyRot,
                                     const glm::vec3& cameraPos,
                                     std::vector<ModelVertex>& verts,
                                     std::vector<uint32_t>& idx) {
        model.SetupAnim(state);

        const glm::mat4 m = EntityMatrix(renderPos, cameraPos, bodyRot, state.ageScale,
                                         state.deathFlipDeg, state.modelScale);
        model.Root().Build(m, model.TexWidth(), model.TexHeight(), verts, idx);
        return m;
    }

    bool MobRenderer::EnsureBowGeometry() {
        if (m_bowState != AssetState::Unloaded) return m_bowState == AssetState::Ready;
        m_bowState = AssetState::Failed;   // pessimistic: every early return is a failure

        std::vector<HeldItemSpriteMesh::Vertex> sprite;
        std::vector<uint32_t> idx;
        if (!HeldItemSpriteMesh::BuildGeometry("bow", 0, sprite, idx)) return false;

        m_bowTexture = LoadTexture("assets/textures/item/bow.png");
        if (m_bowTexture == INVALID_TEXTURE) return false;

        // Straight copy: BuildGeometry now bakes MC's item diffuse lighting
        // (light.glsl minecraft_mix_light, DiffuseLighting.hpp) into the
        // vertex colour, so the bow arrives already shaded. This used to
        // re-derive a shade per quad from its own normal and overwrite the
        // colour with the BLOCK table — which lit an item by the block rules
        // and, once the sprite mesh started shading itself, would have thrown
        // that work away.
        m_bowVerts.clear();
        m_bowVerts.reserve(sprite.size());
        for (const auto& v : sprite) {
            m_bowVerts.push_back({ v.x, v.y, v.z, v.u, v.v, v.r, v.g, v.b, v.a });
        }
        m_bowIndices = std::move(idx);

        m_bowState = AssetState::Ready;
        return true;
    }

    void MobRenderer::AppendHeldBow(const EntityModel& model, const glm::mat4& entityMatrix,
                                    std::vector<ModelVertex>& verts,
                                    std::vector<uint32_t>& idx) {
        glm::mat4 hand;
        if (!model.RightHandMatrix(hand)) return;

        // ── MC ItemInHandLayer.submitArmWithItem ───────────────────────────
        // MC's pose stack is in BLOCKS here; this one is in model PIXELS, so
        // its translate(1/16, 0.125, -0.625) is written as (1, 2, -10).
        glm::mat4 m = entityMatrix * hand;
        m = glm::rotate(m, glm::radians(-90.0f), glm::vec3(1.0f, 0.0f, 0.0f));
        m = glm::rotate(m, glm::radians(180.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        m = glm::translate(m, glm::vec3(1.0f, 2.0f, -10.0f));

        // Everything below is MC's item-model space, which is in blocks.
        m = glm::scale(m, glm::vec3(16.0f));

        // ── assets/models/item/bow.json, display.thirdperson_righthand ─────
        // Order is MC ItemTransform.apply's: translate, then rotationXYZ,
        // then scale. The JSON translation is in sixteenths of a block, which
        // MC folds in at parse time.
        m = glm::translate(m, glm::vec3(-1.0f, -2.0f, 2.5f) * 0.0625f);
        m = glm::rotate(m, glm::radians(-80.0f), glm::vec3(1.0f, 0.0f, 0.0f));
        m = glm::rotate(m, glm::radians(260.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        m = glm::rotate(m, glm::radians(-40.0f), glm::vec3(0.0f, 0.0f, 1.0f));
        m = glm::scale(m, glm::vec3(0.9f));

        // MC renders the item model's [0,1] cell after a translate(-0.5) that
        // centres it on the hand. The extruded sprite is authored 0..16 and is
        // already centred on Z, so only X and Y need the half-cell shift.
        m = glm::translate(m, glm::vec3(-0.5f, -0.5f, 0.0f));
        m = glm::scale(m, glm::vec3(1.0f / 16.0f));

        const auto base = static_cast<uint32_t>(verts.size());
        for (const ModelVertex& v : m_bowVerts) {
            const glm::vec3 p = glm::vec3(m * glm::vec4(v.x, v.y, v.z, 1.0f));
            verts.push_back({ p.x, p.y, p.z, v.u, v.v, v.r, v.g, v.b, v.a });
        }
        for (uint32_t i : m_bowIndices) idx.push_back(base + i);
    }

    void MobRenderer::Render(const glm::mat4& projection, const glm::mat4& view,
                             const glm::vec3& cameraPos,
                             const Client::ClientMobManager& mobs,
                             float partialTick) {
        PROFILE_ZONE_N("MobRender");

        if (!m_initialized || !g_renderBackend) return;
        if (mobs.All().empty()) return;

        const float maxDistSq = kMaxRenderDistance * kMaxRenderDistance;

        // Group by texture so the whole scene collapses into a handful of draws.
        struct Batch {
            TextureHandle texture = INVALID_TEXTURE;
            glm::vec4     overlay{0.0f};
            size_t        firstIndex = 0;
            size_t        indexCount = 0;
        };
        std::vector<Batch> batches;

        m_verts.clear();
        m_indices.clear();

        for (const auto& [id, entry] : mobs.All()) {
            const Game::Mob& mob = *entry.mob;

            // Sub-tick interpolation, the same scheme PlayerRenderer uses.
            const glm::dvec3 renderPos = glm::mix(entry.renderPrevPosition, mob.position,
                                                  static_cast<double>(partialTick));

            const glm::vec3 delta(renderPos.x - cameraPos.x,
                                  renderPos.y - cameraPos.y,
                                  renderPos.z - cameraPos.z);
            if (glm::dot(delta, delta) > maxDistSq) continue;

            ModelEntry* modelEntry = GetModelFor(mob.GetType());
            if (!modelEntry || modelEntry->texture == INVALID_TEXTURE) continue;

            const float bodyRot = Game::Mth::RotLerp(partialTick, entry.renderPrevYBodyRot,
                                                     mob.yBodyRot);
            const float headRot = Game::Mth::RotLerp(partialTick, entry.renderPrevYHeadRot,
                                                     mob.GetYHeadRot());

            EntityRenderState state;
            // MC passes the head yaw RELATIVE to the body, so the model only
            // has to rotate the head part by the difference. Passing the
            // absolute yaw makes every mob look permanently over its shoulder.
            state.yRot = Game::Mth::WrapDegrees(headRot - bodyRot);
            state.xRot = Game::Mth::Lerp(partialTick, entry.renderPrevXRot, mob.xRot);
            state.walkAnimationPos = mob.walkAnimation.PositionAt(partialTick);
            state.walkAnimationSpeed = mob.walkAnimation.SpeedAt(partialTick);
            state.ageInTicks = static_cast<float>(mob.tickCount) + partialTick;
            state.attackTime = mob.attackAnim;
            state.isAggressive = mob.IsAggressive();
            state.isBaby = mob.IsBaby();
            state.ageScale = state.isBaby ? Game::kBabyScale : 1.0f;
            state.isInWater = mob.IsInWater();
            // The topple. mob.deathTime is already synced (SetEntityDataS2C
            // carries it) and counts 1..20 over the second between the death
            // event and the removal packet — the animation and the body's life
            // are the same 20 ticks, which is why the corpse never pops.
            state.deathFlipDeg = DeathFlipDegrees(mob.deathTime, partialTick);

            // MC's AnimationState timers, as the model wants to read them.
            // Copied rather than pointed at: the render state is a plain value
            // built per frame, and the model must not reach back into an entity
            // the renderer does not own.
            //
            // Skipped entirely for the ~90% of mobs that have never started a
            // clip — HasAnimStates is false until the first Anim() call, so a
            // zombie pays nothing for the frog's croak.
            if (mob.HasAnimStates()) {
                for (int slot = 0; slot < Game::kMobAnimCount; ++slot) {
                    const Game::AnimationState& a =
                        mob.Anim(static_cast<Game::MobAnim>(slot));
                    if (!a.IsStarted()) continue;
                    state.animStarted |= (uint64_t(1) << slot);
                    state.animStartTick[slot] = static_cast<float>(a.StartTick());
                }
            }

            // MC AbstractSkeleton.populateDefaultEquipmentSlots always puts a
            // bow in the main hand, so this is a constant rather than a synched
            // field — there is no skeleton in vanilla that spawns without one.
            //
            // The pose is AbstractSkeletonRenderer.getArmPose: BOW_AND_ARROW
            // only while aggressive, otherwise EMPTY and the arm just swings
            // with the walk. That is the whole reason a wandering skeleton
            // carries its bow at its side and a hunting one raises it.
            const bool isSkeleton = (mob.GetType() == Game::EntityTypeId::Skeleton);
            if (isSkeleton) {
                state.isHoldingBow = true;
                state.rightArmPose = state.isAggressive ? ArmPose::BowAndArrow
                                                        : ArmPose::Empty;
            }

            // MC's per-mob extractRenderState, for the clip guards. Each is
            // one field on one MC RenderState subclass; here they are branches
            // on the entity class, the same way the sheep and chicken below
            // already are.
            if (const auto* bat = dynamic_cast<const Game::Bat*>(&mob)) {
                state.isResting = bat->IsResting();
            }
            if (const auto* armadillo = dynamic_cast<const Game::Armadillo*>(&mob)) {
                state.isHidingInShell = armadillo->ShouldHideInShell();
            }

            if (const auto* sheep = dynamic_cast<const Game::Sheep*>(&mob)) {
                state.headEatPositionScale = sheep->GetHeadEatPositionScale(partialTick);
                state.headEatAngleScale = sheep->GetHeadEatAngleScale(partialTick);
            }
            if (const auto* chicken = dynamic_cast<const Game::Chicken*>(&mob)) {
                state.flap = chicken->GetFlap(partialTick);
                state.flapSpeed = chicken->GetFlapSpeed(partialTick);
            }

            // ── Overlay: hurt flash, then the creeper's swell + whiteout ──
            glm::vec4 overlay(0.0f);
            if (mob.hurtTime > 0 || mob.deathTime > 0) {
                // MC OverlayTexture's red row is 0xB2FF0000 and the shader does
                // `mix(overlayColor.rgb, color.rgb, overlayColor.a)` — so the
                // red contributes 1 - 178/255 = 0.302, not the alpha itself.
                overlay = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f - 178.0f / 255.0f);
            }
            if (const auto* creeper = dynamic_cast<const Game::Creeper*>(&mob)) {
                const float swell = creeper->GetSwelling(partialTick);

                // ── MC CreeperRenderer.scale, verbatim ────────────────────
                //
                //   wobble = 1 + sin(swelling * 100) * swelling * 0.01
                //   g      = clamp(swelling, 0, 1)^4
                //   scale  = ((1 + g*0.4) * wobble, (1 + g*0.1) / wobble, ...)
                //
                // Two effects in one: the g^4 term inflates the creeper by up
                // to 40% across and 10% tall — held back until the fuse is
                // nearly out by the fourth power — while sin(swelling * 100)
                // vibrates it at a frequency that rises with the fuse. The
                // wobble divides the height as it multiplies the width, so the
                // body squashes and stretches rather than just pulsing. This
                // whole hook was missing: the creeper swelled in the data and
                // never changed shape on screen.
                {
                    float g = swell;
                    const float wobble =
                        1.0f + std::sin(g * 100.0f) * g * 0.01f;
                    g = std::clamp(g, 0.0f, 1.0f);
                    g *= g;
                    g *= g;
                    const float xz = (1.0f + g * 0.4f) * wobble;
                    const float y  = (1.0f + g * 0.1f) / wobble;
                    state.modelScale = glm::vec3(xz, y, xz);
                }

                // MC CreeperRenderer.getWhiteOverlayProgress — a hard STROBE,
                // not a pulse: white for three ticks, off for three, flipping
                // every tenth of the fuse. The old sin() read as a smooth
                // throb, which is the wrong signal entirely — the flash is
                // what tells you how long you have.
                const int   band = static_cast<int>(swell * 10.0f);
                const float progress = (band % 2 == 0)
                                     ? 0.0f
                                     : std::clamp(swell, 0.5f, 1.0f);
                if (progress > 0.0f) {
                    // MC OverlayTexture's white row: alpha = 1 - u/15 * 0.75
                    // with u = (int)(progress * 15), and the shader keeps that
                    // fraction of the base colour. Our overlay alpha is the
                    // complement (how much WHITE to mix in), so it is the
                    // 0.75 term directly — 75% white at a full-strength flash.
                    const float u = std::floor(progress * 15.0f);
                    overlay = glm::vec4(1.0f, 1.0f, 1.0f, (u / 15.0f) * 0.75f);
                }
            }

            const size_t firstIndex = m_indices.size();
            const glm::mat4 entityMatrix =
                AppendMob(*modelEntry->model, state, renderPos, bodyRot,
                          cameraPos, m_verts, m_indices);

            if (m_indices.size() > firstIndex) {
                batches.push_back({ modelEntry->texture, overlay, firstIndex,
                                    m_indices.size() - firstIndex });
            }

            // The bow is its own texture, so it is its own batch. MC draws the
            // held item with NO_OVERLAY — a skeleton flashing red does not take
            // its bow with it — so the overlay is deliberately left at zero.
            if (isSkeleton && EnsureBowGeometry()) {
                const size_t bowFirst = m_indices.size();
                AppendHeldBow(*modelEntry->model, entityMatrix, m_verts, m_indices);
                if (m_indices.size() > bowFirst) {
                    batches.push_back({ m_bowTexture, glm::vec4(0.0f), bowFirst,
                                        m_indices.size() - bowFirst });
                }
            }

            // Sheep wool — MC SheepWoolLayer, a second model over the same
            // skeleton drawn with renderColoredCutoutModel.
            if (modelEntry->overlayModel) {
                const auto* sheep = dynamic_cast<const Game::Sheep*>(&mob);
                if (sheep && !sheep->IsSheared()) {
                    const size_t woolFirst = m_indices.size();
                    const size_t woolVertFirst = m_verts.size();
                    AppendMob(*modelEntry->overlayModel, state, renderPos,
                              bodyRot, cameraPos, m_verts, m_indices);

                    if (m_indices.size() > woolFirst) {
                        const SheepWoolColor tint =
                            kSheepWoolColors[sheep->GetColor() & 0x0F];

                        // MC passes the dye as the model's VERTEX COLOUR, which
                        // the entity shader multiplies with the texture. Doing
                        // it on the vertices here (rather than through the
                        // overlay uniform) keeps two things right that a flat
                        // replacement destroyed: the wool sheet's own shading
                        // survives, and the per-face directional shade that
                        // ModelPart::Build already baked in survives with it —
                        // so a dyed sheep is still lit, not a solid blob.
                        for (size_t i = woolVertFirst; i < m_verts.size(); ++i) {
                            ModelVertex& v = m_verts[i];
                            v.r = static_cast<uint8_t>((v.r * tint.r) / 255);
                            v.g = static_cast<uint8_t>((v.g * tint.g) / 255);
                            v.b = static_cast<uint8_t>((v.b * tint.b) / 255);
                        }

                        // The SAME overlay as the body. MC hands every render
                        // layer the entity's overlayCoords, so a hurt sheep
                        // flashes red all over rather than only on the skin.
                        batches.push_back({ LoadTexture("assets/textures/entity/sheep/sheep_wool.png"),
                                            overlay, woolFirst,
                                            m_indices.size() - woolFirst });
                    }
                }
            }

            if (m_verts.size() > kMaxVertices - 4096) break;   // buffer guard
        }

        if (m_indices.empty()) return;

        g_renderBackend->UpdateBuffer(m_vertexBuffer, 0,
                                      m_verts.size() * sizeof(ModelVertex), m_verts.data());
        g_renderBackend->UpdateBuffer(m_indexBuffer, 0,
                                      m_indices.size() * sizeof(uint32_t), m_indices.data());

        PipelineState pipeline;
        pipeline.depthTestEnabled = true;
        pipeline.depthWriteEnabled = true;
        pipeline.blendEnabled = false;
        // NO back-face culling — MC EntityModel's default render type is
        // RenderTypes::entityCutoutNoCull, and not one of these eight models
        // overrides it (only bats, arrows, chests, shields and the like ask for
        // a culling type).
        //
        // It is not an optimisation MC left on the table. Entity models are NOT
        // closed: a skeleton's 2-pixel limbs leave wide gaps between the ribs
        // and the legs, and vanilla shows you the FAR side of the ribcage
        // through them. Culling back faces deletes exactly that geometry, so
        // the skeleton reads as a flat shell with its spine missing.
        pipeline.cullMode = CullMode::None;
        pipeline.frontFace = FrontFace::CounterClockwise;
        pipeline.primitiveType = PrimitiveType::Triangles;
        g_renderBackend->SetPipelineState(pipeline);

        g_renderBackend->BindShader(m_shader);

        // Vertices are camera-relative, so the view matrix must not translate
        // again — strip its translation by rebuilding it around the origin.
        glm::mat4 viewNoTranslate = view;
        viewNoTranslate[3] = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
        g_renderBackend->SetUniformMat4(m_shader, "uMVP", projection * viewNoTranslate);

        for (const Batch& batch : batches) {
            g_renderBackend->BindTexture(batch.texture, 0);
            g_renderBackend->SetUniformVec4(m_shader, "uColor", batch.overlay);
            // indexOffset is in INDICES, not bytes — see DrawIndexed's
            // signature. Passing a byte offset draws from the wrong place with
            // no error, which is the classic way to get one mob's geometry
            // wearing another's texture.
            g_renderBackend->DrawIndexed(m_mesh,
                                         static_cast<uint32_t>(batch.indexCount),
                                         static_cast<uint32_t>(batch.firstIndex));
        }

        g_renderBackend->UnbindMesh();
    }

} // namespace Render
