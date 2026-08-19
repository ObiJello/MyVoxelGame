// File: src/common/world/block/BlockStateModels.cpp
#include "BlockStateModels.hpp"
#include "BlockRegistry.hpp"
#include "BlockModel.hpp"
#include "../../core/Log.hpp"

#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <array>
#include <vector>
#include <set>
#include <string>
#include <unordered_map>

using json = nlohmann::json;

namespace Game {

    namespace {

        // Per-BlockID, per-state model name. Empty string = "no override,
        // use the block's plain model".
        std::array<std::vector<std::string>, BlockRegistry::Size> s_stateModels{};
        const std::string kEmpty;

        // "minecraft:block/furnace" -> "furnace". Mirrors
        // BlockModelRegistry's own key format (bare basenames).
        std::string BaseModelName(const std::string& ref) {
            auto slash = ref.find_last_of('/');
            if (slash != std::string::npos) return ref.substr(slash + 1);
            auto colon = ref.find_last_of(':');
            if (colon != std::string::npos) return ref.substr(colon + 1);
            return ref;
        }

        // One "facing=east,lit=false" selector, split into constraints. MC does
        // the same in VariantSelector.predicate, then tests it against every
        // possible BlockState.
        std::vector<std::pair<std::string, std::string>> ParseSelector(const std::string& key) {
            std::vector<std::pair<std::string, std::string>> out;
            size_t start = 0;
            while (start <= key.size() && !key.empty()) {
                size_t comma = key.find(',', start);
                std::string term = key.substr(start, comma == std::string::npos
                                                     ? std::string::npos : comma - start);
                size_t eq = term.find('=');
                if (eq != std::string::npos) {
                    out.emplace_back(term.substr(0, eq), term.substr(eq + 1));
                }
                if (comma == std::string::npos) break;
                start = comma + 1;
            }
            return out;
        }

        struct VariantEntry {
            std::vector<std::pair<std::string, std::string>> constraints;
            std::string model;
            int  xTurns = 0;
            int  yTurns = 0;
            bool uvLock = false;
            int  order  = 0;  // position in the JSON, for deterministic tie-breaks
        };

        // The name a rotated model is registered under. uvlock is part of the
        // key because it changes the BAKED result: the same model at the same
        // rotation is a different quad set with the texture locked to the world
        // than with it turned along. Stairs are where that first bites — nearly
        // every rotated stair variant in vanilla sets uvlock, while the fence
        // and pillar rotations sharing this baker do not.
        std::string RotatedModelName(const std::string& base, int xt, int yt, bool uvLock) {
            std::string n = base + "__x" + std::to_string(xt) + "_y" + std::to_string(yt);
            if (uvLock) n += "_uv";
            return n;
        }

        void ReadVariantValue(const json& value, VariantEntry& out) {
            // A variant value may be a single object or a weighted array; MC
            // picks randomly among an array per block position. We have no
            // per-position random in the mesher, so take the first entry —
            // deterministic, and correct for every non-decorative block.
            const json& v = value.is_array() ? (value.empty() ? json::object() : value[0]) : value;
            if (v.contains("model")) out.model = BaseModelName(v["model"].get<std::string>());
            if (v.contains("x")) out.xTurns = v["x"].get<int>() / 90;
            if (v.contains("y")) out.yTurns = v["y"].get<int>() / 90;
            if (v.contains("uvlock")) out.uvLock = v["uvlock"].get<bool>();
        }

        // ── multipart ────────────────────────────────────────────────────────
        //
        // MC's multipart is ADDITIVE: every entry whose `when` matches
        // contributes its model, and the block is drawn as the union. That is
        // not a detail — it is the whole encoding of segmented ground cover.
        // `leaf_litter` maps segment_amount=3 to leaf_litter_2 (a half) via a
        // "2|3" predicate AND leaf_litter_3 (a quarter) via a "3" predicate;
        // reading only the last match draws one quarter where three belong.
        // wildflowers/pink_petals are worse: their first entry has no amount
        // predicate at all, so it applies to every state, and picking a single
        // match makes a full 4-flower patch render as one flower.
        //
        // A `when` is one of:
        //   {prop: "a", prop2: "b|c"}   — all must hold, `|` is alternatives
        //   {"OR":  [ …, … ]}           — any nested condition holds
        //   {"AND": [ …, … ]}           — every nested condition holds
        // and an absent `when` means "always".
        struct Condition {
            enum class Op { And, Or } op = Op::And;
            std::vector<std::pair<std::string, std::vector<std::string>>> terms;
            std::vector<Condition> nested;
        };

