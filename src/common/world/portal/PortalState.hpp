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

#include "common/world/level/GameRules.hpp"
#include "common/world/portal/PortalFamily.hpp"

#include <algorithm>
#include <atomic>

#include "common/world/block/Blocks.hpp"

#include <cstdint>
#include <optional>
#include <glm/glm.hpp>

namespace Game {

    // MC Portal.Transition — what the CLIENT does while standing in the
    // portal, independent of the travel itself. Confusion is the nether
    // portal's nausea/warp; the End portal has none.
    enum class PortalLocalTransition : uint8_t { None, Confusion };

    class ILevelWrite;

    namespace Portals {

        // ── Immersive nether portals (Features.hpp ENABLE_IMMERSIVE_PORTALS) ──
        // When ON, a lit obsidian frame becomes a see-through surface (an
        // immersive portal) instead of purple portal blocks, and vanilla
        // nether portal blocks left in the world are inert. Nether only —
        // see FamilyIsImmersive below for the other families. The server sets this
        // from its config (/gamerule immersive_portals); it is read by the
        // fire block's onPlace, the portal block's entityInside and the
        // server's portal tick. Defaults on.
        // Atomic: the server thread writes it (the rule), a remote client's
        // network thread writes it (WorldRulesS2C), the main thread reads it.
        inline std::atomic<bool> g_immersiveNetherPortals{true};
        inline bool ImmersiveNetherPortals()      { return g_immersiveNetherPortals.load(std::memory_order_relaxed); }
        inline void SetImmersiveNetherPortals(bool on) { g_immersiveNetherPortals.store(on, std::memory_order_relaxed); }

        // The rule is the NETHER family's switch and nothing else's. The
        // Hush and Aether frames are always vanilla portals — lit with
        // hush_portal / aether_portal blocks, crossed by standing in them —
        // whatever the rule says, so flipping it never changes how they
        // light, how they are crossed, or whether a lit one survives; the
        // Twilight pool is not a frame family and never reads it. Without
        // the immersive feature compiled in, every family is vanilla.
        inline bool FamilyFollowsImmersiveRule(PortalFamilyId family) {
            return family == PortalFamilyId::Nether;
        }
        inline bool FamilyIsImmersive(PortalFamilyId family) {
#if ENABLE_IMMERSIVE_PORTALS
            return FamilyFollowsImmersiveRule(family) && ImmersiveNetherPortals();
#else
            (void)family;
            return false;
#endif
        }
        // A frame-family portal block (nether_portal, hush_portal,
        // aether_portal) whose family is immersive right now: scenery left
        // in a frame, not a doorway — the see-through surface does the
        // crossing, and the server clears or adopts the block.
        inline bool IsInertPortalBlock(BlockID id) {
            const PortalFamily* family = FamilyOfPortalBlock(id);
            return family && FamilyIsImmersive(family->id);
        }

        // /gamerule portal_gun (Features.hpp ENABLE_PORTAL_GUN): may the
        // portal gun fire? Off = the gun does nothing and every placed pair
        // is closed (IntegratedServer::SetPortalGunAllowed). Per world, in
        // the world sidecar. Defaults on.
        inline std::atomic<bool> g_portalGunAllowed{true};
        inline bool PortalGunAllowed()          { return g_portalGunAllowed.load(std::memory_order_relaxed); }
        inline void SetPortalGunAllowed(bool on) { g_portalGunAllowed.store(on, std::memory_order_relaxed); }

        // World options set at creation (default off), server-installed:
        //   • World wrap: the world is `g_worldWrapSize` blocks across and
        //     its borders are global portals onto the opposite border, so
        //     it is a loop. 0 = off. The Nether wraps at an eighth.
        //   • Dimension stack: the bottom of each dimension is a global
        //     portal onto the top of the next — Overworld over Nether over
        //     End over Overworld — and the bedrock at those seams is
        //     generated as ordinary stone so the way through is open.
        inline int  g_worldWrapSize = 0;
        inline bool g_dimensionStack = false;
        inline int  WorldWrapSize()            { return g_worldWrapSize; }
        inline void SetWorldWrapSize(int size) { g_worldWrapSize = size; }
        inline bool DimensionStackEnabled()    { return g_dimensionStack; }
        inline void SetDimensionStack(bool on) { g_dimensionStack = on; }

