// File: src/server/advancements/CriteriaTriggers.cpp
//
// The trigger instances of MC's net.minecraft.advancements.triggers, one
// matcher per trigger over the criterion's data-pack conditions. Field names
// and match rules follow each TriggerInstance's CODEC and matches().
#include "CriteriaTriggers.hpp"
#include "AdvancementPredicates.hpp"
#include "PlayerAdvancements.hpp"
#include "ServerAdvancements.hpp"

#include "server/IntegratedServer.hpp"
#include "server/entity/ServerLevelBridge.hpp"
#include "server/level/ServerLevel.hpp"
#include "server/player/ServerPlayer.hpp"

#include "common/core/Log.hpp"
#include "common/entity/Entity.hpp"
#include "common/entity/IUsePlayer.hpp"
#include "common/entity/Item.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/damagesource/DamageSourceInfo.hpp"
#include "common/world/level/DimensionId.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <set>
#include <unordered_map>

namespace Server::Advancements {

    namespace {
        struct TriggerNameEntry { TriggerType type; const char* name; };
        constexpr TriggerNameEntry kTriggerNames[] = {
            {TriggerType::Impossible, "impossible"},
            {TriggerType::PlayerKilledEntity, "player_killed_entity"},
            {TriggerType::EntityKilledPlayer, "entity_killed_player"},
            {TriggerType::EnterBlock, "enter_block"},
            {TriggerType::InventoryChanged, "inventory_changed"},
            {TriggerType::RecipeUnlocked, "recipe_unlocked"},
            {TriggerType::PlayerHurtEntity, "player_hurt_entity"},
            {TriggerType::EntityHurtPlayer, "entity_hurt_player"},
            {TriggerType::EnchantedItem, "enchanted_item"},
            {TriggerType::FilledBucket, "filled_bucket"},
            {TriggerType::BrewedPotion, "brewed_potion"},
            {TriggerType::ConstructBeacon, "construct_beacon"},
            {TriggerType::UsedEnderEye, "used_ender_eye"},
            {TriggerType::SummonedEntity, "summoned_entity"},
            {TriggerType::BredAnimals, "bred_animals"},
            {TriggerType::Location, "location"},
            {TriggerType::SleptInBed, "slept_in_bed"},
            {TriggerType::CuredZombieVillager, "cured_zombie_villager"},
            {TriggerType::VillagerTrade, "villager_trade"},
            {TriggerType::ItemDurabilityChanged, "item_durability_changed"},
            {TriggerType::Levitation, "levitation"},
            {TriggerType::ChangedDimension, "changed_dimension"},
            {TriggerType::Tick, "tick"},
            {TriggerType::TameAnimal, "tame_animal"},
            {TriggerType::PlacedBlock, "placed_block"},
            {TriggerType::ConsumeItem, "consume_item"},
            {TriggerType::EffectsChanged, "effects_changed"},
            {TriggerType::UsedTotem, "used_totem"},
            {TriggerType::NetherTravel, "nether_travel"},
            {TriggerType::FishingRodHooked, "fishing_rod_hooked"},
            {TriggerType::ChanneledLightning, "channeled_lightning"},
            {TriggerType::ShotCrossbow, "shot_crossbow"},
            {TriggerType::SpearMobs, "spear_mobs"},
            {TriggerType::KilledByArrow, "killed_by_arrow"},
            {TriggerType::HeroOfTheVillage, "hero_of_the_village"},
            {TriggerType::VoluntaryExile, "voluntary_exile"},
            {TriggerType::SlideDownBlock, "slide_down_block"},
            {TriggerType::BeeNestDestroyed, "bee_nest_destroyed"},
            {TriggerType::TargetHit, "target_hit"},
            {TriggerType::ItemUsedOnBlock, "item_used_on_block"},
            {TriggerType::DefaultBlockUse, "default_block_use"},
            {TriggerType::AnyBlockUse, "any_block_use"},
            {TriggerType::PlayerGeneratesContainerLoot, "player_generates_container_loot"},
            {TriggerType::ThrownItemPickedUpByEntity, "thrown_item_picked_up_by_entity"},
            {TriggerType::ThrownItemPickedUpByPlayer, "thrown_item_picked_up_by_player"},
            {TriggerType::PlayerInteractedWithEntity, "player_interacted_with_entity"},
            {TriggerType::PlayerShearedEquipment, "player_sheared_equipment"},
            {TriggerType::StartedRiding, "started_riding"},
            {TriggerType::LightningStrike, "lightning_strike"},
            {TriggerType::UsingItem, "using_item"},
            {TriggerType::FallFromHeight, "fall_from_height"},
            {TriggerType::RideEntityInLava, "ride_entity_in_lava"},
            {TriggerType::KillMobNearSculkCatalyst, "kill_mob_near_sculk_catalyst"},
            {TriggerType::AllayDropItemOnBlock, "allay_drop_item_on_block"},
            {TriggerType::AvoidVibration, "avoid_vibration"},
            {TriggerType::RecipeCrafted, "recipe_crafted"},
            {TriggerType::CrafterRecipeCrafted, "crafter_recipe_crafted"},
            {TriggerType::FallAfterExplosion, "fall_after_explosion"},
        };
        static_assert(sizeof(kTriggerNames) / sizeof(kTriggerNames[0]) == static_cast<size_t>(TriggerType::Count),
                      "every trigger needs its registry name");
    } // namespace

