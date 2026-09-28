// File: src/client/renderer/entity/model/HumanoidArmorModel.cpp
#include "client/renderer/entity/model/HumanoidArmorModel.hpp"

#include <functional>
#include <initializer_list>
#include <string_view>

namespace Render {

    namespace {
        // One deformed cube on a part (MC CubeListBuilder.addBox with a
        // CubeDeformation(x, y, z)).
        void AddBox(ModelPart* part, float texX, float texY, float ox, float oy, float oz,
                    float sx, float sy, float sz, const glm::vec3& g, bool mirror = false) {
            part->cubes.push_back(CubeDefinition{ ox, oy, oz, sx, sy, sz, texX, texY, g.x, g.y, g.z, mirror });
        }

        constexpr const char* kPartNames[] = {
            "head", "hat", "body", "right_arm", "left_arm", "right_leg", "left_leg"
        };

        bool InSet(std::string_view name, std::initializer_list<std::string_view> set) {
            for (const std::string_view s : set) if (s == name) return true;
            return false;
        }

        // MC PartDefinition.retainPartsAndChildren / retainExactParts,
        // expressed as skipDraw over the one mesh: a retained part draws its
        // own cubes (and, for the first, its children's); every other part
        // keeps its pose but draws nothing of its own.
        void Retain(ModelPart& part, std::initializer_list<std::string_view> set, bool withChildren,
                    bool ancestorRetained) {
            for (auto& child : part.children) {
                const bool named = InSet(child->name, set);
                const bool draws = named || (withChildren && ancestorRetained);
                child->visible = true;
                child->skipDraw = !draws;
                Retain(*child, set, withChildren, withChildren && draws);
            }
        }
    } // namespace

    HumanoidArmorModel::Pose HumanoidArmorModel::Capture(const ModelPart& p) {
        return Pose{ p.x, p.y, p.z, p.xRot, p.yRot, p.zRot, p.xScale, p.yScale, p.zScale };
    }

    HumanoidArmorModel::Pose HumanoidArmorModel::RestOf(const ModelPart& p) {
        const PartPose& r = p.pose;
        return Pose{ r.x, r.y, r.z, r.xRot, r.yRot, r.zRot, r.xScale, r.yScale, r.zScale };
    }

