// File: src/common/entity/MobLeash.cpp
//
// Mob's half of MC Leashable (Mob.hpp's "Leashable" block), and the 26.3
// per-class leash overrides.
//
// MC spreads those overrides over thirty mob classes (Wolf.canBeLeashed,
// Fox.getLeashOffset, AbstractHorse.supportQuadLeash, ...). They are ported
// here as one table keyed on the entity type, each case naming the MC class
// it comes from: every one of them is a constant or a read of state the mob
// already exposes, and a hook in thirty class files would spread one system
// over thirty places.
#include "common/entity/Mob.hpp"

#include "common/entity/EntityLevel.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/entity/Item.hpp"
#include "common/entity/Leashable.hpp"
#include "common/entity/ai/Goal.hpp"
#include "common/entity/ai/brain/Brain.hpp"
#include "common/entity/ai/navigation/PathNavigation.hpp"
#include "common/entity/mobs/Animals.hpp"
#include "common/entity/mobs/AnimatedMobs.hpp"
#include "common/entity/mobs/Fish.hpp"
#include "common/entity/projectile/Projectile.hpp"
#include "common/core/JavaRandom.hpp"

#include <algorithm>
#include <cmath>

namespace Game {

    namespace {

        // MC PathfinderMob.isPanicking: the brain's IS_PANICKING memory for a
        // brain mob, else a running PanicGoal.
        bool IsPanickingMc(const PathfinderMob& mob) {
            const Brain* brain = mob.GetBrain();
            if (brain && brain->HasMemoryValue(MemoryModule::IsPanicking)) return true;
            return mob.IsPanicking();
        }

    } // namespace

    // ── Who can be leashed ─────────────────────────────────────────────────

    bool Mob::IsLeashable() const {
        // The pipeline riders: built through NoAiTag (hanging entities, TNT,
        // falling blocks, the crystal, the armor stand) or a projectile.
        return HasAiControls() && dynamic_cast<const Projectile*>(this) == nullptr;
    }

    bool Mob::CanBeLeashed() const {
        // DELIBERATE DIVERGENCE (the user's rule): EVERY mob takes a lead —
        // hostiles, fish, bats, villagers, angry wolves. MC's Mob.canBeLeashed
        // is `!(this instanceof Enemy)` with thirty per-class overrides (the
        // ambient and water mobs, the panda, the turtle, the villagers, an
        // angry wolf or nautilus, a jockey's mount); none of them apply here.
        // Players are never leashable (not Mobs), and the engine's pipeline
        // riders are ruled out by IsLeashable. A villager or trader that can
        // trade still trades on a plain right-click — Leash::EntityInteract.
        return true;
    }

    // ── State ──────────────────────────────────────────────────────────────

    bool Mob::IsLeashed() const {
        if (!m_leashData) return false;
        if (m_level && m_level->IsClientSide()) return m_leashData->delayedHolderId != Leash::kNoHolder;
        return m_leashData->holder != nullptr || m_leashData->holderLost || m_leashData->awaitingPlayer;
    }

    Entity* Mob::GetLeashHolder() const {
        return m_leashData ? m_leashData->holder : nullptr;
    }

    int32_t Mob::GetLeashHolderNetId() const {
        if (!m_leashData) return Leash::kNoHolder;
        if (m_level && m_level->IsClientSide()) return m_leashData->delayedHolderId;
        const Entity* holder = m_leashData->holder;
        if (!holder) return Leash::kNoHolder;
        // A knot made this tick waits in the level's spawn queue without an
        // id; the link goes out once the manager has given it one.
        if (!holder->IsPlayer() && holder->GetId() == 0) return Leash::kNoHolder;
        return holder->GetId();
    }

    double Mob::LeashDistanceTo(const Entity& entity) const {
        // MC: bounding-box centre to bounding-box centre.
        const AABBd a = entity.GetAABBd();
        const AABBd b = GetAABBd();
        return glm::length((a.min + a.max) * 0.5 - (b.min + b.max) * 0.5);
    }

    bool Mob::CanHaveALeashAttachedTo(const Entity& entity) const {
        if (static_cast<const Entity*>(this) == &entity) return false;
        // DELIBERATE DIVERGENCE: no `leashDistanceTo > leashSnapDistance`
        // refusal. Leads never snap here (Leash::TickLeash), so a mob towed
        // along 20 blocks behind its holder must still go onto the fence the
        // holder clicks; the interaction reach and the 32-block scan box of
        // every caller bound how far an attach can reach.
        return CanBeLeashed();
    }

