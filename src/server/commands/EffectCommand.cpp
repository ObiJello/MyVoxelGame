// File: src/server/commands/EffectCommand.cpp
#include "EffectCommand.hpp"
#include "EntitySelector.hpp"
#include "../network/ServerConnection.hpp"
#include "../session/PlayerSessionManager.hpp"
#include "../player/ServerPlayer.hpp"
#include "common/core/Log.hpp"
#include "common/entity/LivingEntity.hpp"
#include "common/entity/Mob.hpp"
#include "common/entity/effect/MobEffects.hpp"

#include <cctype>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

namespace Server {

    namespace {

        // MC CommandResponseTracker over LivingEntity targets, NON_ZERO
        // element type: counts only the targets the command changed, and
        // remembers the single one when there was exactly one.
        struct ResponseTracker {
            int         nonZeroCount = 0;
            std::string onlyNonZeroName;

            void Track(const std::string& name, bool changed) {
                if (!changed) return;
                if (++nonZeroCount == 1) onlyNonZeroName = name;
                else onlyNonZeroName.clear();
            }
        };

        // Entity.getDisplayName: a player's name; a mob's custom name (a name
        // tag), else its translated type name (en_us "entity.minecraft.<id>"
        // is the slug title-cased for every vanilla type this engine has).
        std::string DisplayName(const SelectedEntity& target) {
            if (target.kind == SelectedEntity::Kind::Player) return target.name;
            if (target.mob && target.mob->HasCustomName()) return *target.mob->GetCustomName();
            std::string out;
            bool upper = true;
            for (char c : target.typeSlug) {
                if (c == '_') { out += ' '; upper = true; continue; }
                out += upper ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c;
                upper = false;
            }
            return out;
        }

        // The LivingEntity MC's `entity instanceof LivingEntity` finds: a mob,
        // or the player's view (whose effect list IS the ServerPlayer's).
        Game::LivingEntity* LivingOf(const SelectedEntity& target) {
            switch (target.kind) {
                case SelectedEntity::Kind::Player:
                    return target.player ? target.player->effectEntity() : nullptr;
                case SelectedEntity::Kind::Mob:
                    return target.mob;
                default:
                    return nullptr;   // a dropped item is not a LivingEntity
            }
        }

        // Brigadier IntegerArgumentType.integer(min, max).
        bool ParseInt(const std::string& token, int min, int max, int& out, std::string& error) {
            if (token.empty()) { error = "Expected integer"; return false; }
            char* end = nullptr;
            const long v = std::strtol(token.c_str(), &end, 10);
            if (!end || *end != '\0' ||
                token.find_first_not_of("+-0123456789") != std::string::npos) {
                error = "Invalid integer '" + token + "'";
                return false;
            }
            if (v < min) {
                error = "Integer must not be less than " + std::to_string(min) + ", found " + std::to_string(v);
                return false;
            }
            if (v > max) {
                error = "Integer must not be more than " + std::to_string(max) + ", found " + std::to_string(v);
                return false;
            }
            out = static_cast<int>(v);
            return true;
        }

        // Brigadier BoolArgumentType.bool().
        bool ParseBool(const std::string& token, bool& out, std::string& error) {
            if (token == "true")  { out = true;  return true; }
            if (token == "false") { out = false; return true; }
            error = "Invalid boolean, expected 'true' or 'false' but found '" + token + "'";
            return false;
        }

        // ResourceArgument.resource(context, Registries.MOB_EFFECT).
        bool ParseEffect(const std::string& token, Game::MobEffectId& out, std::string& error) {
            if (Game::ParseEffectId(token, out)) return true;
            const std::string id = token.find(':') == std::string::npos ? "minecraft:" + token : token;
            error = "Can't find element '" + id + "' of type 'minecraft:mob_effect'";
            return false;
        }

        // EffectCommands.computeDurationInTicks.
        int ComputeDurationInTicks(std::optional<int> seconds, Game::MobEffectId effect) {
            const bool instantaneous = Game::IsInstantenousEffect(effect);
            if (seconds) {
                if (instantaneous) return *seconds;
                return *seconds == -1 ? -1 : *seconds * 20;
            }
            return instantaneous ? 1 : 600;
        }

        void GiveEffect(const CommandSourceStack& source, const std::vector<SelectedEntity>& targets,
                        Game::MobEffectId effect, std::optional<int> seconds, int amplifier,
                        bool particles, ServerConnection& connection) {
            const int duration = ComputeDurationInTicks(seconds, effect);
            ResponseTracker tracker;
            Game::Entity* sourceEntity = source.sender ? source.sender->effectEntity() : nullptr;
            for (const SelectedEntity& target : targets) {
                Game::LivingEntity* living = LivingOf(target);
                if (!living) continue;
                // new MobEffectInstance(effect, duration, amplifier, false, particles)
                const Game::MobEffectInstance instance(effect, duration, amplifier,
                                                       /*ambient=*/false, particles);
                tracker.Track(DisplayName(target), living->AddEffect(instance, sourceEntity));
            }
            const std::string effectName = Game::GetEffectDisplayName(effect);
            if (tracker.nonZeroCount == 0) {
                connection.SendChatMessage(
                    "Unable to apply this effect (target is either immune to effects, or has something stronger)", 1);
            } else if (tracker.nonZeroCount == 1) {
                connection.SendChatMessage("Applied effect " + effectName + " to " + tracker.onlyNonZeroName, 1);
            } else {
                connection.SendChatMessage("Applied effect " + effectName + " to " +
                                           std::to_string(tracker.nonZeroCount) + " targets", 1);
            }
        }

