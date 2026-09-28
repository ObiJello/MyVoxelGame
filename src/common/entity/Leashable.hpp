// File: src/common/entity/Leashable.hpp
//
// MC net.minecraft.world.entity.Leashable — leads.
//
// In MC Leashable is an interface with default methods that Mob (and the
// boats, which this engine does not have) implements. Here the per-entity
// half — LeashData, the virtual hooks a mob class overrides — lives on
// Game::Mob (Mob.hpp's "Leashable" block, implemented in MobLeash.cpp),
// and the interface's static half lives in this namespace: tickLeash, the
// elastic spring, the area queries, and the interaction entry points MC
// spreads over Entity.interact, LeadItem and LeashFenceKnotEntity.
//
// ── Who can be leashed ─────────────────────────────────────────────────
// `instanceof Leashable` is Mob::IsLeashable (every real mob; not the
// engine's pipeline riders — hanging entities, primed TNT, projectiles —
// which are Mobs here only because the tracking pipeline is Mob-shaped).
// `canBeLeashed` is Mob::CanBeLeashed: MC Mob's `!(this instanceof Enemy)`,
// with every per-class override of 26.3 ported (MobLeash.cpp).
// Players are never leashable; anything can HOLD a lead.
//
// ── DELIBERATE DIVERGENCES from MC (the user's rules) ──────────────────
//   * Every mob takes a lead (Mob::CanBeLeashed is always true) — hostiles,
//     fish, villagers. A hostile keeps its own AI: with something to attack
//     it does not walk back to its holder; the spring and the home around
//     the holder restrain it. A merchant that trades (a villager with a
//     working profession, the wandering trader) keeps its plain right-click
//     for trading; tying / untying it takes SHIFT+right-click.
//   * Leads never snap. The spring's pull is capped at MC's snap stretch,
//     and a mob more than twice the snap distance behind its holder is
//     teleported beside it (pet-style spot search). Shears, untying, death
//     of either end and the holder logging out still take the lead off.
//   * A player's lead ends in the engine's player model's hand
//     (StickFigureHand), or, in first person, where MC's LocalPlayer puts
//     the on-screen hand.
//
// ── Holder references ──────────────────────────────────────────────────
// MC keeps a Java reference to the holder and lets the next tickLeash see
// it removed (canInteractWithLevel). Here the holder is a raw pointer, so
// the engine's ClearReferenceTo sweep (which runs before any entity is
// freed) turns a vanishing holder into `holderLost`, and the next
// tickLeash drops the lead exactly as MC's removed-holder branch does.
//
// ── The wire ───────────────────────────────────────────────────────────
// MC sends ClientboundSetEntityLinkPacket from setLeashedTo / dropLeash and
// from ServerEntity.sendPairingData. Here the tracker compares each mob's
// holder id against the last one it sent and emits SetEntityLinkS2C on a
// change and on pairing — the same packets, from one place.
#pragma once

