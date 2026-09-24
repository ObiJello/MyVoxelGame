// File: src/client/sound/MusicManager.cpp
#include "client/sound/MusicManager.hpp"

#include "client/sound/AmbientSoundHandlers.hpp"
#include "client/sound/AurelithSounds.hpp"
#include "client/sound/SoundInstance.hpp"
#include "client/sound/SoundManager.hpp"
#include "platform/GameDirectory.hpp"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>

namespace Client {

    namespace {
        constexpr int kStartingDelay = 100;

        // MC MusicFrequency.maxFrequency: minutes × 1200 ticks.
        int MaxFrequencyTicks(MusicFrequency f) {
            switch (f) {
                case MusicFrequency::Default:  return 20 * 1200;
                case MusicFrequency::Frequent: return 10 * 1200;
                case MusicFrequency::Constant: return 0;
            }
            return 20 * 1200;
        }

        // MC Mth.nextInt(random, min, max): inclusive, min when max <= min.
        int NextIntBetween(Game::JavaRandom& random, int minInclusive, int maxInclusive) {
            return minInclusive >= maxInclusive ? minInclusive
                                                : random.NextInt(maxInclusive - minInclusive + 1) + minInclusive;
        }
    } // namespace

    std::string_view MusicFrequencyName(MusicFrequency f) {
        switch (f) {
            case MusicFrequency::Default:  return "DEFAULT";
            case MusicFrequency::Frequent: return "FREQUENT";
            case MusicFrequency::Constant: return "CONSTANT";
        }
        return "DEFAULT";
    }

    MusicFrequency MusicFrequencyFromName(std::string_view name) {
        if (name == "FREQUENT") return MusicFrequency::Frequent;
        if (name == "CONSTANT") return MusicFrequency::Constant;
        return MusicFrequency::Default;
    }

    MusicManager& MusicManager::Get() {
        static MusicManager instance;
        return instance;
    }

    MusicManager::MusicManager()
        : m_random(static_cast<int64_t>(std::chrono::steady_clock::now().time_since_epoch().count())) {
        // MC: gameMusicFrequency = options.musicFrequency().get().
        m_frequency = MusicFrequencyFromName(Platform::g_gameSettings.GetString("musicFrequency", "DEFAULT"));
        // Read the biome / dimension audio attributes now, on the title
        // screen's first tick, rather than as a hitch on the first world tick.
        AudioAttributes::Load();
    }

    // ── Situation ──────────────────────────────────────────────────────────

    std::optional<Music> MusicManager::SituationalMusic(const SoundHost::TickContext& ctx) const {
        // Screen.getBackgroundMusic first (the credits' WinScreen).
        if (ctx.creditsScreen) return Musics::CREDITS;
        if (!ctx.inWorld || !ctx.player) return Musics::MENU;
        if (ctx.dimension == Game::DimensionId::End && ctx.bossMusic) return Musics::END_BOSS;
        // Inside the walls of Aurelith the city decides, whatever biome it
        // stands on: its own tracks, or silence while the Chord is sung
        // (client/sound/AurelithSounds).
        if (const AurelithSounds::CityMusicChoice city = AurelithSounds::CityMusic(ctx); city.inCity) {
            return city.music;
        }
        const AudioAttributes::BackgroundMusic& background = AudioAttributes::BackgroundMusicAt(
            ctx.dimension, AudioAttributes::BiomeAt(ctx.cameraPosition));
        const Music* music = background.Select(ctx.creative, ctx.underwater);
        return music ? std::optional<Music>(*music) : std::nullopt;
    }

    float MusicManager::MusicVolume(const SoundHost::TickContext& ctx) const {
        // MC getMusicVolume: 1 while a screen plays its own music, else the
        // MUSIC_VOLUME attribute at the camera. The Hush's stillness pulls it
        // down too — the eerie silence it is meant to be.
        if (!ctx.inWorld || ctx.creditsScreen) return 1.0f;
        const float attribute = AudioAttributes::MusicVolumeAt(ctx.dimension,
                                                               AudioAttributes::BiomeAt(ctx.cameraPosition));
        // Aurelith fades out a song that does not belong where the camera
        // is (a biome song in an awakened city, anything while the Chord is
        // sung) so the city's own can follow — fadePlaying, never a cut.
        return attribute * AmbientSounds::StillnessGain()
             * AurelithSounds::MusicGain(ctx, m_currentMusicEvent);
    }

