// File: src/server/level/AurelithCities.cpp
//
// See AurelithCities.hpp for the design; the shared facts (geometry,
// timeline, sound names) are in common/world/level/AurelithQuest.hpp.
//
// Layout of this file:
//   the record              (City, Flicker; find, register)
//   the common bridges      (Aurelith::OnSocketSeated / BroadcastBurst /
//                            SuppressesHostileSpawn's list)
//   the Podium              (judging the Chord, the discord)
//   the awakening           (the timeline, the light wave, the flickers)
//   the fight               (raising the Unsung, watching it, the resolution)
//   chunks                  (registration, lighting on arrival)
//   clients                 (CityState / CityForget / Burst)
//   SavedData               (obeycraft_aurelith.dat)
//   /aurelith               (debug)
#include "server/level/AurelithCities.hpp"

#include "server/IntegratedServer.hpp"
#include "server/entity/MobManager.hpp"
#include "server/entity/ServerLevelBridge.hpp"
#include "server/level/ServerLevel.hpp"
#include "server/network/ServerConnection.hpp"
#include "server/player/ServerPlayer.hpp"
#include "server/session/PlayerSession.hpp"
#include "server/session/PlayerSessionManager.hpp"
#include "server/world/storage/NBTParser.hpp"

#include "common/core/Log.hpp"
#include "common/core/SaveVersion.hpp"
#include "common/core/Uuid.hpp"
#include "common/data/BookContent.hpp"
#include "common/data/DataComponents.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/entity/IUsePlayer.hpp"
#include "common/entity/Item.hpp"
#include "common/entity/Mob.hpp"
#include "common/entity/mobs/GenericMobs.hpp"
#include "common/entity/mobs/TheUnsung.hpp"
#include "common/nbt/NbtWrite.hpp"
#include "common/network/PacketRegistry.hpp"
#include "common/network/packets/game/AurelithS2CPacket.hpp"
#include "common/sound/AurelithSoundCues.hpp"
#include "common/sound/SoundSource.hpp"
#include "common/text/TextComponent.hpp"
#include "common/world/block/AurelithQuestBlocks.hpp"
#include "common/world/block/BlockState.hpp"
#include "common/world/block/RedstoneStateUtil.hpp"
#include "common/world/block/entity/AurelithBlockEntities.hpp"
#include "common/world/block/entity/LecternBlockEntity.hpp"
#include "common/world/block/entity/SignBlockEntity.hpp"
#include "common/world/chunk/Chunk.hpp"
#include "common/world/chunk/ChunkSection.hpp"
#include "common/world/level/World.hpp"
#include "common/world/level/WorldDrops.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <mutex>
#include <system_error>

namespace Server {

    namespace A = Game::Aurelith;
    using A::CityState;
    using A::Voice;
    using BurstStyle = Network::AurelithS2CPacket::BurstStyle;

    // ═══ The record ══════════════════════════════════════════════════════

    struct AurelithCities::City {
        glm::ivec3 heart{0};
        int        rotation = -1;
        CityState  state = CityState::Dormant;
        int64_t    stageStart = 0;
        int64_t    awakenedTick = 0;
        bool       heldNoteGiven = false;
        bool       codaRevealed = false;
        Game::Uuid bossUuid{};
        // Runtime only.
        int64_t    lastMark = -1;              // the last stage tick whose marks fired
        int64_t    bossLastSeen = 0;
        bool       bossWasDying = false;
        double     lastWaveRadius = -1.0;
        int64_t    lastWaveStep = 0;
        // The wave's work: chunks still to scan (nearest first, taken from
        // the back), and the dim cells found, nearest first from `waveNext`.
        std::vector<Game::Math::ChunkPos> waveScan;
        std::unordered_set<int64_t>       waveScanned;
        struct Cell { glm::ivec3 pos; float dist; };
        std::vector<Cell> waveCells;
        size_t            waveNext = 0;
        bool              waveCellsSorted = true;
    };

    // A stutter of the lights round a point: every lumen light (dim or lit)
    // within `radius`, toggled to its twin on the listed ticks and restored
    // at the end.
    struct AurelithCities::Flicker {
        int64_t start = 0;
        std::vector<std::pair<glm::ivec3, Game::BlockState>> cells;   // position, original state
        size_t step = 0;
    };

    namespace {

        // Ticks (from the flicker's start) on which the lights change: odd
        // entries invert, even entries restore — a ragged stutter, not a
        // metronome.
        constexpr int kFlickerSteps[] = { 0, 2, 4, 7, 9, 13, 16, 21 };
        constexpr int kFlickerStepCount = static_cast<int>(sizeof(kFlickerSteps) / sizeof(kFlickerSteps[0]));

        constexpr uint32_t kLightFlags = Game::World::UpdateFlags::UpdateClients |
                                         Game::World::UpdateFlags::KnownShape;

        int64_t PackChunk(const Game::Math::ChunkPos& p) {
            return static_cast<int64_t>((static_cast<uint64_t>(static_cast<uint32_t>(p.x))) |
                                        (static_cast<uint64_t>(static_cast<uint32_t>(p.z)) << 32));
        }

        double Horizontal(const glm::ivec3& heart, const glm::dvec3& p) {
            return std::hypot(p.x - (heart.x + 0.5), p.z - (heart.z + 0.5));
        }

        // The world direction of a design direction (0 N, 1 E, 2 S, 3 W).
        Game::Direction WorldDirection(int designDir, int rotation) {
            switch (A::RotateDirection(designDir, rotation < 0 ? 0 : rotation)) {
                case 1:  return Game::Direction::East;
                case 2:  return Game::Direction::South;
                case 3:  return Game::Direction::West;
                default: return Game::Direction::North;
            }
        }

        uint32_t HeartWhite() { return 0xE8FFFFu; }

        // ── The engine's words (tools/aurelith_books.py --emit-cpp) ──────
        struct EngineBook { const char* key; const char* title; const char* author; std::vector<const char*> pages; };
        struct EngineSign { const char* key; const char* lines[4]; };
#define AURELITH_BOOK(key, title, author, ...) EngineBook{ #key, title, author, { __VA_ARGS__ } },
#define AURELITH_SIGN(key, l0, l1, l2, l3)
        const std::vector<EngineBook>& EngineBooks() {
            static const std::vector<EngineBook> books = {
#include "common/world/level/GeneratedAurelithBooks.inc"
            };
            return books;
        }
#undef AURELITH_BOOK
#undef AURELITH_SIGN
#define AURELITH_BOOK(key, title, author, ...)
#define AURELITH_SIGN(key, l0, l1, l2, l3) EngineSign{ #key, { l0, l1, l2, l3 } },
        const std::vector<EngineSign>& EngineSigns() {
            static const std::vector<EngineSign> signs = {
#include "common/world/level/GeneratedAurelithBooks.inc"
            };
            return signs;
        }
#undef AURELITH_BOOK
#undef AURELITH_SIGN

        Game::ItemStack EngineWrittenBook(const char* key) {
            for (const EngineBook& b : EngineBooks()) {
                if (std::string_view(b.key) != key) continue;
                Game::WrittenBookContent content;
                content.title = Game::Filterable<std::string>::PassThrough(b.title);
                content.author = b.author;
                content.generation = 0;
                for (const char* page : b.pages) {
                    content.pages.push_back(Game::Filterable<Game::Text::Component>::PassThrough(
                        Game::Text::Component::Literal(page)));
                }
                content.resolved = true;
                Game::ItemStack stack(Game::Items::WrittenBook, 1);
                stack.components.set(Game::DataComponents::WRITTEN_BOOK_CONTENT, std::move(content));
                return stack;
            }
            return Game::ItemStack{};
        }

