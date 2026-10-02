// File: src/server/commands/DamageCommand.cpp
#include "DamageCommand.hpp"
#include "CommandCoords.hpp"
#include "EntityCommandUtil.hpp"
#include "../IntegratedServer.hpp"
#include "../entity/ItemEntityManager.hpp"
#include "../level/ServerLevel.hpp"
#include "../player/ServerPlayer.hpp"

#include "common/data/components/ToolComponents.hpp"
#include "common/entity/LivingEntity.hpp"
#include "common/entity/Mob.hpp"

#include <cfloat>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string_view>

namespace Server {

    namespace {

        struct TypeRow {
            std::string_view       id;      // without "minecraft:"
            Game::MobDamageSource  source;
        };

        // MC's damage_type keys onto the engine's sources — the inverse of
        // DamageSourceInfo::TypeIdFor, plus the vanilla types that share a
        // source's behaviour here (every projectile kind is Projectile; the
        // fire block is the on-fire source). A type with no engine source of
        // its own (starve, freeze, sting, sonic_boom, dragon_breath, in_wall
        // …) lands as Generic — the same armour, effects and invulnerability
        // rules, its own tags aside.
        constexpr TypeRow kTypes[] = {
            {"generic",              Game::MobDamageSource::Generic},
            {"mob_attack",           Game::MobDamageSource::MobAttack},
            {"mob_attack_no_aggro",  Game::MobDamageSource::MobAttack},
            {"player_attack",        Game::MobDamageSource::PlayerAttack},
            {"fall",                 Game::MobDamageSource::Fall},
            {"ender_pearl",          Game::MobDamageSource::Fall},
            {"on_fire",              Game::MobDamageSource::Fire},
            {"in_fire",              Game::MobDamageSource::Fire},
            {"lava",                 Game::MobDamageSource::Lava},
            {"drown",                Game::MobDamageSource::Drown},
            {"explosion",            Game::MobDamageSource::Explosion},
            {"player_explosion",     Game::MobDamageSource::Explosion},
            {"bad_respawn_point",    Game::MobDamageSource::Explosion},
            {"out_of_world",         Game::MobDamageSource::Void},
            {"magic",                Game::MobDamageSource::Magic},
            {"indirect_magic",       Game::MobDamageSource::Magic},
            {"wither",               Game::MobDamageSource::Wither},
            {"cramming",             Game::MobDamageSource::Cramming},
            {"falling_block",        Game::MobDamageSource::FallingBlock},
            {"falling_anvil",        Game::MobDamageSource::FallingAnvil},
            {"falling_stalactite",   Game::MobDamageSource::FallingStalactite},
            {"stalagmite",           Game::MobDamageSource::Stalagmite},
            {"thorns",               Game::MobDamageSource::Thorns},
            {"mace_smash",           Game::MobDamageSource::MaceSmash},
            {"fireworks",            Game::MobDamageSource::Fireworks},
            {"fly_into_wall",        Game::MobDamageSource::FlyIntoWall},
            {"lightning_bolt",       Game::MobDamageSource::Lightning},
            {"spear",                Game::MobDamageSource::Spear},
            {"cactus",               Game::MobDamageSource::Cactus},
            {"sweet_berry_bush",     Game::MobDamageSource::SweetBerryBush},
            {"hot_floor",            Game::MobDamageSource::HotFloor},
            {"campfire",             Game::MobDamageSource::Campfire},
            {"arrow",                Game::MobDamageSource::Projectile},
            {"trident",              Game::MobDamageSource::Projectile},
            {"fireball",             Game::MobDamageSource::Projectile},
            {"unattributed_fireball", Game::MobDamageSource::Projectile},
            {"wither_skull",         Game::MobDamageSource::Projectile},
            {"thrown",               Game::MobDamageSource::Projectile},
            {"mob_projectile",       Game::MobDamageSource::Projectile},
            {"wind_charge",          Game::MobDamageSource::Projectile},
            {"spit",                 Game::MobDamageSource::Projectile},
        };

        // ResourceArgument<damage_type>: a key the data pack registers.
        bool ParseDamageType(const std::string& token, std::string& outId, Game::MobDamageSource& outSource,
                             std::string& error) {
            std::string ns = "minecraft", path = token;
            if (const size_t colon = token.find(':'); colon != std::string::npos) {
                ns = token.substr(0, colon);
                path = token.substr(colon + 1);
            }
            const char* env = std::getenv("MC_DATA_ROOT");   // the data pack root every reader uses
            const std::filesystem::path file =
                std::filesystem::path(env ? env : "data") / ns / "damage_type" / (path + ".json");
            std::error_code ec;
            if (path.empty() || !std::filesystem::exists(file, ec)) {
                error = "Can't find element '" + ns + ":" + path + "' of type 'minecraft:damage_type'";
                return false;
            }
            outId = ns + ":" + path;
            outSource = Game::MobDamageSource::Generic;
            if (ns == "minecraft") {
                for (const TypeRow& row : kTypes) {
                    if (row.id == path) { outSource = row.source; break; }
                }
            }
            return true;
        }

