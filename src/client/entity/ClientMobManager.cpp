// File: src/client/entity/ClientMobManager.cpp
#include "client/entity/ClientMobManager.hpp"
#include "common/core/TickParallel.hpp"

#include <atomic>
#include "common/entity/FallingBlockEntity.hpp"
#include "common/entity/PrimedTnt.hpp"
#include "common/entity/mobs/Monsters.hpp"
#include "common/entity/mobs/Animals.hpp"
#include "common/entity/mobs/GenericMobs.hpp"
#include "common/entity/projectile/Arrow.hpp"
#include "common/entity/projectile/EyeOfEnder.hpp"
#include "common/entity/projectile/ThrowableProjectile.hpp"
#include "common/entity/projectile/HurtingProjectile.hpp"
#include "common/entity/projectile/ShulkerBullet.hpp"
#include "common/entity/projectile/LlamaSpit.hpp"
#include "common/entity/projectile/ThrownTrident.hpp"
#include "common/entity/projectile/EvokerFangs.hpp"
#include "common/entity/projectile/AreaEffectCloud.hpp"
#include "common/entity/mobs/Slime.hpp"
#include "common/entity/mobs/Fish.hpp"
#include "common/entity/mobs/AnimatedMobs.hpp"
#include "common/core/Mth.hpp"
#include "common/core/Profiling_Tracy.hpp"
#include "common/core/Log.hpp"

#include <algorithm>
#include <cmath>

namespace Client {

    std::unique_ptr<ClientMobManager> g_clientMobManager;

