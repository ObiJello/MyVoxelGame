// File: src/common/world/portal/PortalFamily.cpp
#include "PortalFamily.hpp"

#include "common/core/Features.hpp"
#if ENABLE_IMMERSIVE_PORTALS
#include "common/portal/ImmersivePortal.hpp"
#endif

namespace Game {

    namespace {

        // MC PortalShape.FRAME = BlockStatePredicate.forBlock(Blocks.OBSIDIAN).
        bool NetherIsFrame(BlockID id) {
            return id == BlockID::Obsidian;
        }
        // The mod's loop closes on crying obsidian as well.
        bool NetherIsImmersiveFrame(BlockID id) {
            return id == BlockID::Obsidian || id == BlockID::CryingObsidian;
        }
        // One block, both rules: the ancient city's frame is reinforced
        // deepslate and nothing else.
        bool HushIsFrame(BlockID id) {
            return id == BlockID::ReinforcedDeepslate;
        }
        // AetherPortalShape.FRAME = #aether:aether_portal_blocks, which the
        // mod's generated data fills with glowstone alone. One predicate for
        // both rules, like the Hush's.
        bool AetherIsFrame(BlockID id) {
            return id == BlockID::Glowstone;
        }

        const PortalFamily kFamilies[kPortalFamilyCount] = {
            {
                PortalFamilyId::Nether,
                "nether",
                BlockID::NetherPortal,
                BlockID::Obsidian,
                &NetherIsFrame,
                &NetherIsImmersiveFrame,
                DimensionId::Nether,
                PortalFamily::kFarSearchRadius,
                16,
#if ENABLE_IMMERSIVE_PORTALS
                Immersive::PortalKind::NetherPortal,
#endif
            },
            {
                PortalFamilyId::Hush,
                "hush",
                BlockID::HushPortal,
                BlockID::ReinforcedDeepslate,
                &HushIsFrame,
                &HushIsFrame,
                DimensionId::Hush,
                PortalFamily::kFarSearchRadius,
                16,
#if ENABLE_IMMERSIVE_PORTALS
                Immersive::PortalKind::HushPortal,
#endif
            },
            {
                // AetherPortalBlock / AetherPortalShape / AetherPortalForcer.
                // The forcer searches 128 both ways (findClosestPortalPosition
                // `int i = 128`) and spirals 64 (createPortal).
                PortalFamilyId::Aether,
                "aether",
                BlockID::AetherPortal,
                BlockID::Glowstone,
                &AetherIsFrame,
                &AetherIsFrame,
                DimensionId::Aether,
                PortalFamily::kOverworldSearchRadius,
                64,
#if ENABLE_IMMERSIVE_PORTALS
                Immersive::PortalKind::AetherPortal,
#endif
            },
        };

    } // namespace

    const PortalFamily& Family(PortalFamilyId id) {
        return kFamilies[static_cast<size_t>(id)];
    }

    const PortalFamily* FamilyOfPortalBlock(BlockID id) {
        for (const PortalFamily& f : kFamilies) {
            if (f.portalBlock == id) return &f;
        }
        return nullptr;
    }

    const PortalFamily* FamilyOfFrameBlock(BlockID id) {
        for (const PortalFamily& f : kFamilies) {
            if (f.isImmersiveFrame(id)) return &f;
        }
        return nullptr;
    }

#if ENABLE_IMMERSIVE_PORTALS
    const PortalFamily* FamilyOfKind(Immersive::PortalKind kind) {
        for (const PortalFamily& f : kFamilies) {
            if (f.immersiveKind == kind) return &f;
        }
        return nullptr;
    }
#endif

} // namespace Game
