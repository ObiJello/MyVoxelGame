// File: src/server/world/storage/AsyncChunkSaver.cpp
#include "AsyncChunkSaver.hpp"
#include "AnvilChunkSaver.hpp"
#include "common/world/chunk/Chunk.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/block/BlockState.hpp"
#include <unordered_map>
#include <string>
#include <filesystem>
#include <fstream>
#include <zlib.h>
#include <chrono>
#include <algorithm>
#include <iomanip>
#include <sstream>
#include <cstring>

namespace Game {

    namespace {

        // ── Save keys (MC's section palette, adapted) ───────────────────────
        //
        // A section is written the way MC writes one: a palette of (block name,
        // properties) plus one index per voxel. Names, not BlockIDs, so a save
        // survives the enum being renumbered — which the pending collapse of
        // the synthetic *SlabTop/*SlabDouble/... ids will do.
        //
        // The wrinkle vanilla does not have: 141 BlockIDs are synthetic
        // variants SHARING a registry slug with their base block (OakSlab,
        // OakSlabTop and OakSlabDouble are all "oak_slab"), because each stands
        // in for a property value this engine has not modelled as a property
        // yet. A slug alone therefore does not identify a BlockID. `variant` —
        // the block's position among the ids sharing its slug, in BlockID order
        // — closes the gap. When those ids collapse into real properties every
        // group has size one and this is always 0, and the format becomes
        // exactly vanilla's without another change here.
        struct SaveKeyTable {
            std::vector<std::string> keyOf;      // BlockID -> slug ("" = unwritable)
            std::vector<uint8_t>     variantOf;  // BlockID -> index within its slug group
            std::unordered_map<std::string, std::vector<BlockID>> group;

            SaveKeyTable() {
                const size_t n = BlockRegistry::Size;
                keyOf.resize(n);
                variantOf.assign(n, 0);
                size_t unnamed = 0;
                for (size_t i = 0; i < n; ++i) {
                    const auto id = static_cast<BlockID>(i);
                    std::string slug = BlockRegistry::Get(id).registrySlug;
                    // Air is not a BlockDefs.inc row, so it has no slug of its
                    // own; it is by far the most common value in a save and
                    // must round-trip.
                    if (id == BlockID::Air) slug = "air";
                    keyOf[i] = slug;
                    if (slug.empty()) { ++unnamed; continue; }
                    auto& g = group[slug];
                    variantOf[i] = static_cast<uint8_t>(g.size());
                    g.push_back(id);
                }
                if (unnamed > 0) {
                    // Not fatal — such blocks save as air rather than corrupting
                    // the stream — but it is silent data loss, so say so.
                    Log::Warning("[ChunkSerializer] %zu BlockID(s) have no registry slug "
                                 "and cannot be saved; they will load back as air", unnamed);
                }
            }

            BlockID Resolve(const std::string& key, uint8_t variant) const {
                auto it = group.find(key);
                if (it == group.end()) return BlockID::Air;   // MC: unknown name -> air
                if (variant >= it->second.size()) return it->second[0];
                return it->second[variant];
            }
        };

        const SaveKeyTable& SaveKeys() {
            static const SaveKeyTable t;   // built on first save, after registry init
            return t;
        }

        inline void PutU8 (std::vector<uint8_t>& b, uint8_t v)  { b.push_back(v); }
        inline void PutU16(std::vector<uint8_t>& b, uint16_t v) {
            b.push_back(static_cast<uint8_t>(v & 0xFF));
            b.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
        }
        inline void PutStr(std::vector<uint8_t>& b, std::string_view s) {
            // Registry slugs and property names/values are all short; one length
            // byte is plenty and keeps the palette compact.
            const size_t n = s.size() > 255 ? 255 : s.size();
            b.push_back(static_cast<uint8_t>(n));
            b.insert(b.end(), s.begin(), s.begin() + static_cast<ptrdiff_t>(n));
        }

        inline bool TakeU8(const uint8_t*& d, size_t& rem, uint8_t& out) {
            if (rem < 1) return false;
            out = *d++; --rem; return true;
        }
        inline bool TakeU16(const uint8_t*& d, size_t& rem, uint16_t& out) {
            if (rem < 2) return false;
            out = static_cast<uint16_t>(d[0] | (static_cast<uint16_t>(d[1]) << 8));
            d += 2; rem -= 2; return true;
        }
        inline bool TakeStr(const uint8_t*& d, size_t& rem, std::string& out) {
            uint8_t n;
            if (!TakeU8(d, rem, n)) return false;
            if (rem < n) return false;
            out.assign(reinterpret_cast<const char*>(d), n);
            d += n; rem -= n; return true;
        }

    } // namespace

    // === CHUNK COMPRESSOR IMPLEMENTATION ===

    std::vector<uint8_t> ChunkCompressor::CompressChunk(const Chunk& chunk, int compressionLevel) {
        std::vector<uint8_t> serializedData = ChunkSerializer::SerializeChunk(chunk);
        return CompressData(serializedData, compressionLevel);
    }

    std::vector<uint8_t> ChunkCompressor::CompressData(const std::vector<uint8_t>& data, int compressionLevel) {
        if (data.empty()) {
            return {};
        }

        z_stream stream = {};
        stream.avail_in = static_cast<uInt>(data.size());
        stream.next_in = const_cast<Bytef*>(data.data());

        if (deflateInit(&stream, compressionLevel) != Z_OK) {
            Log::Error("Failed to initialize zlib compression");
            return {};
        }

        std::vector<uint8_t> compressed;
        compressed.resize(data.size()); // Start with original size

        int ret;
        do {
            stream.avail_out = static_cast<uInt>(compressed.size() - stream.total_out);
            stream.next_out = compressed.data() + stream.total_out;

            ret = deflate(&stream, Z_FINISH);

            if (ret == Z_STREAM_ERROR) {
                deflateEnd(&stream);
                Log::Error("Zlib compression stream error");
                return {};
            }

            if (stream.avail_out == 0 && ret != Z_STREAM_END) {
                // Need more output space
                compressed.resize(compressed.size() * 2);
            }

        } while (ret != Z_STREAM_END);

        deflateEnd(&stream);

        // Resize to actual compressed size
        compressed.resize(stream.total_out);
        return compressed;
    }

    size_t ChunkCompressor::EstimateCompressedSize(const Chunk& chunk) {
        // Rough estimate: 60% of original size
        size_t estimatedRawSize = ChunkSerializer::EstimateSerializedSize(chunk);
        return static_cast<size_t>(estimatedRawSize * 0.6f);
    }

