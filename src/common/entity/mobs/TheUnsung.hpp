// File: src/common/entity/mobs/TheUnsung.hpp
//
// The Unsung — Aurelith's boss (docs/the-hush.md, "The Unsung"; lore in
// docs/hush-lore.md). Ysolde silenced the Heart because something far below
// the Hush — the Undersong — was LEARNING the Chord. Waking the Heart lets it
// answer: it rises through the Heart's dais in the only shape it ever heard,
// a conductor's — sculk and stolen light, a blank face split by a crack, a
// hollow crystal ring where a throat should be, and four shards orbiting it:
// the four voices it learned. It has no song of its own; it can only take
// ours.
//
// Spawned by the quest controller (server/level/AurelithCities) when the
// awakening's Undersong peaks:
//
//   auto m = MakeGenericMob(EntityTypeId::TheUnsung, level);
//   auto* u = static_cast<TheUnsung*>(m.get());
//   u->SetArena(heart, rotation);                // before FinalizeSpawn
//   u->position = {heart.x + 0.5, heart.y - 2.0, heart.z + 0.5};
//   u->FinalizeSpawn(SpawnReason::Triggered, nullptr);
//   level->AddFreshEntity(std::move(m));
//
// Without an arena (a spawn egg, /summon) it adopts the nearest resonance
// engine within 32 blocks as its Heart (so the whole fight can be tried in a
// plaza without waking the city), or else takes its spawn point as a
// stand-in Heart and fights without the statue and Heart mechanics.
//
// ── The fight (500 HP, three phases by health) ─────────────────────────
// Emergence (phase 0, 3 s): it rises out of the dais through the Heart's
// core, untouchable.
//   I   "The False Chord" (100–66 %): hovers after its target; a baton sweep
//       with a raised-arm wind-up; discord notes (slow homing notes — dodge
//       them round a pillar or strike them down; a hit is 4 damage, Slowness
//       II and Weakness I for 3 s); a choir of echo wraiths (2 at a time, at
//       most 4 near it). It STEALS A VOICE: one statue of the Four Voices
//       goes dark (its throat crystal turns to sculk) and a tether of that
//       voice's colour runs to it — while it holds a stolen voice it takes
//       15 % damage. Stand at that statue's foot (within 4.5 blocks) for 3 s
//       to sing the voice back: the statue relights, the tether snaps and it
//       reels (staggered 3 s, taking 150 % damage).
//   II  "The Undersong" (66–33 %): adds sonic rings along the plaza floor
//       (jump them: 6 magic damage and a shove) and the HEART'S PULSE:
//       every 30 s the Heart charges for 3 s (its column fills with motes,
//       a rising note) and pulses — if the Unsung is under the rings (within
//       9 blocks of the dais) it is struck for 25, staggered 5 s and any
//       stolen voice is sung back by the Heart itself; players by the dais
//       are given Regeneration II. Lure it in.
//   III "The Last Rest" (below 33 %): every ~35 s it flies to the dais and
//       CHANNELS for 8 s, drinking the Chord from the rings (a dark stream
//       from the Heart into its throat): deal 40 damage during the cast to
//       break it (it reels 3 s); a completed cast heals it 60 and lays the
//       Silence over the plaza — Darkness and Slowness for 6 s.
// Each phase change is a 2 s tell: it rears up, roars, the shards flare, and
// it takes no damage while it re-forms.
// Death (80 ticks): it is sung to rest — it rises and dissolves while its
// four voices stream back to the four statues.
#pragma once

