// File: src/server/commands/CommandSavedData.cpp
#include "CommandSavedData.hpp"
#include "../IntegratedServer.hpp"
#include "../level/ServerLevel.hpp"
#include "../world/storage/anvil/SaveRoot.hpp"
#include "server/world/storage/NBTParser.hpp"

#include "common/core/Log.hpp"
#include "common/core/SaveVersion.hpp"
#include "common/nbt/NbtWrite.hpp"

#include "random/RandomSupport.h"            // terrain library: MC RandomSupport
#include "random/XoroshiroRandomSource.h"    // terrain library: MC XoroshiroRandomSource

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>

namespace Server::CommandSavedData {

    namespace {

        namespace fs = std::filesystem;
        using Compound = ::World::NBTTagCompound;

        // ── Files (the CommandStorage pattern: <world>/data/<name>.dat,
        //    gzipped {data:{…}, DataVersion}) ──────────────────────────────

        fs::path DataDir() {
            if (!g_integratedServer) return {};
            ServerLevel* overworld = g_integratedServer->GetLevel(Game::DimensionId::Overworld);
            if (!overworld || overworld->Config().savePath.empty()) return {};
            std::string reason;
            const auto root = Game::Anvil::SaveRoot::Open(overworld->Config().savePath, reason);
            return root ? root->DataDir(Game::DimensionId::Overworld) : fs::path{};
        }

        bool ReadOnlyWorld() {
            if (!g_integratedServer) return true;
            ServerLevel* overworld = g_integratedServer->GetLevel(Game::DimensionId::Overworld);
            return !overworld || overworld->Config().readOnly;
        }

        // The "data" compound of <world>/data/<name>.dat, or null.
        std::shared_ptr<Compound> ReadData(const char* name) {
            const fs::path dir = DataDir();
            if (dir.empty()) return nullptr;
            std::ifstream f(dir / (std::string(name) + ".dat"), std::ios::binary);
            if (!f) return nullptr;
            const std::vector<uint8_t> raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            std::vector<uint8_t> nbt;
            if (raw.empty() || !Game::Nbt::GzipDecompress(raw, nbt)) return nullptr;
            try {
                auto root = std::dynamic_pointer_cast<Compound>(::World::NBTParser::Parse(nbt));
                return root ? std::dynamic_pointer_cast<Compound>(root->GetTag("data")) : nullptr;
            } catch (const std::exception& e) {
                Log::Warning("[CommandSavedData] unreadable %s.dat: %s", name, e.what());
                return nullptr;
            }
        }

