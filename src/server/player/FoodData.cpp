// File: src/server/player/FoodData.cpp
// Verbatim port of net/minecraft/world/food/FoodData.java (see header).
#include "FoodData.hpp"
#include "ServerPlayer.hpp"
#include "common/world/level/GameRules.hpp"

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
    void FoodData::tick(ServerPlayer& player, int difficulty) {
        const bool peaceful = difficulty == 0;
        // :44 — the natural_health_regeneration game rule.
        const bool naturalRegen = Game::Rules::GetBool(Game::Rules::Id::NaturalHealthRegeneration);

        // MC ServerPlayer.tickRegeneration (from Player.aiStep): on PEACEFUL
        // with natural regeneration, every 20 ticks +1 health (below max)
        // and +1 saturation (below 20 — setSaturation, not clamped to the
        // food level); every 10 ticks +1 food while it is below 20.
        ++m_regenClock;
        if (peaceful && naturalRegen) {
            if (m_regenClock % 20 == 0) {
                if (player.getHealth() < player.getMaxHealth()) player.heal(1.0f);
                if (m_saturationLevel < 20.0f) m_saturationLevel += 1.0f;
            }
            if (m_regenClock % 10 == 0 && needsFood()) m_foodLevel = m_foodLevel + 1;
        }

        // :35-42 — exhaustion drains saturation first, then food.
        if (m_exhaustionLevel > 4.0f) {
            m_exhaustionLevel -= 4.0f;
            if (m_saturationLevel > 0.0f) {
                m_saturationLevel = std::max(m_saturationLevel - 1.0f, 0.0f);
            } else if (!peaceful) {   // MC: difficulty != PEACEFUL
                m_foodLevel = std::max(m_foodLevel - 1, 0);
            }
        }

        // Player.isHurt: 0 < health < getMaxHealth (HEALTH_BOOST raises it).
        const bool isHurt = player.getHealth() > 0.0f && player.getHealth() < player.getMaxHealth();

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
            // :60-68 — starvation, 1 damage every 80 ticks: down to 10
            // health on easy (and peaceful), to 1 on normal, to death on hard.
            ++m_tickTimer;
            if (m_tickTimer >= 80) {
                const float health = player.getHealth();
                if (health > 10.0f || difficulty == 3 || (health > 1.0f && difficulty == 2)) {
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
