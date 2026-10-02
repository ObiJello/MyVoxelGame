// File: src/server/world/storage/anvil/components/GameplayDataNbt.cpp
//
// NBT codecs (MC DataComponentType.codec() over NbtOps) for the components
// of common/data/components/GameplayDataComponents.hpp, registered with
// ComponentNbt — plus the server's adventure-mode checks against a live
// block (they read the block entity's full save for an nbt predicate).
#include "server/world/storage/anvil/ComponentNbt.hpp"
#include "common/data/components/GameplayDataComponents.hpp"
#include "common/world/level/World.hpp"
#include "server/commands/SnbtParser.hpp"

#include <algorithm>
#include <cmath>

namespace Game {

    namespace {
        std::shared_ptr<::World::NBTTagCompound> BlockEntityNbtFor(const AdventureModePredicate& p, World& world,
                                                                   const glm::ivec3& pos) {
            return AdventurePredicateNeedsNbt(p) ? BlockData::SaveBlockEntityNbt(world, pos) : nullptr;
        }
    } // namespace

    bool CanBreakInAdventureAt(const ItemStack& mainHand, World& world, const glm::ivec3& pos, BlockState state) {
        if (mainHand.IsEmpty()) return false;
        const auto p = mainHand.get(DataComponents::CAN_BREAK);
        if (!p) return false;
        const auto nbt = BlockEntityNbtFor(*p, world, pos);
        return AdventurePredicateMatches(*p, state, nbt.get());
    }

    bool CanPlaceOnInAdventureAt(const ItemStack& stack, World& world, const glm::ivec3& clicked) {
        if (stack.IsEmpty()) return false;
        const auto p = stack.get(DataComponents::CAN_PLACE_ON);
        if (!p) return false;
        const auto nbt = BlockEntityNbtFor(*p, world, clicked);
        return AdventurePredicateMatches(*p, world.GetBlockState(clicked.x, clicked.y, clicked.z), nbt.get());
    }

} // namespace Game

namespace Game::Anvil::ComponentNbt {

    namespace {

        std::string Describe(const ::World::NBTTag& tag) { return NbtTagToSnbt(tag); }

        // ── ResolvableInt / ResolvableFloat: a number, or a registry key ──
        bool ReadResolvableInt(const ::World::NBTTag* tag, ResolvableInt& out, const ReadContext& ctx) {
            if (!tag) { ctx.Fail("Missing value"); return false; }
            if (auto s = StringOf(*tag)) {
                const std::string key = WithNamespace(*s);
                if (!IsKnownIntProvider(key)) {
                    ctx.Fail("Unknown registry key in ResourceKey[minecraft:root / minecraft:context_int_provider]: " + key);
                    return false;
                }
                out = ResolvableInt::Reference(key);
                return true;
            }
            if (auto n = NumberOf(*tag)) {
                out = ResolvableInt::Constant(static_cast<int>(*n));
                return true;
            }
            ctx.Fail("Not a number or a registry key: " + Describe(*tag));
            return false;
        }

        bool ReadResolvableFloat(const ::World::NBTTag* tag, ResolvableFloat& out, const ReadContext& ctx) {
            if (!tag) { ctx.Fail("Missing value"); return false; }
            if (auto s = StringOf(*tag)) {
                const std::string key = WithNamespace(*s);
                if (!IsKnownFloatProvider(key)) {
                    ctx.Fail("Unknown registry key in ResourceKey[minecraft:root / minecraft:context_float_provider]: " + key);
                    return false;
                }
                out = ResolvableFloat::Reference(key);
                return true;
            }
            if (auto n = NumberOf(*tag)) {
                out = ResolvableFloat::Constant(static_cast<float>(*n));
                return true;
            }
            ctx.Fail("Not a number or a registry key: " + Describe(*tag));
            return false;
        }

        void WriteResolvableInt(Nbt::Writer& w, std::string_view key, const ResolvableInt& v) {
            if (v.constant) w.Int(key, *v.constant);
            else            w.String(key, v.reference);
        }