    double Mob::LeashSnapDistance() const {
        switch (GetType()) {
            case EntityTypeId::Ghast:       // Ghast / HappyGhast.leashSnapDistance
            case EntityTypeId::HappyGhast:
                return Leash::kMaximumAllowedLeashedDist;
            default:
                return Leash::kLeashTooFarDist;
        }
    }

    double Mob::LeashElasticDistance() const {
        switch (GetType()) {
            case EntityTypeId::Ghast:       // Ghast / HappyGhast.leashElasticDistance
            case EntityTypeId::HappyGhast:
                return 10.0;
            default:
                return Leash::kLeashElasticDist;
        }
    }

    bool Mob::SupportQuadLeash() const {
        switch (GetType()) {
            // AbstractHorse.supportQuadLeash (the llama, an AbstractHorse in
            // MC, turns it back off) and Sniffer.supportQuadLeash.
            case EntityTypeId::Horse:
            case EntityTypeId::Donkey:
            case EntityTypeId::Mule:
            case EntityTypeId::SkeletonHorse:
            case EntityTypeId::ZombieHorse:
            case EntityTypeId::Camel:
            case EntityTypeId::CamelHusk:
            case EntityTypeId::Sniffer:
                return true;
            default:
                return false;
        }
    }

    std::array<glm::dvec3, 4> Mob::GetQuadLeashOffsets() const {
        switch (GetType()) {
            case EntityTypeId::Horse:           // AbstractHorse
            case EntityTypeId::SkeletonHorse:
                return Leash::CreateQuadLeashOffsets(*this, 0.04, 0.52, 0.23, 0.87);
            case EntityTypeId::Donkey:          // AbstractChestedHorse
            case EntityTypeId::Mule:
            case EntityTypeId::ZombieHorse:     // ZombieHorse
                return Leash::CreateQuadLeashOffsets(*this, 0.04, 0.41, 0.18, 0.73);
            case EntityTypeId::Camel:           // Camel
            case EntityTypeId::CamelHusk:
                return Leash::CreateQuadLeashOffsets(*this, 0.02, 0.48, 0.25, 0.82);
            case EntityTypeId::Sniffer:         // Sniffer
                return Leash::CreateQuadLeashOffsets(*this, -0.01, 0.63, 0.38, 1.15);
            default:                            // Leashable.getQuadLeashOffsets
                return Leash::CreateQuadLeashOffsets(*this, 0.0, 0.5, 0.5, 0.5);
        }
    }

