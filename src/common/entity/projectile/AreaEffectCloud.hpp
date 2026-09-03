// File: src/common/entity/projectile/AreaEffectCloud.hpp
//
// MC net.minecraft.world.entity.AreaEffectCloud — the lingering ground hazard
// a dragon fireball leaves behind (and an effect-carrying creeper's death
// cloud). A plain Entity in MC; here it rides the projectile Misc pipeline
// like EvokerFangs (see Projectile.hpp's architecture note) with every bit of
// mob machinery inert — Tick is overridden wholesale.
//
// Ported from AreaEffectCloud.java, serverTick():
//   * ages out after waitTime + duration ticks (duration -1 = infinite);
//   * radius shrinks/grows by radiusPerTick each post-wait tick and the cloud
//     discards below the 0.5 minimum;
//   * every 5 ticks (TIME_BETWEEN_APPLICATIONS) each living entity inside the
//     radius (horizontal distance, exactly as MC measures it) that is not on
//     reapplication cooldown receives every stored effect — instantaneous
//     ones through applyInstantenousEffect at scale 0.5, the rest via
//     addEffect — then goes on cooldown for reapplicationDelay (20) ticks and
//     radiusOnUse/durationOnUse are applied;
//   * potionDurationScale scales stored non-instant durations on application
//     (MC PotionContents.forEachEffect's withScaledDuration).
//
// Not modelled, each named at its site: the particle field (DATA_PARTICLE +
// clientTick — no particle system; the cloud is INVISIBLE, which is the
// documented render answer for this wave), the DATA_RADIUS/DATA_WAITING sync
// (client renders nothing, so nothing reads them), and PotionContents (the
// effect list is stored directly, the shape our witch already uses).
#pragma once

#include "common/entity/projectile/Projectile.hpp"

#include <unordered_map>
#include <vector>

namespace Game {

    class AreaEffectCloud : public Projectile {
    public:
        static constexpr int   kTimeBetweenApplications = 5;    // MC
        static constexpr float kMaxRadius = 32.0f;              // MC MAX_RADIUS
        static constexpr float kMinimalRadius = 0.5f;           // MC MINIMAL_RADIUS
        static constexpr float kDefaultRadius = 3.0f;           // MC DEFAULT_RADIUS
        static constexpr int   kDefaultLingeringDuration = 600; // MC
        static constexpr int   kDefaultWaitTime = 20;           // MC
        static constexpr int   kDefaultReapplicationDelay = 20; // MC

        explicit AreaEffectCloud(EntityLevel* level);

        // MC EntityType.AREA_EFFECT_CLOUD is fireImmune.
        bool FireImmune() const override { return true; }

        // MC getDimensions: EntityDimensions.scalable(radius * 2, 0.5).
        float BaseBbWidth() const override { return m_radius * 2.0f; }
        float BaseBbHeight() const override { return 0.5f; }

        // MC setRadius clamps to [0, 32]; the discard-below-minimum rule
        // lives in the tick, as in MC.
        void  SetRadius(float radius);
        float GetRadius() const { return m_radius; }

        int  GetDuration() const { return m_duration; }
        void SetDuration(int duration) { m_duration = duration; }
        void SetWaitTime(int waitTime) { m_waitTime = waitTime; }
        void SetReapplicationDelay(int delay) { m_reapplicationDelay = delay; }
        void SetRadiusOnUse(float radiusOnUse) { m_radiusOnUse = radiusOnUse; }
        void SetRadiusPerTick(float radiusPerTick) { m_radiusPerTick = radiusPerTick; }
        void SetDurationOnUse(int durationOnUse) { m_durationOnUse = durationOnUse; }
        void SetPotionDurationScale(float scale) { m_potionDurationScale = scale; }

        // Read side of the same set, for the save layer.
        int   GetWaitTime() const { return m_waitTime; }
        int   GetReapplicationDelay() const { return m_reapplicationDelay; }
        int   GetDurationOnUse() const { return m_durationOnUse; }
        float GetRadiusOnUse() const { return m_radiusOnUse; }
        float GetRadiusPerTick() const { return m_radiusPerTick; }
        float GetPotionDurationScale() const { return m_potionDurationScale; }

        // SetRadius clamps and resizes the box; loading must not re-run the
        // "shrunk below minimum" discard, so the save layer uses this.
        void SetRadiusRaw(float radius) { m_radius = radius; }

        // MC addEffect(MobEffectInstance) — appends to the stored contents.
        void AddCloudEffect(const MobEffectInstance& effect) {
            m_effects.push_back(effect);
        }

        void Tick() override;

        void ClearReferenceTo(const Entity* entity) override {
            Projectile::ClearReferenceTo(entity);
            m_victims.erase(const_cast<Entity*>(entity));
        }

    private:
        void ServerTick();

        float m_radius = kDefaultRadius;
        int   m_duration = -1;              // MC INFINITE_DURATION default
        int   m_waitTime = kDefaultWaitTime;
        int   m_reapplicationDelay = kDefaultReapplicationDelay;
        int   m_durationOnUse = 0;
        float m_radiusOnUse = 0.0f;
        float m_radiusPerTick = 0.0f;
        float m_potionDurationScale = 1.0f;

        std::vector<MobEffectInstance> m_effects;

        // MC's victims map: entity -> tickCount at which it may be hit again.
        std::unordered_map<Entity*, int> m_victims;
    };

} // namespace Game
