// File: src/client/entity/ClientMobManager.cpp
#include "client/entity/ClientMobManager.hpp"
#include "common/entity/mobs/Monsters.hpp"
#include "common/entity/mobs/Animals.hpp"
#include "common/entity/mobs/GenericMobs.hpp"
#include "common/core/Mth.hpp"
#include "common/core/Log.hpp"

#include <cmath>

namespace Client {

    std::unique_ptr<ClientMobManager> g_clientMobManager;

    namespace {

        // Flag bits shared with AddEntityS2CPacket / SetEntityDataS2CPacket.
        constexpr uint8_t kFlagBaby       = 0x01;
        constexpr uint8_t kFlagAggressive = 0x02;
        constexpr uint8_t kFlagOnFire     = 0x04;

        std::unique_ptr<Game::Mob> CreateMobOfType(Game::EntityTypeId type,
                                                   Game::EntityLevel* level) {
            switch (type) {
                case Game::EntityTypeId::Zombie:   return std::make_unique<Game::Zombie>(level);
                case Game::EntityTypeId::Skeleton: return std::make_unique<Game::Skeleton>(level);
                case Game::EntityTypeId::Creeper:  return std::make_unique<Game::Creeper>(level);
                case Game::EntityTypeId::Spider:   return std::make_unique<Game::Spider>(level);
                case Game::EntityTypeId::Cow:      return std::make_unique<Game::Cow>(level);
                case Game::EntityTypeId::Pig:      return std::make_unique<Game::Pig>(level);
                case Game::EntityTypeId::Sheep:    return std::make_unique<Game::Sheep>(level);
                case Game::EntityTypeId::Chicken:  return std::make_unique<Game::Chicken>(level);
                default: break;
            }
            // MUST mirror IntegratedServer::MakeMob's fallthrough. The server
            // spawns, tracks and sends every type in the def table; a client
            // that returns null here drops the AddEntityS2C on the floor and
            // the mob simply never appears — which is what an unhandled type
            // looked like from the player's side: right-clicking a spawn egg
            // did nothing at all, with no error anywhere.
            //
            // Registering goals client-side is fine and matches the eight
            // above: IsEffectiveAi() is false on the client, so the goal
            // selectors are built and then never ticked.
            return Game::MakeGenericMob(type, level);
        }

        void ApplyFlags(Game::Mob& mob, uint8_t flags) {
            mob.SetAggressive((flags & kFlagAggressive) != 0);
            mob.SetRemainingFireTicks((flags & kFlagOnFire) != 0 ? 20 : 0);

            // Baby state changes the hitbox and the model scale, so it is
            // carried on the wire rather than inferred.
            if (auto* zombie = dynamic_cast<Game::Zombie*>(&mob)) {
                zombie->SetBaby((flags & kFlagBaby) != 0);
            } else if (auto* ageable = dynamic_cast<Game::AgeableMob*>(&mob)) {
                const bool wantBaby = (flags & kFlagBaby) != 0;
                if (wantBaby != ageable->IsBaby()) {
                    ageable->SetAge(wantBaby ? Game::AgeableMob::kBabyStartAge : 0);
                }
            }
        }

    } // namespace

    ClientMob* ClientMobManager::Find(int32_t id) {
        const auto it = m_mobs.find(id);
        return it == m_mobs.end() ? nullptr : &it->second;
    }

    bool ClientMobManager::GetCodecBase(int32_t id, glm::dvec3& out) const {
        const auto it = m_codecBase.find(id);
        if (it == m_codecBase.end()) return false;
        out = it->second;
        return true;
    }

