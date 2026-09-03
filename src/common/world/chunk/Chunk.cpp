// File: src/common/world/chunk/Chunk.cpp
#include "Chunk.hpp"
#include "../biome/Biomes.hpp"
#include "../block/entity/BlockEntity.hpp"
#include "../../core/Log.hpp"
#include "../math/WorldCoordinates.hpp"
#include <algorithm>

namespace Game {

    // MC ChunkAccess's constructor, whose last act is replaceMissingSections
    // (ChunkAccess.java:101-111):
    //
    //     for (int i = 0; i < sections.length; ++i)
    //         if (sections[i] == null) sections[i] = new LevelChunkSection(f);
    //
    // Every slot is filled, so getSection() is TOTAL and there is no lazy
    // creation anywhere in MC — LevelChunk.setBlockState:248 is just
    // `getSection(getSectionIndex(y))` with no null handling.
    //
    // This engine used to leave the array null and allocate on first write.
    // "Null means all air" is a second encoding of emptiness alongside
    // ChunkSection::IsAllAir, and every place that tested the pointer instead
    // of the contents was a latent bug: the client's render-side mirror went
    // stale so blocks built into a previously-air section were never meshed,
    // and a lazily created section came up with kFallbackBiomeId because the
    // real biomes had never been sent. Both are the same mistake.
    //
    // Measured cost: 296 bytes per ChunkSection, 304 including its two
    // one-entry palette allocations, so 7,296 bytes and 72 allocations per
    // chunk. Against ~24 KB of packed block words for a chunk with terrain in
    // it — and against the ~66 ms it takes to generate one — this is noise.
    // Note also that on the SERVER most of it was already being paid:
    // MyTerrainGenerator fills biomes for all 96 quart layers, and
    // SetBiomeQuart used to call EnsureSection, so a generated chunk already
    // carried 24 sections.
    Chunk::Chunk() {
        for (auto& section : sections) {
            section = std::make_unique<ChunkSection>();
        }
    }

    // Out-of-line so unique_ptr<BlockEntity> destructor sees the complete
    // BlockEntity type (Chunk.hpp only forward-declares it).
    Chunk::~Chunk()                                = default;
    // Written out rather than defaulted: m_contentMutex is not movable, and a
    // move should not carry it anyway — the lock guards the object, and the
    // destination keeps its own. Moving a chunk another thread is holding
    // would be a bug regardless of what happens to the mutex.
    Chunk::Chunk(Chunk&& other) noexcept
        : pos(other.pos)
        , sections(std::move(other.sections))
        , onSectionDirty(std::move(other.onSectionDirty))
        , m_blockEntities(std::move(other.m_blockEntities))
        , m_blockTicks(std::move(other.m_blockTicks))
        , m_heightmaps(other.m_heightmaps)
        , m_heightmapsPrimed(other.m_heightmapsPrimed) {}

    Chunk& Chunk::operator=(Chunk&& other) noexcept {
        if (this == &other) return *this;
        pos                = other.pos;
        sections           = std::move(other.sections);
        onSectionDirty     = std::move(other.onSectionDirty);
        m_blockEntities    = std::move(other.m_blockEntities);
        m_blockTicks       = std::move(other.m_blockTicks);
        m_heightmaps       = other.m_heightmaps;
        m_heightmapsPrimed = other.m_heightmapsPrimed;
        return *this;
    }

    // Block access (local X/Z coordinates, world Y coordinate)
    BlockID Chunk::GetBlock(int localX, int worldY, int localZ) const {
        if (!ValidateCoordinates(localX, worldY, localZ, "GetBlock")) {
            return BlockID::Air;
        }

        // **UPDATED**: Use WorldCoordinates for conversion
        int sectionIndex, sectionY;
        Math::WorldCoordinates::WorldYToSectionCoords(worldY, sectionIndex, sectionY);

        if (sectionIndex < 0 || sectionIndex >= SECTION_COUNT) {
            return BlockID::Air;
        }

        return GetSection(sectionIndex)->GetBlockID(localX, sectionY, localZ);
    }

