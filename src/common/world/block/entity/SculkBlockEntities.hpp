// File: src/common/world/block/entity/SculkBlockEntities.hpp
//
// The sculk family's block entities (MC 26.3):
//
//   SculkSensorBlockEntity            — a vibration listener (radius 8) that
//                                       activates its block; remembers the
//                                       last frequency for the comparator
//   CalibratedSculkSensorBlockEntity  — radius 16, filtered to the frequency
//                                       the redstone behind it selects
//   SculkShriekerBlockEntity          — hears players, shrieks, warns, and
//                                       at warning level 4 summons a warden
//   SculkCatalystBlockEntity          — hears deaths (BY_DISTANCE) and turns
//                                       the XP into a SculkSpreader charge
//
// All four are server-ticking listeners: Tick asserts the entity's
// registration with the level's GameEventDispatcher (see its header note on
// registration by position) and runs MC's ticker. Their persistent state is
// written by server/world/storage/anvil/BlockEntityNbt.cpp (the "listener"
// compound through VibrationNbt, the catalyst's "cursors", the shrieker's
// "warning_level", the sensor's "last_vibration_frequency"); MC sends none of
// it to clients, and neither does the wire payload here.
#pragma once

#include "BlockEntity.hpp"

#include "common/world/block/SculkSpreader.hpp"
#include "common/world/level/gameevent/GameEventDispatcher.hpp"
#include "common/world/level/gameevent/VibrationSystem.hpp"

#include <memory>

namespace Game {

    class LivingEntity;

    // ── Sculk sensor ────────────────────────────────────────────────────────

    class SculkSensorBlockEntity : public BlockEntity,
                                   public GameEventListenerProvider,
                                   public VibrationSystem {
    public:
        SculkSensorBlockEntity(const BlockEntityType* type, glm::ivec3 worldPos, BlockID blockId);

        bool NeedsTicking() const override { return true; }
        void Tick(World* world, float deltaTime) override;

        GameEventListener* GetGameEventListener() override { return &m_listener; }
        VibrationListener& GetListener() { return m_listener; }
        VibrationData& GetVibrationData() override { return m_data; }
        VibrationUser& GetVibrationUser() override { return *m_user; }
        void SetVibrationData(VibrationData data) { m_data = std::move(data); }
        const VibrationData& SavedVibrationData() const { return m_data; }

        int  GetLastVibrationFrequency() const { return m_lastVibrationFrequency; }
        void SetLastVibrationFrequency(int f) { m_lastVibrationFrequency = f; }

    protected:
        // MC SculkSensorBlockEntity.VibrationUser.
        class User : public VibrationUser {
        public:
            explicit User(SculkSensorBlockEntity& owner)
                : m_owner(owner), m_source(PositionSource::OfBlock(owner.GetWorldPos())) {}
            int GetListenerRadius() const override { return 8; }
            const PositionSource& GetPositionSource() const override { return m_source; }
            bool CanTriggerAvoidVibration() const override { return true; }
            bool CanReceiveVibration(World& level, const glm::ivec3& pos, GameEventId event,
                                     const GameEventContext& context) override;
            void OnReceiveVibration(World& level, const glm::ivec3& pos, GameEventId event, Entity* sourceEntity,
                                    Entity* projectileOwner, float receivingDistance) override;
            void OnDataChanged() override { m_owner.MarkDirty(); }
            bool RequiresAdjacentChunksToBeTicking() const override { return true; }

        protected:
            SculkSensorBlockEntity& m_owner;
            PositionSource          m_source;
        };

        // The calibrated sensor swaps in its own user (MC createVibrationUser).
        void SetUser(std::unique_ptr<User> user) { m_user = std::move(user); }

    private:
        std::unique_ptr<User> m_user;
        VibrationData         m_data;
        VibrationListener     m_listener;
        int                   m_lastVibrationFrequency = 0;
    };

    class CalibratedSculkSensorBlockEntity : public SculkSensorBlockEntity {
    public:
        CalibratedSculkSensorBlockEntity(const BlockEntityType* type, glm::ivec3 worldPos, BlockID blockId);

    private:
        // MC CalibratedSculkSensorBlockEntity.VibrationUser.
        class CalibratedUser : public User {
        public:
            using User::User;
            int GetListenerRadius() const override { return 16; }
            bool CanReceiveVibration(World& level, const glm::ivec3& pos, GameEventId event,
                                     const GameEventContext& context) override;
        };
    };

    // ── Sculk shrieker ──────────────────────────────────────────────────────