    glm::dvec3 Mob::GetLeashOffset(float partialTicks) const {
        const float eye = GetEyeHeight();
        const float width = GetBbWidth();
        // Each case is `new Vec3(0, a * eyeHeight, width * b)` in its class,
        // float products widened as MC's casts widen them.
        const auto offset = [](float y, float z) {
            return glm::dvec3(0.0, static_cast<double>(y), static_cast<double>(z));
        };
        switch (GetType()) {
            case EntityTypeId::Fox:        return offset(0.55f * eye, width * 0.4f);
            case EntityTypeId::Pig:
            case EntityTypeId::Wolf:
            case EntityTypeId::Strider:    return offset(0.6f * eye, width * 0.4f);
            case EntityTypeId::Parrot:     return offset(0.5f * eye, width * 0.4f);
            case EntityTypeId::Bee:        return offset(0.5f * eye, width * 0.2f);
            case EntityTypeId::IronGolem:  return offset(0.875f * eye, width * 0.4f);
            case EntityTypeId::SnowGolem:  return offset(0.75f * eye, width * 0.4f);
            case EntityTypeId::CopperGolem: return offset(0.75f * eye, 0.0f);
            // Allay: (0, eyeHeight * 0.6, width * 0.1) in double.
            case EntityTypeId::Allay:
                return glm::dvec3(0.0, static_cast<double>(eye) * 0.6, static_cast<double>(width) * 0.1);
            // Llama: (0, 0.75 * eyeHeight, width * 0.5) in double.
            case EntityTypeId::Llama:
            case EntityTypeId::TraderLlama:
                return glm::dvec3(0.0, 0.75 * static_cast<double>(eye), static_cast<double>(width) * 0.5);
            // SulfurCube: (0, bbHeight / 2, 0).
            case EntityTypeId::SulfurCube:
                return glm::dvec3(0.0, static_cast<double>(GetBbHeight() / 2.0f), 0.0);
            // HappyGhast: Vec3.ZERO — the lead meets the harness's middle.
            case EntityTypeId::HappyGhast:
                return glm::dvec3(0.0);
            // Camel.getLeashOffset(partialTicks): on the front anchor of the
            // body, which the sit/stand clips move.
            case EntityTypeId::Camel:
            case EntityTypeId::CamelHusk:
                if (const auto* camel = dynamic_cast<const Camel*>(this)) {
                    const float ageScale = IsBaby() ? 0.6f : 1.0f;   // Camel.getAgeScale
                    // getDimensions(getPose()): the sitting box while sat
                    // (ADULT / BABY_SITTING_DIMENSIONS), else the standing
                    // one; the SCALE attribute on top of either.
                    float dimWidth = width;
                    float dimHeight = GetBbHeight();
                    if (GetPose() == Pose::Sitting) {
                        const EntityTypeInfo& camelType = GetEntityTypeInfo(EntityTypeId::Camel);
                        dimWidth  = (IsBaby() ? 0.95f : camelType.width) * scale;
                        dimHeight = (IsBaby() ? 0.425f : camelType.height - 1.43f) * scale;
                    }
                    const double y = camel->GetBodyAnchorAnimationYOffset(/*isFront=*/true, partialTicks,
                                                                          dimHeight, ageScale) -
                                     static_cast<double>(0.2f * ageScale);
                    return glm::dvec3(0.0, y, static_cast<double>(dimWidth * 0.56f));
                }
                break;
            default:
                break;
        }
        // Leashable.getLeashOffset.
        return offset(eye, width * 0.4f);
    }

    // ── Attaching and detaching ────────────────────────────────────────────

    void Mob::SetLeashedTo(Entity& holder, bool synch) {
        (void)synch;   // the tracker sends every holder change (Leashable.hpp)
        if (static_cast<Entity*>(this) == &holder) return;

        Entity* oldHolder = nullptr;
        if (!m_leashData) {
            m_leashData = std::make_unique<Leash::LeashData>();
        } else {
            oldHolder = m_leashData->holder;
        }
        // LeashData.setLeashHolder: the holder, and the pending references
        // cleared.
        m_leashData->holder = &holder;
        m_leashData->holderLost = false;
        m_leashData->awaitingPlayer = false;
        m_leashData->delayed = Leash::HolderRef{};
        m_leashData->delayedHolderId = Leash::kNoHolder;
        m_leashData->restoreTicks = 0;
        m_leashData->lastRef = Leash::RefOf(holder);
        // A raw pointer to another entity now lives here; the removal sweep
        // must visit this mob (Entity::HoldsEntityRefs).
        MarkHoldsEntityRefs();

        if (oldHolder && oldHolder != &holder) oldHolder->NotifyLeasheeRemoved(*this);

        // MC: `if (entity.isPassenger()) entity.stopRiding()`.
        if (IsPassenger()) StopRiding();
    }

    namespace {
        // MC Leashable.dropLeash(entity, sendPacket, dropLead). The packet is
        // the tracker's (it sees the holder id go to none).
        void DropLeashImpl(Mob& mob, std::unique_ptr<Leash::LeashData>& data, bool dropLead) {
            if (!data || !(data->holder || data->holderLost || data->awaitingPlayer)) return;
            Entity* holder = data->holder;
            data.reset();
            mob.OnLeashRemoved();
            EntityLevel* level = mob.Level();
            if (!level || level->IsClientSide()) return;
            if (dropLead) {
                // Entity.spawnAtLocation(level, Items.LEAD): an ItemEntity at
                // the feet with ItemEntity's random hop and the default
                // pickup delay.
                JavaRandom& rng = level->Random();
                const double vx = rng.NextDouble() * 0.2 - 0.1;
                const double vz = rng.NextDouble() * 0.2 - 0.1;
                level->SpawnThrownItem(mob.position, glm::dvec3(vx, 0.2, vz),
                                       ItemStack(Items::Lead, 1), /*pickupDelay=*/10);
            }
            if (holder) holder->NotifyLeasheeRemoved(mob);
        }
    } // namespace

