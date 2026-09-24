// File: src/common/world/block/HushBlocks.cpp
//
// See HushBlocks.hpp for the design of both blocks.
#include "common/world/block/HushBlocks.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/sound/SoundEvents.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/entity/IUsePlayer.hpp"
#include "common/entity/Item.hpp"
#include "common/entity/LivingEntity.hpp"
#include "common/entity/effect/MobEffects.hpp"
#include "common/physics/Physics.hpp"
#include "common/world/block/BlockInteraction.hpp"
#include "common/world/block/BlockState.hpp"
#include "common/world/level/ILevelWrite.hpp"
#include "common/world/level/World.hpp"
#include "common/world/level/WorldDrops.hpp"
#include "common/world/ticks/ScheduledTickAccess.hpp"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <string_view>
#include <vector>

namespace Game {

    namespace {

        // ── hanging_whisperfruit ─────────────────────────────────────────

        constexpr std::string_view kAgeDigits[] = { "0", "1", "2" };

        int AgeOf(BlockState state) {
            const std::string_view v = state.GetValueByName("age");
            for (int i = 0; i <= Whisperfruit::kMaxAge; ++i) {
                if (v == kAgeDigits[i]) return i;
            }
            return 0;
        }

        BlockState FruitAtAge(int age) {
            age = std::clamp(age, 0, Whisperfruit::kMaxAge);
            return BlockStates::FromIndex(
                BlockID::HangingWhisperfruit,
                BlockRegistry::GetStateDefinition(BlockID::HangingWhisperfruit)
                    .IndexOfSingle("age", kAgeDigits[age]));
        }

        bool FruitIsRandomlyTicking(BlockState state) {
            return AgeOf(state) < Whisperfruit::kMaxAge;
        }

        // CocoaBlock.randomTick's rate (1 in 5 per random tick) with no light
        // gate: the Hush is dark, and a fruit that needed light 9 there would
        // never ripen.
        void FruitRandomTick(ILevelWrite& level, const glm::ivec3& pos,
                             BlockState state, JavaRandom& random) {
            if (random.NextInt(5) != 0) return;
            const int age = AgeOf(state);
            if (age < Whisperfruit::kMaxAge) {
                level.SetBlock(pos.x, pos.y, pos.z, FruitAtAge(age + 1),
                               World::UpdateFlags::UpdateClients);
            }
        }

        bool FruitIsValidBonemealTarget(const IBlockAccess& level, const glm::ivec3& pos,
                                        BlockState state) {
            (void)level; (void)pos;
            return AgeOf(state) < Whisperfruit::kMaxAge;
        }

        void FruitPerformBonemeal(ILevelWrite& level, const glm::ivec3& pos,
                                  BlockState state, JavaRandom& /*random*/) {
            level.SetBlock(pos.x, pos.y, pos.z, FruitAtAge(AgeOf(state) + 1),
                           World::UpdateFlags::UpdateClients);
        }

        // Picked when ripe: 1-2 fruit (SweetBerryBushBlock's `1 + nextInt(2)`
        // at its top age) and back to a bud. Unripe, the click is not ours.
        // Predicted on both sides like the Aether berry bush (the state), the
        // drop and the sound are the server's.
        UseResult FruitUseWithoutItem(ILevelWrite* world, const glm::ivec3& pos,
                                      IUsePlayer* /*player*/, const BlockHitResult& /*hit*/) {
            if (!world) return UseResult::Pass;
            const BlockState state = world->GetBlockState(pos.x, pos.y, pos.z);
            if (!state.Is(BlockID::HangingWhisperfruit) || AgeOf(state) < Whisperfruit::kMaxAge) {
                return UseResult::Pass;
            }
            if (!world->IsClientSide()) {
                int count = 1;
                float pitch = 1.0f;
                if (JavaRandom* rng = world->Random()) {
                    count += rng->NextInt(2);
                    pitch = 0.8f + rng->NextFloat() * 0.4f;
                }
                DropItemStackNear(world->GetDimension(), pos, ItemStack(Items::Whisperfruit, count));
                // As SweetBerryBushBlock.useWithoutItem:110 — playSound(null,
                // pos, SWEET_BERRY_BUSH_PICK_BERRIES, BLOCKS, 1.0, 0.8..1.2).
                world->PlaySound(nullptr, pos, SoundEvents::SWEET_BERRY_BUSH_PICK_BERRIES, SoundSource::Blocks,
                                 1.0f, pitch);
            }
            world->SetBlock(pos.x, pos.y, pos.z, FruitAtAge(0), World::UpdateFlags::All);
            return UseResult::Success;
        }

