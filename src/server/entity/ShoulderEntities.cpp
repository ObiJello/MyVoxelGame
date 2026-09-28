// File: src/server/entity/ShoulderEntities.cpp
#include "server/entity/ShoulderEntities.hpp"

#include "server/IntegratedServer.hpp"
#include "server/entity/MobManager.hpp"
#include "server/entity/ServerLevelBridge.hpp"
#include "server/level/LevelEntityStore.hpp"   // MakeMobForLoad
#include "server/level/ServerLevel.hpp"
#include "server/network/ServerConnection.hpp"
#include "server/player/ServerPlayer.hpp"
#include "server/session/PlayerSession.hpp"
#include "server/session/PlayerSessionManager.hpp"
#include "server/world/storage/NBTParser.hpp"
#include "server/world/storage/anvil/EntityNbt.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/core/Log.hpp"
#include "common/entity/EntityType.hpp"
#include "common/entity/Mob.hpp"
#include "common/entity/TamableAnimal.hpp"
#include "common/entity/mobs/Animals.hpp"
#include "common/nbt/NbtWrite.hpp"
#include "common/network/PacketRegistry.hpp"
#include "common/network/packets/game/ShoulderParrotsS2CPacket.hpp"
#include "common/physics/Physics.hpp"
#include "common/world/block/Blocks.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/level/DimensionId.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace Server::ShoulderEntities {

    namespace {

        using ::World::NBTTag;
        using ::World::NBTTagCompound;
        using ::World::NBTTagList;
        using ::World::NBTTagType;

        // MC ServerPlayer.removeEntitiesOnShoulder's guard: a rider that sat
        // down within this many ticks stays put.
        constexpr int64_t kShoulderRemovalGuardTicks = 20;
        // MC ServerPlayer.respawnEntityOnShoulder: setPos(x, y + 0.7, z).
        constexpr double kRespawnHeight = 0.699999988079071;   // (double)0.7F

        // ── Where the player is ────────────────────────────────────────────

        ServerLevel* LevelOf(const ServerPlayer& player) {
            auto* server = g_integratedServer.get();
            return server ? server->GetLevel(Game::DimensionFromRaw(player.getDimensionId())) : nullptr;
        }

        PlayerEntityView* ViewOf(const ServerPlayer& player) {
            // Player ids and connection ids are the same number
            // (IntegratedServer::GetPlayerEntityView).
            auto* server = g_integratedServer.get();
            return server ? server->GetPlayerEntityView(player.getPlayerId()) : nullptr;
        }

        // MC Entity.isInPowderSnow: the (1e-5 deflated) box overlaps a powder
        // snow cell — see ParrotGoals.cpp; the engine keeps no flag for it.
        bool IsInPowderSnow(const Game::Entity& entity) {
            const Game::EntityLevel* level = entity.Level();
            const Game::IBlockAccess* blocks = level ? level->Blocks() : nullptr;
            if (!blocks) return false;
            const Game::AABBd box = entity.GetAABBd();
            constexpr double kDeflate = 1.0e-5;
            const auto lo = glm::ivec3(glm::floor(box.min + kDeflate));
            const auto hi = glm::ivec3(glm::floor(box.max - kDeflate));
            for (int y = lo.y; y <= hi.y; ++y) {
                for (int z = lo.z; z <= hi.z; ++z) {
                    for (int x = lo.x; x <= hi.x; ++x) {
                        if (blocks->GetBlock(x, y, z) == Game::BlockID::PowderSnow) return true;
                    }
                }
            }
            return false;
        }

        // ── NBT: entity → compound (MC saveWithoutId + putString("id")) ────

        std::shared_ptr<NBTTagCompound> SaveEntity(const Game::Mob& entity) {
            // Anvil::WriteMob is the one entity writer (the chunk's
            // "Entities" list element: "id" first, then every layer), so the
            // compound is written as a one-element list and read back.
            Game::Nbt::Writer w;
            w.BeginRootCompound();
            auto list = w.BeginList("Entities", Game::Nbt::TagType::Compound);
            if (!Game::Anvil::WriteMob(w, list, entity)) return nullptr;
            w.EndList(list);
            w.EndRootCompound();
            if (!w.ok()) return nullptr;
            try {
                auto root = std::dynamic_pointer_cast<NBTTagCompound>(::World::NBTParser::Parse(w.Bytes()));
                if (!root) return nullptr;
                auto entities = std::dynamic_pointer_cast<NBTTagList>(root->GetTag("Entities"));
                if (!entities || entities->value.empty()) return nullptr;
                return std::dynamic_pointer_cast<NBTTagCompound>(entities->value.front());
            } catch (const std::exception& e) {
                Log::Error("[Shoulder] could not re-read a shoulder entity: %s", e.what());
                return nullptr;
            }
        }

        // ── NBT: parsed compound → writer (for the player file) ────────────

        void WriteCompoundEntries(Game::Nbt::Writer& w, const NBTTagCompound& compound);

        void WriteListElements(Game::Nbt::Writer& w, Game::Nbt::Writer::ListScope& scope,
                               const NBTTagList& list) {
            for (const auto& element : list.value) {
                if (!element) continue;
                switch (list.listType) {
                    case NBTTagType::TAG_Byte:
                        w.ListByte(scope, static_cast<const ::World::NBTTagByte&>(*element).value); break;
                    case NBTTagType::TAG_Short:
                        w.ListShort(scope, static_cast<const ::World::NBTTagShort&>(*element).value); break;
                    case NBTTagType::TAG_Int:
                        w.ListInt(scope, static_cast<const ::World::NBTTagInt&>(*element).value); break;
                    case NBTTagType::TAG_Long:
                        w.ListLong(scope, static_cast<const ::World::NBTTagLong&>(*element).value); break;
                    case NBTTagType::TAG_Float:
                        w.ListFloat(scope, static_cast<const ::World::NBTTagFloat&>(*element).value); break;
                    case NBTTagType::TAG_Double:
                        w.ListDouble(scope, static_cast<const ::World::NBTTagDouble&>(*element).value); break;
                    case NBTTagType::TAG_String:
                        w.ListString(scope, static_cast<const ::World::NBTTagString&>(*element).value); break;
                    case NBTTagType::TAG_Byte_Array: {
                        const auto& a = static_cast<const ::World::NBTTagByteArray&>(*element).value;
                        w.ListByteArray(scope, a.data(), a.size());
                        break;
                    }
                    case NBTTagType::TAG_Int_Array: {
                        const auto& a = static_cast<const ::World::NBTTagIntArray&>(*element).value;
                        w.ListIntArray(scope, a.data(), a.size());
                        break;
                    }
                    case NBTTagType::TAG_Long_Array: {
                        const auto& a = static_cast<const ::World::NBTTagLongArray&>(*element).value;
                        w.ListLongArray(scope, a.data(), a.size());
                        break;
                    }
                    case NBTTagType::TAG_Compound:
                        w.ListCompoundBegin(scope);
                        WriteCompoundEntries(w, static_cast<const NBTTagCompound&>(*element));
                        w.ListCompoundEnd(scope);
                        break;
                    case NBTTagType::TAG_List: {
                        const auto& inner = static_cast<const NBTTagList&>(*element);
                        auto innerScope = w.ListListBegin(scope, static_cast<Game::Nbt::TagType>(inner.listType));
                        WriteListElements(w, innerScope, inner);
                        w.EndList(innerScope);
                        break;
                    }
                    case NBTTagType::TAG_End:
                        break;
                }
            }
        }

        void WriteNamed(Game::Nbt::Writer& w, const std::string& name, const NBTTag& tag) {
            switch (tag.type) {
                case NBTTagType::TAG_Byte:   w.Byte  (name, static_cast<const ::World::NBTTagByte&>(tag).value);   break;
                case NBTTagType::TAG_Short:  w.Short (name, static_cast<const ::World::NBTTagShort&>(tag).value);  break;
                case NBTTagType::TAG_Int:    w.Int   (name, static_cast<const ::World::NBTTagInt&>(tag).value);    break;
                case NBTTagType::TAG_Long:   w.Long  (name, static_cast<const ::World::NBTTagLong&>(tag).value);   break;
                case NBTTagType::TAG_Float:  w.Float (name, static_cast<const ::World::NBTTagFloat&>(tag).value);  break;
                case NBTTagType::TAG_Double: w.Double(name, static_cast<const ::World::NBTTagDouble&>(tag).value); break;
                case NBTTagType::TAG_String: w.String(name, static_cast<const ::World::NBTTagString&>(tag).value); break;
                case NBTTagType::TAG_Byte_Array: {
                    const auto& a = static_cast<const ::World::NBTTagByteArray&>(tag).value;
                    w.ByteArray(name, a.data(), a.size());
                    break;
                }
                case NBTTagType::TAG_Int_Array: {
                    const auto& a = static_cast<const ::World::NBTTagIntArray&>(tag).value;
                    w.IntArray(name, a.data(), a.size());
                    break;
                }
                case NBTTagType::TAG_Long_Array: {
                    const auto& a = static_cast<const ::World::NBTTagLongArray&>(tag).value;
                    w.LongArray(name, a.data(), a.size());
                    break;
                }
                case NBTTagType::TAG_List: {
                    const auto& list = static_cast<const NBTTagList&>(tag);
                    auto scope = w.BeginList(name, static_cast<Game::Nbt::TagType>(list.listType));
                    WriteListElements(w, scope, list);
                    w.EndList(scope);
                    break;
                }
                case NBTTagType::TAG_Compound:
                    w.BeginCompound(name);
                    WriteCompoundEntries(w, static_cast<const NBTTagCompound&>(tag));
                    w.EndCompound();
                    break;
                case NBTTagType::TAG_End:
                    break;
            }
        }

        void WriteCompoundEntries(Game::Nbt::Writer& w, const NBTTagCompound& compound) {
            // Sorted, so the same entity always writes the same bytes.
            std::vector<const std::pair<const std::string, ::World::NBTTagPtr>*> entries;
            entries.reserve(compound.value.size());
            for (const auto& entry : compound.value) {
                if (entry.second) entries.push_back(&entry);
            }
            std::sort(entries.begin(), entries.end(),
                      [](const auto* a, const auto* b) { return a->first < b->first; });
            for (const auto* entry : entries) WriteNamed(w, entry->first, *entry->second);
        }

        // ── The respawn (MC ServerPlayer.respawnEntityOnShoulder) ──────────

        void RespawnEntityOnShoulder(const std::shared_ptr<NBTTagCompound>& tag, ServerPlayer& player,
                                     ServerLevel& level, PlayerEntityView* view) {
            if (!tag) return;
            MobManager*        mobs   = level.Mobs();
            ServerLevelBridge* bridge = level.MobLevel();
            if (!mobs || !bridge) return;

            // EntityType.create(tag, level, LOAD).
            Game::EntityTypeId type{};
            if (Game::Anvil::ClassifyEntity(*tag, type) != Game::Anvil::EntityKind::Mob) {
                Log::Warning("[Shoulder] '%s': shoulder entity '%s' cannot be created; dropped",
                             player.getName().c_str(), tag->GetValue<std::string>("id").c_str());
                return;
            }
            std::unique_ptr<Game::Mob> mob = MakeMobForLoad(type, static_cast<Game::EntityLevel*>(bridge));
            if (!mob) return;
            Game::Anvil::ApplyMobNbt(*tag, *mob);

            // `if (entity instanceof TamableAnimal tamed) tamed.setOwner(this)`.
            if (auto* tamed = dynamic_cast<Game::TamableAnimal*>(mob.get()); tamed && view) {
                tamed->SetOwner(view);
            }
            // entity.setPos(x, y + 0.7, z) — a fresh placement, so last tick's
            // position moves with it.
            const glm::dvec3 pos = player.getPosition() + glm::dvec3(0.0, kRespawnHeight, 0.0);
            mob->position    = pos;
            mob->oldPosition = pos;

            // serverLevel.addWithUUID: refused if that UUID is already in the
            // level (MC logs and drops the entity).
            if (!Game::UuidIsNil(mob->GetUuid()) && mobs->HasUuid(mob->GetUuid())) {
                Log::Warning("[Shoulder] '%s': shoulder entity UUID already in the level; dropped",
                             player.getName().c_str());
                return;
            }
            mobs->Add(std::move(mob));
        }

        // ── Chatter (MC ServerPlayer.playShoulderEntityAmbientSound) ───────

        void PlayShoulderEntityAmbientSound(const std::shared_ptr<NBTTagCompound>& tag,
                                            ServerPlayer& player, ServerLevelBridge& level,
                                            PlayerEntityView& view) {
            if (!tag || tag->value.empty() || tag->GetValue<int8_t>("Silent", 0) != 0) return;
            Game::JavaRandom& random = player.soundRandom();   // MC this.random
            if (random.NextInt(200) != 0) return;
            Game::EntityTypeId type{};
            if (Game::Anvil::ClassifyEntity(*tag, type) != Game::Anvil::EntityKind::Mob ||
                type != Game::EntityTypeId::Parrot) {
                return;
            }
            if (Game::Parrot::ImitateNearbyMobs(level, view)) return;
            const char* event = Game::Parrot::GetAmbient(level, random);
            const float pitch = Game::Parrot::GetPitch(random);
            if (event && event[0]) {
                level.PlaySound(nullptr, player.getPosition(), event, view.GetSoundSource(), 1.0f, pitch);
            }
        }

        void SendTo(ServerConnection& connection, const ServerPlayer& player) {
            Network::ShoulderParrotsS2CPacket packet;
            packet.playerId = player.getPlayerId();
            packet.left  = static_cast<int8_t>(player.getShoulderParrotLeft());
            packet.right = static_cast<int8_t>(player.getShoulderParrotRight());
            connection.SendPacket(static_cast<uint8_t>(Network::PacketId::ShoulderParrotsS2C),
                                  Network::Serialization::Serialize(packet));
        }

    } // namespace

    int ParrotVariantOf(const ::World::NBTTagCompound* tag) {
        // MC Player.extractParrotVariant: a parrot's "Variant" through the
        // LEGACY_CODEC (an int id, clamped by Variant.byId); absent → none.
        if (!tag || tag->value.empty()) return -1;
        Game::EntityTypeId type{};
        if (Game::Anvil::ClassifyEntity(*tag, type) != Game::Anvil::EntityKind::Mob ||
            type != Game::EntityTypeId::Parrot) {
            return -1;
        }
        if (!std::dynamic_pointer_cast<::World::NBTTagInt>(tag->GetTag("Variant"))) return -1;
        return static_cast<int>(Game::Parrot::VariantById(tag->GetValue<int32_t>("Variant", 0)));
    }

    bool SetEntityOnShoulder(PlayerEntityView& view, Game::Mob& entity, int64_t gameTime) {
        ServerPlayer* player = view.GetPlayer();
        if (!player || entity.IsRemoved()) return false;

        // MC ServerPlayer.setEntityOnShoulder's gate.
        if (view.IsPassenger() || !player->isOnGround() || view.IsInWater() || IsInPowderSnow(view)) {
            return false;
        }
        const bool leftFree  = !player->getShoulderEntityLeft();
        const bool rightFree = !player->getShoulderEntityRight();
        if (!leftFree && !rightFree) return false;

        // ShoulderRidingEntity: the entity's whole save, then discard().
        std::shared_ptr<NBTTagCompound> tag = SaveEntity(entity);
        if (!tag) return false;
        const int variant = ParrotVariantOf(tag.get());
        if (leftFree) player->setShoulderEntityLeft(std::move(tag), variant);
        else          player->setShoulderEntityRight(std::move(tag), variant);
        player->setTimeEntitySatOnShoulder(gameTime);
        entity.Discard();
        Broadcast(*player);
        return true;
    }

    void RemoveEntitiesOnShoulder(ServerPlayer& player) {
        ServerLevel* level = LevelOf(player);
        ServerLevelBridge* bridge = level ? level->MobLevel() : nullptr;
        if (!bridge) return;
        if (!(player.getTimeEntitySatOnShoulder() + kShoulderRemovalGuardTicks < bridge->GetGameTime())) return;
        if (!player.getShoulderEntityLeft() && !player.getShoulderEntityRight()) return;

        PlayerEntityView* view = ViewOf(player);
        RespawnEntityOnShoulder(player.getShoulderEntityLeft(), player, *level, view);
        player.setShoulderEntityLeft(nullptr, -1);
        RespawnEntityOnShoulder(player.getShoulderEntityRight(), player, *level, view);
        player.setShoulderEntityRight(nullptr, -1);
        Broadcast(player);
    }

    void Tick(ServerPlayer& player) {
        ServerLevel* level = LevelOf(player);
        ServerLevelBridge* bridge = level ? level->MobLevel() : nullptr;
        PlayerEntityView* view = ViewOf(player);
        if (!bridge || !view) return;

        // MC ServerPlayer.setGameMode(SPECTATOR): removeEntitiesOnShoulder on
        // the change into spectator.
        const bool spectator = player.isSpectator();
        if (spectator && !player.shoulderSawSpectator()) player.requestShoulderEntityRemoval();
        player.setShoulderSawSpectator(spectator);

        // Player.hurtServer / ServerPlayer.die / the spectator switch.
        if (player.takeShoulderEntityRemovalRequest()) RemoveEntitiesOnShoulder(player);

        if (player.isDead()) return;
        if (!player.getShoulderEntityLeft() && !player.getShoulderEntityRight()) return;

        // MC ServerPlayer.handleShoulderEntities' chatter. Its drop rules
        // (falling > 0.5, water, flying, sleeping, powder snow) are
        // DELIBERATELY not applied: the parrots stay put until the player
        // jumps while sneaking (PlayerSession::HandlePlayerMove), dies,
        // switches to spectator or leaves.
        PlayShoulderEntityAmbientSound(player.getShoulderEntityLeft(), player, *bridge, *view);
        PlayShoulderEntityAmbientSound(player.getShoulderEntityRight(), player, *bridge, *view);
    }

    void DropOnDisconnect(ServerPlayer& player) {
        // A deliberate deviation: MC keeps the riders in the player file;
        // here they are set down in the world as the player leaves (no
        // 20-tick guard — there is no later chance).
        if (!player.getShoulderEntityLeft() && !player.getShoulderEntityRight()) return;
        player.setTimeEntitySatOnShoulder(std::numeric_limits<int64_t>::min() / 2);
        RemoveEntitiesOnShoulder(player);
    }

    void Broadcast(const ServerPlayer& player) {
        auto* server = g_integratedServer.get();
        PlayerSessionManager* sessions = server ? server->GetSessionManager() : nullptr;
        if (!sessions) return;
        for (const auto& session : sessions->GetAllSessions()) {
            if (!session) continue;
            if (ServerConnection* connection = session->GetConnection()) SendTo(*connection, player);
        }
    }

    void SyncOnJoin(ServerConnection& joining, const ServerPlayer& joiner) {
        auto* server = g_integratedServer.get();
        PlayerSessionManager* sessions = server ? server->GetSessionManager() : nullptr;
        if (!sessions) {
            SendTo(joining, joiner);
            return;
        }
        // Everyone already here, to the newcomer...
        for (const auto& session : sessions->GetAllSessions()) {
            if (!session || !session->GetPlayer() || session->GetPlayer() == &joiner) continue;
            SendTo(joining, *session->GetPlayer());
        }
        // ...and the newcomer's own pair to everyone, the newcomer included
        // (an empty pair too: it clears whatever a reused id left behind).
        Broadcast(joiner);
    }

    void WritePlayerData(Game::Nbt::Writer& w, const ServerPlayer& player) {
        if (const auto& left = player.getShoulderEntityLeft(); left && !left->value.empty()) {
            w.BeginCompound("ShoulderEntityLeft");
            WriteCompoundEntries(w, *left);
            w.EndCompound();
        }
        if (const auto& right = player.getShoulderEntityRight(); right && !right->value.empty()) {
            w.BeginCompound("ShoulderEntityRight");
            WriteCompoundEntries(w, *right);
            w.EndCompound();
        }
    }

    void ReadPlayerData(const ::World::NBTTagCompound& data, ServerPlayer& player) {
        const auto read = [&](const char* key) -> std::shared_ptr<NBTTagCompound> {
            auto tag = std::dynamic_pointer_cast<NBTTagCompound>(data.GetTag(key));
            return (tag && !tag->value.empty()) ? tag : nullptr;
        };
        auto left  = read("ShoulderEntityLeft");
        auto right = read("ShoulderEntityRight");
        const int leftVariant  = ParrotVariantOf(left.get());
        const int rightVariant = ParrotVariantOf(right.get());
        player.setShoulderEntityLeft(std::move(left), leftVariant);
        player.setShoulderEntityRight(std::move(right), rightVariant);
    }

} // namespace Server::ShoulderEntities
