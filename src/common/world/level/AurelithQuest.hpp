// File: src/common/world/level/AurelithQuest.hpp
//
// Reawakening the Heart of Aurelith (docs/the-hush.md, "Reawakening the
// Heart"; lore in docs/hush-lore.md): the facts every side of the quest
// agrees on — the city's geometry measured from its Heart, the Four Voices,
// the states a city moves through, the awakening's timeline, and the sound
// events the quest plays. Server (server/level/AurelithCities), client
// (client/world/AurelithState, the Heart and beacon renderers, the city's
// sound) and the blocks (common/world/block/AurelithQuestBlocks) all read
// this one header, so a timing or a radius can never disagree between them.
//
// ── The city, measured from its Heart ───────────────────────────────────
// Aurelith is one designed volume (tools/gen_aurelith.py) placed by the
// jigsaw as a whole, rotated by the start piece's random rotation. Every
// landmark is a fixed DESIGN offset from the resonance engine (the Heart's
// core block): x east, z south, north = -z, as the generator draws it. A
// world position is the Heart plus the design offset turned by the city's
// rotation (RotateOffset — MC Rotation.rotate on a BlockPos, which is what
// StructureTemplate.transform applies to every template block). The
// rotation is the resonance engine's block entity's (the template engine
// writes it from the placement settings; ResonanceEngineBlockEntity). A city
// generated before that field existed has rotation -1: it still renders,
// but its quest pieces (sockets, keys) were never placed, so there is
// nothing to awaken.
//
// The wall is an octagon and the Four Voices sit on the four axes, so the
// questions "inside the walls?" and "which statue?" are rotation-free for the
// wall and need the rotation only to name a voice.
//
// ── States ───────────────────────────────────────────────────────────────
//   Dormant    the city as found: the Heart silenced, its lights dim.
//   Awakening  the Chord was sung at the Podium: the rings spin up, the
//              rising chord plays, the four gate beams bend together into
//              one pillar over the Heart, a wave of light runs out from the
//              Heart to the walls — then the Undersong answers.
//   Contested  the Unsung has risen; the fight in the plaza.
//   Awakened   the Unsung is sung to rest: full light for good, no hostile
//              natural spawns inside the walls, the city's own music.
// Transitions are server-only (AurelithCities); clients learn them from
// AurelithS2C and animate from (state, stageStartTick) on the level's game
// time, so every player sees the same moment.
#pragma once

#include "common/world/level/DimensionId.hpp"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string_view>

namespace Game::Aurelith {

    // ── States ───────────────────────────────────────────────────────────
    enum class CityState : uint8_t {
        Dormant   = 0,
        Awakening = 1,
        Contested = 2,
        Awakened  = 3,
    };

    // ── The Four Voices ──────────────────────────────────────────────────
    // Lowest to highest is the order the Chord is raised in ("from the floor
    // to the crown": Bass, Tenor, Alto, Soprano) — the order the Podium's
    // sockets must be filled in. The enum is the gates' order instead
    // (north, east, south, west), as VoiceBeaconRenderer's colours are.
    enum class Voice : uint8_t { Soprano = 0, Alto = 1, Tenor = 2, Bass = 3 };
    inline constexpr int kVoiceCount = 4;
    inline constexpr Voice kChordOrder[kVoiceCount] = {
        Voice::Bass, Voice::Tenor, Voice::Alto, Voice::Soprano,
    };

    inline constexpr std::string_view VoiceName(Voice v) {
        switch (v) {
            case Voice::Soprano: return "soprano";
            case Voice::Alto:    return "alto";
            case Voice::Tenor:   return "tenor";
            case Voice::Bass:    return "bass";
        }
        return "soprano";
    }

    // The voice's colour, 0xRRGGBB (VoiceBeaconRenderer's beams; the keys'
    // glow; the boss's stolen voices).
    inline constexpr uint32_t VoiceColour(Voice v) {
        switch (v) {
            case Voice::Soprano: return 0xDFFFFF;   // pale cyan-white
            case Voice::Alto:    return 0x5FF3FF;   // cyan
            case Voice::Tenor:   return 0xB77CFF;   // violet
            case Voice::Bass:    return 0x6F7BFF;   // indigo
        }
        return 0xDFFFFF;
    }

