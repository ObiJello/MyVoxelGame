#include "levelgen/blockpredicates/BlockPredicate.h"
#include "external/json.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace minecraft {
namespace levelgen {
namespace blockpredicates {

namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

std::string normalizeNamespacedId(const std::string& id, const std::string& defaultNamespace) {
    return id.find(':') != std::string::npos ? id : defaultNamespace + ":" + id;
}

std::optional<fs::path> findTagRoot() {
    // Explicit override first - avoids the CWD dependence entirely.
    if (const char* env = std::getenv("MC_DATA_ROOT")) {
        fs::path candidate(env);
        if (fs::exists(candidate) && fs::is_directory(candidate)) {
            return candidate;
        }
        throw std::runtime_error(
            std::string("MC_DATA_ROOT is set but is not a directory: ") + env);
    }

    fs::path current = fs::current_path();
    while (!current.empty()) {
        fs::path candidate = current / "data";
        if (fs::exists(candidate) && fs::is_directory(candidate)) {
            return candidate;
        }

        if (current == current.root_path()) {
            break;
        }
        current = current.parent_path();
    }

    return std::nullopt;
}

// One `values` entry of a tag file: MC's TagEntry — a plain string, or
// {"id": ..., "required": false} (an optional entry, skipped when what it
// names does not exist).
struct TagFileEntry {
    std::string value;
    bool required = true;
};

// The entries of one tag file, or nullopt when the file does not exist.
// Throws only for a file that exists but cannot be read or parsed.
std::optional<std::vector<TagFileEntry>> readTagFile(const fs::path& tagPath) {
    if (!fs::exists(tagPath)) {
        return std::nullopt;
    }
    std::ifstream input(tagPath);
    if (!input.is_open()) {
        throw std::runtime_error("Block tag file unreadable: " + tagPath.string());
    }
    json parsed;
    try {
        input >> parsed;
    } catch (const std::exception& e) {
        throw std::runtime_error("Block tag file unparseable: " + tagPath.string() +
                                 ": " + e.what());
    }
    std::vector<TagFileEntry> entries;
    if (!parsed.contains("values") || !parsed["values"].is_array()) {
        return entries;
    }
    for (const auto& entry : parsed["values"]) {
        if (entry.is_string()) {
            entries.push_back({entry.get<std::string>(), true});
        } else if (entry.is_object() && entry.contains("id") && entry["id"].is_string()) {
            entries.push_back({entry["id"].get<std::string>(), entry.value("required", true)});
        }
    }
    return entries;
}

class BlockTagRegistry {
public:
    const std::unordered_set<std::string>& resolve(const std::string& tag) {
        std::lock_guard<std::mutex> lock(m_mutex);
        return resolveLocked(tag);
    }

    // Tag values in FILE ORDER with nested tags expanded in place and
    // duplicates dropped (Java TagLoader collects into a LinkedHashSet, and
    // Registry.getRandomElementOf indexes that order with nextInt(size)).
    const std::vector<std::string>& resolveOrdered(const std::string& tag) {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_orderedCache.find(tag);
        if (it != m_orderedCache.end()) {
            return it->second;
        }
        resolveLocked(tag);  // ensures tag root discovery + loud failures
        std::vector<std::string> ordered;
        std::unordered_set<std::string> seen;
        collectOrderedLocked(tag, ordered, seen);
        return m_orderedCache.emplace(tag, std::move(ordered)).first->second;
    }

private:
    std::mutex m_mutex;
    std::optional<fs::path> m_tagRoot;
    std::unordered_map<std::string, std::unordered_set<std::string>> m_cache;
    std::unordered_map<std::string, std::vector<std::string>> m_orderedCache;
    std::unordered_set<std::string> m_inProgress;

