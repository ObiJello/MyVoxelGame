// File: src/common/sound/PlayerMovementSounds.cpp
#include "common/sound/PlayerMovementSounds.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/sound/SoundEvents.hpp"
#include "common/sound/SoundType.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/tags/DataTags.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace Game {

    namespace {

        // The four block tags the step rules read, per BlockID, resolved once
        // from the data pack (data/minecraft/tags/block/*.json).
        enum TagBits : uint8_t {
            kClimbable            = 1 << 0,   // #minecraft:climbable
            kInsideStepSound      = 1 << 1,   // #minecraft:inside_step_sound_blocks
            kCombinationStepSound = 1 << 2,   // #minecraft:combination_step_sound_blocks
            kCrystalSound         = 1 << 3,   // #minecraft:crystal_sound_blocks
        };

        const std::vector<uint8_t>& TagTable() {
            static const std::vector<uint8_t> table = [] {
                std::vector<uint8_t> t(BlockRegistry::Size, 0);
                for (size_t i = 0; i < t.size(); ++i) {
                    const std::string& slug = BlockRegistry::Get(static_cast<BlockID>(i)).registrySlug;
                    for (const std::string& tag : DataTags::TagsFor(DataTags::Registry::Block, slug)) {
                        if (tag == "#minecraft:climbable")                      t[i] |= kClimbable;
                        else if (tag == "#minecraft:inside_step_sound_blocks")  t[i] |= kInsideStepSound;
                        else if (tag == "#minecraft:combination_step_sound_blocks") t[i] |= kCombinationStepSound;
                        else if (tag == "#minecraft:crystal_sound_blocks")      t[i] |= kCrystalSound;
                    }
                }
                return t;
            }();
            return table;
        }

        bool HasTag(BlockID block, uint8_t bit) {
            const auto& t = TagTable();
            const size_t i = static_cast<size_t>(block);
            return i < t.size() && (t[i] & bit) != 0;
        }

        glm::ivec3 BlockContaining(double x, double y, double z) {
            return glm::ivec3(static_cast<int>(std::floor(x)), static_cast<int>(std::floor(y)),
                              static_cast<int>(std::floor(z)));
        }

        void Emit(std::vector<PlayerMovementSound>& out, const char* event, float volume, float pitch) {
            if (!IsEmptySound(event)) out.push_back({event, volume, pitch});
        }

        // MC Entity.playStepSound (the base): the block's step sound at 0.15.
        void BaseStepSound(BlockState state, std::vector<PlayerMovementSound>& out) {
            const SoundType& st = SoundTypeOf(state);
            Emit(out, st.GetStepSound(), st.GetVolume() * 0.15f, st.GetPitch());
        }

        // MC Entity.playMuffledStepSound.
        void MuffledStepSound(BlockState state, std::vector<PlayerMovementSound>& out) {
            const SoundType& st = SoundTypeOf(state);
            Emit(out, st.GetStepSound(), st.GetVolume() * 0.05f, st.GetPitch() * 0.8f);
        }

        // MC Entity.waterSwimSound with Player.getSwimSound (PLAYER_SWIM).
        void WaterSwimSound(const glm::dvec3& movement, JavaRandom& random,
                            std::vector<PlayerMovementSound>& out) {
            const float speed = std::min(1.0f, static_cast<float>(std::sqrt(
                movement.x * movement.x * 0.2 + movement.y * movement.y + movement.z * movement.z * 0.2)) * 0.35f);
            Emit(out, SoundEvents::PLAYER_SWIM, speed, 1.0f + (random.NextFloat() - random.NextFloat()) * 0.4f);
        }

    } // namespace

    void PlayerMovementSounds::Reset() {
        m_moveDist = 0.0f;
        m_nextStep = 1.0f;
        m_firstTick = true;
        m_wasTouchingWater = false;
    }

    void PlayerMovementSounds::Tick(const IBlockAccess& blocks, const Input& in, JavaRandom& random,
                                    std::vector<PlayerMovementSound>& out) {
        ++m_tickCount;
        const glm::dvec3 movement = in.position - in.previousPosition;

        // A jump across the world (a teleport, a respawn) is not a step.
        if (glm::dot(movement, movement) > 10.0 * 10.0 || in.noPhysics) {
            m_wasTouchingWater = in.inWater;
            m_firstTick = false;
            return;
        }

        // MC Entity.updateFluidInteraction: entering water splashes
        // (doWaterSplashEffect, Player's PLAYER_SPLASH / _HIGH_SPEED).
        if (in.inWater && !m_wasTouchingWater && !m_firstTick) {
            const float speed = std::min(1.0f, static_cast<float>(std::sqrt(
                movement.x * movement.x * 0.2 + movement.y * movement.y + movement.z * movement.z * 0.2)) * 0.2f);
            const float pitch = 1.0f + (random.NextFloat() - random.NextFloat()) * 0.4f;
            Emit(out, speed < 0.25f ? SoundEvents::PLAYER_SPLASH : SoundEvents::PLAYER_SPLASH_HIGH_SPEED,
                 speed, pitch);
        }
        m_wasTouchingWater = in.inWater;
        m_firstTick = false;

        // MC Player.getMovementEmission: NONE while flying, and while
        // sneaking on the ground — a sneak is silent.
        if (in.flying || (in.onGround && in.crouching)) return;

        // MC applyMovementEmissionAndPlaySound.
        const float movedDistance = static_cast<float>(glm::length(movement) * 0.6000000238418579);
        const float horizontalMovedDistance = static_cast<float>(
            std::sqrt(movement.x * movement.x + movement.z * movement.z) * 0.6000000238418579);
        // getOnPos / getOnPosLegacy: the block 0.2 under the feet.
        const glm::ivec3 onPos = BlockContaining(in.position.x, in.position.y - 0.2, in.position.z);
        const BlockState onState = blocks.GetBlockState(onPos.x, onPos.y, onPos.z);
        const bool onAir = onState.Block() == BlockID::Air;
        const bool climbing = HasTag(onState.Block(), kClimbable) || onState.Block() == BlockID::PowderSnow;
        m_moveDist += climbing ? movedDistance : horizontalMovedDistance;

        if (!(m_moveDist > m_nextStep) || onAir) return;   // (flapping: players do not flap)

        // vibrationAndSoundEffectsFromBlock(effectPos == supportingPos).
        bool produced = false;
        if ((in.onGround || climbing || (in.crouching && movement.y == 0.0)) && !in.swimming) {
            produced = true;
            // walkingStepSound → Player.playStepSound.
            if (in.inWater) {
                WaterSwimSound(movement, random, out);
                MuffledStepSound(onState, out);
            } else {
                // getPrimaryStepSoundBlockPos: a thin covering on top (carpet,
                // snow layer, petals...) is what the foot actually lands on.
                const BlockState above = blocks.GetBlockState(onPos.x, onPos.y + 1, onPos.z);
                const BlockID aboveBlock = above.Block();
                if (HasTag(aboveBlock, kInsideStepSound) || HasTag(aboveBlock, kCombinationStepSound)) {
                    if (HasTag(aboveBlock, kCombinationStepSound)) {
                        // playCombinationStepSounds: the covering, plus the
                        // floor under it muffled.
                        const SoundType& st = SoundTypeOf(above);
                        Emit(out, st.GetStepSound(), st.GetVolume() * 0.15f, st.GetPitch());
                        MuffledStepSound(onState, out);
                    } else {
                        BaseStepSound(above, out);
                    }
                } else {
                    BaseStepSound(onState, out);
                }
            }
            // shouldPlayAmethystStepSound / playAmethystStepSound.
            if (HasTag(onState.Block(), kCrystalSound) && m_tickCount >= m_lastCrystalSoundPlayTick + 20) {
                m_crystalSoundIntensity *= static_cast<float>(
                    std::pow(0.997, static_cast<double>(m_tickCount - m_lastCrystalSoundPlayTick)));
                m_crystalSoundIntensity = std::min(1.0f, m_crystalSoundIntensity + 0.07f);
                const float pitch = 0.5f + m_crystalSoundIntensity * random.NextFloat() * 1.2f;
                const float volume = 0.1f + m_crystalSoundIntensity * 1.2f;
                Emit(out, SoundEvents::AMETHYST_BLOCK_CHIME, volume, pitch);
                m_lastCrystalSoundPlayTick = m_tickCount;
            }
        }

        if (produced) {
            m_nextStep = static_cast<float>(static_cast<int>(m_moveDist) + 1);
        } else if (in.inWater) {
            m_nextStep = static_cast<float>(static_cast<int>(m_moveDist) + 1);
            WaterSwimSound(movement, random, out);
        }
    }

    void PlayerMovementSounds::Landing(const IBlockAccess& blocks, const glm::dvec3& feet, float fallDistance,
                                       float scale, float safeFallDistance,
                                       std::vector<PlayerMovementSound>& out) {
        // LivingEntity.calculateFallDamage as the server applies it
        // (PlayerSession::UpdateMovementStats): the fall in body heights.
        const float fd = std::min(fallDistance, 512.0f) / std::max(scale, 0.05f);
        const int damage = static_cast<int>(std::floor(fd + 1.0e-6f - safeFallDistance));
        if (damage <= 0) return;
        // getFallDamageSound: Player.getFallSounds — big above 4 damage.
        Emit(out, damage > 4 ? SoundEvents::PLAYER_BIG_FALL : SoundEvents::PLAYER_SMALL_FALL, 1.0f, 1.0f);
        // playBlockFallSound: the block 0.2 under the feet.
        const glm::ivec3 pos = BlockContaining(feet.x, feet.y - 0.2, feet.z);
        const BlockState state = blocks.GetBlockState(pos.x, pos.y, pos.z);
        if (state.Block() != BlockID::Air) {
            const SoundType& st = SoundTypeOf(state);
            Emit(out, st.GetFallSound(), st.GetVolume() * 0.5f, st.GetPitch() * 0.75f);
        }
    }

} // namespace Game
