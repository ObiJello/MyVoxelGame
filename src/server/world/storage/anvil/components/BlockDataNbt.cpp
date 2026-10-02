// File: src/server/world/storage/anvil/components/BlockDataNbt.cpp
//
// NBT codecs (MC DataComponentType.codec() over NbtOps) for the components
// of common/data/components/BlockDataComponents.hpp, registered with
// ComponentNbt; plus the server halves of the block-data helpers
// (BlockItem.updateCustomBlockEntityTag, LockCode.canUnlock).
#include "server/world/storage/anvil/ComponentNbt.hpp"
#include "server/world/storage/anvil/BlockEntityNbt.hpp"
#include "server/world/storage/anvil/ItemStackNbt.hpp"
#include "common/data/components/BlockDataComponents.hpp"
#include "common/world/block/entity/BlockEntity.hpp"
#include "common/world/block/entity/BlockEntityType.hpp"
#include "common/world/level/World.hpp"
#include "common/world/tags/DataTags.hpp"

#include <algorithm>
#include <cmath>

namespace Game {

    namespace {
        // CompoundTag.merge.
        void MergeInto(::World::NBTTagCompound& into, const ::World::NBTTagCompound& from) {
            for (const auto& [key, value] : from.value) {
                if (!value) continue;
                auto existing = into.value.find(key);
                if (value->type == ::World::NBTTagType::TAG_Compound && existing != into.value.end() &&
                    existing->second && existing->second->type == ::World::NBTTagType::TAG_Compound) {
                    MergeInto(static_cast<::World::NBTTagCompound&>(*existing->second),
                              static_cast<const ::World::NBTTagCompound&>(*value));
                } else {
                    into.value[key] = CloneNbtTag(*value);
                }
            }
        }

    }

    namespace BlockData {

        bool LockUnlocksWith(const LockCode& lock, const ItemStack& stack) {
            // LockCode.unlocksWith → ItemPredicate.test: items, count,
            // components and the partial predicates (ItemPredicateNbt.cpp).
            return Anvil::ComponentNbt::ItemPredicateMatches(lock.predicate.Tag(), stack);
        }

        std::shared_ptr<::World::NBTTagCompound> SaveBlockEntityNbt(World& world, const glm::ivec3& pos) {
            BlockEntity* be = world.GetBlockEntity(pos);
            if (!be || !be->GetType()) return nullptr;
            const Math::ChunkPos chunkPos = Math::WorldCoordinates::WorldToChunkPos(pos.x, pos.z);
            Nbt::Writer w;
            w.BeginRootCompound();
            auto list = w.BeginList("be", Nbt::TagType::Compound);
            if (!Anvil::WriteBlockEntity(w, list, *be, chunkPos)) return nullptr;
            w.EndList(list);
            w.EndRootCompound();
            if (!w.ok()) return nullptr;
            size_t offset = 0;
            ::World::NBTTagPtr rootTag;
            try {
                rootTag = ::World::NBTTag::ParseTag(w.Bytes(), offset, true);
            } catch (const std::exception&) {
                return nullptr;
            }
            auto root = std::dynamic_pointer_cast<::World::NBTTagCompound>(rootTag);
            auto saved = root ? std::dynamic_pointer_cast<::World::NBTTagList>(root->GetTag("be")) : nullptr;
            if (!saved || saved->value.empty()) return nullptr;
            return std::dynamic_pointer_cast<::World::NBTTagCompound>(saved->value.front());
        }

        bool ApplyBlockEntityData(World& world, const glm::ivec3& pos, const ItemStack& stack,
                                  bool canUseGameMasterBlocks) {
            // BlockItem.updateCustomBlockEntityTag: the stack's
            // BLOCK_ENTITY_DATA of the placed entity's own type (and, for the
            // operator-only types, only from a player who may use them),
            // merged over the entity's saved form (TypedEntityData.loadInto).
            const auto data = stack.get(DataComponents::BLOCK_ENTITY_DATA);
            if (!data) return false;
            BlockEntity* be = world.GetBlockEntity(pos);
            if (!be || !be->GetType()) return false;
            const std::string beType = be->GetType()->ResourceId();
            if (Anvil::ComponentNbt::WithNamespace(data->type) != beType) return false;
            if (OnlyOpCanSetNbt(beType) && !canUseGameMasterBlocks) return false;

            const Math::ChunkPos chunkPos = Math::WorldCoordinates::WorldToChunkPos(pos.x, pos.z);
            auto compound = SaveBlockEntityNbt(world, pos);
            if (!compound) return false;
            // The data never moves the entity or changes its type.
            auto patch = data->tag.Copy();
            for (const char* key : { "id", "x", "y", "z" }) patch->value.erase(key);
            MergeInto(*compound, *patch);
            glm::ivec3 local{};
            std::unique_ptr<BlockEntity> loaded =
                Anvil::ReadBlockEntity(*compound, chunkPos, world.GetBlock(pos.x, pos.y, pos.z), local);
            if (!loaded) return false;
            world.SetBlockEntity(pos, std::move(loaded));
            world.BlockEntityChanged(pos);
            return true;
        }

    } // namespace BlockData

} // namespace Game

