// File: src/server/world/storage/anvil/SpawnerNbt.cpp
#include "server/world/storage/anvil/SpawnerNbt.hpp"

#include "server/IntegratedServer.hpp"
#include "server/entity/MobManager.hpp"
#include "server/level/LevelEntityStore.hpp"   // MakeMobForLoad
#include "server/level/ServerLevel.hpp"
#include "server/world/storage/anvil/EntityNbt.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/core/Log.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/Mob.hpp"
#include "common/world/biome/Biomes.hpp"
#include "common/world/block/entity/SpawnerBlockEntity.hpp"
#include "common/world/level/World.hpp"
#include "common/world/spawn/SpawnPlacements.hpp"

#include <algorithm>
#include <functional>
#include <limits>
#include <string>

namespace Game::Anvil {

    namespace {

        using ::World::NBTTag;
        using ::World::NBTTagCompound;
        using ::World::NBTTagList;
        using ::World::NBTTagPtr;
        using ::World::NBTTagType;

        Nbt::TagType ToWriterType(NBTTagType t) { return static_cast<Nbt::TagType>(t); }

        // One list element (no name) of any kind.
        void WriteListElement(Nbt::Writer& w, Nbt::Writer::ListScope& list, const NBTTag& tag) {
            switch (tag.type) {
                case NBTTagType::TAG_Byte:   w.ListByte(list, static_cast<const ::World::NBTTagByte&>(tag).value); break;
                case NBTTagType::TAG_Short:  w.ListShort(list, static_cast<const ::World::NBTTagShort&>(tag).value); break;
                case NBTTagType::TAG_Int:    w.ListInt(list, static_cast<const ::World::NBTTagInt&>(tag).value); break;
                case NBTTagType::TAG_Long:   w.ListLong(list, static_cast<const ::World::NBTTagLong&>(tag).value); break;
                case NBTTagType::TAG_Float:  w.ListFloat(list, static_cast<const ::World::NBTTagFloat&>(tag).value); break;
                case NBTTagType::TAG_Double: w.ListDouble(list, static_cast<const ::World::NBTTagDouble&>(tag).value); break;
                case NBTTagType::TAG_String: w.ListString(list, static_cast<const ::World::NBTTagString&>(tag).value); break;
                case NBTTagType::TAG_Byte_Array: {
                    const auto& v = static_cast<const ::World::NBTTagByteArray&>(tag).value;
                    w.ListByteArray(list, v.data(), v.size());
                    break;
                }
                case NBTTagType::TAG_Int_Array: {
                    const auto& v = static_cast<const ::World::NBTTagIntArray&>(tag).value;
                    w.ListIntArray(list, v.data(), v.size());
                    break;
                }
                case NBTTagType::TAG_Long_Array: {
                    const auto& v = static_cast<const ::World::NBTTagLongArray&>(tag).value;
                    w.ListLongArray(list, v.data(), v.size());
                    break;
                }
                case NBTTagType::TAG_List: {
                    const auto& inner = static_cast<const NBTTagList&>(tag);
                    auto nested = w.ListListBegin(list, ToWriterType(inner.listType));
                    for (const NBTTagPtr& e : inner.value) if (e) WriteListElement(w, nested, *e);
                    w.EndList(nested);
                    break;
                }
                case NBTTagType::TAG_Compound: {
                    w.ListCompoundBegin(list);
                    for (const auto& [key, child] : static_cast<const NBTTagCompound&>(tag).value) {
                        if (child) WriteNbtTree(w, key, *child);
                    }
                    w.ListCompoundEnd(list);
                    break;
                }
                default: break;
            }
        }

