// File: src/common/world/block/DecoratedPotBlock.cpp
//
// MC DecoratedPotBlock.useItemOn / useWithoutItem, method by method.
#include "common/world/block/DecoratedPotBlock.hpp"

#include "common/entity/IUsePlayer.hpp"
#include "common/entity/Item.hpp"
#include "common/particle/ParticleOptions.hpp"
#include "common/sound/SoundEvents.hpp"
#include "common/world/block/BlockInteraction.hpp"
#include "common/world/block/entity/DecoratedPotBlockEntity.hpp"
#include "common/world/level/ILevelWrite.hpp"
#include "common/world/level/gameevent/GameEvent.hpp"
#include "common/entity/projectile/Projectile.hpp"
#include "common/world/level/GameRules.hpp"
#include "common/world/level/World.hpp"
#include "common/world/tags/DataTags.hpp"

namespace Game {

    namespace {

        DecoratedPotBlockEntity* PotAt(ILevelWrite& level, const glm::ivec3& pos) {
            auto* pot = dynamic_cast<DecoratedPotBlockEntity*>(level.GetBlockEntity(pos));
            if (pot && !pot->GetLevel() && !level.IsClientSide()) pot->SetLevel(&level);
            return pot;
        }

        // MC DecoratedPotBlock.useItemOn.
        UseResult PotUseItemOn(ItemStack& stack, ILevelWrite* level, const glm::ivec3& pos,
                               IUsePlayer* player, uint32_t /*hand*/, const BlockHitResult& /*hit*/) {
            if (!level) return UseResult::Pass;
            DecoratedPotBlockEntity* pot = PotAt(*level, pos);
            if (!pot) return UseResult::Pass;
            if (level->IsClientSide()) return UseResult::Success;

            // getTheItem() unpacks the loot table first — a corridor pot
            // rolls its find the moment something is put in.
            ItemStack& potItem = pot->GetItem(0);
            const bool fits = !stack.IsEmpty() &&
                (potItem.IsEmpty() ||
                 (IsSameItemSameComponents(potItem, stack) && potItem.count < pot->GetMaxStackSize(potItem)));
            if (!fits) return UseResult::TryEmptyHandInteraction;

            pot->Wobble(DecoratedPotBlockEntity::WobbleStyle::Positive);
            // itemStack.consumeAndReturn(1, player): a copy for a player with
            // infinite materials.
            ItemStack awarded = stack;
            awarded.count = 1;
            if (!player || !player->isCreative()) {
                stack.count -= 1;
                if (stack.count <= 0) stack.Clear();
            }
            float pitchBend = 0.0f;
            if (pot->IsEmpty()) {
                pot->SetItem(0, awarded);
                pitchBend = static_cast<float>(awarded.count) /
                            static_cast<float>(ItemRegistry::Get(awarded.itemId).maxStackSize);
            } else {
                ItemStack grown = pot->GetItem(0);
                grown.count += 1;
                pitchBend = static_cast<float>(grown.count) /
                            static_cast<float>(ItemRegistry::Get(grown.itemId).maxStackSize);
                pot->SetItem(0, grown);
            }
            level->PlaySound(nullptr, pos, SoundEvents::DECORATED_POT_INSERT, SoundSource::Blocks,
                             1.0f, 0.7f + 0.5f * pitchBend);
            level->SendParticles(ParticleOptions(ParticleKind::DustPlume), pos.x + 0.5, pos.y + 1.2,
                                 pos.z + 0.5, 7, 0.0, 0.0, 0.0, 0.0);
            pot->SetChanged();
            level->BlockEntityChanged(pos);
            level->GameEvent(GameEventId::BlockChange, pos,
                             GameEventContext::Of(player ? player->GameEventSource() : nullptr));
            return UseResult::Success;
        }

        // MC DecoratedPotBlock.useWithoutItem.
        UseResult PotUseWithoutItem(ILevelWrite* level, const glm::ivec3& pos,
                                    IUsePlayer* player, const BlockHitResult& /*hit*/) {
            if (!level) return UseResult::Pass;
            DecoratedPotBlockEntity* pot = PotAt(*level, pos);
            if (!pot) return UseResult::Pass;
            if (level->IsClientSide()) return UseResult::Success;
            level->PlaySound(nullptr, pos, SoundEvents::DECORATED_POT_INSERT_FAIL, SoundSource::Blocks,
                             1.0f, 1.0f);
            pot->Wobble(DecoratedPotBlockEntity::WobbleStyle::Negative);
            level->GameEvent(GameEventId::BlockChange, pos,
                             GameEventContext::Of(player ? player->GameEventSource() : nullptr));
            return UseResult::Success;
        }

        // MC DecoratedPotBlock.onProjectileHit: an impact projectile that
        // may interact here and may break blocks cracks the pot and destroys
        // it (its contents spill — DecoratedPotBlockEntity::
        // PreRemoveSideEffects).
        void PotOnProjectileHit(ILevelWrite& level, const glm::ivec3& pos, BlockState state,
                                const glm::dvec3& /*hitPos*/, Direction /*face*/, Entity& projectile) {
            if (level.IsClientSide()) return;
            // Projectile.mayInteract: a player owner may (spawn protection
            // aside); another mob owner only under mobGriefing; no owner may.
            if (auto* shot = dynamic_cast<Projectile*>(&projectile)) {
                if (Entity* owner = shot->GetOwner();
                    owner && !owner->IsPlayer() &&
                    !Rules::GetBool(Rules::Id::MobGriefing)) {
                    return;
                }
            }
            // Projectile.mayBreak: #impact_projectiles and the
            // projectiles_can_break_blocks rule.
            if (!DataTags::HasTag(DataTags::Registry::EntityType, projectile.TypeInfo().slug,
                                  "minecraft:impact_projectiles") ||
                !Rules::GetBool(Rules::Id::ProjectilesCanBreakBlocks)) {
                return;
            }
            // setBlock(pos, state.setValue(CRACKED, true), 260), then
            // destroyBlock(pos, true, projectile).
            level.SetBlock(pos.x, pos.y, pos.z, state.SetName(PropertyId::CRACKED, "true"),
                           World::UpdateFlags::Invisible | World::UpdateFlags::SkipBlockEntitySideEffects);
            level.DestroyBlock(pos, true);
        }

        // BaseEntityBlock.triggerEvent: the wobble's block event goes to
        // the pot's entity (DecoratedPotBlockEntity::TriggerEvent), and is
        // broadcast because it was handled.
        bool PotTriggerEvent(ILevelWrite& level, const glm::ivec3& pos, BlockState /*state*/,
                             int b0, int b1) {
            BlockEntity* be = level.GetBlockEntity(pos);
            return be && be->TriggerEvent(b0, b1);
        }

    } // namespace

    void RegisterDecoratedPotBehaviors(std::array<Block, BlockRegistry::Size>& blocks) {
        Block& b = blocks[static_cast<size_t>(BlockID::DecoratedPot)];
        b.useItemOn      = &PotUseItemOn;
        b.useWithoutItem = &PotUseWithoutItem;
        b.onProjectileHit = &PotOnProjectileHit;
        b.triggerEvent    = &PotTriggerEvent;
    }

} // namespace Game
