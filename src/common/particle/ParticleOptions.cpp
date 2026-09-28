// File: src/common/particle/ParticleOptions.cpp
#include "common/particle/ParticleOptions.hpp"

#include "common/network/PacketRegistry.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>

namespace Game {

    namespace {

        float Channel(uint32_t rgb, int shift) {
            return static_cast<float>((rgb >> shift) & 0xFFu) / 255.0f;
        }

        uint32_t ToByte(float v) {
            return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f) & 0xFFu;
        }

        // MC ScalableParticleOptionsBase: scale is clamped to [0.01, 4].
        float ClampScale(float scale) { return std::clamp(scale, 0.01f, 4.0f); }

    } // namespace

    // ── Option constructors ────────────────────────────────────────────────

    ParticleOptions ParticleOptions::Block(ParticleKind kind, BlockState state) {
        ParticleOptions o(kind);
        o.blockState = state.RawId();
        return o;
    }

    ParticleOptions ParticleOptions::Item(uint32_t itemId) {
        ParticleOptions o(ParticleKind::Item);
        o.itemId = itemId;
        return o;
    }

    ParticleOptions ParticleOptions::Dust(uint32_t rgb, float scale) {
        ParticleOptions o(ParticleKind::Dust);
        o.r = Channel(rgb, 16); o.g = Channel(rgb, 8); o.b = Channel(rgb, 0);
        o.scale = ClampScale(scale);
        return o;
    }

    ParticleOptions ParticleOptions::Dust(const glm::vec3& rgb, float scale) {
        ParticleOptions o(ParticleKind::Dust);
        o.r = rgb.r; o.g = rgb.g; o.b = rgb.b;
        o.scale = ClampScale(scale);
        return o;
    }

    ParticleOptions ParticleOptions::DustColorTransition(uint32_t fromRgb, uint32_t toRgb, float scale) {
        ParticleOptions o(ParticleKind::DustColorTransition);
        o.r = Channel(fromRgb, 16); o.g = Channel(fromRgb, 8); o.b = Channel(fromRgb, 0);
        o.r2 = Channel(toRgb, 16); o.g2 = Channel(toRgb, 8); o.b2 = Channel(toRgb, 0);
        o.scale = ClampScale(scale);
        return o;
    }

    ParticleOptions ParticleOptions::Color(ParticleKind kind, uint32_t argb) {
        ParticleOptions o(kind);
        o.a = Channel(argb, 24);
        o.r = Channel(argb, 16); o.g = Channel(argb, 8); o.b = Channel(argb, 0);
        return o;
    }

    ParticleOptions ParticleOptions::Color(ParticleKind kind, float r, float g, float b) {
        // MC ColorParticleOption.create(type, r, g, b) = ARGB.colorFromFloat(1, r, g, b).
        ParticleOptions o(kind);
        o.r = r; o.g = g; o.b = b; o.a = 1.0f;
        return o;
    }

    ParticleOptions ParticleOptions::Spell(ParticleKind kind, uint32_t rgb, float power) {
        ParticleOptions o(kind);
        o.r = Channel(rgb, 16); o.g = Channel(rgb, 8); o.b = Channel(rgb, 0);
        o.scale = power;
        return o;
    }

    ParticleOptions ParticleOptions::Spell(ParticleKind kind, float r, float g, float b, float power) {
        ParticleOptions o(kind);
        o.r = r; o.g = g; o.b = b;
        o.scale = power;
        return o;
    }

    ParticleOptions ParticleOptions::Power(ParticleKind kind, float power) {
        ParticleOptions o(kind);
        o.scale = power;
        return o;
    }

    ParticleOptions ParticleOptions::VibrationToBlock(const glm::ivec3& pos, int arrivalTicks) {
        // BlockPositionSource.getPosition = Vec3.atCenterOf(pos).
        ParticleOptions o(ParticleKind::Vibration);
        o.target = glm::dvec3(pos) + glm::dvec3(0.5);
        o.targetEntity = -1;
        o.intValue = arrivalTicks;
        return o;
    }

    ParticleOptions ParticleOptions::VibrationToEntity(int32_t entityId, float yOffset, int arrivalTicks) {
        ParticleOptions o(ParticleKind::Vibration);
        o.targetEntity = entityId;
        o.targetEntityYOffset = yOffset;
        o.intValue = arrivalTicks;
        return o;
    }

    ParticleOptions ParticleOptions::Trail(const glm::dvec3& target, uint32_t rgb, int duration) {
        ParticleOptions o(ParticleKind::Trail);
        o.target = target;
        o.r = Channel(rgb, 16); o.g = Channel(rgb, 8); o.b = Channel(rgb, 0);
        o.intValue = std::max(duration, 1);   // ExtraCodecs.POSITIVE_INT
        return o;
    }

    ParticleOptions ParticleOptions::Shriek(int delay) {
        ParticleOptions o(ParticleKind::Shriek);
        o.intValue = delay;
        return o;
    }

    ParticleOptions ParticleOptions::SculkCharge(float roll) {
        ParticleOptions o(ParticleKind::SculkCharge);
        o.scale = roll;
        return o;
    }

    ParticleOptions ParticleOptions::Geyser(ParticleKind kind, int waterBlocks) {
        ParticleOptions o(kind);
        o.intValue = waterBlocks;
        return o;
    }

    ParticleOptions ParticleOptions::GeyserBase(ParticleKind kind, int waterBlocks, float burstImpulseBase) {
        ParticleOptions o(kind);
        o.intValue = waterBlocks;
        o.scale = burstImpulseBase;
        return o;
    }

    ParticleOptions ParticleOptions::FireworkStarter(std::vector<FireworkExplosion> explosions, bool playSound) {
        ParticleOptions o(ParticleKind::FireworkStarter);
        o.fireworkExplosions = std::make_shared<const std::vector<FireworkExplosion>>(std::move(explosions));
        o.fireworkPlaySound = playSound;
        return o;
    }

    uint32_t ParticleOptions::Rgb() const {
        return (ToByte(r) << 16) | (ToByte(g) << 8) | ToByte(b);
    }

    uint32_t ParticleOptions::Argb() const {
        return (ToByte(a) << 24) | Rgb();
    }

    // ── The type table ─────────────────────────────────────────────────────

    namespace ParticleTypes {

        namespace {

            using S = OptionsShape;
            using K = ParticleKind;

            // ParticleKind order. The registry ids and limiter flags are
            // MC 26.3 ParticleTypes.java's; the engine's own kinds carry
            // their mods' / the engine's namespace and MC's flag for the
            // vanilla type each is a recolour of.
            const std::vector<Info>& Table() {
                static const std::vector<Info> table = {
                    {K::Heart,             "minecraft:heart",             false, S::Simple},
                    {K::AngryVillager,     "minecraft:angry_villager",    false, S::Simple},
                    {K::Smoke,             "minecraft:smoke",             false, S::Simple},
                    {K::LargeSmoke,        "minecraft:large_smoke",       false, S::Simple},
                    {K::Poof,              "minecraft:poof",              true,  S::Simple},
                    {K::Explosion,         "minecraft:explosion",         true,  S::Simple},
                    {K::ExplosionEmitter,  "minecraft:explosion_emitter", true,  S::Simple},
                    {K::EntityEffect,      "minecraft:entity_effect",     false, S::Color},
                    {K::WitchMagic,        "minecraft:witch",             false, S::Simple},
                    {K::FallingDust,       "minecraft:falling_dust",      false, S::BlockState},
                    {K::BlockMarker,       "minecraft:block_marker",      true,  S::BlockState},
                    {K::PauseMobGrowth,    "minecraft:pause_mob_growth",  false, S::Simple},
                    {K::ResetMobGrowth,    "minecraft:reset_mob_growth",  false, S::Simple},
                    {K::HushPortal,        "obeycraft:hush_portal",       false, S::Simple},
                    {K::HushMote,          "obeycraft:hush_mote",         false, S::Color},
                    {K::AetherPortal,      "aether:aether_portal",        false, S::Simple},
                    {K::Portal,            "minecraft:portal",            false, S::Simple},
                    {K::Flame,             "minecraft:flame",             false, S::Simple},
                    {K::HushMist,          "obeycraft:hush_mist",         false, S::Simple},
                    {K::VesperGlint,       "obeycraft:vesper_glint",      false, S::Simple},
                    {K::HappyVillager,     "minecraft:happy_villager",    false, S::Simple},
                    {K::SulfurBubbles,     "minecraft:sulfur_bubbles",    false, S::Simple},
                    {K::NoxiousGas,        "minecraft:noxious_gas",       false, S::Simple},
                    {K::NoxiousGasCloud,   "minecraft:noxious_gas_cloud", false, S::Simple},
                    {K::Geyser,            "minecraft:geyser",            true,  S::Geyser},
                    {K::GeyserBase,        "minecraft:geyser_base",       true,  S::GeyserBase},
                    {K::GeyserPoof,        "minecraft:geyser_poof",       true,  S::GeyserBase},
                    {K::GeyserPlume,       "minecraft:geyser_plume",      true,  S::Geyser},
                    {K::Crit,              "minecraft:crit",              false, S::Simple},
                    {K::EnchantedHit,      "minecraft:enchanted_hit",     false, S::Simple},
                    {K::Block,             "minecraft:block",             false, S::BlockState},
                    {K::Bubble,            "minecraft:bubble",            false, S::Simple},
                    {K::Cloud,             "minecraft:cloud",             false, S::Simple},
                    {K::CopperFireFlame,   "minecraft:copper_fire_flame", false, S::Simple},
                    {K::DamageIndicator,   "minecraft:damage_indicator",  true,  S::Simple},
                    {K::DragonBreath,      "minecraft:dragon_breath",     false, S::Power},
                    {K::DrippingLava,      "minecraft:dripping_lava",     false, S::Simple},
                    {K::FallingLava,       "minecraft:falling_lava",      false, S::Simple},
                    {K::LandingLava,       "minecraft:landing_lava",      false, S::Simple},
                    {K::DrippingWater,     "minecraft:dripping_water",    false, S::Simple},
                    {K::FallingWater,      "minecraft:falling_water",     false, S::Simple},
                    {K::Dust,              "minecraft:dust",              false, S::Dust},
                    {K::DustColorTransition, "minecraft:dust_color_transition", false, S::DustTransition},
                    {K::Effect,            "minecraft:effect",            false, S::Spell},
                    {K::ElderGuardian,     "minecraft:elder_guardian",    true,  S::Simple},
                    {K::Enchant,           "minecraft:enchant",           false, S::Simple},
                    {K::EndRod,            "minecraft:end_rod",           false, S::Simple},
                    {K::Gust,              "minecraft:gust",              true,  S::Simple},
                    {K::SmallGust,         "minecraft:small_gust",        false, S::Simple},
                    {K::GustEmitterLarge,  "minecraft:gust_emitter_large", true, S::Simple},
                    {K::GustEmitterSmall,  "minecraft:gust_emitter_small", true, S::Simple},
                    {K::SonicBoom,         "minecraft:sonic_boom",        true,  S::Simple},
                    {K::Firework,          "minecraft:firework",          false, S::Simple},
                    {K::Fishing,           "minecraft:fishing",           false, S::Simple},
                    {K::Infested,          "minecraft:infested",          false, S::Simple},
                    {K::CherryLeaves,      "minecraft:cherry_leaves",     false, S::Simple},
                    {K::PaleOakLeaves,     "minecraft:pale_oak_leaves",   false, S::Simple},
                    {K::RedPoplarLeaves,   "minecraft:red_poplar_leaves", false, S::Simple},
                    {K::OrangePoplarLeaves, "minecraft:orange_poplar_leaves", false, S::Simple},
                    {K::YellowPoplarLeaves, "minecraft:yellow_poplar_leaves", false, S::Simple},
                    {K::TintedLeaves,      "minecraft:tinted_leaves",     false, S::Color},
                    {K::SculkSoul,         "minecraft:sculk_soul",        false, S::Simple},
                    {K::SculkCharge,       "minecraft:sculk_charge",      true,  S::SculkCharge},
                    {K::SculkChargePop,    "minecraft:sculk_charge_pop",  true,  S::Simple},
                    {K::SoulFireFlame,     "minecraft:soul_fire_flame",   false, S::Simple},
                    {K::Soul,              "minecraft:soul",              false, S::Simple},
                    {K::Flash,             "minecraft:flash",             false, S::Color},
                    {K::Composter,         "minecraft:composter",         false, S::Simple},
                    {K::InstantEffect,     "minecraft:instant_effect",    false, S::Spell},
                    {K::Item,              "minecraft:item",              false, S::Item},
                    {K::Vibration,         "minecraft:vibration",         true,  S::Vibration},
                    {K::Trail,             "minecraft:trail",             false, S::Trail},
                    {K::ItemSlime,         "minecraft:item_slime",        false, S::Simple},
                    {K::ItemCobweb,        "minecraft:item_cobweb",       false, S::Simple},
                    {K::ItemSnowball,      "minecraft:item_snowball",     false, S::Simple},
                    {K::Lava,              "minecraft:lava",              false, S::Simple},
                    {K::Mycelium,          "minecraft:mycelium",          false, S::Simple},
                    {K::Note,              "minecraft:note",              false, S::Simple},
                    {K::Rain,              "minecraft:rain",              false, S::Simple},
                    {K::WhiteSmoke,        "minecraft:white_smoke",       false, S::Simple},
                    {K::Sneeze,            "minecraft:sneeze",            false, S::Simple},
                    {K::Spit,              "minecraft:spit",              true,  S::Simple},
                    {K::SquidInk,          "minecraft:squid_ink",         true,  S::Simple},
                    {K::SweepAttack,       "minecraft:sweep_attack",      true,  S::Simple},
                    {K::TotemOfUndying,    "minecraft:totem_of_undying",  false, S::Simple},
                    {K::Underwater,        "minecraft:underwater",        false, S::Simple},
                    {K::Splash,            "minecraft:splash",            false, S::Simple},
                    {K::BubblePop,         "minecraft:bubble_pop",        false, S::Simple},
                    {K::CurrentDown,       "minecraft:current_down",      false, S::Simple},
                    {K::BubbleColumnUp,    "minecraft:bubble_column_up",  false, S::Simple},
                    {K::Nautilus,          "minecraft:nautilus",          false, S::Simple},
                    {K::Dolphin,           "minecraft:dolphin",           false, S::Simple},
                    {K::CampfireCosySmoke, "minecraft:campfire_cosy_smoke", true, S::Simple},
                    {K::CampfireSignalSmoke, "minecraft:campfire_signal_smoke", true, S::Simple},
                    {K::DrippingHoney,     "minecraft:dripping_honey",    false, S::Simple},
                    {K::FallingHoney,      "minecraft:falling_honey",     false, S::Simple},
                    {K::LandingHoney,      "minecraft:landing_honey",     false, S::Simple},
                    {K::FallingNectar,     "minecraft:falling_nectar",    false, S::Simple},
                    {K::FallingSporeBlossom, "minecraft:falling_spore_blossom", false, S::Simple},
                    {K::Ash,               "minecraft:ash",               false, S::Simple},
                    {K::CrimsonSpore,      "minecraft:crimson_spore",     false, S::Simple},
                    {K::WarpedSpore,       "minecraft:warped_spore",      false, S::Simple},
                    {K::SporeBlossomAir,   "minecraft:spore_blossom_air", false, S::Simple},
                    {K::DrippingObsidianTear, "minecraft:dripping_obsidian_tear", false, S::Simple},
                    {K::FallingObsidianTear,  "minecraft:falling_obsidian_tear",  false, S::Simple},
                    {K::LandingObsidianTear,  "minecraft:landing_obsidian_tear",  false, S::Simple},
                    {K::ReversePortal,     "minecraft:reverse_portal",    false, S::Simple},
                    {K::WhiteAsh,          "minecraft:white_ash",         false, S::Simple},
                    {K::SmallFlame,        "minecraft:small_flame",       false, S::Simple},
                    {K::Snowflake,         "minecraft:snowflake",         false, S::Simple},
                    {K::DrippingDripstoneLava,  "minecraft:dripping_dripstone_lava",  false, S::Simple},
                    {K::FallingDripstoneLava,   "minecraft:falling_dripstone_lava",   false, S::Simple},
                    {K::DrippingDripstoneWater, "minecraft:dripping_dripstone_water", false, S::Simple},
                    {K::FallingDripstoneWater,  "minecraft:falling_dripstone_water",  false, S::Simple},
                    {K::GlowSquidInk,      "minecraft:glow_squid_ink",    true,  S::Simple},
                    {K::Glow,              "minecraft:glow",              true,  S::Simple},
                    {K::WaxOn,             "minecraft:wax_on",            true,  S::Simple},
                    {K::WaxOff,            "minecraft:wax_off",           true,  S::Simple},
                    {K::ElectricSpark,     "minecraft:electric_spark",    true,  S::Simple},
                    {K::Scrape,            "minecraft:scrape",            true,  S::Simple},
                    {K::Shriek,            "minecraft:shriek",            false, S::Shriek},
                    {K::EggCrack,          "minecraft:egg_crack",         false, S::Simple},
                    {K::DustPlume,         "minecraft:dust_plume",        false, S::Simple},
                    {K::TrialSpawnerDetection, "minecraft:trial_spawner_detection", true, S::Simple},
                    {K::TrialSpawnerDetectionOminous, "minecraft:trial_spawner_detection_ominous", true, S::Simple},
                    {K::VaultConnection,   "minecraft:vault_connection",  true,  S::Simple},
                    {K::DustPillar,        "minecraft:dust_pillar",       false, S::BlockState},
                    {K::OminousSpawning,   "minecraft:ominous_spawning",  true,  S::Simple},
                    {K::RaidOmen,          "minecraft:raid_omen",         false, S::Simple},
                    {K::TrialOmen,         "minecraft:trial_omen",        false, S::Simple},
                    {K::BlockCrumble,      "minecraft:block_crumble",     false, S::BlockState},
                    {K::Firefly,           "minecraft:firefly",           false, S::Simple},
                    {K::SulfurCubeGoo,     "minecraft:sulfur_cube_goo",   false, S::Simple},
                    {K::FireworkStarter,   "obeycraft:firework_starter",  false, S::FireworkStarter, false},
                };
                return table;
            }

        } // namespace

        const std::vector<Info>& All() { return Table(); }

        const Info& Get(ParticleKind kind) {
            const std::vector<Info>& table = Table();
            const size_t i = static_cast<size_t>(kind);
            if (i < table.size() && table[i].kind == kind) return table[i];
            // Not at its index (a kind appended after this table): search,
            // then fall back to a generic row so a caller never reads past
            // the table.
            for (const Info& info : table) {
                if (info.kind == kind) return info;
            }
            static thread_local std::string generatedName;
            static thread_local Info generated{};
            generatedName = "obeycraft:particle_" + std::to_string(i);
            generated = Info{kind, generatedName.c_str(), false, S::Simple, false};
            return generated;
        }

        std::optional<ParticleKind> FromName(std::string_view name) {
            static const std::unordered_map<std::string, ParticleKind> byName = [] {
                std::unordered_map<std::string, ParticleKind> m;
                for (const Info& info : Table()) {
                    const std::string full(info.name);
                    m.emplace(full, info.kind);
                }
                return m;
            }();
            std::string key(name);
            if (key.find(':') == std::string::npos) key = "minecraft:" + key;
            const auto it = byName.find(key);
            if (it == byName.end()) return std::nullopt;
            return it->second;
        }

    } // namespace ParticleTypes

    // ── Wire format ────────────────────────────────────────────────────────

    void WriteParticleOptions(Network::PacketBuffer& out, const ParticleOptions& o) {
        out.WriteVarInt(static_cast<uint32_t>(o.kind));
        switch (ParticleTypes::Get(o.kind).shape) {
            case ParticleTypes::OptionsShape::Simple:
                break;
            case ParticleTypes::OptionsShape::BlockState:
                out.WriteVarInt(o.blockState);
                break;
            case ParticleTypes::OptionsShape::Item:
                out.WriteVarInt(o.itemId);
                break;
            case ParticleTypes::OptionsShape::Dust:
                out.WriteInt(o.Rgb());
                out.WriteFloat(o.scale);
                break;
            case ParticleTypes::OptionsShape::DustTransition: {
                out.WriteInt(o.Rgb());
                ParticleOptions to;
                to.r = o.r2; to.g = o.g2; to.b = o.b2;
                out.WriteInt(to.Rgb());
                out.WriteFloat(o.scale);
                break;
            }
            case ParticleTypes::OptionsShape::Color:
                out.WriteInt(o.Argb());
                break;
            case ParticleTypes::OptionsShape::Spell:
                out.WriteInt(o.Rgb());
                out.WriteFloat(o.scale);
                break;
            case ParticleTypes::OptionsShape::Power:
                out.WriteFloat(o.scale);
                break;
            case ParticleTypes::OptionsShape::Vibration:
                out.WriteInt(static_cast<uint32_t>(o.targetEntity));
                if (o.targetEntity >= 0) {
                    out.WriteFloat(o.targetEntityYOffset);
                } else {
                    out.WriteDouble(o.target.x);
                    out.WriteDouble(o.target.y);
                    out.WriteDouble(o.target.z);
                }
                out.WriteVarInt(static_cast<uint32_t>(o.intValue));
                break;
            case ParticleTypes::OptionsShape::Trail:
                out.WriteDouble(o.target.x);
                out.WriteDouble(o.target.y);
                out.WriteDouble(o.target.z);
                out.WriteInt(o.Rgb());
                out.WriteVarInt(static_cast<uint32_t>(o.intValue));
                break;
            case ParticleTypes::OptionsShape::Shriek:
                out.WriteVarInt(static_cast<uint32_t>(o.intValue));
                break;
            case ParticleTypes::OptionsShape::SculkCharge:
                out.WriteFloat(o.scale);
                break;
            case ParticleTypes::OptionsShape::Geyser:
                out.WriteInt(static_cast<uint32_t>(o.intValue));
                break;
            case ParticleTypes::OptionsShape::GeyserBase:
                out.WriteInt(static_cast<uint32_t>(o.intValue));
                out.WriteFloat(o.scale);
                break;
            case ParticleTypes::OptionsShape::FireworkStarter: {
                const auto* list = o.fireworkExplosions.get();
                const uint32_t n = list ? static_cast<uint32_t>(list->size()) : 0u;
                out.WriteByte(o.fireworkPlaySound ? 1 : 0);
                out.WriteVarInt(n);
                for (uint32_t i = 0; i < n; ++i) {
                    const FireworkExplosion& e = (*list)[i];
                    out.WriteByte(static_cast<uint8_t>(e.shape));
                    out.WriteVarInt(static_cast<uint32_t>(e.colors.size()));
                    for (int32_t c : e.colors) out.WriteInt(static_cast<uint32_t>(c));
                    out.WriteVarInt(static_cast<uint32_t>(e.fadeColors.size()));
                    for (int32_t c : e.fadeColors) out.WriteInt(static_cast<uint32_t>(c));
                    out.WriteByte(static_cast<uint8_t>((e.hasTrail ? 1 : 0) | (e.hasTwinkle ? 2 : 0)));
                }
                break;
            }
        }
    }

    ParticleOptions ReadParticleOptions(Network::PacketReader& in) {
        ParticleOptions o(static_cast<ParticleKind>(in.ReadVarInt()));
        auto readRgb = [&](float& r, float& g, float& b) {
            const uint32_t rgb = in.ReadInt();
            r = Channel(rgb, 16); g = Channel(rgb, 8); b = Channel(rgb, 0);
        };
        switch (ParticleTypes::Get(o.kind).shape) {
            case ParticleTypes::OptionsShape::Simple:
                break;
            case ParticleTypes::OptionsShape::BlockState:
                o.blockState = in.ReadVarInt();
                break;
            case ParticleTypes::OptionsShape::Item:
                o.itemId = in.ReadVarInt();
                break;
            case ParticleTypes::OptionsShape::Dust:
                readRgb(o.r, o.g, o.b);
                o.scale = ClampScale(in.ReadFloat());
                break;
            case ParticleTypes::OptionsShape::DustTransition:
                readRgb(o.r, o.g, o.b);
                readRgb(o.r2, o.g2, o.b2);
                o.scale = ClampScale(in.ReadFloat());
                break;
            case ParticleTypes::OptionsShape::Color: {
                const uint32_t argb = in.ReadInt();
                o.a = Channel(argb, 24);
                o.r = Channel(argb, 16); o.g = Channel(argb, 8); o.b = Channel(argb, 0);
                break;
            }
            case ParticleTypes::OptionsShape::Spell:
                readRgb(o.r, o.g, o.b);
                o.scale = in.ReadFloat();
                break;
            case ParticleTypes::OptionsShape::Power:
                o.scale = in.ReadFloat();
                break;
            case ParticleTypes::OptionsShape::Vibration:
                o.targetEntity = static_cast<int32_t>(in.ReadInt());
                if (o.targetEntity >= 0) {
                    o.targetEntityYOffset = in.ReadFloat();
                } else {
                    o.target.x = in.ReadDouble();
                    o.target.y = in.ReadDouble();
                    o.target.z = in.ReadDouble();
                }
                o.intValue = static_cast<int32_t>(in.ReadVarInt());
                break;
            case ParticleTypes::OptionsShape::Trail:
                o.target.x = in.ReadDouble();
                o.target.y = in.ReadDouble();
                o.target.z = in.ReadDouble();
                readRgb(o.r, o.g, o.b);
                o.intValue = static_cast<int32_t>(in.ReadVarInt());
                break;
            case ParticleTypes::OptionsShape::Shriek:
                o.intValue = static_cast<int32_t>(in.ReadVarInt());
                break;
            case ParticleTypes::OptionsShape::SculkCharge:
                o.scale = in.ReadFloat();
                break;
            case ParticleTypes::OptionsShape::Geyser:
                o.intValue = static_cast<int32_t>(in.ReadInt());
                break;
            case ParticleTypes::OptionsShape::GeyserBase:
                o.intValue = static_cast<int32_t>(in.ReadInt());
                o.scale = in.ReadFloat();
                break;
            case ParticleTypes::OptionsShape::FireworkStarter: {
                o.fireworkPlaySound = in.ReadByte() != 0;
                const uint32_t n = std::min<uint32_t>(in.ReadVarInt(), 256u);
                std::vector<FireworkExplosion> list;
                list.reserve(n);
                for (uint32_t i = 0; i < n; ++i) {
                    FireworkExplosion e;
                    e.shape = static_cast<FireworkExplosion::Shape>(std::min<uint8_t>(in.ReadByte(), 4));
                    const uint32_t nc = std::min<uint32_t>(in.ReadVarInt(), 256u);
                    for (uint32_t c = 0; c < nc; ++c) e.colors.push_back(static_cast<int32_t>(in.ReadInt()));
                    const uint32_t nf = std::min<uint32_t>(in.ReadVarInt(), 256u);
                    for (uint32_t c = 0; c < nf; ++c) e.fadeColors.push_back(static_cast<int32_t>(in.ReadInt()));
                    const uint8_t flags = in.ReadByte();
                    e.hasTrail = (flags & 1) != 0;
                    e.hasTwinkle = (flags & 2) != 0;
                    list.push_back(std::move(e));
                }
                o.fireworkExplosions = std::make_shared<const std::vector<FireworkExplosion>>(std::move(list));
                break;
            }
        }
        return o;
    }

} // namespace Game