        // MC NumericTag.xxxValue: any numeric tag, narrowed; getShortOr /
        // getIntOr read through it.
        int NumberValue(const NBTTagPtr& t, int fallback) {
            if (!t) return fallback;
            switch (t->type) {
                case NBTTagType::TAG_Byte:   return static_cast<const ::World::NBTTagByte&>(*t).value;
                case NBTTagType::TAG_Short:  return static_cast<const ::World::NBTTagShort&>(*t).value;
                case NBTTagType::TAG_Int:    return static_cast<const ::World::NBTTagInt&>(*t).value;
                case NBTTagType::TAG_Long:   return static_cast<int>(static_cast<const ::World::NBTTagLong&>(*t).value);
                case NBTTagType::TAG_Float:  return static_cast<int>(static_cast<const ::World::NBTTagFloat&>(*t).value);
                case NBTTagType::TAG_Double: return static_cast<int>(static_cast<const ::World::NBTTagDouble&>(*t).value);
                default: return fallback;
            }
        }
        int NumberOr(const NBTTagCompound& tag, const char* key, int fallback) {
            return NumberValue(tag.GetTag(key), fallback);
        }

        bool IsNumeric(const NBTTagPtr& t) {
            if (!t) return false;
            switch (t->type) {
                case NBTTagType::TAG_Byte: case NBTTagType::TAG_Short: case NBTTagType::TAG_Int:
                case NBTTagType::TAG_Long: case NBTTagType::TAG_Float: case NBTTagType::TAG_Double:
                    return true;
                default:
                    return false;
            }
        }

        // MC InclusiveRange.INT (ExtraCodecs.intervalCodec): a single number,
        // a two-element int list/array, or {min_inclusive, max_inclusive};
        // InclusiveRange.create refuses min > max. False when the value does
        // not parse — CustomSpawnRules reads it lenientOptionalFieldOf, so the
        // caller keeps the default then.
        bool ReadIntRange(const NBTTagCompound& tag, const char* key, int& lo, int& hi) {
            NBTTagPtr t = tag.GetTag(key);
            int a = 0, b = 0;
            if (IsNumeric(t)) {
                a = b = NumberOr(tag, key, 0);
            } else if (auto arr = std::dynamic_pointer_cast<::World::NBTTagIntArray>(t);
                       arr && arr->value.size() == 2) {
                a = arr->value[0];
                b = arr->value[1];
            } else if (auto list = std::dynamic_pointer_cast<NBTTagList>(t);
                       list && list->value.size() == 2 && IsNumeric(list->value[0]) &&
                       IsNumeric(list->value[1])) {
                a = NumberValue(list->value[0], 0);
                b = NumberValue(list->value[1], 0);
            } else if (auto obj = std::dynamic_pointer_cast<NBTTagCompound>(t);
                       obj && IsNumeric(obj->GetTag("min_inclusive")) &&
                       IsNumeric(obj->GetTag("max_inclusive"))) {
                a = NumberOr(*obj, "min_inclusive", 0);
                b = NumberOr(*obj, "max_inclusive", 0);
            } else {
                return false;
            }
            if (a > b) return false;
            lo = a;
            hi = b;
            return true;
        }

        // MC CustomSpawnRules.lightLimit: lenientOptionalFieldOf(name, [0, 15])
        // — absent or unparseable is the default — then validated against
        // LIGHT_RANGE: a range outside 0..15 fails the whole SpawnData.
        bool ReadLightLimit(const NBTTagCompound& rules, const char* key, int& lo, int& hi) {
            lo = 0;
            hi = 15;
            if (rules.HasTag(key) && !ReadIntRange(rules, key, lo, hi)) {
                lo = 0;
                hi = 15;
            }
            return lo >= 0 && hi <= 15;
        }

        // MC InclusiveRange.INT encoding: a bare int when min == max, the
        // [min, max] list otherwise. Omitted when it is the default [0, 15]
        // (lenientOptionalFieldOf with a default).
        void WriteIntRange(Nbt::Writer& w, const char* key, int lo, int hi) {
            if (lo == 0 && hi == 15) return;
            if (lo == hi) {
                w.Int(key, lo);
            } else {
                const int32_t v[2] = { lo, hi };
                w.IntArray(key, v, 2);
            }
        }

