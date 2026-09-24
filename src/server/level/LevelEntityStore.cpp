// File: src/server/level/LevelEntityStore.cpp
#include "server/level/LevelEntityStore.hpp"

#include "server/level/ServerLevel.hpp"
#include "server/entity/ExperienceOrbManager.hpp"
#include "server/entity/ItemEntityManager.hpp"
#include "server/entity/MobManager.hpp"
#include "server/entity/ServerLevelBridge.hpp"
#include "server/world/ChunkProvider.hpp"
#include "server/world/ServerWorkerPool.hpp"
#include "server/world/storage/anvil/EntityChunkNbt.hpp"
#include "server/world/storage/anvil/EntityNbt.hpp"
#include "common/core/Profiling_Tracy.hpp"

#include "common/core/Log.hpp"
#include "common/core/SaveVersion.hpp"
#include "common/entity/EntityType.hpp"
#include "common/entity/Mob.hpp"
#include "common/entity/SpawnReason.hpp"
#include "common/nbt/NbtWrite.hpp"
#include "common/world/level/World.hpp"

#include <chrono>
#include <cmath>
#include <deque>
#include <mutex>
#include <thread>

namespace Server {

    namespace {

        Game::Math::ChunkPos ChunkOf(const glm::dvec3& pos) {
            return Game::Math::ChunkPos(
                static_cast<int>(std::floor(pos.x)) >> 4,
                static_cast<int>(std::floor(pos.z)) >> 4);
        }

        Game::ChunkProvider* ProviderOf(ServerLevel& level) {
            Game::World* world = level.World();
            return world ? world->GetChunkProvider() : nullptr;
        }

        // Build one saved mob and everything riding it, mounting as it goes.
        // Returns the mob it added, or nullptr if the mob itself could not be
        // restored (a rider that fails is counted as skipped and the vehicle
        // still loads — losing a jockey must not lose the chicken).
        //
        // The order is vanilla's and it matters: a rider is positioned off its
        // vehicle's LIVE position on the next tick, so the vehicle has to exist
        // and be placed before StartRiding runs. `force` is passed because the
        // seat rules were already satisfied when this jockey was created — a
        // later tightening of CanRide must never silently unstack saved mobs.
        Game::Mob* RestoreMobTree(Game::Anvil::LoadedEntity& entry,
                                  MobManager& mobs, ServerLevelBridge& bridge,
                                  size_t& restored, size_t& skipped) {
            auto tag = std::dynamic_pointer_cast<::World::NBTTagCompound>(entry.mobTag);
            if (!tag) return nullptr;

            std::unique_ptr<Game::Mob> mob =
                MakeMobForLoad(entry.mobType, static_cast<Game::EntityLevel*>(&bridge));
            if (!mob) return nullptr;

            Game::Anvil::ApplyMobNbt(*tag, *mob);

            // Already here? A chunk can be asked to load while its entities are
            // live — a re-entry, or two code paths both making it live. Without
            // this the world fills with clones.
            if (!Game::UuidIsNil(mob->GetUuid()) && mobs.HasUuid(mob->GetUuid())) {
                return nullptr;
            }

            const int32_t id = mobs.Add(std::move(mob));
            if (id == 0) return nullptr;
            Game::Mob* vehicle = mobs.Find(id);
            if (!vehicle) return nullptr;
            ++restored;

            for (auto& rider : entry.passengers) {
                Game::Mob* mounted = RestoreMobTree(rider, mobs, bridge, restored, skipped);
                if (!mounted) { ++skipped; continue; }
                mounted->StartRiding(*vehicle, /*force=*/true);
            }
            return vehicle;
        }

        // MC `entity instanceof Mob` for a type this pipeline builds. Every
        // non-MISC category is a Mob; of the MISC types, the golems and the
        // villager are (Mob subclasses filed under MISC), everything else —
        // the armor stand, projectiles, the end crystal, falling blocks,
        // primed TNT — is a plain Entity or LivingEntity.
        bool IsMcMob(Game::EntityTypeId type) {
            switch (type) {
                case Game::EntityTypeId::Villager:
                case Game::EntityTypeId::IronGolem:
                case Game::EntityTypeId::SnowGolem:
                case Game::EntityTypeId::CopperGolem:
                    return true;
                default:
                    return Game::GetEntityTypeInfo(type).category != Game::MobCategory::Misc;
            }
        }

    } // namespace

