// File: src/common/entity/projectile/LlamaSpit.hpp
//
// MC net.minecraft.world.entity.projectile.LlamaSpit — gravity 0.06, drag
// 0.99, 1.0 damage, dissolves in water or inside solid blocks. Same
// Mob-pipeline deviation as Arrow.hpp documents.
#pragma once

#include "common/entity/projectile/Projectile.hpp"

namespace Game {

    class LlamaSpit : public Projectile {
    public:
        explicit LlamaSpit(EntityLevel* level)
            : Projectile(EntityTypeId::LlamaSpit, level) {}

        // MC LlamaSpit(level, llama): spawn just ahead of the llama's mouth —
        // (bbWidth + 1) * 0.5 along the body facing, eye height minus 0.1.
        void InitFromLlama(LivingEntity& llama);

        static constexpr double kGravity = 0.06;

        void Tick() override;

    protected:
        void OnHitEntity(LivingEntity& target) override;
        void OnHitBlock(const HitResult& hit) override;
    };

} // namespace Game
