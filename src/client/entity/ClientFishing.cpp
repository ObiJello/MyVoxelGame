// File: src/client/entity/ClientFishing.cpp
#include "client/entity/ClientFishing.hpp"

#include "client/entity/ClientMobManager.hpp"
#include "client/entity/Player.hpp"
#include "client/entity/RemotePlayerManager.hpp"
#include "client/network/ClientConnection.hpp"
#include "client/network/NetworkClient.hpp"
#include "client/world/ClientBlockAccess.hpp"
#include "client/world/ClientLevel.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/entity/Inventory.hpp"
#include "common/entity/projectile/FishingHook.hpp"
#include "common/network/packets/game/FishingHookDataS2CPacket.hpp"

namespace Client::Fishing {

    namespace {

        uint32_t LocalPlayerId() {
            if (!g_networkClient) return UINT32_MAX;
            auto connection = g_networkClient->GetConnection();
            return connection ? connection->GetPlayerId() : UINT32_MAX;
        }

        Game::FishingHook* HookById(int32_t id) {
            if (!g_clientMobManager) return nullptr;
            const ClientMob* entry = g_clientMobManager->GetMob(id);
            if (!entry || !entry->mob || entry->mob->GetType() != Game::EntityTypeId::FishingBobber) return nullptr;
            return static_cast<Game::FishingHook*>(entry->mob.get());
        }

    } // namespace

    void ApplyHookData(const Network::FishingHookDataS2CPacket& packet) {
        Game::FishingHook* hook = HookById(packet.entityId);
        if (!hook) return;
        hook->ApplySyncedData(packet.ownerId, packet.hookedId,
                              (packet.flags & Network::FishingHookDataS2CPacket::kFlagBiting) != 0,
                              (packet.flags & Network::FishingHookDataS2CPacket::kFlagOffhand) != 0 ? 1 : 0);
    }

    void OnPullEvent(int32_t hookEntityId, Game::ClientPlayer* localPlayer) {
        Game::FishingHook* hook = HookById(hookEntityId);
        if (!hook || !localPlayer) return;
        const uint32_t localId = LocalPlayerId();
        if (localId == UINT32_MAX || hook->GetHookedNetId() != static_cast<int32_t>(localId)) return;
        // The hooked copy of the player only moves on the client whose player
        // it is — and only while that player stands in the hook's level.
        if (ClientLevels::BoundDimension() != ClientLevels::ActiveDimension()) return;

        // MC pullEntity: owner.position - hook.position, scaled 0.1, added to
        // the hooked entity's motion.
        glm::dvec3 ownerPos(0.0);
        const int32_t ownerId = hook->GetOwnerNetId();
        if (ownerId < 0) return;
        if (static_cast<uint32_t>(ownerId) == localId) {
            ownerPos = localPlayer->physics.position;
        } else {
            if (!g_remotePlayerManager) return;
            const auto& players = g_remotePlayerManager->GetPlayers();
            const auto it = players.find(static_cast<uint32_t>(ownerId));
            if (it == players.end() || !it->second.positionInitialized) return;
            ownerPos = it->second.position;
        }
        const glm::dvec3 delta = (ownerPos - hook->position) * 0.1;
        // Blocks per TICK (MC) into PlayerPhysics' blocks per SECOND — the
        // same conversion the explosion knockback makes.
        constexpr double kTicksPerSecond = 20.0;
        // Horizontal via pushVelocity (MC friction), not the portal momentum.
        const glm::vec3 pull(delta * kTicksPerSecond);
        localPlayer->physics.velocity.y += pull.y;
        localPlayer->physics.pushVelocity.x += pull.x;
        localPlayer->physics.pushVelocity.z += pull.z;
    }

    Game::FishingHook* LocalPlayerHook() {
        if (!ClientLevels::HasSession()) return nullptr;
        ClientMobManager* mobs = ClientLevels::Active().Mobs();
        if (!mobs) return nullptr;
        const uint32_t localId = LocalPlayerId();
        if (localId == UINT32_MAX) return nullptr;
        return Game::FishingHook::FindForOwner(/*clientSide=*/true, static_cast<int32_t>(localId), &mobs->Level());
    }

    int LocalCastHand() {
        if (!LocalPlayerHook()) return -1;
        const Game::ClientPlayer* player = g_clientBlockAccess ? g_clientBlockAccess->GetLocalPlayer() : nullptr;
        if (!player) return -1;
        const Game::ItemStack& main = player->inventory.GetSelectedStack();
        if (!main.IsEmpty() && main.itemId == Game::Items::FishingRod) return 0;
        const Game::ItemStack& off = player->inventory.GetSlot(Game::Inventory::OFFHAND_BEGIN);
        if (!off.IsEmpty() && off.itemId == Game::Items::FishingRod) return 1;
        return -1;
    }

    bool IsCastStack(const Game::ItemStack& stack) {
        if (stack.IsEmpty() || stack.itemId != Game::Items::FishingRod) return false;
        const int hand = LocalCastHand();
        if (hand < 0) return false;
        const Game::ClientPlayer* player = g_clientBlockAccess ? g_clientBlockAccess->GetLocalPlayer() : nullptr;
        if (!player) return false;
        const Game::ItemStack& held = hand == 0 ? player->inventory.GetSelectedStack()
                                                : player->inventory.GetSlot(Game::Inventory::OFFHAND_BEGIN);
        return &held == &stack;
    }

} // namespace Client::Fishing
