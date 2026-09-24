// File: src/common/sound/AurelithSoundCues.hpp
//
// The quest's sound cues, played in layers (docs/the-hush.md, "City sound").
//
// A sounds.json event picks ONE weighted entry per play — MC has no way to
// sound a chord from a single event. Every chord-like cue of the Aurelith
// quest (Game::Aurelith::Sounds) is therefore a PRINCIPAL event, defined in
// assets/sound_overlays/obeycraft/aurelith.json so that it reads on its own,
// plus LAYERS: further plays of vanilla events at their own pitches, sent in
// the same tick from the same point. That is exactly how MC builds its own
// composite moments (the raid horn, a beacon's activation over its ambience:
// several Level.playSound calls), so each layer is an ordinary server sound,
// heard by everyone in range, seeded, subtitle and all.
//
//   Game::Aurelith::SoundCues::Play(level, pos, Game::Aurelith::Sounds::kChord,
//                                   SoundSource::Blocks, 4.0f);
//
// `Level` is anything with MC's Level.playSound(except, x, y, z, ...) — the
// server's ILevelWrite (World) or an EntityLevel (a mob, the item bridge).
// Playing only the principal event (a plain PlaySound) still sounds right;
// the layers are what make it the Chord.
//
// Pitches are MC note-block multipliers: the Chord is F# major over two
// octaves (Game::Aurelith::VoicePitch) — F#3 0.5, C#4 0.749, F#4 1.0,
// A#4 1.26, C#5 1.498, F#5 2.0. A layer marked `follow` is multiplied by the
// pitch the caller passes (a socket's seat sounds in its voice's note); the
// others keep their own. The engine clamps every pitch to [0.5, 2] as MC's
// SoundEngine.calculatePitch does.
//
// Volume: the caller's volume sets the reach (16 blocks x max(1, volume), MC
// Sound.getAttenuationDistance) and every layer shares it, so the whole chord
// is heard from the same distance; a layer's `volume` scale (< 1) makes a
// sparkle quieter AND shorter-reaching, which is what a bright top note does.
#pragma once

#include "common/sound/LevelSound.hpp"
#include "common/sound/SoundSource.hpp"
#include "common/world/level/AurelithQuest.hpp"

#include <glm/glm.hpp>

#include <algorithm>
#include <string_view>

namespace Game::Aurelith::SoundCues {

    struct Layer {
        const char* event;      // vanilla event (bare = minecraft:) or an obeycraft: one
        float       pitch;      // note-block multiplier
        float       volume;     // scale of the caller's volume
        bool        follow;     // multiplied by the caller's pitch
    };

    struct Cue {
        std::string_view principal;
        const Layer*     layers;
        int              count;
    };

    namespace Detail {
        // The F# chord's tones as note-block multipliers.
        inline constexpr float F3 = 0.5f, Cs4 = 0.749f, F4 = 1.0f, As4 = 1.26f, Cs5 = 1.498f, F5 = 2.0f;
        // Clash tones for the discords: G3 (a semitone over the root) and
        // C4 (the tritone).
        inline constexpr float G3 = 0.53f, C4 = 0.707f;

