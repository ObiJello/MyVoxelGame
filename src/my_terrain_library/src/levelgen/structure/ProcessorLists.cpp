#include <mutex>
#include "levelgen/structure/ProcessorLists.h"

#include "levelgen/WorldGenLevel.h"
#include "levelgen/blockpredicates/BlockPredicate.h"
#include "math/Mth.h"
#include "random/LegacyRandomSource.h"
#include "world/level/block/Blocks.h"
#include "world/level/block/state/BlockState.h"
#include "external/json.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <stdexcept>
#include <vector>

// Reference: RuleProcessor.processBlock - ONE RandomSource.create(
// Mth.getSeed(processedPos)) per block, worldState read once, rules tested
// in order with && short-circuit (input predicate draws first), FIRST match
// returns its output_state. BlockRotProcessor: fresh positional random,
// nextFloat() <= integrity keeps (drawn always when integrity < 1).

namespace minecraft {
namespace levelgen {
namespace structure {
namespace ProcessorLists {

namespace {

namespace fs = std::filesystem;
using nlohmann::json;
using world::level::block::Block;
using world::level::block::Blocks;

fs::path findDataRoot() {
    if (const char* env = std::getenv("MC_DATA_ROOT")) {
        fs::path root(env);
        if (fs::is_directory(root)) return root;
        throw std::runtime_error(std::string("MC_DATA_ROOT not a directory: ") + env);
    }
    fs::path current = fs::current_path();
    while (!current.empty()) {
        fs::path candidate = current / "data";
        if (fs::exists(candidate) && fs::is_directory(candidate)) return candidate;
        if (current == current.root_path()) break;
        current = current.parent_path();
    }
    throw std::runtime_error("Data root not found for worldgen/processor_list");
}

// Resolve {"Name": ..., "Properties": {...}} to the matching BlockState.
BlockState* resolveState(const json& stateJson) {
    std::string name = stateJson.at("Name").get<std::string>();
    if (name.find(':') == std::string::npos) name = "minecraft:" + name;
    Block* block = Blocks::getBlock(name);
    if (block == nullptr) {
        throw std::runtime_error("processor output uses unregistered block " + name);
    }
    if (!stateJson.contains("Properties")) return block->defaultBlockState();
    std::map<std::string, std::string> want;
    for (const auto& [key, value] : stateJson.at("Properties").items()) {
        want[key] = value.get<std::string>();
    }
    for (BlockState* candidate : block->getStateDefinition().getPossibleStates()) {
        auto have = candidate->getProperties();
        bool match = true;
        for (const auto& [key, value] : want) {
            auto it = have.find(key);
            if (it == have.end() || it->second != value) {
                match = false;
                break;
            }
        }
        if (match) return candidate;
    }
    throw std::runtime_error("processor output state not found for " + name);
}

// Parsed rule for the rule processor.
struct Rule {
    enum InputKind { INPUT_ALWAYS, INPUT_BLOCK, INPUT_RANDOM_BLOCK, INPUT_STATE, INPUT_TAG };
    InputKind inputKind = INPUT_ALWAYS;
    Block* inputBlock = nullptr;
    BlockState* inputState = nullptr;
    std::string inputTag;   // "#minecraft:..."-style name for matchesBlockTagName
    float probability = 1.0f;

    enum LocationKind { LOC_ALWAYS, LOC_BLOCK };
    LocationKind locationKind = LOC_ALWAYS;
    Block* locationBlock = nullptr;

    // Reference: PosRuleTest. Both concrete tests are the same shape — lerp a
    // chance between two distances and roll it — differing only in how the
    // distance is measured, so one struct carries both.
    enum PositionKind { POS_ALWAYS, POS_LINEAR, POS_AXIS_ALIGNED_LINEAR };
    PositionKind positionKind = POS_ALWAYS;
    float posMinChance = 0.0f;
    float posMaxChance = 0.0f;
    int   posMinDist = 0;
    int   posMaxDist = 0;
    // AxisAlignedLinearPosTest only. 0 = X, 1 = Y, 2 = Z; Java defaults to Y.
    int   posAxis = 1;

