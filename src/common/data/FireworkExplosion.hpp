// File: src/common/data/FireworkExplosion.hpp
//
// MC net.minecraft.world.item.component.FireworkExplosion — the record a
// firework star carries (DataComponents.FIREWORK_EXPLOSION) and a rocket's
// FIREWORKS component lists, and what FireworkParticles.Starter draws.
//
// Standalone (no engine includes) because two unrelated layers hold it: the
// item component system (DataComponents.hpp) and the particle options the
// explosion's Starter rides to the client (ParticleOptions.hpp).
#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace Game {

    struct FireworkExplosion {
        // MC FireworkExplosion.Shape — ids and serialized names verbatim
        // (ByIdMap.continuous, OutOfBoundsStrategy.ZERO: an unknown id reads
        // as SMALL_BALL).
        enum class Shape : uint8_t { SmallBall = 0, LargeBall, Star, Creeper, Burst };
        static constexpr int kShapeCount = 5;

        Shape                 shape = Shape::SmallBall;
        std::vector<int32_t>  colors;        // RGB
        std::vector<int32_t>  fadeColors;    // RGB
        bool                  hasTrail = false;
        bool                  hasTwinkle = false;

        // Record equality (MC's generated equals) — what the rocket tooltip's
        // "N x Shape" run-length grouping compares.
        bool operator==(const FireworkExplosion& o) const {
            return shape == o.shape && colors == o.colors && fadeColors == o.fadeColors &&
                   hasTrail == o.hasTrail && hasTwinkle == o.hasTwinkle;
        }
        bool operator!=(const FireworkExplosion& o) const { return !(*this == o); }

        // MC Shape.byId (out of range -> SMALL_BALL).
        static Shape ShapeById(int id) {
            return (id >= 0 && id < kShapeCount) ? static_cast<Shape>(id) : Shape::SmallBall;
        }
        // MC Shape.getSerializedName.
        static std::string_view ShapeName(Shape s) {
            switch (s) {
                case Shape::SmallBall: return "small_ball";
                case Shape::LargeBall: return "large_ball";
                case Shape::Star:      return "star";
                case Shape::Creeper:   return "creeper";
                case Shape::Burst:     return "burst";
            }
            return "small_ball";
        }
        // MC Shape.CODEC (StringRepresentable.fromValues): false for a name
        // no shape has.
        static bool ShapeFromName(std::string_view name, Shape& out) {
            if (name.rfind("minecraft:", 0) == 0) name.remove_prefix(10);
            for (int i = 0; i < kShapeCount; ++i) {
                if (ShapeName(static_cast<Shape>(i)) == name) { out = static_cast<Shape>(i); return true; }
            }
            return false;
        }
    };

} // namespace Game
