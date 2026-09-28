// File: src/common/entity/Mob.hpp
//
// MC net.minecraft.world.entity.Mob — a LivingEntity with goals, navigation
// and control loops.
//
// The ordering inside ServerAiStep is the most load-bearing thing in the whole
// mob port, so it is spelled out rather than left to the .cpp:
//
//   sensing.Tick()                    clear the line-of-sight cache
//   (tickCount + id) % 2 == 0 ?       full goal evaluation on even ticks,
//     selectors.Tick()                running-goal ticks on odd ones. The id
//   : selectors.TickRunningGoals()    term staggers mobs so a herd does not
//                                     all re-evaluate on the same tick.
//   navigation.Tick()                 advance along the path, which calls
//                                     MoveControl::SetWantedPosition
//   CustomServerAiStep()              per-mob extras (creeper fuse, sheep eat)
//   moveControl.Tick()                wanted position -> yaw + zza
//   lookControl.Tick()                head yaw + pitch
//   jumpControl.Tick()                latch -> jumping flag
//
// Everything above only sets inputs. LivingEntity::AiStep then consumes them
// in the jump and travel phases of the SAME tick — which is why ServerAiStep is
// called from inside AiStep and not before it.
#pragma once

#include "common/entity/LivingEntity.hpp"
#include "common/entity/Leashable.hpp"
#include "common/entity/SpawnReason.hpp"

#include <algorithm>
#include "common/entity/AnimationState.hpp"
#include "common/entity/Item.hpp"
#include "common/entity/EquipmentSlot.hpp"
#include "common/entity/ai/Goal.hpp"
#include "common/entity/ai/Controls.hpp"
#include "common/world/pathfinder/PathType.hpp"

#include <memory>
#include <string>
#include <unordered_map>

namespace Game {

    class PathNavigation;
    class Sensing;
    class JavaRandom;
    class MountInventory;        // MountInventory.hpp
    struct Equippable;           // data/DataComponents.hpp
    struct DifficultyInstance;   // EntityLevel.hpp

    // MC SpawnGroupData — the token finalizeSpawn threads through a pack so
    // its members agree on shared rolls (a sheep herd's wool colour, a zombie
    // pack's baby odds). An empty base; each mob that needs one derives its
    // own, exactly as in MC.
    struct SpawnGroupData {
        virtual ~SpawnGroupData() = default;
    };

    class Mob : public LivingEntity {
    public:
        Mob(EntityTypeId type, EntityLevel* level);
        ~Mob() override;

    protected:
        // ── The no-AI constructor ──────────────────────────────────────────
        //
        // PrimedTnt.hpp and FallingBlockEntity.hpp both open by explaining that
        // MC models these as plain Entities and this engine derives Mob only
        // because the tracking / wire / NBT / client-factory pipelines are
        // Mob-shaped. This is the constructor that stops them PAYING for the
        // part they do not use.
        //
        // Skipped: MoveControl, LookControl, JumpControl, BodyRotationControl,
        // Sensing and GroundPathNavigation — six heap allocations per entity —
        // plus the attribute registration (LivingEntity::NoAttributesTag).
        //
        // Safe because neither type can reach the code that reads them. Both
        // override Tick() and NEITHER chains to Mob::Tick or LivingEntity::Tick
        // (each carries a comment saying so, and the omission is vanilla's), so
        // ServerAiStep — the only reader of the sensing, navigation and the
        // three controls — is unreachable, as is LivingEntity::Tick's
        // TickHeadTurn, the only reader of the body rotation control. Neither
        // registers a goal, so the selectors stay empty either way.
        //
        // The three places that touch a navigator from OUTSIDE that chain
        // (Mob::StopInPlace, Mob::TickHeadTurn and /tp's stop-on-teleport) test
        // HasAiControls first. Anything else added later must too — hence the
        // accessor rather than a bare null check.
        struct NoAiTag {};
        Mob(EntityTypeId type, EntityLevel* level, NoAiTag);

    public:
        // False for an entity built through NoAiTag: it has no navigator, no
        // sensing and none of the three controls, and calling their accessors
        // would dereference null.
        bool HasAiControls() const { return m_navigation != nullptr; }

        // ── AI plumbing ────────────────────────────────────────────────────
        GoalSelector& Goals()   { return m_goalSelector; }
        GoalSelector& Targets() { return m_targetSelector; }

        MoveControl& GetMoveControl() { return *m_moveControl; }
        // LivingEntity::IsServerSteeredFlier, widened: any mob flown by a
        // FlyingMoveControl / GhastMoveControl, plus the fliers that steer
        // themselves (bat, blaze, phantom, vex, wither, the mod fliers).
        bool IsServerSteeredFlier() const override;
        LookControl& GetLookControl() { return *m_lookControl; }
        JumpControl& GetJumpControl() { return *m_jumpControl; }

        // Defined out of line: PathNavigation and Sensing are only
        // forward-declared here (including them would be circular — both
        // depend on Mob), and dereferencing a unique_ptr to an incomplete type
        // in an inline body would force every caller to include them too.
        PathNavigation&       GetNavigation();
        // See Entity::SetLevel. Re-points the navigation and drops the
        // target: it was an entity of the old level.
        void SetLevel(EntityLevel* level) override;
        const PathNavigation& GetNavigation() const;
        Sensing&              GetSensing();

        // MC mobs assign `this.navigation` / `this.moveControl` freely in
        // their constructors (every flyer and swimmer does); these are the
        // port's equivalent. Defined out of line for the same incomplete-type
        // reason as the getters.
        void SetNavigation(std::unique_ptr<PathNavigation> navigation);
        void SetMoveControl(std::unique_ptr<MoveControl> control) {
            m_moveControl = std::move(control);
        }
        void SetLookControl(std::unique_ptr<LookControl> control) {
            m_lookControl = std::move(control);
        }
        // MC Mob.createBodyControl — overridden by exactly one mob (the
        // phantom); a setter mirrors the move/look control pattern above
        // rather than adding a virtual for a single user.
        void SetBodyRotationControl(std::unique_ptr<BodyRotationControl> control) {
            m_bodyRotationControl = std::move(control);
        }

        // ── Home / restriction (MC Mob.homePosition + homeRadius) ──────────
        //
        // MC's setHomeTo/isWithinHome family (the older mappings call it
        // restrictTo/getRestrictCenter). radius -1 means "no home", and every
        // query is written against that sentinel exactly as MC's are — a mob
        // without a home is within it everywhere, so MoveTowardsRestrictionGoal
        // stays dormant until something calls SetHomeTo (the elder guardian's
        // aura tick is the one caller today).
        void SetHomeTo(const glm::ivec3& center, int radius) {
            m_homePosition = center;
            m_homeRadius = radius;
        }
        void ClearHome() { m_homeRadius = -1; }
        bool HasHome() const { return m_homeRadius != -1; }
        const glm::ivec3& GetHomePosition() const { return m_homePosition; }
        int GetHomeRadius() const { return m_homeRadius; }
        // Must be settable BEFORE the home position on load: MC only reads
        // home_pos when home_radius >= 0 (Mob.java:420-423), so restoring them
        // in the other order silently discards the position.
        void SetHomeRadius(int radius) { m_homeRadius = radius; }

