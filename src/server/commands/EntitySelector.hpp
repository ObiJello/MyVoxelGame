// File: src/server/commands/EntitySelector.hpp
//
// MC net.minecraft.commands.arguments.selector.{EntitySelector,
// EntitySelectorParser} + EntitySelectorOptions, reduced to what this engine
// can actually select.
//
// ── Why this is its own file ───────────────────────────────────────────────
//
// In MC a selector is an ARGUMENT TYPE, not a feature of any one command:
// /tp, /kill, /summon, /data, /execute all take the same `@e[...]` grammar
// through EntityArgument. Writing the parser inside TeleportCommand would
// guarantee the next command reimplements a subset of it and the two disagree
// on some corner — which is exactly how vanilla-incompatible selectors happen.
//
// ── What a selection is ────────────────────────────────────────────────────
//
// This engine has three unrelated entity containers — players live in
// PlayerSessionManager, mobs in MobManager, dropped items in
// ItemEntityManager — where MC has one Level-wide list. SelectedEntity is the
// common view over all three, carrying enough to filter, sort, report and act
// on an entity without the caller re-deriving which container it came from.
//
// ── Angle convention ───────────────────────────────────────────────────────
//
// SelectedEntity::yRot / xRot are MC's convention — yaw 0 = +Z (south), pitch
// positive DOWN — and so is everything they are read from. Players and mobs
// agree; see the note on Game::Mth::ViewVector. There is nothing to convert.
#pragma once

#include "common/physics/Physics.hpp"

#include <glm/glm.hpp>
#include "common/world/level/DimensionId.hpp"
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Game { class Entity; class Mob; }

namespace Server {

    class ServerPlayer;
    class PlayerSession;
    class PlayerSessionManager;

    // One entity that a selector matched.
    struct SelectedEntity {
        enum class Kind : uint8_t { Player, Mob, Item };

        Kind kind = Kind::Player;

        // Player  -> the owning session's connection id.
        // Mob     -> MobManager entity id.
        // Item    -> ItemEntityManager entity id.
        int32_t id = 0;

        // Exactly one of these is set, matching `kind`. Items are addressed by
        // id alone: ItemEntity is a value in a map, so a pointer taken here
        // could dangle across a tick, and every caller has to go back through
        // the manager anyway.
        ServerPlayer*                  player = nullptr;
        std::shared_ptr<PlayerSession> session;
        Game::Mob*                     mob = nullptr;

        glm::dvec3 position{0.0};   // feet, MC's convention
        float      yRot = 0.0f;     // MC convention — see the header note
        float      xRot = 0.0f;
        Game::AABB box{};

        // The level the entity lives in (a player's session dimension; a mob
        // or item, the level it was enumerated from). `/execute at` reads it.
        Game::DimensionId dimension = Game::DimensionId::Overworld;

        // MC's registry name without the namespace: "player", "item",
        // "zombie", … — what `type=` matches against.
        std::string typeSlug;
        // What the feedback message prints. Player name, or the type slug.
        std::string name;
    };

    // MC CommandSourceStack, reduced to what a selector reads from it.
    struct CommandSource {
        ServerPlayer*         sender   = nullptr;
        PlayerSessionManager* sessions = nullptr;
        // MC CommandSourceStack.getPosition() — the origin for `distance`,
        // `dx/dy/dz` and every sort. Selector `x=`/`y=`/`z=` override it.
        glm::dvec3 position{0.0};
        // MC CommandSourceStack.getLevel() — the DIMENSION the selector
        // enumerates mobs and items from. MC selectors are level-scoped
        // (EntitySelector.getEntities walks source.getLevel() unless
        // currentEntity), and before this field existed every one of them
        // silently searched the Overworld: `/kill @e` from the End cleared
        // the wrong world. Fill it from the sender's session.
        Game::DimensionId dimension = Game::DimensionId::Overworld;
        // MC CommandSourceStack.entity — what `@s` resolves to. The sender
        // for a plain command; `/execute as <targets>` swaps it for each
        // target, which is how `/execute as @e[type=zombie] run tp @s ~ ~5 ~`
        // moves the zombies and not the player. Empty = no entity (a console
        // would be one; here every command has a sender, so it is empty only
        // when a relation (`/execute on`) found nothing).
        std::optional<SelectedEntity> entity;
        // MC CommandSourceStack.anchor (EntityAnchorArgument.Anchor): FEET
        // (false) or EYES. `^` local coordinates and `facing` start from it.
        bool anchorEyes = false;
    };

    // MC EntityAnchorArgument.Anchor.apply(CommandSourceStack): the source's
    // position, raised by the entity's eye height when the anchor is EYES
    // (an anchor without an entity is FEET whatever it says).
    glm::dvec3 SourceAnchorPosition(const CommandSource& source);

    // MC EntityAnchorArgument.Anchor.apply(Entity): the entity's own feet or
    // eye position.
    glm::dvec3 EntityAnchorPosition(const SelectedEntity& entity, bool eyes);

    // Describe a live entity the way a selector would, so code that reaches
    // an entity by some other route (`/execute on`, a summon) can hand it on
    // as a command source. `entity` is a Mob or the server's player view;
    // dropped items are not Entities here and cannot be described. Fails for
    // an entity that is removed or no longer in `source.dimension`'s level.
    bool DescribeEntity(Game::Entity* entity, const CommandSource& source, SelectedEntity& out);

    // The sender's own SelectedEntity (what `@s` is for a plain command).
    bool DescribePlayer(ServerPlayer& player, const CommandSource& source, SelectedEntity& out);

    // Re-read a previously selected entity's live state (position, rotation,
    // box) from its container. False when it is gone.
    bool RefreshSelectedEntity(const CommandSource& source, SelectedEntity& entity);

    // MC EntityArgument's four flavours. They differ in two ways: whether more
    // than one result is allowed, and whether non-players may be returned at
    // all (`EntityArgument.player()` rejects `@e` outright rather than
    // silently filtering it).
    enum class SelectorKind : uint8_t {
        Entity,     // EntityArgument.entity()   — exactly one, any type
        Entities,   // EntityArgument.entities() — one or more, any type
        Player,     // EntityArgument.player()   — exactly one player
        Players,    // EntityArgument.players()  — one or more players
        // EntityArgument.getOptionalEntities — any number INCLUDING zero.
        // What `/execute as|at|if entity` reads: a selector that matches
        // nothing forks into nothing rather than failing the command.
        OptionalEntities,
    };

    // Parse `token` and resolve it against the live world.
    //
    // Accepts either a bare player name (MC allows a name or a UUID wherever a
    // selector goes; there are no UUIDs here) or a full `@x[key=value,...]`
    // selector.
    //
    // Returns false and fills `error` with a player-facing message on a syntax
    // error, an unsupported option, or a result count the SelectorKind forbids.
    // MC's "no entities found" is also an error, not an empty success — the
    // difference matters, because a command that silently affects nothing looks
    // identical to one that silently failed.
    bool ResolveSelector(const std::string& token, SelectorKind kind,
                         const CommandSource& source,
                         std::vector<SelectedEntity>& out, std::string& error);

} // namespace Server
