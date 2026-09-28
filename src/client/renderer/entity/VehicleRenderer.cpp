// File: src/client/renderer/entity/VehicleRenderer.cpp
//
// MobRenderer::AppendVehicle — MC's boat and minecart renderers:
//
//   AbstractBoatRenderer.submit   translate(0, 0.375, 0), rotate Y 180 - yRot,
//                                 the hurt rock about X, the bubble-column
//                                 tilt about (1, 0, 1), scale(-1, -1, 1),
//                                 rotate Y 90, the model (BoatModel /
//                                 RaftModel, their chest variants, the paddles
//                                 posed by AbstractBoatModel.setupAnim), then
//                                 BoatRenderer's water patch (depth only)
//                                 while the boat is not under water.
//   AbstractMinecartRenderer      the per-entity jitter, oldRender (the cart
//                                 set on its rail: position and pitch from
//                                 the rail 0.3 ahead and behind), the hurt
//                                 rock, the displayed block (0.75 scale,
//                                 displayOffset), scale(-1, -1, 1), the
//                                 MinecartModel. TntMinecartRenderer swells
//                                 and flashes the block as the fuse runs out.
#include "client/renderer/entity/MobRenderer.hpp"

#include "client/entity/ClientMobManager.hpp"
#include "client/renderer/core/RenderOrigin.hpp"
#include "client/renderer/entity/BlockCubeEntityRenderer.hpp"
#include "client/renderer/environment/EntityEnvironment.hpp"
#include "client/renderer/texture/AtlasBuilder.hpp"

#include "common/core/Mth.hpp"
#include "common/entity/vehicle/Boat.hpp"
#include "common/entity/vehicle/Minecart.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/world/block/BlockState.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <memory>
#include <string>
#include <string_view>

namespace Render {

    namespace {

        constexpr float kPi = 3.1415927f;

        // Mth.clampedLerp(factor, min, max).
        float ClampedLerp(float factor, float min, float max) {
            if (factor < 0.0f) return min;
            return factor > 1.0f ? max : min + factor * (max - min);
        }

        // PoseStack.rotate(new Quaternionf().setAngleAxis(angle, x, y, z)) —
        // JOML's setAngleAxis does NOT normalise the axis, and
        // Matrix4f.rotate(Quaternionfc) takes the components as they are, so
        // the (1, 0, 1) bubble tilt of a boat also scales it slightly. The
        // matrix is built exactly as JOML's rotation(Quaternionfc).
        glm::mat4 QuaternionMatrix(float angle, float ax, float ay, float az) {
            const float s = std::sin(angle * 0.5f);
            const float x = ax * s, y = ay * s, z = az * s, w = std::cos(angle * 0.5f);
            const float w2 = w * w, x2 = x * x, y2 = y * y, z2 = z * z;
            const float zw = z * w, dzw = zw + zw, xy = x * y, dxy = xy + xy;
            const float xz = x * z, dxz = xz + xz, yw = y * w, dyw = yw + yw;
            const float yz = y * z, dyz = yz + yz, xw = x * w, dxw = xw + xw;
            glm::mat4 m(1.0f);
            m[0][0] = w2 + x2 - z2 - y2; m[0][1] = dxy + dzw;          m[0][2] = dxz - dyw;
            m[1][0] = -dzw + dxy;        m[1][1] = y2 - z2 + w2 - x2;  m[1][2] = dyz + dxw;
            m[2][0] = dyw + dxz;         m[2][1] = dyz - dxw;          m[2][2] = z2 - y2 - x2 + w2;
            return m;
        }

        glm::mat4 RotateDegrees(const glm::mat4& m, float degrees, const glm::vec3& axis) {
            return glm::rotate(m, glm::radians(degrees), axis);
        }

        // ── Models (MC LayerDefinitions, in pixels) ────────────────────────

