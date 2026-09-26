// File: src/common/entity/decoration/Painting.hpp
//
// MC net.minecraft.world.entity.decoration.painting.Painting — a canvas of a
// data-driven variant (PaintingVariants) hung on a wall. The hanging half —
// cell, facing, survival, breaking — is HangingEntity's.
//
// GEOMETRY (MC Painting.calculateBoundingBox): width × height blocks, 1/16
// deep, flush against the wall, shifted half a block left / up for an even
// width / height.
//
// WIRE: the variant rides the variant byte (its PaintingVariants index); the
// facing, yRot (HangingEntity).
//
// SAVE (MC 26.3): "facing" (2D data value, byte), "block_pos" ([I; x, y, z]),
// "variant" (the variant id) — see EntityNbt.
#pragma once

#include "common/entity/decoration/HangingEntity.hpp"
#include "common/entity/GeneratedItemList.hpp"

#include <memory>

namespace Game {

    class JavaRandom;
    struct PaintingVariant;

    class Painting final : public HangingEntity {
    public:
        explicit Painting(EntityLevel* level);

        // MC Painting.DEPTH.
        static constexpr double kDepth = 0.0625;

        // MC Painting.create(level, pos, direction): the painting a plain
        // painting item puts on that wall — among the #placeable variants
        // that survive there, the largest by area, one of those at random.
        // Null when none fits.
        static std::unique_ptr<Painting> Create(EntityLevel* level, const glm::ivec3& pos,
                                                Direction direction, JavaRandom& random);

        int VariantIndex() const { return m_variant; }
        // The variant, never null while the registry has any.
        const PaintingVariant* Variant() const;
        void SetVariant(int variantIndex);

        // MC Painting.dropItem: the break sound, then a painting item
        // (carrying this painting's name) unless entity drops are off or a
        // creative player broke it.
        void DropItem(Entity* causedBy) override;
        void PlayPlacementSound() override;

        // DELIBERATE DIVERGENCE FROM MC (by request): placing a painting
        // still needs the whole wall behind it (Survives, MC's rule), but a
        // painting already hung stays up while ANY block behind it still
        // supports it — so blocks can be dug out from behind a painting for
        // a hidden doorway. MC pops it on the next 100-tick check.
        bool StillHangs() const override { return SurvivesWith(/*requireFullSupport=*/false); }

        // MC getPickResult.
        static ItemStack PickResult() { return ItemStack(Items::Painting, 1); }

        // The canvas's extents, for the distance / frustum culls that read
        // the entity's size (the real box is the fixed one).
        float BaseBbWidth() const override;
        float BaseBbHeight() const override;

        uint8_t GetVariantByte() const override { return static_cast<uint8_t>(m_variant); }
        // Client: the variant arrives after the centre and yRot; then the
        // facing, cell and box are rebuilt from them.
        void SetVariantByte(uint8_t v) override;

    protected:
        AABBd CalculateBoundingBox(const glm::ivec3& pos, Direction direction) const override;

    private:
        int m_variant = 0;
    };

} // namespace Game