        // MC Mob.isWithinHome() / isWithinHome(BlockPos) / isWithinHome(Vec3):
        // STRICT distance-squared against radius squared. The no-arg form
        // measures BLOCK-to-block (blockPosition), the Vec3 form measures from
        // the home block's centre — MC keeps both metrics and so does this.
        bool IsWithinHome() const { return IsWithinHome(BlockPosition()); }
        bool IsWithinHome(const glm::ivec3& pos) const {
            if (m_homeRadius == -1) return true;
            const glm::ivec3 d = pos - m_homePosition;
            const double distSq = static_cast<double>(d.x) * d.x +
                                  static_cast<double>(d.y) * d.y +
                                  static_cast<double>(d.z) * d.z;
            return distSq <
                   static_cast<double>(m_homeRadius) * static_cast<double>(m_homeRadius);
        }
        bool IsWithinHome(const glm::dvec3& pos) const {
            if (m_homeRadius == -1) return true;
            const glm::dvec3 centre(m_homePosition.x + 0.5, m_homePosition.y + 0.5,
                                    m_homePosition.z + 0.5);
            const glm::dvec3 d = pos - centre;
            return glm::dot(d, d) <
                   static_cast<double>(m_homeRadius) * static_cast<double>(m_homeRadius);
        }

        // ── Leashable (MC world/entity/Leashable — Mob implements it) ──────
        //
        // The per-mob half of leads; the static half (tickLeash, the spring,
        // the interactions) is Game::Leash in Leashable.hpp. Implemented in
        // MobLeash.cpp, which also carries every 26.3 per-class override
        // (Wolf, Panda, the horse family, the happy ghast, ...) keyed on the
        // entity type — one table instead of a hook in thirty mob classes.

        // MC `this instanceof Leashable`: every real mob. False for the
        // engine's pipeline riders (hanging entities, primed TNT, falling
        // blocks, the end crystal, the armor stand, projectiles), which are
        // Mobs here only because the tracking pipeline is Mob-shaped.
        virtual bool IsLeashable() const;
        // MC Leashable.canBeLeashed. DELIBERATE DIVERGENCE: true for every
        // mob (MobLeash.cpp) — hostiles, fish, villagers included.
        virtual bool CanBeLeashed() const;

        Leash::LeashData*       GetLeashData()       { return m_leashData.get(); }
        const Leash::LeashData* GetLeashData() const { return m_leashData.get(); }
        // MC Leashable.isLeashed — a holder is attached (or has just vanished
        // and the lead drops this tick). The client answers from the id the
        // server linked.
        bool IsLeashed() const;
        // MC Leashable.mayBeLeashed — any leash data, even a save's pending
        // reference.
        bool MayBeLeashed() const { return m_leashData != nullptr; }
        // MC Leashable.getLeashHolder (server: the live holder).
        Entity* GetLeashHolder() const;
        // The holder's entity id as the wire carries it: the live holder's
        // on the server, the linked one on the client; Leash::kNoHolder when
        // unleashed (or while a knot waits for its id).
        int32_t GetLeashHolderNetId() const;

        // MC Leashable.canHaveALeashAttachedTo / leashDistanceTo.
        bool   CanHaveALeashAttachedTo(const Entity& entity) const;
        double LeashDistanceTo(const Entity& entity) const;
        // MC leashSnapDistance (12) / leashElasticDistance (6); the ghasts'
        // 16 / 10. Leads never snap here: the snap distance caps the spring's
        // pull and sets the tow-teleport range (Leash::TickLeash).
        virtual double LeashSnapDistance() const;
        virtual double LeashElasticDistance() const;
        // MC supportQuadLeash / getQuadLeashOffsets — the horse family and
        // the sniffer, held by four ropes from a ghast.
        virtual bool SupportQuadLeash() const;
        virtual std::array<glm::dvec3, 4> GetQuadLeashOffsets() const;
        // MC getLeashOffset(partialTicks): where the lead meets the body,
        // unrotated, from the feet.
        virtual glm::dvec3 GetLeashOffset(float partialTicks) const;

        // MC Leashable.setLeashedTo(holder, synch). `synch` is kept for the
        // call sites' documentation value: the tracker sends every holder
        // change (see Leashable.hpp).
        void SetLeashedTo(Entity& holder, bool synch);
        // MC dropLeash (the lead item drops) / removeLeash (it does not).
        void DropLeash();
        void RemoveLeash();
        // MC Leashable.setDelayedLeashHolderId — the CLIENT's link from
        // SetEntityLinkS2C (Leash::kNoHolder unlinks).
        void SetDelayedLeashHolderId(int32_t entityId);
        // MC readLeashData — the save's "leash" reference, resolved by the
        // next tickLeash (MC restoreLeashFromSave). An empty ref clears.
        void SetDelayedLeashRef(const Leash::HolderRef& ref);
        // The reference MC writeLeashData would store, if any.
        Leash::HolderRef GetLeashSaveRef() const;
        // The holder was freed (ClearReferenceTo): the next tickLeash drops
        // the lead, as MC's does for a removed holder.
        void MarkLeashHolderLost();
        // The mob is leaving its level (a portal, /tp to another dimension):
        // MC's removeAfterChangingDimensions takes the lead off the old
        // entity without an item (removeLeash) and the copy made in the new
        // level carries the saved reference — its holder there (a player who
        // came through too) takes it back; 100 ticks without one, it drops.
        void CarryLeashAcrossLevels();

        // MC Leashable hooks, overridable as in MC.
        virtual void OnLeashRemoved();
        virtual void WhenLeashedTo(Entity& holder);
        virtual void LeashTooFarBehaviour();
        virtual void CloseRangeLeashBehaviour(Entity& holder);
        virtual void OnElasticLeashPull();
        // MC Leashable.checkElasticInteractions — the spring. Returns whether
        // any attachment point was past the slack.
        bool CheckElasticInteractions(Entity& holder, Leash::LeashData& data);

        void TickLeash() override;
        void OnStartedRiding(Entity& vehicle) override;

        // ── Target ─────────────────────────────────────────────────────────
        LivingEntity* GetTarget() const { return m_target; }
        virtual void  SetTarget(LivingEntity* target) {
            // Never an entity of another level. The reference-clearing
            // sweeps that keep m_target from dangling (a mob dying, a
            // player's view leaving with them) run per level, over that
            // level's mobs; a target across a portal — the player who hit
            // this mob through one, say, whose view belongs to the level
            // they stood in — would outlive its clearing and be cast on a
            // freed object in the next goal tick. Chasing across a portal
            // is EntityPortalTravel's, by id.
            if (target && m_level && target->Level() && target->Level() != m_level) return;
            if (target) MarkHoldsEntityRefs();   // see Entity::HoldsEntityRefs
            m_target = target;
        }

        // MC Mob.canAttack — overridden by Creeper (ignores goats) and by the
        // player adapter (never attackable in creative/spectator).
        virtual bool CanAttack(const LivingEntity& target) const;

        // ── Aggression / state flags (MC DATA_MOB_FLAGS_ID) ───────────────
        bool IsAggressive() const { return m_aggressive; }
        void SetAggressive(bool v) { m_aggressive = v; }
        bool IsNoAi() const { return m_noAi; }
        void SetNoAi(bool v) { m_noAi = v; }

