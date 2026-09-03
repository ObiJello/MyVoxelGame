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
#include "common/entity/SpawnReason.hpp"

#include <algorithm>
#include "common/entity/AnimationState.hpp"
#include "common/entity/Item.hpp"
#include "common/entity/ai/Goal.hpp"
#include "common/entity/ai/Controls.hpp"
#include "common/world/pathfinder/PathType.hpp"

#include <memory>
#include <unordered_map>

namespace Game {

    class PathNavigation;
    class Sensing;

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

        // ── Target ─────────────────────────────────────────────────────────
        LivingEntity* GetTarget() const { return m_target; }
        virtual void  SetTarget(LivingEntity* target) {
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
        virtual bool RequiresCustomPersistence() const { return IsPassenger(); }

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

        // MC Mob.canPickUpLoot — rolled at spawn for zombies/skeletons. No
        // item-pickup system consumes it yet; the flag keeps the spawn rolls
        // and the eventual behaviour in one place.
        bool CanPickUpLoot() const { return m_canPickUpLoot; }
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
        // and no other entity already occupying it.
        virtual bool CheckSpawnObstruction(EntityLevel& level) const;

        // MC Mob.dropCustomDeathLoot — the non-table drops (an enderman's
        // carried block, equipment when that exists). Called by the server's
        // loot pass alongside the generated table.
        virtual void DropCustomDeathLoot(EntityLevel& level) { (void)level; }

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
        // piece with a full drop chance, Mob.java:330-338) is skipped: no mob
        // equipment system. Animal (1..3 random), Fish/Squid/Dolphin (same),
        // Slime (its size), Zombie (baby x2.5), Hoglin (baby 3) and Chicken
        // (jockey 10) override.
        virtual int GetXpReward() const { return TypeInfo().xpReward; }

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
        // true for every conversion this port runs), fire ticks. Not tracked
        // by this port, so not copied (each a system, not an oversight):
        // equipment + drop chances, absorption, sleeping pos, leashes, teams,
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

        // MC Mob.getAmbientSoundInterval — Animal overrides to 120.
        virtual int GetAmbientSoundInterval() const { return 80; }

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
    };

} // namespace Game
