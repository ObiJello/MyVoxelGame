// File: src/common/world/portal/PortalState.hpp
//
// Port of net.minecraft.world.entity.PortalProcessor plus the two portal
// fields Entity carries alongside it (`portalProcess`, `portalCooldown`).
//
// WHY THE TWO ARE ONE CLASS HERE
// ------------------------------
// MC keeps them apart because `portalProcess` is a nullable object and
// `portalCooldown` is a plain int that outlives it. They are still the same
// piece of state — "where am I in the business of using a portal" — and they
// are only ever read and written together. Folding them into one type means a
// holder gains portal support by declaring one member, which matters because
// this engine has TWO holders: `Game::Entity` (mobs, items, projectiles) and
// `Server::ServerPlayer`, which is NOT an Entity subclass. MC's method names
// are kept so the mapping back to the Java stays obvious.
//
// WHAT THIS CLASS DELIBERATELY DOES NOT DO
// ----------------------------------------
// It does not teleport anything. It answers one question — "did a portal just
// fire this tick, and which one" — and the server does the rest, because
// resolving a destination needs the other dimension's level, the portal index
// and the chunk ticket manager, none of which `common` can see.
#pragma once

#include "common/world/block/Blocks.hpp"

#include <cstdint>
#include <optional>
#include <glm/glm.hpp>

namespace Game {

    // MC Portal.Transition — what the CLIENT does while standing in the
    // portal, independent of the travel itself. Confusion is the nether
    // portal's nausea/warp; the End portal has none.
    enum class PortalLocalTransition : uint8_t { None, Confusion };

    namespace Portals {

        // MC Entity.getDimensionChangingDelay (Entity.java:2628) and its
        // overrides. This is the post-travel cooldown, NOT the time spent
        // standing in the portal — that is the transition time below.
        //
        // The player's 10 ticks is what lets you step straight back through a
        // portal you just arrived at; a mob's 300 is what stops a pig
        // ping-ponging between two linked portals forever.
        inline constexpr int kDefaultPortalCooldown    = 300;  // Entity.java:2630
        inline constexpr int kPlayerPortalCooldown     = 10;   // Player.java:384
        inline constexpr int kProjectilePortalCooldown = 2;    // Projectile.java

        // MC gamerules players_nether_portal_default_delay / _creative_delay
        // (GameRules.java:208-209). How long a player must STAND in a nether
        // portal before travelling. Creative is instant.
        inline constexpr int kNetherPortalPlayerDelay         = 80;
        inline constexpr int kNetherPortalPlayerCreativeDelay = 0;

        // Is this block one you can travel through?
        inline bool IsPortal(BlockID id) {
            return id == BlockID::NetherPortal || id == BlockID::EndPortal;
        }

        // MC NetherPortalBlock.getPortalTransitionTime (:99) and
        // EndPortalBlock, which does not override it and so answers 0.
        //
        // `invulnerable` is MC's `player.getAbilities().invulnerable`, true in
        // creative AND spectator — gating on "is creative" alone would leave a
        // spectator waiting 4 seconds in a block they are flying through.
        inline int GetTransitionTime(BlockID portal, bool isPlayer, bool invulnerable) {
            if (portal != BlockID::NetherPortal) return 0;
            // Non-players travel on the first tick inside. That is vanilla:
            // a minecart or a dropped item crosses instantly.
            if (!isPlayer) return 0;
            return invulnerable ? kNetherPortalPlayerCreativeDelay
                                : kNetherPortalPlayerDelay;
        }

        // MC NetherPortalBlock.getLocalTransition (:178) → CONFUSION;
        // EndPortalBlock does not implement it → NONE.
        inline PortalLocalTransition GetLocalTransition(BlockID portal) {
            return portal == BlockID::NetherPortal ? PortalLocalTransition::Confusion
                                                   : PortalLocalTransition::None;
        }

    } // namespace Portals

    class PortalState {
    public:
        // What HandleTick reports when the portal fires.
        struct Trigger {
            BlockID    portal;
            glm::ivec3 entryPos;
        };

        // MC Entity.setAsInsidePortal (Entity.java:2586). Called once per tick
        // per portal cell the holder overlaps, from the block's entityInside.
        //
        // The cooldown branch is the anti-ping-pong rule and it is easy to get
        // backwards: an entity that is ALREADY on cooldown does not start
        // accumulating portal time, it RE-ARMS the full cooldown. That is what
        // keeps a player who arrived inside the destination portal from being
        // sent straight back — the cooldown cannot run out while they are
        // still standing in it, so it only expires once they walk clear.
        void SetAsInsidePortal(BlockID portal, const glm::ivec3& pos, int cooldownTicks);

        // MC Entity.handlePortal (:2602) fused with
        // PortalProcessor.processPortalTeleportation (:21).
        //
        // Call once per tick from the holder's tick, AFTER movement and AFTER
        // every SetAsInsidePortal for this tick. Decrements the cooldown,
        // advances or decays the portal timer, drops an expired processor, and
        // returns the portal that fired — at which point it has ALREADY armed
        // `cooldownOnFire`, exactly as MC does before resolving the
        // destination.
        //
        // `transitionTime` is Portals::GetTransitionTime for the active
        // portal; the caller computes it because only the caller knows whether
        // it is a player and whether that player is invulnerable.
        //
        // `allowedToTeleport` is MC's `canUsePortal(false)`:
        // `(!isPassenger()) && isAlive()`. MC's LivingEntity adds
        // `&& !isSleeping()`; this engine has no sleeping, so the base rule is
        // the whole rule.
        std::optional<Trigger> HandleTick(int transitionTime, bool allowedToTeleport,
                                          int cooldownOnFire);

        // MC Entity.setPortalCooldown / isOnPortalCooldown (:607-620).
        void SetCooldown(int ticks) { m_cooldown = ticks; }
        int  GetCooldown() const    { return m_cooldown; }
        bool IsOnCooldown() const   { return m_cooldown > 0; }

        // MC PortalProcessor.getPortalTime — how many ticks the holder has
        // been standing in the portal. Drives the client's warp overlay, and
        // is the only reason this is exposed at all.
        int  GetPortalTime() const { return m_process ? m_process->portalTime : 0; }
        bool IsInPortal() const    { return m_process.has_value(); }

        // The portal currently being stood in, for the local transition
        // (nausea) lookup. Air when there is none.
        BlockID CurrentPortal() const {
            return m_process ? m_process->portal : BlockID::Air;
        }

        // Carried across a dimension change: MC copies both fields onto the
        // recreated entity (Entity.java:3008-3009). Without this the arriving
        // entity has a fresh processor, immediately re-enters the portal it
        // landed in, and bounces.
        void CopyFrom(const PortalState& other) { *this = other; }

        // Everything about this holder's portal involvement, forgotten. Used
        // on respawn, where MC constructs a whole new entity.
        void Reset() { m_process.reset(); m_cooldown = 0; }

    private:
        // MC PortalProcessor's four fields.
        struct Process {
            BlockID    portal;
            glm::ivec3 entryPosition;
            int        portalTime           = 0;
            bool       insidePortalThisTick = true;
        };

        std::optional<Process> m_process;
        int                    m_cooldown = 0;
    };

} // namespace Game