        const EngineSign* FindEngineSign(const char* key) {
            for (const EngineSign& s : EngineSigns()) {
                if (std::string_view(s.key) == key) return &s;
            }
            return nullptr;
        }

        // ── Hostile spawns: the awakened list the natural spawner asks ──
        std::mutex& AwakenedMutex() {
            static std::mutex m;
            return m;
        }
        std::unordered_map<int, std::vector<glm::ivec3>>& AwakenedByDimension() {
            static std::unordered_map<int, std::vector<glm::ivec3>> map;
            return map;
        }

        AurelithCities* CitiesFor(Game::DimensionId dimension) {
            IntegratedServer* server = g_integratedServer.get();
            ServerLevel* level = server ? server->GetLevel(dimension) : nullptr;
            return level ? level->Aurelith() : nullptr;
        }

    } // namespace

    // ═══ The common bridges ══════════════════════════════════════════════

} // namespace Server

namespace Game::Aurelith {

    void OnSocketSeated(ILevelWrite& level, const glm::ivec3& socket, IUsePlayer* player) {
        if (level.IsClientSide()) return;
        if (Server::AurelithCities* cities = Server::CitiesFor(level.GetDimension())) {
            cities->OnSocketSeated(socket, player);
        }
    }

    void BroadcastBurst(ILevelWrite& level, const glm::dvec3& origin, uint8_t style, uint32_t colour) {
        if (level.IsClientSide()) return;
        if (Server::AurelithCities* cities = Server::CitiesFor(level.GetDimension())) {
            cities->Burst(origin, style, colour);
        }
    }

    bool SuppressesHostileSpawn(DimensionId dimension, const glm::ivec3& pos) {
        std::lock_guard<std::mutex> lock(Server::AwakenedMutex());
        auto& map = Server::AwakenedByDimension();
        auto it = map.find(static_cast<int>(dimension));
        if (it == map.end()) return false;
        const glm::dvec3 p(pos.x + 0.5, pos.y + 0.5, pos.z + 0.5);
        for (const glm::ivec3& heart : it->second) {
            if (InsideWalls(heart, p)) return true;
        }
        return false;
    }

    void SetAwakenedCities(DimensionId dimension, const glm::ivec3* hearts, size_t count) {
        std::lock_guard<std::mutex> lock(Server::AwakenedMutex());
        Server::AwakenedByDimension()[static_cast<int>(dimension)].assign(hearts, hearts + count);
    }

} // namespace Game::Aurelith

namespace Server {

    AurelithCities::AurelithCities(ServerLevel& level, PlayerSessionManager* sessions,
                                   std::filesystem::path dataDir)
        : m_level(level), m_sessions(sessions), m_dataDir(std::move(dataDir)) {
        Load();
        PublishAwakened();
    }

    AurelithCities::~AurelithCities() = default;

    int64_t AurelithCities::Now() const {
        Game::World* world = m_level.World();
        return world ? world->GameTime() : 0;
    }

    AurelithCities::City* AurelithCities::Find(const glm::ivec3& heart) {
        for (auto& c : m_cities) if (c->heart == heart) return c.get();
        return nullptr;
    }

    const AurelithCities::City* AurelithCities::Find(const glm::ivec3& heart) const {
        for (const auto& c : m_cities) if (c->heart == heart) return c.get();
        return nullptr;
    }

    AurelithCities::City* AurelithCities::CityForPoint(const glm::ivec3& p, int reach) {
        City* best = nullptr;
        double bestD = static_cast<double>(reach);
        for (auto& c : m_cities) {
            const double d = Horizontal(c->heart, glm::dvec3(p) + glm::dvec3(0.5));
            if (d <= bestD && std::abs(p.y - c->heart.y) <= 16) { bestD = d; best = c.get(); }
        }
        return best;
    }

    AurelithCities::City* AurelithCities::Register(const glm::ivec3& heart, int rotation) {
        if (City* existing = Find(heart)) {
            // A record read from disk before the engine's chunk ever loaded
            // this session may lack the rotation.
            if (existing->rotation < 0 && rotation >= 0) {
                existing->rotation = rotation;
                m_dirty = true;
                BroadcastState(*existing);
            }
            return existing;
        }
        auto city = std::make_unique<City>();
        city->heart = heart;
        city->rotation = rotation;
        City* raw = city.get();
        m_cities.push_back(std::move(city));
        m_dirty = true;
        Log::Info("[Aurelith] A city's Heart at (%d, %d, %d), rotation %d", heart.x, heart.y, heart.z, rotation);
        return raw;
    }

    void AurelithCities::SetState(City& city, CityState state) {
        city.state = state;
        city.stageStart = Now();
        city.lastMark = -1;
        if (state == CityState::Awakened) city.awakenedTick = city.stageStart;
        m_dirty = true;
        BroadcastState(city);
        PublishAwakened();
        WriteFile();
    }

    void AurelithCities::PublishAwakened() {
        std::vector<glm::ivec3> hearts;
        for (const auto& c : m_cities) {
            if (c->state == CityState::Awakened) hearts.push_back(c->heart);
        }
        A::SetAwakenedCities(m_level.Dimension(), hearts.data(), hearts.size());
    }

    // ═══ The Podium ══════════════════════════════════════════════════════

    bool AurelithCities::Sockets(const City& city, std::vector<glm::ivec3>& out) const {
        out.clear();
        if (city.rotation < 0) return false;
        for (int i = 0; i < A::kVoiceCount; ++i) {
            out.push_back(A::DesignToWorld(city.heart, city.rotation,
                                           glm::ivec2(A::kSocketXs[i], A::kSocketRow), A::kSocketAboveStreet));
        }
        return true;
    }

    void AurelithCities::OnSocketSeated(const glm::ivec3& socket, Game::IUsePlayer* player) {
        City* city = CityForPoint(socket, A::kQuestReach);
        if (!city) {
            // The Heart's chunk has not come through OnChunkLoaded this
            // session (a blocking load): look for the engine round the socket.
            Game::World* world = m_level.World();
            if (world) {
                for (int dy = -6; dy <= 6 && !city; ++dy) {
                    for (int dx = -A::kQuestReach; dx <= A::kQuestReach && !city; ++dx) {
                        for (int dz = -A::kQuestReach; dz <= A::kQuestReach && !city; ++dz) {
                            const glm::ivec3 p = socket + glm::ivec3(dx, dy, dz);
                            if (world->GetBlock(p.x, p.y, p.z) != Game::BlockID::ResonanceEngine) continue;
                            int rotation = -1;
                            if (auto* engine = dynamic_cast<Game::ResonanceEngineBlockEntity*>(world->GetBlockEntity(p))) {
                                rotation = engine->Rotation();
                            }
                            city = Register(p, rotation);
                        }
                    }
                }
            }
        }
        if (!city) return;
        if (city->state != CityState::Dormant) return;
        JudgeChord(*city, player);
    }

