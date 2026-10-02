// File: src/common/data/components/InlineBlockTransformer.cpp
//
// A direct BLOCK_TRANSFORMER (MC BlockTransformer with its own
// BlockTransformData list instead of a registry key), interpreted from the
// NBT the component keeps: BlockTransformer.transformBlock, the
// block_state_provider types (BlockStateProviderTypes) and the worldgen
// block predicates (BlockPredicateType) a provider's rules test.
#include "GameplayDataComponents.hpp"
#include "ToolComponents.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/entity/Entity.hpp"
#include "common/entity/IUsePlayer.hpp"
#include "common/sound/SoundEvents.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/block/piston/PistonBaseBlock.hpp"
#include "common/world/level/ILevelWrite.hpp"
#include "common/world/level/World.hpp"
#include "common/world/level/WorldDrops.hpp"
#include "common/world/level/gameevent/GameEvent.hpp"
#include "common/world/loot/ChestLootTables.hpp"
#include "common/world/tags/DataTags.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

namespace Game {

    namespace {

        using Tag = ::World::NBTTag;
        using Compound = ::World::NBTTagCompound;
        using List = ::World::NBTTagList;

        const Compound* AsCompound(const Tag* t) {
            return t && t->type == ::World::NBTTagType::TAG_Compound ? static_cast<const Compound*>(t) : nullptr;
        }
        const List* AsList(const Tag* t) {
            return t && t->type == ::World::NBTTagType::TAG_List ? static_cast<const List*>(t) : nullptr;
        }
        std::optional<std::string> StringOf(const Tag* t) {
            if (!t || t->type != ::World::NBTTagType::TAG_String) return std::nullopt;
            return static_cast<const ::World::NBTTagString*>(t)->value;
        }
        std::optional<double> NumberOf(const Tag* t) {
            if (!t) return std::nullopt;
            switch (t->type) {
                case ::World::NBTTagType::TAG_Byte:   return static_cast<const ::World::NBTTagByte*>(t)->value;
                case ::World::NBTTagType::TAG_Short:  return static_cast<const ::World::NBTTagShort*>(t)->value;
                case ::World::NBTTagType::TAG_Int:    return static_cast<const ::World::NBTTagInt*>(t)->value;
                case ::World::NBTTagType::TAG_Long:   return static_cast<double>(static_cast<const ::World::NBTTagLong*>(t)->value);
                case ::World::NBTTagType::TAG_Float:  return static_cast<const ::World::NBTTagFloat*>(t)->value;
                case ::World::NBTTagType::TAG_Double: return static_cast<const ::World::NBTTagDouble*>(t)->value;
                default: return std::nullopt;
            }
        }
        const Tag* Field(const Compound& c, const char* key) {
            auto it = c.value.find(key);
            return it == c.value.end() ? nullptr : it->second.get();
        }
        bool BoolField(const Compound& c, const char* key, bool fallback) {
            const auto n = NumberOf(Field(c, key));
            return n ? *n != 0.0 : fallback;
        }
        std::string_view Bare(std::string_view id) {
            return id.rfind("minecraft:", 0) == 0 ? id.substr(10) : id;
        }

        // HolderSet<Block> entries: "#tag", an id, or a list of either.
        std::vector<std::string> HolderSet(const Tag* t) {
            std::vector<std::string> out;
            if (auto s = StringOf(t)) { out.push_back(*s); return out; }
            if (const List* l = AsList(t)) {
                for (const auto& e : l->value) if (auto s = StringOf(e.get())) out.push_back(*s);
            }
            return out;
        }

        glm::ivec3 Offset(const Compound& c) {
            const Tag* t = Field(c, "offset");
            glm::ivec3 o(0);
            if (const List* l = AsList(t)) {
                for (size_t i = 0; i < 3 && i < l->value.size(); ++i) o[static_cast<int>(i)] = static_cast<int>(NumberOf(l->value[i].get()).value_or(0.0));
            } else if (t && t->type == ::World::NBTTagType::TAG_Int_Array) {
                const auto& v = static_cast<const ::World::NBTTagIntArray*>(t)->value;
                for (size_t i = 0; i < 3 && i < v.size(); ++i) o[static_cast<int>(i)] = v[i];
            }
            return o;
        }

