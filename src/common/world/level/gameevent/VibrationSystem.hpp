// File: src/common/world/level/gameevent/VibrationSystem.hpp
//
// Port of net.minecraft.world.level.gameevent.vibrations (26.3):
//   VibrationInfo, VibrationSelector, VibrationSystem (.Data, .User,
//   .Listener, .Ticker) and the static helpers (frequencies live on
//   GameEvents, the redstone strength for a distance lives here).
//
// A vibration's life: a game event reaches a listener (Listener.
// handleGameEvent); if the user can hear it and nothing occludes the line,
// it becomes a CANDIDATE in the selector — the nearest this tick, ties to the
// higher frequency. On the next tick the ticker promotes the candidate to the
// current vibration, sends the VIBRATION particle flying to the listener and
// counts its travel time down (one tick per block); on arrival the user's
// onReceiveVibration fires (a sensor activates, a shrieker shrieks, a warden
// gets angry).
//
// DEVIATION: MC's VibrationInfo also carries the live Entity; here it holds
// only the UUIDs and resolves them through the level on arrival (the entity
// may have died or been freed during the flight — MC's reference would have
// kept a removed entity alive, which a C++ pointer cannot).
#pragma once

#include "GameEvent.hpp"
#include "GameEventDispatcher.hpp"

#include "common/core/Uuid.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>

namespace Game {

    class World;
    class Entity;
    struct EntityLevel;

    // MC VibrationInfo.
    struct VibrationInfo {
        GameEventId         gameEvent = GameEventId::Step;
        float               distance = 0.0f;
        glm::dvec3          pos{0.0};
        std::optional<Uuid> uuid;                  // the source entity
        std::optional<Uuid> projectileOwnerUuid;   // its owner, when it is a projectile

        // MC VibrationInfo(gameEvent, distance, pos, entity).
        static VibrationInfo Of(GameEventId event, float distance, const glm::dvec3& pos, Entity* entity);

        // MC getEntity(level) / getProjectileOwner(level).
        Entity* ResolveEntity(EntityLevel* level) const;
        Entity* ResolveProjectileOwner(EntityLevel* level) const;
    };

    // MC VibrationSelector — the one candidate for the next vibration.
    class VibrationSelector {
    public:
        VibrationSelector() = default;
        VibrationSelector(std::optional<VibrationInfo> event, int64_t tick) {
            if (event) m_current = Candidate{*event, tick};
        }

        void AddCandidate(const VibrationInfo& vibration, int64_t tickTime);
        // The candidate, once a tick has passed since it was chosen.
        std::optional<VibrationInfo> ChosenCandidate(int64_t time) const;
        void StartOver() { m_current.reset(); }

        // For the codec ("event" + "tick").
        std::optional<VibrationInfo> CandidateEvent() const {
            return m_current ? std::optional<VibrationInfo>(m_current->event) : std::nullopt;
        }
        int64_t CandidateTick() const { return m_current ? m_current->tick : -1; }

    private:
        bool ShouldReplaceVibration(const VibrationInfo& vibration, int64_t tickTime) const;

        struct Candidate {
            VibrationInfo event;
            int64_t       tick;
        };
        std::optional<Candidate> m_current;
    };

    namespace Vibrations {

        // MC VibrationSystem.getRedstoneStrengthForDistance.
        int RedstoneStrengthForDistance(float distance, int listenerRadius);

        // MC VibrationSystem.Listener.distanceBetweenInBlocks.
        float DistanceBetweenInBlocks(const glm::ivec3& origin, const glm::ivec3& dest);

        // #minecraft:occludes_vibration_signals / #minecraft:dampens_vibrations /
        // #minecraft:vibration_resonators for one block.
        bool OccludesVibrationSignals(BlockID block);
        bool DampensVibrations(BlockID block);
        bool IsVibrationResonator(BlockID block);

    } // namespace Vibrations

    // MC VibrationSystem.Data — persisted as the "listener" compound.
    class VibrationData {
    public:
        std::optional<VibrationInfo>& CurrentVibration() { return m_currentVibration; }
        const std::optional<VibrationInfo>& CurrentVibration() const { return m_currentVibration; }
        void SetCurrentVibration(std::optional<VibrationInfo> v) { m_currentVibration = std::move(v); }

        int  GetTravelTimeInTicks() const { return m_travelTimeInTicks; }
        void SetTravelTimeInTicks(int t) { m_travelTimeInTicks = t; }
        void DecrementTravelTime() { m_travelTimeInTicks = m_travelTimeInTicks > 1 ? m_travelTimeInTicks - 1 : 0; }