        // Hangs from a lantern leaf and nothing else: when the block ABOVE
        // stops being one, the fruit falls (becomes air, which the world
        // turns into a destroy with drops — CocoaBlock.updateShape's shape).
        bool FruitUpdateShape(const IBlockAccess& /*level*/, const glm::ivec3& /*pos*/,
                              BlockState /*state*/, Direction toNeighbour, BlockID neighbourId,
                              BlockState& outState, ScheduledTickAccess* /*ticks*/) {
            if (toNeighbour != Direction::Up || neighbourId == BlockID::LanternLeaves) return false;
            outState = BlockState{};
            return true;
        }

        // ── echo_heart ───────────────────────────────────────────────────

        struct HeartEntry {
            DimensionId dim;
            glm::ivec3  pos;
            int64_t     expires;   // game time; renewed by every heart tick
        };

        std::mutex& HeartsMutex() {
            static std::mutex m;
            return m;
        }
        std::vector<HeartEntry>& Hearts() {
            static std::vector<HeartEntry> hearts;
            return hearts;
        }

        // Renew (or add) this heart and drop every entry that has lapsed.
        // An entry outlives three missed ticks, so a busy server that runs a
        // heart's tick a little late never opens a spawn window.
        void RenewHeart(DimensionId dim, const glm::ivec3& pos, int64_t now) {
            std::lock_guard<std::mutex> lock(HeartsMutex());
            auto& hearts = Hearts();
            hearts.erase(std::remove_if(hearts.begin(), hearts.end(),
                                        [&](const HeartEntry& h) { return h.expires < now; }),
                         hearts.end());
            const int64_t expires = now + 3 * EchoHeart::kPushTicks;
            for (HeartEntry& h : hearts) {
                if (h.dim == dim && h.pos == pos) { h.expires = expires; return; }
            }
            hearts.push_back({ dim, pos, expires });
        }

        void ForgetHeart(DimensionId dim, const glm::ivec3& pos) {
            std::lock_guard<std::mutex> lock(HeartsMutex());
            auto& hearts = Hearts();
            hearts.erase(std::remove_if(hearts.begin(), hearts.end(),
                                        [&](const HeartEntry& h) { return h.dim == dim && h.pos == pos; }),
                         hearts.end());
        }

        void BookHeartTick(ILevelWrite& level, const glm::ivec3& pos, int delay) {
            if (ScheduledTickAccess* ticks = level.Ticks()) {
                ticks->ScheduleTick(pos, BlockID::EchoHeart, delay);
            }
        }

        void HeartOnPlace(ILevelWrite& level, const glm::ivec3& pos, BlockState newState,
                          BlockState oldState, bool /*movedByPiston*/) {
            if (!newState.Is(BlockID::EchoHeart) || oldState.Is(BlockID::EchoHeart)) return;
            BookHeartTick(level, pos, 1);
        }

        bool HeartIsRandomlyTicking(BlockState /*state*/) { return true; }