        // BoatModel.addCommonParts (+ the chest parts for createChestBoatModel).
        std::unique_ptr<ModelPart> MakeBoatModel(bool chest) {
            auto root = std::make_unique<ModelPart>();
            root->AddChild("bottom", PartPose::OffsetAndRotation(0.0f, 3.0f, 1.0f, 1.5707964f, 0.0f, 0.0f))
                ->cubes.push_back(CubeDefinition{ -14.0f, -9.0f, -3.0f, 28.0f, 16.0f, 3.0f, 0.0f, 0.0f });
            root->AddChild("back", PartPose::OffsetAndRotation(-15.0f, 4.0f, 4.0f, 0.0f, 4.712389f, 0.0f))
                ->cubes.push_back(CubeDefinition{ -13.0f, -7.0f, -1.0f, 18.0f, 6.0f, 2.0f, 0.0f, 19.0f });
            root->AddChild("front", PartPose::OffsetAndRotation(15.0f, 4.0f, 0.0f, 0.0f, 1.5707964f, 0.0f))
                ->cubes.push_back(CubeDefinition{ -8.0f, -7.0f, -1.0f, 16.0f, 6.0f, 2.0f, 0.0f, 27.0f });
            root->AddChild("right", PartPose::OffsetAndRotation(0.0f, 4.0f, -9.0f, 0.0f, kPi, 0.0f))
                ->cubes.push_back(CubeDefinition{ -14.0f, -7.0f, -1.0f, 28.0f, 6.0f, 2.0f, 0.0f, 35.0f });
            root->AddChild("left", PartPose::Offset(0.0f, 4.0f, 9.0f))
                ->cubes.push_back(CubeDefinition{ -14.0f, -7.0f, -1.0f, 28.0f, 6.0f, 2.0f, 0.0f, 43.0f });
            ModelPart* left = root->AddChild("left_paddle",
                PartPose::OffsetAndRotation(3.0f, -5.0f, 9.0f, 0.0f, 0.0f, 0.19634955f));
            left->cubes.push_back(CubeDefinition{ -1.0f, 0.0f, -5.0f, 2.0f, 2.0f, 18.0f, 62.0f, 0.0f });
            left->cubes.push_back(CubeDefinition{ -1.001f, -3.0f, 8.0f, 1.0f, 6.0f, 7.0f, 62.0f, 0.0f });
            ModelPart* right = root->AddChild("right_paddle",
                PartPose::OffsetAndRotation(3.0f, -5.0f, -9.0f, 0.0f, kPi, 0.19634955f));
            right->cubes.push_back(CubeDefinition{ -1.0f, 0.0f, -5.0f, 2.0f, 2.0f, 18.0f, 62.0f, 20.0f });
            right->cubes.push_back(CubeDefinition{ 0.001f, -3.0f, 8.0f, 1.0f, 6.0f, 7.0f, 62.0f, 20.0f });
            if (chest) {
                root->AddChild("chest_bottom",
                               PartPose::OffsetAndRotation(-2.0f, -5.0f, -6.0f, 0.0f, -1.5707964f, 0.0f))
                    ->cubes.push_back(CubeDefinition{ 0.0f, 0.0f, 0.0f, 12.0f, 8.0f, 12.0f, 0.0f, 76.0f });
                root->AddChild("chest_lid",
                               PartPose::OffsetAndRotation(-2.0f, -9.0f, -6.0f, 0.0f, -1.5707964f, 0.0f))
                    ->cubes.push_back(CubeDefinition{ 0.0f, 0.0f, 0.0f, 12.0f, 4.0f, 12.0f, 0.0f, 59.0f });
                root->AddChild("chest_lock",
                               PartPose::OffsetAndRotation(-1.0f, -6.0f, -1.0f, 0.0f, -1.5707964f, 0.0f))
                    ->cubes.push_back(CubeDefinition{ 0.0f, 0.0f, 0.0f, 2.0f, 4.0f, 1.0f, 0.0f, 59.0f });
            }
            root->ResetPose();
            return root;
        }

