// File: src/server/player/FoodData.hpp
//
// Mirrors net/minecraft/world/food/FoodData.java — the hunger / saturation /
// exhaustion triple on a player, plus the per-tick regen/starvation logic.
// Ported verbatim (FoodData.java:11-116) with two pinned simplifications,
// each commented at the site:
//   • difficulty pinned to NORMAL (no difficulty setting exists)
//   • naturalRegeneration gamerule pinned to true (no gamerules exist)
#pragma once

namespace Server {

    class ServerPlayer;

    class FoodData {
    public:
        // Mirrors FoodData.eat(int, float) — FoodData.java:24-26. Converts a
        // saturation MODIFIER via FoodConstants.saturationByModifier
        // (= nutrition * modifier * 2, FoodConstants.java:30-32).
        void eat(int nutrition, float saturationModifier);

        // Mirrors FoodData.eat(FoodProperties) — :28-30 — takes the FINAL
        // saturation value (our Game::FoodProperties stores it pre-converted).
        void eatFinal(int nutrition, float saturation);

        // Per-tick hunger logic — FoodData.java:32-73.
        void tick(ServerPlayer& player);

        // FoodData.java:101-103 (clamped at 40).
        void addExhaustion(float amount);

        int   getFoodLevel() const      { return m_foodLevel; }         // :89-91
        float getSaturationLevel() const{ return m_saturationLevel; }   // :105-107
        bool  hasEnoughFood() const     { return (float)m_foodLevel > 6.0f; }  // :93-95
        bool  needsFood() const         { return m_foodLevel < 20; }    // :97-99

        void setFoodLevel(int food)         { m_foodLevel = food; }     // :109-111
        void setSaturation(float saturation){ m_saturationLevel = saturation; } // :113-115

    private:
        // Defaults mirror FoodData.java:14-17.
        int   m_foodLevel       = 20;
        float m_saturationLevel = 5.0f;
        float m_exhaustionLevel = 0.0f;
        int   m_tickTimer       = 0;

        // FoodData.add — :19-22.
        void add(int food, float saturation);
    };

} // namespace Server