    void AurelithCities::JudgeChord(City& city, Game::IUsePlayer* player) {
        Game::World* world = m_level.World();
        std::vector<glm::ivec3> sockets;
        if (!world || !Sockets(city, sockets)) return;

        struct Seated { Voice voice; int64_t at; glm::ivec3 pos; };
        std::vector<Seated> seated;
        for (const glm::ivec3& p : sockets) {
            auto* socket = dynamic_cast<Game::ChordSocketBlockEntity*>(world->GetBlockEntity(p));
            if (!socket || !socket->HasKey()) continue;
            const std::optional<Voice> v = A::VoiceOfKey(socket->GetKey().itemId);
            if (!v) continue;
            seated.push_back({ *v, socket->SeatedAt(), p });
        }
        if (static_cast<int>(seated.size()) < A::kVoiceCount) {
            if (player) {
                player->DisplayClientMessage("The Podium holds " + std::to_string(seated.size()) +
                                             " of the Four Voices.", true);
            }
            return;
        }
        // The order they were sung in (ties — two in one tick — by socket).
        std::stable_sort(seated.begin(), seated.end(),
                         [](const Seated& a, const Seated& b) { return a.at < b.at; });
        bool right = true;
        for (int i = 0; i < A::kVoiceCount; ++i) {
            if (seated[static_cast<size_t>(i)].voice != A::kChordOrder[i]) { right = false; break; }
        }
        if (!right) {
            Discord(city, player);
            return;
        }
        // The Chord: the keys are held, the city begins to wake.
        for (const Seated& s : seated) {
            if (auto* socket = dynamic_cast<Game::ChordSocketBlockEntity*>(world->GetBlockEntity(s.pos))) {
                socket->SetLocked(true);
            }
        }
        if (player) player->DisplayClientMessage("The Chord rises from the floor to the crown...", true);
        Log::Info("[Aurelith] The Chord is sung at (%d, %d, %d)", city.heart.x, city.heart.y, city.heart.z);
        BeginAwakening(city);
    }

    void AurelithCities::Discord(City& city, Game::IUsePlayer* player) {
        Game::World* world = m_level.World();
        std::vector<glm::ivec3> sockets;
        if (!world || !Sockets(city, sockets)) return;
        const int64_t now = Now();
        // The keys are thrown off the Podium toward the Heart (and whoever
        // stands before it), each in its own direction.
        const glm::ivec3 podium = A::DesignToWorld(city.heart, city.rotation,
                                                   glm::ivec2(0, -A::kPodiumNorth), 2);
        const glm::ivec2 toHeart = A::RotateOffset(glm::ivec2(0, 1), city.rotation);
        int n = 0;
        for (const glm::ivec3& p : sockets) {
            auto* socket = dynamic_cast<Game::ChordSocketBlockEntity*>(world->GetBlockEntity(p));
            if (!socket || !socket->HasKey()) continue;
            const Game::ItemStack key = socket->TakeKey();
            const double spread = (n - 1.5) * 0.08;
            const glm::dvec3 push(toHeart.x * 0.22 + toHeart.y * spread, 0.32,
                                  toHeart.y * 0.22 - toHeart.x * spread);
            Game::SpawnItemEntity(m_level.Dimension(), glm::dvec3(p) + glm::dvec3(0.5, 1.1, 0.5), push, key, 20);
            ++n;
        }
        A::SoundCues::Play(*world, podium, A::Sounds::kDiscord, Game::SoundSource::Blocks, 3.0f, 1.0f);
        Burst(glm::dvec3(podium) + glm::dvec3(0.5, 0.5, 0.5), static_cast<uint8_t>(BurstStyle::Discord), 0x7A3CC8u);
        StartFlicker(city, podium, A::kDiscordFlickerRadius, now);
        if (player) player->DisplayClientMessage("The voices clash. The Chord will not come.", true);
        Log::Info("[Aurelith] A discord at (%d, %d, %d)", city.heart.x, city.heart.y, city.heart.z);
    }

    // ═══ The awakening ═══════════════════════════════════════════════════

    void AurelithCities::BeginAwakening(City& city) {
        city.waveScan.clear();
        city.waveScanned.clear();
        city.waveCells.clear();
        city.waveNext = 0;
        city.lastWaveRadius = -1.0;
        city.lastWaveStep = 0;
        SetState(city, CityState::Awakening);
        QueueWaveScan(city);
    }

    void AurelithCities::TickAwakening(City& city, int64_t now) {
        Game::World* world = m_level.World();
        if (!world) return;
        const int64_t t = std::max<int64_t>(0, now - city.stageStart);
        const int64_t from = city.lastMark;
        city.lastMark = t;
        auto crossed = [&](int64_t mark) { return from < mark && mark <= t; };

        const glm::ivec3& h = city.heart;
        const glm::dvec3 ringCentre(h.x + 0.5, h.y + 12.0, h.z + 0.5);
        std::vector<glm::ivec3> sockets;
        Sockets(city, sockets);

        // The arpeggio, from the floor to the crown: each socket in turn
        // sounds its voice, then all four together.
        for (int i = 0; i < A::kVoiceCount; ++i) {
            if (!crossed(static_cast<int64_t>(i) * A::kArpeggioStep)) continue;
            const Voice v = A::kChordOrder[i];
            for (const glm::ivec3& p : sockets) {
                auto* socket = dynamic_cast<Game::ChordSocketBlockEntity*>(world->GetBlockEntity(p));
                if (!socket || A::VoiceOfKey(socket->GetKey().itemId) != v) continue;
                A::SoundCues::Play(*world, p, A::Sounds::kSocketNote, Game::SoundSource::Blocks, 2.0f,
                                   A::VoicePitch(v));
                Burst(glm::dvec3(p) + glm::dvec3(0.5, 1.0, 0.5), static_cast<uint8_t>(BurstStyle::KeySeat),
                      A::VoiceColour(v));
            }
        }
        if (crossed(A::kChordStruck)) {
            const glm::ivec3 podium = A::DesignToWorld(h, city.rotation, glm::ivec2(0, -A::kPodiumNorth), 2);
            A::SoundCues::Play(*world, podium, A::Sounds::kChord, Game::SoundSource::Blocks, 4.0f, 1.0f);
        }
        if (crossed(A::kSpinUpStart)) {
            A::SoundCues::Play(*world, ringCentre, A::Sounds::kAwakenRise, Game::SoundSource::Ambient, 8.0f, 1.0f);
        }
        if (crossed(A::kWaveStart)) {
            A::SoundCues::Play(*world, ringCentre, A::Sounds::kAwakenSwell, Game::SoundSource::Ambient, 10.0f, 1.0f);
        }
        if (crossed(A::kMotesBurst)) {
            A::SoundCues::Play(*world, ringCentre, A::Sounds::kAwakenBloom, Game::SoundSource::Ambient, 10.0f, 1.0f);
            Burst(ringCentre, static_cast<uint8_t>(BurstStyle::HeartBloom), HeartWhite());
        }
        // The light passing: a chime swell along the wave front, at the four
        // avenues, every two seconds while it runs.
        for (int64_t mark = A::kWaveStart + 20; mark < A::kUndersong; mark += 40) {
            if (!crossed(mark)) continue;
            const double r = A::WaveRadiusAt(static_cast<double>(mark));
            if (r <= 4.0 || r >= A::kWaveReach) continue;
            for (int d = 0; d < 4; ++d) {
                const glm::ivec2 dir = A::RotateOffset(A::BeaconOffset(static_cast<Voice>(d)) / A::kBeaconDistance, 0);
                const glm::dvec3 at(h.x + 0.5 + dir.x * r, h.y - A::kStreetBelowHeart + 3.0, h.z + 0.5 + dir.y * r);
                A::SoundCues::Play(*world, at, A::Sounds::kLightWave, Game::SoundSource::Ambient, 3.0f, 1.0f);
            }
        }
        // The Undersong: the second note, under ours. The plaza stutters.
        if (crossed(A::kUndersong)) {
            A::SoundCues::Play(*world, glm::dvec3(h) + glm::dvec3(0.5, -2.0, 0.5), A::Sounds::kUndersong,
                               Game::SoundSource::Hostile, 10.0f, 1.0f);
            Burst(glm::dvec3(h) + glm::dvec3(0.5, -1.5, 0.5), static_cast<uint8_t>(BurstStyle::Discord), 0x3A1466u);
            StartFlicker(city, A::StreetOf(h), static_cast<int>(A::kPlazaRadius), now);
            if (m_sessions) {
                for (const auto& session : m_sessions->GetAllSessions()) {
                    if (!session || session->GetDimensionId() != static_cast<int>(m_level.Dimension())) continue;
                    ServerPlayer* player = session->GetPlayer();
                    if (player && A::InsideWalls(h, player->getPosition())) {
                        player->DisplayClientMessage("Something below answers.", true);
                    }
                }
            }
        }

        AdvanceWave(city, now);

        if (t >= A::kUnsungRises) RaiseUnsung(city);
    }