    std::optional<TriggerType> TriggerByName(std::string_view id) {
        if (const size_t colon = id.find(':'); colon != std::string_view::npos) {
            if (id.substr(0, colon) != "minecraft") return std::nullopt;
            id.remove_prefix(colon + 1);
        }
        for (const TriggerNameEntry& e : kTriggerNames) {
            if (id == e.name) return e.type;
        }
        return std::nullopt;
    }

    const char* TriggerName(TriggerType type) {
        for (const TriggerNameEntry& e : kTriggerNames) {
            if (e.type == type) return e.name;
        }
        return "impossible";
    }

} // namespace Server::Advancements

namespace Server::CriteriaTriggers {

    namespace {

        using json = nlohmann::json;
        using Advancements::LootContext;
        using Advancements::TriggerType;

        const json& EmptyObject() {
            static const json kEmpty = json::object();
            return kEmpty;
        }

        const json* Field(const json& j, const char* key) {
            if (!j.is_object()) return nullptr;
            auto it = j.find(key);
            return it == j.end() || it->is_null() ? nullptr : &*it;
        }

        std::string WithNamespace(std::string id) {
            if (id.find(':') == std::string::npos) id = "minecraft:" + id;
            return id;
        }

        ServerLevel* LevelOfPlayer(const ServerPlayer& player) {
            return g_integratedServer ? g_integratedServer->GetLevel(Game::DimensionFromRaw(player.getDimensionId()))
                                      : nullptr;
        }

        // SimpleCriterionTrigger.trigger: every listening criterion whose
        // own conditions match (`match`) and whose `player` predicate passes
        // is collected, then awarded — awarding edits the listener list.
        template <class Match>
        void Fire(ServerPlayer& player, TriggerType type, const Match& match) {
            if (!Advancements::TriggersEnabled()) return;
            Advancements::PlayerAdvancements* advancements = Advancements::Get(player);
            if (!advancements) return;
            std::lock_guard<std::recursive_mutex> lock(advancements->Mutex());
            const auto& listeners = advancements->Listeners(type);
            if (listeners.empty()) return;
            const LootContext playerContext = Advancements::EntityContext(player, player.effectEntity());
            std::vector<Advancements::Listener> matched;
            for (const Advancements::Listener& listener : listeners) {
                const json& conditions = listener.criterion->conditions ? *listener.criterion->conditions : EmptyObject();
                if (!match(conditions)) continue;
                if (const json* predicate = Field(conditions, "player");
                    predicate && !Advancements::TestConditions(predicate, playerContext)) {
                    continue;
                }
                matched.push_back(listener);
            }
            for (const Advancements::Listener& listener : matched) {
                advancements->Award(*listener.advancement, listener.criterion->name);
            }
        }

        // A ContextAwarePredicate field against an entity's context.
        bool EntityField(const json& conditions, const char* key, ServerPlayer& player, Game::Entity* entity) {
            const json* predicate = Field(conditions, key);
            if (!predicate) return true;
            if (!entity) return false;
            return Advancements::TestConditions(predicate, Advancements::EntityContext(player, entity));
        }

        bool ItemField(const json& conditions, const char* key, const Game::ItemStack& stack) {
            const json* predicate = Field(conditions, key);
            return !predicate || Advancements::TestItem(*predicate, stack);
        }

        // An ItemUsedOnLocationTrigger context: ORIGIN the block's centre,
        // THIS_ENTITY the player, BLOCK_STATE and TOOL.
        LootContext LocationContext(ServerPlayer& player, const glm::ivec3& pos, const Game::ItemStack* tool) {
            LootContext ctx;
            ctx.level = LevelOfPlayer(player);
            ctx.origin = glm::dvec3(pos) + glm::dvec3(0.5);
            ctx.thisEntity = player.effectEntity();
            if (ctx.level && ctx.level->World()) ctx.blockState = ctx.level->World()->GetBlockState(pos.x, pos.y, pos.z);
            ctx.tool = tool;
            return ctx;
        }

