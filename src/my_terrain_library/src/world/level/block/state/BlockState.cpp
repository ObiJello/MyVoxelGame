#include "world/level/block/state/BlockState.h"
#include "world/level/block/Block.h"
#include "world/level/block/state/properties/BlockStateProperties.h"

namespace minecraft {
namespace world {
namespace level {
namespace block {
namespace state {

BlockState::BlockState(Block* owner, const ValueMap& values)
    : StateHolder<Block, BlockState>(owner, values)
    , m_isAir(false)
    , m_liquid(false)
    , m_blocksMotion(true)
    , m_forceSolidOff(false)
    , m_forceSolidOn(false)
    , m_noOcclusion(false)
    , m_isReplaceable(false)
    , m_isLeaves(false)
    , m_isLog(false)
    , m_isReplaceableByTrees(false)
    , m_identifier()
{
    // Cached values will be set after all states are created
    // via initCache() called by StateDefinition
}

void BlockState::initCache() {
    // Reference: BlockBehaviour.BlockStateBase.initCache() lines 832-856
    setCachedValues();
}

void BlockState::setCachedValues() {
    // Get values from the owning block
    if (m_owner) {
        const Block::Properties& props = m_owner->getProperties();
        m_isAir = props.isAir();
        m_liquid = props.isLiquid();
        m_blocksMotion = props.blocksMotion();
        m_forceSolidOff = props.forceSolidOff();
        m_forceSolidOn = props.forceSolidOn();
        m_noOcclusion = props.noOcclusion();
        m_isReplaceable = props.isReplaceable();
        m_isLeaves = props.isLeaves();
        m_isLog = props.isLog();
        m_isReplaceableByTrees = props.isReplaceableByTrees();
        m_identifier = m_owner->getIdentifier();
    }
}

std::unordered_map<std::string, std::string> BlockState::getProperties() const {
    // Reference: StateHolder.java getValues() - convert to string map for NBT/serialization
    // Reference: StateHolder.java PROPERTY_ENTRY_TO_STRING_FUNCTION lines 21-35
    std::unordered_map<std::string, std::string> result;

    for (const auto& [prop, value] : m_values) {
        // Get the string representation using type-erased conversion
        // Reference: Property.java getName(T value)
        std::string valueName = prop->getNameFromAny(value);
        result[prop->getName()] = valueName;
    }

    return result;
}

bool BlockState::is(const Block* block) const {
    // Reference: BlockState.java is(Block)
    if (!block) return false;
    return m_owner == block;
}

bool BlockState::blocksMotion() const {
    // Reference: BlockBehaviour.BlockStateBase.blocksMotion()
    return m_identifier != "minecraft:cobweb" &&
           m_identifier != "minecraft:bamboo_sapling" &&
           isSolid();
}

bool BlockState::equals(const BlockState* other) const {
    // Check if same identifier
    if (!other || getIdentifier() != other->getIdentifier()) {
        return false;
    }

    // Check if same properties
    auto otherProps = other->getProperties();
    auto myProps = getProperties();

    if (myProps.size() != otherProps.size()) {
        return false;
    }

    for (const auto& [key, value] : myProps) {
        auto it = otherProps.find(key);
        if (it == otherProps.end() || it->second != value) {
            return false;
        }
    }

    return true;
}

bool BlockState::isSolid() const {
    // Reference: BlockBehaviour.BlockStateBase.isSolid() line 875-877
    if (m_isAir || m_liquid) {
        return false;
    }
    if (m_forceSolidOff) {
        return false;
    }
    if (m_forceSolidOn) {
        return true;
    }
    return m_blocksMotion;
}

bool BlockState::isSolidRender() const {
    // Reference: BlockBehaviour.BlockStateBase.isSolidRender()
    return isSolid() && !m_noOcclusion && !m_isLeaves && !m_isReplaceableByTrees;
}

bool BlockState::canOcclude() const {
    // Reference: BlockBehaviour.BlockStateBase.canOcclude()
    return !m_noOcclusion && !m_isAir && !m_liquid;
}

int BlockState::getLightEmission() const {
    // Reference: BlockBehaviour.BlockStateBase.getLightEmission()
    // Simplified - most blocks emit no light
    // TODO: Implement proper light emission based on block type
    return 0;
}

bool BlockState::hasWaterFluid() const {
    using world::level::block::state::properties::BlockStateProperties;

    if (m_identifier == "minecraft:water") {
        return true;
    }

    return BlockStateProperties::WATERLOGGED &&
           hasProperty(BlockStateProperties::WATERLOGGED) &&
           getValueOrElse(*BlockStateProperties::WATERLOGGED, false);
}

bool BlockState::hasAnyFluid() const {
    return m_liquid || hasWaterFluid();
}

bool BlockState::canSurvive(const minecraft::levelgen::WorldGenLevel& level, const core::BlockPos& pos) const {
    return m_owner && m_owner->canSurvive(const_cast<BlockState*>(this), level, pos);
}

bool BlockState::isCollisionShapeFullBlock(
    const minecraft::levelgen::WorldGenLevel& /*level*/,
    const core::BlockPos& /*pos*/
) const {
    // Reference: BlockBehaviour.BlockStateBase.isCollisionShapeFullBlock()
    // We do not model voxel shapes yet, so use the solid/collision proxy rather
    // than render occlusion. This matches Java more closely than isSolidRender().
    return isSolid();
}

bool BlockState::isFaceSturdy(
    const minecraft::levelgen::WorldGenLevel& /*level*/,
    const core::BlockPos& /*pos*/,
    core::Direction /*direction*/
) const {
    // Reference: BlockBehaviour.BlockStateBase.isFaceSturdy()
    // Leaves override getBlockSupportShape() to Shapes.empty() in Java.
    if (m_isLeaves) {
        return false;
    }

    // Support checks are collision-based in Java, not render-based.
    return isSolid();
}

} // namespace state
} // namespace block
} // namespace level
} // namespace world
} // namespace minecraft
