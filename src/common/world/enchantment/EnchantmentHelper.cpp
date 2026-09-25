// File: src/common/world/enchantment/EnchantmentHelper.cpp
#include "EnchantmentHelper.hpp"
#include "EnchantmentDefinitions.hpp"

#include "../../entity/Item.hpp"
#include "../../entity/GeneratedItemList.hpp" // Game::Items::EnchantedBook
#include "../../data/DataComponents.hpp"
#include "common/core/JavaRandom.hpp"
#include "common/entity/Entity.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/Inventory.hpp"
#include "common/entity/LivingEntity.hpp"
#include "common/entity/effect/MobEffects.hpp"
#include "common/world/damagesource/DamageSourceInfo.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>

namespace Game::EnchantmentHelper {

    namespace {
        // MC getComponentType: STORED_ENCHANTMENTS on an enchanted book,
        // ENCHANTMENTS on everything else.
        const DataComponentType<ItemEnchantments>& ComponentType(const ItemStack& stack) {
            return stack.itemId == Items::EnchantedBook ? DataComponents::STORED_ENCHANTMENTS
                                                        : DataComponents::ENCHANTMENTS;
        }
    }

    void SetEnchantments(ItemStack& stack, ItemEnchantments enchantments) {
        // Mirrors EnchantmentHelper.setEnchantments. PatchedDataComponentMap
        // drops a value equal to the prototype's: every item's default is
        // EMPTY (COMMON_ITEM_COMPONENTS, or the enchanted book's own
        // STORED_ENCHANTMENTS EMPTY), so an emptied map is no override at all
        // — which is what lets a grindstoned sword stack with a fresh one.
        const auto& type = ComponentType(stack);
        if (enchantments.IsEmpty()) {
            const auto prototype = ItemRegistry::Get(stack.itemId).defaultComponents.get(type);
            if (!prototype || prototype->IsEmpty()) {
                stack.components.remove(type);
                return;
            }
        }
        stack.components.set(type, std::move(enchantments));
    }

    bool CanStoreEnchantments(const ItemStack& stack) {
        if (stack.IsEmpty()) return false;
        // ENCHANTMENTS is a common component of every MC item; the enchanted
        // book's STORED_ENCHANTMENTS comes from its Items.java default.
        if (stack.itemId == Items::EnchantedBook) {
            return stack.get(DataComponents::STORED_ENCHANTMENTS).has_value();
        }
        return true;
    }

    ItemEnchantments GetEnchantmentsForCrafting(const ItemStack& stack) {
        return stack.get(ComponentType(stack)).value_or(ItemEnchantments::EMPTY);
    }

    int GetItemEnchantmentLevel(EnchantmentId id, const ItemStack& stack) {
        if (stack.IsEmpty()) return 0;
        const auto enchantments = stack.get(DataComponents::ENCHANTMENTS);
        return enchantments ? enchantments->GetLevel(id) : 0;
    }

    bool HasAnyEnchantments(const ItemStack& stack) {
        const auto enchantments = stack.get(DataComponents::ENCHANTMENTS);
        if (enchantments && !enchantments->IsEmpty()) return true;
        const auto stored = stack.get(DataComponents::STORED_ENCHANTMENTS);
        return stored && !stored->IsEmpty();
    }

    void UpdateEnchantments(ItemStack& stack, const std::function<void(ItemEnchantments&)>& edit) {
        // MC updateEnchantments: `if (oldEnchantments == null) return EMPTY`
        // — only a stack that can hold the component is edited.
        if (!CanStoreEnchantments(stack)) return;
        ItemEnchantments enchantments = GetEnchantmentsForCrafting(stack);
        edit(enchantments);
        SetEnchantments(stack, std::move(enchantments));
    }

    ItemStack CreateBook(EnchantmentInstance inst) {
        // Mirrors EnchantmentHelper.createBook.
        ItemStack stack(Items::EnchantedBook, 1);
        ItemEnchantments enchantments;
        enchantments.Set(inst.id, inst.level);
        SetEnchantments(stack, std::move(enchantments));
        return stack;
    }

    void Enchant(ItemStack& stack, EnchantmentId id, int level) {
        // MC ItemStack.enchant → updateEnchantments(e -> e.upgrade(id, level)).
        UpdateEnchantments(stack, [&](ItemEnchantments& e) { e.Upgrade(id, level); });
    }