        // A property of `block` by name.
        bool FindProperty(BlockID block, std::string_view name, PropertyId& out) {
            const uint16_t n = BlockStates::PropertyCount(block);
            for (uint16_t i = 0; i < n; ++i) {
                const PropertyId prop = BlockStates::PropertyAt(block, i);
                if (BlockStates::PropertyName(prop) == name) { out = prop; return true; }
            }
            return false;
        }

        // BlockState.CODEC: {Name, Properties?}.
        std::optional<BlockState> ParseState(const Compound& c) {
            const auto name = StringOf(Field(c, "Name"));
            if (!name) return std::nullopt;
            BlockState state = BlockStates::FromSlug(Bare(*name));
            if (state.Block() == BlockID::Air && Bare(*name) != "air") return std::nullopt;
            if (const Compound* props = AsCompound(Field(c, "Properties"))) {
                for (const auto& [k, v] : props->value) {
                    const auto value = StringOf(v.get());
                    PropertyId prop{};
                    if (value && FindProperty(state.Block(), k, prop)) state = state.SetName(prop, *value);
                }
            }
            return state;
        }

        // Block.withPropertiesOf: `target`'s state with every property it
        // shares with `from` copied across.
        BlockState WithPropertiesOf(BlockState target, BlockState from) {
            const uint16_t n = BlockStates::PropertyCount(from.Block());
            for (uint16_t i = 0; i < n; ++i) {
                const PropertyId prop = BlockStates::PropertyAt(from.Block(), i);
                if (target.HasProperty(prop)) target = target.SetName(prop, from.GetName(prop));
            }
            return target;
        }

        struct Eval {
            ILevelWrite& level;
            JavaRandom*  random;
            glm::ivec3   pos;
        };

        // The worldgen BlockPredicate (BlockPredicateType).
        bool TestPredicate(const Tag* t, const Eval& e) {
            const Compound* c = AsCompound(t);
            if (!c) return false;
            const std::string type(Bare(StringOf(Field(*c, "type")).value_or("")));
            const glm::ivec3 at = e.pos + Offset(*c);
            const auto stateAt = [&]() { return e.level.GetBlockState(at.x, at.y, at.z); };
            if (type == "true") return true;
            if (type == "not") return !TestPredicate(Field(*c, "predicate"), e);
            if (type == "all_of" || type == "any_of") {
                const List* l = AsList(Field(*c, "predicates"));
                if (!l) return type == "all_of";
                for (const auto& p : l->value) {
                    const bool r = TestPredicate(p.get(), e);
                    if (type == "any_of" && r) return true;
                    if (type == "all_of" && !r) return false;
                }
                return type == "all_of";
            }
            if (type == "matching_blocks") {
                return BlockMatchesHolderSet(BlockRegistry::Get(stateAt().Block()), HolderSet(Field(*c, "blocks")));
            }
            if (type == "matching_block_tag") {
                const auto tag = StringOf(Field(*c, "tag"));
                return tag && BlockInTag(BlockRegistry::Get(stateAt().Block()), *tag);
            }
            if (type == "matching_fluids") {
                // FluidState of the cell: water / lava (the source or flowing
                // fluid ids).
                const BlockState s = stateAt();
                const bool water = BlockRegistry::ContainsWater(s);
                const bool lava = s.Block() == BlockID::Lava;
                for (const std::string& f : HolderSet(Field(*c, "fluids"))) {
                    const std::string_view id = Bare(f);
                    if (water && (id == "water" || id == "flowing_water" || id == "#water")) return true;
                    if (lava && (id == "lava" || id == "flowing_lava" || id == "#lava")) return true;
                }
                return false;
            }
            if (type == "replaceable") return BlockRegistry::Get(stateAt().Block()).replaceable;
            if (type == "solid") return BlockRegistry::IsOcclusionFullCube(stateAt());
            if (type == "inside_world_bounds") {
                return at.y >= DimensionMinY(e.level.GetDimension()) &&
                       at.y < DimensionMinY(e.level.GetDimension()) + DimensionLogicalHeight(e.level.GetDimension()) + 64;
            }
            // matching_biomes, has_sturdy_face, would_survive, unobstructed,
            // height_range, volume_match: not answerable here — false.
            return false;
        }