        // Writes {data:{<body>}, DataVersion} to <world>/data/<name>.dat.
        template <typename Body>
        bool WriteData(const char* name, Body&& body) {
            const fs::path dir = DataDir();
            if (dir.empty()) return false;
            Game::Nbt::Writer w;
            w.BeginRootCompound();
            w.BeginCompound("data");
            body(w);
            w.EndCompound();
            w.Int("DataVersion", Game::Save::DataVersion());
            w.EndRootCompound();
            if (!w.ok()) return false;
            std::vector<uint8_t> gz;
            if (!Game::Nbt::GzipCompress(w.Bytes(), gz)) return false;
            std::error_code ec;
            fs::create_directories(dir, ec);
            const fs::path file = dir / (std::string(name) + ".dat");
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

        // ── Stopwatches ─────────────────────────────────────────────────────

        // MC Stopwatch(creationTime, accumulatedElapsedTime).
        struct Stopwatch {
            int64_t creationTime = 0;
            int64_t accumulated  = 0;
            int64_t ElapsedMilliseconds(int64_t now) const { return accumulated + (now - creationTime); }
        };

        struct StopwatchStore {
            std::map<std::string, Stopwatch> watches;
            bool loaded = false;
            bool dirty  = false;
        };

        StopwatchStore& Stopwatches() {
            static StopwatchStore store;
            if (!store.loaded) {
                store.loaded = true;
                // Stopwatches.unpack: each saved elapsed time resumes from now.
                if (auto data = ReadData("stopwatches")) {
                    if (auto map = std::dynamic_pointer_cast<Compound>(data->GetTag("stopwatches"))) {
                        const int64_t now = StopwatchNow();
                        for (const auto& [id, tag] : map->value) {
                            auto elapsed = std::dynamic_pointer_cast<::World::NBTTagLong>(tag);
                            if (elapsed) store.watches[id] = Stopwatch{now, elapsed->value};
                        }
                    }
                }
            }
            return store;
        }

        // ── Random sequences ────────────────────────────────────────────────

        struct RandomStore {
            int  salt = 0;
            bool includeWorldSeed = true;
            bool includeSequenceId = true;
            std::map<std::string, std::unique_ptr<minecraft::XoroshiroRandomSource>> sequences;
            bool loaded = false;
            bool dirty  = false;
        };

        RandomStore& Randoms() {
            static RandomStore store;
            if (!store.loaded) {
                store.loaded = true;
                if (auto data = ReadData("random_sequences")) {
                    store.salt              = data->GetValue<int32_t>("salt", 0);
                    store.includeWorldSeed  = data->GetValue<int8_t>("include_world_seed", 1) != 0;
                    store.includeSequenceId = data->GetValue<int8_t>("include_sequence_id", 1) != 0;
                    if (auto map = std::dynamic_pointer_cast<Compound>(data->GetTag("sequences"))) {
                        for (const auto& [id, tag] : map->value) {
                            auto sequence = std::dynamic_pointer_cast<Compound>(tag);
                            auto source = sequence
                                ? std::dynamic_pointer_cast<::World::NBTTagLongArray>(sequence->GetTag("source")) : nullptr;
                            if (!source || source->value.size() != 2) continue;
                            store.sequences[id] = std::make_unique<minecraft::XoroshiroRandomSource>(
                                static_cast<uint64_t>(source->value[0]), static_cast<uint64_t>(source->value[1]));
                        }
                    }
                }
            }
            return store;
        }

        // MC RandomSequence.createSequence(seed, key): the unmixed 128-bit
        // upgrade of the seed, xor the MD5 of the id when the id takes
        // part, then mixed.
        std::unique_ptr<minecraft::XoroshiroRandomSource> CreateSequence(const std::string& id, int64_t worldSeed,
                                                                         int salt, bool includeWorldSeed,
                                                                         bool includeSequenceId) {
            const int64_t seed = (includeWorldSeed ? worldSeed : 0) ^ static_cast<int64_t>(salt);
            minecraft::Seed128bit seed128 = minecraft::RandomSupport::upgradeSeedTo128bitUnmixed(seed);
            if (includeSequenceId) seed128 = seed128.xor_(minecraft::RandomSupport::seedFromHashOf(id));
            const minecraft::Seed128bit mixed = seed128.mixed();
            return std::make_unique<minecraft::XoroshiroRandomSource>(mixed.seedLo, mixed.seedHi);
        }

    } // namespace

    // ── Stopwatches ─────────────────────────────────────────────────────────

    int64_t StopwatchNow() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    bool AddStopwatch(const std::string& id) {
        StopwatchStore& store = Stopwatches();
        if (!store.watches.emplace(id, Stopwatch{StopwatchNow(), 0}).second) return false;
        store.dirty = true;
        return true;
    }

    std::optional<double> StopwatchElapsedSeconds(const std::string& id) {
        StopwatchStore& store = Stopwatches();
        auto it = store.watches.find(id);
        if (it == store.watches.end()) return std::nullopt;
        return static_cast<double>(it->second.ElapsedMilliseconds(StopwatchNow())) / 1000.0;
    }

    bool RestartStopwatch(const std::string& id) {
        StopwatchStore& store = Stopwatches();
        auto it = store.watches.find(id);
        if (it == store.watches.end()) return false;
        it->second = Stopwatch{StopwatchNow(), 0};
        store.dirty = true;
        return true;
    }

    bool RemoveStopwatch(const std::string& id) {
        StopwatchStore& store = Stopwatches();
        if (store.watches.erase(id) == 0) return false;
        store.dirty = true;
        return true;
    }