    int Enchantability(const ItemStack& stack) {
        if (stack.IsEmpty()) return 0;
        return stack.get(DataComponents::ENCHANTABLE).value_or(0);
    }

    namespace {

        struct WeightedInstance {
            EnchantmentInstance inst;
            int weight;
        };

        // MC EnchantmentHelper.getAvailableEnchantmentResults.
        std::vector<WeightedInstance> AvailableResults(int value, const ItemStack& stack,
                                                       const std::vector<EnchantmentId>& source) {
            std::vector<WeightedInstance> results;
            const bool isBook = stack.itemId == Items::Book;
            for (const EnchantmentId id : source) {
                if (!(EnchantmentDefinitions::IsPrimaryItem(id, stack.itemId) || isBook)) continue;
                const auto& d = EnchantmentDefinitions::Get(id);
                if (!d.loaded) continue;
                for (int level = d.maxLevel; level >= 1; --level) {
                    if (value >= d.minCost.Calculate(level) && value <= d.maxCost.Calculate(level)) {
                        results.push_back({{id, level}, d.weight});
                        break;
                    }
                }
            }
            return results;
        }

        // MC WeightedRandom.getRandomItem(random, items, weightGetter).
        bool PickWeighted(JavaRandom& random, const std::vector<WeightedInstance>& items,
                          EnchantmentInstance& out) {
            int total = 0;
            for (const auto& w : items) total += w.weight;
            if (total <= 0) return false;
            int i = random.NextInt(total);
            for (const auto& w : items) {
                i -= w.weight;
                if (i < 0) { out = w.inst; return true; }
            }
            return false;
        }

    } // namespace

    std::vector<EnchantmentInstance> SelectEnchantment(JavaRandom& random, const ItemStack& stack,
                                                       int enchantmentCost,
                                                       const std::vector<EnchantmentId>& candidates) {
        std::vector<EnchantmentInstance> results;
        const int enchantable = Enchantability(stack);
        if (enchantable <= 0) return results;              // MC: no ENCHANTABLE component

        enchantmentCost += 1 + random.NextInt(enchantable / 4 + 1) + random.NextInt(enchantable / 4 + 1);
        const float randomSpan = (random.NextFloat() + random.NextFloat() - 1.0f) * 0.15f;
        // Math.round(float): floor(x + 0.5), clamped to at least 1.
        enchantmentCost = std::max(1, static_cast<int>(std::floor(
            static_cast<float>(enchantmentCost) + static_cast<float>(enchantmentCost) * randomSpan + 0.5f)));

        std::vector<WeightedInstance> available = AvailableResults(enchantmentCost, stack, candidates);
        if (available.empty()) return results;

        EnchantmentInstance picked{};
        if (PickWeighted(random, available, picked)) results.push_back(picked);

        while (random.NextInt(50) <= enchantmentCost) {
            if (!results.empty()) {
                // filterCompatibleEnchantments against the last pick.
                const EnchantmentId last = results.back().id;
                available.erase(std::remove_if(available.begin(), available.end(),
                                               [&](const WeightedInstance& w) {
                                                   return !EnchantmentDefinitions::AreCompatible(last, w.inst.id);
                                               }),
                                available.end());
            }
            if (available.empty()) break;
            if (PickWeighted(random, available, picked)) results.push_back(picked);
            enchantmentCost /= 2;
        }
        return results;
    }

    ItemStack EnchantItem(JavaRandom& random, ItemStack stack, int enchantmentCost,
                          const std::vector<EnchantmentId>& candidates) {
        const std::vector<EnchantmentInstance> enchants =
            SelectEnchantment(random, stack, enchantmentCost, candidates);
        if (stack.itemId == Items::Book) {
            stack = ItemStack(Items::EnchantedBook, 1);   // new ItemStack(ENCHANTED_BOOK)
        }
        for (const EnchantmentInstance& e : enchants) Enchant(stack, e.id, e.level);
        return stack;
    }

    // ── Effect runners ─────────────────────────────────────────────────────

    namespace {

        using Definition = EnchantmentDefinitions::Definition;

        // EquipmentSlot.VALUES, in declaration order — the order MC's
        // runIterationOnEquipment and getRandomItemWith walk.
        constexpr EquipmentSlot kEquipmentSlots[] = {
            EquipmentSlot::MAINHAND, EquipmentSlot::OFFHAND, EquipmentSlot::FEET, EquipmentSlot::LEGS,
            EquipmentSlot::CHEST, EquipmentSlot::HEAD, EquipmentSlot::BODY, EquipmentSlot::SADDLE,
        };