        // The safety net: a heart whose scheduled tick was lost (a chunk
        // written by something that does not save block_ticks) restarts its
        // clock on the next random tick instead of staying dark forever.
        void HeartRandomTick(ILevelWrite& level, const glm::ivec3& pos,
                             BlockState /*state*/, JavaRandom& /*random*/) {
            ScheduledTickAccess* ticks = level.Ticks();
            if (ticks && !ticks->HasScheduledTick(pos, BlockID::EchoHeart)) {
                ticks->ScheduleTick(pos, BlockID::EchoHeart, 1);
            }
        }

        bool IsHushHostile(const Entity& e) {
            return e.GetType() == EntityTypeId::EchoWraith || e.GetType() == EntityTypeId::EchoMimic;
        }

        void HeartTick(ILevelWrite& level, const glm::ivec3& pos, BlockState state, JavaRandom&) {
            if (!state.Is(BlockID::EchoHeart)) {
                ForgetHeart(level.GetDimension(), pos);
                return;
            }
            const int64_t now = level.GameTime();
            RenewHeart(level.GetDimension(), pos, now);
            BookHeartTick(level, pos, EchoHeart::kPushTicks);

            EntityLevel* entities = level.Entities();
            if (!entities) return;
            const glm::dvec3 centre(pos.x + 0.5, pos.y + 0.5, pos.z + 0.5);
            const double r = static_cast<double>(EchoHeart::kRadius);

            // The beacon half: every kPulseTicks, the players in range get
            // Regeneration I and Resistance I, ambient (the beacon's quiet
            // particles). The heart ticks every kPushTicks, so exactly one of
            // its ticks in each pulse window lands on the pulse.
            if (now % EchoHeart::kPulseTicks < EchoHeart::kPushTicks) {
                std::vector<LivingEntity*> players;
                entities->GetPlayers(players);
                for (LivingEntity* p : players) {
                    if (!p || !p->IsAlive()) continue;
                    const glm::dvec3 d = p->position - centre;
                    if (std::abs(d.x) > r || std::abs(d.y) > r || std::abs(d.z) > r) continue;
                    p->AddEffect(MobEffectInstance(MobEffectId::Regeneration,
                                                   EchoHeart::kEffectTicks, 0, true, true));
                    p->AddEffect(MobEffectInstance(MobEffectId::Resistance,
                                                   EchoHeart::kEffectTicks, 0, true, true));
                }
            }

            // The ward half: the Hush's hostiles are thrown back out of the
            // radius, the same shove a hit gives (LivingEntity::Knockback,
            // pointed from the heart), a touch stronger the closer they are.
            std::vector<Entity*> nearby;
            entities->GetEntitiesInBox(
                AABB::FromMinMax(glm::vec3(centre - glm::dvec3(r)), glm::vec3(centre + glm::dvec3(r))),
                nullptr, nearby);
            for (Entity* e : nearby) {
                if (!e || e->IsRemoved() || !IsHushHostile(*e)) continue;
                auto* living = dynamic_cast<LivingEntity*>(e);
                if (!living || !living->IsAlive()) continue;
                glm::dvec3 away = living->position - centre;
                away.y = 0.0;
                const double dist = glm::length(away);
                if (dist < 1.0e-4) away = glm::dvec3(1.0, 0.0, 0.0);
                const double power = 0.6 + 0.6 * (1.0 - std::min(dist, r) / r);
                // Knockback's dx/dz point FROM the pusher TOWARD the victim's
                // origin (MC passes attackerX - myX): the heart minus the mob.
                living->Knockback(power, -away.x, -away.z);
            }
        }

        // The light column: teal motes rising from the top face, thicker
        // near the heart and thinning as they climb (the beam's stand-in —
        // no beam renderer exists).
        void HeartAnimateTick(EntityLevel& level, const glm::ivec3& pos,
                              BlockState /*state*/, JavaRandom& random) {
            for (int i = 0; i < 3; ++i) {
                const double x = pos.x + 0.5 + (random.NextDouble() - 0.5) * 0.35;
                const double z = pos.z + 0.5 + (random.NextDouble() - 0.5) * 0.35;
                const double y = pos.y + 1.05 + random.NextDouble() * (i == 0 ? 0.5 : 4.0);
                level.AddParticle(ParticleKind::HushMote, x, y, z,
                                  0.0, 0.06 + random.NextDouble() * 0.04, 0.0);
            }
        }

