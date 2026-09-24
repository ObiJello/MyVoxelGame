// File: src/client/sound/AmbientSoundHandlers.cpp
#include "client/sound/AmbientSoundHandlers.hpp"

#include "client/entity/Player.hpp"
#include "client/renderer/environment/EnvironmentState.hpp"
#include "client/sound/AudioAttributes.hpp"
#include "client/sound/AurelithSounds.hpp"
#include "client/sound/ClientSounds.hpp"
#include "client/sound/LocalPlayerSounds.hpp"
#include "client/sound/SoundInstance.hpp"
#include "client/sound/SoundManager.hpp"
#include "client/world/HushStillnessState.hpp"
#include "common/core/JavaRandom.hpp"
#include "common/core/Profiling_Tracy.hpp"
#include "common/sound/SoundEvents.hpp"
#include "common/world/biome/Biomes.hpp"
#include "common/world/block/GeneratedBlockStates.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "platform/GameDirectory.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <memory>
#include <optional>
#include <string>

namespace Client::AmbientSounds {

    namespace {

        constexpr int kLoopCrossFadeTime = 40;         // BiomeAmbientSoundsHandler.LOOP_SOUND_CROSS_FADE_TIME
        constexpr float kSkyMoodRecoveryRate = 0.001f; // SKY_MOOD_RECOVERY_RATE
        constexpr int kStillnessFadeTicks = 40;

        Game::JavaRandom g_random(static_cast<int64_t>(std::chrono::steady_clock::now().time_since_epoch().count()));

        // Bumped by Reset: instances from an older world stop themselves.
        int   g_generation = 0;
        bool  g_underwater = false;       // LocalPlayer.wasUnderwater
        float g_stillnessGain = 1.0f;

        // ── BiomeAmbientSoundsHandler.LoopSoundInstance ─────────────────────
        class LoopSoundInstance final : public AbstractTickableSoundInstance {
        public:
            explicit LoopSoundInstance(const std::string& event)
                : AbstractTickableSoundInstance(event, Game::SoundSource::Ambient, SoundInstance::UnseededSeed()),
                  m_generation(g_generation) {
                m_looping = true;
                m_delay = 0;
                m_volume = 1.0f;
                m_relative = true;
            }
            void Tick() override {
                if (m_fade < 0 || m_generation != g_generation) Stop();
                m_fade += m_fadeDirection;
                // MC: volume = clamp(fade / 40); the Hush's stillness on top.
                m_volume = std::clamp(static_cast<float>(m_fade) / static_cast<float>(kLoopCrossFadeTime),
                                      0.0f, 1.0f) * g_stillnessGain;
            }
            // MC reads volume * sound.volume; the stillness can start this
            // at 0, and MC's play() would then refuse to start it at all.
            bool CanStartSilent() const override { return true; }
            void FadeOut() { m_fade = std::min(m_fade, kLoopCrossFadeTime); m_fadeDirection = -1; }
            void FadeIn()  { m_fade = std::max(0, m_fade);  m_fadeDirection = 1; }

        private:
            int m_fadeDirection = 0;
            int m_fade = 0;
            int m_generation;
        };

        // ── UnderLiquidAmbientSoundInstance (the underwater loop) ───────────
        class UnderLiquidAmbientSoundInstance final : public AbstractTickableSoundInstance {
        public:
            UnderLiquidAmbientSoundInstance()
                : AbstractTickableSoundInstance(Game::SoundEvents::AMBIENT_UNDERWATER_LOOP,
                                                Game::SoundSource::Ambient, SoundInstance::UnseededSeed()),
                  m_generation(g_generation) {
                m_looping = true;
                m_delay = 0;
                m_volume = 1.0f;
                m_relative = true;
            }
            void Tick() override {
                if (m_generation == g_generation && m_fade >= 0) {
                    if (g_underwater) ++m_fade;
                    else              m_fade -= 2;
                    m_fade = std::min(m_fade, 40);
                    m_volume = std::max(0.0f, std::min(static_cast<float>(m_fade) / 40.0f, 1.0f));
                } else {
                    Stop();
                }
            }
        private:
            int m_fade = 0;
            int m_generation;
        };

        // ── UnderLiquidSubSound (one underwater addition) ───────────────────
        class UnderLiquidSubSound final : public AbstractTickableSoundInstance {
        public:
            explicit UnderLiquidSubSound(const char* event)
                : AbstractTickableSoundInstance(event, Game::SoundSource::Ambient, SoundInstance::UnseededSeed()),
                  m_generation(g_generation) {
                m_looping = false;
                m_delay = 0;
                m_volume = 1.0f;
                m_relative = true;
            }
            void Tick() override {
                if (m_generation != g_generation || !g_underwater) Stop();
            }
        private:
            int m_generation;
        };