        // The item's ENCHANTMENTS (never STORED), copied so an effect that
        // wears or breaks the stack cannot pull the list out from under the
        // walk.
        std::vector<EnchantmentInstance> EnchantmentsOf(const ItemStack& item) {
            if (item.IsEmpty()) return {};
            const auto enchantments = item.get(DataComponents::ENCHANTMENTS);
            return enchantments ? enchantments->entries : std::vector<EnchantmentInstance>{};
        }

        // MC runIterationOnItem(piece, visitor).
        template <class Fn>
        void ForEachOnItem(const ItemStack& item, Fn&& fn) {
            for (const EnchantmentInstance& e : EnchantmentsOf(item)) {
                const Definition& d = EnchantmentDefinitions::Get(e.id);
                if (d.loaded) fn(e.id, e.level, d);
            }
        }

        // MC runIterationOnItem(piece, slot, owner, visitor): only the
        // enchantments that work in `slot`, each with its EnchantedItemInUse.
        template <class Fn>
        void ForEachInSlot(ItemStack* item, EquipmentSlot slot, LivingEntity* owner, Fn&& fn) {
            if (!item || item->IsEmpty()) return;
            for (const EnchantmentInstance& e : EnchantmentsOf(*item)) {
                const Definition& d = EnchantmentDefinitions::Get(e.id);
                if (!d.loaded || !EnchantmentDefinitions::MatchingSlot(e.id, slot)) continue;
                EnchantedItemInUse use;
                use.stack = item;
                use.slot  = slot;
                use.owner = owner;
                fn(e.id, e.level, d, use);
            }
        }

        // MC runIterationOnEquipment.
        template <class Fn>
        void ForEachOnEquipment(const EnchantmentEquipment& equipment, Fn&& fn) {
            if (!equipment.itemBySlot) return;
            for (const EquipmentSlot slot : kEquipmentSlots) {
                ForEachInSlot(equipment.itemBySlot(slot), slot, equipment.owner, fn);
            }
        }

        // Enchantment.itemContext.
        EnchantmentContext ItemContext(JavaRandom& random, int level, const ItemStack& tool) {
            EnchantmentContext ctx;
            ctx.random = &random;
            ctx.enchantmentLevel = level;
            ctx.tool = &tool;
            return ctx;
        }

        // Enchantment.damageContext.
        EnchantmentContext DamageContext(EntityLevel& level, int enchantmentLevel, Entity& victim,
                                         const DamageSourceInfo& source) {
            EnchantmentContext ctx;
            ctx.level = &level;
            ctx.blocks = level.Blocks();
            ctx.random = &level.Random();
            ctx.enchantmentLevel = enchantmentLevel;
            ctx.thisEntity = &victim;
            ctx.origin = victim.position;
            ctx.damage = &source;
            return ctx;
        }

        // Enchantment.entityContext.
        EnchantmentContext EntityContext(EntityLevel& level, int enchantmentLevel, Entity& entity,
                                         const glm::dvec3& position) {
            EnchantmentContext ctx;
            ctx.level = &level;
            ctx.blocks = level.Blocks();
            ctx.random = &level.Random();
            ctx.enchantmentLevel = enchantmentLevel;
            ctx.thisEntity = &entity;
            ctx.origin = position;
            return ctx;
        }

        // Enchantment.applyEffects over a value list.
        float ApplyValueEffects(const std::vector<ConditionalValueEffect>& effects,
                                const EnchantmentContext& ctx, JavaRandom& random, float value) {
            for (const ConditionalValueEffect& e : effects) {
                if (e.Matches(ctx)) value = e.effect.Process(ctx.enchantmentLevel, random, value);
            }
            return value;
        }

        // Enchantment.modifyDamageFilteredValue over one component of the
        // weapon's enchantments.
        float ModifyDamageFiltered(EntityLevel& level, const ItemStack& weapon, Entity& victim,
                                   const DamageSourceInfo& source, float value,
                                   std::vector<ConditionalValueEffect> EnchantmentEffectComponents::*list) {
            ForEachOnItem(weapon, [&](EnchantmentId, int enchantmentLevel, const Definition& d) {
                const EnchantmentContext ctx = DamageContext(level, enchantmentLevel, victim, source);
                value = ApplyValueEffects(d.effects.*list, ctx, level.Random(), value);
            });
            return value;
        }

