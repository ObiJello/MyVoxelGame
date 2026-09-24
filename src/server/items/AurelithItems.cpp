// File: src/server/items/AurelithItems.cpp
//
// The server half of the Held Note, Aurelith's reward (docs/the-hush.md,
// "Reawakening the Heart"; the bridge and its numbers in common/world/
// level/AurelithQuest.hpp). The item shell (ItemBehaviors.cpp) holds the use
// for kHeldNoteUseTicks in the goat horn's pose; this is what the hold does.
//
// The Chord, sounded: every hostile mob within kHeldNoteRadius stops where it
// stands for kHeldNoteHoldTicks (Mob::HoldByNote — the Hush stillness's
// early-out, for one mob), a boss is only slowed, and a ring of the Chord's
// light spreads from the player (the AurelithS2C Burst, the same packet the
// quest's flourishes ride). The cooldown is per player and wall-clock: the
// note works in any dimension, so no one level's game time can keep it.
#include "common/world/level/AurelithQuest.hpp"

#include "server/IntegratedServer.hpp"
#include "server/entity/ServerLevelBridge.hpp"
#include "server/level/ServerLevel.hpp"
#include "server/network/ServerConnection.hpp"
#include "server/player/ServerPlayer.hpp"
#include "server/session/PlayerSession.hpp"
#include "server/session/PlayerSessionManager.hpp"

#include "common/core/Log.hpp"
#include "common/entity/GeneratedEntityTypes.hpp"
#include "common/entity/IUsePlayer.hpp"
#include "common/entity/Mob.hpp"
#include "common/entity/MobCategory.hpp"
#include "common/entity/effect/MobEffects.hpp"
#include "common/network/PacketRegistry.hpp"
#include "common/network/packets/game/AurelithS2CPacket.hpp"
#include "common/sound/AurelithSoundCues.hpp"
#include "common/sound/SoundSource.hpp"
#include "common/world/level/World.hpp"

#include <chrono>
#include <cmath>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Game::Aurelith {

    namespace {

        using Clock = std::chrono::steady_clock;

        std::mutex& CooldownMutex() {
            static std::mutex m;
            return m;
        }
        std::unordered_map<uint32_t, Clock::time_point>& ReadyAt() {
            static std::unordered_map<uint32_t, Clock::time_point> map;
            return map;
        }

        Server::ServerPlayer* AsServerPlayer(IUsePlayer& player) {
            return dynamic_cast<Server::ServerPlayer*>(&player);
        }

        Server::ServerLevel* LevelOf(const Server::ServerPlayer& player) {
            Server::IntegratedServer* server = Server::g_integratedServer.get();
            return server ? server->GetLevel(DimensionFromRaw(player.getDimensionId())) : nullptr;
        }

        // The bosses the note cannot stop, only slow — a still boss would be
        // a free kill.
        bool IsBoss(EntityTypeId type) {
            return type == EntityTypeId::TheUnsung || type == EntityTypeId::ChoirMother ||
                   type == EntityTypeId::SilentWarden || type == EntityTypeId::Warden ||
                   type == EntityTypeId::EnderDragon || type == EntityTypeId::Wither;
        }

        // The ring of light, for every player within 64 blocks.
        void SendRing(Server::ServerLevel& level, const glm::dvec3& origin) {
            Server::IntegratedServer* server = Server::g_integratedServer.get();
            Server::PlayerSessionManager* sessions = server ? server->GetSessionManager() : nullptr;
            if (!sessions) return;
            Network::AurelithS2CPacket p;
            p.kind = Network::AurelithS2CPacket::Kind::Burst;
            p.dimension = static_cast<int8_t>(DimensionToRaw(level.Dimension()));
            p.heart = glm::ivec3(glm::floor(origin));
            p.origin = origin;
            p.style = Network::AurelithS2CPacket::BurstStyle::Resolve;
            p.colour = 0xDFFFFF;
            const std::vector<uint8_t> data = Network::Serialization::Serialize(p);
            for (const auto& session : sessions->GetAllSessions()) {
                if (!session || !session->GetConnection() ||
                    session->GetDimensionId() != static_cast<int>(level.Dimension())) continue;
                Server::ServerPlayer* other = session->GetPlayer();
                if (!other || glm::length(other->getPosition() - origin) > 64.0) continue;
                session->GetConnection()->SendPacket(static_cast<uint8_t>(Network::PacketId::AurelithS2C), data);
            }
        }

    } // namespace

    bool HeldNoteBegin(IUsePlayer& user, uint32_t hand) {
        Server::ServerPlayer* player = AsServerPlayer(user);
        if (!player) return false;
        {
            std::lock_guard<std::mutex> lock(CooldownMutex());
            auto it = ReadyAt().find(player->getPlayerId());
            if (it != ReadyAt().end() && Clock::now() < it->second) {
                const auto left = std::chrono::duration_cast<std::chrono::seconds>(it->second - Clock::now()).count() + 1;
                player->DisplayClientMessage("The note is still ringing (" + std::to_string(left) + "s)", true);
                return false;
            }
        }
        player->startUsingItem(hand);
        return true;
    }

    void HeldNoteFinish(IUsePlayer& user) {
        Server::ServerPlayer* player = AsServerPlayer(user);
        if (!player) return;
        {
            std::lock_guard<std::mutex> lock(CooldownMutex());
            auto& ready = ReadyAt()[player->getPlayerId()];
            if (Clock::now() < ready) return;
            ready = Clock::now() + std::chrono::seconds(kHeldNoteCooldownSeconds);
        }
        Server::ServerLevel* level = LevelOf(*player);
        if (!level || !level->World() || !level->MobLevel()) return;

        const glm::dvec3 at = player->getPosition() + glm::dvec3(0.0, 1.0, 0.0);
        SoundCues::Play(*level->World(), at, Sounds::kHeldNote, SoundSource::Players, 3.0f, 1.0f);
        SendRing(*level, at);

        // Every hostile mob in the Chord's reach stops (a boss is slowed).
        Server::ServerLevelBridge& bridge = *level->MobLevel();
        const int64_t until = bridge.GetGameTime() + kHeldNoteHoldTicks;
        const double r = kHeldNoteRadius;
        std::vector<Entity*> nearby;
        bridge.GetEntitiesInBox(AABB::FromMinMax(glm::vec3(at - glm::dvec3(r)), glm::vec3(at + glm::dvec3(r))),
                                nullptr, nearby);
        int held = 0;
        for (Entity* e : nearby) {
            auto* mob = dynamic_cast<Mob*>(e);
            if (!mob || mob->IsRemoved() || !mob->IsAlive()) continue;
            if (!IsMonsterCategory(GetEntityTypeInfo(mob->GetType()).category)) continue;
            const glm::dvec3 d = mob->position - at;
            if (glm::dot(d, d) > r * r) continue;
            if (IsBoss(mob->GetType())) {
                mob->AddEffect(MobEffectInstance(MobEffectId::Slowness, 80, 1), nullptr);
            } else {
                mob->HoldByNote(until);
            }
            ++held;
        }
        player->DisplayClientMessage(held > 0 ? "The Chord holds them." : "The Chord rings out.", true);
    }

} // namespace Game::Aurelith