    // === CHUNK SERIALIZER IMPLEMENTATION ===

    std::vector<uint8_t> ChunkSerializer::SerializeChunk(const Chunk& chunk) {
        std::vector<uint8_t> buffer;
        buffer.reserve(EstimateSerializedSize(chunk));

        // Write chunk header
        // Magic number (4 bytes)
        const uint32_t magic = 0x434B4E4B; // "CKNK"
        buffer.insert(buffer.end(), reinterpret_cast<const uint8_t*>(&magic),
                     reinterpret_cast<const uint8_t*>(&magic) + 4);

        // Version (4 bytes)
        // v2: palette of (name, properties) + indices. v1 was a flat dump of
        // BlockIDs that kept no block states at all; it is still readable.
        const uint32_t version = 2;
        buffer.insert(buffer.end(), reinterpret_cast<const uint8_t*>(&version),
                     reinterpret_cast<const uint8_t*>(&version) + 4);

        // Chunk position (8 bytes)
        buffer.insert(buffer.end(), reinterpret_cast<const uint8_t*>(&chunk.pos.x),
                     reinterpret_cast<const uint8_t*>(&chunk.pos.x) + 4);
        buffer.insert(buffer.end(), reinterpret_cast<const uint8_t*>(&chunk.pos.z),
                     reinterpret_cast<const uint8_t*>(&chunk.pos.z) + 4);

        // Serialize each section
        for (int sectionY = 0; sectionY < Math::SECTIONS_PER_CHUNK; ++sectionY) {
            const ChunkSection* section = chunk.GetSection(sectionY);
            SerializeSection(section, buffer);
        }

        return buffer;
    }

    bool ChunkSerializer::DeserializeChunk(const std::vector<uint8_t>& data, Chunk& chunk) {
        if (data.size() < 16) { // Minimum header size
            return false;
        }

        const uint8_t* ptr = data.data();
        size_t remaining = data.size();

        // Read magic number
        uint32_t magic;
        std::memcpy(&magic, ptr, 4);
        ptr += 4;
        remaining -= 4;

        if (magic != 0x434B4E4B) {
            Log::Error("Invalid chunk file magic number");
            return false;
        }

        // Read version
        uint32_t version;
        std::memcpy(&version, ptr, 4);
        ptr += 4;
        remaining -= 4;

        if (version != 1 && version != 2) {
            Log::Error("Unsupported chunk file version: %u", version);
            return false;
        }

        // Read chunk position
        std::memcpy(&chunk.pos.x, ptr, 4);
        ptr += 4;
        remaining -= 4;
        std::memcpy(&chunk.pos.z, ptr, 4);
        ptr += 4;
        remaining -= 4;

        // Deserialize sections
        for (int sectionY = 0; sectionY < Math::SECTIONS_PER_CHUNK; ++sectionY) {
            chunk.EnsureSection(sectionY);
            ChunkSection* section = chunk.GetSection(sectionY);
            if (!section || !DeserializeSection(ptr, remaining, *section, version)) {
                return false;
            }
        }

        return true;
    }

    size_t ChunkSerializer::EstimateSerializedSize(const Chunk& chunk) {
        size_t size = 16; // Header

        // Per section: exists flag + palette-count + indexed flag + 4096 uint16
        // indices, plus a palette allowance. A palette entry is a slug, a
        // variant byte, and up to ~6 name/value pairs; 160 bytes covers the
        // worst of them and this is only a reserve() hint.
        size += Math::SECTIONS_PER_CHUNK * (1 + 2 + 1 + 4096 * 2 + 64 * 160);

        return size;
    }

    void ChunkSerializer::SerializeSection(const ChunkSection* section, std::vector<uint8_t>& buffer) {
        if (!section) {
            buffer.push_back(0);          // absent
            return;
        }
        buffer.push_back(1);

        const SaveKeyTable& keys = SaveKeys();
        const PalettedContainer& states = section->States();

        // Build the palette: distinct flat state ids, first-seen order.
        std::unordered_map<uint32_t, uint16_t> indexOf;
        std::vector<uint32_t> palette;
        std::vector<uint16_t> voxels(ChunkSection::TOTAL);
        for (size_t i = 0; i < ChunkSection::TOTAL; ++i) {
            const uint32_t sid = states.Get(i);
            auto it = indexOf.find(sid);
            if (it == indexOf.end()) {
                // A palette index is written as uint16, and the global state
                // space is ~30k, so a section can never overflow this.
                const auto next = static_cast<uint16_t>(palette.size());
                it = indexOf.emplace(sid, next).first;
                palette.push_back(sid);
            }
            voxels[i] = it->second;
        }

        PutU16(buffer, static_cast<uint16_t>(palette.size()));
        for (const uint32_t sid : palette) {
            const BlockState st = BlockState::FromRawId(sid);
            const BlockID    b  = st.Block();
            const size_t     bi = static_cast<size_t>(b);

            PutStr(buffer, bi < keys.keyOf.size() ? keys.keyOf[bi] : std::string{});
            PutU8 (buffer, bi < keys.variantOf.size() ? keys.variantOf[bi] : 0);

            const uint16_t nprops = BlockStates::PropertyCount(b);
            PutU8(buffer, static_cast<uint8_t>(nprops));
            for (uint16_t slot = 0; slot < nprops; ++slot) {
                const PropertyId prop = BlockStates::PropertyAt(b, slot);
                PutStr(buffer, BlockStates::PropertyName(prop));
                PutStr(buffer, st.GetName(prop));
            }
        }

        // A section of one block type — most of a 384-block column — stores no
        // index array at all, mirroring the single-value palette the in-memory
        // container uses for the same case.
        if (palette.size() <= 1) {
            PutU8(buffer, 0);
            return;
        }
        PutU8(buffer, 1);
        for (const uint16_t v : voxels) PutU16(buffer, v);
    }

