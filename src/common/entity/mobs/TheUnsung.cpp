// File: src/common/entity/mobs/TheUnsung.cpp
//
// See TheUnsung.hpp for the fight. Layout of this file:
//   helpers            (facing, players near, the voices' names and colours)
//   the arena          (geometry from the Heart: street, statues, floor)
//   construction / spawn / save
//   the server's fight (phases, movement, the baton, notes, the choir, the
//                       rings, the stolen voices, the Heart's pulse, the
//                       channel)
//   damage and death
//   the client's half  (events, the arena scan, every particle)
#include "common/entity/mobs/TheUnsung.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/core/Mth.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/Item.hpp"
#include "common/entity/ModMobNbt.hpp"
#include "common/entity/ai/goals/BasicGoals.hpp"
#include "common/entity/ai/goals/TargetGoals.hpp"
#include "common/entity/effect/MobEffects.hpp"
#include "common/entity/mobs/GenericMobs.hpp"
#include "common/entity/projectile/ShulkerBullet.hpp"
#include "common/physics/Physics.hpp"
#include "common/sound/SoundSource.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/level/AurelithQuest.hpp"

#include <glm/glm.hpp>

#include <algorithm>
#include <climits>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

namespace Game {

    namespace {

        namespace A = Aurelith;

        constexpr double kTwoPi = 6.283185307179586;
        // Model space: the hollow throat sits 3 blocks over the feet at the
        // render module's modelScale (1.6).
        constexpr double kThroatHeight = 3.0;

        // Sound events (assets/sound_overlays/obeycraft/entities.json).
        constexpr const char* kSoundEmerge       = "obeycraft:entity.the_unsung.emerge";
        constexpr const char* kSoundPhase        = "obeycraft:entity.the_unsung.phase";
        constexpr const char* kSoundWindup       = "obeycraft:entity.the_unsung.windup";
        constexpr const char* kSoundSwing        = "obeycraft:entity.the_unsung.swing";
        constexpr const char* kSoundNote         = "obeycraft:entity.the_unsung.note";
        constexpr const char* kSoundChoir        = "obeycraft:entity.the_unsung.choir";
        constexpr const char* kSoundRing         = "obeycraft:entity.the_unsung.ring";
        constexpr const char* kSoundSteal        = "obeycraft:entity.the_unsung.steal";
        constexpr const char* kSoundRestore      = "obeycraft:entity.the_unsung.restore";
        constexpr const char* kSoundShield       = "obeycraft:entity.the_unsung.shield";
        constexpr const char* kSoundHeartCharge  = "obeycraft:entity.the_unsung.heart_charge";
        constexpr const char* kSoundHeartPulse   = "obeycraft:entity.the_unsung.heart_pulse";
        constexpr const char* kSoundChannel      = "obeycraft:entity.the_unsung.channel";
        constexpr const char* kSoundChannelBreak = "obeycraft:entity.the_unsung.channel_break";
        constexpr const char* kSoundSilence      = "obeycraft:entity.the_unsung.silence";
        constexpr const char* kSoundRest         = "obeycraft:entity.the_unsung.sung_to_rest";

        // The yRot that faces along (dx, dz) — MC's atan2(dz, dx) - 90.
        float YawToward(double dx, double dz) {
            return static_cast<float>(std::atan2(dz, dx) * Mth::kRadToDeg) - 90.0f;
        }

        glm::dvec3 FacingXZ(float yRotDeg) {
            const double r = static_cast<double>(yRotDeg) * Mth::kDegToRad;
            return glm::dvec3(-std::sin(r), 0.0, std::cos(r));
        }

        // Every living, non-spectator, attackable player within `range`.
        void PlayersNear(EntityLevel& level, const glm::dvec3& from, double range,
                         std::vector<LivingEntity*>& out) {
            std::vector<LivingEntity*> all;
            level.GetPlayers(all);
            const double r2 = range * range;
            for (LivingEntity* p : all) {
                if (!p || !p->IsAlive() || p->IsSpectator()) continue;
                const glm::dvec3 d = p->position - from;
                if (glm::dot(d, d) <= r2) out.push_back(p);
            }
        }

        const char* VoiceTitle(int v) {
            switch (v) {
                case 0: return "Soprano";
                case 1: return "Alto";
                case 2: return "Tenor";
                case 3: return "Bass";
            }
            return "Soprano";
        }

        glm::vec3 VoiceRgb(int v) {
            const uint32_t c = A::VoiceColour(static_cast<A::Voice>(v & 3));
            return glm::vec3(((c >> 16) & 0xFF) / 255.0f, ((c >> 8) & 0xFF) / 255.0f, (c & 0xFF) / 255.0f);
        }

        double HDist(const glm::dvec3& a, const glm::dvec3& b) {
            return std::hypot(a.x - b.x, a.z - b.z);
        }

    } // namespace

    // ══ The arena ══════════════════════════════════════════════════════════

    int TheUnsung::StreetY() const { return m_heart.y - A::kStreetBelowHeart; }

    glm::dvec3 TheUnsung::StatueBase(int voice) const {
        const glm::ivec3 p = A::DesignToWorld(m_heart, std::max(0, m_rotation),
                                              A::StatueOffset(static_cast<A::Voice>(voice & 3)), 1);
        return glm::dvec3(p.x + 0.5, p.y, p.z + 0.5);
    }

    glm::ivec3 TheUnsung::StatueCrystal(int voice) const {
        // gen_aurelith.py build_statues: the voice at the throat, street + 9.
        return A::DesignToWorld(m_heart, std::max(0, m_rotation),
                                A::StatueOffset(static_cast<A::Voice>(voice & 3)), 9);
    }

    bool TheUnsung::StatuesPresent() {
        if (m_statues >= 0) return m_statues == 1;
        // Without a resonance engine under it there is no plaza (an egg's
        // stand-in Heart). Not cached: the engine's chunk may simply not be
        // back yet after a reload.
        if (!m_level || !m_level->Blocks() || !m_realHeart) return false;
        const IBlockAccess& blocks = *m_level->Blocks();
        int found = 0;
        for (int v = 0; v < A::kVoiceCount; ++v) {
            const glm::ivec3 c = StatueCrystal(v);
            const BlockID at = blocks.GetBlock(c.x, c.y, c.z);
            if (at == BlockID::ResonantCrystal || at == BlockID::Sculk) ++found;
        }
        // All four: a Plaza of the Held Note. Anything less (a player's
        // copy, a ruined plaza) fights without the statues.
        m_statues = found == A::kVoiceCount ? 1 : 0;
        return m_statues == 1;
    }