        // ── The Held Note (docs/the-hush.md "Reawakening the Heart") ───────
        // A mob that hears the Chord held stops, as in the Hush's stillness:
        // its whole AI step is skipped and it is held where it stands until
        // `untilTick` (the level's game time); its path, goals and target
        // wait for it. Runtime only — a reload lets it go.
        void HoldByNote(int64_t untilTick) { m_heldUntil = std::max(m_heldUntil, untilTick); }
        bool IsHeldByNote(int64_t now) const { return now < m_heldUntil; }

        // ── Equipment (MC Mob's EntityEquipment + DropChances) ─────────────
        //
        // The six humanoid slots (MAINHAND..HEAD) any mob can wear — what an
        // equipment table dresses a spawner's mob in (MobEquipment.hpp). BODY
        // and SADDLE stay with the classes that carry them (the wolf's
        // armour); the armor stand keeps its own six. Stored behind a
        // pointer, allocated on first use: the pipeline also carries a
        // hundred thousand primed TNT that never wear anything. Setting a
        // slot swaps the item's attribute modifiers (weapon damage on the
        // main hand, armour points on the matching piece) and marks the slot
        // for the tracker (SetEquipment → BodyArmorS2C with the slot).
        // Defined in MobEquipment.cpp.
        static constexpr float kDefaultEquipmentDropChance = 0.085f;   // DropChances.DEFAULT
        static constexpr int   kHumanoidEquipmentSlots = 6;
        // All of MC 26.x's EquipmentSlot values: the humanoid six, BODY
        // (horse armour, llama carpets, the happy ghast's harness) and
        // SADDLE. Every mob's EntityEquipment holds all eight.
        static constexpr int   kEquipmentSlotCount = 8;
        const ItemStack& GetEquipment(EquipmentSlot slot) const;
        void SetEquipment(EquipmentSlot slot, const ItemStack& stack);
        // MC's in-place mutation of a worn stack (hurtAndBreak's wear, the
        // shrink of a break): stored, re-synced, its attribute modifiers
        // follow — but it is not an equip (no onEquipItem: no sound, no
        // game event, no skeleton weapon reassessment).
        void ReplaceEquipmentStack(EquipmentSlot slot, const ItemStack& stack);
        // MC DropChances.byEquipment / withEquipmentChance. > 1 is PRESERVED
        // (always dropped, undamaged — isPreserved).
        float GetEquipmentDropChance(EquipmentSlot slot) const;
        void  SetEquipmentDropChance(EquipmentSlot slot, float chance);
        bool  HasAnyEquipment() const;
        // MC Mob.isSaddled: hasValidEquippableItemForSlot(SADDLE) — a stack
        // in the saddle slot that is equippable there.
        bool  IsSaddled() const;
        // MC Mob.hasValidEquippableItemForSlot(slot).
        bool  HasValidEquippableItemForSlot(EquipmentSlot slot) const;
        // The tracker's latch: the slots changed since the last call (bit =
        // EquipmentSlot ordinal). Read-and-clear.
        uint8_t ConsumeEquipmentDirtyMask();
        // LivingEntity's equipment view, over the humanoid slots.
        ItemStack* EquipmentInSlot(EquipmentSlot slot) override;
        // MC LivingEntity.getWeaponItem: the main hand (null when empty).
        ItemStack* GetWeaponItem() override { return EquipmentInSlot(EquipmentSlot::MAINHAND); }
        bool HasEquipmentSlots() const override { return HasAnyEquipment(); }
        // MC LivingEntity.getArmorCoverPercentage: the worn share of the four
        // armour slots.
        float GetArmorCoverPercentage() const override;
        // ConversionType's keepEquipment half: every worn slot moves to `to`
        // with its drop chance (copyAndClear) — a zombie that drowns keeps
        // its armour.
        void MoveEquipmentTo(Mob& to);

        // MC LivingEntity.getMainHandItem / getOffhandItem / isHolding.
        const ItemStack& GetMainHandEquipment() const { return GetEquipment(EquipmentSlot::MAINHAND); }
        const ItemStack& GetOffhandEquipment() const  { return GetEquipment(EquipmentSlot::OFFHAND); }
        bool IsHoldingItem(ItemID id) const {
            return GetMainHandEquipment().itemId == id || GetOffhandEquipment().itemId == id;
        }
        // MC Mob.canUseNonMeleeWeapon — a projectile weapon this mob knows how
        // to fire (the pillager's / piglin's crossbow, a skeleton's bow). The
        // brain's MeleeAttack stands down while one is held, and
        // BehaviorUtils.isWithinAttackRange measures the weapon's range
        // instead of the melee reach.
        virtual bool CanUseNonMeleeWeapon(const ItemStack& stack) const { (void)stack; return false; }

        // MC DropChances.PRESERVE_ITEM_DROP_CHANCE: a picked-up (or player-
        // given) piece always drops, whole (> 1 = isPreserved).
        static constexpr float kGuaranteedDropChance = 2.0f;
        // MC Mob.setGuaranteedDrop(slot).
        void SetGuaranteedDrop(EquipmentSlot slot) { SetEquipmentDropChance(slot, kGuaranteedDropChance); }
        // MC Mob.setItemSlotAndDropWhenKilled.
        void SetItemSlotAndDropWhenKilled(EquipmentSlot slot, const ItemStack& stack) {
            SetEquipment(slot, stack);
            SetGuaranteedDrop(slot);
        }

        // MC LivingEntity.onEquipItem — runs after every SetEquipment (both
        // sides; the base acts on the server only): a changed piece plays the
        // EQUIPPABLE's equip sound in its own slot and emits EQUIP / UNEQUIP,
        // except on the entity's first tick (spawn gear is silent). Override
        // to react to a new weapon (AbstractSkeleton.reassessWeaponGoal) —
        // call the base.
        virtual void OnEquipItem(EquipmentSlot slot, const ItemStack& oldStack, const ItemStack& newStack);

        // ── Mount equipment (MC 26.x: saddles, horse armour, llama carpets,
        //    harnesses, nautilus armour — MobEquipment.cpp) ──────────────────
        //
        // MC LivingEntity.getEquipSound(slot, stack, equippable) — the sound
        // OnEquipItem plays: the EQUIPPABLE's own, or the mount's saddle
        // sound (AbstractHorse HORSE_SADDLE, Pig PIG_SADDLE, …).
        virtual std::string GetEquipSound(EquipmentSlot slot, const ItemStack& stack,
                                          const Equippable& equippable) const;
        // MC LivingEntity.hasItemInSlot.
        bool HasItemInSlot(EquipmentSlot slot) const { return !GetEquipment(slot).IsEmpty(); }
        // MC Equippable.equipOnTarget(player, target, stack): an
        // equip-on-interact item (a saddle, a harness, nautilus armour) put
        // on this mob by a right-click — equippable in its slot, the slot
        // empty, the mob alive. The server takes one off `stack` into the
        // slot and makes it a guaranteed drop. Success, or Pass when it does
        // not fit.
        UseResult EquipOnTarget(const Equippable& equippable, ItemStack& stack);
        // MC LivingEntity.canEquipWithDispenser: alive, the stack a
        // dispensable EQUIPPABLE for a slot this mob can use and may wear,
        // the slot empty, and the mob letting a dispenser fill it.
        bool CanEquipWithDispenser(const ItemStack& stack) const;
        // MC Mob.canDispenserEquipIntoSlot: canPickUpLoot() — the mounts
        // widen it for their saddle / body slot, the fox, dolphin and panda
        // narrow it to the main hand, the allay refuses.
        virtual bool CanDispenserEquipIntoSlot(EquipmentSlot slot) const { (void)slot; return CanPickUpLoot(); }
        // MC Mob.canShearEquipment(player): not while carrying anyone (the
        // wolf: only its owner).
        virtual bool CanShearEquipment(const LivingEntity& player) const;
        // MC Mob.attemptToShearEquipment: the first slot (EquipmentSlot
        // order) holding a canBeSheared piece that is not Curse-of-Binding
        // locked (creative ignores the curse) comes off — the shears wear by
        // one, the piece drops at the passenger attachment point, the
        // piece's shearing sound plays and the SHEAR game event fires.
        // Server-side; false when nothing came off.
        bool AttemptToShearEquipment(LivingEntity& player, ItemStack& shears);