        void WriteResolvableFloat(Nbt::Writer& w, std::string_view key, const ResolvableFloat& v) {
            if (v.constant) w.Float(key, *v.constant);
            else            w.String(key, v.reference);
        }

        // ── compostable {layers} ───────────────────────────────────────────
        void WriteCompostable(Nbt::Writer& w, std::string_view key, const ItemStack& stack) {
            const auto v = stack.components.get(DataComponents::COMPOSTABLE);
            if (!v) return;
            w.BeginCompound(key);
            WriteResolvableInt(w, "layers", v->layers);
            w.EndCompound();
        }

        bool ReadCompostable(const ::World::NBTTag& tag, ItemStack& stack, const ReadContext& ctx) {
            const auto* c = AsCompound(&tag);
            if (!c) { ctx.Fail("Not a map: " + Describe(tag)); return false; }
            if (!c->GetTag("layers")) { ctx.Fail("No key layers in MapLike"); return false; }
            Compostable v;
            if (!ReadResolvableInt(c->GetTag("layers").get(), v.layers, ctx)) return false;
            stack.components.set(DataComponents::COMPOSTABLE, v);
            return true;
        }

        // ── cooking_fuel {burn_time, speed_multiplier} ─────────────────────
        void WriteCookingFuel(Nbt::Writer& w, std::string_view key, const ItemStack& stack) {
            const auto v = stack.components.get(DataComponents::COOKING_FUEL);
            if (!v) return;
            w.BeginCompound(key);
            WriteResolvableInt(w, "burn_time", v->burnTime);
            WriteResolvableFloat(w, "speed_multiplier", v->speedMultiplier);
            w.EndCompound();
        }

        bool ReadCookingFuel(const ::World::NBTTag& tag, ItemStack& stack, const ReadContext& ctx) {
            const auto* c = AsCompound(&tag);
            if (!c) { ctx.Fail("Not a map: " + Describe(tag)); return false; }
            if (!c->GetTag("burn_time")) { ctx.Fail("No key burn_time in MapLike"); return false; }
            if (!c->GetTag("speed_multiplier")) { ctx.Fail("No key speed_multiplier in MapLike"); return false; }
            CookingFuel v;
            if (!ReadResolvableInt(c->GetTag("burn_time").get(), v.burnTime, ctx)) return false;
            if (!ReadResolvableFloat(c->GetTag("speed_multiplier").get(), v.speedMultiplier, ctx)) return false;
            stack.components.set(DataComponents::COOKING_FUEL, v);
            return true;
        }

        // ── brewing_fuel {uses, speed_multiplier} ──────────────────────────
        void WriteBrewingFuel(Nbt::Writer& w, std::string_view key, const ItemStack& stack) {
            const auto v = stack.components.get(DataComponents::BREWING_FUEL);
            if (!v) return;
            w.BeginCompound(key);
            WriteResolvableInt(w, "uses", v->uses);
            WriteResolvableFloat(w, "speed_multiplier", v->speedMultiplier);
            w.EndCompound();
        }

        bool ReadBrewingFuel(const ::World::NBTTag& tag, ItemStack& stack, const ReadContext& ctx) {
            const auto* c = AsCompound(&tag);
            if (!c) { ctx.Fail("Not a map: " + Describe(tag)); return false; }
            if (!c->GetTag("uses")) { ctx.Fail("No key uses in MapLike"); return false; }
            if (!c->GetTag("speed_multiplier")) { ctx.Fail("No key speed_multiplier in MapLike"); return false; }
            BrewingFuel v;
            if (!ReadResolvableInt(c->GetTag("uses").get(), v.uses, ctx)) return false;
            if (!ReadResolvableFloat(c->GetTag("speed_multiplier").get(), v.speedMultiplier, ctx)) return false;
            stack.components.set(DataComponents::BREWING_FUEL, v);
            return true;
        }

