// File: src/server/world/storage/anvil/components/EntityDataNbt.cpp
//
// NBT codecs (MC DataComponentType.codec() over NbtOps) for the components
// of common/data/components/EntityDataComponents.hpp — entity_data,
// bucket_entity_data, sulfur_cube_content and every entity variant
// component — registered with ComponentNbt; plus the server halves of the
// entity-data helpers (TypedEntityData.loadInto, the bucket compound).
#include "server/world/storage/anvil/ComponentNbt.hpp"
#include "server/world/storage/anvil/EntityNbt.hpp"
#include "server/world/storage/anvil/ItemStackNbt.hpp"
#include "common/data/components/EntityDataComponents.hpp"
#include "common/entity/Mob.hpp"

#include <cmath>

namespace Game {

    namespace {
        // CompoundTag.merge: a compound value merges into the compound
        // already there; anything else replaces.
        void MergeCompound(::World::NBTTagCompound& into, const ::World::NBTTagCompound& from) {
            for (const auto& [key, value] : from.value) {
                if (!value) continue;
                auto existing = into.value.find(key);
                if (value->type == ::World::NBTTagType::TAG_Compound && existing != into.value.end() &&
                    existing->second && existing->second->type == ::World::NBTTagType::TAG_Compound) {
                    MergeCompound(static_cast<::World::NBTTagCompound&>(*existing->second),
                                  static_cast<const ::World::NBTTagCompound&>(*value));
                } else {
                    into.value[key] = CloneNbtTag(*value);
                }
            }
        }
    }

    bool LoadEntityDataInto(Mob& entity, const TypedEntityData& data) {
        // TypedEntityData.loadInto: entity.saveWithoutId, the data merged
        // over it, entity.load — the UUID kept.
        Nbt::Writer w;
        w.BeginRootCompound();
        if (!Anvil::WriteMobCompound(w, "entity", entity)) return false;
        w.EndRootCompound();
        if (!w.ok()) return false;
        size_t offset = 0;
        ::World::NBTTagPtr root;
        try {
            root = ::World::NBTTag::ParseTag(w.Bytes(), offset, true);
        } catch (const std::exception&) {
            return false;
        }
        auto rootCompound = std::dynamic_pointer_cast<::World::NBTTagCompound>(root);
        if (!rootCompound) return false;
        auto saved = std::dynamic_pointer_cast<::World::NBTTagCompound>(rootCompound->GetTag("entity"));
        if (!saved) return false;
        const ::World::NBTTagPtr uuid = saved->GetTag("UUID");
        MergeCompound(*saved, data.tag.Tag());
        saved->value.erase("id");
        if (uuid) saved->value["UUID"] = uuid;
        else saved->value.erase("UUID");
        Anvil::ApplyMobNbt(*saved, entity);
        return true;
    }

    BucketEntityData BucketEntityDataFromNbt(const ::World::NBTTagCompound& tag) {
        BucketEntityData data;
        auto extra = std::make_shared<::World::NBTTagCompound>();
        for (const auto& [key, value] : tag.value) {
            if (!value) continue;
            const std::optional<double> n = Anvil::ComponentNbt::NumberOf(*value);
            if      (key == "NoAI" && n)                data.noAi = *n != 0.0;
            else if (key == "Silent" && n)              data.silent = *n != 0.0;
            else if (key == "NoGravity" && n)           data.noGravity = *n != 0.0;
            else if (key == "Glowing" && n)             data.glowing = *n != 0.0;
            else if (key == "Invulnerable" && n)        data.invulnerable = *n != 0.0;
            else if (key == "PersistenceRequired" && n) data.persistenceRequired = *n != 0.0;
            else if (key == "Health" && n)              data.health = static_cast<float>(*n);
            else if (key == "Age" && n)                 data.age = static_cast<int32_t>(*n);
            else if (key == "AgeLocked" && n)           data.ageLocked = *n != 0.0;
            else if (key == "HuntingCooldown" && n)     data.huntingCooldown = static_cast<int64_t>(*n);
            else extra->value[key] = CloneNbtTag(*value);
        }
        data.extra = NbtCompoundValue(std::move(extra));
        return data;
    }

