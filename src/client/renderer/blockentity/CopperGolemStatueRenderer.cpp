// File: src/client/renderer/blockentity/CopperGolemStatueRenderer.cpp
#include "CopperGolemStatueRenderer.hpp"
#include "common/core/Profiling_Tracy.hpp"
#include "../core/RenderOrigin.hpp"
#include "BlockEntityShader.hpp"
#include "common/world/block/entity/BlockEntity.hpp"
#include <algorithm>
#include <vector>
#include "common/world/block/BlockState.hpp"
#include "client/world/ClientBlockAccess.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <string_view>

namespace Render {

    namespace {

        constexpr float kPi = 3.1415927f;

        struct PartDef {
            int parent;          // -1 = root
            glm::vec3 offset;    // PartPose offset, pixels
            glm::vec3 rot;       // PartPose rotation (x, y, z), radians
        };
        struct CubeDef {
            int part;
            float u, v;          // texOffs
            glm::vec3 origin;    // addBox origin, pixels
            glm::vec3 size;
            float grow;          // CubeDeformation
        };
        struct PoseDef {
            std::vector<PartDef> parts;
            std::vector<CubeDef> cubes;
        };

        // CopperGolemModel.createBodyLayer / createSittingPoseBodyLayer /
        // createRunningPoseBodyLayer / createStarPoseBodyLayer, in the
        // blockstate's pose order (standing, sitting, running, star).
        const PoseDef& Pose(int pose) {
            static const PoseDef kStanding = {
                { {-1, {0, -5, 0}, {}},            // 0 body
                  { 0, {0, -6, 0}, {}},            // 1 head
                  { 0, {-4, -6, 0}, {}},           // 2 right_arm
                  { 0, {4, -6, 0}, {}},            // 3 left_arm
                  {-1, {0, -5, 0}, {}},            // 4 right_leg
                  {-1, {0, -5, 0}, {}} },          // 5 left_leg
                { {0, 0, 15, {-4, -6, -3}, {8, 6, 6}, 0.0f},
                  {1, 0, 0, {-4, -5, -5}, {8, 5, 10}, 0.015f},
                  {1, 56, 0, {-1, -2, -6}, {2, 3, 2}, 0.0f},
                  {1, 37, 8, {-1, -9, -1}, {2, 4, 2}, -0.015f},
                  {1, 37, 0, {-2, -13, -2}, {4, 4, 4}, -0.015f},
                  {2, 36, 16, {-3, -1, -2}, {3, 10, 4}, 0.0f},
                  {3, 50, 16, {0, -1, -2}, {3, 10, 4}, 0.0f},
                  {4, 0, 27, {-4, 0, -2}, {4, 5, 4}, 0.0f},
                  {5, 16, 27, {0, 0, -2}, {4, 5, 4}, 0.0f} } };
            static const PoseDef kSitting = {
                { {-1, {0, -3, 2.325f}, {}},                                  // 0 body
                  { 0, {0, -1, -4.325f}, {0, 0, -3.1416f}},                   // 1 body_r1
                  { 0, {0, -6, -0.2f}, {}},                                   // 2 head
                  { 0, {-4, -5.6f, -1.8f}, {0.4363f, 0, 0}},                  // 3 right_arm
                  { 3, {0, 0.0893f, 0.1198f}, {-1.0472f, 0, 0}},              // 4 right_arm_r1
                  { 0, {4, -5.6f, -1.7f}, {0.4363f, 0, 0}},                   // 5 left_arm
                  { 5, {0, -0.0015f, -0.0808f}, {-1.0472f, 0, 0}},            // 6 left_arm_r1
                  {-1, {-2.1f, -2.1f, -2.075f}, {}},                          // 7 right_leg
                  { 7, {0.05f, -1.9f, 1.075f}, {-1.5708f, 0, 0}},             // 8 right_leg_r1
                  {-1, {2, -2, -2.075f}, {}},                                 // 9 left_leg
                  { 9, {0.05f, -2, 1.075f}, {-1.5708f, 0, 0}} },              // 10 left_leg_r1
                { {0, 3, 19, {-3, -4, -4.525f}, {6, 1, 6}, 0.0f},
                  {0, 0, 15, {-4, -3, -3.525f}, {8, 6, 6}, 0.0f},
                  {1, 3, 18, {-4, -3, -2.2f}, {8, 6, 3}, 0.0f},
                  {2, 37, 8, {-1, -7, -3.3f}, {2, 4, 2}, -0.015f},
                  {2, 37, 0, {-2, -11, -4.3f}, {4, 4, 4}, -0.015f},
                  {2, 0, 0, {-4, -3, -7.325f}, {8, 5, 10}, 0.0f},
                  {2, 56, 0, {-1, 0, -8.325f}, {2, 3, 2}, 0.0f},
                  {4, 36, 16, {-3.075f, -0.9733f, -1.9966f}, {3, 10, 4}, 0.0f},
                  {6, 50, 16, {0.075f, -1.0443f, -1.8997f}, {3, 10, 4}, 0.0f},
                  {8, 0, 27, {-2, 0.975f, 0}, {4, 5, 4}, 0.0f},
                  {10, 16, 27, {-2, 0.975f, 0}, {4, 5, 4}, 0.0f} } };
            static const PoseDef kRunning = {
                { {-1, {-1.064f, -5, 0}, {}},                                    // 0 body
                  { 0, {1.1f, 0.1f, 0.7f}, {0.1204f, -0.0064f, -0.0779f}},       // 1 body_r1
                  { 0, {0.7f, -5.6f, -1.8f}, {}},                                // 2 head
                  { 0, {-4, -6, 0}, {}},                                         // 3 right_arm
                  { 3, {0.7f, -0.248f, -1.62f}, {1.0036f, 0, 0}},                // 4 right_arm_r1
                  { 0, {4, -6, 0}, {}},                                          // 5 left_arm
                  { 5, {0.732f, 0, 0}, {-0.8715f, -0.0535f, -0.0449f}},          // 6 left_arm_r1
                  {-1, {-3.064f, -5, 0}, {}},                                    // 7 right_leg
                  { 7, {1.048f, 0, -0.9f}, {-0.8727f, 0, 0}},                    // 8 right_leg_r1
                  {-1, {0.936f, -5, 0}, {}},                                     // 9 left_leg
                  { 9, {1, 0, 0}, {0.7854f, 0, 0}} },                            // 10 left_leg_r1
                { {1, 0, 15, {-4.02f, -6.116f, -3.5f}, {8, 6, 6}, 0.0f},
                  {2, 0, 0, {-4, -5.1f, -5}, {8, 5, 10}, 0.0f},
                  {2, 56, 0, {-1.02f, -2.1f, -6}, {2, 3, 2}, 0.0f},
                  {2, 37, 8, {-1.02f, -9.1f, -1}, {2, 4, 2}, -0.015f},
                  {2, 37, 0, {-2, -13.1f, -2}, {4, 4, 4}, -0.015f},
                  {4, 36, 16, {-3.052f, -1.11f, -2.036f}, {3, 10, 4}, 0.0f},
                  {6, 50, 16, {0.032f, -1.1f, -2}, {3, 10, 4}, 0.0f},
                  {8, 0, 27, {-1.856f, -0.1f, -1.09f}, {4, 5, 4}, 0.0f},
                  {10, 16, 27, {-2.088f, -0.1f, -2}, {4, 5, 4}, 0.0f} } };
            static const PoseDef kStar = {
                { {-1, {0, -5, 0}, {}},                                // 0 body
                  { 0, {0, -6, 0}, {}},                                // 1 head
                  { 0, {-4, -6, 0}, {}},                               // 2 right_arm
                  { 2, {1, 1, 0}, {0, 0, 1.9199f}},                    // 3 right_arm_r1
                  { 0, {4, -6, 0}, {}},                                // 4 left_arm
                  { 4, {-1, 1, 0}, {0, 0, -1.9199f}},                  // 5 left_arm_r1
                  {-1, {-3, -5, 0}, {}},                               // 6 right_leg
                  { 6, {0.35f, 2, 0.01f}, {0, 0, 0.2618f}},            // 7 right_leg_r1
                  {-1, {1, -5, 0}, {}},                                // 8 left_leg
                  { 8, {1.65f, 2, 0}, {0, 0, -0.2618f}} },             // 9 left_leg_r1
                { {0, 0, 15, {-4, -6, -3}, {8, 6, 6}, 0.0f},
                  {1, 0, 0, {-4, -5, -5}, {8, 5, 10}, 0.0f},
                  {1, 56, 0, {-1, -2, -6}, {2, 3, 2}, 0.0f},
                  {1, 37, 8, {-1, -9, -1}, {2, 4, 2}, -0.015f},
                  {1, 37, 0, {-2, -13, -2}, {4, 4, 4}, -0.015f},
                  {3, 36, 16, {-1.5f, -5, -2}, {3, 10, 4}, 0.0f},
                  {5, 50, 16, {-1.5f, -5, -2}, {3, 10, 4}, 0.0f},
                  {7, 0, 27, {-2, -2.5f, -2}, {4, 5, 4}, 0.0f},
                  {9, 16, 27, {-2, -2.5f, -2}, {4, 5, 4}, 0.0f} } };
            switch (pose) {
                case 1:  return kSitting;
                case 2:  return kRunning;
                case 3:  return kStar;
                default: return kStanding;
            }
        }