        // Server-installed: called by the fire block when it is placed in a
        // dimension that allows nether portals and immersive mode is on, by
        // the Echo Shard against a reinforced-deepslate frame and by the
        // water bucket against a glowstone one (FamilyIsImmersive). `seedPos`
        // is an interior cell (the fire's, or one beside the clicked frame
        // block); `family` says which frame material closes the loop.
        // Returns true if a closed loop of that family around `seedPos` was
        // found and the frame taken over (the interior is cleared by the
        // handler).
        using ImmersiveFrameLitHandler = bool (*)(ILevelWrite& level, const glm::ivec3& seedPos,
                                                  PortalFamilyId family);
        inline ImmersiveFrameLitHandler g_immersiveFrameLitHandler = nullptr;
        inline void SetImmersiveFrameLitHandler(ImmersiveFrameLitHandler h) { g_immersiveFrameLitHandler = h; }
        inline ImmersiveFrameLitHandler GetImmersiveFrameLitHandler()        { return g_immersiveFrameLitHandler; }


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

        // Twilight Forest TFGameRules players_twilight_portal_default_delay /
        // _creative_delay: how long a player stands in a twilight_portal
        // pool before travelling. (TF 4.9 registers 60 as the rule default;
        // the engine uses 80, the nether portal's figure, on purpose — the
        // port spec keeps both doorways on the same clock.)
        inline constexpr int kTwilightPortalPlayerDelay         = 80;
        inline constexpr int kTwilightPortalPlayerCreativeDelay = 0;

        // Is this block one you can travel through? Every frame family's
        // portal block (nether, hush, aether), the Twilight Forest pool, and
        // the End's two.
        inline bool IsPortal(BlockID id) {
            return IsFamilyPortalBlock(id) || id == BlockID::TwilightPortal ||
                   id == BlockID::EndPortal || id == BlockID::EndGateway;
        }

        // MC NetherPortalBlock.getPortalTransitionTime (:99) and
        // EndPortalBlock, which does not override it and so answers 0. The
        // hush portal takes the nether portal's timing: it is the same kind
        // of doorway (a frame you stand in), and the two delay rules are
        // the player's tunables for that.
        //
        // `invulnerable` is MC's `player.getAbilities().invulnerable`, true in
        // creative AND spectator — gating on "is creative" alone would leave a
        // spectator waiting 4 seconds in a block they are flying through.
        inline int GetTransitionTime(BlockID portal, bool isPlayer, bool invulnerable) {
            // TFPortalBlock.getPortalTransitionTime: 0 for anything but a
            // player, else the TF delay rule for the player's abilities.
            if (portal == BlockID::TwilightPortal) {
                if (!isPlayer) return 0;
                return invulnerable ? kTwilightPortalPlayerCreativeDelay
                                    : kTwilightPortalPlayerDelay;
            }
            if (!IsFamilyPortalBlock(portal)) return 0;
            // Non-players travel on the first tick inside. That is vanilla:
            // a minecart or a dropped item crosses instantly.
            if (!isPlayer) return 0;
            // MC NetherPortalBlock.getPortalTransitionTime (26.3): the two
            // players_nether_portal_*_delay rules (defaults 0 / 80 — the
            // constants above), clamped at zero.
            const int rule = Rules::GetInt(invulnerable ? Rules::Id::PlayersNetherPortalCreativeDelay
                                                        : Rules::Id::PlayersNetherPortalDefaultDelay);
            // AetherPortalBlock.getLevelPortalTransitionTime reads the same
            // two rules but floors them at ONE tick (Math.max(1, …)), not
            // zero.
            if (portal == BlockID::AetherPortal) return std::max(1, rule);
            return std::max(0, rule);
        }

        // MC NetherPortalBlock.getLocalTransition (:178) → CONFUSION;
        // EndPortalBlock does not implement it → NONE. The hush portal is
        // NONE on purpose: the nausea is the nether's, and the Hush is a
        // silent place. AetherPortalBlock and TFPortalBlock do not implement
        // getLocalTransition either, so both are NONE too.
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
        //
        // `processCooldown` false skips the cooldown countdown for this tick:
        // MC ServerPlayer.processPortalCooldown does nothing while the player
        // isChangingDimension (the cross-dimension teleport not yet acked).
        std::optional<Trigger> HandleTick(int transitionTime, bool allowedToTeleport,
                                          int cooldownOnFire, bool processCooldown = true);

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
        void Reset() { m_process.reset(); m_cooldown = 0; m_crossedSurface = false; }

        // An immersive surface moved this holder this tick (EntityPortal-
        // Travel::MoveMob). The entity tracker takes it on its next pass and
        // tells the watchers to SNAP rather than interpolate: a lerp from
        // one side of a portal to the other is a body sliding across the
        // room, and a three-tick turn is a body spinning in the doorway.
        void MarkCrossedSurface()          { m_crossedSurface = true; }
        // const, and the flag mutable: the tracker walks the mobs as const
        // (it only reads them), and taking this one-shot flag is the one
        // thing it writes — a mailbox, not entity state.
        bool ConsumeCrossedSurface() const { const bool c = m_crossedSurface; m_crossedSurface = false; return c; }

    private:
        mutable bool m_crossedSurface = false;
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
