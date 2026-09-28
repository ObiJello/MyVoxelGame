// File: src/common/world/block/entity/BrushableBlockEntity.cpp
#include "BrushableBlockEntity.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/core/Log.hpp"
#include "common/entity/IUsePlayer.hpp"
#include "common/network/ItemStackSerialization.hpp"
#include "common/network/PacketRegistry.hpp"
#include "common/sound/LevelEventSounds.hpp"
#include "common/sound/LevelSound.hpp"
#include "common/world/block/BlockState.hpp"
#include "common/world/level/DimensionId.hpp"
#include "common/world/level/ILevelWrite.hpp"
#include "common/world/level/World.hpp"
#include "common/world/level/WorldDrops.hpp"
#include "common/world/loot/ChestLootTables.hpp"
#include "common/world/ticks/ScheduledTickAccess.hpp"

#include <algorithm>
#include <chrono>
#include <vector>

namespace Game {

    namespace {

        // MC BrushableBlock.getTurnsInto: Blocks.SAND / Blocks.GRAVEL (the
        // two suspicious blocks' constructor arguments), AIR for anything
        // else carrying this entity.
        BlockID TurnsInto(BlockID block) {
            switch (block) {
                case BlockID::SuspiciousSand:   return BlockID::Sand;
                case BlockID::SuspiciousGravel: return BlockID::Gravel;
                default:                        return BlockID::Air;
            }
        }

        // Direction.relative(1) for a 3D data value (down, up, north, south,
        // west, east).
        glm::ivec3 Step(int direction) {
            switch (direction) {
                case 0: return { 0, -1,  0 };
                case 1: return { 0,  1,  0 };
                case 2: return { 0,  0, -1 };
                case 3: return { 0,  0,  1 };
                case 4: return { -1, 0,  0 };
                case 5: return { 1,  0,  0 };
                default: return { 0, 1, 0 };
            }
        }

    } // namespace

    void BrushableBlockEntity::SetChanged() {
        MarkDirty();
        if (ILevelWrite* level = GetLevel()) level->BlockEntityChanged(GetWorldPos());
    }

    int BrushableBlockEntity::GetCompletionState() const {
        if (m_brushCount == 0) return 0;
        if (m_brushCount < 3) return 1;
        return m_brushCount < 6 ? 2 : 3;
    }

    void BrushableBlockEntity::SetDusted(ILevelWrite& level, int completionState) {
        const glm::ivec3 pos = GetWorldPos();
        const BlockState state = level.GetBlockState(pos.x, pos.y, pos.z);
        if (state.Block() != GetBlockId()) return;
        level.SetBlock(pos.x, pos.y, pos.z, state.SetIndex(PropertyId::DUSTED, completionState),
                       World::UpdateFlags::All);
    }

    bool BrushableBlockEntity::Brush(ILevelWrite& level, IUsePlayer* user, int direction) {
        (void)user;
        if (level.IsClientSide()) return false;
        // A generated entity may not have been handed its level yet by the
        // tick walker; the stroke is what needs it (setChanged's broadcast).
        if (!GetLevel()) SetLevel(&level);
        if (m_hitDirection < 0) m_hitDirection = direction;

        const int64_t gameTime = level.GameTime();
        m_brushCountResetsAtTick = gameTime + kBrushResetTicks;
        if (gameTime < m_coolDownEndsAtTick) return false;

        m_coolDownEndsAtTick = gameTime + kBrushCooldownTicks;
        UnpackLootTable(level);
        const int previousCompletionState = GetCompletionState();
        if (++m_brushCount >= kRequiredBrushesToBreak) {
            BrushingCompleted(level);
            return true;
        }

        if (ScheduledTickAccess* ticks = level.Ticks()) ticks->ScheduleTick(GetWorldPos(), GetBlockId(), 2);
        const int completionState = GetCompletionState();
        if (previousCompletionState != completionState) SetDusted(level, completionState);
        // The first stroke settles hit_direction (and rolls the item): the
        // renderer needs both.
        SetChanged();
        return false;
    }

    void BrushableBlockEntity::UnpackLootTable(ILevelWrite& level) {
        if (m_lootTable.empty()) return;
        // LootParams(ARCHAEOLOGY): ORIGIN = Vec3.atCenterOf(pos); the table's
        // random is the seed's own (LootTable.getRandomItems(params, seed)),
        // the level's when the seed is 0.
        ChestLoot::LootLevelContext context;
        context.dimensionId = DimensionToRaw(level.GetDimension());
        context.origin = glm::dvec3(GetWorldPos()) + glm::dvec3(0.5);

        JavaRandom seeded(m_lootTableSeed);
        JavaRandom fallback(static_cast<int64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count()));
        JavaRandom* levelRandom = level.Random();
        JavaRandom& random = m_lootTableSeed != 0 ? seeded : (levelRandom ? *levelRandom : fallback);