    // MC ChunkAccess.getNoiseBiome — the caller's block coordinates are shifted
    // right by 2 and clamped into the chunk's quarter grid.
    // Biomes live on the SECTION now (MC LevelChunkSection.biomes). These two
    // keep the chunk-level, world-Y vocabulary the rest of the engine uses and
    // route it to the right section's 4x4x4 container.
    uint16_t Chunk::GetBiome(int localX, int worldY, int localZ) const {
        const int qxAll = std::clamp(localX >> 2, 0, BIOME_HORIZONTAL - 1);
        const int qzAll = std::clamp(localZ >> 2, 0, BIOME_HORIZONTAL - 1);
        // Y is world-space and starts at MIN_WORLD_Y (-64), so it has to be
        // rebased before the shift or everything below y=0 lands in cell 0.
        const int qyAll = std::clamp(
            (worldY - Math::WorldCoordinates::MIN_WORLD_Y) >> 2, 0, BIOME_VERTICAL - 1);

        const int sectionIndex = qyAll / ChunkSection::BIOME_AXIS;
        const ChunkSection* section =
            (sectionIndex >= 0 && sectionIndex < SECTION_COUNT) ? GetSection(sectionIndex) : nullptr;
        if (!section) return kFallbackBiomeId;

        return section->GetBiome(qxAll, qyAll % ChunkSection::BIOME_AXIS, qzAll);
    }

    void Chunk::SetBiomeQuart(int qx, int qy, int qz, uint16_t biomeId) {
        if (qx < 0 || qx >= BIOME_HORIZONTAL || qz < 0 || qz >= BIOME_HORIZONTAL ||
            qy < 0 || qy >= BIOME_VERTICAL) {
            return;
        }
        const int sectionIndex = qy / ChunkSection::BIOME_AXIS;
        if (sectionIndex < 0 || sectionIndex >= SECTION_COUNT) return;
        GetSection(sectionIndex)->SetBiome(qx, qy % ChunkSection::BIOME_AXIS, qz, biomeId);
    }

    BlockStateIndex Chunk::GetBlockState(int localX, int worldY, int localZ) const {
        if (!ValidateCoordinates(localX, worldY, localZ, "GetBlockState")) {
            return 0;
        }

        int sectionIndex, sectionY;
        Math::WorldCoordinates::WorldYToSectionCoords(worldY, sectionIndex, sectionY);

        if (sectionIndex < 0 || sectionIndex >= SECTION_COUNT) {
            return 0;
        }

        return GetSection(sectionIndex)->GetState(localX, sectionY, localZ);
    }

    BlockState Chunk::StateAt(int localX, int worldY, int localZ) const {
        if (!ValidateCoordinates(localX, worldY, localZ, "StateAt")) return BlockState{};

        int sectionIndex, sectionY;
        Math::WorldCoordinates::WorldYToSectionCoords(worldY, sectionIndex, sectionY);
        if (sectionIndex < 0 || sectionIndex >= SECTION_COUNT) return BlockState{};

        return GetSection(sectionIndex)->StateAt(localX, sectionY, localZ);
    }

