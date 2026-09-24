// File: src/common/world/block/entity/AurelithBlockEntities.hpp
//
// The block entities of Aurelith's quest (docs/the-hush.md, "Reawakening the
// Heart"; common/world/level/AurelithQuest.hpp):
//
//   ResonanceEngineBlockEntity — the Heart's core. Its rings are still the
//       renderer's pure function of game time; what it stores is how the
//       city was turned: the start piece's rotation ordinal (0..3, -1 for a
//       city generated before the field existed), written by the template
//       engine at generation (TemplateEngine::blockEntityPayloadFor) and read
//       by the server's AurelithCities to find every landmark from the Heart.
//   ChordSocketBlockEntity — one of the Conductor's Podium's four sockets:
//       the voice key seated in it, the game tick it was seated at (the
//       order the four were sung in is the order of those ticks), and
//       whether the Chord has locked it (the city awoke: the keys stay).
//   VoicePedestalBlockEntity — a pedestal holding one item for show (the
//       city's hidden voice keys wait on them); anything a player sets down.
//   ChoirCabinetBlockEntity — the Hall of Instruments' tuned cabinet: what
//       it holds, the song that opens it (a list of lumen colours — the
//       plinths under the chimes before it: "amber", "violet", "cyan",
//       "stave"), how much of it has been sung, and whether it has opened.
//
// Wire (Save/Load, BlockEntityDataS2C): what the client draws — the rotation,
// the socket's key and lock, the pedestal's item, the cabinet's solved flag.
// Disk: server/world/storage/anvil/BlockEntityNbt.cpp (Rotation; Item,
// SeatedAt, Locked; Item; Items, Melody, Progress, Solved).
#pragma once

#include "BlockEntity.hpp"
#include "common/entity/Item.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace Game {

    class ResonanceEngineBlockEntity : public BlockEntity {
    public:
        ResonanceEngineBlockEntity(const BlockEntityType* type, glm::ivec3 worldPos, BlockID blockId)
            : BlockEntity(type, worldPos, blockId) {}

        // The template rotation ordinal (MC Rotation: NONE, CLOCKWISE_90,
        // CLOCKWISE_180, COUNTERCLOCKWISE_90), or -1 when unknown.
        int  Rotation() const { return m_rotation; }
        void SetRotation(int rotation) { m_rotation = rotation < 0 ? -1 : (rotation & 3); }

        void Save(Network::PacketBuffer& out) const override;
        void Load(Network::PacketReader& in) override;

    private:
        int m_rotation = -1;
    };

    class ChordSocketBlockEntity : public BlockEntity {
    public:
        ChordSocketBlockEntity(const BlockEntityType* type, glm::ivec3 worldPos, BlockID blockId)
            : BlockEntity(type, worldPos, blockId) {}

        const ItemStack& GetKey() const { return m_key; }
        bool    HasKey() const { return !m_key.IsEmpty(); }
        int64_t SeatedAt() const { return m_seatedAt; }
        bool    IsLocked() const { return m_locked; }

        // Seat `key` (one item) at game tick `now`. Marks the entity changed.
        void Seat(ItemStack key, int64_t now);
        // Take the key out (EMPTY when there was none). Marks it changed.
        ItemStack TakeKey();
        void SetLocked(bool locked);

        // Disk load (no level yet, nothing to notify).
        void LoadFromNbt(ItemStack key, int64_t seatedAt, bool locked);

        // A socket broken with a key in it gives the key back (up off its
        // cradle), locked or not — the Chord cannot hold what is not there.
        void PreRemoveSideEffects(ILevelWrite& level, const glm::ivec3& pos, BlockState oldState) override;

        void Save(Network::PacketBuffer& out) const override;
        void Load(Network::PacketReader& in) override;

    private:
        void Changed();

        ItemStack m_key{};
        int64_t   m_seatedAt = 0;
        bool      m_locked = false;
    };

    class VoicePedestalBlockEntity : public BlockEntity {
    public:
        VoicePedestalBlockEntity(const BlockEntityType* type, glm::ivec3 worldPos, BlockID blockId)
            : BlockEntity(type, worldPos, blockId) {}

        const ItemStack& GetItem() const { return m_item; }
        bool HasItem() const { return !m_item.IsEmpty(); }
        void SetItem(ItemStack item);          // marks changed
        ItemStack TakeItem();                  // marks changed
        void LoadFromNbt(ItemStack item) { m_item = std::move(item); }

        // A pedestal broken with something on it drops it (MC's lectern /
        // jukebox rule).
        void PreRemoveSideEffects(ILevelWrite& level, const glm::ivec3& pos, BlockState oldState) override;

        void Save(Network::PacketBuffer& out) const override;
        void Load(Network::PacketReader& in) override;

    private:
        void Changed();
        ItemStack m_item{};
    };

    class ChoirCabinetBlockEntity : public BlockEntity {
    public:
        ChoirCabinetBlockEntity(const BlockEntityType* type, glm::ivec3 worldPos, BlockID blockId)
            : BlockEntity(type, worldPos, blockId) {}

        const std::vector<ItemStack>&   Items() const { return m_items; }
        const std::vector<std::string>& Melody() const { return m_melody; }
        int     Progress() const { return m_progress; }
        bool    IsSolved() const { return m_solved; }
        int64_t LastNoteTick() const { return m_lastNoteTick; }

        void SetItems(std::vector<ItemStack> items) { m_items = std::move(items); }
        void SetMelody(std::vector<std::string> melody) { m_melody = std::move(melody); }
        // The song so far (server): Advance on a right note, Reset on a
        // wrong one or a pause too long.
        void Advance(int64_t now) { ++m_progress; m_lastNoteTick = now; MarkDirty(); }
        void Reset() { m_progress = 0; m_lastNoteTick = 0; MarkDirty(); }
        // Solved: the contents are handed out (the caller spawns them).
        std::vector<ItemStack> Open();
        void LoadFromNbt(std::vector<ItemStack> items, std::vector<std::string> melody,
                         int progress, bool solved);

        // Broken open instead of sung open: the contents spill.
        void PreRemoveSideEffects(ILevelWrite& level, const glm::ivec3& pos, BlockState oldState) override;

        void Save(Network::PacketBuffer& out) const override;
        void Load(Network::PacketReader& in) override;

    private:
        std::vector<ItemStack>   m_items;
        std::vector<std::string> m_melody;
        int     m_progress = 0;
        int64_t m_lastNoteTick = 0;
        bool    m_solved = false;
    };

} // namespace Game