    std::shared_ptr<::World::NBTTagCompound> BucketEntityDataToNbt(const BucketEntityData& data) {
        auto tag = data.extra.Copy();
        // Bucketable's booleans are only ever written true (MC's
        // `if (…) putBoolean`).
        const auto flag = [&tag](const char* key, bool v) {
            if (v) tag->value[key] = std::make_shared<::World::NBTTagByte>(static_cast<int8_t>(1));
        };
        flag("NoAI", data.noAi);
        flag("Silent", data.silent);
        flag("NoGravity", data.noGravity);
        flag("Glowing", data.glowing);
        flag("Invulnerable", data.invulnerable);
        flag("PersistenceRequired", data.persistenceRequired);
        if (data.health) tag->value["Health"] = std::make_shared<::World::NBTTagFloat>(*data.health);
        if (data.age) tag->value["Age"] = std::make_shared<::World::NBTTagInt>(*data.age);
        if (data.ageLocked) tag->value["AgeLocked"] = std::make_shared<::World::NBTTagByte>(static_cast<int8_t>(*data.ageLocked ? 1 : 0));
        if (data.huntingCooldown) tag->value["HuntingCooldown"] = std::make_shared<::World::NBTTagLong>(*data.huntingCooldown);
        return tag;
    }

} // namespace Game

namespace Game::Anvil::ComponentNbt {

    namespace {

        // ── entity_data (TypedEntityData.codec: the compound with "id") ───
        void WriteEntityData(Nbt::Writer& w, std::string_view key, const ItemStack& stack) {
            const auto data = stack.components.get(DataComponents::ENTITY_DATA);
            if (!data) return;
            w.BeginCompound(key);
            w.String("id", EntityName(data->type));
            for (const auto& [k, v] : data->tag.Tag().value) {
                if (v && k != "id") WriteNbtTag(w, k, *v);
            }
            w.EndCompound();
        }

        bool ReadEntityData(const ::World::NBTTag& tag, ItemStack& stack, const ReadContext& ctx) {
            const auto* c = AsCompound(&tag);
            if (!c) { ctx.Fail("Not a map"); return false; }
            const std::string id = c->GetValue<std::string>("id", "");
            if (id.empty()) { ctx.Fail("No key id in MapLike"); return false; }
            TypedEntityData data;
            if (!EntityTypeFromName(id, data.type)) {
                ctx.Fail("Unknown registry key in ResourceKey[minecraft:root / minecraft:entity_type]: " + WithNamespace(id));
                return false;
            }
            auto rest = std::dynamic_pointer_cast<::World::NBTTagCompound>(CloneNbtTag(*c));
            rest->value.erase("id");
            data.tag = NbtCompoundValue(std::move(rest));
            stack.components.set(DataComponents::ENTITY_DATA, std::move(data));
            return true;
        }

        // ── bucket_entity_data (CustomData) ────────────────────────────────
        void WriteBucketEntityData(Nbt::Writer& w, std::string_view key, const ItemStack& stack) {
            const auto data = stack.components.get(DataComponents::BUCKET_ENTITY_DATA);
            if (!data) return;
            WriteNbtTag(w, key, *BucketEntityDataToNbt(*data));
        }

        bool ReadBucketEntityData(const ::World::NBTTag& tag, ItemStack& stack, const ReadContext& ctx) {
            const auto* c = AsCompound(&tag);
            if (!c) { ctx.Fail("Not a map"); return false; }
            stack.components.set(DataComponents::BUCKET_ENTITY_DATA, BucketEntityDataFromNbt(*c));
            return true;
        }

        // ── sulfur_cube_content (an ItemStackTemplate) ──────────────────────
        void WriteSulfurContent(Nbt::Writer& w, std::string_view key, const ItemStack& stack) {
            const auto content = stack.components.get(DataComponents::SULFUR_CUBE_CONTENT);
            if (!content || content->absorbed.IsEmpty()) return;
            w.BeginCompound(key);
            WriteItemStackBody(w, content->absorbed);
            w.EndCompound();
        }

        bool ReadSulfurContent(const ::World::NBTTag& tag, ItemStack& stack, const ReadContext& ctx) {
            ItemStack absorbed;
            if (const auto* c = AsCompound(&tag)) {
                absorbed = ReadItemStack(*c);
            } else if (auto id = StringOf(tag)) {
                absorbed = ItemStack(ItemFromName(*id), 1);
            }
            if (absorbed.IsEmpty()) { ctx.Fail("Item must not be minecraft:air"); return false; }
            stack.components.set(DataComponents::SULFUR_CUBE_CONTENT, SulfurCubeContent{std::move(absorbed)});
            return true;
        }

        // ── The variant components ─────────────────────────────────────────
        // Holder variants: the registry id. Enum variants: the serialized
        // name (an int id is accepted too). DyeColor: the colour's name.
        void WriteVariant(Nbt::Writer& w, std::string_view key, const ItemStack& stack, const DataComponentTypeBase& type) {
            using namespace EntityVariantComponents;
            const Info* info = EntityVariantComponents::Find(type);
            if (!info) return;
            if (info->kind == Kind::Holder) {
                const auto v = stack.components.get(static_cast<const DataComponentType<std::string>&>(type));
                if (v && !v->empty()) w.String(key, WithNamespace(*v));
                return;
            }
            const auto v = stack.components.get(static_cast<const DataComponentType<int32_t>&>(type));
            if (!v) return;
            const std::string_view name = EnumName(*info, *v);
            if (!name.empty()) w.String(key, std::string(name));
            else w.Int(key, *v);
        }