    std::vector<std::string> StopwatchIds() {
        std::vector<std::string> out;
        for (const auto& [id, watch] : Stopwatches().watches) out.push_back(id);
        return out;
    }

    // ── Random sequences ────────────────────────────────────────────────────

    minecraft::XoroshiroRandomSource& RandomSequence(const std::string& id, int64_t worldSeed) {
        RandomStore& store = Randoms();
        auto& slot = store.sequences[id];
        if (!slot) slot = CreateSequence(id, worldSeed, store.salt, store.includeWorldSeed, store.includeSequenceId);
        // DirtyMarkingRandomSource: every draw changes what is saved.
        store.dirty = true;
        return *slot;
    }

    void ResetRandomSequence(const std::string& id, int64_t worldSeed) {
        RandomStore& store = Randoms();
        store.sequences[id] = CreateSequence(id, worldSeed, store.salt, store.includeWorldSeed, store.includeSequenceId);
        store.dirty = true;
    }

    void ResetRandomSequence(const std::string& id, int64_t worldSeed, int salt,
                             bool includeWorldSeed, bool includeSequenceId) {
        RandomStore& store = Randoms();
        store.sequences[id] = CreateSequence(id, worldSeed, salt, includeWorldSeed, includeSequenceId);
        store.dirty = true;
    }

    void SetRandomSequenceDefaults(int salt, bool includeWorldSeed, bool includeSequenceId) {
        RandomStore& store = Randoms();
        store.salt = salt;
        store.includeWorldSeed = includeWorldSeed;
        store.includeSequenceId = includeSequenceId;
        store.dirty = true;
    }

    int ClearRandomSequences() {
        RandomStore& store = Randoms();
        const int count = static_cast<int>(store.sequences.size());
        store.sequences.clear();
        store.dirty = true;
        return count;
    }

    std::vector<std::string> RandomSequenceIds() {
        std::vector<std::string> out;
        for (const auto& [id, sequence] : Randoms().sequences) out.push_back(id);
        return out;
    }

    // ── Saving ──────────────────────────────────────────────────────────────

    void Save() {
        if (ReadOnlyWorld()) return;
        // Stopwatches.isDirty: also whenever any exists (their elapsed time
        // moves on by itself).
        StopwatchStore& watches = Stopwatches();
        if (watches.dirty || !watches.watches.empty()) {
            const int64_t now = StopwatchNow();
            const bool ok = WriteData("stopwatches", [&](Game::Nbt::Writer& w) {
                w.BeginCompound("stopwatches");
                for (const auto& [id, watch] : watches.watches) w.Long(id, watch.ElapsedMilliseconds(now));
                w.EndCompound();
            });
            if (ok) watches.dirty = false;
            else Log::Warning("[CommandSavedData] could not save stopwatches.dat");
        }
        RandomStore& randoms = Randoms();
        if (randoms.dirty) {
            const bool ok = WriteData("random_sequences", [&](Game::Nbt::Writer& w) {
                w.Int("salt", randoms.salt);
                w.Bool("include_world_seed", randoms.includeWorldSeed);
                w.Bool("include_sequence_id", randoms.includeSequenceId);
                w.BeginCompound("sequences");
                for (const auto& [id, source] : randoms.sequences) {
                    if (!source) continue;
                    const int64_t state[2] = {
                        static_cast<int64_t>(source->getGenerator().getSeedLo()),
                        static_cast<int64_t>(source->getGenerator().getSeedHi()),
                    };
                    w.BeginCompound(id);
                    w.LongArray("source", state, 2);
                    w.EndCompound();
                }
                w.EndCompound();
            });
            if (ok) randoms.dirty = false;
            else Log::Warning("[CommandSavedData] could not save random_sequences.dat");
        }
    }

    void Close() {
        Save();
        StopwatchStore& watches = Stopwatches();
        watches.watches.clear();
        watches.loaded = false;
        watches.dirty = false;
        RandomStore& randoms = Randoms();
        randoms.sequences.clear();
        randoms.salt = 0;
        randoms.includeWorldSeed = true;
        randoms.includeSequenceId = true;
        randoms.loaded = false;
        randoms.dirty = false;
    }

} // namespace Server::CommandSavedData
