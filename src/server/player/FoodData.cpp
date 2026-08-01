// File: src/server/player/FoodData.cpp
// Verbatim port of net/minecraft/world/food/FoodData.java (see header).
#include "FoodData.hpp"
#include "ServerPlayer.hpp"

#include <algorithm>

namespace Server {

    namespace {
        // FoodConstants.saturationByModifier — FoodConstants.java:30-32:
        //   nutrition * saturationModifier * 2.0F
        float SaturationByModifier(int nutrition, float saturationModifier) {
            return static_cast<float>(nutrition) * saturationModifier * 2.0f;
        }
    }

    // FoodData.add — FoodData.java:19-22.
    void FoodData::add(int food, float saturation) {
        m_foodLevel       = std::clamp(food + m_foodLevel, 0, 20);
        m_saturationLevel = std::clamp(saturation + m_saturationLevel,
                                       0.0f, static_cast<float>(m_foodLevel));
    }

    // FoodData.eat(int, float) — :24-26.
    void FoodData::eat(int nutrition, float saturationModifier) {
        add(nutrition, SaturationByModifier(nutrition, saturationModifier));
    }

    // FoodData.eat(FoodProperties) — :28-30 (properties carry the final value).
    void FoodData::eatFinal(int nutrition, float saturation) {
        add(nutrition, saturation);
    }

    // FoodData.tick — FoodData.java:32-73.
    void FoodData::tick(ServerPlayer& player) {
        // Difficulty pinned to NORMAL (no difficulty setting): the PEACEFUL
        // no-hunger-drain branch and the HARD starvation exception both
        // resolve to NORMAL behaviour below.

        // :35-42 — exhaustion drains saturation first, then food.
        if (m_exhaustionLevel > 4.0f) {
            m_exhaustionLevel -= 4.0f;
            if (m_saturationLevel > 0.0f) {
                m_saturationLevel = std::max(m_saturationLevel - 1.0f, 0.0f);
            } else {  // difficulty != PEACEFUL (pinned)
                m_foodLevel = std::max(m_foodLevel - 1, 0);
            }
        }

        // :44 — naturalRegeneration gamerule pinned to true (no gamerules).
        const bool naturalRegen = true;
        const bool isHurt = player.getHealth() < 20.0f;  // Player.isHurt()

        if (naturalRegen && m_saturationLevel > 0.0f && isHurt && m_foodLevel >= 20) {
            // :45-52 — fast saturation-powered regen: every 10 ticks heal
            // min(saturation, 6)/6 HP at the cost of that much exhaustion.
            ++m_tickTimer;
            if (m_tickTimer >= 10) {
                const float saturationSpent = std::min(m_saturationLevel, 6.0f);
                player.heal(saturationSpent / 6.0f);
                addExhaustion(saturationSpent);
                m_tickTimer = 0;
            }
        } else if (naturalRegen && m_foodLevel >= 18 && isHurt) {
            // :53-59 — slow regen: 1 HP every 80 ticks for 6 exhaustion.
            ++m_tickTimer;
            if (m_tickTimer >= 80) {
                player.heal(1.0f);
                addExhaustion(6.0f);
                m_tickTimer = 0;
            }
        } else if (m_foodLevel <= 0) {
            // :60-68 — starvation: 1 damage every 80 ticks while health > 1
            // (NORMAL difficulty rule; HARD would starve to death).
            ++m_tickTimer;
            if (m_tickTimer >= 80) {
                if (player.getHealth() > 1.0f) {
                    player.damage(1.0f, DamageSource::STARVATION);
                }
                m_tickTimer = 0;
            }
        } else {
            m_tickTimer = 0;  // :70
        }
    }

    // FoodData.addExhaustion — :101-103.
    void FoodData::addExhaustion(float amount) {
        m_exhaustionLevel = std::min(m_exhaustionLevel + amount, 40.0f);
    }

} // namespace Server