        // ── HasCustomInventoryScreen (MC AbstractHorse / AbstractNautilus) ──
        //
        // The mounts with an inventory screen: the horse family, the llamas,
        // the camels and the nautili. `GetMountInventory` is MC
        // AbstractHorse.inventory (null for every other mob); its chest half
        // is AbstractChestedHorse's (MountInventory.hpp).
        virtual MountInventory*       GetMountInventory()       { return nullptr; }
        virtual const MountInventory* GetMountInventory() const { return nullptr; }
        // MC AbstractHorse.getInventoryColumns — 0, a chested equine's 5
        // while it carries a chest, a llama's strength.
        virtual int GetInventoryColumns() const { return 0; }
        // MC AbstractHorse.createInventory: re-size the mount inventory to
        // getInventorySize() (columns × 3), keeping what fits.
        void CreateMountInventory();
        // MC HasCustomInventoryScreen — what the inventory key opens while
        // riding (and a sneak-use on a tamed mount).
        virtual bool HasCustomInventoryScreen() const { return false; }
        // MC openCustomInventoryScreen(player): server-side, the mount
        // screen opens for `player` when the mount allows it (tamed, and
        // carrying nobody but `player`).
        virtual void OpenCustomInventoryScreen(LivingEntity& player) { (void)player; }

        // ── Loot pickup (MC Mob.aiStep "looting" → pickUpItem →
        //    equipItemIfPossible, canReplaceCurrentItem) ─────────────────────
        //
        // A mob with canPickUpLoot, alive, on a server level with mobGriefing
        // on, sweeps the item entities touching its box grown by
        // GetPickupReach, and every one past its pickup delay that it
        // WantsToPickUp goes through PickUpItem. The base PickUpItem equips
        // the stack when it beats what the slot holds (armour by armour then
        // toughness, the main hand by the preferred weapon tag then attack
        // damage, ties by enchantment count, then less damage, then a custom
        // name), drops the displaced piece with the slot's drop chance, and
        // marks the slot a guaranteed drop and the mob persistent.
        //
        // MC LivingEntity.getEquipmentSlotForItem: the EQUIPPABLE slot when
        // the mob can use it, else the main hand.
        EquipmentSlot GetEquipmentSlotForItem(const ItemStack& stack) const;
        // MC LivingEntity.isEquippableInSlot.
        bool IsEquippableInSlot(const ItemStack& stack, EquipmentSlot slot) const;
        // MC LivingEntity.canUseSlot — every slot by default (the humanoid
        // six are all this storage holds).
        virtual bool CanUseSlot(EquipmentSlot slot) const { (void)slot; return true; }
        // MC Mob.equipItemIfPossible: the part of `stack` equipped (empty
        // when it was not). Server-side.
        ItemStack EquipItemIfPossible(const ItemStack& stack);
        // MC Mob.canReplaceCurrentItem (Drowned: never over a nautilus shell).
        virtual bool CanReplaceCurrentItem(const ItemStack& newStack, const ItemStack& current,
                                           EquipmentSlot slot) const;
        // MC Mob.canReplaceEqualItem — the tie-break.
        bool CanReplaceEqualItem(const ItemStack& newStack, const ItemStack& current) const;
        // MC Mob.canHoldItem / wantsToPickUp (default: canHoldItem).
        virtual bool CanHoldItem(const ItemStack& stack) const { (void)stack; return true; }
        virtual bool WantsToPickUp(const ItemStack& stack) const { return CanHoldItem(stack); }
        // MC Mob.getPreferredWeaponType — an item tag ("minecraft:
        // skeleton_preferred_weapons") or null.
        virtual const char* GetPreferredWeaponType() const { return nullptr; }
        // MC Mob.getPickupReach — ITEM_PICKUP_REACH (1, 0, 1).
        virtual glm::ivec3 GetPickupReach() const { return glm::ivec3(1, 0, 1); }
        // MC Mob.pickUpItem(level, itemEntity): `stack` is the item entity's
        // stack; take what is used with TakeItemEntity. Server-side.
        virtual void PickUpItem(int32_t itemEntityId, const ItemStack& stack);
        // MC Mob.onItemPickup(itemEntity) — the piglin's admire / the fox's
        // spit hooks. Default: nothing.
        virtual void OnItemPickup(int32_t itemEntityId, const ItemStack& stack) {
            (void)itemEntityId; (void)stack;
        }
        // MC Mob.take(entity, n) + the item entity's shrink: `count` leaves
        // the item entity (it despawns when emptied). Returns how many went.
        int TakeItemEntity(int32_t itemEntityId, int count);
        // MC Mob.aiStep's "looting" block (called at the end of AiStep).
        void TickLooting();

        // ── Spawn gear (MC Mob.populateDefaultEquipmentSlots /
        //    populateDefaultEquipmentEnchantments / enchantSpawned*) ────────
        //
        // The base PopulateDefaultEquipmentSlots is MC's armour roll: with
        // chance 0.15 * specialMultiplier, a material tier (nextInt(3) plus
        // three 0.1087 bumps — leather, copper, gold, chainmail, iron,
        // diamond), then HEAD, CHEST, LEGS, FEET in that order, each after
        // the first stopping the run with chance 0.1 (Hard) / 0.25. The
        // enchantment pass rolls the main hand at 0.25 and each armour piece
        // at 0.5 (times specialMultiplier) through the mob_spawn_equipment
        // provider. Callers are the subclasses' FinalizeSpawn, in MC's order.
        virtual void PopulateDefaultEquipmentSlots(JavaRandom& random, const DifficultyInstance& difficulty);
        virtual void PopulateDefaultEquipmentEnchantments(JavaRandom& random, const DifficultyInstance& difficulty);
        // MC Mob.getEquipmentForSlot(slot, type) — null (Air) past the table.
        static ItemID GetEquipmentForSlot(EquipmentSlot slot, int type);
        // MC SpecialDates.isHalloween — October 20 .. November 3, local time.
        static bool IsHalloween();
        // MC's Halloween head (Zombie / AbstractSkeleton finalizeSpawn): an
        // empty head gets a carved pumpkin (a jack o'lantern 10% of the
        // time) 25% of the time, never dropped.
        void MaybeWearHalloweenHead(JavaRandom& random);
        // The local difficulty at this mob's block (MC finalizeSpawn's
        // `difficulty` argument).
        DifficultyInstance CurrentDifficulty() const;