    void AurelithCities::QueueWaveScan(City& city) {
        // Every chunk the wave can reach, nearest to the Heart LAST (the
        // queue is consumed from the back): the wave reaches the near ones
        // first, so they are scanned first.
        const int r = static_cast<int>(std::ceil(A::kWaveReach / 16.0)) + 1;
        const Game::Math::ChunkPos c{ city.heart.x >> 4, city.heart.z >> 4 };
        city.waveScan.clear();
        for (int dx = -r; dx <= r; ++dx) {
            for (int dz = -r; dz <= r; ++dz) {
                const Game::Math::ChunkPos p{ c.x + dx, c.z + dz };
                if (city.waveScanned.count(PackChunk(p))) continue;
                city.waveScan.push_back(p);
            }
        }
        const glm::dvec2 hc(city.heart.x + 0.5, city.heart.z + 0.5);
        std::sort(city.waveScan.begin(), city.waveScan.end(),
                  [&](const Game::Math::ChunkPos& a, const Game::Math::ChunkPos& b) {
                      const double da = glm::length(glm::dvec2(a.x * 16 + 8, a.z * 16 + 8) - hc);
                      const double db = glm::length(glm::dvec2(b.x * 16 + 8, b.z * 16 + 8) - hc);
                      return da > db;
                  });
    }

    void AurelithCities::ScanChunkForWave(City& city, const Game::Math::ChunkPos& pos) {
        Game::World* world = m_level.World();
        if (!world) return;
        std::shared_ptr<Game::Chunk> chunk = world->GetLoadedChunk(pos.x, pos.z);
        if (!chunk) return;                          // not loaded: it will be lit on arrival
        city.waveScanned.insert(PackChunk(pos));
        const int yLo = city.heart.y - 40, yHi = city.heart.y + 110;
        for (int si = 0; si < Game::Math::SECTIONS_PER_CHUNK; ++si) {
            const auto& section = chunk->sections[static_cast<size_t>(si)];
            if (!section || section->IsAllAir()) continue;
            const int baseY = Game::World::MIN_Y + si * 16;
            if (baseY + 15 < yLo || baseY > yHi) continue;
            const Game::PalettedContainer& states = section->States();
            bool any = states.IsGlobalPalette();
            if (!any) {
                for (uint32_t raw : states.Palette()) {
                    if (A::IsDimLight(Game::BlockState::FromRawId(raw).Block())) { any = true; break; }
                }
            }
            if (!any) continue;
            for (int y = 0; y < 16; ++y) {
                for (int z = 0; z < 16; ++z) {
                    for (int x = 0; x < 16; ++x) {
                        const Game::BlockState s = Game::BlockState::FromRawId(
                            states.Get(Game::Math::LocalIndex(x, y, z)));
                        if (!A::IsDimLight(s.Block())) continue;
                        const glm::ivec3 w(pos.x * 16 + x, baseY + y, pos.z * 16 + z);
                        const float d = static_cast<float>(Horizontal(city.heart, glm::dvec3(w) + glm::dvec3(0.5)));
                        if (d > A::kWaveReach) continue;
                        city.waveCells.push_back({ w, d });
                        city.waveCellsSorted = false;
                    }
                }
            }
        }
    }

    void AurelithCities::AdvanceWave(City& city, int64_t now) {
        Game::World* world = m_level.World();
        if (!world) return;
        // Scan a few chunks a tick, ahead of the front.
        for (int i = 0; i < kScanChunksPerTick && !city.waveScan.empty(); ++i) {
            const Game::Math::ChunkPos p = city.waveScan.back();
            city.waveScan.pop_back();
            ScanChunkForWave(city, p);
        }
        if (!city.waveCellsSorted) {
            // New cells may be nearer than ones already passed over; the
            // cursor's prefix is already lit, so sort only what remains.
            std::sort(city.waveCells.begin() + static_cast<std::ptrdiff_t>(city.waveNext), city.waveCells.end(),
                      [](const City::Cell& a, const City::Cell& b) { return a.dist < b.dist; });
            city.waveCellsSorted = true;
        }
        // The front moves in steps (each section is rebuilt a handful of
        // times, not every tick), always the radius of this moment.
        if (now - city.lastWaveStep < kWaveStepTicks && city.lastWaveRadius >= 0.0) return;
        const double radius = city.state == CityState::Awakening
            ? A::WaveRadiusAt(static_cast<double>(now - city.stageStart))
            : A::kWaveReach;
        city.lastWaveStep = now;
        city.lastWaveRadius = radius;
        while (city.waveNext < city.waveCells.size() &&
               city.waveCells[city.waveNext].dist <= radius) {
            const glm::ivec3 p = city.waveCells[city.waveNext].pos;
            ++city.waveNext;
            const Game::BlockState s = world->GetBlockState(p.x, p.y, p.z);
            if (!A::IsDimLight(s.Block())) continue;   // broken or already lit
            world->SetBlock(p.x, p.y, p.z, A::ToLit(s), kLightFlags);
        }
        // Done with the cells behind the front: free them now and then.
        if (city.waveNext > 4096 && city.waveNext * 2 > city.waveCells.size()) {
            city.waveCells.erase(city.waveCells.begin(), city.waveCells.begin() + static_cast<std::ptrdiff_t>(city.waveNext));
            city.waveNext = 0;
        }
    }

    void AurelithCities::StartFlicker(City& city, const glm::ivec3& centre, int radius, int64_t now) {
        (void)city;
        Game::World* world = m_level.World();
        if (!world) return;
        auto flicker = std::make_unique<Flicker>();
        flicker->start = now;
        const int yLo = centre.y - 4, yHi = centre.y + 16;
        for (int dx = -radius; dx <= radius; ++dx) {
            for (int dz = -radius; dz <= radius; ++dz) {
                if (dx * dx + dz * dz > radius * radius) continue;
                const int x = centre.x + dx, z = centre.z + dz;
                if (!world->GetLoadedChunk(x >> 4, z >> 4)) continue;
                for (int y = yLo; y <= yHi; ++y) {
                    const Game::BlockState s = world->GetBlockState(x, y, z);
                    if (A::IsDimLight(s.Block()) || A::IsLitLight(s.Block())) {
                        flicker->cells.emplace_back(glm::ivec3(x, y, z), s);
                    }
                }
            }
        }
        if (!flicker->cells.empty()) m_flickers.push_back(std::move(flicker));
    }

