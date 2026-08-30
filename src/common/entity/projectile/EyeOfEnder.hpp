// File: src/common/entity/projectile/EyeOfEnder.hpp
//
// MC net.minecraft.world.entity.projectile.EyeOfEnder — the thrown eye that
// points at the nearest stronghold.
//
// THE THING EVERYONE GETS WRONG ABOUT IT
// --------------------------------------
// The eye does NOT fly to the stronghold. `SignalTo` clamps the target to a
// point 12 blocks away horizontally and 8 blocks up (EyeOfEnder.java:71-82),
// so what the player is actually reading is the DIRECTION of the first 12
// blocks of flight, not the distance. That clamp is also, incidentally, the
// only thing that keeps the trajectory sane when the stronghold is 1500
// blocks away — the raw target would send it nearly horizontal.
//
// A plain Entity in MC. Here it derives Projectile with the mob machinery
// inert, like EvokerFangs — see Projectile.hpp's architecture note.
#pragma once

#include "common/entity/projectile/Projectile.hpp"
#include "common/entity/Item.hpp"

#include <optional>

namespace Game {

    class EyeOfEnder : public Projectile {
    public:
        explicit EyeOfEnder(EntityLevel* level)
            : Projectile(EntityTypeId::EyeOfEnder, level) {}

        // MC EyeOfEnder.signalTo (:71). `target` is the stronghold position;
        // what is stored is the clamped proxy described above.
        //
        // Also rolls `surviveAfterDeath` — MC `random.nextInt(5) > 0`, so the
        // eye survives 4 times in 5 and shatters the fifth.
        void SignalTo(const glm::dvec3& target);

        void Tick() override;

        // MC EyeOfEnder.setItem — the stack the eye drops if it survives.
        void SetItem(const ItemStack& stack);
        const ItemStack& GetItem() const { return m_item; }

        // MC EyeOfEnder is not attackable and takes no damage. Projectile's
        // base already refuses Hurt.
        bool IsAttackable() const override { return false; }

        // MC getLightLevelDependentMagicValue() == 1.0 — the eye is fullbright.
        // No light engine here, so this is documentation for when there is one.

        // Set by Tick when the eye's life runs out. The server reads it to
        // decide between dropping the item and playing the shatter effect,
        // both of which need level access this class does not have.
        enum class Death : uint8_t { None, DropItem, Shatter };
        Death ConsumeDeath();

        // Save/load. Vanilla stores only "Item" for the eye; the rest is
        // engine state that would otherwise restart the flight, so it rides
        // along under engine-private keys the vanilla reader ignores.
        int  GetLife() const { return m_life; }
        void SetLife(int life) { m_life = life; }
        bool SurvivesAfterDeath() const { return m_surviveAfterDeath; }
        void SetSurvivesAfterDeath(bool v) { m_surviveAfterDeath = v; }

    private:
        // MC's three constants (EyeOfEnder.java:23-25).
        static constexpr double kTooFarDistance     = 12.0;
        static constexpr double kTooFarSignalHeight = 8.0;
        static constexpr int    kLifetimeTicks      = 80;   // :99

        std::optional<glm::dvec3> m_target;
        int       m_life = 0;
        bool      m_surviveAfterDeath = false;
        Death     m_pendingDeath = Death::None;
        ItemStack m_item{};
    };

} // namespace Game
