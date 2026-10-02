// File: src/server/commands/ClearCommand.cpp
#include "ClearCommand.hpp"
#include "EntityCommandUtil.hpp"
#include "../IntegratedServer.hpp"
#include "../player/ServerPlayer.hpp"

#include "common/entity/Inventory.hpp"

#include <algorithm>
#include <climits>
#include <string>
#include <vector>

namespace Server {

    namespace {

        // ContainerHelper.clearOrCountMatchingItems(ItemStack, ...): a
        // matching stack counts whole, or loses up to `amountToRemove`
        // (everything when negative).
        int ClearOrCount(Game::ItemStack& stack, const EntityCmd::ItemPredicate& predicate,
                         int amountToRemove, bool countingOnly) {
            if (stack.IsEmpty() || !predicate.Test(stack)) return 0;
            if (countingOnly) return stack.count;
            const int removed = amountToRemove < 0 ? stack.count : std::min(amountToRemove, stack.count);
            stack.count -= removed;
            if (stack.count <= 0) stack.Clear();
            return removed;
        }

        // Inventory.clearOrCountMatchingItems: the inventory in MC's slot
        // order (hotbar, main, armour feet..head, offhand), the 2x2 crafting
        // grid, then the cursor. `amountToRemove - count` stays negative
        // for an unlimited clear.
        int ClearPlayer(ServerPlayer& player, const EntityCmd::ItemPredicate& predicate,
                        int maxCount, bool countingOnly) {
            using Inv = Game::Inventory;
            Game::Inventory& inv = player.getInventory();
            std::vector<int> order;
            for (int i = 0; i < Inv::HOTBAR_SIZE; ++i) order.push_back(Inv::HOTBAR_BEGIN + i);
            for (int i = 0; i < Inv::MAIN_SIZE; ++i)   order.push_back(Inv::MAIN_BEGIN + i);
            for (int i = Inv::ARMOR_SIZE - 1; i >= 0; --i) order.push_back(Inv::ARMOR_BEGIN + i);
            order.push_back(Inv::OFFHAND_BEGIN);
            for (int i = 0; i < Inv::CRAFT_GRID_SIZE; ++i) order.push_back(Inv::CRAFT_GRID_BEGIN + i);

            int count = 0;
            for (const int index : order) {
                Game::ItemStack& stack = inv.MutableSlot(index);
                const int n = ClearOrCount(stack, predicate, maxCount - count, countingOnly);
                if (n > 0 && !countingOnly) player.markSlotDirty(index);
                count += n;
            }
            Game::ItemStack carried = player.getCarried();
            const int fromCursor = ClearOrCount(carried, predicate, maxCount - count, countingOnly);
            if (fromCursor > 0 && !countingOnly) player.setCarried(carried);
            count += fromCursor;
            return count;
        }

    } // namespace

    void ClearCommand::Register(CommandDispatcher& dispatcher) {
        namespace Cmd = Game::Cmd;
        dispatcher.RegisterCommand("clear", ClearCommand::Execute,
            Cmd::Root().Executes()
                .Then(Cmd::Argument("targets", Cmd::Arg::Players).Executes()
                    .Then(Cmd::Argument("item", Cmd::Arg::ItemPredicate).Executes()
                        .Then(Cmd::Argument("maxCount", Cmd::Arg::Integer)
                            .Suggests({"0", "1", "64"}).Executes()))));
    }

    void ClearCommand::Execute(const CommandSourceStack& source,
                               const std::vector<std::string>& args,
                               ServerConnection& connection,
                               PlayerSessionManager& sessionManager) {
        (void)sessionManager;
        using namespace EntityCmd;
        if (args.size() > 3) {
            Failure(connection, "Incorrect argument for command");
            for (const std::string& line : g_integratedServer->GetCommandDispatcher().GetUsageLines("clear")) {
                Failure(connection, line);
            }
            return;
        }

        std::vector<SelectedEntity> targets;
        std::string error;
        if (args.empty()) {
            // getPlayerOrException.
            ServerPlayer* self = source.ExecutorPlayer();
            SelectedEntity me;
            if (!self || !DescribePlayer(*self, source, me)) {
                Failure(connection, CommandSourceStack::kPlayerRequired);
                return;
            }
            targets.push_back(std::move(me));
        } else if (!ResolveSelector(args[0], SelectorKind::Players, source, targets, error)) {
            Failure(connection, error);
            return;
        }

        ItemPredicate predicate;   // Kind::Any: everything
        if (args.size() >= 2 && !ParseItemPredicate(args[1], predicate, error)) {
            Failure(connection, error);
            return;
        }
        int maxCount = -1;
        if (args.size() == 3 && !ParseInt(args[2], 0, INT_MAX, maxCount, error)) {
            Failure(connection, error);
            return;
        }
        const bool countingOnly = maxCount == 0;

        long long total = 0;
        int nonZero = 0;
        std::string onlyNonZero;
        for (const SelectedEntity& target : targets) {
            if (!target.player) continue;
            const int n = ClearPlayer(*target.player, predicate, maxCount, countingOnly);
            total += n;
            if (n != 0 && ++nonZero == 1) onlyNonZero = DisplayName(target);
        }
        if (total == 0) {
            // ERROR_DISPATCH over the ANY element type.
            if (targets.size() == 1) Failure(connection, Tr("clear.failed.single", {DisplayName(targets.front())}));
            else Failure(connection, Tr("clear.failed.multiple", {std::to_string(targets.size())}));
            return;
        }
        const std::string totalText = std::to_string(total);
        if (nonZero == 1) {
            Success(source, connection, /*broadcast=*/true, Tr(countingOnly ? "commands.clear.test.single" : "commands.clear.success.single",
                                   {totalText, onlyNonZero}));
        } else {
            Success(source, connection, /*broadcast=*/true, Tr(countingOnly ? "commands.clear.test.multiple" : "commands.clear.success.multiple",
                                   {totalText, std::to_string(nonZero)}));
        }
    }

} // namespace Server
