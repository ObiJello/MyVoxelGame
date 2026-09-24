#include "levelgen/structure/twilight/TwilightTemplatePieces.h"

#include "levelgen/structure/OrientedPieceBehavior.h"
#include "levelgen/structure/Structures.h"
#include "levelgen/ChunkGenerator.h"
#include "levelgen/Heightmap.h"
#include "levelgen/TwilightBlocks.h"
#include "levelgen/WorldGenLevel.h"
#include "levelgen/WorldgenRandom.h"
#include "levelgen/blockpredicates/BlockPredicate.h"
#include "math/Mth.h"
#include "nbt/AllTags.h"
#include "nbt/NbtIo.h"
#include "world/IChunk.h"
#include "world/level/block/Block.h"
#include "world/level/block/Blocks.h"
#include "world/level/block/state/BlockState.h"
#include "external/json.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace minecraft {
namespace levelgen {
namespace structure {
namespace twilight_template {

namespace {

namespace fs = std::filesystem;
using world::level::block::Blocks;
using core::Direction;

void logOnce(const std::string& key, const std::string& message) {
    static std::mutex s_mutex;
    static std::set<std::string> s_logged;
    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_logged.insert(key).second) {
        std::cerr << "[TwilightTemplatePieces] " << message << std::endl;
    }
}

// Data root discovery shared by the structure loaders: MC_DATA_ROOT, else
// the nearest data/ directory above the working directory.
fs::path dataRoot() {
    if (const char* env = std::getenv("MC_DATA_ROOT")) return fs::path(env);
    fs::path current = fs::current_path();
    while (!current.empty()) {
        fs::path candidate = current / "data";
        if (fs::exists(candidate) && fs::is_directory(candidate)) return candidate;
        if (current == current.root_path()) break;
        current = current.parent_path();
    }
    throw std::runtime_error("Data root not found for Twilight Forest templates");
}

std::string normalizeId(const std::string& id) {
    return id.find(':') != std::string::npos ? id : "minecraft:" + id;
}

// Java long multiplication (two's-complement wrap, no UB).
int64_t mulWrap(int64_t a, int64_t b) {
    return static_cast<int64_t>(static_cast<uint64_t>(a) * static_cast<uint64_t>(b));
}

// StructurePlaceSettings.getRandom(pos) then the TF re-seed
// random.setSeed(random.nextLong() * factor).
LegacyRandomSource processorRandom(const core::BlockPos& pos, int64_t factor) {
    LegacyRandomSource random(Mth::getSeed(pos.getX(), pos.getY(), pos.getZ()));
    random.setSeed(mulWrap(random.nextLong(), factor));
    return random;
}

bool isBlock(const BlockState* state, const char* id) {
    return state != nullptr && state->getBlock() != nullptr
        && state->getBlock()->getIdentifier() == id;
}

bool endsWith(const std::string& s, const char* suffix) {
    const size_t n = std::char_traits<char>::length(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

BlockState* defaultOf(const char* id) {
    return Blocks::getDefaultState(id);
}

// Java's BoundingBox constructor: inverted bounds are swapped into order.
BoundingBox jbox(int minX, int minY, int minZ, int maxX, int maxY, int maxZ) {
    return BoundingBox(std::min(minX, maxX), std::min(minY, maxY), std::min(minZ, maxZ),
                       std::max(minX, maxX), std::max(minY, maxY), std::max(minZ, maxZ));
}

std::optional<FrontAndTop> parseFrontAndTop(const std::string& value) {
    const size_t underscore = value.find('_');
    if (underscore == std::string::npos) return std::nullopt;
    std::optional<Direction> front = directionFromName(value.substr(0, underscore));
    std::optional<Direction> top = directionFromName(value.substr(underscore + 1));
    if (!front || !top) return std::nullopt;
    return FrontAndTop{*front, *top};
}

} // namespace

// ============================================================================
// Rotation / direction helpers
// ============================================================================

const char* rotationName(int rotation) {
    static const char* const kNames[4] = {"NONE", "CLOCKWISE_90", "CLOCKWISE_180",
                                          "COUNTERCLOCKWISE_90"};
    return kNames[rotation & 3];
}

core::Direction clockWise(core::Direction dir) {
    switch (dir) {
        case Direction::NORTH: return Direction::EAST;
        case Direction::EAST: return Direction::SOUTH;
        case Direction::SOUTH: return Direction::WEST;
        case Direction::WEST: return Direction::NORTH;
        default: return dir;
    }
}

core::Direction counterClockWise(core::Direction dir) {
    switch (dir) {
        case Direction::NORTH: return Direction::WEST;
        case Direction::WEST: return Direction::SOUTH;
        case Direction::SOUTH: return Direction::EAST;
        case Direction::EAST: return Direction::NORTH;
        default: return dir;
    }
}

bool isVertical(core::Direction dir) {
    return dir == Direction::UP || dir == Direction::DOWN;
}

core::Direction rotate(int rotation, core::Direction dir) {
    if (isVertical(dir)) return dir;
    switch (rotation & 3) {
        case ROT_CW90: return clockWise(dir);
        case ROT_CW180: return core::getOpposite(dir);
        case ROT_CCW90: return counterClockWise(dir);
        default: return dir;
    }
}

int randomRotation(LegacyRandomSource& random) {
    return random.nextInt(4);
}

int relativeRotation(core::Direction original, core::Direction destination) {
    // Reference: RotationUtil.getRelativeRotation.
    switch (original) {
        case Direction::SOUTH:
            switch (destination) {
                case Direction::NORTH: return ROT_CW180;
                case Direction::WEST: return ROT_CW90;
                case Direction::EAST: return ROT_CCW90;
                default: return ROT_NONE;
            }
        case Direction::EAST:
            switch (destination) {
                case Direction::WEST: return ROT_CW180;
                case Direction::SOUTH: return ROT_CW90;
                case Direction::NORTH: return ROT_CCW90;
                default: return ROT_NONE;
            }
        case Direction::WEST:
            switch (destination) {
                case Direction::EAST: return ROT_CW180;
                case Direction::NORTH: return ROT_CW90;
                case Direction::SOUTH: return ROT_CCW90;
                default: return ROT_NONE;
            }
        default:
            switch (destination) {
                case Direction::SOUTH: return ROT_CW180;
                case Direction::EAST: return ROT_CW90;
                case Direction::WEST: return ROT_CCW90;
                default: return ROT_NONE;
            }
    }
}

std::optional<core::Direction> directionFromName(const std::string& name) {
    if (name == "down") return Direction::DOWN;
    if (name == "up") return Direction::UP;
    if (name == "north") return Direction::NORTH;
    if (name == "south") return Direction::SOUTH;
    if (name == "west") return Direction::WEST;
    if (name == "east") return Direction::EAST;
    return std::nullopt;
}

// ============================================================================
// BoundingBox helpers
// ============================================================================

BoundingBox inflatedBy(const BoundingBox& box, int amount) {
    return inflatedBy(box, amount, amount, amount);
}

BoundingBox inflatedBy(const BoundingBox& box, int x, int y, int z) {
    return jbox(box.minX - x, box.minY - y, box.minZ - z,
                       box.maxX + x, box.maxY + y, box.maxZ + z);
}

BoundingBox moved(const BoundingBox& box, int dx, int dy, int dz) {
    BoundingBox out = box;
    out.move(dx, dy, dz);
    return out;
}

BoundingBox cloneWithAdjustments(const BoundingBox& box, int x1, int y1, int z1,
                                 int x2, int y2, int z2) {
    return jbox(box.minX + x1, box.minY + y1, box.minZ + z1,
                       box.maxX + x2, box.maxY + y2, box.maxZ + z2);
}

BoundingBox extrusionFrom(const BoundingBox& box, core::Direction direction, int length) {
    switch (direction) {
        case Direction::WEST:
            return jbox(box.minX - length, box.minY, box.minZ, box.minX - 1, box.maxY, box.maxZ);
        case Direction::EAST:
            return jbox(box.maxX + 1, box.minY, box.minZ, box.maxX + length, box.maxY, box.maxZ);
        case Direction::DOWN:
            return jbox(box.minX, box.minY - length, box.minZ, box.maxX, box.minY - 1, box.maxZ);
        case Direction::UP:
            return jbox(box.minX, box.maxY + 1, box.minZ, box.maxX, box.maxY + length, box.maxZ);
        case Direction::NORTH:
            return jbox(box.minX, box.minY, box.minZ - length, box.maxX, box.maxY, box.minZ - 1);
        case Direction::SOUTH:
        default:
            return jbox(box.minX, box.minY, box.maxZ + 1, box.maxX, box.maxY, box.maxZ + length);
    }
}

int span(const BoundingBox& box, core::Axis axis) {
    switch (axis) {
        case core::Axis::X: return box.getXSpan();
        case core::Axis::Y: return box.getYSpan();
        default: return box.getZSpan();
    }
}

BoundingBox safeRetract(const BoundingBox& box, core::Direction direction, int length) {
    if (span(box, core::getAxis(direction)) <= length) return box;
    switch (direction) {
        case Direction::WEST: return cloneWithAdjustments(box, length, 0, 0, 0, 0, 0);
        case Direction::EAST: return cloneWithAdjustments(box, 0, 0, 0, -length, 0, 0);
        case Direction::DOWN: return cloneWithAdjustments(box, 0, length, 0, 0, 0, 0);
        case Direction::UP: return cloneWithAdjustments(box, 0, 0, 0, 0, -length, 0);
        case Direction::NORTH: return cloneWithAdjustments(box, 0, 0, length, 0, 0, 0);
        case Direction::SOUTH:
        default: return cloneWithAdjustments(box, 0, 0, 0, 0, 0, -length);
    }
}

std::optional<BoundingBox> intersection(const BoundingBox& a, const BoundingBox& b) {
    if (!a.intersects(b)) return std::nullopt;
    return jbox(std::max(a.minX, b.minX), std::max(a.minY, b.minY), std::max(a.minZ, b.minZ),
                       std::min(a.maxX, b.maxX), std::min(a.maxY, b.maxY), std::min(a.maxZ, b.maxZ));
}

BoundingBox fromCorners(const core::BlockPos& a, const core::BlockPos& b) {
    return jbox(std::min(a.getX(), b.getX()), std::min(a.getY(), b.getY()),
                       std::min(a.getZ(), b.getZ()), std::max(a.getX(), b.getX()),
                       std::max(a.getY(), b.getY()), std::max(a.getZ(), b.getZ()));
}

core::BlockPos center(const BoundingBox& box) {
    return core::BlockPos(box.centerX(), box.centerY(), box.centerZ());
}

core::BlockPos bottomCenterOf(const BoundingBox& box) {
    return core::BlockPos(box.minX + (box.maxX - box.minX + 1) / 2, box.minY,
                          box.minZ + (box.maxZ - box.minZ + 1) / 2);
}

core::BlockPos clampedInside(const BoundingBox& box, const core::BlockPos& pos) {
    return core::BlockPos(std::clamp(pos.getX(), box.minX, box.maxX),
                          std::clamp(pos.getY(), box.minY, box.maxY),
                          std::clamp(pos.getZ(), box.minZ, box.maxZ));
}

int greatestAxalDistance(const BoundingBox& box, const core::BlockPos& pos) {
    core::BlockPos c = clampedInside(box, pos);
    return std::max(std::max(std::abs(c.getX() - pos.getX()), std::abs(c.getY() - pos.getY())),
                    std::abs(c.getZ() - pos.getZ()));
}

int horizontalManhattanDistance(const BoundingBox& box, const core::BlockPos& pos) {
    const int xClamped = std::clamp(pos.getX(), box.minX, box.maxX);
    const int zClamped = std::clamp(pos.getZ(), box.minZ, box.maxZ);
    return std::abs(xClamped - pos.getX()) + std::abs(zClamped - pos.getZ());
}

BoundingBox wrappedCoordinates(int padding, const core::BlockPos& a, const core::BlockPos& b) {
    return jbox(std::min(a.getX(), b.getX()) - padding, std::min(a.getY(), b.getY()) - padding,
                       std::min(a.getZ(), b.getZ()) - padding, std::max(a.getX(), b.getX()) + padding,
                       std::max(a.getY(), b.getY()) + padding, std::max(a.getZ(), b.getZ()) + padding);
}

BoundingBox setY(const BoundingBox& box, int minY, int maxY) {
    return jbox(box.minX, minY, box.minZ, box.maxX, maxY, box.maxZ);
}

int lerpDiscrete(float alpha, int p0, int p1) {
    // Reference: Mth.lerpDiscrete - float multiply, Mth.floor(float).
    const int delta = p1 - p0;
    const float scaled = alpha * static_cast<float>(delta - 1);
    int floored = static_cast<int>(scaled);
    if (scaled < static_cast<float>(floored)) --floored;
    return p0 + floored + (alpha > 0.0f ? 1 : 0);
}

core::BlockPos lerpPosInside(const BoundingBox& box, core::Axis axis, float delta) {
    const core::BlockPos c = center(box);
    switch (axis) {
        case core::Axis::X: return core::BlockPos(lerpDiscrete(delta, box.minX, box.maxX), c.getY(), c.getZ());
        case core::Axis::Y: return core::BlockPos(c.getX(), lerpDiscrete(delta, box.minY, box.maxY), c.getZ());
        default: return core::BlockPos(c.getX(), c.getY(), lerpDiscrete(delta, box.minZ, box.maxZ));
    }
}

// ============================================================================
// Raw templates
// ============================================================================

const RawTemplate& rawTemplate(const std::string& templateId) {
    static std::mutex s_mutex;
    static std::unordered_map<std::string, RawTemplate> s_cache;
    std::lock_guard<std::mutex> lock(s_mutex);
    auto it = s_cache.find(templateId);
    if (it != s_cache.end()) return it->second;

    const std::string id = normalizeId(templateId);
    const size_t colon = id.find(':');
    const fs::path file = dataRoot() / id.substr(0, colon) / "structure"
                        / (id.substr(colon + 1) + ".nbt");
    RawTemplate data;
    if (!fs::exists(file)) {
        return s_cache.emplace(templateId, std::move(data)).first->second;
    }
    auto root = nbt::NbtIo::readCompressedFromFile(file.string());
    if (!root) throw std::runtime_error("Cannot read template: " + file.string());
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
    }
    for (nbt::ListTag* palette : paletteTags) {
        std::vector<RawPaletteEntry> entries;
        entries.reserve(palette->size());
        for (size_t i = 0; i < palette->size(); ++i) {
            auto* entryTag = static_cast<nbt::CompoundTag*>(palette->get(i));
            RawPaletteEntry entry;
            entry.name = normalizeId(entryTag->getStringOr("Name", "minecraft:air"));
            if (nbt::CompoundTag* props = entryTag->getCompoundPtr("Properties")) {
                for (const auto& key : props->keys()) {
                    entry.properties[key] = props->getStringOr(key, "");
                }
            }
            entries.push_back(std::move(entry));
        }
        data.palettes.push_back(std::move(entries));
    }
    if (nbt::ListTag* blocks = root->getListPtr("blocks")) {
        data.blocks.reserve(blocks->size());
        for (size_t i = 0; i < blocks->size(); ++i) {
            auto* blockTag = static_cast<nbt::CompoundTag*>(blocks->get(i));
            nbt::ListTag* pos = blockTag->getListPtr("pos");
            if (pos == nullptr || pos->size() != 3) continue;
            RawBlock block;
            block.pos = core::BlockPos(static_cast<nbt::IntTag*>(pos->get(0))->getValue(),
                                       static_cast<nbt::IntTag*>(pos->get(1))->getValue(),
                                       static_cast<nbt::IntTag*>(pos->get(2))->getValue());
            block.state = blockTag->getIntOr("state", 0);
            if (nbt::CompoundTag* nbtTag = blockTag->getCompoundPtr("nbt")) {
                block.nbt = std::shared_ptr<nbt::CompoundTag>(
                    static_cast<nbt::CompoundTag*>(nbtTag->copy().release()));
            }
            data.blocks.push_back(std::move(block));
        }
    }
    return s_cache.emplace(templateId, std::move(data)).first->second;
}

std::vector<FilteredBlock> filterBlocks(const std::string& templateId,
                                        const core::BlockPos& position,
                                        const TemplatePlaceSettings& settings,
                                        const std::string& blockName, bool absolute,
                                        const BoundingBox* clip) {
    std::vector<FilteredBlock> result;
    const RawTemplate& data = rawTemplate(templateId);
    if (data.palettes.empty()) return result;
    // Reference: settings.getRandomPalette(palettes, position).
    LegacyRandomSource paletteRandom(Mth::getSeed(position.getX(), position.getY(), position.getZ()));
    const auto& palette = data.palettes[static_cast<size_t>(
        paletteRandom.nextInt(static_cast<int32_t>(data.palettes.size())))];
    const std::string wanted = normalizeId(blockName);
    for (const RawBlock& block : data.blocks) {
        if (block.state < 0 || static_cast<size_t>(block.state) >= palette.size()) continue;
        const RawPaletteEntry& entry = palette[static_cast<size_t>(block.state)];
        if (entry.name != wanted) continue;
        core::BlockPos pos = absolute
            ? TemplateEngine::calculateRelativePosition(settings, block.pos)
                  .offset(position.getX(), position.getY(), position.getZ())
            : block.pos;
        if (clip != nullptr && !clip->isInside(pos.getX(), pos.getY(), pos.getZ())) continue;
        result.push_back({pos, &entry, block.nbt.get()});
    }
    return result;
}

std::array<int, 3> templateSize(const std::string& templateId, int rotation) {
    const FullTemplateData& data = TemplateEngine::get(templateId);
    if (rotation == ROT_CW90 || rotation == ROT_CCW90) {
        return {data.sizeZ, data.sizeY, data.sizeX};
    }
    return {data.sizeX, data.sizeY, data.sizeZ};
}

BoundingBox templateBoundingBox(const std::string& templateId,
                                const TemplatePlaceSettings& settings,
                                const core::BlockPos& position) {
    // Reference: StructureTemplate.getBoundingBox(position, rotation, pivot,
    // mirror, size) - an empty template (size 0) gives [pos - 1, pos].
    const FullTemplateData& data = TemplateEngine::get(templateId);
    const core::BlockPos c1 = TemplateEngine::calculateRelativePosition(settings, core::BlockPos(0, 0, 0));
    const core::BlockPos c2 = TemplateEngine::calculateRelativePosition(
        settings, core::BlockPos(data.sizeX - 1, data.sizeY - 1, data.sizeZ - 1));
    BoundingBox box = fromCorners(c1, c2);
    box.move(position.getX(), position.getY(), position.getZ());
    return box;
}

// ============================================================================
// Blocks
// ============================================================================

Block* realTwilightBlock(const std::string& name) {
    static std::mutex s_mutex;
    static std::unordered_map<std::string, Block*> s_cache;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        auto it = s_cache.find(name);
        if (it != s_cache.end()) return it->second;
    }
    Block* block = nullptr;
    if (!twilight_blocks::isStandIn(name)) {
        const std::string resolved = twilight_blocks::resolveName(name);
        if (!resolved.empty()) block = Blocks::getBlock(resolved);
    }
    std::lock_guard<std::mutex> lock(s_mutex);
    s_cache.emplace(name, block);
    return block;
}

BlockState* transferAllStateKeys(BlockState* in, const std::string& targetName) {
    if (in == nullptr) return nullptr;
    static std::mutex s_mutex;
    static std::map<std::pair<const BlockState*, std::string>, BlockState*> s_cache;
    const auto key = std::make_pair(static_cast<const BlockState*>(in), targetName);
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        auto it = s_cache.find(key);
        if (it != s_cache.end()) return it->second;
    }
    BlockState* out = twilight_blocks::state(targetName, in->getProperties());
    if (out == nullptr) out = in;  // nothing resolves: keep the processed state
    std::lock_guard<std::mutex> lock(s_mutex);
    s_cache.emplace(key, out);
    return out;
}

BlockState* withProperty(BlockState* state, const std::string& key, const std::string& value) {
    if (state == nullptr || state->getBlock() == nullptr) return state;
    auto want = state->getProperties();
    auto it = want.find(key);
    if (it == want.end()) return state;
    if (it->second == value) return state;
    it->second = value;
    for (BlockState* candidate : state->getBlock()->getStateDefinition().getPossibleStates()) {
        if (candidate->getProperties() == want) return candidate;
    }
    return state;
}

BlockState* rotateBlockState(BlockState* state, int rotation) {
    if (state == nullptr || (rotation & 3) == ROT_NONE) return state;
    // Reference: SkullBlock/BannerBlock.rotate - ROTATION_16 turns by
    // Rotation.rotate(value, 16).
    const auto props = state->getProperties();
    auto it = props.find("rotation");
    if (it != props.end()) {
        const int value = std::atoi(it->second.c_str());
        const int turned = (value + (rotation & 3) * 4) % 16;
        return withProperty(state, "rotation", std::to_string(turned));
    }
    return state_transforms::rotateState(state, rotation & 3);
}

void removeBlock(WorldGenLevel* level, const core::BlockPos& pos) {
    // Reference: LevelAccessor.removeBlock(pos, false) - the fluid's legacy
    // block (water/lava stay, a waterlogged cell becomes a water source).
    BlockState* current = level->getBlockState(pos);
    BlockState* replacement = Blocks::AIR->defaultBlockState();
    if (current != nullptr) {
        if (isBlock(current, "minecraft:water") || isBlock(current, "minecraft:lava")) {
            replacement = current;
        } else if (current->hasWaterFluid()) {
            replacement = defaultOf("minecraft:water");
        }
    }
    if (::world::IChunk* chunk = level->getChunk(pos.getX() >> 4, pos.getZ() >> 4)) {
        chunk->removeBlockEntity(pos);
    }
    level->setBlock(pos, replacement, 3);
}

void setBlockEntity(WorldGenLevel* level, const core::BlockPos& pos, const std::string& snbt) {
    if (::world::IChunk* chunk = level->getChunk(pos.getX() >> 4, pos.getZ() >> 4)) {
        chunk->setBlockEntityNbt(pos, snbt);
    }
}

std::string lootContainerPayload(const std::string& beId, const std::string& lootTable, int64_t seed) {
    return "{LootTable:\"" + lootTable + "\",LootTableSeed:" + std::to_string(seed)
         + "l,components:{},id:\"" + beId + "\"}";
}

// ============================================================================
// Processors
// ============================================================================

Processor nagastoneVariants() {
    // Reference: NagastoneVariants.process.
    return [](const core::BlockPos& pos, BlockState* state, const core::BlockPos&,
              BlockState*, const core::BlockPos&) -> BlockState* {
        static Block* const kEtched = realTwilightBlock("twilightforest:etched_nagastone");
        static Block* const kPillar = realTwilightBlock("twilightforest:nagastone_pillar");
        static Block* const kStairsLeft = realTwilightBlock("twilightforest:nagastone_stairs_left");
        static Block* const kStairsRight = realTwilightBlock("twilightforest:nagastone_stairs_right");
        if (state == nullptr) return state;
        LegacyRandomSource random = processorRandom(pos, 5);
        Block* block = state->getBlock();
        if (kEtched != nullptr && block == kEtched && random.nextBoolean()) {
            return transferAllStateKeys(state, random.nextBoolean()
                ? "twilightforest:mossy_etched_nagastone" : "twilightforest:cracked_etched_nagastone");
        }
        if (kPillar != nullptr && block == kPillar && random.nextBoolean()) {
            return transferAllStateKeys(state, random.nextBoolean()
                ? "twilightforest:mossy_nagastone_pillar" : "twilightforest:cracked_nagastone_pillar");
        }
        if (kStairsLeft != nullptr && block == kStairsLeft && random.nextBoolean()) {
            return transferAllStateKeys(state, random.nextBoolean()
                ? "twilightforest:mossy_nagastone_stairs_left" : "twilightforest:cracked_nagastone_stairs_left");
        }
        if (kStairsRight != nullptr && block == kStairsRight && random.nextBoolean()) {
            return transferAllStateKeys(state, random.nextBoolean()
                ? "twilightforest:mossy_nagastone_stairs_right" : "twilightforest:cracked_nagastone_stairs_right");
        }
        return state;
    };
}

Processor stoneBricksVariants() {
    // Reference: StoneBricksVariants.process.
    return [](const core::BlockPos& pos, BlockState* state, const core::BlockPos&,
              BlockState*, const core::BlockPos&) -> BlockState* {
        if (state == nullptr) return state;
        LegacyRandomSource random = processorRandom(pos, 3);
        if (isBlock(state, "minecraft:stone_bricks") && random.nextBoolean()) {
            return random.nextBoolean() ? defaultOf("minecraft:mossy_stone_bricks")
                                        : defaultOf("minecraft:cracked_stone_bricks");
        }
        if (isBlock(state, "minecraft:stone_brick_stairs") && random.nextBoolean()) {
            return transferAllStateKeys(state, "minecraft:mossy_stone_brick_stairs");
        }
        if (isBlock(state, "minecraft:stone_brick_slab") && random.nextBoolean()) {
            return transferAllStateKeys(state, "minecraft:mossy_stone_brick_slab");
        }
        if (isBlock(state, "minecraft:stone_brick_wall") && random.nextBoolean()) {
            return transferAllStateKeys(state, "minecraft:mossy_stone_brick_wall");
        }
        return state;
    };
}

Processor smoothStoneVariants() {
    // Reference: SmoothStoneVariants.process.
    return [](const core::BlockPos& pos, BlockState* state, const core::BlockPos&,
              BlockState*, const core::BlockPos&) -> BlockState* {
        if (state == nullptr) return state;
        LegacyRandomSource random = processorRandom(pos, 4);
        if (isBlock(state, "minecraft:smooth_stone_slab") && random.nextBoolean()) {
            return transferAllStateKeys(state, "minecraft:cobblestone_slab");
        }
        if (isBlock(state, "minecraft:smooth_stone") && random.nextBoolean()) {
            return defaultOf("minecraft:cobblestone");
        }
        return state;
    };
}

Processor cobbleVariants() {
    // Reference: CobbleVariants.process.
    return [](const core::BlockPos& pos, BlockState* state, const core::BlockPos&,
              BlockState*, const core::BlockPos&) -> BlockState* {
        if (state == nullptr) return state;
        LegacyRandomSource random = processorRandom(pos, 2);
        if (isBlock(state, "minecraft:cobblestone") && random.nextBoolean()) {
            return defaultOf("minecraft:mossy_cobblestone");
        }
        if (isBlock(state, "minecraft:cobblestone_stairs") && random.nextBoolean()) {
            return transferAllStateKeys(state, "minecraft:mossy_cobblestone_stairs");
        }
        if (isBlock(state, "minecraft:cobblestone_slab") && random.nextBoolean()) {
            return transferAllStateKeys(state, "minecraft:mossy_cobblestone_slab");
        }
        if (isBlock(state, "minecraft:cobblestone_wall") && random.nextBoolean()) {
            return transferAllStateKeys(state, "minecraft:mossy_cobblestone_wall");
        }
        return state;
    };
}

Processor infestBlocks() {
    // Reference: InfestBlocksProcessor.process - positional random 10 above
    // the cell, re-seeded (x2); 1/12 of the convertible blocks get infested.
    return [](const core::BlockPos& pos, BlockState* state, const core::BlockPos&,
              BlockState*, const core::BlockPos&) -> BlockState* {
        if (state == nullptr || state->getBlock() == nullptr) return state;
        static const std::unordered_map<std::string, std::string> kConversions = {
            {"minecraft:stone", "minecraft:infested_stone"},
            {"minecraft:cobblestone", "minecraft:infested_cobblestone"},
            {"minecraft:stone_bricks", "minecraft:infested_stone_bricks"},
            {"minecraft:mossy_stone_bricks", "minecraft:infested_mossy_stone_bricks"},
            {"minecraft:cracked_stone_bricks", "minecraft:infested_cracked_stone_bricks"},
            {"minecraft:chiseled_stone_bricks", "minecraft:infested_chiseled_stone_bricks"},
        };
        LegacyRandomSource random = processorRandom(pos.above(10), 2);
        auto it = kConversions.find(state->getBlock()->getIdentifier());
        if (it == kConversions.end() || random.nextFloat() > 1.0f / 12.0f) return state;
        BlockState* replacement = Blocks::getDefaultState(it->second);
        return replacement != nullptr ? replacement : state;
    };
}

Processor blockRot(float integrity) {
    // Reference: BlockRotProcessor(integrity) without a rottable tag.
    return [integrity](const core::BlockPos& pos, BlockState* state, const core::BlockPos&,
                       BlockState*, const core::BlockPos&) -> BlockState* {
        LegacyRandomSource random(Mth::getSeed(pos.getX(), pos.getY(), pos.getZ()));
        return random.nextFloat() <= integrity ? state : nullptr;
    };
}

Processor ignoreAir() {
    // Reference: BlockIgnoreProcessor.AIR - exactly minecraft:air.
    return [](const core::BlockPos&, BlockState* state, const core::BlockPos&,
              BlockState*, const core::BlockPos&) -> BlockState* {
        return (state != nullptr && state->getBlock() == Blocks::AIR) ? nullptr : state;
    };
}

Processor targetedRot(std::vector<BlockState*> blocksToRot, float integrity) {
    // Reference: TargetedRotProcessor.process - exact-state gate, then
    // BlockRotProcessor.processBlock.
    return [blocksToRot = std::move(blocksToRot), integrity](
               const core::BlockPos& pos, BlockState* state, const core::BlockPos&,
               BlockState*, const core::BlockPos&) -> BlockState* {
        if (std::find(blocksToRot.begin(), blocksToRot.end(), state) == blocksToRot.end()) {
            return state;
        }
        LegacyRandomSource random(Mth::getSeed(pos.getX(), pos.getY(), pos.getZ()));
        return random.nextFloat() <= integrity ? state : nullptr;
    };
}

Processor verticalDecay(std::vector<std::string> blockNames, float chance) {
    // Reference: VerticalDecayProcessor.process. Blocks resolve to their real
    // engine blocks only (a stand-in is not the mod's block).
    std::vector<std::pair<Block*, bool>> blocks;
    for (const std::string& name : blockNames) {
        Block* block = realTwilightBlock(name);
        if (block != nullptr) blocks.emplace_back(block, endsWith(name, "_banister"));
    }
    return [blocks = std::move(blocks), chance](const core::BlockPos& pos, BlockState* state,
                                                const core::BlockPos&, BlockState*,
                                                const core::BlockPos&) -> BlockState* {
        if (state == nullptr) return state;
        for (const auto& [block, banister] : blocks) {
            if (state->getBlock() != block) continue;
            // Banisters draw from the cell below (matching the missing block).
            const int lookDown = banister ? -1 : 0;
            LegacyRandomSource random(Mth::getSeed(pos.getX(), pos.getY() + lookDown, pos.getZ()));
            return random.nextFloat() < chance ? nullptr : state;
        }
        return state;
    };
}

Processor updateMarking(std::vector<std::string> blockNames, WorldGenLevel* level) {
    // Reference: UpdateMarkingProcessor - ProtoChunk.markPosForPostprocessing.
    std::vector<Block*> blocks;
    for (const std::string& name : blockNames) {
        Block* block = realTwilightBlock(name);
        if (block != nullptr) blocks.push_back(block);
    }
    return [blocks = std::move(blocks), level](const core::BlockPos& pos, BlockState* state,
                                               const core::BlockPos&, BlockState*,
                                               const core::BlockPos&) -> BlockState* {
        if (state == nullptr || level == nullptr) return state;
        if (std::find(blocks.begin(), blocks.end(), state->getBlock()) != blocks.end()) {
            if (::world::IChunk* chunk = level->getChunk(pos.getX() >> 4, pos.getZ() >> 4)) {
                chunk->markPosForPostprocessing(pos);
            }
        }
        return state;
    };
}

Processor courtyardTerrace(WorldGenLevel* level) {
    // Reference: CourtyardTerraceTemplateProcessor.process - sandstone slabs
    // are placeholders: a double slab becomes stone brick slab over the
    // stone-brick family, nothing over plain air, stone bricks otherwise; a
    // single slab becomes stone brick slab over anything but air.
    return [level](const core::BlockPos& pos, BlockState* state, const core::BlockPos&,
                   BlockState*, const core::BlockPos&) -> BlockState* {
        if (!isBlock(state, "minecraft:sandstone_slab")) return state;
        static BlockState* const kSandstoneDouble =
            withProperty(defaultOf("minecraft:sandstone_slab"), "type", "double");
        static BlockState* const kReplaceToSlab[5] = {
            defaultOf("minecraft:stone_bricks"), defaultOf("minecraft:mossy_stone_bricks"),
            defaultOf("minecraft:cracked_stone_bricks"), defaultOf("minecraft:stone_brick_slab"),
            defaultOf("minecraft:mossy_stone_brick_slab")};
        BlockState* stateAt = level != nullptr ? level->getBlockState(pos) : nullptr;
        if (stateAt == nullptr) stateAt = Blocks::AIR->defaultBlockState();
        if (state == kSandstoneDouble) {
            for (BlockState* candidate : kReplaceToSlab) {
                if (candidate == stateAt) return defaultOf("minecraft:stone_brick_slab");
            }
            if (stateAt->getBlock() == Blocks::AIR) return nullptr;  // not cave air
            return defaultOf("minecraft:stone_bricks");
        }
        if (stateAt->isAir()) return nullptr;
        return defaultOf("minecraft:stone_brick_slab");
    };
}

Processor softReplace(WorldGenLevel* level) {
    // Reference: SoftReplaceProcessor.process - replaceable cells
    // (canBeReplaced, #twilightforest:worldgen_replaceables =
    // #lush_ground_replaceable + #replaceable_by_trees) take the block; a
    // partial block (fence/wall/slab/stairs) yields to a full one.
    return [level](const core::BlockPos& pos, BlockState* state, const core::BlockPos&,
                   BlockState*, const core::BlockPos&) -> BlockState* {
        if (state == nullptr || level == nullptr) return state;
        BlockState* blockAt = level->getBlockState(pos);
        if (blockAt == nullptr) return state;
        const bool replaceable = blockAt->canBeReplaced()
            || blockpredicates::matchesBlockTagName(blockAt, "minecraft:lush_ground_replaceable")
            || blockpredicates::matchesBlockTagName(blockAt, "minecraft:replaceable_by_trees");
        if (replaceable) return state;
        auto isFullBlock = [](const BlockState* s) {
            const std::string& id = s->getBlock()->getIdentifier();
            return !((endsWith(id, "_fence") && !endsWith(id, "_fence_gate"))
                     || endsWith(id, "_wall") || endsWith(id, "_slab") || endsWith(id, "_stairs"));
        };
        if (!isFullBlock(blockAt) && isFullBlock(state)) return state;
        return nullptr;
    };
}

// ============================================================================
// Jigsaws
// ============================================================================

FrontAndTop rotateFrontAndTop(const FrontAndTop& orientation, int rotation) {
    return FrontAndTop{rotate(rotation, orientation.front), rotate(rotation, orientation.top)};
}

core::Direction absoluteHorizontal(const FrontAndTop& orientation) {
    return isVertical(orientation.front) ? orientation.top : orientation.front;
}

TemplatePlaceSettings JigsawPlaceContext::settings() const {
    TemplatePlaceSettings settings;
    settings.rotation = rotation;
    settings.mirror = 0;
    settings.rotationPivot = pivot;
    return settings;
}

BoundingBox JigsawPlaceContext::makeBoundingBox() const {
    return templateBoundingBox(templateLocation, settings(), templatePos);
}

const JigsawRecord* JigsawPlaceContext::findFirst(const std::string& name) const {
    for (const JigsawRecord& record : spareJigsaws) {
        if (record.name == name) return &record;
    }
    return nullptr;
}

namespace {

struct ConnectableJigsaw {
    core::BlockPos pos;          // template-local
    FrontAndTop orientation;     // raw palette orientation
    const nbt::CompoundTag* nbt;
};

// JigsawUtil.canRearrangeForConnection.
bool canBeRotatedToAlign(Direction source, Direction target) {
    const bool sourceVertical = isVertical(source);
    const bool planesMatch = sourceVertical == isVertical(target);
    if (sourceVertical) return planesMatch && core::getOpposite(source) == target;
    return planesMatch;
}

bool canRearrangeForConnection(const FrontAndTop& source, const FrontAndTop& other) {
    return canBeRotatedToAlign(source.front, other.front)
        && canBeRotatedToAlign(core::getOpposite(source.top), other.top);
}

int selectionPriority(const nbt::CompoundTag* tag) {
    return tag == nullptr ? 0 : tag->getIntOr("selection_priority", 0);
}

// JigsawRecord.fromUnconfiguredJigsaw(info, settings).
JigsawRecord configuredRecord(const ConnectableJigsaw& info, const TemplatePlaceSettings& settings) {
    JigsawRecord record;
    record.priority = selectionPriority(info.nbt);
    record.orientation = rotateFrontAndTop(info.orientation, settings.rotation);
    record.pos = TemplateEngine::calculateRelativePosition(settings, info.pos);
    record.pool = info.nbt != nullptr ? info.nbt->getStringOr("pool", "minecraft:empty") : "minecraft:empty";
    record.name = info.nbt != nullptr ? info.nbt->getStringOr("name", "") : "";
    record.target = info.nbt != nullptr ? info.nbt->getStringOr("target", "") : "";
    return record;
}

template <typename T>
void javaShuffle(std::vector<T>& list, LegacyRandomSource& random) {
    // Reference: Util.shuffle.
    for (size_t i = list.size(); i > 1; --i) {
        const size_t swapTo = static_cast<size_t>(random.nextInt(static_cast<int32_t>(i)));
        std::swap(list[i - 1], list[swapTo]);
    }
}

// JigsawUtil.readConnectableJigsaws(template, new StructurePlaceSettings(), random).
std::vector<ConnectableJigsaw> readConnectableJigsaws(const std::string& templateLocation,
                                                      LegacyRandomSource& random) {
    std::vector<ConnectableJigsaw> result;
    const RawTemplate& data = rawTemplate(templateLocation);
    if (data.palettes.empty() || (data.sizeX == 0 && data.sizeY == 0 && data.sizeZ == 0)) {
        return result;
    }
    TemplatePlaceSettings identity;
    for (const FilteredBlock& block : filterBlocks(templateLocation, core::BlockPos(0, 0, 0),
                                                   identity, "minecraft:jigsaw", true)) {
        ConnectableJigsaw jigsaw;
        jigsaw.pos = block.pos;
        jigsaw.nbt = block.nbt;
        auto it = block.entry->properties.find("orientation");
        std::optional<FrontAndTop> orientation =
            it != block.entry->properties.end() ? parseFrontAndTop(it->second) : std::nullopt;
        jigsaw.orientation = orientation.value_or(FrontAndTop{Direction::NORTH, Direction::UP});
        result.push_back(jigsaw);
    }
    javaShuffle(result, random);
    // "Stable" sort by descending selection priority.
    std::stable_sort(result.begin(), result.end(),
                     [](const ConnectableJigsaw& a, const ConnectableJigsaw& b) {
                         return selectionPriority(a.nbt) > selectionPriority(b.nbt);
                     });
    return result;
}

} // namespace

std::optional<JigsawPlaceContext> pickPlaceableJunction(const core::BlockPos& parentTemplatePos,
                                                        const core::BlockPos& sourceJigsawPos,
                                                        const FrontAndTop& sourceOrientation,
                                                        const std::string& templateLocation,
                                                        const std::string& jigsawNameLabel,
                                                        LegacyRandomSource& random) {
    if (templateLocation.empty()) return std::nullopt;
    std::vector<ConnectableJigsaw> connectables = readConnectableJigsaws(templateLocation, random);
    const core::BlockPos sourceTemplatePos = parentTemplatePos.offset(sourceJigsawPos);

    std::optional<ConnectableJigsaw> connectable;
    for (size_t i = 0; i < connectables.size(); ++i) {
        const ConnectableJigsaw& info = connectables[i];
        if (info.nbt != nullptr && info.nbt->contains("name")
            && info.nbt->getStringOr("name", "") == jigsawNameLabel
            && canRearrangeForConnection(sourceOrientation, info.orientation)) {
            connectable = info;
            connectables.erase(connectables.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }
    }
    if (!connectable) return std::nullopt;

    // generateAtJunction.
    const bool useVertical = isVertical(sourceOrientation.front);
    const int relative = useVertical
        ? relativeRotation(connectable->orientation.top, sourceOrientation.top)
        : relativeRotation(core::getOpposite(connectable->orientation.front), sourceOrientation.front);

    // getPlacement.
    const core::BlockPos otherOffset = connectable->pos;
    JigsawPlaceContext context;
    context.templatePos = sourceTemplatePos.relative(sourceOrientation.front).subtract(otherOffset);
    context.rotation = relative;
    context.pivot = otherOffset;
    context.templateLocation = templateLocation;
    const TemplatePlaceSettings settings = context.settings();
    // JigsawRecord.fromUnprocessedInfos: configure, shuffle, stable sort.
    context.spareJigsaws.reserve(connectables.size());
    for (const ConnectableJigsaw& info : connectables) {
        context.spareJigsaws.push_back(configuredRecord(info, settings));
    }
    javaShuffle(context.spareJigsaws, random);
    std::stable_sort(context.spareJigsaws.begin(), context.spareJigsaws.end(),
                     [](const JigsawRecord& a, const JigsawRecord& b) { return a.priority > b.priority; });
    context.seedJigsaw = configuredRecord(*connectable, settings);
    return context;
}

// ============================================================================
// StructureTemplateDefinitions
// ============================================================================

namespace {

struct TemplatePools {
    std::unordered_map<std::string, std::vector<PoolEntry>> pools;
    std::unordered_map<std::string, int> totalWeights;
};

TemplatePoolInstance parsePoolInstance(const nlohmann::json& value) {
    TemplatePoolInstance instance;
    if (value.is_number_integer()) {
        instance.weight = value.get<int>();
        return instance;
    }
    instance.weight = value.value("weight", 0);
    instance.terrainAdaptation = value.value("terrain_adaptation", std::string("none"));
    instance.ignoreWorldWaterlog = value.value("ignore_world_waterlog", false);
    if (value.contains("height_adjustment")) {
        const nlohmann::json& h = value.at("height_adjustment");
        instance.hasHeightAdjustment = true;
        instance.heightmap = h.value("heightmap", std::string("WORLD_SURFACE_WG"));
        instance.yOffset = h.value("y_offset", 0);
        if (h.contains("ground_junction_diff_clamp")) {
            instance.groundJunctionDiffClamp = h.at("ground_junction_diff_clamp").get<int>();
        }
    }
    instance.hasProcessors = value.contains("processors") || value.contains("randomized_processors");
    if (value.contains("marker_handlers") && value.at("marker_handlers").is_string()) {
        instance.markerHandlers = value.at("marker_handlers").get<std::string>();
    }
    if (value.contains("pool_aliases")) {
        for (auto it = value.at("pool_aliases").begin(); it != value.at("pool_aliases").end(); ++it) {
            instance.poolAliases[it.key()] = it.value().get<std::string>();
        }
    }
    return instance;
}

const TemplatePools& templatePools() {
    static std::once_flag s_once;
    static TemplatePools s_pools;
    std::call_once(s_once, [] {
        // CodecResourceReloadListener over twilight/template_definition:
        // every file is a template id -> {pool: instance}.
        const fs::path root = dataRoot() / "twilightforest" / "twilight" / "template_definition";
        std::map<std::string, std::map<std::string, TemplatePoolInstance>> raw;
        if (fs::exists(root)) {
            for (const auto& entry : fs::recursive_directory_iterator(root)) {
                if (!entry.is_regular_file() || entry.path().extension() != ".json") continue;
                fs::path relative = fs::relative(entry.path(), root);
                relative.replace_extension();
                const std::string templateName = "twilightforest:" + relative.generic_string();
                nlohmann::json json;
                try {
                    std::ifstream in(entry.path());
                    in >> json;
                } catch (const std::exception& e) {
                    throw std::runtime_error("Unparseable template definition " + entry.path().string()
                                             + ": " + e.what());
                }
                for (auto it = json.begin(); it != json.end(); ++it) {
                    raw[it.key()][templateName] = parsePoolInstance(it.value());
                }
            }
        } else {
            logOnce("no-template-definitions", "data/twilightforest/twilight/template_definition missing");
        }
        // afterApply: entries sorted by template id (Map.Entry.comparingByKey
        // over Identifier - path first, then namespace; one namespace here).
        for (auto& [poolId, templates] : raw) {
            std::vector<PoolEntry> entries;
            int total = 0;
            for (auto& [templateId, instance] : templates) {
                total += instance.weight;
                entries.push_back(PoolEntry{templateId, instance});
            }
            s_pools.totalWeights[poolId] = total;
            s_pools.pools[poolId] = std::move(entries);
        }
    });
    return s_pools;
}

} // namespace

const PoolEntry* randomPoolEntry(LegacyRandomSource& random, const std::string& poolId) {
    const TemplatePools& pools = templatePools();
    auto it = pools.pools.find(poolId);
    if (it == pools.pools.end()) return nullptr;
    const int total = pools.totalWeights.at(poolId);
    if (total <= 0) return nullptr;
    // Reference: WeightedList.getRandom - nextInt(totalWeight), cumulative.
    int selection = random.nextInt(total);
    for (const PoolEntry& entry : it->second) {
        selection -= entry.instance.weight;
        if (selection < 0) {
            if (entry.instance.hasProcessors) {
                logOnce("pool-processors:" + entry.templateId,
                        "template definition " + entry.templateId + " in " + poolId
                        + " carries processors; the TF pool-instance processors are not ported");
            }
            return &entry;
        }
    }
    return nullptr;
}

std::string randomTemplate(LegacyRandomSource& random, const std::string& poolId) {
    const PoolEntry* entry = randomPoolEntry(random, poolId);
    return entry != nullptr ? entry->templateId : std::string();
}

std::vector<std::string> shuffledSequence(LegacyRandomSource& random, const std::string& poolId) {
    std::vector<std::string> result;
    const TemplatePools& pools = templatePools();
    auto it = pools.pools.find(poolId);
    if (it == pools.pools.end()) return result;
    // Reference: getShuffledSequence - one nextDouble per entry in pool
    // order, key -ln(u) / weight, ascending.
    std::vector<std::pair<double, std::string>> sampled;
    sampled.reserve(it->second.size());
    for (const PoolEntry& entry : it->second) {
        const double rand = random.nextDouble();
        sampled.emplace_back(-std::log(rand) / static_cast<double>(entry.instance.weight),
                             entry.templateId);
    }
    std::stable_sort(sampled.begin(), sampled.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    for (auto& [key, id] : sampled) result.push_back(std::move(id));
    return result;
}

// ============================================================================
// WorldUtil.adjustForTerrain
// ============================================================================

int firstOccupiedHeight(GenerationContext& ctx, int x, int z, Heightmap::Types heightmapType) {
    // Reference: ChunkGenerator.getFirstOccupiedHeight = getBaseHeight - 1.
    return ctx.generator->getBaseHeight(x, z, heightmapType, ctx.randomState) - 1;
}

int adjustForTerrain(GenerationContext& ctx, int xMin, int zMin, int xMax, int zMax,
                     int gridLength, Heightmap::Types heightmapType) {
    const int subDivisions = gridLength - 1;
    std::vector<int> heights;
    heights.reserve(static_cast<size_t>(gridLength * gridLength));
    for (int zStep = 0; zStep <= subDivisions; ++zStep) {
        const int zPos = lerpDiscrete(static_cast<float>(zStep) / static_cast<float>(subDivisions), zMin, zMax);
        for (int xStep = 0; xStep <= subDivisions; ++xStep) {
            const int xPos = lerpDiscrete(static_cast<float>(xStep) / static_cast<float>(subDivisions), xMin, xMax);
            heights.push_back(firstOccupiedHeight(ctx, xPos, zPos, heightmapType));
        }
    }
    std::sort(heights.begin(), heights.end(), std::greater<int>());  // highest first
    double weightedSum = 0.0;
    double totalWeight = 0.0;
    for (size_t i = 0; i < heights.size(); ++i) {
        const double weight = static_cast<double>(i + 1);
        weightedSum += weight * heights[i];
        totalWeight += weight;
    }
    // Math.round(double) = floor(x + 0.5).
    return static_cast<int>(std::floor(weightedSum / totalWeight + 0.5));
}

int adjustForTerrain(GenerationContext& ctx, int xInCenterChunk, int zInCenterChunk,
                     int radiusFromCenterChunk, int gridLength) {
    const int chunkOriginX = xInCenterChunk & ~0b1111;
    const int chunkOriginZ = zInCenterChunk & ~0b1111;
    return adjustForTerrain(ctx, chunkOriginX - radiusFromCenterChunk, chunkOriginZ - radiusFromCenterChunk,
                            chunkOriginX + 15 + radiusFromCenterChunk, chunkOriginZ + 15 + radiusFromCenterChunk,
                            gridLength, Heightmap::Types::WORLD_SURFACE_WG);
}

// ============================================================================
// Piece records
// ============================================================================

StructurePieceData makePiece(const std::string& pieceType, const BoundingBox& box,
                             int rotation, int genDepth, const std::string& detail) {
    StructurePieceData piece;
    piece.pieceType = pieceType;
    piece.boundingBox = box;
    piece.rotation = rotationName(rotation);
    piece.genDepth = genDepth;
    piece.detail = detail.empty() ? "-" : detail;
    return piece;
}

void applyBeardifierModifier(StructurePieceData& piece, bool adjusts, int groundLevelDelta) {
    piece.poolElement = true;
    piece.rigidProjection = adjusts;
    piece.groundLevelDelta = adjusts ? groundLevelDelta : 0;
    piece.junctions.clear();
}

// ============================================================================
// TemplatePieceBehavior
// ============================================================================

TemplatePlaceSettings TemplatePieceBehavior::makeSettings(WorldGenLevel* level) const {
    TemplatePlaceSettings settings;
    settings.rotation = m_config.rotation;
    settings.mirror = m_config.mirror;
    settings.rotationPivot = m_config.pivot;
    settings.ignoreAir = false;
    settings.keepLiquids = m_config.keepLiquids;
    settings.knownShape = m_config.knownShape;
    settings.jigsawReplacement = m_config.jigsawReplacement;
    if (m_config.processors) m_config.processors(settings.processors, level);
    return settings;
}

void TemplatePieceBehavior::placeTemplate(WorldGenLevel* level, ChunkGenerator* generator,
                                          WorldgenRandom& random, const BoundingBox& chunkBB,
                                          const core::BlockPos& referencePos,
                                          StructurePieceData& self) {
    // Reference: TwilightTemplateStructurePiece.customPostProcess.
    const TemplatePlaceSettings settings = makeSettings(level);
    self.boundingBox = templateBoundingBox(m_config.templateId, settings, m_config.templatePosition);
    const core::BlockPos position = m_config.templatePosition.above(m_config.adjustY);
    const core::BlockPos reference = referencePos.above(m_config.adjustY);
    if (TemplateEngine::placeInWorld(level, m_config.templateId, position, reference, settings,
                                     random, chunkBB)) {
        for (const TemplateEngine::DataMarker& marker :
             TemplateEngine::dataMarkers(m_config.templateId, position, settings, chunkBB)) {
            handleDataMarker(marker.metadata, marker.pos, level, random, chunkBB, generator,
                             settings.rotation);
        }
    }
}

void TemplatePieceBehavior::postProcess(WorldGenLevel* level, ChunkGenerator* generator,
                                        WorldgenRandom& random, const BoundingBox& chunkBB,
                                        const ::world::ChunkPos& chunkPos,
                                        const core::BlockPos& referencePos,
                                        StructurePieceData& self) {
    (void)chunkPos;
    placeTemplate(level, generator, random, chunkBB, referencePos, self);
}

void DoubleTemplatePieceBehavior::postProcess(WorldGenLevel* level, ChunkGenerator* generator,
                                              WorldgenRandom& random, const BoundingBox& chunkBB,
                                              const ::world::ChunkPos& chunkPos,
                                              const core::BlockPos& referencePos,
                                              StructurePieceData& self) {
    TemplatePieceBehavior::postProcess(level, generator, random, chunkBB, chunkPos, referencePos, self);
    // Reference: TwilightDoubleTemplateStructurePiece.postProcess - the
    // overlay with its own settings (makeSettings(rotation) + processors, no
    // known shape). Java leaves the overlay settings without a bounding box,
    // i.e. unclipped; every overlay block lies inside this piece's box, each
    // chunk the box touches runs this pass, and the overlay's processors are
    // positional, so clipping each pass to its chunk yields the same blocks.
    TemplatePlaceSettings overlay;
    overlay.rotation = m_config.rotation;
    overlay.mirror = m_config.mirror;
    overlay.rotationPivot = m_config.pivot;
    overlay.knownShape = false;
    overlay.keepLiquids = true;
    if (m_overlayProcessors) m_overlayProcessors(overlay.processors, level);
    if (TemplateEngine::placeInWorld(level, m_overlayId, m_config.templatePosition, referencePos,
                                     overlay, random, chunkBB)) {
        // Data markers go to the deprecated (no-op) handleDataMarker overload;
        // jigsaw blocks become their final states.
        TemplateEngine::applyJigsawFinalStates(level, m_overlayId, m_config.templatePosition, overlay);
    }
}

// ============================================================================
// Jigsaw pieces
// ============================================================================

int JigsawPiece::firstMatchIndex(const std::function<bool(const JigsawRecord&)>& filter) const {
    for (size_t i = 0; i < context.spareJigsaws.size(); ++i) {
        if (filter(context.spareJigsaws[i])) return static_cast<int>(i);
    }
    return -1;
}

std::vector<JigsawRecord> JigsawPiece::matchSpareJigsaws(
    const std::function<bool(const JigsawRecord&)>& filter) const {
    std::vector<JigsawRecord> result;
    for (const JigsawRecord& record : context.spareJigsaws) {
        if (filter(record)) result.push_back(record);
    }
    return result;
}

JigsawPiece makeJigsawPiece(const std::string& pieceType, int genDepth,
                            const std::string& templateId, const JigsawPlaceContext& context) {
    JigsawPiece piece;
    piece.pieceType = pieceType;
    piece.templateId = templateId;
    piece.genDepth = genDepth;
    piece.context = context;
    piece.boundingBox = templateBoundingBox(templateId, context.settings(), context.templatePos);
    return piece;
}

void reseedForJigsaws(LegacyRandomSource& random, int64_t worldSeed,
                      const core::BlockPos& templatePosition) {
    const int64_t next = random.nextLong();
    random.setSeed(next ^ mulWrap(worldSeed, templatePosition.asLong()));
}

JigsawPieceBehavior::JigsawPieceBehavior(const JigsawPiece& piece, ProcessorFactory processors,
                                         bool keepLiquids)
    : TemplatePieceBehavior(Config{}), m_context(piece.context) {
    m_config.templateId = piece.templateId;
    m_config.rotation = piece.context.rotation;
    m_config.mirror = 0;
    m_config.pivot = piece.context.pivot;
    m_config.templatePosition = piece.context.templatePos;
    m_config.knownShape = true;
    m_config.keepLiquids = keepLiquids;
    m_config.jigsawReplacement = true;
    m_config.adjustY = 0;
    m_config.processors = std::move(processors);
}

} // namespace twilight_template
} // namespace structure
} // namespace levelgen
} // namespace minecraft
