// File: src/client/entity/ClientFallingBlocks.hpp
//
// The client's compact falling blocks: every FallingBlock entity the server
// announces lives here as a row in a struct-of-arrays, never as a Mob object.
//
// The client side of a falling block is small — MC's FallingBlockEntity
// client tick is gravity, move, drag — and it is never picked, never ridden,
// never damaged, never a portal traveller client-side. So unlike the server
// (where anvils, concrete powder and the like keep their Mob object for the
// behaviour it carries), the client can put ALL of them here. What the
// renderer needs is written out as BlockEntityProxy rows, the same struct
// ClientMobManager produces for its TNT, so BlockCubeEntityRenderer draws
// both lists the same way.
//
// The wire contract is the mob one: AddEntityS2C (type FallingBlock),
// MoveEntityS2C deltas against the codec base, EntityPositionSync,
// SetEntityMotion, RemoveEntities. ClientPacketHandler routes by
// Owns(id) / entity type; ClientMobManager never sees these ids.
#pragma once

#include "client/entity/ClientMobManager.hpp"   // BlockEntityProxy

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace Game { struct IBlockAccess; }

namespace Client {

    class ClientFallingBlocks {
    public:
        void SetBlockAccess(const Game::IBlockAccess* blocks) { m_blocks = blocks; }

        bool Owns(int32_t id) const { return m_index.count(id) != 0; }

        // Packet entry points — the same semantics ClientMobManager gives a
        // falling block, minus rotation (a block has none).
        void Spawn(int32_t id, const glm::dvec3& pos, const glm::vec3& vel, uint32_t stateRaw);
        void MoveDelta(int32_t id, bool hasPos, const glm::dvec3& delta, bool onGround);
        void Teleport(int32_t id, const glm::dvec3& pos, const glm::vec3& vel, bool onGround);
        void SetMotion(int32_t id, const glm::vec3& vel);
        void Remove(int32_t id);
        void Clear();

        // 20 Hz client tick: interpolation correction, then the physics, all
        // across the pool. Leaves the proxies for the renderer.
        void Tick();

        const std::vector<BlockEntityProxy>& Proxies() const { return m_proxies; }
        size_t Count() const { return m_id.size(); }

    private:
        void FillProxy(size_t i);

        const Game::IBlockAccess* m_blocks = nullptr;

        std::vector<int32_t>    m_id;
        std::vector<glm::dvec3> m_pos;
        std::vector<glm::dvec3> m_prevPos;     // render interpolation base
        std::vector<glm::dvec3> m_vel;
        std::vector<glm::dvec3> m_codecBase;   // last server position, for deltas
        std::vector<glm::dvec3> m_target;      // interpolation target
        std::vector<int32_t>    m_interpSteps;
        std::vector<uint32_t>   m_state;
        std::vector<uint8_t>    m_onGround;
        std::vector<BlockEntityProxy> m_proxies;   // slot-for-slot with the rows
        std::unordered_map<int32_t, size_t> m_index;
    };

    // Bound-level pointer, owned by ClientLevel (see ClientLevel.hpp).
    extern ClientFallingBlocks* g_clientFallingBlocks;

} // namespace Client
