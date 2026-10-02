// File: src/common/entity/PlayerModelLayout.hpp
//
// MC PlayerModel.createMesh (and PlayerCapeModel.createCapeLayer) as data:
// every box of the classic (WIDE) and slim player models, in MC model space —
// pixels, Y down, each box relative to its part's pivot — with the skin
// texture offsets the 64x64 sheet is laid out for. One table, two readers:
// the game's Render::PlayerModel builds its ModelParts from it, and the
// launcher's skin editor builds its preview mesh and ray-picks texels from
// it, so a texel painted in the editor is the texel the game samples.
//
// The cube's UV layout (CubeListBuilder / ModelPart.Cube): for a box of size
// (w, h, d) at texture offset (u, v), the faces take
//   top    (u+d,     v)     – (u+d+w,     v+d)
//   bottom (u+d+w,   v)     – (u+d+w+w,   v+d)
//   right  (u,       v+d)   – (u+d,       v+d+h)     (the part's −X side)
//   front  (u+d,     v+d)   – (u+d+w,     v+d+h)
//   left   (u+d+w,   v+d)   – (u+d+w+d,   v+d+h)
//   back   (u+d+w+d, v+d)   – (u+d+w+d+w, v+d+h)
// An outer-layer box ("hat", "jacket", sleeves, pants) is inflated by `grow`
// but samples its unInflated size's layout.
#pragma once

#include "common/entity/PlayerAppearance.hpp"

#include <cstddef>
#include <cstdint>

namespace Game::PlayerLayout {

    enum class Part : uint8_t { Head = 0, Body, RightArm, LeftArm, RightLeg, LeftLeg, Count };

    // MC PartPose.offset of each root part (pixels, model space).
    struct Pivot { float x, y, z; };
    inline constexpr Pivot kPivots[static_cast<int>(Part::Count)] = {
        {  0.0f,  0.0f, 0.0f },   // head
        {  0.0f,  0.0f, 0.0f },   // body
        { -5.0f,  2.0f, 0.0f },   // right_arm
        {  5.0f,  2.0f, 0.0f },   // left_arm
        { -1.9f, 12.0f, 0.0f },   // right_leg
        {  1.9f, 12.0f, 0.0f },   // left_leg
    };
    inline const Pivot& PivotOf(Part p) { return kPivots[static_cast<int>(p)]; }

    struct Box {
        Part    part;
        bool    outer;            // the second layer
        uint8_t partBit;          // ModelPartBits toggling an outer box (0 = always)
        float   ox, oy, oz;       // origin, relative to the part pivot
        float   sx, sy, sz;       // size
        int     texU, texV;       // texture offset on the 64x64 sheet
        float   grow;             // CubeDeformation
        const char* name;         // MC part name
    };

    namespace MPB = ModelPartBits;

    // PlayerModel.createMesh(CubeDeformation.NONE, slim = false).
    inline constexpr Box kClassic[] = {
        { Part::Head,     false, 0,               -4, -8, -4,  8,  8,  8,   0,  0, 0.00f, "head" },
        { Part::Head,     true,  MPB::Hat,        -4, -8, -4,  8,  8,  8,  32,  0, 0.50f, "hat" },
        { Part::Body,     false, 0,               -4,  0, -2,  8, 12,  4,  16, 16, 0.00f, "body" },
        { Part::Body,     true,  MPB::Jacket,     -4,  0, -2,  8, 12,  4,  16, 32, 0.25f, "jacket" },
        { Part::RightArm, false, 0,               -3, -2, -2,  4, 12,  4,  40, 16, 0.00f, "right_arm" },
        { Part::RightArm, true,  MPB::RightSleeve,-3, -2, -2,  4, 12,  4,  40, 32, 0.25f, "right_sleeve" },
        { Part::LeftArm,  false, 0,               -1, -2, -2,  4, 12,  4,  32, 48, 0.00f, "left_arm" },
        { Part::LeftArm,  true,  MPB::LeftSleeve, -1, -2, -2,  4, 12,  4,  48, 48, 0.25f, "left_sleeve" },
        { Part::RightLeg, false, 0,               -2,  0, -2,  4, 12,  4,   0, 16, 0.00f, "right_leg" },
        { Part::RightLeg, true,  MPB::RightPants, -2,  0, -2,  4, 12,  4,   0, 32, 0.25f, "right_pants" },
        { Part::LeftLeg,  false, 0,               -2,  0, -2,  4, 12,  4,  16, 48, 0.00f, "left_leg" },
        { Part::LeftLeg,  true,  MPB::LeftPants,  -2,  0, -2,  4, 12,  4,   0, 48, 0.25f, "left_pants" },
    };

