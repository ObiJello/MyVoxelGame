// File: src/server/entity/LocalMobCapCalculator.hpp
//
// MC net.minecraft.world.level.LocalMobCapCalculator — the PER-PLAYER half of
// the mob cap. The census calls AddMob for every counted mob, which charges
// that mob to EVERY player within spawn range of its chunk; a chunk may then
// spawn a category only while SOME nearby player is still under the raw
// category cap (70 monsters, 10 creatures, ...). Unlike the global cap this
// one is NOT scaled by chunk count, and it tightens within the tick as
// afterSpawn feeds new mobs back in.
//
// Built fresh each spawn tick. "Within spawn range" is MC's
// ChunkMap.playerIsCloseEnoughForSpawning: the chunk CENTER (cx*16+8) within
// 128 blocks of the player, XZ only, Y ignored.
#pragma once

#include "common/entity/MobCategory.hpp"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace Server {

    class LocalMobCapCalculator {
    public:
        // `playerPositions` must outlive the calculator (it lives for one
        // spawn tick). Entries are the non-spectator players.
        explicit LocalMobCapCalculator(const std::vector<glm::dvec3>* playerPositions)
            : m_players(playerPositions) {}

        // MC LocalMobCapCalculator.addMob.
        void AddMob(int chunkX, int chunkZ, Game::MobCategory category) {
            for (size_t player : PlayersNear(chunkX, chunkZ)) {
                ++m_counts[player][static_cast<size_t>(category)];
            }
        }

        // MC LocalMobCapCalculator.canSpawn: true iff ANY nearby player is
        // under the category's raw cap. No player near the chunk means false —
        // MC's Stream.anyMatch over an empty list.
        bool CanSpawn(Game::MobCategory category, int chunkX, int chunkZ) {
            const int cap = Game::GetMobCategoryInfo(category).maxInstancesPerChunk;
            for (size_t player : PlayersNear(chunkX, chunkZ)) {
                const auto it = m_counts.find(player);
                const int count =
                    it == m_counts.end() ? 0 : it->second[static_cast<size_t>(category)];
                if (count < cap) return true;
            }
            return false;
        }

    private:
        // MC ChunkMap.getPlayersCloseForSpawning, cached per chunk exactly as
        // MC's playersNearChunk map does — the census probes the same chunks
        // over and over.
        const std::vector<size_t>& PlayersNear(int chunkX, int chunkZ) {
            const uint64_t key =
                (static_cast<uint64_t>(static_cast<uint32_t>(chunkX)) << 32) |
                static_cast<uint32_t>(chunkZ);
            const auto it = m_playersNearChunk.find(key);
            if (it != m_playersNearChunk.end()) return it->second;

            std::vector<size_t>& nearby = m_playersNearChunk[key];
            if (m_players) {
                const double centerX = chunkX * 16.0 + 8.0;
                const double centerZ = chunkZ * 16.0 + 8.0;
                for (size_t i = 0; i < m_players->size(); ++i) {
                    const double dx = (*m_players)[i].x - centerX;
                    const double dz = (*m_players)[i].z - centerZ;
                    if (dx * dx + dz * dz < 128.0 * 128.0) nearby.push_back(i);
                }
            }
            return nearby;
        }

        const std::vector<glm::dvec3>* m_players = nullptr;
        std::unordered_map<uint64_t, std::vector<size_t>> m_playersNearChunk;
        std::unordered_map<size_t,
                           std::array<int, static_cast<size_t>(Game::MobCategory::Count)>>
            m_counts;
    };

} // namespace Server
