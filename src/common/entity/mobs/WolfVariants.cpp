// File: src/common/entity/mobs/WolfVariants.cpp
#include "common/entity/mobs/WolfVariants.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/sound/SoundEvents.hpp"

#include <climits>
#include <iterator>

namespace Game {

    namespace WolfVariants {

        namespace {

            struct Row {
                const char* name;
                const char* texture;
                // The spawn condition: an empty biome list is the
                // priority-0 fallback (pale's `{"priority": 0}`).
                int                     priority;
                const std::string_view* biomes;
                size_t                  biomeCount;
            };

            // data/minecraft/wolf_variant/*.json. The biome TAGS are
            // flattened from data/minecraft/tags/worldgen/biome/ (this engine
            // has no biome-tag resolver): #is_savanna, #is_jungle,
            // #is_badlands.
            constexpr std::string_view kSavanna[]  = { "savanna", "savanna_plateau", "windswept_savanna" };
            constexpr std::string_view kGrove[]    = { "grove" };
            constexpr std::string_view kPine[]     = { "old_growth_pine_taiga" };
            constexpr std::string_view kSnowy[]    = { "snowy_taiga" };
            constexpr std::string_view kJungle[]   = { "bamboo_jungle", "jungle", "sparse_jungle" };
            constexpr std::string_view kForest[]   = { "forest" };
            constexpr std::string_view kSpruce[]   = { "old_growth_spruce_taiga" };
            constexpr std::string_view kBadlands[] = { "badlands", "eroded_badlands", "wooded_badlands" };

            // Indexed by Variant (WolfVariants.bootstrap order).
            constexpr Row kRows[kCount] = {
                { "pale",     "wolf",          0, nullptr,  0 },
                { "spotted",  "wolf_spotted",  1, kSavanna, std::size(kSavanna) },
                { "snowy",    "wolf_snowy",    1, kGrove,   std::size(kGrove) },
                { "black",    "wolf_black",    1, kPine,    std::size(kPine) },
                { "ashen",    "wolf_ashen",    1, kSnowy,   std::size(kSnowy) },
                { "rusty",    "wolf_rusty",    1, kJungle,  std::size(kJungle) },
                { "woods",    "wolf_woods",    1, kForest,  std::size(kForest) },
                { "chestnut", "wolf_chestnut", 1, kSpruce,  std::size(kSpruce) },
                { "striped",  "wolf_striped",  1, kBadlands, std::size(kBadlands) },
            };

            // The datapack registry's listing order (identifier order).
            constexpr Variant kRegistryOrder[kCount] = {
                Variant::Ashen, Variant::Black, Variant::Chestnut, Variant::Pale,
                Variant::Rusty, Variant::Snowy, Variant::Spotted, Variant::Striped,
                Variant::Woods,
            };

            std::string_view StripNamespace(std::string_view id) {
                constexpr std::string_view kNs = "minecraft:";
                if (id.substr(0, kNs.size()) == kNs) id.remove_prefix(kNs.size());
                return id;
            }

            bool Matches(const Row& row, std::string_view biome) {
                if (row.biomeCount == 0) return true;   // the always-true selector
                for (size_t i = 0; i < row.biomeCount; ++i) {
                    if (row.biomes[i] == biome) return true;
                }
                return false;
            }

        } // namespace

        const char* Name(Variant v) {
            const auto i = static_cast<uint8_t>(v);
            return i < kCount ? kRows[i].name : kRows[0].name;
        }

        bool FromName(std::string_view id, Variant& out) {
            id = StripNamespace(id);
            for (uint8_t i = 0; i < kCount; ++i) {
                if (id == kRows[i].name) {
                    out = static_cast<Variant>(i);
                    return true;
                }
            }
            return false;
        }

        const char* TextureBase(Variant v) {
            const auto i = static_cast<uint8_t>(v);
            return i < kCount ? kRows[i].texture : kRows[0].texture;
        }

        std::string TexturePath(Variant v, bool tame, bool angry) {
            std::string path = "assets/textures/entity/wolf/";
            path += TextureBase(v);
            if (tame)       path += "_tame";
            else if (angry) path += "_angry";
            path += ".png";
            return path;
        }

        Variant SelectToSpawn(std::string_view biome, JavaRandom& random) {
            biome = StripNamespace(biome);

            // PriorityProvider.select: every selector, highest priority first
            // (a STABLE sort over the registry listing), dropping each entry
            // below the best matched priority and each that fails its test.
            // Every row carries exactly one selector here.
            Variant sorted[kCount];
            size_t n = 0;
            for (int pass = 1; pass >= 0; --pass) {
                for (Variant v : kRegistryOrder) {
                    if (kRows[static_cast<uint8_t>(v)].priority == pass) sorted[n++] = v;
                }
            }

            Variant kept[kCount];
            size_t keptCount = 0;
            int highestMatched = INT_MIN;
            for (size_t i = 0; i < n; ++i) {
                const Row& row = kRows[static_cast<uint8_t>(sorted[i])];
                if (row.priority < highestMatched) continue;
                if (!Matches(row, biome)) continue;
                highestMatched = row.priority;
                kept[keptCount++] = sorted[i];
            }

            // Util.getRandomSafe: nextInt(size) whenever the list is not
            // empty (pale's fallback guarantees it never is).
            if (keptCount == 0) return kDefault;
            return kept[random.NextInt(static_cast<int32_t>(keptCount))];
        }

    } // namespace WolfVariants

