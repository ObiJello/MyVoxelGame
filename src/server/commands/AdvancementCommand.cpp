// File: src/server/commands/AdvancementCommand.cpp
#include "AdvancementCommand.hpp"
#include "EntitySelector.hpp"
#include "../network/ServerConnection.hpp"
#include "../session/PlayerSession.hpp"
#include "../session/PlayerSessionManager.hpp"
#include "../player/ServerPlayer.hpp"
#include "server/advancements/AdvancementText.hpp"
#include "server/advancements/PlayerAdvancements.hpp"
#include "server/advancements/ServerAdvancements.hpp"

#include "common/advancements/AdvancementLoader.hpp"
#include "common/text/TextComponent.hpp"

#include <string>
#include <vector>

namespace Server {

    namespace {

        using Game::Advancements::Definition;
        using Game::Advancements::Node;
        using Game::Text::Component;

        enum class Action { Grant, Revoke };
        enum class Mode { Only, Through, From, Until, Everything };

        const char* ActionKey(Action action) { return action == Action::Grant ? "grant" : "revoke"; }

        Component Lit(const std::string& text) { return Component::Literal(text); }
        Component Num(size_t n) { return Component::Literal(std::to_string(n)); }

        void Send(ServerConnection& connection, const Component& message) {
            connection.SendChatMessage(Advancements::SystemMessage(message));
        }

        // CommandSyntaxException text: red, the advancement name keeping its
        // own colour inside.
        void Fail(ServerConnection& connection, Component message) {
            message.style.color = Game::Text::TextColor::FromFormatting(static_cast<int>(Game::Text::Formatting::Red));
            Send(connection, message);
        }

        // AdvancementCommands.getAdvancements: the target, plus its ancestors
        // (until/through) and its descendants (from/through).
        void AddChildren(const Node& parent, std::vector<const Definition*>& out) {
            for (const Node* child : parent.children) {
                out.push_back(child->def);
                AddChildren(*child, out);
            }
        }

        std::vector<const Definition*> Collect(const Game::Advancements::Registry& registry, const Definition& target, Mode mode) {
            const Node* node = registry.tree.Get(target.id);
            if (!node) return {&target};
            std::vector<const Definition*> out;
            if (mode == Mode::Until || mode == Mode::Through) {
                for (const Node* p = node->parent; p; p = p->parent) out.push_back(p->def);
            }
            out.push_back(&target);
            if (mode == Mode::From || mode == Mode::Through) AddChildren(*node, out);
            return out;
        }

        // Action.perform(player, advancement).
        bool PerformOne(Advancements::PlayerAdvancements& advancements, Action action, const Definition& def) {
            auto& progress = advancements.GetOrStartProgress(def);
            if (action == Action::Grant) {
                if (progress.IsDone()) return false;
                for (const std::string& criterion : progress.RemainingCriteria()) advancements.Award(def, criterion);
                return true;
            }
            if (!progress.HasProgress()) return false;
            for (const std::string& criterion : progress.CompletedCriteria()) advancements.Revoke(def, criterion);
            return true;
        }

        struct Target {
            ServerPlayer* player = nullptr;
            std::shared_ptr<PlayerSession> session;
            Advancements::PlayerAdvancements* advancements = nullptr;
        };

        void Usage(ServerConnection& connection) {
            connection.SendChatMessage(
                "Usage: /advancement (grant|revoke) <targets> (everything | only <advancement> [<criterion>] | "
                "from|through|until <advancement>)", 1);
        }

    } // namespace

    void AdvancementCommand::Register(CommandDispatcher& dispatcher) {
        namespace Cmd = Game::Cmd;
        auto actionTree = [](const char* action) {
            return Cmd::Literal(action)
                .Then(Cmd::Argument("targets", Cmd::Arg::Players)
                    .Then(Cmd::Literal("only")
                        .Then(Cmd::Argument("advancement", Cmd::Arg::Advancement).Executes()
                            .Then(Cmd::Argument("criterion", Cmd::Arg::AdvancementCriterion).Executes())))
                    .Then(Cmd::Literal("from")
                        .Then(Cmd::Argument("advancement", Cmd::Arg::Advancement).Executes()))
                    .Then(Cmd::Literal("until")
                        .Then(Cmd::Argument("advancement", Cmd::Arg::Advancement).Executes()))
                    .Then(Cmd::Literal("through")
                        .Then(Cmd::Argument("advancement", Cmd::Arg::Advancement).Executes()))
                    .Then(Cmd::Literal("everything").Executes()));
        };
        dispatcher.RegisterCommand("advancement", AdvancementCommand::Execute,
            Cmd::Root().Then(actionTree("grant")).Then(actionTree("revoke")));
    }