        // A list of conditions each needing its own distinct victim
        // (KilledByArrow / ChanneledLightning `victims`).
        bool EachMatchesDistinct(const json* list, ServerPlayer& player, const std::vector<Game::Entity*>& victims) {
            if (!list || !list->is_array()) return true;
            std::vector<Game::Entity*> remaining = victims;
            for (const json& predicate : *list) {
                bool found = false;
                for (auto it = remaining.begin(); it != remaining.end(); ++it) {
                    if (Advancements::TestConditions(&predicate, Advancements::EntityContext(player, *it))) {
                        remaining.erase(it);
                        found = true;
                        break;
                    }
                }
                if (!found) return false;
            }
            return true;
        }

        // A block + state predicate pair (EnterBlock / SlideDownBlock).
        bool BlockAndState(const json& conditions, Game::BlockState state) {
            if (const json* block = Field(conditions, "block"); block && block->is_string()) {
                if (WithNamespace(block->get<std::string>()) != Advancements::BlockIdOf(state)) return false;
            }
            if (const json* props = Field(conditions, "state")) {
                if (!Advancements::TestStateProperties(*props, state)) return false;
            }
            return true;
        }

        // A HolderSet<Recipe> (the codec's `recipes`) or the older single
        // `recipe_id`.
        bool RecipeMatches(const json& conditions, const std::string& recipeId) {
            const std::string id = WithNamespace(recipeId);
            auto one = [&id](const json& e) { return e.is_string() && WithNamespace(e.get<std::string>()) == id; };
            if (const json* single = Field(conditions, "recipe_id")) return one(*single);
            if (const json* set = Field(conditions, "recipes")) {
                if (set->is_array()) {
                    for (const auto& e : *set) if (one(e)) return true;
                    return false;
                }
                return one(*set);
            }
            return false;
        }

        // Each ingredient predicate takes a distinct used ingredient.
        bool IngredientsMatch(const json& conditions, const std::vector<Game::ItemStack>& used) {
            const json* predicates = Field(conditions, "ingredients");
            if (!predicates || !predicates->is_array()) return true;
            std::vector<Game::ItemStack> remaining = used;
            for (const json& predicate : *predicates) {
                bool found = false;
                for (auto it = remaining.begin(); it != remaining.end(); ++it) {
                    if (Advancements::TestItem(predicate, *it)) {
                        remaining.erase(it);
                        found = true;
                        break;
                    }
                }
                if (!found) return false;
            }
            return true;
        }

        // DistanceTrigger.matches (NETHER_TRAVEL, FALL_FROM_HEIGHT,
        // RIDE_ENTITY_IN_LAVA): `start_position` at the entered position,
        // `distance` from it to the player.
        bool DistanceMatches(const json& conditions, ServerPlayer& player, const glm::dvec3& entered) {
            if (const json* start = Field(conditions, "start_position")) {
                if (!Advancements::TestLocation(*start, LevelOfPlayer(player), entered.x, entered.y, entered.z)) return false;
            }
            if (const json* distance = Field(conditions, "distance")) {
                if (!Advancements::TestDistance(*distance, entered, player.getPosition())) return false;
            }
            return true;
        }

        void PlayerTrigger(ServerPlayer& player, TriggerType type) {
            Fire(player, type, [](const json&) { return true; });
        }

    } // namespace

    // ── Player lookup ────────────────────────────────────────────────────

    ServerPlayer* PlayerOf(const Game::Entity* entity) { return Advancements::PlayerOf(entity); }

    ServerPlayer* PlayerOf(Game::IUsePlayer* user) {
        return user ? dynamic_cast<ServerPlayer*>(user) : nullptr;
    }

    // ── Kills and damage ─────────────────────────────────────────────────

    // KilledTrigger.matches: killing_blow, then entity.
    static void Killed(ServerPlayer& player, TriggerType type, Game::Entity& entity, const Game::DamageSourceInfo& source) {
        Fire(player, type, [&](const json& c) {
            if (const json* blow = Field(c, "killing_blow")) {
                if (!Advancements::TestDamageSource(*blow, player, source)) return false;
            }
            return EntityField(c, "entity", player, &entity);
        });
    }