        // RaftModel.addCommonParts (+ createChestRaftModel's chest).
        std::unique_ptr<ModelPart> MakeRaftModel(bool chest) {
            auto root = std::make_unique<ModelPart>();
            ModelPart* bottom = root->AddChild("bottom",
                PartPose::OffsetAndRotation(0.0f, -2.1f, 1.0f, 1.5708f, 0.0f, 0.0f));
            bottom->cubes.push_back(CubeDefinition{ -14.0f, -11.0f, -4.0f, 28.0f, 20.0f, 4.0f, 0.0f, 0.0f });
            bottom->cubes.push_back(CubeDefinition{ -14.0f, -9.0f, -8.0f, 28.0f, 16.0f, 4.0f, 0.0f, 0.0f });
            ModelPart* left = root->AddChild("left_paddle",
                PartPose::OffsetAndRotation(3.0f, -4.0f, 9.0f, 0.0f, 0.0f, 0.19634955f));
            left->cubes.push_back(CubeDefinition{ -1.0f, 0.0f, -5.0f, 2.0f, 2.0f, 18.0f, 0.0f, 24.0f });
            left->cubes.push_back(CubeDefinition{ -1.001f, -3.0f, 8.0f, 1.0f, 6.0f, 7.0f, 0.0f, 24.0f });
            ModelPart* right = root->AddChild("right_paddle",
                PartPose::OffsetAndRotation(3.0f, -4.0f, -9.0f, 0.0f, kPi, 0.19634955f));
            right->cubes.push_back(CubeDefinition{ -1.0f, 0.0f, -5.0f, 2.0f, 2.0f, 18.0f, 40.0f, 24.0f });
            right->cubes.push_back(CubeDefinition{ 0.001f, -3.0f, 8.0f, 1.0f, 6.0f, 7.0f, 40.0f, 24.0f });
            if (chest) {
                root->AddChild("chest_bottom",
                               PartPose::OffsetAndRotation(-2.0f, -10.1f, -6.0f, 0.0f, -1.5707964f, 0.0f))
                    ->cubes.push_back(CubeDefinition{ 0.0f, 0.0f, 0.0f, 12.0f, 8.0f, 12.0f, 0.0f, 76.0f });
                root->AddChild("chest_lid",
                               PartPose::OffsetAndRotation(-2.0f, -14.1f, -6.0f, 0.0f, -1.5707964f, 0.0f))
                    ->cubes.push_back(CubeDefinition{ 0.0f, 0.0f, 0.0f, 12.0f, 4.0f, 12.0f, 0.0f, 59.0f });
                root->AddChild("chest_lock",
                               PartPose::OffsetAndRotation(-1.0f, -11.1f, -1.0f, 0.0f, -1.5707964f, 0.0f))
                    ->cubes.push_back(CubeDefinition{ 0.0f, 0.0f, 0.0f, 2.0f, 4.0f, 1.0f, 0.0f, 59.0f });
            }
            root->ResetPose();
            return root;
        }

        // BoatModel.createWaterPatch.
        std::unique_ptr<ModelPart> MakeWaterPatch() {
            auto root = std::make_unique<ModelPart>();
            root->AddChild("water_patch", PartPose::OffsetAndRotation(0.0f, -3.0f, 1.0f, 1.5707964f, 0.0f, 0.0f))
                ->cubes.push_back(CubeDefinition{ -14.0f, -9.0f, -3.0f, 28.0f, 16.0f, 3.0f, 0.0f, 0.0f });
            root->ResetPose();
            return root;
        }

        // MinecartModel.createBodyLayer (64 × 32).
        std::unique_ptr<ModelPart> MakeMinecartModel() {
            auto root = std::make_unique<ModelPart>();
            root->AddChild("bottom", PartPose::OffsetAndRotation(0.0f, 4.0f, 0.0f, 1.5707964f, 0.0f, 0.0f))
                ->cubes.push_back(CubeDefinition{ -10.0f, -8.0f, -1.0f, 20.0f, 16.0f, 2.0f, 0.0f, 10.0f });
            root->AddChild("front", PartPose::OffsetAndRotation(-9.0f, 4.0f, 0.0f, 0.0f, 4.712389f, 0.0f))
                ->cubes.push_back(CubeDefinition{ -8.0f, -9.0f, -1.0f, 16.0f, 8.0f, 2.0f, 0.0f, 0.0f });
            root->AddChild("back", PartPose::OffsetAndRotation(9.0f, 4.0f, 0.0f, 0.0f, 1.5707964f, 0.0f))
                ->cubes.push_back(CubeDefinition{ -8.0f, -9.0f, -1.0f, 16.0f, 8.0f, 2.0f, 0.0f, 0.0f });
            root->AddChild("left", PartPose::OffsetAndRotation(0.0f, 4.0f, -7.0f, 0.0f, kPi, 0.0f))
                ->cubes.push_back(CubeDefinition{ -8.0f, -9.0f, -1.0f, 16.0f, 8.0f, 2.0f, 0.0f, 0.0f });
            root->AddChild("right", PartPose::Offset(0.0f, 4.0f, 7.0f))
                ->cubes.push_back(CubeDefinition{ -8.0f, -9.0f, -1.0f, 16.0f, 8.0f, 2.0f, 0.0f, 0.0f });
            root->ResetPose();
            return root;
        }

