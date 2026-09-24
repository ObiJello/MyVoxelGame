// File: src/common/entity/mobs/HushCreatures.hpp
//
// The deep Hush's creatures and its second boss (docs/the-hush.md,
// "Creatures of the deep Hush"). Original designs — no MC or mod class
// behind any of them — so each is written against the engine's own Mob API
// in the shape the closest vanilla mob has:
//
//   LumenMoth     — ambient flutterer (the Bat's flight loop, retargeted):
//                   drifts to the nearest light — an emissive block, a torch
//                   or lantern, a player holding one — and circles it.
//   CrystalGolem  — neutral guardian (the IronGolem's NeutralMob anger):
//                   hostile once hit, or once a player breaks a resonant
//                   crystal/cluster within 16 blocks (OnResonantCrystalBroken,
//                   called from PlayerSession's break path).
//   HushLeviathan — ambient sky drifter (the Aerwhale's course-and-steer,
//                   with a turn-rate cap for long curving paths), high above
//                   the surface.
//   EchoMimic     — sneaky hostile: replays the nearest player two seconds
//                   late — their footsteps, look, crouch and swings — and
//                   strikes when it catches up; a shimmer until revealed, and
//                   light hurts it.
//   ChoirMother   — the boss summoned at a Choir Hall's altar
//                   (world/block/ChoirPuzzle): three health-gated phases —
//                   conducting echo wraiths, sonic ring shockwaves, and a
//                   silence field.
//
// All five are built through MakeHushCreature, which MakeGenericMob's
// engine-only promotion calls, so both factories construct the same class.
#pragma once