    void Mob::DropLeash()   { DropLeashImpl(*this, m_leashData, /*dropLead=*/true); }
    void Mob::RemoveLeash() { DropLeashImpl(*this, m_leashData, /*dropLead=*/false); }

    void Mob::SetDelayedLeashHolderId(int32_t entityId) {
        // Client only: the link the server sent, resolved when drawn.
        if (entityId == Leash::kNoHolder) {
            m_leashData.reset();
            return;
        }
        if (!m_leashData) m_leashData = std::make_unique<Leash::LeashData>();
        m_leashData->holder = nullptr;
        m_leashData->delayedHolderId = entityId;
    }

    void Mob::SetDelayedLeashRef(const Leash::HolderRef& ref) {
        // MC readLeashData: a lead that is no longer in the save comes off
        // (removeLeash), and the save's reference replaces whatever was here.
        if (ref.Empty()) {
            if (m_leashData) RemoveLeash();
            m_leashData.reset();
            return;
        }
        m_leashData = std::make_unique<Leash::LeashData>();
        m_leashData->delayed = ref;
        m_leashData->lastRef = ref;
    }

    Leash::HolderRef Mob::GetLeashSaveRef() const {
        // MC LeashData.CODEC: a knot → its fence; a holder → its UUID; else
        // the still-pending reference from the save.
        if (!m_leashData) return {};
        if (m_leashData->holder) return Leash::RefOf(*m_leashData->holder);
        if (m_leashData->holderLost || m_leashData->awaitingPlayer) return m_leashData->lastRef;
        return m_leashData->delayed;
    }

    void Mob::MarkLeashHolderLost() {
        if (!m_leashData || !m_leashData->holder) return;
        // A player's view goes when the player leaves this level — through a
        // portal as much as by logging out. MC keeps the lead on a player in
        // another dimension; tickLeash tells the two apart (Leashable.hpp).
        const bool player = m_leashData->holder->IsPlayer() && m_leashData->lastRef.uuid.has_value();
        m_leashData->holder = nullptr;
        if (player) m_leashData->awaitingPlayer = true;
        else        m_leashData->holderLost = true;
    }

    void Mob::CarryLeashAcrossLevels() {
        if (!m_leashData || !IsLeashable()) return;
        const Leash::HolderRef ref = GetLeashSaveRef();
        RemoveLeash();
        SetDelayedLeashRef(ref);
    }

    void Mob::OnStartedRiding(Entity& vehicle) {
        (void)vehicle;
        if (!IsLeashed()) return;
        // MC Mob.startRiding: dropLeash() — on the client that only clears
        // the copy (no item outside a ServerLevel).
        if (m_level && m_level->IsClientSide()) {
            m_leashData.reset();
            return;
        }
        DropLeash();
    }

    // ── Hooks ──────────────────────────────────────────────────────────────

    void Mob::OnLeashRemoved() {
        // MC Mob.onLeashRemoved: the holder-following home goes with the lead.
        if (!m_leashData) ClearHome();
    }

    void Mob::WhenLeashedTo(Entity& holder) {
        // Leashable.whenLeashedTo.
        holder.NotifyLeashHolder(*this);
    }

    void Mob::LeashTooFarBehaviour() {
        // Mob.leashTooFarBehaviour: the lead breaks, and the goals may not
        // steer until something re-enables MOVE (closeRangeLeashBehaviour).
        DropLeash();
        m_goalSelector.DisableControlFlag(GoalFlag::Move);
    }

    void Mob::CloseRangeLeashBehaviour(Entity& holder) {
        (void)holder;   // Leashable's default: nothing
    }