    void collectOrderedLocked(const std::string& tag, std::vector<std::string>& out,
                              std::unordered_set<std::string>& seen) {
        size_t colon = tag.find(':');
        std::string nameSpace = colon == std::string::npos ? "minecraft" : tag.substr(0, colon);
        std::string pathPart = colon == std::string::npos ? tag : tag.substr(colon + 1);
        fs::path tagPath = *m_tagRoot / nameSpace / "tags" / "block" / (pathPart + ".json");

        // A missing file was already reported by resolveLocked.
        const auto entries = readTagFile(tagPath);
        if (!entries) {
            return;
        }
        for (const TagFileEntry& entry : *entries) {
            const std::string& value = entry.value;
            if (!value.empty() && value[0] == '#') {
                const std::string nested = normalizeNamespacedId(value.substr(1), nameSpace);
                if (!entry.required && !tagFileExists(nested)) continue;
                collectOrderedLocked(nested, out, seen);
            } else {
                std::string id = normalizeNamespacedId(value, nameSpace);
                if (seen.insert(id).second) {
                    out.push_back(std::move(id));
                }
            }
        }
    }

    const std::unordered_set<std::string>& resolveLocked(const std::string& tag) {
        auto it = m_cache.find(tag);
        if (it != m_cache.end()) {
            return it->second;
        }

        if (m_inProgress.find(tag) != m_inProgress.end()) {
            return m_cache.emplace(tag, std::unordered_set<std::string>{}).first->second;
        }

        if (!m_tagRoot.has_value()) {
            m_tagRoot = findTagRoot();
        }
        if (!m_tagRoot.has_value()) {
            // A silently-empty tag makes trees/features silently fail placement
            // checks - a huge, invisible parity divergence. Fail loudly instead.
            throw std::runtime_error(
                "Block tag root not found: no 'data/' directory above the current "
                "working directory and MC_DATA_ROOT is unset. Cannot resolve tag '" +
                tag + "'");
        }

        m_inProgress.insert(tag);
        auto inserted = m_cache.emplace(tag, std::unordered_set<std::string>{});
        std::unordered_set<std::string>& values = inserted.first->second;

        loadTagValuesLocked(tag, values);

        m_inProgress.erase(tag);
        return values;
    }

    fs::path tagPathFor(const std::string& tag) const {
        size_t colon = tag.find(':');
        std::string nameSpace = colon == std::string::npos ? "minecraft" : tag.substr(0, colon);
        std::string pathPart = colon == std::string::npos ? tag : tag.substr(colon + 1);
        return *m_tagRoot / nameSpace / "tags" / "block" / (pathPart + ".json");
    }

    bool tagFileExists(const std::string& tag) const {
        return fs::exists(tagPathFor(tag));
    }