    void AdvancementCommand::Execute(const CommandSourceStack& source,
                                     const std::vector<std::string>& args,
                                     ServerConnection& connection,
                                     PlayerSessionManager& sessionManager) {
        (void)sessionManager;
        if (args.size() < 3 || (args[0] != "grant" && args[0] != "revoke")) { Usage(connection); return; }
        const Action action = args[0] == "grant" ? Action::Grant : Action::Revoke;

        Mode mode;
        const std::string& modeWord = args[2];
        if (modeWord == "everything") mode = Mode::Everything;
        else if (modeWord == "only") mode = Mode::Only;
        else if (modeWord == "from") mode = Mode::From;
        else if (modeWord == "until") mode = Mode::Until;
        else if (modeWord == "through") mode = Mode::Through;
        else { Usage(connection); return; }
        if (mode == Mode::Everything ? args.size() != 3 : args.size() < 4) { Usage(connection); return; }
        if (mode != Mode::Only && mode != Mode::Everything && args.size() != 4) { Usage(connection); return; }

        // EntityArgument.players().
        std::vector<SelectedEntity> selected;
        std::string error;
        if (!ResolveSelector(args[1], SelectorKind::Players, source, selected, error)) {
            Fail(connection, Lit(error));
            return;
        }
        std::vector<Target> targets;
        for (const SelectedEntity& s : selected) {
            if (s.kind != SelectedEntity::Kind::Player || !s.player) continue;
            Target t;
            t.player = s.player;
            t.session = s.session;
            t.advancements = Advancements::Get(*s.player);
            if (t.advancements) targets.push_back(std::move(t));
        }
        if (targets.empty()) {
            Fail(connection, Component::Translatable("argument.entity.notfound.player"));
            return;
        }

        const auto registry = Advancements::GetRegistry();
        const std::string key(ActionKey(action));

        // ResourceKeyArgument.getAdvancement.
        const Definition* target = nullptr;
        if (mode != Mode::Everything) {
            target = registry->Get(args[3]);
            if (!target) {
                Fail(connection, Component::Translatable("advancement.advancementNotFound", {Lit(args[3])}));
                return;
            }
        }

        // ── performCriterion ─────────────────────────────────────────────
        if (mode == Mode::Only && args.size() > 4) {
            std::string criterion = args[4];
            for (size_t i = 5; i < args.size(); ++i) criterion += " " + args[i];   // greedyString
            if (!target->FindCriterion(criterion)) {
                Fail(connection, Component::Translatable("commands.advancement.criterionNotFound",
                                                         {target->Name(), Lit(criterion)}));
                return;
            }
            std::vector<const Target*> changed;
            for (const Target& t : targets) {
                const bool did = action == Action::Grant ? t.advancements->Award(*target, criterion)
                                                         : t.advancements->Revoke(*target, criterion);
                if (did) changed.push_back(&t);
            }
            if (changed.empty()) {
                Fail(connection, targets.size() == 1
                    ? Component::Translatable("commands.advancement." + key + ".criterion.to.one.failure",
                                              {Lit(criterion), target->Name(), Lit(targets[0].player->getName())})
                    : Component::Translatable("commands.advancement." + key + ".criterion.to.many.failure",
                                              {Lit(criterion), target->Name(), Num(targets.size())}));
                return;
            }
            Send(connection, changed.size() == 1
                ? Component::Translatable("commands.advancement." + key + ".criterion.to.one.success",
                                          {Lit(criterion), target->Name(), Lit(changed[0]->player->getName())})
                : Component::Translatable("commands.advancement." + key + ".criterion.to.many.success",
                                          {Lit(criterion), target->Name(), Num(changed.size())}));
            return;
        }

        // ── perform ──────────────────────────────────────────────────────
        std::vector<const Definition*> list;
        if (mode == Mode::Everything) {
            for (const auto& def : registry->definitions) list.push_back(def.get());
        } else {
            list = Collect(*registry, *target, mode);
        }
        // `everything` shows no toasts: what is already pending goes out
        // first, then the bulk change with showAdvancements = false.
        const bool showAdvancements = mode != Mode::Everything;

        std::vector<std::pair<const Target*, int>> results;
        for (const Target& t : targets) {
            ServerConnection* conn = t.session ? t.session->GetConnection() : nullptr;
            if (!showAdvancements) t.advancements->FlushDirty(*t.player, conn, true);
            int count = 0;
            for (const Definition* def : list) {
                if (PerformOne(*t.advancements, action, *def)) ++count;
            }
            if (!showAdvancements) t.advancements->FlushDirty(*t.player, conn, false);
            results.emplace_back(&t, count);
        }

        size_t nonZero = 0;
        const Target* only = nullptr;
        for (const auto& [t, count] : results) {
            if (count > 0) { ++nonZero; only = t; }
        }
        if (list.size() == 1) {
            const Definition& def = *list.front();
            if (nonZero == 0) {
                Fail(connection, targets.size() == 1
                    ? Component::Translatable("commands.advancement." + key + ".one.to.one.failure",
                                              {def.Name(), Lit(targets[0].player->getName())})
                    : Component::Translatable("commands.advancement." + key + ".one.to.many.failure",
                                              {def.Name(), Num(targets.size())}));
                return;
            }
            Send(connection, nonZero == 1
                ? Component::Translatable("commands.advancement." + key + ".one.to.one.success",
                                          {def.Name(), Lit(only->player->getName())})
                : Component::Translatable("commands.advancement." + key + ".one.to.many.success",
                                          {def.Name(), Num(nonZero)}));
            return;
        }
        if (nonZero == 0) {
            Fail(connection, targets.size() == 1
                ? Component::Translatable("commands.advancement." + key + ".many.to.one.failure",
                                          {Num(list.size()), Lit(targets[0].player->getName())})
                : Component::Translatable("commands.advancement." + key + ".many.to.many.failure",
                                          {Num(list.size()), Num(targets.size())}));
            return;
        }
        Send(connection, nonZero == 1
            ? Component::Translatable("commands.advancement." + key + ".many.to.one.success",
                                      {Num(list.size()), Lit(only->player->getName())})
            : Component::Translatable("commands.advancement." + key + ".many.to.many.success",
                                      {Num(list.size()), Num(nonZero)}));
    }

} // namespace Server