    void AurelithCities::TickFlickers(int64_t now) {
        Game::World* world = m_level.World();
        if (!world) { m_flickers.clear(); return; }
        for (auto it = m_flickers.begin(); it != m_flickers.end();) {
            Flicker& f = **it;
            while (f.step < static_cast<size_t>(kFlickerStepCount) &&
                   now - f.start >= kFlickerSteps[f.step]) {
                const bool invert = (f.step % 2) == 0;
                for (const auto& [p, original] : f.cells) {
                    const Game::BlockState cur = world->GetBlockState(p.x, p.y, p.z);
                    // Only cells still one of the pair (a player may have
                    // broken one, the wave may have lit it meanwhile).
                    if (cur.Block() != original.Block() &&
                        cur.Block() != A::LitTwinOf(original.Block()) &&
                        cur.Block() != A::DimTwinOf(original.Block())) continue;
                    Game::BlockState want = original;
                    if (invert) want = A::IsDimLight(original.Block()) ? A::ToLit(original) : A::ToDim(original);
                    if (cur != want) world->SetBlock(p.x, p.y, p.z, want, kLightFlags);
                }
                ++f.step;
            }
            if (f.step >= static_cast<size_t>(kFlickerStepCount)) it = m_flickers.erase(it);
            else ++it;
        }
    }

    // ═══ The fight ═══════════════════════════════════════════════════════

    void AurelithCities::RaiseUnsung(City& city) {
        ServerLevelBridge* bridge = m_level.MobLevel();
        Game::World* world = m_level.World();
        if (!bridge || !world) return;
        std::unique_ptr<Game::Mob> mob = Game::MakeGenericMob(Game::EntityTypeId::TheUnsung, bridge);
        auto* unsung = dynamic_cast<Game::TheUnsung*>(mob.get());
        if (!unsung) {
            Log::Warning("[Aurelith] The Unsung could not be built; the city at (%d, %d, %d) awakes unopposed",
                         city.heart.x, city.heart.y, city.heart.z);
            Resolve(city);
            return;
        }
        unsung->SetArena(city.heart, city.rotation < 0 ? 0 : city.rotation);
        unsung->position = glm::dvec3(city.heart.x + 0.5, city.heart.y - 2.0, city.heart.z + 0.5);
        unsung->yRot = unsung->yBodyRot = unsung->yHeadRot = 0.0f;
        unsung->FinalizeSpawn(Game::SpawnReason::Triggered, nullptr);
        city.bossUuid = unsung->GetUuid();
        const glm::dvec3 at = unsung->position;
        bridge->AddFreshEntity(std::move(mob));
        // AddFreshEntity mints the uuid when the entity had none.
        if (Game::UuidIsNil(city.bossUuid)) {
            if (MobManager* mobs = m_level.Mobs()) {
                for (const Game::Mob* m : mobs->List()) {
                    if (m && m->GetType() == Game::EntityTypeId::TheUnsung && !m->IsRemoved()) {
                        const auto* u = dynamic_cast<const Game::TheUnsung*>(m);
                        if (u && u->HasArena() && u->ArenaHeart() == city.heart) city.bossUuid = m->GetUuid();
                    }
                }
            }
        }
        city.bossLastSeen = Now();
        city.bossWasDying = false;
        A::SoundCues::Play(*world, at, A::Sounds::kUnsungRise, Game::SoundSource::Hostile, 10.0f, 1.0f);
        if (city.state != CityState::Contested) SetState(city, CityState::Contested);
        else { m_dirty = true; WriteFile(); }
        Log::Info("[Aurelith] The Unsung rises at (%d, %d, %d)", city.heart.x, city.heart.y, city.heart.z);
    }

    void AurelithCities::TickContested(City& city, int64_t now) {
        MobManager* mobs = m_level.Mobs();
        Game::World* world = m_level.World();
        if (!mobs || !world) return;
        // The light stays with the city through the fight: finish any wave a
        // restart interrupted.
        AdvanceWave(city, now);

        Game::Mob* boss = Game::UuidIsNil(city.bossUuid) ? nullptr : mobs->FindByUuid(city.bossUuid);
        if (boss && !boss->IsRemoved()) {
            city.bossLastSeen = now;
            if (boss->GetHealth() <= 0.0f) {
                // Sung to rest (its own death plays out; the city resolves
                // as it dissolves).
                Resolve(city);
            }
            return;
        }
        // Lost without dying: raised again once a player stands in the plaza
        // and the Heart's chunk is here.
        if (now - city.bossLastSeen < 200) return;
        if (!world->GetLoadedChunk(city.heart.x >> 4, city.heart.z >> 4)) return;
        if (!m_sessions) return;
        for (const auto& session : m_sessions->GetAllSessions()) {
            if (!session || session->GetDimensionId() != static_cast<int>(m_level.Dimension())) continue;
            ServerPlayer* player = session->GetPlayer();
            if (!player) continue;
            if (Horizontal(city.heart, player->getPosition()) <= A::kPlazaRadius + 6.0) {
                player->DisplayClientMessage("The Undersong rises again.", true);
                RaiseUnsung(city);
                return;
            }
        }
    }

    void AurelithCities::Resolve(City& city) {
        Game::World* world = m_level.World();
        SetState(city, CityState::Awakened);
        if (!world) return;
        const glm::ivec3& h = city.heart;
        const glm::dvec3 ringCentre(h.x + 0.5, h.y + 12.0, h.z + 0.5);
        A::SoundCues::Play(*world, ringCentre, A::Sounds::kResolve, Game::SoundSource::Ambient, 10.0f, 1.0f);
        Burst(ringCentre, static_cast<uint8_t>(BurstStyle::Resolve), HeartWhite());
        // The Held Note: once per city, at the Heart's foot on the dais's
        // north step, where the Podium looks on.
        if (!city.heldNoteGiven) {
            const glm::ivec3 foot = A::DesignToWorld(h, city.rotation, glm::ivec2(0, -5), 4);
            Game::SpawnItemEntity(m_level.Dimension(), glm::dvec3(foot) + glm::dvec3(0.5, 0.4, 0.5),
                                  glm::dvec3(0.0, 0.2, 0.0), Game::ItemStack(Game::Items::HeldNote, 1), 10);
            city.heldNoteGiven = true;
        }
        RevealCoda(city);
        if (m_sessions) {
            for (const auto& session : m_sessions->GetAllSessions()) {
                if (!session || session->GetDimensionId() != static_cast<int>(m_level.Dimension())) continue;
                ServerPlayer* player = session->GetPlayer();
                if (player && Horizontal(h, player->getPosition()) <= kSyncRange) {
                    player->DisplayClientMessage("The Chord is held again. Aurelith is awake.", true);
                }
            }
        }
        m_dirty = true;
        WriteFile();
        Log::Info("[Aurelith] The city at (%d, %d, %d) is awake", h.x, h.y, h.z);
    }

