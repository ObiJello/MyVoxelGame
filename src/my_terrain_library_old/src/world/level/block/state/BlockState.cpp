#include "world/level/block/state/BlockState.h"
#include "world/level/block/Block.h"

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
    , m_isLeaves(false)
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
        m_isLeaves = props.isLeaves();
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

bool BlockState::equals(const ::world::IBlockType* other) const {
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
    // Simplified implementation - in full Minecraft this is calculated
    return m_blocksMotion && !m_isAir && !m_liquid;
}

bool BlockState::canOcclude() const {
    // Reference: BlockBehaviour.BlockStateBase.canOcclude()
    // Simplified - solid blocks occlude light
    return isSolid();
}

int BlockState::getLightEmission() const {
    // Reference: BlockBehaviour.BlockStateBase.getLightEmission()
    // Simplified - most blocks emit no light
    // TODO: Implement proper light emission based on block type
    return 0;
}

} // namespace state
} // namespace block
} // namespace level
} // namespace world
} // namespace minecraft