        // ── Handler state ───────────────────────────────────────────────────
        int   g_underwaterTickDelay = 0;              // UnderwaterAmbientSoundHandler.tickDelay
        bool  g_wasInBubbleColumn = false;            // BubbleColumnAmbientSoundHandler
        bool  g_bubbleFirstTick = true;
        float g_moodiness = 0.0f;                     // BiomeAmbientSoundsHandler
        std::optional<std::string> g_previousLoopSound;
        std::map<std::string, std::shared_ptr<LoopSoundInstance>> g_loopSounds;
        int   g_rainSoundTime = 0;                    // ClientLevel.rainSoundTime

        glm::ivec3 Containing(double x, double y, double z) {
            return glm::ivec3(static_cast<int>(std::floor(x)), static_cast<int>(std::floor(y)),
                              static_cast<int>(std::floor(z)));
        }

        // ── LocalPlayer.updateIsUnderwater ──────────────────────────────────
        void UpdateIsUnderwater(const Game::ClientPlayer& player) {
            const bool was = g_underwater;
            const bool now = player.physics.isEyeInWater;   // isEyeInFluid(WATER)
            g_underwater = now;
            if (player.IsSpectator()) return;
            if (!was && now) {
                Sounds::PlayLocal(player.physics.position, Game::SoundEvents::AMBIENT_UNDERWATER_ENTER,
                                  Game::SoundSource::Ambient, 1.0f, 1.0f, false);
                GetSoundManager().Play(std::make_shared<UnderLiquidAmbientSoundInstance>());
            }
            if (was && !now) {
                Sounds::PlayLocal(player.physics.position, Game::SoundEvents::AMBIENT_UNDERWATER_EXIT,
                                  Game::SoundSource::Ambient, 1.0f, 1.0f, false);
            }
        }

        // ── UnderwaterAmbientSoundHandler.tick ──────────────────────────────
        void TickUnderwater() {
            --g_underwaterTickDelay;
            if (g_underwaterTickDelay <= 0 && g_underwater) {
                const float rand = g_random.NextFloat();
                const char* event = nullptr;
                if (rand < 1.0e-4f)       event = Game::SoundEvents::AMBIENT_UNDERWATER_LOOP_ADDITIONS_ULTRA_RARE;
                else if (rand < 0.001f)   event = Game::SoundEvents::AMBIENT_UNDERWATER_LOOP_ADDITIONS_RARE;
                else if (rand < 0.01f)    event = Game::SoundEvents::AMBIENT_UNDERWATER_LOOP_ADDITIONS;
                if (event) {
                    g_underwaterTickDelay = 0;
                    GetSoundManager().Play(std::make_shared<UnderLiquidSubSound>(event));
                }
            }
        }

        // ── BubbleColumnAmbientSoundHandler.tick ────────────────────────────
        void TickBubbleColumn(const Game::ClientPlayer& player, const Game::IBlockAccess& blocks) {
            // The body box shrunk 0.4 top and bottom, deflated by 1e-6.
            const Game::PlayerPhysics& ph = player.physics;
            const double hw = ph.GetWidth() * 0.5 - 1.0e-6;
            const double minY = ph.position.y + 0.4 + 1.0e-6;
            const double maxY = ph.position.y + ph.GetCurrentHeight() - 0.4 - 1.0e-6;
            std::optional<Game::BlockState> column;
            if (maxY >= minY) {
                const glm::ivec3 lo = Containing(ph.position.x - hw, minY, ph.position.z - hw);
                const glm::ivec3 hi = Containing(ph.position.x + hw, maxY, ph.position.z + hw);
                for (int x = lo.x; x <= hi.x && !column; ++x)
                    for (int y = lo.y; y <= hi.y && !column; ++y)
                        for (int z = lo.z; z <= hi.z && !column; ++z) {
                            const Game::BlockState s = blocks.GetBlockState(x, y, z);
                            if (s.Block() == Game::BlockID::BubbleColumn) column = s;
                        }
            }
            if (column) {
                if (!g_wasInBubbleColumn && !g_bubbleFirstTick && !player.IsSpectator()) {
                    const bool dragDown = column->GetName(Game::PropertyId::DRAG) == "true";
                    Sounds::PlayLocal(ph.position,
                                      dragDown ? Game::SoundEvents::BUBBLE_COLUMN_WHIRLPOOL_INSIDE
                                               : Game::SoundEvents::BUBBLE_COLUMN_UPWARDS_INSIDE,
                                      Game::SoundSource::Players, 1.0f, 1.0f, false);
                }
                g_wasInBubbleColumn = true;
            } else {
                g_wasInBubbleColumn = false;
            }
            g_bubbleFirstTick = false;
        }