        bool ReadVariant(const ::World::NBTTag& tag, ItemStack& stack, const ReadContext& ctx,
                         const DataComponentTypeBase& type) {
            using namespace EntityVariantComponents;
            const Info* info = EntityVariantComponents::Find(type);
            if (!info) return false;
            if (info->kind == Kind::Holder) {
                const std::optional<std::string> s = StringOf(tag);
                if (!s) { ctx.Fail("Not a string"); return false; }
                const std::string id = HolderId(*info, *s);
                if (id.empty()) {
                    ctx.Fail("Unknown registry key in ResourceKey[minecraft:root / minecraft:" +
                             std::string(type.name) + "]: " + WithNamespace(*s));
                    return false;
                }
                stack.components.set(static_cast<const DataComponentType<std::string>&>(type), id);
                return true;
            }
            int id = -1;
            if (const std::optional<std::string> s = StringOf(tag)) id = EnumId(*info, *s);
            else if (auto n = NumberOf(tag)) {
                id = static_cast<int>(*n);
                if (EnumName(*info, id).empty()) id = -1;
            }
            if (id < 0) { ctx.Fail("Unknown element name"); return false; }
            stack.components.set(static_cast<const DataComponentType<int32_t>&>(type), static_cast<int32_t>(id));
            return true;
        }

        // One write/read pair per variant type (the registry takes plain
        // function pointers).
        template<auto Type>
        void WriteVariantT(Nbt::Writer& w, std::string_view key, const ItemStack& stack) { WriteVariant(w, key, stack, *Type); }
        template<auto Type>
        bool ReadVariantT(const ::World::NBTTag& tag, ItemStack& stack, const ReadContext& ctx) {
            return ReadVariant(tag, stack, ctx, *Type);
        }

#define GAME_VARIANT_CODEC(NAME) \
        const Registrar k##NAME{DataComponents::NAME, &WriteVariantT<&DataComponents::NAME>, &ReadVariantT<&DataComponents::NAME>};

        GAME_VARIANT_CODEC(VILLAGER_VARIANT)
        GAME_VARIANT_CODEC(WOLF_VARIANT)
        GAME_VARIANT_CODEC(WOLF_SOUND_VARIANT)
        GAME_VARIANT_CODEC(PIG_VARIANT)
        GAME_VARIANT_CODEC(PIG_SOUND_VARIANT)
        GAME_VARIANT_CODEC(COW_VARIANT)
        GAME_VARIANT_CODEC(COW_SOUND_VARIANT)
        GAME_VARIANT_CODEC(CHICKEN_VARIANT)
        GAME_VARIANT_CODEC(CHICKEN_SOUND_VARIANT)
        GAME_VARIANT_CODEC(ZOMBIE_NAUTILUS_VARIANT)
        GAME_VARIANT_CODEC(FROG_VARIANT)
        GAME_VARIANT_CODEC(CAT_VARIANT)
        GAME_VARIANT_CODEC(CAT_SOUND_VARIANT)
        GAME_VARIANT_CODEC(FOX_VARIANT)
        GAME_VARIANT_CODEC(PARROT_VARIANT)
        GAME_VARIANT_CODEC(MOOSHROOM_VARIANT)
        GAME_VARIANT_CODEC(RABBIT_VARIANT)
        GAME_VARIANT_CODEC(HORSE_VARIANT)
        GAME_VARIANT_CODEC(LLAMA_VARIANT)
        GAME_VARIANT_CODEC(WOLF_COLLAR)
        GAME_VARIANT_CODEC(CAT_COLLAR)
        GAME_VARIANT_CODEC(SHEEP_COLOR)
        GAME_VARIANT_CODEC(SHULKER_COLOR)
#undef GAME_VARIANT_CODEC

        const Registrar kEntityData{DataComponents::ENTITY_DATA, &WriteEntityData, &ReadEntityData};
        const Registrar kBucketEntityData{DataComponents::BUCKET_ENTITY_DATA, &WriteBucketEntityData, &ReadBucketEntityData};
        const Registrar kSulfurContent{DataComponents::SULFUR_CUBE_CONTENT, &WriteSulfurContent, &ReadSulfurContent};

    } // namespace

} // namespace Game::Anvil::ComponentNbt
