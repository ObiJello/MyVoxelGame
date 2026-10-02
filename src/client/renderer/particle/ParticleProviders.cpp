// File: src/client/renderer/particle/ParticleProviders.cpp
//
// MC ParticleResources.registerProviders and every provider's particle
// constructor chain (client/particle/*.java), constants verbatim. Each case
// builds the particle exactly as its Java constructors would, in their order:
//
//   Particle(level, x, y, z)                 base()      — box 0.2, lifetime
//                                                          4 / (r·0.9 + 0.1)
//   Particle(level, x, y, z, xa, ya, za)     baseVel()   — jittered, normalised,
//                                                          rescaled velocity
//   SingleQuadParticle(...)                  quad()      — quadSize
//                                                          0.1·(r·0.5 + 0.5)·2
//
// then the subclass bodies and the provider's own tweaks. A provider that
// returns null (a block particle of air, an item with no sprite) returns false.

#include "MobParticleSystem.hpp"

#include "../mesh/BlockTint.hpp"
#include "../mesh/Mesher.hpp"
#include "../texture/AtlasBuilder.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/entity/Item.hpp"
#include "common/world/biome/Biomes.hpp"
#include "common/world/block/BlockModel.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/block/FallingBlock.hpp"
#include "common/world/block/RedstoneWire.hpp"
#include "common/world/chunk/IBlockAccess.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <optional>

namespace Render {

    namespace {

        using K = Game::ParticleKind;

        // ── Sprites on the block / item atlases ───────────────────────────

        // MC BlockStateModelSet.getParticleMaterial(state).sprite(): the
        // state model's `particle` texture, resolved like MaterialBaker
        // (item atlas, then blocks).
        bool BlockParticleSprite(Game::BlockState state, AtlasSprite& out) {
            const Game::BlockModel& model = Game::BlockRegistry::GetBlockModel(state);
            const std::string tex = model.ResolveTexture("particle");
            if (tex != "missingno" && FindSprite(tex, out)) return true;
            // A model with no particle slot: its first face's texture.
            for (const auto& element : model.elements) {
                for (const auto& [dir, face] : element.faces) {
                    (void)dir;
                    const std::string t = model.ResolveTexture(face.textureRef);
                    if (t != "missingno" && FindSprite(t, out)) return true;
                }
            }
            return false;
        }

        // MC ItemParticleProvider.getSprite: the stack's model's particle
        // material — a block item's block model particle texture, a flat
        // item's layer0.
        bool ItemParticleSprite(Game::ItemID itemId, AtlasSprite& out) {
            if (itemId == Game::Items::Air) return false;
            const Game::Item& item = Game::ItemRegistry::Get(itemId);
            if (item.renderType == Game::ItemRenderType::Block) {
                if (!item.blockModelOverride.empty() &&
                    Game::BlockModelRegistry::HasModel(item.blockModelOverride)) {
                    const Game::BlockModel& model = Game::BlockModelRegistry::GetModel(item.blockModelOverride);
                    const std::string tex = model.ResolveTexture("particle");
                    if (tex != "missingno" && FindSprite(tex, out)) return true;
                }
                if (BlockParticleSprite(Game::BlockStates::Default(item.blockId), out)) return true;
            }
            const std::string& name = !item.spriteName.empty() ? item.spriteName
                                    : (!item.spriteLayers.empty() ? item.spriteLayers.front() : item.spriteName);
            if (name.empty()) return false;
            if (name.find('/') != std::string::npos && FindSprite(name, out)) return true;
            return FindSprite("item/" + name, out) || FindSprite("block/" + name, out);
        }

        // MC BlockColors.getTintSource(state, 0).colorAsTerrainParticle(state,
        // level, pos) — BlockTint's table, the one the section mesher tints
        // with; empty when the block registers no tint source.
        std::optional<uint32_t> TerrainParticleTint(Game::BlockState state, const Game::IBlockAccess* blocks,
                                                    const glm::ivec3& pos) {
            return BlockTint::Layer0Color(state, pos.x, pos.y, pos.z, Mesher::GetMeshOptions().biomeBlendRadius,
                                          /*asTerrainParticle=*/true, [blocks](int x, int y, int z) {
                                              return blocks ? blocks->GetBiome(x, y, z) : Game::kFallbackBiomeId;
                                          });
        }

        // RenderShape.INVISIBLE (air aside): the blocks drawn by nothing.
        bool IsInvisibleRenderShape(Game::BlockID id) {
            return id == Game::BlockID::Barrier || id == Game::BlockID::StructureVoid ||
                   id == Game::BlockID::Light || id == Game::BlockID::MovingPiston;
        }

        // TerrainParticle.createTerrainParticle's guard.
        bool SpawnsTerrainParticles(Game::BlockState state) {
            const Game::BlockID id = state.Block();
            return id != Game::BlockID::Air && id != Game::BlockID::MovingPiston &&
                   id != Game::BlockID::Barrier && id != Game::BlockID::StructureVoid;
        }

        // Mth.nextFloat(random, min, max).
        float NextFloat(Game::JavaRandom& r, float min, float max) {
            return min >= max ? min : r.NextFloat() * (max - min) + min;
        }

        glm::vec3 Rgb(uint32_t rgb) {
            return glm::vec3(static_cast<float>((rgb >> 16) & 255) / 255.0f,
                             static_cast<float>((rgb >> 8) & 255) / 255.0f,
                             static_cast<float>(rgb & 255) / 255.0f);
        }

    } // namespace

    int MobParticleSystem::SetOf(Game::ParticleKind kind) const {
        const size_t i = static_cast<size_t>(kind);
        if (m_setByKind.size() <= i) m_setByKind.resize(i + 1, -2);
        if (m_setByKind[i] != -2) return m_setByKind[i];
        int set = -1;
        switch (kind) {
            // The engine's own kinds borrow vanilla sets.
            case K::HushPortal:
            case K::AetherPortal:
            case K::HushMist:
                set = m_sprites.SetIndex("portal");
                break;
            case K::HushMote:
            case K::VesperGlint:
                set = m_sprites.SetIndex("happy_villager");   // the glint
                break;
            default: {
                const char* name = Game::ParticleTypes::Name(kind);
                if (std::string_view(name).rfind("minecraft:", 0) == 0) set = m_sprites.SetIndex(name);
                break;
            }
        }
        m_setByKind[i] = static_cast<int16_t>(set);
        return set;
    }

