// File: src/server/level/ChunkKeeper.cpp
#include "server/level/ChunkKeeper.hpp"

#include "common/core/Log.hpp"
#include "common/core/SaveVersion.hpp"
#include "common/nbt/NbtWrite.hpp"
#include "common/world/block/BlockState.hpp"
#include "common/world/block/RedstoneFamilies.hpp"
#include "common/world/chunk/Chunk.hpp"
#include "common/world/chunk/ChunkSection.hpp"
#include "server/world/storage/NBTParser.hpp"
#include "server/world/storage/SectionDataUnpacker.hpp"
#include "server/world/storage/anvil/AnvilRegion.hpp"
#include "server/world/ticketing/ChunkLevel.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <regex>
#include <system_error>

namespace Server {

    namespace {

        constexpr const char* kForcedFile = "chunks.dat";                     // MC ForcedChunksSavedData
        constexpr const char* kIndexFile  = "obeycraft_redstone_chunks.dat";  // ours
        constexpr const char* kForcedId   = "forceload";
        constexpr const char* kRedstoneId = "redstone";

        // MC ChunkPos.asLong.
        int64_t PackPos(Game::Math::ChunkPos p) {
            return static_cast<int64_t>((static_cast<uint64_t>(static_cast<uint32_t>(p.x))) |
                                        (static_cast<uint64_t>(static_cast<uint32_t>(p.z)) << 32));
        }
        Game::Math::ChunkPos UnpackPos(int64_t v) {
            const uint64_t u = static_cast<uint64_t>(v);
            return Game::Math::ChunkPos{static_cast<int32_t>(static_cast<uint32_t>(u & 0xFFFFFFFFull)),
                                        static_cast<int32_t>(static_cast<uint32_t>(u >> 32))};
        }

        bool ReadGzipNbt(const std::filesystem::path& file, std::shared_ptr<::World::NBTTagCompound>& root) {
            std::ifstream f(file, std::ios::binary);
            if (!f) return false;
            const std::vector<uint8_t> raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            if (raw.empty()) return false;
            std::vector<uint8_t> nbt;
            if (!Game::Nbt::GzipDecompress(raw, nbt)) return false;
            root = std::dynamic_pointer_cast<::World::NBTTagCompound>(::World::NBTParser::Parse(nbt));
            return root != nullptr;
        }

        // {"": {"data": {<key>: long[]}, "DataVersion": int}} — vanilla's
        // SavedData shape, which is what chunks.dat is.
        bool WriteLongArrayData(const std::filesystem::path& file, const char* key,
                                const std::vector<int64_t>& values) {
            Game::Nbt::Writer w;
            w.BeginRootCompound();
            w.BeginCompound("data");
            w.LongArray(key, values.data(), values.size());
            w.EndCompound();
            w.Int("DataVersion", Game::Save::DataVersion());
            w.EndRootCompound();
            if (!w.ok()) return false;
            std::vector<uint8_t> gz;
            if (!Game::Nbt::GzipCompress(w.Bytes(), gz)) return false;
            std::error_code ec;
            std::filesystem::create_directories(file.parent_path(), ec);
            const std::filesystem::path tmp = file.string() + ".tmp";
            {
                std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
                if (!f) return false;
                f.write(reinterpret_cast<const char*>(gz.data()), static_cast<std::streamsize>(gz.size()));
                if (!f) return false;
            }
            std::filesystem::rename(tmp, file, ec);
            return !ec;
        }

        bool ReadLongArrayData(const std::filesystem::path& file, const char* key, std::vector<int64_t>& out) {
            std::shared_ptr<::World::NBTTagCompound> root;
            if (!ReadGzipNbt(file, root)) return false;
            auto data = std::dynamic_pointer_cast<::World::NBTTagCompound>(root->GetTag("data"));
            if (!data) return false;
            auto arr = std::dynamic_pointer_cast<::World::NBTTagLongArray>(data->GetTag(key));
            if (!arr) return true;     // a file with no entries
            out = arr->value;
            return true;
        }

    } // namespace

