// File: src/common/entity/ItemEntity.cpp
#include "ItemEntity.hpp"

#include <algorithm>
#include <cmath>

namespace Game {

    bool CanMergeItemEntities(const ItemStack& a, const ItemStack& b) {
        // ItemEntity.areMergable. The overflow check comes FIRST and is a hard
        // refusal, not a clamp — MC does not top one stack up and leave the
        // remainder behind.
        if (a.IsEmpty() || b.IsEmpty()) return false;
        const int maxStack = ItemRegistry::Get(b.itemId).maxStackSize;
        if (a.count + b.count > maxStack) return false;
        // Components must match too, or an enchanted pickaxe would silently
        // fuse with a plain one.
        return IsSameItemSameComponents(a, b);
    }

    bool ItemEntity::IsMergable() const {
        if (stack.IsEmpty()) return false;
        if (age >= kLifetimeTicks) return false;
        return stack.count < ItemRegistry::Get(stack.itemId).maxStackSize;
    }

    void ItemEntity::TickMovement(const PhysicsContext& context) {
        ++tickCount;

        if (pickupDelay > 0) --pickupDelay;

        // Gravity. MC skips this entirely while the entity is in a fluid deep
        // enough to float in and applies buoyancy instead; this engine has no
        // fluid-height query for entities, so items sink through water. That is
        // a known simplification, not an oversight — the honest fix is a
        // getFluidHeight equivalent, which does not exist yet.
        vel.y -= kGravity;

        // Sleep optimisation (ItemEntity.tick): an item resting still only
        // integrates on one tick in four. Skipping the move for settled items
        // is most of the cost of a large pile. The `id` term staggers entities
        // against each other so a whole pile doesn't wake on the same tick.
        const double horizSpeedSq = vel.x * vel.x + vel.z * vel.z;
        const bool mustMove = !onGround
                           || horizSpeedSq > kSleepSpeedSqEpsilon
                           || ((tickCount + id) % 4) == 0;

        if (mustMove) {
            const MoveResult move = MoveAABB(pos, vel, HalfExtents(), context);
            onGround = move.onGround;

            // Drag AFTER the move — see the header note. Horizontal drag picks
            // up the ground multiplier once we're resting on something.
            const double horizDrag = onGround ? kGroundDrag : kAirDrag;
            vel.x *= horizDrag;
            vel.y *= kAirDrag;
            vel.z *= horizDrag;

            // Landing bounce: flip and halve whatever downward motion is left.
            // MoveAABB has already zeroed vel.y if the entity actually struck
            // the ground this step, so this only fires for an entity that was
            // already grounded and still carries downward velocity.
            if (onGround && vel.y < 0.0) {
                vel.y *= kBounceDamping;
            }
        }
    }

    bool ItemEntity::Tick(const PhysicsContext& context) {
        // An emptied stack (fully picked up, fully merged away) is a dead
        // entity — MC discards before doing any other work.
        if (stack.IsEmpty()) return false;

        const glm::dvec3 oldVel = vel;

        TickMovement(context);

        if (age < kLifetimeTicks) ++age;

        // Resend when the motion changed materially — MC ItemEntity.tick's
        // `getDeltaMovement().subtract(oldMovement).lengthSqr() > 0.01`. This
        // catches landings, bounces and merges, i.e. the moments the client's
        // own simulation could not have predicted.
        //
        // Note it deliberately does NOT fire during free fall: gravity changes
        // velocity by only 0.04/tick (|Δv|² = 0.0016). MC relies on the client
        // simulating that itself, which ours does too — adding an extra
        // "crossed a block boundary" resend here would just fight the client's
        // prediction with corrections it does not need.
        const glm::dvec3 dv = vel - oldVel;
        if (glm::dot(dv, dv) > 0.01) {
            needsSync = true;
        }

        // Despawn.
        if (age >= kLifetimeTicks) return false;

        return true;
    }

} // namespace Game
