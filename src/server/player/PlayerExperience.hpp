// File: src/server/player/PlayerExperience.hpp
//
// The XP half of MC's Player — experienceLevel / experienceProgress /
// totalExperience plus the arithmetic that moves between them.
//
// Split into its own type for the same reason FoodData is: it is a
// self-contained counter with fiddly, exactly-specified maths that several
// unrelated systems read (furnace payout, anvil cost, enchanting cost), and
// keeping it here means none of them have to reach into ServerPlayer's guts.
//
// Every formula below is verbatim from Player.java — the level curve in
// particular is three different linear segments and is easy to get subtly
// wrong, which shows up as "enchanting costs the wrong number of levels".
#pragma once

#include <algorithm>
#include <cmath>

namespace Server {

    class PlayerExperience {
    public:
        int   Level() const { return m_level; }
        float Progress() const { return m_progress; }
        int   Total() const { return m_total; }

        void SetLevel(int level) { m_level = std::max(0, level); }
        void SetProgress(float p) { m_progress = std::clamp(p, 0.0f, 1.0f); }
        void SetTotal(int total) { m_total = std::max(0, total); }

        // MC Player.getXpNeededForNextLevel (Player.java:1507). Three segments:
        // cheap to 15, steeper to 30, steepest after.
        int XpNeededForNextLevel() const {
            if (m_level >= 30) return 112 + (m_level - 30) * 9;
            if (m_level >= 15) return 37 + (m_level - 15) * 5;
            return 7 + m_level * 2;
        }

        // MC Player.giveExperienceLevels — levels can be handed out or taken
        // away directly (enchanting charges this way).
        void GiveLevels(int amount) {
            m_level = std::max(0, m_level + amount);
            if (m_level == 0) m_progress = 0.0f;
        }

        // MC Player.giveExperiencePoints (Player.java:1452). The two while
        // loops are what carry a partial bar across a level boundary in either
        // direction; a naive "add then divide" gets the leftover wrong.
        void GivePoints(int amount) {
            const int needed = XpNeededForNextLevel();
            if (needed > 0) m_progress += static_cast<float>(amount) / static_cast<float>(needed);
            m_total = std::max(0, m_total + amount);

            while (m_progress < 0.0f) {
                const float remaining = m_progress * static_cast<float>(XpNeededForNextLevel());
                if (m_level > 0) {
                    GiveLevels(-1);
                    m_progress = 1.0f + remaining / static_cast<float>(XpNeededForNextLevel());
                } else {
                    GiveLevels(-1);
                    m_progress = 0.0f;
                }
            }
            while (m_progress >= 1.0f) {
                m_progress = (m_progress - 1.0f) * static_cast<float>(XpNeededForNextLevel());
                GiveLevels(1);
                m_progress /= static_cast<float>(XpNeededForNextLevel());
            }
        }

        // A furnace banks fractional XP per smelt and pays a rounded total when
        // the result is collected — MC AbstractFurnaceBlockEntity
        // .createExperience: floor, then a random chance at the remainder.
        static int RoundBankedExperience(float banked, float randomRoll) {
            int whole = static_cast<int>(std::floor(banked));
            const float fraction = banked - static_cast<float>(whole);
            if (fraction != 0.0f && randomRoll < fraction) ++whole;
            return whole;
        }

    private:
        int   m_level    = 0;
        float m_progress = 0.0f;
        int   m_total    = 0;
    };

} // namespace Server
