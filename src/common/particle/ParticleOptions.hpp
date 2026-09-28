// File: src/common/particle/ParticleOptions.hpp
//
// MC net.minecraft.core.particles — ParticleTypes, ParticleType and the
// ParticleOptions family — for every particle type 26.3 registers.
//
// A particle request is a KIND (Game::ParticleKind, EntityLevel.hpp) plus the
// options its type carries: nothing for a SimpleParticleType, and for the
// others exactly MC's option records —
//
//   BlockParticleOption        block, block_marker, falling_dust,
//                              dust_pillar, block_crumble   -> blockState
//   ItemParticleOption         item                         -> itemId
//   DustParticleOptions        dust                         -> rgb, scale
//   DustColorTransitionOptions dust_color_transition        -> rgb, rgb2, scale
//   ColorParticleOption        entity_effect, tinted_leaves,
//                              flash                        -> rgba
//   SpellParticleOption        effect, instant_effect       -> rgb, power
//   PowerParticleOption        dragon_breath                -> power
//   VibrationParticleOption    vibration                    -> destination
//                                                              (block / entity),
//                                                              arrivalTicks
//   TrailParticleOption        trail                        -> target, rgb,
//                                                              duration
//   ShriekParticleOption       shriek                       -> delay
//   SculkChargeParticleOptions sculk_charge                 -> roll
//   GeyserParticleOptions      geyser, geyser_plume         -> waterBlocks
//   GeyserBaseParticleOptions  geyser_base, geyser_poof     -> waterBlocks,
//                                                              burstImpulseBase
//
// One flat struct rather than a class per record: the options travel through
// the client's particle queue and the level-particles packet by value, and a
// flat struct keeps both of those a memcpy-shaped copy. Only the fields a
// kind's record names are meaningful for it; the factories below fill them.
//
// The TYPE table (name, overrideLimiter, option shape) is ParticleTypes::Info;
// the wire format is Write/ReadParticleOptions (a compact encoding of the same
// fields, not MC's registry-id stream codec — this engine's client and server
// ship together).
#pragma once

#include "common/entity/EntityLevel.hpp"   // Game::ParticleKind
#include "common/data/FireworkExplosion.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Network { class PacketBuffer; class PacketReader; }

namespace Game {

    // MC FireworkExplosion (the component a rocket's explosions list holds),
    // for FireworkParticles.Starter — common/data/FireworkExplosion.hpp.

    struct ParticleOptions {
        ParticleKind kind = ParticleKind::Smoke;

        // Colour payload: DUST / DUST_COLOR_TRANSITION (from), ENTITY_EFFECT,
        // TINTED_LEAVES and FLASH (ARGB), EFFECT / INSTANT_EFFECT (RGB, a = 1),
        // TRAIL (RGB).
        float r = 1.0f, g = 1.0f, b = 1.0f, a = 1.0f;
        // DUST_COLOR_TRANSITION's to_color.
        float r2 = 1.0f, g2 = 1.0f, b2 = 1.0f;
        // DUST / DUST_COLOR_TRANSITION scale (clamped 0.01..4 as MC's
        // ScalableParticleOptionsBase); EFFECT / INSTANT_EFFECT and
        // DRAGON_BREATH power; SCULK_CHARGE roll; GEYSER_BASE / GEYSER_POOF
        // burst_impulse_base.
        float scale = 1.0f;
        // SHRIEK delay; VIBRATION arrival_in_ticks; TRAIL duration; the
        // geyser family's water_blocks.
        int32_t intValue = 0;
        // BlockParticleOption's state (BlockState::RawId).
        uint32_t blockState = 0;
        // ItemParticleOption's item.
        uint32_t itemId = 0;
        // VIBRATION's BlockPositionSource (the block centre) and TRAIL's
        // target.
        glm::dvec3 target{0.0};
        // VIBRATION's EntityPositionSource: the entity id (-1 = the block
        // destination above) and its y_offset.
        int32_t targetEntity = -1;
        float   targetEntityYOffset = 0.0f;
        // FireworkParticles.Starter's explosion list (FIREWORK_STARTER only).
        // Shared so a queued copy of the options is cheap.
        std::shared_ptr<const std::vector<FireworkExplosion>> fireworkExplosions;
        // FireworkParticles.Starter's playSound (ClientLevel.createFireworks:
        // true unless the rocket went off out of sight).
        bool fireworkPlaySound = true;

        ParticleOptions() = default;
        // A SimpleParticleType, or a kind whose options keep their defaults.
        ParticleOptions(ParticleKind k) : kind(k) {}   // NOLINT: implicit on purpose

