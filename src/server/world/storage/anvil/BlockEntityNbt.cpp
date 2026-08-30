// File: src/server/world/storage/anvil/BlockEntityNbt.cpp
#include "server/world/storage/anvil/BlockEntityNbt.hpp"

#include "server/world/storage/anvil/ItemStackNbt.hpp"

#include "common/core/Log.hpp"
#include "common/world/block/entity/BaseContainerBlockEntity.hpp"
#include "common/world/block/entity/BlockEntityTypes.hpp"
#include "common/world/block/entity/CampfireBlockEntity.hpp"
#include "common/world/block/entity/FurnaceBlockEntity.hpp"

#include <string>

namespace Game::Anvil {

    namespace {

        // An ender chest's contents belong to the PLAYER in vanilla
        // (Data.Player.EnderItems), not to the block. Writing an Items list
        // under minecraft:ender_chest produces NBT Minecraft discards, so the
        // slots are deliberately not written — see the note in BlockEntityTypes.
        bool CarriesItems(const BlockEntityType& type) {
            return type.StringId() != std::string("ender_chest");
        }

        void WriteContainerItems(Nbt::Writer& w, const BaseContainerBlockEntity& container) {
            auto items = w.BeginList("Items", Nbt::TagType::Compound);
            const int size = container.GetContainerSize();
            for (int slot = 0; slot < size; ++slot) {
                const ItemStack& stack = container.GetItem(slot);
                if (stack.IsEmpty()) continue;      // vanilla omits empty slots
                w.ListCompoundBegin(items);
                WriteItemStackBody(w, stack, slot);
                w.ListCompoundEnd(items);
            }
            w.EndList(items);
        }

        void ReadContainerItems(const ::World::NBTTagCompound& tag,
                                BaseContainerBlockEntity& container) {
            auto items = std::dynamic_pointer_cast<::World::NBTTagList>(tag.GetTag("Items"));
            if (!items) return;
            for (const auto& element : items->value) {
                auto entry = std::dynamic_pointer_cast<::World::NBTTagCompound>(element);
                if (!entry) continue;
                const int slot = entry->GetValue<int8_t>("Slot", -1);
                if (slot < 0 || slot >= container.GetContainerSize()) continue;
                container.SetItem(slot, ReadItemStack(*entry));
            }
        }

    } // namespace

    bool WriteBlockEntity(Nbt::Writer& w, Nbt::Writer::ListScope& list,
                          const BlockEntity& entity, Math::ChunkPos chunkPos) {
        const BlockEntityType* type = entity.GetType();
        if (!type || type->StringId().empty()) return false;

        const glm::ivec3 local = entity.GetWorldPos();

        w.ListCompoundBegin(list);
        w.String("id", "minecraft:" + std::string(type->StringId()));
        // x/z are chunk-local in the map key; y is already a world coordinate.
        w.Int("x", chunkPos.x * Math::CHUNK_SIZE_X + local.x);
        w.Int("y", local.y);
        w.Int("z", chunkPos.z * Math::CHUNK_SIZE_Z + local.z);
        // MC writes this alongside the metadata; false means "instantiate on
        // load" rather than "keep the tag verbatim".
        w.Bool("keepPacked", false);

        if (const auto* container = dynamic_cast<const BaseContainerBlockEntity*>(&entity)) {
            if (CarriesItems(*type)) WriteContainerItems(w, *container);
        }

        if (const auto* furnace = dynamic_cast<const FurnaceBlockEntity*>(&entity)) {
            // 1.21+ names. The pre-1.21 BurnTime/CookTime/CookTimeTotal keys
            // are what a DataFixer would rename to these, so writing the
            // modern names at a modern DataVersion is the correct pairing.
            w.Short("lit_time_remaining",  static_cast<int16_t>(furnace->LitTime()));
            w.Short("lit_total_time",      static_cast<int16_t>(furnace->LitDuration()));
            w.Short("cooking_time_spent",  static_cast<int16_t>(furnace->CookingTime()));
            w.Short("cooking_total_time",  static_cast<int16_t>(furnace->CookingTotal()));
            w.BeginCompound("RecipesUsed"); w.EndCompound();
        }

        if (const auto* campfire = dynamic_cast<const CampfireBlockEntity*>(&entity)) {
            int32_t progress[CampfireBlockEntity::SLOT_COUNT];
            int32_t total[CampfireBlockEntity::SLOT_COUNT];
            for (int i = 0; i < CampfireBlockEntity::SLOT_COUNT; ++i) {
                progress[i] = campfire->CookingProgress(i);
                total[i]    = campfire->CookingTime(i);
            }
            w.IntArray("CookingTimes",      progress, CampfireBlockEntity::SLOT_COUNT);
            w.IntArray("CookingTotalTimes", total,    CampfireBlockEntity::SLOT_COUNT);
        }

        w.ListCompoundEnd(list);
        return true;
    }

