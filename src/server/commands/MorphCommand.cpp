// File: src/server/commands/MorphCommand.cpp
#include "MorphCommand.hpp"
#include "SummonCommand.hpp"         // ParseEntityType
#include "BlockStateArgument.hpp"    // ParseBlockState
#include "../network/ServerConnection.hpp"
#include "../player/ServerPlayer.hpp"
#include "../session/PlayerSession.hpp"
#include "../session/PlayerSessionManager.hpp"
#include "../level/LevelEntityStore.hpp"   // MakeMobForLoad
#include "../level/ServerLevel.hpp"
#include "../entity/ServerLevelBridge.hpp"   // the level the mob is built against
#include "../IntegratedServer.hpp"
#include "../entity/MorphCarry.hpp"
#include "../entity/MorphBlockAnchor.hpp"
#include "common/entity/Mob.hpp"
#include "common/entity/Morph.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/entity/ai/Controls.hpp"
#include "common/entity/GeneratedEntityTypes.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/level/Explosion.hpp"
#include "common/world/level/BlockClip.hpp"
#include "common/entity/projectile/Arrow.hpp"
#include "common/world/block/Blocks.hpp"
#include "common/core/Mth.hpp"
#include "common/world/level/DimensionId.hpp"
#include "common/core/Log.hpp"

#include <cctype>
#include <string>

namespace Server {

    namespace {
        std::string Lower(const std::string& text) {
            std::string s;
            for (char c : text) s += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        }
        const char* kUsage = "Usage: /morph [baby] <entity> | item <item> | block <block> | xp | herobrine | off";
    }

    void MorphCommand::Register(CommandDispatcher& dispatcher) {
        dispatcher.RegisterCommand("morph", MorphCommand::Execute);
    }

