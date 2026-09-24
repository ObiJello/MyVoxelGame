// File: src/server/world/storage/anvil/BlockEntityNbt.cpp
#include "server/world/storage/anvil/BlockEntityNbt.hpp"

#include "server/world/storage/anvil/ItemStackNbt.hpp"
#include "server/world/storage/anvil/SpawnerNbt.hpp"
#include "server/world/storage/SectionDataUnpacker.hpp"

#include "common/core/Log.hpp"
#include "common/world/block/entity/BaseContainerBlockEntity.hpp"
#include "common/world/block/entity/BlockEntityTypes.hpp"
#include "common/world/block/entity/CampfireBlockEntity.hpp"
#include "common/world/block/entity/ComparatorBlockEntity.hpp"
#include "common/world/block/entity/PistonMovingBlockEntity.hpp"
#include "common/world/block/entity/HopperBlockEntity.hpp"
#include "common/world/block/entity/LecternBlockEntity.hpp"
#include "common/world/block/entity/PotentSulfurBlockEntity.hpp"
#include "common/world/block/entity/AurelithBlockEntities.hpp"
#include "common/world/block/entity/HushLighthouseLampBlockEntity.hpp"
#include "common/world/block/entity/EndGatewayBlockEntity.hpp"
#include "common/world/block/entity/FurnaceBlockEntity.hpp"
#include "common/world/block/entity/BrewingStandBlockEntity.hpp"
#include "common/world/block/entity/SignBlockEntity.hpp"
#include "common/world/block/entity/SpawnerBlockEntity.hpp"

