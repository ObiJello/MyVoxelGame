// File: src/server/entity/ServerLevelBridge.cpp
#include "server/entity/ServerLevelBridge.hpp"
#include "server/entity/MobManager.hpp"
#include "server/player/ServerPlayer.hpp"
#include "server/session/PlayerSessionManager.hpp"
#include "server/session/PlayerSession.hpp"
#include "server/network/ServerConnection.hpp"
#include "common/network/packets/game/MobEntityPackets.hpp"
#include "common/core/Mth.hpp"
#include "common/world/level/World.hpp"
#include "common/world/level/WorldDrops.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/entity/Mob.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Server {

    // ── PlayerEntityView ───────────────────────────────────────────────────

    PlayerEntityView::PlayerEntityView(Game::EntityLevel* level, ServerPlayer* player,
                                       int32_t entityId)
        : Game::LivingEntity(Game::EntityTypeId::Zombie, level), m_player(player) {
        // The type id is a placeholder: nothing reads a player view's EntityType
        // (it is never spawned over the wire as a mob, and dimensions are
        // overridden above). Giving it a real slot in the mob table would be
        // worse — it would show up in spawn caps and the debug counts.
        SetId(entityId);
        SyncFromPlayer();
    }

    bool PlayerEntityView::IsCreative() const {
        return m_player && m_player->isCreative();
    }

    bool PlayerEntityView::IsSpectator() const {
        return m_player && m_player->getGameMode() == GameMode::SPECTATOR;
    }

    void PlayerEntityView::SyncFromPlayer() {
        if (!m_player) return;

        oldPosition = position;
        position = m_player->getPosition();

        yRotO = yRot;
        xRotO = xRot;
        // ServerPlayer's angles are MC's, the same as every mob's, so they
        // copy straight across. This used to negate the pitch (the camera was
        // positive-up) while leaving the yaw alone — which quietly left every
        // mob that looks at a player 90 degrees out.
        yRot = m_player->getYaw();
        xRot = m_player->getPitch();
        yHeadRotO = yHeadRot;
        yHeadRot = yRot;
        yBodyRotO = yBodyRot;
        yBodyRot = yRot;

        m_health = m_player->getHealth();
        if (m_player->isDead()) m_health = 0.0f;
    }

    void PlayerEntityView::TickCombatState() {
        TickCombatTimers();

        // MC LivingEntity.tickDeath — the corpse's topple clock, counted here
        // for the same reason the hurt timers are: nothing else ticks a view.
        // Other clients render the fall from this (PlayerUpdateS2C carries it),
        // and the player's own camera lean reads its own copy.
        if (m_player && m_player->isDead()) {
            if (deathTime < 20) ++deathTime;
        } else {
            deathTime = 0;
        }

        // Damage that never went through this view — fall, void, starvation —
        // still has to flash and tilt. MC funnels every source through
        // LivingEntity.hurtServer and so gets one hurt animation for all of
        // them; here the direct ServerPlayer::damage callers bypass us, and
        // this is where they are noticed.
        if (!m_player) return;
        const uint32_t counter = m_player->getDamageCounter();
        if (counter != m_lastSeenDamageCounter) {
            const bool alreadyFlashing = hurtTime > 0;
            m_lastSeenDamageCounter = counter;
            if (!alreadyFlashing) {
                hurtDuration = 10;
                hurtTime     = hurtDuration;
                // Direction zero = "from straight ahead", i.e. a plain roll,
                // which for a fall or the void is the honest answer: MC has no
                // source position for those either and simply reuses whatever
                // hurtDir was last set. Sent directly rather than through
                // IndicateDamage because atan2(0,0) would resolve to -yRot and
                // lean the camera by wherever the player happens to be looking.
                auto* bridge = static_cast<ServerLevelBridge*>(m_level);
                if (bridge) bridge->SendHurtAnimation(GetId(), 0.0f);
            }
        }
    }

    void PlayerEntityView::ActuallyHurt(Game::MobDamageSource source, float amount,
                                        Game::Entity* attacker) {
        if (!m_player) return;

        // Forward to the real player. ServerPlayer::damage runs its own
        // gamemode and death handling; the invulnerability window has already
        // been applied by LivingEntity::Hurt, so a mob cannot bypass it by
        // going through this path.
        m_player->damage(amount, DamageSource::ENTITY_ATTACK);
        m_health = m_player->getHealth();
    }

    bool PlayerEntityView::Hurt(Game::MobDamageSource source, float amount,
                                Game::Entity* attacker) {
        const glm::dvec3 before = velocity;
        const int hurtTimeBefore = hurtTime;
        const bool hit = Game::LivingEntity::Hurt(source, amount, attacker);

        // The player is client-authoritative for movement, so the knockback
        // LivingEntity just wrote into `velocity` will be overwritten by the
        // next move packet. Capture it so the tick loop can SEND it instead.
        if (hit && velocity != before) {
            m_pendingKnockback = velocity;
            m_hasPendingKnockback = true;
        }

        // MC LivingEntity.hurtServer:1213 — indicateDamage(xd, zd) with the
        // SAME vector the knockback used (attacker minus victim), sent only
        // when the hit actually opened a new hurt window. Damage taken inside
        // the invulnerability window re-flashes nothing in MC either.
        if (hit && attacker && hurtTime > hurtTimeBefore) {
            IndicateDamage(attacker->position.x - position.x,
                           attacker->position.z - position.z);
        }
        return hit;
    }

    void PlayerEntityView::IndicateDamage(double xd, double zd) {
        // MC ServerPlayer.indicateDamage:2019 — the attacker's bearing minus
        // our own yaw, so the client can lean the camera away from the blow.
        const float hurtDir =
            static_cast<float>(std::atan2(zd, xd)) * Game::Mth::kRadToDeg - yRot;

        auto* bridge = static_cast<ServerLevelBridge*>(m_level);
        if (bridge) bridge->SendHurtAnimation(GetId(), hurtDir);
    }

    bool PlayerEntityView::ConsumePendingKnockback(glm::dvec3& out) {
        if (!m_hasPendingKnockback) return false;
        out = m_pendingKnockback;
        m_hasPendingKnockback = false;
        m_pendingKnockback = glm::dvec3(0.0);
        return true;
    }

    // ── ServerLevelBridge ──────────────────────────────────────────────────

    ServerLevelBridge::ServerLevelBridge(Game::World* world, PlayerSessionManager* sessions)
        : m_world(world), m_sessions(sessions) {}

    const Game::IBlockAccess* ServerLevelBridge::Blocks() const { return m_world; }

    int64_t ServerLevelBridge::GetGameTime() const {
        return m_world ? m_world->GetGameTime() : 0;
    }

    int64_t ServerLevelBridge::GetDayTime() const {
        return m_world ? m_world->GetDayTime() : 0;
    }

    bool ServerLevelBridge::IsDay() const {
        // MC Level.isDay: dayTime modulo the 24000-tick cycle, between dawn and
        // dusk. 0 is sunrise, 12000 is sunset.
        const int64_t t = GetDayTime() % 24000;
        return t >= 0 && t < 12000;
    }

    bool ServerLevelBridge::CanSeeSky(int x, int y, int z) const {
        if (!m_world) return true;
        // One heightmap comparison. This used to walk the column, which was hot
        // enough to matter: GetMaxLocalRawBrightness routes through it, and
        // that is called by every monster's walk-target scoring (ten rolls per
        // wander) and by every spawn attempt.
        return m_world->CanSeeSky(x, y, z);
    }

    int ServerLevelBridge::GetSkyBrightness(int x, int y, int z) const {
        // MC LightLayer.SKY — the raw stored value, NOT time-adjusted. With no
        // light engine this is the open-sky stand-in: 15 outdoors, 0 under a
        // roof, at any hour.
        return CanSeeSky(x, y, z) ? 15 : 0;
    }

    int ServerLevelBridge::GetSkyDarken() const {
        // MC Timelines.DAY, the SKY_LIGHT_LEVEL multiply track: a 24000-tick
        // linear curve with four keyframes.
        //
        //   133   -> 1.0            full day
        //   11867 -> 1.0
        //   13670 -> 0.26666668     night (15 * this is exactly 4.0f)
        //   22330 -> 0.26666668
        //
        // Level.updateSkyBrightness then takes skyDarken = (int)(15 - 15*mult),
        // giving 0 by day and 11 at night. Those two numbers are what every
        // spawn light test is calibrated against, so the curve is reproduced
        // rather than approximated with a cosine.
        constexpr float kDayMult   = 1.0f;
        constexpr float kNightMult = 0.26666668f;

        const auto t = static_cast<int>(((GetDayTime() % 24000) + 24000) % 24000);

        float mult;
        if (t >= 133 && t <= 11867) {
            mult = kDayMult;
        } else if (t > 11867 && t < 13670) {
            // Dusk.
            const float f = static_cast<float>(t - 11867) / static_cast<float>(13670 - 11867);
            mult = kDayMult + (kNightMult - kDayMult) * f;
        } else if (t >= 13670 && t <= 22330) {
            mult = kNightMult;
        } else {
            // Dawn, which wraps the period boundary: 22330 -> 24133 (= 133).
            const int tt = (t < 133) ? t + 24000 : t;
            const float f = static_cast<float>(tt - 22330) / static_cast<float>(24133 - 22330);
            mult = kNightMult + (kDayMult - kNightMult) * f;
        }

        const float skyLightLevel = 15.0f * mult;
        return static_cast<int>(15.0f - skyLightLevel);
    }

    bool ServerLevelBridge::MonstersBurn() const {
        // MC Timelines.DAY, MONSTERS_BURN track: false at 12542, true at 23460.
        // Keyframes are step-held, so the burn window wraps: [23460, 12542).
        const auto t = static_cast<int>(((GetDayTime() % 24000) + 24000) % 24000);
        return t >= 23460 || t < 12542;
    }

    int ServerLevelBridge::GetMaxLocalRawBrightness(int x, int y, int z) const {
        return GetMaxLocalRawBrightness(x, y, z, GetSkyDarken());
    }

    int ServerLevelBridge::GetMaxLocalRawBrightness(int x, int y, int z, int amount) const {
        // MC LevelLightEngine.getRawBrightness: max(blockLight, skyLight - amount).
        // Block light is always 0 here — there is no light engine — so this is
        // the sky term alone. When one lands, the max() is already in place.
        constexpr int kBlockLight = 0;
        const int sky = GetSkyBrightness(x, y, z) - amount;
        return std::max(kBlockLight, sky);
    }

    void ServerLevelBridge::GetEntitiesInBox(const Game::AABB& box, const Game::Entity* except,
                                             std::vector<Game::Entity*>& out) const {
        if (m_mobs) m_mobs->CollectInBox(box, except, out);

        // Player views participate in entity queries — BreedGoal does not care,
        // but HurtByTargetGoal's alert scan and the creeper explosion both do.
        for (PlayerEntityView* view : m_playerViewList) {
            if (view == except) continue;
            if (!view->GetAABB().Intersects(box)) continue;
            out.push_back(view);
        }
    }

    Game::LivingEntity* ServerLevelBridge::GetNearestPlayer(double x, double y, double z,
                                                            double maxDistance) const {
        Game::LivingEntity* best = nullptr;
        double bestDist = maxDistance < 0.0 ? std::numeric_limits<double>::max()
                                            : maxDistance * maxDistance;

        for (PlayerEntityView* view : m_playerViewList) {
            // Spectators are never "the nearest player" for any purpose — this
            // is what keeps a spectator from spooking animals or waking mobs.
            if (view->IsSpectator()) continue;
            if (!view->IsAlive()) continue;

            const double d = view->DistanceToSqr(x, y, z);
            if (d < bestDist) { bestDist = d; best = view; }
        }
        return best;
    }

    void ServerLevelBridge::GetPlayers(std::vector<Game::LivingEntity*>& out) const {
        for (PlayerEntityView* view : m_playerViewList) out.push_back(view);
    }

    uint32_t ServerLevelBridge::GetHeldItemId(const Game::LivingEntity& player) const {
        const auto* view = dynamic_cast<const PlayerEntityView*>(&player);
        if (!view || !view->GetPlayer()) return 0;
        return view->GetPlayer()->getItemInHand(0).itemId;
    }

    void ServerLevelBridge::BroadcastEntityEvent(const Game::Entity& entity, uint8_t event) {
        m_pendingEvents.push_back({ entity.GetId(), event });
    }

    void ServerLevelBridge::SendHurtAnimation(int32_t connectionId, float hurtDir) {
        if (!m_sessions) return;
        auto session = m_sessions->GetSessionByConnection(
            static_cast<uint32_t>(connectionId));
        if (!session) return;
        auto* connection = session->GetConnection();
        if (!connection) return;

        Network::HurtAnimationS2CPacket p;
        p.entityId = connectionId;
        p.yaw      = hurtDir;
        connection->SendPacket(
            static_cast<uint8_t>(Network::PacketId::HurtAnimationS2C),
            Network::Serialization::Serialize(p));
    }

    void ServerLevelBridge::DestroyBlock(const glm::ivec3& pos, bool dropResources) {
        if (!m_world) return;
        // MC Level.destroyBlock(pos, dropBlock). The sheep passes false, so
        // grazing a fern yields nothing — the wool IS the yield.
        if (dropResources) {
            const Game::BlockID was = m_world->GetBlock(pos.x, pos.y, pos.z);
            if (was != Game::BlockID::Air) {
                Game::DropItemStackNear(pos, Game::ItemStack(
                    Game::ItemRegistry::FromBlock(was), 1));
            }
        }
        m_world->SetBlock(pos.x, pos.y, pos.z, Game::BlockID::Air,
                          Game::World::UpdateFlags::All);
    }

    void ServerLevelBridge::SetBlock(const glm::ivec3& pos, Game::BlockID block) {
        if (!m_world) return;
        m_world->SetBlock(pos.x, pos.y, pos.z, block, Game::World::UpdateFlags::All);
    }

    bool ServerLevelBridge::MobGriefing() const {
        return m_world ? m_world->GetDoMobGriefing() : true;
    }

    void ServerLevelBridge::SpawnItemDrop(const glm::dvec3& pos, uint32_t itemId, int count) {
        if (count <= 0) return;

        // DropItemStackNear takes a BLOCK position — it is MC's popResource,
        // which scatters within the cell and adds a small hop. That is the
        // right behaviour for a death drop too, so the entity's continuous
        // position is floored rather than a second drop path being added.
        const glm::ivec3 blockPos(static_cast<int>(std::floor(pos.x)),
                                  static_cast<int>(std::floor(pos.y)),
                                  static_cast<int>(std::floor(pos.z)));
        Game::DropItemStackNear(blockPos, Game::ItemStack(itemId, count));
    }

    void ServerLevelBridge::AddFreshEntity(std::unique_ptr<Game::Entity> entity) {
        // Deferred: the mob tick loop is iterating its own container when this
        // is called (breeding, reinforcements), so inserting now would
        // invalidate the iteration.
        m_spawned.push_back(std::move(entity));
    }

    void ServerLevelBridge::SyncPlayerViews() {
        if (!m_sessions) return;

        m_playerViewList.clear();

        const auto sessions = m_sessions->GetAllSessions();
        std::vector<uint32_t> live;
        live.reserve(sessions.size());

        for (const auto& session : sessions) {
            if (!session) continue;
            ServerPlayer* player = session->GetPlayer();
            if (!player) continue;

            const uint32_t id = session->GetConnectionId();
            live.push_back(id);

            auto it = m_playerViews.find(id);
            if (it == m_playerViews.end()) {
                // Player entity ids are connection ids, matching the existing
                // convention documented on Game::kItemEntityIdBase.
                auto view = std::make_unique<PlayerEntityView>(this, player,
                                                               static_cast<int32_t>(id));
                it = m_playerViews.emplace(id, std::move(view)).first;
            } else {
                it->second->SyncFromPlayer();
                // The view is not an entity the server ticks (the client owns
                // player movement), so this is the only place its hurt flash
                // and invulnerability window count down.
                it->second->TickCombatState();
            }

            m_playerViewList.push_back(it->second.get());
        }

        // Drop views for sessions that left — and tell every mob first.
        //
        // A departing player is the other way a referenced entity disappears:
        // mobs targeting, fleeing or watching that player hold a raw pointer to
        // its view and re-check it with IsAlive() next tick. Freeing the view
        // without clearing those is a use-after-free, the same failure mode the
        // mob sweep guards against (see MobManager::Tick).
        for (auto it = m_playerViews.begin(); it != m_playerViews.end();) {
            if (std::find(live.begin(), live.end(), it->first) != live.end()) {
                ++it;
                continue;
            }

            if (m_mobs) {
                PlayerEntityView* departing = it->second.get();
                for (const auto& [mobId, mob] : m_mobs->All()) {
                    mob->ClearReferenceTo(departing);
                }
            }
            it = m_playerViews.erase(it);
        }
    }

    PlayerEntityView* ServerLevelBridge::GetPlayerView(uint32_t connectionId) {
        auto it = m_playerViews.find(connectionId);
        return it == m_playerViews.end() ? nullptr : it->second.get();
    }

} // namespace Server
