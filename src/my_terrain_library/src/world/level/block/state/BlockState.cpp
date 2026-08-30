#include "world/level/block/state/BlockState.h"
#include "world/level/block/Block.h"
#include "world/level/block/state/properties/BlockStateProperties.h"
#include <set>

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

bool BlockState::hasBlockEntity() const {
    // Cached per state: the suffix scans + set lookup below ran on EVERY
    // setBlock during decoration (measured 2026-08-30: 31% of setBlock).
    const int8_t cached = m_hasBlockEntityCache.load(std::memory_order_relaxed);
    if (cached >= 0) return cached != 0;
    const bool r = computeHasBlockEntity();
    m_hasBlockEntityCache.store(r ? 1 : 0, std::memory_order_relaxed);
    return r;
}

bool BlockState::computeHasBlockEntity() const {
    // Reference: the Java EntityBlock implementer set (decompiled scan
    // 2026-08-13), restricted to blocks that can exist in generated terrain.
    auto endsWith = [this](const char* suffix) {
        size_t n = std::strlen(suffix);
        return m_identifier.size() > n
            && m_identifier.compare(m_identifier.size() - n, n, suffix) == 0;
    };
    if (endsWith("_bed") || endsWith("_banner") || endsWith("_sign")
        || endsWith("_skull") || endsWith("_head") || endsWith("shulker_box")
        || endsWith("campfire") || endsWith("_hanging_sign")) {
        return m_identifier != "minecraft:piston_head";
    }
    static const std::set<std::string> s_entityBlocks = {
        "minecraft:chest", "minecraft:trapped_chest", "minecraft:ender_chest",
        "minecraft:furnace", "minecraft:blast_furnace", "minecraft:smoker",
        "minecraft:barrel", "minecraft:beacon", "minecraft:beehive",
        "minecraft:bee_nest", "minecraft:bell", "minecraft:brewing_stand",
        "minecraft:suspicious_sand", "minecraft:suspicious_gravel",
        "minecraft:chiseled_bookshelf", "minecraft:comparator",
        "minecraft:conduit", "minecraft:crafter", "minecraft:creaking_heart",
        "minecraft:daylight_detector", "minecraft:decorated_pot",
        "minecraft:dispenser", "minecraft:dropper",
        "minecraft:enchanting_table", "minecraft:hopper", "minecraft:jigsaw",
        "minecraft:jukebox", "minecraft:lectern", "minecraft:moving_piston",
        "minecraft:sculk_catalyst", "minecraft:sculk_sensor",
        "minecraft:calibrated_sculk_sensor", "minecraft:sculk_shrieker",
        "minecraft:spawner", "minecraft:structure_block",
        "minecraft:trial_spawner", "minecraft:vault",
        "minecraft:end_gateway", "minecraft:end_portal",
        "minecraft:command_block", "minecraft:chain_command_block",
        "minecraft:repeating_command_block",
    };
    return s_entityBlocks.count(m_identifier) != 0;
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
        m_identifier == "minecraft:cobweb" ||
        m_identifier == "minecraft:bamboo" ||
        m_identifier == "minecraft:end_portal_frame") {
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

    // Structure-placed partial blocks (B6): none of these have full-cube
    // collision shapes (slab TYPE double is the one exception).
    {
        auto endsWith = [this](const char* suffix) {
            size_t n = std::strlen(suffix);
            return m_identifier.size() > n
                && m_identifier.compare(m_identifier.size() - n, n, suffix) == 0;
        };
        if (endsWith("_slab")) {
            using ST = state::properties::SlabType;
            ST type = getValueOrElse(*BlockStateProperties::SLAB_TYPE, ST(ST::BOTTOM));
            return type.getValue() == ST::DOUBLE;
        }
        if (endsWith("_stairs") || endsWith("_fence") || endsWith("_fence_gate")
            || endsWith("_pane") || endsWith("_wall") || endsWith("_trapdoor")
            || endsWith("_door") || endsWith("_bed") || endsWith("_carpet")
            || endsWith("_anvil")
            || m_identifier == "minecraft:iron_bars"
            || m_identifier == "minecraft:iron_chain"
            || m_identifier == "minecraft:chain"
            || m_identifier == "minecraft:farmland"
            || m_identifier == "minecraft:flower_pot"
            || m_identifier.rfind("minecraft:potted_", 0) == 0
            || m_identifier == "minecraft:dirt_path"
            || m_identifier == "minecraft:campfire"
            || m_identifier == "minecraft:soul_campfire"
            || m_identifier == "minecraft:stonecutter"
            || m_identifier == "minecraft:lectern"
            || m_identifier == "minecraft:lantern"
            || m_identifier == "minecraft:soul_lantern"
            || m_identifier == "minecraft:bell"
            || m_identifier == "minecraft:grindstone"
            || m_identifier == "minecraft:composter"
            || endsWith("candle")
            || endsWith("_skull")
            || endsWith("_head")
            || m_identifier == "minecraft:decorated_pot"
            || m_identifier == "minecraft:repeater"
            || m_identifier == "minecraft:comparator"
            || m_identifier == "minecraft:hopper"
            || m_identifier == "minecraft:piston_head") {
            return false;  // partial collision shapes
        }
    }

    return isSolid();
}

