// File: src/common/world/block/entity/JukeboxBlockEntity.hpp
//
// Mirrors net.minecraft.world.level.block.entity.JukeboxBlockEntity and
// net.minecraft.world.item.JukeboxSongPlayer — the disc in a jukebox and the
// song it is playing.
//
//   item          the disc (MC "RecordItem"); EMPTY when there is none
//   song player   the song and how many ticks it has been playing (MC
//                 "ticks_since_song_started"). Play raises level event 1010
//                 (the clients within 64 blocks start the streamed song),
//                 Stop raises 1011. While a song plays the jukebox is a
//                 redstone source (JukeboxBlock.ownSignal 15) and every 20
//                 ticks it throws a NOTE particle 1.2 above its top; when the
//                 song has run its length plus 20 ticks it stops by itself.
//
// It is a one-slot container (MC ContainerSingleItem.BlockContainerSingleItem):
// hoppers and droppers put a disc in (only a JUKEBOX_PLAYABLE item, only into
// an empty jukebox) and hoppers pull it out again, each going through
// setTheItem, so the song starts and stops exactly as with a player's hand.
//
// Wire (BlockEntityDataS2C, Save/Load): the song id + 1 (0 = none) and the
// ticks it has played. MC sends a jukebox's client nothing — its particles
// come from the server's sendParticles — so this engine's client, which has
// no particle packet for them, runs the song player's particle cadence
// itself off the synced state (ClientTick). That also gives a player who
// walks up to a jukebox mid-song the notes MC would show them; the music
// itself only ever starts from level event 1010, as in vanilla.
//
// Disk (BlockEntityNbt.cpp): RecordItem (ItemStack codec) when there is a
// disc, ticks_since_song_started while a song is set. A song loaded from
// disk is set without playing (setSongWithoutPlaying): it keeps ticking —
// signal, comparator, particles — but no client hears it.
#pragma once

#include "BlockEntity.hpp"
#include "common/entity/Item.hpp"
#include "common/inventory/Container.hpp"

#include "common/world/level/DimensionId.hpp"

#include <cstdint>
#include <functional>

namespace Game {

    struct JukeboxSong;
    class  JukeboxBlockEntity;

    // The server's record of every jukebox song playing, in every dimension
    // — what the "Jukebox Range: Global" sound option needs (engine
    // addition; MC tells only the players within 64 blocks, through level
    // events 1010 / 1011, which this engine still does for everyone on
    // Normal range). Each change goes out to every player through the sound
    // sink's JukeboxSongEverywhere (JukeboxSongS2C); a joining player gets
    // the whole list (ForEach). Server thread; guarded anyway.
    //
    // A song leaves the record when its player stops it (ejected, broken,
    // finished) and when its jukebox stops being simulated — its chunk
    // unloaded, or its dimension idle — which is MC's setRemoved → 1011: a
    // pass of the dimension's block entities that did not tick it (EndTick)
    // drops it. A jukebox found ticking with a song the record lacks (a chunk
    // back from disk mid-song) is added at its current position in the song.
    namespace JukeboxSongRegistry {
        struct Entry {
            DimensionId dimension = DimensionId::Overworld;
            glm::ivec3  pos{0};
            int         songId = -1;
            int64_t     ticks = 0;
        };

        // JukeboxSongPlayer.play: a fresh start.
        void OnPlay(DimensionId dimension, const glm::ivec3& pos, int songId);
        // JukeboxSongPlayer.stop.
        void OnStop(DimensionId dimension, const glm::ivec3& pos);
        // The song player's server tick: still playing, `ticks` in.
        void Touch(DimensionId dimension, const glm::ivec3& pos, int songId, int64_t ticks);
        // After a pass over `dimension`'s block entities (or a tick the
        // dimension was not simulated at all): drop every song not touched.
        void EndTick(DimensionId dimension);
        // Every song playing, for a joining player.
        void ForEach(const std::function<void(const Entry&)>& fn);
        // A new server session: nothing plays yet.
        void Clear();
    } // namespace JukeboxSongRegistry

    // MC JukeboxSongPlayer.
    class JukeboxSongPlayer {
    public:
        // MC PLAY_EVENT_INTERVAL_TICKS: the JUKEBOX_PLAY game event and the
        // note particle, every 20 ticks from the first.
        static constexpr int kPlayEventIntervalTicks = 20;

        JukeboxSongPlayer(JukeboxBlockEntity& owner, glm::ivec3 blockPos)
            : m_owner(owner), m_blockPos(blockPos) {}

        bool IsPlaying() const { return m_songId >= 0; }
        const JukeboxSong* GetSong() const;
        int     GetSongId() const { return m_songId; }
        int64_t GetTicksSinceSongStarted() const { return m_ticksSinceSongStarted; }

