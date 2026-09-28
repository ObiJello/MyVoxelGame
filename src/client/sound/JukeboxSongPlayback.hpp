// File: src/client/sound/JukeboxSongPlayback.hpp
//
// The client half of the jukebox — MC LevelEventHandler's
// playingJukeboxSongs and the two level events that drive it — plus the
// engine's "Jukebox Range" sound option.
//
// NORMAL range (the default; vanilla):
//   1010 → Play(songId, pos)       LevelEventHandler.playJukeboxSong: stop
//                                  whatever that jukebox was playing, start
//                                  the song as a streamed, positional
//                                  RECORDS sound at the block centre
//                                  (SimpleSoundInstance.forJukeboxSong:
//                                  volume 4, linear attenuation — 64
//                                  blocks), put "Now Playing: <description>"
//                                  on the action bar in cycling colour
//                                  (Hud.setNowPlaying) and tell the living
//                                  entities within 3 blocks
//                                  (notifyNearbyEntities → parrots dance).
//   1011 → StopAndNotifyNearby(pos) stopJukeboxSongAndNotifyNearby.
//   MC's client also raises 1011 locally when the jukebox's block entity is
//   removed on the client (JukeboxBlockEntity.setRemoved → ClientLevel
//   .levelEvent): the block broken or replaced, or its chunk unloaded.
//   Tick() is that path here. A player who arrives mid-song does not hear
//   it, as in vanilla.
//
// GLOBAL range ("jukeboxRange" = "global" in options.txt): every song on the
// server plays for this player — any distance, any dimension — unplaced (no
// attenuation, full volume of the Jukebox/Note Blocks slider), from the
// server's JukeboxSongS2C (OnJukeboxSong), which reaches every client
// whatever its range. The client keeps that record on both settings, so
// switching to Global mid-song, joining the server, or changing dimension
// (which stops every sound) all pick each song up where it is: the stream
// is opened that far into the file (OggAudioStream::SeekToSeconds). A song
// stops when its jukebox's song stops — ejected, broken, run out, or the
// jukebox no longer simulated. The positional 1010 sound is not played on
// Global (it would double the song); the nearby-entity notices still are.
//
// Main thread only (the packet handler and the client tick).
#pragma once

#include "common/world/level/DimensionId.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <string>

namespace Client::JukeboxSongPlayback {

    // MC LevelEventHandler.playJukeboxSong (level event 1010). An unknown
    // song id is ignored (MC: the registry lookup is empty). `startTicks`
    // > 0 picks the song up that far in (the stream is opened there): its
    // jukebox's chunk reached this client mid-song, or the song resumed
    // after its chunk came back. "Now Playing" shows either way — the
    // player newly hears it.
    void Play(int songId, const glm::ivec3& pos, int64_t startTicks = 0);

    // MC stopJukeboxSongAndNotifyNearby (level event 1011).
    void StopAndNotifyNearby(const glm::ivec3& pos);

    // JukeboxSongS2C: the song at (dimension, pos) — `songId` -1 when it
    // stopped — `ticks` into it; `fresh` for a new start.
    void OnJukeboxSong(Game::DimensionId dimension, const glm::ivec3& pos, int songId,
                       int64_t ticks, bool fresh);

    // Once per client tick: the song clock (held while `paused`), the
    // option's changes, the client-side block-entity removal (Normal), and
    // the global songs that should be playing but are not (Global).
    void Tick(bool paused);

    // The level changed (every sound stops with it): the positional songs
    // are gone for good, the global ones start again, in place, next tick.
    void StopAll();

    // Leaving the world: everything, the global record included.
    void Reset();

    // Whether a song is playing at `pos` in the level this client draws: a
    // positional one, or the server's record (JukeboxSongS2C).
    bool IsPlayingAt(const glm::ivec3& pos);

    // Every jukebox playing a song in the level this client draws, positional
    // or on the server's record (the parrots' one-time check on arrival,
    // ClientLevelBridge::GetPlayingJukeboxes).
    void ForEachPlaying(const std::function<void(const glm::ivec3&)>& fn);

    // The "Jukebox Range" option: true for Global.
    bool IsGlobalRange();

    // Hud.setNowPlaying: the action bar line, already translated
    // ("Now Playing: C418 - cat"), to be shown with the animated colour.
    // Installed once by PlatformMain, which owns the HUD.
    void SetNowPlayingHandler(std::function<void(const std::string&)> handler);

} // namespace Client::JukeboxSongPlayback
