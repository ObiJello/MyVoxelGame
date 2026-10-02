// File: src/common/data/components/EntityDataComponents.cpp
#include "EntityDataComponents.hpp"

#include "common/core/Log.hpp"
#include "common/entity/Mob.hpp"
#include "common/entity/SpawnEggs.hpp"
#include "common/entity/mobs/Animals.hpp"
#include "common/entity/mobs/Fish.hpp"
#include "common/entity/mobs/AnimatedMobs.hpp"
#include "common/network/ItemStackSerialization.hpp"

#include <stdexcept>

namespace Game {

    namespace EntityVariantComponents {

        namespace {
            // Registry entries (paths) of the holder-valued variants — the
            // data/minecraft/<registry>/ folders (and MC's bootstraps for the
            // sound variants, which ship no JSON here).
            constexpr const char* kVillagerTypes[]     = {"desert", "jungle", "plains", "savanna", "snow", "swamp", "taiga"};
            constexpr const char* kWolfVariants[]      = {"pale", "spotted", "snowy", "black", "ashen", "rusty", "woods", "chestnut", "striped"};
            constexpr const char* kWolfSounds[]        = {"classic", "puglin", "sad", "angry", "grumpy", "big", "cute"};
            constexpr const char* kTemperature[]       = {"temperate", "warm", "cold"};
            constexpr const char* kPigSounds[]         = {"classic", "mini", "big"};
            constexpr const char* kCowSounds[]         = {"classic", "moody"};
            constexpr const char* kChickenSounds[]     = {"classic", "picky"};
            constexpr const char* kNautilusVariants[]  = {"temperate", "warm"};
            constexpr const char* kCatVariants[]       = {"tabby", "black", "red", "siamese", "british_shorthair", "calico",
                                                          "persian", "ragdoll", "white", "jellie", "all_black"};
            constexpr const char* kCatSounds[]         = {"classic", "royal"};
            // Enum-valued variants, by MC id.
            constexpr const char* kFoxVariants[]       = {"red", "snow"};
            constexpr const char* kParrotVariants[]    = {"red_blue", "blue", "green", "yellow_blue", "gray"};
            constexpr const char* kMooshroomVariants[] = {"red", "brown"};
            constexpr const char* kHorseVariants[]     = {"white", "creamy", "chestnut", "brown", "black", "gray", "dark_brown"};
            constexpr const char* kLlamaVariants[]     = {"creamy", "white", "brown", "gray"};
            constexpr const char* kDyeNames[16]        = {"white", "orange", "magenta", "light_blue", "yellow", "lime", "pink", "gray",
                                                          "light_gray", "cyan", "purple", "blue", "brown", "green", "red", "black"};

            std::string_view Bare(std::string_view s) {
                return s.rfind("minecraft:", 0) == 0 ? s.substr(10) : s;
            }

            // Rabbit.Variant ids: BROWN 0 … SALT 5, EVIL 99.
            constexpr const char* kRabbitNames[] = {"brown", "white", "black", "white_splotched", "gold", "salt"};
        }

