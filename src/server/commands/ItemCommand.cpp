// File: src/server/commands/ItemCommand.cpp
#include "ItemCommand.hpp"
#include "CommandCoords.hpp"
#include "EntityCommandUtil.hpp"
#include "ItemArgument.hpp"
#include "SnbtParser.hpp"
#include "../IntegratedServer.hpp"
#include "../entity/ItemEntityManager.hpp"
#include "../level/ServerLevel.hpp"
#include "../player/ServerPlayer.hpp"
#include "../world/storage/anvil/ComponentNbt.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/data/DataComponents.hpp"
#include "common/entity/Inventory.hpp"
#include "common/entity/Mob.hpp"
#include "common/entity/decoration/ItemFrame.hpp"
#include "common/inventory/Container.hpp"
#include "common/inventory/SlotRanges.hpp"
#include "common/world/block/entity/BlockEntity.hpp"
#include "common/world/level/World.hpp"
#include "common/world/loot/ChestLootTables.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>

namespace Server {

    namespace {

        using EntityCmd::Failure;
        using EntityCmd::Success;
        using EntityCmd::Tr;
        namespace SR = Game::SlotRanges;

        // MC SlotAccess: one slot's get and set (set false when the slot
        // refuses the stack — an armour slot offered a sword).
        struct Slot {
            std::function<Game::ItemStack()>            get;
            std::function<bool(const Game::ItemStack&)> set;
        };

        // ── Entity.getSlot ───────────────────────────────────────────────

        Game::EquipmentSlot EquipmentFor(int id, bool& ok) {
            ok = true;
            switch (id) {
                case SR::kMainHand: return Game::EquipmentSlot::MAINHAND;
                case SR::kOffHand:  return Game::EquipmentSlot::OFFHAND;
                case SR::kFeet:     return Game::EquipmentSlot::FEET;
                case SR::kLegs:     return Game::EquipmentSlot::LEGS;
                case SR::kChest:    return Game::EquipmentSlot::CHEST;
                case SR::kHead:     return Game::EquipmentSlot::HEAD;
                case SR::kBody:     return Game::EquipmentSlot::BODY;
                case SR::kSaddle:   return Game::EquipmentSlot::SADDLE;
                default: ok = false; return Game::EquipmentSlot::MAINHAND;
            }
        }

        // Player.getSlot: the inventory (0-8 hotbar, 9-35 main), the hands
        // and armour (the armour slots take only what is worn there), the
        // cursor (499) and the 2x2 crafting grid (500-503). There is no
        // ender chest inventory in this engine, so 200+ has no slot.
        std::optional<Slot> PlayerSlot(ServerPlayer& player, int id) {
            using Inv = Game::Inventory;
            ServerPlayer* p = &player;
            Inv& inv = player.getInventory();
            const auto invSlot = [p](int index, std::function<bool(const Game::ItemStack&)> accepts) {
                return Slot{
                    [p, index] { return p->getInventory().GetSlot(index); },
                    [p, index, accepts](const Game::ItemStack& s) {
                        if (accepts && !accepts(s)) return false;
                        p->getInventory().SetSlotFull(index, s);
                        p->markSlotDirty(index);
                        return true;
                    }};
            };
            if (id >= 0 && id < Inv::HOTBAR_SIZE) return invSlot(Inv::HOTBAR_BEGIN + id, nullptr);
            if (id >= 9 && id < 9 + Inv::MAIN_SIZE) return invSlot(Inv::MAIN_BEGIN + (id - 9), nullptr);
            if (id == SR::kMainHand) return invSlot(Inv::HotbarToIndex(inv.GetSelectedSlot()), nullptr);
            if (id == SR::kOffHand) return invSlot(Inv::OFFHAND_BEGIN, nullptr);
            if (id >= SR::kFeet && id <= SR::kHead) {
                bool ok = false;
                const Game::EquipmentSlot slot = EquipmentFor(id, ok);
                // stack.isEmpty() || getEquipmentSlotForItem(stack) == slot.
                return invSlot(Game::InventoryIndexFor(slot), [slot](const Game::ItemStack& s) {
                    if (s.IsEmpty()) return true;
                    const auto equippable = s.get(Game::DataComponents::EQUIPPABLE);
                    return equippable && equippable->slot == slot;
                });
            }
            if (id == SR::kCursorOrHorseChest) {
                return Slot{
                    [p] { return Game::ItemStack(p->getCarried()); },
                    [p](const Game::ItemStack& s) { p->setCarried(s); return true; }};
            }
            if (id >= SR::kCraftingOrHorse && id < SR::kCraftingOrHorse + Inv::CRAFT_GRID_SIZE) {
                return invSlot(Inv::CRAFT_GRID_BEGIN + (id - SR::kCraftingOrHorse), nullptr);
            }
            return std::nullopt;
        }