    bool ChunkHasRedstone(const Game::Chunk& chunk) {
        for (const auto& section : chunk.sections) {
            if (!section || section->IsAllAir()) continue;
            const Game::PalettedContainer& states = section->States();
            if (!states.IsGlobalPalette()) {
                for (uint32_t raw : states.Palette()) {
                    if (Game::IsRedstoneComponent(Game::BlockState::FromRawId(raw).Block())) return true;
                }
            } else {
                for (size_t i = 0; i < 4096; ++i) {
                    if (Game::IsRedstoneComponent(Game::BlockState::FromRawId(states.Get(i)).Block())) return true;
                }
            }
        }
        return false;
    }

    ChunkKeeper::ChunkKeeper(Game::DimensionId dimension, std::filesystem::path dataDir,
                             std::filesystem::path regionDir, bool readOnly, ChunkTicketManager* tickets)
        : m_dimension(dimension), m_dataDir(std::move(dataDir)), m_regionDir(std::move(regionDir)),
          m_readOnly(readOnly || m_dataDir.empty()), m_tickets(tickets) {
        if (!m_dataDir.empty()) {
            LoadForced();
            m_indexLoaded = LoadIndex();
        }
        for (const ChunkPos& p : m_forced) AddTicket(p, kForcedId);
        if (!m_forced.empty()) {
            Log::Info("[ChunkKeeper] %s: %zu force-loaded chunk(s)",
                      std::string(Game::DimensionName(m_dimension)).c_str(), m_forced.size());
        }
    }

    ChunkKeeper::~ChunkKeeper() {
        m_scanStop.store(true);
        if (m_scanThread.joinable()) m_scanThread.join();
    }

    // ── tickets / halo ───────────────────────────────────────────────────────

    void ChunkKeeper::AddTicket(ChunkPos pos, const char* identifier) {
        if (m_tickets) m_tickets->AddForcedTicket(identifier, pos, ChunkLevel::ENTITY_TICKING);
        MarkKeptDirty();
    }

    void ChunkKeeper::RemoveTicket(ChunkPos pos, const char* identifier) {
        if (m_tickets) m_tickets->RemoveForcedTicket(identifier, pos);
        MarkKeptDirty();
    }

    void ChunkKeeper::RebuildKept() {
        m_kept.clear();
        const auto halo = [&](const ChunkPos& c) {
            for (int dz = -kHaloRadius; dz <= kHaloRadius; ++dz)
                for (int dx = -kHaloRadius; dx <= kHaloRadius; ++dx)
                    m_kept.insert(ChunkPos{c.x + dx, c.z + dz});
        };
        for (const ChunkPos& c : m_forced) halo(c);
        if (m_redstoneEnabled) for (const ChunkPos& c : m_redstone) halo(c);
        m_keptDirty = false;
    }

    // ── /forceload ───────────────────────────────────────────────────────────

    bool ChunkKeeper::AddForced(ChunkPos pos) {
        if (!m_forced.insert(pos).second) return false;
        AddTicket(pos, kForcedId);
        m_forcedDirty = true;
        return true;
    }

    bool ChunkKeeper::RemoveForced(ChunkPos pos) {
        if (m_forced.erase(pos) == 0) return false;
        RemoveTicket(pos, kForcedId);
        m_forcedDirty = true;
        return true;
    }

    size_t ChunkKeeper::RemoveAllForced() {
        const size_t n = m_forced.size();
        for (const ChunkPos& p : m_forced) RemoveTicket(p, kForcedId);
        m_forced.clear();
        if (n) m_forcedDirty = true;
        return n;
    }

    std::vector<ChunkKeeper::ChunkPos> ChunkKeeper::ForcedChunks() const {
        std::vector<ChunkPos> out(m_forced.begin(), m_forced.end());
        std::sort(out.begin(), out.end(), [](const ChunkPos& a, const ChunkPos& b) {
            return a.z != b.z ? a.z < b.z : a.x < b.x;
        });
        return out;
    }

    bool ChunkKeeper::LoadForced() {
        std::vector<int64_t> values;
        if (!ReadLongArrayData(m_dataDir / kForcedFile, "Forced", values)) return false;
        for (int64_t v : values) m_forced.insert(UnpackPos(v));
        return true;
    }