        const std::vector<Info>& All() {
            static const std::vector<Info> all = [] {
                using namespace DataComponents;
                std::vector<Info> v;
                const auto holder = [&v](const DataComponentTypeBase& t, const char* const* names, int n) {
                    v.push_back(Info{&t, Kind::Holder, names, n});
                };
                const auto enumeration = [&v](const DataComponentTypeBase& t, const char* const* names, int n) {
                    v.push_back(Info{&t, Kind::Enum, names, n});
                };
                holder(VILLAGER_VARIANT, kVillagerTypes, 7);
                holder(WOLF_VARIANT, kWolfVariants, 9);
                holder(WOLF_SOUND_VARIANT, kWolfSounds, 7);
                holder(PIG_VARIANT, kTemperature, 3);
                holder(PIG_SOUND_VARIANT, kPigSounds, 3);
                holder(COW_VARIANT, kTemperature, 3);
                holder(COW_SOUND_VARIANT, kCowSounds, 2);
                holder(CHICKEN_VARIANT, kTemperature, 3);
                holder(CHICKEN_SOUND_VARIANT, kChickenSounds, 2);
                holder(ZOMBIE_NAUTILUS_VARIANT, kNautilusVariants, 2);
                holder(FROG_VARIANT, kTemperature, 3);
                holder(CAT_VARIANT, kCatVariants, 11);
                holder(CAT_SOUND_VARIANT, kCatSounds, 2);
                enumeration(FOX_VARIANT, kFoxVariants, 2);
                enumeration(PARROT_VARIANT, kParrotVariants, 5);
                enumeration(MOOSHROOM_VARIANT, kMooshroomVariants, 2);
                enumeration(RABBIT_VARIANT, kRabbitNames, 6);
                enumeration(HORSE_VARIANT, kHorseVariants, 7);
                enumeration(LLAMA_VARIANT, kLlamaVariants, 4);
                v.push_back(Info{&WOLF_COLLAR, Kind::Dye, nullptr, 0});
                v.push_back(Info{&CAT_COLLAR, Kind::Dye, nullptr, 0});
                v.push_back(Info{&SHEEP_COLOR, Kind::Dye, nullptr, 0});
                v.push_back(Info{&SHULKER_COLOR, Kind::Dye, nullptr, 0});
                return v;
            }();
            return all;
        }

        const Info* Find(const DataComponentTypeBase& type) {
            for (const Info& i : All()) if (i.type == &type) return &i;
            return nullptr;
        }

        int EnumId(const Info& info, std::string_view name) {
            name = Bare(name);
            if (info.kind == Kind::Dye) return DyeFromName(name);
            if (info.type == &DataComponents::RABBIT_VARIANT && name == "evil") return 99;
            for (int i = 0; i < info.count; ++i) if (info.names[i] && name == info.names[i]) return i;
            return -1;
        }

        std::string HolderId(const Info& info, std::string_view name) {
            const std::string_view bare = Bare(name);
            // Only "minecraft:" holders exist; a foreign namespace is unknown.
            if (name.find(':') != std::string_view::npos && name.rfind("minecraft:", 0) != 0) return {};
            for (int i = 0; i < info.count; ++i) {
                if (bare == info.names[i]) return "minecraft:" + std::string(bare);
            }
            return {};
        }

        std::string_view EnumName(const Info& info, int id) {
            if (info.kind == Kind::Dye) return DyeName(id);
            if (info.type == &DataComponents::RABBIT_VARIANT && id == 99) return "evil";
            if (id < 0 || id >= info.count || !info.names[id]) return {};
            return info.names[id];
        }

        std::string_view DyeName(int id) {
            return (id >= 0 && id < 16) ? kDyeNames[id] : std::string_view{};
        }

        int DyeFromName(std::string_view name) {
            name = Bare(name);
            for (int i = 0; i < 16; ++i) if (name == kDyeNames[i]) return i;
            return -1;
        }

    } // namespace EntityVariantComponents

    namespace {

        // The saved-entity key an implicit component lands on (MC
        // applyImplicitComponent → the setter whose value addAdditionalSave-
        // Data writes under this key), for the entity types that have it.
        struct VariantKey {
            const DataComponentTypeBase* type;
            EntityTypeId                 entity;
            const char*                  key;
        };