        // ── BiomeAmbientSoundsHandler.tick ──────────────────────────────────
        void TickBiome(const SoundHost::TickContext& ctx, const Game::ClientPlayer& player,
                       const Game::IBlockAccess& blocks) {
            for (auto it = g_loopSounds.begin(); it != g_loopSounds.end();) {
                if (it->second->IsStopped()) it = g_loopSounds.erase(it);
                else ++it;
            }

            const glm::dvec3 feet = player.physics.position;
            const AudioAttributes::AmbientSounds& ambient = AudioAttributes::AmbientSoundsAt(
                ctx.dimension, AudioAttributes::BiomeAt(feet));

            const std::optional<std::string> currentLoop =
                ambient.loop.empty() ? std::nullopt : std::optional<std::string>(ambient.loop);
            if (currentLoop != g_previousLoopSound) {
                g_previousLoopSound = currentLoop;
                for (auto& [id, loop] : g_loopSounds) { (void)id; loop->FadeOut(); }
                if (currentLoop) {
                    std::shared_ptr<LoopSoundInstance>& loop = g_loopSounds[*currentLoop];
                    if (!loop) {
                        loop = std::make_shared<LoopSoundInstance>(*currentLoop);
                        GetSoundManager().Play(loop);
                    }
                    loop->FadeIn();
                }
            }

            // The Hush's stillness: nothing new starts in the silence.
            const bool stilled = g_stillnessGain < 1.0f;

            for (const AudioAttributes::AmbientAdditionsSettings& additions : ambient.additions) {
                if (g_random.NextDouble() < additions.tickChance && !stilled) {
                    GetSoundManager().Play(SimpleSoundInstance::ForAmbientAddition(additions.sound));
                }
            }

            if (ambient.mood) {
                const AudioAttributes::AmbientMoodSettings& mood = *ambient.mood;
                const int searchSpan = mood.blockSearchExtent * 2 + 1;
                const glm::dvec3 eye = player.GetEyePosition();
                const glm::ivec3 sample = Containing(
                    feet.x + g_random.NextInt(searchSpan) - mood.blockSearchExtent,
                    eye.y  + g_random.NextInt(searchSpan) - mood.blockSearchExtent,
                    feet.z + g_random.NextInt(searchSpan) - mood.blockSearchExtent);
                // LightLayer.SKY via the engine's stand-in (15 open to the sky,
                // 0 roofed); there is no block light to read, so a sample in
                // the dark always counts as MC's "block light 0".
                const int skyBrightness = blocks.GetRawBrightness(sample.x, sample.y, sample.z);
                if (skyBrightness > 0) {
                    g_moodiness -= static_cast<float>(skyBrightness) / 15.0f * kSkyMoodRecoveryRate;
                } else {
                    const int blockBrightness = 0;
                    g_moodiness -= static_cast<float>(blockBrightness - 1) / static_cast<float>(mood.tickDelay);
                }

                if (g_moodiness >= 1.0f && !stilled) {
                    const double sx = sample.x + 0.5, sy = sample.y + 0.5, sz = sample.z + 0.5;
                    const double dx = sx - feet.x, dy = sy - eye.y, dz = sz - feet.z;
                    const double distance = std::sqrt(dx * dx + dy * dy + dz * dz);
                    const double sourceDistance = distance + mood.soundPositionOffset;
                    const double inv = distance > 1.0e-6 ? 1.0 / distance : 0.0;
                    GetSoundManager().Play(SimpleSoundInstance::ForAmbientMood(
                        mood.sound, SoundInstance::UnseededSeed(),
                        feet.x + dx * inv * sourceDistance,
                        eye.y  + dy * inv * sourceDistance,
                        feet.z + dz * inv * sourceDistance));
                    g_moodiness = 0.0f;
                } else {
                    g_moodiness = std::max(g_moodiness, 0.0f);
                }
            }
        }

