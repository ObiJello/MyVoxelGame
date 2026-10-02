// File: src/common/advancements/AdvancementLoader.hpp
//
// MC ServerAdvancementManager.apply over this engine's data pack: every
// data/<namespace>/advancement/**.json becomes a Definition with id
// "<namespace>:<path without .json>", decoded with Advancement.CODEC's rules
// (parent, display, criteria, requirements — AND of every criterion when
// absent —, rewards, sends_telemetry_event). The loaded set is put in a Tree
// and every root with a display is laid out (TreeNodePosition.run), so each
// display carries its x/y before anything reads it.
//
// Not loaded: the recipe-unlock advancements (no display, recipe rewards —
// vanilla's data/minecraft/advancement/recipes/**). They only exist to fill
// the recipe book, which this engine does not have; loading them would put
// ~1500 always-listening criteria on every player for nothing.
#pragma once

#include "Advancement.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Game::Advancements {

    struct Registry {
        // Every definition, parents before children, in a stable order
        // (file-path order, then dependency order).
        std::vector<std::unique_ptr<Definition>> definitions;
        std::unordered_map<std::string, const Definition*> byId;
        Tree tree;

        const Definition* Get(std::string_view id) const;
        // Ids in definition order — what /advancement suggests.
        std::vector<std::string> Ids() const;
    };

    // Builds a display icon from its ItemStackTemplate JSON ({id, count,
    // components}). The default (nullptr) resolves id and count only; the
    // server passes a decoder that applies the components through its NBT
    // codecs, so the stack it sends the client is complete.
    using IconDecoder = std::function<ItemStack(const nlohmann::json& icon)>;

    // Loads everything under `dataRoot` (default: MC_DATA_ROOT, else ./data).
    // Malformed files are logged and skipped, as MC skips them.
    std::shared_ptr<const Registry> LoadRegistry(const IconDecoder& decoder = nullptr,
                                                 const std::filesystem::path& dataRoot = {});

    // The client's copy for command suggestions (ids and criterion names),
    // loaded on first use and kept for the process.
    const Registry& SuggestionRegistry();

} // namespace Game::Advancements