    namespace WolfSoundVariants {

        namespace {

            using namespace Game::SoundEvents;

            // Indexed by SoundVariant — SoundEvents.registerWolfSoundVariants'
            // "entity.<wolf_x>.<kind>" events per set.
            const SoundSet kAdult[kCount] = {
                { ENTITY_WOLF_AMBIENT, ENTITY_WOLF_DEATH, ENTITY_WOLF_GROWL,
                  ENTITY_WOLF_HURT, ENTITY_WOLF_PANT, ENTITY_WOLF_WHINE },
                { ENTITY_WOLF_PUGLIN_AMBIENT, ENTITY_WOLF_PUGLIN_DEATH, ENTITY_WOLF_PUGLIN_GROWL,
                  ENTITY_WOLF_PUGLIN_HURT, ENTITY_WOLF_PUGLIN_PANT, ENTITY_WOLF_PUGLIN_WHINE },
                { ENTITY_WOLF_SAD_AMBIENT, ENTITY_WOLF_SAD_DEATH, ENTITY_WOLF_SAD_GROWL,
                  ENTITY_WOLF_SAD_HURT, ENTITY_WOLF_SAD_PANT, ENTITY_WOLF_SAD_WHINE },
                { ENTITY_WOLF_ANGRY_AMBIENT, ENTITY_WOLF_ANGRY_DEATH, ENTITY_WOLF_ANGRY_GROWL,
                  ENTITY_WOLF_ANGRY_HURT, ENTITY_WOLF_ANGRY_PANT, ENTITY_WOLF_ANGRY_WHINE },
                { ENTITY_WOLF_GRUMPY_AMBIENT, ENTITY_WOLF_GRUMPY_DEATH, ENTITY_WOLF_GRUMPY_GROWL,
                  ENTITY_WOLF_GRUMPY_HURT, ENTITY_WOLF_GRUMPY_PANT, ENTITY_WOLF_GRUMPY_WHINE },
                { ENTITY_WOLF_BIG_AMBIENT, ENTITY_WOLF_BIG_DEATH, ENTITY_WOLF_BIG_GROWL,
                  ENTITY_WOLF_BIG_HURT, ENTITY_WOLF_BIG_PANT, ENTITY_WOLF_BIG_WHINE },
                { ENTITY_WOLF_CUTE_AMBIENT, ENTITY_WOLF_CUTE_DEATH, ENTITY_WOLF_CUTE_GROWL,
                  ENTITY_WOLF_CUTE_HURT, ENTITY_WOLF_CUTE_PANT, ENTITY_WOLF_CUTE_WHINE },
            };

            // Every variant's babySounds.
            const SoundSet kBaby = {
                WOLF_AMBIENT_BABY, WOLF_DEATH_BABY, WOLF_GROWL_BABY,
                WOLF_HURT_BABY, WOLF_PANT_BABY, WOLF_WHINE_BABY,
            };

            constexpr const char* kNames[kCount] = {
                "classic", "puglin", "sad", "angry", "grumpy", "big", "cute",
            };

            // The datapack registry's listing order (identifier order),
            // which is what registry.getRandom indexes.
            constexpr SoundVariant kRegistryOrder[kCount] = {
                SoundVariant::Angry, SoundVariant::Big, SoundVariant::Classic,
                SoundVariant::Cute, SoundVariant::Grumpy, SoundVariant::Puglin,
                SoundVariant::Sad,
            };

        } // namespace

        const SoundSet& Sounds(SoundVariant v, bool baby) {
            if (baby) return kBaby;
            const auto i = static_cast<uint8_t>(v);
            return kAdult[i < kCount ? i : 0];
        }

        const char* Name(SoundVariant v) {
            const auto i = static_cast<uint8_t>(v);
            return kNames[i < kCount ? i : 0];
        }

        bool FromName(std::string_view id, SoundVariant& out) {
            constexpr std::string_view kNs = "minecraft:";
            if (id.substr(0, kNs.size()) == kNs) id.remove_prefix(kNs.size());
            for (uint8_t i = 0; i < kCount; ++i) {
                if (id == kNames[i]) {
                    out = static_cast<SoundVariant>(i);
                    return true;
                }
            }
            return false;
        }

        SoundVariant PickRandom(JavaRandom& random) {
            return kRegistryOrder[random.NextInt(static_cast<int32_t>(kCount))];
        }

        SoundVariant Next(SoundVariant v) {
            return static_cast<SoundVariant>((static_cast<uint8_t>(v) + 1) % kCount);
        }

    } // namespace WolfSoundVariants

} // namespace Game
