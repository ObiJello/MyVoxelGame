// File: src/common/world/portal/PortalFamily.hpp
//
// The three frame-and-portal-block families the engine knows how to light,
// link and travel through:
//
//   Nether  — vanilla's: an obsidian frame, lit by fire, purple nether_portal
//             blocks, Overworld <-> Nether at 8:1.
//   Hush    — ours: the ancient city's reinforced-deepslate frame, lit by an
//             Echo Shard, teal hush_portal blocks, Overworld <-> The Hush at
//             1:1.
//   Aether  — The Aether mod's (AetherPortalBlock / AetherPortalShape /
//             AetherPortalForcer): a glowstone frame, lit by a water bucket,
//             aether_portal blocks, Overworld <-> The Aether at 1:1.
//
// Everything that used to say "obsidian" or "nether_portal" by name — the
// vanilla PortalShape walk, the immersive FrameShape flood fill, the
// far-side generator, PortalForcer, the per-level portal index, the block
// hooks that collapse a portal when its frame is mined — now asks the family
// instead, so the second family costs no second copy of any of it. MC has no
// such abstraction (NetherPortalBlock and PortalShape hard-code Blocks
// .OBSIDIAN), which is exactly why the Hush needed one.
//
// A family is a static descriptor: the table is built once and every lookup
// returns a pointer or reference into it, so `const PortalFamily*` is safe to
// keep. The id doubles as the index into per-family arrays
// (ServerLevel's portal indexes).
#pragma once

#include "common/core/Features.hpp"
#include "common/world/block/Blocks.hpp"
#include "common/world/level/DimensionId.hpp"

#include <cstdint>

namespace Game::Immersive {
    // ImmersivePortal.hpp's wire-stable kind. Forward-declared (fixed
    // underlying type) so this header stays light enough for PortalState.hpp,
    // which every entity header includes.
    enum class PortalKind : uint8_t;
}

namespace Game {

    enum class PortalFamilyId : uint8_t {
        Nether = 0,
        Hush   = 1,
        Aether = 2,
    };
    inline constexpr int kPortalFamilyCount = 3;
    inline constexpr PortalFamilyId kAllPortalFamilies[] = {
        PortalFamilyId::Nether, PortalFamilyId::Hush, PortalFamilyId::Aether,
    };

    struct PortalFamily {
        PortalFamilyId id;
        // The immersive cluster's tag and the log name.
        const char* tag;
        // The vanilla portal block the frame fills with (HORIZONTAL_AXIS
        // property, nether_portal's — hush_portal aliases its state set).
        BlockID portalBlock;
        // What a generated far-side frame is built out of.
        BlockID buildBlock;
        // MC PortalShape.FRAME — the vanilla walk's strict predicate.
        // Obsidian only for the nether: crying obsidian does NOT work in
        // vanilla and must not work here.
        bool (*isFrame)(BlockID);
        // The Immersive Portals mod's looser one, for the flood fill and the
        // integrity sweep: the mod lets crying obsidian close a loop.
        bool (*isImmersiveFrame)(BlockID);
        // The dimension on the other side of the Overworld.
        DimensionId farDimension;
        // PortalForcer.findClosestPortalPosition's radius when ARRIVING in
        // the far dimension (the Overworld side is always 128). Vanilla's 16
        // for the nether and the Hush; AetherPortalForcer searches 128 both
        // ways.
        int farSearchRadius;
        // PortalForcer.createPortal's spiral radius (BlockPos.spiralAround):
        // vanilla's 16, AetherPortalForcer's 64.
        int forcerSpiralRadius;
#if ENABLE_IMMERSIVE_PORTALS
        // The PortalKind an immersive cluster of this family carries.
        Immersive::PortalKind immersiveKind;
#endif

        // Where a frame in `from` leads: the far dimension from the
        // Overworld, the Overworld from anywhere else.
        DimensionId Other(DimensionId from) const {
            return from == farDimension ? DimensionId::Overworld : farDimension;
        }

        // MC BaseFireBlock.inPortalDimension and the Hush's counterpart —
        // may a frame of this family be lit in `dim` at all?
        bool CanIgniteIn(DimensionId dim) const {
            switch (id) {
                case PortalFamilyId::Nether: return DimensionAllowsNetherPortal(dim);
                case PortalFamilyId::Hush:   return DimensionAllowsHushPortal(dim);
                case PortalFamilyId::Aether: return DimensionAllowsAetherPortal(dim);
            }
            return false;
        }

        // MC PortalForcer's two search radii (PortalForcer.java:25-26), keyed
        // on the DESTINATION: 128 when arriving in the Overworld, 16
        // otherwise. The asymmetry is vanilla's — an Overworld portal links
        // to a far portal up to 128 blocks away, the return trip looks 16 —
        // and the Hush keeps it (its scale is 1:1, but the rule is about
        // which side is allowed to be sloppy, not about the scale).
        int SearchRadiusToward(DimensionId to) const {
            return to == DimensionId::Overworld ? kOverworldSearchRadius : farSearchRadius;
        }
        static constexpr int kFarSearchRadius       = 16;
        static constexpr int kOverworldSearchRadius = 128;
    };

    // ── Lookups ──────────────────────────────────────────────────────────
    const PortalFamily& Family(PortalFamilyId id);
    inline const PortalFamily& NetherFamily() { return Family(PortalFamilyId::Nether); }
    inline const PortalFamily& HushFamily()   { return Family(PortalFamilyId::Hush); }
    inline const PortalFamily& AetherFamily() { return Family(PortalFamilyId::Aether); }

    // The family whose PORTAL block this is (nether_portal / hush_portal /
    // aether_portal), or
    // null. End portals and gateways are not a family: they have no frame
    // that links to a far frame.
    const PortalFamily* FamilyOfPortalBlock(BlockID id);
    // The family whose frame this block may be part of (obsidian, crying
    // obsidian / reinforced deepslate), or null. The immersive predicate —
    // this is what the block-removed hooks ask, and a mined crying-obsidian
    // loop must break its cluster too.
    const PortalFamily* FamilyOfFrameBlock(BlockID id);
    inline bool IsFamilyPortalBlock(BlockID id) { return FamilyOfPortalBlock(id) != nullptr; }

#if ENABLE_IMMERSIVE_PORTALS
    // The family an immersive record of this kind belongs to, or null for
    // command portals, mirrors and gun surfaces.
    const PortalFamily* FamilyOfKind(Immersive::PortalKind kind);
#endif

} // namespace Game
