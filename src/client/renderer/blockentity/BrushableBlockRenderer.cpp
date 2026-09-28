// File: src/client/renderer/blockentity/BrushableBlockRenderer.cpp
#include "BrushableBlockRenderer.hpp"
#include "common/core/Profiling_Tracy.hpp"
#include "../backend/RenderBackend.hpp"
#include "../core/RenderOrigin.hpp"
#include "../viewmodel/HeldItemSpriteMesh.hpp"
#include "BlockEntityShader.hpp"
#include "common/world/block/entity/BrushableBlockEntity.hpp"
#include "common/world/block/BlockState.hpp"
#include "common/entity/Item.hpp"
#include "client/world/ClientBlockAccess.hpp"
#include "common/core/Log.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <string>

namespace Render {

    namespace {

        // Direction.relative(1) for a 3D data value.
        glm::ivec3 Step(int direction) {
            switch (direction) {
                case 0: return { 0, -1,  0 };
                case 1: return { 0,  1,  0 };
                case 2: return { 0,  0, -1 };
                case 3: return { 0,  0,  1 };
                case 4: return { -1, 0,  0 };
                case 5: return { 1,  0,  0 };
                default: return { 0, 1, 0 };
            }
        }

        // MC BrushableBlockRenderer.translations(direction, completionState).
        glm::vec3 Translations(int direction, int completionState) {
            glm::vec3 t(0.5f, 0.0f, 0.5f);
            const float completionOffset = static_cast<float>(completionState) / 10.0f * 0.75f;
            switch (direction) {
                case 5: t.x = 0.73f + completionOffset; break;    // EAST
                case 4: t.x = 0.25f - completionOffset; break;    // WEST
                case 1: t.y = 0.25f + completionOffset; break;    // UP
                case 0: t.y = -0.23f - completionOffset; break;   // DOWN
                case 2: t.z = 0.25f - completionOffset; break;    // NORTH
                case 3: t.z = 0.73f + completionOffset; break;    // SOUTH
                default: break;
            }
            return t;
        }

    } // namespace

    bool BrushableBlockRenderer::Initialize() {
        if (!g_renderBackend) return false;
        m_shader = BlockEntityShader::Create();
        if (m_shader == INVALID_SHADER) {
            Log::Error("[BrushableBlockRenderer] shader compile failed");
            return false;
        }
        return true;
    }

    void BrushableBlockRenderer::Shutdown() {
        if (g_renderBackend && m_shader != INVALID_SHADER) g_renderBackend->DestroyShader(m_shader);
        m_shader = INVALID_SHADER;
        // The sprite meshes belong to HeldItemSpriteMesh's shared cache.
    }

    void BrushableBlockRenderer::Render(const Game::BlockEntity& be,
                                        float /*partialTick*/,
                                        const glm::mat4& proj,
                                        const glm::mat4& view,
                                        const glm::vec3& cameraPos) {
        PROFILE_ZONE_N("BE.Brushable");
        if (m_shader == INVALID_SHADER || !g_renderBackend || !Client::g_clientBlockAccess) return;

        const auto* brushable = dynamic_cast<const Game::BrushableBlockEntity*>(&be);
        if (!brushable) return;
        const int hitDirection = brushable->GetHitDirection();
        const Game::ItemStack& stack = brushable->GetItem();
        if (hitDirection < 0 || stack.IsEmpty()) return;

        const glm::ivec3 pos = be.GetWorldPos();
        const Game::BlockState state = Client::g_clientBlockAccess->GetBlockState(pos.x, pos.y, pos.z);
        const int dustProgress = state.GetIndex(Game::PropertyId::DUSTED);
        if (dustProgress <= 0) return;

        // The item as the FIXED display context draws a flat sprite; a find
        // with no sprite (none of the archaeology tables has one) is skipped.
        const Game::Item& item = Game::ItemRegistry::Get(stack.itemId);
        if (item.renderType != Game::ItemRenderType::Sprite || item.spriteName.empty()) return;
        const auto* entry = HeldItemSpriteMesh::GetOrBuild(item.spriteName);
        if (!entry || entry->mesh == INVALID_MESH) return;

        // MC submit, read top-down (the last transform touches the vertex
        // first):
        //   translate(0, 0.5, 0) · translate(translations(dir, dust))
        //   · rotY(75) · rotY((eastWest ? 90 : 0) + 11) · scale(0.5)
        // then the item's FIXED transform, as CampfireRenderer spells out:
        //   rotY(180) · translate(-0.5, -0.5, 0) · scale(1/16)
        // (HeldItemSpriteMesh centres its extrusion on z = 0).
        const bool eastWest = hitDirection == 4 || hitDirection == 5;
        glm::mat4 model = glm::translate(glm::mat4(1.0f), Render::ToRender(glm::dvec3(pos)));
        model = glm::translate(model, glm::vec3(0.0f, 0.5f, 0.0f));
        model = glm::translate(model, Translations(hitDirection, dustProgress));
        model = glm::rotate(model, glm::radians(75.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        model = glm::rotate(model, glm::radians(static_cast<float>((eastWest ? 90 : 0) + 11)),
                            glm::vec3(0.0f, 1.0f, 0.0f));
        model = glm::scale(model, glm::vec3(0.5f));
        model = glm::rotate(model, glm::radians(180.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        model = glm::translate(model, glm::vec3(-0.5f, -0.5f, 0.0f));
        model = glm::scale(model, glm::vec3(1.0f / 16.0f));

        PipelineState s;
        s.depthTestEnabled  = true;
        s.depthWriteEnabled = true;
        s.blendEnabled      = false;
        s.cullMode          = CullMode::None;
        s.primitiveType     = PrimitiveType::Triangles;
        g_renderBackend->SetPipelineState(s);
        g_renderBackend->BindShader(m_shader);
        g_renderBackend->SetUniformFloat(m_shader, "uAlphaTest", 0.5f);
        g_renderBackend->BindTexture(entry->texture, 0);
        g_renderBackend->SetUniformMat4(m_shader, "uMVP", proj * view * model);
        // Lit by the cell in front of the brushed face (MC reads the light
        // at blockPos.relative(hitDirection)): the block's own cell is solid
        // and dark.
        BlockEntityShader::ApplyWorld(m_shader, model, cameraPos, pos + Step(hitDirection));
        g_renderBackend->DrawIndexed(entry->mesh, entry->indexCount);
        g_renderBackend->UnbindMesh();
    }

} // namespace Render