    void PlayerKilledEntity(ServerPlayer& player, Game::Entity& victim, const Game::DamageSourceInfo& source) {
        Killed(player, TriggerType::PlayerKilledEntity, victim, source);
    }

    void EntityKilledPlayer(ServerPlayer& player, Game::Entity& killer, const Game::DamageSourceInfo& source) {
        Killed(player, TriggerType::EntityKilledPlayer, killer, source);
    }

    void KillMobNearSculkCatalyst(ServerPlayer& player, Game::Entity& victim, const Game::DamageSourceInfo& source) {
        Killed(player, TriggerType::KillMobNearSculkCatalyst, victim, source);
    }

    void KilledByArrow(ServerPlayer& player, const std::vector<Game::Entity*>& victims,
                       const Game::ItemStack* firedFromWeapon) {
        std::set<uint16_t> types;
        for (Game::Entity* v : victims) if (v) types.insert(static_cast<uint16_t>(v->GetType()));
        Fire(player, TriggerType::KilledByArrow, [&](const json& c) {
            if (const json* weapon = Field(c, "fired_from_weapon")) {
                if (!firedFromWeapon || !Advancements::TestItem(*weapon, *firedFromWeapon)) return false;
            }
            if (!EachMatchesDistinct(Field(c, "victims"), player, victims)) return false;
            return Advancements::TestBounds(Field(c, "unique_entity_types"), static_cast<double>(types.size()));
        });
    }

    void PlayerHurtEntity(ServerPlayer& player, Game::Entity& victim, const Game::DamageSourceInfo& source,
                          float dealt, float taken, bool blocked) {
        Fire(player, TriggerType::PlayerHurtEntity, [&](const json& c) {
            if (const json* damage = Field(c, "damage")) {
                if (!Advancements::TestDamage(*damage, player, source, dealt, taken, blocked)) return false;
            }
            return EntityField(c, "entity", player, &victim);
        });
    }

    void EntityHurtPlayer(ServerPlayer& player, const Game::DamageSourceInfo& source,
                          float dealt, float taken, bool blocked) {
        Fire(player, TriggerType::EntityHurtPlayer, [&](const json& c) {
            const json* damage = Field(c, "damage");
            return !damage || Advancements::TestDamage(*damage, player, source, dealt, taken, blocked);
        });
    }

    void UsedTotem(ServerPlayer& player, const Game::ItemStack& totem) {
        Fire(player, TriggerType::UsedTotem, [&](const json& c) { return ItemField(c, "item", totem); });
    }

    void ChanneledLightning(ServerPlayer& player, const std::vector<Game::Entity*>& victims) {
        Fire(player, TriggerType::ChanneledLightning, [&](const json& c) {
            return EachMatchesDistinct(Field(c, "victims"), player, victims);
        });
    }

    void LightningStrike(ServerPlayer& player, Game::Entity& bolt, const std::vector<Game::Entity*>& bystanders) {
        Fire(player, TriggerType::LightningStrike, [&](const json& c) {
            if (!EntityField(c, "lightning", player, &bolt)) return false;
            if (const json* bystander = Field(c, "bystander")) {
                bool any = false;
                for (Game::Entity* e : bystanders) {
                    if (Advancements::TestConditions(bystander, Advancements::EntityContext(player, e))) { any = true; break; }
                }
                if (!any) return false;
            }
            return true;
        });
    }

    void TargetHit(ServerPlayer& player, Game::Entity& projectile, const glm::dvec3& hitPos, int signalStrength) {
        (void)hitPos;
        Fire(player, TriggerType::TargetHit, [&](const json& c) {
            if (!Advancements::TestBounds(Field(c, "signal_strength"), signalStrength)) return false;
            return EntityField(c, "projectile", player, &projectile);
        });
    }

    void SpearMobs(ServerPlayer& player, int count) {
        Fire(player, TriggerType::SpearMobs, [&](const json& c) {
            const json* required = Field(c, "count");
            return !required || !required->is_number_integer() || count >= required->get<int>();
        });
    }

    void ShotCrossbow(ServerPlayer& player, const Game::ItemStack& crossbow) {
        Fire(player, TriggerType::ShotCrossbow, [&](const json& c) { return ItemField(c, "item", crossbow); });
    }

    // ── Plain player triggers ────────────────────────────────────────────

