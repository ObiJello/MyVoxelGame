// File: src/client/sound/JukeboxSongPlayback.cpp
#include "client/sound/JukeboxSongPlayback.hpp"

#include "client/entity/ClientMobManager.hpp"
#include "client/entity/ShoulderParrots.hpp"
#include "common/entity/ParrotDanceRange.hpp"
#include "client/sound/SoundInstance.hpp"
#include "client/sound/SoundManager.hpp"
#include "client/world/ClientChunkManager.hpp"
#include "client/world/ClientLevel.hpp"
#include "common/core/Log.hpp"
#include "common/entity/JukeboxSongs.hpp"
#include "common/entity/Mob.hpp"
#include "common/physics/Physics.hpp"
#include "common/text/TextComponent.hpp"
#include "common/world/block/Blocks.hpp"
#include "platform/GameDirectory.hpp"

#include <algorithm>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Client::JukeboxSongPlayback {

    namespace {

        struct PosHash {
            size_t operator()(const glm::ivec3& p) const noexcept {
                size_t h = static_cast<size_t>(static_cast<uint32_t>(p.x)) * 73856093u;
                h ^= static_cast<size_t>(static_cast<uint32_t>(p.y)) * 19349663u;
                h ^= static_cast<size_t>(static_cast<uint32_t>(p.z)) * 83492791u;
                return h;
            }
        };

        // "Jukebox Range: Global": the song unplaced, at the slider's full
        // volume wherever the jukebox is, started `offsetSeconds` in.
        class GlobalJukeboxSoundInstance final : public SimpleSoundInstance {
        public:
            GlobalJukeboxSoundInstance(std::string_view event, double offsetSeconds)
                : SimpleSoundInstance(event, Game::SoundSource::Records, 1.0f, 1.0f, UnseededSeed(),
                                      false, 0, Attenuation::None, 0.0, 0.0, 0.0, true),
                  m_offsetSeconds(offsetSeconds) {}
            double GetStartOffsetSeconds() const override { return m_offsetSeconds; }

        private:
            double m_offsetSeconds;
        };

        // A jukebox song placed at its jukebox, started `offsetSeconds` in
        // (SimpleSoundInstance.forJukeboxSong's RECORDS, volume 4, linear).
        class PositionalJukeboxSoundInstance final : public SimpleSoundInstance {
        public:
            PositionalJukeboxSoundInstance(std::string_view event, const glm::dvec3& pos, double offsetSeconds)
                : SimpleSoundInstance(event, Game::SoundSource::Records, 4.0f, 1.0f, UnseededSeed(),
                                      false, 0, Attenuation::Linear, pos.x, pos.y, pos.z, false),
                  m_offsetSeconds(offsetSeconds) {}
            double GetStartOffsetSeconds() const override { return m_offsetSeconds; }

        private:
            double m_offsetSeconds;
        };

        // A positional song (Normal range) and whether its jukebox has been
        // seen in this client's world yet: a song picked up with its chunk
        // can arrive a moment before the chunk is installed, so it is only
        // stopped for a missing jukebox once the jukebox was there — or when
        // the chunk never comes (kUnseenGraceTicks).
        struct PositionalSong {
            std::shared_ptr<SoundInstance> instance;
            bool seen = false;
            int  unseenTicks = 0;
        };
        constexpr int kUnseenGraceTicks = 100;

        // One song somewhere on the server (JukeboxSongS2C).
        struct GlobalSong {
            int     songId = -1;
            int64_t ticksAtReceipt = 0;   // the server's count when last told
            int64_t receivedAtTick = 0;   // g_clientTicks then
            std::shared_ptr<SoundInstance> instance;   // playing here (Global only)
        };

        struct GlobalKey {
            Game::DimensionId dimension;
            glm::ivec3        pos;
            bool operator==(const GlobalKey& o) const { return dimension == o.dimension && pos == o.pos; }
        };
        struct GlobalKeyHash {
            size_t operator()(const GlobalKey& k) const noexcept {
                return PosHash{}(k.pos) ^ (static_cast<size_t>(static_cast<uint8_t>(k.dimension)) * 2654435761u);
            }
        };

        // MC LevelEventHandler.playingJukeboxSongs (Normal range).
        std::unordered_map<glm::ivec3, PositionalSong, PosHash> g_playing;
        // Every song on the server (both ranges keep it; Global plays it).
        std::unordered_map<GlobalKey, GlobalSong, GlobalKeyHash> g_global;
        std::function<void(const std::string&)> g_nowPlaying;
        int64_t g_clientTicks = 0;
        bool    g_wasGlobal = false;

        // Past the song's end by this much with no stop from the server, a
        // record is dropped (a stop that never came).
        constexpr int64_t kStaleSlackTicks = 200;
        // A song this close to its end is not restarted — there is nothing
        // left worth opening a stream for.
        constexpr double kMinRemainingSeconds = 0.25;

        void ShowNowPlaying(const Game::JukeboxSong& song) {
            if (!g_nowPlaying) return;
            // Hud.setNowPlaying(song.description()): "record.nowPlaying".
            const Game::Text::Component message = Game::Text::Component::Translatable(
                "record.nowPlaying", {Game::Text::Component::Translatable(song.DescriptionKey())});
            g_nowPlaying(Game::Text::GetString(message));
        }

        // MC LevelEventHandler.stopJukeboxSong.
        void StopSong(const glm::ivec3& pos) {
            const auto it = g_playing.find(pos);
            if (it == g_playing.end()) return;
            std::shared_ptr<SoundInstance> instance = std::move(it->second.instance);
            g_playing.erase(it);
            if (instance) GetSoundManager().Stop(instance);
        }

        void StopGlobalInstance(GlobalSong& song) {
            if (song.instance) GetSoundManager().Stop(song.instance);
            song.instance.reset();
        }

        int64_t CurrentTicks(const GlobalSong& song) {
            return song.ticksAtReceipt + (g_clientTicks - song.receivedAtTick);
        }

        // Start (or restart) a global song where it now is.
        void StartGlobal(GlobalSong& entry) {
            const Game::JukeboxSong* song = Game::JukeboxSongs::Get(entry.songId);
            if (!song) return;
            const double offset = static_cast<double>(CurrentTicks(entry)) / 20.0;
            if (offset > static_cast<double>(song->lengthInSeconds) - kMinRemainingSeconds) return;
            auto instance = std::make_shared<GlobalJukeboxSoundInstance>(song->soundEvent, offset);
            entry.instance = instance;
            GetSoundManager().Play(instance);
        }

        // MC notifyNearbyEntities: every LivingEntity whose box touches
        // `new AABB(pos).inflate(3.0)` — the jukebox cell grown by three
        // blocks each way.
        void NotifyNearbyEntities(const glm::ivec3& pos, bool isPlaying) {
            if (!g_clientMobManager) return;
            // DELIBERATE DEVIATION: grown by the parrot dance range (8, see
            // ParrotDanceRange.hpp) instead of MC's 3, so every parrot that
            // can dance to this song hears it start.
            const Game::AABBd area = Game::AABBd::FromMinMax(
                glm::dvec3(pos) - glm::dvec3(Game::kParrotDanceRange),
                glm::dvec3(pos) + glm::dvec3(1.0 + Game::kParrotDanceRange));
            // Snapshot: a callee must be free to touch the mob list.
            // Every PARROT this client has loaded is told, wherever it is (an
            // engine addition): it keeps the jukebox for the song's length and
            // dances whenever it comes within range — one pass per song
            // start/stop, no polling.
            std::vector<Game::Mob*> nearby;
            for (const ClientMob* cm : g_clientMobManager->MobList()) {
                if (!cm || !cm->mob || cm->mob->IsRemoved()) continue;
                if (cm->mob->GetType() == Game::EntityTypeId::Parrot ||
                    cm->mob->GetAABBd().Intersects(area)) {
                    nearby.push_back(cm->mob.get());
                }
            }
            for (Game::Mob* mob : nearby) mob->SetRecordPlayingNearby(pos, isPlaying);
        }

        // Whether the jukebox's block entity still exists on this client —
        // MC's JukeboxBlockEntity.setRemoved is what fires the local 1011,
        // and a block entity is removed exactly when its block goes (broken,
        // replaced) or its chunk unloads. An unloaded chunk reads as air.
        bool JukeboxStillThere(const glm::ivec3& pos) {
            if (!g_clientChunkManager) return false;
            return g_clientChunkManager->GetBlockAt(pos) == Game::BlockID::Jukebox;
        }

    } // namespace

    bool IsGlobalRange() {
        return Platform::g_gameSettings.GetString("jukeboxRange", "normal") == "global";
    }

    void Play(int songId, const glm::ivec3& pos, int64_t startTicks) {
        const Game::JukeboxSong* song = Game::JukeboxSongs::Get(songId);
        if (!song) {
            Log::Warning("[Jukebox] level event 1010 at (%d,%d,%d) names unknown song %d",
                         pos.x, pos.y, pos.z, songId);
            return;
        }
        StopSong(pos);
        const double offset = static_cast<double>(std::max<int64_t>(0, startTicks)) / 20.0;
        if (!IsGlobalRange() && offset <= static_cast<double>(song->lengthInSeconds) - kMinRemainingSeconds) {
            // SimpleSoundInstance.forJukeboxSong(sound, Vec3.atCenterOf(pos)),
            // opened `offset` seconds in when picked up part-way.
            std::shared_ptr<SoundInstance> instance = std::make_shared<PositionalJukeboxSoundInstance>(
                song->soundEvent, glm::dvec3(pos) + glm::dvec3(0.5), offset);
            PositionalSong& entry = g_playing[pos];
            entry.instance = instance;
            entry.seen = JukeboxStillThere(pos);
            entry.unseenTicks = 0;
            GetSoundManager().Play(instance);
            ShowNowPlaying(*song);
        }
        // On Global the song (and its "Now Playing") comes from
        // JukeboxSongS2C; the entities near it are told either way.
        Log::Info("[Jukebox] client song start at (%d,%d,%d) (global=%d, song=%d, startTicks=%lld)",
                  pos.x, pos.y, pos.z, IsGlobalRange() ? 1 : 0, songId, static_cast<long long>(startTicks));
        NotifyNearbyEntities(pos, true);
        // Shoulder parrots dance too (an engine addition, ShoulderParrots).
        ShoulderParrots::OnSongStarted(pos);
    }

    void StopAndNotifyNearby(const glm::ivec3& pos) {
        Log::Info("[Jukebox] client song stop at (%d,%d,%d) (global=%d)",
                  pos.x, pos.y, pos.z, IsGlobalRange() ? 1 : 0);
        StopSong(pos);
        NotifyNearbyEntities(pos, false);
        ShoulderParrots::OnSongStopped(pos);
    }

    void OnJukeboxSong(Game::DimensionId dimension, const glm::ivec3& pos, int songId,
                       int64_t ticks, bool fresh) {
        const GlobalKey key{dimension, pos};
        const auto it = g_global.find(key);
        const bool existed = it != g_global.end();
        // The parrots (ground and shoulder) of the level this client draws
        // learn of the song here too: on Global range, or beyond the 1010
        // event's reach, this record is the client's only word of it.
        const bool thisLevel = dimension == ClientLevels::BoundDimension();
        if (songId < 0) {
            if (existed) {
                Log::Info("[Jukebox] client song stop at (%d,%d,%d) (global record, dim=%d)",
                          pos.x, pos.y, pos.z, static_cast<int>(dimension));
                StopGlobalInstance(it->second);
                g_global.erase(it);
                if (thisLevel && !IsPlayingAt(pos)) {
                    NotifyNearbyEntities(pos, false);
                    ShoulderParrots::OnSongStopped(pos);
                }
            }
            return;
        }
        const Game::JukeboxSong* song = Game::JukeboxSongs::Get(songId);
        if (!song) return;

        GlobalSong& entry = g_global[key];
        const bool sameSongPlaying = existed && entry.songId == songId && !fresh;
        if (!sameSongPlaying) {
            Log::Info("[Jukebox] client song start at (%d,%d,%d) (global record, dim=%d, song=%d, fresh=%d)",
                      pos.x, pos.y, pos.z, static_cast<int>(dimension), songId, fresh ? 1 : 0);
            if (thisLevel) {
                NotifyNearbyEntities(pos, true);
                ShoulderParrots::OnSongStarted(pos);
            }
        }
        entry.ticksAtReceipt = ticks;
        entry.receivedAtTick = g_clientTicks;
        if (sameSongPlaying) return;   // a resync of what is already playing

        StopGlobalInstance(entry);
        entry.songId = songId;
        if (IsGlobalRange()) {
            StartGlobal(entry);
            if (fresh) ShowNowPlaying(*song);
        }
    }

    void Tick(bool paused) {
        if (!paused) ++g_clientTicks;

        const bool global = IsGlobalRange();
        if (global != g_wasGlobal) {
            g_wasGlobal = global;
            if (global) {
                // The positional songs give way to the global ones (which
                // start below, in place).
                for (auto& [pos, entry] : g_playing) {
                    if (entry.instance) GetSoundManager().Stop(entry.instance);
                }
                g_playing.clear();
            } else {
                for (auto& [key, song] : g_global) StopGlobalInstance(song);
            }
        }

        // Normal range: the client-side removal of a jukebox block entity.
        if (!g_playing.empty()) {
            std::vector<glm::ivec3> gone;
            for (auto& [pos, entry] : g_playing) {
                if (JukeboxStillThere(pos)) {
                    entry.seen = true;
                    continue;
                }
                if (entry.seen || ++entry.unseenTicks > kUnseenGraceTicks) gone.push_back(pos);
            }
            for (const glm::ivec3& pos : gone) StopAndNotifyNearby(pos);
        }

        // The global record: drop what ran out long ago (a stop that never
        // came), and on Global start whatever is not playing here.
        for (auto it = g_global.begin(); it != g_global.end();) {
            GlobalSong& song = it->second;
            const Game::JukeboxSong* def = Game::JukeboxSongs::Get(song.songId);
            if (!def || CurrentTicks(song) > static_cast<int64_t>(def->LengthInTicks()) +
                                                 Game::JukeboxSong::kSongEndPaddingTicks + kStaleSlackTicks) {
                StopGlobalInstance(song);
                it = g_global.erase(it);
                continue;
            }
            if (global && !paused && !song.instance) StartGlobal(song);
            ++it;
        }
    }

    void StopAll() {
        for (auto& [pos, entry] : g_playing) {
            if (entry.instance) GetSoundManager().Stop(entry.instance);
        }
        g_playing.clear();
        ShoulderParrots::OnAllSongsStopped();
        // The global songs are still playing on the server: Tick starts them
        // again where they are.
        for (auto& [key, song] : g_global) StopGlobalInstance(song);
    }

    void Reset() {
        StopAll();
        g_global.clear();
    }

    bool IsPlayingAt(const glm::ivec3& pos) {
        // The positional songs (Normal range), and the server's song record
        // for the level this client draws — which is all a Global-range
        // client has (it plays no positional songs).
        if (g_playing.find(pos) != g_playing.end()) return true;
        return g_global.find(GlobalKey{ClientLevels::BoundDimension(), pos}) != g_global.end();
    }

    void ForEachPlaying(const std::function<void(const glm::ivec3&)>& fn) {
        for (const auto& [pos, instance] : g_playing) fn(pos);
        const Game::DimensionId dim = ClientLevels::BoundDimension();
        for (const auto& [key, song] : g_global) {
            if (key.dimension == dim && g_playing.find(key.pos) == g_playing.end()) fn(key.pos);
        }
    }

    void SetNowPlayingHandler(std::function<void(const std::string&)> handler) {
        g_nowPlaying = std::move(handler);
    }

} // namespace Client::JukeboxSongPlayback
