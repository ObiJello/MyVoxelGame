// File: src/common/world/block/SculkBlocks.cpp
#include "SculkBlocks.hpp"

#include "BlockPlacement.hpp"        // IsAmethystClusterBlock
#include "GeneratedBlockStates.hpp"
#include "RedstoneStateUtil.hpp"
#include "entity/SculkBlockEntities.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/entity/Entity.hpp"
#include "common/sound/SoundEvents.hpp"
#include "common/world/level/ILevelWrite.hpp"
#include "common/world/level/World.hpp"
#include "common/world/level/gameevent/GameEvent.hpp"
#include "common/world/level/gameevent/VibrationSystem.hpp"
#include "common/world/ticks/ScheduledTickAccess.hpp"

#include <array>
#include <cmath>

namespace Game {

    namespace {

        constexpr uint32_t kUpdateAll = 3;
        // MC setBlock(pos, state, 18): UPDATE_CLIENTS | UPDATE_KNOWN_SHAPE.
        constexpr uint32_t kUpdateClientsKnownShape = 2 | 16;
        constexpr uint32_t kUpdateClients = 2;

        constexpr std::array<Direction, 6> kDirections = {
            Direction::Down, Direction::Up, Direction::North,
            Direction::South, Direction::West, Direction::East,
        };

        // MC SculkSensorBlock.RESONANCE_PITCH_BEND: NoteBlock.getPitchFromNote
        // (2^((note - 12) / 12)) over the resonance tone map.
        const std::array<float, 16>& ResonancePitchBend() {
            static const std::array<float, 16> table = [] {
                constexpr int toneMap[16] = {0, 0, 2, 4, 6, 7, 9, 10, 12, 14, 15, 18, 19, 21, 22, 24};
                std::array<float, 16> t{};
                for (int i = 0; i < 16; ++i) {
                    t[static_cast<size_t>(i)] =
                        static_cast<float>(std::pow(2.0, static_cast<double>(toneMap[i] - 12) / 12.0));
                }
                return t;
            }();
            return table;
        }

        float NextPitch(ILevelWrite& level, float base, float spread) {
            JavaRandom* r = level.Random();
            return base + (r ? r->NextFloat() : 0.5f) * spread;
        }

        bool Waterlogged(BlockState s) { return BoolOf(s, PropertyId::WATERLOGGED); }

        // MC SculkSensorBlock.updateNeighbours: the block's neighbours, and
        // the neighbours of the block below (the strong-powered one).
        void UpdateSensorNeighbours(ILevelWrite& level, const glm::ivec3& pos, BlockState state) {
            level.UpdateNeighborsAt(pos, state.Block());
            level.UpdateNeighborsAt(Below(pos), state.Block());
        }

        World* ServerWorld(ILevelWrite& level) {
            return level.IsClientSide() ? nullptr : dynamic_cast<World*>(&level);
        }

        // ── Sculk sensor hooks ──────────────────────────────────────────────

        // MC SculkSensorBlock.tick.
        void SensorTick(ILevelWrite& level, const glm::ivec3& pos, BlockState state, JavaRandom& random) {
            (void)random;
            const SculkSensor::Phase phase = SculkSensor::PhaseOf(state);
            if (phase != SculkSensor::Phase::Active) {
                if (phase == SculkSensor::Phase::Cooldown) {
                    level.SetBlock(pos.x, pos.y, pos.z, SculkSensor::WithPhase(state, SculkSensor::Phase::Inactive),
                                   kUpdateAll);
                    if (!Waterlogged(state)) {
                        level.PlaySound(SoundExcept(nullptr), pos, SoundEvents::SCULK_CLICKING_STOP,
                                        SoundSource::Blocks, 1.0f, NextPitch(level, 0.8f, 0.2f));
                    }
                }
                return;
            }
            SculkSensor::Deactivate(level, pos, state);
        }

