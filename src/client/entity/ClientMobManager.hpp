// File: src/client/entity/ClientMobManager.hpp
//
// The client's mirror of the server's mobs.
//
// This deliberately constructs the SAME Game::Mob subclasses the server runs,
// against a client-side EntityLevel whose IsClientSide() is true. That single
// flag is what MC uses to split the two sides, and reusing the classes buys the
// client, for free and in guaranteed agreement with the server:
//
//   * gravity, drag, friction and collision (LivingEntity::Travel), so a mob
//     falling between two position packets falls at the right speed instead of
//     sliding down a straight line;
//   * the walk animation (WalkAnimationState) driven by real displacement;
//   * hurtTime, deathTime and the creeper fuse, so those animations play out
//     smoothly rather than stepping at the packet rate.
//
// What it does NOT get is AI: LivingEntity::IsEffectiveAi() is false on the
// client, so ServerAiStep never runs. Goals, navigation and targeting are
// server-only, exactly as in MC.
//
// Server snapshots are applied as MC-style interpolation corrections
// (InterpolationHandler, 3 steps) rather than hard sets — the same approach
// Client::ItemEntityManager already documents.
#pragma once

#include "common/entity/EntityLevel.hpp"
#include "common/entity/Mob.hpp"
#include "common/core/JavaRandom.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <mutex>
#include <vector>

namespace Game { struct IBlockAccess; }

#include "common/core/Features.hpp"
namespace Game::Immersive { struct Portal; }

namespace Client {

    class ClientMobManager;

    // Game::EntityLevel over the client's block view. Most of the interface is
    // inert here: a client mob never spawns anything, never drops items, and
    // never queries players for AI purposes.
    class ClientLevelBridge : public Game::EntityLevel {
    public:
        void SetBlocks(const Game::IBlockAccess* blocks) { m_blocks = blocks; }
        void SetDayTime(int64_t t) { m_dayTime = t; }
        void SetGameTime(int64_t t) { m_gameTime = t; }

        const Game::IBlockAccess* Blocks() const override { return m_blocks; }
        bool IsClientSide() const override { return true; }
        // Published once per frame from the Particles option (see
        // MobParticleSystem::Update); the explosion debris spawner reads it
        // from a worker thread, hence the atomic.
        void SetParticleStatus(Game::ParticleStatus status) {
            m_particleStatus.store(static_cast<uint8_t>(status), std::memory_order_relaxed);
        }
        Game::ParticleStatus GetParticleStatus() const override {
            return static_cast<Game::ParticleStatus>(m_particleStatus.load(std::memory_order_relaxed));
        }
        int64_t GetGameTime() const override { return m_gameTime; }
        int64_t GetDayTime()  const override { return m_dayTime; }
        Game::JavaRandom& Random() override { return m_random; }

        // The client never runs a spawn or AI light test, so a constant is
        // honest here — anything that did read it would be a bug.
        int  GetSkyBrightness(int, int, int) const override { return 15; }
        int  GetMaxLocalRawBrightness(int, int, int) const override { return 15; }
        int  GetMaxLocalRawBrightness(int, int, int, int) const override { return 15; }
        int  GetSkyDarken() const override { return 0; }
        bool CanSeeSky(int, int, int) const override { return true; }
        bool MonstersBurn() const override { return false; }
        bool IsDay() const override { return (m_dayTime % 24000) < 12000; }

        // REAL, unlike the rest of the entity queries: MC's client runs
        // checkCrystals on its own mirror (EnderDragon.aiStep client branch)
        // so the renderer knows which crystal the healing beam attaches to —
        // this scan is that. Out of line: ClientMobManager is incomplete
        // here. Null manager (tests) answers "nothing".
        void SetMobManager(const ClientMobManager* mobs) { m_mobManager = mobs; }
        void GetEntitiesInBox(const Game::AABB& box, const Game::Entity* except,
                              std::vector<Game::Entity*>& out) const override;
        Game::LivingEntity* GetNearestPlayer(double, double, double, double) const override {
            return nullptr;
        }
        void GetPlayers(std::vector<Game::LivingEntity*>&) const override {}
        void BroadcastEntityEvent(const Game::Entity&, uint8_t) override {}

