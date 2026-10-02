// File: src/server/commands/EnchantCommand.cpp
#include "EnchantCommand.hpp"
#include "EntityCommandUtil.hpp"
#include "../IntegratedServer.hpp"
#include "../player/ServerPlayer.hpp"

#include "common/entity/LivingEntity.hpp"
#include "common/entity/Mob.hpp"
#include "common/world/enchantment/Enchantment.hpp"
#include "common/world/enchantment/EnchantmentDefinitions.hpp"
#include "common/world/enchantment/EnchantmentHelper.hpp"
#include "common/world/enchantment/ItemEnchantments.hpp"

#include <climits>

namespace Server {

    namespace {

        // ResourceArgument<enchantment>: `sharpness` or `minecraft:sharpness`.
        bool ParseEnchantment(const std::string& token, Game::EnchantmentId& out, std::string& error) {
            std::string slug = token;
            if (slug.rfind("minecraft:", 0) == 0) slug.erase(0, 10);
            if (auto id = Game::EnchantmentRegistry::ByName(slug)) {
                out = *id;
                return true;
            }
            const std::string full = token.find(':') == std::string::npos ? "minecraft:" + token : token;
            error = "Can't find element '" + full + "' of type 'minecraft:enchantment'";
            return false;
        }

        // EnchantmentHelper.isEnchantmentCompatible over the item's
        // getEnchantmentsForCrafting: every enchantment already on it must
        // be compatible (Enchantment.areCompatible — which is false for the
        // same enchantment, so a second Sharpness is refused too).
        bool CompatibleWith(const Game::ItemStack& item, Game::EnchantmentId enchantment) {
            for (const Game::EnchantmentInstance& e : Game::EnchantmentHelper::GetEnchantmentsForCrafting(item).entries) {
                if (!Game::EnchantmentDefinitions::AreCompatible(e.id, enchantment)) return false;
            }
            return true;
        }

    } // namespace

    void EnchantCommand::Register(CommandDispatcher& dispatcher) {
        namespace Cmd = Game::Cmd;
        dispatcher.RegisterCommand("enchant", EnchantCommand::Execute,
            Cmd::Root()
                .Then(Cmd::Argument("targets", Cmd::Arg::Entities)
                    .Then(Cmd::Argument("enchantment", Cmd::Arg::Enchantment).Executes()
                        .Then(Cmd::Argument("level", Cmd::Arg::Integer)
                            .Suggests({"1", "2", "3", "4", "5"}).Executes()))));
    }

    void EnchantCommand::Execute(const CommandSourceStack& source,
                                 const std::vector<std::string>& args,
                                 ServerConnection& connection,
                                 PlayerSessionManager& sessionManager) {
        (void)sessionManager;
        using namespace EntityCmd;
        if (args.size() < 2 || args.size() > 3) {
            Failure(connection, "Unknown or incomplete command, see below for error");
            for (const std::string& line : g_integratedServer->GetCommandDispatcher().GetUsageLines("enchant")) {
                Failure(connection, line);
            }
            return;
        }
        std::vector<SelectedEntity> targets;
        std::string error;
        if (!ResolveSelector(args[0], SelectorKind::Entities, source, targets, error)) {
            Failure(connection, error);
            return;
        }
        Game::EnchantmentId enchantment{};
        if (!ParseEnchantment(args[1], enchantment, error)) {
            Failure(connection, error);
            return;
        }
        int level = 1;
        if (args.size() == 3 && !ParseInt(args[2], 0, INT_MAX, level, error)) {
            Failure(connection, error);
            return;
        }
        const Game::Enchantment& def = Game::EnchantmentRegistry::Get(enchantment);
        if (level > def.maxLevel) {
            Failure(connection, Tr("commands.enchant.failed.level", {std::to_string(level), std::to_string(def.maxLevel)}));
            return;
        }

        const bool single = targets.size() == 1;
        int changed = 0;
        std::string onlyChanged;
        for (const SelectedEntity& target : targets) {
            Game::LivingEntity* living = LivingOf(target);
            if (!living) {
                if (single) { Failure(connection, Tr("commands.enchant.failed.entity", {DisplayName(target)})); return; }
                continue;
            }
            // getMainHandItem: a player's selected hotbar stack (the view
            // answers from the inventory), a mob's MAINHAND equipment.
            Game::ItemStack item;
            if (target.kind == SelectedEntity::Kind::Mob && target.mob) {
                item = target.mob->GetEquipment(Game::EquipmentSlot::MAINHAND);
            } else if (const Game::ItemStack* held = living->EquipmentInSlot(Game::EquipmentSlot::MAINHAND)) {
                item = *held;
            }
            if (item.IsEmpty()) {
                if (single) { Failure(connection, Tr("commands.enchant.failed.itemless", {DisplayName(target)})); return; }
                continue;
            }
            if (!Game::EnchantmentDefinitions::IsSupportedItem(enchantment, item.itemId) ||
                !CompatibleWith(item, enchantment)) {
                if (single) {
                    Failure(connection, Tr("commands.enchant.failed.incompatible", {Game::GetItemStackHoverName(item)}));
                    return;
                }
                continue;
            }
            // ItemStack.enchant(holder, level).
            Game::EnchantmentHelper::Enchant(item, enchantment, level);
            if (target.kind == SelectedEntity::Kind::Mob && target.mob) {
                target.mob->SetEquipment(Game::EquipmentSlot::MAINHAND, item);
            } else if (target.player) {
                Game::Inventory& inv = target.player->getInventory();
                const int slot = Game::Inventory::HotbarToIndex(inv.GetSelectedSlot());
                inv.SetSlotFull(slot, item);
                target.player->markSlotDirty(slot);
            }
            if (++changed == 1) onlyChanged = DisplayName(target);
        }
        if (changed == 0) {
            Failure(connection, Tr("commands.enchant.failed"));
            return;
        }
        // Enchantment.getFullname(enchantment, level).
        const std::string name = Game::Enchantment::GetFullname(def, level).text;
        if (changed == 1) Success(source, connection, /*broadcast=*/true, Tr("commands.enchant.success.single", {name, onlyChanged}));
        else              Success(source, connection, /*broadcast=*/true, Tr("commands.enchant.success.multiple", {name, std::to_string(changed)}));
    }

} // namespace Server