#include "common/entity/Mob.hpp"
#include "common/entity/Monster.hpp"
#include "common/entity/NeutralMob.hpp"
#include "common/entity/ai/Goal.hpp"
#include "common/world/block/Blocks.hpp"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace Game {

    struct IBlockAccess;
    struct EntityLevel;

    // ── Light sources (moth attraction, mimic weakness) ────────────────────
    //
    // There is no block light engine: "a light" is the render-side emissive
    // flag (every glowing Hush block, the TF/Aether lights) plus the vanilla
    // blocks that emit light in MC and are placeable light sources.
    bool IsHushLightBlock(BlockID id);
    // A held item that counts as a carried light (the block items above).
    bool IsHushLightItem(uint32_t itemId);
    // Nearest light block within `radius` (cubic scan, nearest by distance
    // squared), written to `out`. False when there is none.
    bool FindNearestLightBlock(const IBlockAccess& blocks, const glm::dvec3& from,
                               int radius, int yRadius, glm::ivec3& out);

    // ── Lumen Moth ─────────────────────────────────────────────────────────

    class LumenMoth : public Mob {
    public:
        explicit LumenMoth(EntityLevel* level);

        static void CreateAttributes(AttributeMap& out);

        // MC AmbientCreature: never a leash holder, never pushes, despawns
        // like the bat (the ambient category's own rules).
        bool IsPushable() const override { return false; }
        bool CauseFallDamage(double, float) override { return false; }
        void Travel(const glm::dvec3& input) override;
        void Tick() override;
        int  GetMaxSpawnClusterSize() const override { return 6; }

        // Bit 0: circling a light (the render module brightens the glow).
        uint8_t GetAnimStateByte() const override { return m_hasLight ? 1 : 0; }
        void    SetAnimStateByte(uint8_t v) override { m_hasLight = (v & 1) != 0; }
        bool    IsCirclingLight() const { return m_hasLight; }

        void ClearReferenceTo(const Entity* entity) override;

    protected:
        void CustomServerAiStep() override;

    private:
        void RescanLight();

        LivingEntity* m_lightCarrier = nullptr;   // a player holding a light
        glm::dvec3 m_light{0.0};         // centre of the light being circled
        bool       m_hasLight = false;
        float      m_orbitAngle = 0.0f;  // radians
        float      m_orbitRadius = 1.5f;
        int        m_orbitDir = 1;
        int        m_rescanIn = 0;
        glm::ivec3 m_wander{0};
        bool       m_hasWander = false;
    };

    // ── Crystal Golem ──────────────────────────────────────────────────────

    class CrystalGolem : public PathfinderMob, public NeutralMob {
    public:
        explicit CrystalGolem(EntityLevel* level);

        static void CreateAttributes(AttributeMap& out);

        // A guardian: never despawns (MC AbstractGolem.removeWhenFarAway).
        bool RemoveWhenFarAway(double) const override { return false; }
        int  GetMaxSpawnClusterSize() const override { return 1; }
        int  DecreaseAirSupply(int currentSupply) override { return currentSupply; }

        std::shared_ptr<SpawnGroupData>
        FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) override;

        // IronGolem.doHurtTarget's swing clock and 0.4 lift, on Mob's
        // ATTACK_KNOCKBACK shove.
        bool DoHurtTarget(Entity& target) override;
        void AiStep() override;
        void HandleEntityEvent(uint8_t id) override;
        void StartPersistentAngerTimer() override;
        void DropCustomDeathLoot(EntityLevel& level) override;

        // The resonant-crystal alarm: turn on `breaker` now.
        void ProvokeBy(LivingEntity& breaker);

        // Bit 0: angry (a target or a live grudge) — the brighter sheet.
        uint8_t GetAnimStateByte() const override { return m_angryVisible ? 1 : 0; }
        void    SetAnimStateByte(uint8_t v) override { m_angryVisible = (v & 1) != 0; }
        bool    IsAngryVisible() const { return m_angryVisible; }
        int     GetAttackAnimationTick() const { return m_attackAnimationTick; }

        void SaveModNbt(ModNbtOut& out) const override;
        void LoadModNbt(const ModNbtIn& in) override;

        void ClearReferenceTo(const Entity* entity) override {
            PathfinderMob::ClearReferenceTo(entity);
            ClearAngerReferenceTo(entity);
        }

    protected:
        void RegisterGoals() override;

    private:
        int  m_attackAnimationTick = 0;
        bool m_angryVisible = false;
    };

    // Every crystal golem within 16 blocks of `pos` turns on `breaker`.
    // Server-side; called when a player breaks resonant_crystal or
    // resonant_cluster.
    void OnResonantCrystalBroken(EntityLevel& level, const glm::ivec3& pos,
                                 LivingEntity& breaker);

    // ── Hush Leviathan ─────────────────────────────────────────────────────

    class HushLeviathan : public Mob {
    public:
        explicit HushLeviathan(EntityLevel* level);

        static void CreateAttributes(AttributeMap& out);

        bool CauseFallDamage(double, float) override { return false; }
        bool IsPushable() const override { return false; }
        bool RemoveWhenFarAway(double distSq) const override { return distSq > 256.0 * 256.0; }
        void Travel(const glm::dvec3& input) override;
        int  GetMaxSpawnClusterSize() const override { return 1; }

        // Spawned at the surface cell the spawner picked, then lifted into
        // the sky here (the natural spawner never proposes a cell 30 blocks
        // up), so the obstruction test asks about the sky, not the ground.
        bool CheckSpawnObstruction(EntityLevel& level) const override;
        std::shared_ptr<SpawnGroupData>
        FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) override;

        // Rarity: no other leviathan within this many blocks.
        static constexpr double kExclusionRadius = 96.0;

    protected:
        void CustomServerAiStep() override;

    private:
        void PickCourse();
        double GroundBelow() const;   // y of the first solid block under it

        glm::dvec3 m_course{0.0};
        bool       m_hasCourse = false;
        int        m_altitudeCheckIn = 0;
        double     m_groundY = 0.0;
    };

    // ── Echo Mimic ─────────────────────────────────────────────────────────

    class EchoMimic : public Monster {
    public:
        // Two seconds of the target's footsteps.
        static constexpr int    kDelayTicks = 40;
        // How far a sample may be bridged across ticks that brought no news.
        // A player's moves arrive as packets at the client's 20 Hz, not on
        // the server's tick, so a tick now and then gets none (and the next
        // gets two): replayed raw, that is a stall and then a double step.
        // A run of unchanged samples this short is interpolated across; a
        // longer one is the player really standing still, and is held.
        static constexpr int    kBridgeTicks = 4;
        // Enough history to bridge around the delayed sample AND the one a
        // tick before it (whose step the replay's glide measures).
        static constexpr int    kTrailSize = kDelayTicks + 2 * kBridgeTicks + 2;
        static constexpr double kRevealDistance = 6.0;
        // A player's crouching box (MC Player's CROUCHING dimensions): the
        // echo of a crouch fits wherever the crouch did.
        static constexpr float  kCrouchHeight    = 1.5f;
        static constexpr float  kCrouchEyeHeight = 1.27f;

        // One tick of the target, as the mimic remembers it.
        struct EchoSample {
            glm::dvec3 pos{0.0};
            float yRot = 0.0f;         // look yaw — a player's head and facing
            float xRot = 0.0f;         // look pitch
            bool  onGround = false;
            bool  crouching = false;   // MC isDiscrete: the shift key
            bool  swing = false;       // an arm swing STARTED this tick
            bool  fresh = false;       // position or look changed since the tick before
        };

        explicit EchoMimic(EntityLevel* level);

        static void CreateAttributes(AttributeMap& out);

        // Light hurts: double damage within 4 blocks of a light, and a slow
        // burn (1 per second) within 2.
        bool Hurt(MobDamageSource source, float amount, Entity* attacker) override;
        void AiStep() override;

        // While a replay step is pending (MirrorThisTick), the step replaces
        // the whole locomotion: the mimic is moved onto the recorded
        // position (collisions still apply) and wears the recorded look.
        // Otherwise — and always on the client — LivingEntity's travel.
        void Travel(const glm::dvec3& input) override;

        // The echo of a crouch is crouched: a player's crouching box.
        float BaseBbHeight()  const override;
        float BaseEyeHeight() const override;

        // A shove — a hit's knockback, a blast — plays out under ordinary
        // physics: the replay lets go for kPushedTicks, and the stalk goal
        // walks the mimic back onto the trail. (A river's steady current is
        // below the threshold and does not count.)
        static constexpr int kPushedTicks = 10;
        void Knockback(double power, double dx, double dz) override;
        void AddDeltaMovement(const glm::dvec3& v) override;
        bool IsBeingPushed() const { return m_pushedTicks > 0; }

        // Bit 0: revealed (a player within kRevealDistance) — the solid sheet.
        // Bit 1: mirroring — the body turns as a player's does (TickHeadTurn),
        //        which the client has to know to turn its copy the same way.
        uint8_t GetAnimStateByte() const override {
            return static_cast<uint8_t>((m_revealed ? 1 : 0) | (m_mirroring ? 2 : 0));
        }
        void    SetAnimStateByte(uint8_t v) override {
            m_revealed  = (v & 1) != 0;
            m_mirroring = (v & 2) != 0;
        }
        bool    IsRevealed() const { return m_revealed; }
        bool    IsMirroring() const { return m_mirroring; }

        // The trail the stalk goal replays: the target's recorded ticks.
        void RecordTrail(const LivingEntity& target);
        void ClearTrail() { m_trailCount = 0; m_trailHead = 0; }
        // The target as it was `delay` ticks ago (0 = this tick), bridged
        // across ticks that brought no new move (kBridgeTicks). The flags
        // (crouch, swing, ground) are that tick's own, never interpolated.
        // False until that much trail has been recorded.
        bool SampleAt(int delay, EchoSample& out) const;

        // Start / stop mirroring (synced: the body rule and the crouch).
        // Stopping stands the mimic back up.
        void SetMirroring(bool on);
        // Queue this tick's replay step for Travel: move toward `s` by at
        // most `maxHorizontal` / `maxVertical` blocks, then take its look.
        void MirrorThisTick(const EchoSample& s, double maxHorizontal, double maxVertical);

    protected:
        void RegisterGoals() override;
        // Mirroring: MC LivingEntity.tickHeadTurn, the rule a PLAYER's body
        // follows (the travel direction eased in at 0.3 a tick, the head kept
        // within 50 degrees of it) — so the echo's body turns exactly as the
        // player's did. Otherwise the mob's BodyRotationControl (Mob's).
        void TickHeadTurn(float yBodyRotTarget) override;

    private:
        // Index of the sample recorded `delay` ticks ago.
        int  TrailIndex(int delay) const {
            return ((m_trailHead - 1 - delay) % kTrailSize + kTrailSize) % kTrailSize;
        }

        std::array<EchoSample, kTrailSize> m_trail{};
        int  m_trailHead = 0;
        int  m_trailCount = 0;
        bool m_revealed = false;
        bool m_mirroring = false;
        bool m_mirrorPending = false;
        EchoSample m_mirror;
        double m_mirrorMaxHorizontal = 0.0;
        double m_mirrorMaxVertical = 0.0;
        int  m_pushedTicks = 0;
        int  m_lightCheckIn = 0;
        bool m_nearLight = false;     // within 4 blocks
        bool m_touchingLight = false; // within 2 blocks
    };

    // The mimic's hunt: record the target every tick, and replay it two
    // seconds late — position, look, crouch and swings — striking (and only
    // then looking at the real target) once within reach of it.
    class EchoMimicStalkGoal : public Goal {
    public:
        explicit EchoMimicStalkGoal(EchoMimic* mob);
        bool CanUse() override;
        bool CanContinueToUse() override;
        void Start() override;
        void Stop() override;
        void Tick() override;
        bool RequiresUpdateEveryTick() const override { return true; }
        const char* Name() const override { return "EchoMimicStalkGoal"; }
    private:
        // Off the trail: walk (path) onto the delayed position.
        void WalkOntoTrail(const EchoMimic::EchoSample& s);

        EchoMimic* m_mob;
        int        m_attackCooldown = 0;
        int32_t    m_trailTargetId = -1;   // whose trail is recorded
        bool       m_onTrail = false;      // replaying (vs. walking onto the trail)
        int        m_repathIn = 0;
    };

    // ── The Choir Mother ───────────────────────────────────────────────────

    class ChoirMother : public Monster {
    public:
        static constexpr float  kMaxHealth = 400.0f;
        static constexpr double kSilenceRadius = 12.0;
        static constexpr double kRingMaxRadius = 20.0;
        static constexpr double kRingSpeed = 0.5;       // blocks per tick
        static constexpr double kRingHalfWidth = 0.75;
        // Entity events (client: HandleEntityEvent).
        static constexpr uint8_t kEventSonicRing = 70;
        static constexpr uint8_t kEventConduct = 71;

        explicit ChoirMother(EntityLevel* level);

        static void CreateAttributes(AttributeMap& out);

        bool RemoveWhenFarAway(double) const override { return false; }
        bool CauseFallDamage(double, float) override { return false; }
        bool IsPushable() const override { return false; }
        int  DecreaseAirSupply(int currentSupply) override { return currentSupply; }
        void Travel(const glm::dvec3& input) override;
        void Tick() override;
        void HandleEntityEvent(uint8_t id) override;
        void DropCustomDeathLoot(EntityLevel& level) override;

        std::shared_ptr<SpawnGroupData>
        FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) override;

        // 1 conducting, 2 sonic rings, 3 silence field — by health.
        int Phase() const;

        // Bits 0-1 the phase, bit 2 "conducting" (arms raised).
        uint8_t GetAnimStateByte() const override {
            return static_cast<uint8_t>((m_syncedPhase & 3) | (m_conductTicks > 0 ? 4 : 0));
        }
        void SetAnimStateByte(uint8_t v) override {
            m_syncedPhase = v & 3;
            m_clientConducting = (v & 4) != 0;
        }
        int  SyncedPhase() const { return m_syncedPhase; }
        bool IsConducting() const { return m_conductTicks > 0 || m_clientConducting; }

        void SaveModNbt(ModNbtOut& out) const override;
        void LoadModNbt(const ModNbtIn& in) override;

    protected:
        void RegisterGoals() override;
        void CustomServerAiStep() override;

    private:
        struct Ring {
            glm::dvec3 centre;
            double     baseY;
            double     radius;
            std::vector<int32_t> hit;   // player entity ids already struck
        };

        void Hover();
        void Conduct();
        void StartRing();
        void TickRings();
        void SilenceField();
        double FloorBelow(const glm::dvec3& p) const;

        glm::dvec3 m_home{0.0};
        bool       m_hasHome = false;
        float      m_hoverAngle = 0.0f;
        int        m_conductIn = 100;
        int        m_ringIn = 80;
        int        m_silenceIn = 0;
        int        m_conductTicks = 0;
        int        m_syncedPhase = 1;
        bool       m_clientConducting = false;
        std::vector<Ring> m_rings;          // server: live shockwaves
        // Client: rings being drawn (centre, floor y, age in ticks).
        struct ClientRing { glm::dvec3 centre; double baseY; int age; };
        std::vector<ClientRing> m_clientRings;
    };

    // The factory hook MakeGenericMob calls for the five types above and
    // Aurelith's Unsung (TheUnsung.hpp); null for any other type.
    std::unique_ptr<Mob> MakeHushCreature(EntityTypeId type, EntityLevel* level);

} // namespace Game