    void SleptInBed(ServerPlayer& player)       { PlayerTrigger(player, TriggerType::SleptInBed); }
    void AvoidVibration(ServerPlayer& player)   { PlayerTrigger(player, TriggerType::AvoidVibration); }
    void HeroOfTheVillage(ServerPlayer& player) { PlayerTrigger(player, TriggerType::HeroOfTheVillage); }
    void VoluntaryExile(ServerPlayer& player)   { PlayerTrigger(player, TriggerType::VoluntaryExile); }
    void StartedRiding(ServerPlayer& player)    { PlayerTrigger(player, TriggerType::StartedRiding); }
    void Tick(ServerPlayer& player)             { PlayerTrigger(player, TriggerType::Tick); }
    void Location(ServerPlayer& player)         { PlayerTrigger(player, TriggerType::Location); }

    // ── Items ────────────────────────────────────────────────────────────

    void InventoryChanged(ServerPlayer& player, const Game::ItemStack& changed) {
        // InventoryChangeTrigger.trigger: the slot counts over the whole
        // inventory, then matches (slots, items).
        const Game::Inventory& inventory = player.getInventory();
        int full = 0, empty = 0, occupied = 0;
        for (int slot = Game::Inventory::ARMOR_BEGIN; slot < Game::Inventory::TOTAL_SIZE; ++slot) {
            const Game::ItemStack& stack = inventory.GetSlot(slot);
            if (stack.IsEmpty()) {
                ++empty;
            } else {
                ++occupied;
                if (stack.count >= Game::GetMaxStackSize(stack)) ++full;
            }
        }
        Fire(player, TriggerType::InventoryChanged, [&](const json& c) {
            if (const json* slots = Field(c, "slots")) {
                if (!Advancements::TestBounds(Field(*slots, "occupied"), occupied) ||
                    !Advancements::TestBounds(Field(*slots, "full"), full) ||
                    !Advancements::TestBounds(Field(*slots, "empty"), empty)) return false;
            }
            const json* items = Field(c, "items");
            if (!items || !items->is_array() || items->empty()) return true;
            if (items->size() == 1) {
                return !changed.IsEmpty() && Advancements::TestItem((*items)[0], changed);
            }
            // Every predicate must find a stack somewhere in the inventory.
            std::vector<const json*> remaining;
            for (const json& p : *items) remaining.push_back(&p);
            for (int slot = Game::Inventory::ARMOR_BEGIN; slot < Game::Inventory::TOTAL_SIZE && !remaining.empty(); ++slot) {
                const Game::ItemStack& stack = inventory.GetSlot(slot);
                if (stack.IsEmpty()) continue;
                remaining.erase(std::remove_if(remaining.begin(), remaining.end(),
                                               [&stack](const json* p) { return Advancements::TestItem(*p, stack); }),
                                remaining.end());
            }
            return remaining.empty();
        });
    }

    void ConsumeItem(ServerPlayer& player, const Game::ItemStack& item) {
        Fire(player, TriggerType::ConsumeItem, [&](const json& c) { return ItemField(c, "item", item); });
    }

    void FilledBucket(ServerPlayer& player, const Game::ItemStack& bucket) {
        Fire(player, TriggerType::FilledBucket, [&](const json& c) { return ItemField(c, "item", bucket); });
    }

    void UsingItem(ServerPlayer& player, const Game::ItemStack& item) {
        Fire(player, TriggerType::UsingItem, [&](const json& c) { return ItemField(c, "item", item); });
    }

    void EnchantedItem(ServerPlayer& player, const Game::ItemStack& item, int levels) {
        Fire(player, TriggerType::EnchantedItem, [&](const json& c) {
            return ItemField(c, "item", item) && Advancements::TestBounds(Field(c, "levels"), levels);
        });
    }

    void BrewedPotion(ServerPlayer& player, const std::string& potion) {
        Fire(player, TriggerType::BrewedPotion, [&](const json& c) {
            const json* want = Field(c, "potion");
            return !want || (want->is_string() && !potion.empty() &&
                             WithNamespace(want->get<std::string>()) == WithNamespace(potion));
        });
    }

    void VillagerTrade(ServerPlayer& player, Game::Entity& villager, const Game::ItemStack& result) {
        Fire(player, TriggerType::VillagerTrade, [&](const json& c) {
            return EntityField(c, "villager", player, &villager) && ItemField(c, "item", result);
        });
    }

    void RecipeUnlocked(ServerPlayer& player, const std::string& recipeId) {
        Fire(player, TriggerType::RecipeUnlocked, [&](const json& c) {
            const json* recipe = Field(c, "recipe");
            return recipe && recipe->is_string() && WithNamespace(recipe->get<std::string>()) == WithNamespace(recipeId);
        });
    }

