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
#include <memory>
#include <string>
#include <vector>

namespace Game { class Mob; }

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
    };

    // MC EntityArgument's four flavours. They differ in two ways: whether more
    // than one result is allowed, and whether non-players may be returned at
    // all (`EntityArgument.player()` rejects `@e` outright rather than
    // silently filtering it).
    enum class SelectorKind : uint8_t {
        Entity,     // EntityArgument.entity()   — exactly one, any type
        Entities,   // EntityArgument.entities() — one or more, any type
        Player,     // EntityArgument.player()   — exactly one player
        Players,    // EntityArgument.players()  — one or more players
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
