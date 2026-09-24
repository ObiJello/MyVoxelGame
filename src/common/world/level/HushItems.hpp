// File: src/common/world/level/HushItems.hpp
//
// The Hush's "tools of the deep" (docs/the-hush.md), reachable from common.
//
// The item behaviours live in common (ItemBehaviors.cpp) but most of what
// these items DO is the server's: a structure lookup, a packet to one player,
// a cross-dimension teleport, an arrow spawned with the player's entity view
// as its owner, a per-player cooldown. Same bridge shape as WorldMobSpawn.hpp
// (SpawnMobFromItem, ThrowEnderPearl): common declares, the server
// (server/items/HushItems.cpp) defines, and every function answers
// "nothing happened" when there is no server behind it.
//
//   tuning fork   — used on a resonant_crystal / resonant_cluster: a ping.
//                   For kPingTicks the player sees, through walls, every ore
//                   (echo/resonite/vanilla) and every mob within kPingRadius,
//                   and the nearest Echo Vault / Warden's Tomb entrance
//                   within kPingStructureRadius; a ring of motes spreads from
//                   the crystal. Cooldown kPingCooldownTicks.
//   recall chime  — hold kRecallUseTicks (like eating), then back to the
//                   landing of the last hush gate crossed (any dimension).
//                   Let go early and nothing happens (the action bar says
//                   so). Cooldown kRecallCooldownTicks.
//   resonance bow — a bow (MC BowItem's draw and release, the vanilla bow
//                   shares it) whose arrow pierces one mob and bursts on
//                   impact (ResonanceArrow).
//   cloak of silence — worn in the chest slot: IsSoundCloaked below.
//
// Cooldowns are server-side only: this engine has no ItemCooldowns (no
// cooldown overlay on the hotbar); a use inside one is refused with an
// action-bar line instead.
#pragma once

#include "common/entity/EntityLevel.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/entity/LivingEntity.hpp"
#include "common/world/block/BlockInteraction.hpp"
#include "common/world/level/DimensionId.hpp"

#include <glm/glm.hpp>

#include <cstdint>

namespace Game {

    class IUsePlayer;
    struct ItemStack;

    namespace HushItems {

        inline constexpr int kPingRadius          = 24;
        inline constexpr int kPingStructureRadius = 64;
        inline constexpr int kPingTicks           = 120;   // ~6 s of outlines
        inline constexpr int kPingCooldownTicks   = 100;   // 5 s
        // The hold is the length of the chime's own note: block.bell.resonate
        // (3.78 s, a swell that peaks at 1.93 s and rings out below -55 dB by
        // 3.0 s) played at pitch 1.6 on top of its variants' 1.0 / 0.9 /
        // 0.85, i.e. rung out after 1.88 / 2.08 / 2.21 s. 45 ticks (2.25 s)
        // recalls just as the slowest variant falls silent.
        inline constexpr int kRecallUseTicks      = 45;
        inline constexpr int kRecallCooldownTicks = 1200;  // 60 s
        inline constexpr float kBurstRadius       = 2.0f;
        inline constexpr float kBurstDamage       = 2.0f;

        // ── Server bridges (server/items/HushItems.cpp) ─────────────────
        UseResult TuningForkStrike(IUsePlayer& player, const glm::ivec3& crystal);
        UseResult RecallChimeBegin(IUsePlayer& player, uint32_t hand);
        void      RecallChimeFinish(IUsePlayer& player, ItemStack& stack);       // ItemFinishUsingFn
        void      RecallChimeRelease(IUsePlayer& player, ItemStack& stack, int remaining); // ItemReleaseUsingFn
        UseResult BowBegin(IUsePlayer& player, uint32_t hand);
        void      BowRelease(IUsePlayer& player, ItemStack& bow, int remaining); // ItemReleaseUsingFn
        // The resonance arrow's burst, as seen: a ring of motes for every
        // player near `at` (HushSignalS2C SonicBurst). The damage is the
        // arrow's own (ResonanceArrow::Burst).
        void      BroadcastSonicBurst(DimensionId dimension, const glm::dvec3& at, float radius);

        // ── Common ──────────────────────────────────────────────────────
        // The cloak of silence: a player wearing it cannot be found by SOUND
        // — the warden's sniff and the echo wraith's hunt skip them. What sees
        // or is struck still answers: a warden you hit, or one already angry
        // at you, keeps its anger (the cloak hides footsteps, not blows).
        // Server-side (the level bridge reads the inventory); false on the
        // client.
        inline bool IsSoundCloaked(const EntityLevel& level, const LivingEntity& entity) {
            return entity.IsPlayer() && level.GetChestItemId(entity) == Items::CloakOfSilence;
        }

    } // namespace HushItems

} // namespace Game