    HumanoidArmorModel::HumanoidArmorModel(Mesh mesh, const glm::vec3& g, const glm::vec3& armOffset)
        : m_mesh(mesh) {
        const glm::vec3 legs = g - glm::vec3(0.1f);    // g.extend(-0.1F)
        const glm::vec3 hatG = g + glm::vec3(0.5f);    // g.extend(0.5F)
        ModelPart *head = nullptr, *hat = nullptr, *body = nullptr, *rightArm = nullptr,
                  *leftArm = nullptr, *rightLeg = nullptr, *leftLeg = nullptr;
        switch (mesh) {
            case Mesh::Humanoid: {
                // HumanoidModel.createBaseArmorMesh: createMesh(g, 0), the
                // legs replaced by ones 0.1 thinner.
                m_texWidth = 64.0f;
                m_texHeight = 32.0f;
                head = m_root.AddChild("head", PartPose::Offset(0.0f, 0.0f, 0.0f));
                AddBox(head, 0, 0, -4.0f, -8.0f, -4.0f, 8.0f, 8.0f, 8.0f, g);
                hat = head->AddChild("hat", PartPose::Zero());
                AddBox(hat, 32, 0, -4.0f, -8.0f, -4.0f, 8.0f, 8.0f, 8.0f, hatG);
                body = m_root.AddChild("body", PartPose::Offset(0.0f, 0.0f, 0.0f));
                AddBox(body, 16, 16, -4.0f, 0.0f, -2.0f, 8.0f, 12.0f, 4.0f, g);
                rightArm = m_root.AddChild("right_arm", PartPose::Offset(-5.0f, 2.0f, 0.0f));
                AddBox(rightArm, 40, 16, -3.0f, -2.0f, -2.0f, 4.0f, 12.0f, 4.0f, g);
                leftArm = m_root.AddChild("left_arm", PartPose::Offset(5.0f, 2.0f, 0.0f));
                AddBox(leftArm, 40, 16, -1.0f, -2.0f, -2.0f, 4.0f, 12.0f, 4.0f, g, true);
                rightLeg = m_root.AddChild("right_leg", PartPose::Offset(-1.9f, 12.0f, 0.0f));
                AddBox(rightLeg, 0, 16, -2.0f, 0.0f, -2.0f, 4.0f, 12.0f, 4.0f, legs);
                leftLeg = m_root.AddChild("left_leg", PartPose::Offset(1.9f, 12.0f, 0.0f));
                AddBox(leftLeg, 0, 16, -2.0f, 0.0f, -2.0f, 4.0f, 12.0f, 4.0f, legs, true);
                break;
            }
            case Mesh::ZombieVillager: {
                // ZombieVillagerModel.createBaseArmorMesh: createMesh(g, 0)
                // with the head replaced (its hat kept — addOrReplaceChild
                // carries the children over), the body and legs +0.1 and the
                // legs at ±2.
                m_texWidth = 64.0f;
                m_texHeight = 32.0f;
                const glm::vec3 wide = g + glm::vec3(0.1f);   // g.extend(0.1F)
                head = m_root.AddChild("head", PartPose::Zero());
                AddBox(head, 0, 0, -4.0f, -10.0f, -4.0f, 8.0f, 8.0f, 8.0f, g);
                hat = head->AddChild("hat", PartPose::Zero());
                AddBox(hat, 32, 0, -4.0f, -8.0f, -4.0f, 8.0f, 8.0f, 8.0f, hatG);
                hat->AddChild("hat_rim", PartPose::Zero());
                body = m_root.AddChild("body", PartPose::Zero());
                AddBox(body, 16, 16, -4.0f, 0.0f, -2.0f, 8.0f, 12.0f, 4.0f, wide);
                rightArm = m_root.AddChild("right_arm", PartPose::Offset(-5.0f, 2.0f, 0.0f));
                AddBox(rightArm, 40, 16, -3.0f, -2.0f, -2.0f, 4.0f, 12.0f, 4.0f, g);
                leftArm = m_root.AddChild("left_arm", PartPose::Offset(5.0f, 2.0f, 0.0f));
                AddBox(leftArm, 40, 16, -1.0f, -2.0f, -2.0f, 4.0f, 12.0f, 4.0f, g, true);
                rightLeg = m_root.AddChild("right_leg", PartPose::Offset(-2.0f, 12.0f, 0.0f));
                AddBox(rightLeg, 0, 16, -2.0f, 0.0f, -2.0f, 4.0f, 12.0f, 4.0f, wide);
                leftLeg = m_root.AddChild("left_leg", PartPose::Offset(2.0f, 12.0f, 0.0f));
                AddBox(leftLeg, 0, 16, -2.0f, 0.0f, -2.0f, 4.0f, 12.0f, 4.0f, wide, true);
                break;
            }
            case Mesh::HumanoidBaby: {
                // HumanoidModel.createBabyArmorMesh(g, armOffset), part for
                // part (MC names the left leg's foot "right_foot" and the
                // right leg's "left_foot"; kept, the FEET set lists both).
                m_texWidth = 64.0f;
                m_texHeight = 64.0f;
                head = m_root.AddChild("head", PartPose::Offset(0.0f, 15.0f, 0.0f));
                AddBox(head, 0, 0, -4.5f, -7.0f, -4.5f, 9.0f, 8.0f, 8.0f, g);
                body = m_root.AddChild("body", PartPose::Offset(0.0f, 18.0f, 0.0f));
                AddBox(body, 0, 17, -3.0f, -3.0f, -1.5f, 6.0f, 5.0f, 3.0f, g);
                ModelPart* waist = m_root.AddChild("waist", PartPose::Offset(0.0f, 19.0f, 0.0f));
                AddBox(waist, 0, 36, -3.0f, -1.2f, -1.49f, 5.9f, 2.0f, 2.9f, legs);
                rightArm = m_root.AddChild("right_arm",
                    PartPose::Offset(-3.5f - armOffset.x, 15.5f + armOffset.y, 0.0f + armOffset.z));
                AddBox(rightArm, 30, 25, -1.0f, 0.0f, -1.53f, 2.0f, 5.0f, 3.0f, g);
                leftArm = m_root.AddChild("left_arm",
                    PartPose::Offset(3.5f + armOffset.x, 15.5f + armOffset.y, 0.0f + armOffset.z));
                AddBox(leftArm, 30, 17, -1.0f, 0.0f, -1.53f, 2.0f, 5.0f, 3.0f, g);
                ModelPart* innerBody = m_root.AddChild("inner_body", PartPose::Offset(0.0f, 18.0f, 0.0f));
                AddBox(innerBody, 0, 17, -3.0f, -3.0f, -1.5f, 6.0f, 5.0f, 3.0f, g);
                leftLeg = m_root.AddChild("left_leg", PartPose::Offset(1.5f, 20.0f, 0.5f));
                AddBox(leftLeg, 18, 24, -2.0f, -0.2f, -2.0f, 3.0f, 4.0f, 3.0f, legs);
                rightLeg = m_root.AddChild("right_leg", PartPose::Offset(-1.5f, 20.0f, 0.5f));
                AddBox(rightLeg, 18, 17, -1.0f, -0.2f, -2.0f, 3.0f, 4.0f, 3.0f, legs);
                ModelPart* rightFoot = leftLeg->AddChild("right_foot", PartPose::Zero());
                AddBox(rightFoot, 0, 25, -2.0f, 2.9f, -2.0f, 3.0f, 1.0f, 3.0f, g);
                ModelPart* leftFoot = rightLeg->AddChild("left_foot", PartPose::Zero());
                AddBox(leftFoot, 0, 29, -1.0f, 2.9f, -2.0f, 3.0f, 1.0f, 3.0f, g, true);
                hat = head->AddChild("hat", PartPose::Zero());
                break;
            }
        }
        m_parts = { head, hat, body, rightArm, leftArm, rightLeg, leftLeg };
        m_root.ResetPose();
        m_rootPose = Capture(m_root);
        for (int i = 0; i < PartCount; ++i) m_poses[i] = Capture(*m_parts[i]);
    }

