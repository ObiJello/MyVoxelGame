// File: src/client/renderer/entity/MobRenderer.cpp
#include "client/resource/ResourcePacks.hpp"
#include "client/renderer/entity/MobRenderer.hpp"
#include "client/renderer/entity/GeneratedBabyTextures.hpp"
#include "client/renderer/entity/model/GeneratedSetupAnim.hpp"
#include "client/renderer/entity/EntityCulling.hpp"
#include "client/renderer/backend/RenderBackend.hpp"
#include "client/entity/ClientMobManager.hpp"
#include "client/renderer/viewmodel/HeldItemSpriteMesh.hpp"
#include "common/entity/mobs/Monsters.hpp"
#include "common/entity/mobs/Slime.hpp"
#include "common/entity/mobs/SulfurCube.hpp"
#include "client/renderer/entity/BlockCubeEntityRenderer.hpp"
#include "client/renderer/texture/AtlasBuilder.hpp"
#include "common/entity/mobs/Animals.hpp"
#include "common/entity/mobs/Fish.hpp"
#include "common/entity/mobs/AnimatedMobs.hpp"
#include "common/entity/TamableAnimal.hpp"
#include "common/entity/EndCrystal.hpp"
#include "common/entity/projectile/HurtingProjectile.hpp"
#include "common/entity/projectile/EvokerFangs.hpp"
#include "common/entity/GeneratedMobDefs.hpp"
#include "client/renderer/entity/model/GeneratedEntityModels.hpp"
#include "common/core/JavaRandom.hpp"
#include "common/core/Mth.hpp"
#include "../mesh/ChunkRenderer.hpp"
#include "common/core/Log.hpp"
#include "common/core/Profiling_Tracy.hpp"

// Declaration only — STB_IMAGE_IMPLEMENTATION is defined in exactly one TU
// elsewhere in the project, the same way ChestRenderer includes it.
#include "stb_image.h"

#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <utility>
#include <cassert>
#include <cmath>
#include <filesystem>

namespace PlatformMain { std::string GetAssetPath(const std::string& relativePath); }

namespace Render {

    namespace {

        // MC EntityModel.MODEL_Y_OFFSET. In BLOCKS, applied after the 1/16
        // scale. See the header for why it is 1.501 and not 1.5.
        constexpr float kModelYOffset = -1.501f;

        // The per-species state extractors below used to find their mob with
        // a dynamic_cast each — forty-odd RTTI walks per mob per frame, most
        // of them failing, and the largest single item in the MobRender zone
        // at a hundred mobs. The entity's type id says which concrete class
        // the factory built (ClientMobManager::CreateMobOfType and
        // Game::MakeGenericMob, which MUST agree with the ids listed at each
        // call site here), so an integer compare replaces the RTTI walk and
        // the cast is static. The debug assert catches a factory change that
        // starts building a listed id from a different class — the one way
        // this can go wrong, and it would otherwise be silent UB.
        //
        // T must sit on Mob's single-inheritance chain (static_cast from
        // Mob*): the mixin bases (TamableAnimal, NeutralMob, RangedAttackMob)
        // are reached through the concrete class instead — see TamableOf.
        template <typename T, typename... Ids>
        const T* MobAs(const Game::Mob& mob, Game::EntityTypeId type, Ids... ids) {
            if (!((type == ids) || ...)) return nullptr;
            assert(dynamic_cast<const T*>(&mob) != nullptr &&
                   "MobAs: entity type built from a different class than listed");
            return static_cast<const T*>(&mob);
        }

