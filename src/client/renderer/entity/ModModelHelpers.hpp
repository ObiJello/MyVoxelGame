// File: src/client/renderer/entity/ModModelHelpers.hpp
//
// Shared pieces of the Twilight Forest / Aether creature models
// (ModMobRender.hpp): the mods' QuadrupedModel / HumanoidModel setupAnim
// ports over generated meshes, and MC's sheep wool tint table (bighorn and
// sheepuff wool). Header-only; each render module includes it.
#pragma once

#include "client/renderer/entity/model/EntityModels.hpp"

#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <string_view>
#include <vector>

namespace Render {

    inline constexpr float kModPi = 3.14159265f;
    inline constexpr float kModDegToRad = kModPi / 180.0f;

    // MC QuadrupedModel.setupAnim — TF DeerModel / BighornModel (via
    // SheepModel) and the Aether's SheepuffModel / SheepuffWoolModel.
    // `adultOnly` parts are hidden on a baby (DeerModel's antlers,
    // BighornModel's horns); `grazes` adds SheepModel's graze pose
    // (head.y += eatPosition * 9 * ageScale, head.xRot = eatAngle —
    // which is the look pitch while not eating).
    class ModQuadrupedModel : public GeneratedModel {
    public:
        ModQuadrupedModel(std::string_view slug,
                          std::initializer_list<const char*> adultOnly, bool grazes)
            : GeneratedModel(slug), m_grazes(grazes) {
            m_head = m_root.Find("head");
            m_rightHind = m_root.Find("right_hind_leg");
            m_leftHind = m_root.Find("left_hind_leg");
            m_rightFront = m_root.Find("right_front_leg");
            m_leftFront = m_root.Find("left_front_leg");
            for (const char* name : adultOnly) {
                if (ModelPart* p = m_root.Find(name)) m_adultOnly.push_back(p);
            }
        }

        void SetupAnim(const EntityRenderState& state) override {
            m_root.ResetPose();
            for (ModelPart* p : m_adultOnly) p->visible = !state.isBaby;
            if (m_head) {
                m_head->xRot = state.xRot * kModDegToRad;
                m_head->yRot = state.yRot * kModDegToRad;
            }
            const float pos = state.walkAnimationPos * 0.6662f;
            const float amt = 1.4f * state.walkAnimationSpeed;
            if (m_rightHind)  m_rightHind->xRot  = std::cos(pos) * amt;
            if (m_leftHind)   m_leftHind->xRot   = std::cos(pos + kModPi) * amt;
            if (m_rightFront) m_rightFront->xRot = std::cos(pos + kModPi) * amt;
            if (m_leftFront)  m_leftFront->xRot  = std::cos(pos) * amt;
            if (m_grazes && m_head) {
                m_head->y += state.headEatPositionScale * 9.0f * state.ageScale;
                m_head->xRot = state.headEatAngleScale;
            }
        }

    private:
        bool m_grazes;
        ModelPart* m_head = nullptr;
        ModelPart* m_rightHind = nullptr;
        ModelPart* m_leftHind = nullptr;
        ModelPart* m_rightFront = nullptr;
        ModelPart* m_leftFront = nullptr;
        std::vector<ModelPart*> m_adultOnly;
    };

    // MC HumanoidModel.setupAnim's walk (head look, the arm/leg swing and
    // the idle arm bob) — TF RedcapModel; with `koboldArms`, TF
    // KoboldModel's override: arms held forward at -0.15*PI, no bob. The
    // attack swing (setupAttackAnimation) is not posed.
    class ModHumanoidModel : public GeneratedModel {
    public:
        ModHumanoidModel(std::string_view slug, bool koboldArms)
            : GeneratedModel(slug), m_koboldArms(koboldArms) {
            m_head = m_root.Find("head");
            m_rightArm = m_root.Find("right_arm");
            m_leftArm = m_root.Find("left_arm");
            m_rightLeg = m_root.Find("right_leg");
            m_leftLeg = m_root.Find("left_leg");
        }