    bool ChunkSerializer::DeserializeSection(const uint8_t*& data, size_t& remaining,
                                             ChunkSection& section, uint32_t version) {
        uint8_t exists;
        if (!TakeU8(data, remaining, exists)) return false;

        if (!exists) {
            section = ChunkSection{};
            return true;
        }

        if (version == 1) {
            // v1: a flat dump of 4096 BlockIDs and no states at all. Read it so
            // worlds saved before the state port still open; every block comes
            // back in its default state because that is genuinely all v1 kept.
            const size_t blockDataSize = ChunkSection::TOTAL * sizeof(uint16_t);
            if (remaining < blockDataSize) return false;
            for (int y = 0; y < ChunkSection::SIZE; ++y) {
                for (int z = 0; z < ChunkSection::SIZE; ++z) {
                    for (int x = 0; x < ChunkSection::SIZE; ++x) {
                        const uint16_t raw = static_cast<uint16_t>(
                            data[0] | (static_cast<uint16_t>(data[1]) << 8));
                        section.Set(x, y, z, raw);
                        data += 2;
                    }
                }
            }
            remaining -= blockDataSize;
            section.RecountRandomTicking();
            return true;
        }

        const SaveKeyTable& keys = SaveKeys();

        uint16_t paletteCount;
        if (!TakeU16(data, remaining, paletteCount)) return false;
        if (paletteCount == 0) return false;

        std::vector<uint32_t> palette;
        palette.reserve(paletteCount);
        std::string key, propName, propValue;
        for (uint16_t e = 0; e < paletteCount; ++e) {
            uint8_t variant, propCount;
            if (!TakeStr(data, remaining, key))       return false;
            if (!TakeU8 (data, remaining, variant))   return false;
            if (!TakeU8 (data, remaining, propCount)) return false;

            const BlockID b = keys.Resolve(key, variant);
            // MC NbtUtils.readBlockState: start from the DEFAULT state and set
            // only what the save names. Anything this build no longer models —
            // a removed property, a renamed value — is skipped rather than
            // failing the load, so a world opened on an older build degrades to
            // sane states instead of refusing to open.
            BlockState st = BlockStates::Default(b);
            const uint16_t nprops = BlockStates::PropertyCount(b);
            for (uint8_t i = 0; i < propCount; ++i) {
                if (!TakeStr(data, remaining, propName))  return false;
                if (!TakeStr(data, remaining, propValue)) return false;
                for (uint16_t slot = 0; slot < nprops; ++slot) {
                    const PropertyId prop = BlockStates::PropertyAt(b, slot);
                    if (BlockStates::PropertyName(prop) == propName) {
                        st = st.SetName(prop, propValue);
                        break;
                    }
                }
            }
            palette.push_back(st.RawId());
        }

        uint8_t indexed;
        if (!TakeU8(data, remaining, indexed)) return false;

        if (!indexed) {
            const BlockState ref = BlockState::FromRawId(palette[0]);
            for (int y = 0; y < ChunkSection::SIZE; ++y)
                for (int z = 0; z < ChunkSection::SIZE; ++z)
                    for (int x = 0; x < ChunkSection::SIZE; ++x)
                        section.SetBlockState(x, y, z, ref.Block(), ref.Index());
            section.RecountRandomTicking();
            return true;
        }

        if (remaining < ChunkSection::TOTAL * sizeof(uint16_t)) return false;
        for (size_t i = 0; i < ChunkSection::TOTAL; ++i) {
            const uint16_t pi = static_cast<uint16_t>(
                data[0] | (static_cast<uint16_t>(data[1]) << 8));
            data += 2;
            // A corrupt or truncated index must not read off the end of the
            // palette; air is the safe answer, as it is for an unknown name.
            const uint32_t sid = pi < palette.size() ? palette[pi] : 0u;
            const BlockState ref = BlockState::FromRawId(sid);
            const int x = static_cast<int>(i & 15);
            const int z = static_cast<int>((i >> 4) & 15);
            const int y = static_cast<int>((i >> 8) & 15);
            section.SetBlockState(x, y, z, ref.Block(), ref.Index());
        }
        remaining -= ChunkSection::TOTAL * sizeof(uint16_t);

        // This path fills a section without going through ChunkSection::Set, so
        // the random-tick census and the has-states flag it maintains have to be
        // rebuilt by hand. Skipping it would load a saved wheat field that
        // silently never grows again.
        section.RecountRandomTicking();
        return true;
    }

    // === ASYNC CHUNK SAVER IMPLEMENTATION ===

    AsyncChunkSaver::AsyncChunkSaver(const SaveWorkerConfig& config)
    : m_config(config) {

        // Set default destination if not using Anvil format
        if (!m_config.enableAnvilFormat && m_destinationPath.empty()) {
            m_destinationPath = "saves/chunks";
        }

        Log::Debug("AsyncChunkSaver created with %d worker threads", config.workerThreads);
    }

    AsyncChunkSaver::~AsyncChunkSaver() {
        Shutdown();
    }

    // === CORE SAVING INTERFACE ===

    ChunkSaveResult AsyncChunkSaver::SaveChunk(const Chunk& chunk) {
        if (!m_initialized) {
            return ChunkSaveResult::Failure(chunk.pos, "Saver not initialized");
        }

        switch (m_saveMode) {
            case SaveMode::Immediate:
                return SaveChunkInternal(chunk, m_compressionEnabled);

            case SaveMode::Queued: {
                auto clonedChunk = chunk.Clone();
                QueueChunkForSave(clonedChunk);
                return ChunkSaveResult::Success(chunk.pos);
            }

            case SaveMode::Background: {
                auto future = SaveChunkAsync(chunk);
                // For synchronous call, wait for completion
                return future.get();
            }

            case SaveMode::OnShutdown: {
                auto clonedChunk = chunk.Clone();
                QueueChunkForSave(clonedChunk);
                return ChunkSaveResult::Success(chunk.pos);
            }

            default:
                return ChunkSaveResult::Failure(chunk.pos, "Unknown save mode");
        }
    }

    std::future<ChunkSaveResult> AsyncChunkSaver::SaveChunkAsync(const Chunk& chunk) {
        if (!m_initialized) {
            std::promise<ChunkSaveResult> promise;
            promise.set_value(ChunkSaveResult::Failure(chunk.pos, "Saver not initialized"));
            return promise.get_future();
        }

        auto clonedChunk = chunk.Clone();
        SaveQueueEntry entry(clonedChunk, m_saveMode);
        auto future = entry.promise->get_future();

        EnqueueSaveEntry(std::move(entry));
        return future;
    }

    void AsyncChunkSaver::QueueChunkForSave(std::shared_ptr<const Chunk> chunk) {
        if (!chunk || !m_initialized) {
            return;
        }

        SaveQueueEntry entry(chunk, m_saveMode);
        // For queued saves, we don't need the future result
        entry.promise.reset();

        EnqueueSaveEntry(std::move(entry));
    }

    std::vector<ChunkSaveResult> AsyncChunkSaver::FlushPendingSaves() {
        std::vector<ChunkSaveResult> results;

        if (!m_initialized) {
            return results;
        }

        // Signal workers to flush and wait for completion
        WaitForQueueEmpty(30.0f); // 30 second timeout

        return results; // Results are handled by worker threads
    }