        // Enchantment.modifyEntityFilteredValue over one component.
        float ModifyEntityFiltered(EntityLevel& level, const ItemStack& weapon, Entity& entity, float value,
                                   std::vector<ConditionalValueEffect> EnchantmentEffectComponents::*list) {
            ForEachOnItem(weapon, [&](EnchantmentId, int enchantmentLevel, const Definition& d) {
                const EnchantmentContext ctx = EntityContext(level, enchantmentLevel, entity, entity.position);
                value = ApplyValueEffects(d.effects.*list, ctx, level.Random(), value);
            });
            return value;
        }

        // Enchantment.modifyItemFilteredCount over one component, the
        // context's TOOL being `tool`.
        float ModifyItemFiltered(const ItemStack& enchanted, const ItemStack& tool, JavaRandom& random,
                                 float value,
                                 std::vector<ConditionalValueEffect> EnchantmentEffectComponents::*list) {
            ForEachOnItem(enchanted, [&](EnchantmentId, int enchantmentLevel, const Definition& d) {
                const EnchantmentContext ctx = ItemContext(random, enchantmentLevel, tool);
                value = ApplyValueEffects(d.effects.*list, ctx, random, value);
            });
            return value;
        }

        // Enchantment.doPostAttack(effect ...) for one enchantment on one side.
        void DoPostAttack(EntityLevel& level, int enchantmentLevel, const Definition& d,
                          const EnchantedItemInUse& item, EnchantmentTarget forTarget, Entity& victim,
                          const DamageSourceInfo& source) {
            for (const TargetedConditionalEntityEffect& effect : d.effects.postAttack) {
                if (effect.enchanted != forTarget) continue;
                if (!effect.Matches(DamageContext(level, enchantmentLevel, victim, source))) continue;
                Entity* target = nullptr;
                switch (effect.affected) {
                    case EnchantmentTarget::Attacker:       target = source.causing; break;
                    case EnchantmentTarget::DamagingEntity: target = source.direct;  break;
                    case EnchantmentTarget::Victim:         target = &victim;        break;
                }
                if (target && !target->IsRemoved()) {
                    effect.effect.Apply(level, enchantmentLevel, item, *target, target->position);
                }
            }
        }

        // THIS_ENTITY's facts for a location / tick runner.
        EnchantmentEntityFacts FactsFor(const EnchantmentEquipment& equipment) {
            if (equipment.facts) return *equipment.facts;
            if (equipment.owner) return EnchantmentEntityFacts::Of(*equipment.owner);
            return EnchantmentEntityFacts{};
        }

        ActiveLocationEnchantments::Entry* FindEntry(ActiveLocationEnchantments& state, EquipmentSlot slot,
                                                     EnchantmentId id) {
            for (auto& e : state.entries) {
                if (e.slot == slot && e.id == id) return &e;
            }
            return nullptr;
        }

        // Enchantment.runLocationChangedEffects for one item in one slot.
        void RunLocationChangedInSlot(EntityLevel* level, const IBlockAccess* blocks, JavaRandom& random,
                                      const EnchantmentEquipment& equipment,
                                      ActiveLocationEnchantments& state, EquipmentSlot slot,
                                      const EnchantmentEntityFacts& facts) {
            ItemStack* item = equipment.itemBySlot ? equipment.itemBySlot(slot) : nullptr;
            ForEachInSlot(item, slot, equipment.owner,
                          [&](EnchantmentId id, int enchantmentLevel, const Definition& d,
                              const EnchantedItemInUse& use) {
                const auto& effects = d.effects.locationChanged;
                if (effects.empty()) return;
                ActiveLocationEnchantments::Entry* entry = FindEntry(state, slot, id);
                for (size_t i = 0; i < effects.size() && i < 32; ++i) {
                    const uint32_t bit = 1u << i;
                    const bool wasActive = entry && (entry->activeMask & bit) != 0;
                    // Enchantment.locationContext: THIS_ENTITY, level,
                    // ORIGIN and ENCHANTMENT_ACTIVE = wasActive.
                    EnchantmentContext ctx;
                    ctx.level = level;
                    ctx.blocks = blocks;
                    ctx.random = &random;
                    ctx.enchantmentLevel = enchantmentLevel;
                    ctx.thisEntity = equipment.owner;
                    ctx.thisFacts = &facts;
                    ctx.origin = facts.position;
                    ctx.enchantmentActive = wasActive ? 1 : 0;
                    if (effects[i].Matches(ctx)) {
                        if (!wasActive) {
                            if (!entry) {
                                state.entries.push_back({slot, id, enchantmentLevel, 0u});
                                entry = &state.entries.back();
                            }
                            entry->activeMask |= bit;
                        }
                        effects[i].effect.OnChangedBlock(level, enchantmentLevel, use, equipment.owner,
                                                         equipment.attributes, facts.position, !wasActive);
                    } else if (entry && (entry->activeMask & bit) != 0) {
                        entry->activeMask &= ~bit;
                        effects[i].effect.OnDeactivated(use, equipment.attributes, entry->level);
                    }
                }
                if (entry && entry->activeMask == 0) {
                    state.entries.erase(state.entries.begin() + (entry - state.entries.data()));
                }
            });
        }

    } // namespace