        // AbstractBoatModel.animatePaddle.
        void AnimatePaddle(float time, int side, ModelPart* paddle) {
            if (!paddle) return;
            paddle->xRot = ClampedLerp((std::sin(-time) + 1.0f) / 2.0f, -1.0471976f, -0.2617994f);
            paddle->yRot = ClampedLerp((std::sin(-time + 1.0f) + 1.0f) / 2.0f, -0.7853982f, 0.7853982f);
            if (side == 1) paddle->yRot = kPi - paddle->yRot;
        }

        // The wood a boat entity is drawn from: its type's name without the
        // "_boat" / "_chest_boat" suffix; both rafts are "bamboo"
        // (ModelLayers.createBoatModelName / createChestBoatModelName).
        std::string BoatWood(Game::EntityTypeId type) {
            if (Game::IsRaftEntityType(type)) return "bamboo";
            std::string slug(Game::GetEntityTypeInfo(type).slug);
            if (const size_t colon = slug.find(':'); colon != std::string::npos) slug.erase(0, colon + 1);
            for (const std::string_view suffix : { std::string_view("_chest_boat"), std::string_view("_boat") }) {
                if (slug.size() > suffix.size() &&
                    slug.compare(slug.size() - suffix.size(), suffix.size(), suffix) == 0) {
                    slug.erase(slug.size() - suffix.size());
                    break;
                }
            }
            return slug;
        }

        // TntRenderer.getSwellAmount / isLit.
        float TntSwellAmount(float fuse) {
            float g = 1.0f - fuse / 10.0f;
            g = std::clamp(g, 0.0f, 1.0f);
            g *= g;
            g *= g;
            return g * 0.3f;
        }
        bool TntIsLit(float fuse) {
            return fuse >= 0.0f && static_cast<int>(fuse / 5.0f) % 2 == 0;
        }

    } // namespace