bool BlockState::isFaceSturdy(
    const minecraft::levelgen::WorldGenLevel& level,
    const core::BlockPos& pos,
    core::Direction direction
) const {
    // Per-state, per-direction cache: the answer depends only on this state
    // and the direction, and the identifier compares below were 46% of all
    // string work in decoration (HasSturdyFacePredicate, measured 2026-08-30).
    const int d = static_cast<int>(direction) & 7;
    const uint16_t cached = m_faceSturdyCache.load(std::memory_order_relaxed);
    if (cached & (uint16_t(1) << (8 + d))) return (cached & (uint16_t(1) << d)) != 0;
    const bool r = computeIsFaceSturdy(level, pos, direction);
    uint16_t bits = cached | (uint16_t(1) << (8 + d)) | (r ? (uint16_t(1) << d) : 0);
    m_faceSturdyCache.store(bits, std::memory_order_relaxed);
    return r;
}

bool BlockState::computeIsFaceSturdy(
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
    // Cobwebs likewise have an empty collision shape despite forceSolidOn.
    if (m_identifier == "minecraft:sculk_vein" ||
        m_identifier == "minecraft:cobweb") {
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

    // Structure-placed partial blocks (B6): face fullness of the collision
    // shape, derived from the Java shape tables.
    auto endsWith = [this](const char* suffix) {
        size_t n = std::strlen(suffix);
        return m_identifier.size() > n
            && m_identifier.compare(m_identifier.size() - n, n, suffix) == 0;
    };
    auto ccw = [](core::Direction d) {
        switch (d) {
            case core::Direction::NORTH: return core::Direction::WEST;
            case core::Direction::WEST: return core::Direction::SOUTH;
            case core::Direction::SOUTH: return core::Direction::EAST;
            case core::Direction::EAST: return core::Direction::NORTH;
            default: return d;
        }
    };
    auto cw = [](core::Direction d) {
        switch (d) {
            case core::Direction::NORTH: return core::Direction::EAST;
            case core::Direction::EAST: return core::Direction::SOUTH;
            case core::Direction::SOUTH: return core::Direction::WEST;
            case core::Direction::WEST: return core::Direction::NORTH;
            default: return d;
        }
    };
    if (endsWith("_stairs")) {
        // Reference: StairBlock shapes. Full faces: the half face; the back
        // (facing) except for outer shapes; the inner shapes add one side.
        using SS = state::properties::StairsShape;
        using HF = state::properties::Half;
        core::Direction facing =
            getValueOrElse(*BlockStateProperties::HORIZONTAL_FACING, core::Direction::NORTH);
        HF half = getValueOrElse(*BlockStateProperties::HALF, HF(HF::BOTTOM));
        SS shape = getValueOrElse(*BlockStateProperties::STAIRS_SHAPE, SS(SS::STRAIGHT));
        core::Direction halfFace = half.getValue() == HF::BOTTOM ? core::Direction::DOWN
                                                                 : core::Direction::UP;
        if (direction == halfFace) return true;
        switch (shape.getValue()) {
            case SS::STRAIGHT:
                return direction == facing;
            case SS::INNER_LEFT:
                return direction == facing || direction == ccw(facing);
            case SS::INNER_RIGHT:
                return direction == facing || direction == cw(facing);
            default:  // outer shapes: only the half face is full
                return false;
        }
    }
    if (endsWith("_slab")) {
        // Reference: SlabBlock shapes - bottom: DOWN; top: UP; double: all.
        using ST = state::properties::SlabType;
        ST type = getValueOrElse(*BlockStateProperties::SLAB_TYPE, ST(ST::BOTTOM));
        if (type.getValue() == ST::DOUBLE) return true;
        return direction == (type.getValue() == ST::BOTTOM ? core::Direction::DOWN
                                                           : core::Direction::UP);
    }
    if (endsWith("_fence") || endsWith("_fence_gate") || endsWith("_pane")
        || endsWith("_wall") || m_identifier == "minecraft:iron_bars"
        || m_identifier == "minecraft:iron_chain" || m_identifier == "minecraft:chain") {
        return false;  // thin post/pane shapes have no full faces
    }
    if (endsWith("_trapdoor")) {
        // Reference: TrapDoorBlock AABBs - closed: the half face; open: the
        // panel lies against the face opposite the facing.
        bool open = getValueOrElse(*BlockStateProperties::OPEN, false);
        if (!open) {
            using HF = state::properties::Half;
            HF half = getValueOrElse(*BlockStateProperties::HALF, HF(HF::BOTTOM));
            return direction == (half.getValue() == HF::BOTTOM ? core::Direction::DOWN
                                                               : core::Direction::UP);
        }
        core::Direction facing =
            getValueOrElse(*BlockStateProperties::HORIZONTAL_FACING, core::Direction::NORTH);
        return direction == core::getOpposite(facing);
    }
    if (endsWith("_door")) {
        // Reference: DoorBlock AABBs - closed panel against facing.opposite;
        // open panel swings to the hinge side.
        using DH = state::properties::DoorHingeSide;
        core::Direction facing =
            getValueOrElse(*BlockStateProperties::HORIZONTAL_FACING, core::Direction::NORTH);
        bool open = getValueOrElse(*BlockStateProperties::OPEN, false);
        if (!open) return direction == core::getOpposite(facing);
        DH hinge = getValueOrElse(*BlockStateProperties::DOOR_HINGE, DH(DH::LEFT));
        return direction == (hinge.getValue() == DH::LEFT ? ccw(facing) : cw(facing));
    }
    if (endsWith("_bed")) {
        return false;  // 3..9 base + legs: no full faces
    }
    if (endsWith("_carpet")) {
        return direction == core::Direction::DOWN;  // 16x1x16
    }
    if (m_identifier == "minecraft:end_portal_frame") {
        return direction == core::Direction::DOWN;  // 16x13x16 base (+ eye)
    }
    if (m_identifier == "minecraft:farmland") {
        return direction == core::Direction::DOWN;  // 16x15x16 slab
    }
    if (endsWith("_anvil")) {
        return false;  // narrow base + top - no full faces
    }
    if (m_identifier.rfind("minecraft:potted_", 0) == 0
        || m_identifier == "minecraft:flower_pot") {
        return false;  // FlowerPotBlock: 6x6x6 pot - never sturdy
    }
    if (m_identifier == "minecraft:dirt_path"
        || m_identifier == "minecraft:campfire"
        || m_identifier == "minecraft:soul_campfire"
        || m_identifier == "minecraft:stonecutter"
        || m_identifier == "minecraft:lectern") {
        return direction == core::Direction::DOWN;  // flat-bottomed partials
    }
    if (m_identifier == "minecraft:lantern" || m_identifier == "minecraft:soul_lantern"
        || m_identifier == "minecraft:bell" || m_identifier == "minecraft:grindstone") {
        return false;  // small centered shapes
    }
    if (m_identifier == "minecraft:composter") {
        return direction != core::Direction::UP;  // hollow bowl at the top
    }
    // Batch-2 structure decor (ancient city / trial chambers) partial shapes.
    if (endsWith("candle")) {
        return false;  // CandleBlock: tiny centered shapes - never sturdy
    }
    if (m_identifier == "minecraft:decorated_pot") {
        return false;  // 14x16x14 box - no full faces
    }
    if (endsWith("_skull")
        || (endsWith("_head") && m_identifier != "minecraft:piston_head")) {
        return false;  // skulls/heads: small boxes
    }
    if (m_identifier == "minecraft:repeater" || m_identifier == "minecraft:comparator") {
        return direction == core::Direction::DOWN;  // DiodeBlock 16x2x16 slab
    }
    if (m_identifier == "minecraft:hopper") {
        return false;  // bowl with open top and narrow tube - no full faces
    }
    if (m_identifier == "minecraft:piston_head") {
        // 16x16x4 plate against the facing end + arm.
        core::Direction facing =
            getValueOrElse(*BlockStateProperties::FACING, core::Direction::NORTH);
        return direction == facing;
    }

    // Support checks are collision-based in Java, not render-based.
    return isSolid();
}

} // namespace state
} // namespace block
} // namespace level
} // namespace world
} // namespace minecraft