#include "common/core/Uuid.hpp"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace Game {

    class Entity;
    class Mob;
    class LivingEntity;
    struct EntityLevel;
    struct ItemStack;
    class LeashFenceKnot;
    enum class UseResult : int;

    namespace Leash {

        // MC Leashable's constants, verbatim.
        inline constexpr double kLeashTooFarDist           = 12.0;   // LEASH_TOO_FAR_DIST
        inline constexpr double kLeashElasticDist          = 6.0;    // LEASH_ELASTIC_DIST
        inline constexpr double kMaximumAllowedLeashedDist = 16.0;   // MAXIMUM_ALLOWED_LEASHED_DIST
        inline constexpr double kAxisElasticityX           = 0.8;    // AXIS_SPECIFIC_ELASTICITY
        inline constexpr double kAxisElasticityY           = 0.2;
        inline constexpr double kAxisElasticityZ           = 0.8;
        inline constexpr float  kSpringDampening           = 0.7f;   // SPRING_DAMPENING
        inline constexpr double kTorsionalElasticity       = 10.0;   // TORSIONAL_ELASTICITY
        inline constexpr double kStiffness                 = 0.11;   // STIFFNESS
        // The half-size of MC leashableInArea's scan box (AABB.ofSize(pos, 32,
        // 32, 32)).
        inline constexpr double kAreaScanHalf              = 16.0;
        // Ticks a leash restored from a save waits for its holder before the
        // lead drops (restoreLeashFromSave's `entity.tickCount > 100`).
        inline constexpr int    kRestoreGraceTicks         = 100;

        // "No holder" on the wire. MC sends entity id 0; entity id 0 is a
        // valid player (connection) id here, so the link uses -1.
        inline constexpr int32_t kNoHolder = -1;

        // MC LeashData.delayedLeashInfo — Either<UUID, BlockPos>: a holder
        // entity by UUID, or the fence a knot hangs on (knots are never
        // saved; the leashed mob remembers the fence and recreates it).
        struct HolderRef {
            std::optional<Uuid>       uuid;
            std::optional<glm::ivec3> knotPos;
            bool Empty() const { return !uuid && !knotPos; }
        };

        // MC Leashable.LeashData.
        struct LeashData {
            // The live holder (server). Null while a save's holder has not
            // been found yet, and after the holder vanished (holderLost).
            Entity*   holder = nullptr;
            // MC delayedLeashHolderId — the CLIENT's copy of the holder, an
            // entity id from SetEntityLinkS2C, resolved at render time
            // (Leashable.getLeashHolder's client branch).
            int32_t   delayedHolderId = kNoHolder;
            // MC delayedLeashInfo — the save's reference, pending resolution.
            HolderRef delayed;
            // The identity of the current (or just-vanished) holder, so a mob
            // saved in the tick between its holder vanishing and the lead
            // dropping still writes what MC would (the removed holder's UUID).
            HolderRef lastRef;
            // The holder was freed under us (ClearReferenceTo). MC's
            // leashHolder would still point at the removed entity; tickLeash
            // treats both the same way — the lead drops.
            bool      holderLost = false;
            // The holder is a player whose view left this level (they went
            // through a portal). MC's holder is the same ServerPlayer object
            // in another level: tickLeash skips it (`holder.level() ==
            // entity.level()` fails) and the lead holds until they come back
            // — or drops if they log out (their entity is removed). Here the
            // player is found again by `lastRef.uuid`.
            bool      awaitingPlayer = false;
            // Ticks spent waiting on `delayed` — MC reads the fresh entity's
            // own tickCount for the 100-tick grace; a restored mob here is not
            // a fresh object, so the grace is counted separately.
            int       restoreTicks = 0;
            // MC LeashData.angularMomentum — the spring's torque, applied to
            // yRot and bled off by angularFriction each tick.
            double    angularMomentum = 0.0;
        };

        // MC Leashable.Wrench.
        struct Wrench {
            glm::dvec3 force{0.0};
            double     torque = 0.0;
        };

        // MC Leashable.Wrench.torqueFromForce.
        inline double TorqueFromForce(const glm::dvec3& leverArm, const glm::dvec3& force) {
            return leverArm.z * force.x - leverArm.x * force.z;
        }

        // MC Vec3.yRot(angle) — the rotation every attachment offset goes
        // through, float trig as Mth.cos / Mth.sin.
        glm::dvec3 YRot(const glm::dvec3& v, float angle);
        // MC Vec3.xRot(angle).
        glm::dvec3 XRot(const glm::dvec3& v, float angle);

        // MC Entity.canInteractWithLevel: alive, not removed, not a spectator.
        bool CanInteractWithLevel(const Entity& e);

        // MC Leashable.createQuadLeashOffsets.
        std::array<glm::dvec3, 4> CreateQuadLeashOffsets(const Entity& entity, double frontOffset,
                                                        double frontBack, double leftRight,
                                                        double height);

        // The mob as MC's Leashable, or null: a real mob (IsLeashable), not a
        // pipeline rider. Every "instanceof Leashable" in MC goes through this.
        Mob*       AsLeashable(Entity* e);
        const Mob* AsLeashable(const Entity* e);

        // The save reference of a holder: a knot → its fence, anything else →
        // its UUID.
        HolderRef RefOf(const Entity& holder);

        // MC Leashable.leashableInArea(level, pos, test) — every leashable in
        // the 32-block box around `pos` that passes `test`.
        void LeashableInArea(EntityLevel& level, const glm::dvec3& pos,
                             const std::function<bool(const Mob&)>& test, std::vector<Mob*>& out);
        // MC leashableInArea(entity, test) — around the entity's box centre.
        void LeashableInArea(const Entity& entity, const std::function<bool(const Mob&)>& test,
                             std::vector<Mob*>& out);
        // MC Leashable.leashableLeashedTo(entity).
        void LeashableLeashedTo(const Entity& holder, std::vector<Mob*>& out);

        // ── Server ticks ────────────────────────────────────────────────
        // MC Leashable.tickLeash — from the end of Entity.baseTick, server
        // only (Mob::TickLeash).
        void TickLeash(Mob& mob);

        // ── Knots ───────────────────────────────────────────────────────
        // MC LeashFenceKnotEntity.getKnot / createKnot / getOrCreateKnot.
        LeashFenceKnot* GetKnot(EntityLevel& level, const glm::ivec3& pos);
        LeashFenceKnot* CreateKnot(EntityLevel& level, const glm::ivec3& pos);
        LeashFenceKnot* GetOrCreateKnot(EntityLevel& level, const glm::ivec3& pos);

        // ── Interactions (server) ───────────────────────────────────────
        //
        // MC LeadItem.bindPlayerMobs — every mob `player` holds within the
        // 32-box of the fence goes onto the fence's knot (made if missing).
        // FenceBlock.useWithoutItem and LeadItem.useOn both land here.
        UseResult BindPlayerMobs(LivingEntity& player, EntityLevel& level, const glm::ivec3& pos);

        // The leash half of MC Entity.interact, for a mob `target`: a sneaking
        // player hands their mobs to it, shears cut every lead on it, its own
        // holder unties it, and a lead in hand ties it to the player.
        // `held` is the hand's live stack; `secondaryUse` is the sneak key;
        // `infiniteMaterials` is creative (the lead is kept, the untie drops
        // nothing). Pass = not a leash interaction.
        UseResult EntityInteract(Mob& target, LivingEntity& player, ItemStack& held,
                                 bool secondaryUse, bool infiniteMaterials);

        // MC LeashFenceKnotEntity.interact: shears cut the knot's leads, a
        // player's mobs go onto the knot, or (not sneaking) the knot's mobs
        // come back to the player.
        UseResult KnotInteract(LeashFenceKnot& knot, LivingEntity& player, ItemStack& held,
                               bool secondaryUse);

        // MC Entity.shearOffAllLeashConnections / dropAllLeashConnections:
        // every lead on `entity` (held by it, or its own) drops; the snip
        // plays when any did.
        bool ShearOffAllLeashConnections(Entity& entity, LivingEntity* player);
        bool DropAllLeashConnections(Entity& entity, LivingEntity* player);

    } // namespace Leash

} // namespace Game