    void RecipeCrafted(ServerPlayer& player, const std::string& recipeId, const std::vector<Game::ItemStack>& ingredients) {
        Fire(player, TriggerType::RecipeCrafted, [&](const json& c) {
            return RecipeMatches(c, recipeId) && IngredientsMatch(c, ingredients);
        });
    }

    void CrafterRecipeCrafted(ServerPlayer& player, const std::string& recipeId,
                              const std::vector<Game::ItemStack>& ingredients) {
        Fire(player, TriggerType::CrafterRecipeCrafted, [&](const json& c) {
            return RecipeMatches(c, recipeId) && IngredientsMatch(c, ingredients);
        });
    }

    void FishingRodHooked(ServerPlayer& player, const Game::ItemStack& rod, Game::Entity& hook,
                          const std::vector<Game::ItemStack>& items) {
        // The hooked-in entity context is the hook's (an item entity is not
        // an Entity here, so `entity` sees the hook and `item` the catch).
        Fire(player, TriggerType::FishingRodHooked, [&](const json& c) {
            if (!ItemField(c, "rod", rod)) return false;
            if (!EntityField(c, "entity", player, &hook)) return false;
            if (const json* item = Field(c, "item")) {
                bool matched = false;
                for (const Game::ItemStack& stack : items) {
                    if (Advancements::TestItem(*item, stack)) { matched = true; break; }
                }
                if (!matched) return false;
            }
            return true;
        });
    }

    void ItemDurabilityChanged(ServerPlayer& player, const Game::ItemStack& item, int newDurability) {
        // `item` is the stack before the change; newDurability its new
        // DAMAGE value, as MC passes them.
        Fire(player, TriggerType::ItemDurabilityChanged, [&](const json& c) {
            if (!ItemField(c, "item", item)) return false;
            if (!Advancements::TestBounds(Field(c, "durability"), Game::GetMaxDamage(item) - newDurability)) return false;
            return Advancements::TestBounds(Field(c, "delta"), Game::GetDamageValue(item) - newDurability);
        });
    }

    void UsedEnderEye(ServerPlayer& player, const glm::ivec3& feature) {
        const glm::dvec3 pos = player.getPosition();
        const double xd = pos.x - feature.x, zd = pos.z - feature.z;
        const double distance = std::sqrt(xd * xd + zd * zd);   // MinMaxBounds.matchesSqr
        Fire(player, TriggerType::UsedEnderEye, [&](const json& c) {
            return Advancements::TestBounds(Field(c, "distance"), distance);
        });
    }

    void GenerateContainerLoot(ServerPlayer& player, const std::string& lootTable) {
        if (lootTable.empty()) return;
        const std::string id = WithNamespace(lootTable);
        Fire(player, TriggerType::PlayerGeneratesContainerLoot, [&](const json& c) {
            const json* table = Field(c, "loot_table");
            return table && table->is_string() && WithNamespace(table->get<std::string>()) == id;
        });
    }

    // ── Entities ─────────────────────────────────────────────────────────

    void PlayerInteractedWithEntity(ServerPlayer& player, const Game::ItemStack& item, Game::Entity& target) {
        Fire(player, TriggerType::PlayerInteractedWithEntity, [&](const json& c) {
            return ItemField(c, "item", item) && EntityField(c, "entity", player, &target);
        });
    }

    void PlayerShearedEquipment(ServerPlayer& player, const Game::ItemStack& item, Game::Entity& target) {
        Fire(player, TriggerType::PlayerShearedEquipment, [&](const json& c) {
            return ItemField(c, "item", item) && EntityField(c, "entity", player, &target);
        });
    }

    void ThrownItemPickedUpByPlayer(ServerPlayer& player, const Game::ItemStack& item, Game::Entity* thrower) {
        Fire(player, TriggerType::ThrownItemPickedUpByPlayer, [&](const json& c) {
            return ItemField(c, "item", item) && EntityField(c, "entity", player, thrower);
        });
    }

    void ThrownItemPickedUpByEntity(ServerPlayer& thrower, const Game::ItemStack& item, Game::Entity& pickedUpBy) {
        Fire(thrower, TriggerType::ThrownItemPickedUpByEntity, [&](const json& c) {
            return ItemField(c, "item", item) && EntityField(c, "entity", thrower, &pickedUpBy);
        });
    }