        // LivingEntity.getSlot for a mob (its equipment slots it can use),
        // ItemFrame.getSlot(0) (the framed item).
        std::optional<Slot> MobSlot(Game::Mob& mob, int id) {
            if (auto* frame = dynamic_cast<Game::ItemFrame*>(&mob)) {
                if (id != 0) return std::nullopt;
                return Slot{
                    [frame] { return frame->GetItem(); },
                    [frame](const Game::ItemStack& s) { frame->SetItem(s); return true; }};
            }
            bool ok = false;
            const Game::EquipmentSlot slot = EquipmentFor(id, ok);
            if (!ok || !mob.CanUseSlot(slot)) return std::nullopt;
            Game::Mob* m = &mob;
            return Slot{
                [m, slot] { return m->GetEquipment(slot); },
                [m, slot](const Game::ItemStack& s) { m->SetEquipment(slot, s); return true; }};
        }

        std::vector<Slot> EntitySlots(const SelectedEntity& e, const SR::Range& range) {
            std::vector<Slot> out;
            for (const int id : range.ids) {
                std::optional<Slot> slot;
                switch (e.kind) {
                    case SelectedEntity::Kind::Player:
                        if (e.player) slot = PlayerSlot(*e.player, id);
                        break;
                    case SelectedEntity::Kind::Mob:
                        if (e.mob) slot = MobSlot(*e.mob, id);
                        break;
                    case SelectedEntity::Kind::Item: {
                        // ItemEntity.getSlot(0): its stack; an emptied
                        // stack discards the entity (the manager's sweep).
                        if (id != 0) break;
                        ServerLevel* level = EntityCmd::LevelOf(e.dimension);
                        ItemEntityManager* items = level ? level->Items() : nullptr;
                        const int32_t itemId = e.id;
                        if (!items || !items->Find(itemId)) break;
                        slot = Slot{
                            [items, itemId] {
                                const Game::ItemEntity* item = items->Find(itemId);
                                return item ? item->stack : Game::ItemStack{};
                            },
                            [items, itemId](const Game::ItemStack& s) {
                                Game::ItemEntity* item = items->Find(itemId);
                                if (!item) return false;
                                item->stack = s;
                                item->pendingSpawn = true;   // a full resend: the stack differs
                                item->needsSync = true;
                                return true;
                            }};
                        break;
                    }
                    case SelectedEntity::Kind::Orb:
                        break;
                }
                if (slot) out.push_back(std::move(*slot));
            }
            return out;
        }

        // Container.getSlot (SlotProvider): 0 .. getContainerSize()-1.
        std::vector<Slot> ContainerSlots(Game::IContainer& container, const SR::Range& range) {
            std::vector<Slot> out;
            for (const int id : range.ids) {
                if (id < 0 || id >= container.GetContainerSize()) continue;
                Game::IContainer* c = &container;
                out.push_back(Slot{
                    [c, id] { return c->GetItem(id); },
                    [c, id](const Game::ItemStack& s) { c->SetItem(id, s); c->SetChanged(); return true; }});
            }
            return out;
        }

        // ── Accessors (ItemAccessor: block <pos> | entity <targets>) ─────

        struct Accessor {
            bool isBlock = false;
            glm::ivec3 pos{0};
            Game::IContainer* container = nullptr;
            std::vector<SelectedEntity> entities;
        };