    void AurelithCities::RevealCoda(City& city) {
        Game::World* world = m_level.World();
        if (!world || city.rotation < 0 || city.codaRevealed) return;
        const glm::ivec3& h = city.heart;
        // The Coda, open on the Conductor's seat (design (0, -14), the
        // chair on the Podium's raised half-tier), facing the Heart.
        const glm::ivec3 seat = A::DesignToWorld(h, city.rotation, glm::ivec2(0, -A::kPodiumNorth - 2), 2);
        if (world->GetLoadedChunk(seat.x >> 4, seat.z >> 4)) {
            Game::BlockState lectern = Game::BlockStates::Default(Game::BlockID::Lectern);
            lectern = Game::WithHorizontalFacing(lectern, WorldDirection(2, city.rotation));
            lectern = Game::WithBool(lectern, Game::PropertyId::HAS_BOOK, true);
            world->SetBlock(seat.x, seat.y, seat.z, lectern, Game::World::UpdateFlags::All);
            if (auto* be = dynamic_cast<Game::LecternBlockEntity*>(world->GetBlockEntity(seat))) {
                be->SetBook(EngineWrittenBook("coda"));
            }
            city.codaRevealed = true;
        }
        // The plaques say what happened.
        auto rewrite = [&](glm::ivec2 design, const char* key) {
            const glm::ivec3 p = A::DesignToWorld(h, city.rotation, design, 1);
            auto* sign = dynamic_cast<Game::SignBlockEntity*>(world->GetBlockEntity(p));
            const EngineSign* text = FindEngineSign(key);
            if (!sign || !text) return;
            Game::SignText front = sign->GetText(Game::SignTextSlot::Front);
            for (int i = 0; i < 4; ++i) front.lines[static_cast<size_t>(i)] = text->lines[i];
            sign->SetText(Game::SignTextSlot::Front, front);
            world->BlockEntityChanged(p);
        };
        rewrite(glm::ivec2(0, -A::kPodiumNorth + 2), "podium_plaque_after");   // YSOLDE ... WE CAME BACK.
        rewrite(glm::ivec2(0, 9), "held_note_plaque_after");                   // HERE THE CHORD ... AND IS AGAIN
        m_dirty = true;
    }

    // ═══ Chunks ══════════════════════════════════════════════════════════

    void AurelithCities::OnChunkLoaded(const Game::Math::ChunkPos& pos, Game::Chunk& chunk) {
        // 1. A Heart: register its city.
        for (const auto& [local, be] : chunk.GetAllBlockEntities()) {
            (void)local;
            if (!be || be->GetBlockId() != Game::BlockID::ResonanceEngine) continue;
            const auto* engine = dynamic_cast<const Game::ResonanceEngineBlockEntity*>(be.get());
            Register(be->GetWorldPos(), engine ? engine->Rotation() : -1);
        }
        // 2. A city whose light has passed: this chunk's dim blocks light
        //    now; one still waking: its wave takes the chunk in.
        const glm::dvec2 centre(pos.x * 16 + 8.0, pos.z * 16 + 8.0);
        for (auto& c : m_cities) {
            const double d = glm::length(centre - glm::dvec2(c->heart.x + 0.5, c->heart.z + 0.5));
            if (d > A::kWaveReach + 12.0) continue;
            if (c->state == CityState::Awakened || c->state == CityState::Contested) {
                LightChunk(*c, chunk, pos, true);
            } else if (c->state == CityState::Awakening) {
                c->waveScanned.erase(PackChunk(pos));
                c->waveScan.push_back(pos);
            }
        }
    }

    int AurelithCities::LightChunk(const City& city, Game::Chunk& chunk, const Game::Math::ChunkPos& pos, bool toLit) {
        Game::World* world = m_level.World();
        if (!world) return 0;
        std::vector<glm::ivec3> cells;
        const int yLo = city.heart.y - 40, yHi = city.heart.y + 110;
        for (int si = 0; si < Game::Math::SECTIONS_PER_CHUNK; ++si) {
            const auto& section = chunk.sections[static_cast<size_t>(si)];
            if (!section || section->IsAllAir()) continue;
            const int baseY = Game::World::MIN_Y + si * 16;
            if (baseY + 15 < yLo || baseY > yHi) continue;
            const Game::PalettedContainer& states = section->States();
            auto wanted = [&](Game::BlockID id) { return toLit ? A::IsDimLight(id) : A::IsLitLight(id); };
            bool any = states.IsGlobalPalette();
            if (!any) {
                for (uint32_t raw : states.Palette()) {
                    if (wanted(Game::BlockState::FromRawId(raw).Block())) { any = true; break; }
                }
            }
            if (!any) continue;
            for (int y = 0; y < 16; ++y) {
                for (int z = 0; z < 16; ++z) {
                    for (int x = 0; x < 16; ++x) {
                        const Game::BlockState s = Game::BlockState::FromRawId(
                            states.Get(Game::Math::LocalIndex(x, y, z)));
                        if (!wanted(s.Block())) continue;
                        const glm::ivec3 w(pos.x * 16 + x, baseY + y, pos.z * 16 + z);
                        if (Horizontal(city.heart, glm::dvec3(w) + glm::dvec3(0.5)) > A::kWaveReach) continue;
                        cells.push_back(w);
                    }
                }
            }
        }
        for (const glm::ivec3& p : cells) {
            const Game::BlockState s = world->GetBlockState(p.x, p.y, p.z);
            world->SetBlock(p.x, p.y, p.z, toLit ? A::ToLit(s) : A::ToDim(s), kLightFlags);
        }
        return static_cast<int>(cells.size());
    }

    // ═══ Tick ════════════════════════════════════════════════════════════

    void AurelithCities::Tick() {
        const int64_t now = Now();
        ++m_tickCounter;
        for (auto& c : m_cities) {
            switch (c->state) {
                case CityState::Awakening: TickAwakening(*c, now); break;
                case CityState::Contested: TickContested(*c, now); break;
                default: break;
            }
        }
        TickFlickers(now);
        if (m_tickCounter % kSyncEvery == 0) SyncPlayers(false);
    }

    // ═══ Clients ═════════════════════════════════════════════════════════

    void AurelithCities::SendState(uint32_t connectionId, const City& city) {
        if (!m_sessions) return;
        auto session = m_sessions->GetSessionByConnection(connectionId);
        if (!session || !session->GetConnection()) return;
        Network::AurelithS2CPacket p;
        p.kind = Network::AurelithS2CPacket::Kind::CityState;
        p.dimension = static_cast<int8_t>(Game::DimensionToRaw(m_level.Dimension()));
        p.heart = city.heart;
        p.rotation = static_cast<int8_t>(city.rotation);
        p.state = city.state;
        p.stageStartTick = city.stageStart;
        p.awakenedTick = city.awakenedTick;
        session->GetConnection()->SendPacket(static_cast<uint8_t>(Network::PacketId::AurelithS2C),
                                             Network::Serialization::Serialize(p));
    }

    void AurelithCities::SendForget(uint32_t connectionId, const glm::ivec3& heart) {
        if (!m_sessions) return;
        auto session = m_sessions->GetSessionByConnection(connectionId);
        if (!session || !session->GetConnection()) return;
        Network::AurelithS2CPacket p;
        p.kind = Network::AurelithS2CPacket::Kind::CityForget;
        p.dimension = static_cast<int8_t>(Game::DimensionToRaw(m_level.Dimension()));
        p.heart = heart;
        session->GetConnection()->SendPacket(static_cast<uint8_t>(Network::PacketId::AurelithS2C),
                                             Network::Serialization::Serialize(p));
    }