    class SculkShriekerBlockEntity : public BlockEntity,
                                     public GameEventListenerProvider,
                                     public VibrationSystem {
    public:
        // MC SculkShriekerBlockEntity constants.
        static constexpr int kWarningSoundRadius = 10;
        static constexpr int kWardenSpawnAttempts = 20;
        static constexpr int kWardenSpawnRangeXZ = 5;
        static constexpr int kWardenSpawnRangeY = 6;
        static constexpr int kDarknessRadius = 40;
        static constexpr int kShriekingTicks = 90;

        SculkShriekerBlockEntity(const BlockEntityType* type, glm::ivec3 worldPos, BlockID blockId);

        bool NeedsTicking() const override { return true; }
        void Tick(World* world, float deltaTime) override;
        // MC preRemoveSideEffects: a shrieker broken mid-shriek still answers.
        void PreRemoveSideEffects(ILevelWrite& level, const glm::ivec3& pos, BlockState oldState) override;

        GameEventListener* GetGameEventListener() override { return &m_listener; }
        VibrationData& GetVibrationData() override { return m_data; }
        VibrationUser& GetVibrationUser() override { return m_user; }
        void SetVibrationData(VibrationData data) { m_data = std::move(data); }
        const VibrationData& SavedVibrationData() const { return m_data; }

        int  GetWarningLevel() const { return m_warningLevel; }
        void SetWarningLevel(int level) { m_warningLevel = level; }

        // MC tryGetPlayer: the player behind an event — the entity itself,
        // its controlling rider, or a projectile's owner.
        static LivingEntity* TryGetPlayer(Entity* sourceEntity);

        // MC tryShriek / tryRespond. The two-argument tryRespond answers for
        // `state` rather than the level's (a shrieker being broken).
        void TryShriek(World& level, LivingEntity* player);
        void TryRespond(World& level);
        void TryRespond(World& level, BlockState state);

    private:
        bool TryToWarn(World& level, LivingEntity& player);
        void Shriek(World& level, Entity* sourceEntity);
        bool CanRespond(World& level, BlockState state) const;
        void PlayWardenReplySound(World& level);
        bool TrySummonWarden(World& level);

        // MC SculkShriekerBlockEntity.VibrationUser.
        class User : public VibrationUser {
        public:
            explicit User(SculkShriekerBlockEntity& owner)
                : m_owner(owner), m_source(PositionSource::OfBlock(owner.GetWorldPos())) {}
            int GetListenerRadius() const override { return 8; }
            const PositionSource& GetPositionSource() const override { return m_source; }
            GameEvents::Tag GetListenableEvents() const override { return GameEvents::Tag::ShriekerCanListen; }
            bool CanReceiveVibration(World& level, const glm::ivec3& pos, GameEventId event,
                                     const GameEventContext& context) override;
            void OnReceiveVibration(World& level, const glm::ivec3& pos, GameEventId event, Entity* sourceEntity,
                                    Entity* projectileOwner, float receivingDistance) override;
            void OnDataChanged() override { m_owner.MarkDirty(); }
            bool RequiresAdjacentChunksToBeTicking() const override { return true; }

        private:
            SculkShriekerBlockEntity& m_owner;
            PositionSource            m_source;
        };

        int               m_warningLevel = 0;
        User              m_user;
        VibrationData     m_data;
        VibrationListener m_listener;
    };

    // ── Sculk catalyst ──────────────────────────────────────────────────────

    class SculkCatalystBlockEntity : public BlockEntity, public GameEventListenerProvider {
    public:
        SculkCatalystBlockEntity(const BlockEntityType* type, glm::ivec3 worldPos, BlockID blockId);

        bool NeedsTicking() const override { return true; }
        // MC SculkCatalystBlockEntity.serverTick: the spreader's step.
        void Tick(World* world, float deltaTime) override;

        GameEventListener* GetGameEventListener() override { return &m_listener; }
        SculkSpreader& GetSculkSpreader() { return m_listener.spreader; }
        const SculkSpreader& GetSculkSpreader() const { return m_listener.spreader; }

    private:
        // MC SculkCatalystBlockEntity.CatalystListener.
        class CatalystListener : public GameEventListener {
        public:
            static constexpr int kPulseTicks = 8;

            explicit CatalystListener(const glm::ivec3& pos)
                : spreader(SculkSpreader::CreateLevelSpreader()), m_source(PositionSource::OfBlock(pos)) {}

            const PositionSource& GetListenerSource() const override { return m_source; }
            int GetListenerRadius() const override { return 8; }
            DeliveryMode GetDeliveryMode() const override { return DeliveryMode::ByDistance; }
            bool HandleGameEvent(World& level, GameEventId event, const GameEventContext& context,
                                 const glm::dvec3& sourcePosition) override;

            SculkSpreader spreader;

        private:
            void Bloom(World& level, const glm::ivec3& pos);
            PositionSource m_source;
        };

        CatalystListener m_listener;
    };

} // namespace Game