    double TheUnsung::FloorBelow(const glm::dvec3& p) const {
        const IBlockAccess* blocks = m_level ? m_level->Blocks() : nullptr;
        const int x = static_cast<int>(std::floor(p.x));
        const int z = static_cast<int>(std::floor(p.z));
        const int top = static_cast<int>(std::floor(p.y));
        if (!blocks) return p.y - 24.0;
        for (int y = top; y > top - 24; --y) {
            if (blocks->IsBlockSolid(x, y, z)) return static_cast<double>(y + 1);
        }
        return p.y - 24.0;
    }

    double TheUnsung::HeartDistanceH() const {
        return HDist(position, glm::dvec3(m_heart) + glm::dvec3(0.5, 0.0, 0.5));
    }

    // ══ Construction, spawn, save ═════════════════════════════════════════

    void TheUnsung::CreateAttributes(AttributeMap& out) {
        CreateMonsterAttributes(out);
        out.Register(Attribute::MaxHealth,           static_cast<double>(kMaxHealth));
        out.Register(Attribute::MovementSpeed,       0.25);
        out.Register(Attribute::FlyingSpeed,         0.3);
        out.Register(Attribute::KnockbackResistance, 1.0);
        out.Register(Attribute::AttackDamage,        8.0);
        out.Register(Attribute::FollowRange,        48.0);
        out.Register(Attribute::Armor,              10.0);
    }