        // ── ClientLevel.tickWeatherEffects: the rain's sound half ───────────
        void TickRain(const SoundHost::TickContext& ctx, const Game::IBlockAccess& blocks) {
            const float rainLevel = Render::EnvironmentState::Get().RainLevel();
            if (!(rainLevel > 0.0f)) return;
            const int particles = Platform::g_gameSettings.GetParticles();   // 0 all, 1 decreased, 2 minimal
            const int weatherRadius = std::clamp(Platform::g_gameSettings.GetInt("weatherRadius", 10), 3, 10);
            const int diameter = 2 * weatherRadius + 1;
            int samples = static_cast<int>(0.225f * static_cast<float>(diameter * diameter) * rainLevel * rainLevel)
                          / (particles == 1 ? 2 : 1);
            // Only the LAST qualifying column is what the sound needs; the
            // particle spawns MC makes from the others are not this module's.
            // A bounded sample keeps the column scans cheap.
            samples = std::min(samples, 24);

            const glm::ivec3 camera = Containing(ctx.cameraPosition.x, ctx.cameraPosition.y, ctx.cameraPosition.z);
            // The MOTION_BLOCKING heightmap within MC's ±10 of the camera:
            // a column open to the sky over the window, scanned down to its
            // first solid or fluid cell.
            std::optional<glm::ivec3> rainPosition;
            for (int i = 0; i < samples; ++i) {
                const int x = camera.x + g_random.NextInt(diameter) - weatherRadius;
                const int z = camera.z + g_random.NextInt(diameter) - weatherRadius;
                // The column must be open to the sky above the scan window.
                if (blocks.GetRawBrightness(x, camera.y + 10, z) == 0) continue;
                std::optional<int> top;
                for (int y = camera.y + 10; y >= camera.y - 10; --y) {
                    if (blocks.IsBlockSolid(x, y, z) || blocks.IsBlockFluid(x, y, z)) { top = y; break; }
                }
                if (!top) continue;
                // getPrecipitationAt == RAIN: a biome that rains and is not
                // cold enough to snow.
                const Game::BiomeInfo& biome = Game::BiomeRegistry::Get(blocks.GetBiome(x, *top, z));
                if (!(biome.downfall > 0.0f) || biome.temperature < 0.15f) continue;
                rainPosition = glm::ivec3(x, *top, z);   // heightmap pos .below()
                if (particles == 2) break;               // MINIMAL
            }
            if (rainPosition && g_random.NextInt(3) < g_rainSoundTime++) {
                g_rainSoundTime = 0;
                // MC: the camera's own column is roofed (its heightmap is
                // above the camera) — the rain is overhead, muffled.
                const bool roofed = blocks.GetRawBrightness(camera.x, camera.y, camera.z) == 0;
                const glm::dvec3 at = glm::dvec3(*rainPosition) + glm::dvec3(0.5);
                if (rainPosition->y > camera.y + 1 && roofed) {
                    Sounds::PlayLocal(at, Game::SoundEvents::WEATHER_RAIN_ABOVE, Game::SoundSource::Weather,
                                      0.1f, 0.5f, false);
                } else {
                    Sounds::PlayLocal(at, Game::SoundEvents::WEATHER_RAIN, Game::SoundSource::Weather,
                                      0.2f, 1.0f, false);
                }
            }
        }

        void TickStillness(const SoundHost::TickContext& ctx) {
            const bool stilled = ctx.dimension == Game::DimensionId::Hush &&
                                 HushStillnessState::g_active.load(std::memory_order_relaxed);
            const float step = 1.0f / static_cast<float>(kStillnessFadeTicks);
            g_stillnessGain = std::clamp(g_stillnessGain + (stilled ? -step : step), 0.0f, 1.0f);
        }

    } // namespace

    void Tick(const SoundHost::TickContext& ctx) {
        PROFILE_ZONE_N("Sound.Ambient");
        if (!ctx.player || !ctx.blocks) return;
        TickStillness(ctx);
        LocalPlayerSounds::Tick(ctx);
        if (ctx.levelLoading) return;
        const Game::ClientPlayer& player = *ctx.player;
        UpdateIsUnderwater(player);
        TickUnderwater();
        TickBubbleColumn(player, *ctx.blocks);
        TickBiome(ctx, player, *ctx.blocks);
        TickRain(ctx, *ctx.blocks);
    }

    void Reset() {
        ++g_generation;
        for (auto& [id, loop] : g_loopSounds) { (void)id; GetSoundManager().Stop(loop); }
        g_loopSounds.clear();
        g_previousLoopSound.reset();
        g_moodiness = 0.0f;
        g_underwater = false;
        g_underwaterTickDelay = 0;
        g_wasInBubbleColumn = false;
        g_bubbleFirstTick = true;
        g_rainSoundTime = 0;
        g_stillnessGain = 1.0f;
        LocalPlayerSounds::Reset();
        // The city loops belong to the same world as the biome loops.
        AurelithSounds::Reset();
    }

    float GetCurrentMood() { return g_moodiness; }

    float StillnessGain() { return g_stillnessGain; }

} // namespace Client::AmbientSounds
