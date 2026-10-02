// File: src/client/renderer/gui/CommandSuggestions.hpp
//
// Chat's command completion and usage hint — MC's CommandSuggestions over the
// server's command trees (ClientboundCommandsPacket → here CommandsS2C's
// syntax block, Game::Cmd::Node).
//
// Nothing in here knows any one command. The tokens typed so far are walked
// down the command's tree (literals first, as Brigadier does; /execute's
// redirects back to its root; `run` re-enters the whole dispatcher), and at
// the cursor:
//   • completion asks each candidate node for entries — a literal offers its
//     keyword, an argument asks its TYPE's provider (block ids, entity
//     types, selectors, sounds, particles with their options, coordinates …),
//     which is where MC's ArgumentType.listSuggestions /
//     SuggestionProviders live;
//   • the hint lists the usage of every branch still possible from there,
//     the argument under the cursor highlighted.
// Ids are shown without "minecraft:" (other namespaces stay, they
// disambiguate); typing "minecraft:" first completes in that form.
//
// Main thread only (the chat screen and the packet handler both run there).
#pragma once

#include "common/command/CommandSyntax.hpp"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Render::CommandSuggestions {

    // The server's trees (CommandsS2C), by command name; replaces the last
    // set. Client-only commands (/clearchat) keep their built-in trees.
    void SetServerSyntax(std::vector<std::pair<std::string, Game::Cmd::Node>> trees);

    // The current dimension's biome / structure ids and tags (WorldgenIdsS2C),
    // full ids in — what /locate offers.
    void SetDimensionWorldgenIds(const std::vector<std::string>& biomes,
                                 const std::vector<std::string>& structures,
                                 const std::vector<std::string>& biomeTags,
                                 const std::vector<std::string>& structureTags);

    // Every custom entity name the server knows (CommandsS2C) — a
    // selector's `name=` offers them with the player names.
    void SetKnownEntityNames(std::vector<std::string> names);

    // The local player's own attribute rows as the server last synced them
    // (UpdateAttributesS2C): each attribute's id ("step_height"), base and
    // modifier Identifiers — what /attribute's `modifier remove|value get
    // <id>` and `base set <value>` offer.
    struct OwnAttribute {
        std::string              id;
        double                   base = 0.0;
        std::vector<std::string> modifierIds;
    };
    void SetOwnAttributes(std::vector<OwnAttribute> rows);

    struct Completion {
        int                      anchor = 0;   // index in the text the entries replace from
        std::vector<std::string> items;
    };

    // Completion for `text` ("/cmd ...") at `cursor`. nullopt when the text is
    // not a command, or names a command with no tree — the caller's fallback.
    std::optional<Completion> Complete(const std::string& text, int cursor);

    struct HintLine {
        std::string text;
        int highlightBegin = -1;   // the argument under the cursor, [begin, end) in text
        int highlightEnd   = -1;
    };

    // The usage lines for `text` at `cursor` (empty when there is nothing to
    // say); nullopt under the same conditions as Complete.
    std::optional<std::vector<HintLine>> Hint(const std::string& text, int cursor);

} // namespace Render::CommandSuggestions
