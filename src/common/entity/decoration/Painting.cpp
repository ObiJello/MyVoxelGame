// File: src/common/entity/decoration/Painting.cpp
#include "common/entity/decoration/Painting.hpp"
#include "common/entity/decoration/PaintingVariants.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/data/DataComponents.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/sound/SoundEvents.hpp"

#include <algorithm>
#include <vector>

namespace Game {

    namespace {
        // MC Painting.offsetForPaintingSize: an even span is centred on the
        // line between two cells, so the canvas shifts half a block.
        double OffsetForPaintingSize(int size) { return size % 2 == 0 ? 0.5 : 0.0; }
    }

    Painting::Painting(EntityLevel* level)
        : HangingEntity(EntityTypeId::Painting, level) {
        RecalculateBoundingBox();
    }

    const PaintingVariant* Painting::Variant() const {
        if (const PaintingVariant* v = PaintingVariants::Get(m_variant)) return v;
        return PaintingVariants::Get(0);
    }

    float Painting::BaseBbWidth() const {
        const PaintingVariant* v = Variant();
        return v ? static_cast<float>(v->width) : TypeInfo().width;
    }

    float Painting::BaseBbHeight() const {
        const PaintingVariant* v = Variant();
        return v ? static_cast<float>(v->height) : TypeInfo().height;
    }

    AABBd Painting::CalculateBoundingBox(const glm::ivec3& pos, Direction direction) const {
        // MC Painting.calculateBoundingBox.
        const PaintingVariant* v = Variant();
        const int width  = v ? v->width : 1;
        const int height = v ? v->height : 1;
        const glm::dvec3 attachedToWall = glm::dvec3(pos) + glm::dvec3(0.5) - Step(direction) * kShiftToBlockWall;
        const glm::dvec3 center = attachedToWall + Step(CounterClockWise(direction)) * OffsetForPaintingSize(width) +
                                  glm::dvec3(0.0, OffsetForPaintingSize(height), 0.0);
        const bool alongX = AxisOf(direction) == Axis::X;
        const glm::dvec3 size(alongX ? kDepth : static_cast<double>(width),
                              static_cast<double>(height),
                              alongX ? static_cast<double>(width) : kDepth);
        return AABBd::FromMinMax(center - size * 0.5, center + size * 0.5);
    }

    void Painting::SetVariant(int variantIndex) {
        m_variant = PaintingVariants::Get(variantIndex) ? variantIndex : 0;
        RecalculateBoundingBox();
    }

    void Painting::SetVariantByte(uint8_t v) {
        m_variant = PaintingVariants::Get(v) ? v : 0;
        SyncFromNetwork();
    }

    std::unique_ptr<Painting> Painting::Create(EntityLevel* level, const glm::ivec3& pos,
                                               Direction direction, JavaRandom& random) {
        // MC Painting.create.
        const std::vector<int>& placeable = PaintingVariants::Placeable();
        if (placeable.empty()) return nullptr;

        auto candidate = std::make_unique<Painting>(level);
        candidate->m_pos = pos;
        candidate->SetDirection(direction);

        std::vector<int> fitting;
        for (int variant : placeable) {
            candidate->SetVariant(variant);
            if (candidate->Survives()) fitting.push_back(variant);
        }
        if (fitting.empty()) return nullptr;

        int largest = 0;
        for (int variant : fitting) largest = std::max(largest, PaintingVariants::Get(variant)->Area());
        fitting.erase(std::remove_if(fitting.begin(), fitting.end(),
                                     [largest](int v) { return PaintingVariants::Get(v)->Area() < largest; }),
                      fitting.end());

        // Util.getRandomSafe: list.get(random.nextInt(size)).
        const int chosen = fitting[static_cast<size_t>(random.NextInt(static_cast<int>(fitting.size())))];
        candidate->SetVariant(chosen);
        candidate->SetDirection(direction);
        return candidate;
    }

    void Painting::DropItem(Entity* causedBy) {
        // MC Painting.dropItem.
        if (!m_level || !m_level->DoEntityDrops()) return;
        PlaySound(SoundEvents::PAINTING_BREAK, 1.0f, 1.0f);
        if (causedBy && causedBy->IsPlayer() && causedBy->IsCreative()) return;
        ItemStack stack = PickResult();
        if (const auto& name = GetCustomName()) stack.components.set(DataComponents::CUSTOM_NAME, *name);
        SpawnAtLocation(stack);
    }

    void Painting::PlayPlacementSound() {
        PlaySound(SoundEvents::PAINTING_PLACE, 1.0f, 1.0f);
    }

} // namespace Game