        std::optional<BlockState> Provide(const Tag* t, const Eval& e, int depth);

        // BlockStateProvider.getOptionalState.
        std::optional<BlockState> Provide(const Tag* t, const Eval& e, int depth) {
            if (depth > 32) return std::nullopt;
            const Compound* c = AsCompound(t);
            if (!c) {
                // A bare block id is a simple provider of its default state.
                if (auto s = StringOf(t)) {
                    const BlockState state = BlockStates::FromSlug(Bare(*s));
                    if (state.Block() != BlockID::Air || Bare(*s) == "air") return state;
                }
                return std::nullopt;
            }
            const auto typeName = StringOf(Field(*c, "type"));
            if (!typeName) return ParseState(*c);   // BlockState.FULL_CODEC
            const std::string type(Bare(*typeName));
            if (type == "simple") {
                const Compound* s = AsCompound(Field(*c, "state"));
                return s ? ParseState(*s) : std::nullopt;
            }
            if (type == "rotated") {
                const Compound* s = AsCompound(Field(*c, "state"));
                auto state = s ? ParseState(*s) : std::nullopt;
                if (!state) return std::nullopt;
                PropertyId axis{};
                if (e.random && FindProperty(state->Block(), "axis", axis)) {
                    static const char* kAxes[3] = {"x", "y", "z"};
                    state = state->SetName(axis, kAxes[e.random->NextInt(3)]);
                }
                return state;
            }
            if (type == "copy_properties") {
                auto source = Provide(Field(*c, "source"), e, depth + 1);
                if (!source) return std::nullopt;
                return WithPropertiesOf(*source, e.level.GetBlockState(e.pos.x, e.pos.y, e.pos.z));
            }
            if (type == "rule_based") {
                if (const List* rules = AsList(Field(*c, "rules"))) {
                    for (const auto& r : rules->value) {
                        const Compound* rule = AsCompound(r.get());
                        if (!rule || !TestPredicate(Field(*rule, "if_true"), e)) continue;
                        if (auto state = Provide(Field(*rule, "then"), e, depth + 1)) return state;
                    }
                }
                if (const Tag* fallback = Field(*c, "fallback")) return Provide(fallback, e, depth + 1);
                return std::nullopt;
            }
            if (type == "weighted") {
                // {entries: [{data: state, weight}]} — WeightedList.getRandom.
                const List* entries = AsList(Field(*c, "entries"));
                if (!entries || entries->value.empty()) return std::nullopt;
                int total = 0;
                for (const auto& en : entries->value) {
                    if (const Compound* ec = AsCompound(en.get())) total += static_cast<int>(NumberOf(Field(*ec, "weight")).value_or(1.0));
                }
                if (total <= 0) return std::nullopt;
                int pick = e.random ? e.random->NextInt(total) : 0;
                for (const auto& en : entries->value) {
                    const Compound* ec = AsCompound(en.get());
                    if (!ec) continue;
                    pick -= static_cast<int>(NumberOf(Field(*ec, "weight")).value_or(1.0));
                    if (pick < 0) {
                        const Compound* data = AsCompound(Field(*ec, "data"));
                        return data ? ParseState(*data) : std::nullopt;
                    }
                }
                return std::nullopt;
            }
            if (type == "random_block") {
                // {blocks: HolderSet<Block>} — one of them, default state.
                std::vector<BlockID> candidates;
                for (const std::string& entry : HolderSet(Field(*c, "blocks"))) {
                    if (!entry.empty() && entry[0] == '#') {
                        for (size_t i = 1; i < static_cast<size_t>(BlockID::Count); ++i) {
                            const Block& b = BlockRegistry::Get(static_cast<BlockID>(i));
                            if (!b.registrySlug.empty() && BlockInTag(b, entry)) candidates.push_back(static_cast<BlockID>(i));
                        }
                    } else {
                        const BlockState s = BlockStates::FromSlug(Bare(entry));
                        if (s.Block() != BlockID::Air) candidates.push_back(s.Block());
                    }
                }
                if (candidates.empty()) return std::nullopt;
                const size_t i = e.random ? static_cast<size_t>(e.random->NextInt(static_cast<int>(candidates.size()))) : 0;
                return BlockStates::Default(candidates[i]);
            }
            // noise / dual_noise / noise_threshold / randomized_int: worldgen
            // noise providers — no answer outside generation.
            return std::nullopt;
        }