    void Chunk::SetBlock(int localX, int worldY, int localZ, BlockID blockId, BlockStateIndex stateIndex) {
        blockWriteCounter.fetch_add(1, std::memory_order_release);
        BumpModStamp();
        // Exclusive against the serialiser (see Chunk::LockShared). Cheap: this
        // is the player-edit / block-update path, not terrain generation —
        // generation fills sections in bulk through AdoptStates and never
        // comes through here.
        const auto guard = LockExclusive();

        if (!ValidateCoordinates(localX, worldY, localZ, "SetBlock")) {
            Log::Warning("Attempted to set block at invalid position (%d, %d, %d) in chunk (%d, %d)",
                        localX, worldY, localZ, pos.x, pos.z);
            return;
        }

        int sectionIndex, sectionY;
        Math::WorldCoordinates::WorldYToSectionCoords(worldY, sectionIndex, sectionY);

        if (sectionIndex < 0 || sectionIndex >= SECTION_COUNT) {
            Log::Warning("Invalid section index %d for world Y %d in chunk (%d, %d)",
                        sectionIndex, worldY, pos.x, pos.z);
            return;
        }

        // Deliberately NOT short-circuiting on "same BlockID" the way the
        // BlockID-only overload does: re-orienting a block (same id, different
        // state) is a real change and must still write and dirty the section.
        const BlockID oldBlockId = GetBlock(localX, worldY, localZ);
        const BlockStateIndex oldState = GetBlockState(localX, worldY, localZ);
        if (oldBlockId == blockId && oldState == stateIndex) {
            return;
        }

        ChunkSection* section = GetSection(sectionIndex);

        // MC LevelChunk.setBlockState:249-251 —
        //     boolean wasEmpty = section.hasOnlyAir();
        //     if (wasEmpty && state.isAir()) return null;
        // Air into an already-empty section is not a change worth writing or
        // dirtying. This replaces the old `!HasSection(sectionIndex)` form,
        // which asked about the POINTER; sections are always allocated now, so
        // the question has to be asked of the CONTENTS. It also fixes a case
        // the old guard got wrong: it required stateIndex == 0, so writing air
        // with a non-zero state index into a missing section was silently
        // dropped instead of clearing the cell.
        if (section->IsAllAir() && blockId == BlockID::Air) {
            return;
        }

        section->Set(localX, sectionY, localZ, blockId);
        section->SetState(localX, sectionY, localZ, stateIndex);

        UpdateHeightmaps(localX, worldY, localZ, blockId);

        if (onSectionDirty) {
            onSectionDirty(sectionIndex);
        }
    }

    void Chunk::SetBlock(int localX, int worldY, int localZ, BlockID blockId) {
        BumpModStamp();
        // Exclusive against the serialiser (see Chunk::LockShared). Cheap: this
        // is the player-edit / block-update path, not terrain generation —
        // generation fills sections in bulk through AdoptStates and never
        // comes through here.
        const auto guard = LockExclusive();

        if (!ValidateCoordinates(localX, worldY, localZ, "SetBlock")) {
            Log::Warning("Attempted to set block at invalid position (%d, %d, %d) in chunk (%d, %d)",
                        localX, worldY, localZ, pos.x, pos.z);
            return;
        }

        // **UPDATED**: Use WorldCoordinates for conversion
        int sectionIndex, sectionY;
        Math::WorldCoordinates::WorldYToSectionCoords(worldY, sectionIndex, sectionY);

        if (sectionIndex < 0 || sectionIndex >= SECTION_COUNT) {
            Log::Warning("Invalid section index %d for world Y %d in chunk (%d, %d)",
                        sectionIndex, worldY, pos.x, pos.z);
            return;
        }

        // Get the old block to check if we're actually changing anything
        BlockID oldBlockId = GetBlock(localX, worldY, localZ);
        if (oldBlockId == blockId) {
            return; // No change needed — leaves any existing state untouched
        }

        ChunkSection* section = GetSection(sectionIndex);

        // MC LevelChunk.setBlockState:249-251, same rule as the overload above.
        if (section->IsAllAir() && blockId == BlockID::Air) {
            return;
        }

        section->Set(localX, sectionY, localZ, blockId);

        // The block genuinely changed, so any state left over from the
        // previous occupant is meaningless — state indices are relative to
        // the owning block's own state list. Reset to the new block's
        // default (MC defaultBlockState()). Without this, mining a
        // west-facing furnace and placing stone would leave stone carrying
        // state index 3.
        section->SetState(localX, sectionY, localZ, 0);

        UpdateHeightmaps(localX, worldY, localZ, blockId);

        // Mark section as dirty for mesh rebuilding
        if (onSectionDirty) {
            onSectionDirty(sectionIndex);
        }
    }

    void Chunk::UpdateHeightmaps(int localX, int worldY, int localZ, BlockID newBlock) {
        // MC LevelChunk.setBlockState updates every heightmap on every write.
        // Skipped entirely until the chunk is primed: before that the stored
        // heights are all minY, so an Update would happily conclude that the
        // first block written is the surface and leave every column below it
        // wrong. The prime pass is what establishes the truth.
        if (!m_heightmapsPrimed) return;

        const auto blockAt = [this](int x, int y, int z) { return GetBlock(x, y, z); };

        for (size_t i = 0; i < static_cast<size_t>(HeightmapType::Count); ++i) {
            m_heightmaps[i].Update(localX, worldY, localZ, newBlock,
                                   static_cast<HeightmapType>(i), blockAt);
        }
    }