namespace Game::Anvil::ComponentNbt {

    namespace {

        // ── block_entity_data ({id, …}) ────────────────────────────────────
        void WriteBlockEntityData(Nbt::Writer& w, std::string_view key, const ItemStack& stack) {
            const auto d = stack.components.get(DataComponents::BLOCK_ENTITY_DATA);
            if (!d) return;
            w.BeginCompound(key);
            w.String("id", WithNamespace(d->type));
            for (const auto& [k, v] : d->tag.Tag().value) if (v && k != "id") WriteNbtTag(w, k, *v);
            w.EndCompound();
        }

        bool ReadBlockEntityData(const ::World::NBTTag& tag, ItemStack& stack, const ReadContext& ctx) {
            const auto* c = AsCompound(&tag);
            if (!c) { ctx.Fail("Not a map"); return false; }
            const std::string id = c->GetValue<std::string>("id", "");
            if (id.empty()) { ctx.Fail("No key id in MapLike"); return false; }
            TypedBlockEntityData d;
            d.type = WithNamespace(id);
            auto rest = std::dynamic_pointer_cast<::World::NBTTagCompound>(CloneNbtTag(*c));
            rest->value.erase("id");
            d.tag = NbtCompoundValue(std::move(rest));
            stack.components.set(DataComponents::BLOCK_ENTITY_DATA, std::move(d));
            return true;
        }

        // ── block_state ({property: value}) ─────────────────────────────────
        void WriteBlockState(Nbt::Writer& w, std::string_view key, const ItemStack& stack) {
            const auto s = stack.components.get(DataComponents::BLOCK_STATE);
            if (!s) return;
            w.BeginCompound(key);
            for (const auto& [name, value] : s->properties) w.String(name, value);
            w.EndCompound();
        }

        bool ReadBlockState(const ::World::NBTTag& tag, ItemStack& stack, const ReadContext& ctx) {
            const auto* c = AsCompound(&tag);
            if (!c) { ctx.Fail("Not a map"); return false; }
            BlockItemStateProperties props;
            for (const auto& [name, value] : c->value) {
                if (!value) continue;
                std::string text;
                if (auto s = StringOf(*value)) text = *s;
                else if (auto n = NumberOf(*value)) text = std::to_string(static_cast<long long>(*n));
                else if (auto b = BoolOf(*value)) text = *b ? "true" : "false";
                else { ctx.Fail("Not a string"); return false; }
                props = props.With(name, text);
            }
            stack.components.set(DataComponents::BLOCK_STATE, std::move(props));
            return true;
        }

        // ── bees ([{entity_data, ticks_in_hive, min_ticks_in_hive}]) ────────
        void WriteBees(Nbt::Writer& w, std::string_view key, const ItemStack& stack) {
            const auto b = stack.components.get(DataComponents::BEES);
            if (!b) return;
            auto list = w.BeginList(key, Nbt::TagType::Compound);
            for (const BeehiveOccupant& bee : b->bees) {
                w.ListCompoundBegin(list);
                w.BeginCompound("entity_data");
                w.String("id", WithNamespace(bee.entityType));
                for (const auto& [k, v] : bee.entityData.Tag().value) if (v && k != "id") WriteNbtTag(w, k, *v);
                w.EndCompound();
                w.Int("ticks_in_hive", bee.ticksInHive);
                w.Int("min_ticks_in_hive", bee.minTicksInHive);
                w.ListCompoundEnd(list);
            }
            w.EndList(list);
        }