    void Mob::OnElasticLeashPull() {
        // Leashable.onElasticLeashPull → Entity.checkFallDistanceAccumulation:
        // a mob hauled up a cliff does not bank the fall it was pulled out of.
        if (velocity.y > -0.5 && fallDistance > 1.0f) fallDistance = 1.0f;

        switch (GetType()) {
            // AbstractHorse.onElasticLeashPull: a grazing horse looks up.
            case EntityTypeId::Horse:
            case EntityTypeId::Donkey:
            case EntityTypeId::Mule:
            case EntityTypeId::SkeletonHorse:
            case EntityTypeId::ZombieHorse:
                if (auto* horse = dynamic_cast<AbstractHorse*>(this); horse && horse->IsEating()) {
                    horse->SetEating(false);
                }
                break;
            // Camel.onElasticLeashPull: a seated camel is hauled to its feet
            // (canCamelChangePose is the headroom check the stand-up clip
            // already makes).
            case EntityTypeId::Camel:
            case EntityTypeId::CamelHusk:
                if (auto* camel = dynamic_cast<Camel*>(this);
                    camel && camel->IsCamelSitting() && !camel->IsInPoseTransition() &&
                    camel->CanCamelChangePose()) {
                    camel->StandUp();
                }
                break;
            // HappyGhast.onElasticLeashPull: getMoveControl().setWait().
            case EntityTypeId::HappyGhast:
                if (HasAiControls()) GetMoveControl().SetWait();
                break;
            default:
                break;
        }
    }

    bool Mob::CheckElasticInteractions(Entity& holder, Leash::LeashData& data) {
        // MC Leashable.checkElasticInteractions.
        const bool quadConnection = holder.SupportQuadLeashAsHolder() && SupportQuadLeash();

        static const glm::dvec3 kEntityAttachmentPoint(0.0, 0.5, 0.5);
        static const glm::dvec3 kLeasherAttachmentPoint(0.0, 0.5, 0.0);
        static const std::array<glm::dvec3, 4> kSharedQuadAttachmentPoints = {
            glm::dvec3(-0.5, 0.5, 0.5), glm::dvec3(-0.5, 0.5, -0.5),
            glm::dvec3(0.5, 0.5, -0.5), glm::dvec3(0.5, 0.5, 0.5)};

        // Leashable.getHolderMovement: a NoAI mob counts as still.
        const auto holderMovement = [](const Entity& e) -> glm::dvec3 {
            if (const auto* mob = dynamic_cast<const Mob*>(&e); mob && mob->IsNoAi()) return glm::dvec3(0.0);
            return e.GetKnownMovement();
        };

        // computeElasticInteraction.
        const double slackDistance = LeashElasticDistance();
        const glm::dvec3 currentMovement = holderMovement(*this);
        const float entityYRot = yRot * 0.017453292f;
        const glm::dvec3 entityDimensions(GetBbWidth(), GetBbHeight(), GetBbWidth());
        const float leashHolderYRot = holder.yRot * 0.017453292f;
        const glm::dvec3 leasherDimensions(holder.GetBbWidth(), holder.GetBbHeight(), holder.GetBbWidth());

        const size_t count = quadConnection ? 4 : 1;
        bool any = false;
        Leash::Wrench sum;
        for (size_t i = 0; i < count; ++i) {
            const glm::dvec3 entityPoint = quadConnection ? kSharedQuadAttachmentPoints[i] : kEntityAttachmentPoint;
            const glm::dvec3 leasherPoint = quadConnection ? kSharedQuadAttachmentPoints[i] : kLeasherAttachmentPoint;
            const glm::dvec3 entityAttachVector = Leash::YRot(entityPoint * entityDimensions, -entityYRot);
            const glm::dvec3 entityAttachPos = position + entityAttachVector;
            const glm::dvec3 leasherAttachVector = Leash::YRot(leasherPoint * leasherDimensions, -leashHolderYRot);
            const glm::dvec3 leasherAttachPos = holder.position + leasherAttachVector;

            // computeDampenedSpringInteraction(pivot = leasher, object =
            // entity, slack, motion, lever arm).
            const double distance = glm::length(entityAttachPos - leasherAttachPos);
            if (distance < slackDistance) continue;
            const glm::dvec3 toPivot = leasherAttachPos - entityAttachPos;
            const double len = glm::length(toPivot);
            // DELIBERATE DIVERGENCE: the stretch is capped where MC's lead
            // would have snapped (leashSnapDistance). Leads never break here,
            // so without the cap a mob left 40 blocks behind would be flung
            // at 25+ blocks a tick; with it, the tow never pulls harder than
            // the strongest pull a vanilla lead survives.
            const double stretch = std::min(distance - slackDistance, LeashSnapDistance() - slackDistance);
            // Vec3.normalize: a vector shorter than 1e-5 normalises to zero.
            glm::dvec3 displacement = len < 1.0e-5 ? glm::dvec3(0.0) : toPivot / len * stretch;
            const double torque = Leash::TorqueFromForce(entityAttachVector, displacement);
            const bool sameDirectionToMovement = glm::dot(currentMovement, displacement) >= 0.0;
            if (sameDirectionToMovement) displacement *= 0.30000001192092896;
            sum.force += displacement;
            sum.torque += torque;
            any = true;
        }
        if (!any) return false;

        // Wrench.accumulate(...).scale(quad ? 0.25 : 1).
        const double scaleBy = quadConnection ? 0.25 : 1.0;
        sum.force *= scaleBy;
        sum.torque *= scaleBy;
        data.angularMomentum += Leash::kTorsionalElasticity * sum.torque;
        const glm::dvec3 relativeVelocityToLeasher = holderMovement(holder) - GetKnownMovement();
        const glm::dvec3 push =
            sum.force * glm::dvec3(Leash::kAxisElasticityX, Leash::kAxisElasticityY, Leash::kAxisElasticityZ) +
            relativeVelocityToLeasher * Leash::kStiffness;
        // MC Entity.addDeltaMovement — a plain velocity add, no impulse flag
        // (the mob's own movement packets carry the result). Not the engine's
        // AddDeltaMovement, which forces a velocity packet every tick.
        if (std::isfinite(push.x) && std::isfinite(push.y) && std::isfinite(push.z)) {
            velocity += push;
            physicsParked = false;
        }
        return true;
    }