        // The live Entity behind a selection (a mob, the player's view).
        Game::Entity* EntityOf(const SelectedEntity& e) {
            if (e.kind == SelectedEntity::Kind::Mob) return e.mob;
            if (e.kind == SelectedEntity::Kind::Player && e.player) return e.player->effectEntity();
            return nullptr;
        }

    } // namespace

    void DamageCommand::Register(CommandDispatcher& dispatcher) {
        namespace Cmd = Game::Cmd;
        dispatcher.RegisterCommand("damage", DamageCommand::Execute,
            Cmd::Root()
                .Then(Cmd::Argument("target", Cmd::Arg::Entity)
                    .Then(Cmd::Argument("amount", Cmd::Arg::Float).Suggests({"1", "5", "10", "20"}).Executes()
                        .Then(Cmd::Argument("damageType", Cmd::Arg::DamageType).Executes()
                            .Then(Cmd::Literal("at")
                                .Then(Cmd::Argument("location", Cmd::Arg::Vec3).Executes()))
                            .Then(Cmd::Literal("by")
                                .Then(Cmd::Argument("entity", Cmd::Arg::Entity).Executes()
                                    .Then(Cmd::Literal("from")
                                        .Then(Cmd::Argument("cause", Cmd::Arg::Entity).Executes()))))))));
    }

    void DamageCommand::Execute(const CommandSourceStack& source,
                                const std::vector<std::string>& args,
                                ServerConnection& connection,
                                PlayerSessionManager& sessionManager) {
        (void)sessionManager;
        using namespace EntityCmd;
        const auto usage = [&] {
            Failure(connection, "Unknown or incomplete command, see below for error");
            for (const std::string& line : g_integratedServer->GetCommandDispatcher().GetUsageLines("damage")) {
                Failure(connection, line);
            }
        };
        if (args.size() < 2) { usage(); return; }

        std::vector<SelectedEntity> targets;
        std::string error;
        if (!ResolveSelector(args[0], SelectorKind::Entity, source, targets, error)) {
            Failure(connection, error);
            return;
        }
        const SelectedEntity& target = targets.front();
        float amount = 0.0f;
        if (!ParseFloat(args[1], 0.0f, FLT_MAX, amount, error)) {
            Failure(connection, error);
            return;
        }

        // damageSources().generic() without a type.
        std::string typeId = "minecraft:generic";
        Game::MobDamageSource mobSource = Game::MobDamageSource::Generic;
        Game::Entity* direct = nullptr;
        Game::Entity* causing = nullptr;
        if (args.size() >= 3) {
            if (!ParseDamageType(args[2], typeId, mobSource, error)) {
                Failure(connection, error);
                return;
            }
            if (args.size() >= 4) {
                const std::string& how = args[3];
                if (how == "at" && args.size() == 7) {
                    // DamageSource(type, sourcePosition): validated as a
                    // Vec3; the engine's hurt path takes its knockback
                    // direction from an entity only, so a bare position
                    // moves nothing.
                    glm::dvec3 at{};
                    if (!ParseVec3(args[4], args[5], args[6], source, source.rotation, at, error)) {
                        Failure(connection, error);
                        return;
                    }
                } else if (how == "by" && (args.size() == 5 || (args.size() == 7 && args[5] == "from"))) {
                    // DamageSource(type, directEntity[, causingEntity]): `by`
                    // is what struck, `from` who gets the credit (the
                    // direct entity when absent).
                    std::vector<SelectedEntity> by;
                    if (!ResolveSelector(args[4], SelectorKind::Entity, source, by, error)) {
                        Failure(connection, error);
                        return;
                    }
                    direct = EntityOf(by.front());
                    causing = direct;
                    if (args.size() == 7) {
                        std::vector<SelectedEntity> cause;
                        if (!ResolveSelector(args[6], SelectorKind::Entity, source, cause, error)) {
                            Failure(connection, error);
                            return;
                        }
                        causing = EntityOf(cause.front());
                    }
                } else {
                    usage();
                    return;
                }
            }
        }

        bool hurt = false;
        if (target.kind == SelectedEntity::Kind::Item) {
            // ItemEntity.hurtServer: refused when the stack's
            // DAMAGE_RESISTANT covers the type; else health -= amount
            // (truncated) and the item is destroyed at zero — an emptied
            // stack is the manager's destroy signal.
            ServerLevel* level = LevelOf(target.dimension);
            ItemEntityManager* items = level ? level->Items() : nullptr;
            if (Game::ItemEntity* item = items ? items->Find(target.id) : nullptr) {
                if (!item->stack.IsEmpty() && Game::ItemStackCanBeHurtBy(item->stack, typeId)) {
                    item->health = static_cast<int>(static_cast<float>(item->health) - amount);
                    if (item->health <= 0) item->stack.Clear();
                    hurt = true;
                }
            }
        } else if (Game::LivingEntity* living = LivingOf(target)) {
            // Entity.hurtServer through the real path: armour, effects,
            // enchantments, the invulnerability window, death.
            hurt = living->HurtFrom(mobSource, amount, causing, direct ? direct : causing);
        }

        if (!hurt) {
            Failure(connection, Tr("commands.damage.invulnerable"));
            return;
        }
        Success(source, connection, /*broadcast=*/true, Tr("commands.damage.success", {JavaFloat(amount), DisplayName(target)}));
    }

} // namespace Server
