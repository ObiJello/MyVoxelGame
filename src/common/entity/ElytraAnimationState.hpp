// File: src/common/entity/ElytraAnimationState.hpp
//
// MC net.minecraft.world.entity.ElytraAnimationState — the elytra wings'
// three angles, eased 30% a tick toward a target: spread and swept back by
// the dive while gliding (isFallFlying), opened a little while crouching,
// folded otherwise. WingsLayer / ElytraModel read them (partial-tick lerped)
// for every body that wears an elytra. Client-side animation state: the
// local player and each remote player's copy tick their own.
#pragma once

#include "common/entity/GeneratedItemList.hpp"
#include "common/entity/Item.hpp"

#include <glm/glm.hpp>
#include <cmath>
#include <cstdint>

namespace Game {

    struct ElytraAnimationState {
        // MC DEFAULT_X_ROT / DEFAULT_Z_ROT.
        static constexpr float kDefaultXRot = 0.2617994f;
        static constexpr float kDefaultZRot = -0.2617994f;

        float rotX = 0.0f, rotY = 0.0f, rotZ = 0.0f;
        float rotXOld = 0.0f, rotYOld = 0.0f, rotZOld = 0.0f;

        // MC tick(): `movement` is the body's delta movement (any units —
        // only its direction is read).
        void Tick(bool fallFlying, bool crouching, const glm::dvec3& movement) {
            rotXOld = rotX;
            rotYOld = rotY;
            rotZOld = rotZ;
            float targetX, targetZ, targetY;
            if (fallFlying) {
                float ratio = 1.0f;
                if (movement.y < 0.0) {
                    const double len = std::sqrt(movement.x * movement.x + movement.y * movement.y +
                                                 movement.z * movement.z);
                    // Vec3.normalize: a vector shorter than 1e-5 is zero.
                    const double ny = len < 1.0e-5 ? 0.0 : movement.y / len;
                    ratio = 1.0f - static_cast<float>(std::pow(-ny, 1.5));
                }
                targetX = kDefaultXRot + ratio * (0.34906584f - kDefaultXRot);
                targetZ = kDefaultZRot + ratio * (-1.5707964f - kDefaultZRot);
                targetY = 0.0f;
            } else if (crouching) {
                targetX = 0.6981317f;
                targetZ = -0.7853982f;
                targetY = 0.08726646f;
            } else {
                targetX = kDefaultXRot;
                targetZ = kDefaultZRot;
                targetY = 0.0f;
            }
            rotX += (targetX - rotX) * 0.3f;
            rotY += (targetY - rotY) * 0.3f;
            rotZ += (targetZ - rotZ) * 0.3f;
        }

        float RotX(float partialTick) const { return rotXOld + (rotX - rotXOld) * partialTick; }
        float RotY(float partialTick) const { return rotYOld + (rotY - rotYOld) * partialTick; }
        float RotZ(float partialTick) const { return rotZOld + (rotZ - rotZOld) * partialTick; }
    };

    // WingsLayer's test on the chest stack — an EQUIPPABLE item whose asset
    // has a WINGS layer, which in vanilla is the elytra alone (broken or
    // not) — and ItemStack.hasFoil for the glint. The PlayerUpdateS2C
    // elytraFlags bits.
    constexpr uint8_t kElytraWorn  = 0x01;
    constexpr uint8_t kElytraGlint = 0x02;
    inline uint8_t ElytraLayerFlags(const ItemStack& chest) {
        if (chest.IsEmpty() || chest.itemId != Items::Elytra) return 0;
        return static_cast<uint8_t>(kElytraWorn | (chest.HasFoil() ? kElytraGlint : 0));
    }

} // namespace Game