        // ── MC's option constructors ─────────────────────────────────────
        static ParticleOptions Block(ParticleKind kind, BlockState state);
        static ParticleOptions Block(BlockState state) { return Block(ParticleKind::Block, state); }
        static ParticleOptions Item(uint32_t itemId);
        // DustParticleOptions(color RGB, scale).
        static ParticleOptions Dust(uint32_t rgb, float scale);
        static ParticleOptions Dust(const glm::vec3& rgb, float scale);
        static ParticleOptions DustColorTransition(uint32_t fromRgb, uint32_t toRgb, float scale);
        // ColorParticleOption.create(type, argb) / (type, r, g, b).
        static ParticleOptions Color(ParticleKind kind, uint32_t argb);
        static ParticleOptions Color(ParticleKind kind, float r, float g, float b);
        // SpellParticleOption.create(type, rgb, power) / (type, r, g, b, power).
        static ParticleOptions Spell(ParticleKind kind, uint32_t rgb, float power);
        static ParticleOptions Spell(ParticleKind kind, float r, float g, float b, float power);
        // PowerParticleOption.create(DRAGON_BREATH, power).
        static ParticleOptions Power(ParticleKind kind, float power);
        // VibrationParticleOption(new BlockPositionSource(pos), ticks).
        static ParticleOptions VibrationToBlock(const glm::ivec3& pos, int arrivalTicks);
        // VibrationParticleOption(new EntityPositionSource(entity, yOffset), ticks).
        static ParticleOptions VibrationToEntity(int32_t entityId, float yOffset, int arrivalTicks);
        // TrailParticleOption(target, color RGB, duration).
        static ParticleOptions Trail(const glm::dvec3& target, uint32_t rgb, int duration);
        static ParticleOptions Shriek(int delay);
        static ParticleOptions SculkCharge(float roll);
        // GeyserParticleOptions(type, waterBlocks).
        static ParticleOptions Geyser(ParticleKind kind, int waterBlocks);
        // GeyserBaseParticleOptions(type, waterBlocks, burstImpulseBase).
        static ParticleOptions GeyserBase(ParticleKind kind, int waterBlocks, float burstImpulseBase);
        static ParticleOptions FireworkStarter(std::vector<FireworkExplosion> explosions, bool playSound);

        uint32_t Rgb() const;    // (r, g, b) as 0xRRGGBB
        uint32_t Argb() const;   // (a, r, g, b) as 0xAARRGGBB
    };

    namespace ParticleTypes {

        // What a type's options record carries (MC: the codec its
        // ParticleType was registered with).
        enum class OptionsShape : uint8_t {
            Simple,              // SimpleParticleType
            BlockState,          // BlockParticleOption
            Item,                // ItemParticleOption
            Dust,                // DustParticleOptions
            DustTransition,      // DustColorTransitionOptions
            Color,               // ColorParticleOption
            Spell,               // SpellParticleOption
            Power,               // PowerParticleOption
            Vibration,           // VibrationParticleOption
            Trail,               // TrailParticleOption
            Shriek,              // ShriekParticleOption
            SculkCharge,         // SculkChargeParticleOptions
            Geyser,              // GeyserParticleOptions
            GeyserBase,          // GeyserBaseParticleOptions
            FireworkStarter,     // engine: FireworkParticles.Starter's explosion list
        };

        struct Info {
            ParticleKind kind;
            // The registry id, "minecraft:angry_villager". Engine kinds carry
            // their own namespace ("obeycraft:hush_mote", "aether:...").
            const char*  name;
            // MC ParticleType.getOverrideLimiter() — the `true` second
            // argument to ParticleTypes.register.
            bool         overrideLimiter;
            OptionsShape shape;
            // False for kinds with no MC registry entry of their own that
            // /particle must not offer (the firework starter).
            bool         commandVisible = true;
        };

        // The type's row; a kind appended to ParticleKind after this table
        // was last updated answers a generic row named "obeycraft:particle_<n>".
        const Info& Get(ParticleKind kind);
        // Every row, in ParticleKind order.
        const std::vector<Info>& All();
        // "dust", "minecraft:dust", "obeycraft:hush_mote" → the kind.
        std::optional<ParticleKind> FromName(std::string_view name);

        inline bool OverrideLimiter(ParticleKind kind) { return Get(kind).overrideLimiter; }
        inline const char* Name(ParticleKind kind) { return Get(kind).name; }

    } // namespace ParticleTypes

    // The level-particles / level-event wire format for a ParticleOptions:
    // the kind, then exactly the fields its OptionsShape names.
    void WriteParticleOptions(Network::PacketBuffer& out, const ParticleOptions& options);
    ParticleOptions ReadParticleOptions(Network::PacketReader& in);

} // namespace Game
