// File: src/common/world/block/ContactDamageBlocks.cpp
//
// See ContactDamageBlocks.hpp — MC CactusBlock / SweetBerryBushBlock /
// CampfireBlock .entityInside and MagmaBlock.stepOn.
#include "ContactDamageBlocks.hpp"

#include "RedstoneStateUtil.hpp"   // LitOf
#include "common/entity/Entity.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/EndCrystal.hpp"
#include "common/entity/FallingBlockEntity.hpp"
#include "common/entity/LivingEntity.hpp"
#include "common/entity/PrimedTnt.hpp"
#include "common/entity/decoration/BlockAttachedEntity.hpp"
#include "common/entity/projectile/Projectile.hpp"
#include "common/entity/vehicle/VehicleEntity.hpp"
#include "common/world/level/ILevelWrite.hpp"
#include "common/core/Log.hpp"

#include <cmath>

namespace Game {

    namespace {

        // MC `entity instanceof LivingEntity`. Every entity but a dropped item
        // runs the living pipeline in this port, including the ones MC keeps
        // as plain Entities — primed TNT, falling blocks, End crystals,
        // projectiles, boats and minecarts, and the hanging decorations — so
        // those are told apart by class. (The armor stand IS a LivingEntity
        // in MC, and stays one here.)
        LivingEntity* AsMcLivingEntity(Entity& entity) {
            LivingEntity* living = entity.AsLiving();
            if (!living) return nullptr;
            if (dynamic_cast<const PrimedTnt*>(&entity) ||
                dynamic_cast<const FallingBlockEntity*>(&entity) ||
                dynamic_cast<const EndCrystal*>(&entity) ||
                dynamic_cast<const Projectile*>(&entity) ||
                dynamic_cast<const BlockAttachedEntity*>(&entity) ||
                IsVehicleEntityType(entity.GetType())) {
                return nullptr;
            }
            return living;
        }

        // MC Entity.hurt: `if (level instanceof ServerLevel) hurtServer(...)`.
        bool OnServer(const ILevelWrite& level, const Entity& entity) {
            if (level.IsClientSide()) return false;
            const EntityLevel* entityLevel = entity.Level();
            return entityLevel && !entityLevel->IsClientSide();
        }

        // ── Cactus ─────────────────────────────────────────────────────────
        // MC CactusBlock.entityInside: `entity.hurt(damageSources().cactus(),
        // 1.0F)` — any entity, living or not. Every entity class here answers
        // Hurt: a projectile, primed TNT or a falling block shrugs it off
        // (MC Projectile.hurtServer returns false), a boat or minecart takes
        // VehicleEntity's damage and breaks, an End crystal explodes.
        void CactusEntityInside(ILevelWrite& level, const glm::ivec3& /*pos*/, BlockState /*state*/,
                                Entity& entity) {
            if (!OnServer(level, entity) || entity.IsRemoved()) return;
            if (LivingEntity* target = entity.AsLiving()) {
                target->Hurt(MobDamageSource::Cactus, 1.0f, nullptr);
            }
        }

        // ── Sweet berry bush ───────────────────────────────────────────────
        // MC SweetBerryBushBlock.entityInside. The bush's makeStuckInBlock
        // slowdown (0.8, 0.75, 0.8) is the shared stuck-in-block mechanism
        // the engine has not ported (cobweb and powder snow ride it too); the
        // damage half is here.
        void SweetBerryBushEntityInside(ILevelWrite& level, const glm::ivec3& /*pos*/, BlockState state,
                                        Entity& entity) {
            LivingEntity* living = AsMcLivingEntity(entity);
            if (!living || living->IsRemoved()) return;
            const EntityTypeId type = entity.GetType();
            if (type == EntityTypeId::Fox || type == EntityTypeId::Bee) return;
            if (!OnServer(level, entity)) return;
            if (state.GetIndex(PropertyId::AGE_3) == 0) return;
            // `isClientAuthoritative() ? getKnownMovement() : oldPosition()
            // .subtract(position())` — a player's movement is the client's
            // report; only the horizontal magnitude matters.
            const glm::dvec3 movement = entity.IsPlayer() ? entity.GetKnownMovement()
                                                          : entity.oldPosition - entity.position;
            if (movement.x * movement.x + movement.z * movement.z <= 0.0) return;
            constexpr double kMinStep = 0.003000000026077032;
            if (std::abs(movement.x) >= kMinStep || std::abs(movement.z) >= kMinStep) {
                living->Hurt(MobDamageSource::SweetBerryBush, 1.0f, nullptr);
            }
        }

        // ── Campfires ──────────────────────────────────────────────────────
        // MC CampfireBlock.entityInside: a LIT campfire hurts a LivingEntity
        // for the block's fireDamage — Blocks.CAMPFIRE is
        // `new CampfireBlock(true, 1, …)`, SOUL_CAMPFIRE `(false, 2, …)`.
        void CampfireEntityInside(ILevelWrite& level, const glm::ivec3& /*pos*/, BlockState state,
                                  Entity& entity) {
            if (!state.HasProperty(PropertyId::LIT) || !LitOf(state)) return;
            LivingEntity* living = AsMcLivingEntity(entity);
            if (!living || living->IsRemoved() || !OnServer(level, entity)) return;
            const float fireDamage = state.Block() == BlockID::SoulCampfire ? 2.0f : 1.0f;
            living->Hurt(MobDamageSource::Campfire, fireDamage, nullptr);
        }

        // ── Magma block ────────────────────────────────────────────────────
        // MC MagmaBlock.stepOn: `if (!entity.isSteppingCarefully() && entity
        // instanceof LivingEntity) entity.hurt(hotFloor(), 1.0F)`.
        // isSteppingCarefully is isShiftKeyDown — a sneaking player (the
        // view's IsDiscrete); no mob sneaks.
        void MagmaStepOn(ILevelWrite& level, const glm::ivec3& /*pos*/, BlockState /*state*/,
                         Entity& entity) {
            LivingEntity* living = AsMcLivingEntity(entity);
            if (!living || living->IsRemoved() || !OnServer(level, entity)) return;
            if (living->IsDiscrete()) return;
            living->Hurt(MobDamageSource::HotFloor, 1.0f, nullptr);
        }

    } // namespace

    void RegisterContactDamageBehaviors(std::array<Block, BlockRegistry::Size>& blocks) {
        auto at = [&blocks](BlockID id) -> Block& { return blocks[static_cast<size_t>(id)]; };

        // The whole cell (EntityInsideShape::FullBlock, MC's default) for all
        // three: the cactus's collision box is inset 1/16, so the entity it
        // holds off never overlaps its shape.
        at(BlockID::Cactus).entityInside         = &CactusEntityInside;
        at(BlockID::SweetBerryBush).entityInside = &SweetBerryBushEntityInside;
        at(BlockID::Campfire).entityInside       = &CampfireEntityInside;
        at(BlockID::SoulCampfire).entityInside   = &CampfireEntityInside;
        for (BlockID id : { BlockID::Cactus, BlockID::SweetBerryBush,
                            BlockID::Campfire, BlockID::SoulCampfire }) {
            at(id).entityInsideShape = EntityInsideShape::FullBlock;
        }

        at(BlockID::MagmaBlock).stepOn = &MagmaStepOn;

        Log::Info("[ContactDamageBlocks] cactus, sweet berry bush, campfires and magma wired");
    }

} // namespace Game
