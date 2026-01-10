#include "server/level/WorldGenRegion.h"
#include "core/BlockPos.h"
#include "world/level/block/state/BlockState.h"
#include "world/chunk/status/ChunkDependencies.h"
#include <stdexcept>
#include <locale>
#include <sstream>

// Reference: net/minecraft/server/level/WorldGenRegion.java

namespace minecraft {
namespace server {
namespace level {

using namespace world::chunk::status;

// Forward declare ServerLevel stub (would be full implementation)
class ServerLevel {
public:
    int64_t getSeed() const { return m_seed; }
    int getMinY() const { return -64; }  // Default Minecraft 1.18+ value
    int getHeight() const { return 384; }  // Default Minecraft 1.18+ value
    void setSeed(int64_t seed) { m_seed = seed; }
private:
    int64_t m_seed = 0;
};

// Reference: WorldGenRegion.java lines 84-94
WorldGenRegion::WorldGenRegion(
    ServerLevel& level,
    util::StaticCache2D<GenerationChunkHolder*>& cache,
    const ChunkStep& generatingStep,
    ChunkAccess& center)
    : m_level(level)
    , m_cache(cache)
    , m_center(center)
    , m_generatingStep(generatingStep)
    , m_seed(level.getSeed())
    , m_currentlyGenerating(nullptr)
{
}

// Reference: WorldGenRegion.java lines 100-102
world::ChunkPos WorldGenRegion::getCenter() const {
    return m_center.getPos();
}

// Reference: WorldGenRegion.java lines 104-106
void WorldGenRegion::setCurrentlyGenerating(std::function<std::string()> currentlyGenerating) {
    m_currentlyGenerating = std::move(currentlyGenerating);
}

// Reference: WorldGenRegion.java lines 108-110
WorldGenRegion::ChunkAccess* WorldGenRegion::getChunk(int chunkX, int chunkZ) {
    return getChunk(chunkX, chunkZ, ChunkStatus::EMPTY, false);
}

// Reference: WorldGenRegion.java lines 112-144
WorldGenRegion::ChunkAccess* WorldGenRegion::getChunk(
    int chunkX,
    int chunkZ,
    const ChunkStatus& targetStatus,
    bool /*loadOrGenerate*/)
{
    // Calculate distance from center
    int distance = m_center.getPos().getChessboardDistance(chunkX, chunkZ);

    // Get the maximum allowed status at this distance
    const ChunkDependencies& deps = m_generatingStep.directDependencies();
    const ChunkStatus* maxAllowedStatus = nullptr;

    if (distance < deps.size()) {
        maxAllowedStatus = &deps.get(distance);
    }

    GenerationChunkHolder* chunkHolder = nullptr;
    if (maxAllowedStatus != nullptr) {
        chunkHolder = m_cache.get(chunkX, chunkZ);
        if (targetStatus.isOrBefore(*maxAllowedStatus)) {
            ChunkAccess* chunk = chunkHolder->getChunkIfPresentUnchecked(*maxAllowedStatus);
            if (chunk != nullptr) {
                return chunk;
            }
        }
    }

    // Chunk not available - create error report
    std::ostringstream oss;
    oss << "Requested chunk unavailable during world generation. ";
    oss << "Requested chunk: (" << chunkX << ", " << chunkZ << "), ";
    oss << "Generating status: " << m_generatingStep.targetStatus().getName() << ", ";
    oss << "Requested status: " << targetStatus.getName() << ", ";

    if (chunkHolder == nullptr) {
        oss << "Actual status: [out of cache bounds], ";
    } else {
        const ChunkStatus* persisted = chunkHolder->getPersistedStatus();
        oss << "Actual status: " << (persisted ? persisted->getName() : "null") << ", ";
    }

    if (maxAllowedStatus == nullptr) {
        oss << "Maximum allowed status: null, ";
    } else {
        oss << "Maximum allowed status: " << maxAllowedStatus->getName() << ", ";
    }

    oss << "Requested distance: " << distance << ", ";
    oss << "Generating chunk: " << m_center.getPos().toString();

    throw std::runtime_error(oss.str());
}

// Reference: WorldGenRegion.java lines 146-149
bool WorldGenRegion::hasChunk(int chunkX, int chunkZ) const {
    int distance = m_center.getPos().getChessboardDistance(chunkX, chunkZ);
    return distance < m_generatingStep.directDependencies().size();
}

// Reference: WorldGenRegion.java lines 230-248
bool WorldGenRegion::ensureCanWrite(const core::BlockPos& pos) const {
    int chunkX = pos.getX() >> 4;  // SectionPos.blockToSectionCoord
    int chunkZ = pos.getZ() >> 4;

    world::ChunkPos centerPos = getCenter();
    int distanceX = std::abs(centerPos.x() - chunkX);
    int distanceZ = std::abs(centerPos.z() - chunkZ);

    int writeRadius = m_generatingStep.blockStateWriteRadius();
    if (distanceX <= writeRadius && distanceZ <= writeRadius) {
        // TODO: Check if center chunk is upgrading and if pos is outside build height
        return true;
    } else {
        // Log warning about writing to far chunk
        std::ostringstream oss;
        oss << "Detected setBlock in a far chunk [" << chunkX << ", " << chunkZ << "], ";
        oss << "pos: " << pos.getX() << "," << pos.getY() << "," << pos.getZ() << ", ";
        oss << "status: " << m_generatingStep.targetStatus().getName();
        if (m_currentlyGenerating) {
            oss << ", currently generating: " << m_currentlyGenerating();
        }
        // Would call Util.logAndPauseIfInIde in Java
        return false;
    }
}

// Reference: WorldGenRegion.java lines 251-287
bool WorldGenRegion::setBlock(
    const core::BlockPos& pos,
    BlockState* blockState,
    int updateFlags,
    int /*updateLimit*/)
{
    if (!ensureCanWrite(pos)) {
        return false;
    }

    ChunkAccess* chunk = getChunk(pos);
    // IChunk::setBlockState takes IBlockType*, BlockState* is compatible
    ::world::IBlockType* oldState = chunk->setBlockState(pos, blockState, (updateFlags & 64) != 0);

    if (oldState != nullptr) {
        // In full implementation: level.updatePOIOnBlockStateChange(pos, oldState, blockState);
    }

    // Handle block entities
    // Reference: WorldGenRegion.java lines 261-279
    // In full implementation: handle EntityBlock, block entity creation/removal

    // Check for post-processing
    // Reference: WorldGenRegion.java lines 281-283
    // In full implementation: if (blockState.hasPostProcess(this, pos) && (updateFlags & 16) == 0)

    return true;
}

// Reference: WorldGenRegion.java lines 151-153
BlockState* WorldGenRegion::getBlockState(const core::BlockPos& pos) const {
    int chunkX = pos.getX() >> 4;  // SectionPos.blockToSectionCoord
    int chunkZ = pos.getZ() >> 4;

    // Get chunk and delegate to it
    // Note: Using const_cast here because getChunk modifies cache access
    WorldGenRegion* self = const_cast<WorldGenRegion*>(this);
    ChunkAccess* chunk = self->getChunk(chunkX, chunkZ);
    // IChunk returns IBlockType*, cast to BlockState*
    return static_cast<BlockState*>(chunk->getBlockState(pos));
}

// Reference: WorldGenRegion.java lines 346-348
int64_t WorldGenRegion::getSeed() const {
    return m_seed;
}

// Reference: WorldGenRegion.java lines 406-408
int WorldGenRegion::getMinY() const {
    return m_level.getMinY();
}

// Reference: WorldGenRegion.java lines 410-412
int WorldGenRegion::getHeight() const {
    return m_level.getHeight();
}

// Reference: WorldGenRegion.java lines 313-316
ServerLevel& WorldGenRegion::getLevel() {
    return m_level;
}

const ServerLevel& WorldGenRegion::getLevel() const {
    return m_level;
}

const ChunkStep& WorldGenRegion::getGeneratingStep() const {
    return m_generatingStep;
}

util::StaticCache2D<GenerationChunkHolder*>& WorldGenRegion::getCache() {
    return m_cache;
}

const util::StaticCache2D<GenerationChunkHolder*>& WorldGenRegion::getCache() const {
    return m_cache;
}

// Helper: Get chunk from ChunkPos
WorldGenRegion::ChunkAccess* WorldGenRegion::getChunk(const world::ChunkPos& pos) {
    return getChunk(pos.x(), pos.z());
}

// Helper: Get chunk from BlockPos
WorldGenRegion::ChunkAccess* WorldGenRegion::getChunk(const core::BlockPos& pos) {
    return getChunk(pos.getX() >> 4, pos.getZ() >> 4);
}

// Helper: Check if position is outside write range
bool WorldGenRegion::isOutsideWriteRange(const core::BlockPos& pos) const {
    return !ensureCanWrite(pos);
}

} // namespace level
} // namespace server
} // namespace minecraft