    void loadTagValuesLocked(const std::string& tag, std::unordered_set<std::string>& out) {
        size_t colon = tag.find(':');
        std::string nameSpace = colon == std::string::npos ? "minecraft" : tag.substr(0, colon);
        const fs::path tagPath = tagPathFor(tag);

        const auto entries = readTagFile(tagPath);
        if (!entries) {
            // MC refuses to load a datapack whose required tag reference
            // dangles. Throwing here instead would kill the worldgen task
            // that asked, whose future then never completes, and every chunk
            // waiting on it — the whole generator — stalls for good. Report it
            // loudly, once (the empty result is cached), and read it as empty.
            std::cerr << "[BlockTags] ERROR: block tag file missing: " << tagPath.string()
                      << " - #" << tag << " reads as empty; add the tag to data/\n";
            return;
        }

        for (const TagFileEntry& entry : *entries) {
            const std::string& value = entry.value;
            if (!value.empty() && value[0] == '#') {
                const std::string nested = normalizeNamespacedId(value.substr(1), nameSpace);
                if (!entry.required && !tagFileExists(nested)) continue;
                const auto& resolved = resolveLocked(nested);
                out.insert(resolved.begin(), resolved.end());
            } else {
                out.insert(normalizeNamespacedId(value, nameSpace));
            }
        }
    }
};

BlockTagRegistry& blockTagRegistry() {
    static BlockTagRegistry registry;
    return registry;
}

} // namespace

// Static instance
TrueBlockPredicate TrueBlockPredicate::INSTANCE;

// Common predicate instances
// Reference: BlockPredicate.java lines 21-22
// 26.3: #air (air, void_air, cave_air); 26.1 matched the air block only.
std::shared_ptr<BlockPredicate> BlockPredicate::ONLY_IN_AIR_PREDICATE =
    BlockPredicate::matchesTag("minecraft:air");

std::shared_ptr<BlockPredicate> BlockPredicate::ONLY_IN_AIR_OR_WATER_PREDICATE =
    BlockPredicate::anyOf(ONLY_IN_AIR_PREDICATE, BlockPredicate::matchesBlocks("minecraft:water"));

//=============================================================================
// Static Factory Method Implementations
//=============================================================================

std::shared_ptr<BlockPredicate> BlockPredicate::allOf(
    const std::vector<std::shared_ptr<BlockPredicate>>& predicates
) {
    if (predicates.empty()) {
        return std::make_shared<TrueBlockPredicate>();
    }
    if (predicates.size() == 1) {
        return predicates[0];
    }
    return std::make_shared<AllOfPredicate>(predicates);
}

std::shared_ptr<BlockPredicate> BlockPredicate::allOf(
    std::shared_ptr<BlockPredicate> a,
    std::shared_ptr<BlockPredicate> b
) {
    return allOf(std::vector<std::shared_ptr<BlockPredicate>>{a, b});
}

std::shared_ptr<BlockPredicate> BlockPredicate::anyOf(
    const std::vector<std::shared_ptr<BlockPredicate>>& predicates
) {
    if (predicates.empty()) {
        return not_(std::make_shared<TrueBlockPredicate>()); // False predicate
    }
    if (predicates.size() == 1) {
        return predicates[0];
    }
    return std::make_shared<AnyOfPredicate>(predicates);
}

std::shared_ptr<BlockPredicate> BlockPredicate::anyOf(
    std::shared_ptr<BlockPredicate> a,
    std::shared_ptr<BlockPredicate> b
) {
    return anyOf(std::vector<std::shared_ptr<BlockPredicate>>{a, b});
}

std::shared_ptr<BlockPredicate> BlockPredicate::matchesBlocks(
    const core::Vec3i& offset,
    const std::vector<std::string>& blocks
) {
    return std::make_shared<MatchingBlocksPredicate>(offset, blocks);
}

std::shared_ptr<BlockPredicate> BlockPredicate::matchesBlocks(
    const std::vector<std::string>& blocks
) {
    return matchesBlocks(core::Vec3i::ZERO(), blocks);
}

std::shared_ptr<BlockPredicate> BlockPredicate::matchesBlocks(const std::string& block) {
    return matchesBlocks(std::vector<std::string>{block});
}

std::shared_ptr<BlockPredicate> BlockPredicate::matchesBlocks(
    const core::Vec3i& offset,
    const std::string& block
) {
    return matchesBlocks(offset, std::vector<std::string>{block});
}

std::shared_ptr<BlockPredicate> BlockPredicate::matchesTag(
    const core::Vec3i& offset,
    const std::string& tag
) {
    return std::make_shared<MatchingBlockTagPredicate>(offset, tag);
}

std::shared_ptr<BlockPredicate> BlockPredicate::matchesTag(const std::string& tag) {
    return matchesTag(core::Vec3i::ZERO(), tag);
}

std::shared_ptr<BlockPredicate> BlockPredicate::matchesFluids(
    const core::Vec3i& offset,
    const std::vector<std::string>& fluids
) {
    return std::make_shared<MatchingFluidsPredicate>(offset, fluids);
}

std::shared_ptr<BlockPredicate> BlockPredicate::matchesFluids(
    const std::vector<std::string>& fluids
) {
    return matchesFluids(core::Vec3i::ZERO(), fluids);
}

std::shared_ptr<BlockPredicate> BlockPredicate::matchesFluids(const std::string& fluid) {
    return matchesFluids(std::vector<std::string>{fluid});
}

std::shared_ptr<BlockPredicate> BlockPredicate::not_(std::shared_ptr<BlockPredicate> predicate) {
    return std::make_shared<NotPredicate>(predicate);
}

std::shared_ptr<BlockPredicate> BlockPredicate::replaceable(const core::Vec3i& offset) {
    return std::make_shared<ReplaceablePredicate>(offset);
}

std::shared_ptr<BlockPredicate> BlockPredicate::replaceable() {
    return replaceable(core::Vec3i::ZERO());
}

std::shared_ptr<BlockPredicate> BlockPredicate::wouldSurvive(
    BlockState* state,
    const core::Vec3i& offset
) {
    return std::make_shared<WouldSurvivePredicate>(offset, state);
}

std::shared_ptr<BlockPredicate> BlockPredicate::hasSturdyFace(
    const core::Vec3i& offset,
    Direction direction
) {
    return std::make_shared<HasSturdyFacePredicate>(offset, direction);
}

std::shared_ptr<BlockPredicate> BlockPredicate::hasSturdyFace(Direction direction) {
    return hasSturdyFace(core::Vec3i::ZERO(), direction);
}

std::shared_ptr<BlockPredicate> BlockPredicate::solid(const core::Vec3i& offset) {
    return std::make_shared<SolidPredicate>(offset);
}

std::shared_ptr<BlockPredicate> BlockPredicate::solid() {
    return solid(core::Vec3i::ZERO());
}

std::shared_ptr<BlockPredicate> BlockPredicate::noFluid() {
    return noFluid(core::Vec3i::ZERO());
}

std::shared_ptr<BlockPredicate> BlockPredicate::noFluid(const core::Vec3i& offset) {
    return matchesFluids(offset, std::vector<std::string>{"minecraft:empty"});
}

std::shared_ptr<BlockPredicate> BlockPredicate::insideWorld(const core::Vec3i& offset) {
    return std::make_shared<InsideWorldBoundsPredicate>(offset);
}

std::shared_ptr<BlockPredicate> BlockPredicate::alwaysTrue() {
    return std::make_shared<TrueBlockPredicate>();
}

std::shared_ptr<BlockPredicate> BlockPredicate::unobstructed(const core::Vec3i& offset) {
    return std::make_shared<UnobstructedPredicate>(offset);
}

std::shared_ptr<BlockPredicate> BlockPredicate::unobstructed() {
    return unobstructed(core::Vec3i::ZERO());
}

int32_t BlockPredicate::HeightAnchor::resolveY(const WorldGenLevel& level) const {
    // WorldGenerationContext: minY, height (getMaxY here is exclusive), sea level
    const int32_t minY = level.getMinY();
    const int32_t height = level.getMaxY() - minY;
    switch (kind) {
        case Kind::ABSOLUTE: return value;
        case Kind::ABOVE_BOTTOM: return minY + value;
        case Kind::BELOW_TOP: return height - 1 + minY - value;
        case Kind::RELATIVE_TO_SEA_LEVEL: return level.getSeaLevel() + value;
    }
    return value;
}

std::shared_ptr<BlockPredicate> BlockPredicate::heightRange(HeightAnchor minInclusive, HeightAnchor maxInclusive) {
    return std::make_shared<HeightRangePredicate>(minInclusive, maxInclusive);
}

std::shared_ptr<BlockPredicate> BlockPredicate::volumeMatch(const core::Vec3i& min, const core::Vec3i& max,
                                                            std::shared_ptr<BlockPredicate> match) {
    return std::make_shared<VolumeMatchPredicate>(min, max, std::move(match));
}

bool matchesBlockTagName(BlockState* state, const std::string& tag) {
    if (!state) {
        return false;
    }

    const auto& values = blockTagRegistry().resolve(tag);
    return values.find(state->getIdentifier()) != values.end();
}

const std::unordered_set<std::string>& blockTagValues(const std::string& tag) {
    return blockTagRegistry().resolve(tag);
}

const std::vector<std::string>& orderedBlockTagValues(const std::string& tag) {
    return blockTagRegistry().resolveOrdered(tag);
}

} // namespace blockpredicates
} // namespace levelgen
} // namespace minecraft
