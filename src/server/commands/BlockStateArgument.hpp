// File: src/server/commands/BlockStateArgument.hpp
//
// MC net.minecraft.commands.arguments.blocks.{BlockStateParser,
// BlockStateArgument, BlockPredicateArgument} — the `[ns:]id[prop=value,...]`
// grammar `/setblock` places and the `#tag[prop=value]` / `id[prop=value]`
// predicate `/execute if block` tests. One parser, so the two agree.
#pragma once

#include "common/world/block/BlockState.hpp"

#include <string>
#include <utility>
#include <vector>

namespace Server {

    // BlockStateParser.parseForBlock: a concrete state. Unknown blocks,
    // properties and values are errors, as in vanilla.
    bool ParseBlockState(const std::string& text, Game::BlockState& out, std::string& error);

    // BlockPredicateArgument: either a block (with the given properties
    // required to match, the rest free) or a `#tag` (with properties
    // matched by NAME on whatever block is there).
    struct BlockPredicate {
        bool             isTag = false;
        std::string      tag;      // "minecraft:logs" when isTag
        Game::BlockState state;    // the block when !isTag
        // The properties the text named, as (name, value) — only these are
        // tested; a block form has them validated at parse time.
        std::vector<std::pair<std::string, std::string>> properties;

        bool Test(const Game::BlockState& actual) const;
    };

    bool ParseBlockPredicate(const std::string& text, BlockPredicate& out, std::string& error);

} // namespace Server