    void ClientMobManager::Spawn(int32_t id, uint16_t type, const glm::dvec3& pos,
                                 const glm::vec3& vel, float yRot, float xRot, float yHeadRot,
                                 float health, uint8_t flags, uint8_t variantData,
                                 uint8_t pose, uint8_t animState) {
        if (!Game::IsValidEntityType(type)) {
            Log::Warning("[ClientMobManager] Unknown entity type %u for id %d", type, id);
            return;
        }

        const auto typeId = static_cast<Game::EntityTypeId>(type);

        ClientMob* existing = Find(id);
        if (!existing) {
            std::unique_ptr<Game::Mob> mob = CreateMobOfType(typeId, &m_level);
            if (!mob) {
                // Only reachable for a type the server can build and this
                // client cannot — i.e. the two factories have drifted apart.
                // It used to return silently, which made the whole failure
                // invisible: the mob existed server-side, ticked, was tracked,
                // and never drew.
                Log::Warning("[ClientMobManager] No client mob for type '%s' (id %d)",
                             std::string(Game::GetEntityTypeInfo(typeId).slug).c_str(), id);
                return;
            }
            mob->SetId(id);

            ClientMob entry;
            entry.mob = std::move(mob);
            existing = &m_mobs.emplace(id, std::move(entry)).first->second;
        }

        Game::Mob& mob = *existing->mob;

        // A spawn packet is authoritative — snap, do not interpolate. This is
        // also the path a re-send takes when a client walks back into range,
        // and easing in from a stale position would look like a slide.
        mob.position = pos;
        mob.oldPosition = pos;
        mob.velocity = glm::dvec3(vel);
        mob.yRot = mob.yRotO = yRot;
        mob.xRot = mob.xRotO = xRot;
        mob.yHeadRot = mob.yHeadRotO = yHeadRot;
        mob.yBodyRot = mob.yBodyRotO = yRot;
        mob.SetHealth(health);
        ApplyFlags(mob, flags);

        if (auto* sheep = dynamic_cast<Game::Sheep*>(&mob)) sheep->SetWoolData(variantData);

        // Pose LAST of the synched fields: SetPose fires OnPoseUpdated, which
        // starts animation timers against the mob's tickCount — so everything
        // those timers read has to already be in place.
        mob.SetAnimStateByte(animState);
        mob.SetPose(Game::PoseById(pose));

        existing->interpSteps = 0;
        existing->renderPrevPosition = pos;
        existing->renderPrevYRot = yRot;
        existing->renderPrevXRot = xRot;
        existing->renderPrevYHeadRot = yHeadRot;
        existing->renderPrevYBodyRot = yRot;

        m_codecBase[id] = pos;
    }

    void ClientMobManager::MoveDelta(int32_t id, bool hasPos, const glm::dvec3& delta,
                                     bool hasRot, float yRot, float xRot, float yHeadRot,
                                     bool onGround) {
        ClientMob* entry = Find(id);
        if (!entry) return;

        if (hasPos) {
            // The delta is relative to the SERVER's last-sent position, not to
            // wherever local simulation has drifted to — so it composes onto the
            // codec base and the result becomes the new interpolation target.
            auto it = m_codecBase.find(id);
            const glm::dvec3 base = (it == m_codecBase.end()) ? entry->mob->position : it->second;
            const glm::dvec3 target = base + delta;
            m_codecBase[id] = target;

            const glm::dvec3 d = target - entry->mob->position;
            if (d.x * d.x + d.y * d.y + d.z * d.z > kSnapDistanceSq) {
                entry->mob->position = target;
                entry->interpSteps = 0;
            } else {
                entry->targetPosition = target;
                entry->interpSteps = kInterpSteps;
            }
        }

        if (hasRot) {
            entry->targetYRot = yRot;
            entry->targetXRot = xRot;
            entry->targetYHeadRot = yHeadRot;
            if (entry->interpSteps == 0) entry->interpSteps = kInterpSteps;
        }

        entry->mob->onGround = onGround;
    }

    void ClientMobManager::Teleport(int32_t id, const glm::dvec3& pos, const glm::vec3& vel,
                                    float yRot, float xRot, float yHeadRot, bool onGround) {
        ClientMob* entry = Find(id);
        if (!entry) return;

        m_codecBase[id] = pos;

        const glm::dvec3 d = pos - entry->mob->position;
        if (d.x * d.x + d.y * d.y + d.z * d.z > kSnapDistanceSq) {
            entry->mob->position = pos;
            entry->interpSteps = 0;
        } else {
            entry->targetPosition = pos;
            entry->interpSteps = kInterpSteps;
        }

        entry->targetYRot = yRot;
        entry->targetXRot = xRot;
        entry->targetYHeadRot = yHeadRot;
        if (entry->interpSteps == 0) entry->interpSteps = kInterpSteps;

        entry->mob->velocity = glm::dvec3(vel);
        entry->mob->onGround = onGround;
    }

    void ClientMobManager::SetMotion(int32_t id, const glm::vec3& vel) {
        if (ClientMob* entry = Find(id)) entry->mob->velocity = glm::dvec3(vel);
    }