    bool ChunkKeeper::SaveForced() {
        if (m_readOnly) return false;
        std::vector<int64_t> values;
        values.reserve(m_forced.size());
        for (const ChunkPos& p : ForcedChunks()) values.push_back(PackPos(p));
        if (!WriteLongArrayData(m_dataDir / kForcedFile, "Forced", values)) {
            Log::Warning("[ChunkKeeper] Could not write %s", (m_dataDir / kForcedFile).string().c_str());
            return false;
        }
        m_forcedDirty = false;
        return true;
    }

    // ── the redstone index ───────────────────────────────────────────────────

    bool ChunkKeeper::LoadIndex() {
        std::vector<int64_t> values;
        if (!ReadLongArrayData(m_dataDir / kIndexFile, "Chunks", values)) return false;
        for (int64_t v : values) m_redstone.insert(UnpackPos(v));
        return true;
    }

    bool ChunkKeeper::SaveIndex() {
        if (m_readOnly) return false;
        std::vector<int64_t> values;
        values.reserve(m_redstone.size());
        for (const ChunkPos& p : m_redstone) values.push_back(PackPos(p));
        std::sort(values.begin(), values.end());
        if (!WriteLongArrayData(m_dataDir / kIndexFile, "Chunks", values)) {
            Log::Warning("[ChunkKeeper] Could not write %s", (m_dataDir / kIndexFile).string().c_str());
            return false;
        }
        m_indexDirty = false;
        return true;
    }

    void ChunkKeeper::SetRedstoneEnabled(bool on) {
        if (on == m_redstoneEnabled) return;
        m_redstoneEnabled = on;
        if (on) {
            for (const ChunkPos& p : m_redstone) AddTicket(p, kRedstoneId);
            // An existing world with no index yet: build it from the region
            // files. A world written with one, or one that has saved chunks
            // since the rule existed, already knows.
            if (!m_indexLoaded && !m_scanStarted && !m_readOnly && !m_regionDir.empty()) StartScan();
            Log::Info("[ChunkKeeper] %s: redstone chunks on — %zu indexed%s",
                      std::string(Game::DimensionName(m_dimension)).c_str(), m_redstone.size(),
                      m_scanStarted && !m_scanFinished.load() ? " (scanning the region files for more)" : "");
        } else {
            for (const ChunkPos& p : m_redstone) RemoveTicket(p, kRedstoneId);
            Log::Info("[ChunkKeeper] %s: redstone chunks off",
                      std::string(Game::DimensionName(m_dimension)).c_str());
        }
        MarkKeptDirty();
    }

    void ChunkKeeper::NoteChunkSaved(ChunkPos pos, bool hasRedstone) {
        std::lock_guard<std::mutex> lock(m_inboxMutex);
        m_inbox.emplace_back(pos, hasRedstone);
    }

    void ChunkKeeper::ApplyIndexChange(ChunkPos pos, bool hasRedstone) {
        if (hasRedstone) {
            if (!m_redstone.insert(pos).second) return;
            if (m_redstoneEnabled) AddTicket(pos, kRedstoneId);
        } else {
            if (m_redstone.erase(pos) == 0) return;
            if (m_redstoneEnabled) RemoveTicket(pos, kRedstoneId);
        }
        m_indexDirty = true;
    }

    void ChunkKeeper::StartScan() {
        m_scanStarted = true;
        m_scanRunning.store(true);
        m_scanFinished.store(false);
        m_scanThread = std::thread([this] { ScanMain(); });
    }