        // createModelTransformation: translation (0.5, 0, 0.5), then
        // YP.rotationDegrees(-facing.getOpposite().toYRot()); toYRot is 90 ×
        // the 2D data value (south 0, west 1, north 2, east 3).
        glm::mat4 FacingTransform(int facing2D) {
            const int opposite = (facing2D + 2) & 3;
            glm::mat4 m = glm::translate(glm::mat4(1.0f), glm::vec3(0.5f, 0.0f, 0.5f));
            return glm::rotate(m, glm::radians(-90.0f * static_cast<float>(opposite)), glm::vec3(0, 1, 0));
        }
        // CopperGolemStatueModel.setupAnim: root y 0, zRot π.
        glm::mat4 RootPose() {
            return glm::rotate(glm::mat4(1.0f), kPi, glm::vec3(0, 0, 1));
        }

        int Facing2D(std::string_view f) {
            if (f == "south") return 0;
            if (f == "west")  return 1;
            if (f == "north") return 2;
            if (f == "east")  return 3;
            return 2;
        }

        // CopperGolemOxidationLevels: the stage's texture.
        const char* TextureFor(Game::BlockID block) {
            switch (block) {
                case Game::BlockID::ExposedCopperGolemStatue:
                case Game::BlockID::WaxedExposedCopperGolemStatue:   return "copper_golem/copper_golem_exposed";
                case Game::BlockID::WeatheredCopperGolemStatue:
                case Game::BlockID::WaxedWeatheredCopperGolemStatue: return "copper_golem/copper_golem_weathered";
                case Game::BlockID::OxidizedCopperGolemStatue:
                case Game::BlockID::WaxedOxidizedCopperGolemStatue:  return "copper_golem/copper_golem_oxidized";
                default:                                             return "copper_golem/copper_golem";
            }
        }

    } // namespace