        // ── Particles (MC ClientLevel.addParticle) ─────────────────────────
        //
        // One queued spawn request. The client mobs emit these during their
        // 20 Hz tick (AiStep client branches) and from HandleEntityEvent;
        // Render::MobParticleSystem drains the queue every frame and owns
        // the per-type particle physics. Single-threaded by design: the mob
        // tick, the event handling and the particle Update all run on the
        // main thread (same loop PlatformMain drives), so the queue needs no
        // lock — do NOT push into it from another thread.
        struct QueuedParticle {
            Game::ParticleKind kind;
            double x, y, z;
            double vx, vy, vz;
            // ENTITY_EFFECT tint (MC ColorParticleOption); 1,1,1,1 for
            // everything spawned through the colourless overload.
            float r = 1.0f, g = 1.0f, b = 1.0f, a = 1.0f;
        };

        // ── The queue cap ──────────────────────────────────────────────
        //
        // MobParticleSystem::kMaxParticles is 16384 and it REJECTS anything
        // past it, so a spawn queued beyond that is built, copied and thrown
        // away. That was not a theoretical waste: primed TNT spawns one smoke
        // particle per entity per tick (PrimedTnt::Tick), so a hundred thousand
        // of them queued ~96,000 QueuedParticles — about 6.5 MB of push_back,
        // insert and clear — every tick, of which ~80,000 could never be
        // accepted.
        //
        // Refusing at the source is what the engine already does one step
        // later, just without paying for it first. Which spawns survive is
        // arbitrary either way — MC's ParticleEngine drops at its own cap with
        // no fairness rule either.
        //
        // Not MobParticleSystem::kMaxParticles by name: this header is on the
        // entity side and must not pull in the renderer. Keep the two in sync.
        static constexpr size_t kMaxQueuedParticles = 16384;

        // Mutex-guarded: primed TNT ticks across the worker pool now (see
        // ClientMobManager::Tick) and its smoke plume lands here. The size
        // probe stays outside the lock — a stale read only mis-judges the
        // cap by a few entries, and the recheck inside is exact.
        void AddParticle(Game::ParticleKind kind, double x, double y, double z,
                         double vx, double vy, double vz) override {
            if (m_particleQueue.size() >= kMaxQueuedParticles) return;
            std::lock_guard<std::mutex> lock(m_particleMutex);
            if (m_particleQueue.size() >= kMaxQueuedParticles) return;
            m_particleQueue.push_back({kind, x, y, z, vx, vy, vz,
                                       1.0f, 1.0f, 1.0f, 1.0f});
        }
        void AddColorParticle(Game::ParticleKind kind, double x, double y, double z,
                              double vx, double vy, double vz,
                              float r, float g, float b, float a) override {
            if (m_particleQueue.size() >= kMaxQueuedParticles) return;
            std::lock_guard<std::mutex> lock(m_particleMutex);
            if (m_particleQueue.size() >= kMaxQueuedParticles) return;
            m_particleQueue.push_back({kind, x, y, z, vx, vy, vz, r, g, b, a});
        }

        // Move the queued spawns into `out` (appending) and clear the queue.
        void DrainParticles(std::vector<QueuedParticle>& out) {
            out.insert(out.end(), m_particleQueue.begin(), m_particleQueue.end());
            m_particleQueue.clear();
        }

        size_t QueuedParticleCount() const { return m_particleQueue.size(); }

    private:
        const Game::IBlockAccess* m_blocks = nullptr;
        const ClientMobManager* m_mobManager = nullptr;
        int64_t m_dayTime = 0;
        int64_t m_gameTime = 0;
        Game::JavaRandom m_random{0};
        std::vector<QueuedParticle> m_particleQueue;
        // Guards the queue: primed TNT ticks in parallel and enqueues here.
        std::mutex m_particleMutex;
        // Game::ParticleStatus ordinal; see SetParticleStatus.
        std::atomic<uint8_t> m_particleStatus{0};
    };

