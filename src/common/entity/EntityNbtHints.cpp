// File: src/common/entity/EntityNbtHints.cpp
#include "common/entity/EntityNbtHints.hpp"
#include "common/entity/decoration/PaintingVariants.hpp"
#include "common/entity/mobs/TropicalFishVariant.hpp"
#include "common/entity/mobs/WolfVariants.hpp"
#include "common/entity/npc/VillagerData.hpp"

#include <algorithm>
#include <initializer_list>
#include <string_view>

namespace Game::EntityNbtHints {

    namespace {

        using T = EntityTypeId;

        const std::vector<std::string> kFlag  = {"1b", "0b"};
        const std::vector<std::string> kTrue  = {"1b"};

        // MC DyeColor order — the byte ids CollarColor / Color take.
        constexpr const char* kDyeNames[16] = {
            "white", "orange", "magenta", "light_blue", "yellow", "lime", "pink", "gray",
            "light_gray", "cyan", "purple", "blue", "brown", "green", "red", "black",
        };

        std::string Quoted(std::string_view s) { return "\"" + std::string(s) + "\""; }

        std::vector<std::string> QuotedAll(std::initializer_list<const char*> names) {
            std::vector<std::string> out;
            for (const char* n : names) out.push_back(Quoted(n));
            return out;
        }

        // Dye ids as bytes ("0b" white … "15b" black).
        std::vector<std::string> DyeBytes() {
            std::vector<std::string> out;
            for (int i = 0; i < 16; ++i) out.push_back(std::to_string(i) + "b");
            return out;
        }

        std::vector<std::string> DyeNamesQuoted() {
            std::vector<std::string> out;
            for (const char* n : kDyeNames) out.push_back(Quoted(n));
            return out;
        }

        std::vector<std::string> Range(int from, int to, const char* suffix = "") {
            std::vector<std::string> out;
            for (int i = from; i <= to; ++i) out.push_back(std::to_string(i) + suffix);
            return out;
        }

        std::vector<std::string> VillagerDataValues() {
            std::vector<std::string> out;
            // Every biome type as a plain villager, then every profession in
            // the plains.
            for (int t = 0; t < kVillagerTypeCount; ++t) {
                out.push_back("{type:\"" + std::string(VillagerTypeId(static_cast<VillagerType>(t))) +
                              "\",profession:\"none\",level:1}");
            }
            for (int p = 1; p < kVillagerProfessionCount; ++p) {
                out.push_back("{type:\"plains\",profession:\"" +
                              std::string(VillagerProfessionId(static_cast<VillagerProfession>(p))) +
                              "\",level:1}");
            }
            return out;
        }

        bool Is(T type, std::initializer_list<T> set) {
            return std::find(set.begin(), set.end(), type) != set.end();
        }

        // Types that are not LivingEntities in MC (projectiles, decorations,
        // block entities): no Health / effects / Mob keys.
        bool IsNonLiving(T type) {
            return Is(type, {T::Arrow, T::BreezeWindCharge, T::DragonFireball, T::Egg, T::Fireball,
                             T::LlamaSpit, T::ShulkerBullet, T::SmallFireball, T::Snowball,
                             T::SplashPotion, T::Trident, T::WindCharge, T::WitherSkull,
                             T::EvokerFangs, T::AreaEffectCloud, T::EyeOfEnder, T::FallingBlock,
                             T::Tnt, T::EndCrystal, T::EnderPearl, T::LightningBolt, T::Painting,
                             T::ItemFrame, T::GlowItemFrame, T::Cushion, T::LeashKnot,
                             T::FireworkRocket});
        }

        // The vanilla AgeableMobs (Age / ForcedAge / AgeLocked).
        bool IsAgeable(T type) {
            return Is(type, {T::Cow, T::Pig, T::Sheep, T::Chicken, T::Armadillo, T::Axolotl, T::Bee,
                             T::Camel, T::CamelHusk, T::Cat, T::Dolphin, T::Donkey, T::Fox, T::Goat,
                             T::GlowSquid, T::HappyGhast, T::Hoglin, T::Horse, T::Llama, T::Mooshroom,
                             T::Mule, T::Nautilus, T::Ocelot, T::Panda, T::Parrot, T::PolarBear,
                             T::Rabbit, T::SkeletonHorse, T::Sniffer, T::Squid, T::Strider,
                             T::TraderLlama, T::Turtle, T::Villager, T::WanderingTrader, T::Wolf,
                             T::ZombieHorse, T::ZombieNautilus, T::SulfurCube});
        }

    } // namespace