    CopperGolemStatueRenderer::~CopperGolemStatueRenderer() { Shutdown(); }

    bool CopperGolemStatueRenderer::Initialize() {
        if (!g_renderBackend) return false;
        m_shader = BlockEntityShader::Create();
        if (m_shader == INVALID_SHADER) {
            Log::Error("[CopperGolemStatueRenderer] shader compile failed");
            return false;
        }
        using namespace BEModel;
        bool ok = true;
        for (int pose = 0; pose < kPoses; ++pose) {
            const PoseDef& def = Pose(pose);
            // Each part's frame in root space (the pose layers declare
            // parents before children).
            std::vector<glm::mat4> frames(def.parts.size());
            for (size_t i = 0; i < def.parts.size(); ++i) {
                const PartDef& part = def.parts[i];
                const glm::mat4 parent = part.parent < 0 ? RootPose() : frames[static_cast<size_t>(part.parent)];
                frames[i] = PartPose(parent, part.offset, part.rot);
            }
            std::vector<Vert> verts;
            std::vector<uint32_t> idx;
            for (int facing = 0; facing < 4; ++facing) {
                for (int set = 0; set < 2; ++set) {
                    const auto lightSet = set ? EntityLighting::LightSet::Nether : EntityLighting::LightSet::Default;
                    for (const CubeDef& cube : def.cubes) {
                        const glm::mat4& frame = frames[static_cast<size_t>(cube.part)];
                        const glm::mat3 normalMat = EntityLighting::NormalMatrix(FacingTransform(facing) * frame);
                        const glm::vec3 grow(cube.grow);
                        AddCube(verts, idx, cube.origin - grow, cube.origin + cube.size + grow,
                                cube.u, cube.v, cube.size.x, cube.size.y, cube.size.z, 64, 64, kAll,
                                CubeSpace(frame), normalMat, lightSet);
                    }
                }
            }
            m_pose[pose] = Upload(verts, idx, 8);
            ok = ok && m_pose[pose].Valid();
        }
        return ok;
    }

