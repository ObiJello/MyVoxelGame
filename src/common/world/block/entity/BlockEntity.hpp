// File: src/common/world/block/entity/BlockEntity.hpp
//
// Stage 1 of the BlockEntity system (see plan: full-MC-parity BERs).
//
// Mirrors MC `BlockEntity.java`. A BlockEntity is per-cell mutable state that
// the chunk mesher can't represent — chest inventory, sign text, banner
// patterns, lid open-count, beacon levels, etc. The BE is owned by its Chunk
// (one map per chunk, keyed by local position), created lazily when a
// BE-needing block is placed, destroyed when that block is replaced or broken.
//
// Stage 1 ships:
//   - this abstract base
//   - the type registry (BlockEntityType / BlockEntityTypes)
//   - per-chunk storage on Chunk
//   - SetBlock lifecycle hook to create/destroy BEs
//   - per-tick simulation pass in World::TileEntityTick
//   - a wire packet so server BE creation reaches the client
//   - one concrete subclass (ChestBlockEntity) as a smoke test
//
// Subsequent stages add renderers, network sync of per-BE state, inventory
// items in chests, sign text editing, etc.
//
// Serialisation note: per the build-stage decision, we DO NOT use the full
// MC NBT codec stack — BE state is written directly to the existing
// `Network::PacketBuffer` (binary tagged record). Anvil save/load is deferred
// to a later stage; BEs currently exist only for the lifetime of the running
// server process and don't persist across restarts. Each BE subclass overrides
// Save/Load with whatever its own state is.
#pragma once

#include "../Blocks.hpp"
#include "../BlockState.hpp"
#include <glm/glm.hpp>
#include <cstdint>
#include <memory>

namespace Network { class PacketBuffer; class PacketReader; }
namespace Game { class World; class DataComponentMap; class ILevelWrite; }

namespace Game {

    class BlockEntityType;

    class BlockEntity {
    public:
        // `worldPos` is the absolute block position (server-wide unique).
        // `blockId` is the placed block — useful for variant-aware BEs (a single
        // ChestBlockEntity class backs Chest, TrappedChest, EnderChest, …).
        BlockEntity(const BlockEntityType* type, glm::ivec3 worldPos, BlockID blockId)
            : m_type(type), m_worldPos(worldPos), m_blockId(blockId) {}
        virtual ~BlockEntity() = default;

        // Non-copyable; movable by base ref only (subclass move semantics own state).
        BlockEntity(const BlockEntity&)            = delete;
        BlockEntity& operator=(const BlockEntity&) = delete;

        const BlockEntityType* GetType() const { return m_type; }

        // MC BlockEntity.level — the level this entity sits in, installed by
        // the world when the entity joins it (World::SetBlock's creation
        // path, World::SetBlockEntity, and the tick walker for entities that
        // arrived with a chunk). Null on the client and before installation.
        ILevelWrite* GetLevel() const { return m_level; }
        void SetLevel(ILevelWrite* level) { m_level = level; }
        const glm::ivec3&      GetWorldPos() const { return m_worldPos; }
        BlockID                GetBlockId() const { return m_blockId; }

        // Dirty bit: set by mutators (subclass logic), drained by the per-tick
        // delta broadcaster after the change has been queued for network send.
        bool IsDirty() const { return m_dirty; }
        void MarkDirty()     { m_dirty = true; }
        void ClearDirty()    { m_dirty = false; }

        // Per-tick simulation hook. Default: no-op. Subclasses override for
        // chest lid auto-close, bell decay, conduit pulse, brushable erosion,
        // etc. `World*` is the SERVER world; only called on the server.
        virtual void Tick(World* /*world*/, float /*deltaTime*/) {}

        // Hint to the per-tick walker so it can skip BEs that don't need ticking
        // (vast majority — most placed BEs never tick).
        virtual bool NeedsTicking() const { return false; }

        // MC's client-side ticker (ClientLevel.tickBlockEntities). Only the
        // piston's moving cell has one; everything else animates in its
        // renderer. `level` is the client's own writable level.
        virtual void ClientTick(ILevelWrite& /*level*/) {}

        // MC BlockEntity.triggerEvent(b0, b1): a block event (ILevelWrite::
        // BlockEvent) addressed to this entity, reached through the block's
        // triggerEvent on BOTH sides — the server when it comes due, the
        // client when the server mirrors it. Returns whether it was handled
        // (the server broadcasts only handled events). The chest's lid rides
        // this: event 1 carries its opener count.
        virtual bool TriggerEvent(int /*b0*/, int /*b1*/) { return false; }

        // MC BlockEntity.preRemoveSideEffects(pos, oldState): the block is
        // being replaced by a different block and this entity is about to
        // go. Runs BEFORE removal, with the new block already in the chunk.
        // The piston's moving-block entity finishes its move here, which is
        // what makes writing over an in-flight block land the carried block
        // instead of losing it.
        virtual void PreRemoveSideEffects(ILevelWrite& /*level*/, const glm::ivec3& /*pos*/,
                                          BlockState /*oldState*/) {}

        // Apply any relevant item components from the held-item stack at the
        // moment the block was placed (sign text, banner patterns, custom name,
        // dye color, …). Default: no-op. Called by PlayerSession after a
        // successful placement creates this BE.
        virtual void ApplyItemComponents(const DataComponentMap& /*components*/) {}

        // The client replaces its copy of a block entity with a fresh one
        // built from every BlockEntityDataS2C; MC instead loads the update
        // into the EXISTING entity, so client-only state (animation clocks)
        // survives. `previous` is the entity being replaced (same type); an
        // override copies whatever of that state must carry over. Default:
        // nothing.
        virtual void CarryClientState(const BlockEntity& /*previous*/) {}

        // Binary serialisation of this BE's persistent state (NOT pos/type id —
        // those are framed by the carrier packet). Subclasses override; base
        // emits nothing. Symmetric with Load.
        virtual void Save(Network::PacketBuffer& /*out*/) const {}
        virtual void Load(Network::PacketReader& /*in*/) {}

    private:
        const BlockEntityType* m_type    = nullptr;
        glm::ivec3             m_worldPos{};
        BlockID                m_blockId = BlockID::Air;
        bool                   m_dirty   = false;
        ILevelWrite*           m_level   = nullptr;
    };

} // namespace Game