        // MC SpawnData.CODEC: {entity, custom_spawn_rules?, equipment?}.
        // False when the codec fails — `entity` missing, custom_spawn_rules
        // not a compound, or a light limit outside 0..15 — and MC's
        // read(...) then yields nothing (the field is treated as absent).
        bool ReadSpawnData(const NBTTagCompound& tag, SpawnData& out) {
            auto entity = std::dynamic_pointer_cast<NBTTagCompound>(tag.GetTag("entity"));
            if (!entity) return false;
            SpawnDataFromEntityTag(*entity, out);
            if (NBTTagPtr rulesTag = tag.GetTag("custom_spawn_rules")) {
                auto rules = std::dynamic_pointer_cast<NBTTagCompound>(rulesTag);
                if (!rules) return false;
                out.hasCustomRules = true;
                if (!ReadLightLimit(*rules, "block_light_limit", out.blockLightMin, out.blockLightMax) ||
                    !ReadLightLimit(*rules, "sky_light_limit", out.skyLightMin, out.skyLightMax)) {
                    return false;
                }
            }
            return true;
        }

        void WriteSpawnDataBody(Nbt::Writer& w, const SpawnData& data) {
            w.BeginCompound("entity");
            if (!data.entityNbt.empty()) {
                try {
                    auto root = std::dynamic_pointer_cast<NBTTagCompound>(
                        ::World::NBTParser::Parse(data.entityNbt));
                    if (root) {
                        for (const auto& [key, child] : root->value) {
                            if (child) WriteNbtTree(w, key, *child);
                        }
                    }
                } catch (const std::exception& e) {
                    Log::Warning("[Spawner] unreadable SpawnData entity: %s", e.what());
                }
            }
            w.EndCompound();
            if (data.hasCustomRules) {
                w.BeginCompound("custom_spawn_rules");
                WriteIntRange(w, "block_light_limit", data.blockLightMin, data.blockLightMax);
                WriteIntRange(w, "sky_light_limit", data.skyLightMin, data.skyLightMax);
                w.EndCompound();
            }
        }

        // ── SpawnerServerHooks ──────────────────────────────────────────────

        std::unique_ptr<Mob> LoadSpawnerEntity(const SpawnData& data, EntityLevel& level) {
            if (!data.hasType || data.entityNbt.empty()) return nullptr;
            std::shared_ptr<NBTTagCompound> tag;
            try {
                tag = std::dynamic_pointer_cast<NBTTagCompound>(::World::NBTParser::Parse(data.entityNbt));
            } catch (const std::exception&) {
                return nullptr;
            }
            if (!tag) return nullptr;
            EntityTypeId type{};
            if (ClassifyEntity(*tag, type) != EntityKind::Mob) return nullptr;
            std::unique_ptr<Mob> mob = ::Server::MakeMobForLoad(type, &level);
            if (!mob) return nullptr;
            ApplyMobNbt(*tag, *mob);
            return mob;
        }

        void SetSpawnerEntityId(SpawnData& data, EntityTypeId type) {
            std::shared_ptr<NBTTagCompound> root;
            if (!data.entityNbt.empty()) {
                try {
                    root = std::dynamic_pointer_cast<NBTTagCompound>(::World::NBTParser::Parse(data.entityNbt));
                } catch (const std::exception&) {
                    root.reset();
                }
            }
            if (!root) root = std::make_shared<NBTTagCompound>();
            root->value["id"] = std::make_shared<::World::NBTTagString>(EntityName(type));
            // Re-encoded from the edited tree, with the parsed fields.
            SpawnData updated;
            SpawnDataFromEntityTag(*root, updated);
            // The rules and everything else of the SpawnData stay; only the
            // entity compound (and what is parsed from it) changes.
            data.entityNbt = std::move(updated.entityNbt);
            data.hasType = updated.hasType;
            data.type = updated.type;
            data.hasPos = updated.hasPos;
            data.pos = updated.pos;
            data.bareId = updated.bareId;
            data.baby = updated.baby;
        }

