// File: src/server/commands/TagCommand.cpp
#include "TagCommand.hpp"
#include "EntityCommandUtil.hpp"
#include "../IntegratedServer.hpp"
#include "../entity/ExperienceOrbManager.hpp"
#include "../entity/ItemEntityManager.hpp"
#include "../entity/MobManager.hpp"
#include "../level/NamedEntityIndex.hpp"
#include "../level/ServerLevel.hpp"
#include "../player/ServerPlayer.hpp"
#include "../session/PlayerSession.hpp"
#include "../session/PlayerSessionManager.hpp"

#include "common/entity/EntityTags.hpp"
#include "common/entity/Mob.hpp"

#include <algorithm>
#include <set>

namespace Server {

    namespace {

        // The tree, with `remove`'s suggestions (MC: the targets' tags; the
        // client cannot see entities' tags, so every tag a loaded entity
        // carries is offered).
        Game::Cmd::Node BuildTree(const std::vector<std::string>& knownTags) {
            namespace Cmd = Game::Cmd;
            return Cmd::Root()
                .Then(Cmd::Argument("targets", Cmd::Arg::Entities)
                    .Then(Cmd::Literal("add")
                        .Then(Cmd::Argument("name", Cmd::Arg::Word).Executes()))
                    .Then(Cmd::Literal("remove")
                        .Then(Cmd::Argument("name", Cmd::Arg::Word).Suggests(knownTags).Executes()))
                    .Then(Cmd::Literal("list").Executes()));
        }

        std::vector<std::string> CollectKnownTags() {
            std::set<std::string> all;
            const auto take = [&all](const Game::EntityTags& tags) {
                for (const std::string& t : tags.All()) all.insert(t);
            };
            if (!g_integratedServer) return {};
            if (PlayerSessionManager* sessions = g_integratedServer->GetSessionManager()) {
                for (const auto& session : sessions->GetAllSessions()) {
                    if (session && session->GetPlayer()) take(session->GetPlayer()->getTags());
                }
            }
            for (const Game::DimensionId d : Game::kAllDimensions) {
                ServerLevel* level = g_integratedServer->GetLevel(d);
                if (!level) continue;
                if (MobManager* mobs = level->Mobs()) {
                    for (const auto& [id, mob] : mobs->All()) {
                        (void)id;
                        if (mob) take(mob->Tags());
                    }
                }
                if (ItemEntityManager* items = level->Items()) {
                    for (const auto& [id, item] : items->All()) { (void)id; take(item.tags); }
                }
                if (ExperienceOrbManager* orbs = level->Orbs()) {
                    for (const auto& [id, orb] : orbs->All()) { (void)id; take(orb.tags); }
                }
            }
            return {all.begin(), all.end()};
        }

        std::vector<std::string>& LastSuggested() {
            static std::vector<std::string> s;
            return s;
        }

    } // namespace

    void TagCommand::Register(CommandDispatcher& dispatcher) {
        LastSuggested().clear();
        dispatcher.RegisterCommand("tag", TagCommand::Execute, BuildTree({}));
    }

    void TagCommand::RefreshSuggestions() {
        if (!g_integratedServer) return;
        std::vector<std::string> tags = CollectKnownTags();
        if (tags == LastSuggested()) return;
        LastSuggested() = tags;
        g_integratedServer->GetCommandDispatcher().UpdateSyntax("tag", BuildTree(tags));
        NamedEntities::ResendCommands();
    }

    void TagCommand::Execute(const CommandSourceStack& source,
                             const std::vector<std::string>& args,
                             ServerConnection& connection,
                             PlayerSessionManager& sessionManager) {
        (void)sessionManager;
        using namespace EntityCmd;
        const auto usage = [&] {
            Failure(connection, "Unknown or incomplete command, see below for error");
            for (const std::string& line : g_integratedServer->GetCommandDispatcher().GetUsageLines("tag")) {
                Failure(connection, line);
            }
        };
        if (args.size() < 2) { usage(); return; }

        std::vector<SelectedEntity> targets;
        std::string error;
        if (!ResolveSelector(args[0], SelectorKind::Entities, source, targets, error)) {
            Failure(connection, error);
            return;
        }
        const std::string& action = args[1];

        if (action == "list") {
            if (args.size() != 2) { usage(); return; }
            // CommandResponseTracker over the targets, each tracked with its
            // tag count; the union of every target's tags is listed.
            std::set<std::string> tags;
            int nonZero = 0;
            std::string onlyNonZero;
            for (const SelectedEntity& target : targets) {
                const Game::EntityTags* entityTags = SelectedEntityTags(target);
                const size_t count = entityTags ? entityTags->Size() : 0;
                if (entityTags) tags.insert(entityTags->All().begin(), entityTags->All().end());
                if (count != 0 && ++nonZero == 1) onlyNonZero = DisplayName(target);
            }
            if (tags.empty()) {
                // RESPONSE_NO_TAGS over the NON_ZERO element type, as MC
                // dispatches it.
                Success(source, connection, /*broadcast=*/false, nonZero == 1 ? Tr("commands.tag.list.single.empty", {onlyNonZero})
                                                 : Tr("commands.tag.list.multiple.empty", {std::to_string(nonZero)}));
                return;
            }
            const std::vector<std::string> list(tags.begin(), tags.end());
            if (nonZero == 1) {
                Success(source, connection, /*broadcast=*/false, Tr("commands.tag.list.single.success",
                                       {onlyNonZero, std::to_string(list.size()), FormatList(list)}));
            } else {
                Success(source, connection, /*broadcast=*/false, Tr("commands.tag.list.multiple.success",
                                       {std::to_string(nonZero), std::to_string(list.size()), FormatList(list)}));
            }
            return;
        }

        if ((action != "add" && action != "remove") || args.size() != 3) { usage(); return; }
        const std::string& name = args[2];
        // StringArgumentType.word(): an unquoted string.
        if (!Game::IsValidEntityTag(name)) {
            Failure(connection, "Invalid tag name '" + name + "'");
            return;
        }
        const bool add = action == "add";
        int changed = 0;
        std::string onlyChanged;
        for (const SelectedEntity& target : targets) {
            Game::EntityTags* tags = SelectedEntityTags(target);
            if (!tags) continue;
            const bool ok = add ? tags->Add(name) : tags->Remove(name);
            if (ok && ++changed == 1) onlyChanged = DisplayName(target);
        }
        if (changed == 0) {
            Failure(connection, Tr(add ? "commands.tag.add.failed" : "commands.tag.remove.failed"));
            return;
        }
        if (changed == 1) {
            Success(source, connection, /*broadcast=*/true, Tr(add ? "commands.tag.add.success.single" : "commands.tag.remove.success.single",
                                   {name, onlyChanged}));
        } else {
            Success(source, connection, /*broadcast=*/true, Tr(add ? "commands.tag.add.success.multiple" : "commands.tag.remove.success.multiple",
                                   {name, std::to_string(changed)}));
        }
        RefreshSuggestions();
    }

} // namespace Server
