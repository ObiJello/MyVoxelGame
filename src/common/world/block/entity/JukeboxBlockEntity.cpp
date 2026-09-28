// File: src/common/world/block/entity/JukeboxBlockEntity.cpp
//
// MC JukeboxBlockEntity + JukeboxSongPlayer, method by method.
#include "JukeboxBlockEntity.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/JukeboxSongs.hpp"
#include "common/network/PacketRegistry.hpp"
#include "common/sound/LevelSound.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/block/RedstoneStateUtil.hpp"
#include "common/world/level/ILevelWrite.hpp"
#include "common/world/level/World.hpp"
#include "common/world/level/WorldDrops.hpp"
#include "common/world/level/gameevent/GameEvent.hpp"

#include <algorithm>
#include <mutex>
#include <vector>

namespace Game {

    namespace {

        // MC LevelEvent.SOUND_PLAY_JUKEBOX_SONG / SOUND_STOP_JUKEBOX_SONG.
        constexpr int kLevelEventPlayJukeboxSong = 1010;
        constexpr int kLevelEventStopJukeboxSong = 1011;

        // A random for the client's particle colour when its level has none.
        JavaRandom& FallbackRandom() {
            static thread_local JavaRandom random(static_cast<int64_t>(Sound::NextSeed()));
            return random;
        }

        // MC Level.levelEvent(null, type, pos, data) for the server side; the
        // client never raises these (its level events come off the wire).
        void LevelEvent(ILevelWrite& level, int type, const glm::ivec3& pos, int data) {
            if (level.IsClientSide()) return;
            Sound::BroadcastLevelEvent(level.GetDimension(), SoundExcept(nullptr), type, pos, data);
        }

    } // namespace

    // ── JukeboxSongRegistry ──────────────────────────────────────────────────

    namespace JukeboxSongRegistry {

        namespace {
            struct Record {
                Entry entry;
                bool  touched = false;
            };
            std::mutex          g_mutex;
            std::vector<Record> g_records;

            struct Announcement {
                Entry entry;
                bool  fresh = false;
            };

            std::vector<Record>::iterator Find(DimensionId dimension, const glm::ivec3& pos) {
                return std::find_if(g_records.begin(), g_records.end(), [&](const Record& r) {
                    return r.entry.dimension == dimension && r.entry.pos == pos;
                });
            }

            // Sent after the lock is released: the sink takes its own.
            void Announce(const std::vector<Announcement>& list) {
                Sound::ServerSoundSink* sink = Sound::GetServerSink();
                if (!sink) return;
                for (const Announcement& a : list) {
                    sink->JukeboxSongEverywhere(a.entry.dimension, a.entry.pos, a.entry.songId,
                                                a.entry.ticks, a.fresh);
                }
            }

            // A song paused because its jukebox left the simulation: MC's
            // JukeboxBlockEntity.setRemoved → levelEvent(1011) for the
            // players near it (Normal range), besides the global stop.
            void AnnouncePauses(const std::vector<Announcement>& list) {
                for (const Announcement& a : list) {
                    Sound::BroadcastLevelEvent(a.entry.dimension, SoundExcept(nullptr), 1011, a.entry.pos, 0);
                }
            }
        } // namespace

        void OnPlay(DimensionId dimension, const glm::ivec3& pos, int songId) {
            Announcement a;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                auto it = Find(dimension, pos);
                if (it == g_records.end()) {
                    g_records.push_back(Record{});
                    it = g_records.end() - 1;
                }
                it->entry = Entry{dimension, pos, songId, 0};
                it->touched = true;
                a = Announcement{it->entry, true};
            }
            Announce({a});
        }

