// File: src/server/level/NamedEntityIndex.cpp
#include "server/level/NamedEntityIndex.hpp"

#include "server/IntegratedServer.hpp"
#include "server/entity/MobManager.hpp"
#include "server/level/ChunkKeeper.hpp"
#include "server/level/LevelEntityStore.hpp"
#include "server/level/ServerLevel.hpp"
#include "server/world/ChunkProvider.hpp"
#include "server/world/storage/NBTParser.hpp"
#include "server/world/storage/anvil/AnvilRegion.hpp"
#include "server/world/storage/anvil/EntityNbt.hpp"
#include "server/world/storage/anvil/ItemStackNbt.hpp"
#include "server/world/storage/anvil/SaveRoot.hpp"

#include "common/core/Log.hpp"
#include "common/core/SaveVersion.hpp"
#include "common/entity/Mob.hpp"
#include "common/nbt/NbtWrite.hpp"
#include "common/text/TextComponent.hpp"
#include "common/world/level/World.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace Server::NamedEntities {

    namespace {

        namespace fs = std::filesystem;
        using ChunkPos = Game::Math::ChunkPos;
        using ChunkSet = std::unordered_set<ChunkPos, Game::Math::ChunkPosHash>;

        constexpr const char* kIndexFile = "obeycraft_named_entities.dat";

        struct Dimension {
            std::unordered_map<ChunkPos, std::vector<Entry>, Game::Math::ChunkPosHash> byChunk;
            fs::path dataDir;
            fs::path entitiesDir;
            bool     complete = false;   // file read, or scan finished: safe to write
            bool     dirty    = false;
            bool     scanning = false;
            ChunkSet notedDuringScan;    // save notes newer than anything the scan reads
        };

        struct ScanResult {
            Game::DimensionId  dimension;
            ChunkPos           chunk;
            std::vector<Entry> entries;
        };

        struct ScanJob {
            struct Target {
                Game::DimensionId    dimension;
                fs::path             entitiesDir;
                Game::ChunkProvider* provider = nullptr;   // null: read the files directly
            };
            std::vector<Target>     targets;
            std::atomic<bool>       stop{false};
            std::atomic<bool>       finished{false};
            std::mutex              mutex;
            std::vector<ScanResult> inbox;
            std::vector<Game::DimensionId> doneDimensions;
        };

        struct State {
            bool opened   = false;
            bool readOnly = false;
            std::unordered_map<int, Dimension> dims;   // by DimensionId value
            std::shared_ptr<ScanJob> scan;
            std::thread scanThread;
            void (*namesChanged)() = nullptr;
            std::vector<std::string> lastNames;
            int64_t tick = 0;
        };

        State& S() {
            static State state;
            return state;
        }

        Dimension& Dim(Game::DimensionId d) { return S().dims[static_cast<int>(d)]; }

        std::string StripMinecraft(std::string id) {
            if (id.rfind("minecraft:", 0) == 0) id.erase(0, 10);
            return id;
        }

        // One saved entity compound → an entry, when it carries a custom name.
        bool EntryFromNbt(const ::World::NBTTagCompound& tag, ChunkPos chunk, Entry& out) {
            const auto nameTag = tag.GetTag("CustomName");
            if (!nameTag) return false;
            const auto component = Game::Anvil::ReadTextComponent(*nameTag);
            if (!component) return false;
            out.name = Game::Text::GetString(*component);
            if (out.name.empty()) return false;
            Game::EntityTypeId type{};
            const std::string id = tag.GetValue<std::string>("id", "");
            out.typeSlug = Game::Anvil::EntityTypeFromName(id, type)
                ? std::string(Game::GetEntityTypeInfo(type).slug) : StripMinecraft(id);
            if (auto uuid = std::dynamic_pointer_cast<::World::NBTTagIntArray>(tag.GetTag("UUID"));
                uuid && uuid->value.size() == 4) {
                const int32_t words[4] = {uuid->value[0], uuid->value[1], uuid->value[2], uuid->value[3]};
                out.uuid = Game::UuidFromIntArray(words);
            }
            out.position = glm::dvec3(chunk.x * 16 + 8, 64.0, chunk.z * 16 + 8);
            if (auto pos = std::dynamic_pointer_cast<::World::NBTTagList>(tag.GetTag("Pos"));
                pos && pos->value.size() == 3) {
                for (int i = 0; i < 3; ++i) {
                    if (auto d = std::dynamic_pointer_cast<::World::NBTTagDouble>(pos->value[static_cast<size_t>(i)])) {
                        out.position[i] = d->value;
                    }
                }
            }
            out.chunk = chunk;
            return true;
        }

        // An entities/*.mca chunk's named entities.
        std::vector<Entry> EntriesFromChunkNbt(const std::vector<uint8_t>& nbt, ChunkPos chunk) {
            std::vector<Entry> out;
            std::shared_ptr<::World::NBTTagCompound> root;
            try {
                root = std::dynamic_pointer_cast<::World::NBTTagCompound>(::World::NBTParser::Parse(nbt));
            } catch (const std::exception&) {
                return out;
            }
            if (!root) return out;
            auto list = std::dynamic_pointer_cast<::World::NBTTagList>(root->GetTag("Entities"));
            if (!list) return out;
            for (const auto& element : list->value) {
                auto entity = std::dynamic_pointer_cast<::World::NBTTagCompound>(element);
                Entry e;
                if (entity && EntryFromNbt(*entity, chunk, e)) out.push_back(std::move(e));
            }
            return out;
        }

        bool ParseRegionName(const std::string& name, int& rx, int& rz) {
            return std::sscanf(name.c_str(), "r.%d.%d.mca", &rx, &rz) == 2 &&
                   name.size() > 4 && name.compare(name.size() - 4, 4, ".mca") == 0;
        }

        void RunScan(ScanJob& job) {
            const auto started = std::chrono::steady_clock::now();
            size_t chunks = 0, named = 0;
            std::vector<uint8_t> nbt;
            std::string error;
            for (const ScanJob::Target& target : job.targets) {
                std::error_code ec;
                std::vector<fs::path> files;
                for (const auto& entry : fs::directory_iterator(target.entitiesDir, ec)) files.push_back(entry.path());
                for (const fs::path& file : files) {
                    if (job.stop.load(std::memory_order_relaxed)) return;
                    int rx = 0, rz = 0;
                    if (!ParseRegionName(file.filename().string(), rx, rz)) continue;
                    std::unique_ptr<Game::Anvil::AnvilRegion> region;
                    if (!target.provider) {
                        error.clear();
                        region = Game::Anvil::AnvilRegion::Open(file, /*writable=*/false, error);
                        if (!region) continue;
                    }
                    std::vector<ScanResult> found;
                    for (int lz = 0; lz < 32; ++lz) {
                        if (job.stop.load(std::memory_order_relaxed)) return;
                        for (int lx = 0; lx < 32; ++lx) {
                            const ChunkPos pos{rx * 32 + lx, rz * 32 + lz};
                            nbt.clear();
                            error.clear();
                            bool ok = false;
                            if (target.provider) {
                                ok = target.provider->ReadEntityChunkNbt(pos, nbt, error);
                            } else if (region->Has(lx, lz)) {
                                ok = region->Read(lx, lz, nbt, error);
                            }
                            if (!ok) continue;
                            ++chunks;
                            std::vector<Entry> entries = EntriesFromChunkNbt(nbt, pos);
                            if (entries.empty()) continue;
                            named += entries.size();
                            found.push_back({target.dimension, pos, std::move(entries)});
                        }
                    }
                    if (!found.empty()) {
                        std::lock_guard<std::mutex> lock(job.mutex);
                        for (ScanResult& r : found) job.inbox.push_back(std::move(r));
                    }
                }
                std::lock_guard<std::mutex> lock(job.mutex);
                job.doneDimensions.push_back(target.dimension);
            }
            const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            Log::Info("[NamedEntities] scan: %zu entity chunk(s) read, %zu named entit%s, %.1f s",
                      chunks, named, named == 1 ? "y" : "ies", secs);
        }

        // ── the file ────────────────────────────────────────────────────────

        bool LoadFile(Dimension& dim) {
            if (dim.dataDir.empty()) return false;
            std::ifstream f(dim.dataDir / kIndexFile, std::ios::binary);
            if (!f) return false;
            const std::vector<uint8_t> raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            std::vector<uint8_t> nbt;
            if (raw.empty() || !Game::Nbt::GzipDecompress(raw, nbt)) return false;
            std::shared_ptr<::World::NBTTagCompound> root;
            try {
                root = std::dynamic_pointer_cast<::World::NBTTagCompound>(::World::NBTParser::Parse(nbt));
            } catch (const std::exception&) {
                return false;
            }
            if (!root) return false;
            auto data = std::dynamic_pointer_cast<::World::NBTTagCompound>(root->GetTag("data"));
            if (!data) return false;
            // Chunks saved before the file was read (a save in the very first
            // ticks) are fresher than it: keep them.
            ChunkSet noted;
            for (const auto& [chunk, entries] : dim.byChunk) { (void)entries; noted.insert(chunk); }
            if (auto list = std::dynamic_pointer_cast<::World::NBTTagList>(data->GetTag("Entities"))) {
                for (const auto& element : list->value) {
                    auto tag = std::dynamic_pointer_cast<::World::NBTTagCompound>(element);
                    if (!tag) continue;
                    const ChunkPos chunk{tag->GetValue<int32_t>("ChunkX", 0), tag->GetValue<int32_t>("ChunkZ", 0)};
                    if (noted.count(chunk)) continue;
                    Entry e;
                    if (EntryFromNbt(*tag, chunk, e)) dim.byChunk[chunk].push_back(std::move(e));
                }
            }
            return true;
        }

        bool SaveFile(const Dimension& dim) {
            if (dim.dataDir.empty()) return false;
            Game::Nbt::Writer w;
            w.BeginRootCompound();
            w.BeginCompound("data");
            auto list = w.BeginList("Entities", Game::Nbt::TagType::Compound);
            for (const auto& [chunk, entries] : dim.byChunk) {
                for (const Entry& e : entries) {
                    w.ListCompoundBegin(list);
                    w.Int("ChunkX", chunk.x);
                    w.Int("ChunkZ", chunk.z);
                    w.String("CustomName", e.name);
                    w.String("id", e.typeSlug.find(':') == std::string::npos ? "minecraft:" + e.typeSlug : e.typeSlug);
                    int32_t words[4];
                    Game::UuidToIntArray(e.uuid, words);
                    w.IntArray("UUID", words, 4);
                    auto pos = w.BeginList("Pos", Game::Nbt::TagType::Double);
                    w.ListDouble(pos, e.position.x);
                    w.ListDouble(pos, e.position.y);
                    w.ListDouble(pos, e.position.z);
                    w.EndList(pos);
                    w.ListCompoundEnd(list);
                }
            }
            w.EndList(list);
            w.EndCompound();
            w.Int("DataVersion", Game::Save::DataVersion());
            w.EndRootCompound();
            if (!w.ok()) return false;
            std::vector<uint8_t> gz;
            if (!Game::Nbt::GzipCompress(w.Bytes(), gz)) return false;
            std::error_code ec;
            fs::create_directories(dim.dataDir, ec);
            const fs::path file = dim.dataDir / kIndexFile;
            const fs::path tmp = file.string() + ".tmp";
            {
                std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
                if (!f) return false;
                f.write(reinterpret_cast<const char*>(gz.data()), static_cast<std::streamsize>(gz.size()));
                if (!f) return false;
            }
            fs::rename(tmp, file, ec);
            return !ec;
        }

        // ── opening ─────────────────────────────────────────────────────────

        void Open(IntegratedServer& server) {
            State& st = S();
            ServerLevel* overworld = server.GetLevel(Game::DimensionId::Overworld);
            if (!overworld) return;   // not yet: retry next tick
            st.opened = true;
            st.readOnly = overworld->Config().readOnly;
            std::optional<Game::Anvil::SaveRoot> root;
            std::string reason;
            if (!overworld->Config().savePath.empty()) root = Game::Anvil::SaveRoot::Open(overworld->Config().savePath, reason);

            auto job = std::make_shared<ScanJob>();
            for (const Game::DimensionId d : Game::kAllDimensions) {
                Dimension& dim = Dim(d);
                if (!root) { dim.complete = true; continue; }   // no save folder: live notes only
                dim.dataDir = root->DataDir(d);
                dim.entitiesDir = root->EntitiesDir(d);
                if (LoadFile(dim)) { dim.complete = true; continue; }
                std::error_code ec;
                if (!fs::is_directory(dim.entitiesDir, ec)) { dim.complete = true; dim.dirty = !st.readOnly; continue; }
                ScanJob::Target target;
                target.dimension = d;
                target.entitiesDir = dim.entitiesDir;
                if (ServerLevel* level = server.GetLevel(d)) {
                    if (Game::World* world = level->World()) {
                        Game::ChunkProvider* provider = world->GetChunkProvider();
                        if (provider && provider->EntityPersistenceEnabled()) target.provider = provider;
                    }
                }
                dim.scanning = true;
                for (const auto& [chunk, entries] : dim.byChunk) { (void)entries; dim.notedDuringScan.insert(chunk); }
                job->targets.push_back(std::move(target));
            }
            if (!job->targets.empty()) {
                Log::Info("[NamedEntities] no index for %zu dimension(s) — scanning their saved entities",
                          job->targets.size());
                st.scan = job;
                st.scanThread = std::thread([job] {
                    RunScan(*job);
                    job->finished.store(true, std::memory_order_release);
                });
            }
        }

        void DrainScan() {
            State& st = S();
            if (!st.scan) return;
            std::vector<ScanResult> results;
            std::vector<Game::DimensionId> done;
            {
                std::lock_guard<std::mutex> lock(st.scan->mutex);
                results.swap(st.scan->inbox);
                done.swap(st.scan->doneDimensions);
            }
            for (ScanResult& r : results) {
                Dimension& dim = Dim(r.dimension);
                if (dim.notedDuringScan.count(r.chunk)) continue;   // a save is fresher
                dim.byChunk[r.chunk] = std::move(r.entries);
                dim.dirty = true;
            }
            for (const Game::DimensionId d : done) {
                Dimension& dim = Dim(d);
                dim.scanning = false;
                dim.complete = true;
                dim.dirty = true;
                dim.notedDuringScan.clear();
            }
            if (st.scan->finished.load(std::memory_order_acquire)) {
                if (st.scanThread.joinable()) st.scanThread.join();
                st.scan.reset();
                Save();
            }
        }

    } // namespace

    void Service(IntegratedServer& server) {
        State& st = S();
        if (!st.opened) Open(server);
        if (!st.opened) return;
        DrainScan();
        if (++st.tick % 20 != 0 || !st.namesChanged) return;
        std::vector<std::string> names = KnownNames(server);
        if (names != st.lastNames) {
            st.lastNames = std::move(names);
            st.namesChanged();
        }
    }

    void NoteChunkSaved(Game::DimensionId dimension, Game::Math::ChunkPos chunk,
                        const std::vector<const Game::Mob*>& mobs) {
        std::vector<Entry> entries;
        for (const Game::Mob* mob : mobs) {
            if (!mob || !mob->HasCustomName() || mob->GetCustomName()->empty()) continue;
            Entry e;
            e.uuid     = mob->GetUuid();
            e.name     = *mob->GetCustomName();
            e.typeSlug = std::string(mob->TypeInfo().slug);
            e.position = mob->position;
            e.chunk    = chunk;
            entries.push_back(std::move(e));
        }
        Dimension& dim = Dim(dimension);
        if (dim.scanning) dim.notedDuringScan.insert(chunk);
        auto it = dim.byChunk.find(chunk);
        if (entries.empty()) {
            if (it == dim.byChunk.end()) return;
            dim.byChunk.erase(it);
        } else {
            dim.byChunk[chunk] = std::move(entries);
        }
        dim.dirty = true;
    }

    std::vector<Entry> Find(Game::DimensionId dimension, const std::string& name) {
        std::vector<Entry> out;
        for (const auto& [chunk, entries] : Dim(dimension).byChunk) {
            (void)chunk;
            for (const Entry& e : entries) if (e.name == name) out.push_back(e);
        }
        return out;
    }

    std::vector<std::string> KnownNames(IntegratedServer& server) {
        std::vector<std::string> names;
        for (const auto& [d, dim] : S().dims) {
            (void)d;
            for (const auto& [chunk, entries] : dim.byChunk) {
                (void)chunk;
                for (const Entry& e : entries) names.push_back(e.name);
            }
        }
        server.ForEachLevel([&](ServerLevel& level) {
            if (MobManager* mobs = level.Mobs()) {
                for (const auto& [id, mob] : mobs->All()) {
                    (void)id;
                    if (mob && mob->HasCustomName() && !mob->GetCustomName()->empty()) names.push_back(*mob->GetCustomName());
                }
            }
        });
        std::sort(names.begin(), names.end());
        names.erase(std::unique(names.begin(), names.end()), names.end());
        return names;
    }

    void SetNamesChangedCallback(void (*callback)()) { S().namesChanged = callback; }

    void Save() {
        State& st = S();
        if (st.readOnly) return;
        for (auto& [d, dim] : st.dims) {
            (void)d;
            // A dimension still being scanned is written once the scan ends;
            // written earlier, a restart would trust a half-built index.
            if (!dim.dirty || !dim.complete) continue;
            if (SaveFile(dim)) dim.dirty = false;
        }
    }

    void StopScan() {
        State& st = S();
        if (st.scan) st.scan->stop.store(true);
        if (st.scanThread.joinable()) st.scanThread.join();
        if (st.scan) {
            // What the scan handed over for finished dimensions still counts;
            // an unfinished dimension stays incomplete (not written).
            DrainScan();
            st.scan.reset();
        }
        for (auto& [d, dim] : st.dims) {
            (void)d;
            dim.scanning = false;
        }
    }

    void Close() {
        StopScan();
        Save();
        State& st = S();
        st = State{};
    }

    // ── holds ───────────────────────────────────────────────────────────────

    void Hold(const std::vector<ChunkRef>& chunks) {
        if (!g_integratedServer) return;
        for (const ChunkRef& ref : chunks) {
            ServerLevel* level = g_integratedServer->GetOrCreateLevel(ref.dimension);
            if (ChunkKeeper* keeper = level ? level->Keeper() : nullptr) keeper->HoldForCommand(ref.chunk);
        }
    }

    void Release(const std::vector<ChunkRef>& chunks) {
        if (!g_integratedServer) return;
        for (const ChunkRef& ref : chunks) {
            ServerLevel* level = g_integratedServer->GetLevel(ref.dimension);
            if (ChunkKeeper* keeper = level ? level->Keeper() : nullptr) keeper->ReleaseCommandHold(ref.chunk);
        }
    }

    bool EntitiesLive(Game::DimensionId dimension, Game::Math::ChunkPos chunk) {
        if (!g_integratedServer) return false;
        ServerLevel* level = g_integratedServer->GetLevel(dimension);
        Game::World* world = level ? level->World() : nullptr;
        if (!world || !world->IsChunkLoaded(chunk.x, chunk.z)) return false;
        LevelEntityStore* store = level->Entities();
        return !store || !store->IsPending(chunk);
    }

    bool Ready(const std::vector<ChunkRef>& chunks) {
        for (const ChunkRef& ref : chunks) {
            if (!EntitiesLive(ref.dimension, ref.chunk)) return false;
        }
        return true;
    }

} // namespace Server::NamedEntities