    bool HasPreventEquipmentDrop(const ItemStack& item) {
        bool found = false;
        ForEachOnItem(item, [&](EnchantmentId, int, const Definition& d) {
            if (d.effects.preventEquipmentDrop) found = true;
        });
        return found;
    }

    bool HasPreventArmorChange(const ItemStack& item) {
        bool found = false;
        ForEachOnItem(item, [&](EnchantmentId, int, const Definition& d) {
            if (d.effects.preventArmorChange) found = true;
        });
        return found;
    }

    int GetEnchantmentLevel(EnchantmentId id, const EnchantmentEquipment& equipment) {
        // Enchantment.getSlotItems: the items in the slots it works in.
        int best = 0;
        if (!equipment.itemBySlot) return 0;
        for (const EquipmentSlot slot : kEquipmentSlots) {
            if (!EnchantmentDefinitions::MatchingSlot(id, slot)) continue;
            const ItemStack* item = equipment.itemBySlot(slot);
            if (!item) continue;
            best = std::max(best, GetItemEnchantmentLevel(id, *item));
        }
        return best;
    }

    int ProcessBlockExperience(const ItemStack& tool, int amount, JavaRandom& random) {
        return static_cast<int>(ModifyItemFiltered(tool, tool, random, static_cast<float>(amount),
                                                   &EnchantmentEffectComponents::blockExperience));
    }

    int ProcessMobExperience(EntityLevel& level, Entity* killer, Entity& killed, int amount) {
        LivingEntity* living = killer ? killer->AsLiving() : nullptr;
        if (!living) return amount;
        float value = static_cast<float>(amount);
        ForEachOnEquipment(EnchantmentEquipment::Of(*living),
                           [&](EnchantmentId, int enchantmentLevel, const Definition& d, const EnchantedItemInUse&) {
            // modifyMobExperience → modifyEntityFilteredValue(..., killed).
            const EnchantmentContext ctx = EntityContext(level, enchantmentLevel, killed, killed.position);
            value = ApplyValueEffects(d.effects.mobExperience, ctx, level.Random(), value);
        });
        return static_cast<int>(value);
    }

    int ProcessAmmoUse(EntityLevel& level, const ItemStack& weapon, const ItemStack& ammo, int amount) {
        return static_cast<int>(ModifyItemFiltered(weapon, ammo, level.Random(), static_cast<float>(amount),
                                                   &EnchantmentEffectComponents::ammoUse));
    }

    int ProcessProjectileCount(EntityLevel& level, const ItemStack& weapon, Entity& shooter, int count) {
        return std::max(0, static_cast<int>(ModifyEntityFiltered(level, weapon, shooter, static_cast<float>(count),
                                                                 &EnchantmentEffectComponents::projectileCount)));
    }

    float ProcessProjectileSpread(EntityLevel& level, const ItemStack& weapon, Entity& shooter, float angle) {
        return std::max(0.0f, ModifyEntityFiltered(level, weapon, shooter, angle,
                                                   &EnchantmentEffectComponents::projectileSpread));
    }

    int GetPiercingCount(EntityLevel& level, const ItemStack& weapon, const ItemStack& ammo) {
        return std::max(0, static_cast<int>(ModifyItemFiltered(weapon, ammo, level.Random(), 0.0f,
                                                               &EnchantmentEffectComponents::projectilePiercing)));
    }