    // One mirrored mob, plus the interpolation state layered on top.
    struct ClientMob {
        std::unique_ptr<Game::Mob> mob;

        // MC InterpolationHandler: `steps` remaining corrections toward
        // `target`. Applied at the START of the client tick, before the mob's
        // own physics runs, so local simulation and correction compose instead
        // of fighting.
        int        interpSteps = 0;
        glm::dvec3 targetPosition{0.0};
        float      targetYRot = 0.0f;
        float      targetXRot = 0.0f;
        float      targetYHeadRot = 0.0f;

        // Previous-tick snapshot for sub-tick render interpolation, exactly
        // like RemotePlayer::renderPrev*.
        glm::dvec3 renderPrevPosition{0.0};
        float      renderPrevYRot = 0.0f;
        float      renderPrevXRot = 0.0f;
        float      renderPrevYHeadRot = 0.0f;
        float      renderPrevYBodyRot = 0.0f;

        // Creeper fuse, mirrored so the render can lerp it.
        uint8_t swell = 0, oldSwell = 0;

        // MC LivingEntity's swimAmount — the client-side 0..1 ramp behind the
        // drowned's whole-body swim tilt (DrownedRenderer.setupRotations).
        // ±0.09 per tick, clamped (LivingEntity.baseTick:3305-3310). Lives on
        // the client mirror because the shared LivingEntity does not carry
        // it; only the drowned ticks it (see Tick).
        float swimAmount = 0.0f, swimAmountO = 0.0f;

        // The riding link the server last announced (AddEntityS2C /
        // SetEntityDataS2C vehicleId; -1 = none). Kept as an ID and re-resolved
        // against m_mobs at the top of every Tick rather than applied eagerly,
        // because the packets for a jockey pair can arrive in either order —
        // the rider's link may name a vehicle whose AddEntity is still one
        // packet behind. Resolution is idempotent and self-healing.
        int32_t wantedVehicleId = -1;

        // Slot in ClientMobManager::m_mobList (dense pointer list the tick
        // iterates instead of the node map); maintained by Spawn/Remove.
        size_t listIndex = 0;
        // Slot in m_modelMobList, or SIZE_MAX for the block-shaped entities
        // (TNT, falling block) that are not in it. See ModelMobList().
        size_t modelListIndex = static_cast<size_t>(-1);
        // This entry's own entity id, so list-driven passes need no map key.
        int32_t selfId = 0;
    };

    // What BlockCubeEntityRenderer needs to draw one entity, written by the
    // client tick into a dense array that mirrors m_mobList slot for slot.
    //
    // The renderer used to walk the entity objects themselves every frame —
    // an entry node, then the Mob behind it, then its state — which at 176k
    // falling blocks was ~165 ns of cache misses per entity per frame even
    // across the worker pool (3.2 ms a frame). The tick already touches every
    // entity, so it leaves this 64-byte summary behind and the renderer reads
    // memory sequentially instead. Positions stay double so the sub-tick
    // interpolation is exactly what the entity path computed.
    //
    // `drawable` is false for every entity that is not a primed TNT or a
    // falling block; those slots exist only so the indices line up.
    struct BlockEntityProxy {
        glm::dvec3 prevPos{0.0};
        glm::dvec3 pos{0.0};
        glm::vec3  half{0.0f};
        uint32_t   stateRaw = 0;
        int32_t    fuse = 0;
        uint8_t    type = 0;
        uint8_t    onGround = 0;
        uint8_t    drawable = 0;
    };

    class ClientMobManager {
    public:
        ClientMobManager() { m_level.SetMobManager(this); }

        static constexpr int kInterpSteps = 3;
        // Past this the mob is snapped rather than interpolated — a correction
        // that large is a teleport or a missed packet, and easing into it would
        // send the mob sliding across the world.
        static constexpr double kSnapDistanceSq = 16.0 * 16.0;