    bool MobParticleSystem::MakeParticle(Particle& p, const Client::ClientLevelBridge::QueuedParticle& q,
                                         const Game::ParticleOptions& o, const Game::IBlockAccess* blocks) {
        Game::JavaRandom& rng = m_rng;
        p.kind = q.kind;
        const K kind = q.kind;
        const int set = SetOf(kind);

        // ── Particle / SingleQuadParticle constructor pieces ──────────────
        const auto base = [&](double x, double y, double z) {
            p.x = x; p.y = y; p.z = z;
            p.xo = x; p.yo = y; p.zo = z;
            SetSize(p, 0.2f, 0.2f);
            p.lifetime = static_cast<int>(4.0f / (rng.NextFloat() * 0.9f + 0.1f));
        };
        const auto baseVel = [&](double xa, double ya, double za) {
            p.xd = xa + static_cast<double>((rng.NextFloat() * 2.0f - 1.0f) * 0.4f);
            p.yd = ya + static_cast<double>((rng.NextFloat() * 2.0f - 1.0f) * 0.4f);
            p.zd = za + static_cast<double>((rng.NextFloat() * 2.0f - 1.0f) * 0.4f);
            const double speed = static_cast<double>((rng.NextFloat() + rng.NextFloat() + 1.0f) * 0.15f);
            const double dd = std::sqrt(p.xd * p.xd + p.yd * p.yd + p.zd * p.zd);
            p.xd = p.xd / dd * speed * 0.4000000059604645;
            p.yd = p.yd / dd * speed * 0.4000000059604645 + 0.10000000149011612;
            p.zd = p.zd / dd * speed * 0.4000000059604645;
        };
        const auto quad = [&]() { p.quadSize = 0.1f * (rng.NextFloat() * 0.5f + 0.5f) * 2.0f; };
        const auto quad4 = [&](double x, double y, double z) { base(x, y, z); quad(); };
        const auto quad7 = [&](double x, double y, double z, double xa, double ya, double za) {
            base(x, y, z); baseVel(xa, ya, za); quad();
        };
        // setSpriteFromAge(sprites): walk the set by age.
        const auto walk = [&](int s) { p.spriteSet = static_cast<int16_t>(s); p.walk = true; };
        // sprite.get(random) — one frame for life.
        const auto pick = [&](int s) { p.spriteSet = static_cast<int16_t>(s); p.walk = false; p.sprite = m_sprites.Random(s, rng); };
        // sprites.first().
        const auto first = [&](int s) { p.spriteSet = static_cast<int16_t>(s); p.walk = false; p.sprite = m_sprites.First(s); };
        const auto color = [&](float r, float g, float b) { p.rCol = r; p.gCol = g; p.bCol = b; };
        const auto rgbColor = [&](uint32_t rgb) { const glm::vec3 c = Rgb(rgb); color(c.r, c.g, c.b); };

        // BaseAshSmokeParticle(level, x, y, z, dirX, dirY, dirZ, xa, ya, za,
        // scale, sprites, colorRandom, maxLifetime, gravity, hasPhysics).
        const auto ashSmoke = [&](double x, double y, double z, float dirX, float dirY, float dirZ,
                                  double xa, double ya, double za, float scale, float colorRandom,
                                  int maxLifetime, float gravity, bool hasPhysics) {
            quad7(x, y, z, 0.0, 0.0, 0.0);
            p.friction = 0.96f;
            p.gravity = gravity;
            p.speedUpWhenYMotionIsBlocked = true;
            walk(set);
            p.xd *= static_cast<double>(dirX); p.yd *= static_cast<double>(dirY); p.zd *= static_cast<double>(dirZ);
            p.xd += xa; p.yd += ya; p.zd += za;
            const float col = rng.NextFloat() * colorRandom;
            color(col, col, col);
            p.quadSize *= 0.75f * scale;
            p.lifetime = static_cast<int>(static_cast<double>(maxLifetime) /
                                          (static_cast<double>(rng.NextFloat()) * 0.8 + 0.2) *
                                          static_cast<double>(scale));
            p.lifetime = std::max(p.lifetime, 1);
            p.hasPhysics = hasPhysics;
        };

        // SpellParticle(level, x, y, z, xa, ya, za, sprites): its constructor
        // draws the horizontal push from a static RandomSource of its own.
        const auto spell = [&](double x, double y, double z, double xa, double ya, double za) {
            const double ax = 0.5 - rng.NextDouble();
            const double az = 0.5 - rng.NextDouble();
            quad7(x, y, z, ax, ya, az);
            p.friction = 0.96f;
            p.gravity = -0.1f;
            p.speedUpWhenYMotionIsBlocked = true;
            p.yd *= 0.20000000298023224;
            if (xa == 0.0 && za == 0.0) {
                p.xd *= 0.10000000149011612;
                p.zd *= 0.10000000149011612;
            }
            p.quadSize *= 0.75f;
            p.lifetime = static_cast<int>(8.0 / (static_cast<double>(rng.NextFloat()) * 0.8 + 0.2));
            p.hasPhysics = false;
            walk(set);
            p.translucent = true;
            p.originalAlpha = 1.0f;
        };

        // RisingParticle(level, x, y, z, xd, yd, zd, sprite).
        const auto rising = [&](double x, double y, double z, double xd, double yd, double zd) {
            quad7(x, y, z, xd, yd, zd);
            p.friction = 0.96f;
            p.xd = p.xd * 0.009999999776482582 + xd;
            p.yd = p.yd * 0.009999999776482582 + yd;
            p.zd = p.zd * 0.009999999776482582 + zd;
            p.x += static_cast<double>((rng.NextFloat() - rng.NextFloat()) * 0.05f);
            p.y += static_cast<double>((rng.NextFloat() - rng.NextFloat()) * 0.05f);
            p.z += static_cast<double>((rng.NextFloat() - rng.NextFloat()) * 0.05f);
            p.xo = p.x; p.yo = p.y; p.zo = p.z;
            p.lifetime = static_cast<int>(8.0 / (static_cast<double>(rng.NextFloat()) * 0.8 + 0.2)) + 4;
        };

        // SimpleAnimatedParticle(level, x, y, z, sprites, gravity).
        const auto animated = [&](double x, double y, double z, float gravity) {
            quad4(x, y, z);
            first(set);
            p.friction = 0.91f;
            p.gravity = gravity;
            p.translucent = true;
            p.light = Light::FullBright;
        };
        const auto fadeColor = [&](uint32_t rgb) {
            const glm::vec3 c = Rgb(rgb);
            p.fadeR = c.r; p.fadeG = c.g; p.fadeB = c.b;
            p.hasFade = true;
        };

        // SuspendedTownParticle(level, x, y, z, xa, ya, za, sprite).
        const auto suspendedTown = [&](double x, double y, double z, double xa, double ya, double za) {
            pick(set);
            quad7(x, y, z, xa, ya, za);
            const float br = rng.NextFloat() * 0.1f + 0.2f;
            color(br, br, br);
            SetSize(p, 0.02f, 0.02f);
            p.quadSize *= rng.NextFloat() * 0.6f + 0.5f;
            p.xd *= 0.019999999552965164; p.yd *= 0.019999999552965164; p.zd *= 0.019999999552965164;
            p.lifetime = static_cast<int>(20.0 / (static_cast<double>(rng.NextFloat()) * 0.8 + 0.2));
            p.freeMove = true;
        };

        // SuspendedParticle (4-arg / 7-arg): y - 0.125.
        const auto suspended4 = [&](double x, double y, double z) {
            pick(set);
            quad4(x, y - 0.125, z);
            SetSize(p, 0.01f, 0.01f);
            p.quadSize *= rng.NextFloat() * 0.6f + 0.2f;
            p.lifetime = static_cast<int>(16.0 / (static_cast<double>(rng.NextFloat()) * 0.8 + 0.2));
            p.hasPhysics = false;
            p.friction = 1.0f;
            p.gravity = 0.0f;
        };
        const auto suspended7 = [&](double x, double y, double z, double xd, double yd, double zd) {
            pick(set);
            quad7(x, y - 0.125, z, xd, yd, zd);
            SetSize(p, 0.01f, 0.01f);
            p.quadSize *= rng.NextFloat() * 0.6f + 0.6f;
            p.lifetime = static_cast<int>(16.0 / (static_cast<double>(rng.NextFloat()) * 0.8 + 0.2));
            p.hasPhysics = false;
            p.friction = 1.0f;
            p.gravity = 0.0f;
        };

        // DripParticle(level, x, y, z, fluid, sprite).
        const auto drip = [&](double x, double y, double z, DripFluid fluid, Drip variant) {
            pick(set);
            quad4(x, y, z);
            SetSize(p, 0.01f, 0.01f);
            p.gravity = 0.06f;
            p.dripFluid = fluid;
            p.drip = variant;
        };
        const auto dripHang = [&](double x, double y, double z, DripFluid fluid, K falling) {
            drip(x, y, z, fluid, Drip::Hang);
            p.chainKind = falling;
            p.gravity *= 0.02f;
            p.lifetime = 40;
        };
        const auto fallAndLand = [&](double x, double y, double z, DripFluid fluid, K land, Drip variant) {
            drip(x, y, z, fluid, variant);
            p.lifetime = static_cast<int>(64.0 / (static_cast<double>(rng.NextFloat()) * 0.8 + 0.2));
            p.chainKind = land;
        };
        const auto dripLand = [&](double x, double y, double z, DripFluid fluid) {
            drip(x, y, z, fluid, Drip::Land);
            p.lifetime = static_cast<int>(16.0 / (static_cast<double>(rng.NextFloat()) * 0.8 + 0.2));
        };

        // FallingParticle (the falling leaves).
        const auto fallingLeaves = [&](double x, double y, double z, float fallAcceleration, float sideAcceleration,
                                       bool swirl, bool flowAway, float scale, float startVelocity) {
            pick(set);
            quad4(x, y, z);
            p.rotSpeed = static_cast<float>((rng.NextBool() ? -30.0 : 30.0) * 3.141592653589793 / 180.0);
            p.spinAcceleration = static_cast<float>((rng.NextBool() ? -5.0 : 5.0) * 3.141592653589793 / 180.0);
            p.windBig = sideAcceleration;
            p.swirl = swirl;
            p.flowAway = flowAway;
            p.lifetime = 300;
            p.gravity = fallAcceleration * 1.2f * 0.0025f;
            const float size = scale * (rng.NextBool() ? 0.05f : 0.075f);
            p.quadSize = size;
            SetSize(p, size, size);
            p.friction = 1.0f;
            p.yd = static_cast<double>(-startVelocity);
            const float particleRandom = rng.NextFloat();
            const double angle = static_cast<double>(particleRandom * 60.0f) * 3.141592653589793 / 180.0;
            p.xaFlowScale = std::cos(angle) * static_cast<double>(p.windBig);
            p.zaFlowScale = std::sin(angle) * static_cast<double>(p.windBig);
            p.swirlPeriod = static_cast<double>(1000.0f + particleRandom * 3000.0f) * 3.141592653589793 / 180.0;
        };

        // WaterDropParticle(level, x, y, z, sprite).
        const auto waterDrop = [&](double x, double y, double z) {
            pick(set);
            quad7(x, y, z, 0.0, 0.0, 0.0);
            p.xd *= 0.30000001192092896;
            p.yd = static_cast<double>(rng.NextFloat() * 0.2f + 0.1f);
            p.zd *= 0.30000001192092896;
            SetSize(p, 0.01f, 0.01f);
            p.gravity = 0.06f;
            p.lifetime = static_cast<int>(8.0 / (static_cast<double>(rng.NextFloat()) * 0.8 + 0.2));
        };

        // GlowParticle(level, x, y, z, xa, ya, za, sprites).
        const auto glow = [&](double x, double y, double z, double xa, double ya, double za) {
            quad7(x, y, z, xa, ya, za);
            p.friction = 0.96f;
            p.speedUpWhenYMotionIsBlocked = true;
            p.quadSize *= 0.75f;
            p.hasPhysics = false;
            walk(set);
            p.light = Light::AgeEmission;
        };

        // FlyTowardsPositionParticle.
        const auto flyTowards = [&](double x, double y, double z, double xd, double yd, double zd, bool glowing,
                                    float laStart, float laEnd, float laStartAt, float laEndAt) {
            pick(set);
            quad4(x, y, z);
            p.laStart = laStart; p.laEnd = laEnd; p.laStartAt = laStartAt; p.laEndAt = laEndAt;
            p.alpha = laStart;
            p.xd = xd; p.yd = yd; p.zd = zd;
            p.xStart = x; p.yStart = y; p.zStart = z;
            p.xo = x + xd; p.yo = y + yd; p.zo = z + zd;
            p.x = p.xo; p.y = p.yo; p.z = p.zo;
            p.quadSize = 0.1f * (rng.NextFloat() * 0.5f + 0.2f);
            const float br = rng.NextFloat() * 0.6f + 0.4f;
            color(0.9f * br, 0.9f * br, br);
            p.hasPhysics = false;
            p.lifetime = static_cast<int>(rng.NextFloat() * 10.0f) + 30;
            p.freeMove = true;
            p.light = glowing ? Light::BlockFull : Light::AgeEmissionPow4;
            p.translucent = !(laStart >= 1.0f && laEnd >= 1.0f);
        };

        // TerrainParticle(level, x, y, z, xa, ya, za, state[, pos]).
        const auto terrain = [&](double x, double y, double z, double xa, double ya, double za,
                                 Game::BlockState state) -> bool {
            if (!SpawnsTerrainParticles(state)) return false;
            AtlasSprite sprite;
            if (!BlockParticleSprite(state, sprite)) return false;
            quad7(x, y, z, xa, ya, za);
            p.gravity = 1.0f;
            color(0.6f, 0.6f, 0.6f);
            // The constructor's pos: the caller's block, else containing(x, y, z).
            const glm::ivec3 pos = q.hasBlockPos
                ? q.blockPos
                : glm::ivec3(static_cast<int>(std::floor(x)), static_cast<int>(std::floor(y)),
                             static_cast<int>(std::floor(z)));
            if (const std::optional<uint32_t> tint = TerrainParticleTint(state, blocks, pos)) {
                const glm::vec3 c = Rgb(*tint);
                p.rCol *= c.r; p.gCol *= c.g; p.bCol *= c.b;
            }
            p.quadSize /= 2.0f;
            const float uo = rng.NextFloat() * 3.0f;
            const float vo = rng.NextFloat() * 3.0f;
            // getU0 = sprite.getU((uo + 1) / 4), getU1 = getU(uo / 4),
            // getV0 = getV(vo / 4), getV1 = getV((vo + 1) / 4).
            const float su = sprite.rect.uvMax.x - sprite.rect.uvMin.x;
            const float sv = sprite.rect.uvMax.y - sprite.rect.uvMin.y;
            p.u0 = sprite.rect.uvMin.x + su * ((uo + 1.0f) / 4.0f);
            p.u1 = sprite.rect.uvMin.x + su * (uo / 4.0f);
            p.v0 = sprite.rect.uvMin.y + sv * (vo / 4.0f);
            p.v1 = sprite.rect.uvMin.y + sv * ((vo + 1.0f) / 4.0f);
            p.sheet = sprite.atlas == AtlasId::Items ? Sheet::Items : Sheet::Blocks;
            // Layer.bySprite: a translucent sprite draws in the translucent
            // layer (stained glass, ice, slime, honey).
            p.translucent = Game::BlockRegistry::Get(state.Block()).renderLayer == Game::RenderLayer::Translucent;
            return true;
        };

        // BreakingItemParticle(level, x, y, z, sprite) — the protected base.
        const auto breakingItem = [&](double x, double y, double z, const AtlasSprite& sprite) {
            quad7(x, y, z, 0.0, 0.0, 0.0);
            p.gravity = 1.0f;
            p.quadSize /= 2.0f;
            const float uo = rng.NextFloat() * 3.0f;
            const float vo = rng.NextFloat() * 3.0f;
            const float su = sprite.rect.uvMax.x - sprite.rect.uvMin.x;
            const float sv = sprite.rect.uvMax.y - sprite.rect.uvMin.y;
            p.u0 = sprite.rect.uvMin.x + su * ((uo + 1.0f) / 4.0f);
            p.u1 = sprite.rect.uvMin.x + su * (uo / 4.0f);
            p.v0 = sprite.rect.uvMin.y + sv * (vo / 4.0f);
            p.v1 = sprite.rect.uvMin.y + sv * ((vo + 1.0f) / 4.0f);
            p.sheet = sprite.atlas == AtlasId::Items ? Sheet::Items : Sheet::Blocks;
        };

        const double x = q.x, y = q.y, z = q.z;
        const double xa = q.vx, ya = q.vy, za = q.vz;

        switch (kind) {
            // ── Hearts ───────────────────────────────────────────────────
            case K::Heart:
            case K::AngryVillager: {
                // HeartParticle; AngryVillagerProvider spawns at y + 0.5.
                pick(set);
                quad7(x, kind == K::AngryVillager ? y + 0.5 : y, z, 0.0, 0.0, 0.0);
                p.speedUpWhenYMotionIsBlocked = true;
                p.friction = 0.86f;
                p.xd *= 0.009999999776482582; p.yd *= 0.009999999776482582; p.zd *= 0.009999999776482582;
                p.yd += 0.1;
                p.quadSize *= 1.5f;
                p.lifetime = 16;
                p.hasPhysics = false;
                if (kind == K::AngryVillager) color(1.0f, 1.0f, 1.0f);
                return true;
            }

            // ── The smoke family (BaseAshSmokeParticle) ──────────────────
            case K::Smoke:
                ashSmoke(x, y, z, 0.1f, 0.1f, 0.1f, xa, ya, za, 1.0f, 0.3f, 8, -0.1f, true);
                return true;
            case K::LargeSmoke:
                ashSmoke(x, y, z, 0.1f, 0.1f, 0.1f, xa, ya, za, 2.5f, 0.3f, 8, -0.1f, true);
                return true;
            case K::WhiteSmoke:
                ashSmoke(x, y, z, 0.1f, 0.1f, 0.1f, xa, ya, za, 1.0f, 0.3f, 8, -0.1f, true);
                color(0.7294118f, 0.69411767f, 0.7607843f);
                return true;
            case K::Ash:
                ashSmoke(x, y, z, 0.1f, -0.1f, 0.1f, 0.0, 0.0, 0.0, 1.0f, 0.5f, 20, 0.1f, false);
                return true;
            case K::WhiteAsh: {
                const double wxa = static_cast<double>(rng.NextFloat()) * -1.9 * static_cast<double>(rng.NextFloat()) * 0.1;
                const double wya = static_cast<double>(rng.NextFloat()) * -0.5 * static_cast<double>(rng.NextFloat()) * 0.1 * 5.0;
                const double wza = static_cast<double>(rng.NextFloat()) * -1.9 * static_cast<double>(rng.NextFloat()) * 0.1;
                ashSmoke(x, y, z, 0.1f, -0.1f, 0.1f, wxa, wya, wza, 1.0f, 0.0f, 20, 0.0125f, false);
                rgbColor(12235202);
                return true;
            }
            case K::DustPlume: {
                ashSmoke(x, y, z, 0.7f, 0.6f, 0.7f, xa, ya + 0.15000000596046448, za, 1.0f, 0.5f, 7, 0.5f, false);
                const float shift = rng.NextFloat() * 0.2f;
                const glm::vec3 c = Rgb(12235202);
                color(c.r - shift, c.g - shift, c.b - shift);
                return true;
            }
            case K::NoxiousGas: {
                // NoxiousGasParticle → BaseAshSmokeParticle(... 0.1 ×3, xa..,
                // 3.0, sprites, 0.3, 5, -0.02, true); then white, a lifetime
                // of 6 / (r·0.5 + 0.5) · scale and a fade over its second half.
                ashSmoke(x, y, z, 0.1f, 0.1f, 0.1f, xa, ya, za, 3.0f, 0.3f, 5, -0.02f, true);
                color(1.0f, 1.0f, 1.0f);
                p.lifetime = static_cast<int>(6.0 / (static_cast<double>(rng.NextFloat()) * 0.5 + 0.5) * 3.0);
                p.fadeStart = static_cast<float>(p.lifetime) / 2.0f;
                p.translucent = true;
                return true;
            }
            case K::GeyserBase:
            case K::GeyserPoof: {
                // GeyserBaseParticle's Provider: ±0.25 around the seed, 0.2
                // up; then BaseAshSmokeParticle(level, x, y, z, burst ×3,
                // xa.., size, sprites, 0, 0, 0, true) with burst =
                // base + 0.25·water, size = 3 + 0.125·water; white, friction
                // 0.725, only ever rising, 20..25 ticks.
                const double gx = x + static_cast<double>((rng.NextFloat() - 0.5f) * 0.5f);
                const double gy = y + static_cast<double>((rng.NextFloat() - 0.5f) * 0.5f) + 0.20000000298023224;
                const double gz = z + static_cast<double>((rng.NextFloat() - 0.5f) * 0.5f);
                const int water = q.hasOptions ? o.intValue : static_cast<int>(q.vx);
                const float burstBase = q.hasOptions ? o.scale : (kind == K::GeyserPoof ? 2.0f : 1.5f);
                const float burst = burstBase + 0.25f * static_cast<float>(water);
                const float size = 3.0f + 0.125f * static_cast<float>(water);
                const double gxa = q.hasOptions ? xa : 0.0;
                const double gya = q.hasOptions ? ya : 0.0;
                const double gza = q.hasOptions ? za : 0.0;
                ashSmoke(gx, gy, gz, burst, burst, burst, gxa, gya, gza, size, 0.0f, 0, 0.0f, true);
                p.friction = 0.725f;
                color(1.0f, 1.0f, 1.0f);
                p.yd = std::abs(p.yd);
                const float lifetimeFactor = 0.8f + 0.2f * rng.NextFloat();
                p.lifetime = static_cast<int>(25.0f * lifetimeFactor);
                return true;
            }

            // ── ExplodeParticle / SpitParticle ───────────────────────────
            case K::Poof:
            case K::Spit: {
                quad4(x, y, z);
                p.gravity = -0.1f;
                p.friction = 0.9f;
                walk(set);
                p.xd = xa + static_cast<double>((rng.NextFloat() * 2.0f - 1.0f) * 0.05f);
                p.yd = ya + static_cast<double>((rng.NextFloat() * 2.0f - 1.0f) * 0.05f);
                p.zd = za + static_cast<double>((rng.NextFloat() * 2.0f - 1.0f) * 0.05f);
                const float col = rng.NextFloat() * 0.3f + 0.7f;
                color(col, col, col);
                p.quadSize = 0.1f * (rng.NextFloat() * rng.NextFloat() * 6.0f + 1.0f);
                p.lifetime = static_cast<int>(16.0 / (static_cast<double>(rng.NextFloat()) * 0.8 + 0.2)) + 2;
                if (kind == K::Spit) p.gravity = 0.5f;
                return true;
            }

            // ── HugeExplosionParticle / SonicBoomParticle / sweep ────────
            case K::Explosion:
            case K::SonicBoom: {
                // `size` rides the xAux slot.
                quad7(x, y, z, 0.0, 0.0, 0.0);
                walk(set);
                p.lifetime = 6 + rng.NextInt(4);
                const float col = rng.NextFloat() * 0.6f + 0.4f;
                color(col, col, col);
                p.quadSize = 2.0f * (1.0f - static_cast<float>(xa) * 0.5f);
                p.light = Light::FullBright;
                p.hasPhysics = false;
                p.freeMove = true;
                if (kind == K::SonicBoom) {
                    p.lifetime = 16;
                    p.quadSize = 1.5f;
                }
                return true;
            }
            case K::SweepAttack: {
                quad7(x, y, z, 0.0, 0.0, 0.0);
                walk(set);
                p.lifetime = 4;
                const float col = rng.NextFloat() * 0.6f + 0.4f;
                color(col, col, col);
                p.quadSize = 1.0f - static_cast<float>(xa) * 0.5f;
                p.light = Light::FullBright;
                p.hasPhysics = false;
                p.freeMove = true;
                return true;
            }

            // ── The unrendered seeds ─────────────────────────────────────
            case K::ExplosionEmitter: {
                // HugeExplosionSeedParticle: NoRenderParticle(level, x, y, z,
                // 0, 0, 0), lifetime 8.
                base(x, y, z);
                baseVel(0.0, 0.0, 0.0);
                p.group = Group::NoRender;
                p.lifetime = 8;
                p.hasPhysics = false;
                return true;
            }
            case K::GustEmitterLarge:
            case K::GustEmitterSmall: {
                // GustSeedParticle.Provider(3.0, 7, 0) / (1.0, 3, 2).
                base(x, y, z);
                baseVel(0.0, 0.0, 0.0);
                p.group = Group::NoRender;
                const bool large = kind == K::GustEmitterLarge;
                p.seedScale = large ? 3.0 : 1.0;
                p.lifetime = large ? 7 : 3;
                p.seedDelay = large ? 0 : 2;
                p.hasPhysics = false;
                return true;
            }
            case K::NoxiousGasCloud:
            case K::Geyser: {
                // NoxiousGasCloudParticle / GeyserEruptionParticle: unrendered
                // 20-tick seeds at rest (NoRenderParticle(level, x, y, z)).
                base(x, y, z);
                p.group = Group::NoRender;
                p.lifetime = 20;
                p.waterBlocks = q.hasOptions ? o.intValue : static_cast<int>(q.vx);
                p.hasPhysics = false;
                return true;
            }
            case K::FireworkStarter: {
                // FireworkParticles.Starter.
                if (!o.fireworkExplosions || o.fireworkExplosions->empty()) return false;
                base(x, y, z);
                p.group = Group::NoRender;
                p.xd = xa; p.yd = ya; p.zd = za;
                p.explosions = o.fireworkExplosions;
                p.playSound = o.fireworkPlaySound;
                p.lifetime = static_cast<int>(p.explosions->size()) * 2 - 1;
                for (const Game::FireworkExplosion& e : *p.explosions) {
                    if (e.hasTwinkle) { p.twinkleDelay = true; p.lifetime += 15; break; }
                }
                p.hasPhysics = false;
                return true;
            }
            case K::ElderGuardian: {
                // ElderGuardianParticle: Particle(level, x, y, z), gravity 0,
                // lifetime 30, its own ELDER_GUARDIANS group.
                base(x, y, z);
                p.group = Group::ElderGuardians;
                p.gravity = 0.0f;
                p.lifetime = 30;
                p.hasPhysics = false;
                return true;
            }

            // ── SpellParticle ────────────────────────────────────────────
            case K::EntityEffect:
                // MobEffectProvider: ColorParticleOption's colour and alpha.
                spell(x, y, z, xa, ya, za);
                color(o.r, o.g, o.b);
                p.alpha = o.a;
                p.originalAlpha = o.a;
                return true;
            case K::WitchMagic: {
                spell(x, y, z, xa, ya, za);
                const float b = rng.NextFloat() * 0.5f + 0.35f;
                color(1.0f * b, 0.0f * b, 1.0f * b);
                return true;
            }
            case K::Effect:
            case K::InstantEffect:
                // InstantProvider: the colour and the power.
                spell(x, y, z, xa, ya, za);
                color(o.r, o.g, o.b);
                SetPower(p, q.hasOptions ? o.scale : 1.0f);
                return true;
            case K::Infested:
            case K::RaidOmen:
            case K::TrialOmen:
                spell(x, y, z, xa, ya, za);
                return true;

            // ── FallingDustParticle ──────────────────────────────────────
            case K::FallingDust: {
                glm::vec3 c(o.r, o.g, o.b);
                if (q.hasOptions) {
                    // FallingDustParticle.Provider: a FallingBlock's dust
                    // colour, else layer 0's colorAsTerrainParticle (white for
                    // grass_block, whose source answers -1), else the map
                    // colour (FallingBlockDustColor's default).
                    const Game::BlockState state = Game::BlockState::FromRawId(o.blockState);
                    if (state.Block() != Game::BlockID::Air && IsInvisibleRenderShape(state.Block())) return false;
                    const glm::ivec3 pos(static_cast<int>(std::floor(x)), static_cast<int>(std::floor(y)),
                                         static_cast<int>(std::floor(z)));
                    std::optional<uint32_t> tint;
                    if (!Game::IsFallingBlock(state.Block())) tint = TerrainParticleTint(state, blocks, pos);
                    c = Rgb(tint ? *tint : Game::FallingBlockDustColor(state));
                }
                quad4(x, y, z);
                color(c.r, c.g, c.b);
                p.quadSize *= 0.67499995f;
                const int baseLifetime = static_cast<int>(32.0 / (static_cast<double>(rng.NextFloat()) * 0.8 + 0.2));
                p.lifetime = std::max(static_cast<int>(static_cast<float>(baseLifetime) * 0.9f), 1);
                walk(set);
                p.rollSpeed = (rng.NextFloat() - 0.5f) * 0.1f;
                p.roll = p.oRoll = rng.NextFloat() * 6.2831855f;
                return true;
            }

            // ── BlockMarker ──────────────────────────────────────────────
            case K::BlockMarker: {
                const Game::BlockState state = Game::BlockState::FromRawId(o.blockState);
                AtlasSprite sprite;
                if (!BlockParticleSprite(state, sprite)) return false;
                quad4(x, y, z);
                p.gravity = 0.0f;
                p.lifetime = 80;
                p.hasPhysics = false;
                p.quadSize = 0.5f;
                p.u0 = sprite.rect.uvMin.x; p.v0 = sprite.rect.uvMin.y;
                p.u1 = sprite.rect.uvMax.x; p.v1 = sprite.rect.uvMax.y;
                p.sheet = sprite.atlas == AtlasId::Items ? Sheet::Items : Sheet::Blocks;
                p.translucent = Game::BlockRegistry::Get(state.Block()).renderLayer == Game::RenderLayer::Translucent;
                return true;
            }

            // ── SimpleVerticalParticle ───────────────────────────────────
            case K::PauseMobGrowth:
            case K::ResetMobGrowth:
                pick(set);
                quad7(x, y, z, xa, ya, za);
                p.xd = xa; p.zd = za; p.yd = ya;
                p.gravity = 0.0f;
                p.yd += kind == K::ResetMobGrowth ? 0.03 : -0.03;
                p.quadSize *= rng.NextFloat() * 0.6f + 0.5f;
                p.lifetime = 8;
                return true;

            // ── PortalParticle and its recolours ─────────────────────────
            case K::Portal:
            case K::HushPortal:
            case K::AetherPortal:
            case K::ReversePortal: {
                pick(set);
                quad4(x, y, z);
                p.xd = xa; p.yd = ya; p.zd = za;
                p.xStart = x; p.yStart = y; p.zStart = z;
                p.quadSize = 0.1f * (rng.NextFloat() * 0.2f + 0.5f);
                const float br = rng.NextFloat() * 0.6f + 0.4f;
                if (kind == K::HushPortal) {
                    // The Hush's portal is sculk teal (#2BD4C0).
                    color(br * 0.17f, br * 0.83f, br * 0.75f);
                } else if (kind == K::AetherPortal) {
                    // AetherParticleTypes.AETHER_PORTAL: pale blue.
                    color(br * 0.6f, br * 0.8f, br * 1.0f);
                } else {
                    color(br * 0.9f, br * 0.3f, br);
                }
                p.lifetime = static_cast<int>(rng.NextFloat() * 10.0f) + 40;
                p.freeMove = true;
                p.light = Light::AgeEmissionPow4;
                if (kind == K::ReversePortal) {
                    p.quadSize *= 1.5f;
                    p.lifetime = static_cast<int>(rng.NextFloat() * 2.0f) + 60;
                }
                return true;
            }

            // ── FlameParticle (and its small variant) ────────────────────
            case K::Flame:
            case K::SoulFireFlame:
            case K::CopperFireFlame:
            case K::SmallFlame:
                pick(set);
                rising(x, y, z, xa, ya, za);
                p.freeMove = true;
                p.light = Light::AgeEmission;
                if (kind == K::SmallFlame) Scale(p, 0.5f);
                return true;

            // ── Critical hits ────────────────────────────────────────────
            case K::Crit:
            case K::EnchantedHit:
            case K::DamageIndicator: {
                // CritParticle(level, x, y, z, xa, ya, za, sprite); the
                // damage indicator's provider adds 1 to ya. The constructor
                // ends with one tick() (run by the caller below).
                pick(set);
                const double cya = kind == K::DamageIndicator ? ya + 1.0 : ya;
                quad7(x, y, z, 0.0, 0.0, 0.0);
                p.friction = 0.7f;
                p.gravity = 0.5f;
                p.xd *= 0.10000000149011612; p.yd *= 0.10000000149011612; p.zd *= 0.10000000149011612;
                p.xd += xa * 0.4; p.yd += cya * 0.4; p.zd += za * 0.4;
                const float col = rng.NextFloat() * 0.3f + 0.6f;
                color(col, col, col);
                p.quadSize *= 0.75f;
                p.lifetime = std::max(static_cast<int>(6.0 / (static_cast<double>(rng.NextFloat()) * 0.8 + 0.6)), 1);
                p.hasPhysics = false;
                {
                    std::vector<ChildSpawn> none;
                    TickParticle(p, nullptr, none);
                }
                if (kind == K::EnchantedHit) { p.rCol *= 0.3f; p.gCol *= 0.8f; }
                if (kind == K::DamageIndicator) p.lifetime = 20;
                return !p.removed;
            }

            // ── SuspendedTownParticle ────────────────────────────────────
            case K::HappyVillager:
            case K::EggCrack:
                suspendedTown(x, y, z, xa, ya, za);
                color(1.0f, 1.0f, 1.0f);
                return true;
            case K::Composter:
                suspendedTown(x, y, z, xa, ya, za);
                color(1.0f, 1.0f, 1.0f);
                p.lifetime = 3 + rng.NextInt(5);
                return true;
            case K::Dolphin:
                suspendedTown(x, y, z, xa, ya, za);
                color(0.3f, 0.5f, 1.0f);
                p.alpha = 1.0f - rng.NextFloat() * 0.7f;
                p.lifetime /= 2;
                return true;
            case K::Mycelium:
                suspendedTown(x, y, z, xa, ya, za);
                return true;

            // ── The engine's own motes ───────────────────────────────────
            case K::HushMist: {
                // Aurelith's river mist: a slow, faint violet puff hugging
                // the water — one full frame of the generic sheet for life.
                quad4(x, y, z);
                p.spriteSet = static_cast<int16_t>(set);
                p.walk = false;
                p.spriteFrame = static_cast<uint8_t>(5 + rng.NextInt(3));
                p.sprite = m_sprites.Get(set, p.spriteFrame, 7);
                p.xd = xa; p.yd = ya; p.zd = za;
                p.quadSize = 0.55f + rng.NextFloat() * 0.45f;
                const float br = 0.75f + rng.NextFloat() * 0.25f;
                color(br * 0.62f, br * 0.42f, br * 1.0f);
                p.alpha = 0.0f;
                p.lifetime = 140 + rng.NextInt(80);
                p.gravity = 0.0f;
                p.hasPhysics = false;
                p.freeMove = true;
                p.light = Light::BlockFull;
                p.translucent = true;
                return true;
            }
            case K::VesperGlint: {
                first(set);
                quad4(x, y, z);
                p.xd = xa; p.yd = ya; p.zd = za;
                p.quadSize = 0.035f + rng.NextFloat() * 0.035f;
                const float cyan = rng.NextFloat();
                color(0.72f - 0.25f * cyan, 0.70f + 0.28f * cyan, 1.0f);
                p.alpha = 0.0f;
                p.lifetime = 20 + rng.NextInt(24);
                p.gravity = 0.0f;
                p.hasPhysics = false;
                p.freeMove = true;
                p.spriteFrame = static_cast<uint8_t>(rng.NextInt(256));   // the twinkle's phase
                p.light = Light::BlockFull;
                p.translucent = true;
                return true;
            }
            case K::HushMote: {
                // The Hush's soft glint — modelled on the end rod's drift,
                // no vanilla counterpart. Tinted when the request carries a
                // colour (Aurelith's voice motes).
                first(set);
                quad4(x, y, z);
                p.xd = xa; p.yd = ya; p.zd = za;
                p.quadSize = 0.03f + rng.NextFloat() * 0.03f;
                const float br = rng.NextFloat() * 0.4f + 0.6f;
                if (o.r < 1.0f || o.g < 1.0f || o.b < 1.0f) color(br * o.r, br * o.g, br * o.b);
                else color(br * 0.55f, br * 0.95f, br * 1.0f);
                p.alpha = 0.0f;
                p.lifetime = 100 + rng.NextInt(60);
                p.gravity = 0.0f;
                p.hasPhysics = false;
                p.freeMove = true;
                p.translucent = true;
                return true;
            }

            // ── The 26.3 geyser family ───────────────────────────────────
            case K::SulfurBubbles: {
                // SulfurBubbleParticle(level, x, y, z, xa, za, sprite) — its
                // provider hands (xAux, yAux) in as (xa, za).
                pick(set);
                quad4(x, y, z);
                p.gravity = -0.04f;
                p.friction = 0.85f;
                SetSize(p, 0.02f, 0.02f);
                p.xd = xa * 0.20000000298023224 + static_cast<double>((rng.NextFloat() * 2.0f - 1.0f) * 0.02f);
                p.zd = ya * 0.20000000298023224 + static_cast<double>((rng.NextFloat() * 2.0f - 1.0f) * 0.02f);
                p.sizeMin = 0.02f + 0.02f * rng.NextFloat();
                p.quadSize = p.sizeMin;
                p.lifetime = INT_MAX;
                p.yStart = p.y;
                p.yEnd = p.y + 4.0 - 1.0;
                p.yPrev = p.y;
                return true;
            }
            case K::GeyserPlume: {
                // GeyserPlumeParticle's Provider: ±0.1 across, 0..1 up.
                const double gx = x + static_cast<double>((rng.NextFloat() - 0.5f) * 0.2f);
                const double gy = y + static_cast<double>(rng.NextFloat());
                const double gz = z + static_cast<double>((rng.NextFloat() - 0.5f) * 0.2f);
                const int water = q.hasOptions ? o.intValue : static_cast<int>(q.vx);
                quad7(gx, gy, gz, 0.0, 0.0, 0.0);
                walk(set);
                const int plumeHeight = 5 * std::max(1, water);
                p.hasPhysics = true;
                p.speedUpWhenYMotionIsBlocked = true;
                p.lifetime = plumeHeight * 5;
                p.yd = 0.0;
                p.yStart = p.y;
                p.yEnd = p.yStart + static_cast<double>(plumeHeight) - 1.0;
                p.sprayX = (rng.NextFloat() - 0.5f) * 0.2f;
                p.sprayZ = (rng.NextFloat() - 0.5f) * 0.2f;
                p.friction = 1.0f;
                p.propulsion = (water == 1 ? 1.5f : 1.0f) * static_cast<float>(plumeHeight) * 1.45f;
                p.gravity = -p.propulsion;
                const float initiallyRandomizedSize = p.quadSize * 0.75f;
                p.sizeMin = initiallyRandomizedSize * (2.0f + static_cast<float>(plumeHeight) / 8.0f);
                p.sizeMax = initiallyRandomizedSize * (3.0f + static_cast<float>(plumeHeight) / 8.0f);
                p.quadSize = p.sizeMin;
                return true;
            }

            // ── TerrainParticle and its providers ────────────────────────
            case K::Block:
                return terrain(x, y, z, xa, ya, za, Game::BlockState::FromRawId(o.blockState));
            case K::DustPillar:
                if (!terrain(x, y, z, xa, ya, za, Game::BlockState::FromRawId(o.blockState))) return false;
                p.xd = rng.NextGaussian() / 30.0;
                p.yd = ya + rng.NextGaussian() / 2.0;
                p.zd = rng.NextGaussian() / 30.0;
                p.lifetime = rng.NextInt(20) + 20;
                return true;
            case K::BlockCrumble:
                if (!terrain(x, y, z, xa, ya, za, Game::BlockState::FromRawId(o.blockState))) return false;
                p.xd = p.yd = p.zd = 0.0;
                p.lifetime = rng.NextInt(10) + 1;
                return true;

            // ── BreakingItemParticle ─────────────────────────────────────
            case K::Item: {
                AtlasSprite sprite;
                if (!ItemParticleSprite(o.itemId, sprite)) return false;
                breakingItem(x, y, z, sprite);
                p.xd *= 0.10000000149011612; p.yd *= 0.10000000149011612; p.zd *= 0.10000000149011612;
                p.xd += xa; p.yd += ya; p.zd += za;
                return true;
            }
            case K::ItemSlime:
            case K::ItemCobweb:
            case K::ItemSnowball: {
                const Game::ItemID item = kind == K::ItemSlime ? Game::Items::SlimeBall
                                        : kind == K::ItemCobweb ? Game::ItemRegistry::FromBlock(Game::BlockID::Cobweb)
                                                                : Game::Items::Snowball;
                AtlasSprite sprite;
                if (!ItemParticleSprite(item, sprite)) return false;
                breakingItem(x, y, z, sprite);
                return true;
            }
            case K::SulfurCubeGoo: {
                // SulfurCubeProvider: the particle atlas' sulfur_cube_goo.
                quad7(x, y, z, 0.0, 0.0, 0.0);
                p.gravity = 1.0f;
                p.quadSize /= 2.0f;
                first(set);
                const float uo = rng.NextFloat() * 3.0f;
                const float vo = rng.NextFloat() * 3.0f;
                // Sub-rect of the particle sprite, kept as fractions and
                // resolved to atlas UVs at render time.
                p.u0 = (uo + 1.0f) / 4.0f; p.u1 = uo / 4.0f;
                p.v0 = vo / 4.0f;          p.v1 = (vo + 1.0f) / 4.0f;
                p.subRect = true;
                // Layer.bySprite on the particle atlas.
                p.translucent = m_sprites.Valid(p.sprite) && m_sprites.GetSprite(p.sprite).translucent;
                return true;
            }

            // ── Bubbles ──────────────────────────────────────────────────
            case K::Bubble:
                pick(set);
                quad4(x, y, z);
                SetSize(p, 0.02f, 0.02f);
                p.quadSize *= rng.NextFloat() * 0.6f + 0.2f;
                p.xd = xa * 0.20000000298023224 + static_cast<double>((rng.NextFloat() * 2.0f - 1.0f) * 0.02f);
                p.yd = ya * 0.20000000298023224 + static_cast<double>((rng.NextFloat() * 2.0f - 1.0f) * 0.02f);
                p.zd = za * 0.20000000298023224 + static_cast<double>((rng.NextFloat() * 2.0f - 1.0f) * 0.02f);
                p.lifetime = static_cast<int>(8.0 / (static_cast<double>(rng.NextFloat()) * 0.8 + 0.2));
                return true;
            case K::BubbleColumnUp:
                pick(set);
                quad4(x, y, z);
                p.gravity = -0.125f;
                p.friction = 0.85f;
                SetSize(p, 0.02f, 0.02f);
                p.quadSize *= rng.NextFloat() * 0.6f + 0.2f;
                p.xd = xa * 0.20000000298023224 + static_cast<double>((rng.NextFloat() * 2.0f - 1.0f) * 0.02f);
                p.yd = ya * 0.20000000298023224 + static_cast<double>((rng.NextFloat() * 2.0f - 1.0f) * 0.02f);
                p.zd = za * 0.20000000298023224 + static_cast<double>((rng.NextFloat() * 2.0f - 1.0f) * 0.02f);
                p.lifetime = static_cast<int>(40.0 / (static_cast<double>(rng.NextFloat()) * 0.8 + 0.2));
                return true;
            case K::BubblePop:
                quad4(x, y, z);
                walk(set);
                p.lifetime = 4;
                p.gravity = 0.008f;
                p.xd = xa; p.yd = ya; p.zd = za;
                return true;
            case K::CurrentDown:
                pick(set);
                quad4(x, y, z);
                p.lifetime = static_cast<int>(rng.NextFloat() * 60.0f) + 30;
                p.hasPhysics = false;
                p.xd = 0.0; p.yd = -0.05; p.zd = 0.0;
                SetSize(p, 0.02f, 0.02f);
                p.quadSize *= rng.NextFloat() * 0.6f + 0.2f;
                p.gravity = 0.002f;
                return true;

            // ── PlayerCloudParticle ──────────────────────────────────────
            case K::Cloud:
            case K::Sneeze: {
                quad7(x, y, z, 0.0, 0.0, 0.0);
                p.friction = 0.96f;
                p.xd *= 0.10000000149011612; p.yd *= 0.10000000149011612; p.zd *= 0.10000000149011612;
                p.xd += xa; p.yd += ya; p.zd += za;
                const float col = 1.0f - rng.NextFloat() * 0.3f;
                color(col, col, col);
                p.quadSize *= 1.875f;
                const int baseLifetime = static_cast<int>(8.0 / (static_cast<double>(rng.NextFloat()) * 0.8 + 0.3));
                p.lifetime = static_cast<int>(std::max(static_cast<float>(baseLifetime) * 2.5f, 1.0f));
                p.hasPhysics = false;
                walk(set);
                p.translucent = true;
                if (kind == K::Sneeze) {
                    color(0.22f, 1.0f, 0.53f);
                    p.alpha = 0.4f;
                }
                return true;
            }

            // ── DragonBreathParticle ─────────────────────────────────────
            case K::DragonBreath:
                quad4(x, y, z);
                p.friction = 0.96f;
                p.xd = xa; p.yd = ya; p.zd = za;
                p.rCol = NextFloat(rng, 0.7176471f, 0.8745098f);
                p.gCol = NextFloat(rng, 0.0f, 0.0f);
                p.bCol = NextFloat(rng, 0.8235294f, 0.9764706f);
                p.quadSize *= 0.75f;
                p.lifetime = static_cast<int>(20.0 / (static_cast<double>(rng.NextFloat()) * 0.8 + 0.2));
                p.hasHitGround = false;
                p.hasPhysics = false;
                walk(set);
                SetPower(p, q.hasOptions ? o.scale : 1.0f);
                return true;

            // ── DripParticle's providers ─────────────────────────────────
            case K::DrippingWater:
                dripHang(x, y, z, DripFluid::Water, K::FallingWater);
                color(0.2f, 0.3f, 1.0f);
                return true;
            case K::FallingWater:
                fallAndLand(x, y, z, DripFluid::Water, K::Splash, Drip::FallAndLand);
                color(0.2f, 0.3f, 1.0f);
                return true;
            case K::DrippingLava:
                dripHang(x, y, z, DripFluid::Lava, K::FallingLava);
                p.drip = Drip::CoolingHang;
                return true;
            case K::FallingLava:
                fallAndLand(x, y, z, DripFluid::Lava, K::LandingLava, Drip::FallAndLand);
                color(1.0f, 0.2857143f, 0.083333336f);
                return true;
            case K::LandingLava:
                dripLand(x, y, z, DripFluid::Lava);
                color(1.0f, 0.2857143f, 0.083333336f);
                return true;
            case K::DrippingHoney:
                dripHang(x, y, z, DripFluid::Empty, K::FallingHoney);
                p.gravity *= 0.01f;
                p.lifetime = 100;
                color(0.622f, 0.508f, 0.082f);
                return true;
            case K::FallingHoney:
                fallAndLand(x, y, z, DripFluid::Empty, K::LandingHoney, Drip::HoneyFallAndLand);
                p.gravity = 0.01f;
                color(0.582f, 0.448f, 0.082f);
                return true;
            case K::LandingHoney:
                dripLand(x, y, z, DripFluid::Empty);
                p.lifetime = static_cast<int>(128.0 / (static_cast<double>(rng.NextFloat()) * 0.8 + 0.2));
                color(0.522f, 0.408f, 0.082f);
                return true;
            case K::FallingNectar:
                drip(x, y, z, DripFluid::Empty, Drip::Falling);
                p.lifetime = static_cast<int>(16.0 / (static_cast<double>(rng.NextFloat()) * 0.8 + 0.2));
                p.gravity = 0.007f;
                color(0.92f, 0.782f, 0.72f);
                return true;
            case K::FallingSporeBlossom:
                drip(x, y, z, DripFluid::Empty, Drip::Falling);
                p.lifetime = static_cast<int>(64.0f / NextFloat(rng, 0.1f, 0.9f));
                p.gravity = 0.005f;
                color(0.32f, 0.5f, 0.22f);
                return true;
            case K::DrippingObsidianTear:
                dripHang(x, y, z, DripFluid::Empty, K::FallingObsidianTear);
                p.light = Light::BlockFull;
                p.gravity *= 0.01f;
                p.lifetime = 100;
                color(0.51171875f, 0.03125f, 0.890625f);
                return true;
            case K::FallingObsidianTear:
                fallAndLand(x, y, z, DripFluid::Empty, K::LandingObsidianTear, Drip::FallAndLand);
                p.light = Light::BlockFull;
                p.gravity = 0.01f;
                color(0.51171875f, 0.03125f, 0.890625f);
                return true;
            case K::LandingObsidianTear:
                dripLand(x, y, z, DripFluid::Empty);
                p.light = Light::BlockFull;
                p.lifetime = static_cast<int>(28.0 / (static_cast<double>(rng.NextFloat()) * 0.8 + 0.2));
                color(0.51171875f, 0.03125f, 0.890625f);
                return true;
            case K::DrippingDripstoneWater:
                dripHang(x, y, z, DripFluid::Water, K::FallingDripstoneWater);
                color(0.2f, 0.3f, 1.0f);
                return true;
            case K::FallingDripstoneWater:
                fallAndLand(x, y, z, DripFluid::Water, K::Splash, Drip::DripstoneFallAndLand);
                color(0.2f, 0.3f, 1.0f);
                return true;
            case K::DrippingDripstoneLava:
                dripHang(x, y, z, DripFluid::Lava, K::FallingDripstoneLava);
                p.drip = Drip::CoolingHang;
                return true;
            case K::FallingDripstoneLava:
                fallAndLand(x, y, z, DripFluid::Lava, K::LandingLava, Drip::DripstoneFallAndLand);
                color(1.0f, 0.2857143f, 0.083333336f);
                return true;

            // ── DustParticle / DustColorTransitionParticle ───────────────
            case K::Dust:
            case K::DustColorTransition: {
                // DustParticleBase.
                const float scale = o.scale;
                quad7(x, y, z, xa, ya, za);
                p.friction = 0.96f;
                p.speedUpWhenYMotionIsBlocked = true;
                p.xd *= 0.10000000149011612; p.yd *= 0.10000000149011612; p.zd *= 0.10000000149011612;
                p.quadSize *= 0.75f * scale;
                const int baseLifetime = static_cast<int>(8.0 / (rng.NextDouble() * 0.8 + 0.2));
                p.lifetime = static_cast<int>(std::max(static_cast<float>(baseLifetime) * scale, 1.0f));
                walk(set);
                const float baseFactor = rng.NextFloat() * 0.4f + 0.6f;
                const auto randomize = [&](float c) { return (rng.NextFloat() * 0.2f + 0.8f) * c * baseFactor; };
                if (kind == K::Dust) {
                    p.rCol = randomize(o.r);
                    p.gCol = randomize(o.g);
                    p.bCol = randomize(o.b);
                } else {
                    p.fromColor.r = randomize(o.r); p.fromColor.g = randomize(o.g); p.fromColor.b = randomize(o.b);
                    p.toColor.r = randomize(o.r2);  p.toColor.g = randomize(o.g2);  p.toColor.b = randomize(o.b2);
                    color(p.fromColor.r, p.fromColor.g, p.fromColor.b);
                }
                return true;
            }

            // ── FlyTowardsPositionParticle's providers ───────────────────
            case K::Enchant:
            case K::Nautilus:
                flyTowards(x, y, z, xa, ya, za, false, 1.0f, 1.0f, 0.0f, 1.0f);
                return true;
            case K::VaultConnection:
                flyTowards(x, y, z, xa, ya, za, true, 0.0f, 0.6f, 0.25f, 1.0f);
                Scale(p, 1.5f);
                return true;

            // ── SimpleAnimatedParticle's providers ───────────────────────
            case K::EndRod:
                animated(x, y, z, 0.0125f);
                p.xd = xa; p.yd = ya; p.zd = za;
                p.quadSize *= 0.75f;
                p.lifetime = 60 + rng.NextInt(12);
                fadeColor(15916745);
                walk(set);
                p.freeMove = true;
                return true;
            case K::TotemOfUndying:
                animated(x, y, z, 1.25f);
                p.friction = 0.6f;
                p.xd = xa; p.yd = ya; p.zd = za;
                p.quadSize *= 0.75f;
                p.lifetime = 60 + rng.NextInt(12);
                walk(set);
                if (rng.NextInt(4) == 0) {
                    color(0.6f + rng.NextFloat() * 0.2f, 0.6f + rng.NextFloat() * 0.3f, rng.NextFloat() * 0.2f);
                } else {
                    color(0.1f + rng.NextFloat() * 0.2f, 0.4f + rng.NextFloat() * 0.3f, rng.NextFloat() * 0.2f);
                }
                return true;
            case K::SquidInk:
            case K::GlowSquidInk: {
                animated(x, y, z, 0.0f);
                p.friction = 0.92f;
                p.quadSize = 0.5f;
                p.alpha = 1.0f;
                if (kind == K::GlowSquidInk) color(0.2f, 0.8f, 0.6f);
                else color(0.0f, 0.0f, 0.0f);
                p.lifetime = static_cast<int>(p.quadSize * 12.0f / (rng.NextFloat() * 0.8f + 0.2f));
                walk(set);
                p.hasPhysics = false;
                p.xd = xa; p.yd = ya; p.zd = za;
                return true;
            }
            case K::Firework:
                // FireworkParticles.SparkParticle (SparkProvider: alpha 0.99).
                animated(x, y, z, 0.1f);
                p.xd = xa; p.yd = ya; p.zd = za;
                p.quadSize *= 0.75f;
                p.lifetime = 48 + rng.NextInt(12);
                walk(set);
                p.alpha = 0.99f;
                return true;

            // ── GustParticle ─────────────────────────────────────────────
            case K::Gust:
            case K::SmallGust:
                quad4(x, y, z);
                walk(set);
                p.lifetime = 12 + rng.NextInt(4);
                p.quadSize = 1.0f;
                SetSize(p, 1.0f, 1.0f);
                p.light = Light::FullBright;
                if (kind == K::SmallGust) Scale(p, 0.15f);
                return true;

            // ── WakeParticle ─────────────────────────────────────────────
            case K::Fishing:
                quad7(x, y, z, 0.0, 0.0, 0.0);
                first(set);
                p.xd *= 0.30000001192092896;
                p.yd = static_cast<double>(rng.NextFloat() * 0.2f + 0.1f);
                p.zd *= 0.30000001192092896;
                SetSize(p, 0.01f, 0.01f);
                p.lifetime = static_cast<int>(8.0 / (static_cast<double>(rng.NextFloat()) * 0.8 + 0.2));
                p.sprite = m_sprites.Get(set, 0, p.lifetime);   // setSpriteFromAge at age 0
                p.gravity = 0.0f;
                p.xd = xa; p.yd = ya; p.zd = za;
                return true;

            // ── FallingLeavesParticle ────────────────────────────────────
            case K::CherryLeaves:
                fallingLeaves(x, y, z, 0.25f, 2.0f, false, true, 1.0f, 0.0f);
                return true;
            case K::PaleOakLeaves:
            case K::RedPoplarLeaves:
            case K::OrangePoplarLeaves:
            case K::YellowPoplarLeaves:
                fallingLeaves(x, y, z, 0.07f, 10.0f, true, false, 2.0f, 0.021f);
                return true;
            case K::TintedLeaves:
                fallingLeaves(x, y, z, 0.07f, 10.0f, true, false, 2.0f, 0.021f);
                color(o.r, o.g, o.b);
                return true;

            // ── EmissiveRisingParticle ───────────────────────────────────
            case K::SculkSoul:
            case K::Soul:
                first(set);
                rising(x, y, z, xa, ya, za);
                Scale(p, 1.5f);
                walk(set);
                p.translucent = true;
                p.alpha = 1.0f;
                if (kind == K::SculkSoul) p.light = Light::BlockFull;
                return true;

            // ── SculkChargeParticle / SculkChargePopParticle ─────────────
            case K::SculkCharge:
            case K::SculkChargePop:
                quad7(x, y, z, xa, ya, za);
                p.friction = 0.96f;
                Scale(p, kind == K::SculkCharge ? 1.5f : 1.0f);
                p.hasPhysics = false;
                walk(set);
                p.light = Light::BlockFull;
                p.translucent = true;
                p.alpha = 1.0f;
                p.xd = xa; p.yd = ya; p.zd = za;
                if (kind == K::SculkCharge) {
                    p.oRoll = p.roll = o.scale;
                    p.lifetime = rng.NextInt(12) + 8;
                } else {
                    p.lifetime = rng.NextInt(4) + 6;
                }
                return true;

            // ── FireworkParticles.OverlayParticle (FLASH) ────────────────
            case K::Flash:
                pick(set);
                quad4(x, y, z);
                p.lifetime = 4;
                color(o.r, o.g, o.b);
                p.alpha = o.a;
                p.translucent = true;
                return true;

            // ── VibrationSignalParticle ──────────────────────────────────
            case K::Vibration: {
                pick(set);
                quad7(x, y, z, 0.0, 0.0, 0.0);
                p.quadSize = 0.3f;
                p.target = o.target;
                p.targetEntity = o.targetEntity;
                p.targetYOffset = o.targetEntityYOffset;
                p.lifetime = o.intValue;
                p.light = Light::BlockFull;
                p.translucent = true;
                p.facing = Facing::Vibration;
                p.alpha = 1.0f;
                p.hasPhysics = false;
                // Aimed at the destination from the start (the entity's is
                // resolved on the first tick).
                if (p.targetEntity < 0) {
                    const double dx = x - p.target.x, dy = y - p.target.y, dz = z - p.target.z;
                    p.rotO = p.rot = static_cast<float>(std::atan2(dx, dz));
                    p.pitchO = p.pitch = static_cast<float>(std::atan2(dy, std::sqrt(dx * dx + dz * dz)));
                }
                return true;
            }

            // ── TrailParticle ────────────────────────────────────────────
            case K::Trail: {
                pick(set);
                quad7(x, y, z, xa, ya, za);
                // ARGB.scaleRGB(color, 0.875 + r·0.25 ×3).
                const float sr = 0.875f + rng.NextFloat() * 0.25f;
                const float sg = 0.875f + rng.NextFloat() * 0.25f;
                const float sb = 0.875f + rng.NextFloat() * 0.25f;
                const uint32_t rgb = o.Rgb();
                const int r8 = std::clamp(static_cast<int>(static_cast<float>((rgb >> 16) & 255) * sr), 0, 255);
                const int g8 = std::clamp(static_cast<int>(static_cast<float>((rgb >> 8) & 255) * sg), 0, 255);
                const int b8 = std::clamp(static_cast<int>(static_cast<float>(rgb & 255) * sb), 0, 255);
                color(static_cast<float>(r8) / 255.0f, static_cast<float>(g8) / 255.0f, static_cast<float>(b8) / 255.0f);
                p.quadSize = 0.26f;
                p.target = o.target;
                p.lifetime = o.intValue;
                p.light = Light::FullBright;
                return true;
            }

            // ── LavaParticle ─────────────────────────────────────────────
            case K::Lava:
                pick(set);
                quad7(x, y, z, 0.0, 0.0, 0.0);
                p.gravity = 0.75f;
                p.friction = 0.999f;
                p.xd *= 0.800000011920929; p.yd *= 0.800000011920929; p.zd *= 0.800000011920929;
                p.yd = static_cast<double>(rng.NextFloat() * 0.4f + 0.05f);
                p.quadSize *= rng.NextFloat() * 2.0f + 0.2f;
                p.lifetime = static_cast<int>(16.0 / (static_cast<double>(rng.NextFloat()) * 0.8 + 0.2));
                p.light = Light::BlockFull;
                return true;

            // ── NoteParticle ─────────────────────────────────────────────
            case K::Note: {
                pick(set);
                quad7(x, y, z, 0.0, 0.0, 0.0);
                p.friction = 0.66f;
                p.speedUpWhenYMotionIsBlocked = true;
                p.xd *= 0.009999999776482582; p.yd *= 0.009999999776482582; p.zd *= 0.009999999776482582;
                p.yd += 0.2;
                const float note = static_cast<float>(xa);
                p.rCol = std::max(0.0f, std::sin((note + 0.0f) * 6.2831855f) * 0.65f + 0.35f);
                p.gCol = std::max(0.0f, std::sin((note + 0.33333334f) * 6.2831855f) * 0.65f + 0.35f);
                p.bCol = std::max(0.0f, std::sin((note + 0.6666667f) * 6.2831855f) * 0.65f + 0.35f);
                p.quadSize *= 1.5f;
                p.lifetime = 6;
                return true;
            }

            // ── WaterDropParticle / SplashParticle ───────────────────────
            case K::Rain:
                waterDrop(x, y, z);
                return true;
            case K::Splash:
                waterDrop(x, y, z);
                p.gravity = 0.04f;
                if (ya == 0.0 && (xa != 0.0 || za != 0.0)) {
                    p.xd = xa; p.yd = 0.1; p.zd = za;
                }
                return true;

            // ── SuspendedParticle's providers ────────────────────────────
            case K::Underwater:
                suspended4(x, y, z);
                color(0.4f, 0.4f, 0.7f);
                return true;
            case K::CrimsonSpore: {
                const double cxa = rng.NextGaussian() * 9.999999974752427E-7;
                const double cya = rng.NextGaussian() * 9.999999747378752E-5;
                const double cza = rng.NextGaussian() * 9.999999974752427E-7;
                suspended7(x, y, z, cxa, cya, cza);
                color(0.9f, 0.4f, 0.5f);
                return true;
            }
            case K::WarpedSpore: {
                const double wya = static_cast<double>(rng.NextFloat()) * -1.9 * static_cast<double>(rng.NextFloat()) * 0.1;
                suspended7(x, y, z, 0.0, wya, 0.0);
                color(0.1f, 0.1f, 0.3f);
                SetSize(p, 0.001f, 0.001f);
                return true;
            }
            case K::SporeBlossomAir:
                suspended7(x, y, z, 0.0, -0.800000011920929, 0.0);
                p.lifetime = rng.NextInt(501) + 500;
                p.gravity = 0.01f;
                color(0.32f, 0.5f, 0.22f);
                p.limitedSporeBlossom = true;
                return true;

            // ── CampfireSmokeParticle ────────────────────────────────────
            case K::CampfireCosySmoke:
            case K::CampfireSignalSmoke: {
                pick(set);
                quad4(x, y, z);
                Scale(p, 3.0f);
                SetSize(p, 0.25f, 0.25f);
                p.lifetime = rng.NextInt(50) + (kind == K::CampfireSignalSmoke ? 280 : 80);
                p.gravity = 3.0E-6f;
                p.xd = xa;
                p.yd = ya + static_cast<double>(rng.NextFloat() / 500.0f);
                p.zd = za;
                p.alpha = kind == K::CampfireSignalSmoke ? 0.95f : 0.9f;
                p.translucent = true;
                return true;
            }

            // ── SnowflakeParticle ────────────────────────────────────────
            case K::Snowflake:
                quad4(x, y, z);
                p.gravity = 0.225f;
                p.friction = 1.0f;
                walk(set);
                p.xd = xa + static_cast<double>((rng.NextFloat() * 2.0f - 1.0f) * 0.05f);
                p.yd = ya + static_cast<double>((rng.NextFloat() * 2.0f - 1.0f) * 0.05f);
                p.zd = za + static_cast<double>((rng.NextFloat() * 2.0f - 1.0f) * 0.05f);
                p.quadSize = 0.1f * (rng.NextFloat() * rng.NextFloat() * 1.0f + 1.0f);
                p.lifetime = static_cast<int>(16.0 / (static_cast<double>(rng.NextFloat()) * 0.8 + 0.2)) + 2;
                color(0.923f, 0.964f, 0.999f);
                return true;

            // ── GlowParticle's providers ─────────────────────────────────
            case K::Glow:
                // GlowSquidProvider.
                glow(x, y, z, 0.5 - rng.NextDouble(), ya, 0.5 - rng.NextDouble());
                if (rng.NextBool()) color(0.6f, 1.0f, 0.8f);
                else color(0.08f, 0.4f, 0.4f);
                p.yd *= 0.20000000298023224;
                if (xa == 0.0 && za == 0.0) {
                    p.xd *= 0.10000000149011612;
                    p.zd *= 0.10000000149011612;
                }
                p.lifetime = static_cast<int>(8.0 / (rng.NextDouble() * 0.8 + 0.2));
                return true;
            case K::WaxOn:
                glow(x, y, z, 0.0, 0.0, 0.0);
                color(0.91f, 0.55f, 0.08f);
                p.xd = xa * 0.01 / 2.0; p.yd = ya * 0.01; p.zd = za * 0.01 / 2.0;
                p.lifetime = rng.NextInt(30) + 10;
                return true;
            case K::WaxOff:
                glow(x, y, z, 0.0, 0.0, 0.0);
                color(1.0f, 0.9f, 1.0f);
                p.xd = xa * 0.01 / 2.0; p.yd = ya * 0.01; p.zd = za * 0.01 / 2.0;
                p.lifetime = rng.NextInt(30) + 10;
                return true;
            case K::ElectricSpark:
                glow(x, y, z, 0.0, 0.0, 0.0);
                color(1.0f, 0.9f, 1.0f);
                p.xd = xa * 0.25; p.yd = ya * 0.25; p.zd = za * 0.25;
                p.lifetime = rng.NextInt(2) + 2;
                return true;
            case K::Scrape:
                glow(x, y, z, 0.0, 0.0, 0.0);
                if (rng.NextBool()) color(0.29f, 0.58f, 0.51f);
                else color(0.43f, 0.77f, 0.62f);
                p.xd = xa * 0.01; p.yd = ya * 0.01; p.zd = za * 0.01;
                p.lifetime = rng.NextInt(30) + 10;
                return true;

            // ── ShriekParticle ───────────────────────────────────────────
            case K::Shriek:
                pick(set);
                quad7(x, y, z, 0.0, 0.0, 0.0);
                p.quadSize = 0.85f;
                p.delay = o.intValue;
                p.lifetime = 30;
                p.gravity = 0.0f;
                p.xd = 0.0; p.yd = 0.1; p.zd = 0.0;
                p.alpha = 1.0f;
                p.light = Light::BlockFull;
                p.translucent = true;
                p.facing = Facing::Shriek;
                return true;

            // ── TrialSpawnerDetectionParticle ────────────────────────────
            case K::TrialSpawnerDetection:
            case K::TrialSpawnerDetectionOminous: {
                constexpr float scale = 1.5f;
                quad7(x, y, z, 0.0, 0.0, 0.0);
                p.friction = 0.96f;
                p.gravity = -0.1f;
                p.speedUpWhenYMotionIsBlocked = true;
                p.xd *= 0.0; p.yd *= 0.9; p.zd *= 0.0;
                p.xd += xa; p.yd += ya; p.zd += za;
                p.quadSize *= 0.75f * scale;
                p.lifetime = static_cast<int>(8.0f / NextFloat(rng, 0.5f, 1.0f) * scale);
                p.lifetime = std::max(p.lifetime, 1);
                walk(set);
                p.hasPhysics = true;
                p.light = Light::BlockFull;
                p.facing = Facing::LookAtY;
                return true;
            }

            // ── FlyStraightTowardsParticle (OMINOUS_SPAWNING) ────────────
            case K::OminousSpawning:
                pick(set);
                quad4(x, y, z);
                p.xd = xa; p.yd = ya; p.zd = za;
                p.xStart = x; p.yStart = y; p.zStart = z;
                p.xo = x + xa; p.yo = y + ya; p.zo = z + za;
                p.x = p.xo; p.y = p.yo; p.z = p.zo;
                p.quadSize = 0.1f * (rng.NextFloat() * 0.5f + 0.2f);
                p.hasPhysics = false;
                p.lifetime = static_cast<int>(rng.NextFloat() * 5.0f) + 25;
                p.startColor = static_cast<uint32_t>(-12210434);
                p.endColor = static_cast<uint32_t>(-1);
                p.freeMove = true;
                p.light = Light::BlockFull;
                Scale(p, NextFloat(rng, 3.0f, 5.0f));
                return true;

            // ── FireflyParticle ──────────────────────────────────────────
            case K::Firefly: {
                pick(set);
                const double fxa = 0.5 - rng.NextDouble();
                const double fya = rng.NextBool() ? ya : -ya;
                const double fza = 0.5 - rng.NextDouble();
                quad7(x, y, z, fxa, fya, fza);
                p.speedUpWhenYMotionIsBlocked = true;
                p.friction = 0.96f;
                p.quadSize *= 0.75f;
                p.yd *= 0.800000011920929; p.xd *= 0.800000011920929; p.zd *= 0.800000011920929;
                p.lifetime = rng.NextInt(101) + 200;
                Scale(p, 1.5f);
                p.alpha = 0.0f;
                p.translucent = true;
                p.light = Light::Firefly;
                return true;
            }
        }
        return false;
    }

} // namespace Render