        void OnStop(DimensionId dimension, const glm::ivec3& pos) {
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                auto it = Find(dimension, pos);
                if (it == g_records.end()) return;
                g_records.erase(it);
            }
            Announce({Announcement{Entry{dimension, pos, -1, 0}, false}});
        }

        void Touch(DimensionId dimension, const glm::ivec3& pos, int songId, int64_t ticks) {
            std::vector<Announcement> out;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                auto it = Find(dimension, pos);
                if (it == g_records.end() || it->entry.songId != songId) {
                    if (it == g_records.end()) {
                        g_records.push_back(Record{});
                        it = g_records.end() - 1;
                    }
                    it->entry = Entry{dimension, pos, songId, ticks};
                    // A song the record lacks is one RESUMING — its chunk
                    // back (from an unload or a reopened world), or its
                    // dimension simulated again: announced like a start,
                    // "Now Playing" and all, at its position.
                    out.push_back(Announcement{it->entry, true});
                }
                it->entry.ticks = ticks;
                it->touched = true;
            }
            Announce(out);
        }

        void EndTick(DimensionId dimension) {
            std::vector<Announcement> out;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                for (auto it = g_records.begin(); it != g_records.end();) {
                    if (it->entry.dimension != dimension) { ++it; continue; }
                    if (!it->touched) {
                        // Paused (not stopped): the jukebox keeps its song
                        // and position, saved with its chunk, and resumes
                        // from there (Touch) when it ticks again.
                        out.push_back(Announcement{Entry{dimension, it->entry.pos, -1, 0}, false});
                        it = g_records.erase(it);
                        continue;
                    }
                    it->touched = false;
                    ++it;
                }
            }
            Announce(out);
            AnnouncePauses(out);
        }

        void ForEach(const std::function<void(const Entry&)>& fn) {
            std::vector<Entry> copy;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                copy.reserve(g_records.size());
                for (const Record& r : g_records) copy.push_back(r.entry);
            }
            for (const Entry& e : copy) fn(e);
        }

        void Clear() {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_records.clear();
        }

    } // namespace JukeboxSongRegistry

    // ── JukeboxSongPlayer ────────────────────────────────────────────────────

    const JukeboxSong* JukeboxSongPlayer::GetSong() const {
        return JukeboxSongs::Get(m_songId);
    }

    void JukeboxSongPlayer::SetSongWithoutPlaying(int songId, int64_t ticksSinceSongStarted) {
        const JukeboxSong* song = JukeboxSongs::Get(songId);
        if (!song || song->HasFinished(ticksSinceSongStarted)) return;
        m_songId = songId;
        m_ticksSinceSongStarted = ticksSinceSongStarted;
    }

    void JukeboxSongPlayer::Play(ILevelWrite& level, int songId) {
        if (!JukeboxSongs::Get(songId)) return;
        m_songId = songId;
        m_ticksSinceSongStarted = 0;
        // level.levelEvent(null, 1010, blockPos, songId): the registry id of
        // the song, which the client looks back up.
        LevelEvent(level, kLevelEventPlayJukeboxSong, m_blockPos, songId);
        if (!level.IsClientSide()) JukeboxSongRegistry::OnPlay(level.GetDimension(), m_blockPos, songId);
        m_owner.OnSongChanged();
    }

    void JukeboxSongPlayer::Stop(ILevelWrite& level) {
        if (m_songId < 0) return;
        m_songId = -1;
        m_ticksSinceSongStarted = 0;
        // gameEvent(JUKEBOX_STOP_PLAY, blockPos, Context.of(blockState)) —
        // what an allay or a sculk sensor hears — then the level event the
        // clients act on.
        level.GameEvent(GameEventId::JukeboxStopPlay, m_blockPos,
                        GameEventContext::Of(level.GetBlockState(m_blockPos.x, m_blockPos.y, m_blockPos.z)));
        LevelEvent(level, kLevelEventStopJukeboxSong, m_blockPos, 0);
        if (!level.IsClientSide()) JukeboxSongRegistry::OnStop(level.GetDimension(), m_blockPos);
        m_owner.OnSongChanged();
    }

    void JukeboxSongPlayer::Tick(ILevelWrite& level) {
        const JukeboxSong* song = GetSong();
        if (!song) return;
        if (song->HasFinished(m_ticksSinceSongStarted)) {
            Stop(level);
            return;
        }
        // shouldEmitJukeboxPlayingEvent → gameEvent(JUKEBOX_PLAY, blockPos,
        // Context.of(blockState)) and spawnMusicParticles, which is the
        // server's sendParticles — drawn by each client from the synced
        // state instead (ClientTick).
        if (ShouldEmitJukeboxPlayingEvent()) {
            level.GameEvent(GameEventId::JukeboxPlay, m_blockPos,
                            GameEventContext::Of(level.GetBlockState(m_blockPos.x, m_blockPos.y, m_blockPos.z)));
        }
        ++m_ticksSinceSongStarted;
        // The position is saved with the chunk (ticks_since_song_started),
        // so a chunk that unloads mid-song resumes exactly here.
        m_owner.MarkDirty();
        // Still playing: the global record's position in the song.
        if (!level.IsClientSide()) {
            JukeboxSongRegistry::Touch(level.GetDimension(), m_blockPos, m_songId, m_ticksSinceSongStarted);
        }
    }

    void JukeboxSongPlayer::ClientTick(ILevelWrite& level) {
        const JukeboxSong* song = GetSong();
        if (!song) return;
        if (song->HasFinished(m_ticksSinceSongStarted)) {
            // The server stops it this same tick and says so; until its
            // update lands the client simply has nothing more to draw.
            m_songId = -1;
            m_ticksSinceSongStarted = 0;
            return;
        }
        if (ShouldEmitJukeboxPlayingEvent()) SpawnMusicParticles(level, m_blockPos);
        ++m_ticksSinceSongStarted;
    }

    void JukeboxSongPlayer::SpawnMusicParticles(ILevelWrite& level, const glm::ivec3& blockPos) {
        // Vec3.atBottomCenterOf(blockPos).add(0, 1.2F, 0); colour
        // nextInt(4) / 24F; sendParticles(NOTE, x, y, z, count 0, xDist =
        // colour, 0, 0, speed 1) — a count of 0 hands the client one particle
        // whose "velocity" is (xDist * speed, ...), i.e. NoteParticle's colour.
        const double x = static_cast<double>(blockPos.x) + 0.5;
        const double y = static_cast<double>(blockPos.y) + static_cast<double>(1.2f);
        const double z = static_cast<double>(blockPos.z) + 0.5;
        JavaRandom* random = level.Random();
        const int pick = (random ? *random : FallbackRandom()).NextInt(4);
        const float color = static_cast<float>(pick) / 24.0f;
        level.AddParticle(ParticleKind::Note, x, y, z, static_cast<double>(color), 0.0, 0.0);
    }

    // ── JukeboxBlockEntity ───────────────────────────────────────────────────

    void JukeboxBlockEntity::SetChanged() {
        // MC BlockEntity.setChanged → Level.blockEntityChanged (saved; here
        // also the clients' copy of the song state) and, since the jukebox
        // has an analog output, updateNeighbourForOutputSignal.
        MarkDirty();
        if (ILevelWrite* level = GetLevel()) {
            level->BlockEntityChanged(GetWorldPos());
            if (BlockRegistry::Get(GetBlockId()).hasAnalogOutputSignal) {
                level->UpdateNeighbourForOutputSignal(GetWorldPos(), GetBlockId());
            }
        }
    }

    void JukeboxBlockEntity::OnSongChanged() {
        if (ILevelWrite* level = GetLevel()) level->UpdateNeighborsAt(GetWorldPos(), GetBlockId());
        SetChanged();
    }

    void JukeboxBlockEntity::NotifyItemChangedInJukebox(ILevelWrite& level, bool wasInserted) {
        // `level.getBlockState(pos) == this.getBlockState()`: the jukebox is
        // still standing (it is not, inside preRemoveSideEffects — the new
        // block is already there).
        const glm::ivec3& p = GetWorldPos();
        const BlockState state = level.GetBlockState(p.x, p.y, p.z);
        if (state.Block() != BlockID::Jukebox) return;
        const BlockState newState = WithBool(state, PropertyId::HAS_RECORD, wasInserted);
        level.SetBlock(p.x, p.y, p.z, newState, World::UpdateFlags::UpdateClients);
        // gameEvent(BLOCK_CHANGE, pos, Context.of(getBlockState())) — the
        // entity's own (updated) state.
        level.GameEvent(GameEventId::BlockChange, p, GameEventContext::Of(newState));
    }

    void JukeboxBlockEntity::PopOutTheItem() {
        ILevelWrite* level = GetLevel();
        if (!level || level->IsClientSide()) return;
        if (m_item.IsEmpty()) return;
        const ItemStack popped = m_item;
        // removeTheItem → splitTheItem(max) → setTheItem(EMPTY): the song
        // stops and HAS_RECORD clears.
        SplitTheItem(1);
        // Vec3.atLowerCornerWithOffset(pos, 0.5, 1.01, 0.5).offsetRandomXZ(
        // random, 0.7F), then new ItemEntity(level, x, y, z, stack) — its
        // own (nextDouble*0.2-0.1, 0.2, nextDouble*0.2-0.1) — with the
        // default 10-tick pickup delay.
        JavaRandom* random = level->Random();
        auto nextFloat  = [random]() { return random ? random->NextFloat() : 0.5f; };
        auto nextDouble = [random]() { return random ? random->NextDouble() : 0.5; };
        const glm::ivec3& p = GetWorldPos();
        const double ox = static_cast<double>((nextFloat() - 0.5f) * 0.7f);
        const double oz = static_cast<double>((nextFloat() - 0.5f) * 0.7f);
        const glm::dvec3 itemPos(p.x + 0.5 + ox, p.y + 1.01, p.z + 0.5 + oz);
        const double vx = nextDouble() * 0.2 - 0.1;
        const double vz = nextDouble() * 0.2 - 0.1;
        SpawnItemEntity(level->GetDimension(), itemPos, glm::dvec3(vx, 0.2, vz), popped, 10);
        OnSongChanged();
    }

    int JukeboxBlockEntity::GetComparatorOutput() const {
        const JukeboxSong* song = JukeboxSongs::FromStack(m_item);
        return song ? song->comparatorOutput : 0;
    }

    void JukeboxBlockEntity::SetTheItem(const ItemStack& stack) {
        m_item = stack;
        if (m_item.IsEmpty()) m_item = ItemStack{};
        const bool itemWasInserted = !m_item.IsEmpty();
        const int songId = JukeboxSongs::IdFromStack(m_item);
        ILevelWrite* level = GetLevel();
        if (!level) {
            // No level yet (a chunk still being installed): the state is the
            // disc's, and nobody is near to hear it start.
            if (itemWasInserted && songId >= 0) m_songPlayer.SetSongWithoutPlaying(songId, 0);
            MarkDirty();
            return;
        }
        NotifyItemChangedInJukebox(*level, itemWasInserted);
        if (itemWasInserted && songId >= 0) m_songPlayer.Play(*level, songId);
        else                                m_songPlayer.Stop(*level);
    }

    ItemStack JukeboxBlockEntity::SplitTheItem(int /*count*/) {
        // MC: the whole stack (a jukebox holds one disc), whatever `count`.
        ItemStack retrieved = m_item;
        SetTheItem(ItemStack{});
        return retrieved;
    }

    void JukeboxBlockEntity::LoadFromNbt(ItemStack item, bool hasTicks, int64_t ticksSinceSongStarted) {
        // MC loadAdditional: a different disc than the one held stops the
        // song (a /data merge on a live jukebox); then the disc, and the
        // song's position when the tag has one.
        if (!m_item.IsEmpty() && !IsSameItemSameComponents(item, m_item)) {
            if (ILevelWrite* level = GetLevel()) m_songPlayer.Stop(*level);
        }
        m_item = item.IsEmpty() ? ItemStack{} : std::move(item);
        if (hasTicks) {
            const int songId = JukeboxSongs::IdFromStack(m_item);
            if (songId >= 0) m_songPlayer.SetSongWithoutPlaying(songId, ticksSinceSongStarted);
        }
    }

    void JukeboxBlockEntity::Tick(World* world, float /*deltaTime*/) {
        if (!world) return;
        m_songPlayer.Tick(*world);
    }

    void JukeboxBlockEntity::ClientTick(ILevelWrite& level) {
        m_songPlayer.ClientTick(level);
    }

    void JukeboxBlockEntity::PreRemoveSideEffects(ILevelWrite& level, const glm::ivec3& pos,
                                                  BlockState oldState) {
        if (!GetLevel()) SetLevel(&level);
        PopOutTheItem();
        // MC setRemoved: gameEvent(JUKEBOX_STOP_PLAY, pos, Context.of(state))
        // for the jukebox leaving the level. (A chunk unload, the other half
        // of setRemoved, takes its listeners with it.)
        level.GameEvent(GameEventId::JukeboxStopPlay, pos, GameEventContext::Of(oldState));
    }

    void JukeboxBlockEntity::Save(Network::PacketBuffer& out) const {
        // The client's copy: which song (id + 1, 0 for none) and how far in.
        out.WriteVarInt(static_cast<uint32_t>(m_songPlayer.GetSongId() + 1));
        out.WriteVarLong(static_cast<uint64_t>(std::max<int64_t>(0, m_songPlayer.GetTicksSinceSongStarted())));
    }

    void JukeboxBlockEntity::Load(Network::PacketReader& in) {
        if (!in.HasMore()) return;
        const int songId = static_cast<int>(in.ReadVarInt()) - 1;
        const int64_t ticks = in.HasMore() ? static_cast<int64_t>(in.ReadVarLong()) : 0;
        // A block entity is reused across updates: "no song" must clear the
        // copy, or a stopped jukebox would read as playing until the song's
        // length ran out (parrots kept dancing to silence).
        if (songId >= 0) m_songPlayer.SetSongWithoutPlaying(songId, ticks);
        else             m_songPlayer.ClearSongWithoutStopping();
    }

    // ── IContainer ───────────────────────────────────────────────────────────

    ItemStack& JukeboxBlockEntity::GetItem(int index) {
        static ItemStack scratch{};
        if (index != 0) { scratch = ItemStack{}; return scratch; }
        return m_item;
    }

    const ItemStack& JukeboxBlockEntity::GetItem(int index) const {
        static const ItemStack kEmpty{};
        return index == 0 ? m_item : kEmpty;
    }

    void JukeboxBlockEntity::SetItem(int index, const ItemStack& stack) {
        if (index == 0) SetTheItem(stack);
    }

    ItemStack JukeboxBlockEntity::RemoveItem(int slot, int count) {
        // ContainerSingleItem.removeItem: slot 0 only.
        return slot == 0 ? SplitTheItem(count) : ItemStack{};
    }

    bool JukeboxBlockEntity::CanPlaceItem(int slot, const ItemStack& stack) const {
        return JukeboxSongs::IsJukeboxPlayable(stack) && GetItem(slot).IsEmpty();
    }

    bool JukeboxBlockEntity::CanTakeItem(const IContainer& into, int /*slot*/, const ItemStack& /*stack*/) const {
        // into.hasAnyMatching(ItemStack::isEmpty).
        const int size = into.GetContainerSize();
        for (int i = 0; i < size; ++i) {
            if (into.GetItem(i).IsEmpty()) return true;
        }
        return false;
    }

} // namespace Game