        void SetBlockAccess(const Game::IBlockAccess* blocks) { m_level.SetBlocks(blocks); }
        void SetTime(int64_t gameTime, int64_t dayTime) {
            m_level.SetGameTime(gameTime);
            m_level.SetDayTime(dayTime);
        }

        // Packet entry points.
        // `blockStateRaw` is AddEntityS2C's per-type data int: the block a
        // falling block or a primed TNT carries. Zero for everything else, and
        // zero is air's default state, so a type that ignores it is unaffected.
        void Spawn(int32_t id, uint16_t type, const glm::dvec3& pos, const glm::vec3& vel,
                   float yRot, float xRot, float yHeadRot,
                   float health, uint8_t flags, uint8_t variantData,
                   uint8_t pose, uint8_t animState, uint32_t blockStateRaw = 0);
        void MoveDelta(int32_t id, bool hasPos, const glm::dvec3& delta,
                       bool hasRot, float yRot, float xRot, float yHeadRot, bool onGround);
        void Teleport(int32_t id, const glm::dvec3& pos, const glm::vec3& vel,
                      float yRot, float xRot, float yHeadRot, bool onGround);
        void SetMotion(int32_t id, const glm::vec3& vel);
        // MC DATA_BEAM_TARGET, arriving as EndCrystalBeamS2C — see
        // DragonPackets.hpp. No-op for anything that is not an End crystal.
        void SetEndCrystalBeam(int32_t id, bool hasTarget, const glm::ivec3& target);
        void SetData(int32_t id, float health, uint8_t flags, uint8_t variantData,
                     uint8_t hurtTime, uint8_t deathTime, uint8_t swellDir, uint8_t swell,
                     uint8_t pose, uint8_t animState);
        void HandleEvent(int32_t id, uint8_t event);
        // The riding link for one mob (vehicleId -1 = dismount). Fed by the
        // appended field on AddEntityS2C / SetEntityDataS2C via
        // Client::ApplyMobVehicleLink (defined in the .cpp — see the seam note
        // in S2CPackets.hpp).
        void SetVehicle(int32_t passengerId, int32_t vehicleId);
        void Remove(int32_t id);
#if ENABLE_IMMERSIVE_PORTALS
        // A mob that just spawned here (`id`, already in this store) is the
        // same one that was `from` in another level a moment ago, having
        // crossed `via` (a portal of that level leading here). Carry its
        // render state through the portal so the frame's interpolation
        // continues across the surface — see ItemEntityManager::CarryOver.
        void CarryOver(int32_t id, const ClientMob& from, const Game::Immersive::Portal& via);
#endif
        void Clear();

        // 20 Hz client tick.
        void Tick();

        const std::unordered_map<int32_t, ClientMob>& All() const { return m_mobs; }
        size_t Count() const { return m_mobs.size(); }

        // See the pick-candidate note in Tick(): set before each Tick, read
        // by ClientPlayerController::PickEntity every frame.
        void SetPickOrigin(const glm::dvec3& origin) { m_pickOrigin = origin; }
        const std::vector<int32_t>& PickCandidates() const { return m_pickCandidates; }
        // Dense entry list for render-side walks (never mutate through it).
        const std::vector<ClientMob*>& MobList() const { return m_mobList; }
        // The subset MobRenderer draws — everything that is NOT a primed TNT
        // or a falling block. Those two are BlockCubeEntityRenderer's, and at
        // a hundred thousand of them the model renderer spent 1.8 ms a frame
        // walking the full list to skip them (a cache miss per entry).
        const std::vector<ClientMob*>& ModelMobList() const { return m_modelMobList; }
        // Slot-for-slot mirror of MobList() — see BlockEntityProxy. Refreshed
        // by Tick; kept in step by Spawn/Remove between ticks, so a block that
        // starts falling draws on the very frame its entity arrives.
        const std::vector<BlockEntityProxy>& BlockProxies() const { return m_blockProxies; }
        const ClientMob* GetMob(int32_t id) const {
            const auto it = m_mobs.find(id);
            return it == m_mobs.end() ? nullptr : &it->second;
        }

