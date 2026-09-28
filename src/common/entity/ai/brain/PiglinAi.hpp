// File: src/common/entity/ai/brain/PiglinAi.hpp
//
// MC net.minecraft.world.entity.monster.piglin.PiglinAi.
//
// The portable surface: idle wander and piglin-to-piglin socialising, the
// jealous stare at players carrying gold, hunting hoglins (with the shared
// 30-120s hunt cooldown and the pack broadcast), celebrating (and sometimes
// dancing) over a kill, retreating from zombified piglins and soul-fire
// repellent blocks, fleeing when hoglins outnumber the pack, the baby's
// nemesis-flight and hoglin-riding games, and retaliation with pack anger.
//
// The item half: loot pickup (PiglinAi.pickUpItem — admire, barter, eat,
// equip, pocket), the ADMIRE_ITEM activity (walk to a loved item, admire it
// in the off hand for 119 ticks, then barter a gold ingot for the
// piglin_bartering table or keep what it took), the NEAREST_ITEMS sensor,
// the crossbow (BackUpIfTooClose + CrossbowAttack), the gold-armour truce
// (isWearingSafeArmor) and the jealous look / sound at a player holding gold.
//
// UNIVERSAL_ANGER (the universal_anger game rule) is honoured: a player's
// hit, or a guarded block / container, angers the pack at the nearest
// targetable player, and an angry piglin takes any visible attackable one.
//
// The spear fight (SpearApproach / SpearAttack / SpearRetreat) runs in the
// FIGHT activity for a piglin holding a spear.
//
// SKIPPED, commented at its MC call site in the .cpp: door interaction (no
// InteractWithDoor behaviour).
#pragma once

#include "common/entity/ai/brain/Brain.hpp"
#include "common/entity/Item.hpp"

#include <cstdint>

namespace Game {

    class Mob;
    class Piglin;
    struct EntityLevel;

    namespace PiglinAi {

        void InitBrain(Piglin& piglin, Brain& brain);

        // MC PiglinAi.initMemories — a fresh piglin starts with the hunt
        // cooldown armed (30-120 s), so a spawn does not immediately gore the
        // nearest hoglin.
        void InitMemories(Piglin& piglin);

        void UpdateActivity(Piglin& piglin);

        // MC PiglinAi.getSoundForCurrentActivity — the voice the piglin's
        // current activity calls for ("" with no activity): ANGRY fighting,
        // RETREAT fleeing something within 12 blocks, ADMIRING_ITEM,
        // CELEBRATE, else AMBIENT. (JEALOUS and the repellent RETREAT need
        // the loved-item and repellent sensors, which are not ported.)
        const char* SoundForCurrentActivity(const Piglin& piglin);

        // MC PiglinAi.wasHurtBy — the retaliate/flee/pack-anger switchboard.
        void WasHurtBy(EntityLevel& level, Piglin& piglin, LivingEntity& attacker);

        // Shared with PiglinBruteAi.
        void MaybeRetaliate(EntityLevel& level, Mob& piglin, LivingEntity& attacker);
        void SetAngerTarget(Mob& piglin, LivingEntity& target);
        // MC PiglinAi.broadcastAngerTarget: every nearby adult piglin (brutes
        // included) that is not hunting-barred takes the target when it is
        // nearer than its own grudge (setAngerTargetIfCloserThanCurrent); a
        // hoglin only for a piglin that can hunt one that can be hunted.
        void BroadcastAngerTarget(EntityLevel& level, Mob& piglin, LivingEntity& target);

        // MC SetLookAndInteract.create(PLAYER, range) — shared with the
        // brute's IDLE activity.
        BehaviorPtr MakeSetLookAndInteractPlayer(int interactionRange);

        // MC PiglinAi.isZombified — zombified piglin or zoglin.
        bool IsZombified(const Entity& entity);

        // MC PiglinAi.isPlayerHoldingLovedItem — a player holding anything in
        // the piglin_loved tag in either hand.
        bool IsPlayerHoldingLovedItem(EntityLevel& level, LivingEntity& entity);

        // ── Items ─────────────────────────────────────────────────────────
        // MC ItemTags.PIGLIN_LOVED / BARTERING_ITEM (the gold ingot) /
        // ItemTags.PIGLIN_FOOD.
        bool IsLovedItem(const ItemStack& stack);
        bool IsBarterCurrency(const ItemStack& stack);
        bool IsFood(const ItemStack& stack);
        // MC PiglinAi.isWearingSafeArmor: any armour slot holding a
        // piglin_safe_armor piece (the gold set) — a player in gold is not a
        // target.
        bool IsWearingSafeArmor(LivingEntity& entity);
        // MC PiglinAi.wantsToPickup.
        bool WantsToPickup(const Piglin& piglin, const ItemStack& stack);
        // MC PiglinAi.pickUpItem(level, piglin, itemEntity): takes the whole
        // stack of nuggets or one of anything else from the item entity.
        void PickUpItem(EntityLevel& level, Piglin& piglin, int32_t itemEntityId, const ItemStack& stack);
        // MC PiglinAi.stopHoldingOffHandItem — the end of an admire: an adult
        // barters a gold ingot (when bartering is enabled) or keeps the loot;
        // a baby keeps a new toy in its main hand.
        void StopHoldingOffHandItem(EntityLevel& level, Piglin& piglin, bool barteringEnabled);
        // MC PiglinAi.cancelAdmiring — the admired item dropped.
        void CancelAdmiring(EntityLevel& level, Piglin& piglin);
        // MC PiglinAi.canAdmire / mobInteract (the server half).
        bool CanAdmire(const Piglin& piglin, const ItemStack& held);
        bool MobInteract(EntityLevel& level, Piglin& piglin, ItemStack& held);
        // MC PiglinAi.angerNearbyPiglins: the idle piglins within 16 blocks of
        // a player (only those that see the player when asked) turn on them —
        // a guarded container opened, a guarded block broken.
        void AngerNearbyPiglins(EntityLevel& level, LivingEntity& player, bool onlyIfTheySeeThePlayer);

    } // namespace PiglinAi

} // namespace Game