        // Parses an accessor at args[at]; `consumed` is how many tokens it
        // took. `isSource` picks the not-a-container message.
        bool ParseAccessor(const std::vector<std::string>& args, size_t at, const CommandSourceStack& source,
                           bool isSource, Accessor& out, size_t& consumed, std::string& error) {
            if (at >= args.size()) { error = "Expected block or entity"; return false; }
            if (args[at] == "block") {
                if (at + 3 >= args.size()) { error = Tr("argument.pos3d.incomplete"); return false; }
                if (!ParseBlockPos(args[at + 1], args[at + 2], args[at + 3], source, source.rotation, out.pos, error)) {
                    return false;
                }
                out.isBlock = true;
                consumed = 4;
                ServerLevel* level = EntityCmd::LevelOf(source.dimension);
                Game::World* world = level ? level->World() : nullptr;
                Game::BlockEntity* be = world ? world->GetBlockEntity(out.pos) : nullptr;
                out.container = dynamic_cast<Game::IContainer*>(be);
                if (!out.container) {
                    error = Tr(isSource ? "commands.item.source.not_a_container" : "commands.item.target.not_a_container",
                               {std::to_string(out.pos.x), std::to_string(out.pos.y), std::to_string(out.pos.z)});
                    return false;
                }
                return true;
            }
            if (args[at] == "entity") {
                if (at + 1 >= args.size()) { error = "Expected an entity"; return false; }
                if (!ResolveSelector(args[at + 1], SelectorKind::Entities, source, out.entities, error)) return false;
                consumed = 2;
                return true;
            }
            error = "Expected block or entity, found '" + args[at] + "'";
            return false;
        }

        // SlotSourceArgument: a SlotRanges name. (MC also takes a
        // slot_source definition — an id or inline object — which this
        // engine has no registry for.)
        const SR::Range* ParseSlots(const std::string& token, std::string& error) {
            if (const SR::Range* r = SR::Find(token)) return r;
            error = "Unknown slot '" + token + "'";
            return nullptr;
        }

        // ResourceOrIdArgument.lootModifier: inline SNBT (one function or a
        // list), or data/<ns>/item_modifier/<path>.json.
        bool ParseModifier(const std::string& token, nlohmann::json& out, std::string& error) {
            if (!token.empty() && (token[0] == '{' || token[0] == '[')) {
                std::string parseError;
                auto wrapped = Snbt::ParseCompound("{v:" + token + "}", parseError);
                auto value = wrapped ? wrapped->GetTag("v") : nullptr;
                if (!value) { error = parseError.empty() ? "Invalid item modifier" : parseError; return false; }
                out = Game::Anvil::ComponentNbt::NbtToJson(*value);
                return true;
            }
            std::string ns = "minecraft", path = token;
            if (const size_t colon = token.find(':'); colon != std::string::npos) {
                ns = token.substr(0, colon);
                path = token.substr(colon + 1);
            }
            const char* env = std::getenv("MC_DATA_ROOT");
            const std::filesystem::path file =
                std::filesystem::path(env ? env : "data") / ns / "item_modifier" / (path + ".json");
            std::ifstream in(file);
            if (path.empty() || !in) {
                error = "Can't find element '" + ns + ":" + path + "' of type 'minecraft:item_modifier'";
                return false;
            }
            try {
                in >> out;
            } catch (const std::exception& e) {
                error = std::string("Invalid item modifier: ") + e.what();
                return false;
            }
            return true;
        }

        Game::JavaRandom& CommandRandom() {
            static Game::JavaRandom random(static_cast<int64_t>(
                std::chrono::steady_clock::now().time_since_epoch().count()));
            return random;
        }

        // ItemCommands.applyModifier: the function, then limitSize to the
        // stack's max.
        bool ApplyModifier(const nlohmann::json& modifier, Game::ItemStack& stack, const CommandSourceStack& source,
                           std::string& error) {
            Game::ChestLoot::LootLevelContext level;
            level.dimensionId = Game::DimensionToRaw(source.dimension);
            level.origin = source.position;
            if (!Game::ChestLoot::ApplyItemModifier(modifier, stack, CommandRandom(), error, &level)) return false;
            const int max = Game::GetMaxStackSize(stack);
            if (stack.count > max) stack.count = max;
            return true;
        }