        VibrationSelector& Selection() { return m_selector; }
        const VibrationSelector& Selection() const { return m_selector; }

        bool ShouldReloadVibrationParticle() const { return m_reloadVibrationParticle; }
        void SetReloadVibrationParticle(bool r) { m_reloadVibrationParticle = r; }

        // MC's codec constructor: a loaded Data always re-sends its particle
        // (reloadVibrationParticle = true), so a vibration in flight when the
        // chunk was saved is seen to finish its flight.
        static VibrationData Loaded(std::optional<VibrationInfo> current, VibrationSelector selector,
                                    int travelTimeInTicks) {
            VibrationData d;
            d.m_currentVibration = std::move(current);
            d.m_selector = std::move(selector);
            d.m_travelTimeInTicks = travelTimeInTicks < 0 ? 0 : travelTimeInTicks;
            d.m_reloadVibrationParticle = true;
            return d;
        }

    private:
        std::optional<VibrationInfo> m_currentVibration;
        int                          m_travelTimeInTicks = 0;
        VibrationSelector            m_selector;
        bool                         m_reloadVibrationParticle = false;
    };

    // MC VibrationSystem.User.
    class VibrationUser {
    public:
        virtual ~VibrationUser() = default;

        virtual int GetListenerRadius() const = 0;
        virtual const PositionSource& GetPositionSource() const = 0;
        virtual bool CanReceiveVibration(World& level, const glm::ivec3& pos, GameEventId event,
                                         const GameEventContext& context) = 0;
        virtual void OnReceiveVibration(World& level, const glm::ivec3& pos, GameEventId event,
                                        Entity* sourceEntity, Entity* projectileOwner,
                                        float receivingDistance) = 0;

        // MC getListenableEvents — #vibrations by default.
        virtual GameEvents::Tag GetListenableEvents() const { return GameEvents::Tag::Vibrations; }
        // MC canTriggerAvoidVibration — only the advancement reads it.
        virtual bool CanTriggerAvoidVibration() const { return false; }
        virtual bool RequiresAdjacentChunksToBeTicking() const { return false; }
        // MC calculateTravelTimeInTicks: one tick per block.
        virtual int CalculateTravelTimeInTicks(float distanceToDestination) const;
        // MC isValidVibration: listenable, not a spectator, not sneaking past
        // an #ignore_vibrations_sneaking event, not a dampening entity or
        // block.
        virtual bool IsValidVibration(GameEventId event, const GameEventContext& context) const;
        virtual void OnDataChanged() {}
    };

    // MC VibrationSystem — what owns a Data and a User (the listening block
    // entities and the warden).
    class VibrationSystem {
    public:
        virtual ~VibrationSystem() = default;
        virtual VibrationData& GetVibrationData() = 0;
        virtual VibrationUser& GetVibrationUser() = 0;
    };

    // MC VibrationSystem.Listener.
    class VibrationListener : public GameEventListener {
    public:
        explicit VibrationListener(VibrationSystem& system) : m_system(system) {}

        const PositionSource& GetListenerSource() const override {
            return m_system.GetVibrationUser().GetPositionSource();
        }
        int GetListenerRadius() const override { return m_system.GetVibrationUser().GetListenerRadius(); }
        bool HandleGameEvent(World& level, GameEventId event, const GameEventContext& context,
                             const glm::dvec3& sourcePosition) override;

        // MC forceScheduleVibration — the sensor's stepOn path, which skips
        // the listenability, occlusion and "already busy" checks.
        void ForceScheduleVibration(World& level, GameEventId event, const GameEventContext& context,
                                    const glm::dvec3& origin);

        // MC Listener.isOccluded: all six nudged rays from the source's
        // block centre to the destination's hit an #occludes_vibration_signals
        // block.
        static bool IsOccluded(const World& level, const glm::dvec3& origin, const glm::dvec3& dest);

    private:
        void ScheduleVibration(World& level, VibrationData& data, GameEventId event,
                               const GameEventContext& context, const glm::dvec3& origin,
                               const glm::dvec3& dest);

        VibrationSystem& m_system;
    };

    // MC VibrationSystem.Ticker — run from the owner's server tick.
    namespace VibrationTicker {
        void Tick(World& level, VibrationData& data, VibrationUser& user);
    }

} // namespace Game