    // Declared in common/network/packets/S2CPackets.hpp (the packet impls call
    // it from apply(), which only ever runs on the client main thread) — the
    // narrow seam that routes the appended riding field here without widening
    // every ClientPacketHandler signature.
    void ApplyMobVehicleLink(int32_t passengerId, int32_t vehicleId) {
        if (g_clientMobManager) g_clientMobManager->SetVehicle(passengerId, vehicleId);
    }

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
                case Game::EntityTypeId::Arrow:    return std::make_unique<Game::Arrow>(level);
                // The client copy runs the same flight code as the server's —
                // its target is never synced, so it simply drifts on its last
                // velocity between position packets, which is what every other
                // projectile here does too.
                case Game::EntityTypeId::EyeOfEnder:
                    return std::make_unique<Game::EyeOfEnder>(level);
                case Game::EntityTypeId::Enderman: return std::make_unique<Game::Enderman>(level);
                case Game::EntityTypeId::Husk:     return std::make_unique<Game::Husk>(level);
                case Game::EntityTypeId::Drowned:  return std::make_unique<Game::Drowned>(level);
                case Game::EntityTypeId::ZombieVillager:
                    return std::make_unique<Game::ZombieVillager>(level);
                case Game::EntityTypeId::ZombifiedPiglin:
                    return std::make_unique<Game::ZombifiedPiglin>(level);
                case Game::EntityTypeId::Slime:
                    return Game::Slime::Make(level);
                case Game::EntityTypeId::MagmaCube:
                    return std::make_unique<Game::MagmaCube>(level);
                case Game::EntityTypeId::Cod:
                case Game::EntityTypeId::Salmon:
                case Game::EntityTypeId::TropicalFish:
                    return std::make_unique<Game::SchoolingFish>(type, level);
                case Game::EntityTypeId::Pufferfish:
                    return std::make_unique<Game::Pufferfish>(level);
                case Game::EntityTypeId::Squid:
                case Game::EntityTypeId::GlowSquid:
                    return std::make_unique<Game::Squid>(type, level);
                case Game::EntityTypeId::Guardian:
                    return std::make_unique<Game::Guardian>(level);
                case Game::EntityTypeId::ElderGuardian:
                    return std::make_unique<Game::ElderGuardian>(level);
                case Game::EntityTypeId::Parrot:
                    return std::make_unique<Game::Parrot>(level);
                case Game::EntityTypeId::IronGolem:
                    return std::make_unique<Game::IronGolem>(level);
                case Game::EntityTypeId::Ravager:
                    return std::make_unique<Game::Ravager>(level);
                case Game::EntityTypeId::Rabbit:
                    return std::make_unique<Game::Rabbit>(level);
                case Game::EntityTypeId::PolarBear:
                    return std::make_unique<Game::PolarBear>(level);
                case Game::EntityTypeId::Fox:
                    return std::make_unique<Game::Fox>(level);
                case Game::EntityTypeId::Turtle:
                    return std::make_unique<Game::Turtle>(level);
                case Game::EntityTypeId::Panda:
                    return std::make_unique<Game::Panda>(level);
                case Game::EntityTypeId::Cat:
                    return std::make_unique<Game::Cat>(level);
                case Game::EntityTypeId::Ocelot:
                    return std::make_unique<Game::Ocelot>(level);
                case Game::EntityTypeId::Dolphin:
                    return std::make_unique<Game::Dolphin>(level);
                case Game::EntityTypeId::HappyGhast:
                    return std::make_unique<Game::HappyGhast>(level);
                case Game::EntityTypeId::Horse:
                    return std::make_unique<Game::Horse>(level);
                case Game::EntityTypeId::Donkey:
                    return std::make_unique<Game::Donkey>(level);
                case Game::EntityTypeId::Mule:
                    return std::make_unique<Game::Mule>(level);
                case Game::EntityTypeId::SkeletonHorse:
                    return std::make_unique<Game::SkeletonHorse>(level);
                case Game::EntityTypeId::ZombieHorse:
                    return std::make_unique<Game::ZombieHorse>(level);
                case Game::EntityTypeId::Blaze:
                    return std::make_unique<Game::Blaze>(level);
                case Game::EntityTypeId::Ghast:
                    return std::make_unique<Game::Ghast>(level);
                case Game::EntityTypeId::Phantom:
                    return std::make_unique<Game::Phantom>(level);
                case Game::EntityTypeId::Vex:
                    return std::make_unique<Game::Vex>(level);
                case Game::EntityTypeId::Evoker:
                    return std::make_unique<Game::Evoker>(level);
                case Game::EntityTypeId::Illusioner:
                    return std::make_unique<Game::Illusioner>(level);
                case Game::EntityTypeId::Vindicator:
                    return std::make_unique<Game::Vindicator>(level);
                case Game::EntityTypeId::Wither:
                    return std::make_unique<Game::Wither>(level);
                case Game::EntityTypeId::EnderDragon:
                    return std::make_unique<Game::EnderDragon>(level);
                case Game::EntityTypeId::Strider:
                    return std::make_unique<Game::Strider>(level);
                case Game::EntityTypeId::SnowGolem:
                    return std::make_unique<Game::SnowGolem>(level);
                case Game::EntityTypeId::Witch:
                    return std::make_unique<Game::Witch>(level);
                case Game::EntityTypeId::Shulker:
                    return std::make_unique<Game::Shulker>(level);
                case Game::EntityTypeId::Llama:
                    return std::make_unique<Game::Llama>(level);
                case Game::EntityTypeId::TraderLlama:
                    return std::make_unique<Game::TraderLlama>(level);
                // ── Projectiles — MUST mirror IntegratedServer::MakeMob ────
                case Game::EntityTypeId::Snowball:
                    return std::make_unique<Game::Snowball>(level);
                case Game::EntityTypeId::Egg:
                    return std::make_unique<Game::ThrownEgg>(level);
                case Game::EntityTypeId::SplashPotion:
                    return std::make_unique<Game::ThrownSplashPotion>(level);
                case Game::EntityTypeId::SmallFireball:
                    return std::make_unique<Game::SmallFireball>(level);
                case Game::EntityTypeId::Fireball:
                    return std::make_unique<Game::LargeFireball>(level);
                case Game::EntityTypeId::DragonFireball:
                    return std::make_unique<Game::DragonFireball>(level);
                // Renders as NOTHING by design: MC draws the cloud purely as
                // particles, and no particle system exists — MobRenderer's
                // GetModelFor returns null for it (no model, no MobDef row)
                // and skips it. The entity still ticks so a late-joining
                // client keeps consistent ids.
                case Game::EntityTypeId::AreaEffectCloud:
                    return std::make_unique<Game::AreaEffectCloud>(level);
                case Game::EntityTypeId::WitherSkull:
                    return std::make_unique<Game::WitherSkull>(level);
                case Game::EntityTypeId::EvokerFangs:
                    return std::make_unique<Game::EvokerFangs>(level);
                case Game::EntityTypeId::ShulkerBullet:
                    return std::make_unique<Game::ShulkerBullet>(level);
                case Game::EntityTypeId::LlamaSpit:
                    return std::make_unique<Game::LlamaSpit>(level);
                case Game::EntityTypeId::Trident:
                    return std::make_unique<Game::ThrownTrident>(level);
                case Game::EntityTypeId::WindCharge:
                    return std::make_unique<Game::WindCharge>(level);
                case Game::EntityTypeId::BreezeWindCharge:
                    return std::make_unique<Game::BreezeWindCharge>(level);
                // ── Block-shaped entities — MUST mirror the client factory ──
                // Neither is a Mob in MC; both ride this pipeline for the same
                // reason the projectiles do (see FallingBlockEntity.hpp).
                case Game::EntityTypeId::FallingBlock:
                    return std::make_unique<Game::FallingBlockEntity>(level);
                case Game::EntityTypeId::Tnt:
                    return std::make_unique<Game::PrimedTnt>(level);
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
                                 uint8_t pose, uint8_t animState,
                                 uint32_t blockStateRaw) {
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
            existing->listIndex = m_mobList.size();
            m_mobList.push_back(existing);
            existing->selfId = id;
            ModelListAdd(*existing);
            // The proxy is filled after the spawn data below is applied, at
            // the end of this function.
            m_blockProxies.emplace_back();
        }

        Game::Mob& mob = *existing->mob;

        // A spawn packet is authoritative — snap, do not interpolate. This is
        // also the path a re-send takes when a client walks back into range,
        // and easing in from a stale position would look like a slide.
        mob.position = pos;
        mob.oldPosition = pos;
        mob.velocity = glm::dvec3(vel);
        mob.physicsParked = false;
        mob.yRot = mob.yRotO = yRot;
        mob.xRot = mob.xRotO = xRot;
        mob.yHeadRot = mob.yHeadRotO = yHeadRot;
        mob.yBodyRot = mob.yBodyRotO = yRot;
        mob.SetHealth(health);
        ApplyFlags(mob, flags);

        mob.SetVariantByte(variantData);

        // Pose LAST of the synched fields: SetPose fires OnPoseUpdated, which
        // starts animation timers against the mob's tickCount — so everything
        // those timers read has to already be in place.
        mob.SetAnimStateByte(animState);
        mob.SetPose(Game::PoseById(pose));

        // The block a block-shaped entity carries — AddEntityS2C's per-type
        // data int. Applied after the synched fields because it is a spawn-time
        // constant, not a mirrored one: neither entity's block ever changes.
        //
        // Zero means "not carrying anything", so a type that ignores it is
        // untouched. A falling block needs this on the very FIRST frame:
        // FallingBlockEntity's own default state is Air, which draws nothing at
        // all, so a missed value is an INVISIBLE entity rather than a wrong one.
        if (blockStateRaw != 0) {
            const Game::BlockState carried = Game::BlockState::FromRawId(blockStateRaw);
            // Enum, not RTTI — both are leaf types, so this answers the same
            // question, and Spawn runs once per entity: a detonation that
            // primes a hundred thousand TNT pays this a hundred thousand times
            // in the same tick.
            if (mob.GetType() == Game::EntityTypeId::FallingBlock) {
                auto* falling = static_cast<Game::FallingBlockEntity*>(&mob);
                falling->SetCarriedState(carried);
                falling->SetStartPos(mob.BlockPosition());
            } else if (mob.GetType() == Game::EntityTypeId::Tnt) {
                static_cast<Game::PrimedTnt*>(&mob)->SetCarriedState(carried);
            }
        }

        existing->interpSteps = 0;
        existing->renderPrevPosition = pos;
        existing->renderPrevYRot = yRot;
        existing->renderPrevXRot = xRot;
        existing->renderPrevYHeadRot = yHeadRot;
        existing->renderPrevYBodyRot = yRot;

        m_codecBase[id] = pos;
        // Renderer summary for this slot — see BlockEntityProxy. Written
        // last so it reflects the spawn packet's position and state.
        if (existing && existing->listIndex < m_blockProxies.size()) {
            FillProxy(m_blockProxies[existing->listIndex], *existing);
        }
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
            // The server moved it; whatever rest the local sim parked into
            // is stale.
            entry->mob->physicsParked = false;
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
        entry->mob->physicsParked = false;
        entry->mob->onGround = onGround;
    }

    void ClientMobManager::SetMotion(int32_t id, const glm::vec3& vel) {
        if (ClientMob* entry = Find(id)) {
            entry->mob->velocity = glm::dvec3(vel);
            entry->mob->physicsParked = false;
        }
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
        //
        // A RISING hurtTime is this port's sight of MC's damage packet, whose
        // client handler (LivingEntity.handleDamageEvent) spikes the walk
        // animation to 1.5 — the limb flinch a standing mob shows on every
        // hit, distinct from the red flash.
        if (hurtTime > mob.hurtTime) {
            mob.walkAnimation.SetSpeed(1.5f);
        }
        mob.hurtTime = hurtTime;
        mob.deathTime = deathTime;

        mob.SetVariantByte(variantData);

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
            default:
                // MC Entity.handleEntityEvent — everything else belongs to the
                // ENTITY, not to this manager. The sheep's graze (10) starts
                // its own 40-tick animation there; 60 (poof) spawns
                // LivingEntity::MakePoofParticles; breeding hearts (18) land
                // in Animal::HandleEntityEvent. The removal packet that
                // follows a poof still does the actual erase.
                entry->mob->HandleEntityEvent(event);
                break;
        }
    }

    void ClientMobManager::SetVehicle(int32_t passengerId, int32_t vehicleId) {
        // Unknown passenger: nothing to link (its AddEntity was dropped or it
        // was already removed). The relation is re-announced with the rider,
        // so nothing is lost.
        if (ClientMob* entry = Find(passengerId)) {
            entry->wantedVehicleId = vehicleId;
        }
    }

    void ClientMobManager::Remove(int32_t id) {
        // This path never runs Entity::Remove (the mob is erased outright), so
        // do what Remove() would have: unlink riders and seat before the
        // pointers they hold go stale. Riders keep their wantedVehicleId; the
        // server's next data packet for them settles the truth.
        const auto it = m_mobs.find(id);
        if (it != m_mobs.end() && it->second.mob) {
            it->second.mob->EjectPassengers();
            it->second.mob->StopRiding();
        }
        if (it != m_mobs.end()) {
            ModelListRemove(it->second);
            // Keep the dense list honest: swap-pop the slot this entry held.
            const size_t idx = it->second.listIndex;
            if (idx < m_mobList.size() && m_mobList[idx] == &it->second) {
                ListSwapPop(idx);
            } else {
                // Should not happen; rebuild rather than iterate a stale slot.
                m_mobList.clear();
                m_modelMobList.clear();
                m_blockProxies.clear();
                for (auto& [mid, entry] : m_mobs) {
                    if (mid == id) continue;
                    entry.listIndex = m_mobList.size();
                    m_mobList.push_back(&entry);
                    entry.modelListIndex = static_cast<size_t>(-1);
                    ModelListAdd(entry);
                    m_blockProxies.emplace_back();
                    FillProxy(m_blockProxies.back(), entry);
                }
            }
        }
        m_mobs.erase(id);
        m_codecBase.erase(id);
    }

    void ClientMobManager::ListSwapPop(size_t idx) {
        m_mobList[idx] = m_mobList.back();
        m_mobList[idx]->listIndex = idx;
        m_mobList.pop_back();
        if (idx < m_blockProxies.size()) {
            m_blockProxies[idx] = m_blockProxies.back();
            m_blockProxies.pop_back();
        }
    }

    void ClientMobManager::FillProxy(BlockEntityProxy& proxy, const ClientMob& entry) {
        const Game::Mob* mob = entry.mob.get();
        if (!mob) { proxy.drawable = 0; return; }
        const Game::EntityTypeId type = mob->GetType();
        if (type == Game::EntityTypeId::FallingBlock) {
            proxy.stateRaw = static_cast<const Game::FallingBlockEntity*>(mob)->CarriedState().RawId();
            proxy.fuse = 0;
        } else if (type == Game::EntityTypeId::Tnt) {
            const auto* tnt = static_cast<const Game::PrimedTnt*>(mob);
            proxy.stateRaw = tnt->CarriedState().RawId();
            proxy.fuse = tnt->GetFuse();
        } else {
            proxy.drawable = 0;
            return;
        }
        proxy.prevPos  = entry.renderPrevPosition;
        proxy.pos      = mob->position;
        proxy.half     = mob->HalfExtents();
        proxy.type     = static_cast<uint8_t>(type);
        proxy.onGround = mob->onGround ? 1 : 0;
        proxy.drawable = mob->IsRemoved() ? 0 : 1;
    }

    void ClientMobManager::ModelListAdd(ClientMob& entry) {
        if (!entry.mob) return;
        const Game::EntityTypeId type = entry.mob->GetType();
        if (type == Game::EntityTypeId::Tnt || type == Game::EntityTypeId::FallingBlock) return;
        entry.modelListIndex = m_modelMobList.size();
        m_modelMobList.push_back(&entry);
    }

    void ClientMobManager::ModelListRemove(ClientMob& entry) {
        const size_t idx = entry.modelListIndex;
        if (idx == static_cast<size_t>(-1)) return;
        if (idx < m_modelMobList.size() && m_modelMobList[idx] == &entry) {
            m_modelMobList[idx] = m_modelMobList.back();
            m_modelMobList[idx]->modelListIndex = idx;
            m_modelMobList.pop_back();
        }
        entry.modelListIndex = static_cast<size_t>(-1);
    }

    void ClientMobManager::Clear() {
        m_mobs.clear();
        m_codecBase.clear();
        m_mobList.clear();
        m_modelMobList.clear();
        m_blockProxies.clear();
        m_serialScratch.clear();
        // Otherwise a leaving-world clear leaves ids from the old level for the
        // next one's first sweep to look up.
        m_removedThisTick.clear();
    }

    void ClientMobManager::Tick() {
        PROFILE_ZONE_N("ClientMobTick");
        PROFILE_PLOT("ClientMob/Count", static_cast<int64_t>(m_mobs.size()));

        // ── Pass 1: the dense list, in parallel where it can be ────────────
        //
        // m_mobList is maintained incrementally (Spawn/Remove/sweep), so this
        // never walks the million-node map. Each slot takes its renderPrev
        // snapshot first — before anything moves, which is the invariant the
        // renderer's interpolation needs — and then a plain primed TNT (no
        // riding links either way, not removed) runs its ENTIRE tick right
        // here in the parallel pass: interpolation correction, LOD choice,
        // physics. Anything else — every ordinary mob, every rider or
        // vehicle, anything already removed — is parked in m_serialScratch
        // at its own slot (deterministic order) for the serial pass below.
        m_removedThisTick.clear();
        m_pickCandidates.clear();
        const Game::IBlockAccess* blocks = m_level.Blocks();
        const size_t n = m_mobList.size();
        m_serialScratch.assign(n, nullptr);
        m_blockProxies.resize(n);
        std::atomic<size_t> lodUsed{0};
        {
            PROFILE_ZONE_N("ClientMob.Simulate");
            const auto pass = [&](size_t i) {
                ClientMob& entry = *m_mobList[i];
                Game::Mob& mob = *entry.mob;

                // A parked TNT with no pending correction has not moved and
                // will not move: its renderPrev fields and oldPosition already
                // equal its position from the tick it parked, so the ~100
                // bytes of snapshot writes per entity are skipped and only the
                // fuse advances. At a million settled TNT those writes were a
                // fifth of the whole client tick.
                if (mob.physicsParked && entry.interpSteps == 0 &&
                    mob.GetType() == Game::EntityTypeId::Tnt &&
                    entry.wantedVehicleId < 0 && !mob.IsVehicle() &&
                    !mob.IsPassenger() && !mob.IsRemoved()) {
                    // Fuse already held at zero: the tick is a provable no-op
                    // on the client (the hold returns before the smoke), so
                    // skip the virtual call too and leave only this branch's
                    // pointer chases.
                    if (mob.GetAnimStateByte() == 0) return;
                    ++mob.tickCount;
                    mob.Tick();
                    // Parked: nothing moved, only the fuse advanced.
                    m_blockProxies[i].fuse = static_cast<const Game::PrimedTnt&>(mob).GetFuse();
                    return;
                }

                entry.renderPrevPosition = mob.position;
                entry.renderPrevYRot = mob.yRot;
                entry.renderPrevXRot = mob.xRot;
                entry.renderPrevYHeadRot = mob.yHeadRot;
                entry.renderPrevYBodyRot = mob.yBodyRot;

                // A falling block takes the same parallel path as TNT, for
                // the same reasons: no goals, no riding, a tick that reads the
                // world and writes only itself (MoveApproximate on the client),
                // and never a pick candidate (IsPickable is false). It has no
                // physics LOD — its airborne move is one overlap test — and it
                // never parks. A collapsing sand pyramid put 176k of these
                // through the serial pass at 11 ms a tick.
                const Game::EntityTypeId entityType = mob.GetType();
                if ((entityType == Game::EntityTypeId::Tnt ||
                     entityType == Game::EntityTypeId::FallingBlock) &&
                    entry.wantedVehicleId < 0 && !mob.IsVehicle() &&
                    !mob.IsPassenger() && !mob.IsRemoved()) {
                    // Order as the serial path: oldPosition BEFORE the
                    // correction, correction BEFORE the physics.
                    mob.SetOldPosAndRot();
                    ++mob.tickCount;
                    if (entry.interpSteps > 0) {
                        const double alpha = 1.0 / static_cast<double>(entry.interpSteps);
                        mob.position += (entry.targetPosition - mob.position) * alpha;
                        mob.yRot     = Game::Mth::RotLerp(static_cast<float>(alpha), mob.yRot, entry.targetYRot);
                        mob.xRot     = Game::Mth::Lerp(static_cast<float>(alpha), mob.xRot, entry.targetXRot);
                        mob.yHeadRot = Game::Mth::RotLerp(static_cast<float>(alpha), mob.yHeadRot,
                                                          entry.targetYHeadRot);
                        --entry.interpSteps;
                    }
                    // Physics detail LOD — full local simulation near the
                    // player, within a global budget claimed atomically; the
                    // rest ride their server packets plus the interpolation
                    // above. See Entity::clientPhysicsLod.
                    if (entityType == Game::EntityTypeId::Tnt) {
                        constexpr double kPhysicsLodDistSq = 48.0 * 48.0;
                        constexpr size_t kPhysicsLodBudget = 32768;
                        const glm::dvec3 dLod = mob.position - m_pickOrigin;
                        if (glm::dot(dLod, dLod) <= kPhysicsLodDistSq &&
                            lodUsed.fetch_add(1, std::memory_order_relaxed) < kPhysicsLodBudget) {
                            mob.clientPhysicsLod = false;
                        } else {
                            mob.clientPhysicsLod = true;
                        }
                    }
                    mob.Tick();
                    FillProxy(m_blockProxies[i], entry);
                    return;
                }
                m_serialScratch[i] = &entry;
            };
            if (n >= 2048 && Core::ParallelWidth() > 1) {
                Core::ParallelFor(n, 256, pass);
            } else {
                for (size_t i = 0; i < n; ++i) pass(i);
            }
        }

        // ── Pass 2: everything else, serial, in list order ─────────────────
        // The original single-loop body, applied to the (comparatively few)
        // entries the parallel pass could not take: ordinary mobs, riders and
        // vehicles, TNT with riding links, and removed entities. Vehicle
        // reconciliation happens here, before the owner's own tick, exactly
        // as the old fused Prepare pass did; TNT never ride, so their having
        // already ticked above cannot reorder a passenger chain.
        {
            PROFILE_ZONE_N("ClientMob.Prepare");
            int  memoCx = 0, memoCz = 0;
            bool memoValid = false, memoLoaded = false;

            for (size_t si = 0; si < n; ++si) {
                if (!m_serialScratch[si]) continue;
                ClientMob& entry = *m_serialScratch[si];
                Game::Mob& mob = *entry.mob;
                const int32_t id = entry.selfId;

                const int32_t actual = mob.GetVehicle() ? mob.GetVehicle()->GetId() : -1;
                if (entry.wantedVehicleId != actual) {
                    if (entry.wantedVehicleId < 0) {
                        mob.StopRiding();
                    } else if (ClientMob* vehicle = Find(entry.wantedVehicleId)) {
                        mob.StartRiding(*vehicle->mob, /*force=*/true);
                    }
                    // else: vehicle not tracked yet — try again next tick.
                }

                if (mob.IsRemoved()) {
                    m_removedThisTick.push_back(id);
                    m_blockProxies[si].drawable = 0;
                    continue;
                }
                // Whatever this entry does below, its proxy is refreshed at
                // the end of its turn (the early continues included).
                struct ProxyRefresh {
                    BlockEntityProxy& p; const ClientMob& e;
                    ~ProxyRefresh() { FillProxy(p, e); }
                } refreshProxy{m_blockProxies[si], entry};

                // Pick candidates: the picker iterates these few dozen nearby
                // non-TNT mobs instead of every entity, every frame.
                if (mob.GetType() != Game::EntityTypeId::Tnt) {
                    const glm::dvec3 dp = mob.position - m_pickOrigin;
                    if (glm::dot(dp, dp) < 48.0 * 48.0) m_pickCandidates.push_back(id);
                }

                if (mob.IsPassenger()) continue;

                // Not while the ground under it has not arrived — see the
                // item/orb managers, which gate on exactly this.
                if (blocks) {
                    const int cx = static_cast<int>(std::floor(mob.position.x / 16.0));
                    const int cz = static_cast<int>(std::floor(mob.position.z / 16.0));
                    if (!memoValid || cx != memoCx || cz != memoCz) {
                        memoCx = cx; memoCz = cz; memoValid = true;
                        memoLoaded = blocks->IsChunkLoaded(cx, cz);
                    }
                    if (!memoLoaded) continue;
                }

                mob.SetOldPosAndRot();
                ++mob.tickCount;

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

                if (mob.IsOnFire()) mob.SetRemainingFireTicks(20);

                entry.swimAmountO = entry.swimAmount;
                if (mob.GetType() == Game::EntityTypeId::Drowned) {
                    const bool visuallySwimming = mob.IsInWater() && !mob.onGround;
                    entry.swimAmount = visuallySwimming
                        ? std::min(1.0f, entry.swimAmount + 0.09f)
                        : std::max(0.0f, entry.swimAmount - 0.09f);
                }

                if (mob.IsVehicle()) TickPassengerChain(mob);

                if (mob.IsRemoved()) m_removedThisTick.push_back(id);
            }
        }
        // ── Pass 3: sweep entities that discarded THEMSELVES ───────────────
        // Driven by the ids collected above; ids can repeat or name a mob
        // Remove() already erased, so the lookup is the guard.
        if (!m_removedThisTick.empty()) {
            PROFILE_ZONE_N("ClientMob.Sweep");
            for (const int32_t removedId : m_removedThisTick) {
                const auto it = m_mobs.find(removedId);
                if (it == m_mobs.end()) continue;
                Game::Mob* mob = it->second.mob.get();
                if (!mob || !mob->IsRemoved()) continue;
                mob->EjectPassengers();
                mob->StopRiding();
                ModelListRemove(it->second);
                {
                    const size_t idx = it->second.listIndex;
                    if (idx < m_mobList.size() && m_mobList[idx] == &it->second) {
                        ListSwapPop(idx);
                    }
                }
                m_codecBase.erase(removedId);
                m_mobs.erase(it);
            }
            m_removedThisTick.clear();
        }
    }

    void ClientMobManager::TickPassengerChain(Game::Entity& vehicle) {
        // Copy — a rider's tick can edit the passenger list.
        const std::vector<Game::Entity*> riders = vehicle.GetPassengers();
        for (Game::Entity* rider : riders) {
            if (rider->GetVehicle() != &vehicle) continue;

            ClientMob* entry = Find(rider->GetId());
            if (!entry) continue;
            Game::Mob& mob = *entry->mob;

            mob.SetOldPosAndRot();
            ++mob.tickCount;

            // ROTATION-only interpolation. The server sends no position for a
            // passenger (its position is derived from the vehicle here, by
            // RideTick's PositionRider), so targetPosition is stale pre-mount
            // data — lerping toward it is exactly the rider-vs-vehicle fight
            // that reads as jitter. Rotation packets do still arrive.
            if (entry->interpSteps > 0) {
                const float alpha = 1.0f / static_cast<float>(entry->interpSteps);
                mob.yRot     = Game::Mth::RotLerp(alpha, mob.yRot, entry->targetYRot);
                mob.xRot     = Game::Mth::Lerp(alpha, mob.xRot, entry->targetXRot);
                mob.yHeadRot = Game::Mth::RotLerp(alpha, mob.yHeadRot,
                                                  entry->targetYHeadRot);
                --entry->interpSteps;
            }

            // MC rideTick: zero own motion, tick (animations, poses — Move is
            // gated off for passengers), then the vehicle seats the rider at
            // its POST-tick position, so rider and vehicle move in lockstep
            // within the tick and the renderer's per-entity lerp keeps them in
            // lockstep within the frame.
            mob.RideTick();

            // A rider can end its own tick removed, and Tick's sweep is driven
            // by this list rather than by a scan — so a removal seen only here
            // has to be recorded here, or the rider survives until something
            // else notices it.
            if (mob.IsRemoved()) m_removedThisTick.push_back(mob.GetId());

            if (mob.IsVehicle()) TickPassengerChain(mob);
        }
    }

} // namespace Client