    bool IsImmuneToDamage(EntityLevel& level, LivingEntity& victim, const DamageSourceInfo& source,
                          const EnchantmentEquipment& equipment) {
        bool immune = false;
        ForEachOnEquipment(equipment, [&](EnchantmentId, int enchantmentLevel, const Definition& d,
                                          const EnchantedItemInUse&) {
            if (immune) return;
            const EnchantmentContext ctx = DamageContext(level, enchantmentLevel, victim, source);
            for (const EnchantmentCondition& c : d.effects.damageImmunity) {
                if (c.Test(ctx)) { immune = true; return; }
            }
        });
        return immune;
    }

    float GetDamageProtection(EntityLevel& level, LivingEntity& victim, const DamageSourceInfo& source,
                              const EnchantmentEquipment& equipment) {
        float result = 0.0f;
        ForEachOnEquipment(equipment, [&](EnchantmentId, int enchantmentLevel, const Definition& d,
                                          const EnchantedItemInUse&) {
            const EnchantmentContext ctx = DamageContext(level, enchantmentLevel, victim, source);
            result = ApplyValueEffects(d.effects.damageProtection, ctx, level.Random(), result);
        });
        return result;
    }

    float ModifyDamage(EntityLevel& level, const ItemStack& weapon, Entity& victim,
                       const DamageSourceInfo& source, float damage) {
        return ModifyDamageFiltered(level, weapon, victim, source, damage, &EnchantmentEffectComponents::damage);
    }

    float ModifyFallBasedDamage(EntityLevel& level, const ItemStack& weapon, Entity& victim,
                                const DamageSourceInfo& source, float damage) {
        return ModifyDamageFiltered(level, weapon, victim, source, damage,
                                    &EnchantmentEffectComponents::smashDamagePerFallenBlock);
    }

    float ModifyArmorEffectiveness(EntityLevel& level, const ItemStack& weapon, Entity& victim,
                                   const DamageSourceInfo& source, float armorFraction) {
        return ModifyDamageFiltered(level, weapon, victim, source, armorFraction,
                                    &EnchantmentEffectComponents::armorEffectiveness);
    }

    float ModifyKnockback(EntityLevel& level, const ItemStack& weapon, Entity& victim,
                          const DamageSourceInfo& source, float knockback) {
        return ModifyDamageFiltered(level, weapon, victim, source, knockback,
                                    &EnchantmentEffectComponents::knockback);
    }

    void DoPostAttackEffects(EntityLevel& level, Entity& victim, const DamageSourceInfo& source) {
        // `attacker.getWeaponItem()` for a living causing entity, else none.
        ItemStack* weapon = nullptr;
        if (source.causing && source.causing->AsLiving()) weapon = source.causing->GetWeaponItem();
        DoPostAttackEffectsWithItemSource(level, victim, source, weapon);
    }

    void DoPostAttackEffectsWithItemSource(EntityLevel& level, Entity& victim,
                                           const DamageSourceInfo& source, ItemStack* weapon) {
        if (level.IsClientSide()) return;
        // The victim's own gear first: enchanted = VICTIM (Thorns).
        LivingEntity* livingVictim = victim.AsLiving();
        if (livingVictim && livingVictim->HasEquipmentSlots()) {
            ForEachOnEquipment(EnchantmentEquipment::Of(*livingVictim),
                               [&](EnchantmentId, int enchantmentLevel, const Definition& d,
                                   const EnchantedItemInUse& item) {
                DoPostAttack(level, enchantmentLevel, d, item, EnchantmentTarget::Victim, victim, source);
            });
        }
        // Then the weapon, when a living attacker wields it: enchanted =
        // ATTACKER, evaluated as if in the main hand. (MC's attacker-less
        // branch needs an onBreak the engine's callers never pass.)
        if (!weapon || weapon->IsEmpty()) return;
        LivingEntity* attacker = source.causing ? source.causing->AsLiving() : nullptr;
        if (!attacker) return;
        ForEachInSlot(weapon, EquipmentSlot::MAINHAND, attacker,
                      [&](EnchantmentId, int enchantmentLevel, const Definition& d, const EnchantedItemInUse& item) {
            DoPostAttack(level, enchantmentLevel, d, item, EnchantmentTarget::Attacker, victim, source);
        });
    }