    void ClientMobManager::SetData(int32_t id, float health, uint8_t flags, uint8_t variantData,
                                   uint8_t hurtTime, uint8_t deathTime, uint8_t swellDir,
                                   uint8_t swell, uint8_t pose, uint8_t animState) {
        ClientMob* entry = Find(id);
        if (!entry) return;

        Game::Mob& mob = *entry->mob;
        mob.SetHealth(health);
        ApplyFlags(mob, flags);

        // hurtTime and deathTime are SET rather than max'd: the server owns
        // them, and letting the client keep a longer local value would leave a
        // mob flashing after the server considered the hit finished.
        mob.hurtTime = hurtTime;
        mob.deathTime = deathTime;

        if (auto* sheep = dynamic_cast<Game::Sheep*>(&mob)) sheep->SetWoolData(variantData);

        if (auto* creeper = dynamic_cast<Game::Creeper*>(&mob)) {
            creeper->SetSwellDir(swellDir ? 1 : -1);
            entry->oldSwell = entry->swell;
            entry->swell = swell;
        }

        // MC onSyncedDataUpdated(DATA_POSE). Both setters are idempotent, so
        // the periodic resend of an unchanged pose does not restart a clip.
        mob.SetAnimStateByte(animState);
        mob.SetPose(Game::PoseById(pose));
    }

    void ClientMobManager::HandleEvent(int32_t id, uint8_t event) {
        ClientMob* entry = Find(id);
        if (!entry) return;

        switch (event) {
            case 3:
                // Death: start the fall-over animation. The removal packet
                // arrives 20 ticks later, which is exactly the animation length.
                entry->mob->deathTime = 1;
                entry->mob->SetHealth(0.0f);
                break;
            case 60:
                // Poof — the removal packet does the actual erase, so there is
                // nothing to do here until particles exist.
                break;
            default:
                // MC Entity.handleEntityEvent — everything else belongs to the
                // ENTITY, not to this manager. The sheep's graze (10) starts
                // its own 40-tick animation there; breeding hearts (18) will
                // land the same way once particles exist.
                entry->mob->HandleEntityEvent(event);
                break;
        }
    }

    void ClientMobManager::Remove(int32_t id) {
        m_mobs.erase(id);
        m_codecBase.erase(id);
    }

    void ClientMobManager::Clear() {
        m_mobs.clear();
        m_codecBase.clear();
    }

    void ClientMobManager::Tick() {
        for (auto& [id, entry] : m_mobs) {
            Game::Mob& mob = *entry.mob;

            // Snapshot BEFORE anything moves — the renderer interpolates
            // between this and the post-tick state.
            entry.renderPrevPosition = mob.position;
            entry.renderPrevYRot = mob.yRot;
            entry.renderPrevXRot = mob.xRot;
            entry.renderPrevYHeadRot = mob.yHeadRot;
            entry.renderPrevYBodyRot = mob.yBodyRot;

            // Order matters and matches MC's tickNonPassenger: oldPosition is
            // captured BEFORE the interpolation correction, so the walk
            // animation counts the corrected movement as distance travelled —
            // which is what stops a mob being pulled forward by a correction
            // from appearing to moonwalk.
            mob.SetOldPosAndRot();
            ++mob.tickCount;

            // ── Correction first, then simulation ──────────────────────────
            //
            // MC's InterpolationHandler runs at the top of aiStep, before
            // travel(). Doing it the other way round would let this tick's
            // gravity be applied to a position the server has already
            // superseded, which shows up as a persistent vertical wobble.
            if (entry.interpSteps > 0) {
                const double alpha = 1.0 / static_cast<double>(entry.interpSteps);
                mob.position += (entry.targetPosition - mob.position) * alpha;
                mob.yRot     = Game::Mth::RotLerp(static_cast<float>(alpha), mob.yRot, entry.targetYRot);
                mob.xRot     = Game::Mth::Lerp(static_cast<float>(alpha), mob.xRot, entry.targetXRot);
                mob.yHeadRot = Game::Mth::RotLerp(static_cast<float>(alpha), mob.yHeadRot,
                                                  entry.targetYHeadRot);
                --entry.interpSteps;
            }

            mob.Tick();
        }
    }

} // namespace Client
