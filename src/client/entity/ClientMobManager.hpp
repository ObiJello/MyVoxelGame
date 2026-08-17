// File: src/client/entity/ClientMobManager.hpp
//
// The client's mirror of the server's mobs.
//
// This deliberately constructs the SAME Game::Mob subclasses the server runs,
// against a client-side EntityLevel whose IsClientSide() is true. That single
// flag is what MC uses to split the two sides, and reusing the classes buys the
// client, for free and in guaranteed agreement with the server:
//
//   * gravity, drag, friction and collision (LivingEntity::Travel), so a mob
//     falling between two position packets falls at the right speed instead of
//     sliding down a straight line;
//   * the walk animation (WalkAnimationState) driven by real displacement;
//   * hurtTime, deathTime and the creeper fuse, so those animations play out
//     smoothly rather than stepping at the packet rate.
//
// What it does NOT get is AI: LivingEntity::IsEffectiveAi() is false on the
// client, so ServerAiStep never runs. Goals, navigation and targeting are
// server-only, exactly as in MC.
//
// Server snapshots are applied as MC-style interpolation corrections
// (InterpolationHandler, 3 steps) rather than hard sets — the same approach
// Client::ItemEntityManager already documents.
#pragma once

#include "common/entity/EntityLevel.hpp"
#include "common/entity/Mob.hpp"
#include "common/core/JavaRandom.hpp"

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace Game { struct IBlockAccess; }

namespace Client {

    // Game::EntityLevel over the client's block view. Most of the interface is
    // inert here: a client mob never spawns anything, never drops items, and
    // never queries players for AI purposes.
    class ClientLevelBridge : public Game::EntityLevel {
    public:
        void SetBlocks(const Game::IBlockAccess* blocks) { m_blocks = blocks; }
        void SetDayTime(int64_t t) { m_dayTime = t; }
        void SetGameTime(int64_t t) { m_gameTime = t; }

        const Game::IBlockAccess* Blocks() const override { return m_blocks; }
        bool IsClientSide() const override { return true; }
        int64_t GetGameTime() const override { return m_gameTime; }
        int64_t GetDayTime()  const override { return m_dayTime; }
        Game::JavaRandom& Random() override { return m_random; }

        // The client never runs a spawn or AI light test, so a constant is
        // honest here — anything that did read it would be a bug.
        int  GetSkyBrightness(int, int, int) const override { return 15; }
        int  GetMaxLocalRawBrightness(int, int, int) const override { return 15; }
        int  GetMaxLocalRawBrightness(int, int, int, int) const override { return 15; }
        int  GetSkyDarken() const override { return 0; }
        bool CanSeeSky(int, int, int) const override { return true; }
        bool MonstersBurn() const override { return false; }
        bool IsDay() const override { return (m_dayTime % 24000) < 12000; }

        void GetEntitiesInBox(const Game::AABB&, const Game::Entity*,
                              std::vector<Game::Entity*>&) const override {}
        Game::LivingEntity* GetNearestPlayer(double, double, double, double) const override {
            return nullptr;
        }
        void GetPlayers(std::vector<Game::LivingEntity*>&) const override {}
        void BroadcastEntityEvent(const Game::Entity&, uint8_t) override {}

    private:
        const Game::IBlockAccess* m_blocks = nullptr;
        int64_t m_dayTime = 0;
        int64_t m_gameTime = 0;
        Game::JavaRandom m_random{0};
    };

    // One mirrored mob, plus the interpolation state layered on top.
    struct ClientMob {
        std::unique_ptr<Game::Mob> mob;

        // MC InterpolationHandler: `steps` remaining corrections toward
        // `target`. Applied at the START of the client tick, before the mob's
        // own physics runs, so local simulation and correction compose instead
        // of fighting.
        int        interpSteps = 0;
        glm::dvec3 targetPosition{0.0};
        float      targetYRot = 0.0f;
        float      targetXRot = 0.0f;
        float      targetYHeadRot = 0.0f;

        // Previous-tick snapshot for sub-tick render interpolation, exactly
        // like RemotePlayer::renderPrev*.
        glm::dvec3 renderPrevPosition{0.0};
        float      renderPrevYRot = 0.0f;
        float      renderPrevXRot = 0.0f;
        float      renderPrevYHeadRot = 0.0f;
        float      renderPrevYBodyRot = 0.0f;

        // Creeper fuse, mirrored so the render can lerp it.
        uint8_t swell = 0, oldSwell = 0;
    };

    class ClientMobManager {
    public:
        static constexpr int kInterpSteps = 3;
        // Past this the mob is snapped rather than interpolated — a correction
        // that large is a teleport or a missed packet, and easing into it would
        // send the mob sliding across the world.
        static constexpr double kSnapDistanceSq = 16.0 * 16.0;

        void SetBlockAccess(const Game::IBlockAccess* blocks) { m_level.SetBlocks(blocks); }
        void SetTime(int64_t gameTime, int64_t dayTime) {
            m_level.SetGameTime(gameTime);
            m_level.SetDayTime(dayTime);
        }

        // Packet entry points.
        void Spawn(int32_t id, uint16_t type, const glm::dvec3& pos, const glm::vec3& vel,
                   float yRot, float xRot, float yHeadRot,
                   float health, uint8_t flags, uint8_t variantData,
                   uint8_t pose, uint8_t animState);
        void MoveDelta(int32_t id, bool hasPos, const glm::dvec3& delta,
                       bool hasRot, float yRot, float xRot, float yHeadRot, bool onGround);
        void Teleport(int32_t id, const glm::dvec3& pos, const glm::vec3& vel,
                      float yRot, float xRot, float yHeadRot, bool onGround);
        void SetMotion(int32_t id, const glm::vec3& vel);
        void SetData(int32_t id, float health, uint8_t flags, uint8_t variantData,
                     uint8_t hurtTime, uint8_t deathTime, uint8_t swellDir, uint8_t swell,
                     uint8_t pose, uint8_t animState);
        void HandleEvent(int32_t id, uint8_t event);
        void Remove(int32_t id);
        void Clear();

        // 20 Hz client tick.
        void Tick();

        const std::unordered_map<int32_t, ClientMob>& All() const { return m_mobs; }
        size_t Count() const { return m_mobs.size(); }

        // The last decoded position for an id, kept so MoveEntity deltas can be
        // accumulated against the same base the server encoded against. MC
        // keeps this in the entity's VecDeltaCodec; here it is explicit because
        // the delta must NOT be applied to the locally-simulated position,
        // which has drifted since the last packet.
        bool GetCodecBase(int32_t id, glm::dvec3& out) const;

    private:
        ClientMob* Find(int32_t id);

        ClientLevelBridge m_level;
        std::unordered_map<int32_t, ClientMob> m_mobs;
        std::unordered_map<int32_t, glm::dvec3> m_codecBase;
    };

    extern std::unique_ptr<ClientMobManager> g_clientMobManager;

} // namespace Client