        // Particle spawns the client mobs queued this tick — see
        // ClientLevelBridge. Render::MobParticleSystem drains this each
        // frame (main thread only).
        void DrainParticles(std::vector<ClientLevelBridge::QueuedParticle>& out) {
            m_level.DrainParticles(out);
        }

        // The Particles option, published once per frame by
        // Render::MobParticleSystem::Update so the explosion debris spawner
        // (which reads it through EntityLevel::GetParticleStatus) sees the
        // same value the limiter uses.
        void SetParticleStatus(Game::ParticleStatus status) {
            m_level.SetParticleStatus(status);
        }

        // The last decoded position for an id, kept so MoveEntity deltas can be
        // accumulated against the same base the server encoded against. MC
        // keeps this in the entity's VecDeltaCodec; here it is explicit because
        // the delta must NOT be applied to the locally-simulated position,
        // which has drifted since the last packet.
        bool GetCodecBase(int32_t id, glm::dvec3& out) const;

        // The EntityLevel the client's mobs and particles run against. Exposed
        // so the ExplodeS2C handler can spawn a blast's particles: the blast
        // has no entity to hang them off, because the TNT that produced it was
        // discarded server-side a tick before the packet went out.
        Game::EntityLevel& Level() { return m_level; }

        // The type of a mob the client knows, Count when it does not (pick
        // block on an entity asks for its spawn egg).
        Game::EntityTypeId EntityTypeOf(int32_t id) {
            ClientMob* cm = Find(id);
            return (cm && cm->mob) ? cm->mob->GetType() : Game::EntityTypeId::Count;
        }

        // The synced size of a mob (Entity::scale), applied by the packet
        // handler on add and on every data update.
        void SetEntityScale(int32_t id, float scale) {
            if (ClientMob* cm = Find(id); cm && cm->mob) cm->mob->scale = scale;
        }

    private:
        ClientMob* Find(int32_t id);

        // Mirror of MobManager::TickPassengerChain — after a vehicle's client
        // tick, its riders get their interpolation (ROTATION only — position
        // is vehicle-derived) and RideTick, recursively.
        void TickPassengerChain(Game::Entity& vehicle);

        ClientLevelBridge m_level;
        std::unordered_map<int32_t, ClientMob> m_mobs;
        std::unordered_map<int32_t, glm::dvec3> m_codecBase;
        // Deferred TNT ticks for the parallel batch; persistent for capacity.
        std::vector<Game::Mob*> m_tntTickBatch;
        // Dense entry list, maintained incrementally — a million-node map
        // walk per tick was most of the client tick by itself.
        std::vector<ClientMob*> m_mobList;
        std::vector<ClientMob*> m_modelMobList;
        std::vector<BlockEntityProxy> m_blockProxies;
        std::vector<ClientMob*> m_serialScratch;

        static void FillProxy(BlockEntityProxy& proxy, const ClientMob& entry);
        // Swap-pop slot `idx` of m_mobList AND m_blockProxies together.
        void ListSwapPop(size_t idx);

        // m_modelMobList maintenance, mirrored wherever m_mobList's is.
        void ModelListAdd(ClientMob& entry);
        void ModelListRemove(ClientMob& entry);
        // Non-TNT mobs near the pick origin, rebuilt each tick (ids, not
        // pointers: a removal packet between ticks frees the mob).
        std::vector<int32_t> m_pickCandidates;
        glm::dvec3 m_pickOrigin{0.0};

        // Ids observed removed during a tick, drained by that tick's sweep.
        // A member, not a local, for two reasons: TickPassengerChain writes to
        // it as well, and the buffer is reused so a detonation does not
        // allocate one per tick. Ids may repeat and may name an already-erased
        // mob — Tick's sweep re-checks both.
        std::vector<int32_t> m_removedThisTick;
    };

    // Bound-level pointer, owned by ClientLevel (see ClientLevel.hpp).
    extern ClientMobManager* g_clientMobManager;

} // namespace Client