    bool HumanoidArmorModel::CopyPoseFrom(EntityModel& wearer) {
        ModelPart& root = wearer.Root();
        std::array<ModelPart*, PartCount> source{};
        for (int i = 0; i < PartCount; ++i) {
            source[i] = root.Find(kPartNames[i]);
            if (!source[i]) return false;
        }
        m_rootPose = Capture(root);
        if (m_mesh != Mesh::HumanoidBaby) {
            // HumanoidModel.copyPropertiesTo: the parts' poses, whole.
            for (int i = 0; i < PartCount; ++i) m_poses[i] = Capture(*source[i]);
            return true;
        }
        // The baby mesh: its own rest pose plus the wearer's change from
        // the wearer's rest pose.
        for (int i = 0; i < PartCount; ++i) {
            const ModelPart& w = *source[i];
            const Pose now = Capture(w);
            const Pose rest = RestOf(w);
            const Pose own = RestOf(*m_parts[i]);
            const auto ratio = [](float a, float b) { return b != 0.0f ? a / b : 1.0f; };
            m_poses[i] = Pose{
                own.x + (now.x - rest.x), own.y + (now.y - rest.y), own.z + (now.z - rest.z),
                own.xRot + (now.xRot - rest.xRot), own.yRot + (now.yRot - rest.yRot),
                own.zRot + (now.zRot - rest.zRot),
                own.xScale * ratio(now.xScale, rest.xScale), own.yScale * ratio(now.yScale, rest.yScale),
                own.zScale * ratio(now.zScale, rest.zScale) };
        }
        return true;
    }

    void HumanoidArmorModel::SetupAnim(const EntityRenderState& /*state*/) {
        const auto apply = [](ModelPart& p, const Pose& pose) {
            p.x = pose.x; p.y = pose.y; p.z = pose.z;
            p.xRot = pose.xRot; p.yRot = pose.yRot; p.zRot = pose.zRot;
            p.xScale = pose.xScale; p.yScale = pose.yScale; p.zScale = pose.zScale;
        };
        apply(m_root, m_rootPose);
        for (int i = 0; i < PartCount; ++i) apply(*m_parts[i], m_poses[i]);
    }

    void HumanoidArmorModel::ShowPartsForSlot(int equipmentSlot) {
        // Game::EquipmentSlot: FEET 2, LEGS 3, CHEST 4, HEAD 5. HEAD keeps
        // the head with everything under it (retainPartsAndChildren); the
        // rest keep exactly their parts (retainExactParts).
        const bool baby = m_mesh == Mesh::HumanoidBaby;
        switch (equipmentSlot) {
            case 5:
                Retain(m_root, { "head" }, /*withChildren=*/true, false);
                break;
            case 4:
                Retain(m_root, { "body", "left_arm", "right_arm" }, false, false);
                break;
            case 3:
                if (baby) Retain(m_root, { "left_leg", "right_leg", "waist" }, false, false);
                else      Retain(m_root, { "left_leg", "right_leg", "body" }, false, false);
                break;
            case 2:
                if (baby) Retain(m_root, { "left_foot", "right_foot" }, false, false);
                else      Retain(m_root, { "left_leg", "right_leg" }, false, false);
                break;
            default:
                Retain(m_root, {}, false, false);
                break;
        }
    }

} // namespace Render