    void CopperGolemStatueRenderer::Shutdown() {
        for (auto& mesh : m_pose) BEModel::Destroy(mesh);
        m_textures.Clear();
        if (g_renderBackend && m_shader != INVALID_SHADER) g_renderBackend->DestroyShader(m_shader);
        m_shader = INVALID_SHADER;
    }

    void CopperGolemStatueRenderer::Render(const Game::BlockEntity& be,
                                           float /*partialTick*/,
                                           const glm::mat4& proj,
                                           const glm::mat4& view,
                                           const glm::vec3& cameraPos) {
        PROFILE_ZONE_N("BE.CopperGolemStatue");
        if (!g_renderBackend || m_shader == INVALID_SHADER) return;
        const glm::ivec3 p = be.GetWorldPos();
        int pose = 0, facing = 2;
        if (Client::g_clientBlockAccess) {
            const Game::BlockState state = Client::g_clientBlockAccess->GetBlockState(p.x, p.y, p.z);
            pose = std::max(0, state.GetIndex(Game::PropertyId::COPPER_GOLEM_POSE)) % kPoses;
            facing = Facing2D(state.GetValueByName("facing"));
        }
        const BEModel::Mesh& mesh = m_pose[pose];
        if (!mesh.Valid()) return;
        const TextureHandle tex = m_textures.Get(TextureFor(be.GetBlockId()));
        if (tex == INVALID_TEXTURE) return;

        const glm::mat4 model = glm::translate(glm::mat4(1.0f), Render::ToRender(glm::dvec3(p))) *
                                FacingTransform(facing);
        PipelineState s;
        s.depthTestEnabled  = true;
        s.depthWriteEnabled = true;
        s.blendEnabled      = false;
        s.cullMode          = CullMode::Back;
        s.primitiveType     = PrimitiveType::Triangles;
        g_renderBackend->SetPipelineState(s);
        g_renderBackend->BindShader(m_shader);
        g_renderBackend->BindTexture(tex, 0);
        // entityCutout.
        g_renderBackend->SetUniformFloat(m_shader, "uAlphaTest", 0.1f);
        g_renderBackend->SetUniformMat4(m_shader, "uMVP", proj * view * model);
        BlockEntityShader::ApplyWorld(m_shader, model, cameraPos, p);
        const bool nether = EntityLighting::Current() == EntityLighting::LightSet::Nether;
        BEModel::Draw(mesh, static_cast<uint32_t>(facing * 2 + (nether ? 1 : 0)));
        g_renderBackend->UnbindMesh();
    }

} // namespace Render