        const std::vector<VariantKey>& VariantKeys() {
            using namespace DataComponents;
            static const std::vector<VariantKey> keys = {
                {&WOLF_VARIANT,            EntityTypeId::Wolf,           "variant"},
                {&WOLF_SOUND_VARIANT,      EntityTypeId::Wolf,           "sound_variant"},
                {&WOLF_COLLAR,             EntityTypeId::Wolf,           "CollarColor"},
                {&CAT_VARIANT,             EntityTypeId::Cat,            "variant"},
                {&CAT_SOUND_VARIANT,       EntityTypeId::Cat,            "sound_variant"},
                {&CAT_COLLAR,              EntityTypeId::Cat,            "CollarColor"},
                {&PIG_VARIANT,             EntityTypeId::Pig,            "variant"},
                {&PIG_SOUND_VARIANT,       EntityTypeId::Pig,            "sound_variant"},
                {&COW_VARIANT,             EntityTypeId::Cow,            "variant"},
                {&COW_SOUND_VARIANT,       EntityTypeId::Cow,            "sound_variant"},
                {&CHICKEN_VARIANT,         EntityTypeId::Chicken,        "variant"},
                {&CHICKEN_SOUND_VARIANT,   EntityTypeId::Chicken,        "sound_variant"},
                {&FROG_VARIANT,            EntityTypeId::Frog,           "variant"},
                {&ZOMBIE_NAUTILUS_VARIANT, EntityTypeId::ZombieNautilus, "variant"},
                {&FOX_VARIANT,             EntityTypeId::Fox,            "Type"},
                {&MOOSHROOM_VARIANT,       EntityTypeId::Mooshroom,      "Type"},
                {&PARROT_VARIANT,          EntityTypeId::Parrot,         "Variant"},
                {&RABBIT_VARIANT,          EntityTypeId::Rabbit,         "RabbitType"},
                {&LLAMA_VARIANT,           EntityTypeId::Llama,          "Variant"},
                {&LLAMA_VARIANT,           EntityTypeId::TraderLlama,    "Variant"},
                {&SHEEP_COLOR,             EntityTypeId::Sheep,          "Color"},
                {&SHULKER_COLOR,           EntityTypeId::Shulker,        "Color"},
            };
            return keys;
        }

        // The variant a stack's component holds, as the saved entity writes
        // it: holders and the fox / mooshroom enums as their name string,
        // the int enums as ints, colours as bytes.
        ::World::NBTTagPtr SavedForm(const ItemStack& stack, const VariantKey& k) {
            using namespace EntityVariantComponents;
            const Info* info = Find(*k.type);
            if (!info) return nullptr;
            if (info->kind == Kind::Holder) {
                const auto* type = static_cast<const DataComponentType<std::string>*>(k.type);
                const auto v = stack.get(*type);
                if (!v || v->empty()) return nullptr;
                return std::make_shared<::World::NBTTagString>(*v);
            }
            const auto* type = static_cast<const DataComponentType<int32_t>*>(k.type);
            const auto v = stack.get(*type);
            if (!v) return nullptr;
            if (info->kind == Kind::Dye) return std::make_shared<::World::NBTTagByte>(static_cast<int8_t>(*v & 15));
            if (k.type == &DataComponents::FOX_VARIANT || k.type == &DataComponents::MOOSHROOM_VARIANT) {
                const std::string_view name = EnumName(*info, *v);
                if (name.empty()) return nullptr;
                return std::make_shared<::World::NBTTagString>(std::string(name));
            }
            return std::make_shared<::World::NBTTagInt>(*v);
        }

    } // namespace

