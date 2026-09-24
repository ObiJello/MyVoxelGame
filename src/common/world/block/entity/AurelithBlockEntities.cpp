// File: src/common/world/block/entity/AurelithBlockEntities.cpp
//
// See AurelithBlockEntities.hpp.
#include "AurelithBlockEntities.hpp"

#include "common/network/ItemStackSerialization.hpp"
#include "common/network/PacketRegistry.hpp"
#include "common/world/level/ILevelWrite.hpp"
#include "common/world/level/WorldDrops.hpp"

#include <algorithm>

namespace Game {

    // ── ResonanceEngineBlockEntity ───────────────────────────────────────

    void ResonanceEngineBlockEntity::Save(Network::PacketBuffer& out) const {
        out.WriteByte(static_cast<uint8_t>(static_cast<int8_t>(m_rotation)));
    }

    void ResonanceEngineBlockEntity::Load(Network::PacketReader& in) {
        if (!in.HasMore()) return;
        SetRotation(static_cast<int8_t>(in.ReadByte()));
    }

    // ── ChordSocketBlockEntity ───────────────────────────────────────────

    void ChordSocketBlockEntity::Changed() {
        MarkDirty();
        if (ILevelWrite* level = GetLevel()) level->BlockEntityChanged(GetWorldPos());
    }

    void ChordSocketBlockEntity::Seat(ItemStack key, int64_t now) {
        key.count = std::min(key.count, 1);
        m_key = std::move(key);
        m_seatedAt = now;
        Changed();
    }

    ItemStack ChordSocketBlockEntity::TakeKey() {
        ItemStack out = std::move(m_key);
        m_key = ItemStack{};
        m_seatedAt = 0;
        Changed();
        return out;
    }

    void ChordSocketBlockEntity::SetLocked(bool locked) {
        if (m_locked == locked) return;
        m_locked = locked;
        Changed();
    }

    void ChordSocketBlockEntity::LoadFromNbt(ItemStack key, int64_t seatedAt, bool locked) {
        m_key = std::move(key);
        m_seatedAt = seatedAt;
        m_locked = locked;
    }

    void ChordSocketBlockEntity::PreRemoveSideEffects(ILevelWrite& level, const glm::ivec3& pos,
                                                      BlockState /*oldState*/) {
        if (level.IsClientSide() || m_key.IsEmpty()) return;
        SpawnItemEntity(level.GetDimension(), glm::dvec3(pos.x + 0.5, pos.y + 1.0, pos.z + 0.5),
                        glm::dvec3(0.0, 0.15, 0.0), m_key, 10);
        m_key = ItemStack{};
    }

    void ChordSocketBlockEntity::Save(Network::PacketBuffer& out) const {
        Network::Serialization::WriteItemStack(out, m_key);
        out.WriteLong(static_cast<uint64_t>(m_seatedAt));
        out.WriteByte(m_locked ? 1 : 0);
    }

    void ChordSocketBlockEntity::Load(Network::PacketReader& in) {
        if (!in.HasMore()) return;
        m_key = Network::Serialization::ReadItemStack(in);
        if (in.HasMore()) m_seatedAt = static_cast<int64_t>(in.ReadLong());
        if (in.HasMore()) m_locked = in.ReadByte() != 0;
    }

    // ── VoicePedestalBlockEntity ─────────────────────────────────────────

    void VoicePedestalBlockEntity::Changed() {
        MarkDirty();
        if (ILevelWrite* level = GetLevel()) level->BlockEntityChanged(GetWorldPos());
    }

    void VoicePedestalBlockEntity::SetItem(ItemStack item) {
        item.count = std::min(item.count, 1);
        m_item = std::move(item);
        Changed();
    }

    ItemStack VoicePedestalBlockEntity::TakeItem() {
        ItemStack out = std::move(m_item);
        m_item = ItemStack{};
        Changed();
        return out;
    }

    void VoicePedestalBlockEntity::PreRemoveSideEffects(ILevelWrite& level, const glm::ivec3& pos,
                                                        BlockState /*oldState*/) {
        if (level.IsClientSide() || m_item.IsEmpty()) return;
        // Up and a little off the top face, as the item was sitting there.
        SpawnItemEntity(level.GetDimension(), glm::dvec3(pos.x + 0.5, pos.y + 1.05, pos.z + 0.5),
                        glm::dvec3(0.0, 0.15, 0.0), m_item, 10);
        m_item = ItemStack{};
    }

    void VoicePedestalBlockEntity::Save(Network::PacketBuffer& out) const {
        Network::Serialization::WriteItemStack(out, m_item);
    }

    void VoicePedestalBlockEntity::Load(Network::PacketReader& in) {
        if (!in.HasMore()) return;
        m_item = Network::Serialization::ReadItemStack(in);
    }

    // ── ChoirCabinetBlockEntity ──────────────────────────────────────────

    std::vector<ItemStack> ChoirCabinetBlockEntity::Open() {
        m_solved = true;
        m_progress = static_cast<int>(m_melody.size());
        std::vector<ItemStack> out = std::move(m_items);
        m_items.clear();
        MarkDirty();
        if (ILevelWrite* level = GetLevel()) level->BlockEntityChanged(GetWorldPos());
        return out;
    }

    void ChoirCabinetBlockEntity::LoadFromNbt(std::vector<ItemStack> items, std::vector<std::string> melody,
                                              int progress, bool solved) {
        m_items = std::move(items);
        m_melody = std::move(melody);
        m_progress = std::clamp(progress, 0, static_cast<int>(m_melody.size()));
        m_solved = solved;
    }

    void ChoirCabinetBlockEntity::PreRemoveSideEffects(ILevelWrite& level, const glm::ivec3& pos,
                                                       BlockState /*oldState*/) {
        if (level.IsClientSide()) return;
        for (const ItemStack& stack : m_items) {
            if (!stack.IsEmpty()) DropItemStackNear(level.GetDimension(), pos, stack);
        }
        m_items.clear();
    }

    void ChoirCabinetBlockEntity::Save(Network::PacketBuffer& out) const {
        // The client needs only whether it has opened (its doors are the
        // block's `open` state; the song itself stays server-side).
        out.WriteByte(m_solved ? 1 : 0);
    }

    void ChoirCabinetBlockEntity::Load(Network::PacketReader& in) {
        if (!in.HasMore()) return;
        m_solved = in.ReadByte() != 0;
    }

} // namespace Game