        // ── villager_food {nutrition: NON_NEGATIVE_INT} ────────────────────
        void WriteVillagerFood(Nbt::Writer& w, std::string_view key, const ItemStack& stack) {
            const auto v = stack.components.get(DataComponents::VILLAGER_FOOD);
            if (!v) return;
            w.BeginCompound(key);
            w.Int("nutrition", v->nutrition);
            w.EndCompound();
        }

        bool ReadVillagerFood(const ::World::NBTTag& tag, ItemStack& stack, const ReadContext& ctx) {
            const auto* c = AsCompound(&tag);
            if (!c) { ctx.Fail("Not a map: " + Describe(tag)); return false; }
            auto n = c->GetTag("nutrition");
            if (!n) { ctx.Fail("No key nutrition in MapLike"); return false; }
            const auto v = NumberOf(*n);
            if (!v) { ctx.Fail("Not a number: " + Describe(*n)); return false; }
            const int nutrition = static_cast<int>(*v);
            if (nutrition < 0) { ctx.Fail("Value must be non-negative: " + std::to_string(nutrition)); return false; }
            stack.components.set(DataComponents::VILLAGER_FOOD, VillagerFood{nutrition});
            return true;
        }

        // ── block_transformer (Holder<BlockTransformer>: a registry key, or
        //    a direct BlockTransformer — its BlockTransformData list) ───────
        void WriteBlockTransformer(Nbt::Writer& w, std::string_view key, const ItemStack& stack) {
            const auto v = stack.components.get(DataComponents::BLOCK_TRANSFORMER);
            if (!v) return;
            if (!v->key.empty()) { w.String(key, WithNamespace(v->key)); return; }
            if (const ::World::NBTTag* transforms = Unwrap(v->direct.Tag().GetTag("transforms").get())) {
                WriteNbtTag(w, key, *transforms);
            }
        }

        bool ReadBlockTransformer(const ::World::NBTTag& tag, ItemStack& stack, const ReadContext& ctx) {
            if (const auto s = StringOf(tag)) {
                const std::string key = WithNamespace(*s);
                if (!IsKnownBlockTransformer(key)) {
                    ctx.Fail("Unknown registry key in ResourceKey[minecraft:root / minecraft:block_transformer]: " + key);
                    return false;
                }
                stack.components.set(DataComponents::BLOCK_TRANSFORMER, BlockTransformerHolder{key, {}});
                return true;
            }
            // BlockTransformer.DIRECT_CODEC: the BlockTransformData list.
            if (!AsList(&tag)) { ctx.Fail("Not a list or a registry key: " + Describe(tag)); return false; }
            auto root = std::make_shared<::World::NBTTagCompound>();
            root->value["transforms"] = CloneNbtTag(tag);
            if (const std::string err = ValidateInlineBlockTransformer(*root); !err.empty()) {
                ctx.Fail(err);
                return false;
            }
            stack.components.set(DataComponents::BLOCK_TRANSFORMER, BlockTransformerHolder{{}, NbtCompoundValue(std::move(root))});
            return true;
        }

        // ── mob_visibility {targeting_entity_types, visibility} ────────────
        void WriteMobVisibility(Nbt::Writer& w, std::string_view key, const ItemStack& stack) {
            const auto v = stack.components.get(DataComponents::MOB_VISIBILITY);
            if (!v) return;
            w.BeginCompound(key);
            std::vector<std::string> types;
            for (const std::string& t : v->targetingEntityTypes) {
                types.push_back(!t.empty() && t[0] == '#' ? "#" + WithNamespace(t.substr(1)) : WithNamespace(t));
            }
            WriteHolderSet(w, "targeting_entity_types", types);
            w.Float("visibility", v->visibility);
            w.EndCompound();
        }

