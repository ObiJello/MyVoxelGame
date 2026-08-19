// File: src/client/renderer/blockentity/CampfireRenderer.cpp
#include "CampfireRenderer.hpp"
#include "../backend/RenderBackend.hpp"
#include "../viewmodel/HeldItemSpriteMesh.hpp"
#include "common/world/block/entity/BlockEntity.hpp"
#include "common/world/block/entity/CampfireBlockEntity.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/entity/Item.hpp"
#include "client/world/ClientBlockAccess.hpp"
#include "common/core/Log.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <string>
#include <string_view>

namespace Render {

    namespace {

        // Plain textured pass. Unlike ChestRenderer's shader this takes UVs
        // already normalised — HeldItemSpriteMesh emits 0..1 UVs against the
        // item's own sprite texture rather than coordinates into a fixed-size
        // atlas, so there is no divisor to fold in.
        constexpr const char* kVS = R"GLSL(
#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec2 aUV;
layout(location=2) in vec4 aColor;
uniform mat4 uMVP;
out vec2 vUV;
out vec4 vColor;
void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
    vUV = aUV;
    vColor = aColor;
}
)GLSL";

        constexpr const char* kFS = R"GLSL(
#version 330 core
in vec2 vUV;
in vec4 vColor;
out vec4 FragColor;
uniform sampler2D uTex;
void main() {
    vec4 t = texture(uTex, vUV);
    if (t.a < 0.5) discard;
    FragColor = t * vColor;
}
)GLSL";

        // MC Direction.get2DDataValue(): the horizontal ring indexed
        // south, west, north, east. NOT the same order as this engine's
        // `facing` property values (north, east, south, west), so it has to be
        // mapped rather than reused — getting this wrong rotates every
        // campfire's food by a quarter turn.
        int Horizontal2DIndex(std::string_view facing) {
            if (facing == "south") return 0;
            if (facing == "west")  return 1;
            if (facing == "north") return 2;
            if (facing == "east")  return 3;
            return 0;
        }

        // The sprite this stack draws as. Campfire recipes only ever accept
        // 2D food items, so the block-model path HeldItemRenderer also has is
        // deliberately not mirrored here — an item with no sprite is skipped.
        const std::string* SpriteFor(const Game::ItemStack& stack) {
            if (stack.IsEmpty()) return nullptr;
            const Game::Item& item = Game::ItemRegistry::Get(stack.itemId);
            if (item.renderType != Game::ItemRenderType::Sprite) return nullptr;
            if (item.spriteName.empty()) return nullptr;
            return &item.spriteName;
        }

    } // namespace

    bool CampfireRenderer::Initialize() {
        if (!g_renderBackend) return false;
        m_shader = g_renderBackend->CreateShader(kVS, kFS);
        if (m_shader == INVALID_SHADER) {
            Log::Error("[CampfireRenderer] shader compile failed");
            return false;
        }
        return true;
    }

    void CampfireRenderer::Shutdown() {
        if (g_renderBackend && m_shader != INVALID_SHADER) {
            g_renderBackend->DestroyShader(m_shader);
        }
        m_shader = INVALID_SHADER;
        // The sprite meshes are owned by HeldItemSpriteMesh's shared cache, so
        // they are deliberately NOT freed here — the held-item renderer is
        // still using them.
    }

    void CampfireRenderer::Render(const Game::BlockEntity& be,
                                  float /*partialTick*/,
                                  const glm::mat4& proj,
                                  const glm::mat4& view,
                                  const glm::vec3& /*cameraPos*/) {
        if (m_shader == INVALID_SHADER || !g_renderBackend) return;

        const auto* campfire = dynamic_cast<const Game::CampfireBlockEntity*>(&be);
        if (!campfire) return;

        const glm::ivec3 pos = be.GetWorldPos();

        // Facing comes off the BLOCK state, as in MC
        // (CampfireRenderer.extractRenderState reads
        // blockEntity.getBlockState().getValue(CampfireBlock.FACING)), so a
        // campfire picks up a state change with no extra sync.
        int facing2D = 0;
        if (Client::g_clientBlockAccess) {
            const Game::BlockState state =
                Client::g_clientBlockAccess->GetBlockState(pos.x, pos.y, pos.z);
            facing2D = Horizontal2DIndex(state.GetValueByName("facing"));
        }

        PipelineState s;
        s.depthTestEnabled  = true;
        s.depthWriteEnabled = true;
        s.blendEnabled      = false;
        // The extruded sprite emits front, back and side quads, and laying it
        // flat means the camera sees it from either side depending on where it
        // stands — same reason HeldItemRenderer disables culling for sprites.
        s.cullMode          = CullMode::None;
        s.primitiveType     = PrimitiveType::Triangles;

        bool stateSet = false;

        for (int slot = 0; slot < Game::CampfireBlockEntity::SLOT_COUNT; ++slot) {
            const std::string* sprite = SpriteFor(campfire->GetItem(slot));
            if (!sprite) continue;

            const auto* entry = HeldItemSpriteMesh::GetOrBuild(*sprite);
            if (!entry || entry->mesh == INVALID_MESH) continue;

            // MC CampfireRenderer.submit, read bottom-up (last line applied to
            // the vertex first):
            //
            //   translate(0.5, 0.44921875, 0.5)   sit on top of the log bed
            //   rotateY(-direction.toYRot())      one quarter turn per slot
            //   rotateX(+90)                      lay the sprite face-up
            //   translate(-0.3125, -0.3125, 0)    push off-centre, so the four
            //                                     slots land in four quadrants
            //   scale(0.375)                      MC's SIZE
            //
            // The remaining three stand in for what MC's `submit` does before
            // any of the above, since we draw the sprite mesh directly rather
            // than going through ItemStackRenderState:
            //
            //   rotateY(180)                     ItemTransform.apply for the
            //   translate(-0.5, -0.5, 0)         FIXED display context, which
            //                                    item/generated declares as
            //                                    rotation [0,180,0] followed by
            //                                    a translate(-0.5,-0.5,-0.5)
            //   scale(1/16)                      ours: 0..16 pixel space -> unit
            //
            // The 180 is easy to miss — ItemStackRenderState.java:238 applies
            // the display transform INSIDE submit, so it never appears in
            // CampfireRenderer itself. Dropping it mirrors every food sprite
            // left-to-right, which reads as the item facing the wrong way.
            //
            // Z translates by 0 rather than -0.5 because HeldItemSpriteMesh
            // already centres its extrusion on Z=0, where MC's model sits at
            // z 7.5..8.5 and needs the shift.
            const int dirIndex = (slot + facing2D) % 4;
            // MC's angle is -toYRot(), and toYRot() for from2DDataValue(k) is
            // k*90 degrees. Same sign convention ChestRenderer already uses.
            const float yRot = glm::radians(-90.0f * static_cast<float>(dirIndex));

            glm::mat4 model = glm::translate(glm::mat4(1.0f),
                glm::vec3(pos) + glm::vec3(0.5f, 0.44921875f, 0.5f));
            model = glm::rotate(model, yRot, glm::vec3(0.0f, 1.0f, 0.0f));
            model = glm::rotate(model, glm::radians(90.0f), glm::vec3(1.0f, 0.0f, 0.0f));
            model = glm::translate(model, glm::vec3(-0.3125f, -0.3125f, 0.0f));
            model = glm::scale(model, glm::vec3(0.375f));
            model = glm::rotate(model, glm::radians(180.0f), glm::vec3(0.0f, 1.0f, 0.0f));
            model = glm::translate(model, glm::vec3(-0.5f, -0.5f, 0.0f));
            model = glm::scale(model, glm::vec3(1.0f / 16.0f));

            if (!stateSet) {
                g_renderBackend->SetPipelineState(s);
                g_renderBackend->BindShader(m_shader);
                stateSet = true;
            }
            g_renderBackend->BindTexture(entry->texture, 0);
            g_renderBackend->SetUniformMat4(m_shader, "uMVP", proj * view * model);
            g_renderBackend->DrawIndexed(entry->mesh, entry->indexCount);
        }

        if (stateSet) g_renderBackend->UnbindMesh();
    }

} // namespace Render
