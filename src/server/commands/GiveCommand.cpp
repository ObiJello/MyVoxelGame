// File: src/server/commands/GiveCommand.cpp
#include "GiveCommand.hpp"
#include "EntitySelector.hpp"
#include "ItemArgument.hpp"
#include "../IntegratedServer.hpp"
#include "../entity/ItemEntityManager.hpp"
#include "../level/ServerLevel.hpp"
#include "../network/ServerConnection.hpp"
#include "../player/ServerPlayer.hpp"
#include "../session/PlayerSession.hpp"
#include "../session/PlayerSessionManager.hpp"
#include "common/core/Log.hpp"
#include "common/core/Mth.hpp"
#include "common/entity/ItemEntity.hpp"
#include "common/physics/Physics.hpp"
#include "common/sound/LevelSound.hpp"
#include "common/sound/SoundEvents.hpp"

#include <algorithm>
#include <cstdlib>
#include <random>

namespace Server {

    namespace {

        // MC LivingEntity.createItemStackToDrop(stack, randomly=false,
        // thrownFromHand=false): out of the player's eyes along their look.
        Game::ItemEntity* DropAtPlayer(ServerPlayer& player, const Game::ItemStack& stack) {
            if (!g_integratedServer) return nullptr;
            ServerLevel* level = g_integratedServer->GetLevel(Game::DimensionFromRaw(player.getDimensionId()));
            ItemEntityManager* items = level ? level->Items() : nullptr;
            if (!items) return nullptr;
            const glm::dvec3 eye = player.getPosition() +
                glm::dvec3(0.0, Game::PlayerPhysics::EYE_HEIGHT_STANDING, 0.0);
            const glm::dvec3 forward(Game::Mth::ViewVector(player.getPitch(), player.getYaw()));
            const int32_t id = items->DropFromPlayer(eye, forward, stack);
            return id != 0 ? items->Find(id) : nullptr;
        }

        std::string DisplayName(const Game::ItemStack& stack) {
            // MC ItemStack.getDisplayName: the hover name in brackets.
            return "[" + Game::GetItemStackHoverName(stack) + "]";
        }

    } // namespace

    void GiveCommand::Register(CommandDispatcher& dispatcher) {
        namespace Cmd = Game::Cmd;
        dispatcher.RegisterCommand("give", GiveCommand::Execute,
            Cmd::Root().Then(Cmd::Argument("targets", Cmd::Arg::Players)
                .Then(Cmd::Argument("item", Cmd::Arg::Item).Executes()
                    .Then(Cmd::Argument("count", Cmd::Arg::Integer).Suggests({"1", "16", "64"}).Executes()))));
    }

    void GiveCommand::Execute(const CommandSourceStack& source,
                              const std::vector<std::string>& args,
                              ServerConnection& connection,
                              PlayerSessionManager& /*sessionManager*/) {
        if (args.size() < 2 || args.size() > 3) {
            connection.SendChatMessage("Usage: /give <targets> <item> [<count>]", 1);
            return;
        }
        std::string error;
        std::vector<SelectedEntity> targets;
        if (!ResolveSelector(args[0], SelectorKind::Players, source, targets, error)) {
            connection.SendChatMessage(error, 1);
            return;
        }
        Game::ItemStack prototype;
        if (!ParseItemArgument(args[1], prototype, error)) {
            connection.SendChatMessage(error, 1);
            return;
        }
        int count = 1;
        if (args.size() == 3) {
            char* end = nullptr;
            const long v = std::strtol(args[2].c_str(), &end, 10);
            if (!end || *end != '\0' || args[2].empty()) {
                connection.SendChatMessage("Invalid integer '" + args[2] + "'", 1);
                return;
            }
            // IntegerArgumentType.integer(1).
            if (v < 1) {
                connection.SendChatMessage("Integer must not be less than 1, found " + args[2], 1);
                return;
            }
            count = static_cast<int>(std::min<long>(v, 1L << 30));
        }

        // prototype.getMaxStackSize() — a max_stack_size patch included, so
        // `stick[max_stack_size=1] 5` gives five separate sticks.
        const int maxStackSize = std::max(1, Game::GetMaxStackSize(prototype));
        const int maxAllowed = maxStackSize * kMaxAllowedItemStacks;
        if (count > maxAllowed) {
            // commands.give.failed.toomanyitems
            connection.SendChatMessage("Can't give more than " + std::to_string(maxAllowed) + " of " +
                                       DisplayName(prototype), 1);
            return;
        }

        static std::mt19937 rng{std::random_device{}()};
        std::uniform_real_distribution<float> unit(0.0f, 1.0f);
        int given = 0;
        const SelectedEntity* only = nullptr;
        for (const SelectedEntity& target : targets) {
            ServerPlayer* player = target.player;
            if (!player) continue;
            int remaining = count;
            while (remaining > 0) {
                const int size = std::min(maxStackSize, remaining);
                remaining -= size;
                Game::ItemStack copy = prototype;
                copy.count = size;
                const int leftover = player->getInventory().AddStack(copy);
                if (leftover == 0) {
                    // Everything fit: MC's fake item (never collectable,
                    // gone next tick) and the pickup pop.
                    if (Game::ItemEntity* fake = DropAtPlayer(*player, prototype)) {
                        fake->pickupDelay = 32767;
                        fake->age = Game::ItemEntity::kLifetimeTicks - 1;
                    }
                    if (Game::Sound::ServerSoundSink* sink = Game::Sound::GetServerSink()) {
                        const float pitch = ((unit(rng) - unit(rng)) * 0.7f + 1.0f) * 2.0f;
                        sink->PlaySound(Game::DimensionFromRaw(player->getDimensionId()), nullptr,
                                        player->getPosition(), Game::SoundEvents::ITEM_PICKUP,
                                        Game::SoundSource::Players, 0.2f, pitch, Game::Sound::NextSeed());
                    }
                } else {
                    // What did not fit lies at the player's feet, theirs alone
                    // and collectable at once (setNoPickUpDelay + setTarget).
                    Game::ItemStack drop = prototype;
                    drop.count = leftover;
                    if (Game::ItemEntity* item = DropAtPlayer(*player, drop)) {
                        item->pickupDelay = 0;
                        item->targetPlayerId = player->getPlayerId();
                    }
                }
            }
            // The inventory diff goes out with the session's next container
            // sync (MC containerMenu.broadcastChanges).
            ++given;
            only = &target;
        }

        // commands.give.success.single / .multiple
        if (given == 1 && only) {
            source.SendSuccess(connection, "Gave " + std::to_string(count) + " " + DisplayName(prototype) +
                                       " to " + only->name, true);
        } else if (given > 1) {
            source.SendSuccess(connection, "Gave " + std::to_string(count) + " " + DisplayName(prototype) +
                                       " to " + std::to_string(given) + " players", true);
        }
        Log::Info("[GiveCommand] %s gave %d x %s to %d player(s)", source.sender ? source.sender->getName().c_str() : "?",
                  count, Game::GetItemStackHoverName(prototype).c_str(), given);
    }

} // namespace Server
