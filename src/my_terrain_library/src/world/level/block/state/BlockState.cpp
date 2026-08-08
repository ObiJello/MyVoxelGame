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
    , m_blocksMotionResult(false)
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
    m_blocksMotionResult = m_identifier != "minecraft:cobweb" &&
                           m_identifier != "minecraft:bamboo_sapling" &&
                           isSolid();
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

std::string BlockState::toStateString() const {
    // Reference: StateHolder.java toString() - properties print in
    // StateDefinition's ImmutableSortedMap order (alphabetical by name).
    // m_values is keyed by PropertyBase pointer (nondeterministic order),
    // so collect and sort by property name explicitly.
    if (m_values.empty()) {
        return m_identifier;
    }

    std::vector<std::pair<std::string, std::string>> entries;
    entries.reserve(m_values.size());
    for (const auto& [prop, value] : m_values) {
        entries.emplace_back(prop->getName(), prop->getNameFromAny(value));
    }
    std::sort(entries.begin(), entries.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    std::string result = m_identifier;
    result += '[';
    bool first = true;
    for (const auto& [name, value] : entries) {
        if (!first) result += ',';
        first = false;
        result += name;
        result += '=';
        result += value;
    }
    result += ']';
    return result;
}

bool BlockState::is(const Block* block) const {
    // Reference: BlockState.java is(Block)
    if (!block) return false;
    return m_owner == block;
}

bool BlockState::blocksMotion() const {
    // Reference: BlockBehaviour.BlockStateBase.blocksMotion()
    // Computed once in setCachedValues(); every input is ctor-fixed.
    return m_blocksMotionResult;
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

namespace {
// Blocks whose fluid state is inherently SOURCE water in Java
// (SeagrassBlock/KelpBlock/BubbleColumnBlock getFluidState overrides).
bool hasInherentSourceWater(const std::string& id) {
    return id == "minecraft:seagrass" || id == "minecraft:tall_seagrass" ||
           id == "minecraft:kelp" || id == "minecraft:kelp_plant" ||
           id == "minecraft:bubble_column";
}
}  // namespace

bool BlockState::hasWaterFluid() const {
    // Java FluidState.is(FluidTags.WATER): source OR flowing water, plus
    // waterlogged blocks and inherent-source-water blocks.
    using world::level::block::state::properties::BlockStateProperties;

    if (m_identifier == "minecraft:water" || hasInherentSourceWater(m_identifier)) {
        return true;
    }

    return BlockStateProperties::WATERLOGGED &&
           hasProperty(BlockStateProperties::WATERLOGGED) &&
           getValueOrElse(*BlockStateProperties::WATERLOGGED, false);
}

bool BlockState::hasSourceWaterFluid() const {
    // Java FluidState.is(Fluids.WATER): SOURCE water only — the water block at
    // level 0 (flowing water is FLOWING_WATER and does not match), waterlogged
    // blocks, and inherent-source-water blocks.
    using world::level::block::state::properties::BlockStateProperties;

    if (m_identifier == "minecraft:water") {
        return !BlockStateProperties::LEVEL ||
               !hasProperty(BlockStateProperties::LEVEL) ||
               getValueOrElse(*BlockStateProperties::LEVEL, 0) == 0;
    }
    if (hasInherentSourceWater(m_identifier)) {
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
    if (m_identifier == "minecraft:sculk_vein" ||
        m_identifier == "minecraft:sculk_sensor" ||
        m_identifier == "minecraft:sculk_shrieker" ||
        m_identifier == "minecraft:bamboo") {
        return false;
    }

    // Partial collision shapes: chests are a 14x14 box, dripstone and amethyst
    // buds/clusters are small centered shapes, carpets are 1px slabs, azalea is
    // a top slab over a trunk, snow collision is SHAPES[layers - 1] (14px even
    // at 8 layers) - none of these fill the whole cube.
    if (m_identifier == "minecraft:chest" ||
        m_identifier == "minecraft:trapped_chest" ||
        m_identifier == "minecraft:pointed_dripstone" ||
        m_identifier == "minecraft:small_amethyst_bud" ||
        m_identifier == "minecraft:medium_amethyst_bud" ||
        m_identifier == "minecraft:large_amethyst_bud" ||
        m_identifier == "minecraft:amethyst_cluster" ||
        m_identifier == "minecraft:moss_carpet" ||
        m_identifier == "minecraft:pale_moss_carpet" ||
        m_identifier == "minecraft:azalea" ||
        m_identifier == "minecraft:flowering_azalea" ||
        m_identifier == "minecraft:snow") {
        return false;
    }

    return isSolid();
}

bool BlockState::isFaceSturdy(
    const minecraft::levelgen::WorldGenLevel& /*level*/,
    const core::BlockPos& /*pos*/,
    core::Direction direction
) const {
    // Reference: BlockBehaviour.BlockStateBase.isFaceSturdy()
    // Leaves override getBlockSupportShape() to Shapes.empty() in Java.
    if (m_isLeaves) {
        return false;
    }

    // Sculk veins have noCollission in Java, so getBlockSupportShape() is
    // empty and they are NEVER face-sturdy regardless of their face flags.
    if (m_identifier == "minecraft:sculk_vein") {
        return false;
    }

    // 16x8x16 bottom slabs in Java: the collision shape reaches the full DOWN
    // boundary but stops at y=8, so only the DOWN face is sturdy.
    if (m_identifier == "minecraft:sculk_sensor" ||
        m_identifier == "minecraft:sculk_shrieker") {
        return direction == core::Direction::DOWN;
    }

    // Chests (14x14 box) and pointed dripstone: never face-sturdy.
    if (m_identifier == "minecraft:chest" ||
        m_identifier == "minecraft:trapped_chest" ||
        m_identifier == "minecraft:pointed_dripstone") {
        return false;
    }

    // Bamboo: thin dynamic-shape column - never face-sturdy, never face-full
    // (Java canAttachTo rejects it; vines cannot hang on bamboo).
    if (m_identifier == "minecraft:bamboo") {
        return false;
    }

    // Amethyst buds/clusters: small centered shapes - never face-sturdy.
    if (m_identifier == "minecraft:small_amethyst_bud" ||
        m_identifier == "minecraft:medium_amethyst_bud" ||
        m_identifier == "minecraft:large_amethyst_bud" ||
        m_identifier == "minecraft:amethyst_cluster") {
        return false;
    }

    // Carpets: 16x1x16 slab - only the DOWN face of the shape is full.
    if (m_identifier == "minecraft:moss_carpet" ||
        m_identifier == "minecraft:pale_moss_carpet") {
        return direction == core::Direction::DOWN;
    }

    // Azalea: full 16x16 top slab at y=8..16 over a 4x4 trunk - only the UP
    // face of the support shape is full (AzaleaBlock.SHAPE).
    if (m_identifier == "minecraft:azalea" ||
        m_identifier == "minecraft:flowering_azalea") {
        return direction == core::Direction::UP;
    }

    // Snow layers: support shape is SHAPES[layers] (SnowLayerBlock) - DOWN is
    // always full, UP only when the stack reaches the full block (layers=8).
    if (m_identifier == "minecraft:snow") {
        if (direction == core::Direction::DOWN) {
            return true;
        }
        int layers = 1;
        if (BlockStateProperties::LAYERS && hasProperty(BlockStateProperties::LAYERS)) {
            layers = getValueOrElse(*BlockStateProperties::LAYERS, 1);
        }
        return layers == 8;
    }

    // Support checks are collision-based in Java, not render-based.
    return isSolid();
}

} // namespace state
} // namespace block
} // namespace level
} // namespace world
} // namespace minecraft