        // ItemStack.getDisplayName: "[" + hover name + "]".
        std::string StackDisplayName(const Game::ItemStack& stack) {
            return "[" + Game::GetItemStackHoverName(stack) + "]";
        }

        // ItemProvider: replace (ItemProvider.of), fill (cycle), override
        // (of(...).orElseProvide(EMPTY)).
        enum class Distribution : uint8_t { Replace, Fill, Override };

        struct Provider {
            const std::vector<Game::ItemStack>* items = nullptr;
            Distribution mode = Distribution::Replace;
            size_t next = 0;
            void Restart() { next = 0; }
            bool HasNext() const {
                if (mode == Distribution::Replace) return next < items->size();
                if (mode == Distribution::Fill) return !items->empty();
                return true;
            }
            Game::ItemStack Next() {
                if (mode == Distribution::Fill) return (*items)[next++ % items->size()];
                if (next < items->size()) return (*items)[next++];
                return Game::ItemStack{};   // override's fallback
            }
        };

        // SlotCollection.replaceSlotItems with a tracking ANY_SLOT selector:
        // every slot visited while the provider has items counts as
        // selected; the ones that accepted the item count as replaced.
        int ReplaceSlots(std::vector<Slot>& slots, Provider& provider, int& selected) {
            int replaced = 0;
            for (Slot& slot : slots) {
                if (!provider.HasNext()) break;
                ++selected;
                if (slot.set(provider.Next())) ++replaced;
            }
            return replaced;
        }

        struct Feedback {
            int nonZero = 0;
            int total = 0;
            std::string onlyNonZeroName;
            void Track(const std::string& name, int value) {
                total += value;
                if (value != 0 && ++nonZero == 1) onlyNonZeroName = name;
            }
        };

    } // namespace

    void ItemCommand::Register(CommandDispatcher& dispatcher) {
        namespace Cmd = Game::Cmd;
        // A target or source: `block <pos>` | `entity <targets>`, each
        // followed by `then`.
        const auto accessor = [](const char* argName, const Cmd::Node& then) {
            std::vector<Cmd::Node> out;
            out.push_back(Cmd::Literal("block").Then(Cmd::Argument(argName, Cmd::Arg::BlockPos).Then(then)));
            out.push_back(Cmd::Literal("entity").Then(Cmd::Argument(argName, Cmd::Arg::Entities).Then(then)));
            return out;
        };
        // `from <source> <sourceSlots> [<modifier>]`.
        const Cmd::Node sourceSlots = Cmd::Argument("sourceSlots", Cmd::Arg::SlotRange).Executes()
            .Then(Cmd::Argument("modifier", Cmd::Arg::ItemModifier).Executes());
        Cmd::Node from = Cmd::Literal("from");
        from.Then(accessor("source", sourceSlots));
        const Cmd::Node with = Cmd::Literal("with")
            .Then(Cmd::Argument("item", Cmd::Arg::Item).Executes()
                .Then(Cmd::Argument("count", Cmd::Arg::Integer).Suggests({"1", "16", "64"}).Executes()));
        const Cmd::Node setSlots = Cmd::Argument("slots", Cmd::Arg::SlotRange).Then(with).Then(from);
        const Cmd::Node modifySlots = Cmd::Argument("slots", Cmd::Arg::SlotRange)
            .Then(Cmd::Argument("modifier", Cmd::Arg::ItemModifier).Executes());

        Cmd::Node root = Cmd::Root();
        for (const char* action : {"replace", "fill", "override"}) {
            root.Then(Cmd::Literal(action).Then(accessor("target", setSlots)));
        }
        root.Then(Cmd::Literal("modify").Then(accessor("target", modifySlots)));
        dispatcher.RegisterCommand("item", ItemCommand::Execute, std::move(root));
    }

