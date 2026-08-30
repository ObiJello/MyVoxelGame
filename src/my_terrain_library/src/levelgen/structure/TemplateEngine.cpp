#include "util/TerrainProfiling.h"
#include "levelgen/structure/TemplateEngine.h"

#include "levelgen/structure/OrientedPieceBehavior.h"
#include "levelgen/WorldGenLevel.h"
#include "levelgen/WorldgenRandom.h"
#include "levelgen/blockpredicates/BlockPredicate.h"
#include "math/Mth.h"
#include "nbt/AllTags.h"
#include "nbt/CanonicalNbt.h"
#include "nbt/NbtIo.h"
#include "random/LegacyRandomSource.h"
#include "core/Direction.h"
#include "world/level/block/Blocks.h"
#include "world/level/block/blocks/FenceBlock.h"
#include "world/level/block/blocks/DoublePlantBlock.h"
#include "world/level/block/state/BlockState.h"
#include "world/level/block/state/properties/BlockStateProperties.h"

#include <algorithm>
#include <cstring>
#include <set>

#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <unordered_map>

// Reference: net/minecraft/world/level/levelgen/structure/templatesystem/
// StructureTemplate.java (loadPalette/placeInWorld) - loader half. The
// placement half lives below; the neighbor shape-update pass is in
// BlockShapeUpdates.cpp.