        std::vector<std::string> SplitAlternatives(const std::string& v) {
            std::vector<std::string> out;
            size_t start = 0;
            for (;;) {
                const size_t bar = v.find('|', start);
                out.push_back(v.substr(start, bar == std::string::npos
                                                  ? std::string::npos : bar - start));
                if (bar == std::string::npos) break;
                start = bar + 1;
            }
            return out;
        }

        Condition ParseCondition(const json& j) {
            Condition c;
            if (!j.is_object()) return c;
            for (auto it = j.begin(); it != j.end(); ++it) {
                if (it.key() == "OR" || it.key() == "AND") {
                    Condition group;
                    group.op = (it.key() == "OR") ? Condition::Op::Or : Condition::Op::And;
                    for (const auto& sub : it.value()) group.nested.push_back(ParseCondition(sub));
                    c.nested.push_back(std::move(group));
                    continue;
                }
                if (!it.value().is_string()) continue;
                c.terms.emplace_back(it.key(), SplitAlternatives(it.value().get<std::string>()));
            }
            return c;
        }

        // `props` is the state's property map. A term naming a property this
        // engine doesn't model FAILS, unlike the `variants` path which treats
        // unknowns as satisfied.
        //
        // The asymmetry is deliberate. `variants` picks exactly one entry, so
        // leniency just widens the match. `multipart` is a UNION, so a term
        // that can't be judged and passes anyway drags its whole model into the
        // block — and if a predicate names a property we got wrong, EVERY entry
        // matches and the block renders as all its parts stacked at once. That
        // is precisely what a `segment_amount`/`flower_amount` mix-up did to
        // wildflowers: a 1-flower patch drew all four models.
        //
        // Failing closed degrades gracefully instead: an unmodelled predicate
        // drops its part, so chiseled_bookshelf renders as the bare shelf
        // rather than every occupied AND empty slot overlapping.
        bool EvalCondition(const Condition& c,
                           const std::unordered_map<std::string, std::string>& props) {
            const bool isOr = (c.op == Condition::Op::Or);
            bool anyTrue = false, allTrue = true;

            auto note = [&](bool v) { anyTrue = anyTrue || v; allTrue = allTrue && v; };

            for (const auto& [prop, values] : c.terms) {
                auto pit = props.find(prop);
                if (pit == props.end()) { note(false); continue; }
                bool hit = false;
                for (const auto& v : values) if (pit->second == v) { hit = true; break; }
                note(hit);
            }
            for (const Condition& sub : c.nested) note(EvalCondition(sub, props));

            if (c.terms.empty() && c.nested.empty()) return true;   // absent `when`
            return isOr ? anyTrue : allTrue;
        }

        struct MultipartEntry {
            bool        always = false;   // no `when` at all
            Condition   when;
            VariantEntry apply;
        };

        // Every property name a condition tree mentions, so a file can be
        // rejected wholesale when it asks about things we don't track.
        void CollectConditionProps(const Condition& c, std::set<std::string>& out) {
            for (const auto& [prop, values] : c.terms) out.insert(prop);
            for (const Condition& sub : c.nested) CollectConditionProps(sub, out);
        }

    } // namespace

    const std::string& BlockStateModels::ModelNameFor(BlockID id, BlockStateIndex stateIndex) {
        const size_t idx = static_cast<size_t>(id);
        if (idx >= BlockRegistry::Size) return kEmpty;
        const auto& states = s_stateModels[idx];
        if (stateIndex >= states.size()) return kEmpty;
        return states[stateIndex];
    }

    void BlockStateModels::Clear() {
        for (auto& v : s_stateModels) v.clear();
    }