    void Chunk::PrimeHeightmaps() {
        // MC Heightmap.primeHeightmaps: one downward scan per column, stopping
        // as soon as every map has found its surface.
        //
        // This is the SLOW path and it is meant to be — a freshly generated
        // chunk copies the terrain library's already-computed heights instead
        // (see MyTerrainGenerator), and a loaded chunk reads them from NBT.
        // Reaching here means neither was available.
        constexpr size_t kTypeCount = static_cast<size_t>(HeightmapType::Count);

        // Start from the top of the highest non-empty section rather than the
        // build limit: most chunks are empty above y=128 and scanning that is
        // pure waste.
        // MC ChunkAccess.getHighestFilledSectionIndex (:127-135) asks
        // hasOnlyAir(), not "does the section exist" — which is the only form
        // that works now that every section exists. Testing the pointer here
        // would pin scanTop to y=319 for every chunk and quietly turn this into
        // a full-height scan, i.e. exactly the waste the comment above
        // describes.
        int scanTop = MIN_WORLD_Y;
        const int highest = HighestFilledSectionIndex();
        if (highest != kNoFilledSection) {
            scanTop = Math::WorldCoordinates::SectionCoordsToWorldY(highest, SECTION_HEIGHT - 1);
        }

        for (size_t i = 0; i < kTypeCount; ++i) m_heightmaps[i].Reset(MIN_WORLD_Y);

        for (int x = 0; x < SIZE_X; ++x) {
            for (int z = 0; z < SIZE_Z; ++z) {
                bool found[kTypeCount] = {};
                size_t remaining = kTypeCount;

                for (int y = scanTop; y >= MIN_WORLD_Y && remaining > 0; --y) {
                    const BlockID block = GetBlock(x, y, z);
                    if (block == BlockID::Air) continue;

                    for (size_t i = 0; i < kTypeCount; ++i) {
                        if (found[i]) continue;
                        if (!HeightmapIsOpaque(static_cast<HeightmapType>(i), block)) continue;
                        m_heightmaps[i].SetHeight(x, z, y + 1);
                        found[i] = true;
                        --remaining;
                    }
                }
            }
        }

        m_heightmapsPrimed = true;
    }

    // Section management
    ChunkSection* Chunk::GetSection(int sectionIndex) {
        if (sectionIndex < 0 || sectionIndex >= SECTION_COUNT) {
            return nullptr;
        }
        return sections[sectionIndex].get();
    }

    const ChunkSection* Chunk::GetSection(int sectionIndex) const {
        if (sectionIndex < 0 || sectionIndex >= SECTION_COUNT) {
            return nullptr;
        }
        return sections[sectionIndex].get();
    }

    // MC ChunkAccess.getHighestFilledSectionIndex (:127-135), sentinel and all:
    //
    //     for (int i = sections.length - 1; i >= 0; --i)
    //         if (!sections[i].hasOnlyAir()) return i;
    //     return NO_FILLED_SECTION;   // -1
    //
    // This is the idiom that REPLACES null-checking. Anything that used to ask
    // "which sections exist" is really asking "which sections have anything in
    // them", and that question has always been IsAllAir's.
    int Chunk::HighestFilledSectionIndex() const {
        for (int i = SECTION_COUNT - 1; i >= 0; --i) {
            const ChunkSection* section = sections[i].get();
            if (section && !section->IsAllAir()) return i;
        }
        return kNoFilledSection;
    }

    bool Chunk::HasContentInSection(int sectionIndex) const {
        const ChunkSection* section = GetSection(sectionIndex);
        return section != nullptr && !section->IsAllAir();
    }

    // ========================================================================
    // BLOCK ENTITY STORAGE
    // ========================================================================

