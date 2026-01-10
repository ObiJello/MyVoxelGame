#include "world/level/chunk/storage/SerializableChunkData.h"
#include "world/ProtoChunk.h"
#include "world/IChunk.h"
#include "server/level/ServerLevel.h"
#include "world/chunk/status/ChunkStatus.h"
#include "nbt/AllTags.h"
#include "util/SimpleBitStorage.h"
#include "util/Palette.h"  // For ceillog2
#include <algorithm>

// Bring ceillog2 into scope
using ::minecraft::util::ceillog2;

// Reference: net/minecraft/world/level/chunk/storage/SerializableChunkData.java

namespace minecraft {
namespace world {
namespace level {
namespace chunk {
namespace storage {

// =========================================================================
// SerializableChunkData parsing
// Reference: SerializableChunkData.java lines 90-162
// =========================================================================

std::unique_ptr<SerializableChunkData> SerializableChunkData::parse(
    int minY,
    int height,
    nbt::CompoundTag& chunkData)
{
    auto data = std::unique_ptr<SerializableChunkData>(new SerializableChunkData());

    // Parse basic info
    // Reference: SerializableChunkData.java lines 95-100
    int chunkX = chunkData.getIntOr("xPos", 0);
    int chunkZ = chunkData.getIntOr("zPos", 0);
    data->m_chunkPos = ChunkPos(chunkX, chunkZ);

    data->m_lastUpdateTime = chunkData.getLongOr("LastUpdate", 0);
    data->m_inhabitedTime = chunkData.getLongOr("InhabitedTime", 0);

    // Parse status
    // Reference: SerializableChunkData.java lines 102-105
    std::string statusName = chunkData.getStringOr("Status", "empty");
    // Remove "minecraft:" prefix if present
    if (statusName.find("minecraft:") == 0) {
        statusName = statusName.substr(10);
    }
    data->m_chunkStatus = world::chunk::status::ChunkStatus::byName(statusName);
    if (!data->m_chunkStatus) {
        data->m_chunkStatus = &world::chunk::status::ChunkStatus::EMPTY;
    }

    data->m_minSectionY = minY >> 4;
    data->m_lightCorrect = chunkData.getBooleanOr("isLightOn", false);

    // Parse sections
    // Reference: SerializableChunkData.java lines 107-115
    nbt::ListTag* sections = chunkData.getListPtr("sections");
    if (sections) {
        parseSections(*data, *sections, minY, height);
    }

    // Parse heightmaps
    // Reference: SerializableChunkData.java lines 117-120
    nbt::CompoundTag* heightmaps = chunkData.getCompoundPtr("Heightmaps");
    if (heightmaps) {
        parseHeightmaps(*data, *heightmaps);
    }

    // Parse ticks
    // Reference: SerializableChunkData.java lines 122-125
    parseTicks(*data, chunkData);

    // Parse entities
    // Reference: SerializableChunkData.java lines 127-135
    nbt::ListTag* entities = chunkData.getListPtr("entities");
    if (entities) {
        for (size_t i = 0; i < entities->size(); ++i) {
            nbt::CompoundTag* entity = entities->getCompound(i);
            if (entity) {
                data->m_entities.push_back(
                    std::unique_ptr<nbt::CompoundTag>(
                        static_cast<nbt::CompoundTag*>(entity->copy().release())));
            }
        }
    }

    // Parse block entities
    // Reference: SerializableChunkData.java lines 137-145
    nbt::ListTag* blockEntities = chunkData.getListPtr("block_entities");
    if (blockEntities) {
        for (size_t i = 0; i < blockEntities->size(); ++i) {
            nbt::CompoundTag* blockEntity = blockEntities->getCompound(i);
            if (blockEntity) {
                data->m_blockEntities.push_back(
                    std::unique_ptr<nbt::CompoundTag>(
                        static_cast<nbt::CompoundTag*>(blockEntity->copy().release())));
            }
        }
    }

    // Parse structures
    // Reference: SerializableChunkData.java lines 147-150
    nbt::CompoundTag* structures = chunkData.getCompoundPtr("structures");
    if (structures) {
        data->m_structureData = std::unique_ptr<nbt::CompoundTag>(
            static_cast<nbt::CompoundTag*>(structures->copy().release()));
    }

    // Parse upgrade data
    // Reference: SerializableChunkData.java lines 152-155
    nbt::CompoundTag* upgradeData = chunkData.getCompoundPtr("UpgradeData");
    if (upgradeData) {
        data->m_upgradeData = std::unique_ptr<nbt::CompoundTag>(
            static_cast<nbt::CompoundTag*>(upgradeData->copy().release()));
    }

    // Parse blending data
    // Reference: SerializableChunkData.java lines 157-160
    nbt::CompoundTag* blendingData = chunkData.getCompoundPtr("blending_data");
    if (blendingData) {
        data->m_blendingData = std::unique_ptr<nbt::CompoundTag>(
            static_cast<nbt::CompoundTag*>(blendingData->copy().release()));
    }

    return data;
}

void SerializableChunkData::parseSections(
    SerializableChunkData& data,
    nbt::ListTag& sections,
    int minY,
    int height)
{
    // Reference: SerializableChunkData.java lines 408-450
    for (size_t i = 0; i < sections.size(); ++i) {
        nbt::CompoundTag* sectionTag = sections.getCompound(i);
        if (!sectionTag) continue;

        SectionData section;
        section.y = sectionTag->getByteOr("Y", static_cast<int8_t>(i));

        // Block states
        nbt::CompoundTag* blockStates = sectionTag->getCompoundPtr("block_states");
        if (blockStates) {
            section.blockStates = std::unique_ptr<nbt::CompoundTag>(
                static_cast<nbt::CompoundTag*>(blockStates->copy().release()));
        }

        // Biomes
        nbt::CompoundTag* biomes = sectionTag->getCompoundPtr("biomes");
        if (biomes) {
            section.biomes = std::unique_ptr<nbt::CompoundTag>(
                static_cast<nbt::CompoundTag*>(biomes->copy().release()));
        }

        // Block light
        auto blockLight = sectionTag->getByteArray("BlockLight");
        if (!blockLight.empty()) {
            auto lightTag = std::make_unique<nbt::CompoundTag>();
            lightTag->putByteArray("data", blockLight);
            section.blockLight = std::move(lightTag);
        }

        // Sky light
        auto skyLight = sectionTag->getByteArray("SkyLight");
        if (!skyLight.empty()) {
            auto lightTag = std::make_unique<nbt::CompoundTag>();
            lightTag->putByteArray("data", skyLight);
            section.skyLight = std::move(lightTag);
        }

        data.m_sectionData.push_back(std::move(section));
    }
}

void SerializableChunkData::parseHeightmaps(
    SerializableChunkData& data,
    nbt::CompoundTag& heightmaps)
{
    // Reference: SerializableChunkData.java lines 452-470
    static const std::map<std::string, levelgen::Heightmap::Types> heightmapNames = {
        {"WORLD_SURFACE_WG", levelgen::Heightmap::Types::WORLD_SURFACE_WG},
        {"WORLD_SURFACE", levelgen::Heightmap::Types::WORLD_SURFACE},
        {"OCEAN_FLOOR_WG", levelgen::Heightmap::Types::OCEAN_FLOOR_WG},
        {"OCEAN_FLOOR", levelgen::Heightmap::Types::OCEAN_FLOOR},
        {"MOTION_BLOCKING", levelgen::Heightmap::Types::MOTION_BLOCKING},
        {"MOTION_BLOCKING_NO_LEAVES", levelgen::Heightmap::Types::MOTION_BLOCKING_NO_LEAVES}
    };

    for (const auto& pair : heightmapNames) {
        auto heightmapData = heightmaps.getLongArray(pair.first);
        if (!heightmapData.empty()) {
            data.m_heightmaps.heightmaps[pair.second] = heightmapData;
        }
    }
}

void SerializableChunkData::parseTicks(
    SerializableChunkData& data,
    nbt::CompoundTag& chunkData)
{
    // Reference: SerializableChunkData.java lines 472-490
    nbt::ListTag* blockTicks = chunkData.getListPtr("block_ticks");
    if (blockTicks) {
        auto copy = blockTicks->copy();
        data.m_packedTicks.blockTicks = std::move(*static_cast<nbt::ListTag*>(copy.release()));
    }

    nbt::ListTag* fluidTicks = chunkData.getListPtr("fluid_ticks");
    if (fluidTicks) {
        auto copy = fluidTicks->copy();
        data.m_packedTicks.fluidTicks = std::move(*static_cast<nbt::ListTag*>(copy.release()));
    }
}

// =========================================================================
// Writing to NBT
// Reference: SerializableChunkData.java write() lines 332-396
// =========================================================================

std::unique_ptr<nbt::CompoundTag> SerializableChunkData::write() const {
    auto tag = std::make_unique<nbt::CompoundTag>();

    // Write basic info
    tag->putInt("DataVersion", ChunkSerializer::DATA_VERSION);
    tag->putInt("xPos", m_chunkPos.x());
    tag->putInt("zPos", m_chunkPos.z());
    tag->putLong("LastUpdate", m_lastUpdateTime);
    tag->putLong("InhabitedTime", m_inhabitedTime);
    tag->putString("Status", "minecraft:" + m_chunkStatus->getName());
    tag->putBoolean("isLightOn", m_lightCorrect);

    // Write sections
    writeSections(*tag);

    // Write heightmaps
    writeHeightmaps(*tag);

    // Write ticks
    writeTicks(*tag);

    // Write entities
    if (!m_entities.empty()) {
        nbt::ListTag entitiesList;
        for (const auto& entity : m_entities) {
            entitiesList.add(entity->copy());
        }
        tag->put("entities", std::make_unique<nbt::ListTag>(std::move(entitiesList)));
    }

    // Write block entities
    if (!m_blockEntities.empty()) {
        nbt::ListTag blockEntitiesList;
        for (const auto& blockEntity : m_blockEntities) {
            blockEntitiesList.add(blockEntity->copy());
        }
        tag->put("block_entities", std::make_unique<nbt::ListTag>(std::move(blockEntitiesList)));
    }

    // Write structures
    if (m_structureData) {
        tag->put("structures", m_structureData->copy());
    }

    // Write upgrade data
    if (m_upgradeData) {
        tag->put("UpgradeData", m_upgradeData->copy());
    }

    // Write blending data
    if (m_blendingData) {
        tag->put("blending_data", m_blendingData->copy());
    }

    return tag;
}

void SerializableChunkData::writeSections(nbt::CompoundTag& tag) const {
    nbt::ListTag sectionsList;

    for (const auto& section : m_sectionData) {
        auto sectionTag = std::make_unique<nbt::CompoundTag>();
        sectionTag->putByte("Y", section.y);

        if (section.blockStates) {
            sectionTag->put("block_states", section.blockStates->copy());
        }

        if (section.biomes) {
            sectionTag->put("biomes", section.biomes->copy());
        }

        if (section.blockLight) {
            auto lightData = section.blockLight->getByteArray("data");
            if (!lightData.empty()) {
                sectionTag->putByteArray("BlockLight", lightData);
            }
        }

        if (section.skyLight) {
            auto lightData = section.skyLight->getByteArray("data");
            if (!lightData.empty()) {
                sectionTag->putByteArray("SkyLight", lightData);
            }
        }

        sectionsList.add(std::move(sectionTag));
    }

    tag.put("sections", std::make_unique<nbt::ListTag>(std::move(sectionsList)));
}

void SerializableChunkData::writeHeightmaps(nbt::CompoundTag& tag) const {
    auto heightmapsTag = std::make_unique<nbt::CompoundTag>();

    static const std::map<levelgen::Heightmap::Types, std::string> heightmapNames = {
        {levelgen::Heightmap::Types::WORLD_SURFACE_WG, "WORLD_SURFACE_WG"},
        {levelgen::Heightmap::Types::WORLD_SURFACE, "WORLD_SURFACE"},
        {levelgen::Heightmap::Types::OCEAN_FLOOR_WG, "OCEAN_FLOOR_WG"},
        {levelgen::Heightmap::Types::OCEAN_FLOOR, "OCEAN_FLOOR"},
        {levelgen::Heightmap::Types::MOTION_BLOCKING, "MOTION_BLOCKING"},
        {levelgen::Heightmap::Types::MOTION_BLOCKING_NO_LEAVES, "MOTION_BLOCKING_NO_LEAVES"}
    };

    for (const auto& pair : m_heightmaps.heightmaps) {
        auto it = heightmapNames.find(pair.first);
        if (it != heightmapNames.end()) {
            heightmapsTag->putLongArray(it->second, pair.second);
        }
    }

    tag.put("Heightmaps", std::move(heightmapsTag));
}

void SerializableChunkData::writeTicks(nbt::CompoundTag& tag) const {
    if (!m_packedTicks.blockTicks.isEmpty()) {
        tag.put("block_ticks", m_packedTicks.blockTicks.copy());
    }
    if (!m_packedTicks.fluidTicks.isEmpty()) {
        tag.put("fluid_ticks", m_packedTicks.fluidTicks.copy());
    }
}

// =========================================================================
// Get chunk status from tag (without full parsing)
// Reference: SerializableChunkData.java getChunkStatusFromTag(CompoundTag)
// =========================================================================

const ChunkStatusPtr* SerializableChunkData::getChunkStatusFromTag(nbt::CompoundTag* tag) {
    if (!tag) return nullptr;

    std::string statusName = tag->getStringOr("Status", "empty");

    // Remove "minecraft:" prefix if present
    if (statusName.find("minecraft:") == 0) {
        statusName = statusName.substr(10);
    }

    return world::chunk::status::ChunkStatus::byName(statusName);
}

// =========================================================================
// Read chunk to ProtoChunk
// Reference: SerializableChunkData.java read() lines 165-268
// =========================================================================

std::unique_ptr<ProtoChunk> SerializableChunkData::read(
    server::level::ServerLevel& level,
    const ChunkPos& /*pos*/) const
{
    // Reference: SerializableChunkData.java read() lines 165-268
    // Extract parameters from ServerLevel and delegate to readDirect()
    return readDirect(
        level.getMinBuildHeight(),
        level.getHeight(),
        level.getAirBlock(),
        level.getDefaultBlock(),
        level.getBlockRegistry()
    );
}

// Direct version that doesn't require ServerLevel
std::unique_ptr<ProtoChunk> SerializableChunkData::readDirect(
    int minY,
    int height,
    IBlockType* airBlock,
    IBlockType* defaultBlock,
    BlockRegistry* registry) const
{
    if (!airBlock || !registry) {
        return nullptr;
    }

    // Create the ProtoChunk
    auto chunk = std::make_unique<ProtoChunk>(
        m_chunkPos,
        minY,
        height,
        airBlock,
        defaultBlock ? defaultBlock : airBlock,
        registry
    );

    // Set status
    chunk->setStatus(m_chunkStatus);

    // Restore inhabitedTime
    // Reference: SerializableChunkData.java read() restores inhabitedTime
    chunk->setInhabitedTime(m_inhabitedTime);

    // Note: lightCorrect is stored in SerializableChunkData but ProtoChunk
    // doesn't have a setter for it (it's computed during light updates)

    // Populate sections from m_sectionData
    for (const auto& sectionData : m_sectionData) {
        // Calculate section index from y value
        int sectionY = sectionData.y;
        int sectionIndex = sectionY - (minY >> 4);

        if (sectionIndex < 0 || sectionIndex >= chunk->getSectionsCount()) {
            continue;
        }

        LevelChunkSection& section = chunk->getSection(sectionIndex);

        // Deserialize block states from paletted container NBT
        if (sectionData.blockStates) {
            nbt::ListTag* palette = sectionData.blockStates->getListPtr("palette");
            std::vector<int64_t> data = sectionData.blockStates->getLongArray("data");

            if (palette && palette->size() > 0) {
                // Build palette entries from NBT
                std::vector<IBlockType*> paletteEntries;
                for (size_t i = 0; i < palette->size(); ++i) {
                    nbt::CompoundTag* blockTag = palette->getCompound(i);
                    if (blockTag) {
                        std::string name = blockTag->getStringOr("Name", "minecraft:air");

                        // Read Properties if present
                        // Reference: StateHolder.java PROPERTIES_TAG = "Properties"
                        nbt::CompoundTag* propsTag = blockTag->getCompoundPtr("Properties");
                        std::unordered_map<std::string, std::string> properties;
                        if (propsTag) {
                            for (const auto& [propName, propTag] : *propsTag) {
                                if (propTag->getId() == nbt::TagType::TAG_STRING) {
                                    properties[propName] = static_cast<nbt::StringTag*>(propTag.get())->getValue();
                                }
                            }
                        }

                        // Look up block state by name and properties in registry
                        // Reference: Java uses BuiltInRegistries.BLOCK.byNameCodec() + StateHolder codec
                        IBlockType* block = nullptr;
                        if (!properties.empty()) {
                            // Look up specific BlockState variant with properties
                            block = registry->getBlockState(name, properties);
                        } else {
                            // No properties - use default block state
                            block = registry->getDefaultBlockState(name);
                        }
                        if (!block) {
                            block = airBlock;  // Fallback for unknown blocks
                        }
                        paletteEntries.push_back(block);
                    }
                }

                if (!paletteEntries.empty()) {
                    section.getStates().deserialize(paletteEntries, data);
                }
            }
        }

        // Deserialize biomes
        // Reference: Java uses biomes.holderByNameCodec() to resolve biome names
        if (sectionData.biomes) {
            nbt::ListTag* biomePalette = sectionData.biomes->getListPtr("palette");
            std::vector<int64_t> biomeData = sectionData.biomes->getLongArray("data");

            if (biomePalette && biomePalette->size() > 0) {
                // Build biome palette
                std::vector<biome::BiomeKey> biomePaletteEntries;
                for (size_t i = 0; i < biomePalette->size(); ++i) {
                    std::string biomeName = biomePalette->getString(i);
                    biomePaletteEntries.push_back(biomeName);
                }

                // Deserialize biome data (4x4x4 = 64 entries per section)
                // Biomes use same packed format as blocks but with 64 entries
                int bitsPerEntry = biomePaletteEntries.size() > 1 ?
                    ceillog2(static_cast<int32_t>(biomePaletteEntries.size())) : 0;

                if (bitsPerEntry == 0) {
                    // Single biome for entire section
                    biome::BiomeKey key = biomePaletteEntries[0];
                    for (int i = 0; i < LevelChunkSection::BIOMES_PER_SECTION; ++i) {
                        section.getBiomes()[i] = key;
                    }
                } else {
                    // Unpack biome data
                    util::SimpleBitStorage storage(bitsPerEntry, 64, biomeData);
                    for (int i = 0; i < 64; ++i) {
                        int paletteIndex = storage.get(i);
                        if (paletteIndex < static_cast<int>(biomePaletteEntries.size())) {
                            section.getBiomes()[i] = biomePaletteEntries[paletteIndex];
                        }
                    }
                }
            }
        }
    }

    // Populate heightmaps from m_heightmaps
    for (const auto& [type, data] : m_heightmaps.heightmaps) {
        auto& heightmap = chunk->getOrCreateHeightmap(type);
        heightmap.setRawData(data);
    }

    return chunk;
}

// =========================================================================
// Copy from chunk
// Reference: SerializableChunkData.java copyOf() lines 274-329
// =========================================================================

std::unique_ptr<SerializableChunkData> SerializableChunkData::copyOf(
    server::level::ServerLevel& level,
    ChunkAccess& chunk)
{
    // Reference: SerializableChunkData.java copyOf() lines 274-329
    // Try to cast to ProtoChunk for full serialization support
    auto* protoChunk = dynamic_cast<ProtoChunk*>(&chunk);
    if (protoChunk) {
        auto data = copyOfDirect(*protoChunk);
        if (data) {
            // Add timestamp from ServerLevel
            data->m_lastUpdateTime = level.getGameTime();
        }
        return data;
    }

    // For non-ProtoChunk types, create minimal serialization
    // This handles cases like LevelChunk (live world chunks)
    auto data = std::unique_ptr<SerializableChunkData>(new SerializableChunkData());
    data->m_chunkPos = chunk.getPos();
    data->m_lastUpdateTime = level.getGameTime();
    data->m_inhabitedTime = 0;  // Would need chunk-specific tracking
    data->m_lightCorrect = false;
    data->m_minSectionY = chunk.getMinBuildHeight() >> 4;

    // Note: Full section serialization for non-ProtoChunk requires additional implementation
    // For now, this returns a minimal stub for LevelChunk types

    return data;
}

// Direct version that doesn't require ServerLevel
std::unique_ptr<SerializableChunkData> SerializableChunkData::copyOfDirect(
    ProtoChunk& chunk)
{
    auto data = std::unique_ptr<SerializableChunkData>(new SerializableChunkData());

    data->m_chunkPos = chunk.getPos();
    data->m_chunkStatus = chunk.getPersistedStatus();
    data->m_lastUpdateTime = 0;  // Set by copyOf() when ServerLevel is available
    data->m_inhabitedTime = chunk.getInhabitedTime();  // Reference: ChunkAccess.getInhabitedTime()
    data->m_lightCorrect = chunk.isLightCorrect();  // Reference: ChunkAccess.isLightCorrect()
    data->m_minSectionY = chunk.getMinBuildHeight() >> 4;

    // Serialize sections
    for (int i = 0; i < chunk.getSectionsCount(); ++i) {
        const LevelChunkSection& section = chunk.getSection(i);

        SectionData sectionData;
        sectionData.y = static_cast<int8_t>(i + data->m_minSectionY);

        // Serialize block states
        auto blockStatesTag = std::make_unique<nbt::CompoundTag>();
        {
            // Get palette entries and data
            auto entries = section.getStates().getPaletteEntries();
            auto rawData = section.getStates().getRawData();

            // Build palette NBT
            // Reference: StateHolder.java NAME_TAG = "Name", PROPERTIES_TAG = "Properties"
            nbt::ListTag palette;
            for (IBlockType* block : entries) {
                auto blockTag = std::make_unique<nbt::CompoundTag>();
                std::string name = block ? block->getIdentifier() : "minecraft:air";
                blockTag->putString("Name", name);

                // Serialize properties if present
                // Reference: StateHolder.java codec() dispatches on "Name", then reads "Properties"
                if (block && block->hasProperties()) {
                    auto propsTag = std::make_unique<nbt::CompoundTag>();
                    for (const auto& [propName, propValue] : block->getProperties()) {
                        propsTag->putString(propName, propValue);
                    }
                    blockTag->put("Properties", std::move(propsTag));
                }

                palette.add(std::move(blockTag));
            }

            blockStatesTag->put("palette", std::make_unique<nbt::ListTag>(std::move(palette)));
            if (!rawData.empty()) {
                blockStatesTag->putLongArray("data", rawData);
            }
        }
        sectionData.blockStates = std::move(blockStatesTag);

        // Serialize biomes
        // Reference: Java iterates section biomes and builds palette
        auto biomesTag = std::make_unique<nbt::CompoundTag>();
        {
            const auto& biomes = section.getBiomes();

            // Build unique palette of biomes
            std::vector<biome::BiomeKey> biomePaletteEntries;
            std::unordered_map<biome::BiomeKey, int> biomeToIndex;

            for (const auto& biomeKey : biomes) {
                if (biomeToIndex.find(biomeKey) == biomeToIndex.end()) {
                    biomeToIndex[biomeKey] = static_cast<int>(biomePaletteEntries.size());
                    biomePaletteEntries.push_back(biomeKey);
                }
            }

            // Build palette NBT (list of strings)
            nbt::ListTag biomePalette;
            for (const auto& key : biomePaletteEntries) {
                biomePalette.add(std::make_unique<nbt::StringTag>(key));
            }
            biomesTag->put("palette", std::make_unique<nbt::ListTag>(std::move(biomePalette)));

            // Pack biome indices if more than 1 biome
            if (biomePaletteEntries.size() > 1) {
                int bitsPerEntry = ceillog2(static_cast<int32_t>(biomePaletteEntries.size()));
                util::SimpleBitStorage storage(bitsPerEntry, 64);

                for (size_t j = 0; j < biomes.size(); ++j) {
                    storage.set(static_cast<int32_t>(j), biomeToIndex[biomes[j]]);
                }

                biomesTag->putLongArray("data", storage.getRaw());
            }
        }
        sectionData.biomes = std::move(biomesTag);

        data->m_sectionData.push_back(std::move(sectionData));
    }

    // Serialize heightmaps
    for (auto type : {levelgen::Heightmap::Types::WORLD_SURFACE_WG,
                      levelgen::Heightmap::Types::OCEAN_FLOOR_WG,
                      levelgen::Heightmap::Types::MOTION_BLOCKING,
                      levelgen::Heightmap::Types::WORLD_SURFACE}) {
        levelgen::Heightmap* hm = chunk.getHeightmap(type);
        if (hm) {
            data->m_heightmaps.heightmaps[type] = hm->getRawData();
        }
    }

    return data;
}

// =========================================================================
// ChunkSerializer namespace functions
// =========================================================================

namespace ChunkSerializer {

std::unique_ptr<ChunkAccess> read(
    server::level::ServerLevel& level,
    const ChunkPos& pos,
    nbt::CompoundTag& tag)
{
    // Parse the data
    // Default dimensions: minY=-64, height=384 (1.18+ defaults)
    auto data = SerializableChunkData::parse(-64, 384, tag);
    if (!data) return nullptr;

    // Create the chunk
    return data->read(level, pos);
}

std::unique_ptr<nbt::CompoundTag> write(
    server::level::ServerLevel& level,
    ChunkAccess& chunk)
{
    auto data = SerializableChunkData::copyOf(level, chunk);
    if (!data) return nullptr;
    return data->write();
}

} // namespace ChunkSerializer

} // namespace storage
} // namespace chunk
} // namespace level
} // namespace world
} // namespace minecraft