        bool CheckSpawnerSpawnRules(EntityTypeId type, ::Game::World& world, const glm::ivec3& pos,
                                    JavaRandom& random) {
            EntityLevel* level = world.Entities();
            if (!level || !level->Blocks()) return true;
            const std::function<std::string_view(int, int, int)> biomeAt =
                [&world](int x, int y, int z) -> std::string_view {
                    return BiomeRegistry::Get(world.GetBiome(x, y, z)).name;
                };
            const std::function<int(int, int)> surfaceHeight = [&world](int x, int z) -> int {
                auto chunk = world.GetLoadedChunk(x >> 4, z >> 4);
                if (!chunk) return std::numeric_limits<int>::min();
                if (!chunk->AreHeightmapsPrimed()) chunk->PrimeHeightmaps();
                return chunk->GetSurfaceHeight(x & 15, z & 15, HeightmapType::WorldSurface);
            };
            int seaLevel = 63;
            if (::Server::g_integratedServer) {
                if (::Server::ServerLevel* sl = ::Server::g_integratedServer->GetLevel(world.GetDimension())) {
                    seaLevel = sl->SeaLevel();
                }
            }
            SpawnRuleContext ctx{ *level, *level->Blocks(), random, SpawnReason::Spawner,
                                  &biomeAt, &surfaceHeight, seaLevel, world.GetGenerationSeed() };
            return CheckSpawnRules(type, ctx, pos);
        }

        // MC ServerLevel.tryAddFreshEntityWithPassengers. Straight into the
        // level's MobManager rather than ServerLevelBridge::AddFreshEntity's
        // deferred queue: block entities tick in World::WorldLoop, outside the
        // mob loop that queue protects, and MC's add is immediate — the
        // next attempt of the cycle must count and collide with this mob,
        // and spawnAnim's event needs the id Add assigns.
        bool AddSpawnerEntity(std::unique_ptr<Mob>& mob, ::Game::World& world) {
            if (!mob) return false;
            ::Server::ServerLevel* level = ::Server::g_integratedServer
                ? ::Server::g_integratedServer->GetLevel(world.GetDimension()) : nullptr;
            ::Server::MobManager* mobs = level ? level->Mobs() : nullptr;
            if (!mobs) {
                // No manager to add to directly: the level's deferred queue.
                EntityLevel* entities = world.Entities();
                if (!entities) return false;
                entities->AddFreshEntity(std::move(mob));
                return true;
            }
            // The UUID half of tryAddFreshEntityWithPassengers: a compound
            // carrying a fixed UUID spawns once, then every later attempt is
            // refused.
            if (!UuidIsNil(mob->GetUuid()) && mobs->FindByUuid(mob->GetUuid())) {
                mob.reset();
                return false;
            }
            mobs->Add(std::move(mob));
            return true;
        }

    } // namespace