    // PlayerModel.createMesh(CubeDeformation.NONE, slim = true): 3-px arms.
    inline constexpr Box kSlim[] = {
        { Part::Head,     false, 0,               -4, -8, -4,  8,  8,  8,   0,  0, 0.00f, "head" },
        { Part::Head,     true,  MPB::Hat,        -4, -8, -4,  8,  8,  8,  32,  0, 0.50f, "hat" },
        { Part::Body,     false, 0,               -4,  0, -2,  8, 12,  4,  16, 16, 0.00f, "body" },
        { Part::Body,     true,  MPB::Jacket,     -4,  0, -2,  8, 12,  4,  16, 32, 0.25f, "jacket" },
        { Part::RightArm, false, 0,               -2, -2, -2,  3, 12,  4,  40, 16, 0.00f, "right_arm" },
        { Part::RightArm, true,  MPB::RightSleeve,-2, -2, -2,  3, 12,  4,  40, 32, 0.25f, "right_sleeve" },
        { Part::LeftArm,  false, 0,               -1, -2, -2,  3, 12,  4,  32, 48, 0.00f, "left_arm" },
        { Part::LeftArm,  true,  MPB::LeftSleeve, -1, -2, -2,  3, 12,  4,  48, 48, 0.25f, "left_sleeve" },
        { Part::RightLeg, false, 0,               -2,  0, -2,  4, 12,  4,   0, 16, 0.00f, "right_leg" },
        { Part::RightLeg, true,  MPB::RightPants, -2,  0, -2,  4, 12,  4,   0, 32, 0.25f, "right_pants" },
        { Part::LeftLeg,  false, 0,               -2,  0, -2,  4, 12,  4,  16, 48, 0.00f, "left_leg" },
        { Part::LeftLeg,  true,  MPB::LeftPants,  -2,  0, -2,  4, 12,  4,   0, 48, 0.25f, "left_pants" },
    };

    inline constexpr size_t kBoxCount = sizeof(kClassic) / sizeof(kClassic[0]);
    static_assert(sizeof(kSlim) / sizeof(kSlim[0]) == kBoxCount, "same parts in both models");

    inline const Box* Boxes(SkinModel model) {
        return model == SkinModel::Slim ? kSlim : kClassic;
    }

    // PlayerCapeModel.createCapeLayer: "cape" on the body, offset (0, 0, 2)
    // turned π about Y, one 10x16x1 box at texOffs(0, 0) sampled over a
    // 64x32 region (texScale 1 x 0.5 of the 64x64 layer).
    inline constexpr Box kCape =
        { Part::Body, false, MPB::Cape, -5, 0, -1, 10, 16, 1, 0, 0, 0.0f, "cape" };
    inline constexpr Pivot kCapeOffset{ 0.0f, 0.0f, 2.0f };
    inline constexpr float kCapeYRot = 3.1415927f;
    inline constexpr int   kCapeTexWidth = 64;
    inline constexpr int   kCapeTexHeight = 32;

    // MC PlayerModel.translateToHand's slim offset: the held item moves half
    // a pixel toward the body on a slim arm.
    inline constexpr float kSlimHandOffset = 0.5f;

} // namespace Game::PlayerLayout