    // The note each voice sounds in the Chord, as an MC note-block pitch
    // multiplier (2^((semitone - 12) / 12), semitone 0..24): the Chord is a
    // major triad spread over two octaves — Bass the root F#3 (0), Tenor the
    // fifth C#4 (7), Alto the third A#4 (16), Soprano the octave F#5 (24).
    inline float VoicePitch(Voice v) {
        int semitone = 0;
        switch (v) {
            case Voice::Bass:    semitone = 0;  break;
            case Voice::Tenor:   semitone = 7;  break;
            case Voice::Alto:    semitone = 16; break;
            case Voice::Soprano: semitone = 24; break;
        }
        return std::pow(2.0f, static_cast<float>(semitone - 12) / 12.0f);
    }

    // ── Design geometry (offsets from the Heart, design frame) ───────────
    // The street surface is the block kStreetBelowHeart under the Heart
    // (gen_aurelith.py: the engine at S + 5); players walk one above it.
    inline constexpr int kStreetBelowHeart = 5;
    // The walls (gen_aurelith.py WALL_IN / WALL_OUT / CHAMFER): an octagon
    // radius max(|dx|, |dz|, (|dx| + |dz|) * kWallIn / kChamfer).
    inline constexpr double kWallIn  = 100.0;
    inline constexpr double kWallOut = 103.0;
    inline constexpr double kChamfer = 150.0;
    // The glowing Stave band on the wall: street + 6, i.e. one over the Heart.
    inline constexpr int kStaveBandAboveHeart = 1;
    // The Plaza of the Held Note (the fight's arena) and its colonnade.
    inline constexpr double kPlazaRadius = 30.0;
    // The Four Voices' statues: design (0, -19) Soprano, (19, 0) Alto,
    // (0, 19) Tenor, (-19, 0) Bass; their plinths are 3x3 at street + 1..3.
    inline constexpr int kStatueDistance = 19;
    // The gate beacons: design (0, -100) north .. (-100, 0) west, 47 over
    // the Heart (street + 52).
    inline constexpr int kBeaconDistance = 100;
    inline constexpr int kBeaconAboveHeart = 47;
    // The Conductor's Podium: its centre 12 north of the Heart; the four
    // chord sockets stand on its raised half-tier before the Conductor's
    // chair (design z = -13) at x = -2, -1, +1, +2, at street + 2 — waist
    // height to a player on the apron — facing the Heart (design south).
    inline constexpr int kPodiumNorth = 12;
    inline constexpr int kSocketRow   = -13;
    inline constexpr int kSocketAboveStreet = 2;
    inline constexpr int kSocketXs[kVoiceCount] = {-2, -1, 1, 2};
    // How far from the Heart the quest looks for its pieces (sockets, the
    // plaza's statues) — well inside the colonnade.
    inline constexpr int kQuestReach = 24;

    inline constexpr glm::ivec2 StatueOffset(Voice v) {
        switch (v) {
            case Voice::Soprano: return { 0, -kStatueDistance };
            case Voice::Alto:    return { kStatueDistance, 0 };
            case Voice::Tenor:   return { 0, kStatueDistance };
            case Voice::Bass:    return { -kStatueDistance, 0 };
        }
        return { 0, 0 };
    }
    inline constexpr glm::ivec2 BeaconOffset(Voice v) {
        switch (v) {
            case Voice::Soprano: return { 0, -kBeaconDistance };
            case Voice::Alto:    return { kBeaconDistance, 0 };
            case Voice::Tenor:   return { 0, kBeaconDistance };
            case Voice::Bass:    return { -kBeaconDistance, 0 };
        }
        return { 0, 0 };
    }

    // MC Rotation.rotate(BlockPos) for the template rotation ordinal
    // (NONE 0, CLOCKWISE_90 1, CLOCKWISE_180 2, COUNTERCLOCKWISE_90 3):
    // a design offset to a world offset. Unknown rotation (-1) = NONE.
    inline constexpr glm::ivec2 RotateOffset(glm::ivec2 d, int rotation) {
        switch (rotation & 3) {
            case 1:  return { -d.y,  d.x };
            case 2:  return { -d.x, -d.y };
            case 3:  return {  d.y, -d.x };
            default: return d;
        }
    }
    // The inverse: a world offset back into the design frame.
    inline constexpr glm::ivec2 UnrotateOffset(glm::ivec2 w, int rotation) {
        return RotateOffset(w, (4 - (rotation & 3)) & 3);
    }
    // A design direction (north = 0, east = 1, south = 2, west = 3, i.e. the
    // Voice enum's gates) turned into the world's.
    inline constexpr int RotateDirection(int designDir, int rotation) {
        return (designDir + (rotation & 3)) & 3;
    }