    void WriteNbtTree(Nbt::Writer& w, std::string_view name, const NBTTag& tag) {
        switch (tag.type) {
            case NBTTagType::TAG_Byte:   w.Byte(name, static_cast<const ::World::NBTTagByte&>(tag).value); break;
            case NBTTagType::TAG_Short:  w.Short(name, static_cast<const ::World::NBTTagShort&>(tag).value); break;
            case NBTTagType::TAG_Int:    w.Int(name, static_cast<const ::World::NBTTagInt&>(tag).value); break;
            case NBTTagType::TAG_Long:   w.Long(name, static_cast<const ::World::NBTTagLong&>(tag).value); break;
            case NBTTagType::TAG_Float:  w.Float(name, static_cast<const ::World::NBTTagFloat&>(tag).value); break;
            case NBTTagType::TAG_Double: w.Double(name, static_cast<const ::World::NBTTagDouble&>(tag).value); break;
            case NBTTagType::TAG_String: w.String(name, static_cast<const ::World::NBTTagString&>(tag).value); break;
            case NBTTagType::TAG_Byte_Array: {
                const auto& v = static_cast<const ::World::NBTTagByteArray&>(tag).value;
                w.ByteArray(name, v.data(), v.size());
                break;
            }
            case NBTTagType::TAG_Int_Array: {
                const auto& v = static_cast<const ::World::NBTTagIntArray&>(tag).value;
                w.IntArray(name, v.data(), v.size());
                break;
            }
            case NBTTagType::TAG_Long_Array: {
                const auto& v = static_cast<const ::World::NBTTagLongArray&>(tag).value;
                w.LongArray(name, v.data(), v.size());
                break;
            }
            case NBTTagType::TAG_List: {
                const auto& list = static_cast<const NBTTagList&>(tag);
                auto scope = w.BeginList(name, ToWriterType(list.listType));
                for (const NBTTagPtr& e : list.value) if (e) WriteListElement(w, scope, *e);
                w.EndList(scope);
                break;
            }
            case NBTTagType::TAG_Compound: {
                w.BeginCompound(name);
                for (const auto& [key, child] : static_cast<const NBTTagCompound&>(tag).value) {
                    if (child) WriteNbtTree(w, key, *child);
                }
                w.EndCompound();
                break;
            }
            default: break;
        }
    }

    void SpawnDataFromEntityTag(const NBTTagCompound& entity, SpawnData& out) {
        out = SpawnData{};
        Nbt::Writer w;
        w.BeginRootCompound();
        for (const auto& [key, child] : entity.value) {
            if (child) WriteNbtTree(w, key, *child);
        }
        w.EndRootCompound();
        if (w.ok()) out.entityNbt = w.TakeBytes();

        // MC SpawnData's constructor keeps "id" only when it parses as an
        // Identifier; an id this build has no type for cannot spawn here.
        const std::string id = entity.GetValue<std::string>("id", "");
        EntityTypeId type{};
        if (!id.empty() && EntityTypeFromName(id, type)) {
            out.hasType = true;
            out.type = type;
        }
        out.bareId = entity.value.size() == 1 && entity.HasTag("id");
        if (auto pos = std::dynamic_pointer_cast<NBTTagList>(entity.GetTag("Pos"));
            pos && pos->value.size() == 3) {
            auto x = std::dynamic_pointer_cast<::World::NBTTagDouble>(pos->value[0]);
            auto y = std::dynamic_pointer_cast<::World::NBTTagDouble>(pos->value[1]);
            auto z = std::dynamic_pointer_cast<::World::NBTTagDouble>(pos->value[2]);
            if (x && y && z) {
                out.hasPos = true;
                out.pos = glm::dvec3(x->value, y->value, z->value);
            }
        }
        out.baby = entity.GetValue<int8_t>("IsBaby", 0) != 0 || NumberOr(entity, "Age", 0) < 0;
    }

