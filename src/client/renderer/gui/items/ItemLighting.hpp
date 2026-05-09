// File: src/client/renderer/gui/items/ItemLighting.hpp
//
// MC's ITEMS_3D Lambertian item-icon shading. Used for ALL block icons in the
// inventory, plus 3D items rendered through SpecialModelRenderer (chest, sign,
// banner, head, bed, shulker box). Matches MC's `Lighting.ITEMS_3D` (pre-baked
// light directions) + the entity vertex shader's `minecraft_mix_light` formula.
//
// MC source references:
//   • com/mojang/blaze3d/platform/Lighting.java       — DIFFUSE_LIGHT_0/1, item3DPose
//   • com/mojang/blaze3d/vertex/VertexConsumer.java   — putBulkData uses pose.transformNormal
//   • core_shaders/lib/light.glsl                     — minecraft_mix_light formula
//
// Per-face shade computation:
//   N_view = scale(1, -1, 1) * Rx(displayRotXDeg°) * Ry(displayRotYDeg°) * face.normal
//   shade  = min(1, (max(0, dot(L0, N_view)) + max(0, dot(L1, N_view))) * 0.6 + 0.4)
//
// Standard block GUI rotation = (30°, 225°, 0°)   (template_block.json / block.json display.gui)
// Chest GUI rotation           = (30°,  45°, 0°)   (template_chest.json display.gui)
#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace Render::ItemLighting {

    struct Lights { glm::vec3 L0; glm::vec3 L1; };

    // ITEMS_3D pre-baked lights. Mirrors Lighting.java line 32-33:
    //   item3DPose = scaling(1,-1,1) * rotateYXZ(1.0821, 3.2375, 0) * rotateYXZ(-PI/8, 2.3561, 0)
    //   Light0_view = item3DPose.transformDirection(normalize(0.2, 1, -0.7))
    //   Light1_view = item3DPose.transformDirection(normalize(-0.2, 1, 0.7))
    inline const Lights& MCItems3DLights() {
        static Lights lights = []{
            glm::mat4 m(1.0f);
            m = glm::scale (m, glm::vec3(1.0f, -1.0f, 1.0f));
            m = glm::rotate(m, 1.0821041f,                 glm::vec3(0, 1, 0));
            m = glm::rotate(m, 3.2375858f,                 glm::vec3(1, 0, 0));
            m = glm::rotate(m, -3.14159265358979323846f / 8.0f, glm::vec3(0, 1, 0));
            m = glm::rotate(m, 2.3561945f,                 glm::vec3(1, 0, 0));
            glm::vec3 L0_in = glm::normalize(glm::vec3( 0.2f, 1.0f, -0.7f));
            glm::vec3 L1_in = glm::normalize(glm::vec3(-0.2f, 1.0f,  0.7f));
            Lights out;
            out.L0 = glm::normalize(glm::vec3(m * glm::vec4(L0_in, 0.0f)));
            out.L1 = glm::normalize(glm::vec3(m * glm::vec4(L1_in, 0.0f)));
            return out;
        }();
        return lights;
    }

    // Build the normal-space transform for an inventory item with display rotation
    // (rotX°, rotY°, 0°). Translates and uniform scales drop out — what's left is:
    //   scale(1, -1, 1) * Rx(rotXDeg°) * Ry(rotYDeg°)
    // The scale(1,-1,1) comes from GuiRenderer.renderItemToAtlas's scale(size,-size,size).
    inline glm::mat3 BuildNormalMatrix(float displayRotXDeg, float displayRotYDeg) {
        glm::mat4 m(1.0f);
        m = glm::scale (m, glm::vec3(1.0f, -1.0f, 1.0f));
        m = glm::rotate(m, glm::radians(displayRotXDeg), glm::vec3(1, 0, 0));
        m = glm::rotate(m, glm::radians(displayRotYDeg), glm::vec3(0, 1, 0));
        return glm::mat3(m);
    }

    // Compute MC's ITEMS_3D shade factor for a face whose model-space normal is
    // `modelNormal`, in an item rendered with GUI display rotation (rotX, rotY, 0).
    // Returns the shade multiplier in [0.4, 1.0] — mirrors `minecraft_mix_light`.
    inline float ComputeShade(glm::vec3 modelNormal,
                              float displayRotXDeg, float displayRotYDeg) {
        glm::mat3 nm = BuildNormalMatrix(displayRotXDeg, displayRotYDeg);
        glm::vec3 N  = glm::normalize(nm * modelNormal);
        const Lights& L = MCItems3DLights();
        float d0 = std::max(0.0f, glm::dot(L.L0, N));
        float d1 = std::max(0.0f, glm::dot(L.L1, N));
        return std::min(1.0f, (d0 + d1) * 0.6f + 0.4f);
    }

    // 0xFFXXXXXX color where each RGB channel is shade*255. Suitable as the per-vertex
    // tint passed to a textured GUI quad (texture color * tint = final color).
    inline uint32_t ShadeAsColor(float shade) {
        uint8_t v = static_cast<uint8_t>(std::clamp(shade, 0.0f, 1.0f) * 255.0f + 0.5f);
        return 0xFF000000u | (uint32_t(v) << 16) | (uint32_t(v) << 8) | uint32_t(v);
    }

    // Multiply two ARGB colors channel-wise (alpha = a.alpha). For tinting a face that
    // ALREADY has a tint (e.g. grass top biome tint) by a shade factor, do
    // MultiplyColor(grassTint, ShadeAsColor(shade)).
    inline uint32_t MultiplyColor(uint32_t a, uint32_t b) {
        auto m = [](uint32_t x, uint32_t y) -> uint8_t {
            return static_cast<uint8_t>((x * y + 127u) / 255u);
        };
        uint8_t aa = (a >> 24) & 0xFF;
        uint8_t ar = m((a >> 16) & 0xFF, (b >> 16) & 0xFF);
        uint8_t ag = m((a >>  8) & 0xFF, (b >>  8) & 0xFF);
        uint8_t ab = m((a      ) & 0xFF, (b      ) & 0xFF);
        return (uint32_t(aa) << 24) | (uint32_t(ar) << 16) | (uint32_t(ag) << 8) | uint32_t(ab);
    }

} // namespace Render::ItemLighting
