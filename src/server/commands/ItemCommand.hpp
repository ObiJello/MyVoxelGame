// File: src/server/commands/ItemCommand.hpp
//
// MC 26.3 ItemCommands: `/item replace|fill|override <target> <slots> with <item>
// [<count>]`, `/item replace|fill|override <target> <slots> from <source>
// <sourceSlots> [<modifier>]` and `/item modify <target> <slots> <modifier>`,
// where a target or source is `block <pos>` (a container block entity) or
// `entity <targets>`, and slots are SlotRanges names (container.5, armor.*,
// weapon.offhand …). replace stops when the items run out, fill cycles them,
// override empties the slots left over. Modifiers are item-modifier functions
// (an inline SNBT function or list, or data/<ns>/item_modifier/<id>.json),
// applied through the loot functions ChestLootTables models.
#pragma once
#include "CommandDispatcher.hpp"

namespace Server {

    class ItemCommand {
    public:
        static void Register(CommandDispatcher& dispatcher);
        static void Execute(const CommandSourceStack& source,
                            const std::vector<std::string>& args,
                            ServerConnection& connection,
                            PlayerSessionManager& sessionManager);
    };

} // namespace Server