    BlockState* output = nullptr;
};

Rule parseRule(const json& ruleJson) {
    Rule rule;
    const json& input = ruleJson.at("input_predicate");
    std::string inputType = input.at("predicate_type").get<std::string>();
    auto blockOf = [](const json& j, const char* key) {
        std::string name = j.at(key).get<std::string>();
        if (name.find(':') == std::string::npos) name = "minecraft:" + name;
        Block* block = Blocks::getBlock(name);
        if (block == nullptr) {
            throw std::runtime_error("processor predicate uses unregistered block " + name);
        }
        return block;
    };
    if (inputType == "minecraft:always_true") {
        rule.inputKind = Rule::INPUT_ALWAYS;
    } else if (inputType == "minecraft:block_match") {
        rule.inputKind = Rule::INPUT_BLOCK;
        rule.inputBlock = blockOf(input, "block");
    } else if (inputType == "minecraft:random_block_match") {
        rule.inputKind = Rule::INPUT_RANDOM_BLOCK;
        rule.inputBlock = blockOf(input, "block");
        rule.probability = input.at("probability").get<float>();
    } else if (inputType == "minecraft:blockstate_match") {
        rule.inputKind = Rule::INPUT_STATE;
        rule.inputState = resolveState(input.at("block_state"));
    } else if (inputType == "minecraft:tag_match") {
        rule.inputKind = Rule::INPUT_TAG;
        std::string tag = input.at("tag").get<std::string>();
        if (tag.find(':') == std::string::npos) tag = "minecraft:" + tag;
        rule.inputTag = tag;
    } else {
        throw std::runtime_error("Unsupported rule input predicate " + inputType);
    }

    if (ruleJson.contains("location_predicate")) {
        const json& loc = ruleJson.at("location_predicate");
        std::string locType = loc.at("predicate_type").get<std::string>();
        if (locType == "minecraft:always_true") {
            rule.locationKind = Rule::LOC_ALWAYS;
        } else if (locType == "minecraft:block_match") {
            rule.locationKind = Rule::LOC_BLOCK;
            rule.locationBlock = blockOf(loc, "block");
        } else {
            throw std::runtime_error("Unsupported rule location predicate " + locType);
        }
    }
    if (ruleJson.contains("position_predicate")) {
        const json& pos = ruleJson.at("position_predicate");
        std::string posType = pos.at("predicate_type").get<std::string>();
        // Every field is optional in Java's codec (`.orElse(...)`), so a
        // missing one is a default rather than malformed data.
        auto f = [&pos](const char* key, float fallback) {
            return pos.contains(key) ? pos.at(key).get<float>() : fallback;
        };
        auto i = [&pos](const char* key, int fallback) {
            return pos.contains(key) ? pos.at(key).get<int>() : fallback;
        };
        if (posType == "minecraft:always_true") {
            rule.positionKind = Rule::POS_ALWAYS;
        } else if (posType == "minecraft:linear_pos"
                   || posType == "minecraft:axis_aligned_linear_pos") {
            rule.positionKind = (posType == "minecraft:linear_pos")
                                    ? Rule::POS_LINEAR
                                    : Rule::POS_AXIS_ALIGNED_LINEAR;
            rule.posMinChance = f("min_chance", 0.0f);
            rule.posMaxChance = f("max_chance", 0.0f);
            rule.posMinDist   = i("min_dist", 0);
            rule.posMaxDist   = i("max_dist", 0);
            if (rule.posMinDist >= rule.posMaxDist) {
                // Java throws from the constructor for the same reason: the
                // inverse lerp would divide by zero or run backwards.
                throw std::runtime_error("Invalid range in " + posType + ": ["
                                         + std::to_string(rule.posMinDist) + ","
                                         + std::to_string(rule.posMaxDist) + "]");
            }
            if (rule.positionKind == Rule::POS_AXIS_ALIGNED_LINEAR) {
                const std::string axis =
                    pos.contains("axis") ? pos.at("axis").get<std::string>() : "y";
                rule.posAxis = (axis == "x") ? 0 : (axis == "z") ? 2 : 1;
            }
        } else {
            throw std::runtime_error("Unsupported rule position predicate " + posType);
        }
    }
    if (ruleJson.contains("output_nbt") || ruleJson.contains("block_entity_modifier")) {
        // No vanilla overworld processor list uses these (survey 2026-08-13).
        throw std::runtime_error("Rule output nbt not supported");
    }
    rule.output = resolveState(ruleJson.at("output_state"));
    return rule;
}

// Reference: AxisAlignedLinearPosTest.test / LinearPosTest.test — both roll a
// chance that ramps linearly with distance, so a structure decays the further
// a block is from its origin. The bastion's ramparts use the axis-aligned form
// to crumble with height.
//
// The nextFloat draw is UNCONDITIONAL once the earlier predicates passed, and
// it is the third draw in ProcessorRule.test's fixed input -> location ->
// position order. Getting the order or the count wrong desynchronises every
// later block in the same template from vanilla.
bool posRuleMatches(const Rule& rule, const core::BlockPos& worldPos,
                    const core::BlockPos& worldReference,
                    LegacyRandomSource& random) {
    if (rule.positionKind == Rule::POS_ALWAYS) return true;

    int dist;
    if (rule.positionKind == Rule::POS_LINEAR) {
        // BlockPos.distManhattan.
        dist = std::abs(worldPos.getX() - worldReference.getX())
             + std::abs(worldPos.getY() - worldReference.getY())
             + std::abs(worldPos.getZ() - worldReference.getZ());
    } else {
        // Java multiplies each component delta by the axis unit vector and
        // sums the absolute values — which is just "the delta on the chosen
        // axis", written that way so one expression covers all three.
        const int d = (rule.posAxis == 0) ? worldPos.getX() - worldReference.getX()
                    : (rule.posAxis == 2) ? worldPos.getZ() - worldReference.getZ()
                                          : worldPos.getY() - worldReference.getY();
        dist = std::abs(d);
    }

    // Mth.clampedLerp(factor, min, max) — the FACTOR comes first (Mth.java:123),
    // and the factor is itself Mth.inverseLerp(dist, minDist, maxDist).
    const float factor = (static_cast<float>(dist) - static_cast<float>(rule.posMinDist))
                       / (static_cast<float>(rule.posMaxDist)
                          - static_cast<float>(rule.posMinDist));
    float chance;
    if (factor < 0.0f)      chance = rule.posMinChance;
    else if (factor > 1.0f) chance = rule.posMaxChance;
    else chance = rule.posMinChance + factor * (rule.posMaxChance - rule.posMinChance);

    // `<=`, not `<` — Java's comparison, and it matters at chance 0.0 only if
    // nextFloat can return exactly 0, which it can.
    return random.nextFloat() <= chance;
}

// Reference: ProcessorRule.test - input predicate draws first (short-circuit).
bool ruleMatches(const Rule& rule, BlockState* state, BlockState* worldState,
                 const core::BlockPos& worldPos, const core::BlockPos& worldReference,
                 LegacyRandomSource& random) {
    bool inputOk;
    switch (rule.inputKind) {
        case Rule::INPUT_ALWAYS:
            inputOk = true;
            break;
        case Rule::INPUT_BLOCK:
            inputOk = state->is(rule.inputBlock);
            break;
        case Rule::INPUT_RANDOM_BLOCK:
            // Reference: RandomBlockMatchTest - nextFloat drawn ONLY when the
            // block matches.
            inputOk = state->is(rule.inputBlock)
                   && random.nextFloat() < rule.probability;
            break;
        case Rule::INPUT_STATE:
            inputOk = state == rule.inputState;
            break;
        case Rule::INPUT_TAG:
            inputOk = ::minecraft::levelgen::blockpredicates::matchesBlockTagName(
                state, rule.inputTag);
            break;
        default:
            inputOk = false;
            break;
    }
    if (!inputOk) return false;
    bool locationOk;
    switch (rule.locationKind) {
        case Rule::LOC_ALWAYS:
            locationOk = true;
            break;
        case Rule::LOC_BLOCK:
            locationOk = worldState != nullptr && worldState->is(rule.locationBlock);
            break;
        default:
            locationOk = false;
            break;
    }
    if (!locationOk) return false;
    return posRuleMatches(rule, worldPos, worldReference, random);
}

struct ParsedList {
    // Each entry is one processor: a rule chain, a block_rot integrity, a
    // protected_blocks tag, or a capped tag->state replace.
    struct Processor {
        bool isBlockRot = false;
        float integrity = 1.0f;
        std::string rottableTag;    // block_rot "rottable_blocks" ('#'-stripped)
        std::vector<Rule> rules;
        std::string protectedTag;   // protected_blocks "value"
        bool isCapped = false;      // capped: delegate rule + limit
        std::string cappedFromTag;
        std::string cappedLootTable;  // append_loot loot_table ("" = none)
        BlockState* cappedOutput = nullptr;
        int cappedLimit = 0;
    };
    std::vector<Processor> processors;
};

const ParsedList& loadList(const std::string& listId) {
    // Read from several decoration threads; see JigsawBehaviors::featureById.
    static std::mutex s_cacheMutex;
    static std::map<std::string, std::unique_ptr<ParsedList>> cache;
    std::lock_guard<std::mutex> lock(s_cacheMutex);
    auto it = cache.find(listId);
    if (it != cache.end()) return *it->second;

    std::string path = listId;
    size_t colon = path.find(':');
    if (colon != std::string::npos) path = path.substr(colon + 1);
    fs::path file = findDataRoot() / "minecraft" / "worldgen" / "processor_list"
                    / (path + ".json");
    std::ifstream input(file);
    if (!input) {
        throw std::runtime_error("Unknown processor list " + listId);
    }
    json parsed;
    input >> parsed;

    auto list = std::make_unique<ParsedList>();
    for (const json& processorJson : parsed.at("processors")) {
        std::string type = processorJson.at("processor_type").get<std::string>();
        ParsedList::Processor processor;
        if (type == "minecraft:rule") {
            for (const json& ruleJson : processorJson.at("rules")) {
                processor.rules.push_back(parseRule(ruleJson));
            }
        } else if (type == "minecraft:block_rot") {
            processor.isBlockRot = true;
            processor.integrity = processorJson.at("integrity").get<float>();
            if (processorJson.contains("rottable_blocks")) {
                // Reference: BlockRotProcessor.rottableBlocks - only blocks
                // whose ORIGINAL template state is in the tag rot; others
                // never draw and always survive.
                std::string tag =
                    processorJson.at("rottable_blocks").get<std::string>();
                if (!tag.empty() && tag[0] == '#') tag = tag.substr(1);
                if (tag.find(':') == std::string::npos) tag = "minecraft:" + tag;
                processor.rottableTag = tag;
            }
        } else if (type == "minecraft:protected_blocks") {
            // Reference: ProtectedBlockProcessor - drop the block when the
            // WORLD block at the target is in the tag.
            std::string tag = processorJson.at("value").get<std::string>();
            if (!tag.empty() && tag[0] == '#') tag = tag.substr(1);
            if (tag.find(':') == std::string::npos) tag = "minecraft:" + tag;
            processor.protectedTag = tag;
        } else if (type == "minecraft:capped") {
            // Reference: CappedProcessor - runs its delegate over at most
            // `limit` shuffled survivors in finalizeProcessing. Vanilla
            // delegates here are single-rule tag_match -> archaeology state
            // (the append_loot block_entity_modifier is a B8 concern).
            const json& delegate = processorJson.at("delegate");
            if (delegate.at("processor_type").get<std::string>() != "minecraft:rule"
                || delegate.at("rules").size() != 1) {
                throw std::runtime_error("Unsupported capped delegate in " + listId);
            }
            const json& rule = delegate.at("rules")[0];
            const json& input = rule.at("input_predicate");
            if (input.at("predicate_type").get<std::string>() != "minecraft:tag_match") {
                throw std::runtime_error("Unsupported capped input in " + listId);
            }
            std::string tag = input.at("tag").get<std::string>();
            if (!tag.empty() && tag[0] == '#') tag = tag.substr(1);
            if (tag.find(':') == std::string::npos) tag = "minecraft:" + tag;
            processor.isCapped = true;
            processor.cappedFromTag = tag;
            processor.cappedOutput = resolveState(rule.at("output_state"));
            // B8: the rule's block_entity_modifier append_loot -> each
            // replaced block gets a BrushableBlock BE payload.
            if (rule.contains("block_entity_modifier")) {
                const json& modifier = rule.at("block_entity_modifier");
                std::string mtype = modifier.value("type", std::string());
                if (mtype == "minecraft:append_loot") {
                    processor.cappedLootTable =
                        modifier.at("loot_table").get<std::string>();
                } else if (mtype != "minecraft:passthrough" && !mtype.empty()) {
                    throw std::runtime_error("Unsupported capped BE modifier "
                                             + mtype + " in " + listId);
                }
            }
            const json& limit = processorJson.at("limit");
            if (limit.is_number_integer()) {
                processor.cappedLimit = limit.get<int>();
            } else if (limit.is_object()
                       && limit.value("type", std::string()) == "minecraft:constant") {
                processor.cappedLimit = limit.at("value").get<int>();
            } else {
                throw std::runtime_error("Unsupported capped limit in " + listId);
            }
        } else {
            throw std::runtime_error("Unsupported processor type " + type
                                     + " in " + listId);
        }
        list->processors.push_back(std::move(processor));
    }
    const ParsedList& result = *list;
    cache[listId] = std::move(list);
    return result;
}

} // namespace

void appendProcessors(const std::string& listId, TemplatePlaceSettings& settings,
                      WorldGenLevel* level) {
    const ParsedList& list = loadList(listId);
    for (const ParsedList::Processor& processor : list.processors) {
        if (!processor.protectedTag.empty()) {
            const std::string& tag = processor.protectedTag;
            settings.processors.push_back(
                [level, tag](const core::BlockPos& worldPos, BlockState* state,
                             const core::BlockPos&, BlockState*,
                             const core::BlockPos&) -> BlockState* {
                    BlockState* worldState = level->getBlockState(worldPos);
                    if (worldState != nullptr
                        && ::minecraft::levelgen::blockpredicates::matchesBlockTagName(
                               worldState, tag)) {
                        return nullptr;
                    }
                    return state;
                });
            continue;
        }
        if (processor.isCapped) {
            TemplatePlaceSettings::CappedReplace capped;
            capped.fromTag = processor.cappedFromTag;
            capped.toState = processor.cappedOutput;
            capped.limit = processor.cappedLimit;
            capped.lootTable = processor.cappedLootTable;
            settings.cappedReplaces.push_back(capped);
            continue;
        }
        if (processor.isBlockRot) {
            float integrity = processor.integrity;
            std::string rottableTag = processor.rottableTag;
            settings.processors.push_back(
                [level, integrity, rottableTag](
                    const core::BlockPos& worldPos, BlockState* state,
                    const core::BlockPos&, BlockState* originalState,
                    const core::BlockPos&) -> BlockState* {
                    (void)level;
                    // Reference: BlockRotProcessor.processBlock - the tag
                    // gate tests the ORIGINAL template state and short-
                    // circuits BEFORE the nextFloat draw.
                    if (!rottableTag.empty()
                        && !::minecraft::levelgen::blockpredicates::
                               matchesBlockTagName(originalState, rottableTag)) {
                        return state;
                    }
                    if (integrity >= 1.0f) return state;
                    LegacyRandomSource random(Mth::getSeed(
                        worldPos.getX(), worldPos.getY(), worldPos.getZ()));
                    return random.nextFloat() <= integrity ? state : nullptr;
                });
        } else {
            const std::vector<Rule>* rules = &processor.rules;
            settings.processors.push_back(
                [level, rules](const core::BlockPos& worldPos, BlockState* state,
                               const core::BlockPos&, BlockState*,
                               const core::BlockPos& referencePos) -> BlockState* {
                    LegacyRandomSource random(Mth::getSeed(
                        worldPos.getX(), worldPos.getY(), worldPos.getZ()));
                    BlockState* worldState = level->getBlockState(worldPos);
                    for (const Rule& rule : *rules) {
                        if (ruleMatches(rule, state, worldState, worldPos,
                                        referencePos, random)) {
                            return rule.output;
                        }
                    }
                    return state;
                });
        }
    }
}

} // namespace ProcessorLists
} // namespace structure
} // namespace levelgen
} // namespace minecraft