#include <string>
#include <unordered_map>

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
            // MC RandomizableContainerBlockEntity.saveAdditional: an unrolled
            // table is written INSTEAD of the items (trySaveLootTable), the
            // seed only when non-zero.
            if (container->HasLootTable()) {
                w.String("LootTable", container->GetLootTable());
                if (container->GetLootTableSeed() != 0) w.Long("LootTableSeed", container->GetLootTableSeed());
            } else if (CarriesItems(*type)) {
                WriteContainerItems(w, *container);
            }
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

        if (const auto* stand = dynamic_cast<const BrewingStandBlockEntity*>(&entity)) {
            // MC BrewingStandBlockEntity.saveAdditional.
            w.Int  ("BrewTime",         stand->BrewTime());
            w.Int  ("total_brew_time",  stand->TotalBrewTime());
            w.Int  ("Fuel",             stand->Fuel());
            w.Int  ("total_fuel",       stand->TotalFuel());
            w.Float("speed_multiplier", stand->SpeedMultiplier());
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

        if (const auto* sign = dynamic_cast<const SignBlockEntity*>(&entity)) {
            // MC SignBlockEntity.saveAdditional: front_text / back_text are
            // SignText's codec — `messages` (4 text components; a bare
            // string is a valid component), `color`, `has_glowing_text` —
            // and is_waxed.
            auto writeText = [&](const char* key, const SignText& text) {
                w.BeginCompound(key);
                {
                    auto lines = w.BeginList("messages", Nbt::TagType::String);
                    for (const std::string& line : text.lines) w.ListString(lines, line);
                    w.EndList(lines);
                }
                w.String("color", DyeColorName(text.color));
                w.Bool("has_glowing_text", text.glowing);
                w.EndCompound();
            };
            writeText("front_text", sign->GetText(SignTextSlot::Front));
            writeText("back_text",  sign->GetText(SignTextSlot::Back));
            w.Bool("is_waxed", sign->IsWaxed());
        }

        if (const auto* hopper = dynamic_cast<const HopperBlockEntity*>(&entity)) {
            w.Int("TransferCooldown", hopper->CooldownTime());
        }

        if (const auto* lectern = dynamic_cast<const LecternBlockEntity*>(&entity)) {
            // MC LecternBlockEntity.saveAdditional: Book (ItemStack.CODEC)
            // and Page, only while a book is on it.
            if (!lectern->GetBook().IsEmpty()) {
                w.BeginCompound("Book");
                WriteItemStackBody(w, lectern->GetBook());
                w.EndCompound();
                w.Int("Page", lectern->GetPage());
            }
        }

        if (const auto* comparator = dynamic_cast<const ComparatorBlockEntity*>(&entity)) {
            w.Int("OutputSignal", comparator->GetOutputSignal());
        }

        if (const auto* sulfur = dynamic_cast<const PotentSulfurBlockEntity*>(&entity)) {
            // MC PotentSulfurBlockEntity.saveAdditional.
            w.Int("countdown", sulfur->waitingCountdown);
        }

        // Aurelith's quest block entities (AurelithBlockEntities.hpp).
        if (const auto* engine = dynamic_cast<const ResonanceEngineBlockEntity*>(&entity)) {
            if (engine->Rotation() >= 0) w.Byte("Rotation", static_cast<int8_t>(engine->Rotation()));
        }
        if (const auto* socket = dynamic_cast<const ChordSocketBlockEntity*>(&entity)) {
            if (socket->HasKey()) {
                w.BeginCompound("Item");
                WriteItemStackBody(w, socket->GetKey());
                w.EndCompound();
                w.Long("SeatedAt", socket->SeatedAt());
            }
            if (socket->IsLocked()) w.Bool("Locked", true);
        }
        if (const auto* pedestal = dynamic_cast<const VoicePedestalBlockEntity*>(&entity)) {
            if (pedestal->HasItem()) {
                w.BeginCompound("Item");
                WriteItemStackBody(w, pedestal->GetItem());
                w.EndCompound();
            }
        }
        if (const auto* cabinet = dynamic_cast<const ChoirCabinetBlockEntity*>(&entity)) {
            auto items = w.BeginList("Items", Nbt::TagType::Compound);
            int slot = 0;
            for (const ItemStack& stack : cabinet->Items()) {
                if (!stack.IsEmpty()) {
                    w.ListCompoundBegin(items);
                    WriteItemStackBody(w, stack, slot);
                    w.ListCompoundEnd(items);
                }
                ++slot;
            }
            w.EndList(items);
            auto melody = w.BeginList("Melody", Nbt::TagType::String);
            for (const std::string& note : cabinet->Melody()) w.ListString(melody, note);
            w.EndList(melody);
            w.Int("Progress", cabinet->Progress());
            if (cabinet->IsSolved()) w.Bool("Solved", true);
        }

        if (const auto* lamp = dynamic_cast<const HushLighthouseLampBlockEntity*>(&entity)) {
            // Engine block entity (The Hush): the nearest Aurelith the lamp
            // guides toward, once the server has looked (LighthouseGuide).
            // Nothing written until then, so an unchecked lamp saves as the
            // bare {id} the template placed.
            if (lamp->GuideChecked()) {
                w.Bool("GuideChecked", true);
                if (lamp->HasGuide()) {
                    w.Int("GuideX", lamp->GuideX());
                    w.Int("GuideZ", lamp->GuideZ());
                }
            }
        }

        if (const auto* piston = dynamic_cast<const PistonMovingBlockEntity*>(&entity)) {
            piston->WriteNbt(w);
        }

        if (const auto* spawner = dynamic_cast<const SpawnerBlockEntity*>(&entity)) {
            // MC SpawnerBlockEntity.saveAdditional -> BaseSpawner.save.
            WriteSpawner(w, *spawner);
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
            // MC loadAdditional: tryLoadLootTable first; items only without one.
            // This is also how an imported Minecraft world's untouched
            // structure chests arrive — with the key, never with items.
            const std::string lootTable = tag.GetValue<std::string>("LootTable", "");
            if (!lootTable.empty()) {
                container->SetLootTable(lootTable, tag.GetValue<int64_t>("LootTableSeed", 0));
            } else if (CarriesItems(*type)) {
                ReadContainerItems(tag, *container);
            }
        }

        if (auto* hopper = dynamic_cast<HopperBlockEntity*>(entity.get())) {
            hopper->SetCooldownTime(tag.GetValue<int32_t>("TransferCooldown", -1));
        }

        if (auto* lectern = dynamic_cast<LecternBlockEntity*>(entity.get())) {
            // MC LecternBlockEntity.loadAdditional: the Book stack (EMPTY
            // when absent or unreadable), then Page clamped to the book.
            ItemStack book;
            if (auto bookTag = std::dynamic_pointer_cast<::World::NBTTagCompound>(tag.GetTag("Book"))) {
                book = ReadItemStack(*bookTag);
            }
            lectern->LoadFromNbt(std::move(book), tag.GetValue<int32_t>("Page", 0));
        }

        if (auto* comparator = dynamic_cast<ComparatorBlockEntity*>(entity.get())) {
            comparator->SetOutputSignal(tag.GetValue<int32_t>("OutputSignal", 0));
        }

        if (auto* sulfur = dynamic_cast<PotentSulfurBlockEntity*>(entity.get())) {
            // MC PotentSulfurBlockEntity.loadAdditional: keeps its value when absent.
            if (tag.GetTag("countdown")) sulfur->waitingCountdown = tag.GetValue<int32_t>("countdown", -1);
        }

        // Aurelith's quest block entities (AurelithBlockEntities.hpp).
        if (auto* engine = dynamic_cast<ResonanceEngineBlockEntity*>(entity.get())) {
            engine->SetRotation(tag.GetTag("Rotation") ? tag.GetValue<int8_t>("Rotation", -1) : -1);
        }
        auto readItem = [&](const char* key) {
            ItemStack stack;
            if (auto t = std::dynamic_pointer_cast<::World::NBTTagCompound>(tag.GetTag(key))) {
                stack = ReadItemStack(*t);
            }
            return stack;
        };
        if (auto* socket = dynamic_cast<ChordSocketBlockEntity*>(entity.get())) {
            socket->LoadFromNbt(readItem("Item"), tag.GetValue<int64_t>("SeatedAt", 0),
                                tag.GetValue<int8_t>("Locked", 0) != 0);
        }
        if (auto* pedestal = dynamic_cast<VoicePedestalBlockEntity*>(entity.get())) {
            pedestal->LoadFromNbt(readItem("Item"));
        }
        if (auto* cabinet = dynamic_cast<ChoirCabinetBlockEntity*>(entity.get())) {
            std::vector<ItemStack> items;
            if (auto list = std::dynamic_pointer_cast<::World::NBTTagList>(tag.GetTag("Items"))) {
                for (const auto& element : list->value) {
                    auto entry = std::dynamic_pointer_cast<::World::NBTTagCompound>(element);
                    if (!entry) continue;
                    ItemStack stack = ReadItemStack(*entry);
                    if (!stack.IsEmpty()) items.push_back(std::move(stack));
                }
            }
            std::vector<std::string> melody;
            if (auto list = std::dynamic_pointer_cast<::World::NBTTagList>(tag.GetTag("Melody"))) {
                for (const auto& element : list->value) {
                    if (auto str = std::dynamic_pointer_cast<::World::NBTTagString>(element)) {
                        melody.push_back(str->value);
                    }
                }
            }
            cabinet->LoadFromNbt(std::move(items), std::move(melody),
                                 tag.GetValue<int32_t>("Progress", 0),
                                 tag.GetValue<int8_t>("Solved", 0) != 0);
        }

        if (auto* lamp = dynamic_cast<HushLighthouseLampBlockEntity*>(entity.get())) {
            lamp->LoadGuide(tag.GetValue<int8_t>("GuideChecked", 0) != 0,
                            tag.GetTag("GuideX") != nullptr,
                            tag.GetValue<int32_t>("GuideX", 0), tag.GetValue<int32_t>("GuideZ", 0));
        }

        if (auto* piston = dynamic_cast<PistonMovingBlockEntity*>(entity.get())) {
            BlockState moved{};
            if (auto bs = std::dynamic_pointer_cast<::World::NBTTagCompound>(tag.GetTag("blockState"))) {
                if (auto nameTag = std::dynamic_pointer_cast<::World::NBTTagString>(bs->GetTag("Name"))) {
                    std::unordered_map<std::string, std::string> props;
                    if (auto p = std::dynamic_pointer_cast<::World::NBTTagCompound>(bs->GetTag("Properties"))) {
                        for (const auto& [k, v] : p->value) {
                            if (auto str = std::dynamic_pointer_cast<::World::NBTTagString>(v)) props[k] = str->value;
                        }
                    }
                    const NbtBlockState resolved = BlockStateRegistry::CreateBlockState(nameTag->value, props);
                    moved = BlockStates::FromIndex(resolved.resolvedId, resolved.resolvedState);
                }
            }
            const int facing = tag.GetValue<int32_t>("facing", 0);
            piston->SetFromNbt(moved, static_cast<Direction>(facing % 6),
                               tag.GetValue<float>("progress", 0.0f),
                               tag.GetValue<int8_t>("extending", 0) != 0,
                               tag.GetValue<int8_t>("source", 0) != 0);
        }

        if (auto* furnace = dynamic_cast<FurnaceBlockEntity*>(entity.get())) {
            furnace->SetLitTime     (tag.GetValue<int16_t>("lit_time_remaining", 0));
            furnace->SetLitDuration (tag.GetValue<int16_t>("lit_total_time",     0));
            furnace->SetCookingTime (tag.GetValue<int16_t>("cooking_time_spent", 0));
            furnace->SetCookingTotal(tag.GetValue<int16_t>("cooking_total_time", 0));
        }

        if (auto* stand = dynamic_cast<BrewingStandBlockEntity*>(entity.get())) {
            // MC BrewingStandBlockEntity.loadAdditional (its defaults: 0, 400,
            // 0, 20, 1.0), then the brewing ingredient is re-read from slot 3.
            stand->SetBrewTime       (tag.GetValue<int32_t>("BrewTime", 0));
            stand->SetTotalBrewTime  (tag.GetValue<int32_t>("total_brew_time", 400));
            stand->SetFuel           (tag.GetValue<int32_t>("Fuel", 0));
            stand->SetTotalFuel      (tag.GetValue<int32_t>("total_fuel", 20));
            stand->SetSpeedMultiplier(tag.GetValue<float>("speed_multiplier", 1.0f));
            stand->RestoreIngredientFromSlot();
        }

        if (auto* sign = dynamic_cast<SignBlockEntity*>(entity.get())) {
            auto readText = [&](const char* key, SignTextSlot slot) {
                auto compound = std::dynamic_pointer_cast<::World::NBTTagCompound>(tag.GetTag(key));
                if (!compound) return;
                SignText text;
                if (auto list = std::dynamic_pointer_cast<::World::NBTTagList>(compound->GetTag("messages"))) {
                    for (size_t i = 0; i < text.lines.size() && i < list->value.size(); ++i) {
                        auto str = std::dynamic_pointer_cast<::World::NBTTagString>(list->value[i]);
                        if (!str) continue;
                        std::string line = str->value;
                        // A real Minecraft file stores each line as a text
                        // component: a bare string, or a JSON object whose
                        // "text" is the line. Take the plain text either way.
                        if (!line.empty() && line.front() == '{') {
                            const size_t k = line.find("\"text\":\"");
                            if (k != std::string::npos) {
                                const size_t start = k + 8;
                                const size_t end = line.find('"', start);
                                line = end != std::string::npos ? line.substr(start, end - start) : std::string();
                            } else {
                                line.clear();
                            }
                        } else if (line.size() >= 2 && line.front() == '"' && line.back() == '"') {
                            line = line.substr(1, line.size() - 2);
                        }
                        text.lines[i] = line;
                    }
                }
                DyeColor colour = DyeColor::Black;
                DyeColorFromName(compound->GetValue<std::string>("color", "black"), colour);
                text.color   = colour;
                text.glowing = compound->GetValue<int8_t>("has_glowing_text", 0) != 0;
                sign->SetText(slot, text);
            };
            readText("front_text", SignTextSlot::Front);
            readText("back_text",  SignTextSlot::Back);
            sign->SetWaxed(tag.GetValue<int8_t>("is_waxed", 0) != 0);
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

        if (auto* spawner = dynamic_cast<SpawnerBlockEntity*>(entity.get())) {
            // MC SpawnerBlockEntity.loadAdditional -> BaseSpawner.load.
            ReadSpawner(tag, *spawner);
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
