// File: src/client/renderer/entity/EntityLighting.hpp
//
// MC's entity diffuse lighting, baked on the CPU into vertex colour.
//
// MC draws every entity and block-entity model with the rendertype_entity_*
// shaders, which shade each face from its WORLD-space normal against two fixed
// directional lights (light.glsl minecraft_mix_light):
//
//   accum = min(1, (max(0, n·L0) + max(0, n·L1)) * 0.6 + 0.4)
//
// with the LEVEL pair from Lighting.setupLevel, chosen by the dimension's
// cardinal light type (Lighting.java:17-20):
//   DEFAULT  L0 = (0.2, 1, -0.7)  L1 = (-0.2,  1, 0.7)
//   NETHER   L0 = (0.2, 1, -0.7)  L1 = (-0.2, -1, 0.7)   (only the Nether)
// For axis-aligned faces in the default set that is up 1.0, down 0.4,
// north/south 0.74, east/west 0.50 — NOT the block-model face table
// (1.0 / 0.5 / 0.8 / 0.6), and never in model space: a model that turns keeps
// its light fixed in the world.
//
// This engine's model shaders take no normal, so the renderers bake the shade
// per face from the posed normal instead. The result equals MC's per-vertex
// value for the flat cube faces these models are made of.
#pragma once

#include "client/world/ClientLevel.hpp"
#include "common/world/level/DimensionId.hpp"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace Render::EntityLighting {

    enum class LightSet : uint8_t { Default, Nether };

    // DimensionType.cardinalLightType: NETHER for the Nether only.
    inline LightSet ForDimension(Game::DimensionId dimension) {
        return dimension == Game::DimensionId::Nether ? LightSet::Nether : LightSet::Default;
    }

    // The level being drawn right now — portal views bind the far level, so a
    // mob seen through a nether portal is lit the Nether's way, as in MC.
    inline LightSet Current() { return ForDimension(Client::ClientLevels::BoundDimension()); }

    // minecraft_mix_light for a world-space normal (need not be unit length).
    inline float Shade(const glm::vec3& worldNormal, LightSet set) {
        const float len = glm::length(worldNormal);
        if (!(len > 1e-8f)) return 1.0f;
        const glm::vec3 n = worldNormal / len;
        static const glm::vec3 kLight0       = glm::normalize(glm::vec3( 0.2f,  1.0f, -0.7f));
        static const glm::vec3 kLight1       = glm::normalize(glm::vec3(-0.2f,  1.0f,  0.7f));
        static const glm::vec3 kNetherLight1 = glm::normalize(glm::vec3(-0.2f, -1.0f,  0.7f));
        const glm::vec3& l1 = (set == LightSet::Nether) ? kNetherLight1 : kLight1;
        const float a = std::max(0.0f, glm::dot(kLight0, n));
        const float b = std::max(0.0f, glm::dot(l1, n));
        return std::min(1.0f, (a + b) * 0.6f + 0.4f);
    }

    inline uint8_t ShadeByte(const glm::vec3& worldNormal, LightSet set) {
        return static_cast<uint8_t>(Shade(worldNormal, set) * 255.0f);
    }

    // The matrix that carries a model-space normal into world space for a
    // pose `m` (the inverse transpose of its linear part; MC PoseStack keeps
    // the same thing as Pose.normal). A degenerate pose (a part scaled to
    // zero draws nothing anyway) falls back to the linear part itself.
    inline glm::mat3 NormalMatrix(const glm::mat4& m) {
        const glm::mat3 linear(m);
        const float det = glm::determinant(linear);
        if (!(std::abs(det) > 1e-20f)) return linear;
        return glm::transpose(glm::inverse(linear));
    }

} // namespace Render::EntityLighting