        void SetupAnim(const EntityRenderState& state) override {
            m_root.ResetPose();
            if (m_head) {
                m_head->xRot = state.xRot * kModDegToRad;
                m_head->yRot = state.yRot * kModDegToRad;
            }
            const float pos = state.walkAnimationPos * 0.6662f;
            const float spd = state.walkAnimationSpeed;
            if (m_rightLeg) m_rightLeg->xRot = std::cos(pos) * 1.4f * spd;
            if (m_leftLeg)  m_leftLeg->xRot  = std::cos(pos + kModPi) * 1.4f * spd;
            if (!m_rightArm || !m_leftArm) return;
            if (m_koboldArms) {
                m_rightArm->xRot = m_leftArm->xRot = -(kModPi * 0.15f);
                m_rightArm->zRot = m_leftArm->zRot = 0.0f;
                return;
            }
            m_rightArm->xRot = std::cos(pos + kModPi) * 2.0f * spd * 0.5f;
            m_leftArm->xRot  = std::cos(pos) * 2.0f * spd * 0.5f;
            // AnimationUtils.bobModelPart.
            const float bobZ = std::cos(state.ageInTicks * 0.09f) * 0.05f + 0.05f;
            const float bobX = std::sin(state.ageInTicks * 0.067f) * 0.05f;
            m_rightArm->zRot += bobZ;
            m_leftArm->zRot  -= bobZ;
            m_rightArm->xRot += bobX;
            m_leftArm->xRot  -= bobX;
        }

    private:
        bool m_koboldArms;
        ModelPart* m_head = nullptr;
        ModelPart* m_rightArm = nullptr;
        ModelPart* m_leftArm = nullptr;
        ModelPart* m_rightLeg = nullptr;
        ModelPart* m_leftLeg = nullptr;
    };

    // MC ColorLerper.Type.SHEEP — the wool tint per dye colour.
    //
    // NOT DyeColor's raw textureDiffuseColor: SHEEP is registered with a
    // brightness of 0.75, so every channel is floored to three quarters
    // (ColorLerper.getModifiedColor). White is the one special case and is
    // hardcoded to 0xE6E6E6 rather than scaled.
    //
    // These MULTIPLY the wool texture, exactly as MC's
    // renderColoredCutoutModel passes them as the model's vertex colour.
    // Using them as a flat replacement colour is what makes a sheep one
    // solid bright blob with no wool shading at all.
    struct SheepWoolColor { uint8_t r, g, b; };
    inline constexpr SheepWoolColor kSheepWoolColors[16] = {
        { 230, 230, 230 },  // white
        { 186,  96,  21 },  // orange
        { 149,  58, 141 },  // magenta
        {  43, 134, 163 },  // light_blue
        { 190, 162,  45 },  // yellow
        {  96, 149,  23 },  // lime
        { 182, 104, 127 },  // pink
        {  53,  59,  61 },  // gray
        { 117, 117, 113 },  // light_gray
        {  16, 117, 117 },  // cyan
        { 102,  37, 138 },  // purple
        {  45,  51, 127 },  // blue
        {  98,  63,  37 },  // brown
        {  70,  93,  16 },  // green
        { 132,  34,  28 },  // red
        {  21,  21,  24 },  // black
    };

    // MC ColorLerper.getLerpedColor(ColorLerper.Type.SHEEP, tick) — the
    // rainbow sheep's wool. SHEEP cycles every DyeColor in ordinal order
    // (DyeColor.values(), the order of the table above), 25 ticks per
    // colour, and each step is ARGB.srgbLerp — Mth.lerpInt per channel, in
    // sRGB — between two of the SHEEP colours above (white's hardcoded
    // 0xE6E6E6 included). SheepRenderState.getWoolColor feeds it the
    // sheep's ageInTicks with no per-entity offset, so every rainbow sheep
    // in view changes in step.
    inline SheepWoolColor SheepLerpedWoolColor(float tick) {
        constexpr int kColorDuration = 25;
        constexpr int kColorCount = 16;
        const float floored = std::floor(tick);
        const int tickCount = static_cast<int>(floored);
        const int value = tickCount / kColorDuration;
        const SheepWoolColor& c1 = kSheepWoolColors[value % kColorCount];
        const SheepWoolColor& c2 = kSheepWoolColors[(value + 1) % kColorCount];
        const float subStep = (static_cast<float>(tickCount % kColorDuration) + (tick - floored)) /
                              static_cast<float>(kColorDuration);
        // Mth.lerpInt: p0 + floor(alpha * (p1 - p0)).
        const auto lerpInt = [subStep](uint8_t p0, uint8_t p1) {
            return static_cast<uint8_t>(
                p0 + static_cast<int>(std::floor(subStep * static_cast<float>(p1 - p0))));
        };
        return { lerpInt(c1.r, c2.r), lerpInt(c1.g, c2.g), lerpInt(c1.b, c2.b) };
    }

} // namespace Render