    void BredAnimals(ServerPlayer& player, Game::Entity& parent, Game::Entity& partner, Game::Entity* child) {
        Fire(player, TriggerType::BredAnimals, [&](const json& c) {
            if (Field(c, "child") && !EntityField(c, "child", player, child)) return false;
            const bool straight = EntityField(c, "parent", player, &parent) && EntityField(c, "partner", player, &partner);
            const bool swapped  = EntityField(c, "parent", player, &partner) && EntityField(c, "partner", player, &parent);
            return straight || swapped;
        });
    }

    void TameAnimal(ServerPlayer& player, Game::Entity& animal) {
        Fire(player, TriggerType::TameAnimal, [&](const json& c) { return EntityField(c, "entity", player, &animal); });
    }

    void SummonedEntity(ServerPlayer& player, Game::Entity& entity) {
        Fire(player, TriggerType::SummonedEntity, [&](const json& c) { return EntityField(c, "entity", player, &entity); });
    }

    void CuredZombieVillager(ServerPlayer& player, Game::Entity& zombie, Game::Entity& villager) {
        Fire(player, TriggerType::CuredZombieVillager, [&](const json& c) {
            return EntityField(c, "zombie", player, &zombie) && EntityField(c, "villager", player, &villager);
        });
    }

    // ── Blocks ───────────────────────────────────────────────────────────

    static void ItemUsedOnLocation(ServerPlayer& player, TriggerType type, const glm::ivec3& pos,
                                   const Game::ItemStack& tool) {
        const LootContext ctx = LocationContext(player, pos, &tool);
        Fire(player, type, [&](const json& c) {
            const json* location = Field(c, "location");
            return !location || Advancements::TestConditions(location, ctx);
        });
    }

    void PlacedBlock(ServerPlayer& player, const glm::ivec3& pos, const Game::ItemStack& tool) {
        ItemUsedOnLocation(player, TriggerType::PlacedBlock, pos, tool);
    }

    void ItemUsedOnBlock(ServerPlayer& player, const glm::ivec3& pos, const Game::ItemStack& tool) {
        ItemUsedOnLocation(player, TriggerType::ItemUsedOnBlock, pos, tool);
    }

    void AllayDropItemOnBlock(ServerPlayer& player, const glm::ivec3& pos, const Game::ItemStack& item) {
        ItemUsedOnLocation(player, TriggerType::AllayDropItemOnBlock, pos, item);
    }

    void DefaultBlockUse(ServerPlayer& player, const glm::ivec3& pos) {
        // DefaultBlockInteractionTrigger: ORIGIN, THIS_ENTITY, BLOCK_STATE.
        const LootContext ctx = LocationContext(player, pos, nullptr);
        Fire(player, TriggerType::DefaultBlockUse, [&](const json& c) {
            const json* location = Field(c, "location");
            return !location || Advancements::TestConditions(location, ctx);
        });
    }

    void AnyBlockUse(ServerPlayer& player, const glm::ivec3& pos, const Game::ItemStack& tool) {
        ItemUsedOnLocation(player, TriggerType::AnyBlockUse, pos, tool);
    }

    void BeeNestDestroyed(ServerPlayer& player, Game::BlockState state, const Game::ItemStack& tool, int beesInside) {
        Fire(player, TriggerType::BeeNestDestroyed, [&](const json& c) {
            if (const json* block = Field(c, "block"); block && block->is_string()) {
                if (WithNamespace(block->get<std::string>()) != Advancements::BlockIdOf(state)) return false;
            }
            if (!ItemField(c, "item", tool)) return false;
            return Advancements::TestBounds(Field(c, "num_bees_inside"), beesInside);
        });
    }

    void ConstructBeacon(ServerPlayer& player, int levels) {
        Fire(player, TriggerType::ConstructBeacon, [&](const json& c) {
            return Advancements::TestBounds(Field(c, "level"), levels);
        });
    }

    void SlideDownBlock(ServerPlayer& player, Game::BlockState state) {
        Fire(player, TriggerType::SlideDownBlock, [&](const json& c) { return BlockAndState(c, state); });
    }

    void EnterBlock(ServerPlayer& player, Game::BlockState state) {
        Fire(player, TriggerType::EnterBlock, [&](const json& c) { return BlockAndState(c, state); });
    }

    // ── Player state ─────────────────────────────────────────────────────

    void EffectsChanged(ServerPlayer& player, Game::Entity* source) {
        Fire(player, TriggerType::EffectsChanged, [&](const json& c) {
            if (const json* effects = Field(c, "effects")) {
                // MobEffectsPredicate.matches(player) — the effects predicate
                // shape is shared with EntityPredicate's `effects`.
                const json wrapped = {{"effects", *effects}};
                if (!Advancements::TestEntity(wrapped, LevelOfPlayer(player), player.getPosition(),
                                              player.effectEntity())) return false;
            }
            if (Field(c, "source")) return EntityField(c, "source", player, source);
            return true;
        });
    }