    protected:
        virtual void EnchantSpawnedWeapon(JavaRandom& random, const DifficultyInstance& difficulty);
        virtual void EnchantSpawnedArmor(JavaRandom& random, EquipmentSlot slot, const DifficultyInstance& difficulty);
        // MC Mob.enchantSpawnedEquipment — the provider roll on one slot.
        void EnchantSpawnedEquipment(EquipmentSlot slot, JavaRandom& random, float chance,
                                     const DifficultyInstance& difficulty);
        // MC Mob.sunProtectionSlot — the slot whose piece shades a burning
        // undead (and wears by 0..1 each burn tick instead).
        virtual EquipmentSlot SunProtectionSlot() const { return EquipmentSlot::HEAD; }

    public:

        // ── Persistence / despawn ──────────────────────────────────────────
        bool IsPersistenceRequired() const { return m_persistenceRequired; }
        void SetPersistenceRequired(bool v) { m_persistenceRequired = v; }

        // MC Mob.requiresCustomPersistence — despawn immunity the mob earns by
        // STATE rather than by flag: MC's base returns isPassenger(); raiders
        // in a raid, endermen holding a block, fish from a bucket override it.
        // Checked by CheckDespawn AND skipped by the spawn census, exactly
        // like the persistence flag — so a mounted rider (jockey) never
        // distance-despawns and never counts against the mob cap, while its
        // VEHICLE still can (MC keeps a jockey chicken despawnable via
        // Chicken.removeWhenFarAway -> isChickenJockey, an override that lands
        // with the jockey wave).
        // MC 26.3: `isPassenger() || isLeashed()` — a mob on a lead never
        // despawns.
        virtual bool RequiresCustomPersistence() const { return IsPassenger() || IsLeashed(); }

        // MC checkDespawn's `isPersistent` (isPersistenceRequired() ||
        // requiresCustomPersistence()), plus this engine's rule that a mob
        // with a custom name NEVER despawns, however it got the name (a name
        // tag also sets PersistenceRequired, as in MC; /summon {CustomName},
        // a named spawn egg or an imported world may not) — and likewise a
        // tamed pet (IsTamedPet). Every despawn
        // path reads this: the distance checks, the endermite's ageing and
        // the warden's dig-away. (The spawn census keeps MC's own two.) Peaceful's removal
        // of a hostile type is NOT gated by it — MC discards those named or
        // not.
        bool IsDespawnPersistent() const {
            return IsPersistenceRequired() || RequiresCustomPersistence() || HasCustomName() ||
                   IsTamedPet();
        }
        // Tamed by a player: TamableAnimal.isTame (wolf, cat, parrot) or
        // AbstractHorse.isTamed (horse, donkey, mule, skeleton/zombie horse).
        // Engine rule for IsDespawnPersistent: a pet never despawns.
        virtual bool IsTamedPet() const { return false; }

        // MC Mob.removeWhenFarAway — true means "eligible for despawn". The
        // base says yes; Animal overrides to no, which is why cows you walked
        // away from are still there when you come back.
        virtual bool RemoveWhenFarAway(double distanceToClosestPlayerSq) const { return true; }

        // MC Mob.checkDespawn. Runs BEFORE tick() for every mob, whether or not
        // it is in ticking range.
        virtual void CheckDespawn();

        // MC Entity.kill(ServerLevel), as the /kill command's fallback for a
        // mob whose Hurt refused the blow (primed TNT, projectiles, the
        // dragon's players-and-explosions-only gate). The default is the
        // command's old plain discard; the dragon overrides it to hand its
        // fight the victory first — a bare discard just made the fight
        // respawn a fresh dragon.
        virtual void KillFromCommand() { Discard(); }

        // MC Mob.finalizeSpawn — the post-construction randomisation every
        // spawn path runs: EntityType.create calls it for spawn eggs and for
        // /summon, and NaturalSpawner calls it for natural spawns. Anything
        // rolled once at spawn (a sheep's wool colour) belongs here rather
        // than in the constructor, so that an egg, /summon and the spawner
        // agree instead of each inventing their own answer.
        //
        // Called AFTER the position is set — a subclass may read the biome it
        // landed in — and AFTER the spawner's validity tests, matching MC's
        // order in spawnCategoryForPosition (snapTo -> isValidPositionForMob
        // -> finalizeSpawn). `groupData` is threaded through a whole pack;
        // the first member creates it, the rest read it.
        //
        // The base rolls MC Mob.finalizeSpawn's two universal draws: the
        // triangle(0, 0.11485) FOLLOW_RANGE bonus and the 5% left-handed flag.
        virtual std::shared_ptr<SpawnGroupData>
        FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData);

        // MC Mob.isLeftHanded — rolled at spawn, read by the renderer.
        bool IsLeftHanded() const { return m_leftHanded; }
        void SetLeftHanded(bool v) { m_leftHanded = v; }

        // MC Mob.canPickUpLoot — rolled at spawn for zombies/skeletons, set
        // for villagers, foxes, piglins. Gates TickLooting.
        // Virtual as MC's is: the allay answers from its own state.
        virtual bool CanPickUpLoot() const { return m_canPickUpLoot; }
        void SetCanPickUpLoot(bool v) { m_canPickUpLoot = v; }

        // MC Mob.checkMobSpawnRules (static): spawner-driven spawns skip it;
        // otherwise the block below must be a valid spawn surface.
        static bool CheckMobSpawnRules(EntityLevel& level, SpawnReason reason,
                                       const glm::ivec3& pos);

        // MC Mob.checkSpawnRules (instance) — the mob's OWN veto after it has
        // been constructed and positioned. Base is true; PathfinderMob gates
        // on the walk-target value.
        virtual bool CheckSpawnRules(EntityLevel& level, SpawnReason reason) {
            (void)level; (void)reason;
            return true;
        }

        // MC Mob.checkSpawnObstruction: no liquid anywhere in the bounding box
        // and no other entity already occupying it. The water mobs (MC
        // WaterAnimal, AgeableWaterCreature, Axolotl, Drowned, the nautili)
        // override it to IsUnobstructed alone; Ravager to !ContainsAnyLiquid.
        virtual bool CheckSpawnObstruction(EntityLevel& level) const;

        // The two halves of the base test, for the overrides.
        // MC LevelReader.containsAnyLiquid(getBoundingBox()).
        bool ContainsAnyLiquid(EntityLevel& level) const;
        // MC EntityGetter.isUnobstructed(this): no live entity overlapping
        // the bounding box.
        bool IsUnobstructed(EntityLevel& level) const;

        // MC Mob.dropCustomDeathLoot — the non-table drops (an enderman's
        // carried block, equipment when that exists). Called by the server's
        // loot pass alongside the generated table.
        virtual void DropCustomDeathLoot(EntityLevel& level) { (void)level; }
        // MC LivingEntity.dropEquipment — called by dropAllDeathLoot OUTSIDE
        // the mob_drops / shouldDropLoot gate (an allay's carried stacks
        // always come down). Default: nothing.
        virtual void DropEquipment(EntityLevel& level) { (void)level; }

        // The mod mobs' own saved fields (ModMobNbt.hpp) — a Twilight Forest
        // / Aether class's addAdditionalSaveData / readAdditionalSaveData.
        // EntityNbt calls both for every mob after the vanilla fields; the
        // vanilla classes keep their rows in EntityNbt and leave these empty.
        virtual void SaveModNbt(class ModNbtOut& out) const { (void)out; }
        virtual void LoadModNbt(const class ModNbtIn& in) { (void)in; }