    // ── MC IOWorker, for entities/*.mca ──────────────────────────────────────
    //
    // The server thread encodes a chunk's entities (MC does too:
    // EntityStorage.storeEntities builds the tag on the main thread) and hands
    // the bytes over; compressing them and writing the region file happen on
    // a world-I/O job. Before this, SaveChunk did the zlib and the region
    // write on the server thread for every chunk it unloaded.
    //
    // MC IOWorker's two guarantees, kept:
    //   - one writer, writes in order: one Drain at a time, and a newer store
    //     for a chunk replaces the older one still waiting;
    //   - reads see pending writes (IOWorker.loadAsync checks pendingWrites
    //     first): a chunk unloaded and loaded again before its write lands
    //     reads what was queued, not the stale file. An entry leaves the
    //     pending map only once its bytes are on disk.
    //
    // Reads run here too (MC EntityStorage.loadEntities -> IOWorker.loadAsync):
    // the region read and inflate on world-I/O jobs, the result into an inbox
    // the server thread drains (LevelEntityStore::ProcessPendingLoads, MC
    // PersistentEntitySectionManager.processPendingLoads). They ran on the
    // server thread inside the watch-set scan: a join storm of 50 players at
    // 50 saved positions read 3,600 chunks in one tick — a 2.8 s tick
    // (Tracy, 2026-09-24).
    class EntityIoWorker : public std::enable_shared_from_this<EntityIoWorker> {
    public:
        explicit EntityIoWorker(Game::ChunkProvider* provider) : m_provider(provider) {}

        struct LoadResult {
            Game::Math::ChunkPos pos{0, 0};
            uint64_t ticket = 0;
            std::vector<uint8_t> nbt;
            bool ok = false;
            std::string error;
        };