    std::future<std::vector<ChunkSaveResult>> AsyncChunkSaver::FlushPendingSavesAsync() {
        auto promise = std::make_shared<std::promise<std::vector<ChunkSaveResult>>>();
        auto future = promise->get_future();

        // Submit flush task to thread pool
        JobSystem::g_ThreadPool.Enqueue([this, promise]() {
            std::vector<ChunkSaveResult> results = FlushPendingSaves();
            promise->set_value(std::move(results));
        });

        return future;
    }

    // === BATCH OPERATIONS ===

    std::vector<ChunkSaveResult> AsyncChunkSaver::SaveChunks(const std::vector<std::shared_ptr<const Chunk>>& chunks) {
        std::vector<ChunkSaveResult> results;
        results.reserve(chunks.size());

        for (const auto& chunk : chunks) {
            if (chunk) {
                results.push_back(SaveChunk(*chunk));
            } else {
                results.push_back(ChunkSaveResult::Failure(Math::ChunkPos{0, 0}, "Null chunk pointer"));
            }
        }

        return results;
    }

    // === CONFIGURATION ===

    void AsyncChunkSaver::SetDestination(const std::string& destinationPath) {
        if (m_initialized) {
            Log::Warning("Cannot change destination while saver is running");
            return;
        }

        m_destinationPath = destinationPath;
        Log::Info("AsyncChunkSaver destination set to: %s", destinationPath.c_str());
    }

    std::string AsyncChunkSaver::GetDestination() const {
        return m_destinationPath;
    }

    void AsyncChunkSaver::SetCompressionEnabled(bool enabled) {
        m_compressionEnabled = enabled;
    }

    bool AsyncChunkSaver::IsCompressionEnabled() const {
        return m_compressionEnabled;
    }

    void AsyncChunkSaver::SetCompressionLevel(int level) {
        m_compressionLevel = std::clamp(level, 0, 9);
    }

    int AsyncChunkSaver::GetCompressionLevel() const {
        return m_compressionLevel;
    }

    // === SAVE POLICIES ===

    void AsyncChunkSaver::SetSaveMode(SaveMode mode) {
        m_saveMode = mode;
    }

    AsyncChunkSaver::SaveMode AsyncChunkSaver::GetSaveMode() const {
        return m_saveMode;
    }

    void AsyncChunkSaver::SetAutoSaveEnabled(bool enabled) {
        m_autoSaveEnabled = enabled;
        if (enabled) {
            m_lastAutoSave = std::chrono::steady_clock::now();
        }
    }

    void AsyncChunkSaver::SetAutoSaveInterval(float seconds) {
        m_autoSaveInterval = std::max(1.0f, seconds);
    }

    bool AsyncChunkSaver::IsAutoSaveEnabled() const {
        return m_autoSaveEnabled;
    }

    float AsyncChunkSaver::GetAutoSaveInterval() const {
        return m_autoSaveInterval;
    }

    // === LIFECYCLE ===

    bool AsyncChunkSaver::Initialize() {
        if (m_initialized) {
            return true;
        }

        Log::Info("Initializing AsyncChunkSaver...");

        try {
            // For custom format, validate destination path only if it's set
            if (!m_config.enableAnvilFormat) {
                if (m_destinationPath.empty()) {
                    // Set a default destination path if none provided
                    m_destinationPath = "saves/chunks";
                    Log::Info("No destination path set, using default: %s", m_destinationPath.c_str());
                }

                // Create destination directory for custom format
                if (!CreateDirectoryRecursive(m_destinationPath)) {
                    Log::Warning("Failed to create destination directory: %s", m_destinationPath.c_str());
                    // Don't fail initialization - we can create it later when needed
                }
            }

            // Initialize Anvil saver if requested
            if (m_config.enableAnvilFormat) {
                if (!m_config.minecraftWorldPath.empty()) {
                    if (!InitializeAnvilSaver()) {
                        Log::Warning("Failed to initialize Anvil saver, falling back to custom format");
                        m_useAnvilFormat = false;

                        // Fall back to custom format
                        if (m_destinationPath.empty()) {
                            m_destinationPath = "saves/chunks";
                            Log::Info("Fallback: Using custom format with destination: %s", m_destinationPath.c_str());
                        }
                    } else {
                        m_useAnvilFormat = true;
                        Log::Info("AsyncChunkSaver: Using Anvil .mca format for world: %s",
                                 m_config.minecraftWorldPath.c_str());
                    }
                } else {
                    Log::Warning("Anvil format requested but no world path provided, using custom format");
                    m_config.enableAnvilFormat = false;
                    m_useAnvilFormat = false;
                    if (m_destinationPath.empty()) {
                        m_destinationPath = "saves/chunks";
                    }
                }
            }

            // Start worker threads
            m_workersRunning = true;
            m_shutdownRequested = false;

            try {
                for (int i = 0; i < m_config.workerThreads; ++i) {
                    m_workerThreads.emplace_back(&AsyncChunkSaver::WorkerThreadMain, this);
                    Log::Debug("Started worker thread %d", i);
                }
                Log::Info("Started %d worker threads", m_config.workerThreads);
            } catch (const std::exception& e) {
                Log::Error("Failed to start worker threads: %s", e.what());
                // Clean up any started threads
                m_shutdownRequested = true;
                for (auto& thread : m_workerThreads) {
                    if (thread.joinable()) {
                        thread.join();
                    }
                }
                m_workerThreads.clear();
                return false;
            }

            // Reset statistics
            {
                std::lock_guard<std::mutex> lock(m_statsMutex);
                m_stats.Reset();
            }

            m_initialized = true;
            Log::Info("AsyncChunkSaver initialized with %d worker threads (format: %s)",
                     m_config.workerThreads, m_useAnvilFormat ? "Anvil" : "Custom");
            return true;

        } catch (const std::exception& e) {
            Log::Error("AsyncChunkSaver initialization failed: %s", e.what());
            SetLastError("Initialization failed: " + std::string(e.what()));
            return false;
        } catch (...) {
            Log::Error("AsyncChunkSaver initialization failed with unknown exception");
            SetLastError("Unknown initialization error");
            return false;
        }
    }

    void AsyncChunkSaver::Shutdown() {
        if (!m_initialized) {
            return;
        }

        Log::Info("Shutting down AsyncChunkSaver...");

        // Save any pending chunks in immediate mode
        if (m_saveMode == SaveMode::OnShutdown) {
            FlushPendingSaves();
        }

        // Signal workers to stop
        m_shutdownRequested = true;
        m_queueCondition.notify_all();

        // Wait for workers to finish
        for (auto& thread : m_workerThreads) {
            if (thread.joinable()) {
                thread.join();
            }
        }

        m_workerThreads.clear();
        m_workersRunning = false;

        // NEW: Shutdown Anvil saver
        ShutdownAnvilSaver();

        // Clear queues
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            while (!m_normalPriorityQueue.empty()) {
                m_normalPriorityQueue.pop();
            }
            while (!m_highPriorityQueue.empty()) {
                m_highPriorityQueue.pop();
            }
            m_pendingPositions.clear();
        }

