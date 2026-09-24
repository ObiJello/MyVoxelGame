// File: src/client/renderer/blockentity/BlockEntityShader.hpp
//
// The one shader every block-entity renderer draws with — chest, shulker
// box, bed, skull, campfire food, sign board and text, and their item forms
// (shaders/blockentity.{vert,frag} and the _vk twins). MC gives block
// entities the lightmap and the fog like any entity (their render types run
// entity.fsh), so this shader fogs toward the frame's fog colour and dims by
// the sky light exactly as the terrain does: under Blindness a chest four
// blocks away is as black as the floor it stands on.
//
// Per draw it needs the mesh's model matrix into RENDER space
// (uLocalToRender — on Vulkan three push-constant rows, never a UBO slot),
// its light (uBlockEntityLight — a push constant too) and the fog
// (EntityEnvironment::ApplyWorld), which on Vulkan only costs a UBO slot
// when the frame's fog actually changes.
#pragma once

#include "BlockEntityRenderer.hpp"
#include "client/renderer/backend/RenderBackend.hpp"
#include "client/renderer/environment/EntityEnvironment.hpp"
#include "client/renderer/core/RenderOrigin.hpp"

#include <cmath>

namespace Render::BlockEntityShader {

    inline ShaderHandle Create() {
        return EntityEnvironment::CreateShader("shaders/blockentity.vert", "shaders/blockentity.frag");
    }

    // The draw's light — the lightmap colour it is multiplied by: on
    // Vulkan a push constant, so switching it between draws (a sign's
    // glowing text) is free. The scalar form is a grey (the Aurelith
    // pulses' hand-driven intensities).
    inline void SetLight(ShaderHandle shader, const glm::vec3& light) {
        g_renderBackend->SetUniformVec3(shader, "uBlockEntityLight", light);
    }
    inline void SetLight(ShaderHandle shader, float light) {
        SetLight(shader, glm::vec3(light));
    }

    // MC's block-entity light: LevelRenderer.getLightCoords at the block
    // entity's own cell (EntityEnvironment::LevelLightCoordsAt) — packed.
    inline int PackedLightAt(const glm::ivec3& blockPos) {
        return EntityEnvironment::LevelLightCoordsAt(blockPos);
    }
    inline glm::vec3 LightAt(const glm::ivec3& blockPos) {
        return EntityEnvironment::LightColor(PackedLightAt(blockPos));
    }

    // A block entity in the level: `localToRender` places the mesh (the
    // model part of its MVP), `cameraWorld` is the view's eye, `blockPos`
    // the block entity's cell, whose light it is drawn with (unless the
    // caller sets its own afterwards).
    inline void ApplyWorld(ShaderHandle shader, const glm::mat4& localToRender,
                           const glm::vec3& cameraWorld, const glm::ivec3& blockPos) {
        g_renderBackend->SetUniformMat4(shader, "uLocalToRender", localToRender);
        EntityEnvironment::ApplyWorld(shader, glm::dvec3(cameraWorld));
        SetLight(shader, LightAt(blockPos));
    }

    // The same without a cell: the cell holding the mesh's origin (the
    // model's translation, back in world space). Renderers that know their
    // block position pass it instead.
    inline void ApplyWorld(ShaderHandle shader, const glm::mat4& localToRender,
                           const glm::vec3& cameraWorld) {
        const glm::dvec3 world = Render::ToWorld(glm::vec3(localToRender[3]));
        ApplyWorld(shader, localToRender, cameraWorld,
                   glm::ivec3(static_cast<int>(std::floor(world.x)),
                              static_cast<int>(std::floor(world.y)),
                              static_cast<int>(std::floor(world.z))));
    }

    // An item form — see BEWLRLight. `meshToItem` is whatever the renderer
    // puts between the caller's matrix and the mesh (a part's pose), so the
    // fog sees the same placement the MVP does.
    inline void ApplyItem(ShaderHandle shader, const BEWLRLight& light,
                          const glm::mat4& meshToItem = glm::mat4(1.0f)) {
        g_renderBackend->SetUniformMat4(shader, "uLocalToRender", light.localToRender * meshToItem);
        if (light.world) {
            EntityEnvironment::ApplyWorld(shader, light.cameraWorld);
        } else {
            EntityEnvironment::ApplyUnfogged(shader, 1.0f);
        }
        SetLight(shader, light.light);
    }

} // namespace Render::BlockEntityShader