        // Server thread: queue a read of the chunk's entities. At most
        // kMaxReadJobs jobs drain the queue, so a burst of thousands of reads
        // is a couple of jobs in the worker pool rather than thousands.
        void RequestRead(Game::Math::ChunkPos pos, uint64_t ticket) {
            bool submit = false;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_closed) return;
                m_reads.push_back(ReadRequest{pos, ticket});
                if (m_readJobs < kMaxReadJobs) {
                    ++m_readJobs;
                    submit = true;
                }
            }
            if (!submit) return;
            if (Threading::g_serverWorkerPool) {
                std::shared_ptr<EntityIoWorker> self = shared_from_this();
                Threading::g_serverWorkerPool->SubmitWorldIOJob([self] { while (self->ReadOne(true)) {} });
            } else {
                while (ReadOne(true)) {}   // no pool (shutdown, tools): read inline
            }
        }

        // Server thread: the reads finished since the last call.
        void TakeResults(std::vector<LoadResult>& out) {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_results.empty()) return;
            out.swap(m_results);
            m_results.clear();
        }

        // Any read queued, running, or finished and not yet taken.
        bool ReadsOutstanding() const {
            std::lock_guard<std::mutex> lock(m_mutex);
            return !m_reads.empty() || m_readsRunning > 0 || !m_results.empty();
        }

        // Server thread: run the queued reads here, now (the shutdown save,
        // which must not wait on a pool that may be winding down).
        void RunQueuedReadsInline() {
            while (ReadOne(false)) {}
        }

        // Queue the chunk's next write: uncompressed NBT, or EMPTY to clear
        // the chunk from the region file.
        void Store(Game::Math::ChunkPos pos, std::vector<uint8_t> nbt) {
            bool schedule = false;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_closed) return;
                Entry& e = m_pending[pos];
                e.nbt = std::move(nbt);
                e.seq = ++m_seq;
                if (!m_scheduled) {
                    m_scheduled = true;
                    schedule = true;
                }
            }
            if (!schedule) return;
            if (Threading::g_serverWorkerPool) {
                std::shared_ptr<EntityIoWorker> self = shared_from_this();
                Threading::g_serverWorkerPool->SubmitWorldIOJob([self] { self->Drain(); });
            } else {
                Drain();   // no pool (shutdown, tools): write inline
            }
        }

        // The newest write queued for the chunk. False: nothing pending, read
        // the file. True with `cleared`: the chunk is about to be cleared.
        bool PendingRead(Game::Math::ChunkPos pos, std::vector<uint8_t>& out, bool& cleared) const {
            std::lock_guard<std::mutex> lock(m_mutex);
            const auto it = m_pending.find(pos);
            if (it == m_pending.end()) return false;
            cleared = it->second.nbt.empty();
            if (!cleared) out = it->second.nbt;
            return true;
        }

        // Write everything queued. The background job, and Flush.
        void Drain() {
            std::lock_guard<std::mutex> writeLock(m_writeMutex);
            for (;;) {
                Game::Math::ChunkPos pos{0, 0};
                std::vector<uint8_t> nbt;
                uint64_t seq = 0;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    if (m_pending.empty()) {
                        m_scheduled = false;
                        return;
                    }
                    const auto it = m_pending.begin();
                    pos = it->first;
                    nbt = it->second.nbt;
                    seq = it->second.seq;
                }
                Write(pos, nbt);
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    const auto it = m_pending.find(pos);
                    // A newer store arrived during the write: it stays queued
                    // and goes out on a later pass of this loop.
                    if (it != m_pending.end() && it->second.seq == seq) m_pending.erase(it);
                }
            }
        }

        // Write what is left, then accept nothing more. After this the
        // provider may go; a job still queued finds nothing to do.
        void Close() {
            Drain();
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_closed = true;
                m_reads.clear();
            }
            // Wait out a read or write in progress on a worker, then refuse
            // any more: the provider may be destroyed after this returns.
            std::lock_guard<std::mutex> io(m_ioMutex);
            m_ioClosed = true;
        }

    private:
        struct Entry {
            std::vector<uint8_t> nbt;
            uint64_t seq = 0;
        };
        struct ReadRequest {
            Game::Math::ChunkPos pos{0, 0};
            uint64_t ticket = 0;
        };
        static constexpr int kMaxReadJobs = 2;

        // Pop one queued read and perform it. False when the queue is empty;
        // a job (`asJob`) then retires under the same lock, so a read queued
        // after its last pop always finds a job slot free.
        bool ReadOne(bool asJob) {
            ReadRequest request;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_reads.empty()) {
                    if (asJob) --m_readJobs;
                    return false;
                }
                request = m_reads.front();
                m_reads.pop_front();
                ++m_readsRunning;
            }
            LoadResult result;
            result.pos = request.pos;
            result.ticket = request.ticket;
            {
                PROFILE_ZONE_N("EntityLoad.Read");
                // A write still queued for this chunk is the truth, not the
                // file. No new write can be queued for it while its read is
                // outstanding: LevelEntityStore saves only Loaded chunks.
                bool cleared = false;
                if (PendingRead(request.pos, result.nbt, cleared)) {
                    result.ok = !cleared;   // cleared = "not on disk", an empty error
                } else {
                    std::lock_guard<std::mutex> io(m_ioMutex);
                    if (!m_ioClosed) {
                        result.ok = m_provider->ReadEntityChunkNbt(request.pos, result.nbt, result.error);
                    }
                }
            }
            std::lock_guard<std::mutex> lock(m_mutex);
            m_results.push_back(std::move(result));
            --m_readsRunning;
            return true;
        }

        void Write(Game::Math::ChunkPos pos, const std::vector<uint8_t>& nbt) {
            PROFILE_ZONE_N("EntityIo.Write");
            std::lock_guard<std::mutex> io(m_ioMutex);
            if (m_ioClosed) return;
            std::string error;
            bool ok = true;
            if (nbt.empty()) {
                ok = m_provider->ClearEntityChunk(pos, error) || error.empty();
            } else {
                std::vector<uint8_t> packed;
                if (!Game::Nbt::ZlibCompress(nbt, packed)) {
                    ok = false;
                    error = "zlib failed";
                } else {
                    ok = m_provider->WriteEntityChunkNbt(pos, packed, error) || error.empty();
                }
            }
            if (!ok) {
                Log::Error("[Anvil] entity write failed for chunk (%d,%d): %s", pos.x, pos.z, error.c_str());
            }
        }

        Game::ChunkProvider* m_provider;
        mutable std::mutex m_mutex;
        std::mutex m_writeMutex;
        // Held for each single read or write of the region files, so Close
        // can wait one out and then shut the door on the provider.
        std::mutex m_ioMutex;
        bool m_ioClosed = false;
        std::unordered_map<Game::Math::ChunkPos, Entry, Game::Math::ChunkPosHash> m_pending;
        uint64_t m_seq = 0;
        bool m_scheduled = false;
        bool m_closed = false;
        std::deque<ReadRequest> m_reads;
        std::vector<LoadResult> m_results;
        int m_readJobs = 0;
        int m_readsRunning = 0;
    };

    LevelEntityStore::~LevelEntityStore() {
        if (m_io) m_io->Close();
    }

    EntityIoWorker& LevelEntityStore::Io() {
        if (!m_io) m_io = std::make_shared<EntityIoWorker>(ProviderOf(m_level));
        return *m_io;
    }

    void LevelEntityStore::Flush() {
        if (m_io) m_io->Drain();
    }

    void LevelEntityStore::RequestLoad(Game::Math::ChunkPos pos) {
        Game::ChunkProvider* provider = ProviderOf(m_level);
        if (!provider) return;
        if (!provider->EntityPersistenceEnabled()) {
            // Nothing to read, but the chunk's worldgen mobs still join the
            // level. Idempotent: the provider hands each list out once.
            AddWorldgenEntities(pos);
            return;
        }

        // Already asked, or already ours.
        const auto it = m_state.find(pos);
        if (it != m_state.end() && it->second != State::Absent) return;

        // MC PersistentEntitySectionManager.requestChunkLoad: PENDING, and the
        // read goes to the I/O worker; ProcessPendingLoads adopts the result.
        // Until then the chunk's entities cannot be saved and the chunk cannot
        // unload (ReadyToUnload), so nothing live in it is ever written over
        // or dropped.
        m_state[pos] = State::Pending;
        const uint64_t ticket = ++m_loadTicketSeq;
        m_loadTickets[pos] = ticket;
        Io().RequestRead(pos, ticket);
    }

    void LevelEntityStore::ProcessPendingLoads() {
        if (!m_io) return;
        std::vector<EntityIoWorker::LoadResult> results;
        m_io->TakeResults(results);
        if (results.empty()) return;
        PROFILE_ZONE_N("EntityLoad.Apply");
        for (EntityIoWorker::LoadResult& r : results) {
            // Only the answer to the request still outstanding: a chunk
            // forgotten (or forgotten and asked again) meanwhile ignores it.
            const auto state = m_state.find(r.pos);
            const auto ticket = m_loadTickets.find(r.pos);
            if (state == m_state.end() || state->second != State::Pending ||
                ticket == m_loadTickets.end() || ticket->second != r.ticket) {
                continue;
            }
            m_loadTickets.erase(ticket);
            if (!r.ok && !r.error.empty()) {
                // Say the consequence, not just the failure. This path is
                // deliberately non-fatal — the chunk still loads and the world
                // still plays — but the entities that were on disk are now
                // dropped, and the next save of this chunk overwrites them. A
                // player who sees only "read failed" has no way to know they
                // are one autosave away from losing whatever was in there.
                Log::Error("[Anvil] entity read failed for chunk (%d,%d): %s", r.pos.x, r.pos.z,
                           r.error.c_str());
                Log::Error("[Anvil]   chunk (%d,%d) loads with NO entities; anything stored "
                           "there is dropped and will be overwritten on the next save of it",
                           r.pos.x, r.pos.z);
            }
            ApplyLoadResult(r.pos, r.nbt, r.ok);

            // Only now, with the chunk Loaded: a mob added while it was Absent
            // would be written over whatever the region file holds.
            AddWorldgenEntities(r.pos);
        }
    }

    void LevelEntityStore::CompleteLoads() {
        if (!m_io) return;
        m_io->RunQueuedReadsInline();
        while (m_io->ReadsOutstanding()) {
            ProcessPendingLoads();
            // What is left is running on a worker: a single region read.
            if (m_io->ReadsOutstanding()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ProcessPendingLoads();
    }

    bool LevelEntityStore::IsPending(Game::Math::ChunkPos pos) const {
        const auto it = m_state.find(pos);
        return it != m_state.end() && it->second == State::Pending;
    }

    std::unordered_set<Game::Math::ChunkPos, Game::Math::ChunkPosHash> LevelEntityStore::OccupiedChunks() const {
        std::unordered_set<Game::Math::ChunkPos, Game::Math::ChunkPosHash> out;
        if (MobManager* mobs = m_level.Mobs()) {
            for (const auto& [id, mob] : mobs->All()) if (mob) out.insert(ChunkOf(mob->position));
        }
        if (ItemEntityManager* items = m_level.Items()) {
            for (const auto& [id, item] : items->All()) out.insert(ChunkOf(item.pos));
        }
        if (ExperienceOrbManager* orbs = m_level.Orbs()) {
            for (const auto& [id, orb] : orbs->All()) out.insert(ChunkOf(orb.pos));
        }
        return out;
    }

    bool LevelEntityStore::ReadyToUnload(Game::Math::ChunkPos pos, bool holdsEntities) {
        Game::ChunkProvider* provider = ProviderOf(m_level);
        if (!provider || !provider->EntityPersistenceEnabled()) return true;
        const auto it = m_state.find(pos);
        const State state = it == m_state.end() ? State::Absent : it->second;
        // MC PersistentEntitySectionManager.storeChunkSections, which an
        // unload must pass: PENDING cannot be stored yet; FRESH with entities
        // first reads what the file holds (they merge on arrival); FRESH and
        // empty, or LOADED, can go.
        switch (state) {
            case State::Loaded:  return true;
            case State::Pending: return false;
            case State::Absent:
                if (!holdsEntities) return true;
                RequestLoad(pos);
                return false;
        }
        return true;
    }

    void LevelEntityStore::AddWorldgenEntities(Game::Math::ChunkPos pos) {
        Game::ChunkProvider* provider = ProviderOf(m_level);
        if (!provider) return;
        std::vector<Game::WorldgenEntity> pending = provider->TakeWorldgenEntities(pos);
        if (pending.empty()) return;

        MobManager*        mobs   = m_level.Mobs();
        ServerLevelBridge* bridge = m_level.MobLevel();
        if (!mobs || !bridge) return;

        PROFILE_ZONE_N("EntityLoad.Worldgen");
        size_t added = 0, skipped = 0;
        for (const Game::WorldgenEntity& entry : pending) {
            std::shared_ptr<::World::NBTTagCompound> tag;
            try {
                tag = std::dynamic_pointer_cast<::World::NBTTagCompound>(
                    ::World::NBTParser::Parse(entry.nbt));
            } catch (const std::exception& e) {
                Log::Error("[Worldgen] chunk (%d,%d): unreadable structure entity: %s",
                           pos.x, pos.z, e.what());
            }
            if (!tag) { ++skipped; continue; }

            // MC StructureTemplate.createEntityIgnoreException /
            // EntityType.create: an id this build cannot construct is dropped
            // quietly (a TF gazebo's text_display, an item frame), the same as
            // a failed create in Java.
            Game::EntityTypeId type{};
            if (Game::Anvil::ClassifyEntity(*tag, type) != Game::Anvil::EntityKind::Mob) {
                ++skipped;
                continue;
            }
            std::unique_ptr<Game::Mob> mob =
                MakeMobForLoad(type, static_cast<Game::EntityLevel*>(bridge));
            if (!mob) { ++skipped; continue; }

            // EntityType.create loads the compound (Entity.load: Pos,
            // Rotation, and Rotation's yaw copied to the head and body), then
            // the placer snaps it there and sets both yaws again.
            Game::Anvil::ApplyMobNbt(*tag, *mob);
            mob->yHeadRot = mob->yHeadRotO = mob->yRot;
            mob->yBodyRot = mob->yBodyRotO = mob->yRot;

            // Mob.finalizeSpawn(level, getCurrentDifficultyAt(pos),
            // STRUCTURE, null) — the pool elements' and the pieces' spawns;
            // the igloo's template pair and the end city's shulkers skip it.
            // Only for what is a Mob in MC (`entity instanceof Mob`): the
            // taiga armorer's armor stands ride this pipeline but are not.
            if (entry.finalizeSpawn && IsMcMob(type)) {
                mob->FinalizeSpawn(Game::SpawnReason::Structure, nullptr);
            }

            if (mobs->Add(std::move(mob)) != 0) ++added;
            else ++skipped;
        }

        if (added > 0 || skipped > 0) {
            Log::Info("[Worldgen] chunk (%d,%d): %zu structure entities added%s",
                      pos.x, pos.z, added,
                      skipped ? (" (" + std::to_string(skipped) + " skipped)").c_str() : "");
        }
    }

    void LevelEntityStore::ApplyLoadResult(Game::Math::ChunkPos pos,
                                           const std::vector<uint8_t>& nbt,
                                           bool readSucceeded) {
        // Whatever happens below, the managers are authoritative from here on.
        // Leaving it Pending would mean this chunk could never be saved.
        m_state[pos] = State::Loaded;

        if (!readSucceeded || nbt.empty()) return;   // nothing on disk yet
        m_onDisk.insert(pos);

        MobManager*           mobs  = m_level.Mobs();
        ItemEntityManager*    items = m_level.Items();
        ExperienceOrbManager* orbs  = m_level.Orbs();
        ServerLevelBridge*    bridge = m_level.MobLevel();
        if (!mobs || !items || !orbs || !bridge) return;

        std::vector<Game::Anvil::LoadedEntity> loaded;
        size_t skipped = 0;
        std::string error;
        // Own scope: two ZoneScopedN in one scope is a redefinition of
        // ___tracy_scoped_zone, and the adopt loop below needs its own.
        bool deserialised = false;
        {
            PROFILE_ZONE_N("EntityLoad.Deserialise");
            deserialised = Game::Anvil::DeserialiseEntityChunk(
                nbt, pos, static_cast<Game::EntityLevel*>(bridge), loaded, skipped, error);
        }
        if (!deserialised) {
            Log::Error("[Anvil] entity chunk (%d,%d): %s", pos.x, pos.z, error.c_str());
            return;
        }

        size_t restored = 0;
        PROFILE_ZONE_N("EntityLoad.Adopt");
        for (auto& entry : loaded) {
            switch (entry.kind) {
                case Game::Anvil::LoadedEntity::Kind::Item:
                    items->Adopt(std::move(entry.item));
                    ++restored;
                    break;
                case Game::Anvil::LoadedEntity::Kind::Orb:
                    orbs->Adopt(std::move(entry.orb));
                    ++restored;
                    break;
                case Game::Anvil::LoadedEntity::Kind::Mob:
                    // Recursive: a vehicle brings its riders (and theirs) with
                    // it, mounted, before the next root entity is touched.
                    if (!RestoreMobTree(entry, *mobs, *bridge, restored, skipped)) {
                        ++skipped;
                    }
                    break;
            }
        }

        if (restored > 0 || skipped > 0) {
            Log::Info("[Anvil] chunk (%d,%d): restored %zu entities%s",
                      pos.x, pos.z, restored,
                      skipped ? (" (" + std::to_string(skipped) + " skipped)").c_str() : "");
        }
    }

    bool LevelEntityStore::SaveChunk(Game::Math::ChunkPos pos, std::string& error) {
        return SaveChunkWith(pos, nullptr, error);
    }

    bool LevelEntityStore::SaveChunkWith(Game::Math::ChunkPos pos,
                                         const Game::Anvil::EntityChunkContents* gathered,
                                         std::string& error) {
        Game::ChunkProvider* provider = ProviderOf(m_level);
        if (!provider || !provider->EntityPersistenceEnabled()) { error.clear(); return false; }

        // Gather FIRST, then decide. What we hold for this chunk is the thing
        // that determines whether refusing to write is safe.
        auto gather = [&](Game::Anvil::EntityChunkContents& out) {
            out = {};
            if (MobManager* mobs = m_level.Mobs()) {
                for (const auto& [id, mob] : mobs->All()) {
                    if (mob && ChunkOf(mob->position) == pos) out.mobs.push_back(mob.get());
                }
            }
            if (ItemEntityManager* items = m_level.Items()) {
                for (const auto& [id, item] : items->All()) {
                    if (ChunkOf(item.pos) == pos) out.items.push_back(&item);
                }
            }
            if (ExperienceOrbManager* orbs = m_level.Orbs()) {
                for (const auto& [id, orb] : orbs->All()) {
                    if (ChunkOf(orb.pos) == pos) out.orbs.push_back(&orb);
                }
            }
        };

        Game::Anvil::EntityChunkContents contents;
        if (gathered) contents = *gathered;
        else gather(contents);

        auto claimed = [&] {
            const auto it = m_state.find(pos);
            return it != m_state.end() && it->second == State::Loaded;
        };

        if (!claimed()) {
            // An UNCLAIMED chunk is one whose entities/*.mca entry we have
            // never read. Writing it blind would stamp whatever we happen to
            // hold over the top of entities still on disk.
            //
            // But refusing outright is what silently ate dropped items: a
            // chunk can acquire live entities without ever having been
            // claimed (it reached the cache without going through the async
            // result queue, so nothing called RequestLoad), and then every
            // save politely declined to write them.
            //
            // So: if we hold nothing, decline — there is nothing to lose and
            // clearing unread data would be wrong. If we DO hold something,
            // claim the chunk: RequestLoad reads the file and merges what was
            // there into the managers, after which writing is correct rather
            // than destructive.
            //
            // The read is asynchronous (MC storeChunkSections: FRESH with
            // entities -> requestChunkLoad, not saved this time), so this save
            // declines and the next one — or the unload, which waits for the
            // read (ReadyToUnload) — writes the merged list. A Pending chunk
            // declines quietly: its read is already on the way.
            if (contents.Empty()) { error.clear(); return false; }
            const auto st = m_state.find(pos);
            if (st == m_state.end() || st->second == State::Absent) {
                Log::Warning("[Anvil] chunk (%d,%d) holds %zu live entities but was never claimed "
                             "— claiming it now so they are not lost",
                             pos.x, pos.z, contents.Count());
                RequestLoad(pos);
            }
            error.clear();
            return false;
        }

        std::vector<uint8_t> nbt;
        if (!Game::Anvil::SerialiseEntityChunk(contents, pos, Game::Save::DataVersion(),
                                               nbt, error)) {
            if (!error.empty()) return false;

            // Nothing saveable here. Vanilla DELETES the entry rather than
            // writing an empty list — measured across 415 real entity chunks,
            // not one held an empty Entities list.
            if (m_onDisk.erase(pos) > 0) {
                Io().Store(pos, {});   // clear, on the I/O worker
            }
            return true;
        }

        // Compression and the region write happen on the I/O worker.
        Io().Store(pos, std::move(nbt));
        m_onDisk.insert(pos);
        return true;
    }

    void LevelEntityStore::SaveAllLoaded() {
        Game::ChunkProvider* provider = ProviderOf(m_level);
        if (!provider || !provider->EntityPersistenceEnabled()) return;

        // Walk the entities, not the chunk list: entities are not dirty-tracked
        // (a mob walking changes a chunk's entity list without touching a
        // block), so every chunk that holds one has to be considered.
        // One pass over the entities, grouped by chunk. Gathering per chunk
        // walked every entity of the level for every occupied chunk — 9 s on
        // the server thread for 853 chunks and 1,875 mobs (stress test,
        // 2026-09-23), on every autosave and every pause-save.
        std::unordered_map<Game::Math::ChunkPos, Game::Anvil::EntityChunkContents, Game::Math::ChunkPosHash> byChunk;
        if (MobManager* mobs = m_level.Mobs()) {
            for (const auto& [id, mob] : mobs->All()) if (mob) byChunk[ChunkOf(mob->position)].mobs.push_back(mob.get());
        }
        if (ItemEntityManager* items = m_level.Items()) {
            for (const auto& [id, item] : items->All()) byChunk[ChunkOf(item.pos)].items.push_back(&item);
        }
        if (ExperienceOrbManager* orbs = m_level.Orbs()) {
            for (const auto& [id, orb] : orbs->All()) byChunk[ChunkOf(orb.pos)].orbs.push_back(&orb);
        }

        // Plus every chunk that USED to hold something: those need clearing.
        std::vector<Game::Math::ChunkPos> targets;
        targets.reserve(byChunk.size() + m_onDisk.size());
        for (const auto& [pos, contents] : byChunk) targets.push_back(pos);
        for (const auto& pos : m_onDisk) {
            if (!byChunk.count(pos)) targets.push_back(pos);
        }

        // Counted so the summary says what actually went to disk. A count of
        // chunks alone hid the bug this pass exists to prevent: "saved
        // entities for 68 chunks" looks healthy whether or not a single item
        // entity was among them.
        size_t mobCount = 0, itemCount = 0, orbCount = 0;
        if (MobManager* mobs = m_level.Mobs())          mobCount  = mobs->All().size();
        if (ItemEntityManager* items = m_level.Items()) itemCount = items->All().size();
        if (ExperienceOrbManager* orbs = m_level.Orbs()) orbCount = orbs->All().size();

        size_t saved = 0;
        const Game::Anvil::EntityChunkContents none{};
        for (const auto& pos : targets) {
            std::string error;
            const auto it = byChunk.find(pos);
            if (SaveChunkWith(pos, it != byChunk.end() ? &it->second : &none, error)) ++saved;
            else if (!error.empty()) {
                Log::Error("[Anvil] entity save failed for chunk (%d,%d): %s",
                           pos.x, pos.z, error.c_str());
            }
        }
        if (saved > 0) {
            Log::Info("[Anvil] saved entities for %zu chunks (%zu mobs, %zu items, %zu orbs live)",
                      saved, mobCount, itemCount, orbCount);
        }
    }

    void LevelEntityStore::SaveAndForget(Game::Math::ChunkPos pos) {
        SaveAndForgetMany({pos});
    }

    void LevelEntityStore::SaveAndForgetMany(const std::vector<Game::Math::ChunkPos>& chunks) {
        if (chunks.empty()) return;
        PROFILE_ZONE_N("EntitySaveAndForget");
        // One pass over the level's entities for all of them (MC finds a
        // chunk's entities through its section storage instead of a scan).
        std::unordered_map<Game::Math::ChunkPos, Game::Anvil::EntityChunkContents, Game::Math::ChunkPosHash> byChunk;
        byChunk.reserve(chunks.size());
        for (const auto& pos : chunks) byChunk.emplace(pos, Game::Anvil::EntityChunkContents{});
        if (MobManager* mobs = m_level.Mobs()) {
            for (const auto& [id, mob] : mobs->All()) {
                if (!mob) continue;
                const auto it = byChunk.find(ChunkOf(mob->position));
                if (it != byChunk.end()) it->second.mobs.push_back(mob.get());
            }
        }
        if (ItemEntityManager* items = m_level.Items()) {
            for (const auto& [id, item] : items->All()) {
                const auto it = byChunk.find(ChunkOf(item.pos));
                if (it != byChunk.end()) it->second.items.push_back(&item);
            }
        }
        if (ExperienceOrbManager* orbs = m_level.Orbs()) {
            for (const auto& [id, orb] : orbs->All()) {
                const auto it = byChunk.find(ChunkOf(orb.pos));
                if (it != byChunk.end()) it->second.orbs.push_back(&orb);
            }
        }
        for (const auto& pos : chunks) {
            std::string error;
            if (!SaveChunkWith(pos, &byChunk[pos], error) && !error.empty()) {
                Log::Error("[Anvil] entity save failed on unload of chunk (%d,%d): %s",
                           pos.x, pos.z, error.c_str());
            }
            m_state.erase(pos);
            m_loadTickets.erase(pos);   // a read still on the way is dropped on arrival
        }
    }

    size_t LevelEntityStore::PendingCount() const {
        size_t n = 0;
        for (const auto& [pos, state] : m_state) if (state == State::Pending) ++n;
        return n;
    }

} // namespace Server
