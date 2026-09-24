// File: src/common/entity/ai/brain/VillagerAi.hpp
//
// MC net.minecraft.world.entity.ai.behavior.VillagerGoalPackages plus the
// villager's sensors and every behaviour those packages name — the whole of
// Villager.BRAIN_PROVIDER.
//
// The packages are built for the villager's CURRENT profession and age, so a
// villager that takes a job or grows up rebuilds its brain (Villager::
// RefreshBrain), exactly as MC's refreshBrain does:
//
//   CORE    swim, doors, look, panic trigger, wake up, bell reaction, POI
//           validation, walking, job competition, the trading partner,
//           picking up items, acquiring a job / bed / bell, taking a
//           profession from a job site, losing an unused one
//   WORK    (adults with a JOB_SITE) work at the site — restocking there —,
//           stroll around it, farm, show trades, gifts for a hero
//   PLAY    (babies) tag, jumping on beds, following other villagers
//   REST    walk home, sleep in the claimed bed, or find any bed / village
//   MEET    (with a MEETING_POINT) gather at the bell, gossip and trade food
//   IDLE    wander the village, visit villagers and cats, breed
//   PANIC   flee hostiles and whoever hurt it, calm down when safe
//   HIDE    after hearing a bell: run to a house and stay put
//   PRE_RAID / RAID  registered but unreachable — this engine has no raids,
//           and only SetRaidStatus (a raid in progress) enters them
//
// The SCHEDULE is MC 26.3's `gameplay/villager_activity` timeline
// (data/minecraft/timeline/villager_schedule.json): keyed on the day time,
// switched by UpdateActivityFromSchedule at priority 99 in every package.
#pragma once

#include "common/entity/ai/brain/Activity.hpp"

#include <cstdint>

namespace Game {

    class Brain;
    class Villager;

    namespace VillagerAi {

        // Villager.BRAIN_PROVIDER.makeBrain: memories, sensors and the
        // activities for this villager's profession and age.
        void InitBrain(Villager& villager, Brain& brain);

        // The timeline value at this day time — `villager_activity` for an
        // adult, `baby_villager_activity` for a baby.
        Activity ScheduledActivity(bool baby, int64_t dayTime);

    } // namespace VillagerAi
} // namespace Game