    void Mob::TickLeash() {
        if (!m_leashData || !IsLeashable()) return;
        Leash::TickLeash(*this);
    }

    // ── PathfinderMob ──────────────────────────────────────────────────────

    bool PathfinderMob::ShouldStayCloseToLeashHolder() const {
        // Leashed hostiles keep their own behaviour (the user's rule): a mob
        // with something to attack pursues it with its own goals / brain and
        // the lead only restrains it (the spring past the slack, and the home
        // around the holder that whenLeashedTo keeps setting). Only an idle
        // one walks back to its holder, as a leashed animal does.
        if (const LivingEntity* target = GetTarget(); target && target->IsAlive()) return false;
        if (const Brain* brain = GetBrain(); brain && brain->HasMemoryValue(MemoryModule::AttackTarget)) {
            return false;
        }
        switch (GetType()) {
            case EntityTypeId::Allay:       // Allay.shouldStayCloseToLeashHolder
            case EntityTypeId::HappyGhast:  // HappyGhast.shouldStayCloseToLeashHolder
                return false;
            default:
                return true;
        }
    }

    double PathfinderMob::FollowLeashSpeed() const {
        switch (GetType()) {
            case EntityTypeId::Llama:       // Llama.followLeashSpeed
            case EntityTypeId::TraderLlama:
                return 2.0;
            default:
                return 1.0;
        }
    }

    void PathfinderMob::CloseRangeLeashBehaviour(Entity& holder) {
        // MC PathfinderMob.closeRangeLeashBehaviour: walk back to two blocks
        // from the holder.
        Mob::CloseRangeLeashBehaviour(holder);
        if (!HasAiControls()) return;
        if (ShouldStayCloseToLeashHolder() && !IsPanickingMc(*this)) {
            m_goalSelector.EnableControlFlag(GoalFlag::Move);
            const float distanceTo = static_cast<float>(DistanceTo(holder));
            const glm::dvec3 toHolder = holder.position - position;
            const double len = glm::length(toHolder);
            const glm::dvec3 dir = len < 1.0e-5 ? glm::dvec3(0.0) : toHolder / len;
            const glm::dvec3 delta = dir * static_cast<double>(std::max(distanceTo - 2.0f, 0.0f));
            GetNavigation().MoveTo(position.x + delta.x, position.y + delta.y, position.z + delta.z,
                                   FollowLeashSpeed());
        }
    }

    void PathfinderMob::WhenLeashedTo(Entity& holder) {
        // MC PathfinderMob.whenLeashedTo: the home follows the holder.
        SetHomeTo(holder.BlockPosition(), static_cast<int>(LeashElasticDistance()) - 1);
        Mob::WhenLeashedTo(holder);
    }

} // namespace Game