        // MC setSongWithoutPlaying: taken only if the song has not already
        // run out at that tick count.
        void SetSongWithoutPlaying(int songId, int64_t ticksSinceSongStarted);
        // The client's copy of a stopped song (the wire's "no song"): cleared
        // without an event — the server's own Stop already raised 1011.
        void ClearSongWithoutStopping() { m_songId = -1; m_ticksSinceSongStarted = 0; }

        // MC play: from the top, level event 1010 with the song id, and the
        // owner's onSongChanged.
        void Play(ILevelWrite& level, int songId);

        // MC stop: only when a song is set — level event 1011 and
        // onSongChanged.
        void Stop(ILevelWrite& level);

        // MC tick (the server's ticker): stop a finished song, else emit the
        // particle on the interval and count the tick.
        void Tick(ILevelWrite& level);

        // The client's half: the same cadence over the synced state, NOTE
        // particles only (spawnMusicParticles), never an event.
        void ClientTick(ILevelWrite& level);

    private:
        bool ShouldEmitJukeboxPlayingEvent() const {
            return m_ticksSinceSongStarted % kPlayEventIntervalTicks == 0;
        }
        static void SpawnMusicParticles(ILevelWrite& level, const glm::ivec3& blockPos);

        JukeboxBlockEntity& m_owner;
        glm::ivec3          m_blockPos;
        int                 m_songId = -1;
        int64_t             m_ticksSinceSongStarted = 0;
    };

    class JukeboxBlockEntity : public BlockEntity, public IContainer {
    public:
        JukeboxBlockEntity(const BlockEntityType* type, glm::ivec3 worldPos, BlockID blockId)
            : BlockEntity(type, worldPos, blockId), m_songPlayer(*this, worldPos) {}

        JukeboxSongPlayer&       GetSongPlayer()       { return m_songPlayer; }
        const JukeboxSongPlayer& GetSongPlayer() const { return m_songPlayer; }

        // MC onSongChanged: the neighbours re-read the signal and the
        // comparators the reading.
        void OnSongChanged();

        // MC popOutTheItem: the disc hops out of the top (server only).
        void PopOutTheItem();

        // MC getComparatorOutput: the disc's song's comparator_output, 0
        // without a (playable) disc.
        int GetComparatorOutput() const;

        // MC getTheItem / setTheItem / splitTheItem.
        const ItemStack& GetTheItem() const { return m_item; }
        void      SetTheItem(const ItemStack& stack);
        ItemStack SplitTheItem(int count);

        // MC loadAdditional, from disk: the disc and, when the tag has one,
        // the song's elapsed ticks (setSongWithoutPlaying). No events.
        void LoadFromNbt(ItemStack item, bool hasTicks, int64_t ticksSinceSongStarted);

        // ── BlockEntity ───────────────────────────────────────────────────
        // MC getTicker: only while HAS_RECORD, and the ticker does nothing
        // without a song — so the song is the test.
        bool NeedsTicking() const override { return m_songPlayer.IsPlaying(); }
        void Tick(World* world, float deltaTime) override;
        void ClientTick(ILevelWrite& level) override;
        // MC preRemoveSideEffects: popOutTheItem.
        void PreRemoveSideEffects(ILevelWrite& level, const glm::ivec3& pos, BlockState oldState) override;
        void Save(Network::PacketBuffer& out) const override;
        void Load(Network::PacketReader& in) override;

        // ── IContainer (MC ContainerSingleItem) ───────────────────────────
        int GetContainerSize() const override { return 1; }
        ItemStack&       GetItem(int index) override;
        const ItemStack& GetItem(int index) const override;
        // setItem(0, stack) → setTheItem.
        void SetItem(int index, const ItemStack& stack) override;
        // removeItem(0, count) → splitTheItem.
        ItemStack RemoveItem(int slot, int count) override;
        // MC getMaxStackSize: 1.
        int GetMaxStackSize() const override { return 1; }
        // MC canPlaceItem: a JUKEBOX_PLAYABLE stack into an empty jukebox.
        bool CanPlaceItem(int slot, const ItemStack& stack) const override;
        // MC canTakeItem: the taker must have an empty slot.
        bool CanTakeItem(const IContainer& into, int slot, const ItemStack& stack) const override;
        // MC BlockEntity.setChanged.
        void SetChanged() override;

    private:
        // MC notifyItemChangedInJukebox: HAS_RECORD follows the disc while
        // the jukebox is still in the world.
        void NotifyItemChangedInJukebox(ILevelWrite& level, bool wasInserted);

        ItemStack         m_item{};
        JukeboxSongPlayer m_songPlayer;
    };

} // namespace Game