    std::unique_ptr<BlockEntity> ReadBlockEntity(const ::World::NBTTagCompound& tag,
                                                 Math::ChunkPos chunkPos,
                                                 BlockID blockAt,
                                                 glm::ivec3& outLocal) {
        std::string id = tag.GetValue<std::string>("id");
        if (id.rfind("minecraft:", 0) == 0) id = id.substr(10);

        const BlockEntityType* type = BlockEntityTypes::ByStringId(id);
        if (!type) return nullptr;                  // a block entity this build lacks

        const int worldX = tag.GetValue<int32_t>("x", 0);
        const int worldY = tag.GetValue<int32_t>("y", 0);
        const int worldZ = tag.GetValue<int32_t>("z", 0);

        // Must belong to THIS chunk. Vanilla warns and clamps; dropping is
        // safer here, because a mis-filed block entity would otherwise attach
        // itself to whatever block happens to sit at the wrapped coordinate.
        if ((worldX >> 4) != chunkPos.x || (worldZ >> 4) != chunkPos.z) {
            Log::Warning("[Anvil] block entity at (%d,%d,%d) is not in chunk (%d,%d) — dropping it",
                         worldX, worldY, worldZ, chunkPos.x, chunkPos.z);
            return nullptr;
        }

        // The block entity must match the block under it. This is vanilla's
        // own check, and it is the reason chest / trapped_chest / ender_chest
        // had to become three types: written as one id, two of the three would
        // fail exactly here and be dropped with their contents.
        if (!type->IsValidFor(blockAt)) {
            Log::Warning("[Anvil] %s at (%d,%d,%d) sits on a block it does not belong to — dropping it",
                         id.c_str(), worldX, worldY, worldZ);
            return nullptr;
        }

        outLocal = glm::ivec3(worldX & 15, worldY, worldZ & 15);

        // The type's own factory decides the concrete class and the slot count.
        auto entity = type->Create(outLocal, blockAt);
        if (!entity) return nullptr;

        if (auto* container = dynamic_cast<BaseContainerBlockEntity*>(entity.get())) {
            if (CarriesItems(*type)) ReadContainerItems(tag, *container);
        }

        if (auto* furnace = dynamic_cast<FurnaceBlockEntity*>(entity.get())) {
            furnace->SetLitTime     (tag.GetValue<int16_t>("lit_time_remaining", 0));
            furnace->SetLitDuration (tag.GetValue<int16_t>("lit_total_time",     0));
            furnace->SetCookingTime (tag.GetValue<int16_t>("cooking_time_spent", 0));
            furnace->SetCookingTotal(tag.GetValue<int16_t>("cooking_total_time", 0));
        }

        if (auto* campfire = dynamic_cast<CampfireBlockEntity*>(entity.get())) {
            auto progress = std::dynamic_pointer_cast<::World::NBTTagIntArray>(tag.GetTag("CookingTimes"));
            auto total    = std::dynamic_pointer_cast<::World::NBTTagIntArray>(tag.GetTag("CookingTotalTimes"));
            for (int i = 0; i < CampfireBlockEntity::SLOT_COUNT; ++i) {
                if (progress && i < static_cast<int>(progress->value.size())) {
                    campfire->SetCookingProgress(i, progress->value[i]);
                }
                if (total && i < static_cast<int>(total->value.size())) {
                    campfire->SetCookingTime(i, total->value[i]);
                }
            }
        }

        // SetItem marks the container changed, so a freshly loaded chest would
        // otherwise queue itself for an immediate rewrite.
        entity->ClearDirty();
        return entity;
    }

} // namespace Game::Anvil