    void OnProjectileSpawned(EntityLevel& level, ItemStack& weapon, Entity& projectile, LivingEntity* owner,
                             const std::function<void(const ItemStack&)>& onBreak) {
        if (level.IsClientSide()) return;
        EnchantedItemInUse item;
        item.stack = &weapon;
        item.owner = owner;
        item.onBreak = onBreak;
        ForEachOnItem(weapon, [&](EnchantmentId, int enchantmentLevel, const Definition& d) {
            const EnchantmentContext ctx = EntityContext(level, enchantmentLevel, projectile, projectile.position);
            for (const ConditionalEntityEffect& e : d.effects.projectileSpawned) {
                if (e.Matches(ctx)) e.effect.Apply(level, enchantmentLevel, item, projectile, projectile.position);
            }
        });
    }

    void OnHitBlock(EntityLevel& level, ItemStack& weapon, LivingEntity* owner, Entity& projectile,
                    const glm::dvec3& hitLocation, BlockState hitBlock,
                    const std::function<void(const ItemStack&)>& onBreak) {
        if (level.IsClientSide()) return;
        EnchantedItemInUse item;
        item.stack = &weapon;
        item.owner = owner;
        item.onBreak = onBreak;
        ForEachOnItem(weapon, [&](EnchantmentId, int enchantmentLevel, const Definition& d) {
            // Enchantment.blockHitContext: THIS_ENTITY, ORIGIN, BLOCK_STATE.
            EnchantmentContext ctx = EntityContext(level, enchantmentLevel, projectile, hitLocation);
            ctx.hasBlockState = true;
            ctx.blockState = hitBlock;
            for (const ConditionalEntityEffect& e : d.effects.hitBlock) {
                if (e.Matches(ctx)) e.effect.Apply(level, enchantmentLevel, item, projectile, hitLocation);
            }
        });
    }

    void TickEffects(EntityLevel& level, const EnchantmentEquipment& equipment) {
        if (level.IsClientSide() || !equipment.owner) return;
        LivingEntity& entity = *equipment.owner;
        ForEachOnEquipment(equipment, [&](EnchantmentId, int enchantmentLevel, const Definition& d,
                                          const EnchantedItemInUse& item) {
            if (d.effects.tick.empty()) return;
            const EnchantmentContext ctx = EntityContext(level, enchantmentLevel, entity, entity.position);
            for (const ConditionalEntityEffect& e : d.effects.tick) {
                if (e.Matches(ctx)) e.effect.Apply(level, enchantmentLevel, item, entity, entity.position);
            }
        });
    }

    void RunLocationChangedEffects(EntityLevel* level, const IBlockAccess* blocks, JavaRandom& random,
                                   const EnchantmentEquipment& equipment,
                                   ActiveLocationEnchantments& state) {
        const EnchantmentEntityFacts facts = FactsFor(equipment);
        for (const EquipmentSlot slot : kEquipmentSlots) {
            RunLocationChangedInSlot(level, blocks, random, equipment, state, slot, facts);
        }
    }

    void RunLocationChangedEffectsInSlot(EntityLevel* level, const IBlockAccess* blocks, JavaRandom& random,
                                         const EnchantmentEquipment& equipment,
                                         ActiveLocationEnchantments& state, EquipmentSlot slot) {
        RunLocationChangedInSlot(level, blocks, random, equipment, state, slot, FactsFor(equipment));
    }

    void StopLocationBasedEffectsInSlot(const EnchantmentEquipment& equipment,
                                        ActiveLocationEnchantments& state, EquipmentSlot slot) {
        // Enchantment.stopLocationBasedEffects: everything the slot had on
        // goes off, at the level it went on with — the item itself may
        // already be gone (swapped, broken, dropped).
        for (size_t i = 0; i < state.entries.size();) {
            const ActiveLocationEnchantments::Entry entry = state.entries[i];
            if (entry.slot != slot) { ++i; continue; }
            state.entries.erase(state.entries.begin() + static_cast<std::ptrdiff_t>(i));
            const Definition& d = EnchantmentDefinitions::Get(entry.id);
            EnchantedItemInUse use;
            use.stack = equipment.itemBySlot ? equipment.itemBySlot(slot) : nullptr;
            use.slot = slot;
            use.owner = equipment.owner;
            for (size_t e = 0; e < d.effects.locationChanged.size() && e < 32; ++e) {
                if (entry.activeMask & (1u << e)) {
                    d.effects.locationChanged[e].effect.OnDeactivated(use, equipment.attributes, entry.level);
                }
            }
        }
    }