    void ReadSpawner(const NBTTagCompound& tag, SpawnerBlockEntity& spawner) {
        // MC BaseSpawner.load, field for field and in order.
        spawner.SetSpawnDelay(NumberOr(tag, "Delay", SpawnerBlockEntity::kDefaultSpawnDelay));
        if (auto data = std::dynamic_pointer_cast<NBTTagCompound>(tag.GetTag("SpawnData"))) {
            SpawnData next;
            if (ReadSpawnData(*data, next)) spawner.SetNextSpawnData(next);
        }
        // SpawnData.LIST_CODEC (WeightedList.codec: a list of {data, weight}
        // with weight a required non-negative int). One bad entry fails the
        // whole list, and a failed read is an absent one.
        bool potentialsRead = false;
        if (auto list = std::dynamic_pointer_cast<NBTTagList>(tag.GetTag("SpawnPotentials"))) {
            std::vector<WeightedSpawnData> potentials;
            potentialsRead = true;
            for (const NBTTagPtr& element : list->value) {
                auto entry = std::dynamic_pointer_cast<NBTTagCompound>(element);
                auto data = entry ? std::dynamic_pointer_cast<NBTTagCompound>(entry->GetTag("data")) : nullptr;
                WeightedSpawnData weighted;
                if (!data || !ReadSpawnData(*data, weighted.data) ||
                    !IsNumeric(entry->GetTag("weight"))) {
                    potentialsRead = false;
                    break;
                }
                weighted.weight = NumberOr(*entry, "weight", 0);
                if (weighted.weight < 0) {
                    potentialsRead = false;
                    break;
                }
                potentials.push_back(std::move(weighted));
            }
            if (potentialsRead) spawner.SetSpawnPotentials(std::move(potentials), /*present=*/true);
        }
        if (!potentialsRead) spawner.SetSpawnPotentials({}, /*present=*/false);
        spawner.SetConfig(NumberOr(tag, "MinSpawnDelay", SpawnerBlockEntity::kDefaultMinSpawnDelay),
                          NumberOr(tag, "MaxSpawnDelay", SpawnerBlockEntity::kDefaultMaxSpawnDelay),
                          NumberOr(tag, "SpawnCount", SpawnerBlockEntity::kDefaultSpawnCount),
                          NumberOr(tag, "MaxNearbyEntities", SpawnerBlockEntity::kDefaultMaxNearbyEntities),
                          NumberOr(tag, "RequiredPlayerRange", SpawnerBlockEntity::kDefaultRequiredPlayerRange),
                          NumberOr(tag, "SpawnRange", SpawnerBlockEntity::kDefaultSpawnRange));
    }

    void WriteSpawner(Nbt::Writer& w, const SpawnerBlockEntity& spawner) {
        // MC BaseSpawner.save — every scalar a short.
        w.Short("Delay", static_cast<int16_t>(spawner.GetSpawnDelay()));
        w.Short("MinSpawnDelay", static_cast<int16_t>(spawner.GetMinSpawnDelay()));
        w.Short("MaxSpawnDelay", static_cast<int16_t>(spawner.GetMaxSpawnDelay()));
        w.Short("SpawnCount", static_cast<int16_t>(spawner.GetSpawnCount()));
        w.Short("MaxNearbyEntities", static_cast<int16_t>(spawner.GetMaxNearbyEntities()));
        w.Short("RequiredPlayerRange", static_cast<int16_t>(spawner.GetRequiredPlayerRange()));
        w.Short("SpawnRange", static_cast<int16_t>(spawner.GetSpawnRange()));
        if (spawner.HasNextSpawnData()) {      // storeNullable
            w.BeginCompound("SpawnData");
            WriteSpawnDataBody(w, spawner.GetNextSpawnData());
            w.EndCompound();
        }
        auto list = w.BeginList("SpawnPotentials", Nbt::TagType::Compound);
        for (const WeightedSpawnData& weighted : spawner.GetSpawnPotentials()) {
            w.ListCompoundBegin(list);
            w.BeginCompound("data");
            WriteSpawnDataBody(w, weighted.data);
            w.EndCompound();
            w.Int("weight", weighted.weight);
            w.ListCompoundEnd(list);
        }
        w.EndList(list);
    }

    void InstallSpawnerServerHooks() {
        SpawnerServerHooks hooks;
        hooks.loadEntity = &LoadSpawnerEntity;
        hooks.setEntityId = &SetSpawnerEntityId;
        hooks.checkSpawnRules = &CheckSpawnerSpawnRules;
        hooks.addFreshEntity = &AddSpawnerEntity;
        SetSpawnerServerHooks(hooks);
    }

} // namespace Game::Anvil
