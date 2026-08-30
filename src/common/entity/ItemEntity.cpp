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

    void ItemEntity::TickMovement(const PhysicsContext& context,
                                  JavaRandom* serverRng) {
        ++tickCount;

        if (pickupDelay > 0) --pickupDelay;

        // ── Buoyancy or gravity (ItemEntity.tick's fluid branch) ───────────
        // MC: `isInWater() && getFluidHeight(WATER) > FLOAT_HEIGHT` floats,
        // lava likewise, otherwise applyGravity. setFluidMovement drags the
        // horizontal axes and feeds a small upward acceleration capped at
        // 0.06/tick — no gravity while floating, which is the whole trick.
        const AABB box = GetAABB();
        const double waterHeight = FluidHeightAbove(box, /*lava=*/false, context);
        if (waterHeight > kFloatHeight) {
            vel.x *= kWaterDrag;
            if (vel.y < kFluidMaxRise) vel.y += kFluidBuoyancy;
            vel.z *= kWaterDrag;
        } else if (FluidHeightAbove(box, /*lava=*/true, context) > kFloatHeight) {
            vel.x *= kLavaDrag;
            if (vel.y < kFluidMaxRise) vel.y += kFluidBuoyancy;
            vel.z *= kLavaDrag;
        } else {
            vel.y -= kGravity;
        }

        // ── Stuck-in-block escape (server side of ItemEntity.tick) ─────────
        // `noPhysics = !level.noCollision(this, box.deflate(1.0E-7))`, and a
        // stuck entity gets its velocity pointed at the nearest open
        // neighbour. The client forces noPhysics false instead — its copy
        // keeps colliding and follows the server's corrections out.
        if (serverRng) {
            AABB deflated = box;
            deflated.min += glm::vec3(1.0e-7f);
            deflated.max -= glm::vec3(1.0e-7f);
            noPhysics = CollidesAt(deflated, context);
            if (noPhysics) {
                // MC aims at the box's vertical CENTRE, not the feet — the
                // cell containing the feet can be the block below.
                const glm::dvec3 center(pos.x, pos.y + kHeight * 0.5, pos.z);
                const float speed = serverRng->NextFloat() * 0.2f + 0.1f;
                EscapeTowardsClosestSpace(center, speed, vel, context);
            }
        } else {
            noPhysics = false;
        }

        // Sleep optimisation (ItemEntity.tick): an item resting still only
        // integrates on one tick in four. Skipping the move for settled items
        // is most of the cost of a large pile. The `id` term staggers entities
        // against each other so a whole pile doesn't wake on the same tick.
        const double horizSpeedSq = vel.x * vel.x + vel.z * vel.z;
        const bool mustMove = !onGround
                           || horizSpeedSq > kSleepSpeedSqEpsilon
                           || ((tickCount + id) % 4) == 0;

        if (mustMove) {
            if (noPhysics) {
                // MC Entity.move with noPhysics set: position integrates
                // directly, no collision resolution at all. Anything else
                // would cancel the escape velocity against the very block the
                // entity is escaping from.
                pos += vel;
                onGround = false;
            } else {
                const MoveResult move = MoveAABB(pos, vel, HalfExtents(), context);
                onGround = move.onGround;
            }

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

    bool ItemEntity::Tick(const PhysicsContext& context, JavaRandom& rng) {
        // An emptied stack (fully picked up, fully merged away) is a dead
        // entity — MC discards before doing any other work.
        if (stack.IsEmpty()) return false;

        const glm::dvec3 oldVel = vel;

        TickMovement(context, &rng);

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
