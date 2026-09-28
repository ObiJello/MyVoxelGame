// File: src/common/world/block/TurtleEggBlock.cpp
//
// See TurtleEggBlock.hpp. Every function names the MC method it ports.
#include "common/world/block/TurtleEggBlock.hpp"
#include "common/world/level/gameevent/GameEvent.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/entity/Entity.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/LivingEntity.hpp"
#include "common/entity/mobs/Animals.hpp"
#include "common/sound/LevelEventSounds.hpp"
#include "common/sound/SoundEvents.hpp"
#include "common/world/level/DimensionId.hpp"
#include "common/world/level/ILevelWrite.hpp"
#include "common/world/level/World.hpp"

#include <memory>

namespace Game {

    namespace {

        int EggsOf(BlockState state) {
            const int index = state.GetIndex(PropertyId::EGGS);   // values run 1..4
            return index < 0 ? 1 : index + 1;
        }

        int HatchOf(BlockState state) {
            const int index = state.GetIndex(PropertyId::HATCH);  // values run 0..2
            return index < 0 ? 0 : index;
        }

        // The level's own random (MC level.getRandom()), or the tick's where
        // the level offers none.
        JavaRandom& LevelRandom(ILevelWrite& level, JavaRandom& fallback) {
            JavaRandom* r = level.Random();
            return r ? *r : fallback;
        }

        // TurtleEggBlock.onSand / isSand: the block below is in #minecraft:sand.
        bool OnSand(const ILevelWrite& level, const glm::ivec3& pos) {
            return Turtle::IsSandBlock(level.GetBlock(pos.x, pos.y - 1, pos.z));
        }

        // The TURTLE_EGG_HATCH_CHANCE environment attribute at `pos`:
        // DimensionDefaults' 0.002 everywhere, raised by the overworld day
        // timeline's track (Timelines: CONSTANT easing, MAXIMUM modifier) to
        // 1.0 from day time 21062 until 21905 — the stretch before dawn.
        float HatchChance(ILevelWrite& level) {
            constexpr float kDefault = 0.002f;
            if (level.GetDimension() != DimensionId::Overworld) return kDefault;
            const EntityLevel* entities = level.Entities();
            if (!entities) return kDefault;
            const int64_t t = ((entities->GetDayTime() % 24000) + 24000) % 24000;
            return (t >= 21062 && t < 21905) ? 1.0f : kDefault;
        }

        // TurtleEggBlock.shouldUpdateHatchLevel.
        bool ShouldUpdateHatchLevel(ILevelWrite& level, JavaRandom& fallback) {
            const float chance = HatchChance(level);
            return chance > 0.0f && LevelRandom(level, fallback).NextFloat() < chance;
        }

        // TurtleEggBlock.decreaseEggs: the break sound, then one egg fewer —
        // or the block gone (Level.destroyBlock(pos, false): the break
        // particles and sound of level event 2001, no drops).
        void DecreaseEggs(ILevelWrite& level, const glm::ivec3& pos, BlockState state) {
            JavaRandom fallback(static_cast<int64_t>(pos.x) * 3129871 ^ static_cast<int64_t>(pos.z) * 116129781);
            JavaRandom& random = LevelRandom(level, fallback);
            level.PlaySound(nullptr, pos, SoundEvents::TURTLE_EGG_BREAK, SoundSource::Blocks, 0.7f,
                            0.9f + random.NextFloat() * 0.2f);
            const int eggs = EggsOf(state);
            if (eggs <= 1) {
                if (level.GetBlock(pos.x, pos.y, pos.z) == BlockID::TurtleEgg) {
                    PlayLevelEventSound(level, nullptr, LevelEvent::PARTICLES_DESTROY_BLOCK, pos,
                                        static_cast<int>(state.RawId()), level.Random());
                    level.DestroyBlock(pos, false);
                }
            } else {
                level.SetBlock(pos.x, pos.y, pos.z, state.SetIndex(PropertyId::EGGS, eggs - 2),
                               World::UpdateFlags::UpdateClients);
                // gameEvent(BLOCK_DESTROY, pos, Context.of(state)).
                level.GameEvent(GameEventId::BlockDestroy, pos, GameEventContext::Of(state));
                PlayLevelEventSound(level, nullptr, LevelEvent::PARTICLES_DESTROY_BLOCK, pos,
                                    static_cast<int>(state.RawId()), level.Random());
            }
        }

        bool IsZombieFamily(EntityTypeId type) {
            // `entity instanceof Zombie`: the zombie and its subclasses.
            return type == EntityTypeId::Zombie || type == EntityTypeId::Husk ||
                   type == EntityTypeId::Drowned || type == EntityTypeId::ZombieVillager ||
                   type == EntityTypeId::ZombifiedPiglin;
        }

        // TurtleEggBlock.canDestroyEgg: never a turtle or a bat; a player
        // always (this engine has no spawn protection); any other living
        // entity under mobGriefing; nothing else.
        bool CanDestroyEgg(ILevelWrite& level, const Entity& entity) {
            // A player's server-side view carries a placeholder entity type,
            // so players are answered before the type is read.
            if (entity.IsPlayer()) return true;
            if (entity.GetType() == EntityTypeId::Turtle || entity.GetType() == EntityTypeId::Bat) {
                return false;
            }
            if (entity.AsLiving() != nullptr) {
                const EntityLevel* entities = level.Entities();
                return entities ? entities->MobGriefing() : true;
            }
            return false;
        }