    void ApplyComponentsFromItemStack(Mob& entity, const ItemStack& stack) {
        if (stack.IsEmpty()) return;
        // applyImplicitComponentIfPresent(components, CUSTOM_NAME).
        if (auto name = stack.get(DataComponents::CUSTOM_NAME)) entity.SetCustomName(*name);

        const EntityTypeId type = entity.GetType();
        // Fish (salmon size, the tropical fish pattern and colours) and the
        // axolotl keep their own setters.
        if (auto* fish = dynamic_cast<Fish*>(&entity)) fish->ApplyImplicitComponents(stack);
        if (auto* axolotl = dynamic_cast<Axolotl*>(&entity)) {
            if (auto v = stack.get(DataComponents::AXOLOTL_VARIANT); v && *v >= 0 && *v < Axolotl::kVariantCount) {
                axolotl->SetVariant(static_cast<Axolotl::Variant>(*v));
            }
        }
        // Horse.applyImplicitComponent(HORSE_VARIANT): the coat only; the
        // markings stay.
        if (auto* horse = dynamic_cast<Horse*>(&entity)) {
            if (auto v = stack.get(DataComponents::HORSE_VARIANT); v && *v >= 0 && *v < 7) horse->SetVariant(*v);
        }

        // The rest through the entity's own save format: the saved form with
        // each present component's key replaced, loaded back (every one of
        // these setters is what the matching load key calls).
        auto patch = std::make_shared<::World::NBTTagCompound>();
        for (const VariantKey& k : VariantKeys()) {
            if (k.entity != type) continue;
            if (auto tag = SavedForm(stack, k)) patch->value[k.key] = std::move(tag);
        }
        // Villager.applyImplicitComponent(VILLAGER_VARIANT): the type inside
        // VillagerData.
        if (type == EntityTypeId::Villager) {
            if (auto v = stack.get(DataComponents::VILLAGER_VARIANT); v && !v->empty()) {
                auto data = std::make_shared<::World::NBTTagCompound>();
                data->value["type"] = std::make_shared<::World::NBTTagString>(*v);
                patch->value["VillagerData"] = std::move(data);
            }
        }
        if (patch->value.empty()) return;
        TypedEntityData data;
        data.type = type;
        data.tag = NbtCompoundValue(std::move(patch));
        if (!LoadEntityDataInto(entity, data)) {
            Log::Warning("[EntityData] could not apply the item's variant components to entity type %d",
                         static_cast<int>(type));
        }
    }

    void ApplyDefaultStackConfig(Mob& entity, const ItemStack& stack, bool userIsPlayer) {
        ApplyComponentsFromItemStack(entity, stack);
        // EntityType.updateCustomEntityTag: ENTITY_DATA whose type is the
        // entity's (a spawn egg's own default carries nothing else). The
        // op-only types (onlyOpCanSetNbt: falling block, command block
        // minecart, spawner minecart) need a player — never mobs here.
        (void)userIsPlayer;
        const auto data = stack.get(DataComponents::ENTITY_DATA);
        if (!data || data->type != entity.GetType() || data->tag.IsEmpty()) return;
        LoadEntityDataInto(entity, *data);
    }

    EntityTypeId SpawnEggType(const ItemStack& stack) {
        if (auto data = stack.get(DataComponents::ENTITY_DATA)) return data->type;
        return EntityTypeId::Count;
    }

    void ItemRegistry_RegisterEntityDataDefaults(std::unordered_map<ItemID, Item>& pureItems) {
        // Items.java: every spawn egg is registered with
        // .component(ENTITY_DATA, TypedEntityData.of(type, new CompoundTag())).
        for (const SpawnEggEntry& e : kSpawnEggTable) {
            auto it = pureItems.find(e.item);
            if (it == pureItems.end()) continue;
            it->second.defaultComponents.set(DataComponents::ENTITY_DATA, TypedEntityData{e.type, NbtCompoundValue{}});
        }
    }

} // namespace Game

namespace Game::DataComponents {

    namespace {
        void SerHolder(Network::PacketBuffer& b, const std::string& v) { b.WriteString(v); }
        std::string DeHolder(Network::PacketReader& r) { return r.ReadString(); }
        void SerId(Network::PacketBuffer& b, const int32_t& v) { b.WriteVarInt(static_cast<uint32_t>(v)); }
        int32_t DeId(Network::PacketReader& r) { return static_cast<int32_t>(r.ReadVarInt()); }

        // TypedEntityData.STREAM_CODEC: the type, then the tag.
        void SerTyped(Network::PacketBuffer& b, const TypedEntityData& v) {
            b.WriteVarInt(static_cast<uint32_t>(v.type));
            SerNbtCompoundValue(b, v.tag);
        }
        TypedEntityData DeTyped(Network::PacketReader& r) {
            TypedEntityData v;
            const uint32_t type = r.ReadVarInt();
            if (type >= static_cast<uint32_t>(EntityTypeId::Count)) throw std::runtime_error("entity_data: bad entity type");
            v.type = static_cast<EntityTypeId>(type);
            v.tag = DeNbtCompoundValue(r);
            return v;
        }

