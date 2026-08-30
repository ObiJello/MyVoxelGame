// File: src/server/commands/KillCommand.cpp
#include "KillCommand.hpp"
#include "EntitySelector.hpp"
#include "../network/ServerConnection.hpp"
#include "../session/PlayerSessionManager.hpp"
#include "../session/PlayerSession.hpp"
#include "../player/ServerPlayer.hpp"
#include "common/core/Log.hpp"
#include "common/entity/LivingEntity.hpp"
#include "common/entity/Mob.hpp"
#include "common/entity/ItemEntity.hpp"
#include "../IntegratedServer.hpp"
#include "../entity/ItemEntityManager.hpp"

#include <algorithm>

namespace Server {

    void KillCommand::Register(CommandDispatcher& dispatcher) {
        dispatcher.RegisterCommand("kill", KillCommand::Execute);
    }

    void KillCommand::Execute(ServerPlayer& sender,
                              const std::vector<std::string>& args,
                              ServerConnection& connection,
                              PlayerSessionManager& sessionManager) {
        // MC KillCommand takes an EntityArgument.entities(), so every selector
        // form works and a bare player name is accepted too. This used to
        // hand-roll a name lookup and could only ever kill players — `/kill @e`
        // failed with "Player not found: @e".
        CommandSource source;
        source.sender   = &sender;
        source.sessions = &sessionManager;
        source.position = sender.getPosition();

        std::vector<SelectedEntity> targets;
        std::string error;
        // No argument means MC's `/kill` overload with no target: the sender.
        const std::string token = args.empty() ? "@s" : args[0];
        if (!ResolveSelector(token, SelectorKind::Entities, source, targets, error)) {
            connection.SendChatMessage(error, 1);
            return;
        }

        // DELIBERATE DIVERGENCE, requested: vanilla's `@e` includes players, so
        // `/kill @e` in MC kills you along with everything else. Here the
        // SENDER is always spared, because the overwhelmingly common use is
        // "clear the world around me" and doing that should not cost you your
        // inventory. Every other player is still killed, and `/kill @s` or
        // `/kill <yourname>` still works if you actually mean yourself.
        const bool sparedSelf = [&] {
            if (token == "@s") return false;   // asked for yourself explicitly
            const size_t before = targets.size();
            targets.erase(std::remove_if(targets.begin(), targets.end(),
                [&](const SelectedEntity& e) { return e.player == &sender; }),
                targets.end());
            return targets.size() != before;
        }();

        int killed = 0;
        for (const SelectedEntity& target : targets) {
            switch (target.kind) {
                case SelectedEntity::Kind::Player: {
                    if (!target.player) break;
                    // MC /kill uses damageSources().genericKill(), which
                    // BYPASSES_INVULNERABILITY — a creative player dies too.
                    // ServerPlayer::damage is gamemode-gated, so say so rather
                    // than silently no-op.
                    if (target.player->getGameMode() == GameMode::CREATIVE ||
                        target.player->getGameMode() == GameMode::SPECTATOR) {
                        connection.SendChatMessage(
                            target.player->getName() + " is invulnerable (creative/spectator)", 1);
                        break;
                    }
                    target.player->damage(1000.0f, DamageSource::VOID_DAMAGE);
                    ++killed;
                    break;
                }
                case SelectedEntity::Kind::Mob: {
                    if (!target.mob) break;
                    // Kill rather than discard, so loot, XP and the death
                    // animation all happen — MC's /kill is a damage source, not
                    // a removal.
                    target.mob->Hurt(Game::MobDamageSource::Generic, 1.0e6f, nullptr);
                    // A mob that refuses damage entirely (a projectile, primed
                    // TNT) still has to go, or `/kill @e` leaves the things it
                    // most obviously should clear.
                    if (!target.mob->IsRemoved()) target.mob->Discard();
                    ++killed;
                    break;
                }
                case SelectedEntity::Kind::Item: {
                    // Items are addressed by id — see the note in
                    // EntitySelector.hpp about why no pointer is carried.
                    // Same route the selector took to find them.
                    if (auto* items = g_integratedServer
                            ? g_integratedServer->GetItemEntities() : nullptr) {
                        if (Game::ItemEntity* item = items->Find(target.id)) {
                            item->stack.Clear();   // emptied -> reaped next tick
                            ++killed;
                        }
                    }
                    break;
                }
            }
        }

        if (killed == 0) {
            connection.SendChatMessage("No entities were killed", 1);
        } else if (killed == 1 && !targets.empty()) {
            connection.SendChatMessage("Killed " + targets.front().name, 1);
        } else {
            connection.SendChatMessage("Killed " + std::to_string(killed) + " entities" +
                                       (sparedSelf ? " (you were spared)" : ""), 1);
        }
        Log::Info("[KillCommand] %s killed %d entit%s via '%s'",
                  sender.getName().c_str(), killed, killed == 1 ? "y" : "ies",
                  token.c_str());
    }

} // namespace Server
