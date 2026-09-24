#include "world/level/chunk/storage/SerializableChunkData.h"
#include "world/ProtoChunk.h"
#include "world/chunk/status/ChunkStatus.h"
#include "levelgen/structure/StructureSerialization.h"
#include "nbt/AllTags.h"
#include "nbt/CanonicalNbt.h"
#include "util/SimpleBitStorage.h"
#include "util/Palette.h"  // ceillog2
#include "world/level/block/Blocks.h"
#include "world/level/block/Block.h"
#include <mutex>
#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <unordered_map>

// Reference: net/minecraft/world/level/chunk/storage/SerializableChunkData.java

namespace minecraft {
namespace world {
namespace level {
namespace chunk {
namespace storage {

namespace {

using ::minecraft::util::ceillog2;
using ::minecraft::world::level::block::Block;
using ::minecraft::world::level::block::Blocks;
using ::minecraft::world::chunk::status::ChunkStatus;
using HeightmapType = levelgen::Heightmap::Types;

constexpr int kBlocksPerSection = 4096;
constexpr int kBiomesPerSection = 64;
// The largest bit width with a local palette (HashMapPalette); above it the
// container uses the global palette.
constexpr int kMaxLocalPaletteBits = 8;

// Heightmap.Types.getSerializationKey, in Heightmap.Types declaration order.
const std::vector<std::pair<HeightmapType, const char*>>& heightmapKeys() {
    static const std::vector<std::pair<HeightmapType, const char*>> keys = {
        {HeightmapType::WORLD_SURFACE_WG, "WORLD_SURFACE_WG"},
        {HeightmapType::WORLD_SURFACE, "WORLD_SURFACE"},
        {HeightmapType::OCEAN_FLOOR_WG, "OCEAN_FLOOR_WG"},
        {HeightmapType::OCEAN_FLOOR, "OCEAN_FLOOR"},
        {HeightmapType::MOTION_BLOCKING, "MOTION_BLOCKING"},
        {HeightmapType::MOTION_BLOCKING_NO_LEAVES, "MOTION_BLOCKING_NO_LEAVES"},
    };
    return keys;
}

// A saved "Status" as MC 26.3 reads it. Saves from before 26.3 name the
// three statuses 26.3 merged into TERRAIN; MC's MergeTerrainChunkStatusFix
// (DataVersion 5013, util/datafix/fixes/MergeTerrainChunkStatusFix.java)
// maps "carvers" to "terrain", and "noise"/"surface" to "biomes" — a chunk
// stopped half way through terrain restarts it, so the fix also drops its
// block states and heightmaps (`intermediateTerrain`).
struct ResolvedStatus {
    const ChunkStatus* status = nullptr;
    bool intermediateTerrain = false;
};

ResolvedStatus resolveStatusName(std::string name) {
    if (name.rfind("minecraft:", 0) == 0) name = name.substr(10);
    if (name == "carvers") return {&ChunkStatus::TERRAIN, false};
    if (name == "noise" || name == "surface") return {&ChunkStatus::BIOMES, true};
    return {ChunkStatus::byName(name), false};
}

// The Status string for a DataVersion: before MergeTerrainChunkStatusFix a
// finished-terrain chunk is "carvers" (every other status kept its name).
std::string statusNameFor(const ChunkStatus& status, int dataVersion) {
    if (dataVersion < ChunkSerializer::MERGED_TERRAIN_STATUS_VERSION && &status == &ChunkStatus::TERRAIN) {
        return "minecraft:carvers";
    }
    return "minecraft:" + status.getName();
}

// Strategy.createForBlockStates / createForBiomes: bits a palette of `size`
// entries occupies ON DISK (Configuration.bitsInStorage). Blocks: 0 for a
// single value, at least 4 otherwise; biomes: ceillog2(size).
int blockBitsOnDisk(size_t size) {
    const int bits = ceillog2(static_cast<int32_t>(size));
    return bits == 0 ? 0 : std::max(4, bits);
}

int biomeBitsOnDisk(size_t size) {
    return ceillog2(static_cast<int32_t>(size));
}

size_t packedLongCount(int bits, int entries) {
    const int perLong = 64 / bits;
    return static_cast<size_t>((entries + perLong - 1) / perLong);
}

// SimpleBitStorage layout: values never span two longs.
std::vector<int32_t> unpackIndices(const std::vector<int64_t>& data, int bits, int entries,
                                   size_t paletteSize, const char* what) {
    std::vector<int32_t> out(static_cast<size_t>(entries), 0);
    if (bits == 0) return out;
    if (data.size() != packedLongCount(bits, entries)) {
        throw std::runtime_error(std::string("Invalid length given for storage (") + what + "), got: "
                                 + std::to_string(data.size()) + " but expected: "
                                 + std::to_string(packedLongCount(bits, entries)));
    }
    util::SimpleBitStorage storage(bits, entries, data);
    for (int i = 0; i < entries; ++i) {
        int32_t index = storage.get(i);
        // MC's palette lookup of an out-of-range id yields the default
        // value; index 0 is always a valid entry.
        if (static_cast<size_t>(index) >= paletteSize) index = 0;
        out[static_cast<size_t>(i)] = index;
    }
    return out;
}

std::vector<int64_t> packIndices(const std::vector<int32_t>& indices, int bits) {
    util::SimpleBitStorage storage(bits, static_cast<int32_t>(indices.size()));
    for (size_t i = 0; i < indices.size(); ++i) {
        storage.set(static_cast<int32_t>(i), indices[i]);
    }
    return storage.getRaw();
}

// PalettedContainer.pack over a copyOf snapshot: a fresh palette in
// first-seen order over index order, so the bytes are what MC itself writes.
void packSnapshotBlocks(const SerializableChunkData::SectionData& section, std::vector<BlockState*>& palette,
                        std::vector<int32_t>& indices) {
    indices.resize(kBlocksPerSection);
    if (!section.snapshotCells.empty()) {
        std::unordered_map<BlockState*, int32_t> ids;
        for (int index = 0; index < kBlocksPerSection; ++index) {
            BlockState* state = section.snapshotCells[static_cast<size_t>(index)];
            auto [it, inserted] = ids.emplace(state, static_cast<int32_t>(palette.size()));
            if (inserted) palette.push_back(state);
            indices[static_cast<size_t>(index)] = it->second;
        }
        return;
    }
    // Container id -> packed id, filled as each id is first met.
    std::vector<int32_t> remap(section.snapshotPalette.size(), -1);
    auto take = [&](int32_t id) {
        if (id < 0 || static_cast<size_t>(id) >= remap.size()) {
            throw std::runtime_error("chunk snapshot: palette id out of range");
        }
        int32_t& packed = remap[static_cast<size_t>(id)];
        if (packed < 0) {
            packed = static_cast<int32_t>(palette.size());
            palette.push_back(section.snapshotPalette[static_cast<size_t>(id)]);
        }
        return packed;
    };
    if (section.snapshotBits == 0) {
        const int32_t packed = take(0);
        std::fill(indices.begin(), indices.end(), packed);
        return;
    }
    const util::SimpleBitStorage storage(section.snapshotBits, kBlocksPerSection, section.snapshotData);
    for (int index = 0; index < kBlocksPerSection; ++index) {
        indices[static_cast<size_t>(index)] = take(storage.get(index));
    }
}

void packSnapshotBiomes(const SerializableChunkData::SectionData& section, std::vector<std::string>& palette,
                        std::vector<int32_t>& indices) {
    indices.resize(kBiomesPerSection);
    std::vector<const world::biome::Biome*> seen;
    for (int index = 0; index < kBiomesPerSection; ++index) {
        const world::biome::Biome* biome = section.snapshotBiomes[static_cast<size_t>(index)];
        size_t id = 0;
        while (id < seen.size() && seen[id] != biome) ++id;
        if (id == seen.size()) {
            seen.push_back(biome);
            palette.push_back(biome ? std::string(biome->getName()) : std::string("minecraft:plains"));
        }
        indices[static_cast<size_t>(index)] = static_cast<int32_t>(id);
    }
}

// A block-state palette entry: {Name, Properties} (before 5006) or
// {id, properties} (26.3), resolved through the block table (the block
// registry id map only knows the states used so far). A property the block
// lacks or a value it does not take keeps that property's default, as MC's
// StateHolder codec does; an unknown block becomes air (orElsePartial).
BlockState* readBlockState(const nbt::CompoundTag& tag, BlockState* airBlock) {
    std::string name = tag.getStringOr("id", "");
    const nbt::CompoundTag* props = tag.getCompoundPtr("properties");
    if (name.empty()) {
        name = tag.getStringOr("Name", "minecraft:air");
        props = tag.getCompoundPtr("Properties");
    }
    std::map<std::string, std::string> properties;
    if (props != nullptr) {
        for (const auto& [key, value] : *props) {
            if (value->getId() == nbt::TagType::TAG_STRING) {
                properties[key] = static_cast<const nbt::StringTag*>(value.get())->getValue();
            }
        }
    }

    BlockState* resolved = Blocks::resolveState(name, properties);
    return resolved != nullptr ? resolved : airBlock;
}

std::unique_ptr<nbt::CompoundTag> writeBlockState(const BlockState* state, int dataVersion) {
    const bool newNames = dataVersion >= ChunkSerializer::BLOCK_STATE_FIELD_NAMES_VERSION;
    auto tag = std::make_unique<nbt::CompoundTag>();
    tag->putString(newNames ? "id" : "Name", state != nullptr ? state->getIdentifier() : "minecraft:air");
    if (state != nullptr && state->hasProperties()) {
        auto props = std::make_unique<nbt::CompoundTag>();
        for (const auto& [key, value] : state->getProperties()) {
            props->putString(key, value);
        }
        tag->put(newNames ? "properties" : "Properties", std::move(props));
    }
    return tag;
}

std::unique_ptr<nbt::CompoundTag> copyCompound(const nbt::CompoundTag& tag) {
    return std::unique_ptr<nbt::CompoundTag>(static_cast<nbt::CompoundTag*>(tag.copy().release()));
}

// ---- engine extension: structure spawn areas ------------------------------

constexpr const char* kSpawnAreasKey = "obeycraft:structure_spawn_areas";
constexpr const char* kFinalizeSpawnKey = "obeycraft:finalize_spawn";

std::unique_ptr<nbt::Tag> writeBox(const levelgen::structure::BoundingBox& box) {
    return std::make_unique<nbt::IntArrayTag>(std::vector<int32_t>{
        box.minX, box.minY, box.minZ, box.maxX, box.maxY, box.maxZ});
}

bool readBox(const nbt::CompoundTag& tag, const std::string& key, levelgen::structure::BoundingBox& out) {
    const std::vector<int32_t> v = tag.getIntArray(key);
    if (v.size() != 6) return false;
    out = levelgen::structure::BoundingBox(v[0], v[1], v[2], v[3], v[4], v[5]);
    return true;
}

} // namespace

// =========================================================================
// parse
// =========================================================================

std::unique_ptr<SerializableChunkData> SerializableChunkData::parse(
    int minY, int height, const nbt::CompoundTag& chunkData, BlockState* airBlock)
{
    const std::string statusName = chunkData.getStringOr("Status", "");
    if (statusName.empty()) {
        return nullptr;
    }

    auto data = std::unique_ptr<SerializableChunkData>(new SerializableChunkData());
    data->m_chunkPos = ChunkPos(chunkData.getIntOr("xPos", 0), chunkData.getIntOr("zPos", 0));
    data->m_lastUpdateTime = chunkData.getLongOr("LastUpdate", 0);
    data->m_inhabitedTime = chunkData.getLongOr("InhabitedTime", 0);
    const ResolvedStatus resolved = resolveStatusName(statusName);
    data->m_chunkStatus = resolved.status != nullptr ? resolved.status : &ChunkStatus::EMPTY;
    data->m_lightCorrect = chunkData.getBooleanOr("isLightOn", false);
    data->m_minSectionY = minY >> 4;
    const int maxSectionY = ((minY + height) >> 4) - 1;
    const int sectionCount = maxSectionY - data->m_minSectionY + 1;

    if (const nbt::CompoundTag* heightmaps = chunkData.getCompoundPtr("Heightmaps")) {
        for (const auto& [type, key] : heightmapKeys()) {
            std::vector<int64_t> values = heightmaps->getLongArray(key);
            if (!values.empty()) data->m_heightmaps[type] = std::move(values);
        }
    }

    // PostProcessing: one ShortList per section index (empty lists absent).
    if (const nbt::ListTag* post = chunkData.getListPtr("PostProcessing")) {
        data->m_postProcessing.resize(post->size());
        for (size_t i = 0; i < post->size(); ++i) {
            const nbt::Tag* entry = post->get(i);
            if (entry == nullptr || entry->getId() != nbt::TagType::TAG_LIST) continue;
            const nbt::ListTag* offsets = static_cast<const nbt::ListTag*>(entry);
            for (size_t j = 0; j < offsets->size(); ++j) {
                const nbt::Tag* value = offsets->get(j);
                if (value != nullptr && value->getId() == nbt::TagType::TAG_SHORT) {
                    data->m_postProcessing[i].push_back(static_cast<const nbt::ShortTag*>(value)->getValue());
                }
            }
        }
    }

    if (const nbt::ListTag* entities = chunkData.getListPtr("entities")) {
        for (size_t i = 0; i < entities->size(); ++i) {
            const nbt::CompoundTag* entity = entities->getCompound(i);
            if (entity == nullptr) continue;
            auto copy = copyCompound(*entity);
            ::minecraft::world::IChunk::GeneratedEntity generated;
            generated.finalizeSpawn = copy->getByteOr(kFinalizeSpawnKey, 0) != 0;
            copy->remove(kFinalizeSpawnKey);
            generated.tag = std::shared_ptr<const nbt::CompoundTag>(std::move(copy));
            data->m_entities.push_back(std::move(generated));
        }
    }

    if (const nbt::ListTag* blockEntities = chunkData.getListPtr("block_entities")) {
        for (size_t i = 0; i < blockEntities->size(); ++i) {
            if (const nbt::CompoundTag* blockEntity = blockEntities->getCompound(i)) {
                data->m_blockEntities.push_back(copyCompound(*blockEntity));
            }
        }
    }

    if (const nbt::CompoundTag* structures = chunkData.getCompoundPtr("structures")) {
        data->m_structureData = copyCompound(*structures);
    }

    if (const nbt::ListTag* areas = chunkData.getListPtr(kSpawnAreasKey)) {
        for (size_t i = 0; i < areas->size(); ++i) {
            const nbt::CompoundTag* areaTag = areas->getCompound(i);
            if (areaTag == nullptr) continue;
            ::minecraft::world::IChunk::StructureSpawnArea area;
            area.structure = areaTag->getStringOr("structure", "");
            if (area.structure.empty() || !readBox(*areaTag, "box", area.startBox)) continue;
            if (const nbt::ListTag* pieces = areaTag->getListPtr("pieces")) {
                for (size_t j = 0; j < pieces->size(); ++j) {
                    const nbt::CompoundTag* pieceTag = pieces->getCompound(j);
                    if (pieceTag == nullptr) continue;
                    ::minecraft::world::IChunk::StructureSpawnPiece piece;
                    if (!readBox(*pieceTag, "box", piece.box)) continue;
                    piece.templateId = pieceTag->getStringOr("template", "");
                    piece.rotation = pieceTag->getIntOr("rotation", 0);
                    piece.pieceType = pieceTag->getStringOr("type", "");
                    area.pieces.push_back(std::move(piece));
                }
            }
            data->m_spawnAreas.push_back(std::move(area));
        }
    }

    if (const nbt::ListTag* sections = chunkData.getListPtr("sections")) {
        for (size_t i = 0; i < sections->size(); ++i) {
            const nbt::CompoundTag* sectionTag = sections->getCompound(i);
            if (sectionTag == nullptr) continue;
            const int y = sectionTag->getByteOr("Y", 0);
            // Light-only sections (one past each end) carry no container.
            if (y < data->m_minSectionY || y > maxSectionY) continue;

            SectionData section;
            section.y = y;
            if (const nbt::CompoundTag* blockStates = sectionTag->getCompoundPtr("block_states")) {
                if (const nbt::ListTag* palette = blockStates->getListPtr("palette")) {
                    for (size_t p = 0; p < palette->size(); ++p) {
                        const nbt::CompoundTag* entry = palette->getCompound(p);
                        section.blockPalette.push_back(
                            entry != nullptr ? readBlockState(*entry, airBlock) : airBlock);
                    }
                }
                if (!section.blockPalette.empty()) {
                    section.blockIndices = unpackIndices(
                        blockStates->getLongArray("data"), blockBitsOnDisk(section.blockPalette.size()),
                        kBlocksPerSection, section.blockPalette.size(), "block_states");
                }
            }
            if (const nbt::CompoundTag* biomes = sectionTag->getCompoundPtr("biomes")) {
                if (const nbt::ListTag* palette = biomes->getListPtr("palette")) {
                    for (size_t p = 0; p < palette->size(); ++p) {
                        section.biomePalette.push_back(palette->getString(p));
                    }
                }
                if (!section.biomePalette.empty()) {
                    section.biomeIndices = unpackIndices(
                        biomes->getLongArray("data"), biomeBitsOnDisk(section.biomePalette.size()),
                        kBiomesPerSection, section.biomePalette.size(), "biomes");
                }
            }
            data->m_sections.push_back(std::move(section));
        }
    }
    (void)sectionCount;

    // A pre-26.3 chunk saved at NOISE or SURFACE regenerates its terrain from
    // BIOMES: MergeTerrainChunkStatusFix.removeBlockStates drops every
    // section's block states plus the chunk's heightmaps and blending data.
    if (resolved.intermediateTerrain) {
        for (SectionData& section : data->m_sections) {
            section.blockPalette.clear();
            section.blockIndices.clear();
        }
        data->m_heightmaps.clear();
    }

    return data;
}

// =========================================================================
// copyOf
// =========================================================================

std::unique_ptr<SerializableChunkData> SerializableChunkData::copyOf(ProtoChunk& chunk, int64_t gameTime) {
    auto data = std::unique_ptr<SerializableChunkData>(new SerializableChunkData());
    data->m_chunkPos = chunk.getPos();
    data->m_minSectionY = chunk.getMinBuildHeight() >> 4;
    data->m_lastUpdateTime = gameTime;
    data->m_inhabitedTime = chunk.getInhabitedTime();
    data->m_chunkStatus = chunk.getPersistedStatus();
    data->m_lightCorrect = chunk.isLightCorrect();

    // Sections: LevelChunkSection.copy() - the container's palette and
    // packed ids as they are. write() does PalettedContainer.pack (a fresh
    // palette in first-seen order) on the I/O thread.
    for (int i = 0; i < chunk.getSectionsCount(); ++i) {
        const LevelChunkSection& section = chunk.getSection(i);
        const auto& states = section.getStates();
        SectionData out;
        out.y = data->m_minSectionY + i;
        out.snapshot = true;
        out.snapshotBits = states.getBitsPerEntry();
        if (out.snapshotBits <= kMaxLocalPaletteBits) {
            out.snapshotPalette = states.getPaletteEntries();
            out.snapshotData = states.getRawData();
        } else {
            // Global palette (more than 256 states): the palette is the
            // whole registry, so copy the states themselves.
            out.snapshotCells.resize(kBlocksPerSection);
            for (int index = 0; index < kBlocksPerSection; ++index) {
                // Strategy.getIndex: (y << 4 | z) << 4 | x.
                out.snapshotCells[static_cast<size_t>(index)] = states.get(index & 15, index >> 8, (index >> 4) & 15);
            }
        }
        const auto& biomes = section.getBiomes();
        for (int index = 0; index < kBiomesPerSection; ++index) {
            out.snapshotBiomes[static_cast<size_t>(index)] = biomes[static_cast<size_t>(index)];
        }
        data->m_sections.push_back(std::move(out));
    }

    for (const auto& [type, key] : heightmapKeys()) {
        (void)key;
        if (levelgen::Heightmap* heightmap = chunk.getHeightmap(type)) {
            data->m_heightmaps[type] = heightmap->getRawData();
        }
    }

    const auto& postProcessing = chunk.getPostProcessing();
    data->m_postProcessing.resize(postProcessing.size());
    for (size_t i = 0; i < postProcessing.size(); ++i) {
        data->m_postProcessing[i].assign(postProcessing[i].begin(), postProcessing[i].end());
    }

    // Pending block-entity tags, as the chunk holds them (canonical text);
    // write() turns them into the real tags with their positions
    // (ChunkAccess.getBlockEntityNbtForSaving).
    if (const auto* pending = chunk.getBlockEntityNbts()) {
        data->m_blockEntityText.assign(pending->begin(), pending->end());
    }

    if (const auto* entities = chunk.getEntities()) {
        data->m_entities = *entities;
    }
    if (const auto* areas = chunk.getStructureSpawnAreas()) {
        data->m_spawnAreas = *areas;
    }

    data->m_structureData = levelgen::structure::StructureSerialization::packStructureData(
        data->m_chunkPos, chunk.getAllStructureStarts(), chunk.getAllStructureReferences());
    return data;
}

// =========================================================================
// write
// =========================================================================

std::unique_ptr<nbt::CompoundTag> SerializableChunkData::write(int dataVersion) const {
    auto tag = std::make_unique<nbt::CompoundTag>();
    tag->putInt("DataVersion", dataVersion);
    tag->putInt("xPos", m_chunkPos.x());
    tag->putInt("yPos", m_minSectionY);
    tag->putInt("zPos", m_chunkPos.z());
    tag->putLong("LastUpdate", m_lastUpdateTime);
    tag->putLong("InhabitedTime", m_inhabitedTime);
    tag->putString("Status", statusNameFor(*m_chunkStatus, dataVersion));

    auto sections = std::make_unique<nbt::ListTag>();
    for (const SectionData& section : m_sections) {
        auto sectionTag = std::make_unique<nbt::CompoundTag>();
        // A copyOf snapshot is packed here (off the server thread); a parsed
        // chunk already holds the packed form.
        std::vector<BlockState*> snapshotBlockPalette;
        std::vector<int32_t> snapshotBlockIndices;
        std::vector<std::string> snapshotBiomePalette;
        std::vector<int32_t> snapshotBiomeIndices;
        if (section.snapshot) {
            packSnapshotBlocks(section, snapshotBlockPalette, snapshotBlockIndices);
            packSnapshotBiomes(section, snapshotBiomePalette, snapshotBiomeIndices);
        }
        const auto& blockPalette = section.snapshot ? snapshotBlockPalette : section.blockPalette;
        const auto& blockIndices = section.snapshot ? snapshotBlockIndices : section.blockIndices;
        const auto& biomePalette = section.snapshot ? snapshotBiomePalette : section.biomePalette;
        const auto& biomeIndices = section.snapshot ? snapshotBiomeIndices : section.biomeIndices;
        if (!blockPalette.empty()) {
            auto blockStates = std::make_unique<nbt::CompoundTag>();
            auto palette = std::make_unique<nbt::ListTag>();
            for (const BlockState* state : blockPalette) {
                palette->add(writeBlockState(state, dataVersion));
            }
            blockStates->put("palette", std::move(palette));
            const int bits = blockBitsOnDisk(blockPalette.size());
            if (bits != 0) blockStates->putLongArray("data", packIndices(blockIndices, bits));
            sectionTag->put("block_states", std::move(blockStates));
        }
        if (!biomePalette.empty()) {
            auto biomes = std::make_unique<nbt::CompoundTag>();
            auto palette = std::make_unique<nbt::ListTag>();
            for (const std::string& name : biomePalette) {
                palette->add(std::make_unique<nbt::StringTag>(name));
            }
            biomes->put("palette", std::move(palette));
            const int bits = biomeBitsOnDisk(biomePalette.size());
            if (bits != 0) biomes->putLongArray("data", packIndices(biomeIndices, bits));
            sectionTag->put("biomes", std::move(biomes));
        }
        if (!sectionTag->isEmpty()) {
            sectionTag->putByte("Y", static_cast<int8_t>(section.y));
            sections->add(std::move(sectionTag));
        }
    }
    tag->put("sections", std::move(sections));

    if (m_lightCorrect) {
        tag->putBoolean("isLightOn", true);
    }

    auto blockEntities = std::make_unique<nbt::ListTag>();
    for (const auto& blockEntity : m_blockEntities) {
        blockEntities->add(blockEntity->copy());
    }
    for (const auto& [key, text] : m_blockEntityText) {
        std::unique_ptr<nbt::CompoundTag> blockEntity;
        try {
            blockEntity = nbt::canonical::parseCompound(text);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[SerializableChunkData] chunk (%d,%d): dropping unreadable block entity: %s\n",
                         m_chunkPos.x(), m_chunkPos.z(), e.what());
            continue;
        }
        blockEntity->putInt("x", std::get<2>(key));
        blockEntity->putInt("y", std::get<0>(key));
        blockEntity->putInt("z", std::get<1>(key));
        blockEntities->add(std::move(blockEntity));
    }
    tag->put("block_entities", std::move(blockEntities));