#include "common/entity/Monster.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Game {

    class TheUnsung : public Monster {
    public:
        static constexpr float  kMaxHealth       = 500.0f;
        static constexpr int    kEmergeTicks     = 60;    // phase 0: rising out of the Heart
        static constexpr int    kTransitionTicks = 40;    // a phase change's tell
        static constexpr int    kRestoreTicks    = 60;    // standing at a statue to restore it
        static constexpr double kRestoreRadius   = 4.5;
        static constexpr float  kShieldFactor    = 0.15f; // damage taken with a stolen voice
        static constexpr float  kStaggerFactor   = 1.5f;
        static constexpr int    kHeartChargeTicks = 60;
        static constexpr double kHeartReach      = 9.0;   // "under the rings"
        static constexpr float  kHeartStrike     = 25.0f;
        static constexpr int    kChannelTicks    = 160;
        static constexpr float  kChannelBreak    = 40.0f;
        static constexpr float  kChannelHeal     = 60.0f;
        static constexpr double kLeash           = 26.0;  // how far from the Heart it will go
        static constexpr double kRingMaxRadius   = 18.0;
        static constexpr double kRingSpeed       = 0.45;  // blocks per tick
        static constexpr double kRingHalfWidth   = 0.75;
        static constexpr int    kDeathTicks      = 80;

        // Entity events (client: HandleEntityEvent).
        static constexpr uint8_t kEventRisen          = 72;
        static constexpr uint8_t kEventPhaseShift     = 73;
        static constexpr uint8_t kEventSonicRing      = 74;
        static constexpr uint8_t kEventSteal          = 75;
        static constexpr uint8_t kEventRestored       = 76;
        static constexpr uint8_t kEventHeartCharge    = 77;
        static constexpr uint8_t kEventHeartPulse     = 78;
        static constexpr uint8_t kEventChannelBroken  = 79;
        static constexpr uint8_t kEventSilence        = 80;
        static constexpr uint8_t kEventShieldHit      = 81;
        static constexpr uint8_t kEventSwing          = 82;

        explicit TheUnsung(EntityLevel* level);

        static void CreateAttributes(AttributeMap& out);

        // ── The arena (the quest controller's contract) ─────────────────
        // `heart` is the resonance engine block; `rotation` the city's
        // template rotation (0..3; -1 unknown — taken as 0: the statues
        // stand on the four axes either way, only the voices' names may then
        // be turned).
        void SetArena(const glm::ivec3& heart, int rotation);
        bool HasArena() const { return m_hasArena; }
        const glm::ivec3& ArenaHeart() const { return m_heart; }
        int  ArenaRotation() const { return m_rotation; }

        bool RemoveWhenFarAway(double) const override { return false; }
        bool CauseFallDamage(double, float) override { return false; }
        bool IsPushable() const override { return false; }
        int  DecreaseAirSupply(int currentSupply) override { return currentSupply; }
        void Travel(const glm::dvec3& input) override;
        void Tick() override;
        void TickDeath() override;
        void HandleEntityEvent(uint8_t id) override;
        bool Hurt(MobDamageSource source, float amount, Entity* attacker) override;
        void DropCustomDeathLoot(EntityLevel& level) override;
        // Taken out of the world without dying (/kill, peaceful): a stolen
        // voice goes back to its statue first, so the plaza is never left
        // with a darkened statue.
        void KillFromCommand() override;
        void CheckDespawn() override;

        std::shared_ptr<SpawnGroupData>
        FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) override;

        // ── Synced state (anim byte + variant byte) ─────────────────────
        // Anim byte: bits 0-1 phase (0 = emerging, 1..3), bit 2 winding up /
        // conducting (arms raised), bit 3 channelling, bit 4 holding a stolen
        // voice, bit 5 the Heart charging its pulse, bit 7 staggered.
        // Variant byte: bits 0-1 the stolen Voice (Game::Aurelith::Voice),
        // bits 2-3 the city's rotation (0 when unknown), bits 4-6 the
        // restoration progress (0..7), bit 7 "the arena has statues". With
        // the rotation the client finds each voice's statue from the Heart.
        uint8_t GetAnimStateByte() const override;
        void    SetAnimStateByte(uint8_t v) override { m_anim = v; }
        uint8_t GetVariantByte() const override;
        void    SetVariantByte(uint8_t v) override { m_variant = v; }

        int  SyncedPhase() const { return m_anim & 3; }
        bool IsEmerging() const { return (m_anim & 3) == 0; }
        bool IsWindingUp() const { return (m_anim & 4) != 0; }
        bool IsChannelling() const { return (m_anim & 8) != 0; }
        bool IsShielded() const { return (m_anim & 16) != 0; }
        bool IsStaggered() const { return (m_anim & 128) != 0; }
        bool IsHeartCharging() const { return (m_anim & 32) != 0; }
        int  StolenVoice() const { return m_variant & 3; }
        int  SyncedRotation() const { return (m_variant >> 2) & 3; }
        float RestoreProgress() const { return static_cast<float>((m_variant >> 4) & 7) / 7.0f; }
        bool ArenaHasStatues() const { return (m_variant & 128) != 0; }
        // Client: ticks since this client saw it begin to rise (the render
        // module's emergence), or a large number when it arrived risen.
        int  ClientEmergeAge() const { return m_clientEmergeAge; }

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
            std::vector<int32_t> hit;
        };
        struct ClientRing { glm::dvec3 centre; double baseY; int age; };

        // Arena geometry (all from the Heart; AurelithQuest.hpp).
        int         StreetY() const;
        glm::dvec3  StatueBase(int voice) const;       // plinth centre at street level
        glm::ivec3  StatueCrystal(int voice) const;    // the voice at the throat
        bool        StatuesPresent();
        double      FloorBelow(const glm::dvec3& p) const;
        double      HeartDistanceH() const;

        int  HealthPhase() const;
        void BeginPhase(int phase);
        void Hover(const glm::dvec3& want, double maxSpeed);
        void ChooseMove();
        void TickMelee(LivingEntity* target);
        void FireNotes(LivingEntity* target);
        void Conduct();
        void StartRing();
        void TickRings();
        void StealVoice();
        void TickRestore();
        void RestoreVoice(bool byHeart);
        void TickHeartPulse();
        void TickChannel(LivingEntity* target);
        void Stagger(int ticks);
        void TellPlayers(const std::string& text, double range) const;
        void ClientEffects();
        bool ClientFindArena();

        // ── Server ──────────────────────────────────────────────────────
        glm::ivec3 m_heart{0};
        int        m_rotation = -1;
        bool       m_hasArena = false;
        bool       m_realHeart = false;       // a resonance engine is there
        bool       m_heartChecked = false;
        int        m_statues = -1;            // -1 unknown, 0 none, 1 present
        double     m_spawnY = 0.0;
        int        m_emerge = 0;              // ticks risen so far (phase 0)
        int        m_phase = 0;               // 0 emerging, 1..3
        int        m_transition = 0;
        int        m_windup = 0;
        int        m_attackCooldown = 40;
        int        m_noteIn = 80;
        int        m_choirIn = 160;
        int        m_ringIn = 100;
        int        m_stealIn = 40;
        int        m_stolen = -1;             // the Voice held, -1 none
        int        m_lastStolen = -1;
        int        m_restore = 0;             // 0..kRestoreTicks
        int        m_heartPulseIn = 240;
        int        m_heartCharge = 0;         // > 0 while the Heart charges
        int        m_stagger = 0;
        int        m_channelIn = 200;
        int        m_channel = 0;             // > 0 while channelling
        bool       m_channelFlying = false;
        float      m_channelDamage = 0.0f;
        bool       m_heartStrike = false;     // the next Hurt is the Heart's own
        int        m_conductTicks = 0;
        float      m_orbit = 0.0f;
        std::vector<Ring> m_rings;

        // ── Client ──────────────────────────────────────────────────────
        uint8_t    m_anim = 0;
        uint8_t    m_variant = 0;
        int        m_clientEmergeAge = 1000;
        bool       m_clientSawEmerging = false;
        bool       m_clientArena = false;
        glm::ivec3 m_clientHeart{0};
        int        m_clientScanIn = 0;
        int        m_clientScans = 0;
        int        m_clientHeartCharge = 0;
        int        m_clientPulseAge = -1;
        std::vector<ClientRing> m_clientRings;
    };

} // namespace Game
