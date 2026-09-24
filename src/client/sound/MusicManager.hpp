// File: src/client/sound/MusicManager.hpp
//
// MC net.minecraft.client.sounds.MusicManager (+ sounds.Music / Musics and
// Minecraft.getSituationalMusic / getMusicVolume): which song plays, when the
// next one starts, and the fade between.
//
// The rules, verbatim from MC:
//   • a song starts when nothing is playing and nextSongDelay counts down to
//     0 — never under the level-loading screen;
//   • the situation's Music caps the delay at its maxDelay; a finished song
//     re-arms it at MusicFrequency.getNextSongDelay (DEFAULT: at most 20
//     minutes, FREQUENT 10, CONSTANT 5 seconds), bounded by the Music's own
//     min/max delay;
//   • a situation whose Music replaces the current one (menu, credits, the
//     End, the dragon) stops a different song at once and starts within
//     half its minDelay;
//   • the music volume (the MUSIC_VOLUME attribute — the pale garden's 0 — and
//     here also the Hush's stillness) is approached by fadePlaying: a slow
//     rise, a 3%-a-tick fall, and a song faded to nothing stops.
//
// Situations (getSituationalMusic): no world → MENU; the credits → CREDITS;
// the End with a music-playing boss bar → END_BOSS; otherwise the
// BACKGROUND_MUSIC attribute at the camera, picked for creative / underwater
// (client/sound/AudioAttributes).
//
// Main thread; ticked by SoundHost once per client tick.
#pragma once

#include "client/sound/AudioAttributes.hpp"
#include "client/sound/SoundHost.hpp"
#include "common/core/JavaRandom.hpp"

#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace Client {

    class SoundInstance;

    using Music = AudioAttributes::Music;

    // MC sounds.Musics.
    namespace Musics {
        inline const Music MENU{"music.menu", 20, 600, true};
        inline const Music CREATIVE{"music.creative", 12000, 24000, false};
        inline const Music CREDITS{"music.credits", 0, 0, true};
        inline const Music END_BOSS{"music.dragon", 0, 0, true};
        inline const Music END{"music.end", 6000, 24000, true};
        inline const Music UNDER_WATER{"music.under_water", 12000, 24000, false};
        inline const Music GAME{"music.game", 12000, 24000, false};
    }

    // MC MusicManager.MusicFrequency: the most minutes between songs.
    enum class MusicFrequency : uint8_t { Default, Frequent, Constant };
    // options.txt `musicFrequency` names (MC getSerializedName).
    std::string_view MusicFrequencyName(MusicFrequency f);
    MusicFrequency   MusicFrequencyFromName(std::string_view name);

    class MusicManager {
    public:
        static MusicManager& Get();

        void Tick(const SoundHost::TickContext& context);

        void StartPlaying(const Music& music);
        void StopPlaying(const Music& music);
        void StopPlaying();
        bool IsPlayingMusic(const Music& music) const;
        // MC setMinutesBetweenSongs (the Music Frequency option).
        void SetMinutesBetweenSongs(MusicFrequency frequency);
        MusicFrequency GetMusicFrequency() const { return m_frequency; }

        // The world went away (quit to title, a new session): whatever was
        // playing was stopped with it; start over from MC's STARTING_DELAY.
        void OnWorldChanged();

        // The playing song's event id ("" when none) — for debug output.
        std::string CurrentMusic() const;

    private:
        MusicManager();

        std::optional<Music> SituationalMusic(const SoundHost::TickContext& context) const;
        float MusicVolume(const SoundHost::TickContext& context) const;
        int   NextSongDelay(const std::optional<Music>& music);
        bool  FadePlaying(float volume);

        Game::JavaRandom               m_random;
        std::shared_ptr<SoundInstance> m_currentMusic;
        std::string                    m_currentMusicEvent;
        MusicFrequency                 m_frequency = MusicFrequency::Default;
        float                          m_currentGain = 1.0f;
        int                            m_nextSongDelay = 100;   // MC STARTING_DELAY
        std::optional<Music>           m_lastSituational;       // for StopPlaying's re-arm
    };

} // namespace Client