    void AurelithCities::BroadcastState(const City& city) {
        // Everyone who knows the city hears the change now; the next sync
        // picks up anyone who does not yet.
        for (auto& [connectionId, hearts] : m_known) {
            if (std::find(hearts.begin(), hearts.end(), city.heart) != hearts.end()) {
                SendState(connectionId, city);
            }
        }
        SyncPlayers(false);
    }

    void AurelithCities::SyncPlayers(bool force) {
        if (!m_sessions) return;
        std::unordered_map<uint32_t, std::vector<glm::ivec3>> wanted;
        const int dimension = static_cast<int>(m_level.Dimension());
        for (const auto& session : m_sessions->GetAllSessions()) {
            if (!session || session->GetDimensionId() != dimension) continue;
            ServerPlayer* player = session->GetPlayer();
            if (!player) continue;
            std::vector<glm::ivec3>& list = wanted[session->GetConnectionId()];
            for (const auto& c : m_cities) {
                if (Horizontal(c->heart, player->getPosition()) <= kSyncRange) list.push_back(c->heart);
            }
        }
        // New in range: the whole record.
        for (const auto& [connectionId, hearts] : wanted) {
            std::vector<glm::ivec3>& known = m_known[connectionId];
            for (const glm::ivec3& heart : hearts) {
                const bool had = std::find(known.begin(), known.end(), heart) != known.end();
                if (had && !force) continue;
                if (const City* c = Find(heart)) SendState(connectionId, *c);
                if (!had) known.push_back(heart);
            }
        }
        // Out of range, out of the dimension, or gone: forget.
        for (auto it = m_known.begin(); it != m_known.end();) {
            auto w = wanted.find(it->first);
            std::vector<glm::ivec3>& known = it->second;
            for (auto k = known.begin(); k != known.end();) {
                const bool keep = w != wanted.end() &&
                                  std::find(w->second.begin(), w->second.end(), *k) != w->second.end();
                if (!keep) {
                    SendForget(it->first, *k);
                    k = known.erase(k);
                } else {
                    ++k;
                }
            }
            if (known.empty() && w == wanted.end()) it = m_known.erase(it);
            else ++it;
        }
    }

    void AurelithCities::Burst(const glm::dvec3& origin, uint8_t style, uint32_t colour) {
        if (!m_sessions) return;
        Network::AurelithS2CPacket p;
        p.kind = Network::AurelithS2CPacket::Kind::Burst;
        p.dimension = static_cast<int8_t>(Game::DimensionToRaw(m_level.Dimension()));
        p.heart = glm::ivec3(glm::floor(origin));
        p.origin = origin;
        p.style = static_cast<BurstStyle>(style);
        p.colour = colour;
        const std::vector<uint8_t> data = Network::Serialization::Serialize(p);
        const int dimension = static_cast<int>(m_level.Dimension());
        // The Heart's blooms are seen from across the city.
        const double range = (p.style == BurstStyle::HeartBloom || p.style == BurstStyle::Resolve)
            ? kSyncRange : kBurstRange;
        for (const auto& session : m_sessions->GetAllSessions()) {
            if (!session || session->GetDimensionId() != dimension || !session->GetConnection()) continue;
            ServerPlayer* player = session->GetPlayer();
            if (!player) continue;
            if (glm::length(player->getPosition() - origin) > range) continue;
            session->GetConnection()->SendPacket(static_cast<uint8_t>(Network::PacketId::AurelithS2C), data);
        }
    }

    void AurelithCities::RemoveAll() {
        for (const auto& [connectionId, hearts] : m_known) {
            for (const glm::ivec3& heart : hearts) SendForget(connectionId, heart);
        }
        m_known.clear();
        m_flickers.clear();
        A::SetAwakenedCities(m_level.Dimension(), nullptr, 0);
    }

    // ═══ SavedData ═══════════════════════════════════════════════════════
    // {"": {data: {Cities: [{Heart: int[3], Rotation: byte, State: byte,
    //   StageStart: long, AwakenedTick: long, HeldNote: byte, Coda: byte,
    //   Boss: int[4] (uuid)}]}, DataVersion: int}} — vanilla's SavedData
    // shape (chunks.dat, raids.dat).

    namespace {
        constexpr const char* kCitiesFile = "obeycraft_aurelith.dat";
    }

    void AurelithCities::Save() {
        if (m_dirty) WriteFile();
    }

