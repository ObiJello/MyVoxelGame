// File: src/common/entity/DragonFight.hpp
//
// The dragon fight, as seen from COMMON code. The real controller — MC
// net.minecraft.world.level.dimension.end.EndDragonFight — lives server-side
// (src/server/level/EndDragonFight) because it owns level-shaped things: the
// exit portal blocks, the gateway list, the boss bar's player set, level.dat
// persistence. But the DRAGON and the CRYSTAL are common entities, and MC
// wires them to the fight through exactly these calls (level.getDragonFight()
// → the methods below), so the boundary is an interface on EntityLevel:
// ServerLevelBridge for the End returns the fight, every other level — the
// client mirror included — returns null, which reproduces MC's own "no
// dragon fight outside the End" null.
#pragma once

namespace Game {

    class EnderDragon;
    class EndCrystal;
    class Entity;

    class IDragonFight {
    public:
        virtual ~IDragonFight() = default;

        // MC EndDragonFight.getCrystalsAlive — the count from the periodic
        // spike scan. Feeds the dragon's node-graph gating (outer ring while
        // crystals stand) and the holding pattern's landing/strafe odds.
        virtual int CrystalsAlive() const = 0;

        // MC EndDragonFight.hasPreviouslyKilledDragon — decides the 12,000 vs
        // 500 XP death shower and whether the egg spawns.
        virtual bool HasPreviouslyKilledDragon() const = 0;

        // MC EndDragonFight.updateDragon — health onto the boss bar, the
        // dragon-seen timer reset. Called every dragon AI tick and every
        // death-cinematic tick, as in MC.
        virtual void UpdateDragon(EnderDragon& dragon) = 0;

        // MC EndDragonFight.setDragonKilled — the exit portal activates, a
        // gateway spawns, the egg appears on a first kill.
        virtual void SetDragonKilled(EnderDragon& dragon) = 0;

        // MC EndDragonFight.onCrystalDestroyed(crystal, source). `attacker`
        // is the damage source's entity (the player, or null for e.g. a
        // chained explosion); the fight re-counts crystals, aborts a respawn
        // ritual if one was in progress, and relays to the dragon.
        virtual void OnCrystalDestroyed(EndCrystal& crystal, Entity* attacker) = 0;
    };

} // namespace Game
