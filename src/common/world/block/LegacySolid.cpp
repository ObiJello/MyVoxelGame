// File: src/common/world/block/LegacySolid.cpp
#include "common/world/block/LegacySolid.hpp"
#include "common/world/block/BlockRegistry.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>
#include <unordered_map>

namespace Game {

    namespace {
        enum class Force : uint8_t { None, On, Off };

        // One entry per BlockID, resolved from the generated slug list once.
        const std::array<Force, static_cast<size_t>(BlockID::Count)>& ForceTable() {
            static const auto table = [] {
                std::unordered_map<std::string_view, Force> bySlug;
#define LEGACY_SOLID(slug, forced) bySlug.emplace(slug, (forced) ? Force::On : Force::Off);
#include "GeneratedLegacySolid.inc"
#undef LEGACY_SOLID
                std::array<Force, static_cast<size_t>(BlockID::Count)> out{};
                for (size_t i = 0; i < out.size(); ++i) {
                    const auto it = bySlug.find(BlockRegistry::Get(static_cast<BlockID>(i)).registrySlug);
                    out[i] = it != bySlug.end() ? it->second : Force::None;
                }
                return out;
            }();
            return table;
        }
    }

    bool IsLegacySolid(BlockState state) {
        const auto index = static_cast<size_t>(state.Block());
        const auto& table = ForceTable();
        if (index < table.size()) {
            if (table[index] == Force::On)  return true;
            if (table[index] == Force::Off) return false;
        }
        // noCollision blocks (plants, air, fluids): the collision shape set
        // falls back to the OUTLINE shape, so the "empty collision" answer
        // has to come from HasCollision.
        if (!BlockRegistry::HasCollision(state.Block())) return false;
        const BlockRegistry::BlockShapeSet shapes = BlockRegistry::GetBlockCollisionShapeSet(state);
        if (shapes.count == 0) return false;
        glm::vec3 lo = shapes.boxes[0].min;
        glm::vec3 hi = shapes.boxes[0].max;
        for (const auto& box : shapes) {
            lo = glm::min(lo, box.min);
            hi = glm::max(hi, box.max);
        }
        const glm::vec3 size = hi - lo;
        if (size.x <= 0.0f || size.y <= 0.0f || size.z <= 0.0f) return false;   // shape.isEmpty()
        // AABB.getSize(): the average of the three sides.
        if ((size.x + size.y + size.z) / 3.0f >= 0.7291666666666666f) return true;
        return size.y >= 1.0f;
    }

} // namespace Game