    void StopLocationBasedEffects(const EnchantmentEquipment& equipment, ActiveLocationEnchantments& state) {
        for (const EquipmentSlot slot : kEquipmentSlots) StopLocationBasedEffectsInSlot(equipment, state, slot);
    }

    int ModifyDurabilityToRepairFromXp(const ItemStack& item, int durability, JavaRandom& random) {
        return std::max(0, static_cast<int>(ModifyItemFiltered(item, item, random, static_cast<float>(durability),
                                                               &EnchantmentEffectComponents::repairWithXp)));
    }

    ItemStack* GetRandomDamagedItemWithRepairWithXp(const EnchantmentEquipment& equipment, JavaRandom& random) {
        // getRandomItemWith(REPAIR_WITH_XP, source, ItemStack::isDamaged):
        // one candidate per (slot, enchantment carrying the component that
        // works in that slot), then Util.getRandomSafe.
        std::vector<ItemStack*> candidates;
        if (!equipment.itemBySlot) return nullptr;
        for (const EquipmentSlot slot : kEquipmentSlots) {
            ItemStack* item = equipment.itemBySlot(slot);
            if (!item || !IsDamaged(*item)) continue;
            for (const EnchantmentInstance& e : EnchantmentsOf(*item)) {
                const Definition& d = EnchantmentDefinitions::Get(e.id);
                if (d.loaded && !d.effects.repairWithXp.empty() && EnchantmentDefinitions::MatchingSlot(e.id, slot)) {
                    candidates.push_back(item);
                }
            }
        }
        if (candidates.empty()) return nullptr;
        return candidates[static_cast<size_t>(random.NextInt(static_cast<int>(candidates.size())))];
    }

    void ForEachModifier(const ItemStack& item, EquipmentSlot slot,
                         const std::function<void(Attribute, const AttributeModifier&)>& consumer) {
        ForEachOnItem(item, [&](EnchantmentId id, int enchantmentLevel, const Definition& d) {
            if (d.effects.attributes.empty() || !EnchantmentDefinitions::MatchingSlot(id, slot)) return;
            for (const EnchantmentAttributeEffect& a : d.effects.attributes) {
                consumer(a.attribute, a.GetModifier(enchantmentLevel, slot));
            }
        });
    }

    double PlayerAttributeValue(Attribute attribute, double base, Inventory& inventory,
                                const std::vector<MobEffectInstance>& effects,
                                const AttributeMap* locationModifiers) {
        AttributeInstance instance(attribute, base);
        // collectEquipmentChanges: every worn, unbroken item's modifiers.
        const EnchantmentEquipment equipment = EnchantmentEquipment::OfInventory(inventory);
        for (const EquipmentSlot slot : kEquipmentSlots) {
            const ItemStack* item = equipment.itemBySlot(slot);
            if (!item || item->IsEmpty() || IsBrokenItem(*item)) continue;
            ForEachModifier(*item, slot, [&](Attribute a, const AttributeModifier& m) {
                if (a == attribute) instance.AddModifier(m);
            });
        }
        if (locationModifiers) {
            if (const AttributeInstance* location = locationModifiers->Find(attribute)) {
                for (const AttributeModifier& m : location->Modifiers()) instance.AddModifier(m);
            }
        }
        AddEffectAttributeModifiers(instance, effects);
        return instance.GetValue();
    }

} // namespace Game::EnchantmentHelper

namespace Game {

    EnchantmentEquipment EnchantmentEquipment::Of(LivingEntity& living) {
        EnchantmentEquipment e;
        e.itemBySlot = [&living](EquipmentSlot slot) { return living.EquipmentInSlot(slot); };
        e.owner = &living;
        e.attributes = &living.Attributes();
        return e;
    }

    EnchantmentEquipment EnchantmentEquipment::OfInventory(Inventory& inventory) {
        EnchantmentEquipment e;
        e.itemBySlot = [&inventory](EquipmentSlot slot) -> ItemStack* {
            if (slot == EquipmentSlot::MAINHAND) {
                return &inventory.MutableSlot(Inventory::HotbarToIndex(inventory.GetSelectedSlot()));
            }
            const int index = InventoryIndexFor(slot);
            return index >= 0 ? &inventory.MutableSlot(index) : nullptr;
        };
        return e;
    }

} // namespace Game