    void ItemCommand::Execute(const CommandSourceStack& source,
                              const std::vector<std::string>& args,
                              ServerConnection& connection,
                              PlayerSessionManager& sessionManager) {
        (void)sessionManager;
        const auto usage = [&] {
            Failure(connection, "Unknown or incomplete command, see below for error");
            for (const std::string& line : g_integratedServer->GetCommandDispatcher().GetUsageLines("item")) {
                Failure(connection, line);
            }
        };
        if (args.size() < 4) { usage(); return; }
        const std::string& action = args[0];
        const bool modify = action == "modify";
        Distribution mode = Distribution::Replace;
        if (action == "fill") mode = Distribution::Fill;
        else if (action == "override") mode = Distribution::Override;
        else if (action != "replace" && !modify) { usage(); return; }

        std::string error;
        Accessor target;
        size_t consumed = 0;
        if (!ParseAccessor(args, 1, source, /*isSource=*/false, target, consumed, error)) {
            Failure(connection, error);
            return;
        }
        size_t at = 1 + consumed;
        if (at >= args.size()) { usage(); return; }
        const SR::Range* slots = ParseSlots(args[at], error);
        if (!slots) { Failure(connection, error); return; }
        ++at;

        // The per-target slot collections (EntityItemAccessor concatenates
        // them; BlockItemAccessor has the one container).
        const auto targetSlots = [&](size_t index) -> std::vector<Slot> {
            return target.isBlock ? ContainerSlots(*target.container, *slots)
                                  : EntitySlots(target.entities[index], *slots);
        };
        const size_t targetCount = target.isBlock ? 1 : target.entities.size();
        const auto targetName = [&](size_t index) {
            return target.isBlock ? std::string() : EntityCmd::DisplayName(target.entities[index]);
        };
        const auto noSuchSlot = [&] { Failure(connection, Tr("commands.item.target.no_such_slot", {slots->name})); };

        // ── modify <target> <slots> <modifier> ───────────────────────────
        if (modify) {
            if (at + 1 != args.size()) { usage(); return; }
            nlohmann::json modifier;
            if (!ParseModifier(args[at], modifier, error)) { Failure(connection, error); return; }
            int selected = 0;
            Feedback feedback;
            for (size_t i = 0; i < targetCount; ++i) {
                std::vector<Slot> list = targetSlots(i);
                int updated = 0;
                for (Slot& slot : list) {
                    Game::ItemStack stack = slot.get();
                    if (stack.IsEmpty()) continue;   // SlotSelector.NON_EMPTY_SLOTS
                    ++selected;
                    if (!ApplyModifier(modifier, stack, source, error)) { Failure(connection, error); return; }
                    if (slot.set(stack)) ++updated;
                }
                feedback.Track(targetName(i), updated);
            }
            if (selected == 0) { noSuchSlot(); return; }
            if (feedback.nonZero == 0) { Failure(connection, Tr("commands.item.target.failed")); return; }
            if (target.isBlock) {
                Success(source, connection, /*broadcast=*/true, Tr("commands.item.block.modify.success",
                                       {std::to_string(feedback.total), std::to_string(target.pos.x),
                                        std::to_string(target.pos.y), std::to_string(target.pos.z)}));
            } else if (feedback.nonZero == 1) {
                Success(source, connection, /*broadcast=*/true, Tr("commands.item.entity.modify.success.single",
                                       {std::to_string(feedback.total), feedback.onlyNonZeroName}));
            } else {
                Success(source, connection, /*broadcast=*/true, Tr("commands.item.entity.modify.success.multiple",
                                       {std::to_string(feedback.nonZero)}));
            }
            return;
        }

        // ── the items: `with <item> [<count>]` or `from <source> …` ───────
        std::vector<Game::ItemStack> items;
        std::optional<Game::ItemStack> knownItem;
        if (at < args.size() && args[at] == "with") {
            if (at + 2 != args.size() && at + 3 != args.size()) { usage(); return; }
            Game::ItemStack stack;
            if (!ParseItemArgument(args[at + 1], stack, error)) { Failure(connection, error); return; }
            int count = 1;
            if (at + 3 == args.size()) {
                if (!EntityCmd::ParseInt(args[at + 2], 1, 99, count, error)) { Failure(connection, error); return; }
                // ItemInput.createItemStack(count, true): ERROR_STACK_TOO_BIG.
                const int max = Game::GetMaxStackSize(stack);
                if (count > max) {
                    Failure(connection, Tr("arguments.item.overstacked",
                                           {Game::GetItemStackHoverName(stack), std::to_string(max)}));
                    return;
                }
            }
            stack.count = count;
            items.push_back(stack);
            knownItem = stack;
        } else if (at < args.size() && args[at] == "from") {
            Accessor sourceAccessor;
            size_t sourceConsumed = 0;
            if (!ParseAccessor(args, at + 1, source, /*isSource=*/true, sourceAccessor, sourceConsumed, error)) {
                Failure(connection, error);
                return;
            }
            size_t s = at + 1 + sourceConsumed;
            if (s >= args.size() || s + 2 < args.size()) { usage(); return; }
            const SR::Range* sourceSlots = ParseSlots(args[s], error);
            if (!sourceSlots) { Failure(connection, error); return; }
            std::optional<nlohmann::json> modifier;
            if (s + 1 < args.size()) {
                nlohmann::json m;
                if (!ParseModifier(args[s + 1], m, error)) { Failure(connection, error); return; }
                modifier = std::move(m);
            }
            // ItemCommands.getItems: copies of every source slot (empty ones
            // too), each through the modifier.
            std::vector<Slot> sourceList;
            if (sourceAccessor.isBlock) {
                sourceList = ContainerSlots(*sourceAccessor.container, *sourceSlots);
            } else {
                for (const SelectedEntity& e : sourceAccessor.entities) {
                    std::vector<Slot> part = EntitySlots(e, *sourceSlots);
                    for (Slot& p : part) sourceList.push_back(std::move(p));
                }
            }
            for (Slot& slot : sourceList) {
                Game::ItemStack copy = slot.get();
                if (modifier && !copy.IsEmpty() && !ApplyModifier(*modifier, copy, source, error)) {
                    Failure(connection, error);
                    return;
                }
                items.push_back(std::move(copy));
            }
            if (items.empty()) {
                Failure(connection, Tr("commands.item.source.no_such_slot", {sourceSlots->name}));
                return;
            }
        } else {
            usage();
            return;
        }

        // ── setItems ─────────────────────────────────────────────────────
        Provider provider{&items, mode};
        int selected = 0;
        Feedback feedback;
        for (size_t i = 0; i < targetCount; ++i) {
            provider.Restart();
            std::vector<Slot> list = targetSlots(i);
            feedback.Track(targetName(i), ReplaceSlots(list, provider, selected));
        }
        if (selected == 0) { noSuchSlot(); return; }
        if (feedback.nonZero == 0) {
            if (knownItem) Failure(connection, Tr("commands.item.target.failed.known_item", {StackDisplayName(*knownItem)}));
            else           Failure(connection, Tr("commands.item.target.failed"));
            return;
        }
        const std::string total = std::to_string(feedback.total);
        if (target.isBlock) {
            const std::string x = std::to_string(target.pos.x), y = std::to_string(target.pos.y),
                              z = std::to_string(target.pos.z);
            if (knownItem) Success(source, connection, /*broadcast=*/true, Tr("commands.item.block.replace.success.known_item",
                                                  {total, x, y, z, StackDisplayName(*knownItem)}));
            else           Success(source, connection, /*broadcast=*/true, Tr("commands.item.block.replace.success", {total, x, y, z}));
        } else if (feedback.nonZero == 1) {
            if (knownItem) Success(source, connection, /*broadcast=*/true, Tr("commands.item.entity.replace.success.single.known_item",
                                                  {total, feedback.onlyNonZeroName, StackDisplayName(*knownItem)}));
            else           Success(source, connection, /*broadcast=*/true, Tr("commands.item.entity.replace.success.single",
                                                  {total, feedback.onlyNonZeroName}));
        } else {
            const std::string n = std::to_string(feedback.nonZero);
            if (knownItem) Success(source, connection, /*broadcast=*/true, Tr("commands.item.entity.replace.success.multiple.known_item",
                                                  {n, StackDisplayName(*knownItem)}));
            else           Success(source, connection, /*broadcast=*/true, Tr("commands.item.entity.replace.success.multiple", {n}));
        }
    }

} // namespace Server