        // TurtleEggBlock.destroyEgg.
        void DestroyEgg(ILevelWrite& level, BlockState state, const glm::ivec3& pos, Entity& entity,
                        int randomness) {
            if (!state.Is(BlockID::TurtleEgg) || level.IsClientSide()) return;
            JavaRandom fallback(static_cast<int64_t>(pos.x) ^ (static_cast<int64_t>(pos.z) << 16));
            if (CanDestroyEgg(level, entity) && LevelRandom(level, fallback).NextInt(randomness) == 0) {
                DecreaseEggs(level, pos, state);
            }
        }

        // TurtleEggBlock.stepOn: one in a hundred ticks, unless stepping
        // carefully (isShiftKeyDown — only a player sneaks).
        void TurtleEggStepOn(ILevelWrite& level, const glm::ivec3& pos, BlockState state, Entity& entity) {
            const LivingEntity* living = entity.AsLiving();
            const bool steppingCarefully = living && living->IsDiscrete();
            if (!steppingCarefully) DestroyEgg(level, state, pos, entity, 100);
        }

        // TurtleEggBlock.fallOn: one in three, unless a zombie landed.
        void TurtleEggFallOn(ILevelWrite& level, const glm::ivec3& pos, BlockState state, Entity& entity,
                             double /*fallDistance*/) {
            if (entity.IsPlayer() || !IsZombieFamily(entity.GetType())) {
                DestroyEgg(level, state, pos, entity, 3);
            }
        }

        bool TurtleEggIsRandomlyTicking(BlockState /*state*/) { return true; }

        // TurtleEggBlock.randomTick.
        void TurtleEggRandomTick(ILevelWrite& level, const glm::ivec3& pos, BlockState state,
                                 JavaRandom& random) {
            if (!ShouldUpdateHatchLevel(level, random) || !OnSand(level, pos)) return;
            const int hatch = HatchOf(state);
            if (hatch < 2) {
                level.PlaySound(nullptr, pos, SoundEvents::TURTLE_EGG_CRACK, SoundSource::Blocks, 0.7f,
                                0.9f + random.NextFloat() * 0.2f);
                level.SetBlock(pos.x, pos.y, pos.z, state.SetIndex(PropertyId::HATCH, hatch + 1),
                               World::UpdateFlags::UpdateClients);
                // gameEvent(BLOCK_CHANGE, pos, Context.of(state)).
                level.GameEvent(GameEventId::BlockChange, pos, GameEventContext::Of(state));
                return;
            }

            level.PlaySound(nullptr, pos, SoundEvents::TURTLE_EGG_HATCH, SoundSource::Blocks, 0.7f,
                            0.9f + random.NextFloat() * 0.2f);
            // level.removeBlock(pos, false), gameEvent(BLOCK_DESTROY, pos,
            // Context.of(state)).
            level.SetBlock(pos.x, pos.y, pos.z, BlockID::Air, World::UpdateFlags::All);
            level.GameEvent(GameEventId::BlockDestroy, pos, GameEventContext::Of(state));

            EntityLevel* entities = level.Entities();
            const int eggs = EggsOf(state);
            for (int i = 0; i < eggs; ++i) {
                PlayLevelEventSound(level, nullptr, LevelEvent::PARTICLES_DESTROY_BLOCK, pos,
                                    static_cast<int>(state.RawId()), level.Random());
                if (!entities) continue;
                // EntityTypes.TURTLE.create(level, BREEDING): a baby
                // (setAge(-24000)) whose home is the nest, set down in a row
                // across the cell.
                auto turtle = std::make_unique<Turtle>(entities);
                turtle->SetAge(-24000);
                turtle->SetHomePos(pos);
                turtle->position = glm::dvec3(pos.x + 0.3 + i * 0.2, pos.y, pos.z + 0.3);
                turtle->yRot = 0.0f;
                turtle->xRot = 0.0f;
                entities->AddFreshEntity(std::move(turtle));
            }
        }

        // TurtleEggBlock.onPlace: on sand, the growth sparkle (level event
        // 2012, 15 particles) — every write, a crack or an extra egg too.
        void TurtleEggOnPlace(ILevelWrite& level, const glm::ivec3& pos, BlockState /*newState*/,
                              BlockState /*oldState*/, bool /*movedByPiston*/) {
            if (OnSand(level, pos) && !level.IsClientSide()) {
                level.PlayLevelEvent(nullptr, LevelEvent::PARTICLES_TURTLE_EGG_PLACEMENT, pos, 15);
            }
        }

    } // namespace

    void TurtleEgg::PlayerDestroy(ILevelWrite& level, const glm::ivec3& pos, BlockState state) {
        if (!state.Is(BlockID::TurtleEgg) || level.IsClientSide()) return;
        DecreaseEggs(level, pos, state);
    }

    void RegisterTurtleEggBehaviors(std::array<Block, BlockRegistry::Size>& blocks) {
        Block& egg = blocks[static_cast<size_t>(BlockID::TurtleEgg)];
        egg.isRandomlyTicking = &TurtleEggIsRandomlyTicking;
        egg.randomTick        = &TurtleEggRandomTick;
        egg.onPlace           = &TurtleEggOnPlace;
        egg.stepOn            = &TurtleEggStepOn;
        egg.fallOn            = &TurtleEggFallOn;
    }

} // namespace Game
