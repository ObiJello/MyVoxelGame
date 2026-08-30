#include "util/TerrainProfiling.h"
#include "levelgen/structure/JigsawTemplates.h"

#include "math/Mth.h"
#include "nbt/AllTags.h"
#include "nbt/NbtIo.h"
#include "random/LegacyRandomSource.h"

#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <unordered_map>

// Reference: StructureTemplate.java getJigsaws / Palette.jigsaws /
// JigsawBlockInfo.of + StructurePlaceSettings.getRandomPalette (positional
// LegacyRandomSource(Mth.getSeed(pos)).nextInt(paletteCount)).

namespace minecraft {
namespace levelgen {
namespace structure {

int oppositeDir(int d) {
    static const int opp[6] = {D_UP, D_DOWN, D_SOUTH, D_NORTH, D_EAST, D_WEST};
    return opp[d];
}

// Y-rotation: NORTH->EAST->SOUTH->WEST->NORTH per CW90 step; DOWN/UP fixed.
int rotateDirY(int d, int rotation) {
    if (d == D_DOWN || d == D_UP) return d;
    static const int cw[6] = {D_DOWN, D_UP, D_EAST, D_WEST, D_NORTH, D_SOUTH};
    for (int i = 0; i < (rotation & 3); ++i) d = cw[d];
    return d;
}

int dirStepX(int d) { return d == D_EAST ? 1 : (d == D_WEST ? -1 : 0); }
int dirStepY(int d) { return d == D_UP ? 1 : (d == D_DOWN ? -1 : 0); }
int dirStepZ(int d) { return d == D_SOUTH ? 1 : (d == D_NORTH ? -1 : 0); }

namespace {

namespace fs = std::filesystem;

fs::path jigsawDataRoot() {
    if (const char* env = std::getenv("MC_DATA_ROOT")) return fs::path(env);
    fs::path current = fs::current_path();
    while (!current.empty()) {
        fs::path candidate = current / "data";
        if (fs::exists(candidate) && fs::is_directory(candidate)) return candidate;
        if (current == current.root_path()) break;
        current = current.parent_path();
    }
    throw std::runtime_error("Data root not found for jigsaw templates");
}

bool parseOrientation(const std::string& value, int& front, int& top) {
    struct Entry { const char* name; int front, top; };
    static const Entry entries[12] = {
        {"down_east", D_DOWN, D_EAST}, {"down_north", D_DOWN, D_NORTH},
        {"down_south", D_DOWN, D_SOUTH}, {"down_west", D_DOWN, D_WEST},
        {"up_east", D_UP, D_EAST}, {"up_north", D_UP, D_NORTH},
        {"up_south", D_UP, D_SOUTH}, {"up_west", D_UP, D_WEST},
        {"west_up", D_WEST, D_UP}, {"east_up", D_EAST, D_UP},
        {"north_up", D_NORTH, D_UP}, {"south_up", D_SOUTH, D_UP},
    };
    for (const auto& e : entries) {
        if (value == e.name) { front = e.front; top = e.top; return true; }
    }
    return false;
}

std::string normalizeId(const std::string& id) {
    return id.find(':') != std::string::npos ? id : "minecraft:" + id;
}

std::string tagString(nbt::CompoundTag* tag, const char* key, const char* fallback) {
    if (tag == nullptr) return fallback;
    return tag->getStringOr(key, fallback);
}

// Parse one palette ListTag -> map of state-index -> (front, top) for jigsaws.
void collectJigsawStates(nbt::ListTag* palette,
                         std::unordered_map<int, std::pair<int, int>>& out) {
    for (size_t i = 0; i < palette->size(); ++i) {
        auto* entry = static_cast<nbt::CompoundTag*>(palette->get(i));
        if (entry->getStringOr("Name", "") != "minecraft:jigsaw") continue;
        int front = D_NORTH, top = D_UP;
        nbt::CompoundTag* props = entry->getCompoundPtr("Properties");
        if (props != nullptr) {
            std::string orientation = props->getStringOr("orientation", "");
            if (!orientation.empty()) parseOrientation(orientation, front, top);
        }
        out.emplace(static_cast<int>(i), std::make_pair(front, top));
    }
}

const JigsawTemplateData& load(const std::string& templateId) {
    static std::mutex s_mutex;
    static std::unordered_map<std::string, JigsawTemplateData> s_cache;
    std::lock_guard<std::mutex> lock(s_mutex);
    auto it = s_cache.find(templateId);
    if (it != s_cache.end()) return it->second;

    std::string id = normalizeId(templateId);
    size_t colon = id.find(':');
    fs::path file = jigsawDataRoot() / id.substr(0, colon) / "structure"
                  / (id.substr(colon + 1) + ".nbt");
    if (!fs::exists(file)) {
        // Reference: StructureTemplateManager.getOrCreate - a MISSING template
        // silently becomes an empty one (vanilla's own datapack references
        // nonexistent templates, e.g. ancient_city/walls/
        // intact_horizontal_wall_stairs_5). Empty: size 0, no palettes ->
        // getJigsaws yields nothing (and no palette draw), bbox = [pos-1, pos].
        JigsawTemplateData emptyData;
        emptyData.sizeX = 0;
        emptyData.sizeY = 0;
        emptyData.sizeZ = 0;
        return s_cache.emplace(templateId, std::move(emptyData)).first->second;
    }
    TERRAIN_ZONE_N("Tmpl.LoadJigsaw");
    TERRAIN_ZONE_TEXT(id.c_str(), id.size());
    auto root = nbt::NbtIo::readCompressedFromFile(file.string());
    if (!root) throw std::runtime_error("Cannot read template: " + file.string());

    JigsawTemplateData data;
    nbt::ListTag* sizeList = root->getListPtr("size");
    if (sizeList == nullptr || sizeList->size() != 3) {
        throw std::runtime_error("Template missing size: " + id);
    }
    data.sizeX = static_cast<nbt::IntTag*>(sizeList->get(0))->getValue();
    data.sizeY = static_cast<nbt::IntTag*>(sizeList->get(1))->getValue();
    data.sizeZ = static_cast<nbt::IntTag*>(sizeList->get(2))->getValue();

    // Palettes: either "palette" (one) or "palettes" (several, all sharing the
    // same "blocks" list with per-palette state meanings).
    std::vector<std::unordered_map<int, std::pair<int, int>>> jigsawStates;
    if (nbt::ListTag* single = root->getListPtr("palette")) {
        jigsawStates.emplace_back();
        collectJigsawStates(single, jigsawStates.back());
    } else if (nbt::ListTag* multi = root->getListPtr("palettes")) {
        for (size_t i = 0; i < multi->size(); ++i) {
            jigsawStates.emplace_back();
            collectJigsawStates(static_cast<nbt::ListTag*>(multi->get(i)), jigsawStates.back());
        }
    } else {
        throw std::runtime_error("Template missing palette: " + id);
    }

    data.jigsawsPerPalette.resize(jigsawStates.size());
    nbt::ListTag* blocks = root->getListPtr("blocks");
    if (blocks != nullptr) {
        for (size_t bi = 0; bi < blocks->size(); ++bi) {
            auto* block = static_cast<nbt::CompoundTag*>(blocks->get(bi));
            int state = block->getIntOr("state", -1);
            nbt::ListTag* pos = block->getListPtr("pos");
            nbt::CompoundTag* nbtTag = block->getCompoundPtr("nbt");
            for (size_t pi = 0; pi < jigsawStates.size(); ++pi) {
                auto stateIt = jigsawStates[pi].find(state);
                if (stateIt == jigsawStates[pi].end()) continue;
                JigsawBlockData jigsaw;
                jigsaw.x = static_cast<nbt::IntTag*>(pos->get(0))->getValue();
                jigsaw.y = static_cast<nbt::IntTag*>(pos->get(1))->getValue();
                jigsaw.z = static_cast<nbt::IntTag*>(pos->get(2))->getValue();
                jigsaw.front = stateIt->second.first;
                jigsaw.top = stateIt->second.second;
                jigsaw.name = normalizeId(tagString(nbtTag, "name", "minecraft:empty"));
                jigsaw.pool = normalizeId(tagString(nbtTag, "pool", "minecraft:empty"));
                jigsaw.target = normalizeId(tagString(nbtTag, "target", "minecraft:empty"));
                // Reference: getJointType - nbt "joint" else default from
                // orientation (horizontal front -> aligned, else rollable).
                std::string joint = tagString(nbtTag, "joint", "");
                if (joint == "rollable") jigsaw.rollable = true;
                else if (joint == "aligned") jigsaw.rollable = false;
                else jigsaw.rollable = !(jigsaw.front >= D_NORTH);  // vertical front -> rollable
                jigsaw.placementPriority = nbtTag ? nbtTag->getIntOr("placement_priority", 0) : 0;
                jigsaw.selectionPriority = nbtTag ? nbtTag->getIntOr("selection_priority", 0) : 0;
                data.jigsawsPerPalette[pi].push_back(std::move(jigsaw));
            }
        }
    }
    return s_cache.emplace(templateId, std::move(data)).first->second;
}

// Pivot-ZERO transform of a local position.
void transformZero(int32_t x, int32_t y, int32_t z, int rotation,
                   int32_t& ox, int32_t& oy, int32_t& oz) {
    switch (rotation & 3) {
        case 1: ox = -z; oy = y; oz = x; break;   // CW90
        case 2: ox = -x; oy = y; oz = -z; break;  // CW180
        case 3: ox = z; oy = y; oz = -x; break;   // CCW90
        default: ox = x; oy = y; oz = z; break;
    }
}

} // namespace

namespace JigsawTemplates {

const JigsawTemplateData& get(const std::string& templateId) {
    return load(templateId);
}

std::vector<PlacedJigsaw> getJigsaws(const std::string& templateId,
                                     int32_t posX, int32_t posY, int32_t posZ, int rotation) {
    const JigsawTemplateData& data = load(templateId);
    // Reference: StructurePlaceSettings.getRandomPalette - positional random,
    // separate from the worldgen stream.
    int paletteCount = static_cast<int>(data.jigsawsPerPalette.size());
    int paletteIndex = 0;
    if (paletteCount > 0) {
        LegacyRandomSource paletteRandom(Mth::getSeed(posX, posY, posZ));
        paletteIndex = paletteRandom.nextInt(paletteCount);
    }
    std::vector<PlacedJigsaw> result;
    if (paletteCount == 0) return result;
    for (const JigsawBlockData& jigsaw : data.jigsawsPerPalette[static_cast<size_t>(paletteIndex)]) {
        PlacedJigsaw placed;
        int32_t tx, ty, tz;
        transformZero(jigsaw.x, jigsaw.y, jigsaw.z, rotation, tx, ty, tz);
        placed.x = tx + posX;
        placed.y = ty + posY;
        placed.z = tz + posZ;
        placed.front = rotateDirY(jigsaw.front, rotation);
        placed.top = rotateDirY(jigsaw.top, rotation);
        placed.name = jigsaw.name;
        placed.pool = jigsaw.pool;
        placed.target = jigsaw.target;
        placed.rollable = jigsaw.rollable;
        placed.placementPriority = jigsaw.placementPriority;
        placed.selectionPriority = jigsaw.selectionPriority;
        result.push_back(std::move(placed));
    }
    return result;
}

BoundingBox elementBoundingBox(const std::string& templateId,
                               int32_t posX, int32_t posY, int32_t posZ, int rotation) {
    const JigsawTemplateData& data = load(templateId);
    int32_t x1, y1, z1, x2, y2, z2;
    transformZero(0, 0, 0, rotation, x1, y1, z1);
    transformZero(data.sizeX - 1, data.sizeY - 1, data.sizeZ - 1, rotation, x2, y2, z2);
    BoundingBox box(std::min(x1, x2), std::min(y1, y2), std::min(z1, z2),
                    std::max(x1, x2), std::max(y1, y2), std::max(z1, z2));
    box.move(posX, posY, posZ);
    return box;
}

} // namespace JigsawTemplates

} // namespace structure
} // namespace levelgen
} // namespace minecraft
