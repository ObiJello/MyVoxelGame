// File: src/server/entity/MorphBlockAnchor.cpp
#include "MorphBlockAnchor.hpp"

#include "../IntegratedServer.hpp"
#include "../level/ServerLevel.hpp"
#include "../player/ServerPlayer.hpp"
#include "../session/PlayerSession.hpp"
#include "../session/PlayerSessionManager.hpp"
#include "common/entity/Morph.hpp"
#include "common/world/block/BlockState.hpp"
#include "common/world/level/World.hpp"
#include "common/core/Log.hpp"

#include <algorithm>

namespace Server {

    namespace {
        Game::World* WorldOf(Game::DimensionId dim) {
            ServerLevel* level = g_integratedServer ? g_integratedServer->GetLevel(dim) : nullptr;
            return level ? level->World() : nullptr;
        }
    }

    bool MorphBlockAnchor::IsLocked(uint32_t playerId) const {
        for (const Entry& e : m_entries) if (e.player == playerId) return true;
        return false;
    }

    bool MorphBlockAnchor::Lock(uint32_t playerId, const glm::ivec3& cell) {
        auto session = m_sessions.GetSession(playerId);
        if (!session || !session->GetPlayer()) return false;
        const uint32_t code = session->GetPlayer()->getMorph();
        if (!Game::Morph::IsValid(code) || Game::Morph::KindOf(code) != Game::Morph::Kind::Block) return false;
        const auto block = static_cast<Game::BlockID>(Game::Morph::BlockOf(code));
        const Game::DimensionId dim = Game::DimensionFromRaw(static_cast<int8_t>(session->GetDimensionId()));
        Game::World* world = WorldOf(dim);
        if (!world || !world->IsValidPosition(cell.x, cell.y, cell.z) ||
            !world->IsChunkLoaded(cell.x >> 4, cell.z >> 4)) return false;
        // A lock a second time re-places (the client re-snaps on every Alt).
        Unlock(playerId);
        // Only into an empty cell: a player standing in tall grass or water
        // does not become a block over it.
        if (world->GetBlock(cell.x, cell.y, cell.z) != Game::BlockID::Air) return false;
        const bool twoHigh = Game::Morph::IsDoubleBlock(code);
        const glm::ivec3 upper(cell.x, cell.y + 1, cell.z);
        if (twoHigh && world->GetBlock(upper.x, upper.y, upper.z) != Game::BlockID::Air) return false;
        if (!world->SetBlock(cell, Game::Morph::BlockStateOf(code), Game::World::UpdateFlags::All,
                             Game::World::kUpdateLimit)) return false;
        if (twoHigh) {
            world->SetBlock(upper, Game::Morph::BlockStateOf(code, nullptr, /*upper=*/true),
                            Game::World::UpdateFlags::All, Game::World::kUpdateLimit);
        }
        m_entries.push_back({playerId, dim, cell, block});
        session->GetPlayer()->setInvisible(true);
        return true;
    }

    void MorphBlockAnchor::Remove(const Entry& e, bool takeBlock) {
        if (takeBlock) {
            if (Game::World* world = WorldOf(e.dimension)) {
                // The upper half first, so removing the lower does not drop it.
                if (world->GetBlock(e.cell.x, e.cell.y + 1, e.cell.z) == e.block &&
                    Game::BlockStates::Default(e.block).HasProperty(Game::PropertyId::DOUBLE_BLOCK_HALF)) {
                    world->SetBlock(e.cell.x, e.cell.y + 1, e.cell.z, Game::BlockID::Air, Game::World::UpdateFlags::All);
                }
                if (world->GetBlock(e.cell.x, e.cell.y, e.cell.z) == e.block) {
                    world->SetBlock(e.cell.x, e.cell.y, e.cell.z, Game::BlockID::Air, Game::World::UpdateFlags::All);
                }
            }
        }
        if (auto session = m_sessions.GetSession(e.player)) {
            if (session->GetPlayer()) session->GetPlayer()->setInvisible(false);
        }
    }

    void MorphBlockAnchor::Refresh(uint32_t playerId) {
        auto it = std::find_if(m_entries.begin(), m_entries.end(),
                               [&](const Entry& e) { return e.player == playerId; });
        if (it == m_entries.end()) return;
        auto session = m_sessions.GetSession(playerId);
        if (!session || !session->GetPlayer()) return;
        Game::World* world = WorldOf(it->dimension);
        if (!world || world->GetBlock(it->cell.x, it->cell.y, it->cell.z) != it->block) return;
        const uint32_t code = session->GetPlayer()->getMorph();
        world->SetBlock(it->cell, Game::Morph::BlockStateOf(code),
                        Game::World::UpdateFlags::All, Game::World::kUpdateLimit);
        if (Game::Morph::IsDoubleBlock(code)) {
            const glm::ivec3 upper(it->cell.x, it->cell.y + 1, it->cell.z);
            world->SetBlock(upper, Game::Morph::BlockStateOf(code, nullptr, /*upper=*/true),
                            Game::World::UpdateFlags::All, Game::World::kUpdateLimit);
        }
    }

    void MorphBlockAnchor::Unlock(uint32_t playerId) {
        auto it = std::find_if(m_entries.begin(), m_entries.end(),
                               [&](const Entry& e) { return e.player == playerId; });
        if (it == m_entries.end()) return;
        const Entry e = *it;
        m_entries.erase(it);
        Remove(e, true);
    }

    void MorphBlockAnchor::Tick() {
        for (size_t i = 0; i < m_entries.size();) {
            const Entry e = m_entries[i];
            auto session = m_sessions.GetSession(e.player);
            const ServerPlayer* player = session ? session->GetPlayer() : nullptr;
            bool drop = false, takeBlock = true;
            if (!player) {
                drop = true;                                            // left: the block goes with them
            } else if (Game::DimensionFromRaw(static_cast<int8_t>(session->GetDimensionId())) != e.dimension ||
                       !Game::Morph::IsValid(player->getMorph()) ||
                       Game::Morph::KindOf(player->getMorph()) != Game::Morph::Kind::Block ||
                       static_cast<Game::BlockID>(Game::Morph::BlockOf(player->getMorph())) != e.block) {
                drop = true;                                            // no longer that block
            } else if (Game::World* world = WorldOf(e.dimension)) {
                if (world->GetBlock(e.cell.x, e.cell.y, e.cell.z) != e.block) {
                    drop = true;                                        // mined: nothing to take away
                    takeBlock = false;
                }
            }
            if (drop) {
                m_entries.erase(m_entries.begin() + static_cast<std::ptrdiff_t>(i));
                Remove(e, takeBlock);
                continue;
            }
            ++i;
        }
    }

} // namespace Server
