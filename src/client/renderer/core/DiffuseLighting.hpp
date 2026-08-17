// File: src/client/renderer/core/DiffuseLighting.hpp
//
// MC's entity/item diffuse lighting — the "two fixed lights" that give a
// dropped item, a held item and a mob their 3D form independently of the
// block/sky lightmap.
//
// This is NOT the same thing as Game::DirectionalShade (BlockModel.hpp). MC
// runs two different models and the numbers differ:
//
//   • BLOCKS in the world go through ModelBlockRenderer, which multiplies each
//     face by ClientLevel.getShade(direction) — the flat 1.0 / 0.8 / 0.6 / 0.5
//     table. That is Game::DirectionalShade.
//
//   • ITEMS AND ENTITIES go through the entity render types, whose shader runs
//     `minecraft_mix_light` (light.glsl) against the two directional lights
//     Lighting.updateLevel installs — DIFFUSE_LIGHT_0/1 below. That is what
//     this header implements.
//
// Using the block table for an item is why an extruded sprite lying on the
// ground read as flat: nothing in that path ever applied either model.
#pragma once

#include <glm/glm.hpp>
#include <algorithm>

namespace Render {

    // com.mojang.blaze3d.platform.Lighting — DIFFUSE_LIGHT_0 / DIFFUSE_LIGHT_1,
    // the LEVEL entry (Lighting.updateLevel, CardinalLightType.DEFAULT). The
    // Nether swaps the second light's Y sign; no dimension work here yet.
    //
    // They are WORLD-space directions, which is the one place this port
    // diverges: MC dots them with the vertex's world normal, so a dropped item
    // shimmers as it spins. Baking the shade into a cached mesh fixes the
    // lighting to the MODEL instead, so the item keeps its bright top and dark
    // underside but does not shimmer. Matching MC exactly would need a normal
    // in the vertex format and the light in the shader.
    inline const glm::vec3 kLevelDiffuse0 = glm::normalize(glm::vec3( 0.2f, 1.0f, -0.7f));
    inline const glm::vec3 kLevelDiffuse1 = glm::normalize(glm::vec3(-0.2f, 1.0f,  0.7f));

    // light.glsl `minecraft_mix_light`, verbatim:
    //
    //   lightAccum = min(1, (max(0, dot(L0, n)) + max(0, dot(L1, n))) * 0.6 + 0.4)
    //
    // The 0.4 floor is why an item's underside is dim rather than black, and
    // the min() is why the top saturates at full brightness.
    inline float DiffuseShade(const glm::vec3& normal) {
        const float l0 = std::max(0.0f, glm::dot(kLevelDiffuse0, normal));
        const float l1 = std::max(0.0f, glm::dot(kLevelDiffuse1, normal));
        return std::min(1.0f, (l0 + l1) * 0.6f + 0.4f);
    }

    // The six axis-aligned results, so a mesh builder that already knows which
    // way a quad faces pays nothing:
    //
    //   +Y  1.000     -Y  0.400
    //   +Z  0.740     -Z  0.740
    //   +X  0.497     -X  0.497
    //
    // Both lights share a Y component and mirror in X/Z, which is why the
    // opposing horizontal faces come out equal — one light lands on each.
    inline float DiffuseShadeUp()    { return DiffuseShade({ 0.0f,  1.0f,  0.0f}); }
    inline float DiffuseShadeDown()  { return DiffuseShade({ 0.0f, -1.0f,  0.0f}); }
    inline float DiffuseShadeZ()     { return DiffuseShade({ 0.0f,  0.0f,  1.0f}); }
    inline float DiffuseShadeX()     { return DiffuseShade({ 1.0f,  0.0f,  0.0f}); }

    // Multiply a vertex colour by a shade factor. Straight multiply in gamma
    // space — the same thing the chunk mesher does for AO and face shade, so
    // items and terrain agree about what "half as bright" means.
    inline void ApplyShade(float shade, uint8_t& r, uint8_t& g, uint8_t& b) {
        const auto scale = [shade](uint8_t c) {
            return static_cast<uint8_t>(
                std::clamp(static_cast<int>(static_cast<float>(c) * shade + 0.5f), 0, 255));
        };
        r = scale(r);
        g = scale(g);
        b = scale(b);
    }

} // namespace Render