    void MorphCommand::Execute(const CommandSourceStack& source,
                               const std::vector<std::string>& args,
                               ServerConnection& connection,
                               PlayerSessionManager& sessionManager) {
        auto session = sessionManager.GetSession(connection.GetPlayerId());
        ServerPlayer* player = session ? session->GetPlayer() : nullptr;
        if (!player) {
            connection.SendChatMessage("Only a player can morph", 1);
            return;
        }
        if (args.empty()) {
            connection.SendChatMessage(kUsage, 1);
            return;
        }

        // The code rides the next PlayerUpdateS2C broadcast for everyone
        // else; the morphed client itself learns its new body from the
        // abilities packet sent here.
        auto apply = [&](uint32_t code, float walkSpeed, const std::string& what) {
            // A different morph is not the locked block any more.
            if (g_integratedServer) g_integratedServer->BlockAnchor().Unlock(connection.GetPlayerId());
            player->setMorph(code, walkSpeed);
            // A fresh item waits like a fresh drop before anyone can pick it
            // up (MC ItemEntity pickupDelay); the carry manager also lets a
            // held player go on its next tick when they stop being an item.
            if (g_integratedServer && Game::Morph::IsValid(code) &&
                Game::Morph::KindOf(code) == Game::Morph::Kind::Item) {
                g_integratedServer->Carry().SetPickupDelay(connection.GetPlayerId(), 20);
            }
            connection.SendPlayerAbilities(*player);
            connection.SendChatMessage(what, 1);
            Log::Info("[MorphCommand] %s -> %s", player->getName().c_str(), what.c_str());
        };

        const std::string first = Lower(args[0]);
        if (first == "lock") {
            // Left Alt on a block morph: the client snapped to a cell; the
            // world gets the block there (MorphBlockAnchor).
            if (args.size() < 4 || !g_integratedServer) return;
            glm::ivec3 cell;
            try {
                cell = glm::ivec3(std::stoi(args[1]), std::stoi(args[2]), std::stoi(args[3]));
            } catch (...) { return; }
            g_integratedServer->BlockAnchor().Lock(connection.GetPlayerId(), cell);
            return;
        }
        if (first == "unlock") {
            if (g_integratedServer) g_integratedServer->BlockAnchor().Unlock(connection.GetPlayerId());
            return;
        }
        if (first == "ability") {
            // Left Alt on a mob morph: the mob's own act. The client already
            // plays the animation (sheep graze, skeleton draw); the world-
            // side of it happens here.
            const uint32_t code = player->getMorph();
            Log::Info("[Morph] ability from %s: code=0x%08x kind=%u",
                      player->getName().c_str(), code, static_cast<unsigned>(Game::Morph::KindOf(code)));
            if (!Game::Morph::IsValid(code)) return;
            if (Game::Morph::KindOf(code) == Game::Morph::Kind::Item) {
                // A carried item's act: out of the holder's hand, thrown as
                // if they had pressed Q.
                if (g_integratedServer) g_integratedServer->Carry().Release(connection.GetPlayerId(), true);
                return;
            }
            ServerLevel* level = g_integratedServer ? g_integratedServer->GetLevel(source.dimension) : nullptr;
            ServerLevelBridge* mobLevel = level ? level->MobLevel() : nullptr;
            if (!mobLevel) return;

            if (Game::Morph::KindOf(code) == Game::Morph::Kind::Player) {
                // Herobrine: while another player is looking at him, a press
                // puts him two blocks behind that player (facing them) if
                // the space is clear. Otherwise — nobody looking, or no room
                // behind the watcher — he goes to the block his own
                // crosshair is on, so every press moves him.
                const Game::IBlockAccess* blocks = mobLevel->Blocks();
                if (!blocks) return;
                const glm::dvec3 me = player->getPosition();
                const Game::Morph::Dims dims = Game::Morph::DimsOf(code);
                // "In view" is the watcher's whole screen, not their
                // crosshair: any point of the body inside a 60° half-angle
                // cone (a 70° vertical FOV reaches ~55° into the screen
                // corners on a wide monitor) with nothing solid between it
                // and the watcher's eye. The nearest such watcher wins.
                const ServerPlayer* looking = nullptr;
                double lookingDist = 0.0;
                const double kCosHalfFov = std::cos(glm::radians(60.0));
                const glm::dvec3 samples[] = {
                    me + glm::dvec3(0.0, dims.height * 0.1, 0.0),
                    me + glm::dvec3(0.0, dims.height * 0.5, 0.0),
                    me + glm::dvec3(0.0, dims.height * 0.9, 0.0),
                };
                for (const auto& s : sessionManager.GetAllSessions()) {
                    const ServerPlayer* other = s ? s->GetPlayer() : nullptr;
                    if (!other || other == player ||
                        Game::DimensionFromRaw(static_cast<int8_t>(s->GetDimensionId())) != source.dimension) continue;
                    const glm::dvec3 eye = other->getPosition() + glm::dvec3(0.0, other->getEyeHeight(), 0.0);
                    const glm::dvec3 look(Game::Mth::ViewVector(other->getPitch(), other->getYaw()));
                    bool sees = false;
                    for (const glm::dvec3& point : samples) {
                        const glm::dvec3 to = point - eye;
                        const double dist = glm::length(to);
                        if (dist < 1.0e-6) { sees = true; break; }
                        if (glm::dot(look, to / dist) < kCosHalfFov) continue;
                        glm::ivec3 blocker;
                        if (ClipBlocksCollider(*blocks, eye, point, blocker)) continue;
                        sees = true;
                        break;
                    }
                    if (!sees) continue;
                    const double d = glm::length(other->getPosition() - me);
                    if (!looking || d < lookingDist) { looking = other; lookingDist = d; }
                }

                // A spot stands: solid under it, a body's worth of air on it.
                const int cells = static_cast<int>(std::ceil(dims.height));
                auto standable = [&](int bx, int by, int bz) {
                    if (!Game::BlockRegistry::HasCollision(blocks->GetBlock(bx, by - 1, bz))) return false;
                    for (int dy = 0; dy < cells; ++dy) {
                        if (Game::BlockRegistry::HasCollision(blocks->GetBlock(bx, by + dy, bz))) return false;
                        if (blocks->ContainsWater(bx, by + dy, bz)) return false;
                    }
                    return true;
                };
                if (looking) {
                    // Two blocks behind the watcher, at their level.
                    const glm::vec3 fwd = Game::Mth::ViewVector(0.0f, looking->getYaw());
                    const glm::dvec3 lp = looking->getPosition();
                    const glm::dvec3 behind(lp.x - fwd.x * 2.0, lp.y, lp.z - fwd.z * 2.0);
                    const glm::ivec3 bb(static_cast<int>(std::floor(behind.x)), static_cast<int>(std::floor(behind.y)),
                                        static_cast<int>(std::floor(behind.z)));
                    for (int dy = 0; dy <= 1; ++dy) {   // the exact level, or one step up
                        if (standable(bb.x, bb.y + dy, bb.z)) {
                            const glm::dvec3 dest(bb.x + 0.5, bb.y + dy, bb.z + 0.5);
                            // Landing facing the watcher.
                            const float yaw = Game::Mth::YRotFromVector(glm::vec3(lp - dest));
                            connection.Teleport(dest.x, dest.y, dest.z, yaw, 0.0f);
                            return;
                        }
                    }
                }
                // Nobody looking (or no room behind the watcher): he goes to
                // the block his own crosshair is on — the cell the look ray
                // hits, entered from the face it struck, dropped to the
                // ground — and keeps his own view.
                const glm::dvec3 eye = me + glm::dvec3(0.0, player->getEyeHeight(), 0.0);
                const glm::vec3 look = Game::Mth::ViewVector(player->getPitch(), player->getYaw());
                const glm::dvec3 far = eye + glm::dvec3(look) * 160.0;
                glm::ivec3 hit;
                if (!ClipBlocksCollider(*blocks, eye, far, hit)) return;
                // The face the ray entered the hit cell through: the axis
                // whose slab the ray crosses last on the way in.
                glm::ivec3 normal(0);
                {
                    double tBest = -1.0;
                    for (int axis = 0; axis < 3; ++axis) {
                        const double d = static_cast<double>(look[axis]);
                        if (std::abs(d) < 1.0e-9) continue;
                        const double plane = d > 0.0 ? static_cast<double>(hit[axis])
                                                     : static_cast<double>(hit[axis] + 1);
                        const double t = (plane - eye[axis]) / d;
                        if (t > tBest) {
                            tBest = t;
                            normal = glm::ivec3(0);
                            normal[axis] = d > 0.0 ? -1 : 1;
                        }
                    }
                }
                glm::ivec3 cell = hit + normal;
                // Down to the ground under that cell (a side face aimed at a
                // wall lands at its foot), then one step up if the floor cell
                // itself is too tight.
                for (int drop = 0; drop < 64 && cell.y > -64 &&
                     !Game::BlockRegistry::HasCollision(blocks->GetBlock(cell.x, cell.y - 1, cell.z)); ++drop) {
                    --cell.y;
                }
                for (int dy = 0; dy <= 1; ++dy) {
                    if (standable(cell.x, cell.y + dy, cell.z)) {
                        connection.Teleport(cell.x + 0.5, static_cast<double>(cell.y + dy), cell.z + 0.5,
                                            player->getYaw(), player->getPitch());
                        return;
                    }
                }
                return;
            }
            if (Game::Morph::KindOf(code) != Game::Morph::Kind::Mob) return;
            const auto type = static_cast<Game::EntityTypeId>(Game::Morph::MobTypeOf(code));
            const glm::dvec3 pos = player->getPosition();

            if (type == Game::EntityTypeId::Enderman) {
                // MC Enderman.teleport: a random spot within ±32 blocks and
                // ±32 up or down, dropped to the ground, refused in water
                // or a body's worth of blocks. One press is a handful of
                // tries, since a real enderman keeps trying every tick.
                const Game::IBlockAccess* blocks = mobLevel->Blocks();
                if (!blocks) return;
                Game::JavaRandom& rng = mobLevel->Random();
                const Game::Morph::Dims dims = Game::Morph::DimsOf(code);
                for (int attempt = 0; attempt < 16; ++attempt) {
                    const double x = pos.x + (rng.NextDouble() - 0.5) * 64.0;
                    const double z = pos.z + (rng.NextDouble() - 0.5) * 64.0;
                    int y = static_cast<int>(std::floor(pos.y + (rng.NextInt(64) - 32)));
                    const int bx = static_cast<int>(std::floor(x)), bz = static_cast<int>(std::floor(z));
                    while (y > -64 && !Game::BlockRegistry::HasCollision(blocks->GetBlock(bx, y, bz))) --y;
                    if (y <= -64) continue;
                    if (blocks->ContainsWater(bx, y, bz) || blocks->ContainsWater(bx, y + 1, bz)) continue;
                    bool free = true;
                    const int cells = static_cast<int>(std::ceil(dims.height));
                    for (int dy = 1; dy <= cells && free; ++dy) {
                        if (Game::BlockRegistry::HasCollision(blocks->GetBlock(bx, y + dy, bz))) free = false;
                    }
                    if (!free) continue;
                    connection.Teleport(x, static_cast<double>(y + 1), z, player->getYaw(), player->getPitch());
                    return;
                }
                return;
            }
            if (type == Game::EntityTypeId::Armadillo) {
                // Rolls up / unrolls (MC Armadillo.rollUp / rollOut): the
                // flag bit, drawn as the shell pose everywhere.
                player->setMorph(Game::Morph::WithSheared(code, !Game::Morph::IsSheared(code)), player->getMorphSpeed());
                connection.SendPlayerAbilities(*player);
                return;
            }
            if (type == Game::EntityTypeId::Chicken) {
                // MC Chicken.aiStep, eggTime run out: an egg at the feet.
                mobLevel->SpawnItemDrop(pos, Game::Items::Egg, 1);
                return;
            }
            if (type == Game::EntityTypeId::Sheep) {
                // MC EatBlockGoal: grass at the feet is eaten, a grass block
                // below turns to dirt (both behind mobGriefing); eating
                // grows the wool back (Sheep.ate).
                const Game::IBlockAccess* blocks = mobLevel->Blocks();
                if (!blocks) return;
                const glm::ivec3 feet(static_cast<int>(std::floor(pos.x)), static_cast<int>(std::floor(pos.y + 1.0e-4)),
                                      static_cast<int>(std::floor(pos.z)));
                const glm::ivec3 below(feet.x, feet.y - 1, feet.z);
                const Game::BlockID at = blocks->GetBlock(feet.x, feet.y, feet.z);
                const bool edible = at == Game::BlockID::ShortGrass || at == Game::BlockID::Fern ||
                                    at == Game::BlockID::ShortDryGrass || at == Game::BlockID::TallDryGrass;
                Log::Info("[Morph] sheep graze (server): feet=(%d,%d,%d) at=%u below=%u sheared=%d",
                          feet.x, feet.y, feet.z, static_cast<unsigned>(at),
                          static_cast<unsigned>(blocks->GetBlock(below.x, below.y, below.z)),
                          Game::Morph::IsSheared(code) ? 1 : 0);
                bool ate = false;
                if (edible) {
                    if (mobLevel->MobGriefing()) mobLevel->DestroyBlock(feet, false);
                    ate = true;
                } else if (blocks->GetBlock(below.x, below.y, below.z) == Game::BlockID::Grass) {
                    if (mobLevel->MobGriefing()) mobLevel->SetBlock(below, Game::BlockID::Dirt);
                    ate = true;
                }
                if (ate && Game::Morph::IsSheared(code)) {
                    player->setMorph(Game::Morph::WithSheared(code, false), player->getMorphSpeed());
                    connection.SendPlayerAbilities(*player);
                }
                return;
            }
            if (type == Game::EntityTypeId::Skeleton || type == Game::EntityTypeId::Stray ||
                type == Game::EntityTypeId::Bogged) {
                // MC AbstractSkeleton.performRangedAttack, aimed where the
                // player looks instead of at a target: from the eye, speed
                // 1.6, the difficulty's inaccuracy, the player as owner.
                auto arrow = std::make_unique<Game::Arrow>(mobLevel);
                arrow->SetOwner(g_integratedServer->GetPlayerEntityView(connection.GetPlayerId()));
                arrow->position = glm::dvec3(pos.x, pos.y + player->getEyeHeight() - 0.1, pos.z);
                arrow->SetBaseDamageFromMob(1.0f);
                const glm::vec3 dir = Game::Mth::ViewVector(player->getPitch(), player->getYaw());
                const int difficultyId = static_cast<int>(mobLevel->GetDifficulty());
                arrow->Shoot(dir.x, dir.y, dir.z, 1.6f, static_cast<float>(14 - difficultyId * 4));
                mobLevel->AddFreshEntity(std::move(arrow));
                return;
            }
            return;
        }
        if (first == "boom") {
            // A creeper morph held to a full swell: the creeper's own
            // explosion (MC Creeper.explodeCreeper — radius 3, MOB
            // interaction, so mobGriefing decides the blocks), from the
            // body's feet, attributed to the player. The morph stays.
            if (!Game::Morph::IsMob(player->getMorph(), Game::EntityTypeId::Creeper)) return;
            ServerLevel* level = g_integratedServer ? g_integratedServer->GetLevel(source.dimension) : nullptr;
            if (!level || !level->MobLevel()) return;
            Game::ExplosionParams params;
            params.center       = player->getPosition();
            params.radius       = 3.0f;
            params.source       = g_integratedServer->GetPlayerEntityView(connection.GetPlayerId());
            params.attributedTo = params.source;
            params.interaction  = Game::ExplosionInteraction::Mob;
            Game::Explode(*level->MobLevel(), params);
            player->setMorphAnim(0);
            return;
        }
        if (first == "rotate") {
            // Shift+Alt on a block morph: a quarter turn, in the code so
            // every client turns the block.
            const uint32_t code = player->getMorph();
            if (!Game::Morph::IsValid(code) || Game::Morph::KindOf(code) != Game::Morph::Kind::Block) {
                connection.SendChatMessage("Only a block morph rotates", 1);
                return;
            }
            player->setMorph(Game::Morph::WithBlockRotation(code, Game::Morph::BlockRotationOf(code) + 1), 0.0f);
            connection.SendPlayerAbilities(*player);
            // Locked into a cell: the world's block turns with the morph.
            if (g_integratedServer) g_integratedServer->BlockAnchor().Refresh(connection.GetPlayerId());
            return;
        }
        if (first == "off" || first == "none" || first == "player") {
            apply(Game::Morph::kNone, 0.0f, "You are yourself again");
            return;
        }
        if (first == "herobrine") {
            apply(Game::Morph::Encode(Game::Morph::Kind::Player, Game::Morph::kHerobrine), 0.0f,
                  "You are now Herobrine");
            return;
        }
        if (first == "xp" || first == "experience_orb" || first == "orb") {
            apply(Game::Morph::Encode(Game::Morph::Kind::Xp, 0), 0.0f, "You are now an experience orb");
            return;
        }
        if (first == "item") {
            if (args.size() < 2) { connection.SendChatMessage("Usage: /morph item <item>", 1); return; }
            Game::ItemID item{};
            if (!Game::Morph::ParseItemSlug(args[1], item)) {
                connection.SendChatMessage("Unknown item: " + args[1], 1);
                return;
            }
            apply(Game::Morph::Encode(Game::Morph::Kind::Item, item), 0.0f,
                  "You are now a dropped " + Game::ItemRegistry::Get(item).name);
            return;
        }
        if (first == "block") {
            if (args.size() < 2) { connection.SendChatMessage("Usage: /morph block <block>", 1); return; }
            Game::BlockState state;
            std::string error;
            // There is no "bed" block, only coloured ones; the plain word
            // gets the default red one (MC's is red too).
            std::string blockName = args[1];
            if (blockName == "bed") blockName = "red_bed";
            if (!ParseBlockState(blockName, state, error) || state.Block() == Game::BlockID::Air) {
                connection.SendChatMessage(error.empty() ? "Unknown block: " + args[1] : error, 1);
                return;
            }
            apply(Game::Morph::Encode(Game::Morph::Kind::Block, static_cast<uint32_t>(state.Block())), 0.0f,
                  "You are now a " + Game::BlockRegistry::Get(state.Block()).name + " block");
            return;
        }

        // `/morph baby <entity>` (or `/morph <entity> baby`): the mob's baby
        // form. Whether the type HAS one is the mob's own answer below
        // (Mob::SetBaby is a no-op on a mob with no baby form — the spawn-
        // egg-on-parent rule), so nothing here keeps a list.
        std::string entityArg = args[0];
        bool baby = false;
        if (first == "baby") {
            if (args.size() < 2) { connection.SendChatMessage("Usage: /morph baby <entity>", 1); return; }
            baby = true;
            entityArg = args[1];
        } else if (args.size() >= 2 && Lower(args[1]) == "baby") {
            baby = true;
        }

        Game::EntityTypeId type{};
        if (!SummonCommand::ParseEntityType(entityArg, type)) {
            connection.SendChatMessage("Unknown entity type: " + entityArg, 1);
            return;
        }
        const std::string slug(Game::GetEntityTypeInfo(type).slug);

        // The speed the mob's own move control walks it at: MOVEMENT_SPEED
        // times the land factor MC's SmoothSwimmingMoveControl applies out
        // of the water (a frog's 1.0 is a SWIM speed; on land it is 0.1 of
        // that). Read off an instance built the way a saved mob is rebuilt
        // and dropped again, since every type registers its attributes and
        // picks its control in its constructor.
        // A type with no AI (an end crystal, a projectile) has no move
        // control and no speed of its own: it takes the player's, like an
        // item does (speed 0 → PlayerPhysics keeps the walk factor at 1).
        float speed = 0.0f;
        bool  built = false;
        ServerLevel* level = g_integratedServer ? g_integratedServer->GetLevel(source.dimension) : nullptr;
        if (level && level->MobLevel()) {
            if (std::unique_ptr<Game::Mob> mob = MakeMobForLoad(type, level->MobLevel())) {
                built = true;
                if (baby) {
                    // The baby's own speed where the type has one (a baby
                    // zombie's SPEED_MODIFIER_BABY rides SetBaby), and the
                    // type's word on whether a baby form exists at all.
                    mob->SetBaby(true);
                    if (!mob->IsBaby()) {
                        connection.SendChatMessage("A " + slug + " has no baby form", 1);
                        return;
                    }
                }
                if (mob->HasAiControls()) {
                    float land = mob->GetLandSpeedFactor();
                    if (const auto* swim = dynamic_cast<const Game::SmoothSwimmingMoveControl*>(&mob->GetMoveControl())) {
                        land *= swim->OutsideWaterSpeedModifier();
                    }
                    speed = static_cast<float>(mob->GetAttributeValue(Game::Attribute::MovementSpeed)) * land;
                }
            }
        }
        if (!built) {
            connection.SendChatMessage("Cannot morph into " + slug, 1);
            return;
        }
        const uint32_t code = Game::Morph::WithBaby(
            Game::Morph::Encode(Game::Morph::Kind::Mob, static_cast<uint32_t>(type)), baby);
        apply(code, speed, std::string("You are now a ") + (baby ? "baby " : "") + slug);
    }

} // namespace Server
