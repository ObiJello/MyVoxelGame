// File: src/server/advancements/AdvancementPredicates.cpp
#include "AdvancementPredicates.hpp"

#include "server/commands/SnbtParser.hpp"
#include "server/entity/ServerLevelBridge.hpp"
#include "server/IntegratedServer.hpp"
#include "server/level/LocateFinder.hpp"
#include "server/level/ServerLevel.hpp"
#include "server/player/ServerPlayer.hpp"
#include "server/world/storage/anvil/ComponentNbt.hpp"
#include "server/world/storage/anvil/EntityNbt.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/core/Mth.hpp"
#include "common/core/Log.hpp"
#include "common/entity/LivingEntity.hpp"
#include "common/entity/LightningBolt.hpp"
#include "common/entity/Mob.hpp"
#include "common/entity/effect/MobEffects.hpp"
#include "common/entity/mobs/AnimatedMobs.hpp"
#include "common/entity/mobs/Animals.hpp"
#include "common/entity/mobs/Slime.hpp"
#include "common/entity/mobs/WolfVariants.hpp"
#include "common/entity/projectile/FishingHook.hpp"
#include "common/world/biome/Biomes.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/block/entity/BeehiveBlockEntity.hpp"
#include "common/world/damagesource/DamageSourceInfo.hpp"
#include "common/world/fluid/FluidState.hpp"
#include "common/world/level/DimensionId.hpp"
#include "common/world/level/World.hpp"
#include "common/world/tags/DataTags.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace Server::Advancements {

    namespace {

        using json = nlohmann::json;

        std::string WithNamespace(std::string id) {
            if (id.find(':') == std::string::npos) id = "minecraft:" + id;
            return id;
        }

        const json* Field(const json& j, const char* key) {
            if (!j.is_object()) return nullptr;
            auto it = j.find(key);
            return it == j.end() || it->is_null() ? nullptr : &*it;
        }

        // A HolderSet in its codec forms: one id, a list of ids, or "#tag".
        // `inTag(tag)` answers tag membership for the value being tested.
        bool HolderSetContains(const json& set, std::string_view id,
                               const std::function<bool(const std::string&)>& inTag) {
            auto one = [&](const json& e) {
                if (!e.is_string()) return false;
                const std::string entry = e.get<std::string>();
                if (!entry.empty() && entry[0] == '#') return inTag && inTag(WithNamespace(entry.substr(1)));
                return WithNamespace(entry) == id;
            };
            if (set.is_array()) {
                for (const auto& e : set) if (one(e)) return true;
                return false;
            }
            return one(set);
        }

        // The things each test needs from the server world, null-safe.
        Game::World* WorldOf(ServerLevel* level) { return level ? level->World() : nullptr; }

        ServerLevel* LevelOfEntity(const Game::Entity& entity) {
            if (!g_integratedServer || !entity.Level()) return nullptr;
            return g_integratedServer->GetLevel(entity.Level()->Dimension());
        }

        // ItemPredicate JSON → its NBT form, once per JSON node (the data
        // pack's conditions are immutable for the registry's life).
        std::shared_ptr<::World::NBTTagCompound> ItemPredicateNbt(const json& predicate) {
            static std::mutex s_mutex;
            static std::unordered_map<const json*, std::shared_ptr<::World::NBTTagCompound>> s_cache;
            std::lock_guard<std::mutex> lock(s_mutex);
            auto it = s_cache.find(&predicate);
            if (it != s_cache.end()) return it->second;
            std::string error;
            // JSON is SNBT: quoted keys and strings, numbers, lists, objects.
            auto compound = Snbt::ParseCompound(predicate.is_object() ? predicate.dump() : "{}", error);
            if (!compound) {
                Log::Warning("[Advancements] item predicate %s: %s", predicate.dump().c_str(), error.c_str());
                compound = std::make_shared<::World::NBTTagCompound>();
            }
            s_cache[&predicate] = compound;
            return compound;
        }

        // ── LocationPredicate pieces ─────────────────────────────────────

        bool BiomeMatches(const json& set, Game::World& world, const glm::ivec3& pos) {
            const Game::BiomeInfo& info =
                Game::BiomeRegistry::Get(world.GetBiome(pos.x, pos.y, pos.z));
            const std::string id = WithNamespace(std::string(info.name));
            return HolderSetContains(set, id, [&id](const std::string& tag) {
                static std::mutex s_mutex;
                static std::unordered_map<std::string, std::unordered_set<std::string>> s_tags;
                std::lock_guard<std::mutex> lock(s_mutex);
                auto it = s_tags.find(tag);
                if (it == s_tags.end()) it = s_tags.emplace(tag, ResolveBiomeIdOrTag("#" + tag)).first;
                return it->second.count(id) != 0;
            });
        }

        bool StructureMatches(const json& set, ServerLevel& level, const glm::ivec3& pos) {
            std::vector<std::string> ids;
            auto add = [&ids](const json& e) {
                if (!e.is_string()) return;
                std::string entry = e.get<std::string>();
                if (!entry.empty() && entry[0] == '#') {
                    for (std::string& id : ResolveStructureIdOrTag(entry)) ids.push_back(WithNamespace(std::move(id)));
                } else {
                    ids.push_back(WithNamespace(std::move(entry)));
                }
            };
            if (set.is_array()) for (const auto& e : set) add(e);
            else add(set);
            if (ids.empty()) return false;
            return FindStructureStartAt(level, ids, pos).has_value();
        }

        bool FluidMatches(const json& predicate, Game::World& world, const glm::ivec3& pos) {
            const Game::FluidState fluid = Game::GetFluidState(world, pos.x, pos.y, pos.z);
            if (const json* fluids = Field(predicate, "fluids")) {
                std::string id;
                switch (fluid.type) {
                    case Game::FluidType::Water: id = fluid.source ? "minecraft:water" : "minecraft:flowing_water"; break;
                    case Game::FluidType::Lava:  id = fluid.source ? "minecraft:lava" : "minecraft:flowing_lava"; break;
                    default:                     id = "minecraft:empty"; break;
                }
                if (!HolderSetContains(*fluids, id, [&id](const std::string& tag) {
                        return Game::DataTags::HasTag(Game::DataTags::Registry::Fluid, id, tag);
                    })) return false;
            }
            if (const json* state = Field(predicate, "state")) {
                // FluidState properties: `level` (8 − amount for flowing,
                // 0 for a source) and `falling`.
                for (auto it = state->begin(); it != state->end(); ++it) {
                    if (it.key() == "falling") {
                        const bool want = it.value().is_boolean() ? it.value().get<bool>()
                                                                   : it.value().is_string() && it.value().get<std::string>() == "true";
                        if (fluid.falling != want) return false;
                    } else if (it.key() == "level") {
                        const int level = fluid.source ? 0 : std::max(0, 8 - static_cast<int>(fluid.amount));
                        if (!TestBounds(&it.value(), level) &&
                            !(it.value().is_string() && it.value().get<std::string>() == std::to_string(level))) return false;
                    } else {
                        return false;
                    }
                }
            }
            return true;
        }

        // BlockPredicate.matches(level, pos): blocks, state, nbt (not
        // modelled: a predicate that names nbt never matches), components.
        bool BlockPredicateMatches(const json& predicate, Game::World& world, const glm::ivec3& pos) {
            const Game::BlockState state = world.GetBlockState(pos.x, pos.y, pos.z);
            if (const json* blocks = Field(predicate, "blocks")) {
                if (!BlockMatchesHolderSet(*blocks, state)) return false;
            }
            if (const json* props = Field(predicate, "state")) {
                if (!TestStateProperties(*props, state)) return false;
            }
            if (Field(predicate, "nbt")) return false;
            return true;
        }

        // ── EntityPredicate pieces ───────────────────────────────────────

        bool EntityTypeMatches(const json& type, const Game::Entity& entity) {
            const std::string id = EntityTypeIdOf(entity);
            return HolderSetContains(type, id, [&id](const std::string& tag) {
                return Game::DataTags::HasTag(Game::DataTags::Registry::EntityType, id, tag);
            });
        }

        // EntityFlagsPredicate.
        bool FlagsMatch(const json& flags, Game::Entity& entity) {
            ServerPlayer* player = PlayerOf(&entity);
            auto* living = entity.AsLiving();
            auto check = [&flags](const char* key, bool actual) {
                const json* want = Field(flags, key);
                return !want || !want->is_boolean() || want->get<bool>() == actual;
            };
            const bool onGround  = player ? player->isOnGround() : entity.onGround;
            const bool onFire    = player ? player->isOnFire() : entity.IsOnFire();
            const bool sneaking  = player ? player->IsSneaking() : (living && living->IsDiscrete());
            const bool sprinting = player ? player->isSprinting() : entity.IsSprinting();
            const bool swimming  = living && living->IsSwimming();
            const bool flying    = player ? player->isFlying() : entity.IsAbilityFlying();
            const bool fallFly   = player ? player->isFallFlying() : entity.IsFallFlying();
            return check("is_on_ground", onGround) && check("is_on_fire", onFire) &&
                   check("is_sneaking", sneaking) && check("is_sprinting", sprinting) &&
                   check("is_swimming", swimming) && check("is_flying", flying) &&
                   check("is_baby", entity.IsBaby()) && check("is_in_water", entity.IsInWater()) &&
                   check("is_fall_flying", fallFly);
        }

        // EntityEquipmentPredicate.
        bool EquipmentMatches(const json& equipment, Game::Entity& entity) {
            Game::LivingEntity* living = entity.AsLiving();
            static const std::pair<const char*, Game::EquipmentSlot> kSlots[] = {
                {"head", Game::EquipmentSlot::HEAD},       {"chest", Game::EquipmentSlot::CHEST},
                {"legs", Game::EquipmentSlot::LEGS},       {"feet", Game::EquipmentSlot::FEET},
                {"body", Game::EquipmentSlot::BODY},       {"mainhand", Game::EquipmentSlot::MAINHAND},
                {"offhand", Game::EquipmentSlot::OFFHAND},
            };
            for (const auto& [key, slot] : kSlots) {
                const json* predicate = Field(equipment, key);
                if (!predicate) continue;
                const Game::ItemStack* stack = living ? living->EquipmentInSlot(slot) : nullptr;
                if (!TestItem(*predicate, stack ? *stack : Game::ItemStack{})) return false;
            }
            return true;
        }

        // MobEffectsPredicate: each named effect present, its MobEffect-
        // InstancePredicate (amplifier, duration, ambient, visible) met.
        bool EffectsMatch(const json& effects, Game::Entity& entity) {
            const Game::LivingEntity* living = entity.AsLiving();
            if (!living) return false;
            for (auto it = effects.begin(); it != effects.end(); ++it) {
                Game::MobEffectId id;
                if (!Game::ParseEffectId(it.key(), id)) return false;
                const Game::MobEffectInstance* instance = living->GetEffect(id);
                if (!instance) return false;
                const json& p = it.value();
                if (!TestBounds(Field(p, "amplifier"), instance->amplifier)) return false;
                if (!TestBounds(Field(p, "duration"), instance->duration)) return false;
                if (const json* a = Field(p, "ambient"); a && a->is_boolean() && a->get<bool>() != instance->ambient) return false;
                if (const json* v = Field(p, "visible"); v && v->is_boolean() && v->get<bool>() != instance->visible) return false;
            }
            return true;
        }

        // EntityExactDataComponentsPredicate over the entity variant
        // components the engine's mobs carry.
        std::optional<std::string> EntityComponentValue(const Game::Entity& entity, const std::string& component) {
            if (component == "minecraft:cat/variant") {
                if (const auto* cat = dynamic_cast<const Game::Cat*>(&entity)) {
                    static const char* kNames[] = {"tabby", "black", "red", "siamese", "british_shorthair", "calico",
                                                   "persian", "ragdoll", "white", "jellie", "all_black"};
                    const uint8_t v = cat->GetVariantByte();
                    if (v < sizeof(kNames) / sizeof(kNames[0])) return std::string("minecraft:") + kNames[v];
                }
                return std::nullopt;
            }
            if (component == "minecraft:wolf/variant") {
                if (const auto* wolf = dynamic_cast<const Game::Wolf*>(&entity)) {
                    return std::string("minecraft:") + Game::WolfVariants::Name(wolf->GetVariant());
                }
                return std::nullopt;
            }
            if (component == "minecraft:frog/variant") {
                if (const auto* frog = dynamic_cast<const Game::Frog*>(&entity)) {
                    return std::string("minecraft:") + Game::Frog::VariantName(frog->GetVariant());
                }
                return std::nullopt;
            }
            return std::nullopt;
        }

        bool ComponentsMatch(const json& components, const Game::Entity& entity) {
            for (auto it = components.begin(); it != components.end(); ++it) {
                const std::string key = WithNamespace(it.key());
                const auto value = EntityComponentValue(entity, key);
                if (!value || !it.value().is_string()) return false;
                if (WithNamespace(it.value().get<std::string>()) != *value) return false;
            }
            return true;
        }

        // MovementPredicate: the entity's velocity in blocks per second.
        bool MovementMatches(const json& movement, const Game::Entity& entity) {
            const glm::dvec3 v = entity.velocity * 20.0;
            const double horizontal = std::sqrt(v.x * v.x + v.z * v.z);
            return TestBounds(Field(movement, "x"), v.x) && TestBounds(Field(movement, "y"), v.y) &&
                   TestBounds(Field(movement, "z"), v.z) &&
                   TestBounds(Field(movement, "speed"), glm::length(v)) &&
                   TestBounds(Field(movement, "horizontal_speed"), horizontal) &&
                   TestBounds(Field(movement, "vertical_speed"), std::abs(v.y)) &&
                   TestBounds(Field(movement, "fall_distance"), entity.fallDistance);
        }

        // MC PlayerPredicate.looking_at: the first non-spectator entity on
        // the 100-block view ray, in line of sight.
        Game::Entity* LookedAtEntity(ServerPlayer& player) {
            Game::LivingEntity* view = player.effectEntity();
            Game::EntityLevel* level = view ? view->Level() : nullptr;
            if (!view || !level) return nullptr;
            const glm::dvec3 eye = player.getPosition() + glm::dvec3(0.0, player.getEyeHeight(), 0.0);
            // getViewVector(1): Entity.calculateViewVector(xRot, yRot).
            const glm::dvec3 dir(Game::Mth::ViewVector(player.getPitch(), player.getYaw()));
            const glm::dvec3 to = eye + dir * 100.0;
            const glm::dvec3 lo = glm::min(eye, to) - glm::dvec3(1.0);
            const glm::dvec3 hi = glm::max(eye, to) + glm::dvec3(1.0);
            std::vector<Game::Entity*> candidates;
            level->GetEntitiesInBox(Game::AABB::FromMinMax(glm::vec3(lo), glm::vec3(hi)), view, candidates);
            Game::Entity* best = nullptr;
            double bestDist = 100.0 * 100.0;
            for (Game::Entity* e : candidates) {
                if (!e || e->IsSpectator() || !e->IsPickable()) continue;
                const Game::AABBd box = e->GetAABBd();
                const double grow = e->GetPickRadius();
                const glm::dvec3 bmin(box.min.x - grow, box.min.y - grow, box.min.z - grow);
                const glm::dvec3 bmax(box.max.x + grow, box.max.y + grow, box.max.z + grow);
                // Slab test of the ray against the box.
                double tmin = 0.0, tmax = 1.0;
                const glm::dvec3 d = to - eye;
                bool hit = true;
                for (int axis = 0; axis < 3 && hit; ++axis) {
                    if (std::abs(d[axis]) < 1e-9) {
                        if (eye[axis] < bmin[axis] || eye[axis] > bmax[axis]) hit = false;
                    } else {
                        double t1 = (bmin[axis] - eye[axis]) / d[axis];
                        double t2 = (bmax[axis] - eye[axis]) / d[axis];
                        if (t1 > t2) std::swap(t1, t2);
                        tmin = std::max(tmin, t1);
                        tmax = std::min(tmax, t2);
                        if (tmin > tmax) hit = false;
                    }
                }
                if (!hit) continue;
                const double dist = glm::length(d * tmin);
                if (dist * dist < bestDist) { bestDist = dist * dist; best = e; }
            }
            if (!best) return nullptr;
            // LivingEntity.hasLineOfSight: eye to eye through colliders.
            const Game::IBlockAccess* blocks = level->Blocks();
            if (!blocks) return best;
            const glm::dvec3 target(best->position.x, best->GetEyeY(), best->position.z);
            const glm::dvec3 delta = target - eye;
            const double distance = glm::length(delta);
            if (distance > 128.0) return nullptr;
            const int steps = std::max(1, static_cast<int>(std::ceil(distance * 4.0)));
            glm::dvec3 p = eye;
            for (int i = 1; i < steps; ++i) {
                p += delta / static_cast<double>(steps);
                if (Game::BlockRegistry::HasCollision(blocks->GetBlock(static_cast<int>(std::floor(p.x)),
                                                                      static_cast<int>(std::floor(p.y)),
                                                                      static_cast<int>(std::floor(p.z))))) {
                    return nullptr;
                }
            }
            return best;
        }

        bool TypeSpecificMatches(const json& predicate, ServerLevel* level,
                                 const std::optional<glm::dvec3>& position, Game::Entity& entity) {
            const json* typeField = Field(predicate, "type");
            if (!typeField || !typeField->is_string()) return false;
            const std::string type = WithNamespace(typeField->get<std::string>());
            if (type == "minecraft:player") {
                ServerPlayer* player = PlayerOf(&entity);
                if (!player) return false;
                if (const json* modes = Field(predicate, "gamemode")) {
                    static const char* kModes[] = {"survival", "creative", "adventure", "spectator"};
                    const char* mode = kModes[static_cast<int>(player->getGameMode()) & 3];
                    bool any = false;
                    if (modes->is_array()) {
                        for (const auto& m : *modes) any |= m.is_string() && m.get<std::string>() == mode;
                    } else {
                        any = modes->is_string() && modes->get<std::string>() == mode;
                    }
                    if (!any) return false;
                }
                if (!TestBounds(Field(predicate, "level"), player->getExperience().Level())) return false;
                // stats / recipes / advancements need systems this engine
                // does not have (statistics, the recipe book) or would
                // recurse into progress while it is being awarded.
                if (Field(predicate, "stats") || Field(predicate, "recipes") || Field(predicate, "advancements")) {
                    return false;
                }
                if (const json* lookingAt = Field(predicate, "looking_at")) {
                    Game::Entity* target = LookedAtEntity(*player);
                    if (!target || !TestEntity(*lookingAt, level, position, target)) return false;
                }
                return true;
            }
            if (type == "minecraft:lightning") {
                auto* bolt = dynamic_cast<Game::LightningBolt*>(&entity);
                if (!bolt) return false;
                if (!TestBounds(Field(predicate, "blocks_set_on_fire"), bolt->GetBlocksSetOnFire())) return false;
                // entity_struck: the bolt's hit entities are only known to
                // the strike trigger itself, which passes its bystanders.
                return !Field(predicate, "entity_struck");
            }
            if (type == "minecraft:fishing_hook") {
                auto* hook = dynamic_cast<Game::FishingHook*>(&entity);
                if (!hook) return false;
                if (const json* open = Field(predicate, "in_open_water"); open && open->is_boolean()) {
                    return hook->IsOpenWaterFishing() == open->get<bool>();
                }
                return true;
            }
            if (type == "minecraft:slime" || type == "minecraft:cube_mob") {
                auto* slime = dynamic_cast<Game::Slime*>(&entity);
                return slime && TestBounds(Field(predicate, "size"), slime->GetSize());
            }
            if (type == "minecraft:sheep") {
                auto* sheep = dynamic_cast<Game::Sheep*>(&entity);
                if (!sheep) return false;
                if (const json* sheared = Field(predicate, "sheared"); sheared && sheared->is_boolean()) {
                    return sheep->IsSheared() == sheared->get<bool>();
                }
                return true;
            }
            // raider (raids), and anything else this engine cannot answer.
            return false;
        }

    } // namespace

    // ── Ids ──────────────────────────────────────────────────────────────

    std::string EntityTypeIdOf(const Game::Entity& entity) {
        if (entity.IsPlayer()) return "minecraft:player";
        return Game::Anvil::EntityName(entity.GetType());
    }

    std::string BlockIdOf(Game::BlockState state) {
        const auto& block = Game::BlockRegistry::Get(state.Block());
        return WithNamespace(block.registrySlug.empty() ? std::string(block.name) : block.registrySlug);
    }

    std::string DimensionIdOf(const ServerLevel* level) {
        if (!level) return {};
        return WithNamespace(std::string(Game::DimensionRegistryName(level->Dimension())));
    }

    ServerPlayer* PlayerOf(const Game::Entity* entity) {
        if (!entity || !entity->IsPlayer()) return nullptr;
        const auto* view = dynamic_cast<const PlayerEntityView*>(entity);
        return view ? view->GetPlayer() : nullptr;
    }

    LootContext EntityContext(ServerPlayer& player, Game::Entity* entity) {
        LootContext ctx;
        ctx.level = g_integratedServer ? g_integratedServer->GetLevel(Game::DimensionFromRaw(player.getDimensionId())) : nullptr;
        ctx.origin = player.getPosition();
        ctx.thisEntity = entity;
        return ctx;
    }

    // ── MinMaxBounds ─────────────────────────────────────────────────────

    bool TestBounds(const json* bounds, double value) {
        if (!bounds) return true;
        if (bounds->is_number()) return value == bounds->get<double>();
        if (bounds->is_object()) {
            if (const json* lo = Field(*bounds, "min"); lo && lo->is_number() && value < lo->get<double>()) return false;
            if (const json* hi = Field(*bounds, "max"); hi && hi->is_number() && value > hi->get<double>()) return false;
            return true;
        }
        return false;
    }

    // ── StatePropertiesPredicate / blocks ────────────────────────────────

    bool TestStateProperties(const json& predicate, Game::BlockState state) {
        if (!predicate.is_object()) return true;
        for (auto it = predicate.begin(); it != predicate.end(); ++it) {
            const std::string_view actual = state.GetValueByName(it.key());
            if (actual.empty()) return false;   // the block has no such property
            const json& want = it.value();
            if (want.is_string()) {
                if (actual != want.get<std::string>()) return false;
            } else if (want.is_boolean()) {
                if (actual != (want.get<bool>() ? "true" : "false")) return false;
            } else if (want.is_number_integer()) {
                if (actual != std::to_string(want.get<long long>())) return false;
            } else if (want.is_object()) {
                // RangedMatcher: min/max compared as the property's values;
                // the numeric properties are what vanilla ranges over.
                char* end = nullptr;
                const std::string a(actual);
                const double v = std::strtod(a.c_str(), &end);
                if (end == a.c_str()) return false;
                auto bound = [](const json* b, double& out) {
                    if (!b) return false;
                    if (b->is_number()) { out = b->get<double>(); return true; }
                    if (b->is_string()) { out = std::strtod(b->get<std::string>().c_str(), nullptr); return true; }
                    return false;
                };
                double lo = 0.0, hi = 0.0;
                if (bound(Field(want, "min"), lo) && v < lo) return false;
                if (bound(Field(want, "max"), hi) && v > hi) return false;
            } else {
                return false;
            }
        }
        return true;
    }

    bool BlockMatchesHolderSet(const json& set, Game::BlockState state) {
        const std::string id = BlockIdOf(state);
        return HolderSetContains(set, id, [&id](const std::string& tag) {
            return Game::DataTags::HasTag(Game::DataTags::Registry::Block, id, tag);
        });
    }

    // ── ItemPredicate ────────────────────────────────────────────────────

    bool TestItem(const json& predicate, const Game::ItemStack& stack) {
        if (!predicate.is_object()) return false;
        if (predicate.empty()) return true;
        if (stack.IsEmpty()) {
            // An empty stack still answers a count of 0 / no items.
            if (Field(predicate, "items") || Field(predicate, "components") || Field(predicate, "predicates")) return false;
        }
        const auto nbt = ItemPredicateNbt(predicate);
        return Game::Anvil::ComponentNbt::ItemPredicateMatches(*nbt, stack);
    }

    // ── DistancePredicate ────────────────────────────────────────────────

    bool TestDistance(const json& predicate, const glm::dvec3& from, const glm::dvec3& to) {
        const double dx = from.x - to.x, dy = from.y - to.y, dz = from.z - to.z;
        if (!TestBounds(Field(predicate, "x"), std::abs(dx))) return false;
        if (!TestBounds(Field(predicate, "y"), std::abs(dy))) return false;
        if (!TestBounds(Field(predicate, "z"), std::abs(dz))) return false;
        // MinMaxBounds.Doubles.matchesSqr for the two euclidean distances.
        if (const json* h = Field(predicate, "horizontal")) {
            if (!TestBounds(h, std::sqrt(dx * dx + dz * dz))) return false;
        }
        if (const json* a = Field(predicate, "absolute")) {
            if (!TestBounds(a, std::sqrt(dx * dx + dy * dy + dz * dz))) return false;
        }
        return true;
    }

    // ── LocationPredicate ────────────────────────────────────────────────

    bool TestLocation(const json& predicate, ServerLevel* level, double x, double y, double z) {
        if (!predicate.is_object()) return false;
        if (const json* position = Field(predicate, "position")) {
            if (!TestBounds(Field(*position, "x"), x) || !TestBounds(Field(*position, "y"), y) ||
                !TestBounds(Field(*position, "z"), z)) return false;
        }
        if (const json* dimension = Field(predicate, "dimension")) {
            if (!dimension->is_string() || WithNamespace(dimension->get<std::string>()) != DimensionIdOf(level)) return false;
        }
        Game::World* world = WorldOf(level);
        if (!world) return false;
        const glm::ivec3 pos(static_cast<int>(std::floor(x)), static_cast<int>(std::floor(y)), static_cast<int>(std::floor(z)));
        // MC: `if (!level.isLoaded(pos)) return false` before anything that
        // reads the world.
        const bool needsWorld = Field(predicate, "biomes") || Field(predicate, "structures") ||
                                Field(predicate, "smokey") || Field(predicate, "light") ||
                                Field(predicate, "block") || Field(predicate, "fluid") || Field(predicate, "can_see_sky");
        if (!needsWorld) return true;
        if (!world->IsChunkLoaded(pos.x >> 4, pos.z >> 4)) return false;
        if (const json* biomes = Field(predicate, "biomes")) {
            if (!BiomeMatches(*biomes, *world, pos)) return false;
        }
        if (const json* structures = Field(predicate, "structures")) {
            if (!StructureMatches(*structures, *level, pos)) return false;
        }
        if (const json* smokey = Field(predicate, "smokey"); smokey && smokey->is_boolean()) {
            if (Game::IsSmokeyPos(*world, pos) != smokey->get<bool>()) return false;
        }
        if (const json* light = Field(predicate, "light")) {
            // LightPredicate: level.getMaxLocalRawBrightness(pos).
            const Game::EntityLevel* entities = level->MobLevel();
            const int brightness = entities ? entities->GetMaxLocalRawBrightness(pos.x, pos.y, pos.z)
                                            : world->GetRawBrightness(pos.x, pos.y, pos.z);
            if (!TestBounds(Field(*light, "light"), brightness)) return false;
        }
        if (const json* block = Field(predicate, "block")) {
            if (!BlockPredicateMatches(*block, *world, pos)) return false;
        }
        if (const json* fluid = Field(predicate, "fluid")) {
            if (!FluidMatches(*fluid, *world, pos)) return false;
        }
        if (const json* sky = Field(predicate, "can_see_sky"); sky && sky->is_boolean()) {
            if (world->CanSeeSky(pos.x, pos.y, pos.z) != sky->get<bool>()) return false;
        }
        return true;
    }

    // ── EntityPredicate ──────────────────────────────────────────────────

    bool TestEntity(const json& predicate, ServerLevel* level, const std::optional<glm::dvec3>& position,
                    Game::Entity* entity) {
        if (!entity) return false;
        if (!predicate.is_object()) return false;
        ServerLevel* entityLevel = LevelOfEntity(*entity);
        if (!entityLevel) entityLevel = level;
        const glm::dvec3 epos = entity->position;

        if (const json* type = Field(predicate, "type")) {
            if (!EntityTypeMatches(*type, *entity)) return false;
        }
        if (const json* type = Field(predicate, "entity_type")) {
            if (!EntityTypeMatches(*type, *entity)) return false;
        }
        if (const json* distance = Field(predicate, "distance")) {
            if (!position || !TestDistance(*distance, *position, epos)) return false;
        }
        if (const json* movement = Field(predicate, "movement")) {
            if (!MovementMatches(*movement, *entity)) return false;
        }
        if (const json* location = Field(predicate, "location")) {
            if (!TestLocation(*location, entityLevel, epos.x, epos.y, epos.z)) return false;
        }
        if (const json* stepping = Field(predicate, "stepping_on")) {
            // Entity.getOnPos: the block a hair under the feet.
            const glm::ivec3 on(static_cast<int>(std::floor(epos.x)), static_cast<int>(std::floor(epos.y - 1.0e-5)),
                                static_cast<int>(std::floor(epos.z)));
            if (!TestLocation(*stepping, entityLevel, on.x + 0.5, on.y + 0.5, on.z + 0.5)) return false;
        }
        if (const json* affected = Field(predicate, "movement_affected_by")) {
            // getBlockPosBelowThatAffectsMyMovement: half a block down.
            const glm::ivec3 below(static_cast<int>(std::floor(epos.x)), static_cast<int>(std::floor(epos.y - 0.5000001)),
                                   static_cast<int>(std::floor(epos.z)));
            if (!TestLocation(*affected, entityLevel, below.x + 0.5, below.y + 0.5, below.z + 0.5)) return false;
        }
        if (const json* effects = Field(predicate, "effects")) {
            if (!EffectsMatch(*effects, *entity)) return false;
        }
        if (Field(predicate, "nbt") || Field(predicate, "team") || Field(predicate, "slots")) {
            // Entity NBT, scoreboard teams and slot ranges: not modelled for
            // advancements (no vanilla advancement uses them).
            return false;
        }
        if (const json* flags = Field(predicate, "flags")) {
            if (!FlagsMatch(*flags, *entity)) return false;
        }
        if (const json* equipment = Field(predicate, "equipment")) {
            if (!EquipmentMatches(*equipment, *entity)) return false;
        }
        if (const json* periodic = Field(predicate, "periodic_tick"); periodic && periodic->is_number_integer()) {
            const int n = periodic->get<int>();
            if (n <= 0 || entity->tickCount % n != 0) return false;
        }
        if (const json* vehicle = Field(predicate, "vehicle")) {
            Game::Entity* v = entity->GetVehicle();
            if (!TestEntity(*vehicle, level, position, v)) return false;
        }
        if (const json* passenger = Field(predicate, "passenger")) {
            bool any = false;
            for (Game::Entity* p : entity->GetPassengers()) {
                if (TestEntity(*passenger, level, position, p)) { any = true; break; }
            }
            if (!any) return false;
        }
        if (const json* targeted = Field(predicate, "targeted_entity")) {
            auto* mob = dynamic_cast<Game::Mob*>(entity);
            if (!TestEntity(*targeted, level, position, mob ? mob->GetTarget() : nullptr)) return false;
        }
        if (const json* components = Field(predicate, "components")) {
            if (!ComponentsMatch(*components, *entity)) return false;
        }
        if (const json* typeSpecific = Field(predicate, "type_specific")) {
            if (!TypeSpecificMatches(*typeSpecific, level, position, *entity)) return false;
        }
        return true;
    }

    // ── Damage ───────────────────────────────────────────────────────────

    bool TestDamageSource(const json& predicate, ServerPlayer& player, const Game::DamageSourceInfo& source) {
        if (!predicate.is_object()) return false;
        ServerLevel* level = g_integratedServer
            ? g_integratedServer->GetLevel(Game::DimensionFromRaw(player.getDimensionId())) : nullptr;
        const glm::dvec3 origin = player.getPosition();
        if (const json* tags = Field(predicate, "tags"); tags && tags->is_array()) {
            for (const auto& t : *tags) {
                const json* id = Field(t, "id");
                if (!id || !id->is_string()) return false;
                const bool expected = t.value("expected", true);
                if (source.Is(id->get<std::string>()) != expected) return false;
            }
        }
        if (const json* direct = Field(predicate, "direct_entity")) {
            Game::Entity* d = source.direct ? source.direct : source.causing;   // getDirectEntity
            if (!TestEntity(*direct, level, origin, d)) return false;
        }
        if (const json* causing = Field(predicate, "source_entity")) {
            if (!TestEntity(*causing, level, origin, source.causing)) return false;
        }
        if (const json* isDirect = Field(predicate, "is_direct"); isDirect && isDirect->is_boolean()) {
            if (source.IsDirect() != isDirect->get<bool>()) return false;
        }
        return true;
    }

    bool TestDamage(const json& predicate, ServerPlayer& player, const Game::DamageSourceInfo& source,
                    float dealt, float taken, bool blocked) {
        if (!predicate.is_object()) return false;
        if (!TestBounds(Field(predicate, "dealt"), dealt)) return false;
        if (!TestBounds(Field(predicate, "taken"), taken)) return false;
        if (const json* b = Field(predicate, "blocked"); b && b->is_boolean() && b->get<bool>() != blocked) return false;
        if (const json* causing = Field(predicate, "source_entity")) {
            ServerLevel* level = g_integratedServer
                ? g_integratedServer->GetLevel(Game::DimensionFromRaw(player.getDimensionId())) : nullptr;
            if (!TestEntity(*causing, level, player.getPosition(), source.causing)) return false;
        }
        if (const json* type = Field(predicate, "type")) {
            if (!TestDamageSource(*type, player, source)) return false;
        }
        return true;
    }

    // ── LootItemCondition ────────────────────────────────────────────────

    bool TestConditions(const json* conditions, const LootContext& ctx) {
        if (!conditions || conditions->is_null()) return true;
        const json& c = *conditions;
        if (c.is_array()) {
            for (const auto& term : c) if (!TestConditions(&term, ctx)) return false;
            return true;
        }
        if (!c.is_object()) return false;
        const json* type = Field(c, "condition");
        if (!type || !type->is_string()) {
            // The pre-1.20.5 shorthand: a bare EntityPredicate for "this".
            return TestEntity(c, ctx.level, ctx.origin, ctx.thisEntity);
        }
        const std::string kind = WithNamespace(type->get<std::string>());

        if (kind == "minecraft:entity_properties") {
            const std::string target = c.value("entity", std::string("this"));
            // Advancement contexts only carry THIS_ENTITY.
            Game::Entity* entity = target == "this" ? ctx.thisEntity : nullptr;
            const json* predicate = Field(c, "predicate");
            if (!entity) return false;
            return !predicate || TestEntity(*predicate, ctx.level, ctx.origin, entity);
        }
        if (kind == "minecraft:location_check") {
            if (!ctx.origin) return false;
            const json* predicate = Field(c, "predicate");
            if (!predicate) return true;
            const int ox = c.value("offsetX", 0), oy = c.value("offsetY", 0), oz = c.value("offsetZ", 0);
            // LocationCheck: BlockPos.containing(origin) + offset, tested
            // at that block's corner coordinates.
            const glm::ivec3 base(static_cast<int>(std::floor(ctx.origin->x)), static_cast<int>(std::floor(ctx.origin->y)),
                                  static_cast<int>(std::floor(ctx.origin->z)));
            return TestLocation(*predicate, ctx.level, base.x + ox, base.y + oy, base.z + oz);
        }
        if (kind == "minecraft:block_state_property") {
            if (!ctx.blockState) return false;
            const json* block = Field(c, "block");
            if (!block || !block->is_string() || WithNamespace(block->get<std::string>()) != BlockIdOf(*ctx.blockState)) {
                return false;
            }
            const json* props = Field(c, "properties");
            return !props || TestStateProperties(*props, *ctx.blockState);
        }
        if (kind == "minecraft:match_tool") {
            if (!ctx.tool) return false;
            const json* predicate = Field(c, "predicate");
            return !predicate || TestItem(*predicate, *ctx.tool);
        }
        if (kind == "minecraft:all_of") {
            const json* terms = Field(c, "terms");
            if (!terms || !terms->is_array()) return true;
            for (const auto& t : *terms) if (!TestConditions(&t, ctx)) return false;
            return true;
        }
        if (kind == "minecraft:any_of") {
            const json* terms = Field(c, "terms");
            if (!terms || !terms->is_array()) return false;
            for (const auto& t : *terms) if (TestConditions(&t, ctx)) return true;
            return false;
        }
        if (kind == "minecraft:inverted") {
            const json* term = Field(c, "term");
            return term && !TestConditions(term, ctx);
        }
        if (kind == "minecraft:random_chance") {
            static Game::JavaRandom s_random(0x5DEECE66DLL ^ static_cast<int64_t>(std::time(nullptr)));
            const json* chance = Field(c, "chance");
            const double p = chance && chance->is_number() ? chance->get<double>() : 0.0;
            return s_random.NextFloat() < p;
        }
        if (kind == "minecraft:weather_check") {
            const Game::EntityLevel* lvl = ctx.level ? ctx.level->MobLevel() : nullptr;
            if (!lvl) return false;
            if (const json* r = Field(c, "raining"); r && r->is_boolean() && r->get<bool>() != lvl->IsRaining()) return false;
            if (const json* t = Field(c, "thundering"); t && t->is_boolean() && t->get<bool>() != lvl->IsThundering()) return false;
            return true;
        }
        if (kind == "minecraft:time_check") {
            const Game::EntityLevel* lvl = ctx.level ? ctx.level->MobLevel() : nullptr;
            if (!lvl) return false;
            int64_t time = lvl->GetDayTime();
            if (const json* period = Field(c, "period"); period && period->is_number_integer() && period->get<int64_t>() > 0) {
                time %= period->get<int64_t>();
            }
            return TestBounds(Field(c, "value"), static_cast<double>(time));
        }
        static std::once_flag s_warned;
        std::call_once(s_warned, [&kind] {
            Log::Warning("[Advancements] loot condition %s is not supported; criteria using it never match", kind.c_str());
        });
        return false;
    }

} // namespace Server::Advancements
