// File: src/common/world/level/WorldMobSpawn.cpp
#include "WorldMobSpawn.hpp"
#include "server/IntegratedServer.hpp"
#include "server/entity/ServerLevelBridge.hpp"
#include "server/level/ServerLevel.hpp"
#include "server/level/LevelEntityStore.hpp"
#include "server/entity/MobManager.hpp"
#include "server/world/storage/anvil/EntityNbt.hpp"
#include "server/world/storage/NBTParser.hpp"
#include "common/nbt/NbtWrite.hpp"
#include "common/entity/Mob.hpp"

#include <exception>

namespace Game {

    bool SpawnMobFromItem(EntityTypeId type, const glm::ivec3& spawnPos,
                          bool tryMoveDown, bool movedUp, DimensionId dimension,
                          int portalCooldownTicks,
                          const std::function<void(Mob&)>& configure,
                          SpawnReason reason) {
        auto* server = Server::g_integratedServer.get();
        if (!server) return false;
        return server->SpawnMobFromItemUse(type, spawnPos, tryMoveDown, movedUp, dimension,
                                           portalCooldownTicks, configure, reason);
    }

    bool AnyPlayerCloseEnoughForSpawning(DimensionId dimension, const glm::ivec3& pos) {
        auto* server = Server::g_integratedServer.get();
        if (!server) return false;
        Server::ServerLevel* level = server->GetLevel(dimension);
        Server::ServerLevelBridge* bridge = level ? level->MobLevel() : nullptr;
        if (!bridge) return false;
        // euclideanDistanceSquared(chunkPos, player): chunk centre to the
        // player, XZ only, < 128².
        const double centerX = static_cast<double>(pos.x >> 4) * 16.0 + 8.0;
        const double centerZ = static_cast<double>(pos.z >> 4) * 16.0 + 8.0;
        for (const Server::PlayerEntityView* player : bridge->PlayerViews()) {
            if (!player || player->IsSpectator()) continue;
            const double dx = player->position.x - centerX;
            const double dz = player->position.z - centerZ;
            if (dx * dx + dz * dz < 16384.0) return true;
        }
        return false;
    }

    std::shared_ptr<::World::NBTTagCompound> SaveMobData(const Mob& mob) {
        Nbt::Writer w;
        w.BeginRootCompound();
        if (!Anvil::WriteMobCompound(w, "entity", mob)) return nullptr;
        w.EndRootCompound();
        if (!w.ok()) return nullptr;
        size_t offset = 0;
        ::World::NBTTagPtr root;
        try {
            root = ::World::NBTTag::ParseTag(w.Bytes(), offset, true);
        } catch (const std::exception&) {
            return nullptr;
        }
        auto rootCompound = std::dynamic_pointer_cast<::World::NBTTagCompound>(root);
        if (!rootCompound) return nullptr;
        auto saved = std::dynamic_pointer_cast<::World::NBTTagCompound>(rootCompound->GetTag("entity"));
        if (saved) saved->value.erase("id");
        return saved;
    }

    Mob* SpawnMobFromSavedData(DimensionId dimension, std::string_view typeSlug,
                               const ::World::NBTTagCompound& data,
                               const std::function<void(Mob&)>& place) {
        auto* server = Server::g_integratedServer.get();
        if (!server) return nullptr;
        Server::ServerLevel* level = server->GetLevel(dimension);
        Server::ServerLevelBridge* bridge = level ? level->MobLevel() : nullptr;
        Server::MobManager* mobs = level ? level->Mobs() : nullptr;
        if (!bridge || !mobs) return nullptr;
        EntityTypeId type{};
        if (!Anvil::EntityTypeFromName(typeSlug, type)) return nullptr;
        std::unique_ptr<Mob> mob = Server::MakeMobForLoad(type, static_cast<EntityLevel*>(bridge));
        if (!mob) return nullptr;
        Anvil::ApplyMobNbt(data, *mob);
        if (place) place(*mob);
        if (!UuidIsNil(mob->GetUuid()) && mobs->HasUuid(mob->GetUuid())) return nullptr;
        const int32_t id = mobs->Add(std::move(mob));
        return id != 0 ? mobs->Find(id) : nullptr;
    }

} // namespace Game