    // Background: every chunk in every region file, palette names only.
    void ChunkKeeper::ScanMain() {
        const auto t0 = std::chrono::steady_clock::now();
        size_t chunks = 0, hits = 0;
        static const std::regex kName(R"(r\.(-?\d+)\.(-?\d+)\.mca)");
        std::error_code ec;
        std::vector<std::filesystem::path> files;
        for (const auto& entry : std::filesystem::directory_iterator(m_regionDir, ec)) {
            if (entry.is_regular_file(ec)) files.push_back(entry.path());
        }
        std::vector<std::pair<ChunkPos, bool>> found;
        for (const auto& file : files) {
            if (m_scanStop.load()) break;
            std::smatch m;
            const std::string name = file.filename().string();
            if (!std::regex_match(name, m, kName)) continue;
            const int rx = std::stoi(m[1].str()), rz = std::stoi(m[2].str());
            std::string error;
            auto region = Game::Anvil::AnvilRegion::Open(file, /*writable=*/false, error);
            if (!region) continue;
            std::vector<uint8_t> nbt;
            for (int lz = 0; lz < 32 && !m_scanStop.load(); ++lz) {
                for (int lx = 0; lx < 32; ++lx) {
                    if (!region->Has(lx, lz)) continue;
                    nbt.clear();
                    if (!region->Read(lx, lz, nbt, error)) continue;
                    auto root = std::dynamic_pointer_cast<::World::NBTTagCompound>(::World::NBTParser::Parse(nbt));
                    if (!root) continue;
                    ++chunks;
                    const ChunkPos pos{root->GetValue<int32_t>("xPos", rx * 32 + lx),
                                       root->GetValue<int32_t>("zPos", rz * 32 + lz)};
                    bool has = false;
                    if (auto sections = std::dynamic_pointer_cast<::World::NBTTagList>(root->GetTag("sections"))) {
                        for (const auto& s : sections->value) {
                            auto sec = std::dynamic_pointer_cast<::World::NBTTagCompound>(s);
                            if (!sec) continue;
                            auto bs = std::dynamic_pointer_cast<::World::NBTTagCompound>(sec->GetTag("block_states"));
                            if (!bs) continue;
                            auto pal = std::dynamic_pointer_cast<::World::NBTTagList>(bs->GetTag("palette"));
                            if (!pal) continue;
                            for (const auto& e : pal->value) {
                                auto ent = std::dynamic_pointer_cast<::World::NBTTagCompound>(e);
                                if (!ent) continue;
                                auto nm = std::dynamic_pointer_cast<::World::NBTTagString>(ent->GetTag("Name"));
                                if (!nm) continue;
                                const Game::BlockID id = Game::BlockStateRegistry::CreateBlockState(nm->value, {}).resolvedId;
                                if (Game::IsRedstoneComponent(id)) { has = true; break; }
                            }
                            if (has) break;
                        }
                    }
                    if (has) { ++hits; found.emplace_back(pos, true); }
                }
            }
            if (!found.empty()) {
                std::lock_guard<std::mutex> lock(m_inboxMutex);
                m_inbox.insert(m_inbox.end(), found.begin(), found.end());
                found.clear();
            }
        }
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        Log::Info("[ChunkKeeper] %s: redstone scan %s — %zu chunk(s) read, %zu with redstone, %.1f s",
                  std::string(Game::DimensionName(m_dimension)).c_str(),
                  m_scanStop.load() ? "stopped" : "done", chunks, hits, secs);
        m_scanFinished.store(true);
        m_scanRunning.store(false);
    }

    // ── per tick ─────────────────────────────────────────────────────────────

    void ChunkKeeper::Service(int64_t serverTick,
                              const std::function<bool(ChunkPos)>& isResidentOrPending,
                              const std::function<void(ChunkPos)>& requestLoad,
                              size_t budget) {
        {
            std::vector<std::pair<ChunkPos, bool>> notes;
            {
                std::lock_guard<std::mutex> lock(m_inboxMutex);
                notes.swap(m_inbox);
            }
            for (const auto& [pos, has] : notes) ApplyIndexChange(pos, has);
        }
        if (m_scanStarted && m_scanFinished.load() && m_scanThread.joinable()) {
            m_scanThread.join();
            m_indexLoaded = true;
            m_indexDirty  = true;       // write the freshly built index even if empty
            Save();
        }
        if (m_keptDirty) RebuildKept();
        if (m_kept.empty() || budget == 0) return;
        // The residency walk is cheap but not free at tens of thousands of
        // kept chunks; every fifth tick is plenty for chunks that stay put.
        if (serverTick % 5 != 0) return;
        for (const ChunkPos& pos : m_kept) {
            if (isResidentOrPending(pos)) continue;
            requestLoad(pos);
            if (--budget == 0) break;
        }
    }

    void ChunkKeeper::Save() {
        if (m_readOnly) return;
        if (m_forcedDirty) SaveForced();
        if (m_indexDirty)  SaveIndex();
    }

} // namespace Server