        int FaceFromName(std::string_view name) {
            static const char* kFaces[6] = {"down", "up", "north", "south", "west", "east"};
            for (int i = 0; i < 6; ++i) if (name == kFaces[i]) return i;
            return -1;
        }

        int ParticleEvent(std::string_view name) {
            if (name == "scrape")  return 3005;
            if (name == "wax_on")  return 3003;
            if (name == "wax_off") return 3004;
            return 0;
        }

        std::string ValidateProvider(const Tag* t, int depth) {
            if (depth > 32) return "Block state provider nested too deeply";
            if (StringOf(t)) return {};
            const Compound* c = AsCompound(t);
            if (!c) return "Not a block state provider";
            const auto type = StringOf(Field(*c, "type"));
            if (!type) return Field(*c, "Name") ? std::string() : std::string("No key Name in MapLike");
            const std::string_view tn = Bare(*type);
            if (tn == "simple" || tn == "rotated") return Field(*c, "state") ? std::string() : std::string("No key state in MapLike");
            if (tn == "copy_properties") return ValidateProvider(Field(*c, "source"), depth + 1);
            if (tn == "rule_based") {
                if (!AsList(Field(*c, "rules"))) return "No key rules in MapLike";
                for (const auto& r : AsList(Field(*c, "rules"))->value) {
                    const Compound* rule = AsCompound(r.get());
                    if (!rule || !Field(*rule, "if_true") || !Field(*rule, "then")) return "Rule needs if_true and then";
                    if (std::string err = ValidateProvider(Field(*rule, "then"), depth + 1); !err.empty()) return err;
                }
                if (const Tag* f = Field(*c, "fallback")) return ValidateProvider(f, depth + 1);
                return {};
            }
            if (tn == "weighted") return AsList(Field(*c, "entries")) ? std::string() : std::string("No key entries in MapLike");
            if (tn == "random_block") return Field(*c, "blocks") ? std::string() : std::string("No key blocks in MapLike");
            if (tn == "noise" || tn == "dual_noise" || tn == "noise_threshold" || tn == "randomized_int") return {};
            return "Unknown registry key in ResourceKey[minecraft:root / minecraft:worldgen/block_state_provider_type]: " +
                   std::string(*type);
        }

    } // namespace

    std::string ValidateInlineBlockTransformer(const ::World::NBTTagCompound& transformer) {
        const List* transforms = AsList(Field(transformer, "transforms"));
        if (!transforms || transforms->value.empty()) return "List must have contents";
        if (transforms->value.size() > 200) return "List is too long: " + std::to_string(transforms->value.size()) + ", expected range [1-200]";
        for (const auto& t : transforms->value) {
            const Compound* data = AsCompound(t.get());
            if (!data) return "Not a map";
            if (!Field(*data, "block_state_provider")) return "No key block_state_provider in MapLike";
            if (std::string err = ValidateProvider(Field(*data, "block_state_provider"), 0); !err.empty()) return err;
        }
        return {};
    }

