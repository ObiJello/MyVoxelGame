// File: src/server/commands/LootCommand.cpp
#include "LootCommand.hpp"
#include "EntitySelector.hpp"
#include "../network/ServerConnection.hpp"
#include "../session/PlayerSession.hpp"
#include "../session/PlayerSessionManager.hpp"
#include "../player/ServerPlayer.hpp"
#include "common/core/JavaRandom.hpp"
#include "common/entity/Item.hpp"
#include "common/text/TextComponent.hpp"
#include "common/world/loot/ChestLootTables.hpp"

#include <chrono>
#include <string>
#include <vector>

namespace Server {

    namespace {
        std::string Translate(const char* key, std::vector<std::string> args) {
            std::vector<Game::Text::Component> with;
            with.reserve(args.size());
            for (auto& a : args) with.push_back(Game::Text::Component::Literal(std::move(a)));
            return Game::Text::GetString(Game::Text::Component::Translatable(key, std::move(with)));
        }
    } // namespace

    void LootCommand::Register(CommandDispatcher& dispatcher) {
        namespace Cmd = Game::Cmd;
        // MC LootCommand, its one target/source pair here:
        // give <players> loot <loot_table>.
        dispatcher.RegisterCommand("loot", LootCommand::Execute,
            Cmd::Root().Then(Cmd::Literal("give")
                .Then(Cmd::Argument("players", Cmd::Arg::Players)
                    .Then(Cmd::Literal("loot")
                        .Then(Cmd::Argument("loot_table", Cmd::Arg::LootTable).Executes())))));
    }

    void LootCommand::Execute(const CommandSourceStack& source,
                              const std::vector<std::string>& args,
                              ServerConnection& connection,
                              PlayerSessionManager& sessionManager) {
        (void)sessionManager;
        if (args.size() != 4 || args[0] != "give" || args[2] != "loot") {
            connection.SendChatMessage("Usage: /loot give <targets> loot <loot_table>", 1);
            return;
        }

        // EntityArgument.players() for the give target.
        std::vector<SelectedEntity> targets;
        std::string error;
        if (!ResolveSelector(args[1], SelectorKind::Players, source, targets, error)) {
            connection.SendChatMessage(error, 1);
            return;
        }

        std::string table = args[3];
        if (table.find(':') == std::string::npos) table = "minecraft:" + table;

        // The level's random, as LootParams gets it with no seed — here a
        // time-seeded java.util.Random. Luck is not set by /loot (0).
        static Game::JavaRandom s_random(static_cast<int64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count()));
        std::vector<Game::ItemStack> items;
        // LootParams ORIGIN = source.getPosition(), in the source's level —
        // what an exploration_map in the table searches from.
        Game::ChestLoot::LootLevelContext lootLevel;
        lootLevel.dimensionId = Game::DimensionToRaw(source.dimension);
        lootLevel.origin = source.position;
        if (!Game::ChestLoot::GetRandomItems(table, s_random, 0.0f, items, &lootLevel)) {
            connection.SendChatMessage("Unknown loot table: " + table, 1);
            return;
        }

        // LootCommand.giveItems: every stack to every target (a copy each).
        for (const SelectedEntity& target : targets) {
            if (target.kind != SelectedEntity::Kind::Player || !target.session) continue;
            for (const Game::ItemStack& stack : items) target.session->GiveItem(stack);
        }

        // LootCommand.callback: one stack names it, several count them.
        if (items.size() == 1) {
            source.SendSuccess(connection, Translate("commands.drop.success.single_with_table",
                                                 {std::to_string(items.front().count),
                                                  Game::GetItemStackHoverName(items.front()), table}), false);
        } else {
            source.SendSuccess(connection, Translate("commands.drop.success.multiple_with_table",
                                                 {std::to_string(items.size()), table}), false);
        }
    }

} // namespace Server
