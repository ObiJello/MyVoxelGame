// File: src/server/commands/BlockStateArgument.cpp
#include "BlockStateArgument.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/tags/DataTags.hpp"

#include <algorithm>

namespace Server {

    namespace {

        // `[ns:]id[props]` / `#[ns:]tag[props]` → the id (namespace stripped,
        // '#' stripped, isTag set) and the raw `a=b,c=d` text.
        bool SplitIdAndProperties(const std::string& text, std::string& id, std::string& props,
                                  bool& isTag, std::string& error) {
            id = text;
            props.clear();
            if (const size_t bracket = text.find('['); bracket != std::string::npos) {
                id = text.substr(0, bracket);
                const size_t close = text.find(']', bracket);
                if (close == std::string::npos) { error = "Expected closing ] for block state properties"; return false; }
                props = text.substr(bracket + 1, close - bracket - 1);
            }
            isTag = !id.empty() && id[0] == '#';
            if (isTag) id.erase(0, 1);
            if (id.rfind("minecraft:", 0) == 0) id.erase(0, 10);
            if (id.empty()) { error = isTag ? "Expected tag" : "Expected block"; return false; }
            return true;
        }

        bool SplitProperties(const std::string& props,
                             std::vector<std::pair<std::string, std::string>>& out, std::string& error) {
            size_t pos = 0;
            while (pos < props.size()) {
                size_t comma = props.find(',', pos);
                if (comma == std::string::npos) comma = props.size();
                const std::string pair = props.substr(pos, comma - pos);
                pos = comma + 1;
                if (pair.empty()) continue;
                const size_t eq = pair.find('=');
                if (eq == std::string::npos) { error = "Expected value for property '" + pair + "'"; return false; }
                out.emplace_back(pair.substr(0, eq), pair.substr(eq + 1));
            }
            return true;
        }

        // Apply one named property to `state`, validating name and value
        // against the block (BlockStateParser.readProperty / parseValue).
        bool ApplyProperty(const std::string& id, Game::BlockState& state,
                           const std::string& name, const std::string& value, std::string& error) {
            const Game::BlockID block = state.Block();
            const uint16_t count = Game::BlockStates::PropertyCount(block);
            for (uint16_t slot = 0; slot < count; ++slot) {
                const Game::PropertyId prop = Game::BlockStates::PropertyAt(block, slot);
                if (Game::BlockStates::PropertyName(prop) != name) continue;
                const Game::BlockState next = state.SetName(prop, value);
                if (next == state && state.GetName(prop) != value) {
                    error = "Block minecraft:" + id + " does not accept '" + value + "' for " + name + " property";
                    return false;
                }
                state = next;
                return true;
            }
            error = "Block minecraft:" + id + " does not have property '" + name + "'";
            return false;
        }

        bool ResolveBlock(const std::string& id, Game::BlockState& state, std::string& error) {
            // Air is not in the generated slug table (BlockID::Air is the one
            // hand-registered block, with no registrySlug), and MC's
            // cave_air / void_air both alias to it here.
            if (id == "air" || id == "cave_air" || id == "void_air") {
                state = Game::BlockStates::Default(Game::BlockID::Air);
                return true;
            }
            state = Game::BlockStates::FromSlug(id);
            if (Game::BlockRegistry::Get(state.Block()).registrySlug != id) {
                error = "Unknown block type 'minecraft:" + id + "'";
                return false;
            }
            return true;
        }

    } // namespace

    bool ParseBlockState(const std::string& text, Game::BlockState& out, std::string& error) {
        std::string id, props;
        bool isTag = false;
        if (!SplitIdAndProperties(text, id, props, isTag, error)) return false;
        if (isTag) { error = "Tags aren't allowed here, only actual blocks"; return false; }
        Game::BlockState state;
        if (!ResolveBlock(id, state, error)) return false;
        std::vector<std::pair<std::string, std::string>> pairs;
        if (!SplitProperties(props, pairs, error)) return false;
        for (const auto& [name, value] : pairs) {
            if (!ApplyProperty(id, state, name, value, error)) return false;
        }
        out = state;
        return true;
    }

    bool ParseBlockPredicate(const std::string& text, BlockPredicate& out, std::string& error) {
        std::string id, props;
        bool isTag = false;
        if (!SplitIdAndProperties(text, id, props, isTag, error)) return false;
        BlockPredicate pred;
        pred.isTag = isTag;
        if (!SplitProperties(props, pred.properties, error)) return false;
        if (isTag) {
            pred.tag = "minecraft:" + id;
            if (!Game::DataTags::TagExists(Game::DataTags::Registry::Block, pred.tag)) {
                error = "Can't find tag '" + pred.tag + "' of type 'minecraft:block'";
                return false;
            }
        } else {
            if (!ResolveBlock(id, pred.state, error)) return false;
            // Validate the named properties against the block now, so a
            // typo is an error rather than a predicate that never matches.
            Game::BlockState probe = pred.state;
            for (const auto& [name, value] : pred.properties) {
                if (!ApplyProperty(id, probe, name, value, error)) return false;
            }
            pred.state = probe;
        }
        out = std::move(pred);
        return true;
    }

    bool BlockPredicate::Test(const Game::BlockState& actual) const {
        const Game::BlockID block = actual.Block();
        if (isTag) {
            const std::string slug = "minecraft:" + Game::BlockRegistry::Get(block).registrySlug;
            const auto& tags = Game::DataTags::TagsFor(Game::DataTags::Registry::Block, slug);
            if (std::find(tags.begin(), tags.end(), "#" + tag) == tags.end()) return false;
        } else if (block != state.Block()) {
            return false;
        }
        // MC BlockPredicate: each named property must READ as the given
        // value; a property the block lacks fails the match.
        for (const auto& [name, value] : properties) {
            bool found = false;
            const uint16_t count = Game::BlockStates::PropertyCount(block);
            for (uint16_t slot = 0; slot < count; ++slot) {
                const Game::PropertyId prop = Game::BlockStates::PropertyAt(block, slot);
                if (Game::BlockStates::PropertyName(prop) != name) continue;
                found = true;
                if (actual.GetName(prop) != value) return false;
                break;
            }
            if (!found) return false;
        }
        return true;
    }

} // namespace Server
