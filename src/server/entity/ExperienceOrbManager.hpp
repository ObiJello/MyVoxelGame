// File: src/server/entity/ExperienceOrbManager.hpp
//
// Owns every experience orb in the world and drives it: physics, the pull
// toward nearby players, merging, pickup and despawn. The server half of MC's
// ExperienceOrb — clients simulate the same physics on read-only copies
// (Client::XpOrbManager) and treat the periodic snapshots as corrections.
//
// Deliberately a sibling of ItemEntityManager rather than a shared base:
// the two entities agree on almost nothing that matters (value-splitting vs
// stacks, count-drain pickup vs inventory insertion, group-keyed merging vs
// stack merging), and the sync bookkeeping is the only true overlap.
#pragma once

#include "common/entity/EntityIdAllocator.hpp"

#include "common/entity/ExperienceOrb.hpp"
#include "common/entity/Entity.hpp"     // kXpOrbEntityIdBase
#include "common/physics/Physics.hpp"
#include "common/world/math/WorldMath.hpp"
#include "common/core/JavaRandom.hpp"

#include <glm/glm.hpp>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace Game { class World; }

namespace Server {

    class PlayerSessionManager;

    // A player absorbed one count from an orb this tick. Reuses the item
    // pickup packet on the wire (MC ClientboundTakeItemEntityPacket serves
    // both kinds); the client tells them apart by id range.
    struct XpOrbPickupEvent {
        int32_t  orbId;
        uint32_t playerId;
    };

    class ExperienceOrbManager {
    public:
        // MC ExperienceOrb.award / awardWithDirection: split `amount` into
        // MC's denominations, try to stack each onto an existing nearby orb
        // (the 40-group clumping), and spawn what's left as fresh orbs with
        // the death-burst scatter velocity.
        void Award(const glm::dvec3& pos, int amount);

        // One server tick: physics + pull, merging, pickup, despawn.
        //
        // Same contract as ItemEntityManager::Tick — `outRemoved` gets ids
        // that despawned or merged away (broadcast as removals); orbs consumed
        // by pickup are retired client-side by their take packet instead.
        void Tick(Game::World* world, PlayerSessionManager* sessions,
                  std::vector<int32_t>& outRemoved,
                  std::vector<XpOrbPickupEvent>& outPickups);

        void RemoveInChunk(Game::Math::ChunkPos chunk, std::vector<int32_t>& outRemoved);
        void Clear();

        // Reinstate an orb restored from disk with its saved value, count and
        // age intact. SpawnOrb splits a value into vanilla-sized orbs and
        // would turn one saved orb into several.
        int32_t Adopt(Game::ExperienceOrb orb);
        // Moving between levels — same contract as ItemEntityManager's.
        std::optional<Game::ExperienceOrb> Extract(int32_t id);
        bool AdoptWithId(Game::ExperienceOrb orb);

        const std::unordered_map<int32_t, Game::ExperienceOrb>& All() const {
            return m_entities;
        }

        // Mutable view — see ItemEntityManager::AllMutable. Orbs are only
        // pushed by a blast, never destroyed by one.
        std::unordered_map<int32_t, Game::ExperienceOrb>& AllMutable() {
            return m_entities;
        }
        size_t Count() const { return m_entities.size(); }

        // Same split as ItemEntityManager::CollectSyncSets: full spawn packet
        // for new orbs and the periodic self-heal rotation, compact move-only
        // update for motion the client couldn't predict. Orbs being drained by
        // a pickup are excluded outright — the client already removed them.
        void CollectSyncSets(int64_t serverTick,
                             std::vector<int32_t>& outFullRefresh,
                             std::vector<int32_t>& outMoveOnly);

    private:
        static constexpr int kSyncIntervalTicks = 20;

        std::unordered_map<int32_t, Game::ExperienceOrb> m_entities;
        Game::JavaRandom m_random{0};
        bool m_seeded = false;

        // Player.takeXpDelay, per player: at most one orb count absorbed
        // every 2 ticks. Keyed by connection id; entries for departed players
        // are pruned as sessions disappear from the tick loop.
        std::unordered_map<uint32_t, int> m_takeDelay;

        int32_t SpawnOrb(const glm::dvec3& pos, int value);
        // The `tryMergeToExisting` half of Award. Returns true when the value
        // stacked onto an existing orb's count instead of needing an entity.
        bool TryMergeToExisting(const glm::dvec3& pos, int value);
    };

} // namespace Server
