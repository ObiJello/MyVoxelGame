// File: src/common/entity/JukeboxSongs.hpp
//
// MC net.minecraft.world.item.JukeboxSong + JukeboxSongs + the
// JUKEBOX_PLAYABLE item component, for the music discs.
//
//   JukeboxSong      one song: its sound event (a streamed "music_disc.*"
//                    event in sounds.json), its description key
//                    ("jukebox_song.minecraft.<song>" → "C418 - 13"), its
//                    length in seconds and the comparator reading a jukebox
//                    holding it gives.
//   JukeboxSongs     the registry, in JukeboxSongs.bootstrap order (26.3:
//                    22 songs, "13" .. "bounce"). The index is the song's
//                    network id — what level event 1010 carries as its data,
//                    exactly as MC sends the registry id of the song.
//   FromStack        MC JukeboxSong.fromStack: the song a stack plays. Only
//                    the music disc items are JUKEBOX_PLAYABLE (Items.java
//                    `.jukeboxPlayable(...)`); the engine keys the song on
//                    the item rather than on a stack component, so a stack
//                    cannot be given a different song with /give components.
//
// The table is JukeboxSongs.bootstrap verbatim (the data pack's
// data/minecraft/jukebox_song/*.json are generated from it and carry the same
// numbers — the extracted pack predates "bounce", so the table, not the pack,
// is the source here).
#pragma once

#include "common/entity/Item.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace Game {

    struct JukeboxSong {
        std::string_view key;              // registry path: "13", "cat", "lava_chicken"
        std::string_view soundEvent;       // "music_disc.13" (namespace minecraft)
        float            lengthInSeconds;  // JukeboxSong.lengthInSeconds
        int              comparatorOutput; // 0..15
        ItemID           discItem;         // the music_disc_<key> item that plays it

        // MC JukeboxSong.SONG_END_PADDING_TICKS.
        static constexpr int kSongEndPaddingTicks = 20;

        // MC lengthInTicks: Mth.ceil(lengthInSeconds * 20).
        int LengthInTicks() const;
        // MC hasFinished: ticksElapsed >= lengthInTicks + 20.
        bool HasFinished(int64_t ticksElapsed) const {
            return ticksElapsed >= static_cast<int64_t>(LengthInTicks() + kSongEndPaddingTicks);
        }
        // "jukebox_song.minecraft.<key>" — MC Util.makeDescriptionId.
        std::string DescriptionKey() const;
        // The translated description ("C418 - 13"), or the key when the
        // language file has no entry.
        std::string Description() const;
    };

    namespace JukeboxSongs {

        // Number of registered songs; valid ids are 0 .. Count() - 1.
        int Count();

        // The song with network id `id`, or null.
        const JukeboxSong* Get(int id);

        // MC JukeboxSong.fromStack: the song id the stack plays, -1 when the
        // stack is not JUKEBOX_PLAYABLE (or is empty).
        int IdFromStack(const ItemStack& stack);
        inline const JukeboxSong* FromStack(const ItemStack& stack) { return Get(IdFromStack(stack)); }

        // MC `stack.has(DataComponents.JUKEBOX_PLAYABLE)`.
        inline bool IsJukeboxPlayable(const ItemStack& stack) { return IdFromStack(stack) >= 0; }

        // The registry id by key ("pigstep"), -1 when unknown.
        int IdFromKey(std::string_view key);

    } // namespace JukeboxSongs

} // namespace Game