    // Proto chunks keep their generated entities (ChunkType.PROTOCHUNK).
    if (m_chunkStatus->getChunkType() == ::minecraft::world::chunk::status::ChunkType::PROTOCHUNK) {
        auto entities = std::make_unique<nbt::ListTag>();
        for (const auto& entity : m_entities) {
            if (!entity.tag) continue;
            auto copy = copyCompound(*entity.tag);
            if (entity.finalizeSpawn) copy->putByte(kFinalizeSpawnKey, 1);
            entities->add(std::move(copy));
        }
        tag->put("entities", std::move(entities));
    }

    // SerializableChunkData.packOffsets: one list per section, empty ones too.
    auto postProcessing = std::make_unique<nbt::ListTag>();
    for (const auto& offsets : m_postProcessing) {
        auto list = std::make_unique<nbt::ListTag>();
        for (int16_t packed : offsets) {
            list->add(std::make_unique<nbt::ShortTag>(packed));
        }
        postProcessing->add(std::move(list));
    }
    tag->put("PostProcessing", std::move(postProcessing));

    auto heightmaps = std::make_unique<nbt::CompoundTag>();
    for (const auto& [type, key] : heightmapKeys()) {
        auto it = m_heightmaps.find(type);
        if (it != m_heightmaps.end()) heightmaps->putLongArray(key, it->second);
    }
    tag->put("Heightmaps", std::move(heightmaps));

