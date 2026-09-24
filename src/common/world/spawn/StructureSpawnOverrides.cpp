// File: src/common/world/spawn/StructureSpawnOverrides.cpp
#include "common/world/spawn/StructureSpawnOverrides.hpp"

#include "common/core/Log.hpp"
#include "common/entity/EntityType.hpp"
#include "common/world/chunk/Chunk.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Game::StructureSpawns {

    namespace {

        // Same rule as DataTags and the terrain library: MC_DATA_ROOT, else ./data.
        std::filesystem::path DataRoot() {
            if (const char* env = std::getenv("MC_DATA_ROOT")) return env;
            return "data";
        }

        // MobCategory.getSerializedName, plus the Aether's enumextensions
        // names (MobCategory.hpp's appended rows).
        bool CategoryFromName(std::string_view name, MobCategory& out) {
            struct Row { std::string_view name; MobCategory category; };
            static constexpr Row kRows[] = {
                { "monster",                        MobCategory::Monster },
                { "creature",                       MobCategory::Creature },
                { "ambient",                        MobCategory::Ambient },
                { "axolotls",                       MobCategory::Axolotls },
                { "underground_water_creature",     MobCategory::UndergroundWaterCreature },
                { "water_creature",                 MobCategory::WaterCreature },
                { "water_ambient",                  MobCategory::WaterAmbient },
                { "misc",                           MobCategory::Misc },
                { "aether:aether_surface_monster",  MobCategory::AetherSurfaceMonster },
                { "aether:aether_darkness_monster", MobCategory::AetherDarknessMonster },
                { "aether:aether_sky_monster",      MobCategory::AetherSkyMonster },
                { "aether:aether_aerwhale",         MobCategory::AetherAerwhale },
            };
            if (name.rfind("minecraft:", 0) == 0) name.remove_prefix(10);
            for (const Row& row : kRows) {
                if (row.name == name) { out = row.category; return true; }
            }
            return false;
        }

        // "minecraft:witch" / "witch" -> EntityTypeId, by the generated slug
        // table (the same index the Anvil reader uses).
        bool EntityTypeFromId(std::string_view id, EntityTypeId& out) {
            static const std::unordered_map<std::string, EntityTypeId> index = [] {
                std::unordered_map<std::string, EntityTypeId> map;
                for (uint16_t i = 0; i < static_cast<uint16_t>(EntityTypeId::Count); ++i) {
                    const auto type = static_cast<EntityTypeId>(i);
                    const std::string_view slug = GetEntityTypeInfo(type).slug;
                    if (!slug.empty()) map.emplace(std::string(slug), type);
                }
                return map;
            }();
            if (const size_t colon = id.find(':'); colon != std::string_view::npos) {
                id.remove_prefix(colon + 1);
            }
            const auto it = index.find(std::string(id));
            if (it == index.end()) return false;
            out = it->second;
            return true;
        }

        // The engine's district extension (see the header): a named part of a
        // structure, as template-local boxes of its pieces, with its own list.
        struct District {
            struct Area {
                std::string template_;   // jigsaw element location
                glm::ivec3  lo{0}, hi{0}; // template-local, inclusive
            };
            std::string name;
            std::vector<Area> areas;
            std::vector<MobSpawnEntry> entries;
            BiomeSpawnList list{};
        };

        // One StructureSpawnOverride: its box rule and its spawn list, viewed
        // as a BiomeSpawnList so the spawner's weighted pick and canSpawnMobAt
        // test run over it unchanged.
        struct Override {
            bool pieceBox = false;                 // BoundingBoxType.PIECE
            std::vector<MobSpawnEntry> entries;
            BiomeSpawnList list{};
            std::vector<District> districts;       // "obeycraft:districts"
        };

        struct StructureOverrides {
            Override byCategory[kMobCategoryCount];
            bool has[kMobCategoryCount] = {};
        };

        // Twilight Forest ControlledSpawns.ControlledSpawningConfig — the
        // structure JSON's "controlled_spawns": monster lists by label (a
        // piece's spawn index), plus the ambient and water-creature lists.
        struct Controlled {
            std::string structureType;   // "twilightforest:hollow_hill" (the validator)
            std::unordered_map<std::string, std::vector<MobSpawnEntry>> monsters;
            std::vector<MobSpawnEntry> ambient;
            std::vector<MobSpawnEntry> water;
        };

        struct Table {
            std::unordered_map<std::string, StructureOverrides> byStructure;
            std::unordered_map<std::string, Controlled> controlled;
            // NetherFortressStructure.FORTRESS_ENEMIES — a code constant in
            // MC, not the fortress JSON (they hold the same five entries).
            std::vector<MobSpawnEntry> fortressEntries;
            BiomeSpawnList fortressList{};
        };

        // A StructureSpawnOverride's "spawns" (MobSpawnSettings.SpawnerData:
        // type, weight, minCount, maxCount).
        void ReadSpawns(const nlohmann::json& body, MobCategory category,
                        std::vector<MobSpawnEntry>& out) {
            const auto spawns = body.find("spawns");
            if (spawns == body.end() || !spawns->is_array()) return;
            for (const auto& spawn : *spawns) {
                EntityTypeId type{};
                if (!spawn.is_object() || !EntityTypeFromId(spawn.value("type", std::string()), type)) {
                    continue;   // a mob this build lacks: MC would spawn it; we cannot
                }
                MobSpawnEntry entry{};
                entry.category = category;
                entry.type = type;
                entry.weight = spawn.value("weight", 1);
                entry.minCount = spawn.value("minCount", 1);
                entry.maxCount = spawn.value("maxCount", entry.minCount);
                out.push_back(entry);
            }
        }

        void LoadStructureFile(Table& table, const std::string& structureName,
                               const std::filesystem::path& file) {
            std::ifstream in(file);
            if (!in) return;
            nlohmann::json parsed;
            try {
                in >> parsed;
            } catch (const std::exception& e) {
                Log::Warning("[StructureSpawns] %s: %s", file.string().c_str(), e.what());
                return;
            }
            // Twilight Forest landmarks: ControlledSpawningConfig's codec.
            if (const auto cs = parsed.find("controlled_spawns"); cs != parsed.end() && cs->is_object()) {
                Controlled& controlled = table.controlled[structureName];
                controlled.structureType = parsed.value("type", std::string());
                if (const auto labelled = cs->find("labelled_monster_spawns");
                    labelled != cs->end() && labelled->is_object()) {
                    for (const auto& [label, list] : labelled->items()) {
                        const nlohmann::json wrapper = { { "spawns", list } };
                        ReadSpawns(wrapper, MobCategory::Monster, controlled.monsters[label]);
                    }
                }
                ReadSpawns(nlohmann::json{ { "spawns", cs->value("ambient_spawns", nlohmann::json::array()) } },
                           MobCategory::Ambient, controlled.ambient);
                ReadSpawns(nlohmann::json{ { "spawns", cs->value("water_spawns", nlohmann::json::array()) } },
                           MobCategory::WaterCreature, controlled.water);
            }

            const auto it = parsed.find("spawn_overrides");
            if (it == parsed.end() || !it->is_object() || it->empty()) return;

            // The map's own key backs the list's name view: node-based, so it
            // stays put as later files are inserted.
            const auto slot = table.byStructure.try_emplace(structureName).first;
            const std::string& key = slot->first;
            StructureOverrides& overrides = slot->second;
            for (const auto& [categoryName, body] : it->items()) {
                MobCategory category{};
                if (!CategoryFromName(categoryName, category) || !body.is_object()) continue;
                const size_t index = static_cast<size_t>(category);
                Override& out = overrides.byCategory[index];
                overrides.has[index] = true;
                out.pieceBox = body.value("bounding_box", std::string("piece")) == "piece";
                ReadSpawns(body, category, out.entries);
                if (const auto districts = body.find("obeycraft:districts");
                    districts != body.end() && districts->is_array()) {
                    for (const auto& d : *districts) {
                        if (!d.is_object()) continue;
                        District district;
                        district.name = d.value("name", std::string());
                        if (const auto areas = d.find("areas"); areas != d.end() && areas->is_array()) {
                            for (const auto& a : *areas) {
                                const auto box = a.find("box");
                                if (!a.is_object() || box == a.end() || !box->is_array() || box->size() != 6) {
                                    continue;
                                }
                                District::Area area;
                                area.template_ = a.value("template", std::string());
                                area.lo = glm::ivec3((*box)[0].get<int>(), (*box)[1].get<int>(), (*box)[2].get<int>());
                                area.hi = glm::ivec3((*box)[3].get<int>(), (*box)[4].get<int>(), (*box)[5].get<int>());
                                district.areas.push_back(std::move(area));
                            }
                        }
                        ReadSpawns(d, category, district.entries);
                        out.districts.push_back(std::move(district));
                    }
                }
            }
            for (size_t i = 0; i < kMobCategoryCount; ++i) {
                Override& o = overrides.byCategory[i];
                o.list = BiomeSpawnList{ key, o.entries.data(),
                                         static_cast<int>(o.entries.size()), nullptr, 0 };
                for (District& d : o.districts) {
                    d.list = BiomeSpawnList{ key, d.entries.data(),
                                             static_cast<int>(d.entries.size()), nullptr, 0 };
                }
            }
        }

        const Table& GetTable() {
            static Table table;
            static std::once_flag once;
            std::call_once(once, [] {
                const std::filesystem::path root = DataRoot();
                std::error_code ec;
                if (std::filesystem::is_directory(root, ec)) {
                    for (const auto& ns : std::filesystem::directory_iterator(root, ec)) {
                        if (!ns.is_directory()) continue;
                        const std::string nsName = ns.path().filename().string();
                        const std::filesystem::path dir = ns.path() / "worldgen" / "structure";
                        if (!std::filesystem::is_directory(dir, ec)) continue;
                        for (const auto& file : std::filesystem::directory_iterator(dir, ec)) {
                            if (file.path().extension() != ".json") continue;
                            LoadStructureFile(table, nsName + ":" + file.path().stem().string(),
                                              file.path());
                        }
                    }
                }

                // BLAZE 2-3 x10, ZOMBIFIED_PIGLIN 4 x5, WITHER_SKELETON 5 x8,
                // SKELETON 5 x2, MAGMA_CUBE 4 x3 — in MC's builder order.
                const MobSpawnEntry fortress[] = {
                    { MobCategory::Monster, EntityTypeId::Blaze,           10, 2, 3 },
                    { MobCategory::Monster, EntityTypeId::ZombifiedPiglin,  5, 4, 4 },
                    { MobCategory::Monster, EntityTypeId::WitherSkeleton,   8, 5, 5 },
                    { MobCategory::Monster, EntityTypeId::Skeleton,         2, 5, 5 },
                    { MobCategory::Monster, EntityTypeId::MagmaCube,        3, 4, 4 },
                };
                table.fortressEntries.assign(std::begin(fortress), std::end(fortress));
                table.fortressList = BiomeSpawnList{ "minecraft:fortress",
                                                     table.fortressEntries.data(),
                                                     static_cast<int>(table.fortressEntries.size()),
                                                     nullptr, 0 };
                Log::Info("[StructureSpawns] %zu structures carry spawn overrides",
                          table.byStructure.size());
            });
            return table;
        }

        bool Inside(const glm::ivec3& lo, const glm::ivec3& hi, int x, int y, int z) {
            return x >= lo.x && x <= hi.x && y >= lo.y && y <= hi.y && z >= lo.z && z <= hi.z;
        }

        // StructureTemplate.transform(pos, NONE, rotation, ZERO) — the jigsaw
        // placement of a rigid piece (pivot at the template origin, no mirror).
        glm::ivec2 RotateXZ(int x, int z, uint8_t rotation) {
            switch (rotation & 3) {
                case 1:  return glm::ivec2(-z, x);    // CLOCKWISE_90
                case 2:  return glm::ivec2(-x, -z);   // CLOCKWISE_180
                case 3:  return glm::ivec2(z, -x);    // COUNTERCLOCKWISE_90
                default: return glm::ivec2(x, z);
            }
        }

        // Is (x, y, z) inside `area`, a template-local box of `piece`? The
        // piece's world box is its rotated template, so the template origin
        // is the box corner the rotation carries (0, 0) to.
        bool InsideArea(const StructureSpawnArea::Piece& piece, const District::Area& area,
                        int x, int y, int z) {
            if (piece.templateId != area.template_) return false;
            glm::ivec3 origin(piece.min.x, piece.min.y, piece.min.z);
            switch (piece.rotation & 3) {
                case 1: origin.x = piece.max.x; break;                          // CW90
                case 2: origin.x = piece.max.x; origin.z = piece.max.z; break;  // CW180
                case 3: origin.z = piece.max.z; break;                          // CCW90
                default: break;
            }
            const glm::ivec2 a = RotateXZ(area.lo.x, area.lo.z, piece.rotation);
            const glm::ivec2 b = RotateXZ(area.hi.x, area.hi.z, piece.rotation);
            const int x0 = origin.x + std::min(a.x, b.x), x1 = origin.x + std::max(a.x, b.x);
            const int z0 = origin.z + std::min(a.y, b.y), z1 = origin.z + std::max(a.y, b.y);
            const int y0 = origin.y + area.lo.y, y1 = origin.y + area.hi.y;
            return x >= x0 && x <= x1 && y >= y0 && y <= y1 && z >= z0 && z <= z1;
        }

        // TF SpawnIndexProvider.getSpawnIndex for the pieces this port places,
        // by StructurePieceType id; nullopt for a piece that is no provider
        // (it does not take part in getSpawnListIndexAt). HollowHillComponent
        // keeps TFStructureComponent's default 0; the Lich Tower's revamp
        // pieces answer LichTowerPieces.YARD_SPAWNS (0), INTERIOR_SPAWNS (1),
        // EMPTY (2) or SpawnIndexProvider.Deny (-1).
        std::optional<int> TwilightSpawnIndex(const std::string& pieceType) {
            static const std::unordered_map<std::string, int> kIndex = {
                { "twilightforest:tfhill",        0 },   // HollowHillComponent
                { "twilightforest:tfltpath",      0 },   // LichYardBox          YARD_SPAWNS
                { "twilightforest:tfltctbase",    1 },   // LichTowerBase        INTERIOR_SPAWNS
                { "twilightforest:tfltctseg",     1 },   // LichTowerSegment
                { "twilightforest:tflttroom",     1 },   // LichTowerWingRoom
                { "twilightforest:tflttgallery",  1 },   // LichTowerMagicGallery
                { "twilightforest:tflttfoy",      2 },   // LichTowerFoyer       EMPTY
                { "twilightforest:tflttboss",    -1 },   // LichBossRoom         Deny
                { "twilightforest:tflttbossroof", -1 },  // LichBossRoof         Deny
                { "twilightforest:tfltfence",    -1 },   // LichPerimeterFence   Deny
            };
            const auto it = kIndex.find(pieceType);
            if (it == kIndex.end()) return std::nullopt;
            return it->second;
        }

        // TF EntityEvents.gatherPotentialSpawns — what a Twilight Forest
        // landmark adds to the potential spawns at pos (NeoForge's
        // PotentialSpawns event, which runs on mobsAt's list). The first
        // controlled-spawning start referenced here decides, both branches
        // returning: a non-monster category gets that landmark's list for it;
        // a monster gets the list labelled with the highest spawn index of the
        // pieces containing pos (none: nothing). Conquest (TFStructureStart
        // .isConquered — the boss beaten) is not tracked by this port, so a
        // landmark never goes quiet after its boss.
        const std::vector<MobSpawnEntry>* ControlledSpawnsAt(const Table& table, const Chunk& chunk,
                                                             MobCategory category, int x, int y, int z) {
            if (table.controlled.empty()) return nullptr;
            static const std::vector<MobSpawnEntry> kNone;
            for (const StructureSpawnArea& area : chunk.structureSpawnAreas) {
                const auto it = table.controlled.find(area.structure);
                if (it == table.controlled.end()) continue;
                const Controlled& landmark = it->second;

                if (category != MobCategory::Monster) {
                    // ConfigurableSpawns.getSpawnableList.
                    if (category == MobCategory::Ambient) return &landmark.ambient;
                    if (category == MobCategory::WaterCreature) return &landmark.water;
                    return &kNone;
                }

                // HollowHillStructure.canSpawnMob: inside the hill's
                // ellipsoid, measured over the start's box.
                if (landmark.structureType == "twilightforest:hollow_hill") {
                    auto inverseLerp = [](float v, float a, float b) { return (v - a) / (b - a); };
                    const float hX = inverseLerp(static_cast<float>(x), static_cast<float>(area.startMin.x),
                                                 static_cast<float>(area.startMax.x)) * 2.0f - 1.0f;
                    const float hY = inverseLerp(static_cast<float>(y), static_cast<float>(area.startMin.y),
                                                 static_cast<float>(area.startMax.y));
                    const float hZ = inverseLerp(static_cast<float>(z), static_cast<float>(area.startMin.z),
                                                 static_cast<float>(area.startMax.z)) * 2.0f - 1.0f;
                    if (!(std::sqrt(hX * hX + hY * hY + hZ * hZ) < 0.975f)) return nullptr;
                }

                // getSpawnListIndexAt.
                int index = -1;
                for (const StructureSpawnArea::Piece& piece : area.pieces) {
                    if (!Inside(piece.min, piece.max, x, y, z)) continue;
                    if (const std::optional<int> pieceIndex = TwilightSpawnIndex(piece.pieceType);
                        pieceIndex && *pieceIndex > index) {
                        index = *pieceIndex;
                    }
                }
                if (index < 0) return nullptr;
                const auto list = landmark.monsters.find(std::to_string(index));
                return list != landmark.monsters.end() ? &list->second : &kNone;
            }
            return nullptr;
        }

        // mobsAt's list with a landmark's controlled spawns appended, built
        // once per (base list, additions) pair and kept: the spawner holds a
        // pointer to the entry it picked across the whole pack, so the list
        // must outlive the call.
        const BiomeSpawnList* MergedList(const BiomeSpawnList* base,
                                         const std::vector<MobSpawnEntry>& extra) {
            struct Merged {
                std::vector<MobSpawnEntry> entries;
                BiomeSpawnList list{};
            };
            static std::mutex mutex;
            static std::map<std::pair<const BiomeSpawnList*, const std::vector<MobSpawnEntry>*>,
                            std::unique_ptr<Merged>> cache;
            std::lock_guard<std::mutex> lock(mutex);
            auto& slot = cache[{ base, &extra }];
            if (!slot) {
                slot = std::make_unique<Merged>();
                if (base) slot->entries.assign(base->entries, base->entries + base->count);
                slot->entries.insert(slot->entries.end(), extra.begin(), extra.end());
                slot->list = BiomeSpawnList{ base ? base->biome : std::string_view("controlled"),
                                             slot->entries.data(),
                                             static_cast<int>(slot->entries.size()),
                                             base ? base->costs : nullptr, base ? base->costCount : 0 };
            }
            return &slot->list;
        }

    } // namespace

    bool ChunkHasOverrides(const Chunk& chunk) {
        return !chunk.structureSpawnAreas.empty();
    }

    namespace {
        const BiomeSpawnList* VanillaMobsAt(const Table& table, const Chunk& chunk, MobCategory category,
                                            int x, int y, int z, BlockID blockBelow);
    }

    const BiomeSpawnList* MobsAt(const Chunk& chunk, MobCategory category,
                                 int x, int y, int z, BlockID blockBelow,
                                 const BiomeSpawnList* biomeList) {
        if (chunk.structureSpawnAreas.empty()) return nullptr;
        const Table& table = GetTable();
        const BiomeSpawnList* list = VanillaMobsAt(table, chunk, category, x, y, z, blockBelow);
        // The Twilight Forest's landmarks add their controlled spawns on top
        // of whatever mobsAt settled on (an empty override, as their JSONs
        // carry, or the biome's).
        const std::vector<MobSpawnEntry>* extra = ControlledSpawnsAt(table, chunk, category, x, y, z);
        if (!extra || extra->empty()) return list;
        return MergedList(list ? list : biomeList, *extra);
    }

    namespace {
    const BiomeSpawnList* VanillaMobsAt(const Table& table, const Chunk& chunk, MobCategory category,
                                        int x, int y, int z, BlockID blockBelow) {

        // MC NaturalSpawner.isInNetherFortressBounds: MONSTER over nether
        // bricks, inside a fortress START's box (getStructureAt), not a piece.
        if (category == MobCategory::Monster && blockBelow == BlockID::NetherBricks) {
            for (const StructureSpawnArea& area : chunk.structureSpawnAreas) {
                if (area.structure == "minecraft:fortress"
                    && Inside(area.startMin, area.startMax, x, y, z)) {
                    return &table.fortressList;
                }
            }
        }

        // MC ChunkGenerator.getMobsAt — the first structure whose override
        // for this category covers pos wins (its list, even when empty).
        const size_t index = static_cast<size_t>(category);
        for (const StructureSpawnArea& area : chunk.structureSpawnAreas) {
            const auto it = table.byStructure.find(area.structure);
            if (it == table.byStructure.end() || !it->second.has[index]) continue;
            const Override& o = it->second.byCategory[index];
            // The engine's districts first: a named part of the structure
            // with its own list (vanilla overrides carry none).
            for (const District& district : o.districts) {
                for (const StructureSpawnArea::Piece& piece : area.pieces) {
                    if (!Inside(piece.min, piece.max, x, y, z)) continue;
                    for (const District::Area& districtArea : district.areas) {
                        if (InsideArea(piece, districtArea, x, y, z)) return &district.list;
                    }
                }
            }
            bool inside = false;
            if (o.pieceBox) {
                // structureHasPieceAt — any piece box.
                for (const StructureSpawnArea::Piece& piece : area.pieces) {
                    if (Inside(piece.min, piece.max, x, y, z)) {
                        inside = true;
                        break;
                    }
                }
            } else {
                inside = Inside(area.startMin, area.startMax, x, y, z);
            }
            if (inside) return &o.list;
        }
        return nullptr;
    }
    } // namespace

} // namespace Game::StructureSpawns