    UseResult UseOnInlineBlockTransformer(const UseOnContext& ctx, ItemStack& stack) {
        const auto holder = stack.get(DataComponents::BLOCK_TRANSFORMER);
        if (!holder || !holder->IsDirect() || !ctx.world) return UseResult::Pass;
        // playerHasBlockingItemUseIntent: the main hand, with a BLOCKS_ATTACKS
        // item in the off hand and not sneaking.
        if (ctx.player && ctx.hand == 0 && !ctx.player->IsSneaking() &&
            ctx.player->getItemInHand(1).has(DataComponents::BLOCKS_ATTACKS)) {
            return UseResult::Pass;
        }
        const List* transforms = AsList(Field(holder->direct.Tag(), "transforms"));
        if (!transforms) return UseResult::Pass;
        ILevelWrite& level = *ctx.world;
        const glm::ivec3 pos = ctx.hitResult.blockPos;
        const int face = ctx.hitResult.face;
        Eval eval{level, level.Random(), pos};
        Entity* source = ctx.player ? ctx.player->GameEventSource() : nullptr;

        for (const auto& t : transforms->value) {
            const Compound* data = AsCompound(t.get());
            if (!data) continue;
            bool disallowed = false;
            if (const List* faces = AsList(Field(*data, "disallowed_faces"))) {
                for (const auto& f : faces->value) {
                    if (auto n = StringOf(f.get()); n && FaceFromName(*n) == face) disallowed = true;
                }
            }
            if (disallowed) continue;
            const std::optional<BlockState> newState = Provide(Field(*data, "block_state_provider"), eval, 0);
            if (!newState) continue;
            const BlockState updated = BoolField(*data, "update_from_neighbors", true)
                ? UpdateFromNeighbourShapes(level, *newState, pos) : *newState;
            const BlockState oldState = level.GetBlockState(pos.x, pos.y, pos.z);

            // The loot table, popped from the middle or the clicked face.
            if (!level.IsClientSide()) {
                if (auto loot = StringOf(Field(*data, "loot")); loot && level.Random()) {
                    std::vector<ItemStack> drops;
                    ChestLoot::GetRandomItems(std::string(*loot).find(':') == std::string::npos ? "minecraft:" + *loot : *loot,
                                              *level.Random(), 0.0f, drops);
                    const bool fromFace = StringOf(Field(*data, "drop_strategy")).value_or("from_middle") == "clicked_face";
                    for (const ItemStack& d : drops) {
                        if (fromFace) DropItemStackFromFace(level.GetDimension(), pos, face, d);
                        else          DropItemStackNear(level.GetDimension(), pos, d);
                    }
                }
            }
            (void)oldState;

            // itemInHand.consume(consumeOnUse ? 1 : 0) for a stackable item,
            // else hurtAndBreak(item_damage_per_use).
            if (GetMaxStackSize(stack) > 1) {
                if (BoolField(*data, "consume_on_use", true) && !(ctx.player && ctx.player->isCreative())) {
                    if (--stack.count <= 0) stack.Clear();
                }
            } else if (ctx.player) {
                const int damage = static_cast<int>(NumberOf(Field(*data, "item_damage_per_use")).value_or(0.0));
                if (damage > 0) HurtAndBreak(stack, damage, ctx.world, ctx.player, ctx.hand);
            }

            level.SetBlock(pos.x, pos.y, pos.z, updated, World::UpdateFlags::All | World::UpdateFlags::Immediate);
            if (auto sound = StringOf(Field(*data, "sound"))) {
                level.PlaySound(ctx.player, pos, Bare(*sound), SoundSource::Blocks, 1.0f, 1.0f);
            } else if (const Compound* sc = AsCompound(Field(*data, "sound"))) {
                if (auto id = StringOf(Field(*sc, "sound_id"))) {
                    level.PlaySound(ctx.player, pos, Bare(*id), SoundSource::Blocks, 1.0f, 1.0f);
                }
            }
            if (auto particle = StringOf(Field(*data, "particle"))) {
                if (const int event = ParticleEvent(*particle)) level.PlayLevelEvent(ctx.player, event, pos, 0);
            }
            level.GameEvent(GameEventId::BlockChange, pos, GameEventContext::Of(source, updated));
            return UseResult::Success;
        }
        return UseResult::Pass;
    }

} // namespace Game
