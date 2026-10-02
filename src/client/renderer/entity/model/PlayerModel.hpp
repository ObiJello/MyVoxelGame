// File: src/client/renderer/entity/model/PlayerModel.hpp
//
// MC net.minecraft.client.model.player.PlayerModel and PlayerCapeModel — the
// Minecraft player body a skin is drawn on, and the cape hung off its back.
//
// The mesh is Game::PlayerLayout's (common/entity/PlayerModelLayout.hpp):
// HumanoidModel's parts with the 64x64 skin layout — the left arm and leg on
// their own texels (no mirror), the second layer (hat, jacket, sleeves,
// pants) as children of the part they cover, so every pose moves both
// layers together. The slim model is the same with 3-px arms.
//
// setupAnim is HumanoidModel's (walk, sneak, seat, held-item poses, the
// swing, the idle bob) — PlayerModel.setupAnim only adds the part toggles
// and the spectator's head-only body — plus the two HumanoidModel branches
// only a player reaches in this engine: the elytra glide's lifted head
// (isFallFlying) and setupSwimAnimation (swimAmount).
#pragma once

#include "client/renderer/entity/model/EntityModels.hpp"
#include "common/entity/PlayerAppearance.hpp"

namespace Render {

    class PlayerModel : public HumanoidModel {
    public:
        explicit PlayerModel(bool slim);

        // AvatarRenderState's own fields, set before SetupAnim (the shared
        // EntityRenderState carries none of them): the skin's part toggles
        // (Game::ModelPartBits) and the spectator's floating head.
        void SetParts(uint8_t modelParts, bool spectator) {
            m_modelParts = modelParts;
            m_spectator = spectator;
        }

        void SetupAnim(const EntityRenderState& state) override;

        // MC PlayerModel.translateToHand: a slim arm's grip sits half a pixel
        // in toward the body.
        bool RightHandMatrix(glm::mat4& out) const override;
        bool LeftHandMatrix(glm::mat4& out) const override;

        // entityTranslucent: no culling (the default) — and blended, which
        // the renderer applies to the skin's batches.

        bool Slim() const { return m_slim; }

        // MC AvatarRenderer.renderHand: one arm (with or without its sleeve)
        // as the first-person hand draws it — resetPose, visible, zRot ∓0.1.
        // Returns the arm, posed, for the caller to Build.
        ModelPart& PrepareFirstPersonArm(bool right, bool sleeve);

        ModelPart* Body() const { return m_body; }

    private:
        // MC HumanoidModel.setupSwimAnimation.
        void SetupSwimAnimation(const EntityRenderState& state, float animationPos, float swimAmount);

        bool m_slim = false;
        uint8_t m_modelParts = Game::ModelPartBits::All;
        bool m_spectator = false;
        ModelPart* m_jacket = nullptr;
        ModelPart* m_leftSleeve = nullptr;
        ModelPart* m_rightSleeve = nullptr;
        ModelPart* m_leftPants = nullptr;
        ModelPart* m_rightPants = nullptr;
    };

    // MC PlayerCapeModel: the player model's skeleton with only the cape's
    // box on it — the cape hangs from the body, so it sways with the body's
    // sneak lean and swing twist — and the cape turned by the cape physics
    // (CapeLayer / AvatarRenderer.extractCapeState) on top. Its sheet is the
    // 64x32 cape texture.
    class PlayerCapeModel : public PlayerModel {
    public:
        PlayerCapeModel();

        // AvatarRenderState.capeFlap / capeLean / capeLean2 (degrees), set
        // before SetupAnim.
        void SetCape(float flap, float lean, float lean2) {
            m_capeFlap = flap;
            m_capeLean = lean;
            m_capeLean2 = lean2;
        }

        void SetupAnim(const EntityRenderState& state) override;

    private:
        ModelPart* m_cape = nullptr;
        float m_capeFlap = 0.0f;
        float m_capeLean = 0.0f;
        float m_capeLean2 = 0.0f;
    };

} // namespace Render
