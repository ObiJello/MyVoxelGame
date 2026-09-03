// File: src/server/entity/ServerLevelBridge.cpp
#include "server/entity/ServerLevelBridge.hpp"
#include "common/entity/PrimedTnt.hpp"
#include "server/entity/ExperienceOrbManager.hpp"
#include "server/entity/MobManager.hpp"
#include "server/IntegratedServer.hpp"
#include "server/world/storage/anvil/PlayerUuid.hpp"
#include "server/level/ServerLevel.hpp"
#include "server/player/ServerPlayer.hpp"
#include "server/session/PlayerSessionManager.hpp"
#include "server/session/PlayerSession.hpp"
#include "server/network/ServerConnection.hpp"
#include "common/network/packets/game/MobEntityPackets.hpp"
#include "common/core/Mth.hpp"
#include "common/world/level/Explosion.hpp"
#include "common/core/TickParallel.hpp"
#include "common/core/Profiling_Tracy.hpp"
#include "server/entity/ItemEntityManager.hpp"
#include "common/world/level/DimensionId.hpp"
#include "common/world/level/World.hpp"
#include "common/world/level/WorldDrops.hpp"
#include "common/world/biome/Biomes.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/block/entity/BaseContainerBlockEntity.hpp"
#include "common/world/chunk/Chunk.hpp"
#include "common/world/math/WorldCoordinates.hpp"
#include "common/entity/Mob.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <chrono>

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

        // The view's UUID is the OFFLINE uuid derived from the player's name —
        // the same derivation playerdata/<uuid>.dat and ResolvePlayer use.
        // Without it EntityRef::Set on a view stored nil and the ref read as
        // Empty, so a projectile's owner check (CanHitEntity → Matches) never
        // recognised the thrower: the first thrown ender pearl clipped its own
        // thrower's hitbox on tick one and teleported them in place.
        if (m_player) {
            SetUuid(Game::Anvil::OfflinePlayerUuid(m_player->getName()));
        }

        // MC Entity.restoreFrom(:3008-3009) — the dimension change's other
        // half. See ServerPlayer::portalState(): a view is per LEVEL, so this
        // constructor IS the "entity recreated in the destination", and the
        // portal cooldown it inherits is the only thing keeping the portal it
        // just arrived inside from firing straight back.
        if (m_player) {
            portal.CopyFrom(m_player->portalState());
        }
    }

    bool PlayerEntityView::IsCreative() const {
        return m_player && m_player->isCreative();
    }

    bool PlayerEntityView::IsSpectator() const {
        return m_player && m_player->getGameMode() == GameMode::SPECTATOR;
    }

    bool PlayerEntityView::IsAbilityFlying() const {
        return m_player && m_player->isFlying();
    }

    void PlayerEntityView::SyncFromPlayer() {
        if (!m_player) return;

        oldPosition = position;
        position = m_player->getPosition();
        // The player's size (scaled portals, /scale): the mobs' view of
        // them is as tall and as wide as they are.
        scale = m_player->getScale();
        // The client reports its ground state with every move packet;
        // mirrored here so shootFromRotation's "add the shooter's y movement
        // only while airborne" reads the truth.
        onGround = m_player->isOnGround();

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

        // MC ticks a player's effects from LivingEntity.baseTick like any
        // other entity; the view is never BaseTick'ed (the client owns player
        // movement), so this is where a player's poison damages, regeneration
        // heals and mining fatigue counts down. Runs AFTER SyncFromPlayer so
        // the health the ticks read is this tick's truth.
        TickEffects();

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
        // Name the killer for the death broadcast. MC pulls this from
        // DamageSource.causingEntity in CombatTracker; we have no combat
        // tracker, so it rides along with the damage call. The slug is
        // title-cased ("zombie" -> "Zombie") to stand in for MC's translated
        // entity name.
        std::string attackerName;
        if (attacker) {
            const std::string_view slug = attacker->TypeInfo().slug;
            attackerName.assign(slug.begin(), slug.end());
            bool upper = true;
            for (char& c : attackerName) {
                if (c == '_') { c = ' '; upper = true; continue; }
                c = upper ? static_cast<char>(::toupper((unsigned char)c)) : c;
                upper = false;
            }
        }
        // Map the mob-system source onto ServerPlayer's own enum so the death
        // message tells the truth — a player killed by poison ticks reads
        // "was killed by magic", not "was slain by".
        DamageSource playerSource = DamageSource::ENTITY_ATTACK;
        switch (source) {
            case Game::MobDamageSource::Magic:
            case Game::MobDamageSource::Wither:
                playerSource = DamageSource::MAGIC;
                break;
            case Game::MobDamageSource::Fire:
                playerSource = DamageSource::FIRE;
                break;
            // Explosion used to fall through to ENTITY_ATTACK, which made
            // DamageSource::EXPLOSION unreachable and reported every creeper
            // kill as "was slain by Creeper" instead of "blew up".
            case Game::MobDamageSource::Explosion:
                playerSource = DamageSource::EXPLOSION;
                break;
            case Game::MobDamageSource::Fall:
                playerSource = DamageSource::FALL;
                break;
            case Game::MobDamageSource::Drown:
                playerSource = DamageSource::DROWNING;
                break;
            case Game::MobDamageSource::Void:
                playerSource = DamageSource::VOID_DAMAGE;
                break;
            case Game::MobDamageSource::FallingBlock:
                playerSource = DamageSource::FALLING_BLOCK;
                break;
            case Game::MobDamageSource::FallingAnvil:
                playerSource = DamageSource::FALLING_ANVIL;
                break;
            case Game::MobDamageSource::FallingStalactite:
                playerSource = DamageSource::FALLING_STALACTITE;
                break;
            case Game::MobDamageSource::Stalagmite:
                playerSource = DamageSource::STALAGMITE;
                break;
            default:
                break;
        }
        // MC Player.hurtServer's difficulty pass, applied to sources whose
        // damage type is `"scaling": "always"` — of which EXPLOSION is one.
        //
        //     PEACEFUL -> 0, and the hit is DROPPED (MC: `damage == 0 ? false`)
        //     EASY     -> min(dmg/2 + 1, dmg)
        //     NORMAL   -> unchanged
        //     HARD     -> dmg * 3/2
        //
        // This is what makes full cover on Peaceful cost nothing. MC's
        // explosion formula has a flat `+1` floor that lands on every entity in
        // range regardless of exposure, so without this pass hiding behind
        // obsidian still took half a heart.
        if (Game::DamageSourceScalesWithDifficulty(source)) {
            switch (GetDifficultyOfLevel()) {
                case Game::Difficulty::Peaceful: amount = 0.0f; break;
                case Game::Difficulty::Easy:
                    amount = std::min(amount / 2.0f + 1.0f, amount);
                    break;
                case Game::Difficulty::Hard:     amount = amount * 3.0f / 2.0f; break;
                case Game::Difficulty::Normal:   break;
            }
            if (amount == 0.0f) return;   // MC returns false without hurting
        }

        m_player->damage(amount, playerSource, attackerName);
        m_health = m_player->getHealth();
    }

    void PlayerEntityView::Heal(float amount) {
        if (!m_player || m_player->isDead()) return;
        m_player->heal(amount);
        m_health = m_player->getHealth();
    }

    void PlayerEntityView::CauseFoodExhaustion(float amount) {
        // MC Player.causeFoodExhaustion — HUNGER's per-tick drain.
        if (m_player) m_player->getFoodData().addExhaustion(amount);
    }

    void PlayerEntityView::EatFood(int nutrition, float saturationModifier) {
        // MC FoodData.eat — SATURATION's instant top-up.
        if (m_player) m_player->getFoodData().eat(nutrition, saturationModifier);
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

    Game::Difficulty PlayerEntityView::GetDifficultyOfLevel() const {
        // The view's own Level() is the ServerLevelBridge, which knows the
        // world's difficulty. Null only during teardown.
        return m_level ? m_level->GetDifficulty() : Game::Difficulty::Normal;
    }

    bool PlayerEntityView::ConsumePendingKnockback(glm::dvec3& out) {
        if (!m_hasPendingKnockback) return false;
        out = m_pendingKnockback;
        m_hasPendingKnockback = false;
        m_pendingKnockback = glm::dvec3(0.0);
        // Reset the view's velocity too. AddDeltaMovement reports the
        // ACCUMULATED velocity as the pending push (which is right — several
        // things can shove you in one tick and the client should get the sum),
        // but nothing else ever clears it: the player is client-authoritative
        // for movement, so this view's velocity is never integrated or damped
        // the way a mob's is. Without this reset every push a player has ever
        // received stays in the total, and the next one sends the running sum.
        velocity = glm::dvec3(0.0);
        return true;
    }

    // ── ServerLevelBridge ──────────────────────────────────────────────────

    ServerLevelBridge::~ServerLevelBridge() = default;

    ServerLevelBridge::ServerLevelBridge(Game::World* world, PlayerSessionManager* sessions)
        : m_world(world), m_sessions(sessions) {}

    const Game::IBlockAccess* ServerLevelBridge::Blocks() const { return m_world; }

    uint64_t ServerLevelBridge::BlockWriteEpoch() const {
        return m_world ? m_world->BlockWriteEpoch() : 0;
    }

    namespace {
        // How much of a 50 ms tick may go to detonations. The rest of the tick
        // still has to run — entities, chunks, packets — and a blast is not
        // interruptible once started, so the real worst case is this budget
        // plus one blast. At the measured ~1,295 us per blast that is roughly
        // 19 detonations a tick, ~380 a second.
        constexpr auto kExplosionBudget = std::chrono::microseconds(25000);
    // Ceiling for the tick-proportional budget (see BeginExplosionBudget).
    constexpr auto kExplosionBudgetMax = std::chrono::microseconds(500000);
    }

    void ServerLevelBridge::BeginExplosionBudget() {
        const auto now = std::chrono::steady_clock::now();
        // How long the REST of the last tick took (everything but the
        // resolve). When a mass detonation already has the tick at 100 ms of
        // entity physics, capping blasts at 25 ms means four times as many
        // ticks — and four times the physics — to get through the pile. Let
        // the blasts have as long as the rest of the tick did, within
        // [kExplosionBudget, kExplosionBudgetMax]; on a healthy server the
        // rest of the tick is a few ms and the floor is what applies.
        int64_t budgetNs = std::chrono::duration_cast<std::chrono::nanoseconds>(kExplosionBudget).count();
        if (m_explosionBudgetStart.time_since_epoch().count() != 0) {
            const int64_t tickNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                       now - m_explosionBudgetStart).count();
            const int64_t otherNs = tickNs - m_lastResolveTotalNs;
            const int64_t maxNs = std::chrono::duration_cast<std::chrono::nanoseconds>(kExplosionBudgetMax).count();
            budgetNs = std::clamp<int64_t>(otherNs, budgetNs, maxNs);
        }
        m_explosionBudgetStart = now;
        m_explosionsThisTick.store(0, std::memory_order_relaxed);

        // ── From an elapsed-time gate to a measured count ───────────────────
        //
        // The old gate asked "has 25 ms passed since the tick began?" at the
        // moment a fuse ran out. That worked when the blast ran INLINE right
        // there. It cannot work now: the cost is paid later, in
        // ResolveQueuedExplosions, so at gate time the elapsed clock knows
        // nothing about what has been admitted.
        //
        // So the gate is a COUNT, derived from what the last resolve actually
        // cost per blast. Same contract as before — about 25 ms of tick spent
        // detonating — but measured rather than sampled, and it re-tunes itself
        // as blasts get cheaper or dearer (a crater in open air and one eating
        // into stone differ by several times).
        if (m_lastBlastCostNs > 0) {
            // The batched apply has a per-TICK cost that does not scale with
            // the blast count — one victim gather over the pile, one binning
            // pass — measured separately as m_lastResolveFixedNs. Dividing the
            // whole resolve by the count charged that overhead to every blast
            // and admitted a tenth of what the budget could actually afford.
            // Budget the marginal cost against what is left after the fixed
            // part; if the fixed part alone eats the budget, still admit a
            // useful batch, since the overhead is paid per tick regardless.
            const int64_t marginalBudget = std::max<int64_t>(
                budgetNs - m_lastResolveFixedNs, budgetNs / 4);
            const int64_t allowed = marginalBudget / m_lastBlastCostNs;
            // Floor of 1 for the same reason the old gate always let the first
            // blast through: a single detonation dearer than the whole budget
            // must still happen, or the cascade livelocks with fuses pinned at
            // zero. Ceiling because the pending vectors are sized by this.
            m_blastsAllowedThisTick =
                static_cast<int>(std::clamp<int64_t>(allowed, 1, 8192));
        } else {
            // First blasts of a cascade, before any resolve has been
            // measured: admit a real batch rather than one. A single blast
            // against a fully stacked pile takes the one-blast path, and it
            // is the batch path that carries the million-victim machinery.
            m_blastsAllowedThisTick = 64;
        }
    }

    bool ServerLevelBridge::TryBeginExplosion() {
        // Atomic: primed TNT calls this from the parallel tick batch. The
        // first claim of the tick is always granted — a single detonation
        // dearer than the whole budget must still happen, or the cascade
        // livelocks with fuses pinned at zero.
        const int n = m_explosionsThisTick.fetch_add(1, std::memory_order_relaxed);
        return n == 0 || n < m_blastsAllowedThisTick;
    }

    void ServerLevelBridge::QueueExplosion(const Game::ExplosionParams& p) {
        // Locked because primed TNT now ticks across the worker pool (see
        // MobManager::Tick's parallel batch), and this push is the ONE piece of
        // shared mutable state a TNT's tick reaches. Contended a few thousand
        // times a tick against work measured in tens of milliseconds.
        std::lock_guard<std::mutex> lock(m_pendingExplosionsMutex);
        m_pendingExplosions.push_back(p);
    }

    void ServerLevelBridge::ResolveQueuedExplosions() {
        if (m_pendingExplosions.empty()) { m_lastResolveTotalNs = 0; return; }
        PROFILE_ZONE_N("ResolveExplosions");

        const auto started = std::chrono::steady_clock::now();

        // Thread scheduling decided the push order; entity id decides the APPLY
        // order. Without this the sequence of Hurts, knockbacks and block
        // writes would vary run to run for identical input — the blasts would
        // still all happen, but the world would not be reproducible from a
        // seed, and a bug found once could not be re-run.
        std::sort(m_pendingExplosions.begin(), m_pendingExplosions.end(),
                  [](const Game::ExplosionParams& a, const Game::ExplosionParams& b) {
                      return (a.source ? a.source->GetId() : 0) <
                             (b.source ? b.source->GetId() : 0);
                  });

        const size_t count = m_pendingExplosions.size();

        // ── Phase A: draw the jitter, serially, on this thread ──────────────
        //
        // The per-ray power jitter is the only shared mutable state the crater
        // scan would touch, and JavaRandom is not thread-safe. Drawing it here,
        // in queue order and at a fixed count per blast, is what lets the scans
        // run anywhere.
        //
        // It does move the whole batch's jitter ahead of the whole batch's drop
        // rolls, which shifts the RNG stream against a serial detonation — see
        // the note on Game::ExplosionScanCrater. Deterministic, replayable, and
        // observable only as different crater raggedness and drop luck.
        {
            PROFILE_ZONE_N("ResolveExplosions.Jitter");
            m_explosionJitter.resize(count * Game::kExplosionRayCount);
            for (size_t i = 0; i < count * Game::kExplosionRayCount; ++i) {
                m_explosionJitter[i] = m_random.NextFloat();
            }
        }

        // ── Phase B: the crater scans, off the tick thread ──────────────────
        //
        // Pure reads of the block field: nothing here writes a block, spawns an
        // entity or touches the RNG. Phase C is the first thing in a blast that
        // mutates anything, and it has not run yet — so the field every scan
        // sees is this tick's, identical for all of them.
        //
        // THAT IS A DIVERGENCE and it is deliberate: run serially, blast 2
        // would march through blast 1's fresh crater and reach further. Batched,
        // they do not compound within a tick. With tens of blasts a tick in one
        // pile the difference is a marginally smaller crater per tick, made up
        // for by the ticks that follow.
        m_explosionCraters.resize(count);
        const auto scanStart = std::chrono::steady_clock::now();
        {
            PROFILE_ZONE_N("ResolveExplosions.Scan");
            // Below this the fork/join costs more than the scans save — the
            // same reasoning as Explosion.cpp's exposure threshold, scaled to a
            // ~245 us unit of work rather than a ~1.3 us one.
            constexpr size_t kParallelThreshold = 4;
            if (count >= kParallelThreshold && Core::ParallelWidth() > 1) {
                Core::ParallelFor(count, 1, [&](size_t i) {
                    m_explosionCraters[i] = Game::ExplosionScanCrater(
                        *this, m_pendingExplosions[i],
                        m_explosionJitter.data() + i * Game::kExplosionRayCount);
                });
            } else {
                for (size_t i = 0; i < count; ++i) {
                    m_explosionCraters[i] = Game::ExplosionScanCrater(
                        *this, m_pendingExplosions[i],
                        m_explosionJitter.data() + i * Game::kExplosionRayCount);
                }
            }
        }

        const int64_t scanNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now() - scanStart).count();

        // ── Phase C: apply, serially, in queue order ────────────────────────
        //
        // Every mutation a blast makes happens here and in exactly the order it
        // did before: each Hurt, each AddDeltaMovement, each SetBlock, each
        // drop roll and each RNG draw they reach. Only the pure half moved.
        //
        // Iterated by index rather than by iterator: an apply can prime a TNT
        // block into a fresh PrimedTnt, which is deferred to DrainSpawned and
        // cannot queue into this vector — but a range-for would still be a
        // silent trap for the day something does.
        {
            PROFILE_ZONE_N("ResolveExplosions.Apply");
            if (count >= 2) {
                m_explosionFixedNs = Game::ExplodeApplyBatch(*this, m_pendingExplosions,
                                                             m_explosionCraters,
                                                             &m_explosionBlastNs);
            } else {
                Game::ExplodeApply(*this, m_pendingExplosions[0], m_explosionCraters[0]);
            }
        }

        // What the next tick's gate is sized from. Measured over the WHOLE
        // resolve, so the jitter draws and the join overhead are charged to the
        // blasts that caused them rather than being invisible.
        const auto elapsedNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now() - started).count();
        // Fixed part: what the batch spent before any blast-proportional work
        // (the gather and binning), reported by the apply; zero on the
        // single-blast path.
        m_lastResolveTotalNs = elapsedNs;
        // Marginal cost measured DIRECTLY: the crater scans plus the
        // block/fire/broadcast tail are the only parts that scale with the
        // blast count. Everything else — the victim gather, the tables, the
        // knockback passes — scales with the pile, repeats per tick whatever
        // is admitted, and lives in the fixed part. A 10 us floor keeps a
        // batch of trivial air blasts from opening the gate to the cap.
        const int64_t marginalNs = m_explosionBlastNs > 0 || scanNs > 0
            ? (m_explosionBlastNs + scanNs) : (elapsedNs - m_explosionFixedNs);
        m_lastResolveFixedNs = std::min<int64_t>(elapsedNs,
            std::max<int64_t>(0, elapsedNs - marginalNs));
        m_lastBlastCostNs = std::max<int64_t>(10000,
            marginalNs / static_cast<int64_t>(count));
        m_explosionFixedNs = 0;
        m_explosionBlastNs = 0;

        m_pendingExplosions.clear();
        m_explosionCraters.clear();
    }

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

    float ServerLevelBridge::GetBiomeTemperature(int x, int y, int z) const {
        // MC Biome.getBaseTemperature. Same lookup the natural spawner's
        // biomeAt lambda makes — World::GetBiome hands back the registry id.
        if (!m_world) return 0.8f;
        return Game::BiomeRegistry::Get(m_world->GetBiome(x, y, z)).temperature;
    }

    int ServerLevelBridge::GetSkyBrightness(int x, int y, int z) const {
        // MC LightLayer.SKY — the raw stored value, NOT time-adjusted. With no
        // light engine this is the open-sky stand-in: 15 outdoors, 0 under a
        // roof, at any hour.
        //
        // ZERO in a dimension without skylight (MC DimensionType.hasSkyLight:
        // the End and the Nether store an all-zero sky layer). The Nether got
        // this right by accident — its bedrock roof fails CanSeeSky — but the
        // End's open sky read as full daylight here, which is exactly the
        // condition the monster-spawn darkness test rejects: no enderman ever
        // spawned on the island.
        if (m_world && !Game::DimensionHasSkyLight(m_world->GetDimension())) {
            return 0;
        }
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
        if (m_mobs) m_mobs->CollectInBoxParallel(box, except, out);

        // Player views participate in entity queries — BreedGoal does not care,
        // but HurtByTargetGoal's alert scan and the creeper explosion both do.
        for (PlayerEntityView* view : m_playerViewList) {
            if (view == except) continue;
            // MC EntityGetter.java:29 — the two-arg getEntities is
            //     getEntities(except, bb, EntitySelector.NO_SPECTATORS)
            // and EntitySelector.java:26 is `!entity.isSpectator()`. A
            // spectator is excluded from the LIST, not merely from damage.
            //
            // Without this a spectator watching a TNT chain entered
            // LivingEntity::Hurt once per blast: i-frames armed, hurtTime set,
            // last-attacker clobbered, and a red flash plus camera tilt from
            // every explosion. Health survived only because ServerPlayer's
            // damage bails on gamemode, which is not the same thing.
            //
            // NOT a replacement for the separate creative-flying gate in the
            // explosion knockback path — that is MC's hitPlayers rule
            // (ServerExplosion.java:198) and still applies.
            if (view->IsSpectator()) continue;
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
    Game::Entity* ServerLevelBridge::ResolveEntity(const Game::Uuid& uuid) const {
        if (Game::UuidIsNil(uuid)) return nullptr;

        // This level first — the common case, and it is a hash lookup.
        if (m_mobs) {
            if (Game::Mob* mob = m_mobs->FindByUuid(uuid)) return mob;
        }

        // Then every sibling dimension. A saved reference can cross a portal,
        // and vanilla resolves the same way rather than treating a reference
        // as level-local.
        if (g_integratedServer) {
            Game::Entity* found = nullptr;
            g_integratedServer->ForEachLevel([&](ServerLevel& level) {
                if (found) return;
                if (MobManager* mobs = level.Mobs()) {
                    if (Game::Mob* mob = mobs->FindByUuid(uuid)) found = mob;
                }
            });
            if (found) return found;
        }
        return nullptr;
    }

    Game::LivingEntity* ServerLevelBridge::ResolvePlayer(const Game::Uuid& uuid) const {
        if (Game::UuidIsNil(uuid)) return nullptr;

        // Players are keyed by the OFFLINE uuid derived from their name, the
        // same derivation playerdata/<uuid>.dat uses — so a pet saved with its
        // owner's uuid finds that owner again on any later session.
        //
        // Every level's views are searched, not just this one: an owner may be
        // standing in another dimension.
        if (!g_integratedServer) return nullptr;

        Game::LivingEntity* found = nullptr;
        g_integratedServer->ForEachLevel([&](ServerLevel& level) {
            if (found) return;
            ServerLevelBridge* bridge = level.MobLevel();
            if (!bridge) return;
            std::vector<Game::LivingEntity*> players;
            bridge->GetPlayers(players);
            for (Game::LivingEntity* p : players) {
                auto* view = dynamic_cast<PlayerEntityView*>(p);
                if (!view || !view->GetPlayer()) continue;
                if (Game::Anvil::OfflinePlayerUuid(view->GetPlayer()->getName()) == uuid) {
                    found = p;
                    return;
                }
            }
        });
        return found;
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
        connection->SendPacketIn(Dimension(),
            static_cast<uint8_t>(Network::PacketId::HurtAnimationS2C),
            Network::Serialization::Serialize(p));
    }

    Game::ILevelWrite* ServerLevelBridge::MutableBlocks() {
        return m_world;
    }

    void ServerLevelBridge::OnTntExploded(const glm::ivec3& pos, Game::Entity* igniter) {
        // MC TntBlock.wasExploded: a TNT block caught in a blast re-primes with
        // a SHORT random fuse — nextInt(80/4) + 80/8, i.e. 10..29 ticks. The
        // stagger is the whole reason a 3x3x3 of TNT ripples outward instead of
        // detonating as one instant sphere.
        if (!m_world || !m_world->GetTntExplodes()) return;

        auto tnt = std::make_unique<Game::PrimedTnt>(this);
        tnt->InitPrimed(glm::dvec3(static_cast<double>(pos.x) + 0.5,
                                   static_cast<double>(pos.y),
                                   static_cast<double>(pos.z) + 0.5),
                        igniter);
        const int base = Game::PrimedTnt::kDefaultFuse;
        tnt->SetFuse(Random().NextInt(base / 4) + base / 8);
        AddFreshEntity(std::move(tnt));
    }

    void ServerLevelBridge::ApplyExplosionsToLooseEntities(
            const Game::ExplosionParams* const* params, size_t count,
            const Game::CollisionGrid* occlusion) {
        auto* server = Server::g_integratedServer.get();
        if (!server || count == 0) return;

        // Bounds of the whole batch, for the cheap reject; then each loose
        // entity that survives it is tested against each blast in order —
        // the same per-blast work as ApplyExplosionToLooseEntities, walked
        // once per batch instead of once per blast.
        glm::dvec3 lo(std::numeric_limits<double>::infinity());
        glm::dvec3 hi(-std::numeric_limits<double>::infinity());
        for (size_t i = 0; i < count; ++i) {
            const double reach = static_cast<double>(params[i]->radius) * 2.0;
            lo = glm::min(lo, params[i]->center - glm::dvec3(reach));
            hi = glm::max(hi, params[i]->center + glm::dvec3(reach));
        }
        const auto outOfReachAll = [&](const glm::dvec3& p) {
            return p.x < lo.x || p.x > hi.x || p.y < lo.y || p.y > hi.y ||
                   p.z < lo.z || p.z > hi.z;
        };
        const auto outOfReachOf = [](const Game::ExplosionParams& b, const glm::dvec3& p) {
            const double reach = static_cast<double>(b.radius) * 2.0;
            return p.x < b.center.x - reach || p.x > b.center.x + reach ||
                   p.y < b.center.y - reach || p.y > b.center.y + reach ||
                   p.z < b.center.z - reach || p.z > b.center.z + reach;
        };

        if (auto* items = m_items) {
            for (auto& [id, item] : items->AllMutable()) {
                if (item.stack.IsEmpty()) continue;
                if (outOfReachAll(item.pos)) continue;
                const double kW = Game::ItemEntity::kWidth;
                const double kH = Game::ItemEntity::kHeight;
                for (size_t i = 0; i < count; ++i) {
                    const Game::ExplosionParams& b = *params[i];
                    if (outOfReachOf(b, item.pos)) continue;
                    Game::AABBd box;
                    box.min = item.pos - glm::dvec3(kW * 0.5, 0.0, kW * 0.5);
                    box.max = box.min + glm::dvec3(kW, kH, kW);
                    const Game::ExplosionImpact impact =
                        Game::ComputeExplosionImpact(*this, b, item.pos, box, occlusion);
                    if (!impact.inRange) continue;
                    item.vel += impact.knockback;
                    item.needsSync = true;
                    if (impact.damage > 0.0f) {
                        item.health -= static_cast<int>(impact.damage);
                        if (item.health <= 0) { item.stack.Clear(); break; }
                    }
                }
            }
        }

        if (auto* orbs = m_orbs) {
            for (auto& [id, orb] : orbs->AllMutable()) {
                if (outOfReachAll(orb.pos)) continue;
                for (size_t i = 0; i < count; ++i) {
                    const Game::ExplosionParams& b = *params[i];
                    if (outOfReachOf(b, orb.pos)) continue;
                    Game::AABBd box;
                    box.min = orb.pos - glm::dvec3(0.25, 0.0, 0.25);
                    box.max = box.min + glm::dvec3(0.5, 0.5, 0.5);
                    const Game::ExplosionImpact impact =
                        Game::ComputeExplosionImpact(*this, b, orb.pos, box, occlusion);
                    if (impact.inRange) orb.vel += impact.knockback;
                }
            }
        }
    }

    void ServerLevelBridge::ApplyExplosionToLooseEntities(
            const Game::ExplosionParams& params,
            const Game::CollisionGrid* occlusion) {
        auto* server = Server::g_integratedServer.get();
        if (!server) return;

        // Cheap bounds reject, hoisted out of both loops below.
        //
        // Neither manager has a spatial index — both store a flat
        // unordered_map — so this pass is O(all loose entities) per blast no
        // matter what. What it must NOT be is O(all loose entities) x
        // sqrt + AABB construction + ComputeExplosionImpact call, which is what
        // it was: a world already full of dropped items (which is exactly what
        // a world you have blown up a lot looks like) paid that for every
        // single blast, and a TNT chain is hundreds of blasts.
        //
        // Three comparisons per axis reject everything outside the blast's
        // reach before any of that runs. The real fix is an index on the item
        // manager; this makes the constant small enough that it stops
        // mattering first.
        const double reach = static_cast<double>(params.radius) * 2.0;
        const glm::dvec3 lo = params.center - glm::dvec3(reach);
        const glm::dvec3 hi = params.center + glm::dvec3(reach);
        const auto outOfReach = [&](const glm::dvec3& p) {
            return p.x < lo.x || p.x > hi.x ||
                   p.y < lo.y || p.y > hi.y ||
                   p.z < lo.z || p.z > hi.z;
        };

        // Dropped items: MC ItemEntity.hurtServer subtracts the damage from a
        // health of 5 and discards at zero, so anything close to a TNT is gone.
        // Clearing the stack is this manager's own "destroyed" signal — its
        // tick sweeps empty entities and broadcasts the removal (see step 4 of
        // ItemEntityManager::Tick), so the client is told without a second
        // removal path.
        if (auto* items = m_items) {
            for (auto& [id, item] : items->AllMutable()) {
                if (item.stack.IsEmpty()) continue;
                if (outOfReach(item.pos)) continue;

                // Built in DOUBLE, like MC's AABB. The float form narrowed
                // item.pos, shifting the exposure sample origins away from
                // vanilla by more the further from the origin you play.
                const double kW = Game::ItemEntity::kWidth;
                const double kH = Game::ItemEntity::kHeight;
                Game::AABBd box;
                box.min = item.pos - glm::dvec3(kW * 0.5, 0.0, kW * 0.5);
                box.max = box.min + glm::dvec3(kW, kH, kW);

                const Game::ExplosionImpact impact =
                    Game::ComputeExplosionImpact(*this, params, item.pos, box, occlusion);
                if (!impact.inRange) continue;

                item.vel += impact.knockback;
                item.needsSync = true;

                if (impact.damage > 0.0f) {
                    item.health -= static_cast<int>(impact.damage);
                    if (item.health <= 0) item.stack.Clear();
                }
            }
        }

        // XP orbs: MC ExperienceOrb has no explosion damage (its hurtServer
        // ignores everything but the void), only the push.
        if (auto* orbs = m_orbs) {
            for (auto& [id, orb] : orbs->AllMutable()) {
                if (outOfReach(orb.pos)) continue;

                // Double, as above and as MC.
                Game::AABBd box;
                box.min = orb.pos - glm::dvec3(0.25, 0.0, 0.25);
                box.max = box.min + glm::dvec3(0.5, 0.5, 0.5);

                const Game::ExplosionImpact impact =
                    Game::ComputeExplosionImpact(*this, params, orb.pos, box, occlusion);
                if (impact.inRange) orb.vel += impact.knockback;
            }
        }
    }

    void ServerLevelBridge::BroadcastExplosion(const glm::dvec3& center, float radius,
                                               int blockCount, bool small) {
        if (!m_sessions) return;

        // MC sends to every player within 64 blocks (distanceToSqr < 4096).
        // Beyond that the particles would be outside the render distance
        // anyway, and a big TNT chain would otherwise packet-storm the server.
        constexpr double kSendRangeSq = 4096.0;

        Network::ExplodeS2CPacket packet;
        packet.center     = center;
        packet.radius     = radius;
        packet.blockCount = blockCount;
        packet.small      = small;

        for (PlayerEntityView* view : m_playerViewList) {
            if (!view) continue;
            if (view->DistanceToSqr(center.x, center.y, center.z) >= kSendRangeSq) continue;

            auto session = m_sessions->GetSessionByConnection(
                static_cast<uint32_t>(view->GetId()));
            if (!session) continue;
            auto* connection = session->GetConnection();
            if (!connection) continue;

            // The player's push travels IN THIS PACKET rather than as a
            // velocity write. Player movement is client-authoritative here, so
            // a server-side velocity on the view would be overwritten by the
            // player's very next move packet — the client has to apply it.
            // PlayerEntityView::AddDeltaMovement parked it during the blast's
            // entity sweep; this drains it.
            glm::dvec3 knockback(0.0);
            view->ConsumePendingKnockback(knockback);
            packet.playerKnockback = glm::vec3(knockback);

            connection->SendPacketIn(Dimension(),
                static_cast<uint8_t>(Network::PacketId::ExplodeS2C),
                Network::Serialization::Serialize(packet));
        }
    }

    void ServerLevelBridge::DestroyBlock(const glm::ivec3& pos, bool dropResources) {
        if (!m_world) return;
        // MC Level.destroyBlock(pos, dropBlock). The sheep passes false, so
        // grazing a fern yields nothing — the wool IS the yield.
        if (dropResources) {
            const Game::BlockID was = m_world->GetBlock(pos.x, pos.y, pos.z);
            if (was != Game::BlockID::Air && m_items) {
                m_items->PopResource(pos, Game::ItemStack(
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

    void ServerLevelBridge::SetBlockState(const glm::ivec3& pos,
                                          Game::BlockState state) {
        if (!m_world) return;
        // MC flag 2 (update clients, no neighbour shape updates) — the flag
        // the rabbit's carrot-age write and the bee's crop-growth write carry
        // in vanilla, and the same combination BlockGrowth's random ticks use.
        m_world->SetBlock(pos.x, pos.y, pos.z, state,
                          Game::World::UpdateFlags::MarkDirty);
    }

    bool ServerLevelBridge::DoMobSpawning() const {
        return m_world ? m_world->GetDoMobSpawning() : true;
    }

    bool ServerLevelBridge::TeleportPlayer(Game::LivingEntity& player,
                                           const glm::dvec3& pos) {
        // The view's id IS the connection id (see the id-space note in
        // HandleInteract). Keep the player's own look; zero the velocity —
        // MC's pearl teleport carries Relative.ROTATION and drops the delta.
        auto* view = dynamic_cast<PlayerEntityView*>(&player);
        if (!view || !m_sessions) return false;
        auto session = m_sessions->GetSessionByConnection(
            static_cast<uint32_t>(view->GetId()));
        if (!session) return false;
        ServerPlayer* serverPlayer = session->GetPlayer();
        auto* connection = session->GetConnection();
        if (!serverPlayer || !connection) return false;

        connection->Teleport(pos.x, pos.y, pos.z,
                             serverPlayer->getYaw(), serverPlayer->getPitch(),
                             0.0, 0.0, 0.0);
        // MC resetFallDistance on arrival — the server-side accumulator half;
        // the client's own resets when it applies the teleport.
        view->ResetFallDistance();
        return true;
    }

    Game::DimensionId ServerLevelBridge::Dimension() const {
        return m_world ? m_world->GetDimension() : Game::DimensionId::Overworld;
    }

    int ServerLevelBridge::GetMinY() const {
        // MC Level.getMinY() = dimensionType().minY().
        return m_world ? Game::DimensionMinY(m_world->GetDimension()) : -64;
    }

    int ServerLevelBridge::GetMaxY() const {
        // MC Level.getMaxY() = getMinY() + getHeight() - 1, and getHeight() is
        // the dimension's logical height: nether 128, end 256, overworld 384.
        if (!m_world) return 319;
        const Game::DimensionId d = m_world->GetDimension();
        return Game::DimensionMinY(d) + Game::DimensionLogicalHeight(d) - 1;
    }

    bool ServerLevelBridge::MobGriefing() const {
        return m_world ? m_world->GetDoMobGriefing() : true;
    }

    Game::Difficulty ServerLevelBridge::GetDifficulty() const {
        return m_world ? m_world->GetDifficulty() : Game::Difficulty::Normal;
    }

    // Each forwards to the world's rule, falling back to MC's default when
    // there is no world — which only happens during teardown.
    bool ServerLevelBridge::TntExplodes() const {
        return m_world ? m_world->GetTntExplodes() : true;
    }
    bool ServerLevelBridge::DoEntityDrops() const {
        return m_world ? m_world->GetDoEntityDrops() : true;
    }
    bool ServerLevelBridge::TntExplosionDropDecay() const {
        return m_world ? m_world->GetTntExplosionDropDecay() : false;
    }
    bool ServerLevelBridge::BlockExplosionDropDecay() const {
        return m_world ? m_world->GetBlockExplosionDropDecay() : true;
    }
    bool ServerLevelBridge::MobExplosionDropDecay() const {
        return m_world ? m_world->GetMobExplosionDropDecay() : true;
    }

    Game::BaseContainerBlockEntity*
    ServerLevelBridge::GetContainerBlockEntity(const glm::ivec3& pos) const {
        if (!m_world) return nullptr;
        const auto cp = Game::Math::WorldCoordinates::WorldToChunkPos(pos.x, pos.z);
        auto chunk = m_world->GetChunk(cp.x, cp.z);
        if (!chunk) return nullptr;
        return dynamic_cast<Game::BaseContainerBlockEntity*>(
            chunk->GetBlockEntity(pos.x - cp.x * 16, pos.y, pos.z - cp.z * 16));
    }

    void ServerLevelBridge::GetContainerBlockEntities(
        const glm::ivec3& center, int chunkRange,
        std::vector<Game::BaseContainerBlockEntity*>& out) const {
        if (!m_world) return;
        const auto cp = Game::Math::WorldCoordinates::WorldToChunkPos(center.x, center.z);
        for (int cz = cp.z - chunkRange; cz <= cp.z + chunkRange; ++cz) {
            for (int cx = cp.x - chunkRange; cx <= cp.x + chunkRange; ++cx) {
                auto chunk = m_world->GetChunk(cx, cz);
                if (!chunk) continue;   // MC getChunkNow: unloaded = skipped
                for (const auto& [local, be] : chunk->GetAllBlockEntities()) {
                    if (auto* container =
                            dynamic_cast<Game::BaseContainerBlockEntity*>(be.get())) {
                        out.push_back(container);
                    }
                }
            }
        }
    }

    void ServerLevelBridge::SpawnItemDrop(const glm::dvec3& pos, uint32_t itemId, int count) {
        if (count <= 0) return;

        // MC popResource: scatter within the cell with a small hop, so the
        // entity's continuous position is floored to its block. THIS level's
        // item manager, not the Game::DropItemStackNear free function — that
        // helper is Overworld-pinned (IntegratedServer::GetItemEntities), so
        // an End enderman's pearl was dropping a dimension away.
        const glm::ivec3 blockPos(static_cast<int>(std::floor(pos.x)),
                                  static_cast<int>(std::floor(pos.y)),
                                  static_cast<int>(std::floor(pos.z)));
        if (m_items) m_items->PopResource(blockPos, Game::ItemStack(itemId, count));
    }

    void ServerLevelBridge::AwardExperience(const glm::dvec3& pos, int amount,
                                            int32_t /*creditPlayerEntityId*/) {
        if (amount <= 0) return;

        // MC ExperienceOrb.award: real orb entities at the award point, split
        // into MC's denominations, that fly to whichever player comes within
        // 8 blocks. The kill credit no longer matters here — it gated whether
        // XP drops at all (the callers check that), not who may collect it.
        //
        // THIS level's manager, not IntegratedServer::GetXpOrbs() — that
        // accessor is Overworld-pinned, and the dragon's death shower was
        // landing a dimension away from its corpse.
        if (m_orbs) m_orbs->Award(pos, amount);
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

            // Only players standing in THIS bridge's dimension.
            //
            // Without the filter every level holds a view of every player, and
            // two things break at once: a Nether mob can target, path toward
            // and attack an Overworld player standing at the same x/z, and the
            // per-level portal tick runs each player against all three worlds
            // — so an Overworld player standing anywhere would be pulled
            // through a Nether portal that happens to occupy the same
            // coordinates.
            if (!m_world ||
                Game::DimensionFromRaw(session->GetDimensionId()) != m_world->GetDimension()) {
                continue;
            }

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