    BlockEntity* Chunk::GetBlockEntity(int localX, int worldY, int localZ) {
        auto it = m_blockEntities.find(glm::ivec3(localX, worldY, localZ));
        return (it != m_blockEntities.end()) ? it->second.get() : nullptr;
    }

    const BlockEntity* Chunk::GetBlockEntity(int localX, int worldY, int localZ) const {
        auto it = m_blockEntities.find(glm::ivec3(localX, worldY, localZ));
        return (it != m_blockEntities.end()) ? it->second.get() : nullptr;
    }

    void Chunk::SetBlockEntity(int localX, int worldY, int localZ,
                                std::unique_ptr<BlockEntity> entity) {
        BumpModStamp();
        const auto guard = LockExclusive();
        if (!entity) {
            m_blockEntities.erase(glm::ivec3(localX, worldY, localZ));
            return;
        }
        m_blockEntities[glm::ivec3(localX, worldY, localZ)] = std::move(entity);
    }

    std::unique_ptr<BlockEntity>
    Chunk::RemoveBlockEntity(int localX, int worldY, int localZ) {
        const auto guard = LockExclusive();
        auto it = m_blockEntities.find(glm::ivec3(localX, worldY, localZ));
        if (it == m_blockEntities.end()) return nullptr;
        std::unique_ptr<BlockEntity> out = std::move(it->second);
        m_blockEntities.erase(it);
        return out;
    }

    // Statistics
    size_t Chunk::GetBlockCount() const {
        return SIZE_X * TOTAL_HEIGHT * SIZE_Z;
    }

    size_t Chunk::GetNonAirBlockCount() const {
        size_t count = 0;

        for (int sectionIndex = 0; sectionIndex < SECTION_COUNT; ++sectionIndex) {
            const ChunkSection* section = GetSection(sectionIndex);
            // Emptiness is a CONTENT question. The null test this replaces was
            // free only while sky sections were absent; with every section
            // allocated it never fires, and the triple loop below would run
            // 98,304 times per call over mostly air.
            if (!section || section->IsAllAir()) {
                continue;
            }

            // Off the palette, MC LevelChunkSection.recalcBlockCounts style:
            // the container already knows how many of each distinct state it
            // holds, so there is no reason to visit 4096 voxels.
            section->States().ForEachValue([&](uint32_t stateId, int n) {
                if (BlockState::FromRawId(stateId).Block() != BlockID::Air) {
                    count += static_cast<size_t>(n);
                }
            });
        }

        return count;
    }

    // MC has no direct equivalent — the closest is
    // getHighestFilledSectionIndex() == NO_FILLED_SECTION — but the meaning is
    // the same: is there anything in this column at all.
    //
    // This USED to be `every section pointer is null`, which is permanently
    // false now that the constructor fills them.
    //
    // NOTE: empty is NOT invalid. The End's void chunks are all air and
    // perfectly legitimate; the load/generation validators accept them
    // (rejecting them regenerated saved End chunks on every load and spun the
    // async pipeline in a request/fail loop). Failure on those paths is a
    // null chunk, never an empty one.
    bool Chunk::IsEmpty() const {
        return HighestFilledSectionIndex() == kNoFilledSection;
    }

    // **NEW**: Helper method for coordinate validation with detailed logging
    bool Chunk::ValidateCoordinates(int localX, int worldY, int localZ, const char* operation) const {
        if (localX < 0 || localX >= SIZE_X) {
            Log::Warning("%s: Invalid localX %d (must be 0-%d) in chunk (%d, %d)",
                        operation, localX, SIZE_X - 1, pos.x, pos.z);
            return false;
        }

        if (localZ < 0 || localZ >= SIZE_Z) {
            Log::Warning("%s: Invalid localZ %d (must be 0-%d) in chunk (%d, %d)",
                        operation, localZ, SIZE_Z - 1, pos.x, pos.z);
            return false;
        }

        if (!Math::WorldCoordinates::IsValidWorldY(worldY)) {
            Log::Warning("%s: Invalid worldY %d (must be %d-%d) in chunk (%d, %d)",
                        operation, worldY, MIN_WORLD_Y, MAX_WORLD_Y, pos.x, pos.z);
            return false;
        }

        return true;
    }

} // namespace Game