        bool ReadBees(const ::World::NBTTag& tag, ItemStack& stack, const ReadContext& ctx) {
            const auto* list = AsList(&tag);
            if (!list) { ctx.Fail("Not a list"); return false; }
            Bees bees;
            for (const auto& element : list->value) {
                const auto* c = AsCompound(Unwrap(element.get()));
                if (!c) { ctx.Fail("Not a map"); return false; }
                BeehiveOccupant bee;
                if (const auto* data = AsCompound(c->GetTag("entity_data").get())) {
                    bee.entityType = WithNamespace(data->GetValue<std::string>("id", "minecraft:bee"));
                    auto rest = std::dynamic_pointer_cast<::World::NBTTagCompound>(CloneNbtTag(*data));
                    rest->value.erase("id");
                    bee.entityData = NbtCompoundValue(std::move(rest));
                }
                bee.ticksInHive = c->GetValue<int32_t>("ticks_in_hive", 0);
                bee.minTicksInHive = c->GetValue<int32_t>("min_ticks_in_hive", 0);
                bees.bees.push_back(std::move(bee));
            }
            stack.components.set(DataComponents::BEES, std::move(bees));
            return true;
        }

        // ── lock (an ItemPredicate compound) ─────────────────────────────────
        void WriteLock(Nbt::Writer& w, std::string_view key, const ItemStack& stack) {
            if (const auto l = stack.components.get(DataComponents::LOCK)) WriteNbtTag(w, key, l->predicate.Tag());
        }

        bool ReadLock(const ::World::NBTTag& tag, ItemStack& stack, const ReadContext& ctx) {
            const auto* c = AsCompound(&tag);
            if (!c) { ctx.Fail("Not a map"); return false; }
            stack.components.set(DataComponents::LOCK,
                                 LockCode{NbtCompoundValue(std::dynamic_pointer_cast<::World::NBTTagCompound>(CloneNbtTag(*c)))});
            return true;
        }

        // ── container_loot ({loot_table, seed = 0}) ─────────────────────────
        void WriteContainerLoot(Nbt::Writer& w, std::string_view key, const ItemStack& stack) {
            const auto l = stack.components.get(DataComponents::CONTAINER_LOOT);
            if (!l) return;
            w.BeginCompound(key);
            w.String("loot_table", WithNamespace(l->lootTable));
            if (l->seed != 0) w.Long("seed", l->seed);
            w.EndCompound();
        }

        bool ReadContainerLoot(const ::World::NBTTag& tag, ItemStack& stack, const ReadContext& ctx) {
            const auto* c = AsCompound(&tag);
            if (!c) { ctx.Fail("Not a map"); return false; }
            const std::string table = c->GetValue<std::string>("loot_table", "");
            if (table.empty()) { ctx.Fail("No key loot_table in MapLike"); return false; }
            stack.components.set(DataComponents::CONTAINER_LOOT,
                                 SeededContainerLoot{WithNamespace(table), c->GetValue<int64_t>("seed", 0)});
            return true;
        }

        // ── sign_text_front / sign_text_back (SignText.DIRECT_CODEC) ───────
        void WriteSignText(Nbt::Writer& w, std::string_view key, const SignTextComponent& t) {
            w.BeginCompound(key);
            auto messages = w.BeginList("messages", Nbt::TagType::Compound);
            for (const Text::Component& m : t.messages) {
                w.ListCompoundBegin(messages);
                WriteTextComponent(w, "", m);
                w.ListCompoundEnd(messages);
            }
            w.EndList(messages);
            if (t.filteredMessages) {
                auto filtered = w.BeginList("filtered_messages", Nbt::TagType::Compound);
                for (const Text::Component& m : *t.filteredMessages) {
                    w.ListCompoundBegin(filtered);
                    WriteTextComponent(w, "", m);
                    w.ListCompoundEnd(filtered);
                }
                w.EndList(filtered);
            }
            w.String("color", DyeName(t.color));
            if (t.glowing) w.Bool("has_glowing_text", true);
            w.EndCompound();
        }

        bool ReadSignText(const ::World::NBTTag& tag, SignTextComponent& out, const ReadContext& ctx) {
            const auto* c = AsCompound(&tag);
            if (!c) { ctx.Fail("Not a map"); return false; }
            const auto readLines = [&ctx](const ::World::NBTTag* t, std::array<Text::Component, 4>& lines) {
                const auto* list = AsList(t);
                if (!list || list->value.size() != 4) { ctx.Fail("Expected 4 messages"); return false; }
                for (size_t i = 0; i < 4; ++i) {
                    auto line = list->value[i] ? ReadTextComponent(*list->value[i]) : std::nullopt;
                    if (!line) { ctx.Fail("Not a text component"); return false; }
                    lines[i] = std::move(*line);
                }
                return true;
            };
            if (!readLines(c->GetTag("messages").get(), out.messages)) return false;
            if (auto filtered = c->GetTag("filtered_messages")) {
                std::array<Text::Component, 4> lines;
                if (!readLines(filtered.get(), lines)) return false;
                out.filteredMessages = std::move(lines);
            }
            if (auto color = c->GetTag("color")) {
                const auto name = StringOf(*color);
                const auto id = name ? DyeFromName(*name) : std::nullopt;
                if (!id) { ctx.Fail("Unknown dye color"); return false; }
                out.color = *id;
            }
            if (auto glow = c->GetTag("has_glowing_text")) out.glowing = BoolOf(*glow).value_or(false);
            return true;
        }