        // MC SculkSensorBlock.stepOn: an entity walking on an idle sensor is
        // felt at once (forceScheduleVibration skips the listener's checks),
        // unless it is a warden.
        void SensorStepOn(ILevelWrite& level, const glm::ivec3& pos, BlockState onState, Entity& entity) {
            World* world = ServerWorld(level);
            if (!world || !SculkSensor::CanActivate(onState) || entity.GetType() == EntityTypeId::Warden) return;
            auto* sensor = dynamic_cast<SculkSensorBlockEntity*>(level.GetBlockEntity(pos));
            if (!sensor) return;
            if (sensor->GetVibrationUser().CanReceiveVibration(*world, pos, GameEventId::Step,
                                                               GameEventContext::Of(onState))) {
                sensor->GetListener().ForceScheduleVibration(*world, GameEventId::Step,
                                                             GameEventContext::Of(&entity), entity.position);
            }
        }

        // MC SculkSensorBlock.onPlace: a sensor placed carrying power (a
        // structure, a piston) with no tick to clear it drops to 0.
        void SensorOnPlace(ILevelWrite& level, const glm::ivec3& pos, BlockState newState, BlockState oldState,
                           bool /*movedByPiston*/) {
            if (level.IsClientSide() || newState.Block() == oldState.Block()) return;
            if (PowerOf(newState) <= 0) return;
            ScheduledTickAccess* ticks = level.Ticks();
            if (ticks && ticks->HasScheduledTick(pos, newState.Block())) return;
            level.SetBlock(pos.x, pos.y, pos.z, WithPower(newState, 0), kUpdateClientsKnownShape);
        }

        // MC SculkSensorBlock.affectNeighborsAfterRemoval: an active sensor
        // taken away un-powers what it fed.
        void SensorAffectNeighborsAfterRemoval(ILevelWrite& level, const glm::ivec3& pos, BlockState state,
                                               bool /*movedByPiston*/) {
            if (SculkSensor::PhaseOf(state) == SculkSensor::Phase::Active) UpdateSensorNeighbours(level, pos, state);
        }

        // MC SculkSensorBlock.getSignal (ownSignal: POWER in every direction)
        // and getDirectSignal (only toward UP — vanilla's direction points
        // from the asking block, so the one strong-powered is the block
        // BELOW the sensor, which is why updateNeighbours also wakes its
        // neighbours).
        int SensorGetSignal(const IBlockAccess&, const glm::ivec3&, BlockState state, Direction) {
            return PowerOf(state);
        }
        int SensorGetDirectSignal(const IBlockAccess& level, const glm::ivec3& pos, BlockState state,
                                  Direction direction) {
            return direction == Direction::Up ? SensorGetSignal(level, pos, state, direction) : 0;
        }

        // MC CalibratedSculkSensorBlock.getSignal: dark toward its facing
        // (its input side is the opposite; the facing side is its front).
        int CalibratedGetSignal(const IBlockAccess&, const glm::ivec3&, BlockState state, Direction direction) {
            return direction != HorizontalFacingOf(state) ? PowerOf(state) : 0;
        }
        int CalibratedGetDirectSignal(const IBlockAccess& level, const glm::ivec3& pos, BlockState state,
                                      Direction direction) {
            return direction == Direction::Up ? CalibratedGetSignal(level, pos, state, Direction::Up) : 0;
        }

        // MC SculkSensorBlock.getAnalogOutputSignal: while ACTIVE, the
        // frequency of the vibration that activated it.
        int SensorAnalogOutput(ILevelWrite& level, const glm::ivec3& pos, BlockState state, Direction) {
            auto* sensor = dynamic_cast<SculkSensorBlockEntity*>(level.GetBlockEntity(pos));
            if (!sensor) return 0;
            return SculkSensor::PhaseOf(state) == SculkSensor::Phase::Active ? sensor->GetLastVibrationFrequency() : 0;
        }

        // ── Sculk shrieker hooks ────────────────────────────────────────────

