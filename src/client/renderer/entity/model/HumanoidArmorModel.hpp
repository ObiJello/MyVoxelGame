// File: src/client/renderer/entity/model/HumanoidArmorModel.hpp
//
// MC HumanoidModel's armour meshes — what HumanoidArmorLayer draws a mob's
// worn armour with. Three mesh shapes, each built for one CubeDeformation:
//
//   Humanoid        HumanoidModel.createBaseArmorMesh — the humanoid mesh
//                   inflated by the deformation, legs 0.1 thinner. The
//                   zombie / skeleton families at outer 1.0 / inner 0.5, the
//                   piglin family (PiglinModel.createArmorMeshSet, the
//                   player's set with the ears taken out) at 1.02 / 0.5.
//                   64x32 humanoid / humanoid_leggings sheets.
//   ZombieVillager  ZombieVillagerModel.createBaseArmorMesh — the taller
//                   villager head (top at -10), body and legs +0.1 and the
//                   legs at ±2. 64x32 sheets.
//   HumanoidBaby    HumanoidModel.createBabyArmorMesh — the 26.x baby body's
//                   own armour (head, body, waist, arms, legs with their
//                   feet), with the piglin babies' arm offset. 64x64
//                   humanoid_baby sheet for every slot.
//
// MC keeps one mesh per slot (createArmorMeshSet's retainPartsAndChildren /
// retainExactParts); here one mesh serves all four, ShowPartsForSlot
// marking which parts draw their own cubes (skipDraw) for the piece.
//
// It has no animation of its own. MC poses the armour with the wearer's
// setupAnim over the armour mesh; CopyPoseFrom captures the wearer's posed
// parts (head, hat, body, arms, legs). The adult meshes share the wearer's
// rest layout, so they take the poses whole — which is also what carries a
// classic-look baby's transform (offset / scale in the pose) onto the
// armour. The baby mesh has its own rest layout, so it takes the wearer's
// change from ITS rest pose (the setupAnim's rotations and offsets) on top of
// the armour's.
#pragma once

#include "client/renderer/entity/model/EntityModels.hpp"

#include <array>

namespace Render {

    class HumanoidArmorModel : public EntityModel {
    public:
        enum class Mesh { Humanoid, ZombieVillager, HumanoidBaby };

        // `grow` is the CubeDeformation (x, y, z); `babyArmOffset` MC's
        // armOffset PartPose for HumanoidBaby (the piglin babies' (0.5,
        // -0.5, 0)), ignored otherwise.
        HumanoidArmorModel(Mesh mesh, const glm::vec3& grow,
                           const glm::vec3& babyArmOffset = glm::vec3(0.0f));
        // The adult humanoid mesh at a uniform deformation.
        explicit HumanoidArmorModel(float grow)
            : HumanoidArmorModel(Mesh::Humanoid, glm::vec3(grow)) {}

        void SetupAnim(const EntityRenderState& state) override;

        // The wearer's current pose (see the header comment). False when
        // the wearer is not humanoid (a part missing) — nothing to wear the
        // armour on.
        bool CopyPoseFrom(EntityModel& wearer);

        // MC ADULT_ARMOR_PARTS_PER_SLOT / BABY_ARMOR_PARTS_PER_SLOT: only the
        // parts a slot's piece covers draw. `equipmentSlot` is
        // Game::EquipmentSlot's value: 2 feet, 3 legs, 4 chest, 5 head.
        void ShowPartsForSlot(int equipmentSlot);

        Mesh GetMesh() const { return m_mesh; }

    private:
        enum Part : int { Head, Hat, Body, RightArm, LeftArm, RightLeg, LeftLeg, PartCount };
        struct Pose {
            float x, y, z, xRot, yRot, zRot, xScale, yScale, zScale;
        };
        static Pose Capture(const ModelPart& p);
        static Pose RestOf(const ModelPart& p);

        Mesh m_mesh;
        std::array<ModelPart*, PartCount> m_parts{};
        Pose m_rootPose{};
        std::array<Pose, PartCount> m_poses{};
    };

} // namespace Render