        m_initialized = false;
        Log::Info("AsyncChunkSaver shutdown complete");
    }

    bool AsyncChunkSaver::IsReady() const {
        return m_initialized && m_workersRunning;
    }

    // === QUEUE MANAGEMENT ===

    size_t AsyncChunkSaver::GetPendingChunkCount() const {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        return m_normalPriorityQueue.size() + m_highPriorityQueue.size();
    }

    void AsyncChunkSaver::ClearPendingChunks() {
        std::lock_guard<std::mutex> lock(m_queueMutex);

        while (!m_normalPriorityQueue.empty()) {
            m_normalPriorityQueue.pop();
        }
        while (!m_highPriorityQueue.empty()) {
            m_highPriorityQueue.pop();
        }

        m_pendingPositions.clear();
        Log::Info("Cleared all pending chunk saves");
    }

    bool AsyncChunkSaver::IsChunkPending(Math::ChunkPos position) const {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        return m_pendingPositions.find(position) != m_pendingPositions.end();
    }

    bool AsyncChunkSaver::RemovePendingChunk(Math::ChunkPos position) {
        std::lock_guard<std::mutex> lock(m_queueMutex);

        auto it = m_pendingPositions.find(position);
        if (it != m_pendingPositions.end()) {
            m_pendingPositions.erase(it);
            return true;
        }

        return false;
    }

    // === STATISTICS ===

    AsyncChunkSaver::SaverStats AsyncChunkSaver::GetStats() const {
        std::lock_guard<std::mutex> lock(m_statsMutex);
        return m_stats;
    }

    void AsyncChunkSaver::ResetStats() {
        std::lock_guard<std::mutex> lock(m_statsMutex);
        m_stats.Reset();
    }

    // === VALIDATION ===

    bool AsyncChunkSaver::CanSaveChunk(const Chunk& chunk) const {
        // Basic validation
        if (chunk.IsEmpty()) {
            return false; // Don't save empty chunks
        }

        return true;
    }

    bool AsyncChunkSaver::VerifySavedChunk(Math::ChunkPos position) {
        std::string filePath = GetChunkSavePath(position);
        return FileExists(filePath) && VerifyFileIntegrity(filePath);
    }

    // === ERROR HANDLING ===

    std::string AsyncChunkSaver::GetLastError() const {
        std::lock_guard<std::mutex> lock(m_errorMutex);
        return m_lastError;
    }

    void AsyncChunkSaver::ClearErrors() {
        std::lock_guard<std::mutex> lock(m_errorMutex);
        m_lastError.clear();
    }

    void AsyncChunkSaver::SetErrorPolicy(ErrorPolicy policy) {
        m_errorPolicy = policy;
    }

    AsyncChunkSaver::ErrorPolicy AsyncChunkSaver::GetErrorPolicy() const {
        return m_errorPolicy;
    }

    // === BACKUP AND RECOVERY ===

    void AsyncChunkSaver::SetBackupEnabled(bool enabled) {
        m_backupEnabled = enabled;
    }

    bool AsyncChunkSaver::IsBackupEnabled() const {
        return m_backupEnabled;
    }

    void AsyncChunkSaver::SetMaxBackups(int maxBackups) {
        m_maxBackups = std::max(0, maxBackups);
    }

    int AsyncChunkSaver::GetMaxBackups() const {
        return m_maxBackups;
    }

    // === ASYNC-SPECIFIC FEATURES ===

    void AsyncChunkSaver::SetWorkerThreadCount(int count) {
        if (m_initialized) {
            Log::Warning("Cannot change worker thread count while saver is running");
            return;
        }

        m_config.workerThreads = std::max(1, count);
    }

    int AsyncChunkSaver::GetWorkerThreadCount() const {
        return m_config.workerThreads;
    }

    bool AsyncChunkSaver::AreWorkersRunning() const {
        return m_workersRunning;
    }

    void AsyncChunkSaver::SaveChunkHighPriority(std::shared_ptr<const Chunk> chunk) {
        if (!chunk || !m_initialized) {
            return;
        }

        SaveQueueEntry entry(chunk, m_saveMode, true);
        entry.promise.reset(); // Don't need result for high priority saves

        EnqueueSaveEntry(std::move(entry));
    }

    size_t AsyncChunkSaver::GetHighPriorityQueueSize() const {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        return m_highPriorityQueue.size();
    }

    void AsyncChunkSaver::SetBatchingEnabled(bool enabled) {
        m_config.enableBatching = enabled;
    }

    bool AsyncChunkSaver::IsBatchingEnabled() const {
        return m_config.enableBatching;
    }

    void AsyncChunkSaver::SetBatchSize(int size) {
        m_config.batchSize = std::max(1, size);
    }

    int AsyncChunkSaver::GetBatchSize() const {
        return m_config.batchSize;
    }

    float AsyncChunkSaver::GetAverageSaveTime() const {
        std::lock_guard<std::mutex> lock(m_statsMutex);
        return m_stats.averageSaveTimeMs;
    }

    float AsyncChunkSaver::GetCompressionRatio() const {
        std::lock_guard<std::mutex> lock(m_statsMutex);
        return static_cast<float>(m_stats.compressionRatio);
    }

    size_t AsyncChunkSaver::GetTotalBytesWritten() const {
        std::lock_guard<std::mutex> lock(m_statsMutex);
        return m_stats.totalBytesWritten;
    }

    size_t AsyncChunkSaver::GetQueueMemoryUsage() const {
        return CalculateQueueMemoryUsage();
    }

    void AsyncChunkSaver::FlushAllQueues() {
        WaitForQueueEmpty(60.0f); // 1 minute timeout
    }

    bool AsyncChunkSaver::WaitForQueueEmpty(float timeoutSeconds) {
        auto deadline = std::chrono::steady_clock::now() +
                       std::chrono::milliseconds(static_cast<int>(timeoutSeconds * 1000));

        while (std::chrono::steady_clock::now() < deadline) {
            if (GetPendingChunkCount() == 0) {
                return true;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        return false; // Timeout
    }

    // === PROTECTED METHODS ===

    void AsyncChunkSaver::UpdateStats(const ChunkSaveResult& result) {
        std::lock_guard<std::mutex> lock(m_statsMutex);

        if (result.success) {
            m_stats.chunksSaved++;
            m_stats.totalSaveTimeMs += result.saveTimeMs;
            m_stats.totalBytesWritten += result.bytesWritten;

            if (m_stats.chunksSaved > 0) {
                m_stats.averageSaveTimeMs = m_stats.totalSaveTimeMs / m_stats.chunksSaved;
            }
        } else {
            m_stats.saveFailures++;
        }
    }

    // === WORKER THREAD IMPLEMENTATION ===

    void AsyncChunkSaver::WorkerThreadMain() {
        Log::Debug("AsyncChunkSaver worker thread started");

        while (!m_shutdownRequested) {
            SaveQueueEntry entry({}, SaveMode::Immediate);

            if (GetNextSaveEntry(entry, 100.0f)) {
                ChunkSaveResult result = ProcessSaveEntry(entry);

                if (entry.promise) {
                    entry.promise->set_value(std::move(result));
                }

                RemoveFromPending(entry.chunk->pos);
            }
        }

        Log::Debug("AsyncChunkSaver worker thread stopped");
    }

    ChunkSaveResult AsyncChunkSaver::ProcessSaveEntry(const SaveQueueEntry& entry) {
        if (!entry.chunk) {
            return ChunkSaveResult::Failure(Math::ChunkPos{0, 0}, "Null chunk");
        }

        auto startTime = std::chrono::high_resolution_clock::now();

        ChunkSaveResult result = SaveChunkInternal(*entry.chunk, m_compressionEnabled);

        auto endTime = std::chrono::high_resolution_clock::now();
        result.saveTimeMs = std::chrono::duration<float, std::milli>(endTime - startTime).count();

        UpdateStats(result);
        return result;
    }

    bool AsyncChunkSaver::GetNextSaveEntry(SaveQueueEntry& entry, float timeoutMs) {
        std::unique_lock<std::mutex> lock(m_queueMutex);

        auto timeout = std::chrono::milliseconds(static_cast<int>(timeoutMs));

        if (m_queueCondition.wait_for(lock, timeout, [this] {
            return m_shutdownRequested || !m_highPriorityQueue.empty() || !m_normalPriorityQueue.empty();
        })) {
            if (m_shutdownRequested) {
                return false;
            }

            // Process high priority queue first
            if (!m_highPriorityQueue.empty()) {
                entry = std::move(m_highPriorityQueue.front());
                m_highPriorityQueue.pop();
                return true;
            }

            // Then normal priority queue
            if (!m_normalPriorityQueue.empty()) {
                entry = std::move(m_normalPriorityQueue.front());
                m_normalPriorityQueue.pop();
                return true;
            }
        }

        return false;
    }

    // === CORE SAVE IMPLEMENTATION ===

    ChunkSaveResult AsyncChunkSaver::SaveChunkInternal(const Chunk& chunk, bool compress) {
        // NEW: Route to Anvil saver if enabled
        if (ShouldUseAnvilFormat()) {
            return SaveChunkAnvil(chunk);
        }

        if (!CanSaveChunk(chunk)) {
            return ChunkSaveResult::Failure(chunk.pos, "Chunk validation failed");
        }

        try {
            // Create backup if enabled
            if (m_backupEnabled) {
                CreateBackup(chunk.pos);
            }

            // Serialize chunk
            std::vector<uint8_t> data;
            if (compress && m_compressionEnabled) {
                data = ChunkCompressor::CompressChunk(chunk, m_compressionLevel);
            } else {
                data = ChunkSerializer::SerializeChunk(chunk);
            }

            if (data.empty()) {
                return ChunkSaveResult::Failure(chunk.pos, "Failed to serialize chunk");
            }

            // Write to file
            if (!WriteChunkToFile(chunk, data)) {
                return ChunkSaveResult::Failure(chunk.pos, "Failed to write chunk file");
            }

            // Clean old backups
            if (m_backupEnabled) {
                CleanOldBackups(chunk.pos);
            }

            ChunkSaveResult result = ChunkSaveResult::Success(chunk.pos, data.size());
            result.wasCompressed = compress && m_compressionEnabled;
            return result;

        } catch (const std::exception& e) {
            std::string error = "Exception saving chunk: " + std::string(e.what());
            SetLastError(error);
            return ChunkSaveResult::Failure(chunk.pos, error);
        }
    }

    bool AsyncChunkSaver::WriteChunkToFile(const Chunk& chunk, const std::vector<uint8_t>& data) {
        std::string filePath = GetChunkSavePath(chunk.pos);

        if (!EnsureSaveDirectory(chunk.pos)) {
            return false;
        }

        std::ofstream file(filePath, std::ios::binary);
        if (!file.is_open()) {
            SetLastError("Failed to open file for writing: " + filePath);
            return false;
        }

        file.write(reinterpret_cast<const char*>(data.data()), data.size());

        if (!file.good()) {
            SetLastError("Failed to write chunk data to file: " + filePath);
            return false;
        }

        return true;
    }

    std::string AsyncChunkSaver::GetChunkSavePath(Math::ChunkPos position) const {
        // Create directory structure similar to Minecraft: r.x.z.mca pattern
        std::ostringstream oss;
        oss << m_destinationPath << "/chunk_" << position.x << "_" << position.z << ".dat";
        return oss.str();
    }

    bool AsyncChunkSaver::EnsureSaveDirectory(Math::ChunkPos position) const {
        return CreateDirectoryRecursive(m_destinationPath);
    }

    // === QUEUE MANAGEMENT ===

    void AsyncChunkSaver::EnqueueSaveEntry(SaveQueueEntry&& entry) {
        std::lock_guard<std::mutex> lock(m_queueMutex);

        if (!HasQueueSpace()) {
            Log::Warning("Save queue is full, dropping chunk save for (%d, %d)",
                        entry.chunk->pos.x, entry.chunk->pos.z);
            return;
        }

        Math::ChunkPos pos = entry.chunk->pos;

        // Check if already pending
        if (m_pendingPositions.find(pos) != m_pendingPositions.end()) {
            return; // Already queued
        }

        // Add to appropriate queue
        if (entry.highPriority) {
            m_highPriorityQueue.push(std::move(entry));
        } else {
            m_normalPriorityQueue.push(std::move(entry));
        }

        m_pendingPositions.insert(pos);
        m_queueCondition.notify_one();
    }

    void AsyncChunkSaver::RemoveFromPending(Math::ChunkPos position) {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_pendingPositions.erase(position);
    }

    bool AsyncChunkSaver::HasQueueSpace() const {
        return GetTotalQueueSize() < m_config.maxQueueSize;
    }

    size_t AsyncChunkSaver::GetTotalQueueSize() const {
        return m_normalPriorityQueue.size() + m_highPriorityQueue.size();
    }

    // === AUTO-SAVE SYSTEM ===

    bool AsyncChunkSaver::ShouldAutoSave() const {
        if (!m_autoSaveEnabled) {
            return false;
        }

        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - m_lastAutoSave);
        return elapsed.count() >= m_autoSaveInterval;
    }

    void AsyncChunkSaver::TriggerAutoSave() {
        // Auto-save implementation would need integration with the world system
        // to get all dirty chunks. For now, this is a placeholder.
        Log::Info("Auto-save triggered");
        m_lastAutoSave = std::chrono::steady_clock::now();
    }

    // === BACKUP SYSTEM ===

    bool AsyncChunkSaver::CreateBackup(Math::ChunkPos position) {
        if (!m_backupEnabled) {
            return true;
        }

        std::string originalPath = GetChunkSavePath(position);
        if (!FileExists(originalPath)) {
            return true; // No file to backup
        }

        // Find next backup index
        int backupIndex = 0;
        std::string backupPath;
        do {
            backupPath = GetBackupPath(position, backupIndex);
            backupIndex++;
        } while (FileExists(backupPath) && backupIndex < m_maxBackups);

        if (backupIndex >= m_maxBackups) {
            // Remove oldest backup
            std::string oldestBackup = GetBackupPath(position, 0);
            std::filesystem::remove(oldestBackup);

            // Shift all backups down
            for (int i = 1; i < m_maxBackups; ++i) {
                std::string from = GetBackupPath(position, i);
                std::string to = GetBackupPath(position, i - 1);
                if (FileExists(from)) {
                    std::filesystem::rename(from, to);
                }
            }
            backupPath = GetBackupPath(position, m_maxBackups - 1);
        }

        // Copy original to backup
        try {
            std::filesystem::copy_file(originalPath, backupPath);
            return true;
        } catch (const std::exception& e) {
            Log::Warning("Failed to create backup for chunk (%d, %d): %s",
                        position.x, position.z, e.what());
            return false;
        }
    }

    void AsyncChunkSaver::CleanOldBackups(Math::ChunkPos position) {
        if (!m_backupEnabled) {
            return;
        }

        // Remove excess backups
        for (int i = m_maxBackups; i < m_maxBackups + 10; ++i) {
            std::string backupPath = GetBackupPath(position, i);
            if (FileExists(backupPath)) {
                std::filesystem::remove(backupPath);
            }
        }
    }

    std::string AsyncChunkSaver::GetBackupPath(Math::ChunkPos position, int backupIndex) const {
        std::ostringstream oss;
        oss << m_destinationPath << "/backups/chunk_" << position.x << "_" << position.z
            << ".backup." << backupIndex << ".dat";
        return oss.str();
    }

    // === VALIDATION ===

    bool AsyncChunkSaver::ValidateChunkData(const Chunk& chunk) const {
        // Additional validation beyond CanSaveChunk
        if (chunk.pos.x == 0 && chunk.pos.z == 0) {
            return false; // Invalid default position
        }

        // Check for reasonable non-air block count
        size_t nonAirBlocks = chunk.GetNonAirBlockCount();
        size_t totalBlocks = chunk.GetBlockCount();

        if (nonAirBlocks == 0) {
            return false; // Completely empty chunk
        }

        if (nonAirBlocks == totalBlocks) {
            Log::Warning("Chunk (%d, %d) is completely solid, this might be incorrect",
                        chunk.pos.x, chunk.pos.z);
        }

        return true;
    }

    bool AsyncChunkSaver::VerifyFileIntegrity(const std::string& filePath) const {
        std::ifstream file(filePath, std::ios::binary);
        if (!file.is_open()) {
            return false;
        }

        // Read magic number
        uint32_t magic;
        file.read(reinterpret_cast<char*>(&magic), sizeof(magic));

        if (!file.good() || magic != 0x434B4E4B) {
            return false;
        }

        // Basic file size check
        file.seekg(0, std::ios::end);
        size_t fileSize = file.tellg();

        // Minimum size check (header + at least some data)
        return fileSize >= 16;
    }

    // === ERROR HANDLING ===

    void AsyncChunkSaver::SetLastError(const std::string& error) const {
        std::lock_guard<std::mutex> lock(m_errorMutex);
        m_lastError = error;
    }

    void AsyncChunkSaver::LogError(const std::string& operation, const std::string& error) const {
        Log::Error("AsyncChunkSaver %s: %s", operation.c_str(), error.c_str());
        SetLastError(operation + ": " + error);
    }

    bool AsyncChunkSaver::ShouldRetryOnError(const std::string& error) const {
        // Determine if an error is retryable
        return error.find("disk full") == std::string::npos &&
               error.find("permission denied") == std::string::npos &&
               error.find("file not found") == std::string::npos;
    }

    // === STATISTICS ===

    void AsyncChunkSaver::UpdateSaveStats(const ChunkSaveResult& result, size_t originalSize, size_t compressedSize) {
        std::lock_guard<std::mutex> lock(m_statsMutex);

        if (result.success) {
            m_stats.chunksSaved++;
            m_stats.totalSaveTimeMs += result.saveTimeMs;
            m_stats.totalBytesWritten += result.bytesWritten;

            if (m_stats.chunksSaved > 0) {
                m_stats.averageSaveTimeMs = m_stats.totalSaveTimeMs / m_stats.chunksSaved;
            }

            UpdateCompressionStats(originalSize, compressedSize);
        } else {
            m_stats.saveFailures++;
        }
    }

    void AsyncChunkSaver::UpdateCompressionStats(size_t originalSize, size_t compressedSize) {
        if (originalSize > 0 && compressedSize > 0) {
            size_t ratio = (compressedSize * 100) / originalSize;

            // Update running average of compression ratio
            if (m_stats.chunksSaved > 1) {
                m_stats.compressionRatio = (m_stats.compressionRatio + ratio) / 2;
            } else {
                m_stats.compressionRatio = ratio;
            }
        }
    }

    // === UTILITY METHODS ===

    size_t AsyncChunkSaver::CalculateQueueMemoryUsage() const {
        std::lock_guard<std::mutex> lock(m_queueMutex);

        size_t usage = 0;

        // Estimate memory usage of queued chunks
        size_t totalQueuedChunks = m_normalPriorityQueue.size() + m_highPriorityQueue.size();
        usage += totalQueuedChunks * sizeof(Chunk); // Rough estimate

        // Add queue overhead
        usage += sizeof(SaveQueueEntry) * totalQueuedChunks;

        return usage;
    }

    std::string AsyncChunkSaver::GetTimestamp() const {
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);

        std::ostringstream oss;
        oss << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S");
        return oss.str();
    }

    bool AsyncChunkSaver::FileExists(const std::string& path) const {
        return std::filesystem::exists(path);
    }

    bool AsyncChunkSaver::CreateDirectoryRecursive(const std::string& path) const {
        if (path.empty()) {
            return false;
        }

        try {
            if (std::filesystem::exists(path)) {
                if (std::filesystem::is_directory(path)) {
                    return true; // Already exists and is a directory
                } else {
                    Log::Error("Path exists but is not a directory: %s", path.c_str());
                    return false;
                }
            }

            // Create the directory and any parent directories
            bool result = std::filesystem::create_directories(path);
            if (result) {
                Log::Debug("Created directory: %s", path.c_str());
            } else {
                Log::Warning("Failed to create directory: %s", path.c_str());
            }
            return result;

        } catch (const std::filesystem::filesystem_error& e) {
            Log::Error("Filesystem error creating directory %s: %s", path.c_str(), e.what());
            return false;
        } catch (const std::exception& e) {
            Log::Error("Exception creating directory %s: %s", path.c_str(), e.what());
            return false;
        }
    }

    size_t AsyncChunkSaver::GetFileSize(const std::string& path) const {
        try {
            return std::filesystem::file_size(path);
        } catch (const std::exception&) {
            return 0;
        }
    }

    // === RAII SAVE OPERATION ===

    SaveOperation::SaveOperation(AsyncChunkSaver* saver, std::shared_ptr<const Chunk> chunk, bool highPriority)
        : m_saver(saver), m_chunk(chunk) {

        if (m_saver && m_chunk) {
            if (highPriority) {
                m_saver->SaveChunkHighPriority(m_chunk);
            } else {
                m_future = m_saver->SaveChunkAsync(*m_chunk);
            }
        }
    }

    SaveOperation::~SaveOperation() {
        if (!m_cancelled && m_future.valid()) {
            // Wait for completion if not cancelled
            try {
                m_future.get();
            } catch (const std::exception& e) {
                Log::Warning("SaveOperation destructor caught exception: %s", e.what());
            }
        }
    }

    std::future<ChunkSaveResult> SaveOperation::GetFuture() {
        return std::move(m_future);
    }

    bool SaveOperation::IsComplete() const {
        return m_future.valid() &&
               m_future.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
    }

    bool SaveOperation::WaitForCompletion(float timeoutSeconds) {
        if (!m_future.valid()) {
            return false;
        }

        auto timeout = std::chrono::milliseconds(static_cast<int>(timeoutSeconds * 1000));
        return m_future.wait_for(timeout) == std::future_status::ready;
    }

    void SaveOperation::Cancel() {
        m_cancelled = true;
        // Note: We can't actually cancel an in-progress save, but we can mark it as cancelled
    }

    // === ANVIL FORMAT INTEGRATION ===

    bool AsyncChunkSaver::InitializeAnvilSaver() {
        if (m_config.minecraftWorldPath.empty()) {
            Log::Warning("Anvil format requested but no Minecraft world path specified");
            return false;
        }

        try {
            // Configure Anvil saver
            AnvilSaverConfig anvilConfig;
            anvilConfig.worldPath = m_config.minecraftWorldPath;
            anvilConfig.createWorldStructure = m_config.createMinecraftStructure;
            anvilConfig.maxCachedRegions = m_config.maxRegionCacheSize;
            anvilConfig.enableAsyncSaving = true;
            anvilConfig.strictAnvilCompliance = true;

            // Create Anvil saver
            m_anvilSaver = std::make_unique<AnvilChunkSaver>(anvilConfig);

            if (!m_anvilSaver->Initialize()) {
                std::string error = m_anvilSaver->GetLastError();
                Log::Error("Failed to initialize AnvilChunkSaver: %s", error.c_str());
                m_anvilSaver.reset();
                return false;
            }

            Log::Info("AnvilChunkSaver initialized for world: %s", anvilConfig.worldPath.c_str());
            return true;

        } catch (const std::exception& e) {
            Log::Error("Exception initializing AnvilChunkSaver: %s", e.what());
            m_anvilSaver.reset();
            return false;
        }
    }

    void AsyncChunkSaver::ShutdownAnvilSaver() {
        if (m_anvilSaver) {
            m_anvilSaver->Shutdown();
            m_anvilSaver.reset();
        }
    }

    ChunkSaveResult AsyncChunkSaver::SaveChunkAnvil(const Chunk& chunk) {
        if (!m_anvilSaver) {
            return ChunkSaveResult::Failure(chunk.pos, "Anvil saver not initialized");
        }

        // Use the Anvil saver for actual saving
        ChunkSaveResult result = m_anvilSaver->SaveChunk(chunk);
        
        // Update our statistics with the result
        UpdateSaveStats(result, 0, 0); // Size info not available from Anvil saver
        
        return result;
    }

    // === ANVIL FORMAT UTILITY METHODS ===

    void AsyncChunkSaver::EnableAnvilFormat(const std::string& minecraftWorldPath) {
        if (m_initialized) {
            Log::Warning("Cannot change save format while saver is running");
            return;
        }

        m_config.enableAnvilFormat = true;
        m_config.minecraftWorldPath = minecraftWorldPath;
        m_config.createMinecraftStructure = true;
        
        Log::Info("AsyncChunkSaver configured for Anvil format: %s", minecraftWorldPath.c_str());
    }

    void AsyncChunkSaver::DisableAnvilFormat() {
        if (m_initialized) {
            Log::Warning("Cannot change save format while saver is running");
            return;
        }

        m_config.enableAnvilFormat = false;
        m_config.minecraftWorldPath.clear();
        
        Log::Info("AsyncChunkSaver configured for custom format");
    }

    bool AsyncChunkSaver::IsAnvilFormatEnabled() const {
        return m_config.enableAnvilFormat && m_useAnvilFormat;
    }

    std::string AsyncChunkSaver::GetMinecraftWorldPath() const {
        return m_config.minecraftWorldPath;
    }

    // === UTILITY FUNCTIONS ===

    std::unique_ptr<IChunkSaver> CreateAsyncChunkSaver(const SaveWorkerConfig& config) {
        return std::make_unique<AsyncChunkSaver>(config);
    }

} // namespace Game