        // MC SculkShriekerBlock.stepOn: a player on it tries to shriek.
        void ShriekerStepOn(ILevelWrite& level, const glm::ivec3& pos, BlockState /*onState*/, Entity& entity) {
            World* world = ServerWorld(level);
            if (!world) return;
            LivingEntity* player = SculkShriekerBlockEntity::TryGetPlayer(&entity);
            if (!player) return;
            if (auto* shrieker = dynamic_cast<SculkShriekerBlockEntity*>(level.GetBlockEntity(pos))) {
                shrieker->TryShriek(*world, player);
            }
        }

        // MC SculkShriekerBlock.tick: the 90-tick shriek ends — and the
        // shrieker answers (a warden reply or a summon, and darkness).
        void ShriekerTick(ILevelWrite& level, const glm::ivec3& pos, BlockState state, JavaRandom& /*random*/) {
            if (!BoolOf(state, PropertyId::SHRIEKING)) return;
            level.SetBlock(pos.x, pos.y, pos.z, WithBool(state, PropertyId::SHRIEKING, false), kUpdateAll);
            World* world = ServerWorld(level);
            if (!world) return;
            if (auto* shrieker = dynamic_cast<SculkShriekerBlockEntity*>(level.GetBlockEntity(pos))) {
                shrieker->TryRespond(*world);
            }
        }

        // ── Sculk catalyst hooks ────────────────────────────────────────────

        // MC SculkCatalystBlock.tick: the bloom's 8 ticks are up.
        void CatalystTick(ILevelWrite& level, const glm::ivec3& pos, BlockState state, JavaRandom& /*random*/) {
            if (BoolOf(state, PropertyId::BLOOM)) {
                level.SetBlock(pos.x, pos.y, pos.z, WithBool(state, PropertyId::BLOOM, false), kUpdateAll);
            }
        }

        // ── Amethyst ────────────────────────────────────────────────────────

        // MC AmethystBlock.onProjectileHit (the block, the budding block and
        // every AmethystClusterBlock): the hit and the chime.
        void AmethystOnProjectileHit(ILevelWrite& level, const glm::ivec3& pos, BlockState /*state*/,
                                     const glm::dvec3& /*hitPos*/, Direction /*face*/, Entity& /*projectile*/) {
            if (level.IsClientSide()) return;
            level.PlaySound(SoundExcept(nullptr), pos, SoundEvents::AMETHYST_BLOCK_HIT, SoundSource::Blocks,
                            1.0f, NextPitch(level, 0.5f, 1.2f));
            level.PlaySound(SoundExcept(nullptr), pos, SoundEvents::AMETHYST_BLOCK_CHIME, SoundSource::Blocks,
                            1.0f, NextPitch(level, 0.5f, 1.2f));
        }

    } // namespace

    namespace SculkSensor {

        bool IsSculkSensor(BlockID block) {
            return block == BlockID::SculkSensor || block == BlockID::CalibratedSculkSensor;
        }

        Phase PhaseOf(BlockState state) {
            const int i = state.GetIndex(PropertyId::SCULK_SENSOR_PHASE);
            return i < 0 ? Phase::Inactive : static_cast<Phase>(i);
        }

        BlockState WithPhase(BlockState state, Phase phase) {
            return state.SetIndex(PropertyId::SCULK_SENSOR_PHASE, static_cast<int>(phase));
        }

        bool CanActivate(BlockState state) {
            return IsSculkSensor(state.Block()) && PhaseOf(state) == Phase::Inactive;
        }

        int ActiveTicks(BlockID block) {
            return block == BlockID::CalibratedSculkSensor ? kCalibratedActiveTicks : kActiveTicks;
        }

        void Activate(Entity* sourceEntity, ILevelWrite& level, const glm::ivec3& pos, BlockState state,
                      int calculatedPower, int vibrationFrequency) {
            level.SetBlock(pos.x, pos.y, pos.z, WithPower(WithPhase(state, Phase::Active), calculatedPower),
                           kUpdateAll);
            if (ScheduledTickAccess* ticks = level.Ticks()) {
                ticks->ScheduleTick(pos, state.Block(), ActiveTicks(state.Block()));
            }
            UpdateSensorNeighbours(level, pos, state);
            TryResonateVibration(sourceEntity, level, pos, vibrationFrequency);
            level.GameEvent(sourceEntity, GameEventId::SculkSensorTendrilsClicking, pos);
            if (!Waterlogged(state)) {
                level.PlaySound(SoundExcept(nullptr), glm::dvec3(pos.x + 0.5, pos.y + 0.5, pos.z + 0.5),
                                SoundEvents::SCULK_CLICKING, SoundSource::Blocks, 1.0f, NextPitch(level, 0.8f, 0.2f));
            }
        }