        void WriteSignFront(Nbt::Writer& w, std::string_view key, const ItemStack& stack) {
            if (auto t = stack.components.get(DataComponents::SIGN_TEXT_FRONT)) WriteSignText(w, key, *t);
        }
        bool ReadSignFront(const ::World::NBTTag& tag, ItemStack& stack, const ReadContext& ctx) {
            SignTextComponent t;
            if (!ReadSignText(tag, t, ctx)) return false;
            stack.components.set(DataComponents::SIGN_TEXT_FRONT, std::move(t));
            return true;
        }
        void WriteSignBack(Nbt::Writer& w, std::string_view key, const ItemStack& stack) {
            if (auto t = stack.components.get(DataComponents::SIGN_TEXT_BACK)) WriteSignText(w, key, *t);
        }
        bool ReadSignBack(const ::World::NBTTag& tag, ItemStack& stack, const ReadContext& ctx) {
            SignTextComponent t;
            if (!ReadSignText(tag, t, ctx)) return false;
            stack.components.set(DataComponents::SIGN_TEXT_BACK, std::move(t));
            return true;
        }

        // ── waxed (Unit) ─────────────────────────────────────────────────────
        void WriteWaxed(Nbt::Writer& w, std::string_view key, const ItemStack& stack) {
            if (!stack.components.get(DataComponents::WAXED)) return;
            w.BeginCompound(key);
            w.EndCompound();
        }
        bool ReadWaxed(const ::World::NBTTag&, ItemStack& stack, const ReadContext&) {
            stack.components.set(DataComponents::WAXED, true);
            return true;
        }

        // ── DyeColor-valued: cushion/color, base_color ───────────────────────
        template<auto Type>
        void WriteDye(Nbt::Writer& w, std::string_view key, const ItemStack& stack) {
            if (auto v = stack.components.get(*Type)) w.String(key, DyeName(*v));
        }
        template<auto Type>
        bool ReadDye(const ::World::NBTTag& tag, ItemStack& stack, const ReadContext& ctx) {
            const auto name = StringOf(tag);
            const auto id = name ? DyeFromName(*name) : std::nullopt;
            if (!id) { ctx.Fail("Unknown element name:" + name.value_or("?")); return false; }
            stack.components.set(*Type, static_cast<int32_t>(*id));
            return true;
        }

        // ── note_block_sound (Identifier) ────────────────────────────────────
        void WriteNoteSound(Nbt::Writer& w, std::string_view key, const ItemStack& stack) {
            if (auto v = stack.components.get(DataComponents::NOTE_BLOCK_SOUND)) w.String(key, WithNamespace(*v));
        }
        bool ReadNoteSound(const ::World::NBTTag& tag, ItemStack& stack, const ReadContext& ctx) {
            const auto id = StringOf(tag);
            if (!id || id->empty()) { ctx.Fail("Not a string"); return false; }
            stack.components.set(DataComponents::NOTE_BLOCK_SOUND, WithNamespace(*id));
            return true;
        }

        // ── profile (ResolvableProfile.CODEC: a name string, or the full
        // {name?, id?, properties?, texture?, cape?, elytra?, model?}) ──────
        void WriteProfile(Nbt::Writer& w, std::string_view key, const ItemStack& stack) {
            const auto p = stack.components.get(DataComponents::PROFILE);
            if (!p) return;
            const bool bareName = p->name && !p->id && p->properties.empty() && p->texture.empty() &&
                                  p->cape.empty() && p->elytra.empty() && p->model.empty();
            if (bareName) {
                w.String(key, *p->name);
                return;
            }
            w.BeginCompound(key);
            if (p->name) w.String("name", *p->name);
            if (p->id) w.IntArray("id", p->id->data(), 4);
            if (!p->properties.empty()) {
                auto list = w.BeginList("properties", Nbt::TagType::Compound);
                for (const ResolvableProfile::Property& prop : p->properties) {
                    w.ListCompoundBegin(list);
                    w.String("name", prop.name);
                    w.String("value", prop.value);
                    if (prop.signature) w.String("signature", *prop.signature);
                    w.ListCompoundEnd(list);
                }
                w.EndList(list);
            }
            if (!p->texture.empty()) w.String("texture", p->texture);
            if (!p->cape.empty()) w.String("cape", p->cape);
            if (!p->elytra.empty()) w.String("elytra", p->elytra);
            if (!p->model.empty()) w.String("model", p->model);
            w.EndCompound();
        }