        // A key clicks into its cradle; its voice rings (follow = the voice).
        inline constexpr Layer kSeat[] = {
            {"block.vault.insert_item",        1.25f, 1.0f, false},
            {"block.amethyst_block.resonate",  1.0f,  0.6f, false},
            {"block.note_block.chime",         1.0f,  0.5f, true},
        };
        inline constexpr Layer kTake[] = {
            {"block.amethyst_cluster.hit",     0.8f,  0.8f, false},
        };
        // One voice of the arpeggio: harp, doubled by a bell and a flute.
        inline constexpr Layer kNote[] = {
            {"block.note_block.bell",          1.0f,  0.7f, true},
            {"block.note_block.flute",         1.0f,  0.5f, true},
            {"block.amethyst_block.chime",     1.0f,  0.6f, false},
        };
        // The Chord: all four voices on harp and bell, a chime crown, the
        // big bell's sustain under it.
        inline constexpr Layer kChord[] = {
            {"block.note_block.harp",  F3,  1.0f, false}, {"block.note_block.harp",  Cs4, 1.0f, false},
            {"block.note_block.harp",  As4, 1.0f, false}, {"block.note_block.harp",  F5,  1.0f, false},
            {"block.note_block.bell",  F3,  1.0f, false}, {"block.note_block.bell",  Cs4, 1.0f, false},
            {"block.note_block.bell",  As4, 1.0f, false}, {"block.note_block.bell",  F5,  1.0f, false},
            {"block.note_block.chime", Cs5, 0.6f, false}, {"block.note_block.chime", F5,  0.6f, false},
            {"block.bell.resonate",    F4,  1.0f, false},
            {"block.amethyst_block.resonate", F4, 0.8f, false},
        };
        // The discord: the root against a semitone and a tritone on the
        // buzzing instruments, a low bell struck, glass cracking.
        inline constexpr Layer kDiscord[] = {
            {"block.note_block.didgeridoo", F3,  1.0f, false}, {"block.note_block.didgeridoo", G3, 1.0f, false},
            {"block.note_block.harp",       C4,  1.0f, false}, {"block.note_block.harp",       Cs4, 0.8f, false},
            {"block.note_block.bass",       F3,  1.0f, false},
            {"block.bell.use",              0.5f, 1.0f, false},
            {"block.vault.insert_item_fail", 0.6f, 1.0f, false},
            {"block.amethyst_cluster.break", 0.5f, 0.8f, false},
        };
        inline constexpr Layer kLocked[] = {
            {"block.amethyst_block.resonate", 1.5f, 0.5f, false},
        };
        inline constexpr Layer kPedestalTake[] = {
            {"block.vault.eject_item",   1.1f, 0.8f, false},
            {"block.note_block.chime",   Cs5,  0.5f, true},
        };
        inline constexpr Layer kPedestalPlace[] = {
            {"block.amethyst_block.resonate", 1.2f, 0.6f, false},
        };
        inline constexpr Layer kCabinetLocked[] = {
            {"block.amethyst_cluster.hit", 0.8f, 0.6f, false},
        };
        inline constexpr Layer kCabinetWrong[] = {
            {"block.note_block.harp",        F3,   1.0f, false},
            {"block.amethyst_block.break",   0.8f, 0.6f, false},
        };
        // The cabinet opens on the Alto's note (A#4), bell and chime.
        inline constexpr Layer kCabinetOpen[] = {
            {"block.note_block.bell",      As4,  1.0f, false},
            {"block.note_block.chime",     As4,  0.8f, false},
            {"block.note_block.chime",     F5,   0.6f, false},
            {"block.amethyst_block.chime", 1.0f, 1.0f, false},
            {"block.barrel.open",          0.8f, 0.7f, false},
        };
        // The rings spin up: a slow swell from the root and fifth.
        inline constexpr Layer kAwakenRise[] = {
            {"block.conduit.activate",        0.7f, 1.0f, false},
            {"block.respawn_anchor.charge",   0.6f, 1.0f, false},
            {"block.note_block.bell",         F3,   1.0f, false},
            {"block.note_block.bell",         Cs4,  1.0f, false},
            {"block.amethyst_block.resonate", 0.5f, 1.0f, false},
            {"block.amethyst_block.resonate", 0.75f, 1.0f, false},
            {"block.bell.resonate",           0.5f, 1.0f, false},
        };
        // The wave leaves the Heart: the whole chord, bells ringing on.
        inline constexpr Layer kAwakenSwell[] = {
            {"block.bell.resonate",    Cs4, 1.0f, false}, {"block.bell.resonate",    F4,  1.0f, false},
            {"block.note_block.bell",  F3,  1.0f, false}, {"block.note_block.bell",  Cs4, 1.0f, false},
            {"block.note_block.bell",  As4, 1.0f, false}, {"block.note_block.bell",  F5,  1.0f, false},
            {"block.note_block.chime", As4, 0.8f, false}, {"block.note_block.chime", F5,  0.8f, false},
            {"block.amethyst_block.resonate", F4, 1.0f, false},
        };
        // The motes burst: glass and chimes high in the chord.
        inline constexpr Layer kAwakenBloom[] = {
            {"block.amethyst_block.chime", 0.75f, 1.0f, false},
            {"block.amethyst_block.chime", 1.0f,  1.0f, false},
            {"block.amethyst_block.chime", As4,   1.0f, false},
            {"block.note_block.chime",     As4,   0.8f, false},
            {"block.note_block.chime",     Cs5,   0.8f, false},
            {"block.note_block.chime",     F5,    0.8f, false},
            {"block.note_block.pling",     F5,    0.5f, false},
            {"block.beacon.activate",      1.4f,  1.0f, false},
        };
        // The Undersong: our root with a note a semitone above it, sung by
        // something that is not us — the deep dark's heartbeat under it.
        inline constexpr Layer kUndersong[] = {
            {"entity.warden.heartbeat",      0.5f, 1.0f, false},
            {"block.sculk_catalyst.bloom",   0.5f, 1.0f, false},
            {"block.note_block.didgeridoo",  G3,   1.0f, false},
            {"block.note_block.bass",        F3,   1.0f, false},
            {"block.bell.resonate",          0.5f, 1.0f, false},
            {"entity.warden.sonic_charge",   0.5f, 0.5f, false},
            {"block.sculk_shrieker.shriek",  0.5f, 0.4f, false},
        };
        inline constexpr Layer kUnsungRise[] = {
            {"block.sculk_catalyst.bloom",        0.6f, 1.0f, false},
            {"block.respawn_anchor.deplete",      0.5f, 1.0f, false},
            {"block.trial_spawner.ominous_activate", 0.6f, 1.0f, false},
            {"block.end_portal.spawn",            0.5f, 0.7f, false},
            {"entity.warden.roar",                0.5f, 0.6f, false},
        };
        // The Chord resolves: every voice, bright, the big bell on the third.
        inline constexpr Layer kResolve[] = {
            {"block.note_block.bell",  F3,  1.0f, false}, {"block.note_block.bell",  Cs4, 1.0f, false},
            {"block.note_block.bell",  F4,  1.0f, false}, {"block.note_block.bell",  As4, 1.0f, false},
            {"block.note_block.bell",  Cs5, 1.0f, false}, {"block.note_block.bell",  F5,  1.0f, false},
            {"block.note_block.harp",  F3,  1.0f, false}, {"block.note_block.harp",  F4,  1.0f, false},
            {"block.note_block.harp",  As4, 1.0f, false}, {"block.note_block.harp",  Cs5, 1.0f, false},
            {"block.bell.resonate",    F4,  1.0f, false}, {"block.bell.resonate",    As4, 1.0f, false},
            {"block.beacon.activate",  1.2f, 1.0f, false},
            {"block.amethyst_block.chime", 1.5f, 0.8f, false},
        };
        inline constexpr Layer kLightWave[] = {
            {"block.amethyst_block.chime", 1.0f, 0.7f, false},
        };
        // The Held Note: a horn's call on the Chord, the big bell under it.
        inline constexpr Layer kHeldNote[] = {
            {"block.note_block.bell", F3,  1.0f, false}, {"block.note_block.bell", Cs4, 1.0f, false},
            {"block.note_block.bell", As4, 1.0f, false}, {"block.note_block.bell", F5,  1.0f, false},
            {"block.bell.resonate",   F4,  1.0f, false},
            {"block.beacon.power_select", 1.3f, 0.8f, false},
            {"block.amethyst_block.resonate", 1.5f, 0.8f, false},
        };