    TheUnsung::TheUnsung(EntityLevel* level) : Monster(EntityTypeId::TheUnsung, level) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();
        SetNoGravity(true);
        SetPersistenceRequired(true);
        RegisterGoals();
    }

    void TheUnsung::RegisterGoals() {
        // Movement and every attack are the phase machine in
        // CustomServerAiStep; the goals only pick the target and turn the
        // head.
        m_goalSelector.AddGoal(8, std::make_unique<LookAtPlayerGoal>(this, 32.0f));
        m_targetSelector.AddGoal(1, std::make_unique<HurtByTargetGoal>(this));
        m_targetSelector.AddGoal(2, std::make_unique<NearestAttackablePlayerGoal>(this, false));
    }

    void TheUnsung::SetArena(const glm::ivec3& heart, int rotation) {
        m_heart = heart;
        m_rotation = rotation;
        m_hasArena = true;
        m_statues = -1;
    }

    std::shared_ptr<SpawnGroupData>
    TheUnsung::FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) {
        if (!m_hasArena && m_level && m_level->Blocks()) {
            // An egg or /summon in a Plaza of the Held Note: adopt its Heart
            // (the nearest resonance engine within reach), so the whole
            // fight can be tried without waking the city. The rotation is
            // unknown here (-1: the statues are found on the axes either way).
            const IBlockAccess& blocks = *m_level->Blocks();
            const glm::ivec3 at = BlockPosition();
            int best = INT_MAX;
            glm::ivec3 found(0);
            for (int dy = -8; dy <= 12; ++dy) {
                for (int dx = -32; dx <= 32; ++dx) {
                    for (int dz = -32; dz <= 32; ++dz) {
                        const int d2 = dx * dx + dz * dz;
                        if (d2 >= best) continue;
                        if (blocks.GetBlock(at.x + dx, at.y + dy, at.z + dz) != BlockID::ResonanceEngine) continue;
                        best = d2;
                        found = at + glm::ivec3(dx, dy, dz);
                    }
                }
            }
            if (best != INT_MAX) {
                SetArena(found, -1);
                m_emerge = kEmergeTicks;
            }
        }
        if (!m_hasArena) {
            // An egg or /summon: the ground under the spawn is its plaza;
            // the stand-in Heart sits where Aurelith's would (street + 5).
            const double floor = FloorBelow(position + glm::dvec3(0.0, 1.0, 0.0));
            SetArena(glm::ivec3(static_cast<int>(std::floor(position.x)),
                                static_cast<int>(std::floor(floor)) - 1 + A::kStreetBelowHeart,
                                static_cast<int>(std::floor(position.z))), -1);
            // Nothing to rise out of: it appears already risen.
            m_emerge = kEmergeTicks;
        }
        m_spawnY = position.y;
        return Monster::FinalizeSpawn(reason, std::move(groupData));
    }

    void TheUnsung::SaveModNbt(ModNbtOut& out) const {
        out.Bool("HasArena", m_hasArena);
        out.Int("HeartX", m_heart.x);
        out.Int("HeartY", m_heart.y);
        out.Int("HeartZ", m_heart.z);
        out.Int("Rotation", m_rotation);
        out.Int("Emerge", m_emerge);
        out.Float("SpawnY", static_cast<float>(m_spawnY));
        out.Int("Phase", m_phase);
        out.Int("Stolen", m_stolen);
        out.Int("Restore", m_restore);
        out.Int("ChannelIn", m_channelIn);
        out.Int("HeartPulseIn", m_heartPulseIn);
    }

    void TheUnsung::LoadModNbt(const ModNbtIn& in) {
        m_hasArena = in.Bool("HasArena", false);
        m_heart = glm::ivec3(in.Int("HeartX", 0), in.Int("HeartY", 0), in.Int("HeartZ", 0));
        m_rotation = in.Int("Rotation", -1);
        m_emerge = in.Int("Emerge", kEmergeTicks);
        m_spawnY = static_cast<double>(in.Float("SpawnY", static_cast<float>(position.y)));
        m_phase = std::clamp(in.Int("Phase", 0), 0, 3);
        m_stolen = std::clamp(in.Int("Stolen", -1), -1, 3);
        m_restore = std::clamp(in.Int("Restore", 0), 0, kRestoreTicks);
        m_channelIn = in.Int("ChannelIn", 200);
        m_heartPulseIn = in.Int("HeartPulseIn", 240);
        m_statues = -1;
    }

    uint8_t TheUnsung::GetAnimStateByte() const { return m_anim; }
    uint8_t TheUnsung::GetVariantByte() const { return m_variant; }

    // ══ The server's fight ════════════════════════════════════════════════

    int TheUnsung::HealthPhase() const {
        const float f = GetMaxHealth() > 0.0f ? GetHealth() / GetMaxHealth() : 0.0f;
        if (f > 2.0f / 3.0f) return 1;
        if (f > 1.0f / 3.0f) return 2;
        return 3;
    }

    void TheUnsung::TellPlayers(const std::string& text, double range) const {
        if (!m_level) return;
        std::vector<LivingEntity*> players;
        PlayersNear(*m_level, position, range, players);
        for (LivingEntity* p : players) m_level->DisplayClientMessage(*p, text, true);
    }

    void TheUnsung::BeginPhase(int phase) {
        const bool first = m_phase == 0;
        m_phase = phase;
        if (!m_level) return;
        if (first) {
            // Risen: the fight begins. No tell — the emergence was the tell.
            m_level->BroadcastEntityEvent(*this, kEventRisen);
            TellPlayers("The Unsung rises. It has no voice of its own - it will take yours.", 64.0);
            m_stealIn = 60;
            return;
        }
        m_transition = kTransitionTicks;
        m_windup = 0;
        m_channel = 0;
        m_channelFlying = false;
        m_level->BroadcastEntityEvent(*this, kEventPhaseShift);
        PlaySound(kSoundPhase, 5.0f, phase == 2 ? 0.8f : 0.6f);
        if (phase == 2) {
            TellPlayers("The Undersong swells. When the Heart pulses, draw it beneath the rings.", 64.0);
            m_heartPulseIn = 160;
            m_ringIn = 80;
        } else if (phase == 3) {
            TellPlayers("It reaches for the Heart itself. Break its song before the Silence falls.", 64.0);
            m_channelIn = 140;
        }
        if (m_stolen < 0) m_stealIn = std::min(m_stealIn, 80);
    }

    void TheUnsung::Stagger(int ticks) {
        m_stagger = std::max(m_stagger, ticks);
        m_windup = 0;
        m_channelFlying = false;
    }

    void TheUnsung::Hover(const glm::dvec3& want, double maxSpeed) {
        const glm::dvec3 d = want - position;
        const double len = glm::length(d);
        if (len > 1.0e-4) {
            const glm::dvec3 push = d / len * std::min(maxSpeed, len * 0.08);
            velocity += (push - velocity) * 0.2;
        } else {
            velocity *= 0.8;
        }
    }

    void TheUnsung::ChooseMove() {
        const glm::dvec3 heartCentre = glm::dvec3(m_heart) + glm::dvec3(0.5, 0.0, 0.5);
        const double breathe = 0.25 * std::sin(tickCount * 0.06);
        LivingEntity* target = GetTarget();

        if (m_channel > 0 || m_channelFlying) {
            // Over the Heart's core, where the rings hang lowest.
            const glm::dvec3 want = heartCentre + glm::dvec3(0.0, 1.3, 0.0);
            Hover(want, m_channel > 0 ? 0.05 : 0.35);
        } else if (m_stagger > 0) {
            // Reeling: it sags toward the floor where it is.
            const glm::dvec3 want(position.x, FloorBelow(position + glm::dvec3(0.0, 2.0, 0.0)) + 0.3,
                                  position.z);
            Hover(want, 0.08);
        } else if (target) {
            glm::dvec3 to = target->position - position;
            to.y = 0.0;
            const double len = glm::length(to);
            glm::dvec3 want = len > 1.0e-4 ? target->position - to / len * 3.0 : position;
            // Never far from the dais: it is bound to the Heart it rose from.
            glm::dvec3 off = want - heartCentre;
            off.y = 0.0;
            const double r = glm::length(off);
            if (r > kLeash) want = heartCentre + off / r * kLeash + glm::dvec3(0.0, want.y - heartCentre.y, 0.0);
            want.y = FloorBelow(glm::dvec3(want.x, want.y + 5.0, want.z)) + 1.0 + breathe;
            // During a phase's tell it rears up instead.
            if (m_transition > 0) want = position + glm::dvec3(0.0, 0.12, 0.0);
            Hover(want, m_phase >= 3 ? 0.32 : 0.26);
        } else {
            // No one to sing at: a slow circle round the dais.
            m_orbit += 0.008f;
            glm::dvec3 want = heartCentre + glm::dvec3(8.0 * std::cos(m_orbit), 0.0, 8.0 * std::sin(m_orbit));
            want.y = FloorBelow(want + glm::dvec3(0.0, 8.0, 0.0)) + 1.5 + breathe;
            Hover(want, 0.15);
        }

        if (target && m_channel <= 0) {
            yRot = Mth::ApproachDegrees(yRot, YawToward(target->position.x - position.x,
                                                        target->position.z - position.z), 8.0f);
        } else if (velocity.x * velocity.x + velocity.z * velocity.z > 1.0e-5) {
            yRot = Mth::ApproachDegrees(yRot, YawToward(velocity.x, velocity.z), 5.0f);
        }
        yBodyRot = yRot;
    }

    void TheUnsung::Travel(const glm::dvec3& input) {
        (void)input;
        // Emerging, it passes up through the dais and the Heart's own core
        // (no collision); afterwards it moves like any flier.
        if (m_emerge < kEmergeTicks && !(m_level && m_level->IsClientSide())) {
            position += velocity;
            return;
        }
        Move(velocity);
        velocity *= 0.9;
    }

    void TheUnsung::TickMelee(LivingEntity* target) {
        if (m_attackCooldown > 0) --m_attackCooldown;
        if (m_windup > 0) {
            if (--m_windup == 0) {
                // The sweep: a wide arc in front of it.
                m_level->BroadcastEntityEvent(*this, kEventSwing);
                PlaySound(kSoundSwing, 3.0f, 0.7f);
                const float damage = m_phase >= 3 ? 12.0f : (m_phase == 2 ? 10.0f : 8.0f);
                const glm::dvec3 facing = FacingXZ(yRot);
                std::vector<LivingEntity*> players;
                PlayersNear(*m_level, position, 5.0, players);
                for (LivingEntity* p : players) {
                    glm::dvec3 d = p->position - position;
                    if (std::abs(d.y) > 4.5) continue;
                    d.y = 0.0;
                    const double len = glm::length(d);
                    if (len > 4.5) continue;
                    if (len > 0.5 && glm::dot(d / len, facing) < -0.2) continue;   // behind it
                    if (p->Hurt(MobDamageSource::MobAttack, damage, this)) {
                        const glm::dvec3 out = len > 1.0e-4 ? d / len : facing;
                        p->velocity += glm::dvec3(out.x * 0.9, 0.35, out.z * 0.9);
                        p->hurtMarked = true;
                    }
                }
                m_attackCooldown = 36;
            }
            return;
        }
        if (!target || m_attackCooldown > 0) return;
        glm::dvec3 d = target->position - position;
        if (std::abs(d.y) > 4.5) return;
        d.y = 0.0;
        if (glm::length(d) > 4.0) return;
        // The wind-up: the baton raised for 0.7 s — step back out of the arc.
        m_windup = 14;
        PlaySound(kSoundWindup, 2.0f, 0.9f);
    }

    void TheUnsung::FireNotes(LivingEntity* target) {
        if (--m_noteIn > 0 || !target) return;
        m_noteIn = m_phase >= 3 ? 70 : (m_phase == 2 ? 80 : 90);
        std::vector<LivingEntity*> players;
        PlayersNear(*m_level, position, 28.0, players);
        if (players.empty()) return;
        const int volley = m_phase == 1 ? 1 : 2;
        for (int i = 0; i < volley; ++i) {
            LivingEntity* aim = players[static_cast<size_t>(i) % players.size()];
            if (i == 0) aim = target;
            auto note = std::make_unique<ShulkerBullet>(m_level);
            note->InitShot(*this, *aim, -1);
            m_level->AddFreshEntity(std::move(note));
        }
        m_conductTicks = 12;
        PlaySound(kSoundNote, 2.5f, 0.6f + 0.1f * static_cast<float>(m_phase));
    }

    void TheUnsung::Conduct() {
        if (--m_choirIn > 0) return;
        m_choirIn = m_phase >= 3 ? 320 : (m_phase == 2 ? 360 : 400);
        const glm::vec3 c(static_cast<float>(position.x), static_cast<float>(position.y),
                          static_cast<float>(position.z));
        std::vector<Entity*> nearby;
        m_level->GetEntitiesInBox(AABB::FromMinMax(c - glm::vec3(32.0f), c + glm::vec3(32.0f)),
                                  this, nearby);
        int wraiths = 0;
        for (Entity* e : nearby) {
            if (e && !e->IsRemoved() && e->GetType() == EntityTypeId::EchoWraith) ++wraiths;
        }
        const int count = std::min(2, 4 - wraiths);
        if (count <= 0) return;
        m_conductTicks = 30;
        PlaySound(kSoundChoir, 3.0f, 1.2f);
        JavaRandom& rng = m_level->Random();
        for (int i = 0; i < count; ++i) {
            std::unique_ptr<Mob> wraith = MakeGenericMob(EntityTypeId::EchoWraith, m_level);
            if (!wraith) return;
            wraith->position = position + glm::dvec3(rng.NextDouble() * 6.0 - 3.0,
                                                     1.0 + rng.NextDouble() * 1.5,
                                                     rng.NextDouble() * 6.0 - 3.0);
            wraith->yRot = rng.NextFloat() * 360.0f;
            wraith->FinalizeSpawn(SpawnReason::MobSummoned, nullptr);
            if (LivingEntity* t = GetTarget()) wraith->SetTarget(t);
            m_level->AddFreshEntity(std::move(wraith));
        }
    }

    void TheUnsung::StartRing() {
        Ring ring;
        ring.centre = position;
        ring.baseY = FloorBelow(position + glm::dvec3(0.0, 1.0, 0.0));
        ring.radius = 1.0;
        m_rings.push_back(std::move(ring));
        m_level->BroadcastEntityEvent(*this, kEventSonicRing);
        PlaySound(kSoundRing, 3.0f, 0.7f);
    }

    void TheUnsung::TickRings() {
        if (m_rings.empty()) return;
        std::vector<LivingEntity*> players;
        PlayersNear(*m_level, position, kRingMaxRadius + 10.0, players);
        for (Ring& ring : m_rings) {
            ring.radius += kRingSpeed;
            for (LivingEntity* p : players) {
                const double dx = p->position.x - ring.centre.x;
                const double dz = p->position.z - ring.centre.z;
                const double d = std::sqrt(dx * dx + dz * dz);
                if (std::abs(d - ring.radius) > kRingHalfWidth) continue;
                // Jumped: feet more than 0.9 over the ring's floor.
                const double above = p->position.y - ring.baseY;
                if (above > 0.9 || above < -2.5) continue;
                if (std::find(ring.hit.begin(), ring.hit.end(), p->GetId()) != ring.hit.end()) continue;
                ring.hit.push_back(p->GetId());
                p->Hurt(MobDamageSource::Magic, 6.0f, this);
                const glm::dvec3 out = d > 1.0e-4 ? glm::dvec3(dx / d, 0.0, dz / d) : glm::dvec3(1.0, 0.0, 0.0);
                p->velocity += glm::dvec3(out.x * 1.0, 0.4, out.z * 1.0);
                p->hurtMarked = true;
            }
        }
        m_rings.erase(std::remove_if(m_rings.begin(), m_rings.end(),
                                     [](const Ring& r) { return r.radius > kRingMaxRadius; }),
                      m_rings.end());
    }

    void TheUnsung::StealVoice() {
        JavaRandom& rng = m_level->Random();
        int voice = rng.NextInt(A::kVoiceCount);
        if (voice == m_lastStolen) voice = (voice + 1 + rng.NextInt(A::kVoiceCount - 1)) % A::kVoiceCount;
        m_stolen = voice;
        m_restore = 0;
        // The statue goes dark: its voice at the throat turns to sculk.
        const glm::ivec3 c = StatueCrystal(voice);
        if (m_level->Blocks() && m_level->Blocks()->GetBlock(c.x, c.y, c.z) == BlockID::ResonantCrystal) {
            m_level->SetBlock(c, BlockID::Sculk);
        }
        m_conductTicks = 30;
        m_level->BroadcastEntityEvent(*this, kEventSteal);
        PlaySound(kSoundSteal, 4.0f, 0.8f);
        m_level->PlaySound(nullptr, glm::dvec3(c) + glm::dvec3(0.5), kSoundSteal, SoundSource::Hostile, 3.0f, 1.3f);
        TellPlayers(std::string("It has taken the ") + VoiceTitle(voice) + "'s voice. Stand at the "
                    + VoiceTitle(voice) + "'s statue to sing it back.", 64.0);
    }

    void TheUnsung::RestoreVoice(bool byHeart) {
        if (m_stolen < 0) return;
        const int voice = m_stolen;
        const glm::ivec3 c = StatueCrystal(voice);
        if (m_level->Blocks() && m_level->Blocks()->GetBlock(c.x, c.y, c.z) == BlockID::Sculk) {
            m_level->SetBlock(c, BlockID::ResonantCrystal);
        }
        m_lastStolen = voice;
        m_stolen = -1;
        m_restore = 0;
        m_stealIn = m_phase >= 3 ? 300 : (m_phase == 2 ? 360 : 400);
        m_level->BroadcastEntityEvent(*this, kEventRestored);
        m_level->PlaySound(nullptr, glm::dvec3(c) + glm::dvec3(0.5), kSoundRestore, SoundSource::Hostile, 4.0f,
                           A::VoicePitch(static_cast<A::Voice>(voice)));
        if (!byHeart) {
            Stagger(60);
            TellPlayers(std::string("The ") + VoiceTitle(voice) + " sings again. The Unsung reels.", 64.0);
        }
    }

    void TheUnsung::TickRestore() {
        if (m_stolen < 0) {
            if (StatuesPresent() && m_transition <= 0 && --m_stealIn <= 0) StealVoice();
            return;
        }
        const glm::dvec3 base = StatueBase(m_stolen);
        std::vector<LivingEntity*> players;
        PlayersNear(*m_level, base, kRestoreRadius + 2.0, players);
        std::vector<LivingEntity*> singing;
        for (LivingEntity* p : players) {
            if (HDist(p->position, base) > kRestoreRadius) continue;
            if (p->position.y < base.y - 1.5 || p->position.y > base.y + 5.0) continue;
            singing.push_back(p);
        }
        if (singing.empty()) {
            m_restore = std::max(0, m_restore - 1);
            return;
        }
        m_restore += singing.size() >= 2 ? 2 : 1;
        if (m_restore >= kRestoreTicks) {
            RestoreVoice(false);
            return;
        }
        if (tickCount % 10 == 0) {
            const int pct = m_restore * 100 / kRestoreTicks;
            for (LivingEntity* p : singing) {
                m_level->DisplayClientMessage(*p, std::string("Singing the ") + VoiceTitle(m_stolen)
                                                  + " back... " + std::to_string(pct) + "%", true);
            }
        }
    }

    void TheUnsung::TickHeartPulse() {
        if (m_phase < 2 || !m_realHeart) return;
        const glm::dvec3 core = glm::dvec3(m_heart) + glm::dvec3(0.5, 0.5, 0.5);
        if (m_heartCharge > 0) {
            if (--m_heartCharge > 0) return;
            // The pulse.
            m_level->BroadcastEntityEvent(*this, kEventHeartPulse);
            m_level->PlaySound(nullptr, core, kSoundHeartPulse, SoundSource::Hostile, 6.0f, 1.0f);
            std::vector<LivingEntity*> players;
            PlayersNear(*m_level, core, 14.0, players);
            for (LivingEntity* p : players) {
                if (HDist(p->position, core) <= 10.0) {
                    p->AddEffect(MobEffectInstance(MobEffectId::Regeneration, 100, 1), this);
                }
            }
            if (HeartDistanceH() <= kHeartReach) {
                RestoreVoice(true);
                m_heartStrike = true;
                Hurt(MobDamageSource::Magic, kHeartStrike, nullptr);
                m_heartStrike = false;
                Stagger(100);
                m_channel = 0;
                TellPlayers("The Heart sings through it.", 48.0);
            }
            m_heartPulseIn = 600;
            return;
        }
        if (--m_heartPulseIn <= 0) {
            m_heartCharge = kHeartChargeTicks;
            m_level->BroadcastEntityEvent(*this, kEventHeartCharge);
            m_level->PlaySound(nullptr, core, kSoundHeartCharge, SoundSource::Hostile, 5.0f, 1.0f);
            TellPlayers("The Heart gathers itself...", 48.0);
        }
    }

    void TheUnsung::TickChannel(LivingEntity* target) {
        if (m_phase < 3) return;
        const glm::dvec3 over = glm::dvec3(m_heart) + glm::dvec3(0.5, 1.3, 0.5);
        if (m_channel > 0) {
            if (--m_channel == 0) {
                // The Silence falls.
                Heal(kChannelHeal);
                m_level->BroadcastEntityEvent(*this, kEventSilence);
                PlaySound(kSoundSilence, 6.0f, 0.5f);
                std::vector<LivingEntity*> players;
                PlayersNear(*m_level, position, 32.0, players);
                for (LivingEntity* p : players) {
                    p->AddEffect(MobEffectInstance(MobEffectId::Darkness, 120, 0), this);
                    p->AddEffect(MobEffectInstance(MobEffectId::Slowness, 120, 0), this);
                }
                TellPlayers("The Silence falls. The Unsung drank the Chord.", 64.0);
                m_channelIn = 700;
            }
            return;
        }
        if (m_channelFlying) {
            if (glm::length(over - position) < 1.4) {
                m_channelFlying = false;
                m_channel = kChannelTicks;
                m_channelDamage = 0.0f;
                PlaySound(kSoundChannel, 5.0f, 0.8f);
                TellPlayers("It is drinking the Chord! Strike it before the song ends.", 64.0);
            }
            return;
        }
        if (target && m_stagger <= 0 && m_windup <= 0 && --m_channelIn <= 0) m_channelFlying = true;
    }

    void TheUnsung::CustomServerAiStep() {
        if (!m_level) return;
        if (!m_hasArena) {
            SetArena(BlockPosition() + glm::ivec3(0, A::kStreetBelowHeart, 0), -1);
            m_emerge = kEmergeTicks;
        }
        if ((tickCount % 20 == 1 || !m_heartChecked) && m_level->Blocks()) {
            m_heartChecked = true;
            m_realHeart = m_level->Blocks()->GetBlock(m_heart.x, m_heart.y, m_heart.z) == BlockID::ResonanceEngine;
        }

        // ── Emergence: up through the dais and the Heart's core ────────
        if (m_emerge < kEmergeTicks) {
            ++m_emerge;
            const double top = m_heart.y + 1.5;
            const double t = static_cast<double>(m_emerge) / kEmergeTicks;
            const double want = m_spawnY + (top - m_spawnY) * (t * t * (3.0 - 2.0 * t));
            velocity = glm::dvec3(0.0, want - position.y, 0.0);
            yRot += 6.0f * static_cast<float>(1.0 - t);
            yBodyRot = yRot;
            if (m_emerge == 1) PlaySound(kSoundEmerge, 6.0f, 0.7f);
            if (m_emerge >= kEmergeTicks) {
                velocity = glm::dvec3(0.0);
                BeginPhase(1);
            }
        } else {
            if (m_phase == 0) BeginPhase(1);
            // ── Phases by health ────────────────────────────────────────
            const int wanted = HealthPhase();
            if (wanted > m_phase) BeginPhase(wanted);

            if (m_transition > 0) --m_transition;
            if (m_stagger > 0) --m_stagger;
            if (m_conductTicks > 0) --m_conductTicks;

            LivingEntity* target = GetTarget();
            TickRings();
            TickRestore();
            TickHeartPulse();
            if (m_transition <= 0 && m_stagger <= 0) {
                TickChannel(target);
                if (m_channel <= 0 && !m_channelFlying) {
                    TickMelee(target);
                    if (m_windup <= 0) {
                        FireNotes(target);
                        Conduct();
                        if (m_phase >= 2 && --m_ringIn <= 0) {
                            StartRing();
                            m_ringIn = m_phase == 2 ? 160 : 120;
                        }
                    }
                }
            }
            ChooseMove();
        }

        // ── The two synced bytes ────────────────────────────────────────
        uint8_t anim = static_cast<uint8_t>(m_phase & 3);
        if (m_windup > 0 || m_conductTicks > 0 || m_transition > 0) anim |= 4;
        if (m_channel > 0) anim |= 8;
        if (m_stolen >= 0) anim |= 16;
        if (m_heartCharge > 0) anim |= 32;
        if (m_stagger > 0) anim |= 128;
        m_anim = anim;
        uint8_t variant = static_cast<uint8_t>(std::max(0, m_stolen) & 3);
        variant |= static_cast<uint8_t>((std::max(0, m_rotation) & 3) << 2);
        variant |= static_cast<uint8_t>(std::min(7, m_restore * 8 / kRestoreTicks) << 4);
        if (m_statues == 1) variant |= 128;
        m_variant = variant;
    }

    // ══ Damage and death ══════════════════════════════════════════════════

    bool TheUnsung::Hurt(MobDamageSource source, float amount, Entity* attacker) {
        if (source == MobDamageSource::Void) return Monster::Hurt(source, amount, attacker);
        if (m_level && !m_level->IsClientSide()) {
            // Emerging or re-forming between phases: untouchable.
            if (m_emerge < kEmergeTicks || m_transition > 0) {
                if (attacker && attacker->IsPlayer() && tickCount % 4 == 0) {
                    m_level->BroadcastEntityEvent(*this, kEventShieldHit);
                }
                return false;
            }
            if (!m_heartStrike) {
                if (m_stolen >= 0) {
                    amount *= kShieldFactor;
                    if (attacker && attacker->IsPlayer()) {
                        m_level->BroadcastEntityEvent(*this, kEventShieldHit);
                        PlaySound(kSoundShield, 1.5f, A::VoicePitch(static_cast<A::Voice>(m_stolen)));
                    }
                }
                if (m_stagger > 0) amount *= kStaggerFactor;
            }
        }
        const float before = GetHealth();
        const bool hurt = Monster::Hurt(source, amount, attacker);
        if (hurt && m_channel > 0 && m_level && !m_level->IsClientSide()) {
            m_channelDamage += std::max(0.0f, before - GetHealth());
            if (m_channelDamage >= kChannelBreak) {
                m_channel = 0;
                m_channelIn = 700;
                Stagger(60);
                m_level->BroadcastEntityEvent(*this, kEventChannelBroken);
                PlaySound(kSoundChannelBreak, 5.0f, 0.9f);
                TellPlayers("Its song breaks!", 64.0);
            }
        }
        return hurt;
    }

    void TheUnsung::TickDeath() {
        ++deathTime;
        const bool client = m_level && m_level->IsClientSide();
        if (client) {
            ClientEffects();
        } else if (m_level) {
            if (deathTime == 1) {
                // Sung to rest: every stolen voice goes home.
                RestoreVoice(true);
                m_rings.clear();
                PlaySound(kSoundRest, 8.0f, 1.0f);
                TellPlayers("The Unsung is sung to rest.", 96.0);
            }
        }
        // It rises as it dissolves.
        position.y += 0.04;
        if (deathTime >= kDeathTicks && m_level && !client && !IsRemoved()) {
            m_level->BroadcastEntityEvent(*this, 60);
            TriggerOnDeathMobEffects(RemovalReason::Killed);
            Remove(RemovalReason::Killed);
        }
    }

    void TheUnsung::DropCustomDeathLoot(EntityLevel& level) {
        // 3-5 resonant crystal (a block item — the Choir Mother's note); the
        // shards and the resonite are the loot table's. The Held Note is the
        // quest's reward and is given by the city (AurelithCities), once.
        const int n = 3 + level.Random().NextInt(3);
        level.SpawnItemDrop(position, ItemRegistry::FromBlock(BlockID::ResonantCrystal), n);
    }

    void TheUnsung::KillFromCommand() {
        if (m_level && !m_level->IsClientSide()) RestoreVoice(true);
        Monster::KillFromCommand();
    }

    void TheUnsung::CheckDespawn() {
        if (m_level && !m_level->IsClientSide() && m_stolen >= 0 &&
            m_level->GetDifficulty() == Difficulty::Peaceful) {
            RestoreVoice(true);
        }
        Monster::CheckDespawn();
    }

    // ══ The client's half ═════════════════════════════════════════════════

    void TheUnsung::HandleEntityEvent(uint8_t id) {
        if (!m_level) { Monster::HandleEntityEvent(id); return; }
        JavaRandom& rng = m_level->Random();
        const glm::dvec3 throat = position + glm::dvec3(0.0, kThroatHeight, 0.0);
        switch (id) {
            case kEventSonicRing:
                m_clientRings.push_back(ClientRing{ position, FloorBelow(position + glm::dvec3(0.0, 1.0, 0.0)), 0 });
                return;
            case kEventHeartCharge:
                m_clientHeartCharge = kHeartChargeTicks;
                return;
            case kEventHeartPulse:
                m_clientPulseAge = 0;
                m_clientHeartCharge = 0;
                return;
            case kEventRisen:
            case kEventPhaseShift:
                // The tell: a burst of dark smoke and a flare of motes.
                for (int i = 0; i < 40; ++i) {
                    const double a = rng.NextDouble() * kTwoPi;
                    const double s = 0.15 + rng.NextDouble() * 0.2;
                    m_level->AddParticle(ParticleKind::LargeSmoke, throat.x, throat.y - 1.0, throat.z,
                                         std::cos(a) * s, 0.05, std::sin(a) * s);
                    m_level->AddParticle(ParticleKind::HushMote, throat.x, throat.y, throat.z,
                                         std::cos(a) * s * 0.6, (rng.NextDouble() - 0.3) * 0.2,
                                         std::sin(a) * s * 0.6);
                }
                return;
            case kEventSteal:
            case kEventRestored: {
                const glm::vec3 c = VoiceRgb(StolenVoice());
                for (int i = 0; i < 30; ++i) {
                    m_level->AddColorParticle(ParticleKind::EntityEffect,
                                              throat.x + (rng.NextDouble() - 0.5) * 2.0,
                                              throat.y + (rng.NextDouble() - 0.5) * 2.0,
                                              throat.z + (rng.NextDouble() - 0.5) * 2.0,
                                              0.0, 0.05, 0.0, c.r, c.g, c.b, 1.0f);
                }
                return;
            }
            case kEventShieldHit: {
                const glm::vec3 c = IsShielded() ? VoiceRgb(StolenVoice()) : glm::vec3(0.9f, 0.9f, 1.0f);
                for (int i = 0; i < 8; ++i) {
                    m_level->AddColorParticle(ParticleKind::EntityEffect,
                                              position.x + (rng.NextDouble() - 0.5) * 1.8,
                                              position.y + 0.5 + rng.NextDouble() * 3.0,
                                              position.z + (rng.NextDouble() - 0.5) * 1.8,
                                              0.0, 0.0, 0.0, c.r, c.g, c.b, 1.0f);
                }
                return;
            }
            case kEventSwing: {
                // The baton's arc, a crescent of motes in front of it.
                const glm::dvec3 f = FacingXZ(yRot);
                const glm::dvec3 side(-f.z, 0.0, f.x);
                for (int i = 0; i <= 16; ++i) {
                    const double t = (i / 16.0 - 0.5) * 2.4;
                    const glm::dvec3 p = position + f * (2.6 * std::cos(t)) + side * (2.6 * std::sin(t))
                                       + glm::dvec3(0.0, 1.4 + 0.3 * std::sin(t * 2.0), 0.0);
                    m_level->AddParticle(ParticleKind::HushMote, p.x, p.y, p.z, f.x * 0.05, 0.0, f.z * 0.05);
                }
                return;
            }
            case kEventChannelBroken:
                for (int i = 0; i < 30; ++i) {
                    m_level->AddParticle(ParticleKind::WitchMagic, throat.x, throat.y, throat.z,
                                         (rng.NextDouble() - 0.5) * 0.6, rng.NextDouble() * 0.3,
                                         (rng.NextDouble() - 0.5) * 0.6);
                }
                return;
            case kEventSilence:
                for (int i = 0; i < 60; ++i) {
                    const double a = rng.NextDouble() * kTwoPi;
                    const double r = 4.0 + rng.NextDouble() * 26.0;
                    m_level->AddParticle(ParticleKind::LargeSmoke, position.x + std::cos(a) * r,
                                         FloorBelow(position + glm::dvec3(0.0, 1.0, 0.0)) + rng.NextDouble() * 2.0,
                                         position.z + std::sin(a) * r, 0.0, 0.02, 0.0);
                }
                return;
            default:
                Monster::HandleEntityEvent(id);
                return;
        }
    }

    bool TheUnsung::ClientFindArena() {
        if (m_clientArena) return true;
        if (!m_level || !m_level->Blocks() || m_clientScans >= 6) return false;
        if (--m_clientScanIn > 0) return false;
        m_clientScanIn = 40;
        ++m_clientScans;
        // The Heart is within its leash, a few blocks over its feet.
        const IBlockAccess& blocks = *m_level->Blocks();
        const glm::ivec3 at = BlockPosition();
        int best = INT_MAX;
        for (int dy = -6; dy <= 8; ++dy) {
            for (int dx = -30; dx <= 30; ++dx) {
                for (int dz = -30; dz <= 30; ++dz) {
                    const int d2 = dx * dx + dz * dz;
                    if (d2 >= best) continue;
                    if (blocks.GetBlock(at.x + dx, at.y + dy, at.z + dz) != BlockID::ResonanceEngine) continue;
                    best = d2;
                    m_clientHeart = at + glm::ivec3(dx, dy, dz);
                    m_clientArena = true;
                }
            }
        }
        return m_clientArena;
    }

    void TheUnsung::ClientEffects() {
        JavaRandom& rng = m_level->Random();
        const glm::dvec3 throat = position + glm::dvec3(0.0, kThroatHeight, 0.0);
        const bool arena = ClientFindArena();
        const int rotation = SyncedRotation();
        auto statueCrystal = [&](int voice) {
            const glm::ivec3 c = A::DesignToWorld(m_clientHeart, rotation,
                                                  A::StatueOffset(static_cast<A::Voice>(voice & 3)), 9);
            return glm::dvec3(c) + glm::dvec3(0.5);
        };

        // Dying: the four voices stream home to their statues and it rises
        // apart in motes.
        if (deathTime > 0) {
            for (int i = 0; i < 4; ++i) {
                m_level->AddParticle(ParticleKind::HushMote,
                                     position.x + (rng.NextDouble() - 0.5) * 1.6,
                                     position.y + rng.NextDouble() * 3.8,
                                     position.z + (rng.NextDouble() - 0.5) * 1.6,
                                     0.0, 0.05 + rng.NextDouble() * 0.05, 0.0);
            }
            if (arena && ArenaHasStatues()) {
                for (int v = 0; v < A::kVoiceCount; ++v) {
                    const glm::dvec3 to = statueCrystal(v);
                    const glm::vec3 c = VoiceRgb(v);
                    const double t = rng.NextDouble();
                    const glm::dvec3 p = throat + (to - throat) * t
                                       + glm::dvec3(0.0, std::sin(t * 3.14159) * 3.0, 0.0);
                    const glm::dvec3 v3 = (to - throat) * 0.02;
                    m_level->AddColorParticle(ParticleKind::EntityEffect, p.x, p.y, p.z,
                                              v3.x, v3.y, v3.z, c.r, c.g, c.b, 1.0f);
                }
            }
            return;
        }

        // Emerging: sculk breath from the dais, motes drawn up with it.
        if (IsEmerging()) {
            if (m_clientSawEmerging == false) { m_clientSawEmerging = true; m_clientEmergeAge = 0; }
            for (int i = 0; i < 3; ++i) {
                const double a = rng.NextDouble() * kTwoPi;
                const double r = 1.0 + rng.NextDouble() * 3.0;
                m_level->AddParticle(ParticleKind::LargeSmoke, position.x + std::cos(a) * r,
                                     position.y - 0.5, position.z + std::sin(a) * r, 0.0, 0.08, 0.0);
            }
            m_level->AddParticle(ParticleKind::HushMote, position.x, position.y + rng.NextDouble() * 3.0,
                                 position.z, (rng.NextDouble() - 0.5) * 0.1, 0.1, (rng.NextDouble() - 0.5) * 0.1);
        }
        if (m_clientSawEmerging) ++m_clientEmergeAge;

        // The stolen voice's tether: from the dark statue to its throat, in
        // the voice's colour, and the statue's restoration climbing its
        // robe as the player sings.
        if (IsShielded() && arena && ArenaHasStatues()) {
            const int voice = StolenVoice();
            const glm::dvec3 from = statueCrystal(voice);
            const glm::vec3 c = VoiceRgb(voice);
            for (int i = 0; i < 4; ++i) {
                const double t = rng.NextDouble();
                const glm::dvec3 p = from + (throat - from) * t;
                m_level->AddColorParticle(ParticleKind::EntityEffect, p.x, p.y, p.z,
                                          0.0, 0.0, 0.0, c.r, c.g, c.b, 1.0f);
            }
            if (rng.NextInt(3) == 0) {
                m_level->AddParticle(ParticleKind::Smoke, from.x, from.y + 0.6, from.z, 0.0, 0.02, 0.0);
            }
            const float progress = RestoreProgress();
            const int climb = static_cast<int>(progress * 6.0f);
            for (int i = 0; i < climb; ++i) {
                const double a = rng.NextDouble() * kTwoPi;
                const double h = rng.NextDouble() * 9.0 * progress;
                m_level->AddColorParticle(ParticleKind::EntityEffect, from.x + std::cos(a) * 1.6,
                                          from.y - 8.0 + h, from.z + std::sin(a) * 1.6,
                                          0.0, 0.04, 0.0, c.r, c.g, c.b, 1.0f);
            }
        }

        // Channelling: the Chord drained out of the rings into its throat.
        if (IsChannelling() && arena) {
            const glm::dvec3 rings = glm::dvec3(m_clientHeart) + glm::dvec3(0.5, 12.0, 0.5);
            for (int i = 0; i < 5; ++i) {
                const double a = rng.NextDouble() * kTwoPi;
                const glm::dvec3 p = rings + glm::dvec3(std::cos(a) * 3.0, (rng.NextDouble() - 0.5) * 2.0,
                                                        std::sin(a) * 3.0);
                const glm::dvec3 v3 = (throat - p) * 0.06;
                m_level->AddParticle(ParticleKind::WitchMagic, p.x, p.y, p.z, v3.x, v3.y, v3.z);
            }
        }

        // The Heart gathering its pulse: motes climbing the column to the
        // rings; then the pulse itself, a ring of light across the dais.
        if (arena && m_clientHeartCharge > 0) {
            --m_clientHeartCharge;
            const glm::dvec3 core = glm::dvec3(m_clientHeart) + glm::dvec3(0.5, 0.0, 0.5);
            for (int i = 0; i < 6; ++i) {
                const double a = rng.NextDouble() * kTwoPi;
                const double r = 1.0 + rng.NextDouble() * 1.5;
                m_level->AddParticle(ParticleKind::HushMote, core.x + std::cos(a) * r,
                                     core.y + rng.NextDouble() * 10.0, core.z + std::sin(a) * r,
                                     -std::cos(a) * 0.02, 0.18, -std::sin(a) * 0.02);
            }
        }
        if (arena && m_clientPulseAge >= 0) {
            const glm::dvec3 core = glm::dvec3(m_clientHeart) + glm::dvec3(0.5, 0.0, 0.5);
            const double r = 1.0 + m_clientPulseAge * 0.6;
            const double floorY = m_clientHeart.y - A::kStreetBelowHeart + 1.2;
            const int n = static_cast<int>(r * 6.0);
            for (int i = 0; i < n; ++i) {
                const double a = (i + rng.NextDouble()) * kTwoPi / n;
                m_level->AddParticle(ParticleKind::HushMote, core.x + std::cos(a) * r, floorY,
                                     core.z + std::sin(a) * r, 0.0, 0.05, 0.0);
            }
            if (++m_clientPulseAge > 16) m_clientPulseAge = -1;
        }

        // The sonic rings (the Choir Mother's shape, violet).
        for (ClientRing& ring : m_clientRings) {
            ++ring.age;
            const double r = 1.0 + ring.age * kRingSpeed;
            const int n = std::min(56, static_cast<int>(r * kTwoPi * 1.2));
            for (int i = 0; i < n; ++i) {
                const double a = (i + rng.NextDouble() * 0.5) * kTwoPi / n;
                m_level->AddColorParticle(ParticleKind::EntityEffect, ring.centre.x + r * std::cos(a),
                                          ring.baseY + 0.15, ring.centre.z + r * std::sin(a),
                                          0.0, 0.0, 0.0, 0.72f, 0.49f, 1.0f, 1.0f);
            }
        }
        m_clientRings.erase(std::remove_if(m_clientRings.begin(), m_clientRings.end(),
                                           [](const ClientRing& r) {
                                               return 1.0 + r.age * kRingSpeed > kRingMaxRadius;
                                           }),
                            m_clientRings.end());

        // Stagger: motes falling off it like struck sparks.
        if (IsStaggered()) {
            m_level->AddParticle(ParticleKind::HushMote, position.x + (rng.NextDouble() - 0.5) * 1.5,
                                 position.y + 1.0 + rng.NextDouble() * 2.5,
                                 position.z + (rng.NextDouble() - 0.5) * 1.5, 0.0, -0.05, 0.0);
        }
        // Always: its tendrils drip stolen light.
        if (rng.NextInt(2) == 0) {
            m_level->AddParticle(ParticleKind::HushMote, position.x + (rng.NextDouble() - 0.5) * 1.4,
                                 position.y - 0.2, position.z + (rng.NextDouble() - 0.5) * 1.4,
                                 0.0, -0.03, 0.0);
        }
    }

    void TheUnsung::Tick() {
        Monster::Tick();
        if (!m_level || !m_level->IsClientSide() || deathTime > 0) return;
        ClientEffects();
    }

} // namespace Game
