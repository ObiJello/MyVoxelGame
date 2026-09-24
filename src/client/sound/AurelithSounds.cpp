// File: src/client/sound/AurelithSounds.cpp
//
// See AurelithSounds.hpp. Layout:
//   the loop            a positional / relative looping tickable instance that
//                       eases toward a target volume and pitch
//   finding the sources the once-a-second chunk walk for engines and beacons
//   the Heart / beacons per-tick targets from AurelithState
//   the wind, the choir
//   the music
#include "client/sound/AurelithSounds.hpp"

#include "client/renderer/environment/EnvironmentState.hpp"
#include "client/sound/AmbientSoundHandlers.hpp"
#include "client/sound/SoundInstance.hpp"
#include "client/sound/SoundManager.hpp"
#include "client/world/AurelithState.hpp"
#include "client/world/ClientChunkManager.hpp"
#include "common/core/JavaRandom.hpp"
#include "common/core/Profiling_Tracy.hpp"
#include "common/world/block/BlockState.hpp"
#include "common/world/block/entity/BlockEntity.hpp"
#include "common/world/block/entity/BlockEntityTypes.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/level/AurelithQuest.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Client::AurelithSounds {

    namespace {

        namespace A = Game::Aurelith;
        using State = Client::AurelithState::City;

        // ── Events (assets/sound_overlays/obeycraft/aurelith.json) ─────────
        constexpr const char* kHum        = "obeycraft:block.resonance_engine.hum";
        constexpr const char* kHumVoice   = "obeycraft:block.resonance_engine.hum_voice";
        constexpr const char* kHumSour    = "obeycraft:block.resonance_engine.hum_sour";
        constexpr const char* kThrum      = "obeycraft:block.voice_beacon.thrum";
        constexpr const char* kWind       = "obeycraft:ambient.aurelith.wind";
        constexpr const char* kWindGust   = "obeycraft:ambient.aurelith.wind_gust";
        constexpr const char* kChoirFlute = "obeycraft:ambient.aurelith.choir.flute";
        constexpr const char* kChoirChime = "obeycraft:ambient.aurelith.choir.chime";
        constexpr const char* kChoirBell  = "obeycraft:ambient.aurelith.choir.bell";
        constexpr const char* kChoirGlass = "obeycraft:ambient.aurelith.choir.glass";
        constexpr const char* kMusicDormant  = "obeycraft:music.aurelith.dormant";
        constexpr const char* kMusicUnsung   = "obeycraft:music.aurelith.unsung";
        constexpr const char* kMusicAwakened = "obeycraft:music.aurelith.awakened";
        constexpr std::string_view kCityMusicPrefix = "obeycraft:music.aurelith.";

        // ── Budgets ─────────────────────────────────────────────────────────
        constexpr int    kScanPeriod    = 20;     // ticks between chunk walks
        constexpr double kHeartReach    = 64.0;   // blocks (horizontal) to keep a hum
        constexpr double kBeaconReach   = 80.0;   // the lens is ~47 up: reach it from the gate
        constexpr size_t kMaxHearts     = 2;
        constexpr size_t kMaxBeacons    = 3;
        // The wind: from 52 to 72 blocks over the street (the Spire's view
        // terrace is at street + 70, the Archive's crown at + 63).
        constexpr double kWindFrom      = 52.0;
        constexpr double kWindFull      = 72.0;
        // The choir: down in the streets only (not the towers' tops).
        constexpr double kChoirCeiling  = 40.0;

        // The F# chord's tones as note-block multipliers (A::VoicePitch's).
        constexpr float F3 = 0.5f, Cs4 = 0.749f, F4 = 1.0f, As4 = 1.26f, Cs5 = 1.498f, F5 = 2.0f;

        Game::JavaRandom g_random(static_cast<int64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count()) ^ 0x415552454C495448ll);
        int g_generation = 0;
        int g_scanIn = 0;
        int g_gustIn = 120;
        int g_choirIn = 400;

        double Smooth(double e0, double e1, double x) {
            const double t = std::clamp((x - e0) / (e1 - e0), 0.0, 1.0);
            return t * t * (3.0 - 2.0 * t);
        }

        // ── The loop ────────────────────────────────────────────────────────
        // MC's TickableSoundInstance shape (BeeSoundInstance, the elytra's
        // loop): a looping sound whose Tick eases its volume and pitch
        // toward targets set from outside, and which stops itself once
        // released and faded, or when the world it belonged to is gone.
        // The reach is the overlay entry's attenuation_distance (the volume
        // stays <= 1 so MC's max(1, volume) never widens it); a relative loop
        // (the wind) is unattenuated at the listener, as MC's local ambience.
        class CityLoop final : public AbstractTickableSoundInstance {
        public:
            CityLoop(const char* event, const glm::dvec3& pos, bool relative, float pitch)
                : AbstractTickableSoundInstance(event, Game::SoundSource::Ambient, SoundInstance::UnseededSeed()),
                  m_generation(g_generation) {
                m_looping = true;
                m_delay = 0;
                m_volume = 0.0f;
                m_pitch = pitch;
                m_targetPitch = pitch;
                m_relative = relative;
                m_attenuation = relative ? Attenuation::None : Attenuation::Linear;
                if (!relative) { m_x = pos.x; m_y = pos.y; m_z = pos.z; }
            }

            void SetTarget(float volume, float pitch) {
                m_target = std::clamp(volume, 0.0f, 1.0f);
                m_targetPitch = pitch;
            }
            void Release() { m_released = true; }
            bool Released() const { return m_released; }

            void Tick() override {
                if (m_generation != g_generation) { Stop(); return; }
                const float target = m_released ? 0.0f : m_target;
                // ~0.6 s to settle; a released loop fades a touch faster.
                m_volume += (target - m_volume) * (m_released ? 0.12f : 0.08f);
                m_pitch  += (m_targetPitch - m_pitch) * 0.1f;
                if (m_released && m_volume < 0.004f) Stop();
            }
            // The loop starts at zero and swells in; MC's play() would refuse
            // a silent start otherwise.
            bool CanStartSilent() const override { return true; }

        private:
            float m_target = 0.0f;
            float m_targetPitch = 1.0f;
            bool  m_released = false;
            int   m_generation;
        };

        std::shared_ptr<CityLoop> StartLoop(const char* event, const glm::dvec3& pos, bool relative,
                                            float pitch) {
            auto loop = std::make_shared<CityLoop>(event, pos, relative, pitch);
            GetSoundManager().Play(loop);
            return loop;
        }

        // A loop that is still ours to steer: not stopped, not fading out
        // after a Release (a swell that comes back starts a fresh one), and
        // actually on a channel (a full pool refuses a start; it is retried).
        bool Alive(const std::shared_ptr<CityLoop>& loop) {
            return loop && !loop->IsStopped() && !loop->Released() && GetSoundManager().IsActive(loop);
        }

        // ── Finding the sources ─────────────────────────────────────────────
        struct Found {
            glm::ivec3 pos;
            double     dist2;
        };

        uint64_t Key(const glm::ivec3& p) {
            return (static_cast<uint64_t>(static_cast<uint32_t>(p.x)) << 38)
                 ^ (static_cast<uint64_t>(static_cast<uint32_t>(p.z)) << 12)
                 ^ static_cast<uint64_t>(static_cast<uint32_t>(p.y) & 0xFFFu);
        }

        // Once a second: the nearest resonance engines and voice beacons in
        // the loaded chunks round the listener (the client's per-chunk
        // block-entity maps; a few dozen map lookups, no block reads).
        void Scan(const glm::dvec3& at, std::vector<Found>& hearts, std::vector<Found>& beacons) {
            hearts.clear();
            beacons.clear();
            const ClientChunkManager* chunks = g_clientChunkManager;
            if (!chunks) return;
            const double reach = std::max(kHeartReach, kBeaconReach);
            const int minCx = static_cast<int>(std::floor((at.x - reach) / 16.0));
            const int maxCx = static_cast<int>(std::floor((at.x + reach) / 16.0));
            const int minCz = static_cast<int>(std::floor((at.z - reach) / 16.0));
            const int maxCz = static_cast<int>(std::floor((at.z + reach) / 16.0));
            for (int cz = minCz; cz <= maxCz; ++cz) {
                for (int cx = minCx; cx <= maxCx; ++cx) {
                    const ClientChunk* chunk = chunks->GetChunk(Game::Math::ChunkPos{cx, cz});
                    if (!chunk || !chunk->IsLoaded() || !chunk->chunkData) continue;
                    const auto& entities = chunk->chunkData->GetAllBlockEntities();
                    if (entities.empty()) continue;
                    for (const auto& [local, be] : entities) {
                        (void)local;
                        if (!be || !be->GetType()) continue;
                        const uint16_t type = be->GetType()->TypeId();
                        if (type != Game::BlockEntityTypeIds::RESONANCE_ENGINE &&
                            type != Game::BlockEntityTypeIds::VOICE_BEACON) continue;
                        const glm::ivec3 p = be->GetWorldPos();
                        const double dx = p.x + 0.5 - at.x, dz = p.z + 0.5 - at.z;
                        const double d2 = dx * dx + dz * dz;
                        if (type == Game::BlockEntityTypeIds::RESONANCE_ENGINE) {
                            if (d2 <= kHeartReach * kHeartReach) hearts.push_back({p, d2});
                        } else if (d2 <= kBeaconReach * kBeaconReach) {
                            beacons.push_back({p, d2});
                        }
                    }
                }
            }
            auto nearest = [](std::vector<Found>& v, size_t keep) {
                std::sort(v.begin(), v.end(), [](const Found& a, const Found& b) { return a.dist2 < b.dist2; });
                if (v.size() > keep) v.resize(keep);
            };
            nearest(hearts, kMaxHearts);
            nearest(beacons, kMaxBeacons);
        }

        // ── The Heart ───────────────────────────────────────────────────────
        struct HeartVoices {
            glm::ivec3 pos{0};
            std::shared_ptr<CityLoop> hum, voice, sour;
        };
        std::unordered_map<uint64_t, HeartVoices> g_hearts;

        struct BeaconVoice {
            glm::ivec3 pos{0};
            float      pitch = 1.0f;
            std::shared_ptr<CityLoop> thrum;
        };
        std::unordered_map<uint64_t, BeaconVoice> g_beacons;

        void ReleaseHeart(HeartVoices& h) {
            for (auto* l : {&h.hum, &h.voice, &h.sour}) {
                if (*l) (*l)->Release();
            }
        }

        // The city a Heart belongs to, or a dormant stand-in for one the
        // server has not described (a legacy city, or before the first
        // AurelithS2C arrives): the hum then simply drones.
        State CityFor(Game::DimensionId dim, const glm::ivec3& heart) {
            if (auto c = AurelithState::CityAt(dim, heart)) return *c;
            State s;
            s.dimension = dim;
            s.heart = heart;
            return s;
        }

        void TickHeart(HeartVoices& h, const State& city, double ticks, float stillGain) {
            const double voice = AurelithState::Voice(city, ticks);
            const double sour  = AurelithState::Sourness(city, ticks);
            const double light = AurelithState::LightLevel(city, ticks);
            const double pace  = std::max(0.25, AurelithState::RingPace(city, ticks));
            const double climb = std::log2(pace);                  // 0 dormant .. ~2.6 at full spin
            const double sec   = ticks / 20.0;
            const glm::dvec3 at = glm::dvec3(h.pos) + glm::dvec3(0.5, 6.0, 0.5);   // up among the rings
            const float hush = 0.4f + 0.6f * stillGain;

            // The drone: always there, fuller once lit, rising a little with
            // the rings; a slow wobble while the Undersong pulls at it.
            const float wobble = static_cast<float>(1.0 + 0.035 * sour * std::sin(sec * 2.3)
                                                        * std::sin(sec * 0.61 + 1.7));
            if (!Alive(h.hum)) h.hum = StartLoop(kHum, at, false, 1.0f);
            h.hum->SetTarget(static_cast<float>((0.6 + 0.35 * light) * hush),
                             static_cast<float>(std::clamp(1.0 + 0.09 * climb, 0.8, 1.35)) * wobble);

            // The sung layer: the Chord held again.
            if (voice > 0.01) {
                if (!Alive(h.voice)) h.voice = StartLoop(kHumVoice, at, false, 1.0f);
                h.voice->SetTarget(static_cast<float>(0.85 * voice * hush),
                                   static_cast<float>(std::clamp(0.95 + 0.12 * climb, 0.8, 1.6)));
            } else if (h.voice) {
                h.voice->Release();
            }

            // The second note, under ours.
            if (sour > 0.01) {
                if (!Alive(h.sour)) h.sour = StartLoop(kHumSour, at, false, 1.0f);
                h.sour->SetTarget(static_cast<float>(0.75 * sour * hush),
                                  static_cast<float>(1.0 + 0.06 * std::sin(sec * 0.9)));
            } else if (h.sour) {
                h.sour->Release();
            }
        }

        // A beacon's voice: the city's (rotation-true) when known, else the
        // block's facing as VoiceBeaconRenderer reads it.
        float ThrumPitch(Game::DimensionId dim, const glm::ivec3& pos, const Game::IBlockAccess* blocks) {
            int voice = -1;
            if (auto c = AurelithState::Nearest(dim, glm::dvec3(pos) + glm::dvec3(0.5),
                                                A::kBeaconDistance + 40.0)) {
                voice = AurelithState::BeaconVoice(*c, pos);
            }
            if (voice < 0 && blocks) {
                const std::string_view facing = blocks->GetBlockState(pos.x, pos.y, pos.z).GetValueByName("facing");
                voice = facing == "east" ? 1 : facing == "south" ? 2 : facing == "west" ? 3 : 0;
            }
            // A thrum a fifth apart per voice, high to low: Soprano, Alto,
            // Tenor, Bass (the beacon's drone at 1.0 is the Alto).
            switch (static_cast<A::Voice>(voice)) {
                case A::Voice::Soprano: return 1.26f;
                case A::Voice::Alto:    return 1.0f;
                case A::Voice::Tenor:   return 0.84f;
                case A::Voice::Bass:    return 0.67f;
            }
            return 1.0f;
        }

        void TickBeacon(BeaconVoice& b, Game::DimensionId dim, double ticks, float stillGain) {
            float light = 0.0f, bend = 0.0f;
            if (auto c = AurelithState::Nearest(dim, glm::dvec3(b.pos) + glm::dvec3(0.5),
                                                A::kBeaconDistance + 40.0)) {
                light = static_cast<float>(AurelithState::LightLevel(*c, ticks));
                bend  = static_cast<float>(AurelithState::BeamConvergence(*c, ticks));
                if (c->state != A::CityState::Awakening) bend = 0.0f;   // the swell is the bend itself
            }
            const glm::dvec3 lens = glm::dvec3(b.pos) + glm::dvec3(0.5, 1.2, 0.5);
            if (!Alive(b.thrum)) b.thrum = StartLoop(kThrum, lens, false, b.pitch);
            b.thrum->SetTarget((0.45f + 0.4f * light + 0.3f * bend) * (0.5f + 0.5f * stillGain),
                               b.pitch * (1.0f + 0.06f * bend));
        }

        // ── The wind ────────────────────────────────────────────────────────
        std::shared_ptr<CityLoop> g_wind;

        void TickWind(const SoundHost::TickContext& ctx, const std::optional<State>& city, float stillGain) {
            double height = 0.0;
            if (city) {
                const double street = city->heart.y - A::kStreetBelowHeart;
                height = Smooth(street + kWindFrom, street + kWindFull, ctx.cameraPosition.y);
            }
            if (height > 0.01) {
                if (!Alive(g_wind)) g_wind = StartLoop(kWind, glm::dvec3(0.0), true, 1.0f);
                g_wind->SetTarget(static_cast<float>(0.85 * height * stillGain),
                                  static_cast<float>(0.9 + 0.15 * height));
            } else if (g_wind) {
                g_wind->Release();
                if (g_wind->IsStopped()) g_wind.reset();
            }

            // Gusts: round the listener, now and then, while it is windy.
            if (height > 0.25 && stillGain > 0.99f && --g_gustIn <= 0) {
                g_gustIn = 100 + g_random.NextInt(160);
                const double a = g_random.NextDouble() * 6.283185307179586;
                const glm::dvec3 at = ctx.cameraPosition + glm::dvec3(std::cos(a) * 6.0, 1.0, std::sin(a) * 6.0);
                GetSoundManager().Play(std::make_shared<SimpleSoundInstance>(
                    kWindGust, Game::SoundSource::Ambient, static_cast<float>(0.55 + 0.45 * height),
                    0.9f + g_random.NextFloat() * 0.2f, SoundInstance::UnseededSeed(), at.x, at.y, at.z));
            }
        }

        // ── The empty choir ─────────────────────────────────────────────────
        // A motif: up to four notes (chord tones), each with its delay after
        // the first. Dormant ones stop short of home (on the fifth or the
        // third); awakened ones come home to F#.
        struct Motif {
            float pitches[4];
            int   delays[4];
            int   count;
        };
        constexpr Motif kDormantMotifs[] = {
            {{F4, Cs5, 0, 0},        {0, 14, 0, 0},       2},
            {{As4, Cs5, F4, 0},      {0, 10, 26, 0},      3},
            {{F5, Cs5, As4, Cs5},    {0, 9, 18, 34},      4},
            {{Cs4, F4, As4, 0},      {0, 16, 32, 0},      3},
            {{Cs5, As4, 0, 0},       {0, 20, 0, 0},       2},
            {{F3 * 2.0f, Cs5, 0, 0}, {0, 12, 0, 0},       2},
        };
        constexpr Motif kAwakenedMotifs[] = {
            {{Cs4, F4, As4, F5},     {0, 8, 16, 28},      4},
            {{As4, Cs5, F5, 0},      {0, 8, 18, 0},       3},
            {{F5, Cs5, As4, F4},     {0, 7, 14, 26},      4},
            {{F4, As4, F4, 0},       {0, 10, 22, 0},      3},
        };

        const char* PickInstrument(bool awakened) {
            const int r = g_random.NextInt(awakened ? 8 : 6);
            if (awakened) return r < 3 ? kChoirFlute : r < 5 ? kChoirChime : r < 7 ? kChoirBell : kChoirGlass;
            return r < 3 ? kChoirFlute : r < 5 ? kChoirGlass : kChoirBell;
        }

        void SingFragment(const SoundHost::TickContext& ctx, const State& city, bool awakened) {
            const Game::IBlockAccess* blocks = ctx.blocks;
            const double street = city.heart.y - A::kStreetBelowHeart;
            // Somewhere open, 6..14 blocks off, a little above the paving.
            glm::dvec3 at(0.0);
            bool found = false;
            for (int attempt = 0; attempt < 6 && !found; ++attempt) {
                const double a = g_random.NextDouble() * 6.283185307179586;
                const double r = 6.0 + g_random.NextDouble() * 8.0;
                at = glm::dvec3(ctx.cameraPosition.x + std::cos(a) * r,
                                std::max(street + 1.5, ctx.cameraPosition.y - 2.0) + g_random.NextDouble() * 4.0,
                                ctx.cameraPosition.z + std::sin(a) * r);
                if (!A::InsideWalls(city.heart, at)) continue;
                found = !blocks || !blocks->IsBlockSolid(static_cast<int>(std::floor(at.x)),
                                                         static_cast<int>(std::floor(at.y)),
                                                         static_cast<int>(std::floor(at.z)));
            }
            if (!found) return;

            const Motif* motifs = awakened ? kAwakenedMotifs : kDormantMotifs;
            const int motifCount = awakened ? static_cast<int>(std::size(kAwakenedMotifs))
                                            : static_cast<int>(std::size(kDormantMotifs));
            const Motif& m = motifs[g_random.NextInt(motifCount)];
            const char* lead = PickInstrument(awakened);
            // Awakened, a second voice doubles the line an octave up (clamped
            // to the note block's range) on glass.
            const bool doubled = awakened && g_random.NextInt(2) == 0;
            // The voice drifts as it sings: each note a step along a heading.
            const double heading = g_random.NextDouble() * 6.283185307179586;
            const glm::dvec3 drift(std::cos(heading) * 0.9, 0.15, std::sin(heading) * 0.9);
            const float volume = (awakened ? 1.3f : 1.1f) * AmbientSounds::StillnessGain();
            for (int i = 0; i < m.count; ++i) {
                const glm::dvec3 p = at + drift * static_cast<double>(i);
                auto note = std::make_shared<SimpleSoundInstance>(
                    lead, Game::SoundSource::Ambient, volume, m.pitches[i], SoundInstance::UnseededSeed(),
                    p.x, p.y, p.z);
                GetSoundManager().PlayDelayed(note, m.delays[i]);
                if (doubled) {
                    auto octave = std::make_shared<SimpleSoundInstance>(
                        kChoirGlass, Game::SoundSource::Ambient, volume * 0.8f,
                        std::min(2.0f, m.pitches[i] * 2.0f), SoundInstance::UnseededSeed(), p.x, p.y, p.z);
                    GetSoundManager().PlayDelayed(octave, m.delays[i] + 1);
                }
            }
        }

        void TickChoir(const SoundHost::TickContext& ctx, const std::optional<State>& city, double ticks) {
            if (!city || ctx.underwater) return;
            const double street = city->heart.y - A::kStreetBelowHeart;
            if (ctx.cameraPosition.y > street + kChoirCeiling || ctx.cameraPosition.y < street - 12.0) return;
            // Not over the Chord itself, nor through the fight.
            if (city->state == A::CityState::Awakening || city->state == A::CityState::Contested) return;
            if (AmbientSounds::StillnessGain() < 1.0f) return;
            if (--g_choirIn > 0) return;
            const bool awakened = city->state == A::CityState::Awakened &&
                                  AurelithState::Resolution(*city, ticks) <= 0.0;
            // Sparse in the empty city (15-40 s), a little livelier once it
            // sings again (10-25 s).
            g_choirIn = awakened ? 200 + g_random.NextInt(300) : 300 + g_random.NextInt(500);
            SingFragment(ctx, *city, awakened);
        }

        double GameTicks() {
            return ::Render::EnvironmentState::Get().GameTimeF(0.0f);
        }

    } // namespace

    void Tick(const SoundHost::TickContext& ctx) {
        PROFILE_ZONE_N("Sound.Aurelith");
        if (!ctx.player || ctx.levelLoading) return;
        // No sound files at all (never extracted, no Minecraft install): the
        // engine answers every play with NotStarted; do not keep asking.
        if (!GetSoundManager().HasSounds()) return;
        const Game::DimensionId dim = ctx.dimension;
        const double ticks = GameTicks();
        const float stillGain = AmbientSounds::StillnessGain();

        // Once a second: which engines and beacons are near enough to sound.
        if (--g_scanIn <= 0) {
            g_scanIn = kScanPeriod;
            std::vector<Found> hearts, beacons;
            if (dim == Game::DimensionId::Hush) Scan(ctx.cameraPosition, hearts, beacons);

            std::unordered_map<uint64_t, bool> keepHearts, keepBeacons;
            for (const Found& f : hearts) {
                const uint64_t k = Key(f.pos);
                keepHearts[k] = true;
                HeartVoices& h = g_hearts[k];
                h.pos = f.pos;
            }
            for (const Found& f : beacons) {
                const uint64_t k = Key(f.pos);
                keepBeacons[k] = true;
                BeaconVoice& b = g_beacons[k];
                if (!b.thrum) b.pitch = ThrumPitch(dim, f.pos, ctx.blocks);
                b.pos = f.pos;
            }
            for (auto it = g_hearts.begin(); it != g_hearts.end();) {
                if (!keepHearts.count(it->first)) {
                    ReleaseHeart(it->second);
                    it = g_hearts.erase(it);
                } else {
                    ++it;
                }
            }
            for (auto it = g_beacons.begin(); it != g_beacons.end();) {
                if (!keepBeacons.count(it->first)) {
                    if (it->second.thrum) it->second.thrum->Release();
                    it = g_beacons.erase(it);
                } else {
                    ++it;
                }
            }
        }

        for (auto& [key, h] : g_hearts) {
            (void)key;
            TickHeart(h, CityFor(dim, h.pos), ticks, stillGain);
        }
        for (auto& [key, b] : g_beacons) {
            (void)key;
            TickBeacon(b, dim, ticks, stillGain);
        }

        const std::optional<State> city = AurelithState::InsideWalls(dim, ctx.cameraPosition);
        TickWind(ctx, city, stillGain);
        TickChoir(ctx, city, ticks);
    }

    void Reset() {
        ++g_generation;   // every live loop stops itself on its next tick
        for (auto& [k, h] : g_hearts) { (void)k; for (auto* l : {&h.hum, &h.voice, &h.sour}) if (*l) GetSoundManager().Stop(*l); }
        for (auto& [k, b] : g_beacons) { (void)k; if (b.thrum) GetSoundManager().Stop(b.thrum); }
        if (g_wind) GetSoundManager().Stop(g_wind);
        g_hearts.clear();
        g_beacons.clear();
        g_wind.reset();
        g_scanIn = 0;
        g_gustIn = 120;
        g_choirIn = 400;
    }

    // ── The music ────────────────────────────────────────────────────────

    CityMusicChoice CityMusic(const SoundHost::TickContext& ctx) {
        CityMusicChoice out;
        if (!ctx.inWorld || !ctx.player) return out;
        const std::optional<State> city = AurelithState::InsideWalls(ctx.dimension, ctx.cameraPosition);
        if (!city) return out;
        out.inCity = true;
        const double ticks = GameTicks();
        switch (city->state) {
            case A::CityState::Dormant:
                // Replaces nothing: the song playing when you pass the gate
                // plays out; the city's own follows within four minutes.
                out.music = AudioAttributes::Music{kMusicDormant, 1200, 4800, false};
                break;
            case A::CityState::Awakening:
                // Silence: the Chord is the music (MusicGain fades what plays).
                break;
            case A::CityState::Contested:
                out.music = AudioAttributes::Music{kMusicUnsung, 0, 60, false};
                break;
            case A::CityState::Awakened:
                // Soon after the resolution, then the city's own pace.
                out.music = AurelithState::Resolution(*city, ticks) > 0.0
                    ? AudioAttributes::Music{kMusicAwakened, 100, 300, false}
                    : AudioAttributes::Music{kMusicAwakened, 600, 3600, false};
                break;
        }
        return out;
    }

    float MusicGain(const SoundHost::TickContext& ctx, const std::string& currentEvent) {
        if (currentEvent.empty() || !ctx.inWorld || !ctx.player) return 1.0f;
        const std::optional<State> city = AurelithState::InsideWalls(ctx.dimension, ctx.cameraPosition);
        if (!city) {
            // The fight's music never follows you out of the walls.
            return currentEvent == kMusicUnsung ? 0.0f : 1.0f;
        }
        switch (city->state) {
            case A::CityState::Dormant:   return currentEvent == kMusicUnsung ? 0.0f : 1.0f;
            case A::CityState::Awakening: return 0.0f;
            case A::CityState::Contested: return currentEvent == kMusicUnsung ? 1.0f : 0.0f;
            case A::CityState::Awakened:  return currentEvent == kMusicAwakened ? 1.0f : 0.0f;
        }
        (void)kCityMusicPrefix;
        return 1.0f;
    }

} // namespace Client::AurelithSounds
