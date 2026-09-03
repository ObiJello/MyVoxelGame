// File: src/server/world/storage/anvil/BlockEntityNbt.cpp
#include "server/world/storage/anvil/BlockEntityNbt.hpp"

#include "server/world/storage/anvil/ItemStackNbt.hpp"

#include "common/core/Log.hpp"
#include "common/world/block/entity/BaseContainerBlockEntity.hpp"
#include "common/world/block/entity/BlockEntityTypes.hpp"
#include "common/world/block/entity/CampfireBlockEntity.hpp"
#include "common/world/block/entity/EndGatewayBlockEntity.hpp"
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

        // The entity's position is a WORLD position (World::SetBlock creates
        // it with one, and the renderers read it as one). The chunk-local
        // key of the map it lives in is not what is written; adding the
        // chunk's origin to a world position, as this once did, filed every
        // chest two chunks away and the loader dropped them all.
        const glm::ivec3 world = entity.GetWorldPos();
        (void)chunkPos;

        w.ListCompoundBegin(list);
        w.String("id", "minecraft:" + std::string(type->StringId()));
        w.Int("x", world.x);
        w.Int("y", world.y);
        w.Int("z", world.z);
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

        if (const auto* gateway = dynamic_cast<const EndGatewayBlockEntity*>(&entity)) {
            // MC TheEndGatewayBlockEntity.saveAdditional: Age, the cached
            // exit (BlockPos codec = int array), ExactTeleport only when set.
            // The teleport cooldown is runtime-only in vanilla too.
            w.Long("Age", gateway->Age());
            if (gateway->HasExitPosition()) {
                const int32_t exit[3] = { gateway->ExitPosition().x,
                                          gateway->ExitPosition().y,
                                          gateway->ExitPosition().z };
                w.IntArray("exit_portal", exit, 3);
            }
            if (gateway->ExactTeleport()) w.Bool("ExactTeleport", true);
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

        int worldX = tag.GetValue<int32_t>("x", 0);
        const int worldY = tag.GetValue<int32_t>("y", 0);
        int worldZ = tag.GetValue<int32_t>("z", 0);

        // Worlds saved before the writer above was fixed carry the chunk's
        // origin twice in x and z. That is a recognisable signature: taking
        // the origin back off lands the entity in this chunk exactly when
        // the record is one of those. Repaired in place; the next save
        // writes it right.
        if ((worldX >> 4) != chunkPos.x || (worldZ >> 4) != chunkPos.z) {
            const int fixedX = worldX - chunkPos.x * Math::CHUNK_SIZE_X;
            const int fixedZ = worldZ - chunkPos.z * Math::CHUNK_SIZE_Z;
            if ((fixedX >> 4) == chunkPos.x && (fixedZ >> 4) == chunkPos.z) {
                Log::Info("[Anvil] block entity at (%d,%d,%d) repaired to (%d,%d,%d) in chunk (%d,%d)",
                          worldX, worldY, worldZ, fixedX, worldY, fixedZ, chunkPos.x, chunkPos.z);
                worldX = fixedX;
                worldZ = fixedZ;
            }
        }

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
        // The entity itself carries the WORLD position, like one created in
        // play; the local triple is only the chunk map's key.
        const glm::ivec3 worldPos(worldX, worldY, worldZ);

        // The type's own factory decides the concrete class and the slot count.
        auto entity = type->Create(worldPos, blockAt);
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

        if (auto* gateway = dynamic_cast<EndGatewayBlockEntity*>(entity.get())) {
            gateway->SetAge(tag.GetValue<int64_t>("Age", 0));
            if (auto exit = std::dynamic_pointer_cast<::World::NBTTagIntArray>(
                    tag.GetTag("exit_portal"));
                exit && exit->value.size() == 3) {
                gateway->SetExitPosition(
                    glm::ivec3(exit->value[0], exit->value[1], exit->value[2]),
                    tag.GetValue<int8_t>("ExactTeleport", 0) != 0);
            }
        }

        // SetItem marks the container changed, so a freshly loaded chest would
        // otherwise queue itself for an immediate rewrite.
        entity->ClearDirty();
        return entity;
    }

} // namespace Game::Anvil