    inline glm::ivec3 StreetOf(const glm::ivec3& heart) {
        return { heart.x, heart.y - kStreetBelowHeart, heart.z };
    }
    // World position of a design offset at a height above the street.
    inline glm::ivec3 DesignToWorld(const glm::ivec3& heart, int rotation, glm::ivec2 design,
                                    int aboveStreet) {
        const glm::ivec2 w = RotateOffset(design, rotation);
        return { heart.x + w.x, heart.y - kStreetBelowHeart + aboveStreet, heart.z + w.y };
    }

    // The wall's octagon radius of a horizontal offset from the Heart.
    inline double OctagonRadius(double dx, double dz) {
        const double ax = std::abs(dx), az = std::abs(dz);
        return std::max(std::max(ax, az), (ax + az) * (kWallIn / kChamfer));
    }
    // Inside the walls (the walled city proper, not the wall itself), at any
    // height from the vault under the plaza to the Spire's needle.
    inline bool InsideWalls(const glm::ivec3& heart, const glm::dvec3& p) {
        if (p.y < heart.y - 40.0 || p.y > heart.y + 140.0) return false;
        return OctagonRadius(p.x - (heart.x + 0.5), p.z - (heart.z + 0.5)) < kWallIn;
    }

    // ── The awakening's timeline (ticks from the Awakening stage's start) ─
    // Every client animates from these on the level's game time; the server
    // plays the sounds and runs the light wave on the same marks.
    inline constexpr int kArpeggioStep    = 12;    // the four sockets sound one by one
    inline constexpr int kChordStruck     = 56;    // all four together
    inline constexpr int kSpinUpStart     = 60;    // the rings begin to turn up
    inline constexpr int kSpinUpTicks     = 120;
    inline constexpr int kBeamBendStart   = 100;   // the gate beams lean in
    inline constexpr int kBeamBendTicks   = 140;
    inline constexpr int kWaveStart       = 140;   // the light leaves the Heart
    inline constexpr double kWaveSpeed    = 0.6;   // blocks per tick (12 blocks/s)
    inline constexpr double kWaveReach    = 118.0; // past the wall, then done
    inline constexpr int kMotesBurst      = 150;   // motes burst from the Heart
    inline constexpr int kUndersong       = 360;   // the second note, under ours
    inline constexpr int kUnsungRises     = 440;   // the Unsung rises; Contested
    inline constexpr int kResolveTicks    = 200;   // the resolution after the fight
    // The discord's light flicker round the Podium (wrong order).
    inline constexpr int kDiscordFlickerRadius = 20;

    // Where the four gate beams meet over the Heart once they bend together
    // (VoiceBeaconRenderer), and where the Heart's own pillar of light is
    // joined by them (AurelithHeartRenderer): this far above the engine.
    inline constexpr double kConvergeAboveHeart = 90.0;

    inline double WaveRadiusAt(double ticksIntoAwakening) {
        return std::clamp((ticksIntoAwakening - kWaveStart) * kWaveSpeed, 0.0, kWaveReach);
    }