        void Deactivate(ILevelWrite& level, const glm::ivec3& pos, BlockState state) {
            level.SetBlock(pos.x, pos.y, pos.z, WithPower(WithPhase(state, Phase::Cooldown), 0), kUpdateAll);
            if (ScheduledTickAccess* ticks = level.Ticks()) {
                ticks->ScheduleTick(pos, state.Block(), kCooldownTicks);
            }
            UpdateSensorNeighbours(level, pos, state);
        }

        void TryResonateVibration(Entity* sourceEntity, ILevelWrite& level, const glm::ivec3& pos,
                                  int vibrationFrequency) {
            if (vibrationFrequency < 1 || vibrationFrequency > 15) return;
            for (Direction direction : kDirections) {
                const glm::ivec3 relativePos = Relative(pos, direction);
                const BlockState blockState = level.GetBlockState(relativePos.x, relativePos.y, relativePos.z);
                if (!Vibrations::IsVibrationResonator(blockState.Block())) continue;
                level.GameEvent(GameEvents::ResonanceEventForFrequency(vibrationFrequency), relativePos,
                                GameEventContext::Of(sourceEntity, blockState));
                const float pitch = ResonancePitchBend()[static_cast<size_t>(vibrationFrequency)];
                level.PlaySound(SoundExcept(nullptr), relativePos, SoundEvents::AMETHYST_BLOCK_RESONATE,
                                SoundSource::Blocks, 1.0f, pitch);
            }
        }

    } // namespace SculkSensor

    void RegisterSculkBehaviors(std::array<Block, BlockRegistry::Size>& blocks) {
        auto at = [&blocks](BlockID id) -> Block& { return blocks[static_cast<size_t>(id)]; };

        for (BlockID id : {BlockID::SculkSensor, BlockID::CalibratedSculkSensor}) {
            Block& b = at(id);
            b.tick                        = &SensorTick;
            b.stepOn                      = &SensorStepOn;
            b.onPlace                     = &SensorOnPlace;
            b.affectNeighborsAfterRemoval = &SensorAffectNeighborsAfterRemoval;
            b.getAnalogOutputSignal       = &SensorAnalogOutput;
            b.isSignalSource              = true;
            b.hasAnalogOutputSignal       = true;
        }
        at(BlockID::SculkSensor).getSignal                 = &SensorGetSignal;
        at(BlockID::SculkSensor).getDirectSignal           = &SensorGetDirectSignal;
        at(BlockID::CalibratedSculkSensor).getSignal       = &CalibratedGetSignal;
        at(BlockID::CalibratedSculkSensor).getDirectSignal = &CalibratedGetDirectSignal;

        Block& shrieker = at(BlockID::SculkShrieker);
        shrieker.stepOn = &ShriekerStepOn;
        shrieker.tick   = &ShriekerTick;

        at(BlockID::SculkCatalyst).tick = &CatalystTick;

        // AmethystBlock and its subclasses (BuddingAmethystBlock,
        // AmethystClusterBlock — the cluster and the three buds).
        for (size_t i = 0; i < BlockRegistry::Size; ++i) {
            const BlockID id = static_cast<BlockID>(i);
            if (id == BlockID::AmethystBlock || id == BlockID::BuddingAmethyst ||
                (IsAmethystClusterBlock(id) && blocks[i].registrySlug.find("amethyst") != std::string::npos)) {
                if (!blocks[i].onProjectileHit) blocks[i].onProjectileHit = &AmethystOnProjectileHit;
            }
        }
    }

} // namespace Game