        template <size_t N>
        constexpr Cue Make(const char* principal, const Layer (&layers)[N]) {
            return Cue{principal, layers, static_cast<int>(N)};
        }

        inline const Cue* Find(std::string_view event) {
            static const Cue kCues[] = {
                Make(Sounds::kSocketSeat,    kSeat),
                Make(Sounds::kSocketTake,    kTake),
                Make(Sounds::kSocketNote,    kNote),
                Make(Sounds::kChord,         kChord),
                Make(Sounds::kDiscord,       kDiscord),
                Make(Sounds::kSocketLocked,  kLocked),
                Make(Sounds::kPedestalTake,  kPedestalTake),
                Make(Sounds::kPedestalPlace, kPedestalPlace),
                Make(Sounds::kCabinetLocked, kCabinetLocked),
                Make(Sounds::kCabinetWrong,  kCabinetWrong),
                Make(Sounds::kCabinetOpen,   kCabinetOpen),
                Make(Sounds::kAwakenRise,    kAwakenRise),
                Make(Sounds::kAwakenSwell,   kAwakenSwell),
                Make(Sounds::kAwakenBloom,   kAwakenBloom),
                Make(Sounds::kUndersong,     kUndersong),
                Make(Sounds::kUnsungRise,    kUnsungRise),
                Make(Sounds::kResolve,       kResolve),
                Make(Sounds::kLightWave,     kLightWave),
                Make(Sounds::kHeldNote,      kHeldNote),
            };
            for (const Cue& c : kCues) {
                if (c.principal == event) return &c;
            }
            return nullptr;
        }
    } // namespace Detail

    // Play `event` and, when it is one of the quest's cues, its layers.
    // `except` as MC's Level.playSound (nullptr: everyone hears it).
    template <class Level>
    void Play(Level& level, const glm::dvec3& pos, std::string_view event, SoundSource source,
              float volume = 1.0f, float pitch = 1.0f, const SoundExcept& except = SoundExcept{}) {
        level.PlaySound(except, pos, event, source, volume, pitch);
        const Cue* cue = Detail::Find(event);
        if (!cue) return;
        for (int i = 0; i < cue->count; ++i) {
            const Layer& l = cue->layers[i];
            const float p = std::clamp(l.pitch * (l.follow ? pitch : 1.0f), 0.5f, 2.0f);
            level.PlaySound(except, pos, l.event, source, volume * l.volume, p);
        }
    }

    // Block-position convenience: the block's centre, as MC's BlockPos overload.
    template <class Level>
    void Play(Level& level, const glm::ivec3& pos, std::string_view event, SoundSource source,
              float volume = 1.0f, float pitch = 1.0f, const SoundExcept& except = SoundExcept{}) {
        Play(level, Sound::BlockCenter(pos), event, source, volume, pitch, except);
    }

} // namespace Game::Aurelith::SoundCues