        // MC Mob.mobInteract — the ENTITY's own answer to a right-click,
        // e.g. shears on a sheep or a saddle on a pig.
        //
        // MC Player.interactOn runs this BEFORE the held item's
        // Item.interactLivingEntity, and only falls through to the item when
        // this returns a non-consuming result. Returning Pass is what lets dye
        // reach a sheep at all.
        virtual UseResult MobInteract(LivingEntity& player, ItemStack& held) {
            (void)player; (void)held;
            return UseResult::Pass;
        }

        // MC Mob.setBaby — a no-op on the base; AgeableMob turns it into an
        // age, the zombie/piglin/zoglin family into their synched flag. The
        // spawn-egg-on-parent path (SpawnEggItem.spawnOffspringFromSpawnEgg)
        // calls it and then checks IsBaby(): a mob with no baby form stays an
        // adult and the egg is not spent.
        virtual void SetBaby(bool baby) { (void)baby; }

        // MC Mob.onOffspringSpawnedFromEgg — after a spawn egg used on this
        // mob made `offspring` (the server's SpawnOffspringFromSpawnEgg).
        // Only the fox answers: its cub trusts the player who used the egg.
        virtual void OnOffspringSpawnedFromEgg(LivingEntity& spawner, Mob& offspring) {
            (void)spawner; (void)offspring;
        }

        // MC AgeableMob.AGE_LOCKED, the synched flag behind the golden
        // dandelion: a locked baby never grows up. Declared here, like
        // SetBaby, because the wire packs it beside the baby bit for every
        // mob and the sulfur cube carries its own copy (MC SulfurCube has the
        // field too, outside AgeableMob). No-ops on a mob with no age.
        virtual bool IsAgeLocked() const { return false; }
        virtual void SetAgeLocked(bool locked) { (void)locked; }

        // The chunk-column bucket Server::MobManager last filed this mob under.
        // Written by the manager only, read back when the mob is removed so the
        // bucket it is actually IN is patched — recomputing the key from the
        // current position is wrong after the mob has ticked (a TNT crossing a
        // chunk edge between the rebuild and the sweep left a freed pointer in
        // its old bucket, which the natural spawner then dereferenced).
        uint64_t spatialIndexKey = 0;

        int  GetNoActionTime() const { return m_noActionTime; }
        void SetNoActionTime(int t) { m_noActionTime = t; }
        // MC LivingEntity.hurtServer zeroes noActionTime on every accepted
        // hit; LivingEntity::Hurt calls this hook.
        void ResetNoActionTime() override { m_noActionTime = 0; }

        // MC Mob.getMaxSpawnClusterSize — how many of this type one spawn
        // attempt may place.
        virtual int GetMaxSpawnClusterSize() const { return 4; }
        virtual bool IsMaxGroupSizeReached(int groupSize) const { return false; }

        // MC Mob.getMaxFallDistance — with a target the mob trades health for
        // the drop: everything above a third of max health, minus a
        // (3 - difficultyId) * 4 allowance. This is what the pathfinder's
        // ledge check reads.
        int GetMaxFallDistance() const override;

        // ── Pathfinding maluses (MC Mob.setPathfindingMalus) ──────────────
        float GetPathfindingMalus(PathType type) const;
        void  SetPathfindingMalus(PathType type, float malus);

        // ── Combat ─────────────────────────────────────────────────────────
        // MC Mob.doHurtTarget. Returns whether the hit landed.
        virtual bool DoHurtTarget(Entity& target);
        // MC LivingEntity.playAttackSound — silent by default; the panda,
        // dolphin, killer bunny, axolotl and creaking bite audibly.
        virtual void PlayAttackSound() {}
        // MC Mob.chargeSpeedModifier — how much faster a spear charge of this
        // mob's RIDER runs (SpearUseGoal / SpearAttack): 1, the zombie horse's
        // 1.4, the camel husk's 4.
        virtual float ChargeSpeedModifier() const {
            return GetType() == EntityTypeId::CamelHusk ? 4.0f : 1.0f;
        }

        // MC LivingEntity.hurtServer → resolvePlayerResponsibleForDamage
        // (LivingEntity.java:1332): an accepted hit from a player opens a
        // 100-tick kill-credit window. LivingEntity::Hurt is shared with the
        // player view, so the capture lives in Mob's override — its only
        // consumers (killed_by_player loot pools, the XP drop) are mob-side.
        bool Hurt(MobDamageSource source, float amount, Entity* attacker) override;

        // Whether the player kill-credit window (MC's
        // lastHurtByPlayerMemoryTime > 0) is still open. MC reads it in
        // dropAllDeathLoot / dropExperience (LivingEntity.java:1479,1493).
        bool HasPlayerKillCredit() const { return m_lastHurtByPlayerTime > 0; }
        // Entity id of the crediting player's view; -1 when none.
        int32_t LastHurtByPlayerId() const { return m_lastHurtByPlayerId; }

        // MC Mob.getBaseExperienceReward (Mob.java:327-343) — the ctor-seeded
        // xpReward, baked per type into the entity table by
        // tools/gen_entity_types.py. The equipment bonus (+1..3 per worn
        // piece that is not a guaranteed drop, Mob.java:330-338) is added by
        // the death pass (MobEquipment::ExperienceBonus). Animal (1..3 random), Fish/Squid/Dolphin (same),
        // Slime (its size), Zombie (baby x2.5), Hoglin (baby 3) and Chicken
        // (jockey 10) override.
        virtual int GetXpReward() const { return TypeInfo().xpReward; }
        // MC LivingEntity.getExperienceReward for a mob: its base reward
        // through the killer's mob_experience enchantment effects
        // (EnchantmentHelper.processMobExperience).
        int GetExperienceReward(EntityLevel& level, Entity* killer) override;

        // MC Mob.isWithinMeleeAttackRange — an AABB overlap test against the
        // attacker's box inflated by the reach, NOT a centre-to-centre
        // distance. Using distance instead makes wide mobs (spiders) unable to
        // reach a target their body is already touching.
        bool IsWithinMeleeAttackRange(const LivingEntity& target) const;

        static constexpr double kDefaultAttackReach = 0.8284271247461903; // sqrt(2.04) - 0.6

        // ── Inputs (Mob couples speed to forward motion) ───────────────────
        // MC Mob.setSpeed sets BOTH `speed` and `zza`. A mob that only had its
        // `speed` set would face the right way and never move.
        void SetSpeed(float s) override {
            const float v = s * m_landSpeedFactor;
            LivingEntity::SetSpeed(v);
            zza = v;
        }

        // MC SmoothSwimmingMoveControl's outsideWaterSpeedModifier — the last
        // multiplier before setSpeed, applied only by the mobs MC gives that
        // control to (frog, dolphin, tadpole 0.1; nautilus 0.0). Those mobs
        // carry a MOVEMENT_SPEED of 1.0 or more because it is a SWIM speed;
        // without the factor a frog walks at five times a cow. 1.0 for every
        // mob on the ordinary MoveControl, which is all the rest.
        float GetLandSpeedFactor() const { return m_landSpeedFactor; }
        void  SetLandSpeedFactor(float f) { m_landSpeedFactor = f; }