    std::vector<Key> KeysFor(EntityTypeId type) {
        std::vector<Key> keys;
        const auto add = [&](const char* name, std::vector<std::string> values) {
            keys.push_back(Key{name, std::move(values)});
        };

        // ── Entity (every type) ─────────────────────────────────────────────
        add("CustomName",        {"\"Name\""});
        add("CustomNameVisible", kTrue);
        add("Invulnerable",      kTrue);
        add("NoGravity",         kTrue);
        add("Silent",            kTrue);
        add("Motion",            {"[0.0,0.5,0.0]"});
        add("Rotation",          {"[0.0f,0.0f]", "[90.0f,0.0f]", "[180.0f,0.0f]", "[270.0f,0.0f]"});
        add("Fire",              {"100s"});

        const bool living = !IsNonLiving(type);
        if (living) {
            // ── LivingEntity ────────────────────────────────────────────────
            add("Health",           {"20.0f", "1.0f"});
            add("AbsorptionAmount", {"4.0f"});
            add("active_effects",   {"[{id:\"speed\",amplifier:1b,duration:600}]",
                                     "[{id:\"glowing\",duration:-1}]",
                                     "[{id:\"invisibility\",duration:-1,show_particles:0b}]"});
            add("attributes",       {"[{id:\"max_health\",base:40.0}]",
                                     "[{id:\"movement_speed\",base:0.4}]",
                                     "[{id:\"scale\",base:2.0}]"});
            if (type != T::ArmorStand) {
                // ── Mob ─────────────────────────────────────────────────────
                add("NoAI",                kTrue);
                add("PersistenceRequired", kTrue);
                add("LeftHanded",          kTrue);
                add("CanPickUpLoot",       kTrue);
            }
        }
        if (IsAgeable(type)) {
            add("Age",       {"-24000", "0", "6000"});
            add("ForcedAge", {"-24000"});
            add("AgeLocked", kTrue);
        }
        if (Is(type, {T::Wolf, T::Cat, T::Parrot})) add("Sitting", kTrue);

        // ── the type's own ──────────────────────────────────────────────────
        switch (type) {
            case T::Cow:
            case T::Pig:
                add("variant", QuotedAll({"temperate", "warm", "cold"}));
                break;
            case T::Chicken:
                add("variant", QuotedAll({"temperate", "warm", "cold"}));
                add("IsChickenJockey", kTrue);
                break;
            case T::Sheep:
                add("Color", DyeBytes());
                add("Sheared", kTrue);
                break;
            case T::Wolf: {
                std::vector<std::string> coats, sounds;
                for (uint8_t i = 0; i < WolfVariants::kCount; ++i) {
                    coats.push_back(Quoted(WolfVariants::Name(static_cast<WolfVariants::Variant>(i))));
                }
                for (uint8_t i = 0; i < WolfSoundVariants::kCount; ++i) {
                    sounds.push_back(Quoted(WolfSoundVariants::Name(static_cast<WolfSoundVariants::SoundVariant>(i))));
                }
                add("variant", std::move(coats));
                add("sound_variant", std::move(sounds));
                add("CollarColor", DyeBytes());
                break;
            }
            case T::Cat:
                add("variant", QuotedAll({"tabby", "black", "red", "siamese", "british_shorthair", "calico",
                                          "persian", "ragdoll", "white", "jellie", "all_black"}));
                add("CollarColor", DyeBytes());
                break;
            case T::Parrot:
                add("Variant", Range(0, 4));   // red_blue, blue, green, yellow_blue, gray
                break;
            case T::Axolotl:
                add("Variant", Range(0, 4));   // lucy, wild, gold, cyan, blue
                break;
            case T::Fox:
                add("Type", QuotedAll({"red", "snow"}));
                add("Sleeping", kTrue);
                add("Crouching", kTrue);
                add("Sitting", kTrue);
                break;
            case T::Panda: {
                const auto genes = QuotedAll({"normal", "lazy", "worried", "playful", "brown", "weak", "aggressive"});
                add("MainGene", genes);
                add("HiddenGene", genes);
                break;
            }
            case T::Rabbit:
                add("MoreCarrotTicks", {"40"});
                break;
            case T::Ocelot:
                add("Trusting", kTrue);
                break;
            case T::Llama:
            case T::TraderLlama:
                add("Strength", Range(1, 5));
                // MC Llama.Variant.LEGACY_CODEC: creamy, white, brown, gray.
                add("Variant", Range(0, 3));
                break;
            case T::Turtle:
                add("has_egg", kTrue);
                break;
            case T::Horse:
            case T::Donkey:
            case T::Mule:
            case T::SkeletonHorse:
            case T::ZombieHorse:
                add("Tame", kTrue);
                add("Temper", {"0", "100"});
                add("EatingHaystack", kTrue);
                break;
            case T::Bat:
                add("BatFlags", {"1b"});
                break;
            case T::Bee:
                add("HasNectar", kTrue);
                add("HasStung", kTrue);
                break;
            case T::Dolphin:
                add("Moistness", {"2400"});
                break;
            case T::Pufferfish:
                add("PuffState", Range(0, 2));
                add("FromBucket", kTrue);
                break;
            case T::Cod:
                add("FromBucket", kTrue);
                break;
            case T::Salmon:
                // MC Salmon "type" — Salmon.Variant's names.
                add("type", QuotedAll({"small", "medium", "large"}));
                add("FromBucket", kTrue);
                break;
            case T::TropicalFish: {
                // MC TropicalFish "Variant" — the packed int (pattern id |
                // base colour << 16 | pattern colour << 24). The 22 named
                // COMMON_VARIANTS first, then every pattern in white.
                std::vector<std::string> variants;
                for (const TropicalFishVariants::Variant& v : TropicalFishVariants::CommonVariants()) {
                    variants.push_back(std::to_string(TropicalFishVariants::Pack(v)));
                }
                for (int p = 0; p < TropicalFishVariants::kPatternCount; ++p) {
                    const TropicalFishVariants::Variant v{
                        static_cast<TropicalFishVariants::Pattern>(p), 0, 0 };
                    const std::string packed = std::to_string(TropicalFishVariants::Pack(v));
                    if (std::find(variants.begin(), variants.end(), packed) == variants.end()) {
                        variants.push_back(packed);
                    }
                }
                add("Variant", std::move(variants));
                add("FromBucket", kTrue);
                break;
            }
            case T::Armadillo:
                add("state", QuotedAll({"idle", "rolling", "scared", "unrolling"}));
                break;
            case T::SnowGolem:
                add("Pumpkin", {"0b", "1b"});
                break;
            case T::Slime:
            case T::MagmaCube:
                add("Size", {"0", "1", "3", "7"});
                break;
            case T::SulfurCube:
                add("Size", {"0", "1", "3"});
                add("fuse", {"30"});
                break;
            case T::Phantom:
                add("size", Range(0, 5));
                break;
            case T::Creeper:
                add("ignited", kTrue);
                break;
            case T::Enderman:
                add("carriedBlockState", {"{Name:\"grass_block\"}", "{Name:\"tnt\"}"});
                break;
            case T::Shulker:
                add("AttachFace", Range(0, 5, "b"));
                add("Peek", {"0b", "30b", "100b"});
                break;
            case T::Endermite:
                add("Lifetime", {"0", "2300"});
                break;
            // MC FireworkRocketEntity: Life / LifeTime / FireworksItem /
            // ShotAtAngle.
            case T::FireworkRocket:
                add("LifeTime", {"20", "30", "40"});
                add("Life", {"0"});
                add("ShotAtAngle", kTrue);
                add("FireworksItem", {"{id:\"firework_rocket\",count:1,components:{\"minecraft:fireworks\":"
                                      "{flight_duration:1b,explosions:[{shape:\"large_ball\",colors:[I;11743532],"
                                      "has_trail:1b}]}}}"});
                break;
            case T::Vex:
                add("life_ticks", {"100"});
                break;
            case T::Ghast:
                add("ExplosionPower", {"1b", "4b"});
                break;
            case T::Wither:
                add("Invul", {"0", "220"});
                break;
            case T::Zombie:
            case T::Husk:
            case T::Drowned:
            case T::ZombifiedPiglin:
                add("IsBaby", kTrue);
                add("CanBreakDoors", kTrue);
                break;
            case T::ZombieVillager:
                add("IsBaby", kTrue);
                add("CanBreakDoors", kTrue);
                add("VillagerData", VillagerDataValues());
                break;
            case T::Villager:
                add("VillagerData", VillagerDataValues());
                add("Xp", {"0", "250"});
                break;
            case T::Piglin:
                add("IsBaby", kTrue);
                add("IsImmuneToZombification", kTrue);
                add("CannotHunt", kTrue);
                break;
            case T::PiglinBrute:
                add("IsImmuneToZombification", kTrue);
                break;
            case T::Zoglin:
                add("IsBaby", kTrue);
                break;
            case T::Tnt:
                add("fuse", {"80", "20", "1"});
                add("explosion_power", {"4.0f", "8.0f"});
                add("block_state", {"{Name:\"tnt\"}"});
                break;
            case T::FallingBlock:
                add("BlockState", {"{Name:\"sand\"}", "{Name:\"anvil\"}", "{Name:\"gravel\"}"});
                add("DropItem", {"0b"});
                add("HurtEntities", kTrue);
                add("Time", {"1"});
                break;
            case T::Painting: {
                std::vector<std::string> variants;
                for (const PaintingVariant& v : PaintingVariants::All()) {
                    std::string_view id = v.id;
                    if (id.substr(0, 10) == "minecraft:") id.remove_prefix(10);
                    variants.push_back(Quoted(id));
                }
                add("variant", std::move(variants));
                add("facing", Range(0, 3, "b"));   // south, west, north, east
                break;
            }
            case T::ItemFrame:
            case T::GlowItemFrame:
                add("Item", {"{id:\"diamond\",count:1}"});
                add("ItemRotation", Range(0, 7, "b"));
                add("Facing", Range(0, 5, "b"));
                add("Invisible", kTrue);
                add("Fixed", kTrue);
                break;
            case T::ArmorStand:
                add("Invisible", kTrue);
                add("Small", kTrue);
                add("ShowArms", kTrue);
                add("NoBasePlate", kTrue);
                add("Marker", kTrue);
                add("Pose", {"{Head:[0f,0f,0f],RightArm:[-90f,0f,0f]}"});
                break;
            case T::EndCrystal:
                add("ShowBottom", {"0b"});
                add("beam_target", {"[I;0,64,0]"});
                break;
            case T::Cushion:
                add("color", DyeNamesQuoted());
                break;
            // ── mod mobs (their LoadModNbt keys) ────────────────────────────
            case T::TinyBird:
                add("variant", QuotedAll({"twilightforest:blue", "twilightforest:brown",
                                          "twilightforest:gold", "twilightforest:red"}));
                break;
            case T::DwarfRabbit:
                add("variant", QuotedAll({"twilightforest:brown", "twilightforest:dutch",
                                          "twilightforest:white"}));
                break;
            case T::BighornSheep:
                add("Color", DyeBytes());
                add("Sheared", kTrue);
                break;
            case T::Sheepuff:
                add("Color", DyeBytes());
                add("Sheared", kTrue);
                add("Puffed", kTrue);
                break;
            case T::Moa:
                add("MoaType", QuotedAll({"aether:blue", "aether:white", "aether:black"}));
                add("IsBaby", kTrue);
                add("Saddled", kTrue);
                break;
            case T::Phyg:
            case T::FlyingCow:
                add("Saddled", kTrue);
                break;
            case T::HelmetCrab:
                add("blue", kTrue);
                break;
            case T::Troll:
                add("HasRock", kTrue);
                break;
            case T::UpperGoblinKnight:
                add("hasArmor", kTrue);
                add("hasShield", kTrue);
                break;
            case T::LowerGoblinKnight:
                add("hasArmor", kTrue);
                break;
            default:
                break;
        }

        std::sort(keys.begin(), keys.end(), [](const Key& a, const Key& b) { return a.name < b.name; });
        return keys;
    }

} // namespace Game::EntityNbtHints