        std::vector<ItemStack> loot;
        if (!ChestLoot::GetRandomItems(m_lootTable, random, 0.0f, loot, &context)) {
            Log::Warning("[BrushableBlock] (%d,%d,%d) named loot table '%s', which does not exist",
                         GetWorldPos().x, GetWorldPos().y, GetWorldPos().z, m_lootTable.c_str());
        }
        if (loot.size() > 1) {
            Log::Warning("[BrushableBlock] expected max 1 loot from loot table %s, but got %zu",
                         m_lootTable.c_str(), loot.size());
        }
        m_item = loot.empty() ? ItemStack{} : loot.front();
        m_lootTable.clear();
        m_lootTableSeed = 0;
        SetChanged();
    }

    void BrushableBlockEntity::BrushingCompleted(ILevelWrite& level) {
        DropContent(level);
        const glm::ivec3 pos = GetWorldPos();
        const BlockState state = level.GetBlockState(pos.x, pos.y, pos.z);
        // level.levelEvent(3008, pos, Block.getId(state)): the dust burst and
        // the block's brush-complete sound.
        level.PlayLevelEvent(nullptr, LevelEvent::PARTICLES_AND_SOUND_BRUSH_BLOCK_COMPLETE, pos,
                             static_cast<int>(state.RawId()));
        level.SetBlock(pos.x, pos.y, pos.z, BlockStates::Default(TurnsInto(state.Block())),
                       World::UpdateFlags::All);
    }

    void BrushableBlockEntity::DropContent(ILevelWrite& level) {
        UnpackLootTable(level);
        if (m_item.IsEmpty()) return;
        // EntityTypes.ITEM is 0.25 x 0.25: centred in the cell one step out
        // of the brushed face, half the item's height above its middle.
        constexpr double kSize = 0.25;
        const double centerRange = 1.0 - kSize;
        const double halfSize = kSize / 2.0;
        const glm::ivec3 dropPos = GetWorldPos() + Step(m_hitDirection < 0 ? 1 : m_hitDirection);
        const glm::dvec3 at(dropPos.x + 0.5 * centerRange + halfSize,
                            dropPos.y + 0.5 + kSize / 2.0,
                            dropPos.z + 0.5 * centerRange + halfSize);
        // this.item.split(level.getRandom().nextInt(21) + 10).
        int splitCount = 10;
        if (JavaRandom* random = level.Random()) splitCount = random->NextInt(21) + 10;
        ItemStack drop = m_item;
        drop.count = std::min(drop.count, splitCount);
        // new ItemEntity(...) then setDeltaMovement(Vec3.ZERO): no scatter,
        // no pickup delay.
        SpawnItemEntity(level.GetDimension(), at, glm::dvec3(0.0), drop, 0);
        m_item = ItemStack{};
    }

    void BrushableBlockEntity::CheckReset(ILevelWrite& level) {
        if (level.IsClientSide()) return;
        if (!GetLevel()) SetLevel(&level);
        const int64_t gameTime = level.GameTime();
        if (m_brushCount != 0 && gameTime >= m_brushCountResetsAtTick) {
            const int previousCompletionState = GetCompletionState();
            m_brushCount = std::max(0, m_brushCount - 2);
            const int completionState = GetCompletionState();
            if (previousCompletionState != completionState) SetDusted(level, completionState);
            m_brushCountResetsAtTick = gameTime + 4;
        }

        if (m_brushCount == 0) {
            if (m_hitDirection >= 0) {
                m_hitDirection = -1;
                SetChanged();
            }
            m_brushCountResetsAtTick = 0;
            m_coolDownEndsAtTick = 0;
        } else if (ScheduledTickAccess* ticks = level.Ticks()) {
            ticks->ScheduleTick(GetWorldPos(), GetBlockId(), 2);
        }
    }

    void BrushableBlockEntity::LoadFromNbt(std::string lootTable, int64_t seed, ItemStack item,
                                           int hitDirection) {
        m_lootTable = std::move(lootTable);
        m_lootTableSeed = m_lootTable.empty() ? 0 : seed;
        m_item = m_lootTable.empty() ? std::move(item) : ItemStack{};
        m_hitDirection = (hitDirection >= 0 && hitDirection < 6) ? hitDirection : -1;
    }

    void BrushableBlockEntity::Save(Network::PacketBuffer& out) const {
        // MC getUpdateTag: hit_direction (when set) and the item (when rolled).
        out.WriteVarInt(static_cast<uint32_t>(m_hitDirection + 1));
        Network::Serialization::WriteItemStack(out, m_item);
    }

    void BrushableBlockEntity::Load(Network::PacketReader& in) {
        if (!in.HasMore()) return;
        const int direction = static_cast<int>(in.ReadVarInt()) - 1;
        m_hitDirection = (direction >= 0 && direction < 6) ? direction : -1;
        m_item = in.HasMore() ? Network::Serialization::ReadItemStack(in) : ItemStack{};
    }

} // namespace Game