        bool ReadMobVisibility(const ::World::NBTTag& tag, ItemStack& stack, const ReadContext& ctx) {
            const auto* c = AsCompound(&tag);
            if (!c) { ctx.Fail("Not a map: " + Describe(tag)); return false; }
            if (!c->GetTag("targeting_entity_types")) { ctx.Fail("No key targeting_entity_types in MapLike"); return false; }
            auto vis = c->GetTag("visibility");
            if (!vis) { ctx.Fail("No key visibility in MapLike"); return false; }
            MobVisibility v;
            v.targetingEntityTypes = ReadHolderSet(c->GetTag("targeting_entity_types").get());
            const auto f = NumberOf(*vis);
            if (!f) { ctx.Fail("Not a number: " + Describe(*vis)); return false; }
            v.visibility = static_cast<float>(*f);
            stack.components.set(DataComponents::MOB_VISIBILITY, std::move(v));
            return true;
        }

        // ── intangible_projectile (Unit: {}) ───────────────────────────────
        void WriteIntangible(Nbt::Writer& w, std::string_view key, const ItemStack& stack) {
            if (!stack.components.get(DataComponents::INTANGIBLE_PROJECTILE)) return;
            w.BeginCompound(key);
            w.EndCompound();
        }

        bool ReadIntangible(const ::World::NBTTag&, ItemStack& stack, const ReadContext&) {
            stack.components.set(DataComponents::INTANGIBLE_PROJECTILE, true);
            return true;
        }

        // ── can_place_on / can_break (AdventureModePredicate) ──────────────
        //
        // compactListCodec(BlockPredicate): one predicate, or a non-empty
        // list of them. BlockPredicate: {blocks?, state?, nbt?} — blocks a
        // HolderSet<Block>, state a map of property -> "value" or {min?,
        // max?}, nbt a compound or its SNBT string (TagParser.LENIENT_CODEC).

        bool ReadBlockPredicate(const ::World::NBTTag& tag, BlockPredicateSpec& out, const ReadContext& ctx) {
            const auto* c = AsCompound(&tag);
            if (!c) { ctx.Fail("Not a map: " + Describe(tag)); return false; }
            if (auto blocks = c->GetTag("blocks")) {
                out.blocks = ReadHolderSet(blocks.get());
                for (std::string& b : *out.blocks) {
                    if (!b.empty() && b[0] == '#') b = "#" + WithNamespace(b.substr(1));
                    else b = WithNamespace(b);
                }
            }
            if (auto stateTag = c->GetTag("state")) {
                const auto* state = AsCompound(stateTag.get());
                if (!state) { ctx.Fail("Not a map: " + Describe(*stateTag)); return false; }
                std::vector<BlockPredicateSpec::PropertyMatcher> matchers;
                std::vector<std::string> names;
                for (const auto& [name, _] : state->value) names.push_back(name);
                std::sort(names.begin(), names.end());
                for (const std::string& name : names) {
                    const ::World::NBTTag* value = Unwrap(state->value.at(name).get());
                    BlockPredicateSpec::PropertyMatcher m;
                    m.name = name;
                    if (const auto* range = AsCompound(value)) {
                        if (auto lo = range->GetTag("min")) {
                            auto s = StringOf(*lo);
                            if (!s) { ctx.Fail("Not a string: " + Describe(*lo)); return false; }
                            m.min = *s;
                        }
                        if (auto hi = range->GetTag("max")) {
                            auto s = StringOf(*hi);
                            if (!s) { ctx.Fail("Not a string: " + Describe(*hi)); return false; }
                            m.max = *s;
                        }
                    } else if (value) {
                        auto s = StringOf(*value);
                        if (!s) { ctx.Fail("Not a string: " + Describe(*value)); return false; }
                        m.exact = *s;
                    }
                    matchers.push_back(std::move(m));
                }
                out.state = std::move(matchers);
            }
            if (auto nbt = c->GetTag("nbt")) {
                if (const auto* compound = AsCompound(nbt.get())) {
                    auto copy = std::dynamic_pointer_cast<::World::NBTTagCompound>(CloneNbtTag(*compound));
                    out.nbt = NbtCompoundValue(std::move(copy));
                } else if (auto text = StringOf(*nbt)) {
                    std::string parseError;
                    auto parsed = Server::Snbt::ParseCompound(*text, parseError);
                    if (!parsed) { ctx.Fail(parseError); return false; }
                    out.nbt = NbtCompoundValue(std::move(parsed));
                } else {
                    ctx.Fail("Not a compound tag: " + Describe(*nbt));
                    return false;
                }
            }
            return true;
        }

