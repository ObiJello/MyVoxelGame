// File: src/common/world/loot/LootTables.cpp
#include "LootTables.hpp"
#include "GeneratedLootTables.hpp"
#include "common/core/JavaRandom.hpp"
#include "common/core/Log.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/crafting/RecipeManager.hpp"
#include "common/world/level/World.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Game {

    namespace {

        bool s_initialized = false;

        // BlockID → index into kLootTables, or -1 for "no loot table"
        // (MC's Optional<ResourceKey<LootTable>> being empty).
        std::vector<int16_t> s_tableForBlock;
        // Entry index → resolved ItemID. Pre-resolving means GetDrops never
        // touches a string.
        std::vector<ItemID> s_entryItem;
        // Condition arg index → resolved ItemID, for match_tool item sets.
        std::vector<ItemID> s_argItem;
        // location_check's target block slug → BlockID, same idea.
        std::vector<BlockID> s_argBlock;

        std::string_view Arg(uint32_t index) { return kLootArgs[index]; }

        // MC's one use of Properties.overrideLootTable (Blocks.java:1285): the
        // shared `wallProperties` builder hands every wall-mounted variant its
        // STANDING counterpart's loot table, so those 45 blocks ship no table
        // file of their own. Without this a wall torch or wall sign would drop
        // nothing at all. Returns "" when `slug` is not a wall variant.
        std::string StandingCounterpart(const std::string& slug) {
            static const std::pair<std::string_view, std::string_view> kWallSuffixes[] = {
                {"_wall_hanging_sign", "_hanging_sign"},
                {"_wall_sign",         "_sign"},
                {"_wall_banner",       "_banner"},
                {"_wall_head",         "_head"},
                {"_wall_skull",        "_skull"},
                {"_wall_fan",          "_fan"},
                {"_wall_torch",        "_torch"},
            };
            for (const auto& [from, to] : kWallSuffixes) {
                if (slug.size() > from.size() &&
                    slug.compare(slug.size() - from.size(), from.size(), from) == 0) {
                    return slug.substr(0, slug.size() - from.size()) + std::string(to);
                }
            }
            // The un-prefixed one: "wall_torch" -> "torch".
            if (slug == "wall_torch") return "torch";
            return {};
        }

        // ── Enchantments ────────────────────────────────────────────────────
        // Tools cannot hold enchantments yet: DataComponents registers
        // STORED_ENCHANTMENTS (enchanted books) but not ENCHANTMENTS — see the
        // notes at Item.cpp:544 and EnchantmentHelper.hpp:24. Every Silk Touch
        // and Fortune test below therefore evaluates at level 0, which is
        // exactly MC's no-enchantment branch, so stone gives cobblestone and
        // ores give their raw drop at ×1.
        //
        // This is the ONE function to change when ENCHANTMENTS lands. Every
        // silk-touch alternative, table_bonus and apply_bonus in the baked data
        // is already wired to it and starts working the moment it returns
        // a real level.
        int EnchantmentLevel(const ItemStack* tool, LootEnchantment which) {
            if (!tool || tool->IsEmpty()) return 0;
            (void)which;   // Enchantment::SilkTouch / Enchantment::Fortune
            return 0;
        }

        // ── Conditions ──────────────────────────────────────────────────────
        bool TestCondition(const LootCondRow& cond, const LootContext& ctx);

        bool TestConditions(uint32_t begin, uint16_t count, const LootContext& ctx) {
            for (uint16_t i = 0; i < count; ++i) {
                if (!TestCondition(kLootConditions[begin + i], ctx)) return false;
            }
            return true;   // MC AllOfCondition — an empty list passes
        }

        bool TestCondition(const LootCondRow& cond, const LootContext& ctx) {
            switch (static_cast<LootCondType>(cond.type)) {

            case LootCondType::SurvivesExplosion:
                // MC SurvivesExplosionCondition: true when there is no
                // explosion radius parameter. Nothing produces explosions yet.
                return true;

            case LootCondType::EntityProperties:
                // Every use in the block tables is {entity:"this", predicate:{}}
                // — an empty predicate that only asserts the entity EXISTS,
                // i.e. something broke the block rather than an explosion.
                // Returning false here would silently stop snow and
                // chorus_flower from dropping at all.
                return ctx.brokenByEntity;

            case LootCondType::RandomChance:
                return ctx.rng->NextFloat() < cond.chance;

            case LootCondType::Inverted:
                return !TestConditions(cond.childBegin, cond.childCount, ctx);

            case LootCondType::AnyOf:
                for (uint16_t i = 0; i < cond.childCount; ++i) {
                    if (TestCondition(kLootConditions[cond.childBegin + i], ctx)) return true;
                }
                return false;

            case LootCondType::MatchToolItems: {
                if (!ctx.tool || ctx.tool->IsEmpty()) return false;
                for (uint16_t i = 0; i < cond.argCount; ++i) {
                    if (s_argItem[cond.argBegin + i] == ctx.tool->itemId) return true;
                }
                return false;
            }

            case LootCondType::MatchToolEnchantment:
                return EnchantmentLevel(ctx.tool, static_cast<LootEnchantment>(cond.ench))
                       >= cond.i0;

            case LootCondType::TableBonus: {
                // MC BonusLevelTableCondition: chances[] indexed by level,
                // clamped to the last entry.
                if (cond.floatCount == 0) return false;
                const int level = EnchantmentLevel(ctx.tool,
                                                   static_cast<LootEnchantment>(cond.ench));
                const uint16_t idx = static_cast<uint16_t>(
                    std::min<int>(level, cond.floatCount - 1));
                return ctx.rng->NextFloat() < kLootFloats[cond.floatBegin + idx];
            }

            case LootCondType::BlockStateProperty: {
                // Args are flat (property, value) pairs, ALL of which must
                // match. This engine's state list only carries facing / axis /
                // fire's booleans, so `age`, `half`, `candles`, … simply do not
                // exist and the test fails — which is the vanilla answer for a
                // block that is not in that state. Wheat, for one, is never
                // age=7 here (nothing grows it), so it drops seeds.
                const auto& def = BlockRegistry::GetStateDefinition(ctx.block);
                for (uint16_t i = 0; i + 1 < cond.argCount; i += 2) {
                    const std::string_view prop  = Arg(cond.argBegin + i);
                    const std::string_view value = Arg(cond.argBegin + i + 1);
                    if (def.ValueOf(ctx.blockState, prop) != value) return false;
                }
                return true;
            }

            case LootCondType::LocationCheck: {
                // Only the "block at +offsetY is X" shape occurs (tall_grass
                // and large_fern confirming their own upper half is present).
                if (!ctx.world || cond.argCount == 0) return true;
                const BlockID want = s_argBlock[cond.argBegin];
                if (want == BlockID::Air) return true;   // unresolvable — don't block the drop
                const BlockID got = ctx.world->GetBlock(ctx.pos.x,
                                                        ctx.pos.y + cond.i0,
                                                        ctx.pos.z);
                if (got == want) return true;
                // The wanted state may be a promoted variant BlockID of the
                // same vanilla block (tall_grass{half:upper} is TallGrassTop
                // here), so compare registry slugs as well.
                return BlockRegistry::Get(got).registrySlug ==
                       BlockRegistry::Get(want).registrySlug;
            }
            }
            return true;
        }

        // ── Functions ───────────────────────────────────────────────────────
        // MC's apply_bonus formulas, ported verbatim from ApplyBonusCount.
        int ApplyBonusFormula(const LootFuncRow& fn, int count, int level, JavaRandom& rng) {
            if (level <= 0) return count;   // every formula is identity at level 0
            switch (static_cast<LootBonusFormula>(fn.mode)) {
            case LootBonusFormula::OreDrops: {
                // OreDrops.calculateNewCount: count * max(0, nextInt(level+2) - 1) + count
                const int bonus = std::max(0, rng.NextInt(level + 2) - 1);
                return count * (bonus + 1);
            }
            case LootBonusFormula::UniformBonusCount:
                // UniformBonusCount: count + nextInt(bonusMultiplier * level + 1)
                return count + rng.NextInt(fn.i0 * level + 1);
            case LootBonusFormula::BinomialWithBonusCount: {
                // BinomialWithBonusCount: n = level + extra trials at p
                int result = count;
                const int trials = level + fn.i1;
                for (int i = 0; i < trials; ++i) {
                    if (rng.NextFloat() < fn.a) ++result;
                }
                return result;
            }
            }
            return count;
        }

        void ApplyFunction(const LootFuncRow& fn, ItemStack& stack, const LootContext& ctx) {
            if (!TestConditions(fn.condBegin, fn.condCount, ctx)) return;

            switch (static_cast<LootFuncType>(fn.type)) {
            case LootFuncType::SetCount: {
                int value = 0;
                switch (static_cast<LootCountMode>(fn.mode)) {
                case LootCountMode::Constant:
                    value = static_cast<int>(fn.a);
                    break;
                case LootCountMode::Uniform:
                    // MC UniformGenerator.getInt — inclusive both ends.
                    value = ctx.rng->NextInt(static_cast<int>(fn.a), static_cast<int>(fn.b));
                    break;
                case LootCountMode::Binomial: {
                    // MC BinomialDistributionGenerator: n trials at p.
                    const int n = static_cast<int>(fn.a);
                    for (int i = 0; i < n; ++i) {
                        if (ctx.rng->NextFloat() < fn.b) ++value;
                    }
                    break;
                }
                }
                stack.count = fn.add ? stack.count + value : value;
                break;
            }

            case LootFuncType::ApplyBonus: {
                const int level = EnchantmentLevel(ctx.tool,
                                                   static_cast<LootEnchantment>(fn.ench));
                stack.count = ApplyBonusFormula(fn, stack.count, level, *ctx.rng);
                break;
            }

            case LootFuncType::LimitCount:
                stack.count = std::clamp(stack.count, fn.i0, fn.i1);
                break;

            case LootFuncType::ExplosionDecay:
                // MC ApplyExplosionDecay drops each item with probability
                // 1/radius. No explosions exist, so nothing decays.
                break;

            case LootFuncType::CopyComponents:
            case LootFuncType::CopyState:
                // Both copy block-entity data onto the dropped stack (chest
                // contents, shulker inventories, bee nest honey level). We do
                // not carry block-entity NBT on an ItemStack yet, so the item
                // drops without it — the block still drops, just empty.
                break;
            }
        }

        void ApplyFunctions(uint32_t begin, uint16_t count, ItemStack& stack,
                            const LootContext& ctx) {
            for (uint16_t i = 0; i < count; ++i) {
                ApplyFunction(kLootFunctions[begin + i], stack, ctx);
            }
        }

        // ── Entries ─────────────────────────────────────────────────────────
        // MC LootPoolEntryContainer.expand + LootPoolSingletonContainer
        // .createItemStack. Returns false when nothing was produced.
        bool ExpandEntry(uint32_t entryIndex, const LootContext& ctx,
                         std::vector<ItemStack>& out, int depth = 0) {
            if (depth > 8) return false;   // baked data nests 2 deep; this is a guard
            const LootEntryRow& entry = kLootEntries[entryIndex];
            if (!TestConditions(entry.condBegin, entry.condCount, ctx)) return false;

            switch (static_cast<LootEntryKind>(entry.kind)) {
            case LootEntryKind::Item: {
                const ItemID id = s_entryItem[entryIndex];
                if (id == Items::Air) return false;   // unresolved slug, already logged
                ItemStack stack(id, 1);
                ApplyFunctions(entry.fnBegin, entry.fnCount, stack, ctx);
                if (stack.count > 0) out.push_back(std::move(stack));
                return true;
            }
            case LootEntryKind::Alternatives:
                // MC AlternativesEntry: the FIRST child whose own conditions
                // pass wins. This is what makes "silk touch → the block itself,
                // otherwise → cobblestone" work.
                for (uint16_t i = 0; i < entry.childCount; ++i) {
                    if (ExpandEntry(entry.childBegin + i, ctx, out, depth + 1)) return true;
                }
                return false;
            case LootEntryKind::Empty:
                return true;   // EmptyLootItem "succeeds" and yields nothing
            }
            return false;
        }

        // MC LootTable.createStackSplitter (LootTable.java:56-73): a roll that
        // produced more than one stack's worth becomes several stacks.
        void SplitAndAppend(std::vector<ItemStack>& out, std::vector<ItemStack>&& rolled) {
            for (ItemStack& stack : rolled) {
                if (stack.count <= 0) continue;
                const int max = std::max(1, ItemRegistry::Get(stack.itemId).maxStackSize);
                if (stack.count <= max) {
                    out.push_back(std::move(stack));
                    continue;
                }
                int remaining = stack.count;
                while (remaining > 0) {
                    ItemStack piece = stack;
                    piece.count = std::min(max, remaining);
                    remaining -= piece.count;
                    out.push_back(std::move(piece));
                }
            }
        }

    } // namespace

    void LootTables::Initialize() {
        if (s_initialized) return;
        s_initialized = true;

        // Slug → table index. Built first so the BlockID pass below is a hash
        // lookup per block rather than a scan of 1084 rows.
        std::unordered_map<std::string_view, int16_t> bySlug;
        bySlug.reserve(kLootTableCount);
        for (size_t i = 0; i < kLootTableCount; ++i) {
            bySlug.emplace(kLootTables[i].blockSlug, static_cast<int16_t>(i));
        }

        s_tableForBlock.assign(static_cast<size_t>(BlockID::Count), -1);
        size_t matched = 0, viaWall = 0;
        for (int i = 1; i < static_cast<int>(BlockID::Count); ++i) {
            const auto& block = BlockRegistry::Get(static_cast<BlockID>(i));
            if (block.registrySlug.empty()) continue;
            auto it = bySlug.find(std::string_view(block.registrySlug));
            if (it == bySlug.end()) {
                // No table of its own — it may be a wall-mounted variant that
                // borrows the standing block's, as MC does.
                const std::string standing = StandingCounterpart(block.registrySlug);
                if (standing.empty()) continue;
                it = bySlug.find(std::string_view(standing));
                if (it == bySlug.end()) continue;
                ++viaWall;
            }
            s_tableForBlock[i] = it->second;
            ++matched;
        }

        // Resolve every slug once. RecipeManager owns the shared slug → ItemID
        // map (block items reuse their BlockID numerically), so borrow it
        // rather than building a second one that could disagree.
        size_t unresolvedItems = 0;
        s_entryItem.assign(kLootEntryCount, Items::Air);
        for (size_t i = 0; i < kLootEntryCount; ++i) {
            const LootEntryRow& entry = kLootEntries[i];
            if (static_cast<LootEntryKind>(entry.kind) != LootEntryKind::Item) continue;
            const ItemID id = RecipeManager::ItemFromSlug(entry.itemSlug);
            if (id == Items::Air) {
                ++unresolvedItems;   // an item we don't have — that entry drops nothing
                continue;
            }
            s_entryItem[i] = id;
        }

        // match_tool item sets and location_check target blocks share the arg
        // pool; resolving both is cheap and keeps GetDrops string-free.
        s_argItem.assign(kLootArgCount, Items::Air);
        s_argBlock.assign(kLootArgCount, BlockID::Air);
        for (size_t i = 0; i < kLootConditionCount; ++i) {
            const LootCondRow& cond = kLootConditions[i];
            if (static_cast<LootCondType>(cond.type) == LootCondType::MatchToolItems) {
                for (uint16_t k = 0; k < cond.argCount; ++k) {
                    s_argItem[cond.argBegin + k] =
                        RecipeManager::ItemFromSlug(kLootArgs[cond.argBegin + k]);
                }
            } else if (static_cast<LootCondType>(cond.type) == LootCondType::LocationCheck
                       && cond.argCount > 0) {
                const ItemID id = RecipeManager::ItemFromSlug(kLootArgs[cond.argBegin]);
                s_argBlock[cond.argBegin] = ItemRegistry::IsBlockItem(id)
                                          ? ItemRegistry::ToBlock(id) : BlockID::Air;
            }
        }

        Log::Info("[LootTables] %zu tables baked, %zu blocks matched (%zu of them "
                  "wall variants borrowing the standing block's table), "
                  "%zu item entries unresolved",
                  kLootTableCount, matched, viaWall, unresolvedItems);
    }

    bool LootTables::HasLootTable(BlockID block) {
        const size_t index = static_cast<size_t>(block);
        return index < s_tableForBlock.size() && s_tableForBlock[index] >= 0;
    }

    std::vector<ItemStack> LootTables::GetDrops(const LootContext& ctx) {
        std::vector<ItemStack> out;
        const size_t blockIndex = static_cast<size_t>(ctx.block);
        if (!ctx.rng || blockIndex >= s_tableForBlock.size()) return out;
        const int16_t tableIndex = s_tableForBlock[blockIndex];
        if (tableIndex < 0) return out;   // MC's noLootTable() — drops nothing

        const LootTableRow& table = kLootTables[tableIndex];
        std::vector<ItemStack> rolled;

        // MC LootTable.getRandomItemsRaw: every pool, in order.
        for (uint16_t p = 0; p < table.poolCount; ++p) {
            const LootPoolRow& pool = kLootPools[table.poolBegin + p];
            if (!TestConditions(pool.condBegin, pool.condCount, ctx)) continue;

            // LootPool.addRandomItems: `rolls` iterations. Vanilla block tables
            // hold exactly one entry per pool and never set `weight`, so MC's
            // weighted pick in addRandomItem collapses to expanding that entry
            // (the generator rejects any table that would break this).
            for (uint8_t roll = 0; roll < pool.rolls; ++roll) {
                for (uint16_t e = 0; e < pool.entryCount; ++e) {
                    const size_t before = rolled.size();
                    ExpandEntry(pool.entryBegin + e, ctx, rolled);
                    // Pool-level functions decorate whatever this roll produced.
                    for (size_t s = before; s < rolled.size(); ++s) {
                        ApplyFunctions(pool.fnBegin, pool.fnCount, rolled[s], ctx);
                    }
                }
            }
        }

        // Table-level functions run over everything (MC decorates the consumer
        // with compositeFunction before any pool is visited; the ordering that
        // matters — entry, then pool, then table — is preserved).
        for (ItemStack& stack : rolled) {
            ApplyFunctions(table.fnBegin, table.fnCount, stack, ctx);
        }

        SplitAndAppend(out, std::move(rolled));
        return out;
    }

} // namespace Game
