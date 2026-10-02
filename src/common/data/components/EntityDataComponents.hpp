// File: src/common/data/components/EntityDataComponents.hpp
//
// MC 26.3 item components (entity data): entity_data, sulfur_cube_content and
// the entity variant components (villager/variant … cat/collar, sheep/color,
// shulker/color). The older ones this group also owns — bucket_entity_data,
// axolotl/variant, salmon/size, tropical_fish/*, painting/variant — are
// declared in DataComponents.hpp.
// Wire ids 290-339 (DataComponents.hpp's id table). The NBT codecs live in
// server/world/storage/anvil/components/EntityDataNbt.cpp, the tooltip
// providers register with ComponentTooltips.
//
// The spawn path (MC EntityType.createDefaultStackConfig): whatever makes an
// entity from an item — a spawn egg, a mob bucket, an armor stand, a
// painting / item frame, a boat or minecart, a dispenser doing any of those —
// runs ApplyDefaultStackConfig on the new entity after its finalizeSpawn:
// Entity.applyComponentsFromItemStack (the custom name and the type's
// implicit components: its variant, collar, colour, sound variant …), then
// the stack's ENTITY_DATA merged over the entity's saved form
// (EntityType.updateCustomEntityTag → TypedEntityData.loadInto).
#pragma once

#include "../DataComponents.hpp"
#include "../NbtCompoundValue.hpp"
#include "common/entity/EntityType.hpp"

#include <string>
#include <string_view>
#include <unordered_map>

namespace Game {

    class Mob;

    // MC TypedEntityData<EntityType<?>> (world/item/component/
    // TypedEntityData.java): the entity type ("id" in the saved compound)
    // and the rest of the compound, id stripped.
    struct TypedEntityData {
        EntityTypeId     type{};
        NbtCompoundValue tag;
    };

    // MC SulfurCubeContent: the block item a bucketed sulfur cube swallowed
    // (an ItemStackTemplate — one item, its components).
    struct SulfurCubeContent {
        ItemStack absorbed;
    };

    namespace EntityVariantComponents {

        // How a variant component's value is written (its MC codec):
        //   Holder — a registry holder, the id string ("minecraft:ashen");
        //            the value is kept as that namespaced id.
        //   Enum   — a StringRepresentable enum by name ("snow"); the value
        //            is the enum's id (MC's `id`, which is also the wire's).
        //   Dye    — DyeColor by name ("light_blue"); the value is its id.
        enum class Kind : uint8_t { Holder, Enum, Dye };

        struct Info {
            const DataComponentTypeBase* type = nullptr;
            Kind                         kind = Kind::Enum;
            // Holder: the registry's entries (paths). Enum: the names indexed
            // by id (empty entries are ids that do not exist). Dye: unused.
            const char* const*           names = nullptr;
            int                          count = 0;
        };

        // Every variant component of this group that has an int or holder
        // value (not the ones declared in DataComponents.hpp).
        const std::vector<Info>& All();
        const Info* Find(const DataComponentTypeBase& type);

        // Enum/Dye: the id for a name (with or without "minecraft:"), -1 when
        // unknown. Holder: the namespaced id for a known entry, "" when
        // unknown.
        int         EnumId(const Info& info, std::string_view name);
        std::string HolderId(const Info& info, std::string_view name);
        // Enum/Dye: the name of an id ("" when out of range).
        std::string_view EnumName(const Info& info, int id);

        // DyeColor names by id (white 0 … black 15).
        std::string_view DyeName(int id);
        int DyeFromName(std::string_view name);

    } // namespace EntityVariantComponents

    // MC Entity.applyComponentsFromItemStack(stack): CUSTOM_NAME and the
    // entity type's implicit components (Wolf: variant, sound variant,
    // collar; Cat: variant, sound variant, collar; Sheep: colour; …).
    void ApplyComponentsFromItemStack(Mob& entity, const ItemStack& stack);

    // MC EntityType.createDefaultStackConfig(level, stack, user) applied to
    // `entity`: the above, then ENTITY_DATA through updateCustomEntityTag —
    // only when its type is the entity's, and for the op-only types
    // (falling block, command block / spawner minecart) only when a player
    // made it (`userIsPlayer`; the engine has no operator levels).
    void ApplyDefaultStackConfig(Mob& entity, const ItemStack& stack, bool userIsPlayer);