        void WriteBlockPredicate(Nbt::Writer& w, const BlockPredicateSpec& p) {
            if (p.blocks) WriteHolderSet(w, "blocks", *p.blocks);
            if (p.state) {
                w.BeginCompound("state");
                for (const auto& m : *p.state) {
                    if (m.exact) {
                        w.String(m.name, *m.exact);
                    } else {
                        w.BeginCompound(m.name);
                        if (m.min) w.String("min", *m.min);
                        if (m.max) w.String("max", *m.max);
                        w.EndCompound();
                    }
                }
                w.EndCompound();
            }
            if (p.nbt) WriteNbtTag(w, "nbt", p.nbt->Tag());
        }

        template<auto Type>
        void WriteAdventure(Nbt::Writer& w, std::string_view key, const ItemStack& stack) {
            const auto v = stack.components.get(*Type);
            if (!v) return;
            if (v->predicates.size() == 1) {
                w.BeginCompound(key);
                WriteBlockPredicate(w, v->predicates.front());
                w.EndCompound();
                return;
            }
            auto list = w.BeginList(key, Nbt::TagType::Compound);
            for (const BlockPredicateSpec& p : v->predicates) {
                w.ListCompoundBegin(list);
                WriteBlockPredicate(w, p);
                w.ListCompoundEnd(list);
            }
            w.EndList(list);
        }

        template<auto Type>
        bool ReadAdventure(const ::World::NBTTag& tag, ItemStack& stack, const ReadContext& ctx) {
            AdventureModePredicate v;
            if (const auto* list = AsList(&tag)) {
                if (list->value.empty()) { ctx.Fail("List must have contents"); return false; }
                for (const auto& element : list->value) {
                    BlockPredicateSpec p;
                    const ::World::NBTTag* e = Unwrap(element.get());
                    if (!e || !ReadBlockPredicate(*e, p, ctx)) return false;
                    v.predicates.push_back(std::move(p));
                }
            } else {
                BlockPredicateSpec p;
                if (!ReadBlockPredicate(tag, p, ctx)) return false;
                v.predicates.push_back(std::move(p));
            }
            stack.components.set(*Type, std::move(v));
            return true;
        }

        const Registrar kCompostable{DataComponents::COMPOSTABLE, &WriteCompostable, &ReadCompostable};
        const Registrar kCookingFuel{DataComponents::COOKING_FUEL, &WriteCookingFuel, &ReadCookingFuel};
        const Registrar kBrewingFuel{DataComponents::BREWING_FUEL, &WriteBrewingFuel, &ReadBrewingFuel};
        const Registrar kVillagerFood{DataComponents::VILLAGER_FOOD, &WriteVillagerFood, &ReadVillagerFood};
        // (additional_trade_cost is transient in MC — never saved, never
        // settable from a command: no codec.)
        const Registrar kBlockTransformer{DataComponents::BLOCK_TRANSFORMER, &WriteBlockTransformer, &ReadBlockTransformer};
        const Registrar kMobVisibility{DataComponents::MOB_VISIBILITY, &WriteMobVisibility, &ReadMobVisibility};
        const Registrar kIntangible{DataComponents::INTANGIBLE_PROJECTILE, &WriteIntangible, &ReadIntangible};
        const Registrar kCanPlaceOn{DataComponents::CAN_PLACE_ON,
                                    &WriteAdventure<&DataComponents::CAN_PLACE_ON>,
                                    &ReadAdventure<&DataComponents::CAN_PLACE_ON>};
        const Registrar kCanBreak{DataComponents::CAN_BREAK,
                                  &WriteAdventure<&DataComponents::CAN_BREAK>,
                                  &ReadAdventure<&DataComponents::CAN_BREAK>};

    } // namespace

} // namespace Game::Anvil::ComponentNbt