    tag->put("structures", m_structureData ? m_structureData->copy()
                                           : std::make_unique<nbt::CompoundTag>());

    if (!m_spawnAreas.empty()) {
        auto areas = std::make_unique<nbt::ListTag>();
        for (const auto& area : m_spawnAreas) {
            auto areaTag = std::make_unique<nbt::CompoundTag>();
            areaTag->putString("structure", area.structure);
            areaTag->put("box", writeBox(area.startBox));
            auto pieces = std::make_unique<nbt::ListTag>();
            for (const auto& piece : area.pieces) {
                auto pieceTag = std::make_unique<nbt::CompoundTag>();
                pieceTag->put("box", writeBox(piece.box));
                if (!piece.templateId.empty()) pieceTag->putString("template", piece.templateId);
                pieceTag->putInt("rotation", piece.rotation);
                if (!piece.pieceType.empty()) pieceTag->putString("type", piece.pieceType);
                pieces->add(std::move(pieceTag));
            }
            areaTag->put("pieces", std::move(pieces));
            areas->add(std::move(areaTag));
        }
        tag->put(kSpawnAreasKey, std::move(areas));
    }
    return tag;
}

// =========================================================================
// getChunkStatusFromTag
// =========================================================================

const ChunkStatusPtr* SerializableChunkData::getChunkStatusFromTag(const nbt::CompoundTag* tag) {
    if (tag == nullptr) return &ChunkStatus::EMPTY;
    const ResolvedStatus resolved = resolveStatusName(tag->getStringOr("Status", "empty"));
    return resolved.status != nullptr ? resolved.status : &ChunkStatus::EMPTY;
}

// =========================================================================
// read
// =========================================================================

std::unique_ptr<ProtoChunk> SerializableChunkData::read(
    const ChunkPos& expectedPos,
    int minY,
    int height,
    BlockState* airBlock,
    BlockState* defaultBlock,
    BlockRegistry* registry) const
{
    if (airBlock == nullptr || registry == nullptr) {
        return nullptr;
    }
    if (!(m_chunkPos == expectedPos)) {
        // SerializableChunkData.read: log and relocate to the indexed spot.
        std::fprintf(stderr, "[SerializableChunkData] chunk file at (%d,%d) is in the wrong location; relocating (got %d,%d)\n",
                     expectedPos.x(), expectedPos.z(), m_chunkPos.x(), m_chunkPos.z());
    }

    auto chunk = std::make_unique<ProtoChunk>(
        expectedPos, minY, height, airBlock, defaultBlock ? defaultBlock : airBlock, registry);
    chunk->setStatus(m_chunkStatus);
    chunk->setInhabitedTime(m_inhabitedTime);

    const int minSectionY = minY >> 4;
    for (const SectionData& sectionData : m_sections) {
        const int sectionIndex = sectionData.y - minSectionY;
        if (sectionIndex < 0 || sectionIndex >= chunk->getSectionsCount()) continue;
        LevelChunkSection& section = chunk->getSection(sectionIndex);

        if (!sectionData.blockPalette.empty()) {
            const size_t paletteSize = sectionData.blockPalette.size();
            if (paletteSize == 1) {
                section.getStates().deserialize(sectionData.blockPalette, {});
            } else if (paletteSize <= 256) {
                // In memory the section keeps MC's linear/hashmap layout for
                // up to 8 bits — the same bit count as on disk.
                section.getStates().deserialize(
                    sectionData.blockPalette,
                    packIndices(sectionData.blockIndices, blockBitsOnDisk(paletteSize)));
            } else {
                // A global-palette section: in memory it indexes the block
                // registry, not this palette, so place each state.
                for (int index = 0; index < kBlocksPerSection; ++index) {
                    section.getStates().set(index & 15, (index >> 8) & 15, (index >> 4) & 15,
                                            sectionData.blockPalette[static_cast<size_t>(
                                                sectionData.blockIndices[static_cast<size_t>(index)])]);
                }
            }
            section.recalcBlockCounts();
        }

        if (!sectionData.biomePalette.empty()) {
            auto& biomes = section.getBiomes();
            std::vector<biome::BiomeHolder> holders;
            holders.reserve(sectionData.biomePalette.size());
            for (const std::string& name : sectionData.biomePalette) {
                holders.push_back(biome::Biomes::get(name));
            }
            for (int index = 0; index < kBiomesPerSection; ++index) {
                biomes[static_cast<size_t>(index)] =
                    holders[static_cast<size_t>(sectionData.biomeIndices[static_cast<size_t>(index)])];
            }
        }
    }

    // Heightmaps: restore the saved ones, prime the ones this status should
    // have but the save lacks (SerializableChunkData.read -> primeHeightmaps).
    std::set<levelgen::Heightmap::Types> toPrime = m_chunkStatus->heightmapsAfter();
    for (const auto& [type, values] : m_heightmaps) {
        chunk->getOrCreateHeightmap(type).setRawData(values);
        toPrime.erase(type);
    }
    if (!toPrime.empty()) {
        levelgen::Heightmap::primeHeightmaps(chunk.get(), toPrime);
    }

    if (m_structureData) {
        levelgen::structure::StructureSerialization::unpackReferences(*m_structureData, expectedPos, *chunk);
        if (const nbt::CompoundTag* starts =
                levelgen::structure::StructureSerialization::savedStarts(*m_structureData)) {
            chunk->setSavedStructureStarts(copyCompound(*starts));
        }
    }

    for (size_t sectionIndex = 0; sectionIndex < m_postProcessing.size(); ++sectionIndex) {
        for (int16_t packed : m_postProcessing[sectionIndex]) {
            chunk->addPackedPostProcess(packed, static_cast<int32_t>(sectionIndex));
        }
    }

    if (m_chunkStatus->getChunkType() == ::minecraft::world::chunk::status::ChunkType::PROTOCHUNK) {
        for (const auto& entity : m_entities) {
            chunk->addEntity(entity);
        }
    }

    for (const auto& blockEntity : m_blockEntities) {
        const core::BlockPos pos(blockEntity->getIntOr("x", 0), blockEntity->getIntOr("y", 0),
                                 blockEntity->getIntOr("z", 0));
        chunk->setBlockEntityNbt(pos, nbt::canonical::serializeBlockEntity(*blockEntity));
    }

    for (const auto& area : m_spawnAreas) {
        chunk->addStructureSpawnArea(area);
    }

    return chunk;
}

} // namespace storage
} // namespace chunk
} // namespace level
} // namespace world
} // namespace minecraft