    int MusicManager::NextSongDelay(const std::optional<Music>& music) {
        // MC MusicFrequency.getNextSongDelay.
        const int maxFrequency = MaxFrequencyTicks(m_frequency);
        if (!music) return maxFrequency;
        if (m_frequency == MusicFrequency::Constant) return 100;
        const int minF = std::min(music->minDelay, maxFrequency);
        const int maxF = std::min(music->maxDelay, maxFrequency);
        return NextIntBetween(m_random, minF, maxF);
    }

    // ── Tick ───────────────────────────────────────────────────────────────

    void MusicManager::Tick(const SoundHost::TickContext& ctx) {
        SoundManager& sounds = GetSoundManager();
        const float volume = MusicVolume(ctx);
        if (m_currentMusic && m_currentGain != volume) {
            if (!FadePlaying(volume)) return;
        }

        const std::optional<Music> music = SituationalMusic(ctx);
        m_lastSituational = music;
        if (!music) {
            m_nextSongDelay = std::max(m_nextSongDelay, kStartingDelay);
            return;
        }

        if (m_currentMusic) {
            // MC canReplace: a replacing situation with a different song.
            if (music->replaceCurrentMusic && music->sound != m_currentMusicEvent) {
                sounds.Stop(m_currentMusic);
                m_nextSongDelay = NextIntBetween(m_random, 0, music->minDelay / 2);
            }
            if (!sounds.IsActive(m_currentMusic)) {
                m_currentMusic.reset();
                m_currentMusicEvent.clear();
                m_nextSongDelay = std::min(m_nextSongDelay, NextSongDelay(music));
            }
        }

        m_nextSongDelay = std::min(m_nextSongDelay, music->maxDelay);
        if (!m_currentMusic && !ctx.levelLoading && --m_nextSongDelay <= 0) {
            StartPlaying(*music);
        }
    }

    bool MusicManager::FadePlaying(float volume) {
        // MC fadePlaying.
        if (!m_currentMusic) return false;
        if (m_currentGain == volume) return true;
        if (m_currentGain < volume) {
            m_currentGain += std::clamp(m_currentGain, 5.0e-4f, 0.005f);
            if (m_currentGain > volume) m_currentGain = volume;
        } else {
            m_currentGain = 0.03f * volume + 0.97f * m_currentGain;
            if (std::fabs(m_currentGain - volume) < 1.0e-4f || m_currentGain < volume) m_currentGain = volume;
        }
        m_currentGain = std::clamp(m_currentGain, 0.0f, 1.0f);
        if (m_currentGain <= 1.0e-4f) {
            StopPlaying();
            return false;
        }
        GetSoundManager().UpdateCategoryVolume(Game::SoundSource::Music, m_currentGain);
        return true;
    }

    void MusicManager::StartPlaying(const Music& music) {
        m_currentMusic = SimpleSoundInstance::ForMusic(music.sound);
        m_currentMusicEvent = music.sound;
        // MC shows the "Now Playing" toast on STARTED; this engine has no
        // toast system, so the result is only what decides nothing else.
        (void)GetSoundManager().Play(m_currentMusic);
        m_nextSongDelay = INT_MAX;
    }

    void MusicManager::StopPlaying(const Music& music) {
        if (IsPlayingMusic(music)) StopPlaying();
    }

    void MusicManager::StopPlaying() {
        if (m_currentMusic) {
            GetSoundManager().Stop(m_currentMusic);
            m_currentMusic.reset();
            m_currentMusicEvent.clear();
        }
        m_nextSongDelay = NextSongDelay(m_lastSituational) + 100;
    }

    bool MusicManager::IsPlayingMusic(const Music& music) const {
        return m_currentMusic && music.sound == m_currentMusicEvent;
    }

    void MusicManager::SetMinutesBetweenSongs(MusicFrequency frequency) {
        m_frequency = frequency;
        m_nextSongDelay = NextSongDelay(m_lastSituational);
    }

    void MusicManager::OnWorldChanged() {
        // The song itself was stopped with every other sound (SoundHost's
        // StopAll); forget it, and the music category gain the engine reset.
        m_currentMusic.reset();
        m_currentMusicEvent.clear();
        m_currentGain = 1.0f;
        m_nextSongDelay = kStartingDelay;
    }

    std::string MusicManager::CurrentMusic() const { return m_currentMusicEvent; }

} // namespace Client