    bool BlockStateModels::Load(const std::string& blockstatesPath) {
        Clear();

        if (!std::filesystem::exists(blockstatesPath)) {
            Log::Info("No blockstates directory at %s - blocks will use their default models",
                      blockstatesPath.c_str());
            return false;
        }

        // model name -> BlockID, so a blockstate file can be matched to the
        // block it describes. Blockstate filenames are the MC block names,
        // which is exactly what BlockDefs.inc stores as the model name.
        std::unordered_map<std::string, BlockID> nameToBlock;
        for (size_t i = 0; i < BlockRegistry::Size; ++i) {
            const Block& b = BlockRegistry::blockDefinitions[i];
            if (!b.modelName.empty()) nameToBlock.emplace(b.modelName, static_cast<BlockID>(i));
        }

        // blockstate file stem -> every BlockID it describes, each with the
        // properties that BlockID already stands for. Usually one entry with no
        // implied properties; the segmented blocks are the exception, where four
        // BlockIDs share one file and each pins its own segment count.
        struct FileTarget {
            BlockID id;
            std::unordered_map<std::string, std::string> implied;
        };
        std::unordered_map<std::string, std::vector<FileTarget>> fileTargets;
        for (const auto& [modelName, id] : nameToBlock) {
            fileTargets[modelName].push_back({ id, {} });
        }
        // The segmented families (leaf_litter, wildflowers, pink_petals) are the
        // one case where the modelName-keyed pass above cannot find the file:
        // their model is the per-count "leaf_litter_1" while their blockstate
        // JSON is "leaf_litter". Add them under their registry slug.
        //
        // This used to be a kSegmentedBlocks table pinning an IMPLIED
        // `segment_amount`/`flower_amount` for each of four BlockIDs. The count
        // is a real property now, so pinning it would force every state onto the
        // 1-segment model; the state itself selects the variant instead.
        for (const char* slug : { "leaf_litter", "wildflowers", "pink_petals" }) {
            const BlockID id = BlockStates::FromSlug(slug).Block();
            if (id == BlockID::Air) continue;
            fileTargets[slug].push_back({ id, {} });
        }

        size_t filesRead = 0, blocksMatched = 0, rotatedModels = 0;
        size_t mergedModels = 0, multipartBlocks = 0, multipartUnjudgeable = 0;

        for (const auto& entry : std::filesystem::directory_iterator(blockstatesPath)) {
            if (!entry.is_regular_file() || entry.path().extension() != ".json") continue;

            const std::string blockName = entry.path().stem().string();
            auto targetsIt = fileTargets.find(blockName);
            if (targetsIt == fileTargets.end()) continue;   // block we don't implement
            filesRead++;

            json j;
            try {
                std::ifstream f(entry.path());
                if (!f.is_open()) continue;
                f >> j;
            } catch (const std::exception& e) {
                Log::Warning("Failed to parse blockstate %s: %s", blockName.c_str(), e.what());
                continue;
            }

            std::vector<VariantEntry>   variants;
            std::vector<MultipartEntry> multipart;

            if (j.contains("variants")) {
                int order = 0;
                for (auto it = j["variants"].begin(); it != j["variants"].end(); ++it) {
                    VariantEntry ve;
                    ve.constraints = ParseSelector(it.key());
                    ve.order = order++;
                    ReadVariantValue(it.value(), ve);
                    if (!ve.model.empty()) variants.push_back(std::move(ve));
                }
            } else if (j.contains("multipart")) {
                int order = 0;
                for (const auto& part : j["multipart"]) {
                    if (!part.contains("apply")) continue;
                    MultipartEntry me;
                    me.apply.order = order++;
                    ReadVariantValue(part["apply"], me.apply);
                    if (me.apply.model.empty()) continue;
                    if (part.contains("when")) me.when = ParseCondition(part["when"]);
                    else                       me.always = true;
                    multipart.push_back(std::move(me));
                }
            }
            if (variants.empty() && multipart.empty()) continue;

            std::set<std::string> multipartProps;
            for (const MultipartEntry& me : multipart) {
                if (!me.always) CollectConditionProps(me.when, multipartProps);
            }

            for (const FileTarget& target : targetsIt->second) {
                const BlockID id = target.id;
                const auto& def = BlockRegistry::GetStateDefinition(id);
                const uint16_t stateCount = def.StateCount();

                // Multipart is only usable when we can actually answer every
                // question the file asks. Fences, walls, glass panes and iron
                // bars — 70-odd blocks — dispatch on CONNECTION properties
                // (north/south/east/west/up) that are derived from neighbours
                // at runtime and that this engine does not model at all. With
                // the fail-closed rule above, every one of their arm entries
                // would drop and they'd render as a bare post, silently losing
                // their collision box too (none of them ship a plain model, so
                // today they resolve to the default full cube).
                //
                // Refusing the file outright leaves those blocks exactly as
                // they were, and the check self-enables the moment their
                // properties are modelled — no hardcoded block list.
                if (!multipart.empty()) {
                    bool judgeable = true;
                    for (const std::string& p : multipartProps) {
                        if (target.implied.count(p)) continue;
                        bool modelled = false;
                        for (const auto& dp : def.properties) {
                            if (dp.name == p) { modelled = true; break; }
                        }
                        if (!modelled) { judgeable = false; break; }
                    }
                    if (!judgeable) {
                        multipartUnjudgeable++;
                        continue;
                    }
                    multipartBlocks++;
                }

                auto& slot = s_stateModels[static_cast<size_t>(id)];
                slot.assign(stateCount, std::string{});
                blocksMatched++;

                for (uint16_t state = 0; state < stateCount; ++state) {
                    auto props = def.PropertiesOf(state);
                    // Fold in what this BlockID already stands for, so a predicate
                    // like `segment_amount=2|3` can be judged even though the state
                    // itself only carries `facing`.
                    for (const auto& [k, v] : target.implied) props[k] = v;

                    if (!multipart.empty()) {
                        // Collect EVERY matching part, rotate each, then merge —
                        // multipart is a union, not a lookup.
                        std::vector<const BlockModel*> parts;
                        std::string mergedName;
                        for (const MultipartEntry& me : multipart) {
                            if (!me.always && !EvalCondition(me.when, props)) continue;

                            const int xt = ((me.apply.xTurns % 4) + 4) % 4;
                            const int yt = ((me.apply.yTurns % 4) + 4) % 4;
                            std::string partName = me.apply.model;
                            if (xt != 0 || yt != 0) {
                                partName = RotatedModelName(partName, xt, yt, me.apply.uvLock);
                                if (!BlockModelRegistry::HasModel(partName)) {
                                    BlockModelRegistry::RegisterModel(
                                        partName,
                                        BlockModelRegistry::RotateModel(
                                            BlockModelRegistry::GetModel(me.apply.model),
                                            xt, yt, me.apply.uvLock));
                                    rotatedModels++;
                                }
                            }
                            parts.push_back(&BlockModelRegistry::GetModel(partName));
                            mergedName += (mergedName.empty() ? "" : "+") + partName;
                        }
                        if (parts.empty()) continue;

                        if (parts.size() == 1) {
                            slot[state] = mergedName;   // already a registered model
                            continue;
                        }
                        if (!BlockModelRegistry::HasModel(mergedName)) {
                            BlockModelRegistry::RegisterModel(
                                mergedName, BlockModelRegistry::MergeModels(parts));
                            mergedModels++;
                        }
                        slot[state] = mergedName;
                        continue;
                    }

                    // Pick the variant whose selector this state satisfies.
                    // Constraints naming a property we don't model (a stair's
                    // `shape`, a door's `hinge`) can't be judged, so they're counted
                    // as "extra" and we prefer the variant with the fewest of them —
                    // i.e. the one that pins down the least beyond what we track.
                    //
                    // Declaring a property in BlockRegistry::InitBlockStates is what
                    // moves it out of "extra" and lets its variants be selected at
                    // all: a furnace could only ever resolve the unlit model until
                    // `lit` was added there, because both lit=true and lit=false
                    // scored one extra and JSON order handed it to `false`.
                    // Ties break on JSON order, which puts MC's default-ish states
                    // first and keeps the choice deterministic across runs.
                    const VariantEntry* best = nullptr;
                    int bestExtras = 0;
                    for (const auto& ve : variants) {
                        bool ok = true;
                        int extras = 0;
                        for (const auto& [prop, want] : ve.constraints) {
                            auto pit = props.find(prop);
                            if (pit == props.end()) { extras++; continue; }
                            if (pit->second != want) { ok = false; break; }
                        }
                        if (!ok) continue;
                        if (!best || extras < bestExtras ||
                            (extras == bestExtras && ve.order < best->order)) {
                            best = &ve;
                            bestExtras = extras;
                        }
                    }
                    if (!best) continue;

                    if (best->xTurns == 0 && best->yTurns == 0) {
                        slot[state] = best->model;
                        continue;
                    }

                    // Synthesise the rotated model once and register it under a
                    // derived name, the way MC bakes one quad set per state.
                    const int xt = ((best->xTurns % 4) + 4) % 4;
                    const int yt = ((best->yTurns % 4) + 4) % 4;
                    const std::string rotatedName =
                        RotatedModelName(best->model, xt, yt, best->uvLock);
                    if (!BlockModelRegistry::HasModel(rotatedName)) {
                        BlockModelRegistry::RegisterModel(
                            rotatedName,
                            BlockModelRegistry::RotateModel(
                                BlockModelRegistry::GetModel(best->model),
                                best->xTurns, best->yTurns, best->uvLock));
                        rotatedModels++;
                    }
                    slot[state] = rotatedName;
                }   // states
            }       // BlockIDs sharing this blockstate file
        }           // blockstate files

        // ── Fallback: horizontal-facing blocks with no variant table ─────────
        //
        // A block can declare a `facing` property and still get nothing from the
        // loop above: its blockstate file may be `multipart` (leaf_litter,
        // wildflowers, pink_petals, chiseled_bookshelf), or its BlockID may
        // carry a model name that doesn't match any blockstate filename — which
        // is the case for every segmented block here, since `segment_amount` is
        // spent on separate BlockIDs named leaf_litter_1 … leaf_litter_4.
        //
        // Every one of those blocks still rotates the same way, because MC's
        // whole HorizontalDirectionalBlock family shares one convention: the
        // model is authored facing north and the blockstate applies
        // y=0/90/180/270 for north/east/south/west. That is literally what
        // furnace.json and leaf_litter.json both spell out, so deriving it is
        // reproducing the data rather than guessing at it.
        //
        // Only ever fills slots the JSON pass left empty, so a real `variants`
        // file always wins.
        size_t fallbackBlocks = 0;
        for (size_t i = 0; i < BlockRegistry::Size; ++i) {
            auto& slot = s_stateModels[i];
            if (!slot.empty()) continue;                     // JSON already spoke

            const auto& def = BlockRegistry::GetStateDefinition(static_cast<BlockID>(i));
            const bool horizontalFacing =
                def.properties.size() == 1 &&
                def.properties[0].name == "facing" &&
                def.properties[0].values.size() == 4;
            if (!horizontalFacing) continue;

            const std::string& base = BlockRegistry::blockDefinitions[i].modelName;
            if (base.empty() || !BlockModelRegistry::HasModel(base)) continue;

            const uint16_t stateCount = def.StateCount();
            slot.assign(stateCount, std::string{});
            fallbackBlocks++;

            const BlockID blockId = static_cast<BlockID>(i);
            for (uint16_t state = 0; state < stateCount; ++state) {
                const std::string_view facing =
                    BlockStates::FromIndex(blockId, state).GetValueByName("facing");
                int yTurns = 0;
                if      (facing == "north") yTurns = 0;
                else if (facing == "east")  yTurns = 1;
                else if (facing == "south") yTurns = 2;
                else if (facing == "west")  yTurns = 3;
                else continue;

                if (yTurns == 0) { slot[state] = base; continue; }

                // No uvlock: this fallback exists for blocks with no variant
                // table to read one from, and MC's own multipart entries for
                // them (leaf_litter, wildflowers, pink_petals) do not set it.
                const std::string rotatedName = RotatedModelName(base, 0, yTurns, false);
                if (!BlockModelRegistry::HasModel(rotatedName)) {
                    BlockModelRegistry::RegisterModel(
                        rotatedName,
                        BlockModelRegistry::RotateModel(
                            BlockModelRegistry::GetModel(base), 0, yTurns, false));
                    rotatedModels++;
                }
                slot[state] = rotatedName;
            }
        }

        Log::Info("Blockstates loaded - %zu files matched %zu blocks (%zu multipart, "
                  "%zu multipart skipped for unmodelled properties), "
                  "%zu rotated + %zu merged models synthesised, "
                  "%zu blocks on the horizontal-facing fallback",
                  filesRead, blocksMatched, multipartBlocks, multipartUnjudgeable,
                  rotatedModels, mergedModels, fallbackBlocks);
        return blocksMatched > 0 || fallbackBlocks > 0;
    }

} // namespace Game