        void ClearEffects(const std::vector<SelectedEntity>& targets, ServerConnection& connection) {
            ResponseTracker tracker;
            for (const SelectedEntity& target : targets) {
                Game::LivingEntity* living = LivingOf(target);
                if (!living) continue;
                tracker.Track(DisplayName(target), living->RemoveAllEffects());
            }
            if (tracker.nonZeroCount == 0) {
                connection.SendChatMessage("Target has no effects to remove", 1);
            } else if (tracker.nonZeroCount == 1) {
                connection.SendChatMessage("Removed every effect from " + tracker.onlyNonZeroName, 1);
            } else {
                connection.SendChatMessage("Removed every effect from " +
                                           std::to_string(tracker.nonZeroCount) + " targets", 1);
            }
        }

        void ClearEffect(const std::vector<SelectedEntity>& targets, Game::MobEffectId effect,
                         ServerConnection& connection) {
            ResponseTracker tracker;
            for (const SelectedEntity& target : targets) {
                Game::LivingEntity* living = LivingOf(target);
                if (!living) continue;
                tracker.Track(DisplayName(target), living->RemoveEffect(effect));
            }
            const std::string effectName = Game::GetEffectDisplayName(effect);
            if (tracker.nonZeroCount == 0) {
                connection.SendChatMessage("Target doesn't have the requested effect", 1);
            } else if (tracker.nonZeroCount == 1) {
                connection.SendChatMessage("Removed effect " + effectName + " from " + tracker.onlyNonZeroName, 1);
            } else {
                connection.SendChatMessage("Removed effect " + effectName + " from " +
                                           std::to_string(tracker.nonZeroCount) + " targets", 1);
            }
        }

        void SendUsage(ServerConnection& connection) {
            connection.SendChatMessage("Unknown or incomplete command, see below for error", 1);
            connection.SendChatMessage(
                "/effect give <targets> <effect> [<seconds>|infinite] [<amplifier>] [<hideParticles>]", 1);
            connection.SendChatMessage("/effect clear [<targets>] [<effect>]", 1);
        }

    } // namespace

    void EffectCommand::Register(CommandDispatcher& dispatcher) {
        dispatcher.RegisterCommand("effect", EffectCommand::Execute);
    }

    void EffectCommand::Execute(const CommandSourceStack& source,
                                const std::vector<std::string>& args,
                                ServerConnection& connection,
                                PlayerSessionManager& sessionManager) {
        (void)sessionManager;
        if (args.empty()) { SendUsage(connection); return; }
        const std::string& sub = args[0];
        std::string error;

        if (sub == "clear") {
            if (args.size() > 3) {
                connection.SendChatMessage("Incorrect argument for command", 1);
                return;
            }
            // `clear` alone is the source entity (getEntityOrException).
            const std::string token = args.size() >= 2 ? args[1] : "@s";
            std::vector<SelectedEntity> targets;
            if (!ResolveSelector(token, SelectorKind::Entities, source, targets, error)) {
                connection.SendChatMessage(error, 1);
                return;
            }
            if (args.size() == 3) {
                Game::MobEffectId effect;
                if (!ParseEffect(args[2], effect, error)) {
                    connection.SendChatMessage(error, 1);
                    return;
                }
                ClearEffect(targets, effect, connection);
            } else {
                ClearEffects(targets, connection);
            }
            return;
        }

        if (sub == "give") {
            if (args.size() < 3) { SendUsage(connection); return; }
            if (args.size() > 6) {
                connection.SendChatMessage("Incorrect argument for command", 1);
                return;
            }
            std::vector<SelectedEntity> targets;
            if (!ResolveSelector(args[1], SelectorKind::Entities, source, targets, error)) {
                connection.SendChatMessage(error, 1);
                return;
            }
            Game::MobEffectId effect;
            if (!ParseEffect(args[2], effect, error)) {
                connection.SendChatMessage(error, 1);
                return;
            }

            std::optional<int> seconds;       // null = the default duration
            int  amplifier = 0;
            bool particles = true;
            if (args.size() >= 4) {
                if (args[3] == "infinite") {
                    seconds = -1;
                } else {
                    int s = 0;
                    if (!ParseInt(args[3], 1, 1000000, s, error)) {
                        connection.SendChatMessage(error, 1);
                        return;
                    }
                    seconds = s;
                }
            }
            if (args.size() >= 5 && !ParseInt(args[4], 0, 255, amplifier, error)) {
                connection.SendChatMessage(error, 1);
                return;
            }
            if (args.size() >= 6) {
                bool hideParticles = false;
                if (!ParseBool(args[5], hideParticles, error)) {
                    connection.SendChatMessage(error, 1);
                    return;
                }
                particles = !hideParticles;
            }
            GiveEffect(source, targets, effect, seconds, amplifier, particles, connection);
            Log::Info("[EffectCommand] give %s %s", args[1].c_str(), Game::GetEffectName(effect));
            return;
        }

        connection.SendChatMessage("Incorrect argument for command", 1);
    }

} // namespace Server