    void AurelithCities::WriteFile() {
        if (m_dataDir.empty()) { m_dirty = false; return; }
        Game::Nbt::Writer w;
        w.BeginRootCompound();
        w.BeginCompound("data");
        {
            auto list = w.BeginList("Cities", Game::Nbt::TagType::Compound);
            for (const auto& c : m_cities) {
                w.ListCompoundBegin(list);
                const int32_t heart[3] = { c->heart.x, c->heart.y, c->heart.z };
                w.IntArray("Heart", heart, 3);
                w.Byte("Rotation", static_cast<int8_t>(c->rotation));
                w.Byte("State", static_cast<int8_t>(c->state));
                w.Long("StageStart", c->stageStart);
                w.Long("AwakenedTick", c->awakenedTick);
                w.Bool("HeldNote", c->heldNoteGiven);
                w.Bool("Coda", c->codaRevealed);
                if (!Game::UuidIsNil(c->bossUuid)) {
                    int32_t uuid[4];
                    Game::UuidToIntArray(c->bossUuid, uuid);
                    w.IntArray("Boss", uuid, 4);
                }
                w.ListCompoundEnd(list);
            }
            w.EndList(list);
        }
        w.EndCompound();
        w.Int("DataVersion", Game::Save::DataVersion());
        w.EndRootCompound();
        if (!w.ok()) return;
        std::vector<uint8_t> gz;
        if (!Game::Nbt::GzipCompress(w.Bytes(), gz)) return;
        std::error_code ec;
        std::filesystem::create_directories(m_dataDir, ec);
        const std::filesystem::path file = m_dataDir / kCitiesFile;
        const std::filesystem::path tmp = file.string() + ".tmp";
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            if (!f) { Log::Warning("[Aurelith] Could not write %s", tmp.string().c_str()); return; }
            f.write(reinterpret_cast<const char*>(gz.data()), static_cast<std::streamsize>(gz.size()));
            if (!f) return;
        }
        std::filesystem::rename(tmp, file, ec);
        if (ec) {
            Log::Warning("[Aurelith] Could not replace %s: %s", file.string().c_str(), ec.message().c_str());
            return;
        }
        m_dirty = false;
    }

    void AurelithCities::Load() {
        if (m_dataDir.empty()) return;
        const std::filesystem::path file = m_dataDir / kCitiesFile;
        std::ifstream f(file, std::ios::binary);
        if (!f) return;
        const std::vector<uint8_t> raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        std::vector<uint8_t> nbt;
        if (raw.empty() || !Game::Nbt::GzipDecompress(raw, nbt)) {
            Log::Warning("[Aurelith] %s is unreadable; the cities start dormant", file.string().c_str());
            return;
        }
        auto root = std::dynamic_pointer_cast<::World::NBTTagCompound>(::World::NBTParser::Parse(nbt));
        auto data = root ? std::dynamic_pointer_cast<::World::NBTTagCompound>(root->GetTag("data")) : nullptr;
        auto list = data ? std::dynamic_pointer_cast<::World::NBTTagList>(data->GetTag("Cities")) : nullptr;
        if (!list) return;
        const int64_t now = Now();
        for (const auto& element : list->value) {
            auto entry = std::dynamic_pointer_cast<::World::NBTTagCompound>(element);
            if (!entry) continue;
            auto heart = std::dynamic_pointer_cast<::World::NBTTagIntArray>(entry->GetTag("Heart"));
            if (!heart || heart->value.size() != 3) continue;
            auto city = std::make_unique<City>();
            city->heart = glm::ivec3(heart->value[0], heart->value[1], heart->value[2]);
            city->rotation = entry->GetValue<int8_t>("Rotation", -1);
            const int state = entry->GetValue<int8_t>("State", 0);
            city->state = static_cast<CityState>(std::clamp(state, 0, 3));
            city->stageStart = entry->GetValue<int64_t>("StageStart", 0);
            city->awakenedTick = entry->GetValue<int64_t>("AwakenedTick", 0);
            city->heldNoteGiven = entry->GetValue<int8_t>("HeldNote", 0) != 0;
            city->codaRevealed = entry->GetValue<int8_t>("Coda", 0) != 0;
            if (auto boss = std::dynamic_pointer_cast<::World::NBTTagIntArray>(entry->GetTag("Boss"));
                boss && boss->value.size() == 4) {
                const int32_t uuid[4] = { boss->value[0], boss->value[1], boss->value[2], boss->value[3] };
                city->bossUuid = Game::UuidFromIntArray(uuid);
            }
            // No mark replays after a restart: the timeline resumes from now.
            city->lastMark = std::max<int64_t>(-1, now - city->stageStart);
            city->bossLastSeen = now;
            if (city->state == CityState::Awakening || city->state == CityState::Contested) {
                QueueWaveScan(*city);
            }
            m_cities.push_back(std::move(city));
        }
        Log::Info("[Aurelith] %zu cit%s from %s", m_cities.size(), m_cities.size() == 1 ? "y" : "ies",
                  file.string().c_str());
    }

    // ═══ /aurelith ═══════════════════════════════════════════════════════

    bool AurelithCities::Nearest(const glm::dvec3& pos, double maxDistance, Summary& out) const {
        const City* best = nullptr;
        double bestD = maxDistance;
        for (const auto& c : m_cities) {
            const double d = Horizontal(c->heart, pos);
            if (d <= bestD) { bestD = d; best = c.get(); }
        }
        if (!best) return false;
        out.heart = best->heart;
        out.rotation = best->rotation;
        out.state = best->state;
        out.stageTicks = Now() - best->stageStart;
        out.heldNoteGiven = best->heldNoteGiven;
        out.distance = bestD;
        return true;
    }

    bool AurelithCities::DesignPoint(const glm::ivec3& heart, glm::ivec2 design, int aboveStreet,
                                     glm::ivec3& out) const {
        const City* c = Find(heart);
        if (!c || c->rotation < 0) return false;
        out = A::DesignToWorld(heart, c->rotation, design, aboveStreet);
        return true;
    }

    std::string AurelithCities::DebugSingTheChord(const glm::ivec3& heart) {
        City* city = Find(heart);
        Game::World* world = m_level.World();
        std::vector<glm::ivec3> sockets;
        if (!city || !world) return "No city there.";
        if (city->state != CityState::Dormant) return "That city is not dormant (use /aurelith reset).";
        if (!Sockets(*city, sockets)) return "That city's rotation is unknown (generated before the quest).";
        // One key per socket, in the Chord's order, a tick apart.
        const int64_t now = Now();
        for (int i = 0; i < A::kVoiceCount; ++i) {
            auto* socket = dynamic_cast<Game::ChordSocketBlockEntity*>(
                world->GetBlockEntity(sockets[static_cast<size_t>(i)]));
            if (!socket) return "The Podium's sockets are not loaded or not there.";
            if (socket->HasKey()) (void)socket->TakeKey();
            socket->Seat(Game::ItemStack(A::KeyOf(A::kChordOrder[i]), 1), now - A::kVoiceCount + i);
        }
        JudgeChord(*city, nullptr);
        return "The Four Voices are sung from the floor to the crown.";
    }

    std::string AurelithCities::DebugAdvance(const glm::ivec3& heart) {
        City* city = Find(heart);
        if (!city) return "No city there.";
        switch (city->state) {
            case CityState::Dormant:
                return "Dormant: sing the Chord first (/aurelith sing).";
            case CityState::Awakening:
                city->stageStart = Now() - A::kUnsungRises;
                return "The awakening runs ahead: the Unsung rises.";
            case CityState::Contested: {
                if (MobManager* mobs = m_level.Mobs()) {
                    if (Game::Mob* boss = Game::UuidIsNil(city->bossUuid) ? nullptr : mobs->FindByUuid(city->bossUuid)) {
                        boss->Hurt(Game::MobDamageSource::Generic, boss->GetHealth() + 1000.0f, nullptr);
                    }
                }
                Resolve(*city);
                return "The Unsung is sung to rest.";
            }
            case CityState::Awakened:
                return "That city is already awake.";
        }
        return "";
    }

    std::string AurelithCities::DebugReset(const glm::ivec3& heart, Game::IUsePlayer* keysTo) {
        City* city = Find(heart);
        Game::World* world = m_level.World();
        if (!city || !world) return "No city there.";
        // The boss.
        if (MobManager* mobs = m_level.Mobs()) {
            if (Game::Mob* boss = Game::UuidIsNil(city->bossUuid) ? nullptr : mobs->FindByUuid(city->bossUuid)) {
                boss->Discard();
            }
        }
        // The sockets: unlocked, emptied.
        std::vector<glm::ivec3> sockets;
        if (Sockets(*city, sockets)) {
            for (const glm::ivec3& p : sockets) {
                auto* socket = dynamic_cast<Game::ChordSocketBlockEntity*>(world->GetBlockEntity(p));
                if (!socket) continue;
                socket->SetLocked(false);
                if (socket->HasKey()) {
                    const Game::ItemStack key = socket->TakeKey();
                    if (keysTo) {
                        Game::SpawnItemEntity(m_level.Dimension(), keysTo->getPosition(), glm::dvec3(0.0), key, 0);
                    }
                }
            }
        }
        // The lights of every loaded chunk back to dim.
        const int r = static_cast<int>(std::ceil(A::kWaveReach / 16.0)) + 1;
        int dimmed = 0;
        for (int dx = -r; dx <= r; ++dx) {
            for (int dz = -r; dz <= r; ++dz) {
                const Game::Math::ChunkPos p{ (heart.x >> 4) + dx, (heart.z >> 4) + dz };
                if (auto chunk = world->GetLoadedChunk(p.x, p.z)) dimmed += LightChunk(*city, *chunk, p, false);
            }
        }
        city->bossUuid = Game::Uuid{};
        city->waveCells.clear();
        city->waveNext = 0;
        city->waveScan.clear();
        city->waveScanned.clear();
        SetState(*city, CityState::Dormant);
        return "The city is dormant again (" + std::to_string(dimmed) + " lights dimmed; the Held Note, the "
               "Coda and the plaques are kept).";
    }

} // namespace Server