        // SulfurCubeContent.STREAM_CODEC: an ItemStackTemplate.
        void SerSulfur(Network::PacketBuffer& b, const SulfurCubeContent& v) { Network::Serialization::WriteItemStack(b, v.absorbed); }
        SulfurCubeContent DeSulfur(Network::PacketReader& r) { return SulfurCubeContent{Network::Serialization::ReadItemStack(r)}; }
    }

    const DataComponentType<TypedEntityData> ENTITY_DATA{"entity_data", 290, &SerTyped, &DeTyped};

    const DataComponentType<std::string> VILLAGER_VARIANT       {"villager/variant",        291, &SerHolder, &DeHolder};
    const DataComponentType<std::string> WOLF_VARIANT           {"wolf/variant",            292, &SerHolder, &DeHolder};
    const DataComponentType<std::string> WOLF_SOUND_VARIANT     {"wolf/sound_variant",      293, &SerHolder, &DeHolder};
    const DataComponentType<std::string> PIG_VARIANT            {"pig/variant",             294, &SerHolder, &DeHolder};
    const DataComponentType<std::string> PIG_SOUND_VARIANT      {"pig/sound_variant",       295, &SerHolder, &DeHolder};
    const DataComponentType<std::string> COW_VARIANT            {"cow/variant",             296, &SerHolder, &DeHolder};
    const DataComponentType<std::string> COW_SOUND_VARIANT      {"cow/sound_variant",       297, &SerHolder, &DeHolder};
    const DataComponentType<std::string> CHICKEN_VARIANT        {"chicken/variant",         298, &SerHolder, &DeHolder};
    const DataComponentType<std::string> CHICKEN_SOUND_VARIANT  {"chicken/sound_variant",   299, &SerHolder, &DeHolder};
    const DataComponentType<std::string> ZOMBIE_NAUTILUS_VARIANT{"zombie_nautilus/variant", 300, &SerHolder, &DeHolder};
    const DataComponentType<std::string> FROG_VARIANT           {"frog/variant",            301, &SerHolder, &DeHolder};
    const DataComponentType<std::string> CAT_VARIANT            {"cat/variant",             302, &SerHolder, &DeHolder};
    const DataComponentType<std::string> CAT_SOUND_VARIANT      {"cat/sound_variant",       303, &SerHolder, &DeHolder};

    const DataComponentType<int32_t> FOX_VARIANT      {"fox/variant",       304, &SerId, &DeId};
    const DataComponentType<int32_t> PARROT_VARIANT   {"parrot/variant",    305, &SerId, &DeId};
    const DataComponentType<int32_t> MOOSHROOM_VARIANT{"mooshroom/variant", 306, &SerId, &DeId};
    const DataComponentType<int32_t> RABBIT_VARIANT   {"rabbit/variant",    307, &SerId, &DeId};
    const DataComponentType<int32_t> HORSE_VARIANT    {"horse/variant",     308, &SerId, &DeId};
    const DataComponentType<int32_t> LLAMA_VARIANT    {"llama/variant",     309, &SerId, &DeId};

    const DataComponentType<int32_t> WOLF_COLLAR  {"wolf/collar",   310, &SerId, &DeId};
    const DataComponentType<int32_t> CAT_COLLAR   {"cat/collar",    312, &SerId, &DeId};
    const DataComponentType<int32_t> SHEEP_COLOR  {"sheep/color",   313, &SerId, &DeId};
    const DataComponentType<int32_t> SHULKER_COLOR{"shulker/color", 314, &SerId, &DeId};

    const DataComponentType<SulfurCubeContent> SULFUR_CUBE_CONTENT{"sulfur_cube_content", 311, &SerSulfur, &DeSulfur};

} // namespace Game::DataComponents