namespace minecraft {
namespace levelgen {
namespace structure {
namespace TemplateEngine {

namespace {

namespace fs = std::filesystem;
using world::level::block::Blocks;
using world::level::block::Block;

fs::path templateDataRoot() {
    if (const char* env = std::getenv("MC_DATA_ROOT")) return fs::path(env);
    fs::path current = fs::current_path();
    while (!current.empty()) {
        fs::path candidate = current / "data";
        if (fs::exists(candidate) && fs::is_directory(candidate)) return candidate;
        if (current == current.root_path()) break;
        current = current.parent_path();
    }
    throw std::runtime_error("Data root not found for structure templates");
}

std::string normalizeId(const std::string& id) {
    return id.find(':') != std::string::npos ? id : "minecraft:" + id;
}

// Reference: RandomizableContainerBlockEntity subclasses (loot-seed draw).
bool isLootContainerBlock(const std::string& blockId) {
    return blockId == "minecraft:chest" || blockId == "minecraft:trapped_chest"
        || blockId == "minecraft:barrel" || blockId == "minecraft:dispenser"
        || blockId == "minecraft:dropper" || blockId == "minecraft:hopper"
        || blockId == "minecraft:decorated_pot"  // DecoratedPotBlockEntity is
                                                 // a RandomizableContainer
        || blockId.size() > 12
               && blockId.compare(blockId.size() - 12, 12, "_shulker_box") == 0
        || blockId == "minecraft:shulker_box";
}

// Resolve one palette entry (Name + Properties) to a registry BlockState by
// matching the property map against the block's possible states.
BlockState* resolvePaletteEntry(nbt::CompoundTag* entry, const std::string& templateId) {
    std::string name = normalizeId(entry->getStringOr("Name", ""));
    if (name == "minecraft:jigsaw") {
        // Jigsaw blocks are placed then immediately overwritten by their
        // final_state (TemplateStructurePiece jigsaw pass); the orientation
        // property is layout-only, so the plain block state suffices here.
        return Blocks::getDefaultState("minecraft:jigsaw");
    }
    Block* block = Blocks::getBlock(name);
    if (block == nullptr) {
        throw std::runtime_error("Template " + templateId + " uses unregistered block "
                                 + name + " - register it with its full property set");
    }
    nbt::CompoundTag* props = entry->getCompoundPtr("Properties");
    if (props == nullptr) {
        return block->defaultBlockState();
    }
    std::unordered_map<std::string, std::string> want;
    for (const auto& key : props->keys()) {
        want[key] = props->getStringOr(key.c_str(), "");
    }
    for (BlockState* state : block->getStateDefinition().getPossibleStates()) {
        auto have = state->getProperties();
        bool match = true;
        for (const auto& [key, value] : want) {
            auto it = have.find(key);
            if (it == have.end() || it->second != value) {
                match = false;
                break;
            }
        }
        if (match) return state;
    }
    throw std::runtime_error("Template " + templateId + " palette entry " + name
                             + " has properties the registered block lacks");
}

const FullTemplateData& load(const std::string& templateId) {
    static std::mutex s_mutex;
    static std::unordered_map<std::string, FullTemplateData> s_cache;
    std::lock_guard<std::mutex> lock(s_mutex);
    auto it = s_cache.find(templateId);
    if (it != s_cache.end()) return it->second;

    std::string id = normalizeId(templateId);
    size_t colon = id.find(':');
    fs::path file = templateDataRoot() / id.substr(0, colon) / "structure"
                  / (id.substr(colon + 1) + ".nbt");
    if (!fs::exists(file)) {
        // Reference: StructureTemplateManager.getOrCreate - missing template
        // silently becomes empty.
        return s_cache.emplace(templateId, FullTemplateData{}).first->second;
    }
    TERRAIN_ZONE_N("Tmpl.LoadFull");
    TERRAIN_ZONE_TEXT(id.c_str(), id.size());
    auto root = nbt::NbtIo::readCompressedFromFile(file.string());
    if (!root) throw std::runtime_error("Cannot read template: " + file.string());

    FullTemplateData data;
    nbt::ListTag* sizeList = root->getListPtr("size");
    if (sizeList == nullptr || sizeList->size() != 3) {
        throw std::runtime_error("Template missing size: " + id);
    }
    data.sizeX = static_cast<nbt::IntTag*>(sizeList->get(0))->getValue();
    data.sizeY = static_cast<nbt::IntTag*>(sizeList->get(1))->getValue();
    data.sizeZ = static_cast<nbt::IntTag*>(sizeList->get(2))->getValue();

    std::vector<nbt::ListTag*> paletteTags;
    if (nbt::ListTag* single = root->getListPtr("palette")) {
        paletteTags.push_back(single);
    } else if (nbt::ListTag* multi = root->getListPtr("palettes")) {
        for (size_t i = 0; i < multi->size(); ++i) {
            paletteTags.push_back(static_cast<nbt::ListTag*>(multi->get(i)));
        }
    } else {
        throw std::runtime_error("Template missing palette: " + id);
    }
    data.palettes.reserve(paletteTags.size());
    for (nbt::ListTag* palette : paletteTags) {
        std::vector<BlockState*> states;
        states.reserve(palette->size());
        for (size_t i = 0; i < palette->size(); ++i) {
            states.push_back(resolvePaletteEntry(
                static_cast<nbt::CompoundTag*>(palette->get(i)), id));
        }
        data.palettes.push_back(std::move(states));
    }

    if (nbt::ListTag* blocks = root->getListPtr("blocks")) {
        data.blocks.reserve(blocks->size());
        for (size_t bi = 0; bi < blocks->size(); ++bi) {
            auto* blockTag = static_cast<nbt::CompoundTag*>(blocks->get(bi));
            nbt::ListTag* pos = blockTag->getListPtr("pos");
            TemplateBlockInfo info;
            info.x = static_cast<nbt::IntTag*>(pos->get(0))->getValue();
            info.y = static_cast<nbt::IntTag*>(pos->get(1))->getValue();
            info.z = static_cast<nbt::IntTag*>(pos->get(2))->getValue();
            info.stateIdx = blockTag->getIntOr("state", 0);
            nbt::CompoundTag* nbtTag = blockTag->getCompoundPtr("nbt");
            if (nbtTag != nullptr) {
                info.hasNbt = true;
                // B8: retain the full BE nbt for save-format payloads.
                info.nbt = std::shared_ptr<nbt::CompoundTag>(
                    static_cast<nbt::CompoundTag*>(nbtTag->copy().release()));
                std::string blockName = normalizeId(
                    static_cast<nbt::CompoundTag*>(
                        paletteTags.front()->get(static_cast<size_t>(info.stateIdx)))
                        ->getStringOr("Name", ""));
                if (blockName == "minecraft:structure_block") {
                    if (nbtTag->getStringOr("mode", "") == "DATA") {
                        info.isDataMarker = true;
                        info.metadata = nbtTag->getStringOr("metadata", "");
                    }
                } else if (blockName == "minecraft:jigsaw") {
                    info.jigsawFinalState =
                        nbtTag->getStringOr("final_state", "minecraft:air");
                } else if (isLootContainerBlock(blockName)) {
                    info.isLootContainer = true;
                }
            }
            data.blocks.push_back(std::move(info));
        }
    }
    return s_cache.emplace(templateId, std::move(data)).first->second;
}

// ===========================================================================
// Neighbor shape updates (Java BlockState.updateShape per-block virtuals).
// Implemented for the block families template placement can disturb; the
// default is identity. Extend per family as gates demand - a missing rule
// shows up as a byte mismatch, never silently.
// ===========================================================================
namespace shape_updates {

using core::Direction;
using world::level::block::state::properties::BlockStateProperties;
namespace props = world::level::block::state::properties;

int stepY(Direction dir) {
    return dir == Direction::DOWN ? -1 : (dir == Direction::UP ? 1 : 0);
}

core::BlockPos relative(const core::BlockPos& pos, Direction dir) {
    return pos.offset(core::getStepX(dir), stepY(dir), core::getStepZ(dir));
}

bool idEndsWith(const BlockState* state, const char* suffix) {
    const std::string& name = state->getBlock()->getIdentifier();
    size_t n = std::strlen(suffix);
    return name.size() > n && name.compare(name.size() - n, n, suffix) == 0;
}

bool isId(const BlockState* state, const char* id) {
    return state->getBlock()->getIdentifier() == id;
}

// Reference: BushBlock/VegetationBlock.mayPlaceOn for the overworld plants
// near structures (grass/fern/flowers/saplings: #dirt or farmland).
bool isBushLike(const BlockState* state) {
    // Potted plants are FlowerPotBlock - no survival rule (the suffix match
    // must not catch potted_* variants).
    if (state->getBlock()->getIdentifier().rfind("minecraft:potted_", 0) == 0) {
        return false;
    }
    return isId(state, "minecraft:short_grass") || isId(state, "minecraft:fern")
        || isId(state, "minecraft:tall_grass") || isId(state, "minecraft:large_fern")
        || isId(state, "minecraft:dandelion") || isId(state, "minecraft:poppy")
        || idEndsWith(state, "_tulip") || isId(state, "minecraft:oxeye_daisy")
        || isId(state, "minecraft:azure_bluet") || isId(state, "minecraft:cornflower")
        || isId(state, "minecraft:lily_of_the_valley")
        || idEndsWith(state, "_sapling");
}

bool bushMayPlaceOn(BlockState* below) {
    return ::minecraft::levelgen::blockpredicates::matchesBlockTagName(
               below, "minecraft:dirt")
        || isId(below, "minecraft:farmland");
}

// Reference: StairBlock.canTakeShape - neighbor at `dir` is not stairs with
// the same facing and half.
bool stairsCanTakeShape(BlockState* state, WorldGenLevel* level,
                        const core::BlockPos& pos, Direction dir) {
    BlockState* neighbor = level->getBlockState(relative(pos, dir));
    if (!idEndsWith(neighbor, "_stairs")) return true;
    return neighbor->getValue(*BlockStateProperties::HORIZONTAL_FACING)
               != state->getValue(*BlockStateProperties::HORIZONTAL_FACING)
        || neighbor->getValue(*BlockStateProperties::HALF).getValue()
               != state->getValue(*BlockStateProperties::HALF).getValue();
}

Direction counterClockWise(Direction dir) {
    switch (dir) {
        case Direction::NORTH: return Direction::WEST;
        case Direction::WEST: return Direction::SOUTH;
        case Direction::SOUTH: return Direction::EAST;
        case Direction::EAST: return Direction::NORTH;
        default: return dir;
    }
}

// Reference: StairBlock.getStairsShape.
props::StairsShape getStairsShape(BlockState* state, WorldGenLevel* level,
                                  const core::BlockPos& pos) {
    using SS = props::StairsShape;
    Direction facing = state->getValue(*BlockStateProperties::HORIZONTAL_FACING);
    BlockState* behind = level->getBlockState(relative(pos, facing));
    if (idEndsWith(behind, "_stairs")
        && state->getValue(*BlockStateProperties::HALF).getValue()
               == behind->getValue(*BlockStateProperties::HALF).getValue()) {
        Direction behindFacing = behind->getValue(*BlockStateProperties::HORIZONTAL_FACING);
        bool axisDiffers = (behindFacing == Direction::NORTH || behindFacing == Direction::SOUTH)
            != (facing == Direction::NORTH || facing == Direction::SOUTH);
        if (axisDiffers
            && stairsCanTakeShape(state, level, pos, core::getOpposite(behindFacing))) {
            return behindFacing == counterClockWise(facing) ? SS(SS::OUTER_LEFT)
                                                            : SS(SS::OUTER_RIGHT);
        }
    }
    BlockState* front = level->getBlockState(relative(pos, core::getOpposite(facing)));
    if (idEndsWith(front, "_stairs")
        && state->getValue(*BlockStateProperties::HALF).getValue()
               == front->getValue(*BlockStateProperties::HALF).getValue()) {
        Direction frontFacing = front->getValue(*BlockStateProperties::HORIZONTAL_FACING);
        bool axisDiffers = (frontFacing == Direction::NORTH || frontFacing == Direction::SOUTH)
            != (facing == Direction::NORTH || facing == Direction::SOUTH);
        if (axisDiffers && stairsCanTakeShape(state, level, pos, frontFacing)) {
            return frontFacing == counterClockWise(facing) ? SS(SS::INNER_LEFT)
                                                           : SS(SS::INNER_RIGHT);
        }
    }
    return SS(SS::STRAIGHT);
}

// Reference: IronBarsBlock.attachsTo.
bool barsAttachTo(BlockState* neighbor, bool sturdyFace) {
    return sturdyFace || isId(neighbor, "minecraft:iron_bars")
        || idEndsWith(neighbor, "_pane") || idEndsWith(neighbor, "_wall");
}

BlockState* airState() {
    return world::level::block::Blocks::AIR->defaultBlockState();
}

/** Reference: BlockState.updateShape dispatch for the supported families. */
BlockState* updateShape(BlockState* state, WorldGenLevel* level,
                        const core::BlockPos& pos, Direction dir,
                        const core::BlockPos& neighborPos, BlockState* neighborState) {
    if (state->isAir()) return state;

    // Reference: VegetationBlock.updateShape - DOWN survival.
    if (isBushLike(state)) {
        if (dir == Direction::DOWN && !bushMayPlaceOn(neighborState)) {
            return airState();
        }
        return state;
    }

    // Reference: CarpetBlock.updateShape - below must not be air.
    if (idEndsWith(state, "_carpet")) {
        if (dir == Direction::DOWN && neighborState->isAir()) return airState();
        return state;
    }

    // Reference: SnowLayerBlock.updateShape - DOWN survival.
    if (isId(state, "minecraft:snow")) {
        if (dir == Direction::DOWN) {
            if (::minecraft::levelgen::blockpredicates::matchesBlockTagName(
                    neighborState, "minecraft:snow_layer_cannot_survive_on")) {
                return airState();
            }
            if (!::minecraft::levelgen::blockpredicates::matchesBlockTagName(
                    neighborState, "minecraft:snow_layer_can_survive_on")
                && !neighborState->isFaceSturdy(*level, neighborPos, Direction::UP)) {
                return airState();
            }
        }
        return state;
    }

    // Reference: LadderBlock/WallSignBlock/WallTorchBlock.updateShape -
    // detach when the wall behind goes non-sturdy.
    if (isId(state, "minecraft:ladder") || idEndsWith(state, "_wall_sign")
        || isId(state, "minecraft:wall_torch")) {
        Direction facing = state->getValue(*BlockStateProperties::HORIZONTAL_FACING);
        if (core::getOpposite(dir) == facing
            && !neighborState->isFaceSturdy(*level, neighborPos, facing)) {
            return airState();
        }
        return state;
    }

    // Reference: TorchBlock (incl. redstone) - canSurvive uses
    // Block.canSupportCenter (SupportType.CENTER): fences/walls/bars/panes/
    // chains have a center post and DO support a torch (the mineshaft
    // chain-hang lesson, applied upward).
    if (isId(state, "minecraft:torch") || isId(state, "minecraft:redstone_torch")) {
        if (dir == Direction::DOWN) {
            bool centerPost = idEndsWith(neighborState, "_fence")
                || idEndsWith(neighborState, "_wall")
                || idEndsWith(neighborState, "_pane")
                || isId(neighborState, "minecraft:iron_bars")
                || isId(neighborState, "minecraft:iron_chain")
                || isId(neighborState, "minecraft:chain");
            bool leaves = idEndsWith(neighborState, "_leaves");
            if (leaves
                || (!centerPost
                    && !neighborState->isFaceSturdy(*level, neighborPos, Direction::UP))) {
                return airState();
            }
        }
        return state;
    }

    // Reference: IronBarsBlock (CrossCollisionBlock).updateShape - glass
    // panes are IronBarsBlock subclasses and share the rule.
    if (isId(state, "minecraft:iron_bars") || idEndsWith(state, "_pane")) {
        if (dir != Direction::UP && dir != Direction::DOWN) {
            using FB = world::level::block::FenceBlock;
            props::BooleanProperty* side =
                dir == Direction::NORTH ? FB::NORTH
                : dir == Direction::SOUTH ? FB::SOUTH
                : dir == Direction::WEST ? FB::WEST : FB::EAST;
            bool sturdy = neighborState->isFaceSturdy(*level, neighborPos,
                                                      core::getOpposite(dir));
            return state->setValue(*side, barsAttachTo(neighborState, sturdy));
        }
        return state;
    }

    // Reference: FenceBlock.updateShape - horizontal connections. connectsTo:
    // (!exception && sturdy) || same-fence-kind (wooden vs nether) || gates.
    if (idEndsWith(state, "_fence")) {
        if (dir != Direction::UP && dir != Direction::DOWN) {
            using FB = world::level::block::FenceBlock;
            props::BooleanProperty* side =
                dir == Direction::NORTH ? FB::NORTH
                : dir == Direction::SOUTH ? FB::SOUTH
                : dir == Direction::WEST ? FB::WEST : FB::EAST;
            bool sturdy = neighborState->isFaceSturdy(*level, neighborPos,
                                                      core::getOpposite(dir));
            // Reference: isExceptionForConnection - barriers/carved pumpkins/
            // melons/leaves/shulker boxes never connect.
            const std::string& nid = neighborState->getBlock()->getIdentifier();
            bool exception = idEndsWith(neighborState, "_leaves")
                || nid == "minecraft:barrier" || nid == "minecraft:melon"
                || nid == "minecraft:pumpkin" || nid == "minecraft:carved_pumpkin"
                || nid == "minecraft:jack_o_lantern"
                || idEndsWith(neighborState, "shulker_box");
            bool stateWooden = !isId(state, "minecraft:nether_brick_fence");
            bool neighborIsFence = idEndsWith(neighborState, "_fence");
            bool neighborWooden = neighborIsFence
                && !isId(neighborState, "minecraft:nether_brick_fence");
            bool sameFence = neighborIsFence && stateWooden == neighborWooden;
            bool gate = idEndsWith(neighborState, "_fence_gate");
            bool connects = (!exception && sturdy) || sameFence || gate;
            return state->setValue(*side, connects);
        }
        return state;
    }

    // Reference: WallBlock.updateShape - topUpdate/sideUpdate recompute the
    // four WallSide connections and the UP post from the neighbors and the
    // block above. isCovered voxel tests approximated: full-DOWN-face blocks
    // (full cubes, bottom/double slabs, bottom stairs) cover everything; a
    // wall above covers a side strip when that side is connected and the
    // post test when its own post is up.
    if (idEndsWith(state, "_wall")) {
        if (dir == Direction::DOWN) return state;
        using WS = props::WallSide;
        auto* NW = BlockStateProperties::NORTH_WALL;
        auto* EW = BlockStateProperties::EAST_WALL;
        auto* SW = BlockStateProperties::SOUTH_WALL;
        auto* WW = BlockStateProperties::WEST_WALL;
        auto isConnected = [&](props::EnumProperty<props::WallSide>* side) {
            return state->getValue(*side).getValue() != WS::NONE;
        };
        // Reference: WallBlock.connectsTo (toward = direction from the
        // neighbor to this wall; gates connect when their FACING axis is
        // perpendicular to it).
        auto wallConnectsTo = [&](BlockState* neighbor, bool sturdy, Direction toward) {
            const std::string& nid = neighbor->getBlock()->getIdentifier();
            bool gate = idEndsWith(neighbor, "_fence_gate");
            if (gate) {
                Direction gateFacing =
                    neighbor->getValue(*BlockStateProperties::HORIZONTAL_FACING);
                bool gateAxisX = gateFacing == Direction::WEST || gateFacing == Direction::EAST;
                bool towardAxisX = toward == Direction::WEST || toward == Direction::EAST;
                gate = gateAxisX != towardAxisX;  // clockwise axis match
            }
            bool exception = idEndsWith(neighbor, "_leaves")
                || nid == "minecraft:barrier" || nid == "minecraft:melon"
                || nid == "minecraft:pumpkin" || nid == "minecraft:carved_pumpkin"
                || nid == "minecraft:jack_o_lantern"
                || idEndsWith(neighbor, "shulker_box");
            return idEndsWith(neighbor, "_wall") || (!exception && sturdy)
                || isId(neighbor, "minecraft:iron_bars") || idEndsWith(neighbor, "_pane")
                || gate;
        };
        bool n, e, s, w;
        if (dir == Direction::UP) {
            n = isConnected(NW);
            e = isConnected(EW);
            s = isConnected(SW);
            w = isConnected(WW);
        } else {
            Direction opposite = core::getOpposite(dir);
            bool connects = wallConnectsTo(
                neighborState,
                neighborState->isFaceSturdy(*level, neighborPos, opposite), opposite);
            n = dir == Direction::NORTH ? connects : isConnected(NW);
            e = dir == Direction::EAST ? connects : isConnected(EW);
            s = dir == Direction::SOUTH ? connects : isConnected(SW);
            w = dir == Direction::WEST ? connects : isConnected(WW);
        }
        core::BlockPos abovePos(pos.getX(), pos.getY() + 1, pos.getZ());
        BlockState* aboveState = level->getBlockState(abovePos);
        bool aboveCoversAll = aboveState->isCollisionShapeFullBlock(*level, abovePos);
        if (std::getenv("WALL_PROBE")) {
            fprintf(stderr, "WALL_PROBE pos=%d,%d,%d dir=%d above=%s coversAll=%d\n",
                    pos.getX(), pos.getY(), pos.getZ(), static_cast<int>(dir),
                    aboveState->getBlock()->getIdentifier().c_str(),
                    aboveCoversAll ? 1 : 0);
        }
        if (!aboveCoversAll && idEndsWith(aboveState, "_slab")) {
            using ST = props::SlabType;
            aboveCoversAll = aboveState->getValue(*BlockStateProperties::SLAB_TYPE)
                                 .getValue() != ST::TOP;
        }
        if (!aboveCoversAll && idEndsWith(aboveState, "_stairs")) {
            using HF = props::Half;
            aboveCoversAll = aboveState->getValue(*BlockStateProperties::HALF)
                                 .getValue() == HF::BOTTOM;
        }
        bool aboveIsWall = idEndsWith(aboveState, "_wall");
        auto sideCovered = [&](props::EnumProperty<props::WallSide>* side) {
            if (aboveCoversAll) return true;
            return aboveIsWall
                && aboveState->getValue(*side).getValue() != WS::NONE;
        };
        auto makeWallState = [&](bool connected,
                                 props::EnumProperty<props::WallSide>* side) {
            if (!connected) return WS(WS::NONE);
            return sideCovered(side) ? WS(WS::TALL) : WS(WS::LOW);
        };
        BlockState* result = state->setValue(*NW, makeWallState(n, NW));
        result = result->setValue(*EW, makeWallState(e, EW));
        result = result->setValue(*SW, makeWallState(s, SW));
        result = result->setValue(*WW, makeWallState(w, WW));
        // Reference: WallBlock.shouldRaisePost.
        bool up;
        if (aboveIsWall && aboveState->getValue(*BlockStateProperties::UP)) {
            up = true;
        } else {
            WS::Value nv = result->getValue(*NW).getValue();
            WS::Value ev = result->getValue(*EW).getValue();
            WS::Value sv = result->getValue(*SW).getValue();
            WS::Value wv = result->getValue(*WW).getValue();
            bool noneN = nv == WS::NONE;
            bool noneE = ev == WS::NONE;
            bool noneS = sv == WS::NONE;
            bool noneW = wv == WS::NONE;
            if ((noneN && noneS && noneE && noneW) || noneN != noneS || noneE != noneW) {
                up = true;
            } else if ((nv == WS::TALL && sv == WS::TALL)
                       || (ev == WS::TALL && wv == WS::TALL)) {
                up = false;
            } else {
                bool postOverride = ::minecraft::levelgen::blockpredicates::matchesBlockTagName(
                    aboveState, "minecraft:wall_post_override");
                bool postCovered = aboveCoversAll
                    || (aboveIsWall && aboveState->getValue(*BlockStateProperties::UP));
                up = postOverride || postCovered;
            }
        }
        return result->setValue(*BlockStateProperties::UP, up);
    }

    // Reference: StairBlock.updateShape - horizontal changes recompute SHAPE.
    if (idEndsWith(state, "_stairs")) {
        if (dir != Direction::UP && dir != Direction::DOWN) {
            return state->setValue(*BlockStateProperties::STAIRS_SHAPE,
                                   getStairsShape(state, level, pos));
        }
        return state;
    }

    // Reference: DoorBlock.updateShape - halves keep each other (copying the
    // neighbor's facing/open/hinge), lower half needs a sturdy floor.
    if (idEndsWith(state, "_door")) {
        using DBH = props::DoubleBlockHalf;
        DBH half = state->getValue(*BlockStateProperties::DOUBLE_BLOCK_HALF);
        if (dir == Direction::UP || dir == Direction::DOWN) {
            bool towardOther = (half.getValue() == DBH::LOWER) == (dir == Direction::UP);
            if (towardOther) {
                if (idEndsWith(neighborState, "_door")
                    && neighborState->getValue(*BlockStateProperties::DOUBLE_BLOCK_HALF)
                               .getValue() != half.getValue()) {
                    return neighborState->setValue(*BlockStateProperties::DOUBLE_BLOCK_HALF, half);
                }
                return airState();
            }
            if (half.getValue() == DBH::LOWER && dir == Direction::DOWN
                && !neighborState->isFaceSturdy(*level, neighborPos, Direction::UP)) {
                return airState();
            }
        }
        return state;
    }

    // Reference: DoublePlantBlock.updateShape - the half toward the partner
    // must be the same plant's other half, else AIR.
    {
        const std::string& id = state->getBlock()->getIdentifier();
        bool doublePlant = id == "minecraft:tall_seagrass" || id == "minecraft:tall_grass"
            || id == "minecraft:large_fern" || id == "minecraft:sunflower"
            || id == "minecraft:lilac" || id == "minecraft:rose_bush"
            || id == "minecraft:peony" || id == "minecraft:pitcher_plant";
        if (doublePlant) {
            using DP = world::level::block::DoublePlantBlock;
            using DBH = props::DoubleBlockHalf;
            DBH half = state->getValue(*DP::HALF);
            bool towardOther = (half.getValue() == DBH::LOWER) == (dir == Direction::UP);
            if ((dir == Direction::UP || dir == Direction::DOWN) && towardOther) {
                if (neighborState->getBlock() == state->getBlock()
                    && neighborState->getValue(*DP::HALF).getValue() != half.getValue()) {
                    return state;
                }
                return airState();
            }
            if (half.getValue() == DBH::LOWER && dir == Direction::DOWN) {
                // Reference: canSurvive - seagrass needs a sturdy top face,
                // the land plants need dirt/farmland.
                if (id == "minecraft:tall_seagrass") {
                    if (!neighborState->isFaceSturdy(*level, neighborPos, Direction::UP)
                        || neighborState->getBlock()->getIdentifier() == "minecraft:magma_block") {
                        return airState();
                    }
                } else if (!bushMayPlaceOn(neighborState)) {
                    return airState();
                }
            }
            return state;
        }
    }

    // Reference: BedBlock.updateShape - the two halves keep each other.
    if (idEndsWith(state, "_bed")) {
        using BP = props::BedPart;
        Direction facing = state->getValue(*BlockStateProperties::HORIZONTAL_FACING);
        BP part = state->getValue(*BlockStateProperties::BED_PART);
        Direction neighbourDir = part.getValue() == BP::FOOT ? facing
                                                             : core::getOpposite(facing);
        if (dir == neighbourDir) {
            if (idEndsWith(neighborState, "_bed")
                && neighborState->getValue(*BlockStateProperties::BED_PART).getValue()
                       != part.getValue()) {
                return state->setValue(*BlockStateProperties::OCCUPIED,
                    neighborState->getValue(*BlockStateProperties::OCCUPIED));
            }
            return airState();
        }
        return state;
    }

    return state;
}

} // namespace shape_updates

} // namespace

BlockState* updateShapeForBlock(BlockState* state, WorldGenLevel* level,
                                const core::BlockPos& pos, core::Direction dir,
                                const core::BlockPos& neighborPos,
                                BlockState* neighborState) {
    return shape_updates::updateShape(state, level, pos, dir, neighborPos, neighborState);
}

const FullTemplateData& get(const std::string& templateId) {
    return load(templateId);
}

// Reference: StructureTemplate.transform(pos, mirror, rotation, pivot).
core::BlockPos calculateRelativePosition(const TemplatePlaceSettings& settings,
                                         const core::BlockPos& localPos) {
    int x = localPos.getX();
    int y = localPos.getY();
    int z = localPos.getZ();
    bool transformed = true;
    switch (settings.mirror) {
        case 1:  // LEFT_RIGHT: flip Z
            z = -z;
            break;
        case 2:  // FRONT_BACK: flip X
            x = -x;
            break;
        default:
            transformed = false;
            break;
    }
    int pivotX = settings.rotationPivot.getX();
    int pivotZ = settings.rotationPivot.getZ();
    switch (settings.rotation) {
        case 3:  // COUNTERCLOCKWISE_90
            return core::BlockPos(pivotX - pivotZ + z, y, pivotX + pivotZ - x);
        case 1:  // CLOCKWISE_90
            return core::BlockPos(pivotX + pivotZ - z, y, pivotZ - pivotX + x);
        case 2:  // CLOCKWISE_180
            return core::BlockPos(pivotX + pivotX - x, y, pivotZ + pivotZ - z);
        default:
            return transformed ? core::BlockPos(x, y, z) : localPos;
    }
}

std::vector<DataMarker> dataMarkers(const std::string& templateId,
                                    const core::BlockPos& position,
                                    const TemplatePlaceSettings& settings,
                                    const BoundingBox& chunkBB) {
    // Reference: template.filterBlocks(templatePosition, settings,
    // STRUCTURE_BLOCK) - all palettes share positions; markers come from the
    // shared blocks list. filterBlocks clips each world position to the
    // settings bounding box (= chunkBB), so a marker fires only during the
    // pass of the chunk that contains it.
    std::vector<DataMarker> markers;
    const FullTemplateData& data = get(templateId);
    for (const TemplateBlockInfo& info : data.blocks) {
        if (!info.isDataMarker) continue;
        core::BlockPos world = calculateRelativePosition(
            settings, core::BlockPos(info.x, info.y, info.z)).offset(
                position.getX(), position.getY(), position.getZ());
        if (!chunkBB.isInside(world.getX(), world.getY(), world.getZ())) continue;
        markers.push_back({world, info.metadata});
    }
    return markers;
}

namespace {

// Canonical serialization of a template-nbt VALUE (CanonicalNbt over the
// retained tag). Used to pass template subtrees (Items, patterns, SpawnData,
// components, sign text) straight into the save payload.
std::string canonicalValue(const nbt::Tag* tag) {
    std::string out;
    nbt::canonical::appendTag(out, tag);
    return out;
}

} // namespace

std::string blockEntityPayloadFor(const std::string& blockId,
                                  const nbt::CompoundTag* templateNbt,
                                  std::optional<int64_t> lootSeed) {
    // Reference: the BlockEntity load->save round trip that
    // getBlockEntityNbtForSaving serializes (pinned by the java_be_* dumps in
    // tests/parity/targets/B8_BE_INVENTORY.md). Returns "" when the type is
    // not modeled yet - the pending {id:"DUMMY"} tag then remains, which is
    // VISIBLE as an E mismatch, never silent.
    auto lootContainer = [&](const char* beId) -> std::string {
        if (templateNbt == nullptr) return "";
        std::string lootTable = templateNbt->getStringOr("LootTable", "");
        if (!lootTable.empty() && lootSeed) {
            // Loot-table container: the seed is the nextLong drawn at
            // placement.
            return "{LootTable:\"" + lootTable + "\",LootTableSeed:"
                 + std::to_string(*lootSeed) + "l,components:{},id:\""
                 + beId + "\"}";
        }
        // Plain container: Items straight from the template nbt (empty list
        // when absent) - Java saves Items:[] for empty containers.
        std::string items = "[]";
        if (const nbt::ListTag* list = templateNbt->getListPtr("Items")) {
            items = canonicalValue(list);
        }
        return "{Items:" + items + ",components:{},id:\"" + beId + "\"}";
    };

    if (blockId == "minecraft:chest") return lootContainer("minecraft:chest");
    if (blockId == "minecraft:trapped_chest") return lootContainer("minecraft:trapped_chest");
    if (blockId == "minecraft:barrel") return lootContainer("minecraft:barrel");
    if (blockId == "minecraft:dispenser") return lootContainer("minecraft:dispenser");
    if (blockId == "minecraft:dropper") return lootContainer("minecraft:dropper");
    if (blockId == "minecraft:hopper") {
        if (templateNbt != nullptr
            && !templateNbt->getStringOr("LootTable", "").empty()) {
            return lootContainer("minecraft:hopper");
        }
        // Plain hopper save carries the cooldown field.
        return "{Items:[],TransferCooldown:0,components:{},id:\"minecraft:hopper\"}";
    }
    if (blockId == "minecraft:decorated_pot") {
        // Loot fields (when the template carries a LootTable + the placement
        // draw), then item/sherds straight from the template nbt. Canonical
        // key order is alphabetical: LootTable, LootTableSeed, components,
        // id, item, sherds.
        std::string out = "{";
        if (templateNbt != nullptr) {
            std::string lootTable = templateNbt->getStringOr("LootTable", "");
            if (!lootTable.empty() && lootSeed) {
                out += "LootTable:\"" + lootTable + "\",LootTableSeed:"
                     + std::to_string(*lootSeed) + "l,";
            }
        }
        out += "components:{},id:\"minecraft:decorated_pot\"";
        if (templateNbt != nullptr) {
            if (const nbt::CompoundTag* item = templateNbt->getCompoundPtr("item")) {
                out += ",item:" + canonicalValue(item);
            }
            if (const nbt::ListTag* sherds = templateNbt->getListPtr("sherds")) {
                if (sherds->size() > 0) out += ",sherds:" + canonicalValue(sherds);
            }
        }
        out += "}";
        return out;
    }
    if (blockId == "minecraft:vault") {
        // config from the template nbt; server_data/shared_data reset empty.
        // The VaultConfig codec OMITS default-valued fields on save: the
        // normal vaults' loot_table (chests/trial_chambers/reward) is the
        // default and disappears; ominous reward_ominous is kept.
        std::string config = "{}";
        if (templateNbt != nullptr) {
            if (const nbt::CompoundTag* c = templateNbt->getCompoundPtr("config")) {
                auto copy = std::unique_ptr<nbt::CompoundTag>(
                    static_cast<nbt::CompoundTag*>(c->copy().release()));
                if (copy->getStringOr("loot_table", "")
                    == "minecraft:chests/trial_chambers/reward") {
                    copy->remove("loot_table");
                }
                config = canonicalValue(copy.get());
            }
        }
        return "{components:{},config:" + config
             + ",id:\"minecraft:vault\",server_data:{},shared_data:{}}";
    }
    if (blockId == "minecraft:trial_spawner") {
        std::string out = "{components:{},id:\"minecraft:trial_spawner\"";
        if (templateNbt != nullptr) {
            std::string normal = templateNbt->getStringOr("normal_config", "");
            std::string ominous = templateNbt->getStringOr("ominous_config", "");
            if (!normal.empty()) out += ",normal_config:\"" + normal + "\"";
            if (!ominous.empty()) out += ",ominous_config:\"" + ominous + "\"";
        }
        out += "}";
        return out;
    }
    if (blockId == "minecraft:sculk_sensor" || blockId == "minecraft:calibrated_sculk_sensor") {
        return "{components:{},id:\"" + blockId
             + "\",last_vibration_frequency:0,listener:{event_delay:0,"
               "selector:{tick:-1l}}}";
    }
    if (blockId == "minecraft:lectern") {
        return "{components:{},id:\"minecraft:lectern\"}";
    }
    if (blockId == "minecraft:comparator") {
        return "{OutputSignal:0,components:{},id:\"minecraft:comparator\"}";
    }
    if (blockId.size() > 4
        && blockId.compare(blockId.size() - 4, 4, "_bed") == 0) {
        // All colors share the "minecraft:bed" BE type; saves no fields.
        return "{components:{},id:\"minecraft:bed\"}";
    }
    if (blockId == "minecraft:furnace" || blockId == "minecraft:blast_furnace"
        || blockId == "minecraft:smoker") {
        std::string beId = blockId;
        return "{Items:[],RecipesUsed:{},components:{},cooking_time_spent:0s,"
               "cooking_total_time:0s,id:\"" + beId
             + "\",lit_time_remaining:0s,lit_total_time:0s}";
    }
    if (blockId == "minecraft:bell") {
        return "{components:{},id:\"minecraft:bell\"}";
    }
    if (blockId == "minecraft:spawner") {
        // BaseSpawner defaults + SpawnData from the template nbt (mansion
        // jail spawners etc.); bare spawners keep only the defaults.
        std::string spawnData;
        if (templateNbt != nullptr) {
            if (const nbt::CompoundTag* sd = templateNbt->getCompoundPtr("SpawnData")) {
                spawnData = canonicalValue(sd);
            }
        }
        std::string out = "{Delay:20s,MaxNearbyEntities:6s,MaxSpawnDelay:800s,"
                          "MinSpawnDelay:200s,RequiredPlayerRange:16s,SpawnCount:4s,";
        if (!spawnData.empty()) {
            out += "SpawnData:" + spawnData + ",";
        }
        out += "SpawnPotentials:[],SpawnRange:4s,components:{},"
               "id:\"minecraft:mob_spawner\"}";
        return out;
    }
    if (blockId.size() > 7
        && blockId.compare(blockId.size() - 7, 7, "_banner") == 0) {
        // All colors share the "minecraft:banner" BE id; patterns and any
        // components come straight from the template nbt (ominous outpost
        // banners carry item_name/rarity components there).
        std::string patterns;
        std::string components = "{}";
        if (templateNbt != nullptr) {
            if (const nbt::ListTag* list = templateNbt->getListPtr("patterns")) {
                // Java omits the key entirely for empty pattern lists.
                if (list->size() > 0) patterns = canonicalValue(list);
            }
            if (const nbt::CompoundTag* comp = templateNbt->getCompoundPtr("components")) {
                components = canonicalValue(comp);
            }
        }
        std::string out = "{components:" + components + ",id:\"minecraft:banner\"";
        if (!patterns.empty()) {
            out += ",patterns:" + patterns;
        }
        out += "}";
        return out;
    }
    return "";
}

BlockState* parseBlockStateSpec(const std::string& spec) {
    using world::level::block::Blocks;
    // Parse "namespace:block[k=v,...]".
    std::string blockName = spec;
    std::unordered_map<std::string, std::string> want;
    size_t bracket = spec.find('[');
    if (bracket != std::string::npos) {
        blockName = spec.substr(0, bracket);
        std::string props = spec.substr(bracket + 1,
                                        spec.find(']') - bracket - 1);
        size_t start = 0;
        while (start < props.size()) {
            size_t comma = props.find(',', start);
            if (comma == std::string::npos) comma = props.size();
            std::string pair = props.substr(start, comma - start);
            size_t eq = pair.find('=');
            if (eq != std::string::npos) {
                want[pair.substr(0, eq)] = pair.substr(eq + 1);
            }
            start = comma + 1;
        }
    }
    if (blockName.find(':') == std::string::npos) {
        blockName = "minecraft:" + blockName;
    }
    Block* block = Blocks::getBlock(blockName);
    if (block == nullptr) {
        throw std::runtime_error("block-state spec uses unregistered block "
                                 + blockName);
    }
    BlockState* target = block->defaultBlockState();
    if (!want.empty()) {
        // Reference: BlockStateParser - the spec is applied ON TOP of the
        // default state, so properties NOT listed keep their default values
        // (a partial spec like waxed_copper_bulb[lit=true] must keep
        // powered=false). Match the unique state where listed properties
        // equal the spec and every other property equals the default's.
        if (target == nullptr) {
            throw std::runtime_error("block-state spec block has no default state: "
                                     + blockName);
        }
        auto defaults = target->getProperties();
        for (BlockState* candidate : block->getStateDefinition().getPossibleStates()) {
            auto have = candidate->getProperties();
            bool match = true;
            for (const auto& [key, value] : have) {
                auto it = want.find(key);
                if (it != want.end()) {
                    if (value != it->second) {
                        match = false;
                        break;
                    }
                } else {
                    auto dv = defaults.find(key);
                    if (dv != defaults.end() && value != dv->second) {
                        match = false;
                        break;
                    }
                }
            }
            if (match) {
                target = candidate;
                break;
            }
        }
    }
    return target;
}

void applyJigsawFinalStates(WorldGenLevel* level, const std::string& templateId,
                            const core::BlockPos& position,
                            const TemplatePlaceSettings& settings) {
    const FullTemplateData& data = get(templateId);
    for (const TemplateBlockInfo& info : data.blocks) {
        if (info.jigsawFinalState.empty()) continue;
        core::BlockPos world = calculateRelativePosition(
            settings, core::BlockPos(info.x, info.y, info.z)).offset(
                position.getX(), position.getY(), position.getZ());
        level->setBlock(world, parseBlockStateSpec(info.jigsawFinalState), 3);
    }
}

bool placeInWorld(WorldGenLevel* level, const std::string& templateId,
                  const core::BlockPos& position, const core::BlockPos& referencePos,
                  const TemplatePlaceSettings& settings, WorldgenRandom& random,
                  const BoundingBox& chunkBB) {
    using core::Direction;
    using world::level::block::Blocks;
    const FullTemplateData& data = get(templateId);
    if (data.palettes.empty()) return false;

    // Reference: settings.getRandomPalette(palettes, position) -
    // LegacyRandomSource(Mth.getSeed(pos)).nextInt(paletteCount).
    size_t paletteIndex = 0;
    if (data.palettes.size() > 1) {
        LegacyRandomSource paletteRandom(
            Mth::getSeed(position.getX(), position.getY(), position.getZ()));
        paletteIndex = static_cast<size_t>(
            paletteRandom.nextInt(static_cast<int32_t>(data.palettes.size())));
    }
    const std::vector<BlockState*>& palette = data.palettes[paletteIndex];
    if (data.blocks.empty() || data.sizeX < 1 || data.sizeY < 1 || data.sizeZ < 1) {
        return false;
    }

    // Reference: processBlockInfos - world position transform, then the
    // processor chain on the UNROTATED state (ignore-air etc.).
    struct Processed {
        core::BlockPos pos;
        BlockState* state;
        const TemplateBlockInfo* info;
        // B8: capped append_loot payload, written AFTER setBlock (which lays
        // down the pending DUMMY tag this replaces).
        std::string cappedPayload;
    };
    std::vector<Processed> processed;
    processed.reserve(data.blocks.size());
    Block* structureBlock = Blocks::getBlock("minecraft:structure_block");
    for (const TemplateBlockInfo& info : data.blocks) {
        core::BlockPos world = calculateRelativePosition(
            settings, core::BlockPos(info.x, info.y, info.z)).offset(
                position.getX(), position.getY(), position.getZ());
        BlockState* state = palette[static_cast<size_t>(info.stateIdx)];
        // Java threads originalBlockInfo (the raw palette state) through the
        // whole processor chain; BlockRotProcessor's rottable gate tests it.
        BlockState* originalState = state;
        // Reference: BlockIgnoreProcessor.STRUCTURE_BLOCK - always first.
        if (state->is(structureBlock)) continue;
        // Reference: JigsawReplacementProcessor - jigsaw + nbt becomes the
        // parsed final_state (default air); structure_void drops the block.
        // Runs BEFORE the rule-processor lambdas.
        if (settings.jigsawReplacement
            && state->getBlock()->getIdentifier() == "minecraft:jigsaw") {
            const std::string& spec =
                info.jigsawFinalState.empty() ? "minecraft:air" : info.jigsawFinalState;
            if (spec == "minecraft:structure_void" || spec == "structure_void") {
                continue;
            }
            state = parseBlockStateSpec(spec);
        }
        // Reference: LegacySinglePoolElement pops STRUCTURE_BLOCK and APPENDS
        // STRUCTURE_AND_AIR - the air ignore runs AFTER JigsawReplacement, so
        // jigsaw-produced air is dropped too (an outpost feature_plate must
        // NOT erase the natural surface). Template air takes the same path.
        if (settings.ignoreAir && state->isAir()) continue;
        bool dropped = false;
        for (const auto& processor : settings.processors) {
            state = processor(world, state, core::BlockPos(info.x, info.y, info.z),
                              originalState, referencePos);
            if (state == nullptr) {
                dropped = true;
                break;
            }
        }
        if (dropped) continue;
        // Reference: GravityProcessor (TERRAIN_MATCHING) - LAST in the chain:
        // rules saw the pre-gravity pos; the placed y snaps to the heightmap
        // plus the ORIGINAL template-local y.
        if (settings.terrainMatchingGravity) {
            int height = level->getHeight(Heightmap::Types::WORLD_SURFACE_WG,
                                          world.getX(), world.getZ())
                       + settings.gravityOffset;
            world = core::BlockPos(world.getX(), height + info.y, world.getZ());
        }
        processed.push_back({world, state, &info});
    }

    // Reference: CappedProcessor.finalizeProcessing - runs after the
    // per-block chain over the SURVIVING list; positional random at the
    // template position; ConstantInt limits sample without drawing.
    for (const TemplatePlaceSettings::CappedReplace& capped : settings.cappedReplaces) {
        if (capped.limit == 0 || processed.empty()) continue;
        LegacyRandomSource base(level->getSeed());
        LegacyRandomSource cappedRandom = base.forkPositional().at(
            position.getX(), position.getY(), position.getZ());
        int maxToReplace = std::min(capped.limit, static_cast<int>(processed.size()));
        if (maxToReplace < 1) continue;
        std::vector<int> indices(processed.size());
        for (size_t i = 0; i < indices.size(); ++i) indices[i] = static_cast<int>(i);
        // Util.toShuffledList - reverse Fisher-Yates.
        for (size_t i = indices.size(); i > 1; --i) {
            std::swap(indices[i - 1],
                      indices[static_cast<size_t>(cappedRandom.nextInt(static_cast<int32_t>(i)))]);
        }
        Block* fromBlock = nullptr;
        if (capped.fromTag.empty()) {
            fromBlock = Blocks::getBlock(capped.fromBlock);
            if (fromBlock == nullptr) {
                throw std::runtime_error("CappedReplace uses unregistered block: "
                                         + capped.fromBlock);
            }
        }
        BlockState* toState = capped.toState;
        if (toState == nullptr) {
            toState = Blocks::getDefaultState(capped.toBlock);
            if (toState == nullptr) {
                throw std::runtime_error("CappedReplace uses unregistered block: "
                                         + capped.toBlock);
            }
        }
        int replaced = 0;
        for (size_t k = 0; k < indices.size() && replaced < maxToReplace; ++k) {
            Processed& entry = processed[static_cast<size_t>(indices[k])];
            bool matches = capped.fromTag.empty()
                ? entry.state->is(fromBlock)
                : ::minecraft::levelgen::blockpredicates::matchesBlockTagName(
                      entry.state, capped.fromTag);
            if (matches) {
                entry.state = toState;
                ++replaced;
                if (!capped.lootTable.empty()) {
                    // Reference: AppendLoot.apply via the delegate rule -
                    // seed = first nextLong of the rule's positional random.
                    // Stored on the entry and written after setBlock (which
                    // lays down the pending DUMMY tag this must replace).
                    LegacyRandomSource ruleRandom(Mth::getSeed(
                        entry.pos.getX(), entry.pos.getY(), entry.pos.getZ()));
                    int64_t seed = ruleRandom.nextLong();
                    entry.cappedPayload = "{LootTable:\"" + capped.lootTable
                        + "\",LootTableSeed:" + std::to_string(seed)
                        + "l,components:{},id:\"" + capped.beId + "\"}";
                }
            }
        }
    }

    // Reference: the placement loop.
    std::vector<core::BlockPos> placedPositions;
    placedPositions.reserve(processed.size());
    std::vector<core::BlockPos> toFill;
    std::set<int64_t> lockedFluids;
    auto packPos = [](const core::BlockPos& p) {
        return (static_cast<int64_t>(p.getX()) << 40)
             ^ (static_cast<int64_t>(p.getY() & 0xFFFFF) << 20)
             ^ static_cast<int64_t>(p.getZ() & 0xFFFFF);
    };
    int minX = INT32_MAX, minY = INT32_MAX, minZ = INT32_MAX;
    int maxX = INT32_MIN, maxY = INT32_MIN, maxZ = INT32_MIN;
    // Reference: FluidState during worldgen - water sources come from the
    // water block itself, waterlogged states, and always-waterlogged plants.
    Block* waterBlock = Blocks::getBlock("minecraft:water");
    auto* waterloggedProp =
        world::level::block::state::properties::BlockStateProperties::WATERLOGGED;
    // Reference: LiquidBlockContainer.canPlaceLiquid - SlabBlock overrides it
    // to refuse waterlogging when TYPE == DOUBLE (a full cube). The plain
    // hasProperty(WATERLOGGED) proxy would waterlog double slabs placed into
    // water, which Java never does.
    auto* slabTypeProp =
        world::level::block::state::properties::BlockStateProperties::SLAB_TYPE;
    auto canPlaceLiquid = [waterloggedProp, slabTypeProp](BlockState* s) {
        if (!s->hasProperty(waterloggedProp)) return false;
        if (s->hasProperty(slabTypeProp)
            && s->getValue(*slabTypeProp) ==
                   world::level::block::state::properties::SlabType::DOUBLE) {
            return false;
        }
        return true;
    };
    auto hasWaterSource = [&](BlockState* state) {
        if (state->is(waterBlock)) return true;
        if (state->hasProperty(waterloggedProp) && state->getValue(*waterloggedProp)) {
            return true;
        }
        const std::string& id = state->getBlock()->getIdentifier();
        return id == "minecraft:kelp" || id == "minecraft:kelp_plant"
            || id == "minecraft:seagrass" || id == "minecraft:tall_seagrass"
            || id == "minecraft:bubble_column";
    };
    for (const Processed& entry : processed) {
        const core::BlockPos& pos = entry.pos;
        if (!chunkBB.isInside(pos.getX(), pos.getY(), pos.getZ())) continue;
        bool hadWater = settings.keepLiquids && hasWaterSource(level->getBlockState(pos));
        BlockState* state = state_transforms::mirrorState(entry.state, settings.mirror);
        state = state_transforms::rotateState(state, settings.rotation);
        // (barrier pre-placement for nbt blocks is overwritten immediately -
        // dump-invisible, skipped.)
        level->setBlock(pos, state, 2);
        minX = std::min(minX, pos.getX());
        minY = std::min(minY, pos.getY());
        minZ = std::min(minZ, pos.getZ());
        maxX = std::max(maxX, pos.getX());
        maxY = std::max(maxY, pos.getY());
        maxZ = std::max(maxZ, pos.getZ());
        placedPositions.push_back(pos);
        if (entry.info->hasNbt) {
            // Reference: the template BE load (loadWithComponents) + the
            // RandomizableContainer LootTableSeed draw. The pending
            // {id:"DUMMY"} tag written by WorldGenRegion.setBlock is
            // overwritten with the save-format payload when the type is
            // modeled; unmodeled types keep DUMMY (visible in E gates).
            std::optional<int64_t> lootSeed;
            if (entry.info->isLootContainer) {
                lootSeed = random.nextLong();
            }
            std::string payload = blockEntityPayloadFor(
                state->getBlock()->getIdentifier(), entry.info->nbt.get(),
                lootSeed);
            if (!payload.empty()) {
                if (::world::IChunk* chunk =
                        level->getChunk(pos.getX() >> 4, pos.getZ() >> 4)) {
                    chunk->setBlockEntityNbt(pos, std::move(payload));
                }
            }
        }
        if (!entry.cappedPayload.empty()) {
            // Capped append_loot (archaeology) - replaces the pending tag.
            if (::world::IChunk* chunk =
                    level->getChunk(pos.getX() >> 4, pos.getZ() >> 4)) {
                chunk->setBlockEntityNbt(pos, entry.cappedPayload);
            }
        }
        if (settings.keepLiquids) {
            // Reference: the waterlogging block - previousFluidState is read
            // for EVERY placed block; placed water sources lock; waterloggable
            // blocks absorb a previous source, and ANY waterloggable placed
            // over a non-source (including dry ground!) joins toFill for the
            // neighbor flood pass below.
            if (hasWaterSource(state)) {
                lockedFluids.insert(packPos(pos));
            } else if (canPlaceLiquid(state)) {
                if (hadWater) {
                    level->setBlock(pos, state->setValue(*waterloggedProp, true), 2);
                }
                if (!hadWater) {
                    toFill.push_back(pos);
                }
            }
        }
    }

    // Reference: the flood loop - directions [UP, N, E, S, W]; a toFill
    // position adjacent to an unlocked water source becomes waterlogged;
    // repeat until stable.
    {
        static const Direction kFloodDirs[5] = {
            Direction::UP, Direction::NORTH, Direction::EAST,
            Direction::SOUTH, Direction::WEST};
        bool filled = true;
        while (filled && !toFill.empty()) {
            filled = false;
            for (auto it = toFill.begin(); it != toFill.end();) {
                const core::BlockPos& pos = *it;
                bool source = hasWaterSource(level->getBlockState(pos));
                for (int i = 0; i < 5 && !source; ++i) {
                    core::BlockPos neighborPos(
                        pos.getX() + core::getStepX(kFloodDirs[i]),
                        pos.getY() + (kFloodDirs[i] == Direction::UP ? 1 : 0),
                        pos.getZ() + core::getStepZ(kFloodDirs[i]));
                    if (hasWaterSource(level->getBlockState(neighborPos))
                        && lockedFluids.find(packPos(neighborPos)) == lockedFluids.end()) {
                        source = true;
                    }
                }
                if (source) {
                    BlockState* state = level->getBlockState(pos);
                    if (canPlaceLiquid(state)
                        && !state->getValue(*waterloggedProp)) {
                        level->setBlock(pos, state->setValue(*waterloggedProp, true), 2);
                    }
                    it = toFill.erase(it);
                    filled = true;
                } else {
                    ++it;
                }
            }
        }
    }

    if (placedPositions.empty()) return true;

    // Reference: if (!settings.getKnownShape()) - pool elements set
    // knownShape and skip BOTH shape-update passes entirely.
    if (settings.knownShape) return true;

    // Reference: updateShapeAtEdge over the discrete shape of placed
    // positions - forAllFaces in AxisCycle order NONE (Z faces), FORWARD
    // (Y faces), BACKWARD (X faces); boundary transitions only.
    int sizeX = maxX - minX + 1;
    int sizeY = maxY - minY + 1;
    int sizeZ = maxZ - minZ + 1;
    std::vector<uint8_t> filled(static_cast<size_t>(sizeX) * sizeY * sizeZ, 0);
    auto fillIndex = [&](int x, int y, int z) {
        return (static_cast<size_t>(x) * sizeY + y) * sizeZ + z;
    };
    for (const core::BlockPos& pos : placedPositions) {
        filled[fillIndex(pos.getX() - minX, pos.getY() - minY, pos.getZ() - minZ)] = 1;
    }
    auto isFull = [&](int x, int y, int z) {
        if (x < 0 || y < 0 || z < 0 || x >= sizeX || y >= sizeY || z >= sizeZ) return false;
        return filled[fillIndex(x, y, z)] != 0;
    };
    auto faceUpdate = [&](Direction dir, int x, int y, int z) {
        core::BlockPos pos(minX + x, minY + y, minZ + z);
        core::BlockPos neighborPos = shape_updates::relative(pos, dir);
        BlockState* state = level->getBlockState(pos);
        BlockState* neighborState = level->getBlockState(neighborPos);
        BlockState* newState = shape_updates::updateShape(
            state, level, pos, dir, neighborPos, neighborState);
        if (state != newState) level->setBlock(pos, newState, 2);
        BlockState* newNeighbor = shape_updates::updateShape(
            neighborState, level, neighborPos, core::getOpposite(dir), pos, newState);
        if (neighborState != newNeighbor) level->setBlock(neighborPos, newNeighbor, 2);
    };
    // Pass 1: NONE - c axis Z (north/south faces), a = X, b = Y.
    for (int a = 0; a < sizeX; ++a) {
        for (int b = 0; b < sizeY; ++b) {
            bool lastFull = false;
            for (int c = 0; c <= sizeZ; ++c) {
                bool full = c != sizeZ && isFull(a, b, c);
                if (!lastFull && full) faceUpdate(Direction::NORTH, a, b, c);
                if (lastFull && !full) faceUpdate(Direction::SOUTH, a, b, c - 1);
                lastFull = full;
            }
        }
    }
    // Pass 2: FORWARD - c axis Y (down/up faces), a = Z, b = X.
    for (int a = 0; a < sizeZ; ++a) {
        for (int b = 0; b < sizeX; ++b) {
            bool lastFull = false;
            for (int c = 0; c <= sizeY; ++c) {
                bool full = c != sizeY && isFull(b, c, a);
                if (!lastFull && full) faceUpdate(Direction::DOWN, b, c, a);
                if (lastFull && !full) faceUpdate(Direction::UP, b, c - 1, a);
                lastFull = full;
            }
        }
    }
    // Pass 3: BACKWARD - c axis X (west/east faces), a = Y, b = Z.
    for (int a = 0; a < sizeY; ++a) {
        for (int b = 0; b < sizeZ; ++b) {
            bool lastFull = false;
            for (int c = 0; c <= sizeX; ++c) {
                bool full = c != sizeX && isFull(c, a, b);
                if (!lastFull && full) faceUpdate(Direction::WEST, c, a, b);
                if (lastFull && !full) faceUpdate(Direction::EAST, c - 1, a, b);
                lastFull = full;
            }
        }
    }

    // Reference: per placed pos - Block.updateFromNeighbourShapes over
    // UPDATE_SHAPE_ORDER [W,E,N,S,DOWN,UP].
    static const Direction kUpdateOrder[6] = {
        Direction::WEST, Direction::EAST, Direction::NORTH,
        Direction::SOUTH, Direction::DOWN, Direction::UP};
    for (const core::BlockPos& pos : placedPositions) {
        BlockState* state = level->getBlockState(pos);
        BlockState* newState = state;
        for (Direction dir : kUpdateOrder) {
            core::BlockPos neighborPos = shape_updates::relative(pos, dir);
            newState = shape_updates::updateShape(
                newState, level, pos, dir, neighborPos,
                level->getBlockState(neighborPos));
        }
        if (state != newState) level->setBlock(pos, newState, 2 | 16);
    }
    return true;
}

} // namespace TemplateEngine
} // namespace structure
} // namespace levelgen
} // namespace minecraft