    void LevitationStarted(ServerPlayer& player) {
        Advancements::PlayerAdvancements* advancements = Advancements::Get(player);
        if (!advancements) return;
        std::lock_guard<std::recursive_mutex> lock(advancements->Mutex());
        Advancements::TrackedState& tracked = advancements->Tracked();
        tracked.levitationStartTime = tracked.tickCount;
        tracked.levitationStartPos = player.getPosition();
    }

    void LevitationStopped(ServerPlayer& player) {
        Advancements::PlayerAdvancements* advancements = Advancements::Get(player);
        if (!advancements) return;
        std::lock_guard<std::recursive_mutex> lock(advancements->Mutex());
        advancements->Tracked().levitationStartPos.reset();
    }

    void Levitation(ServerPlayer& player, const glm::dvec3& start, int duration) {
        Fire(player, TriggerType::Levitation, [&](const json& c) {
            if (const json* distance = Field(c, "distance")) {
                if (!Advancements::TestDistance(*distance, start, player.getPosition())) return false;
            }
            return Advancements::TestBounds(Field(c, "duration"), duration);
        });
    }

    void ChangedDimension(ServerPlayer& player, Game::DimensionId from, Game::DimensionId to,
                          const glm::dvec3& positionBefore) {
        if (from == to) return;
        Advancements::PlayerAdvancements* advancements = Advancements::Get(player);
        if (advancements) {
            std::lock_guard<std::recursive_mutex> lock(advancements->Mutex());
            // MC teleport: entering the Nether from the Overworld remembers
            // where (enteredNetherPosition).
            if (from == Game::DimensionId::Overworld && to == Game::DimensionId::Nether) {
                advancements->Tracked().enteredNetherPosition = positionBefore;
            }
        }
        const std::string fromId = WithNamespace(std::string(Game::DimensionRegistryName(from)));
        const std::string toId = WithNamespace(std::string(Game::DimensionRegistryName(to)));
        Fire(player, TriggerType::ChangedDimension, [&](const json& c) {
            if (const json* f = Field(c, "from"); f && (!f->is_string() || WithNamespace(f->get<std::string>()) != fromId)) return false;
            if (const json* t = Field(c, "to"); t && (!t->is_string() || WithNamespace(t->get<std::string>()) != toId)) return false;
            return true;
        });
        if (from == Game::DimensionId::Nether && to == Game::DimensionId::Overworld && advancements) {
            std::optional<glm::dvec3> entered;
            {
                std::lock_guard<std::recursive_mutex> lock(advancements->Mutex());
                entered = advancements->Tracked().enteredNetherPosition;
                advancements->Tracked().enteredNetherPosition.reset();
            }
            if (entered) NetherTravel(player, *entered);
        }
    }

    void NetherTravel(ServerPlayer& player, const glm::dvec3& entered) {
        Fire(player, TriggerType::NetherTravel, [&](const json& c) { return DistanceMatches(c, player, entered); });
    }

    void FallFromHeight(ServerPlayer& player, const glm::dvec3& start) {
        Fire(player, TriggerType::FallFromHeight, [&](const json& c) { return DistanceMatches(c, player, start); });
    }

    void RideEntityInLava(ServerPlayer& player, const glm::dvec3& start) {
        Fire(player, TriggerType::RideEntityInLava, [&](const json& c) { return DistanceMatches(c, player, start); });
    }

    void FallAfterExplosion(ServerPlayer& player, const glm::dvec3& start, Game::Entity* cause) {
        Fire(player, TriggerType::FallAfterExplosion, [&](const json& c) {
            if (!DistanceMatches(c, player, start)) return false;
            if (Field(c, "cause")) return EntityField(c, "cause", player, cause);
            return true;
        });
    }

    void ExplosionLaunched(ServerPlayer& player, const glm::dvec3& impactPos, Game::Entity* cause) {
        Advancements::PlayerAdvancements* advancements = Advancements::Get(player);
        if (!advancements) return;
        std::lock_guard<std::recursive_mutex> lock(advancements->Mutex());
        Advancements::TrackedState& tracked = advancements->Tracked();
        tracked.currentExplosionImpactPos = impactPos;
        tracked.hasExplosionCause = cause != nullptr;
        tracked.currentExplosionCauseId = cause ? cause->GetId() : 0;
    }

} // namespace Server::CriteriaTriggers