        // MC LivingEntity.updateWalkAnimation and the three overrides of it.
        // The defaults ARE LivingEntity's, so a hand-written mob that never
        // calls the setter behaves exactly as before.
        void SetWalkAnimParams(float scale, float cap, float factor, float babyScale) {
            m_walkAnimScale = scale;
            m_walkAnimCap = cap;
            m_walkAnimFactor = factor;
            m_walkAnimBabyScale = babyScale;
        }

    protected:
        void UpdateWalkAnimation(float distance) override {
            const float target = std::min(distance * m_walkAnimScale, m_walkAnimCap);
            walkAnimation.Update(target, m_walkAnimFactor,
                                 IsBaby() ? m_walkAnimBabyScale : 1.0f);
        }

    public:
        // ── Animation state timers (MC AnimationState) ─────────────────────
        //
        // MC gives each mob named fields; this port gives every mob the same
        // slot table (see MobAnim) so the generated model can name a slot
        // without knowing which class it is talking to.
        //
        // Allocated on first use: nine of ninety mob types have any timers at
        // all, and a fixed member would put 140 unused bytes on every zombie.
        AnimationState& Anim(MobAnim slot) {
            if (!m_animStates) m_animStates = std::make_unique<MobAnimationStates>();
            return (*m_animStates)[static_cast<size_t>(slot)];
        }
        // The const read never allocates — a mob that has never started a clip
        // reports every slot stopped, which is the truth.
        const AnimationState& Anim(MobAnim slot) const {
            static const AnimationState kStopped;
            if (!m_animStates) return kStopped;
            return (*m_animStates)[static_cast<size_t>(slot)];
        }
        bool HasAnimStates() const { return m_animStates != nullptr; }

        // MC's per-mob `setupAnimationStates()`. Called from Tick, CLIENT-SIDE
        // ONLY, exactly where MC calls it — the timers are derived from synched
        // state rather than sent, so running this on the server would start
        // clips nobody ever sees and burn the server's RNG stream doing it.
        virtual void SetupAnimationStates() {}

        // The one synched byte a mob's animations key on beyond the pose — MC's
        // per-class enum (Armadillo.ARMADILLO_STATE, Bat's resting bit). The
        // meaning is private to the subclass on both sides; the tracker just
        // ships whatever the server's copy reports and the client hands it back.
        virtual uint8_t GetAnimStateByte() const { return 0; }
        virtual void    SetAnimStateByte(uint8_t v) { (void)v; }
        // The animation state a mob's own tick eases on BOTH sides and MC
        // never saves nor sends — a wolf's beg tilt and shake, a cat's
        // lie-down, a fox's head tilt and crouch — as up to kRenderPhaseMax
        // floats (the layout private to the subclass). Carried from the
        // host's client at Save and Quit, through the save (obey_render_phase)
        // and the spawn packet, and restored as is, previous-tick values
        // included, so a rejoined world shows the pose the last-world
        // panorama does. Set restores exactly what Get returned.
        static constexpr int kRenderPhaseMax = 8;
        virtual int  GetRenderPhase(float* out) const { (void)out; return 0; }
        virtual void SetRenderPhase(const float* in, int count) { (void)in; (void)count; }
        // Whether the tracker sends this mob's attributes (MC
        // ClientboundUpdateAttributesPacket, UpdateAttributesS2C) when a
        // watcher starts tracking it and whenever they change. MC syncs every
        // living entity's syncable attributes; here only the mobs whose
        // client copy needs them opt in — the mounts whose rolled speed,
        // jump and health the steering client simulates and shows.
        virtual bool SyncsAttributesToClient() const { return false; }
        // True when the client advances this byte itself from the value it
        // was given at spawn, so the tracker must NOT treat every tick's
        // change as dirty data. Primed TNT's fuse is the case: MC's client
        // counts it down locally and the server never resends it. Without
        // this the tracker shipped a SetEntityData packet per TNT per tick,
        // which at 100k TNT is two million packets a second.
        virtual bool AnimStateTicksOnClient() const { return false; }

        // The wire's per-mob VARIANT byte (AddEntityS2C / SetEntityData).
        // Same contract as the anim byte: the meaning is private to the type —
        // a sheep's wool data, a slime's size. The tracker ships the server's
        // value; the client hands it back here.
        virtual uint8_t GetVariantByte() const { return 0; }
        virtual void    SetVariantByte(uint8_t v) { (void)v; }

        // The block state a block-carrying mob shows (raw id, 0 = none):
        // the sulfur cube's swallowed block. MC syncs it as the BODY
        // equipment slot; here it rides AddEntityS2C's per-type data int
        // (which the falling block and primed TNT already use, read-only)
        // and, because a cube's block CHANGES, an appended SetEntityData
        // field. The tracker resends on change; the client applies it here.
        virtual uint32_t GetCarriedBlockRaw() const { return 0; }
        virtual void     SetCarriedBlockRaw(uint32_t raw) { (void)raw; }

        void SetZza(float v) { zza = v; }
        void SetXxa(float v) { xxa = v; }
        void SetYya(float v) { yya = v; }

        // MC Mob.stopInPlace — cancel navigation and all steering at once.
        void StopInPlace();

        // ── Conversion (MC Mob.convertTo + ConversionType.SINGLE) ──────────
        //
        // One mob becoming another in place: copy the shared state onto the
        // replacement, add it to the level, discard this. The caller
        // constructs the concrete replacement (MC's EntityType.create half)
        // and does its own per-family copies between CopyConversionState and
        // FinishConversion — see Zombie::ConvertToZombieType, the shape MC's
        // afterConversion callback takes.
        //
        // Returns the replacement, now owned by the level (null if this mob
        // is already removed). NOTE MC does NOT copy health: a fresh convert
        // stands at full health, and so does ours.
        Mob* ConvertTo(std::unique_ptr<Mob> replacement);

    protected:
        // The ConversionType.SINGLE + convertCommon copy, reduced to what
        // this port tracks. Copied: position/rotations/velocity/fallDistance/
        // hurtTime/onGround, passengers and vehicle, active effects, left
        // hand, NoAi, persistence, canPickUpLoot (MC preserveCanPickUpLoot —
        // true for every conversion this port runs), fire ticks, the lead
        // (setLeashedTo on the new mob). Not tracked by this port, so not
        // copied (each a system, not an oversight) — the equipment is the
        // caller's (MoveEquipmentTo, for a keepEquipment conversion):
        // absorption, sleeping pos, teams,
        // custom name, invulnerable/silent/noGravity flags, entity tags,
        // portal cooldown, the brain's ANGRY_AT memory.
        void CopyConversionState(Mob& to);
        Mob* FinishConversion(std::unique_ptr<Mob> replacement);

    public:

        void Tick() override;

        // Also clears the current target and forwards to every goal in both
        // selectors. See Goal::ClearReferenceTo.
        void ClearReferenceTo(const Entity* entity) override;

        // MC Mob.tickHeadTurn is replaced by the body rotation control.
        int GetMaxHeadXRot() const override { return 40; }
        int GetMaxHeadYRot() const override { return 75; }
        int GetHeadRotSpeed() const override { return 10; }

        // MC Mob.getAmbientSoundInterval — 80, Animal 120, and the per-class
        // overrides MC has (the generated row carries each type's answer).
        virtual int GetAmbientSoundInterval() const;