    // ── Sound events (assets/sound_overlays/obeycraft/aurelith.json) ─────
    // Played by the server with ILevelWrite/EntityLevel::PlaySound; the
    // overlay composes each from vanilla sounds ("type": "event" entries).
    namespace Sounds {
        // The Podium.
        inline constexpr const char* kSocketSeat    = "obeycraft:block.chord_socket.seat";     // pitch = VoicePitch
        inline constexpr const char* kSocketTake    = "obeycraft:block.chord_socket.take";
        inline constexpr const char* kSocketNote    = "obeycraft:block.chord_socket.note";     // one voice of the arpeggio
        inline constexpr const char* kChord         = "obeycraft:block.chord_socket.chord";    // the four together
        inline constexpr const char* kDiscord       = "obeycraft:block.chord_socket.discord";  // wrong order
        inline constexpr const char* kSocketLocked  = "obeycraft:block.chord_socket.locked";   // the Chord holds them
        // The pedestals and the Hall of Instruments' cabinet.
        inline constexpr const char* kPedestalTake  = "obeycraft:block.voice_pedestal.take";
        inline constexpr const char* kPedestalPlace = "obeycraft:block.voice_pedestal.place";
        inline constexpr const char* kCabinetLocked = "obeycraft:block.choir_cabinet.locked";
        inline constexpr const char* kCabinetWrong  = "obeycraft:block.choir_cabinet.wrong";
        inline constexpr const char* kCabinetOpen   = "obeycraft:block.choir_cabinet.open";
        // The awakening (at the Heart, long range).
        inline constexpr const char* kAwakenRise    = "obeycraft:event.aurelith.awaken_rise";  // the rings spin up
        inline constexpr const char* kAwakenSwell   = "obeycraft:event.aurelith.awaken_swell"; // the wave leaves the Heart
        inline constexpr const char* kAwakenBloom   = "obeycraft:event.aurelith.awaken_bloom"; // the motes burst
        inline constexpr const char* kUndersong     = "obeycraft:event.aurelith.undersong";    // the second note
        inline constexpr const char* kUnsungRise    = "obeycraft:event.aurelith.unsung_rise";  // it rises
        inline constexpr const char* kResolve       = "obeycraft:event.aurelith.resolve";      // the Chord resolves
        inline constexpr const char* kLightWave     = "obeycraft:event.aurelith.light_wave";   // a district lights (played along the wave)
        // The reward.
        inline constexpr const char* kHeldNote      = "obeycraft:item.held_note.sound";
    }

    // ── Hostile spawns inside an awakened city (server) ──────────────────
    // The natural spawner's gate, beside EchoHeart::SuppressesHostileSpawn:
    // true when `pos` is inside the walls of an awakened city in `dimension`.
    // The server's AurelithCities keeps the list (SetAwakenedCities); it is
    // empty on a client.
    bool SuppressesHostileSpawn(DimensionId dimension, const glm::ivec3& pos);

    // Replace the list of awakened Hearts for a dimension (server thread).
    void SetAwakenedCities(DimensionId dimension, const glm::ivec3* hearts, size_t count);

} // namespace Game::Aurelith

namespace Game {
    class ILevelWrite;
    class IUsePlayer;
}

namespace Game::Aurelith {

    // ── Server bridges (server/level/AurelithCities.cpp) ─────────────────
    // The quest blocks live in common (AurelithQuestBlocks.cpp) but what a
    // seated key MEANS — which city, which order, the awakening — is the
    // server's. Same bridge shape as HushItems.hpp: common declares, the
    // server defines, and a level with no server behind it does nothing.

    // A key was just seated in the chord socket at `socket` (server, after
    // the socket's entity took it): when the Podium's four sockets are full,
    // the city judges the order — the Chord, or the discord.
    void OnSocketSeated(ILevelWrite& level, const glm::ivec3& socket, IUsePlayer* player);

    // A flourish of particles at `origin` for the players near it (the
    // AurelithS2C Burst kind; `style` is its BurstStyle, `colour` 0xRRGGBB).
    // The server has no particles of its own; this is how a block's moment
    // (a key seated, a cabinet sung open) is seen.
    void BroadcastBurst(ILevelWrite& level, const glm::dvec3& origin, uint8_t style, uint32_t colour);

    // ── The Held Note (server/items/AurelithItems.cpp) ───────────────────
    // The reward for singing the Heart awake: the Chord's own note set solid.
    // Held for kHeldNoteUseTicks (the goat horn's pose) it sounds the Chord:
    // every hostile mob within kHeldNoteRadius is held still for
    // kHeldNoteHoldTicks (Mob::HoldByNote — its AI stops as in the Hush's
    // stillness), a boss only slowed (Slowness II for 4 s). Cooldown
    // kHeldNoteCooldownSeconds, per player, server-side (this engine has no
    // item cooldowns; a use inside one is refused on the action bar).
    inline constexpr int    kHeldNoteUseTicks        = 20;
    inline constexpr double kHeldNoteRadius          = 16.0;
    inline constexpr int    kHeldNoteHoldTicks       = 120;
    inline constexpr int    kHeldNoteCooldownSeconds = 40;
    // True when the hold began (the item shell answers CONSUME), false when
    // refused (FAIL: still ringing).
    bool HeldNoteBegin(IUsePlayer& player, uint32_t hand);
    void HeldNoteFinish(IUsePlayer& player);

} // namespace Game::Aurelith
