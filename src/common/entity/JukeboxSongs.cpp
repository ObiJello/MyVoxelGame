// File: src/common/entity/JukeboxSongs.cpp
#include "common/entity/JukeboxSongs.hpp"

#include "common/entity/GeneratedItemList.hpp"
#include "common/text/Language.hpp"

#include <array>
#include <cmath>

namespace Game {

    namespace {

        // MC JukeboxSongs.bootstrap, in registration order: (key, sound
        // event, length in seconds, comparator output). The item column is
        // Items.java's `.jukeboxPlayable(JukeboxSongs.<KEY>)` pairing.
        const std::array<JukeboxSong, 22> kSongs = {{
            { "13",                "music_disc.13",                178.0f,  1, Items::MusicDisc13 },
            { "cat",               "music_disc.cat",               185.0f,  2, Items::MusicDiscCat },
            { "blocks",            "music_disc.blocks",            345.0f,  3, Items::MusicDiscBlocks },
            { "chirp",             "music_disc.chirp",             185.0f,  4, Items::MusicDiscChirp },
            { "far",               "music_disc.far",               174.0f,  5, Items::MusicDiscFar },
            { "mall",              "music_disc.mall",              197.0f,  6, Items::MusicDiscMall },
            { "mellohi",           "music_disc.mellohi",            96.0f,  7, Items::MusicDiscMellohi },
            { "stal",              "music_disc.stal",              150.0f,  8, Items::MusicDiscStal },
            { "strad",             "music_disc.strad",             188.0f,  9, Items::MusicDiscStrad },
            { "ward",              "music_disc.ward",              251.0f, 10, Items::MusicDiscWard },
            { "11",                "music_disc.11",                 71.0f, 11, Items::MusicDisc11 },
            { "wait",              "music_disc.wait",              238.0f, 12, Items::MusicDiscWait },
            { "pigstep",           "music_disc.pigstep",           149.0f, 13, Items::MusicDiscPigstep },
            { "otherside",         "music_disc.otherside",         195.0f, 14, Items::MusicDiscOtherside },
            { "5",                 "music_disc.5",                 178.0f, 15, Items::MusicDisc5 },
            { "relic",             "music_disc.relic",             218.0f, 14, Items::MusicDiscRelic },
            { "precipice",         "music_disc.precipice",         299.0f, 13, Items::MusicDiscPrecipice },
            { "creator",           "music_disc.creator",           176.0f, 12, Items::MusicDiscCreator },
            { "creator_music_box", "music_disc.creator_music_box",  73.0f, 11, Items::MusicDiscCreatorMusicBox },
            { "tears",             "music_disc.tears",             175.0f, 10, Items::MusicDiscTears },
            { "lava_chicken",      "music_disc.lava_chicken",      134.0f,  9, Items::MusicDiscLavaChicken },
            { "bounce",            "music_disc.bounce",            234.0f,  8, Items::MusicDiscBounce },
        }};

    } // namespace

    int JukeboxSong::LengthInTicks() const {
        // Mth.ceil(float): (int)f, plus one when f is above that.
        const float ticks = lengthInSeconds * 20.0f;
        const int truncated = static_cast<int>(ticks);
        return ticks > static_cast<float>(truncated) ? truncated + 1 : truncated;
    }

    std::string JukeboxSong::DescriptionKey() const {
        return "jukebox_song.minecraft." + std::string(key);
    }

    std::string JukeboxSong::Description() const {
        return Language::Get(DescriptionKey());
    }

    namespace JukeboxSongs {

        int Count() { return static_cast<int>(kSongs.size()); }

        const JukeboxSong* Get(int id) {
            if (id < 0 || id >= Count()) return nullptr;
            return &kSongs[static_cast<size_t>(id)];
        }

        int IdFromStack(const ItemStack& stack) {
            if (stack.IsEmpty()) return -1;
            for (size_t i = 0; i < kSongs.size(); ++i) {
                if (kSongs[i].discItem == stack.itemId) return static_cast<int>(i);
            }
            return -1;
        }

        int IdFromKey(std::string_view key) {
            for (size_t i = 0; i < kSongs.size(); ++i) {
                if (kSongs[i].key == key) return static_cast<int>(i);
            }
            return -1;
        }

    } // namespace JukeboxSongs

} // namespace Game