        bool ReadProfile(const ::World::NBTTag& tag, ItemStack& stack, const ReadContext& ctx) {
            ResolvableProfile p;
            if (auto name = StringOf(tag)) {
                if (name->empty() || name->size() > 16) { ctx.Fail("Invalid player name: " + *name); return false; }
                p.name = *name;
            } else if (const auto* c = AsCompound(&tag)) {
                if (auto n = c->GetTag("name")) if (auto s = StringOf(*n)) p.name = *s;
                const std::vector<int32_t> id = ReadIntList(c->GetTag("id").get());
                if (id.size() == 4) p.id = std::array<int32_t, 4>{id[0], id[1], id[2], id[3]};
                if (const auto* props = AsList(c->GetTag("properties").get())) {
                    for (const auto& element : props->value) {
                        const auto* pc = AsCompound(Unwrap(element.get()));
                        if (!pc) continue;
                        ResolvableProfile::Property prop;
                        prop.name = pc->GetValue<std::string>("name", "");
                        prop.value = pc->GetValue<std::string>("value", "");
                        if (pc->HasTag("signature")) prop.signature = pc->GetValue<std::string>("signature", "");
                        p.properties.push_back(std::move(prop));
                    }
                }
                p.texture = c->GetValue<std::string>("texture", "");
                p.cape = c->GetValue<std::string>("cape", "");
                p.elytra = c->GetValue<std::string>("elytra", "");
                p.model = c->GetValue<std::string>("model", "");
            } else {
                ctx.Fail("Not a string or map");
                return false;
            }
            stack.components.set(DataComponents::PROFILE, std::move(p));
            return true;
        }

        // ── debug_stick_state ({block id: property name}) ───────────────────
        void WriteDebugStick(Nbt::Writer& w, std::string_view key, const ItemStack& stack) {
            const auto d = stack.components.get(DataComponents::DEBUG_STICK_STATE);
            if (!d) return;
            w.BeginCompound(key);
            for (const auto& [block, prop] : d->properties) w.String(WithNamespace(block), prop);
            w.EndCompound();
        }
        bool ReadDebugStick(const ::World::NBTTag& tag, ItemStack& stack, const ReadContext& ctx) {
            const auto* c = AsCompound(&tag);
            if (!c) { ctx.Fail("Not a map"); return false; }
            DebugStickState d;
            for (const auto& [block, value] : c->value) {
                if (!value) continue;
                if (auto s = StringOf(*value)) d = d.With(WithNamespace(block), *s);
            }
            stack.components.set(DataComponents::DEBUG_STICK_STATE, std::move(d));
            return true;
        }

        const Registrar kBlockEntityData{DataComponents::BLOCK_ENTITY_DATA, &WriteBlockEntityData, &ReadBlockEntityData};
        const Registrar kBlockState{DataComponents::BLOCK_STATE, &WriteBlockState, &ReadBlockState};
        const Registrar kBees{DataComponents::BEES, &WriteBees, &ReadBees};
        const Registrar kLock{DataComponents::LOCK, &WriteLock, &ReadLock};
        const Registrar kContainerLoot{DataComponents::CONTAINER_LOOT, &WriteContainerLoot, &ReadContainerLoot};
        const Registrar kSignFront{DataComponents::SIGN_TEXT_FRONT, &WriteSignFront, &ReadSignFront};
        const Registrar kSignBack{DataComponents::SIGN_TEXT_BACK, &WriteSignBack, &ReadSignBack};
        const Registrar kWaxed{DataComponents::WAXED, &WriteWaxed, &ReadWaxed};
        const Registrar kCushionColor{DataComponents::CUSHION_COLOR, &WriteDye<&DataComponents::CUSHION_COLOR>,
                                      &ReadDye<&DataComponents::CUSHION_COLOR>};
        const Registrar kBaseColor{DataComponents::BASE_COLOR, &WriteDye<&DataComponents::BASE_COLOR>,
                                   &ReadDye<&DataComponents::BASE_COLOR>};
        const Registrar kNoteSound{DataComponents::NOTE_BLOCK_SOUND, &WriteNoteSound, &ReadNoteSound};
        const Registrar kProfile{DataComponents::PROFILE, &WriteProfile, &ReadProfile};
        const Registrar kDebugStick{DataComponents::DEBUG_STICK_STATE, &WriteDebugStick, &ReadDebugStick};

    } // namespace

} // namespace Game::Anvil::ComponentNbt
