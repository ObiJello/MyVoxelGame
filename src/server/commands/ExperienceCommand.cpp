// File: src/server/commands/ExperienceCommand.cpp
#include "ExperienceCommand.hpp"
#include "EntityCommandUtil.hpp"
#include "../IntegratedServer.hpp"
#include "../player/ServerPlayer.hpp"

#include <climits>
#include <cmath>

namespace Server {

    namespace {

        enum class XpType : uint8_t { Points, Levels };

        const char* TypeName(XpType t) { return t == XpType::Points ? "points" : "levels"; }

        // ServerPlayer.setExperiencePoints: the bar set to amount / needed,
        // clamped short of a full bar.
        void SetExperiencePoints(PlayerExperience& xp, int amount) {
            const float limit = static_cast<float>(xp.XpNeededForNextLevel());
            const float max = (limit - 1.0f) / limit;
            float progress = static_cast<float>(amount) / limit;
            progress = progress < 0.0f ? 0.0f : (progress > max ? max : progress);
            xp.SetProgress(progress);
        }

        // Player.giveExperienceLevels: saturated add; below zero clears the
        // level, the bar and the total.
        void GiveExperienceLevels(PlayerExperience& xp, int amount) {
            if (amount == 0) return;
            const long long sum = static_cast<long long>(xp.Level()) + amount;
            const int level = sum > INT_MAX ? INT_MAX : (sum < INT_MIN ? INT_MIN : static_cast<int>(sum));
            if (level < 0) {
                xp.SetLevel(0);
                xp.SetProgress(0.0f);
                xp.SetTotal(0);
                return;
            }
            xp.SetLevel(level);
        }

        int Query(const PlayerExperience& xp, XpType type) {
            // Mth.floor(experienceProgress * getXpNeededForNextLevel()).
            if (type == XpType::Points) {
                return static_cast<int>(std::floor(xp.Progress() * static_cast<float>(xp.XpNeededForNextLevel())));
            }
            return xp.Level();
        }

        bool ParseType(const std::vector<std::string>& args, size_t at, XpType& out) {
            if (args.size() <= at) { out = XpType::Points; return true; }   // the bare form is points
            if (args.size() != at + 1) return false;
            if (args[at] == "points") { out = XpType::Points; return true; }
            if (args[at] == "levels") { out = XpType::Levels; return true; }
            return false;
        }

    } // namespace

    void ExperienceCommand::Register(CommandDispatcher& dispatcher) {
        namespace Cmd = Game::Cmd;
        // add <targets> <amount> [points|levels] | set <targets> <amount>
        // [points|levels] | query <target> (points|levels). /xp is MC's
        // redirect to the same node.
        const auto amount = [](std::vector<std::string> examples) {
            return Cmd::Argument("amount", Cmd::Arg::Integer).Suggests(std::move(examples)).Executes()
                .Then(Cmd::Literals({"levels", "points"}));
        };
        Cmd::Node tree = Cmd::Root()
            .Then(Cmd::Literal("add")
                .Then(Cmd::Argument("targets", Cmd::Arg::Players).Then(amount({"1", "10", "30", "-1"}))))
            .Then(Cmd::Literal("set")
                .Then(Cmd::Argument("targets", Cmd::Arg::Players).Then(amount({"0", "10", "30"}))))
            .Then(Cmd::Literal("query")
                .Then(Cmd::Argument("target", Cmd::Arg::Player)
                    .Then(Cmd::Literals({"levels", "points"}))));
        dispatcher.RegisterCommand("experience", ExperienceCommand::Execute, tree);
        dispatcher.RegisterCommand("xp", ExperienceCommand::Execute, std::move(tree));
    }

    void ExperienceCommand::Execute(const CommandSourceStack& source,
                                    const std::vector<std::string>& args,
                                    ServerConnection& connection,
                                    PlayerSessionManager& sessionManager) {
        (void)sessionManager;
        using namespace EntityCmd;
        const auto usage = [&] {
            Failure(connection, "Unknown or incomplete command, see below for error");
            for (const std::string& line : g_integratedServer->GetCommandDispatcher().GetUsageLines("experience")) {
                Failure(connection, line);
            }
        };
        if (args.size() < 2) { usage(); return; }
        const std::string& action = args[0];
        std::string error;

        if (action == "query") {
            XpType type{};
            if (args.size() != 3 || !ParseType(args, 2, type)) { usage(); return; }
            std::vector<SelectedEntity> targets;
            if (!ResolveSelector(args[1], SelectorKind::Player, source, targets, error)) {
                Failure(connection, error);
                return;
            }
            const SelectedEntity& target = targets.front();
            if (!target.player) return;
            Success(source, connection, /*broadcast=*/false, Tr(type == XpType::Points ? "commands.experience.query.points"
                                                          : "commands.experience.query.levels",
                                   {DisplayName(target), std::to_string(Query(target.player->getExperience(), type))}));
            return;
        }

        if ((action != "add" && action != "set") || args.size() < 3) { usage(); return; }
        const bool add = action == "add";
        XpType type{};
        if (!ParseType(args, 3, type)) { usage(); return; }
        std::vector<SelectedEntity> targets;
        if (!ResolveSelector(args[1], SelectorKind::Players, source, targets, error)) {
            Failure(connection, error);
            return;
        }
        int amount = 0;
        // add: IntegerArgumentType.integer(); set: integer(0).
        if (!ParseInt(args[2], add ? INT_MIN : 0, INT_MAX, amount, error)) {
            Failure(connection, error);
            return;
        }

        int changed = 0;
        std::string only;
        for (const SelectedEntity& target : targets) {
            if (!target.player) continue;
            PlayerExperience& xp = target.player->getExperience();
            bool ok = true;
            if (add) {
                if (type == XpType::Points) xp.GivePoints(amount);
                else GiveExperienceLevels(xp, amount);
            } else if (type == XpType::Points) {
                // Type.POINTS.set: refused at or past the current level's bar.
                if (amount >= xp.XpNeededForNextLevel()) ok = false;
                else SetExperiencePoints(xp, amount);
            } else {
                xp.SetLevel(amount);
            }
            if (ok && ++changed == 1) only = DisplayName(target);
        }
        if (changed == 0) {
            // add tracks every player, so only `set points` reaches zero.
            Failure(connection, Tr("commands.experience.set.points.invalid"));
            return;
        }
        const std::string key = std::string("commands.experience.") + (add ? "add." : "set.") + TypeName(type) +
                                (changed == 1 ? ".success.single" : ".success.multiple");
        Success(source, connection, /*broadcast=*/true, Tr(key.c_str(), {std::to_string(amount), changed == 1 ? only : std::to_string(changed)}));
    }

} // namespace Server