        // MC Mob.getAmbientSound: the idle voice, from the generated row
        // unless the class knows better. "" = none (MC null).
        virtual const char* GetAmbientSound() const;
        // MC Mob.playAmbientSound — makeSound(getAmbientSound()).
        virtual void PlayAmbientSound() { MakeSound(GetAmbientSound()); }
        // MC Mob.playHurtSound: a hurt mob restarts its ambient clock.
        void PlayHurtSound(MobDamageSource source) override {
            ResetAmbientSoundTime();
            LivingEntity::PlayHurtSound(source);
        }
        // MC Mob.resetAmbientSoundTime.
        void ResetAmbientSoundTime() { m_ambientSoundTime = -GetAmbientSoundInterval(); }

    protected:
        // Subclasses register their goals here. Called once from the concrete
        // mob's constructor — NOT from Mob's, because a virtual call during
        // base construction would dispatch to the base version.
        virtual void RegisterGoals() {}

        // MC Mob.customServerAiStep — per-mob work that must happen after
        // navigation but before the controls.
        //
        // A brain mob's whole AI runs from here: MC's Frog.customServerAiStep is
        // `getBrain().tick(level, this); FrogAi.updateActivity(this);` and
        // nothing else. TickBrain below is that first half.
        virtual void CustomServerAiStep() {}

        // Runs the brain and then lets the subclass choose the next activity.
        // Split so that a mob with a brain does not also have to remember the
        // tick order — MC ticks the brain BEFORE updating the activity, so a
        // behaviour that writes a memory this tick is seen by the activity
        // switch on the SAME tick.
        void TickBrain();
        virtual void UpdateBrainActivity() {}

        void ServerAiStep() final;
        void TickHeadTurn(float yBodyRotTarget) override;
        void BaseTick() override;

        // MC Mob.aiStep — the base step, then the daylight burn. The order is
        // MC's and it matters: burnUndead draws from the level random, so
        // running it first would shift every roll the base step makes.
        void AiStep() override;

    protected:
        // MC's EntityTypeTags.BURN_IN_DAYLIGHT membership. A tag in vanilla,
        // an override here — with eight mobs a data file would be more
        // machinery than the two `return true`s it replaces.
        virtual bool BurnsInDaylight() const { return false; }

        // MC Entity.isSensitiveToWater — blaze and snow golem. Consulted by
        // AiStep's per-tick wet damage.
        virtual bool IsSensitiveToWater() const { return false; }

    public:
        // The same answer, readable from outside — a thrown water potion
        // hurts exactly these mobs (AbstractThrownPotion.affectEntitiesAround).
        bool SensitiveToWater() const { return IsSensitiveToWater(); }

    protected:

        // MC Mob.isSunBurnTick / burnUndead. NOT const: the brightness roll
        // consumes the level's random exactly once per tick per burning mob,
        // and that draw is part of the shared spawn/AI RNG stream.
        bool IsSunBurnTick();
        void BurnUndead();

        // MC Mob.updateControlFlags — every 5 ticks, and only relevant once
        // riding exists. Kept so the cadence is visible.
        void UpdateControlFlags();

        float m_landSpeedFactor = 1.0f;

        float m_walkAnimScale     = 4.0f;   // LivingEntity.java:2501
        float m_walkAnimCap       = 1.0f;
        float m_walkAnimFactor    = 0.4f;
        float m_walkAnimBabyScale = 3.0f;

        GoalSelector m_goalSelector;
        GoalSelector m_targetSelector;

        std::unique_ptr<MoveControl>         m_moveControl;
        std::unique_ptr<LookControl>         m_lookControl;
        std::unique_ptr<JumpControl>         m_jumpControl;
        std::unique_ptr<BodyRotationControl> m_bodyRotationControl;
        std::unique_ptr<PathNavigation>      m_navigation;
        std::unique_ptr<Sensing>             m_sensing;

        LivingEntity* m_target = nullptr;

        bool m_aggressive = false;
        bool m_noAi = false;
        bool m_persistenceRequired = false;
        int64_t m_heldUntil = 0;           // HoldByNote

        // MC LivingEntity.lastHurtByPlayer / lastHurtByPlayerMemoryTime
        // (LivingEntity.java:215-216) — the player kill-credit window, set to
        // 100 by Hurt and counted down in BaseTick (LivingEntity.java:437-440).
        // Stored as an entity id, not a pointer: the crediting player may
        // disconnect before the mob dies.
        int32_t m_lastHurtByPlayerId   = -1;
        int     m_lastHurtByPlayerTime = 0;
        bool m_leftHanded = false;
        bool m_canPickUpLoot = false;

        int  m_noActionTime = 0;
        int  m_ambientSoundTime = 0;

        // MC Mob.homePosition / homeRadius (-1 = no home). See the accessor
        // block above.
        glm::ivec3 m_homePosition{0};
        int        m_homeRadius = -1;

        // Sparse: only the types a mob actually overrides. Everything else
        // falls through to PathType's default malus.
        std::unordered_map<uint8_t, float> m_pathfindingMalus;

        std::unique_ptr<MobAnimationStates> m_animStates;

        // MC Mob.leashData — null when no lead (a pointer, so the hundred
        // thousand primed TNT the pipeline carries pay 8 bytes, not 80).
        std::unique_ptr<Leash::LeashData> m_leashData;

        // The humanoid equipment and its drop chances (see the accessor block
        // above). Null until something is equipped or a chance changes.
        struct EquipmentSlots {
            ItemStack items[kEquipmentSlotCount];
            float     dropChances[kEquipmentSlotCount] = {
                kDefaultEquipmentDropChance, kDefaultEquipmentDropChance, kDefaultEquipmentDropChance,
                kDefaultEquipmentDropChance, kDefaultEquipmentDropChance, kDefaultEquipmentDropChance,
                kDefaultEquipmentDropChance, kDefaultEquipmentDropChance };
            uint8_t   dirtyMask = 0;
        };
        std::unique_ptr<EquipmentSlots> m_equipment;
    };

    // MC PathfinderMob — a Mob that walks. Adds the walk-target cost that the
    // spawner uses to reject a position the mob would immediately flee.
    class PathfinderMob : public Mob {
    public:
        PathfinderMob(EntityTypeId type, EntityLevel* level) : Mob(type, level) {}

        virtual float GetWalkTargetValue(const glm::ivec3& pos) const { return 0.0f; }

        // MC PathfinderMob.checkSpawnRules — the walk-target value at the
        // spawn position must not be negative. For a Monster that means
        // brightness <= 12; for an Animal, grass or brightness >= 12.
        bool CheckSpawnRules(EntityLevel& level, SpawnReason reason) override {
            (void)level; (void)reason;
            return GetWalkTargetValue(BlockPosition()) >= 0.0f;
        }

        bool IsPathFinding() const;
        bool IsPanicking() const { return m_goalSelector.IsRunning("PanicGoal"); }

        // ── MC PathfinderMob's leash behaviour (MobLeash.cpp) ──────────────
        // Inside the elastic range a leashed mob walks back toward its
        // holder, and its home follows the holder (radius elastic - 1).
        void CloseRangeLeashBehaviour(Entity& holder) override;
        void WhenLeashedTo(Entity& holder) override;
        // MC shouldStayCloseToLeashHolder — false for the allay and the happy
        // ghast, which float where the lead lets them.
        virtual bool ShouldStayCloseToLeashHolder() const;
        // MC followLeashSpeed — 1.0; the llama's 2.0.
        virtual double FollowLeashSpeed() const;
    };

} // namespace Game
