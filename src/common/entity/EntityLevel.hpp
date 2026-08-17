// File: src/common/entity/EntityLevel.hpp
//
// The seam between the ported entity system and this engine's world.
//
// MC's entities talk to `Level`, which is enormous and exists in two flavours
// (ServerLevel / ClientLevel). Rather than drag that in, the port talks to this
// narrow interface — everything Entity, LivingEntity, Mob, the goals, the
// navigation and the spawner actually need, and nothing else. Server and client
// each provide one implementation.
//
// Keeping it explicit has a second benefit: every place the port had to adapt
// to an engine that is not Minecraft is visible here as a named method with a
// comment, instead of being buried in a mob class.
#pragma once

#include "common/entity/Attributes.hpp"
#include "common/physics/Physics.hpp"
#include "common/world/block/Blocks.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace Game {

    struct IBlockAccess;
    class Entity;
    class LivingEntity;
    class JavaRandom;

    // MC net.minecraft.world.Difficulty. Nothing in this engine set a
    // difficulty before mobs existed, so the level implementations default to
    // Normal; PEACEFUL is still honoured everywhere MC honours it, so wiring a
    // setting to it later is a one-line change.
    enum class Difficulty : uint8_t {
        Peaceful = 0,
        Easy,
        Normal,
        Hard,
    };

    // MC DifficultyInstance, reduced to the one number mobs read. MC scales it
    // by chunk inhabited time and moon phase; without those this is a plain
    // function of the difficulty setting, which is what `difficulty * X` call
    // sites in Zombie/Skeleton expect.
    inline float GetSpecialMultiplier(Difficulty d) {
        switch (d) {
            case Difficulty::Peaceful: return 0.0f;
            case Difficulty::Easy:     return 0.0f;
            case Difficulty::Normal:   return 0.5f;
            case Difficulty::Hard:     return 1.0f;
        }
        return 0.5f;
    }

    struct EntityLevel {
        virtual ~EntityLevel() = default;

        // ── World data ─────────────────────────────────────────────────────
        virtual const IBlockAccess* Blocks() const = 0;

        // Convenience: a PhysicsContext wrapping Blocks(), for the mover.
        PhysicsContext Physics() const {
            PhysicsContext ctx;
            ctx.blockAccess = Blocks();
            return ctx;
        }

        // MC Level.isClientSide. Gates every server-only branch in the port —
        // AI, damage, drops, despawn — so a client-side mob runs travel() and
        // animation only, exactly as in MC.
        virtual bool IsClientSide() const = 0;

        virtual int64_t GetGameTime() const = 0;
        virtual int64_t GetDayTime()  const = 0;
        virtual Difficulty GetDifficulty() const { return Difficulty::Normal; }

        virtual JavaRandom& Random() = 0;

        // ── Light ──────────────────────────────────────────────────────────
        //
        // This engine has NO light engine (see IBlockAccess::GetRawBrightness —
        // it answers 15 for a column open to the sky and 0 for a roofed one).
        // That stand-in is correct for crops, which read raw sky light, but it
        // is not usable for mob spawning: monsters would never spawn on the
        // surface at night, because the raw value never dims.
        //
        // So the port routes every spawn/AI light test through this method,
        // which is MC's getMaxLocalRawBrightness — raw sky light MINUS the
        // time-of-day darkening. Open sky at midnight reads ~4, a cave reads 0,
        // and open sky at noon reads 15, which is the behaviour the rules were
        // written against. Torches still contribute nothing; that arrives with
        // a real light engine and only this method changes.
        virtual int GetMaxLocalRawBrightness(int x, int y, int z) const = 0;

        // MC LevelReader.getBrightness(LightLayer.SKY, pos) — the RAW stored sky
        // light, with no time-of-day darkening applied. Distinct from
        // GetMaxLocalRawBrightness on purpose: Monster.isDarkEnoughToSpawn
        // tests both, and they answer differently. Outdoors this is 15 at
        // midnight as well as at noon.
        virtual int GetSkyBrightness(int x, int y, int z) const = 0;

        // MC Level.getMaxLocalRawBrightness(pos, amount) — the explicit-amount
        // overload, used when thundering (amount = 10) instead of skyDarken.
        virtual int GetMaxLocalRawBrightness(int x, int y, int z, int amount) const = 0;

        // MC Level.getSkyDarken. 0 in full day, 11 at night, interpolated
        // across dawn and dusk — see the implementation for the keyframes.
        virtual int GetSkyDarken() const = 0;

        // MC Level.canSeeSky — needed by the zombie daylight burn and by
        // skeleton sun avoidance.
        virtual bool CanSeeSky(int x, int y, int z) const = 0;

        // MC's EnvironmentAttributes.MONSTERS_BURN timeline track. NOT the same
        // as IsDay(): the burn window is [23460, 12542), narrower than the day
        // at both ends, so undead survive a little past dawn and catch fire a
        // little before dusk.
        virtual bool MonstersBurn() const = 0;

        // MC Level.isDay / isThundering.
        virtual bool IsDay() const = 0;
        virtual bool IsThundering() const { return false; }

        // ── Entity queries ─────────────────────────────────────────────────
        //
        // MC's getEntitiesOfClass is generic; here the caller filters by type
        // after the fact, because the port has a closed set of mob classes and
        // a template-per-class query would buy nothing.
        //
        // `except` is skipped. Results are NOT sorted.
        virtual void GetEntitiesInBox(const AABB& box, const Entity* except,
                                      std::vector<Entity*>& out) const = 0;

        // MC Level.getNearestPlayer. Returns null when none is in range;
        // `maxDistance` < 0 means unlimited. Players are exposed as
        // LivingEntity so goals can target and damage them uniformly — see
        // the adapter note in ServerLevelBridge.
        virtual LivingEntity* GetNearestPlayer(double x, double y, double z,
                                               double maxDistance) const = 0;

        virtual void GetPlayers(std::vector<LivingEntity*>& out) const = 0;

        // The item id in a player's main hand, or 0 for empty.
        //
        // The item system lives outside the entity port (Game::ItemStack,
        // Game::Inventory) and TemptGoal is the only thing that needs to reach
        // it, so it comes through the bridge rather than being a dependency of
        // every mob. Returns 0 for non-players.
        virtual uint32_t GetHeldItemId(const LivingEntity& player) const { return 0; }

        // ── Effects the entity system causes ───────────────────────────────

        // Broadcast a one-byte entity event to everyone tracking `entity`
        // (MC Level.broadcastEntityEvent): 3 = death, 60 = poof particles,
        // 10 = sheep eat, 18 = breeding hearts.
        virtual void BroadcastEntityEvent(const Entity& entity, uint8_t event) = 0;

        // Drop an item stack in the world. Server-only; the client
        // implementation is a no-op.
        virtual void SpawnItemDrop(const glm::dvec3& pos, uint32_t itemId, int count) {}

        // Add a freshly created entity to the level, taking ownership.
        // Server-only (the spawner, zombie reinforcements, breeding, arrows).
        virtual void AddFreshEntity(std::unique_ptr<Entity> entity) {}

        // ── Block edits made BY mobs ────────────────────────────────────────
        //
        // MC Level.destroyBlock(pos, dropResources) and Level.setBlock. A sheep
        // grazing is the first user; endermen and creepers want the same pair.
        //
        // Both are gated by the caller on MobGriefing() — MC's `mobGriefing`
        // gamerule — rather than being gated in here, because MC checks it at
        // the call site and the decision belongs to whatever is doing the
        // griefing, not to the level.
        //
        // Server-only; the client implementations are no-ops, which is right
        // because client mobs never run AI.
        virtual void DestroyBlock(const glm::ivec3& pos, bool dropResources) {}
        virtual void SetBlock(const glm::ivec3& pos, BlockID block) {}

        // MC GameRules.RULE_MOBGRIEFING. Default true, as in vanilla.
        virtual bool MobGriefing() const { return true; }
    };

} // namespace Game