        // The ripe fruit sheds the odd mote, so a whisperwood grove shows
        // where the harvest is.
        void FruitAnimateTick(EntityLevel& level, const glm::ivec3& pos,
                              BlockState state, JavaRandom& random) {
            if (AgeOf(state) < Whisperfruit::kMaxAge || random.NextInt(8) != 0) return;
            level.AddParticle(ParticleKind::HushMote,
                              pos.x + 0.3 + random.NextDouble() * 0.4, pos.y + 0.4,
                              pos.z + 0.3 + random.NextDouble() * 0.4,
                              0.0, -0.01, 0.0);
        }

        // ── hush_lighthouse_lamp ─────────────────────────────────────────
        // Moths to a flame: now and then a mote drifts in toward the lens
        // from a block or so out and settles on it. The beams themselves are
        // HushLighthouseRenderer's (client/renderer/blockentity).
        void LampAnimateTick(EntityLevel& level, const glm::ivec3& pos,
                             BlockState /*state*/, JavaRandom& random) {
            if (random.NextInt(3) != 0) return;
            const double angle = random.NextDouble() * 6.283185307179586;
            const double r = 0.9 + random.NextDouble() * 0.6;
            const double dx = std::cos(angle) * r;
            const double dz = std::sin(angle) * r;
            level.AddParticle(ParticleKind::HushMote,
                              pos.x + 0.5 + dx, pos.y + 0.3 + random.NextDouble() * 0.5, pos.z + 0.5 + dz,
                              -dx * 0.02, 0.005, -dz * 0.02);
        }

    } // namespace

    namespace EchoHeart {
        bool SuppressesHostileSpawn(DimensionId dimension, const glm::ivec3& pos, int64_t gameTime) {
            std::lock_guard<std::mutex> lock(HeartsMutex());
            for (const HeartEntry& h : Hearts()) {
                if (h.dim != dimension || h.expires < gameTime) continue;
                const glm::ivec3 d = pos - h.pos;
                if (std::abs(d.x) <= kRadius && std::abs(d.y) <= kRadius && std::abs(d.z) <= kRadius) {
                    return true;
                }
            }
            return false;
        }
    }

    void BlockRegistry_RegisterHushBlocks(std::array<Block, BlockRegistry::Size>& blocks) {
        Block& fruit = blocks[static_cast<size_t>(BlockID::HangingWhisperfruit)];
        fruit.isRandomlyTicking     = &FruitIsRandomlyTicking;
        fruit.randomTick            = &FruitRandomTick;
        fruit.isValidBonemealTarget = &FruitIsValidBonemealTarget;
        fruit.performBonemeal       = &FruitPerformBonemeal;
        fruit.useWithoutItem        = &FruitUseWithoutItem;
        fruit.updateShape           = &FruitUpdateShape;
        fruit.animateTick           = &FruitAnimateTick;
        fruit.emissive              = true;

        Block& heart = blocks[static_cast<size_t>(BlockID::EchoHeart)];
        heart.onPlace           = &HeartOnPlace;
        heart.tick              = &HeartTick;
        heart.isRandomlyTicking = &HeartIsRandomlyTicking;
        heart.randomTick        = &HeartRandomTick;
        heart.animateTick       = &HeartAnimateTick;
        heart.emissive          = true;

        // The lighthouse lamp: full-bright (there is no block light engine,
        // so the lens glowing is all the light it gives), and motes.
        Block& lamp = blocks[static_cast<size_t>(BlockID::HushLighthouseLamp)];
        lamp.animateTick = &LampAnimateTick;
        lamp.emissive    = true;
    }

} // namespace Game
