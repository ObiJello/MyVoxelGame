// File: src/server/commands/HealCommand.cpp
#include "HealCommand.hpp"
#include "EntitySelector.hpp"
#include "../network/ServerConnection.hpp"
#include "../session/PlayerSessionManager.hpp"
#include "../player/ServerPlayer.hpp"
#include "common/entity/Mob.hpp"

namespace Server {

    void HealCommand::Register(CommandDispatcher& dispatcher) {
        dispatcher.RegisterCommand("heal", HealCommand::Execute);
    }

    void HealCommand::Execute(const CommandSourceStack& source,
                              const std::vector<std::string>& args,
                              ServerConnection& connection,
                              PlayerSessionManager& sessionManager) {
        (void)sessionManager;
        // Like /kill: a selector or a bare name, the sender when nothing is
        // given.
        std::vector<SelectedEntity> targets;
        std::string error;
        const std::string token = args.empty() ? "@s" : args[0];
        if (!ResolveSelector(token, SelectorKind::Entities, source, targets, error)) {
            connection.SendChatMessage(error, 1);
            return;
        }

        int healed = 0;
        for (const SelectedEntity& target : targets) {
            switch (target.kind) {
                case SelectedEntity::Kind::Player: {
                    ServerPlayer* player = target.player;
                    if (!player || player->isDead()) break;
                    // Health back to the maximum through heal() (it clamps),
                    // the hunger bar and its saturation to a fresh spawn's
                    // (FoodData's 20 / 5), exhaustion cleared so the bar does
                    // not start dropping again at once.
                    player->heal(20.0f);
                    player->getFoodData().setFoodLevel(20);
                    player->getFoodData().setSaturation(5.0f);
                    player->getFoodData().setExhaustion(0.0f);
                    ++healed;
                    break;
                }
                case SelectedEntity::Kind::Mob: {
                    Game::Mob* mob = target.mob;
                    if (!mob || !mob->IsAlive()) break;
                    mob->SetHealth(mob->GetMaxHealth());
                    mob->SetRemainingFireTicks(0);
                    ++healed;
                    break;
                }
                default:
                    break;   // a dropped item has no health
            }
        }

        if (healed == 0) {
            connection.SendChatMessage("Nothing to heal", 1);
        } else if (healed == 1 && !targets.empty() && targets.front().kind == SelectedEntity::Kind::Player &&
                   targets.front().player == source.sender) {
            connection.SendChatMessage("Healed", 1);
        } else {
            connection.SendChatMessage("Healed " + std::to_string(healed) + " " +
                                       (healed == 1 ? "entity" : "entities"), 1);
        }
    }

} // namespace Server