    void MobRenderer::AppendVehicle(const Client::ClientMob& entry, const glm::dvec3& renderPos, float partialTick,
                                    std::vector<VehiclePiece>& out) {
        out.clear();
        if (!entry.mob) return;
        const Game::Mob& mob = *entry.mob;
        const Game::EntityTypeId type = mob.GetType();
        if (!Game::IsVehicleEntityType(type)) return;
        const auto& vehicle = static_cast<const Game::VehicleEntity&>(mob);

        // EntityRenderer.getPackedLightCoords at the light probe (the eye).
        const int light = EntityEnvironment::PackedLightAt(
            renderPos + glm::dvec3(0.0, static_cast<double>(mob.TypeInfo().eyeHeight), 0.0));
        const float hurt = static_cast<float>(vehicle.GetHurtTime()) - partialTick;
        const float damageTime = std::max(vehicle.GetDamage() - partialTick, 0.0f);
        const float hurtDir = static_cast<float>(vehicle.GetHurtDir());

        if (Game::IsBoatEntityType(type)) {
            const auto* boat = dynamic_cast<const Game::Boat*>(&mob);
            if (!boat) return;
            const bool raft = boat->IsRaft();
            const bool chest = boat->IsChest();
            const std::string wood = BoatWood(type);
            const TextureHandle tex = LoadTexture(std::string("assets/textures/entity/") +
                                                  (chest ? "chest_boat/" : "boat/") + wood + ".png");
            if (tex == INVALID_TEXTURE) return;

            static const std::unique_ptr<ModelPart> kBoat      = MakeBoatModel(false);
            static const std::unique_ptr<ModelPart> kChestBoat = MakeBoatModel(true);
            static const std::unique_ptr<ModelPart> kRaft      = MakeRaftModel(false);
            static const std::unique_ptr<ModelPart> kChestRaft = MakeRaftModel(true);
            static const std::unique_ptr<ModelPart> kWaterPatch = MakeWaterPatch();
            ModelPart& model = raft ? (chest ? *kChestRaft : *kRaft) : (chest ? *kChestBoat : *kBoat);

            // AbstractBoatModel.setupAnim: the rest pose, then the paddles.
            model.ResetPose();
            AnimatePaddle(boat->GetRowingTime(0, partialTick), 0, model.Find("left_paddle"));
            AnimatePaddle(boat->GetRowingTime(1, partialTick), 1, model.Find("right_paddle"));

            const float yRot = Game::Mth::RotLerp(partialTick, entry.renderPrevYRot, mob.yRot);
            const float bubbleAngle = boat->GetBubbleAngle(partialTick);
            const bool underWater = boat->IsBoatUnderWater();

            glm::mat4 m = glm::translate(glm::mat4(1.0f), ToRender(renderPos));
            m = glm::translate(m, glm::vec3(0.0f, 0.375f, 0.0f));
            m = RotateDegrees(m, 180.0f - yRot, glm::vec3(0.0f, 1.0f, 0.0f));
            if (hurt > 0.0f) {
                m = RotateDegrees(m, std::sin(hurt) * hurt * damageTime / 10.0f * hurtDir, glm::vec3(1.0f, 0.0f, 0.0f));
            }
            if (!underWater && std::abs(bubbleAngle) > 1.0e-5f) {
                m = m * QuaternionMatrix(bubbleAngle * 0.017453292f, 1.0f, 0.0f, 1.0f);
            }
            m = glm::scale(m, glm::vec3(-1.0f, -1.0f, 1.0f));
            m = RotateDegrees(m, 90.0f, glm::vec3(0.0f, 1.0f, 0.0f));
            m = glm::scale(m, glm::vec3(1.0f / 16.0f));

            const float texHeight = chest ? 128.0f : 64.0f;
            VehiclePiece body;
            body.texture = tex;
            body.first = m_indices.size();
            model.Build(m, 128.0f, texHeight, m_verts, m_indices);
            body.count = m_indices.size() - body.first;
            body.packedLight = light;
            if (body.count > 0) out.push_back(body);

            // BoatRenderer.submitTypeAdditions: the water patch, a raft has
            // none (RaftRenderer has no additions).
            if (!raft && !underWater) {
                VehiclePiece patch;
                patch.texture = tex;
                patch.first = m_indices.size();
                kWaterPatch->Build(m, 128.0f, texHeight, m_verts, m_indices);
                patch.count = m_indices.size() - patch.first;
                patch.depthOnly = true;
                patch.packedLight = light;
                if (patch.count > 0) out.push_back(patch);
            }
            return;
        }

        // ── Minecarts ──────────────────────────────────────────────────────
        const auto* cart = dynamic_cast<const Game::AbstractMinecart*>(&mob);
        if (!cart) return;
        const TextureHandle cartTex = LoadTexture("assets/textures/entity/minecart/minecart.png");
        if (cartTex == INVALID_TEXTURE) return;
        static const std::unique_ptr<ModelPart> kMinecart = MakeMinecartModel();

        // The per-entity jitter against z-fighting between stacked carts.
        const uint64_t seed0 = static_cast<uint64_t>(static_cast<int64_t>(mob.GetId())) * 493286711ull;
        const uint64_t offsetSeed = seed0 * seed0 * 4392167121ull + seed0 * 98761ull;
        const float offsetX = ((static_cast<float>((offsetSeed >> 16) & 7ull) + 0.5f) / 8.0f - 0.5f) * 0.004f;
        const float offsetY = ((static_cast<float>((offsetSeed >> 20) & 7ull) + 0.5f) / 8.0f - 0.5f) * 0.004f;
        const float offsetZ = ((static_cast<float>((offsetSeed >> 24) & 7ull) + 0.5f) / 8.0f - 0.5f) * 0.004f;

        glm::mat4 m = glm::translate(glm::mat4(1.0f), ToRender(renderPos));
        m = glm::translate(m, glm::vec3(offsetX, offsetY, offsetZ));

        // oldExtractState + oldRender: set on the rail.
        float xRot = Game::Mth::Lerp(partialTick, entry.renderPrevXRot, mob.xRot);
        float rotation = Game::Mth::RotLerp(partialTick, entry.renderPrevYRot, mob.yRot);
        if (const Game::EntityLevel* level = mob.Level(); level && level->Blocks()) {
            const Game::IBlockAccess& blocks = *level->Blocks();
            glm::dvec3 onRail;
            if (Game::AbstractMinecart::GetRailPos(blocks, renderPos.x, renderPos.y, renderPos.z, onRail)) {
                glm::dvec3 front = onRail, back = onRail;
                glm::dvec3 p;
                if (Game::AbstractMinecart::GetRailPosOffs(blocks, renderPos.x, renderPos.y, renderPos.z,
                                                           0.30000001192092896, p)) {
                    front = p;
                }
                if (Game::AbstractMinecart::GetRailPosOffs(blocks, renderPos.x, renderPos.y, renderPos.z,
                                                           -0.30000001192092896, p)) {
                    back = p;
                }
                m = glm::translate(m, glm::vec3(static_cast<float>(onRail.x - renderPos.x),
                                                static_cast<float>((front.y + back.y) / 2.0 - renderPos.y),
                                                static_cast<float>(onRail.z - renderPos.z)));
                glm::dvec3 direction = back - front;
                const double len = glm::length(direction);
                if (len != 0.0) {
                    direction /= len;
                    rotation = static_cast<float>(std::atan2(direction.z, direction.x) * 180.0 / 3.141592653589793);
                    xRot = static_cast<float>(std::atan(direction.y) * 73.0);
                }
            }
        }
        m = glm::translate(m, glm::vec3(0.0f, 0.375f, 0.0f));
        m = RotateDegrees(m, 180.0f - rotation, glm::vec3(0.0f, 1.0f, 0.0f));
        m = RotateDegrees(m, -xRot, glm::vec3(0.0f, 0.0f, 1.0f));
        if (hurt > 0.0f) {
            m = RotateDegrees(m, std::sin(hurt) * hurt * damageTime / 10.0f * hurtDir, glm::vec3(1.0f, 0.0f, 0.0f));
        }

        // The displayed block: scale 0.75, translate(-0.5, (offset - 8)/16,
        // 0.5), rotate Y 90, the block model in its unit cell.
        const Game::BlockState display = cart->GetDisplayBlockState();
        if (display.Block() != Game::BlockID::Air && g_atlasBuilder) {
            static thread_local std::vector<ItemCubeVert> bv;
            static thread_local std::vector<uint32_t> bi;
            bv.clear();
            bi.clear();
            BlockCubeEntityRenderer::BuildStateMesh(display, bv, bi);
            if (!bv.empty()) {
                glm::mat4 b = glm::scale(m, glm::vec3(0.75f));
                b = glm::translate(b, glm::vec3(-0.5f, static_cast<float>(cart->GetDisplayOffset() - 8) / 16.0f, 0.5f));
                b = RotateDegrees(b, 90.0f, glm::vec3(0.0f, 1.0f, 0.0f));
                bool white = false;
                if (const auto* tnt = dynamic_cast<const Game::MinecartTNT*>(cart)) {
                    // TntMinecartRenderer.submitMinecartContents.
                    const float fuse = tnt->GetFuse() > -1
                        ? static_cast<float>(tnt->GetFuse()) - partialTick + 1.0f : -1.0f;
                    if (fuse > -1.0f && fuse < 10.0f) {
                        const float swell = TntSwellAmount(fuse);
                        b = glm::translate(b, glm::vec3(-swell * 0.5f, 0.0f, -swell * 0.5f));
                        b = glm::scale(b, glm::vec3(1.0f + swell));
                    }
                    white = TntIsLit(fuse);
                }
                VehiclePiece block;
                block.texture = g_atlasBuilder->GetBackendTextureHandle();
                block.first = m_indices.size();
                const auto base = static_cast<uint32_t>(m_verts.size());
                for (const ItemCubeVert& v : bv) {
                    const glm::vec3 p = glm::vec3(b * glm::vec4(v.x, v.y, v.z, 1.0f));
                    m_verts.push_back({ p.x, p.y, p.z, v.u, v.v, v.r, v.g, v.b, v.a });
                }
                for (const uint32_t i : bi) m_indices.push_back(base + i);
                block.count = m_indices.size() - block.first;
                block.whiteFlash = white;
                block.packedLight = light;
                if (block.count > 0) out.push_back(block);
            }
        }

        m = glm::scale(m, glm::vec3(-1.0f, -1.0f, 1.0f));
        m = glm::scale(m, glm::vec3(1.0f / 16.0f));
        VehiclePiece body;
        body.texture = cartTex;
        body.first = m_indices.size();
        kMinecart->Build(m, 64.0f, 32.0f, m_verts, m_indices);
        body.count = m_indices.size() - body.first;
        body.packedLight = light;
        if (body.count > 0) out.push_back(body);
    }

} // namespace Render
