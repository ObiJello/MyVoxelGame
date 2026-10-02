// File: src/server/commands/EntityCommandUtil.hpp
//
// What MC's entity and player commands (/clear, /enchant, /experience,
// /damage, /ride, /rotate, /tag, /spectate, /swing, /item, /bossbar) share:
//
//   CommandSourceStack.sendSuccess / sendFailure — the feedback line, the
//   failure in red; Component.translatable over the en_us strings (the
//   exact vanilla wording, filled in through the language file);
//   Entity.getDisplayName; `instanceof LivingEntity`;
//   Brigadier's Integer / Float / Double argument parsing with their
//   bounds and error texts; Java's Float.toString for the numbers the
//   messages print; ItemPredicateArgument (`/clear`'s item filter).
//
// Kept in one place for the reason EntitySelector is: a second copy of any
// of these drifts from the first.
#pragma once

#include "EntitySelector.hpp"
#include "CommandSourceStack.hpp"

#include "common/entity/Item.hpp"

#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

namespace Game { class LivingEntity; }

namespace Server {

    class ServerConnection;
    class ServerLevel;

    namespace EntityCmd {

        // Component.translatable(key, args...).getString() — the en_us line
        // with its %s filled in order.
        std::string Tr(const char* key, std::initializer_list<std::string> args = {});

        // CommandSourceStack.sendSuccess(text, broadcast) — through
        // source.SendSuccess, so send_command_feedback / log_admin_commands
        // apply; `broadcast` is MC's flag for that command's line.
        void Success(const CommandSourceStack& source, ServerConnection& connection, bool broadcast,
                     const std::string& text);
        // CommandSourceStack.sendFailure: the line in red (0xFF5555).
        void Failure(ServerConnection& connection, const std::string& text);

        // Entity.getDisplayName().getString(): a player's name; a mob's
        // custom name, else its translated type name; a dropped item's
        // stack hover name; "Experience Orb".
        std::string DisplayName(const SelectedEntity& entity);

        // `entity instanceof LivingEntity`: a mob, or the player's live view
        // (whose effect list and equipment ARE the ServerPlayer's). Null for
        // dropped items and orbs, and for a player between levels.
        Game::LivingEntity* LivingOf(const SelectedEntity& entity);

        // The level an entity lives in (null when the server or level is gone).
        ServerLevel* LevelOf(Game::DimensionId dimension);

        // Brigadier IntegerArgumentType.integer(min, max) /
        // FloatArgumentType / DoubleArgumentType — the value and its bounds,
        // with Brigadier's own messages.
        bool ParseInt(const std::string& token, int min, int max, int& out, std::string& error);
        bool ParseFloat(const std::string& token, float min, float max, float& out, std::string& error);
        bool ParseDouble(const std::string& token, double min, double max, double& out, std::string& error);

        // Java's Float.toString / Double.toString for the numbers MC's
        // messages print ("5.0", "2.5", "1.0E10").
        std::string JavaFloat(float v);
        std::string JavaDouble(double v);

        // MC ComponentUtils.formatList: "a, b, c".
        std::string FormatList(const std::vector<std::string>& items);

        // MC RotationArgument: `<yaw> <pitch>`, `~` relative to the
        // source's rotation; `^` refused. Yaw first, as typed. `yRelative`
        // and `xRelative` report which halves were `~` (WorldCoordinates
        // .isYRelative / isXRelative — /rotate turns BY those).
        bool ParseRotation(const std::string& yaw, const std::string& pitch, const CommandSourceStack& source,
                           float& outYRot, float& outXRot, bool& yRelative, bool& xRelative,
                           std::string& error);

        // MC ItemPredicateArgument: `*`, an item id, or `#tag`, then
        // optionally `[...]` — comma-separated tests, each `name=value`
        // (DataComponentExactPredicate), `name~value` (the named
        // DataComponentPredicate — damage, enchantments, custom_data …),
        // or a bare `name` (the component is present), any of them negated
        // with a leading `!`, alternatives joined with `|`.
        class ItemPredicate {
        public:
            bool Test(const Game::ItemStack& stack) const;

            // One `[...]` test. `nbt` is the compound ItemPredicateMatches
            // evaluates ({components:{…}} / {predicates:{…}}); a bare name
            // keeps the component type instead.
            struct Term {
                bool negated = false;
                std::shared_ptr<::World::NBTTagCompound> nbt;
                const void* presentType = nullptr;   // DataComponentTypeBase*
            };

            enum class Kind : uint8_t { Any, Item, Tag };
            Kind        kind = Kind::Any;
            Game::ItemID item{};
            std::string tag;     // "minecraft:logs"
            // AND of OR-groups.
            std::vector<std::vector<Term>> groups;
        };
        bool ParseItemPredicate(const std::string& text, ItemPredicate& out, std::string& error);

    } // namespace EntityCmd

} // namespace Server