    // MC SpawnEggItem.getType(stack): the stack's ENTITY_DATA type (every
    // egg carries one by default, Items.java), Count when it has none.
    EntityTypeId SpawnEggType(const ItemStack& stack);

    // MC TypedEntityData.loadInto(entity): the entity's saved form, the
    // data merged over it (CompoundTag.merge), loaded back; the UUID kept.
    // Server-side (EntityDataNbt.cpp — the entity NBT codec lives there).
    // False when the entity's saved form cannot be produced.
    bool LoadEntityDataInto(Mob& entity, const TypedEntityData& data);

    // Items.java's ENTITY_DATA defaults: every spawn egg's
    // TypedEntityData.of(type) (kSpawnEggTable). Called by
    // ItemRegistry::Initialize.
    void ItemRegistry_RegisterEntityDataDefaults(std::unordered_map<ItemID, Item>& pureItems);

    // BUCKET_ENTITY_DATA's compound form (CustomData): the typed keys and
    // `extra`, merged (FromNbt / ToNbt). Server-side (EntityDataNbt.cpp).
    BucketEntityData BucketEntityDataFromNbt(const ::World::NBTTagCompound& tag);
    std::shared_ptr<::World::NBTTagCompound> BucketEntityDataToNbt(const BucketEntityData& data);

} // namespace Game

namespace Game::DataComponents {

    // MC ENTITY_DATA (290).
    extern const DataComponentType<TypedEntityData> ENTITY_DATA;

    // Registry-holder variants (the value is the namespaced id).
    extern const DataComponentType<std::string> VILLAGER_VARIANT;        // 291 "villager/variant"
    extern const DataComponentType<std::string> WOLF_VARIANT;            // 292 "wolf/variant"
    extern const DataComponentType<std::string> WOLF_SOUND_VARIANT;      // 293 "wolf/sound_variant"
    extern const DataComponentType<std::string> PIG_VARIANT;             // 294 "pig/variant"
    extern const DataComponentType<std::string> PIG_SOUND_VARIANT;       // 295 "pig/sound_variant"
    extern const DataComponentType<std::string> COW_VARIANT;             // 296 "cow/variant"
    extern const DataComponentType<std::string> COW_SOUND_VARIANT;       // 297 "cow/sound_variant"
    extern const DataComponentType<std::string> CHICKEN_VARIANT;         // 298 "chicken/variant"
    extern const DataComponentType<std::string> CHICKEN_SOUND_VARIANT;   // 299 "chicken/sound_variant"
    extern const DataComponentType<std::string> ZOMBIE_NAUTILUS_VARIANT; // 300 "zombie_nautilus/variant"
    extern const DataComponentType<std::string> FROG_VARIANT;            // 301 "frog/variant"
    extern const DataComponentType<std::string> CAT_VARIANT;             // 302 "cat/variant"
    extern const DataComponentType<std::string> CAT_SOUND_VARIANT;       // 303 "cat/sound_variant"

    // Enum variants (the value is the enum's id).
    extern const DataComponentType<int32_t> FOX_VARIANT;                 // 304 "fox/variant"
    extern const DataComponentType<int32_t> PARROT_VARIANT;              // 305 "parrot/variant"
    extern const DataComponentType<int32_t> MOOSHROOM_VARIANT;           // 306 "mooshroom/variant"
    extern const DataComponentType<int32_t> RABBIT_VARIANT;              // 307 "rabbit/variant" (EVIL = 99)
    extern const DataComponentType<int32_t> HORSE_VARIANT;               // 308 "horse/variant"
    extern const DataComponentType<int32_t> LLAMA_VARIANT;               // 309 "llama/variant"

    // DyeColor components (the value is the colour's id).
    extern const DataComponentType<int32_t> WOLF_COLLAR;                 // 310 "wolf/collar"
    extern const DataComponentType<int32_t> CAT_COLLAR;                  // 312 "cat/collar"
    extern const DataComponentType<int32_t> SHEEP_COLOR;                 // 313 "sheep/color"
    extern const DataComponentType<int32_t> SHULKER_COLOR;               // 314 "shulker/color"

    // MC SULFUR_CUBE_CONTENT (311) — what a bucket of sulfur cube's cube had
    // swallowed; the rest of its bucket state rides BUCKET_ENTITY_DATA
    // ("age", "age_locked").
    extern const DataComponentType<SulfurCubeContent> SULFUR_CUBE_CONTENT;

} // namespace Game::DataComponents