        // MC TamableAnimal, reached via the concrete pets: it is a mixin base
        // here, not on Mob's chain.
        const Game::TamableAnimal* TamableOf(const Game::Mob& mob, Game::EntityTypeId type) {
            switch (type) {
                case Game::EntityTypeId::Wolf:   return MobAs<Game::Wolf>(mob, type, type);
                case Game::EntityTypeId::Cat:    return MobAs<Game::Cat>(mob, type, type);
                case Game::EntityTypeId::Parrot: return MobAs<Game::Parrot>(mob, type, type);
                default: return nullptr;
            }
        }

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
                case Game::EntityTypeId::Arrow:    return "assets/textures/entity/projectiles/arrow.png";
                // MC BeeRenderer's DEFAULT sheet. The generated def row pins
                // bee_angry.png (the decompile's map-default); the calm sheet
                // is the vanilla resting look, and the angry/nectar variants
                // swap per instance below.
                case Game::EntityTypeId::Bee:      return "assets/textures/entity/bee/bee.png";
                // Modelled projectiles (the sprite ones never reach here —
                // see SpriteNameForProjectile).
                case Game::EntityTypeId::Trident:      return "assets/textures/entity/trident.png";
                case Game::EntityTypeId::ShulkerBullet: return "assets/textures/entity/shulker/spark.png";
                case Game::EntityTypeId::LlamaSpit:    return "assets/textures/entity/llama/spit.png";
                case Game::EntityTypeId::WindCharge:
                case Game::EntityTypeId::BreezeWindCharge:
                    return "assets/textures/entity/projectiles/wind_charge.png";
                // The dangerous (blue) skull swaps to wither_invulnerable.png
                // per instance in Render.
                case Game::EntityTypeId::WitherSkull:  return "assets/textures/entity/wither/wither.png";
                // MC EnderDragonRenderer's DRAGON_LOCATION. The generated def
                // row pins the end-crystal beam sheet (a decompile artefact);
                // the eyes glow layer and the exploding decal are separate
                // render layers — skipped with the layered-model pipeline.
                case Game::EntityTypeId::EnderDragon:
                    return "assets/textures/entity/enderdragon/dragon.png";
                case Game::EntityTypeId::EndCrystal:
                    return "assets/textures/entity/end_crystal/end_crystal.png";
                case Game::EntityTypeId::EvokerFangs:  return "assets/textures/entity/illager/evoker_fangs.png";
                // The generated def row resolved this one to the zombie sheet
                // (the renderer class picks per-variant); the real texture.
                case Game::EntityTypeId::ZombieNautilus:
                    return "assets/textures/entity/nautilus/zombie_nautilus.png";
                default: break;
            }
            // Read off the mob's own def, which took it from the renderer class
            // in the decompile (or from the pinned default variant when that
            // renderer picks its texture from a map).
            const Game::MobDef* def = Game::FindMobDef(type);
            return def ? def->texture : std::string_view{};
        }

        BabyModelLook s_babyLook = BabyModelLook::New;
        int           s_babyLookGeneration = 0;

        bool NewBabies() { return s_babyLook == BabyModelLook::New; }

        // A remodel mesh with its own compiled setupAnim when the generator
        // produced one, else animated by `animSlug`'s program — the adult's,
        // whose part names the 26.x baby meshes keep (BabySquidModel's
        // tentacles, for one, where MC2's setupAnim did not compile).
        std::unique_ptr<EntityModel> MakeRemodel(const std::string& slug,
                                                 std::string_view animSlug) {
            if (FindAnimProgram(slug)) return std::make_unique<GeneratedModel>(slug);
            return std::make_unique<GeneratedModel>(slug, animSlug);
        }

        // Every cube of a model grown by a hair — the New-look baby sheep's
        // wool rides the SAME mesh as its body (MC's SHEEP_BABY_WOOL row is
        // the body row; SheepWoolLayer submits it with order 1 so it draws
        // after the skin). Batches here draw in push order too, but a
        // coplanar pair is one depth-precision wobble from flickering, so
        // the wool gets MC's intent as geometry: 0.02 px outward.
        void InflateCubes(ModelPart& part, float by) {
            for (CubeDefinition& c : part.cubes) {
                c.growX += by; c.growY += by; c.growZ += by;
            }
            for (auto& child : part.children) InflateCubes(*child, by);
        }

        // The farm animals' variant meshes (MC ModelLayers.WARM_COW, COLD_COW,
        // COLD_PIG, COLD_CHICKEN). Null when the variant uses the normal mesh.
        std::unique_ptr<EntityModel> CreateVariantModelFor(Game::EntityTypeId type, uint8_t variant) {
            switch (type) {
                case Game::EntityTypeId::Cow:
                    if (variant == 1) return std::make_unique<CowModel>(CowModel::Kind::Warm);
                    if (variant == 2) return std::make_unique<CowModel>(CowModel::Kind::Cold);
                    return nullptr;
                case Game::EntityTypeId::Pig:
                    return variant == 2 ? std::make_unique<PigModel>(true) : nullptr;
                case Game::EntityTypeId::Chicken:
                    return variant == 2 ? std::make_unique<ChickenModel>(true) : nullptr;
                default:
                    return nullptr;
            }
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
                case Game::EntityTypeId::Arrow:    return std::make_unique<ArrowModel>();
                case Game::EntityTypeId::Trident:  return std::make_unique<TridentModel>();
                case Game::EntityTypeId::ShulkerBullet:
                    return std::make_unique<ShulkerBulletModel>();
                case Game::EntityTypeId::LlamaSpit:
                    return std::make_unique<LlamaSpitModel>();
                case Game::EntityTypeId::WindCharge:
                case Game::EntityTypeId::BreezeWindCharge:
                    return std::make_unique<WindChargeModel>();
                case Game::EntityTypeId::WitherSkull:
                    return std::make_unique<WitherSkullModel>();
                case Game::EntityTypeId::EvokerFangs:
                    return std::make_unique<EvokerFangsModel>();
                // The dragon reuses its GENERATED mesh but replaces the
                // compiled setupAnim wholesale — see DragonModel.
                case Game::EntityTypeId::EnderDragon: return std::make_unique<DragonModel>();
                case Game::EntityTypeId::EndCrystal:
                    return std::make_unique<EndCrystalModel>();
                default: break;
            }
            // Everything else uses the mesh generated from MC's own
            // createBodyLayer. The eight above keep hand-written classes only
            // because each has a real setupAnim worth porting exactly.
            const std::string_view slug = Game::GetEntityTypeInfo(type).slug;
            // MC 26.1's AdultRabbitModel (the remodel's one ADULT change),
            // under the New look only.
            if (type == Game::EntityTypeId::Rabbit && NewBabies() &&
                FindGenModel("rabbit_new")) {
                return MakeRemodel("rabbit_new", "rabbit");
            }
            if (FindGenModel(slug)) return std::make_unique<GeneratedModel>(slug);
            return nullptr;
        }

        // MC AgeableMobRenderer's babyModel. Generated mobs get the
        // '<slug>_baby' mesh (LayerDefinitions' *_BABY rows, run through
        // BabyModelTransform by the generator); the hand-written models
        // rebuild the adult mesh and run their class's BABY_TRANSFORMER over
        // it via BecomeBaby. Null for a mob MC never draws through a baby
        // mesh — the caller keeps the adult model plus the uniform shrink.
        std::unique_ptr<EntityModel> CreateBabyModelFor(Game::EntityTypeId type) {
            const std::string_view slug = Game::GetEntityTypeInfo(type).slug;
            // MC 26.1's dedicated baby meshes (BabyCowModel and friends —
            // the generator's REMODEL_MESHES), New look only. Each row has
            // its own compiled setupAnim under the same slug.
            if (NewBabies()) {
                const std::string remodel = std::string(slug) + "_baby_new";
                if (FindGenModel(remodel)) return MakeRemodel(remodel, slug);
            }
            const std::string babySlug = std::string(slug) + "_baby";
            if (FindGenModel(babySlug)) {
                return std::make_unique<GeneratedModel>(babySlug);
            }
            std::unique_ptr<EntityModel> model = CreateModelFor(type);
            if (model && model->BecomeBaby()) return model;
            return nullptr;
        }

        // MC ThrownItemRenderer's clients — projectiles drawn as a
        // camera-facing item sprite instead of a mesh. Null for everything
        // else. The dragon fireball uses the fire-charge sprite: its own
        // texture is an animated strip MC billboards through a bespoke
        // renderer, and the fire charge is what its item form shows anyway.
        const char* SpriteNameForProjectile(Game::EntityTypeId type) {
            switch (type) {
                case Game::EntityTypeId::Snowball:      return "snowball";
                case Game::EntityTypeId::Egg:           return "egg";
                // The bottle sprite alone — MC layers potion_overlay tinted
                // by the potion colour, which waits on the potion-item system.
                case Game::EntityTypeId::SplashPotion:  return "splash_potion";
                case Game::EntityTypeId::SmallFireball:
                case Game::EntityTypeId::Fireball:
                case Game::EntityTypeId::DragonFireball: return "fire_charge";
                // MC ThrownItemRenderer draws the eye as its own item sprite,
                // the same as the snowball and the egg.
                case Game::EntityTypeId::EyeOfEnder:    return "ender_eye";
                case Game::EntityTypeId::EnderPearl:    return "ender_pearl";
                default: return nullptr;
            }
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

        // Re-emits an ALREADY BUILT vertex/index range with an RGB multiplier
        // — MC's renderColoredCutoutModel over the same posed geometry, at
        // the cost of a copy instead of a re-pose. The copy draws at equal
        // depth AFTER the original, which wins under the LessEqual depth
        // test, exactly how MC's coincident layer meshes (the sheep's wool
        // undercoat is the sheep BODY layer) resolve.
        void AppendTintedCopy(std::vector<ModelVertex>& verts,
                              std::vector<uint32_t>& idx,
                              size_t vertFirst, size_t vertCount,
                              size_t idxFirst, size_t idxCount,
                              uint8_t r, uint8_t g, uint8_t b) {
            const auto newBase = static_cast<uint32_t>(verts.size());
            verts.reserve(verts.size() + vertCount);
            for (size_t i = 0; i < vertCount; ++i) {
                ModelVertex v = verts[vertFirst + i];
                v.r = static_cast<uint8_t>((v.r * r) / 255);
                v.g = static_cast<uint8_t>((v.g * g) / 255);
                v.b = static_cast<uint8_t>((v.b * b) / 255);
                verts.push_back(v);
            }
            const auto oldBase = static_cast<uint32_t>(vertFirst);
            for (size_t i = 0; i < idxCount; ++i) {
                idx.push_back(idx[idxFirst + i] - oldBase + newBase);
            }
        }

        void MarkRetained(ModelPart& part,
                          std::initializer_list<std::string_view> retain,
                          std::vector<std::pair<ModelPart*, bool>>& saved) {
            bool keep = false;
            for (std::string_view n : retain) {
                if (part.name == n) { keep = true; break; }
            }
            saved.emplace_back(&part, part.skipDraw);
            if (!keep) part.skipDraw = true;
            for (auto& child : part.children) MarkRetained(*child, retain, saved);
        }

        // MC PartDefinition.retainExactParts, applied at render time: exactly
        // the named parts keep their cubes, every other part (ancestors and
        // descendants included) becomes an empty container. skipDraw is
        // precisely that per part, so the ALREADY POSED model is rebuilt with
        // skipDraw toggled and restored — no second mesh instance, no
        // re-pose. `alpha` lands on the vertex colour for the translucent
        // emissive layers (the warden's pulsating spots / heart).
        size_t AppendRetainedParts(EntityModel& model,
                                   const glm::mat4& entityMatrix,
                                   std::initializer_list<std::string_view> retain,
                                   float alpha,
                                   std::vector<ModelVertex>& verts,
                                   std::vector<uint32_t>& idx) {
            std::vector<std::pair<ModelPart*, bool>> saved;
            MarkRetained(model.Root(), retain, saved);
            const size_t vertFirst = verts.size();
            const size_t idxFirst = idx.size();
            model.Root().Build(entityMatrix, model.TexWidth(), model.TexHeight(),
                               verts, idx, model.CullBackFaces());
            for (auto& [part, old] : saved) part->skipDraw = old;
            if (alpha < 1.0f) {
                const auto a = static_cast<uint8_t>(
                    std::clamp(alpha, 0.0f, 1.0f) * 255.0f + 0.5f);
                for (size_t i = vertFirst; i < verts.size(); ++i) verts[i].a = a;
            }
            return idx.size() - idxFirst;
        }

        // MC RenderSetup.sortOnUpload for one translucent batch: the quads
        // of [first, end) — six indices per quad, as ModelPart::Build emits
        // them — reordered far to near by centroid distance. Vertices are
        // camera-relative (EntityMatrix subtracts cameraPos), so the
        // distance is the position's own length. Each quad's own vertices
        // and winding are untouched; only the quad order in the index
        // stream changes, which is what MC's BufferBuilder sort does.
        void SortQuadsBackToFront(const std::vector<ModelVertex>& verts,
                                  std::vector<uint32_t>& idx,
                                  size_t first, size_t end) {
            const size_t quadCount = (end - first) / 6;
            if (quadCount < 2) return;
            std::vector<std::pair<float, uint32_t>> order;
            order.reserve(quadCount);
            for (size_t q = 0; q < quadCount; ++q) {
                const uint32_t* tri = &idx[first + q * 6];
                // Corners 0,1,2 and 3 of the quad: indices 0,1,2 and 5.
                float sx = 0.0f, sy = 0.0f, sz = 0.0f;
                for (const uint32_t k : { tri[0], tri[1], tri[2], tri[5] }) {
                    sx += verts[k].x; sy += verts[k].y; sz += verts[k].z;
                }
                order.emplace_back(sx * sx + sy * sy + sz * sz,
                                   static_cast<uint32_t>(q));
            }
            std::stable_sort(order.begin(), order.end(),
                             [](const auto& a, const auto& b) { return a.first > b.first; });
            std::vector<uint32_t> sorted;
            sorted.reserve(quadCount * 6);
            for (const auto& [dist, q] : order) {
                const size_t at = first + static_cast<size_t>(q) * 6;
                sorted.insert(sorted.end(), idx.begin() + static_cast<std::ptrdiff_t>(at),
                              idx.begin() + static_cast<std::ptrdiff_t>(at + 6));
            }
            std::copy(sorted.begin(), sorted.end(),
                      idx.begin() + static_cast<std::ptrdiff_t>(first));
        }

        // The posed transform chain from the model root down to `name` — MC
        // ModelPart.translateAndRotate along the bake chain. The layers that
        // use it (mooshroom head, golem arm) sit directly under an identity
        // root in MC, so composing the whole chain is the same matrix, and
        // stays right if a mesh ever nests them. Valid only after SetupAnim.
        bool PartChainMatrix(const ModelPart& part, std::string_view name,
                             glm::mat4& out) {
            const glm::mat4 here = out * part.LocalMatrix();
            if (part.name == name) { out = here; return true; }
            for (const auto& child : part.children) {
                glm::mat4 m = here;
                if (PartChainMatrix(*child, name, m)) { out = m; return true; }
            }
            return false;
        }

        // The two crossed quads of MC's block/cross.json (poppy,
        // red_mushroom): each plane runs corner to corner from 0.8 to 15.2
        // px carrying the full 16x16 sprite — the same span the JSON's ±45°
        // rotation with rescale=true produces. `m` maps the UNIT block cell
        // to world space; cross.json is shade=false, so the verts stay
        // white. Both quads single-sided: the mob pipeline culls nothing.
        void AppendCrossBlock(const glm::mat4& m,
                              std::vector<ModelVertex>& verts,
                              std::vector<uint32_t>& idx) {
            constexpr float lo = 0.8f / 16.0f, hi = 15.2f / 16.0f;
            const glm::vec3 planes[2][2] = {
                { { lo, 0.0f, lo }, { hi, 0.0f, hi } },
                { { hi, 0.0f, lo }, { lo, 0.0f, hi } },
            };
            for (const auto& p : planes) {
                const auto base = static_cast<uint32_t>(verts.size());
                const auto push = [&](const glm::vec3& c, float y,
                                      float u, float v) {
                    const glm::vec3 w =
                        glm::vec3(m * glm::vec4(c.x, y, c.z, 1.0f));
                    verts.push_back({ w.x, w.y, w.z, u, v,
                                      255, 255, 255, 255 });
                };
                // v = 0 at the top of the sprite (y = 1 in the cell).
                push(p[0], 0.0f, 0.0f, 1.0f);
                push(p[1], 0.0f, 1.0f, 1.0f);
                push(p[1], 1.0f, 1.0f, 0.0f);
                push(p[0], 1.0f, 0.0f, 0.0f);
                idx.insert(idx.end(), { base, base + 1, base + 2,
                                        base, base + 2, base + 3 });
            }
        }

        // One face set of the UNIT block cell — for MC's block/orientable
        // model (the snow golem's carved pumpkin): the caller appends one
        // sheet's faces at a time so each lands in its own texture batch.
        // Face UVs follow BlockModel's cube faces (u left→right as seen
        // facing the face, v top→bottom), shade is the block model's
        // directional shade. `m` maps the unit cell to world space.
        enum BlockFace : int {
            kFaceDown = 1, kFaceUp = 2, kFaceNorth = 4,
            kFaceSouth = 8, kFaceWest = 16, kFaceEast = 32,
        };
        void AppendUnitBlockFaces(const glm::mat4& m, int faces,
                                  std::vector<ModelVertex>& verts,
                                  std::vector<uint32_t>& idx) {
            const auto face = [&](const glm::vec3& a, const glm::vec3& b,
                                  const glm::vec3& c, const glm::vec3& d, float shade) {
                // a = (u0,v0), b = (u1,v0), c = (u1,v1), d = (u0,v1)
                const auto base = static_cast<uint32_t>(verts.size());
                const auto sh = static_cast<uint8_t>(shade * 255.0f);
                const auto push = [&](const glm::vec3& p, float u, float v) {
                    const glm::vec3 w = glm::vec3(m * glm::vec4(p, 1.0f));
                    verts.push_back({ w.x, w.y, w.z, u, v, sh, sh, sh, 255 });
                };
                push(a, 0.0f, 0.0f); push(b, 1.0f, 0.0f);
                push(c, 1.0f, 1.0f); push(d, 0.0f, 1.0f);
                idx.insert(idx.end(), { base, base + 1, base + 2,
                                        base, base + 2, base + 3 });
            };
            if (faces & kFaceDown)  face({0,0,0}, {1,0,0}, {1,0,1}, {0,0,1}, 0.5f);
            if (faces & kFaceUp)    face({0,1,0}, {1,1,0}, {1,1,1}, {0,1,1}, 1.0f);
            if (faces & kFaceNorth) face({1,1,0}, {0,1,0}, {0,0,0}, {1,0,0}, 0.8f);
            if (faces & kFaceSouth) face({0,1,1}, {1,1,1}, {1,0,1}, {0,0,1}, 0.8f);
            if (faces & kFaceWest)  face({0,1,0}, {0,1,1}, {0,0,1}, {0,0,0}, 0.6f);
            if (faces & kFaceEast)  face({1,1,1}, {1,1,0}, {1,0,0}, {1,0,1}, 0.6f);
        }

        // MC FlameFeatureRenderer.renderFlame — the stack of camera-facing
        // fire quads over a burning entity: scaled by width*1.4, one
        // 1.4-high slice every 0.45 up the box, each shrinking 0.9x and
        // stepping -0.03 toward the camera, sampling fire_0 and fire_1
        // alternately with a u-flip every second segment. This appends only
        // the segments of one sprite (`odd` = the fire_1 ones), so each
        // sheet lands in its own texture batch. `frame` selects the strip's
        // animation frame (v-range of one 16x16 cell in the 16x512 strip).
        void AppendFlame(bool odd, const glm::dvec3& renderPos,
                         const glm::vec3& cameraPos, float bbWidth,
                         float bbHeight, int frame, float camYaw,
                         std::vector<ModelVertex>& verts,
                         std::vector<uint32_t>& idx) {
            const glm::vec3 rel(
                static_cast<float>(renderPos.x - cameraPos.x),
                static_cast<float>(renderPos.y - cameraPos.y),
                static_cast<float>(renderPos.z - cameraPos.z));
            glm::mat4 m = glm::translate(glm::mat4(1.0f), rel);
            // MC billboards by the camera's Y rotation alone
            // (Mth.rotationAroundAxis(Y_AXIS, camera.orientation)).
            m = glm::rotate(m, camYaw, glm::vec3(0.0f, 1.0f, 0.0f));
            const float s = bbWidth * 1.4f;
            m = glm::scale(m, glm::vec3(s));
            float h = bbHeight / s;
            m = glm::translate(m, glm::vec3(
                0.0f, 0.0f, 0.3f - static_cast<float>(static_cast<int>(h)) * 0.02f));

            const float v0 = static_cast<float>(frame) / 32.0f;
            const float v1 = static_cast<float>(frame + 1) / 32.0f;
            float r = 0.5f, yo = 0.0f, zo = 0.0f;
            for (int ss = 0; h > 0.0f;
                 ++ss, h -= 0.45f, yo -= 0.45f, r *= 0.9f, zo -= 0.03f) {
                if ((ss % 2 == 1) != odd) continue;
                float u0 = 0.0f, u1 = 1.0f;
                if ((ss / 2) % 2 == 0) std::swap(u0, u1);
                const auto base = static_cast<uint32_t>(verts.size());
                const auto push = [&](float x, float y, float u, float v) {
                    const glm::vec3 p =
                        glm::vec3(m * glm::vec4(x, y, zo, 1.0f));
                    verts.push_back({ p.x, p.y, p.z, u, v, 255, 255, 255, 255 });
                };
                // MC fireVertex order: (-r,0)(r,0)(r,1.4)(-r,1.4) with
                // (u1,v1)(u0,v1)(u0,v0)(u1,v0).
                push(-r, 0.0f - yo, u1, v1);
                push( r, 0.0f - yo, u0, v1);
                push( r, 1.4f - yo, u0, v0);
                push(-r, 1.4f - yo, u1, v0);
                idx.insert(idx.end(), { base, base + 1, base + 2,
                                        base, base + 2, base + 3 });
            }
        }

    } // namespace

    MobRenderer::~MobRenderer() { Shutdown(); }

    TextureHandle MobRenderer::BeamTexture() {
        if (!m_beamTextureTried) {
            m_beamTextureTried = true;
            m_beamTexture = LoadTexture(
                "assets/textures/entity/end_crystal/end_crystal_beam.png");
            if (m_beamTexture != INVALID_TEXTURE && g_renderBackend) {
                // The beam scrolls its V past 1 (v1 = length/32 - t*0.01), so
                // the sheet must REPEAT — the loader's default clamp smears
                // the last row down the whole tube.
                g_renderBackend->SetTextureWrap(m_beamTexture,
                                                TextureWrap::Repeat,
                                                TextureWrap::Repeat);
            }
        }
        return m_beamTexture;
    }

    void MobRenderer::AppendCrystalBeam(const glm::dvec3& baseWorld,
                                        const glm::dvec3& tipWorld,
                                        float ageInTicks,
                                        const glm::vec3& cameraPos,
                                        std::vector<ModelVertex>& verts,
                                        std::vector<uint32_t>& idx) {
        // MC EnderDragonRenderer.submitCrystalBeams: an 8-segment tube,
        // narrow (x0.2) and black at the base, full-width white at the tip,
        // V scrolling with time. MC aims it with two pose rotations; an
        // orthonormal basis around the axis is the same tube with the seam
        // in a slightly different place.
        const glm::vec3 base(baseWorld.x - cameraPos.x,
                             baseWorld.y - cameraPos.y,
                             baseWorld.z - cameraPos.z);
        const glm::vec3 tipRel(tipWorld.x - cameraPos.x,
                               tipWorld.y - cameraPos.y,
                               tipWorld.z - cameraPos.z);
        const glm::vec3 axis = tipRel - base;
        const float length = glm::length(axis);
        if (length < 1.0e-4f) return;
        const glm::vec3 w = axis / length;
        glm::vec3 u = glm::cross(w, glm::vec3(0.0f, 1.0f, 0.0f));
        if (glm::dot(u, u) < 1.0e-6f) u = glm::vec3(1.0f, 0.0f, 0.0f);
        u = glm::normalize(u);
        const glm::vec3 v = glm::cross(w, u);

        const float v0 = -ageInTicks * 0.01f;
        const float v1 = length / 32.0f - ageInTicks * 0.01f;

        float lastSin = 0.0f;
        float lastCos = 0.75f;
        float lastU = 0.0f;
        for (int i = 1; i <= 8; ++i) {
            const float ringSin =
                std::sin(static_cast<float>(i) * Game::Mth::kPi * 2.0f / 8.0f) * 0.75f;
            const float ringCos =
                std::cos(static_cast<float>(i) * Game::Mth::kPi * 2.0f / 8.0f) * 0.75f;
            const float ringU = static_cast<float>(i) / 8.0f;

            const glm::vec3 p0 = base + u * (lastSin * 0.2f) + v * (lastCos * 0.2f);
            const glm::vec3 p1 = base + u * lastSin + v * lastCos + w * length;
            const glm::vec3 p2 = base + u * ringSin + v * ringCos + w * length;
            const glm::vec3 p3 = base + u * (ringSin * 0.2f) + v * (ringCos * 0.2f);

            const auto vbase = static_cast<uint32_t>(verts.size());
            verts.push_back({ p0.x, p0.y, p0.z, lastU, v0, 0, 0, 0, 255 });
            verts.push_back({ p1.x, p1.y, p1.z, lastU, v1, 255, 255, 255, 255 });
            verts.push_back({ p2.x, p2.y, p2.z, ringU, v1, 255, 255, 255, 255 });
            verts.push_back({ p3.x, p3.y, p3.z, ringU, v0, 0, 0, 0, 255 });
            idx.push_back(vbase + 0);
            idx.push_back(vbase + 1);
            idx.push_back(vbase + 2);
            idx.push_back(vbase + 0);
            idx.push_back(vbase + 2);
            idx.push_back(vbase + 3);

            lastSin = ringSin;
            lastCos = ringCos;
            lastU = ringU;
        }
    }

    void MobRenderer::AppendDragonRays(const glm::dvec3& centerWorld,
                                       float deathTime01,
                                       const glm::vec3& cameraPos,
                                       std::vector<ModelVertex>& verts,
                                       std::vector<uint32_t>& idx) {
        // MC EnderDragonRenderer.submitRays: a growing fan of triangles,
        // white core to zero-alpha magenta edges, orientations drawn from a
        // FIXED seed (432) so the fan is stable frame to frame — only the
        // death-driven spin term and the count move.
        const glm::vec3 center(centerWorld.x - cameraPos.x,
                               centerWorld.y - cameraPos.y,
                               centerWorld.z - cameraPos.z);
        const float overdrive =
            std::min(deathTime01 > 0.8f ? (deathTime01 - 0.8f) / 0.2f : 0.0f, 1.0f);
        const auto innerAlpha =
            static_cast<uint8_t>(std::clamp(1.0f - overdrive, 0.0f, 1.0f) * 255.0f);
        Game::JavaRandom random(432);
        const int rayCount = static_cast<int>(std::floor(
            (deathTime01 + deathTime01 * deathTime01) / 2.0f * 60.0f));

        for (int i = 0; i < rayCount; ++i) {
            // MC composes six random euler rotations plus the death spin;
            // three plus the spin give the same uniformly-scattered fan.
            const float rx = random.NextFloat() * Game::Mth::kPi * 2.0f;
            const float ry = random.NextFloat() * Game::Mth::kPi * 2.0f;
            const float rz = random.NextFloat() * Game::Mth::kPi * 2.0f +
                             deathTime01 * Game::Mth::kPi * 0.5f;
            glm::mat4 rot(1.0f);
            rot = glm::rotate(rot, rx, glm::vec3(1.0f, 0.0f, 0.0f));
            rot = glm::rotate(rot, ry, glm::vec3(0.0f, 1.0f, 0.0f));
            rot = glm::rotate(rot, rz, glm::vec3(0.0f, 0.0f, 1.0f));

            const float rayLength =
                random.NextFloat() * 20.0f + 5.0f + overdrive * 10.0f;
            const float rayWidth =
                random.NextFloat() * 2.0f + 1.0f + overdrive * 2.0f;
            constexpr float kHalfSqrt3 = 0.8660254f;

            const glm::vec3 o = center;
            const glm::vec3 a = center + glm::vec3(rot * glm::vec4(
                -kHalfSqrt3 * rayWidth, rayLength, -0.5f * rayWidth, 0.0f));
            const glm::vec3 b = center + glm::vec3(rot * glm::vec4(
                kHalfSqrt3 * rayWidth, rayLength, -0.5f * rayWidth, 0.0f));
            const glm::vec3 c = center + glm::vec3(rot * glm::vec4(
                0.0f, rayLength, rayWidth, 0.0f));

            const auto vbase = static_cast<uint32_t>(verts.size());
            // 0xFF00FF at alpha 0 on the rim — the blend fades each ray out
            // toward its tip exactly as MC's dragonRays render type does.
            verts.push_back({ o.x, o.y, o.z, 0.0f, 0.0f,
                              255, 255, 255, innerAlpha });
            verts.push_back({ a.x, a.y, a.z, 0.0f, 0.0f, 255, 0, 255, 0 });
            verts.push_back({ b.x, b.y, b.z, 0.0f, 0.0f, 255, 0, 255, 0 });
            verts.push_back({ c.x, c.y, c.z, 0.0f, 0.0f, 255, 0, 255, 0 });
            idx.push_back(vbase + 0); idx.push_back(vbase + 1); idx.push_back(vbase + 2);
            idx.push_back(vbase + 0); idx.push_back(vbase + 2); idx.push_back(vbase + 3);
            idx.push_back(vbase + 0); idx.push_back(vbase + 3); idx.push_back(vbase + 1);
        }
    }

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

        for (FrameBuffers& fb : m_frames) {
            fb.vb = g_renderBackend->CreateBuffer(
                BufferUsage::Vertex, kMaxVertices * sizeof(ModelVertex),
                nullptr, BufferAccess::Streaming);
            fb.ib = g_renderBackend->CreateBuffer(
                BufferUsage::Index, kMaxIndices * sizeof(uint32_t),
                nullptr, BufferAccess::Streaming);
            fb.mesh = g_renderBackend->CreateMesh(fb.vb, fb.ib, GetBlockVertexLayout());
        }

        // 1x1 white — the untextured-geometry stand-in (the dragon's death
        // rays are colour-only in MC; every batch here binds a texture).
        {
            const uint32_t whitePixel = 0xFFFFFFFFu;
            m_whiteTexture = g_renderBackend->CreateTexture2D(
                1, 1, TextureFormat::RGBA8, &whitePixel);
        }

        m_verts.reserve(65536);
        m_indices.reserve(98304);

        m_initialized = true;
        Log::Info("[MobRenderer] initialized");
        return true;
    }

    void MobRenderer::Shutdown() {
        if (!g_renderBackend) return;

        for (FrameBuffers& fb : m_frames) {
            if (fb.mesh != INVALID_MESH) { g_renderBackend->DestroyMesh(fb.mesh); fb.mesh = INVALID_MESH; }
            if (fb.vb != INVALID_BUFFER) { g_renderBackend->DestroyBuffer(fb.vb); fb.vb = INVALID_BUFFER; }
            if (fb.ib != INVALID_BUFFER) { g_renderBackend->DestroyBuffer(fb.ib); fb.ib = INVALID_BUFFER; }
        }
        if (m_shader != INVALID_SHADER)      { g_renderBackend->DestroyShader(m_shader); m_shader = INVALID_SHADER; }

        for (auto& [path, tex] : m_textureCache) {
            if (tex != INVALID_TEXTURE) g_renderBackend->DestroyTexture(tex);
        }
        m_textureCache.clear();
        m_models.clear();

        // Sprite textures lived in m_textureCache and were destroyed above;
        // only the CPU geometry caches are ours.
        m_spriteEntries.clear();
        m_heldTridentModel.reset();

        m_initialized = false;
    }

    TextureHandle MobRenderer::LoadTexture(const std::string& relativePath, bool repeatWrap) {
        const std::string cacheKey = repeatWrap ? relativePath + "|repeat" : relativePath;
        const auto it = m_textureCache.find(cacheKey);
        if (it != m_textureCache.end()) return it->second;

        // Same load as ChestRenderer::LoadVariantTexture — nearest filtering
        // and clamped wrap, because entity sheets are pixel art whose edges
        // must not bleed into the neighbouring part's texels.
        const std::string full = PlatformMain::GetAssetPath(relativePath);
        if (!std::filesystem::exists(full)) {
            Log::Warning("[MobRenderer] missing texture %s", relativePath.c_str());
            m_textureCache[cacheKey] = INVALID_TEXTURE;
            return INVALID_TEXTURE;
        }

        int w = 0, h = 0, ch = 0;
        stbi_set_flip_vertically_on_load(0);
        unsigned char* pixels = stbi_load(full.c_str(), &w, &h, &ch, STBI_rgb_alpha);
        if (!pixels) {
            Log::Warning("[MobRenderer] failed to decode %s", relativePath.c_str());
            m_textureCache[cacheKey] = INVALID_TEXTURE;
            return INVALID_TEXTURE;
        }

        TextureHandle tex = g_renderBackend->CreateTexture2D(w, h, TextureFormat::RGBA8, pixels);
        stbi_image_free(pixels);

        g_renderBackend->SetTextureFilter(tex, TextureFilter::Nearest, TextureFilter::Nearest);
        const TextureWrap wrap = repeatWrap ? TextureWrap::Repeat : TextureWrap::ClampToEdge;
        g_renderBackend->SetTextureWrap(tex, wrap, wrap);

        m_textureCache[cacheKey] = tex;
        return tex;
    }

    void SetBabyModelLook(BabyModelLook look) {
        if (s_babyLook == look) return;
        s_babyLook = look;
        ++s_babyLookGeneration;
    }
    BabyModelLook GetBabyModelLook() { return s_babyLook; }
    int BabyModelLookGeneration() { return s_babyLookGeneration; }

    MobRenderer::ModelEntry* MobRenderer::GetModelFor(Game::EntityTypeId type) {
        const auto key = static_cast<uint16_t>(type);
        const auto it = m_models.find(key);
        if (it != m_models.end()) return &it->second;

        std::unique_ptr<EntityModel> model = CreateModelFor(type);
        if (!model) {
            // Cache the miss. Without this every type with no model — primed
            // TNT and falling blocks, which BlockCubeEntityRenderer draws —
            // paid a hash miss plus CreateModelFor's 120-entry string scan on
            // EVERY frame, which at a hundred thousand TNT was the whole of
            // the MobRender zone. The loop treats an INVALID_TEXTURE entry as
            // "nothing to draw", so an empty entry is the negative answer.
            m_models.emplace(key, ModelEntry{});
            return nullptr;
        }

        const std::string_view texturePath = TexturePathFor(type);
        if (texturePath.empty()) return nullptr;

        ModelEntry entry;
        entry.model = std::move(model);
        entry.texture = LoadTexture(std::string(texturePath));
        switch (type) {
            case Game::EntityTypeId::Sheep:
                entry.overlayModel = std::make_unique<SheepModel>(true);
                break;
            // MC DrownedOuterLayer — the DROWNED_OUTER_LAYER mesh (the body
            // inflated 0.25/0.5), posed by the drowned's own program.
            case Game::EntityTypeId::Drowned:
                if (FindGenModel("drowned_outer")) {
                    entry.overlayModel = std::make_unique<GeneratedModel>(
                        "drowned_outer", "drowned");
                }
                break;
            // MC SkeletonClothingLayer — STRAY_OUTER_LAYER (humanoid mesh
            // inflated 0.25) / BOGGED_OUTER_LAYER (inflated 0.2).
            case Game::EntityTypeId::Stray:
                if (FindGenModel("stray_clothes")) {
                    entry.overlayModel = std::make_unique<GeneratedModel>(
                        "stray_clothes", "stray");
                }
                break;
            case Game::EntityTypeId::Bogged:
                if (FindGenModel("bogged_clothes")) {
                    entry.overlayModel = std::make_unique<GeneratedModel>(
                        "bogged_clothes", "bogged");
                }
                break;
            // MC PufferfishRenderer — the mid/big puff-stage meshes.
            case Game::EntityTypeId::Pufferfish:
                if (FindGenModel("pufferfish_mid")) {
                    entry.pufferMid =
                        std::make_unique<PufferfishModel>("pufferfish_mid");
                }
                if (FindGenModel("pufferfish_big")) {
                    entry.pufferBig =
                        std::make_unique<PufferfishModel>("pufferfish_big");
                }
                break;
            default:
                break;
        }

        return &m_models.emplace(key, std::move(entry)).first->second;
    }

    glm::mat4 MobRenderer::EntityMatrix(const glm::dvec3& renderPos,
                                        const glm::vec3& cameraPos,
                                        float bodyRot, float scale,
                                        float deathFlipDeg,
                                        const glm::vec3& modelScale,
                                        float swimPitchDeg,
                                        float swimPivotY,
                                        const glm::vec3& modelOffset) {
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
        // MC DrownedRenderer.setupRotations:33-42 — the swim tilt: after the
        // base rotations (super.setupRotations, death flip included) and
        // before the Y flip, the whole body pitches about a pivot half the
        // bounding box up (`rotateAround(Axis.XP.rotationDegrees(rotationX),
        // 0, boundingBoxHeight / 2 / scale, 0)`).
        if (swimPitchDeg != 0.0f) {
            m = glm::translate(m, glm::vec3(0.0f, swimPivotY, 0.0f));
            m = glm::rotate(m, glm::radians(swimPitchDeg), glm::vec3(1.0f, 0.0f, 0.0f));
            m = glm::translate(m, glm::vec3(0.0f, -swimPivotY, 0.0f));
        }
        // MC applies state.scale (the SCALE attribute) before setupRotations
        // and the -1,-1,1 flip after; a uniform scale commutes with the
        // rotations, so folding the two together here is the same chain.
        m = glm::scale(m, glm::vec3(-scale, -scale, scale));
        // MC LivingEntityRenderer.submit calls the renderer's scale() hook
        // HERE — after the -1,-1,1 flip and before the model Y offset, so the
        // offset below is scaled along with the body. Identity for everything
        // except the swelling creeper.
        if (modelScale != glm::vec3(1.0f)) m = glm::scale(m, modelScale);
        m = glm::scale(m, glm::vec3(1.0f / 16.0f));

        // The Y offset is in blocks, so it is applied in the flipped, scaled
        // space: MC's translate(0, -1.501, 0) sits after scale(-1,-1,1), where
        // -Y is up. Pre-multiplying it in block units here is the same thing.
        // MC's poseStack.translate after scale() and the −1.501 fold into one
        // pixel-space translate (both sit after every scale).
        m = glm::translate(m, glm::vec3(0.0f, kModelYOffset * 16.0f, 0.0f) + modelOffset * 16.0f);
        return m;
    }

    glm::mat4 MobRenderer::AppendMob(EntityModel& model, const EntityRenderState& state,
                                     const glm::dvec3& renderPos, float bodyRot,
                                     const glm::vec3& cameraPos,
                                     std::vector<ModelVertex>& verts,
                                     std::vector<uint32_t>& idx) {
        model.SetupAnim(state);

        const glm::mat4 m = EntityMatrix(renderPos, cameraPos, bodyRot, state.scale,
                                         state.deathFlipDeg, state.modelScale,
                                         state.swimPitchDeg, state.swimPivotY,
                                         state.modelOffset);
        model.Root().Build(m, model.TexWidth(), model.TexHeight(), verts, idx,
                           model.CullBackFaces());
        return m;
    }

    TextureHandle MobRenderer::AppendHeldSprite(const EntityModel& model,
                                                const glm::mat4& entityMatrix,
                                                const std::string& itemName,
                                                const DisplaySpec& spec,
                                                std::vector<ModelVertex>& verts,
                                                std::vector<uint32_t>& idx) {
        glm::mat4 hand;
        if (!model.RightHandMatrix(hand)) return INVALID_TEXTURE;
        SpriteEntry* entry = EnsureSpriteGeometry(itemName);
        if (!entry) return INVALID_TEXTURE;

        // ── MC ItemInHandLayer.submitArmWithItem ───────────────────────────
        // MC's pose stack is in BLOCKS here; this one is in model PIXELS, so
        // its translate(1/16, 0.125, -0.625) is written as (1, 2, -10). The
        // display block that follows is the item JSON's
        // thirdperson_righthand, applied in MC ItemTransform.apply's order:
        // translate (sixteenths of a block), rotationXYZ, scale.
        glm::mat4 m = entityMatrix * hand;
        m = glm::rotate(m, glm::radians(-90.0f), glm::vec3(1.0f, 0.0f, 0.0f));
        m = glm::rotate(m, glm::radians(180.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        m = glm::translate(m, glm::vec3(1.0f, 2.0f, -10.0f));
        m = glm::scale(m, glm::vec3(16.0f));

        m = glm::translate(m, spec.translation * 0.0625f);
        m = glm::rotate(m, glm::radians(spec.rotationDeg.x), glm::vec3(1.0f, 0.0f, 0.0f));
        m = glm::rotate(m, glm::radians(spec.rotationDeg.y), glm::vec3(0.0f, 1.0f, 0.0f));
        m = glm::rotate(m, glm::radians(spec.rotationDeg.z), glm::vec3(0.0f, 0.0f, 1.0f));
        m = glm::scale(m, glm::vec3(spec.scale));

        m = glm::translate(m, glm::vec3(-0.5f, -0.5f, 0.0f));
        m = glm::scale(m, glm::vec3(1.0f / 16.0f));

        const auto base = static_cast<uint32_t>(verts.size());
        for (const ModelVertex& v : entry->verts) {
            const glm::vec3 p = glm::vec3(m * glm::vec4(v.x, v.y, v.z, 1.0f));
            verts.push_back({ p.x, p.y, p.z, v.u, v.v, v.r, v.g, v.b, v.a });
        }
        for (uint32_t i : entry->indices) idx.push_back(base + i);
        return entry->texture;
    }

    TextureHandle MobRenderer::AppendHeldTrident(const EntityModel& model,
                                                 const glm::mat4& entityMatrix,
                                                 std::vector<ModelVertex>& verts,
                                                 std::vector<uint32_t>& idx) {
        glm::mat4 hand;
        if (!model.RightHandMatrix(hand)) return INVALID_TEXTURE;
        if (!m_heldTridentModel) {
            m_heldTridentModel = std::make_unique<TridentModel>();
        }
        // The geometry beneath the projectile wrappers (pivot/yaw/orient) is
        // MC's raw vertical trident; the wrappers only exist to point the
        // FLYING trident along +X, so they are bypassed here.
        ModelPart* orient = m_heldTridentModel->Root().Find("orient");
        if (!orient) return INVALID_TEXTURE;

        // MC path: ItemInHandLayer grip, then trident_in_hand.json's
        // thirdperson_righthand block, then the special-model renderer's
        // scale(1, -1, -1) around the centred [0,1] cell, and finally the
        // model's own pixel space. VERIFY IN GAME: the special-renderer flip
        // and cell centring were derived from the code, not observed — if
        // the trident sits mirrored or offset, this is the block to adjust.
        glm::mat4 m = entityMatrix * hand;
        m = glm::rotate(m, glm::radians(-90.0f), glm::vec3(1.0f, 0.0f, 0.0f));
        m = glm::rotate(m, glm::radians(180.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        m = glm::translate(m, glm::vec3(1.0f, 2.0f, -10.0f));
        m = glm::scale(m, glm::vec3(16.0f));

        // trident_in_hand.json display.thirdperson_righthand.
        m = glm::translate(m, glm::vec3(11.0f, 17.0f, -2.0f) * 0.0625f);
        m = glm::rotate(m, glm::radians(60.0f), glm::vec3(0.0f, 1.0f, 0.0f));

        // MC ItemRenderer centres the unit cell, then the trident special
        // renderer flips Y/Z before drawing the entity-space model.
        m = glm::translate(m, glm::vec3(-0.5f, -0.5f, -0.5f));
        m = glm::scale(m, glm::vec3(1.0f, -1.0f, -1.0f));
        m = glm::scale(m, glm::vec3(1.0f / 16.0f));

        for (const auto& child : orient->children) {
            child->Build(m, m_heldTridentModel->TexWidth(),
                         m_heldTridentModel->TexHeight(), verts, idx,
                         m_heldTridentModel->CullBackFaces());
        }
        return LoadTexture("assets/textures/entity/trident.png");
    }

    MobRenderer::SpriteEntry* MobRenderer::EnsureSpriteGeometry(const std::string& itemName) {
        auto it = m_spriteEntries.find(itemName);
        if (it != m_spriteEntries.end()) {
            return it->second.state == AssetState::Ready ? &it->second : nullptr;
        }

        SpriteEntry entry;
        entry.state = AssetState::Failed;   // pessimistic, like the bow

        std::vector<HeldItemSpriteMesh::Vertex> sprite;
        std::vector<uint32_t> idx;
        if (HeldItemSpriteMesh::BuildGeometry(itemName, 0, sprite, idx)) {
            entry.texture = LoadTexture("assets/textures/item/" + itemName + ".png");
            if (entry.texture != INVALID_TEXTURE) {
                entry.verts.reserve(sprite.size());
                for (const auto& v : sprite) {
                    entry.verts.push_back({ v.x, v.y, v.z, v.u, v.v,
                                            v.r, v.g, v.b, v.a });
                }
                entry.indices = std::move(idx);
                entry.state = AssetState::Ready;
            }
        }

        auto& stored = m_spriteEntries.emplace(itemName, std::move(entry)).first->second;
        return stored.state == AssetState::Ready ? &stored : nullptr;
    }

    TextureHandle MobRenderer::AppendSpriteProjectile(const std::string& itemName,
                                                      const glm::dvec3& renderPos,
                                                      float halfHeight,
                                                      const glm::vec3& cameraPos,
                                                      std::vector<ModelVertex>& verts,
                                                      std::vector<uint32_t>& idx) {
        SpriteEntry* entry = EnsureSpriteGeometry(itemName);
        if (!entry) return INVALID_TEXTURE;

        // MC ThrownItemRenderer: the item model, centred on the entity,
        // rotated to face the camera, at the GROUND display scale (0.5).
        glm::vec3 rel(static_cast<float>(renderPos.x - cameraPos.x),
                      static_cast<float>(renderPos.y - cameraPos.y) + halfHeight,
                      static_cast<float>(renderPos.z - cameraPos.z));

        const float len = std::sqrt(rel.x * rel.x + rel.y * rel.y + rel.z * rel.z);
        if (len < 1.0e-4f) return INVALID_TEXTURE;
        // The sprite's front face (+Z in its own space) must point back at
        // the camera: yaw about Y, then pitch about X (see the derivation of
        // the billboard normal in the header comment for HeldItemSpriteMesh's
        // authoring space).
        const float yaw = std::atan2(-rel.x, -rel.z);
        const float pitch = std::asin(std::clamp(rel.y / len, -1.0f, 1.0f));

        glm::mat4 m = glm::translate(glm::mat4(1.0f), rel);
        m = glm::rotate(m, yaw, glm::vec3(0.0f, 1.0f, 0.0f));
        m = glm::rotate(m, pitch, glm::vec3(1.0f, 0.0f, 0.0f));
        m = glm::scale(m, glm::vec3(0.5f));
        m = glm::translate(m, glm::vec3(-0.5f, -0.5f, 0.0f));
        m = glm::scale(m, glm::vec3(1.0f / 16.0f));

        const auto base = static_cast<uint32_t>(verts.size());
        for (const ModelVertex& v : entry->verts) {
            const glm::vec3 p = glm::vec3(m * glm::vec4(v.x, v.y, v.z, 1.0f));
            verts.push_back({ p.x, p.y, p.z, v.u, v.v, v.r, v.g, v.b, v.a });
        }
        for (uint32_t i : entry->indices) idx.push_back(base + i);
        return entry->texture;
    }

    void MobRenderer::Render(const glm::mat4& projection, const glm::mat4& view,
                             const glm::vec3& cameraPos, const Frustum& frustum,
                             const Client::ClientMobManager& mobs,
                             float partialTick) {
        PROFILE_ZONE_N("MobRender");

        if (!m_initialized || !g_renderBackend) return;
        // Resource pack reload: the textures AND everything holding their
        // handles — the model entries, the projectile sprites, the crystal
        // beam — go together; the models come back lazily.
        if (Resources::CacheStale(m_textureCacheGeneration)) {
            for (auto& [path, tex] : m_textureCache) if (tex != INVALID_TEXTURE) g_renderBackend->DestroyTexture(tex);
            m_textureCache.clear();
            m_models.clear();
            m_spriteEntries.clear();
            m_beamTexture = INVALID_TEXTURE;
            m_beamTextureTried = false;
        }
        // World Settings → Baby Models: every cached model (adult rabbit
        // included) is rebuilt for the other look; textures are re-picked
        // per frame anyway.
        if (m_babyLookGeneration != BabyModelLookGeneration()) {
            m_babyLookGeneration = BabyModelLookGeneration();
            m_models.clear();
        }
        if (mobs.All().empty()) return;

        // Which streaming set this call writes, and where in it. A new frame
        // flips the set and starts it over; a later call in the same frame
        // (the portal pass) appends after the main pass's geometry so both
        // survive to submit. See EntityCulling.hpp.
        if (m_frameCursor.Advance()) {
            m_vertCursor = 0;
            m_idxCursor  = 0;
        }
        FrameBuffers& fb = m_frames[m_frameCursor.parity];
        if (fb.mesh == INVALID_MESH) return;
        // Room left in this frame's set. Nothing to do when a previous call
        // filled it — the guard inside the loop stops before overrunning.
        if (m_vertCursor + 4096 >= kMaxVertices || m_idxCursor + 8192 >= kMaxIndices) return;
        const size_t vertRoom = kMaxVertices - m_vertCursor;
        const size_t idxRoom  = kMaxIndices  - m_idxCursor;

        const float maxDistSq = kMaxRenderDistance * kMaxRenderDistance;
        int culledCount = 0;

        // Group by texture so the whole scene collapses into a handful of draws.
        struct Batch {
            TextureHandle texture = INVALID_TEXTURE;
            glm::vec4     overlay{0.0f};
            size_t        firstIndex = 0;
            size_t        indexCount = 0;
            // Alpha-blended draw (MC RenderTypes.entityTranslucentEmissive)
            // — only the warden's fading emissive layers set it; their
            // per-vertex alpha carries the fade.
            bool          blend = false;
            // Back-face culled (MC's entityCutout / entitySolid /
            // entityTranslucent models — EntityModel::CullBackFaces).
            bool          cull = false;
        };
        std::vector<Batch> batches;

        m_verts.clear();
        m_indices.clear();

        // MC submitFlame billboards a burning mob's fire by the camera's Y
        // rotation alone (Mth.rotationAroundAxis(Y_AXIS, camera.orientation)).
        // The view rotation's third row is the camera's world-space z-axis,
        // so the yaw that turns the flame quad's +Z back at the camera is
        // atan2 of its x/z — one value for the whole frame.
        const float flameYaw = std::atan2(view[0][2], view[2][2]);

        // Primed TNT and falling blocks are BlockCubeEntityRenderer's; this
        // renderer has no model for them, and ModelMobList() is the list
        // with them already left out — walking the full list to skip them on
        // the type field was 1.8 ms a frame at 176k falling blocks, a cache
        // miss per entry to learn nothing.
        for (const Client::ClientMob* entryPtr : mobs.ModelMobList()) {
            const Client::ClientMob& entry = *entryPtr;
            const int32_t id = entry.selfId;
            (void)id;
            const Game::Mob& mob = *entry.mob;
            const Game::EntityTypeId type = mob.GetType();

            // Sub-tick interpolation, the same scheme PlayerRenderer uses.
            glm::dvec3 renderPos = glm::mix(entry.renderPrevPosition, mob.position,
                                            static_cast<double>(partialTick));

            // MC EndermanRenderer.getRenderOffset: the creepy ±0.02-block
            // gaussian x/z jitter every frame — the signature vibration of a
            // stared-at enderman. (creepy == aggressive, same mapping the
            // render-state block below uses.)
            if (type == Game::EntityTypeId::Enderman && mob.IsAggressive()) {
                static Game::JavaRandom creepyJitter(0x5DEECE66DLL);
                renderPos.x += creepyJitter.NextGaussian() * 0.02;
                renderPos.z += creepyJitter.NextGaussian() * 0.02;
            }

            // MC PufferfishRenderer.setupRotations:59 — the vertical bob:
            // translate(0, cos(ageInTicks * 0.05) * 0.08, 0) BEFORE the body
            // rotations; a pure-Y shift commutes with the yaw, so folding it
            // into the render position is the same matrix.
            if (type == Game::EntityTypeId::Pufferfish) {
                renderPos.y += std::cos((static_cast<float>(mob.tickCount) +
                                         partialTick) * 0.05f) * 0.08;
            }

            // MC EntityRenderer.shouldRender's frustum test on the culling
            // box, then the visible-section gate — everything behind the
            // camera or behind a wall used to be posed, built and uploaded
            // regardless. The box is the entity's own dimensions around the
            // INTERPOLATED position, inflated 0.5 inside ShouldRender.
            //
            // Beam exemption — MC EndCrystalRenderer.shouldRender:
            //     super.shouldRender(...) || entity.getBeamTarget() != null
            // A crystal's beam spans blocks its own 2x2 culling box knows
            // nothing about (a ritual crystal beams 128 blocks up), so a
            // crystal with an active beam is never culled: look up at the
            // beam with the crystal off-screen and the beam must still be
            // there. The dragon gets the same exemption while its healing
            // beam is live or its death rays are playing — its geometry
            // extends far outside the body box in exactly the same way.
            bool beamExempt = false;
            if (type == Game::EntityTypeId::EndCrystal) {
                const auto* c = MobAs<Game::EndCrystal>(
                    mob, type, Game::EntityTypeId::EndCrystal);
                beamExempt = c && c->HasBeamTarget();
            } else if (type == Game::EntityTypeId::EnderDragon) {
                const auto* d = MobAs<Game::EnderDragon>(
                    mob, type, Game::EntityTypeId::EnderDragon);
                beamExempt = d && (d->nearestCrystal != nullptr ||
                                   mob.IsDeadOrDying());
            }
            // The distance gate sits under the same exemption — MC's
            // EndCrystalRenderer `||` bypasses EntityRenderer.shouldRender's
            // distance half as well as its frustum half, and a far-ring
            // pillar crystal (up to ~190 blocks out during the ritual) would
            // otherwise drop its beam at 160.
            if (!beamExempt) {
                const glm::vec3 delta(renderPos.x - cameraPos.x,
                                      renderPos.y - cameraPos.y,
                                      renderPos.z - cameraPos.z);
                const float distSq = glm::dot(delta, delta);
                if (distSq > maxDistSq) continue;
                // A crossing pass wants only the mobs in the portal.
                if (EntityCulling::g_crossingFilter) {
                    const glm::vec3 half(mob.GetBbWidth() * 0.5f, 0.0f, mob.GetBbWidth() * 0.5f);
                    const glm::vec3 lo = glm::vec3(renderPos) - half;
                    const glm::vec3 hi = glm::vec3(renderPos) + half + glm::vec3(0.0f, mob.GetBbHeight(), 0.0f);
                    if (!EntityCulling::PassesCrossingFilter(lo, hi)) continue;
                }
                // MC Entity.shouldRenderAtSqrDistance — size- and view-
                // scaled, the half the Entity Distance option drives.
                if (!EntityCulling::ShouldRenderAtSqrDistance(distSq, mob.GetBbWidth(),
                                                              mob.GetBbHeight())) {
                    ++culledCount;
                    continue;
                }

                if (!EntityCulling::ShouldRender(frustum, glm::vec3(renderPos),
                                                 mob.GetBbWidth(),
                                                 mob.GetBbHeight())) {
                    ++culledCount;
                    continue;
                }
            }

            // Sprite projectiles (MC ThrownItemRenderer) — a camera-facing
            // item sprite, no entity model at all.
            if (const char* sprite = SpriteNameForProjectile(type)) {
                const size_t firstIndex = m_indices.size();
                const TextureHandle tex = AppendSpriteProjectile(
                    sprite, renderPos, mob.GetBbHeight() * 0.5f, cameraPos,
                    m_verts, m_indices);
                if (tex != INVALID_TEXTURE && m_indices.size() > firstIndex) {
                    batches.push_back({ tex, glm::vec4(0.0f), firstIndex,
                                        m_indices.size() - firstIndex });
                }
                continue;
            }

            ModelEntry* modelEntry = GetModelFor(type);
            if (!modelEntry || modelEntry->texture == INVALID_TEXTURE) continue;

            float bodyRot = Game::Mth::RotLerp(partialTick, entry.renderPrevYBodyRot,
                                               mob.yBodyRot);
            const float headRot = Game::Mth::RotLerp(partialTick, entry.renderPrevYHeadRot,
                                                     mob.GetYHeadRot());

            // MC EnderDragonRenderer.submit poses the body from the FLIGHT
            // HISTORY, not yBodyRot: yaw is sample 7's yRot (rotated by -yr
            // where LivingEntityRenderer rotates by 180-yaw — the model is
            // authored NOSE-AFT, so the two fold into bodyRot = yr + 180),
            // and the pitch is the 5-vs-10 height delta times 10.
            const auto* dragonMob =
                MobAs<Game::EnderDragon>(mob, type, Game::EntityTypeId::EnderDragon);
            const auto* crystalMob =
                MobAs<Game::EndCrystal>(mob, type, Game::EntityTypeId::EndCrystal);
            float dragonPitchDeg = 0.0f;
            float dragonPartial = partialTick;
            if (dragonMob) {
                // MC: state.partialTicks is zeroed while dead so the corpse
                // freezes instead of sliding through stale samples.
                if (mob.IsDeadOrDying()) dragonPartial = 0.0f;
                bodyRot = dragonMob->flightHistory.Get(7, dragonPartial).yRot +
                          180.0f;
                dragonPitchDeg = static_cast<float>(
                    dragonMob->flightHistory.Get(5, dragonPartial).y -
                    dragonMob->flightHistory.Get(10, dragonPartial).y) * 10.0f;
            }

            EntityRenderState state;
            // MC passes the head yaw RELATIVE to the body, so the model only
            // has to rotate the head part by the difference. Passing the
            // absolute yaw makes every mob look permanently over its shoulder.
            state.yRot = Game::Mth::WrapDegrees(headRot - bodyRot);
            state.xRot = Game::Mth::Lerp(partialTick, entry.renderPrevXRot, mob.xRot);
            state.walkAnimationPos = mob.walkAnimation.PositionAt(partialTick);
            state.walkAnimationSpeed = mob.walkAnimation.SpeedAt(partialTick);
            state.ageInTicks = static_cast<float>(mob.tickCount) + partialTick;
            // MC EndCrystalRenderState.ageInTicks reads the crystal's own
            // random-seeded clock — a ring of crystals must not bob in step.
            if (crystalMob) {
                state.ageInTicks =
                    static_cast<float>(crystalMob->time) + partialTick;
                state.crystalShowsBottom = crystalMob->ShowsBottom();
            }
            // MC ArmedEntityRenderState: attackTime = getAttackAnim(partial) —
            // the wrap-aware lerp; the raw field renders a 6-tick swing as 6
            // discrete steps.
            state.attackTime = mob.GetAttackAnim(partialTick);
            state.isAggressive = mob.IsAggressive();
            state.isBaby = mob.IsBaby();
            // MC LivingEntity.getAgeScale — a model INPUT (attack-swing arm
            // slide, sheep graze drop), not a render scale.
            state.ageScale = state.isBaby ? Game::kBabyScale : 1.0f;

            // ── The baby MESH — MC AgeableMobRenderer.submit ──────────────
            //
            // MC draws a baby through a SEPARATE model (BabyModelTransform's
            // big head / half body), with NO uniform shrink on top:
            // LivingEntityRenderer scales only by the SCALE attribute, which
            // is 1.0. The old whole-model kBabyScale survives purely as the
            // fallback for a baby with no baby mesh (MC has none either, or
            // this port lacks its texture — the nautilus).
            EntityModel* model = modelEntry->model.get();
            EntityModel* overlayModel = modelEntry->overlayModel.get();
            if (state.isBaby) {
                if (!modelEntry->babyTried) {
                    modelEntry->babyTried = true;
                    modelEntry->babyModel = CreateBabyModelFor(type);
                    if (modelEntry->babyModel &&
                        type == Game::EntityTypeId::Sheep) {
                        // MC 26.1 SheepWoolLayer.babyModel (SHEEP_BABY_WOOL)
                        // under the New look; the classic wool transform
                        // otherwise.
                        if (NewBabies() && FindGenModel("sheep_wool_baby_new")) {
                            auto wool = MakeRemodel("sheep_wool_baby_new", "sheep_baby_new");
                            InflateCubes(wool->Root(), 0.02f);
                            modelEntry->babyOverlayModel = std::move(wool);
                        } else {
                            auto fur = std::make_unique<SheepModel>(true);
                            fur->BecomeBaby();
                            modelEntry->babyOverlayModel = std::move(fur);
                        }
                    }
                    // MC DrownedOuterLayer's babyModel — the outer-layer
                    // mesh through HumanoidModel.BABY_TRANSFORMER.
                    if (modelEntry->babyModel &&
                        type == Game::EntityTypeId::Drowned) {
                        // 26.2's BabyDrownedModel outer layer (its own
                        // program) under the New look; the classic
                        // transform row otherwise.
                        if (NewBabies() && FindGenModel("drowned_outer_baby_new")) {
                            // BabyZombieModel's head and hat cubes take
                            // literal deformations, so the outer layer's
                            // head sits ON the body's; MC wins that by draw
                            // order, this gets it as geometry (see the
                            // sheep's wool).
                            auto outer = MakeRemodel("drowned_outer_baby_new", "drowned");
                            InflateCubes(outer->Root(), 0.02f);
                            modelEntry->babyOverlayModel = std::move(outer);
                        } else if (FindGenModel("drowned_outer_baby")) {
                            modelEntry->babyOverlayModel =
                                std::make_unique<GeneratedModel>(
                                    "drowned_outer_baby", "drowned");
                        }
                    }
                }
                if (modelEntry->babyModel) {
                    model = modelEntry->babyModel.get();
                    overlayModel = modelEntry->babyOverlayModel.get();
                } else {
                    state.scale = Game::kBabyScale;   // mini-adult fallback
                }
            }
            // ── The variant MESH — MC CowRenderer/PigRenderer/ChickenRenderer
            //    .submit picks the AdultAndBabyModelPair for the variant's
            //    ModelType; the texture follows below.
            if (type == Game::EntityTypeId::Cow || type == Game::EntityTypeId::Pig ||
                type == Game::EntityTypeId::Chicken) {
                const uint8_t variant = std::min<uint8_t>(mob.GetVariantByte(), 2);
                if (variant != 0) {
                    if (!modelEntry->variantTried[variant]) {
                        modelEntry->variantTried[variant] = true;
                        modelEntry->variantModels[variant] = CreateVariantModelFor(type, variant);
                        if (modelEntry->variantModels[variant]) {
                            auto baby = CreateVariantModelFor(type, variant);
                            if (baby && baby->BecomeBaby()) modelEntry->variantBabyModels[variant] = std::move(baby);
                        }
                    }
                    // 26.1's babies share ONE mesh per species (the warm/
                    // cold rows all map to BabyCowModel & co.), so the New
                    // look keeps the babyModel picked above.
                    EntityModel* variantModel = state.isBaby
                        ? (NewBabies() ? nullptr : modelEntry->variantBabyModels[variant].get())
                        : modelEntry->variantModels[variant].get();
                    if (variantModel) model = variantModel;
                }
            }
            // The entity's own size (scaled portals, /scale) on top of the
            // type's — the box follows it too (Entity::GetBbWidth).
            state.scale *= mob.scale;
            // MC PufferfishRenderer.submit — the model is swapped whole by
            // the synced puff state: 0 small, 1 mid, anything else big.
            if (type == Game::EntityTypeId::Pufferfish) {
                switch (static_cast<const Game::Pufferfish&>(mob).GetPuffState()) {
                    case 0:
                        break;
                    case 1:
                        if (modelEntry->pufferMid) model = modelEntry->pufferMid.get();
                        break;
                    default:
                        if (modelEntry->pufferBig) model = modelEntry->pufferBig.get();
                        break;
                }
            }
            state.isInWater = mob.IsInWater();
            // The topple. mob.deathTime is already synced (SetEntityDataS2C
            // carries it) and counts 1..20 over the second between the death
            // event and the removal packet — the animation and the body's life
            // are the same 20 ticks, which is why the corpse never pops.
            //
            // A mob playing a DEATH clip does not also topple: MC's
            // CreakingRenderer zeroes deathTime while tearing down so the
            // 45-tick twitch plays upright, and the clip being started is
            // exactly that condition here.
            // MC overrides getFlipDegrees() to 180 for the bugs — spiders,
            // silverfish and endermites topple onto their backs, not their
            // sides (SpiderRenderer/SilverfishRenderer/EndermiteRenderer).
            float flipDegrees = 90.0f;
            switch (type) {
                case Game::EntityTypeId::Spider:
                case Game::EntityTypeId::CaveSpider:
                case Game::EntityTypeId::Silverfish:
                case Game::EntityTypeId::Endermite:
                    flipDegrees = 180.0f;
                    break;
                default:
                    break;
            }
            state.deathFlipDeg = mob.Anim(Game::MobAnim::Death).IsStarted()
                                     ? 0.0f
                                     : DeathFlipDegrees(mob.deathTime, partialTick,
                                                        flipDegrees);

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
            const bool isSkeleton = (type == Game::EntityTypeId::Skeleton);
            // The bow-armed skeleton family shares the pose: stray, bogged and
            // parched also spawn with bows in MC. The wither skeleton carries a
            // stone SWORD, so it stays out — its aggressive arm-raise is the
            // melee branch, keyed on isHoldingBow being false. Only the plain
            // skeleton also renders the bow ITEM below (the held-item transform
            // needs SkeletonModel::RightHandMatrix, which GeneratedModel does
            // not provide yet).
            const bool holdsBow = isSkeleton
                || type == Game::EntityTypeId::Stray
                || type == Game::EntityTypeId::Bogged
                || type == Game::EntityTypeId::Parched;
            if (holdsBow) {
                state.isHoldingBow = true;
                state.rightArmPose = state.isAggressive ? ArmPose::BowAndArrow
                                                        : ArmPose::Empty;
            }
            // MC HumanoidMobRenderer sets the ITEM arm pose from a non-empty
            // main hand: the wither skeleton carries its stone sword. Its
            // aggressive melee raise still comes from SkeletonModel's own
            // branch (keyed on isHoldingBow being false).
            if (type == Game::EntityTypeId::WitherSkeleton) {
                state.rightArmPose = ArmPose::Item;
            }
            // MC DrownedRenderer.getArmPose: THROW_TRIDENT while aggressive
            // with the trident, ITEM otherwise (the 6.25% spawn roll).
            if (const auto* drowned = MobAs<Game::Drowned>(mob, type, Game::EntityTypeId::Drowned)) {
                if (drowned->HasTrident()) {
                    state.rightArmPose = state.isAggressive
                        ? ArmPose::ThrowTrident : ArmPose::Item;
                }
                // MC LivingEntityRenderer.extractRenderState: swimAmount =
                // getSwimAmount(partialTick). The ramp itself is client-side
                // (ClientMob::swimAmount — see ClientMobManager::Tick).
                state.swimAmount =
                    Game::Mth::Lerp(partialTick, entry.swimAmountO,
                                    entry.swimAmount);
                // MC DrownedRenderer.setupRotations:33-42 — while swimming
                // the body pitches toward -10 - xRot about the mid-box pivot.
                // (MC's box is the 0.6-high SWIMMING pose box by then; the
                // port never applies the swimming pose, so this pivots about
                // the standing box's middle — the only box it has.)
                if (state.swimAmount > 0.0f) {
                    state.swimPitchDeg = Game::Mth::Lerp(
                        state.swimAmount, 0.0f, -10.0f - state.xRot);
                    state.swimPivotY = mob.GetBbHeight() * 0.5f;
                }
            }
            // The vindicator's iron axe: hasMainHandItem flips IllagerModel's
            // ATTACKING branch from the empty-hand zombie arms to MC's
            // swingWeaponDown.
            if (type == Game::EntityTypeId::Vindicator) {
                state.hasMainHandItem = true;
            }

            // MC's per-mob extractRenderState, for the clip guards. Each is
            // one field on one MC RenderState subclass; here they are branches
            // on the entity class, the same way the sheep and chicken below
            // already are.
            // MC FoxRenderer.extractRenderState — the pose flags and the two
            // client ramps (the pounce pitch itself rides the synced xRot).
            if (const auto* fox = MobAs<Game::Fox>(mob, type, Game::EntityTypeId::Fox)) {
                state.crouchAmount = fox->GetCrouchAmount(partialTick);
                state.headRollAngle = fox->GetHeadRollAngle(partialTick);
                state.isFaceplanted = fox->IsFaceplanted();
                state.isSleeping = fox->IsSleeping();
                state.isSitting = fox->IsSitting();
            }
            // MC TurtleRenderer.extractRenderState — hasEgg bulges the
            // shell, isLayingEgg drives the dig pose (isOnLand is set by the
            // type switch below).
            if (const auto* turtle = MobAs<Game::Turtle>(mob, type, Game::EntityTypeId::Turtle)) {
                // MC gates the bulge on !isBaby — a gravid baby cannot exist,
                // but the flag could linger across an age-down command.
                state.hasEgg = !state.isBaby && turtle->HasEgg();
                state.isLayingEgg = turtle->IsLayingEgg();
            }
            // MC PandaRenderer.extractRenderState — the three ramps, the
            // sneeze clock, and the mood flags.
            if (const auto* panda = MobAs<Game::Panda>(mob, type, Game::EntityTypeId::Panda)) {
                state.sitAmount = panda->GetSitAmount(partialTick);
                state.lieOnBackAmount = panda->GetLieOnBackAmount(partialTick);
                state.rollAmount = panda->GetRollAmount(partialTick);
                state.isSneezing = panda->IsSneezing();
                state.sneezeTime =
                    static_cast<float>(panda->GetSneezeCounter()) + partialTick;
                state.isSitting = panda->IsSitting();
                state.isEating = panda->IsEatingPanda();
                state.isScared = panda->IsScared();
                state.isUnhappy = panda->IsUnhappy();
            }
            // MC CatRenderer/FelineRenderState — the lie/relax ramps (idle
            // at 0 while taming is skipped) and the stalk crouch/sprint the
            // synced pose carries.
            if (const auto* cat = MobAs<Game::Cat>(mob, type, Game::EntityTypeId::Cat)) {
                state.lieDownAmount = cat->GetLieDownAmount(partialTick);
                state.lieDownAmountTail = cat->GetLieDownAmountTail(partialTick);
                state.relaxStateOneAmount = cat->GetRelaxStateOneAmount(partialTick);
                state.isCrouching = mob.GetPose() == Game::Pose::Crouching;
                state.isSprinting = mob.IsSprinting();
            }
            if (type == Game::EntityTypeId::Ocelot) {
                state.isCrouching = mob.GetPose() == Game::Pose::Crouching;
                state.isSprinting = mob.IsSprinting();
            }
            // MC AbstractHorseRenderer.extractRenderState — the eat/stand
            // ramps and the tail swish window.
            // The five AbstractHorse subclasses (the llama is a GenericAnimal
            // in MC and here).
            if (const auto* horse = MobAs<Game::AbstractHorse>(
                    mob, type, Game::EntityTypeId::Horse, Game::EntityTypeId::Donkey,
                    Game::EntityTypeId::Mule, Game::EntityTypeId::SkeletonHorse,
                    Game::EntityTypeId::ZombieHorse)) {
                state.eatAnimation = horse->GetEatAnim(partialTick);
                state.standAnimation = horse->GetStandAnim(partialTick);
                state.animateTail = horse->IsAnimatingTail();
            }
            // MC TamableAnimal — isInSittingPose drives the sit pose the
            // cat/wolf/parrot setupAnim programs key on (state.isSitting).
            // The flag arrives on the anim state byte (bit 0), so a tamed
            // pet ordered to sit sits on every client. No overlap with the
            // fox/panda isSitting writers above: neither is a TamableAnimal.
            if (const auto* tamable = TamableOf(mob, type)) {
                state.isSitting = tamable->IsInSittingPose();
            }
            // MC AxolotlRenderer.extractRenderState — the four IN_OUT_SINE
            // animator factors the model blends legs/tail by, ticked
            // client-side on the entity.
            if (const auto* axolotl = MobAs<Game::Axolotl>(mob, type, Game::EntityTypeId::Axolotl)) {
                state.playingDeadFactor = axolotl->GetPlayingDeadFactor(partialTick);
                state.inWaterFactor     = axolotl->GetInWaterFactor(partialTick);
                state.onGroundFactor    = axolotl->GetOnGroundFactor(partialTick);
                state.movingFactor      = axolotl->GetMovingFactor(partialTick);
            }
            // MC PiglinRenderer: the DANCING arm pose while dancing (after a
            // hoglin hunt celebration).
            if (const auto* piglin = MobAs<Game::Piglin>(mob, type, Game::EntityTypeId::Piglin)) {
                if (piglin->IsDancing()) {
                    state.isDancing = true;
                    state.mobArmPose = 4.0f;  // PiglinArmPose.DANCING
                }
            }
            if (const auto* bat = MobAs<Game::Bat>(mob, type, Game::EntityTypeId::Bat)) {
                state.isResting = bat->IsResting();
            }
            if (const auto* armadillo = MobAs<Game::Armadillo>(mob, type, Game::EntityTypeId::Armadillo)) {
                state.isHidingInShell = armadillo->ShouldHideInShell();
            }
            // MC CopperGolemRenderer.extractRenderState — isHoldingItem gates
            // the WALK vs WALK_ITEM clip pair (AnimGuard::IsHoldingItem).
            if (const auto* golem = MobAs<Game::CopperGolem>(mob, type, Game::EntityTypeId::CopperGolem)) {
                state.isHoldingItem = golem->IsHoldingItem();
            }

            if (const auto* sheep = MobAs<Game::Sheep>(mob, type, Game::EntityTypeId::Sheep)) {
                state.headEatPositionScale = sheep->GetHeadEatPositionScale(partialTick);
                state.headEatAngleScale = sheep->GetHeadEatAngleScale(partialTick);
            }
            if (const auto* chicken = MobAs<Game::Chicken>(mob, type, Game::EntityTypeId::Chicken)) {
                state.flap = chicken->GetFlap(partialTick);
                state.flapSpeed = chicken->GetFlapSpeed(partialTick);
            }

            // NOTE on MC's LayerDefinitions mesh scales (wither skeleton 1.2,
            // giant 6.0, cave spider 0.7, husk 1.0625, horse 1.1, cat 0.8,
            // villager-likes 0.9375, ghast 4.5, elder guardian 2.35, ...):
            // the generated mesh carries them as PartPose scale on its root
            // part plus the 24.016·(1−f) re-anchor, exactly MC's
            // MeshTransformer.scaling — the scale must ride the pose, not the
            // cube geometry, because UV layout derives from the UNSCALED cube
            // size. Do NOT also apply a factor here: a renderer-side scale
            // double-scales the model AND mis-anchors the root offset. (A
            // table doing exactly that briefly lived here and made 1.2 render
            // as 1.44.)

            // ── Inputs for the compiled setupAnim programs ────────────────
            //
            // Each line mirrors one MC <Mob>Renderer.extractRenderState
            // statement; the constants (arm-pose ordinals, the wolf's angry
            // tail) are read from the MC entity getters they replace. Fields
            // not set here keep the EntityRenderState defaults, which are
            // MC's values for behaviour this port does not run yet.
            state.entityId = static_cast<float>(id);       // witch nose seed
            state.yHeadRotAbs = headRot;                   // shulker head math
            state.yBodyRotAbs = bodyRot;
            switch (type) {
                case Game::EntityTypeId::Phantom:
                    // PhantomRenderer: uniqueFlapTickOffset = id * 3.
                    state.flapTime = static_cast<float>(id) * 3.0f + state.ageInTicks;
                    break;
                case Game::EntityTypeId::EvokerFangs:
                    // EvokerFangsRenderer: biteProgress = getAnimationProgress.
                    state.biteProgress =
                        static_cast<const Game::EvokerFangs&>(mob)
                            .GetAnimationProgress(partialTick);
                    break;
                case Game::EntityTypeId::Parrot:
                    // ParrotModel.getPose without perching/sitting/party:
                    // FLYING(0) off the ground, STANDING(1) on it.
                    state.mobPose = mob.onGround ? 1.0f : 0.0f;
                    break;
                case Game::EntityTypeId::Vindicator:
                    // Vindicator.getArmPose: ATTACKING while aggressive,
                    // else CROSSED.
                    state.mobArmPose = state.isAggressive ? 1.0f : 0.0f;
                    break;
                case Game::EntityTypeId::Pillager:
                    // Pillager.getArmPose: a pillager always holds its
                    // crossbow -> CROSSBOW_HOLD.
                    state.mobArmPose = 4.0f;
                    break;
                case Game::EntityTypeId::Evoker:
                    // SpellcasterIllager.getArmPose: CROSSED(0); the
                    // SPELLCASTING override rides the spellcaster block below.
                    state.mobArmPose = 0.0f;
                    break;
                case Game::EntityTypeId::Illusioner:
                    // Illusioner.getArmPose: BOW_AND_ARROW(3) while
                    // aggressive, else CROSSED(0); SPELLCASTING(2) overrides
                    // via the SpellcasterIllager cast below, matching MC's
                    // isCastingSpell-first ordering because the cast runs
                    // after this switch.
                    state.mobArmPose = state.isAggressive ? 3.0f : 0.0f;
                    break;
                case Game::EntityTypeId::Piglin:
                    // Piglin.getArmPose: DEFAULT (no items/crossbows here).
                    state.mobArmPose = 5.0f;
                    break;
                case Game::EntityTypeId::PiglinBrute:
                    // PiglinBrute.getArmPose: melee-armed and aggressive ->
                    // ATTACKING_WITH_MELEE_WEAPON(0), else DEFAULT(5).
                    state.mobArmPose = state.isAggressive ? 0.0f : 5.0f;
                    break;
                case Game::EntityTypeId::Dolphin: {
                    // DolphinRenderer: horizontal speed² > 1e-7.
                    const double hx = mob.velocity.x, hz = mob.velocity.z;
                    state.isMoving = (hx * hx + hz * hz) > 1.0e-7;
                    break;
                }
                case Game::EntityTypeId::Turtle:
                    // TurtleRenderer: !isInWater && onGround.
                    state.isOnLand = !state.isInWater && mob.onGround;
                    break;
                case Game::EntityTypeId::Wolf:
                    // WolfRenderer.extractRenderState: real getTailAngle (the
                    // tame health wag included) and the beg head tilt.
                    state.isAngry = state.isAggressive;
                    if (const auto* wolf = MobAs<Game::Wolf>(mob, type, Game::EntityTypeId::Wolf)) {
                        state.tailAngle = wolf->GetTailAngle();
                        state.headRollAngle = wolf->GetHeadRollAngle(partialTick);
                    } else if (state.isAggressive) {
                        state.tailAngle = 1.5393804f;
                    }
                    break;
                case Game::EntityTypeId::Enderman:
                    // EndermanRenderer: creepy == angry at a target.
                    state.isCreepy = state.isAggressive;
                    break;
                case Game::EntityTypeId::Bee:
                    state.isAngry = state.isAggressive;
                    break;
                default:
                    break;
            }

            // ── Overlay: hurt flash, then the creeper's swell + whiteout ──
            glm::vec4 overlay(0.0f);
            // MC LivingEntityRenderer: hasRedOverlay = hurtTime>0 || deathTime>0,
            // with two per-mob overrides — the dragon flashes on hurtTime ONLY
            // (EnderDragonRenderer: the 20-tick death dive is not a red-out)
            // and the creaking suppresses the flash while its 45-tick death
            // clip tears it down (CreakingRenderer forces it false there).
            bool hasRedOverlay = mob.hurtTime > 0 || mob.deathTime > 0;
            if (dragonMob) hasRedOverlay = mob.hurtTime > 0;
            // MC EnderDragonRenderer: through the 200-tick death the body
            // dissolves via the dragon_exploding alpha ramp; the overlay
            // whiteout approximates that burn-out (rays appended below).
            float dragonDeath01 = 0.0f;
            if (dragonMob && mob.deathTime > 0) {
                dragonDeath01 = std::clamp(
                    (static_cast<float>(mob.deathTime) + partialTick) / 200.0f,
                    0.0f, 1.0f);
            }
            if (type == Game::EntityTypeId::Creaking &&
                mob.Anim(Game::MobAnim::Death).IsStarted()) {
                hasRedOverlay = false;
            }
            if (hasRedOverlay) {
                // MC OverlayTexture's red row is 0xB2FF0000 and the shader does
                // `mix(overlayColor.rgb, color.rgb, overlayColor.a)` — so the
                // red contributes 1 - 178/255 = 0.302, not the alpha itself.
                overlay = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f - 178.0f / 255.0f);
            } else if (dragonDeath01 > 0.0f) {
                overlay = glm::vec4(1.0f, 1.0f, 1.0f, dragonDeath01 * 0.8f);
            }
            // MC EnderDragonRenderer.extractRenderState — the pre-lerped
            // flight-history window, the lerped flap clock, and the
            // getHeadPartYOffset inputs. (The crystal beam, the red decal and
            // the death rays are the skipped End-fight render layers.)
            if (dragonMob) {
                state.hasDragonHistory = true;
                for (int d = 0; d < EntityRenderState::kDragonHistorySamples; ++d) {
                    const Game::DragonFlightHistory::Sample s =
                        dragonMob->flightHistory.Get(d, dragonPartial);
                    state.dragonY[d] = s.y;
                    state.dragonYRot[d] = s.yRot;
                }
                state.dragonFlapTime = Game::Mth::Lerp(
                    partialTick, dragonMob->oFlapTime, dragonMob->flapTime);
                state.dragonIsSitting = dragonMob->IsPhaseSitting();
                state.dragonIsLandingOrTakingOff =
                    dragonMob->IsLandingOrTakingOff();
                // Client dragons never learn the fight origin (it does not
                // cross the wire), so the podium distance is pinned to 0 —
                // it only shapes the neck droop during landing/takeoff, and
                // the 0 fallback reads as the full droop, the sitting pose's
                // own value.
                state.dragonDistanceToEgg = 0.0;
            }

            // MC SquidRenderer.extractRenderState — one cast covers GlowSquid,
            // which derives Squid exactly as in MC.
            if (const auto* squid = MobAs<Game::Squid>(
                    mob, type, Game::EntityTypeId::Squid, Game::EntityTypeId::GlowSquid)) {
                state.tentacleAngle = squid->GetTentacleAngle(partialTick);
            }
            // MC GuardianRenderer.extractRenderState — ElderGuardian derives
            // Guardian, so one cast covers both.
            if (const auto* guardian = MobAs<Game::Guardian>(
                    mob, type, Game::EntityTypeId::Guardian, Game::EntityTypeId::ElderGuardian)) {
                state.tailAnimation = guardian->GetTailAnimation(partialTick);
                state.spikesAnimation = guardian->GetSpikesAnimation(partialTick);
            }
            // MC ShulkerRenderer.extractRenderState — the lid-lerp the
            // compiled shulker program reads as PeekAmount.
            if (const auto* shulker = MobAs<Game::Shulker>(mob, type, Game::EntityTypeId::Shulker)) {
                state.peekAmount = shulker->GetClientPeekAmount(partialTick);
            }
            // MC ParrotRenderer.extractRenderState: flapAngle =
            // (sin(flap) + 1) * flapSpeed, already lerped — the compiled
            // parrot program reads it as "Flap".
            if (const auto* parrot = MobAs<Game::Parrot>(mob, type, Game::EntityTypeId::Parrot)) {
                state.flap = parrot->GetFlapAngle(partialTick);
            }
            // MC IronGolemRenderer.extractRenderState: the attack clock minus
            // the partial tick, and the offer-flower clock raw. (crackiness
            // has no render-state field yet.)
            if (const auto* golem = MobAs<Game::IronGolem>(mob, type, Game::EntityTypeId::IronGolem)) {
                state.attackTicksRemaining =
                    golem->GetAttackAnimationTick() > 0
                        ? static_cast<float>(golem->GetAttackAnimationTick()) - partialTick
                        : 0.0f;
                state.offerFlowerTick = static_cast<float>(golem->GetOfferFlowerTick());
            }
            // MC RavagerRenderer.extractRenderState: stun and attack clocks
            // minus the partial tick; the roar is normalised 0..1 across its
            // 20-tick clock.
            if (const auto* ravager = MobAs<Game::Ravager>(mob, type, Game::EntityTypeId::Ravager)) {
                state.stunnedTicksRemaining =
                    ravager->GetStunnedTick() > 0
                        ? static_cast<float>(ravager->GetStunnedTick()) - partialTick
                        : 0.0f;
                state.attackTicksRemaining =
                    ravager->GetAttackTick() > 0
                        ? static_cast<float>(ravager->GetAttackTick()) - partialTick
                        : 0.0f;
                if (ravager->GetRoarTick() > 0) {
                    state.roarAnimation =
                        (static_cast<float>(20 - ravager->GetRoarTick()) + partialTick) / 20.0f;
                } else {
                    state.roarAnimation = 0.0f;
                }
            }
            // MC AbstractHoglinRenderer.extractRenderState — one statement
            // shared by HoglinRenderer and ZoglinRenderer through HoglinBase;
            // here they are separate classes, so two casts.
            if (const auto* hoglin = MobAs<Game::Hoglin>(mob, type, Game::EntityTypeId::Hoglin)) {
                state.attackAnimationRemainingTicks =
                    static_cast<float>(hoglin->GetAttackAnimationRemainingTicks());
            }
            if (const auto* zoglin = MobAs<Game::Zoglin>(mob, type, Game::EntityTypeId::Zoglin)) {
                state.attackAnimationRemainingTicks =
                    static_cast<float>(zoglin->GetAttackAnimationRemainingTicks());
            }
            // MC RabbitRenderer.extractRenderState (isToast and the variant
            // belong to the skipped variant system).
            if (const auto* rabbit = MobAs<Game::Rabbit>(mob, type, Game::EntityTypeId::Rabbit)) {
                state.jumpCompletion = rabbit->GetJumpCompletion(partialTick);
            }
            // MC PolarBearRenderer.extractRenderState: the RAW 0..1 lerp —
            // the model squares it.
            if (const auto* bear = MobAs<Game::PolarBear>(mob, type, Game::EntityTypeId::PolarBear)) {
                state.standScale = bear->GetStandingAnimationScale(partialTick);
            }
            // MC BeeRenderer.extractRenderState (the stinger flag belongs to
            // the sting system's render half).
            if (const auto* bee = MobAs<Game::Bee>(mob, type, Game::EntityTypeId::Bee)) {
                state.rollAmount = bee->GetRollAmount(partialTick);
                // isOnGround = onGround && essentially motionless — BeeModel
                // keys the wing buzz, tucked legs and hover bob on its
                // NEGATION, so leaving the default true froze every flying
                // bee's wings.
                state.isOnGround = mob.onGround &&
                    glm::dot(mob.velocity, mob.velocity) < 1.0e-7;
            }
            // MC CamelRenderer.extractRenderState: the dash cooldown minus
            // the partial tick, floored at 0.
            // MakeGenericMob builds the camel husk from the Camel class too.
            if (const auto* camel = MobAs<Game::Camel>(
                    mob, type, Game::EntityTypeId::Camel, Game::EntityTypeId::CamelHusk)) {
                state.jumpCooldown = std::max(
                    static_cast<float>(camel->GetJumpCooldown()) - partialTick, 0.0f);
            }
            // MC PhantomRenderer.scale: 1 + 0.15 * size, applied to the whole
            // model (the entity's box scales through Phantom::GetBbWidth).
            if (const auto* phantom = MobAs<Game::Phantom>(mob, type, Game::EntityTypeId::Phantom)) {
                const float phantomScale =
                    1.0f + 0.15f * static_cast<float>(phantom->GetPhantomSize());
                state.modelScale = glm::vec3(phantomScale);
            }
            // MC VexRenderer.extractRenderState: isCharging — the model
            // raises the arms and the texture swaps (below, with the other
            // per-instance texture picks).
            if (const auto* vex = MobAs<Game::Vex>(mob, type, Game::EntityTypeId::Vex)) {
                state.isCharging = vex->IsCharging();
            }
            // MC SpellcasterIllager.getArmPose: SPELLCASTING(2) while a spell
            // is up — overrides the CROSSED(0) the type-switch above set.
            // (CELEBRATING waits on raids.) One cast covers the evoker and
            // any future spellcaster promotion.
            if (const auto* caster = MobAs<Game::SpellcasterIllager>(
                    mob, type, Game::EntityTypeId::Evoker, Game::EntityTypeId::Illusioner)) {
                if (caster->IsCastingSpell()) {
                    state.mobArmPose = 2.0f;
                }
            }
            if (const auto* cube = MobAs<Game::SulfurCube>(mob, type, Game::EntityTypeId::SulfurCube)) {
                // MC SulfurCubeRenderer.scale, in order: downscaleSlightly
                // (0.999), AbstractCubeMobRenderer's size × squish stretch
                // (no squish while a block is inside — applySizeAndSquish's
                // `ss = 0`), the TNT swell over the last 10 fuse ticks, then
                // an extra 0.5 for the adult (its 18px shell is 1.125 blocks
                // at scale 1) and the translate that seats the cube on the
                // ground: vOffset 0.98 adult / 1.24 baby, minus one pixel.
                const float size = static_cast<float>(cube->GetSize());
                const float squish = cube->HasBodyItem() ? 0.0f : cube->GetSquish(partialTick);
                state.squish = squish;
                const float stretch = 1.0f / (squish / (size * 0.5f + 1.0f) + 1.0f);
                glm::vec3 scale(size * stretch, size / stretch, size * stretch);
                scale *= 0.999f;
                if (cube->IsPrimed()) {
                    const float fuse = static_cast<float>(cube->GetFuse()) - partialTick + 1.0f;
                    if (fuse < 10.0f && fuse > 0.0f) {
                        float g = std::clamp(1.0f - fuse / 10.0f, 0.0f, 1.0f);
                        g *= g;
                        g *= g;
                        scale *= 1.0f + g * 0.3f;
                    }
                }
                const float extraDownscale = state.isBaby ? 1.0f : 0.5f;
                scale *= extraDownscale;
                state.modelScale = scale;
                const float vOffset = state.isBaby ? 1.24f : 0.98f;
                state.modelOffset = glm::vec3(0.0f, vOffset - 1.0f / 16.0f, 0.0f);
            }
            if (const auto* slime = MobAs<Game::Slime>(
                    mob, type, Game::EntityTypeId::Slime, Game::EntityTypeId::MagmaCube)) {
                // MC SlimeRenderer.scale: 0.999 * size overall (the hair under
                // the full size stops z-fighting between stacked slimes), then
                // the squish spring stretches width against height so a
                // landing slime flattens and a launching one elongates.
                // MagmaCubeRenderer.scale is the SAME stretch WITHOUT the
                // 0.999 — its model has no outer shell to z-fight.
                const float size = static_cast<float>(slime->GetSize());
                const float squish = slime->GetSquish(partialTick);
                // MagmaCubeModel reads the squish directly — its body cubes
                // accordion apart on it — where the slime expresses it through
                // the scale stretch below.
                state.squish = squish;
                const float shrink =
                    type == Game::EntityTypeId::MagmaCube ? 1.0f : 0.999f;
                const float stretch = 1.0f / (squish / (size * 0.5f + 1.0f) + 1.0f);
                state.modelScale = glm::vec3(shrink * size * stretch,
                                             shrink * size / stretch,
                                             shrink * size * stretch);
            }
            if (const auto* creeper = MobAs<Game::Creeper>(mob, type, Game::EntityTypeId::Creeper)) {
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
                // MC OverlayTexture: every red row (v < 8, the hurt rows) is
                // opaque red at EVERY white column u — so a swelling creeper
                // that is also hurt flashes RED, never white. Keep the red
                // overlay set above.
                if (progress > 0.0f && !hasRedOverlay) {
                    // MC OverlayTexture's white row: alpha = 1 - u/15 * 0.75
                    // with u = (int)(progress * 15), and the shader keeps that
                    // fraction of the base colour. Our overlay alpha is the
                    // complement (how much WHITE to mix in), so it is the
                    // 0.75 term directly — 75% white at a full-strength flash.
                    const float u = std::floor(progress * 15.0f);
                    overlay = glm::vec4(1.0f, 1.0f, 1.0f, (u / 15.0f) * 0.75f);
                }
            }

            // Per-instance texture swaps: MC WitherSkullRenderer picks the
            // blue invulnerable sheet for a dangerous skull, and
            // GhastRenderer swaps to the shooting face while charging.
            // MC 26.x sheets under the New baby look: a baby swaps to its
            // `_baby` sheet and the remodeled adult rabbit to its 64x64 one
            // (RemodelTexturePath, generated from the assets); every other path
            // is loaded as given. Every texture pick below goes through this.
            const bool remodelTex = NewBabies() &&
                (state.isBaby || type == Game::EntityTypeId::Rabbit);
            const auto MobTex = [&](std::string_view path) {
                return LoadTexture(remodelTex ? RemodelTexturePath(path, state.isBaby)
                                              : std::string(path));
            };
            // Every baby under the New look starts from its own `_baby`
            // sheet (and the remodeled rabbit from its 64x64 one), so the
            // type's default pick is remapped HERE, before the per-mob
            // overrides below re-pick through the same MobTex.
            TextureHandle batchTexture = remodelTex ? MobTex(TexturePathFor(type))
                                                    : modelEntry->texture;
            // MC HappyGhastRenderer.getTextureLocation: babies have their own
            // sheet (the baby mesh's extra inner_body maps onto it).
            if (state.isBaby &&
                type == Game::EntityTypeId::HappyGhast) {
                batchTexture = MobTex(
                    "assets/textures/entity/ghast/happy_ghast_baby.png");
            }
            if (type == Game::EntityTypeId::SulfurCube) {
                // SulfurCubeRenderer.getTextureLocation: the small sheet for
                // the baby (SmallSulfurCubeModel's 64x64 layout).
                batchTexture = MobTex(state.isBaby
                    ? "assets/textures/entity/sulfur_cube/sulfur_cube_outer_small.png"
                    : "assets/textures/entity/sulfur_cube/sulfur_cube_outer.png");
            }
            if (const auto* skull = MobAs<Game::WitherSkull>(mob, type, Game::EntityTypeId::WitherSkull)) {
                if (skull->IsDangerous()) {
                    batchTexture = MobTex(
                        "assets/textures/entity/wither/wither_invulnerable.png");
                }
            }
            // The happy ghast is a GenericAnimal, not a Ghast — Ghast alone.
            if (const auto* ghast = MobAs<Game::Ghast>(mob, type, Game::EntityTypeId::Ghast)) {
                if (ghast->IsCharging()) {
                    batchTexture = MobTex(
                        "assets/textures/entity/ghast/ghast_shooting.png");
                }
            }
            // MC VexRenderer.getTextureLocation: the charging sheet while
            // isCharging.
            if (const auto* chargingVex = MobAs<Game::Vex>(mob, type, Game::EntityTypeId::Vex)) {
                if (chargingVex->IsCharging()) {
                    batchTexture = MobTex(
                        "assets/textures/entity/illager/vex_charging.png");
                }
            }
            // MC BeeRenderer.getTextureLocation: the 2x2 angry/nectar sheet
            // matrix. Angry rides the wire's aggressive bit (what
            // state.isAngry already reads); nectar is bit 2 of the bee's anim
            // state byte (bits 0/1 are roll/stung) — the server-side setter
            // is the Bee-class half of the pollination wiring, and until it
            // lands the bit reads 0, i.e. the no-nectar sheet.
            if (const auto* texBee = MobAs<Game::Bee>(mob, type, Game::EntityTypeId::Bee)) {
                const bool nectar = (texBee->GetAnimStateByte() & 4) != 0;
                batchTexture = MobTex(state.isAngry
                    ? (nectar ? "assets/textures/entity/bee/bee_angry_nectar.png"
                              : "assets/textures/entity/bee/bee_angry.png")
                    : (nectar ? "assets/textures/entity/bee/bee_nectar.png"
                              : "assets/textures/entity/bee/bee.png"));
            }
            // MC StriderRenderer.getTextureLocation: the cold sheet while
            // suffocating (out of lava, shivering).
            if (const auto* strider = MobAs<Game::Strider>(mob, type, Game::EntityTypeId::Strider)) {
                if (strider->IsSuffocating()) {
                    batchTexture = MobTex(
                        "assets/textures/entity/strider/strider_cold.png");
                }
            }
            // MC WitherBossRenderer: the blue invulnerable sheet through the
            // spawn charge, FLICKERING against the normal sheet every other
            // 5-tick band over the last 80 ticks (getTextureLocation:
            // `ticks > 80 || ticks / 5 % 2 != 1`). The tick count rides the
            // anim byte at exactly the flicker's own 5-tick grain — see
            // Wither::GetAnimStateByte. (The half-health armor OVERLAY is a
            // second model layer — skipped with the armor visual.)
            if (const auto* wither = MobAs<Game::Wither>(mob, type, Game::EntityTypeId::Wither)) {
                const int invTicks = wither->GetClientInvulnerableTicks();
                if (wither->IsInvulnerablePhaseClient() &&
                    (invTicks > 80 || (invTicks / 5) % 2 != 1)) {
                    batchTexture = MobTex(
                        "assets/textures/entity/wither/wither_invulnerable.png");
                }
                // MC WitherBossRenderer.scale: 2.0 always, ramping up from
                // 1.5 as the 220-tick spawn charge runs out. This hook was
                // missing outright — the boss drew at HALF size.
                float witherScale = 2.0f;
                if (wither->IsInvulnerablePhaseClient()) {
                    witherScale -=
                        static_cast<float>(invTicks) / 220.0f * 0.5f;
                }
                state.modelScale = glm::vec3(witherScale);
            }
            // Variant-byte texture tables — MC picks these in each renderer
            // from the entity's variant registry entry; the wire's variant
            // byte carries the same ordinal, in each registry's declaration
            // order.
            switch (type) {
                case Game::EntityTypeId::Wolf: {
                    // WolfRenderer.getTextureLocation: tame > angry > wild
                    // (the pale/default variant — biome wolf variants keep
                    // their assets on disk for when variant rolls land).
                    if (const auto* wolf = MobAs<Game::Wolf>(mob, type, Game::EntityTypeId::Wolf)) {
                        if (wolf->IsTame()) {
                            batchTexture = MobTex(
                                "assets/textures/entity/wolf/wolf_tame.png");
                        } else if (state.isAggressive) {
                            batchTexture = MobTex(
                                "assets/textures/entity/wolf/wolf_angry.png");
                        }
                    }
                    break;
                }
                case Game::EntityTypeId::Cow: {
                    // CowRenderer.getTextureLocation: the variant's asset
                    // (CowVariants: temperate_cow / warm_cow / cold_cow).
                    static const char* const kCow[3] = {
                        "assets/textures/entity/cow/temperate_cow.png",
                        "assets/textures/entity/cow/warm_cow.png",
                        "assets/textures/entity/cow/cold_cow.png"};
                    batchTexture = MobTex(kCow[std::min<uint8_t>(mob.GetVariantByte(), 2)]);
                    break;
                }
                case Game::EntityTypeId::Pig: {
                    static const char* const kPig[3] = {
                        "assets/textures/entity/pig/temperate_pig.png",
                        "assets/textures/entity/pig/warm_pig.png",
                        "assets/textures/entity/pig/cold_pig.png"};
                    batchTexture = MobTex(kPig[std::min<uint8_t>(mob.GetVariantByte(), 2)]);
                    break;
                }
                case Game::EntityTypeId::Chicken: {
                    static const char* const kChicken[3] = {
                        "assets/textures/entity/chicken/temperate_chicken.png",
                        "assets/textures/entity/chicken/warm_chicken.png",
                        "assets/textures/entity/chicken/cold_chicken.png"};
                    batchTexture = MobTex(kChicken[std::min<uint8_t>(mob.GetVariantByte(), 2)]);
                    break;
                }
                case Game::EntityTypeId::Fox: {
                    // FoxRenderer: red/snow sheet, with the _sleep face while
                    // sleeping (variant 0 = red, 1 = snow — FinalizeSpawn's
                    // biome roll).
                    const bool snow = mob.GetVariantByte() == 1;
                    const char* path = state.isSleeping
                        ? (snow ? "assets/textures/entity/fox/snow_fox_sleep.png"
                                : "assets/textures/entity/fox/fox_sleep.png")
                        : (snow ? "assets/textures/entity/fox/snow_fox.png"
                                : "assets/textures/entity/fox/fox.png");
                    batchTexture = MobTex(path);
                    break;
                }
                case Game::EntityTypeId::Panda: {
                    // Panda.Gene ordinals: NORMAL, LAZY, WORRIED, PLAYFUL,
                    // BROWN, WEAK, AGGRESSIVE — the variant byte is the
                    // EFFECTIVE gene.
                    static constexpr const char* kPandaTextures[7] = {
                        "assets/textures/entity/panda/panda.png",
                        "assets/textures/entity/panda/lazy_panda.png",
                        "assets/textures/entity/panda/worried_panda.png",
                        "assets/textures/entity/panda/playful_panda.png",
                        "assets/textures/entity/panda/brown_panda.png",
                        "assets/textures/entity/panda/weak_panda.png",
                        "assets/textures/entity/panda/aggressive_panda.png",
                    };
                    const uint8_t gene = mob.GetVariantByte();
                    if (gene < 7) batchTexture = MobTex(kPandaTextures[gene]);
                    break;
                }
                case Game::EntityTypeId::Axolotl: {
                    // Axolotl.Variant ordinals: LUCY, WILD, GOLD, CYAN, BLUE
                    // (anim byte low 3 bits).
                    static constexpr const char* kAxolotlTextures[5] = {
                        "assets/textures/entity/axolotl/axolotl_lucy.png",
                        "assets/textures/entity/axolotl/axolotl_wild.png",
                        "assets/textures/entity/axolotl/axolotl_gold.png",
                        "assets/textures/entity/axolotl/axolotl_cyan.png",
                        "assets/textures/entity/axolotl/axolotl_blue.png",
                    };
                    const uint8_t v = mob.GetAnimStateByte() & 0x7;
                    if (v < 5) batchTexture = MobTex(kAxolotlTextures[v]);
                    break;
                }
                case Game::EntityTypeId::Cat: {
                    // CatVariants declaration order (tabby..all_black); the
                    // spawn roll is uniform 0-10 (no witch huts / moon phase).
                    static constexpr const char* kCatTextures[11] = {
                        "assets/textures/entity/cat/tabby.png",
                        "assets/textures/entity/cat/black.png",
                        "assets/textures/entity/cat/red.png",
                        "assets/textures/entity/cat/siamese.png",
                        "assets/textures/entity/cat/british_shorthair.png",
                        "assets/textures/entity/cat/calico.png",
                        "assets/textures/entity/cat/persian.png",
                        "assets/textures/entity/cat/ragdoll.png",
                        "assets/textures/entity/cat/white.png",
                        "assets/textures/entity/cat/jellie.png",
                        "assets/textures/entity/cat/all_black.png",
                    };
                    const uint8_t v = mob.GetVariantByte();
                    if (v < 11) batchTexture = MobTex(kCatTextures[v]);
                    break;
                }
                default:
                    break;
            }
            if (batchTexture == INVALID_TEXTURE) batchTexture = modelEntry->texture;

            const size_t firstIndex = m_indices.size();
            const size_t firstVert  = m_verts.size();
            glm::mat4 entityMatrix;
            if (dragonMob) {
                // MC EnderDragonRenderer.submit's own pose-stack chain, which
                // EntityMatrix cannot express: after the yaw comes a PITCH
                // (the 5-vs-10 climb bank) and a one-block forward shift,
                // BEFORE the flip. No death topple — MC's dragon corpse never
                // tips over (its 200-tick float-up is the skipped cinematic).
                modelEntry->model->SetupAnim(state);
                const glm::vec3 relative(renderPos.x - cameraPos.x,
                                         renderPos.y - cameraPos.y,
                                         renderPos.z - cameraPos.z);
                glm::mat4 m = glm::translate(glm::mat4(1.0f), relative);
                m = glm::rotate(m, glm::radians(180.0f - bodyRot),
                                glm::vec3(0.0f, 1.0f, 0.0f));
                m = glm::rotate(m, glm::radians(dragonPitchDeg),
                                glm::vec3(1.0f, 0.0f, 0.0f));
                m = glm::translate(m, glm::vec3(0.0f, 0.0f, 1.0f));
                m = glm::scale(m, glm::vec3(-1.0f, -1.0f, 1.0f));
                m = glm::scale(m, glm::vec3(1.0f / 16.0f));
                m = glm::translate(m, glm::vec3(0.0f, kModelYOffset * 16.0f, 0.0f));
                modelEntry->model->Root().Build(m, modelEntry->model->TexWidth(),
                                                modelEntry->model->TexHeight(),
                                                m_verts, m_indices,
                                                modelEntry->model->CullBackFaces());
                entityMatrix = m;
            } else if (crystalMob) {
                // MC EndCrystalRenderer.submit: scale(2,2,2), translate
                // (0,-0.5,0), NO entity yaw and NO living-entity flip — the
                // crystal model's +y is up as authored (glass above base),
                // which is why this chain has neither the (-1,-1,1) mirror
                // nor kModelYOffset.
                modelEntry->model->SetupAnim(state);
                const glm::vec3 relative(renderPos.x - cameraPos.x,
                                         renderPos.y - cameraPos.y,
                                         renderPos.z - cameraPos.z);
                glm::mat4 m = glm::translate(glm::mat4(1.0f), relative);
                m = glm::scale(m, glm::vec3(2.0f));
                m = glm::translate(m, glm::vec3(0.0f, -0.5f, 0.0f));
                m = glm::scale(m, glm::vec3(1.0f / 16.0f));
                modelEntry->model->Root().Build(m, modelEntry->model->TexWidth(),
                                                modelEntry->model->TexHeight(),
                                                m_verts, m_indices,
                                                modelEntry->model->CullBackFaces());
                entityMatrix = m;
            } else {
                entityMatrix =
                    AppendMob(*model, state, renderPos, bodyRot,
                              cameraPos, m_verts, m_indices);
            }

            const size_t bodyIndexCount = m_indices.size() - firstIndex;
            const size_t bodyVertCount  = m_verts.size() - firstVert;
            if (bodyIndexCount > 0) {
                batches.push_back({ batchTexture, overlay, firstIndex,
                                    bodyIndexCount, false,
                                    model->CullBackFaces() });
            }

            // ── Sulfur cube shell + inner cube — MC SulfurCubeRenderer /
            //    SulfurCubeInnerLayer ──────────────────────────────────────
            //
            // The shell is entityTranslucent (sulfur_cube_outer.png is all
            // partial alpha): blended, its quads sorted far to near as MC's
            // sortOnUpload does. Under it, at order −1, either the swallowed
            // block — its model under the cube's OWN pose, so it turns with
            // the body while the look control settles it onto 0/180 — or the
            // inner cube on its own sheet.
            if (const auto* cube = MobAs<Game::SulfurCube>(mob, type, Game::EntityTypeId::SulfurCube)) {
                if (bodyIndexCount > 0) {
                    batches.back().blend = true;
                    SortQuadsBackToFront(m_verts, m_indices, firstIndex, firstIndex + bodyIndexCount);
                }
                // A lit fuse strobes whatever sits inside white on the TNT's
                // 5-tick cadence (OverlayTexture.pack(u(1), 10)).
                glm::vec4 innerOverlay = overlay;
                if (cube->IsPrimed()) {
                    const float fuse = static_cast<float>(cube->GetFuse()) - partialTick + 1.0f;
                    if (fuse >= 0.0f && (static_cast<int>(fuse) / 5) % 2 == 0) {
                        innerOverlay = glm::vec4(1.0f, 1.0f, 1.0f, 0.753f);
                    }
                }
                if (cube->HasBodyItem()) {
                    // SulfurCubeInnerLayer.submit with a contained block:
                    // rotateDegrees(XP, 180), scale 0.5 for the baby,
                    // translate(-0.5, -0.518, -0.5), then the block model in
                    // its unit cell — in BLOCK units on the pose the body was
                    // drawn with (entityMatrix is that pose in pixels, so
                    // scale by 16 first). The block atlas is the texture.
                    const Game::BlockID block = cube->GetBodyBlock();
                    if (block != Game::BlockID::Air && g_atlasBuilder) {
                        std::vector<ItemCubeVert> bv;
                        std::vector<uint32_t> bi;
                        BlockCubeEntityRenderer::BuildStateMesh(Game::BlockStates::Default(block), bv, bi);
                        if (!bv.empty()) {
                            glm::mat4 m = glm::scale(entityMatrix, glm::vec3(16.0f));
                            m = glm::rotate(m, glm::radians(180.0f), glm::vec3(1.0f, 0.0f, 0.0f));
                            if (state.isBaby) m = glm::scale(m, glm::vec3(0.5f));
                            m = glm::translate(m, glm::vec3(-0.5f, -0.518f, -0.5f));
                            const size_t f = m_indices.size();
                            const auto base = static_cast<uint32_t>(m_verts.size());
                            for (const ItemCubeVert& v : bv) {
                                const glm::vec3 p = glm::vec3(m * glm::vec4(v.x, v.y, v.z, 1.0f));
                                m_verts.push_back({ p.x, p.y, p.z, v.u, v.v, v.r, v.g, v.b, v.a });
                            }
                            for (const uint32_t i : bi) m_indices.push_back(base + i);
                            batches.push_back({ g_atlasBuilder->GetBackendTextureHandle(), innerOverlay,
                                                f, m_indices.size() - f });
                            // order(-1): the block draws BEFORE the shell.
                            std::swap(batches[batches.size() - 2], batches.back());
                        }
                    }
                }
                if (!cube->HasBodyItem()) {
                    if (!modelEntry->innerTried) {
                        modelEntry->innerTried = true;
                        if (FindGenModel("sulfur_cube_inner"))
                            modelEntry->innerModel = MakeRemodel("sulfur_cube_inner", "sulfur_cube");
                        if (FindGenModel("sulfur_cube_baby_inner"))
                            modelEntry->innerBabyModel = MakeRemodel("sulfur_cube_baby_inner", "sulfur_cube");
                    }
                    EntityModel* inner = state.isBaby ? modelEntry->innerBabyModel.get()
                                                      : modelEntry->innerModel.get();
                    const TextureHandle innerTex = LoadTexture(state.isBaby
                        ? "assets/textures/entity/sulfur_cube/sulfur_cube_inner_small.png"
                        : "assets/textures/entity/sulfur_cube/sulfur_cube_inner.png");
                    if (inner && innerTex != INVALID_TEXTURE) {
                        const size_t f = m_indices.size();
                        AppendMob(*inner, state, renderPos, bodyRot, cameraPos, m_verts, m_indices);
                        if (m_indices.size() > f) {
                            batches.push_back({ innerTex, innerOverlay, f, m_indices.size() - f });
                            // order(-1): the inner cube draws BEFORE the shell.
                            std::swap(batches[batches.size() - 2], batches.back());
                        }
                    }
                }
            }

            // ── End-fight beams (MC EnderDragonRenderer.submitCrystalBeams) ──
            //
            // A crystal with a beam target runs its tube from two above the
            // TARGET up to its own bobbing glass; a dragon healing off its
            // nearestCrystal runs one from two above itself to that glass.
            // Both anchors are MC's own quirky choices, kept verbatim.
            if (crystalMob && crystalMob->HasBeamTarget()) {
                const TextureHandle beamTex = BeamTexture();
                if (beamTex != INVALID_TEXTURE) {
                    const glm::ivec3 t = crystalMob->BeamTarget();
                    const glm::dvec3 base(t.x + 0.5, t.y + 2.5, t.z + 0.5);
                    const glm::dvec3 tip =
                        renderPos +
                        glm::dvec3(0.0,
                                   2.0 + EndCrystalModel::GetY(state.ageInTicks),
                                   0.0);
                    const size_t f = m_indices.size();
                    AppendCrystalBeam(base, tip, state.ageInTicks, cameraPos,
                                      m_verts, m_indices);
                    if (m_indices.size() > f) {
                        batches.push_back({ beamTex, glm::vec4(0.0f), f,
                                            m_indices.size() - f });
                    }
                }
            }
            if (dragonMob && dragonMob->nearestCrystal &&
                !dragonMob->nearestCrystal->IsRemoved()) {
                const TextureHandle beamTex = BeamTexture();
                if (beamTex != INVALID_TEXTURE) {
                    const Game::EndCrystal& healer = *dragonMob->nearestCrystal;
                    const float healerAge =
                        static_cast<float>(healer.time) + partialTick;
                    const glm::dvec3 base = renderPos + glm::dvec3(0.0, 2.0, 0.0);
                    const glm::dvec3 tip =
                        healer.position +
                        glm::dvec3(0.0, 2.0 + EndCrystalModel::GetY(healerAge),
                                   0.0);
                    const size_t f = m_indices.size();
                    AppendCrystalBeam(base, tip, state.ageInTicks, cameraPos,
                                      m_verts, m_indices);
                    if (m_indices.size() > f) {
                        batches.push_back({ beamTex, glm::vec4(0.0f), f,
                                            m_indices.size() - f });
                    }
                }
            }

            // ── The death rays (MC submitRays) ────────────────────────────
            if (dragonDeath01 > 0.0f && m_whiteTexture != INVALID_TEXTURE) {
                const size_t f = m_indices.size();
                AppendDragonRays(renderPos + glm::dvec3(0.0, 2.0, 0.0),
                                 dragonDeath01, cameraPos, m_verts, m_indices);
                if (m_indices.size() > f) {
                    batches.push_back({ m_whiteTexture, glm::vec4(0.0f), f,
                                        m_indices.size() - f, /*blend=*/true });
                }
            }

            // ── Eyes layers — MC EyesLayer subclasses ─────────────────────
            //
            // MC renders the SAME model again with the eyes sheet through
            // RenderTypes.eyes (fullbright, NO_OVERLAY). This port's mobs
            // are unlit, so a second pass over the same posed geometry is
            // faithful — literally the body's index range again with the
            // eyes texture, which wins the LessEqual depth test as the later
            // equal-depth draw. (Kept vertex colours carry the port's baked
            // directional shade onto the eyes — accepted: everything here is
            // shaded that way.)
            {
                const char* eyesTex = nullptr;
                switch (type) {
                    // MC SpiderEyesLayer — CaveSpiderRenderer derives
                    // SpiderRenderer, so both wear it.
                    case Game::EntityTypeId::Spider:
                    case Game::EntityTypeId::CaveSpider:
                        eyesTex = "assets/textures/entity/spider_eyes.png";
                        break;
                    // MC EnderEyesLayer.
                    case Game::EntityTypeId::Enderman:
                        eyesTex = "assets/textures/entity/enderman/enderman_eyes.png";
                        break;
                    // MC PhantomEyesLayer.
                    case Game::EntityTypeId::Phantom:
                        eyesTex = "assets/textures/entity/phantom_eyes.png";
                        break;
                    default:
                        break;
                }
                if (eyesTex && bodyIndexCount > 0) {
                    const TextureHandle tex = LoadTexture(eyesTex);
                    if (tex != INVALID_TEXTURE) {
                        // NO_OVERLAY: MC EyesLayer submits with
                        // OverlayTexture.NO_OVERLAY — hurt eyes do not flash.
                        batches.push_back({ tex, glm::vec4(0.0f), firstIndex,
                                            bodyIndexCount });
                    }
                }
            }

            // ── Breeze wind + eyes — MC BreezeWindLayer / BreezeEyesLayer ─
            //
            // BreezeModel builds THREE LayerDefinitions from one base mesh
            // with PartDefinition.retainPartsAndChildren: BREEZE keeps
            // {"head", "rods"} (32x32 breeze.png), BREEZE_WIND keeps
            // {"wind_body"} — the three stacked rings on the 128x128
            // breeze_wind.png — and BREEZE_EYES keeps {"eyes"} (32x32
            // breeze_eyes.png). The generator applies the same filter, so
            // each row here is exactly MC's part set; the earlier version
            // carried all twelve parts in every row, which put the rings'
            // 128-scale texOffs onto the 32x32 body sheet and the head onto
            // the wind sheet. Both layers submit at order 1, after the body.
            //
            // Wind: RenderPipelines.BREEZE_WIND — translucent blend, depth
            // write ON, no cull, NO_OVERLAY, NO_CARDINAL_LIGHTING (the rings
            // take no per-face shade: vertex colour flattened to white), and
            // an OffsetTextureTransform that scrolls u by ageInTicks * 0.02
            // mod 1 — applied here to the appended vertices on a REPEAT-
            // wrapped sheet so the scroll wraps across its edge. The sheet
            // is nothing but partial alpha (no opaque texel), so with depth
            // write on the draw order of the nested rings decides what
            // shows through: MC's breezeWind is sortOnUpload — quads sorted
            // far to near before the draw — done here per quad.
            //
            // Eyes: RenderTypes.breezeEyes = entityTranslucentEmissive —
            // translucent blend, no cull, depth write off. The eyes cubes
            // are the head cubes' exact geometry drawn after the head; with
            // the LessEqual depth test they win on equal depth as in MC.
            if (type == Game::EntityTypeId::Breeze) {
                if (!modelEntry->layersTried) {
                    modelEntry->layersTried = true;
                    if (FindGenModel("breeze_wind")) modelEntry->windModel = MakeRemodel("breeze_wind", "breeze");
                    if (FindGenModel("breeze_eyes")) modelEntry->eyesModel = MakeRemodel("breeze_eyes", "breeze");
                }
                if (modelEntry->windModel) {
                    const TextureHandle wind = LoadTexture(
                        "assets/textures/entity/breeze/breeze_wind.png", /*repeatWrap=*/true);
                    if (wind != INVALID_TEXTURE) {
                        const size_t f = m_indices.size();
                        const size_t vf = m_verts.size();
                        AppendMob(*modelEntry->windModel, state, renderPos,
                                  bodyRot, cameraPos, m_verts, m_indices);
                        // BreezeWindLayer.xOffset(ageInTicks) = t * 0.02, mod 1.
                        // ModelVertex uv is already normalised (ModelPart::
                        // Build divides by the sheet size), so the offset is
                        // a fraction of the sheet, as MC's texture matrix.
                        const float uOffset = std::fmod(state.ageInTicks * 0.02f, 1.0f);
                        for (size_t i = vf; i < m_verts.size(); ++i) {
                            ModelVertex& v = m_verts[i];
                            v.u += uOffset;
                            v.r = v.g = v.b = 255;   // NO_CARDINAL_LIGHTING
                        }
                        SortQuadsBackToFront(m_verts, m_indices, f, m_indices.size());
                        batches.push_back({ wind, glm::vec4(0.0f), f,
                                            m_indices.size() - f, /*blend=*/true });
                    }
                }
                if (modelEntry->eyesModel) {
                    const TextureHandle eyes = LoadTexture(
                        "assets/textures/entity/breeze/breeze_eyes.png");
                    if (eyes != INVALID_TEXTURE) {
                        const size_t f = m_indices.size();
                        AppendMob(*modelEntry->eyesModel, state, renderPos,
                                  bodyRot, cameraPos, m_verts, m_indices);
                        batches.push_back({ eyes, glm::vec4(0.0f), f,
                                            m_indices.size() - f, /*blend=*/true });
                    }
                }
            }

            // ── Creaking eyes — MC CreakingRenderer's emissive layer ──────
            //
            // LivingEntityEmissiveLayer over CreakingModel.createEyesLayer()
            // (retainExactParts {"head"}), alpha = eyesGlowing ? 1 : 0.
            if (type == Game::EntityTypeId::Creaking) {
                const auto& creaking = static_cast<const Game::Creaking&>(mob);
                bool eyesGlowing;
                if (creaking.IsTearingDown()) {
                    // MC Creaking.tickDeath flickers eyesGlowing at random
                    // 2..8-tick intervals through the 45-tick crumble. The
                    // flicker state never crosses the wire, so a fixed
                    // 4-tick alternation stands in — same character,
                    // deterministic.
                    eyesGlowing = (mob.tickCount / 4) % 2 == 0;
                } else {
                    eyesGlowing = creaking.IsActive();
                }
                if (eyesGlowing) {
                    const TextureHandle tex = LoadTexture(
                        "assets/textures/entity/creaking/creaking_eyes.png");
                    const size_t f = m_indices.size();
                    const size_t n = (tex != INVALID_TEXTURE)
                        ? AppendRetainedParts(*model, entityMatrix, { "head" },
                                              1.0f, m_verts, m_indices)
                        : 0;
                    if (n > 0) {
                        // The emissive layer takes the entity's overlay
                        // coords (getOverlayCoords), unlike EyesLayer.
                        batches.push_back({ tex, overlay, f, n });
                    }
                }
            }

            // ── Warden emissive layers — MC WardenRenderer:25-29 ──────────
            //
            // Four LivingEntityEmissiveLayers over part-filtered copies of
            // the warden mesh (WardenModel.create*Layer's retainExactParts
            // sets), each with its own alpha. The TENDRILS layer is skipped:
            // its alpha is tendrilAnimation, driven by vibration game events
            // this port does not run — permanently 0, MC would draw nothing.
            if (type == Game::EntityTypeId::Warden) {
                const auto emissive = [&](const char* texPath,
                                          std::initializer_list<std::string_view> parts,
                                          float alpha, bool blend) {
                    if (alpha <= 1.0e-5f) return;
                    const TextureHandle tex = LoadTexture(texPath);
                    if (tex == INVALID_TEXTURE) return;
                    const size_t f = m_indices.size();
                    const size_t n = AppendRetainedParts(
                        *model, entityMatrix, parts, alpha, m_verts, m_indices);
                    if (n > 0) batches.push_back({ tex, overlay, f, n, blend });
                };
                // Bioluminescent layer: alpha 1, always.
                emissive("assets/textures/entity/warden/warden_bioluminescent_layer.png",
                         { "head", "left_arm", "right_arm", "left_leg", "right_leg" },
                         1.0f, false);
                // Pulsating spots: max(0, cos(age * 0.045) * 0.25), the
                // second sheet half a period out of phase. Translucent
                // emissive in MC — blended here.
                const float spots1 = std::max(
                    0.0f, std::cos(state.ageInTicks * 0.045f) * 0.25f);
                const float spots2 = std::max(
                    0.0f, std::cos(state.ageInTicks * 0.045f + Game::Mth::kPi) * 0.25f);
                emissive("assets/textures/entity/warden/warden_pulsating_spots_1.png",
                         { "body", "head", "left_arm", "right_arm",
                           "left_leg", "right_leg" },
                         spots1, true);
                emissive("assets/textures/entity/warden/warden_pulsating_spots_2.png",
                         { "body", "head", "left_arm", "right_arm",
                           "left_leg", "right_leg" },
                         spots2, true);
                // Heart layer: alpha = Warden.getHeartAnimation — the client
                // heartbeat snaps heartAnimation to 10 every
                // getHeartBeatDelay() ticks and decays 1 per tick (the O/cur
                // pair make it (10 - phase) - partialTick, over 10). The
                // delay is 40 - floor(clamp(anger/80, 0, 1) * 30); client
                // anger is not synced here, so it pins at 40 — MC's
                // calm-warden heartbeat.
                const int   heartPhase = mob.tickCount % 40;
                const float heart = std::clamp(
                    ((10.0f - static_cast<float>(heartPhase)) - partialTick)
                        / 10.0f,
                    0.0f, 1.0f);
                emissive("assets/textures/entity/warden/warden_heart.png",
                         { "body" }, heart, true);
            }

            // ── Iron golem cracks + flower — MC IronGolem*Layer ───────────
            if (type == Game::EntityTypeId::IronGolem) {
                // IronGolemCrackinessLayer: the PARENT model again with the
                // crack sheet — the body's own index range re-drawn.
                // Thresholds are Crackiness.GOLEM (0.75 / 0.5 / 0.25 of max
                // health); health is synced, max health is the attribute.
                const float maxHealth = mob.GetMaxHealth();
                const float fraction =
                    maxHealth > 0.0f ? mob.GetHealth() / maxHealth : 1.0f;
                const char* crackTex = nullptr;
                if (fraction < 0.25f) {
                    crackTex = "assets/textures/entity/iron_golem/iron_golem_crackiness_high.png";
                } else if (fraction < 0.5f) {
                    crackTex = "assets/textures/entity/iron_golem/iron_golem_crackiness_medium.png";
                } else if (fraction < 0.75f) {
                    crackTex = "assets/textures/entity/iron_golem/iron_golem_crackiness_low.png";
                }
                if (crackTex && bodyIndexCount > 0) {
                    const TextureHandle tex = LoadTexture(crackTex);
                    if (tex != INVALID_TEXTURE) {
                        batches.push_back({ tex, overlay, firstIndex,
                                            bodyIndexCount });
                    }
                }

                // IronGolemFlowerLayer: the poppy block model seated in the
                // flower-holding (right) arm while offerFlowerTick != 0.
                // The pose-stack ops are MC's, verbatim, in BLOCK units —
                // entityMatrix maps model PIXELS, so a scale(16) folds the
                // chain back to MC's block-space stack.
                glm::mat4 arm;
                if (state.offerFlowerTick != 0.0f &&
                    model->RightHandMatrix(arm)) {
                    glm::mat4 m = entityMatrix * arm;
                    m = glm::scale(m, glm::vec3(16.0f));
                    m = glm::translate(m, glm::vec3(-1.1875f, 1.0625f, -0.9375f));
                    m = glm::translate(m, glm::vec3(0.5f, 0.5f, 0.5f));
                    m = glm::scale(m, glm::vec3(0.5f));
                    m = glm::rotate(m, glm::radians(-90.0f),
                                    glm::vec3(1.0f, 0.0f, 0.0f));
                    m = glm::translate(m, glm::vec3(-0.5f, -0.5f, -0.5f));
                    const TextureHandle tex =
                        LoadTexture("assets/textures/block/poppy.png");
                    if (tex != INVALID_TEXTURE) {
                        const size_t f = m_indices.size();
                        AppendCrossBlock(m, m_verts, m_indices);
                        // NO_OVERLAY, like MC's flower submit.
                        batches.push_back({ tex, glm::vec4(0.0f), f,
                                            m_indices.size() - f });
                    }
                }
            }

            // ── Mooshroom mushrooms — MC MushroomCowMushroomLayer:25-55 ───
            //
            // The mushroom BLOCK model (cross.json) three times: twice on
            // the back, once on the head, at MC's exact pose offsets, adults
            // only. The port has no mooshroom variant sync, so the RED
            // mushroom stands for every mooshroom (the def texture is the
            // red sheet too); a brown variant would swap to
            // brown_mushroom.png.
            if (type == Game::EntityTypeId::Mooshroom &&
                !state.isBaby) {
                const TextureHandle tex =
                    LoadTexture("assets/textures/block/red_mushroom.png");
                if (tex != INVALID_TEXTURE) {
                    const glm::mat4 block =
                        glm::scale(entityMatrix, glm::vec3(16.0f));
                    const auto putMushroom = [&](const glm::mat4& m) {
                        const size_t f = m_indices.size();
                        AppendCrossBlock(m, m_verts, m_indices);
                        // The mushrooms take the entity's overlay coords —
                        // a hurt mooshroom flashes red, hat included.
                        batches.push_back({ tex, overlay, f,
                                            m_indices.size() - f });
                    };
                    // Back mushroom 1.
                    glm::mat4 m = glm::translate(
                        block, glm::vec3(0.2f, -0.35f, 0.5f));
                    m = glm::rotate(m, glm::radians(-48.0f),
                                    glm::vec3(0.0f, 1.0f, 0.0f));
                    m = glm::scale(m, glm::vec3(-1.0f, -1.0f, 1.0f));
                    m = glm::translate(m, glm::vec3(-0.5f, -0.5f, -0.5f));
                    putMushroom(m);
                    // Back mushroom 2.
                    m = glm::translate(block, glm::vec3(0.2f, -0.35f, 0.5f));
                    m = glm::rotate(m, glm::radians(42.0f),
                                    glm::vec3(0.0f, 1.0f, 0.0f));
                    m = glm::translate(m, glm::vec3(0.1f, 0.0f, -0.6f));
                    m = glm::rotate(m, glm::radians(-48.0f),
                                    glm::vec3(0.0f, 1.0f, 0.0f));
                    m = glm::scale(m, glm::vec3(-1.0f, -1.0f, 1.0f));
                    m = glm::translate(m, glm::vec3(-0.5f, -0.5f, -0.5f));
                    putMushroom(m);
                    // Head mushroom — seated by the head part's transform
                    // (MC getHead().translateAndRotate), pixel chain folded
                    // to blocks the same way as the golem's arm.
                    glm::mat4 head(1.0f);
                    if (PartChainMatrix(model->Root(), "head", head)) {
                        m = entityMatrix * head;
                        m = glm::scale(m, glm::vec3(16.0f));
                        m = glm::translate(m, glm::vec3(0.0f, -0.7f, -0.2f));
                        m = glm::rotate(m, glm::radians(-78.0f),
                                        glm::vec3(0.0f, 1.0f, 0.0f));
                        m = glm::scale(m, glm::vec3(-1.0f, -1.0f, 1.0f));
                        m = glm::translate(m, glm::vec3(-0.5f, -0.5f, -0.5f));
                        putMushroom(m);
                    }
                }
            }

            // ── Snow golem pumpkin — MC SnowGolemHeadLayer ────────────────
            //
            // The carved-pumpkin BLOCK model seated by the head part's
            // transform: MC translates 0.34375 up (its Y points down),
            // turns it 180° so the carved face looks forward, scales by
            // 0.625 with the model's Y/Z flip, and centres the unit cell.
            // Only while it still wears one — shears take it off
            // (SnowGolem::Shear; the flag rides the variant byte).
            if (type == Game::EntityTypeId::SnowGolem &&
                static_cast<const Game::SnowGolem&>(mob).HasPumpkin()) {
                glm::mat4 head(1.0f);
                if (PartChainMatrix(model->Root(), "head", head)) {
                    glm::mat4 m = entityMatrix * head;
                    m = glm::scale(m, glm::vec3(16.0f));   // pixels -> blocks
                    m = glm::translate(m, glm::vec3(0.0f, -0.34375f, 0.0f));
                    m = glm::rotate(m, glm::radians(180.0f), glm::vec3(0.0f, 1.0f, 0.0f));
                    m = glm::scale(m, glm::vec3(0.625f, -0.625f, -0.625f));
                    m = glm::translate(m, glm::vec3(-0.5f, -0.5f, -0.5f));
                    const auto put = [&](int faces, const char* texPath) {
                        const TextureHandle tex = LoadTexture(texPath);
                        if (tex == INVALID_TEXTURE) return;
                        const size_t f = m_indices.size();
                        AppendUnitBlockFaces(m, faces, m_verts, m_indices);
                        // The pumpkin takes the entity's overlay coords —
                        // a hurt golem flashes red, hat included.
                        batches.push_back({ tex, overlay, f, m_indices.size() - f });
                    };
                    // block/orientable: top sheet above and below, the
                    // carved front on north, the plain side elsewhere.
                    put(kFaceUp | kFaceDown, "assets/textures/block/pumpkin_top.png");
                    put(kFaceNorth,          "assets/textures/block/carved_pumpkin.png");
                    put(kFaceSouth | kFaceWest | kFaceEast,
                        "assets/textures/block/pumpkin_side.png");
                }
            }

            // ── Sheep wool undercoat — MC SheepWoolUndercoatLayer ─────────
            //
            // The sheep BODY mesh again (LayerDefinitions'
            // SHEEP_WOOL_UNDERCOAT row IS sheepBodyLayer) with the undercoat
            // sheet, dyed by the wool colour — the short dyed coat a sheared
            // coloured sheep shows. MC draws it for every non-white sheep
            // (under the wool when not sheared); a tinted copy of the body's
            // own vertex range is the same geometry at the same pose, and
            // the later equal-depth draw wins under LessEqual. (The jeb
            // rainbow case is skipped with the rest of the jeb system.)
            // MC 26.1 SheepWoolUndercoatLayer skips babies (`!state.isBaby`)
            // — the baby sheet has no undercoat region — so the New look
            // does too; the classic baby keeps drawing it as it always has.
            if (type == Game::EntityTypeId::Sheep &&
                bodyIndexCount > 0 && !(state.isBaby && NewBabies())) {
                const auto* sheep = static_cast<const Game::Sheep*>(&mob);
                if ((sheep->GetColor() & 0x0F) != 0) {
                    const TextureHandle tex = LoadTexture(
                        "assets/textures/entity/sheep/sheep_wool_undercoat.png");
                    if (tex != INVALID_TEXTURE) {
                        const SheepWoolColor tint =
                            kSheepWoolColors[sheep->GetColor() & 0x0F];
                        const size_t f = m_indices.size();
                        AppendTintedCopy(m_verts, m_indices,
                                         firstVert, bodyVertCount,
                                         firstIndex, bodyIndexCount,
                                         tint.r, tint.g, tint.b);
                        batches.push_back({ tex, overlay, f,
                                            m_indices.size() - f });
                    }
                }
            }

            // ── Held items — MC ItemInHandLayer ───────────────────────────
            //
            // Each is its own texture, so its own batch. MC draws the held
            // item with NO_OVERLAY — a skeleton flashing red does not take
            // its bow with it — so the overlay is deliberately zero. Display
            // blocks are the item JSONs' thirdperson_righthand, verbatim.
            {
                // models/item/bow.json.
                static constexpr DisplaySpec kBowSpec{
                    {-1.0f, -2.0f, 2.5f}, {-80.0f, 260.0f, -40.0f}, 0.9f};
                // models/item/handheld.json — swords, axes.
                static constexpr DisplaySpec kHandheldSpec{
                    {0.0f, 4.0f, 0.5f}, {0.0f, -90.0f, 55.0f}, 0.85f};
                // models/item/crossbow.json.
                static constexpr DisplaySpec kCrossbowSpec{
                    {2.0f, 0.1f, -3.0f}, {-90.0f, 0.0f, -60.0f}, 0.9f};

                const char* heldSprite = nullptr;
                const DisplaySpec* heldSpec = nullptr;
                bool heldTrident = false;
                switch (type) {
                    // The bow-armed skeleton family plus the illusioner.
                    case Game::EntityTypeId::Skeleton:
                    case Game::EntityTypeId::Stray:
                    case Game::EntityTypeId::Bogged:
                    case Game::EntityTypeId::Parched:
                    case Game::EntityTypeId::Illusioner:
                        heldSprite = "bow";
                        heldSpec = &kBowSpec;
                        break;
                    case Game::EntityTypeId::WitherSkeleton:
                        heldSprite = "stone_sword";
                        heldSpec = &kHandheldSpec;
                        break;
                    case Game::EntityTypeId::Pillager:
                        // The uncharged crossbow sprite — charge states wait
                        // on a mob item-use clock.
                        heldSprite = "crossbow_standby";
                        heldSpec = &kCrossbowSpec;
                        break;
                    case Game::EntityTypeId::Vindicator:
                        // MC hides the axe while the arms are crossed; it
                        // shows the moment the pose turns ATTACKING.
                        if (state.isAggressive) {
                            heldSprite = "iron_axe";
                            heldSpec = &kHandheldSpec;
                        }
                        break;
                    case Game::EntityTypeId::Drowned:
                        heldTrident =
                            static_cast<const Game::Drowned&>(mob).HasTrident();
                        break;
                    default:
                        break;
                }

                if (heldSprite) {
                    const size_t itemFirst = m_indices.size();
                    const TextureHandle tex = AppendHeldSprite(
                        *model, entityMatrix, heldSprite,
                        *heldSpec, m_verts, m_indices);
                    if (tex != INVALID_TEXTURE && m_indices.size() > itemFirst) {
                        batches.push_back({ tex, glm::vec4(0.0f), itemFirst,
                                            m_indices.size() - itemFirst });
                    }
                } else if (heldTrident) {
                    const size_t itemFirst = m_indices.size();
                    const TextureHandle tex = AppendHeldTrident(
                        *model, entityMatrix, m_verts, m_indices);
                    if (tex != INVALID_TEXTURE && m_indices.size() > itemFirst) {
                        batches.push_back({ tex, glm::vec4(0.0f), itemFirst,
                                            m_indices.size() - itemFirst });
                    }
                }
            }

            // ── Second-model overlays over the same skeleton ──────────────
            //
            // Sheep wool (MC SheepWoolLayer, dyed), the drowned's outer
            // layer (DrownedOuterLayer) and the stray/bogged clothing
            // (SkeletonClothingLayer) — each is MC's
            // (colored)CutoutModelCopyLayerRender: a second mesh posed by
            // the same state. For a baby this is the babyOverlayModel
            // instance (MC's *_BABY layer rows), selected above.
            if (overlayModel) {
                const char* overlayTexPath = nullptr;
                bool tinted = false;
                SheepWoolColor tint{ 255, 255, 255 };
                switch (type) {
                    case Game::EntityTypeId::Sheep: {
                        const auto* sheep =
                            static_cast<const Game::Sheep*>(&mob);
                        if (!sheep->IsSheared()) {
                            overlayTexPath =
                                "assets/textures/entity/sheep/sheep_wool.png";
                            tinted = true;
                            tint = kSheepWoolColors[sheep->GetColor() & 0x0F];
                        }
                        break;
                    }
                    // MC DrownedOuterLayer — unconditional, untinted.
                    case Game::EntityTypeId::Drowned:
                        overlayTexPath =
                            "assets/textures/entity/zombie/drowned_outer_layer.png";
                        break;
                    // MC SkeletonClothingLayer, per renderer.
                    case Game::EntityTypeId::Stray:
                        overlayTexPath =
                            "assets/textures/entity/skeleton/stray_overlay.png";
                        break;
                    case Game::EntityTypeId::Bogged:
                        overlayTexPath =
                            "assets/textures/entity/skeleton/bogged_overlay.png";
                        break;
                    default:
                        break;
                }
                if (overlayTexPath) {
                    const size_t layerFirst = m_indices.size();
                    const size_t layerVertFirst = m_verts.size();
                    AppendMob(*overlayModel, state, renderPos,
                              bodyRot, cameraPos, m_verts, m_indices);

                    if (m_indices.size() > layerFirst) {
                        // MC passes the dye as the model's VERTEX COLOUR, which
                        // the entity shader multiplies with the texture. Doing
                        // it on the vertices here (rather than through the
                        // overlay uniform) keeps two things right that a flat
                        // replacement destroyed: the wool sheet's own shading
                        // survives, and the per-face directional shade that
                        // ModelPart::Build already baked in survives with it —
                        // so a dyed sheep is still lit, not a solid blob.
                        if (tinted) {
                            for (size_t i = layerVertFirst; i < m_verts.size(); ++i) {
                                ModelVertex& v = m_verts[i];
                                v.r = static_cast<uint8_t>((v.r * tint.r) / 255);
                                v.g = static_cast<uint8_t>((v.g * tint.g) / 255);
                                v.b = static_cast<uint8_t>((v.b * tint.b) / 255);
                            }
                        }

                        // The SAME overlay as the body. MC hands every render
                        // layer the entity's overlayCoords, so a hurt sheep
                        // flashes red all over rather than only on the skin.
                        batches.push_back({ MobTex(overlayTexPath),
                                            overlay, layerFirst,
                                            m_indices.size() - layerFirst,
                                            false, overlayModel->CullBackFaces() });
                    }
                }
            }

            // Tamed collar — MC Wolf/CatCollarLayer: the SAME model drawn
            // again with the collar sheet, tinted by the dye (default RED —
            // no collar dyeing yet, so every collar is the default). The
            // collar textures only paint the neck band; the rest of the
            // sheet is transparent, so re-drawing the whole skeleton is
            // exactly MC's renderColoredCutoutModel. `model` is the baby
            // instance for a pup/kitten, which is MC's WOLF_BABY_ARMOR /
            // CAT_BABY_COLLAR — the same mesh through the baby transform.
            if (type == Game::EntityTypeId::Wolf ||
                type == Game::EntityTypeId::Cat) {
                const Game::TamableAnimal* tamable = TamableOf(mob, type);
                if (tamable && tamable->IsTame()) {
                    const size_t collarFirst = m_indices.size();
                    const size_t collarVertFirst = m_verts.size();
                    AppendMob(*model, state, renderPos,
                              bodyRot, cameraPos, m_verts, m_indices);
                    if (m_indices.size() > collarFirst) {
                        // DyeColor.RED — the same floored row the sheep table
                        // carries.
                        const SheepWoolColor tint = kSheepWoolColors[14];
                        for (size_t i = collarVertFirst; i < m_verts.size(); ++i) {
                            ModelVertex& v = m_verts[i];
                            v.r = static_cast<uint8_t>((v.r * tint.r) / 255);
                            v.g = static_cast<uint8_t>((v.g * tint.g) / 255);
                            v.b = static_cast<uint8_t>((v.b * tint.b) / 255);
                        }
                        const char* collarTex =
                            type == Game::EntityTypeId::Wolf
                                ? "assets/textures/entity/wolf/wolf_collar.png"
                                : "assets/textures/entity/cat/cat_collar.png";
                        batches.push_back({ MobTex(collarTex), overlay,
                                            collarFirst,
                                            m_indices.size() - collarFirst,
                                            false, model->CullBackFaces() });
                    }
                }
            }

            // ── Fire — MC EntityRenderDispatcher:159-161 + FlameFeature-
            // Renderer.renderFlame: the stacked fire quads over any entity
            // with displayFireAnimation (isOnFire && !spectator). The
            // on-fire flag rides the wire (ClientMobManager's kFlagOnFire →
            // SetRemainingFireTicks). Sprite projectiles `continue` before
            // this point — none of them burn visibly, which matches how
            // rarely MC shows one on fire. The two fire sheets are 32-frame
            // strips animated per the .mcmeta frame order [16..31, 0..15]
            // at 1 tick/frame; MC advances them on a GLOBAL atlas clock —
            // the mob's own tickCount stands in (same flicker, per-mob
            // phase), as the renderer has no world clock.
            if (mob.IsOnFire()) {
                const int frame = (mob.tickCount + 16) % 32;
                // MC sizes the flame from the render state's bounding box —
                // the entity's own dimensions here.
                const float bbWidth  = mob.GetBbWidth();
                const float bbHeight = mob.GetBbHeight();
                const TextureHandle fire0 =
                    LoadTexture("assets/textures/block/fire_0.png");
                const TextureHandle fire1 =
                    LoadTexture("assets/textures/block/fire_1.png");
                if (fire0 != INVALID_TEXTURE && fire1 != INVALID_TEXTURE) {
                    size_t f = m_indices.size();
                    AppendFlame(false, renderPos, cameraPos, bbWidth, bbHeight,
                                frame, flameYaw, m_verts, m_indices);
                    if (m_indices.size() > f) {
                        batches.push_back({ fire0, glm::vec4(0.0f), f,
                                            m_indices.size() - f });
                    }
                    f = m_indices.size();
                    AppendFlame(true, renderPos, cameraPos, bbWidth, bbHeight,
                                frame, flameYaw, m_verts, m_indices);
                    if (m_indices.size() > f) {
                        batches.push_back({ fire1, glm::vec4(0.0f), f,
                                            m_indices.size() - f });
                    }
                }
            }

            // Buffer guard — against what is LEFT of this frame's set.
            if (m_verts.size() + 4096 > vertRoom || m_indices.size() + 8192 > idxRoom) break;
        }

        PROFILE_PLOT("Mobs/Culled", static_cast<int64_t>(culledCount));
        PROFILE_PLOT("Mobs/Rendered",
                     static_cast<int64_t>(mobs.ModelMobList().size()) - culledCount);
        if (m_indices.empty()) return;
        // The loop guard leaves a 4k-vertex margin, but one mob with every
        // layer (a warden, a dragon) can be larger than that; never write
        // past the set.
        if (m_verts.size() > vertRoom || m_indices.size() > idxRoom) return;

        // Indices were built against this call's own vertex list; rebase
        // them onto the set when an earlier call this frame already filled
        // the front of it (DrawIndexed has no base-vertex parameter).
        if (m_vertCursor > 0) {
            const auto base = static_cast<uint32_t>(m_vertCursor);
            for (uint32_t& i : m_indices) i += base;
        }
        g_renderBackend->UpdateBuffer(fb.vb, m_vertCursor * sizeof(ModelVertex),
                                      m_verts.size() * sizeof(ModelVertex), m_verts.data());
        g_renderBackend->UpdateBuffer(fb.ib, m_idxCursor * sizeof(uint32_t),
                                      m_indices.size() * sizeof(uint32_t), m_indices.data());
        const size_t firstIndexThisCall = m_idxCursor;
        m_vertCursor += m_verts.size();
        m_idxCursor  += m_indices.size();

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
        // ...except for the models MC itself draws culled (Batch::cull —
        // the bat, arrow, trident, 26.3's baby turtle), whose batches flip
        // the mode below.
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
        // Portal clip plane (world space) for camera-relative vertices:
        // with p = world - c, n·world + w = n·p + (w + n·c). Zero when no
        // portal view is active.
        {
            glm::vec4 plane = ::Render::ChunkRenderer::PortalEntityClipPlane();
            if (plane.x != 0.0f || plane.y != 0.0f || plane.z != 0.0f) {
                plane.w += glm::dot(glm::vec3(plane), cameraPos);
            }
            g_renderBackend->SetUniformVec4(m_shader, "uEntityClipPlane", plane);
        }

        bool blendOn = false;
        bool cullOn = false;
        for (const Batch& batch : batches) {
            // The warden's fading emissive layers draw alpha-blended (MC
            // RenderTypes.entityTranslucentEmissive — SrcAlpha /
            // OneMinusSrcAlpha, the PipelineState defaults); everything else
            // stays cutout. Depth write stays on either way — the layers sit
            // ON the body surface and batches draw in mob order.
            // Culling follows the model's MC render type the same way.
            if (batch.blend != blendOn || batch.cull != cullOn) {
                blendOn = batch.blend;
                cullOn = batch.cull;
                pipeline.blendEnabled = blendOn;
                pipeline.cullMode = cullOn ? CullMode::Back : CullMode::None;
                g_renderBackend->SetPipelineState(pipeline);
            }
            g_renderBackend->BindTexture(batch.texture, 0);
            g_renderBackend->SetUniformVec4(m_shader, "uColor", batch.overlay);
            // indexOffset is in INDICES, not bytes — see DrawIndexed's
            // signature. Passing a byte offset draws from the wrong place with
            // no error, which is the classic way to get one mob's geometry
            // wearing another's texture.
            g_renderBackend->DrawIndexed(fb.mesh,
                                         static_cast<uint32_t>(batch.indexCount),
                                         static_cast<uint32_t>(firstIndexThisCall + batch.firstIndex));
        }

        g_renderBackend->UnbindMesh();
    }

} // namespace Render
