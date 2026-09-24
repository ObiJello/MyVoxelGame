// File: src/common/world/block/ShapeOcclusion.cpp
#include "common/world/block/ShapeOcclusion.hpp"

#include <algorithm>
#include <vector>

namespace Game::Shapes {

    namespace {

        constexpr float kEps = 1.0e-4f;

        // The footprint, in the face's tangent coordinates, of every box that
        // touches the cell boundary at `face`.
        void FaceRects(const BlockRegistry::BlockShapeSet& set, Direction face,
                       std::vector<FaceRect>& out) {
            for (const BlockRegistry::BlockShape& b : set) {
                switch (face) {
                    case Direction::Up:
                        if (b.max.y >= 1.0f - kEps) out.push_back({b.min.x, b.min.z, b.max.x, b.max.z});
                        break;
                    case Direction::Down:
                        if (b.min.y <= kEps)        out.push_back({b.min.x, b.min.z, b.max.x, b.max.z});
                        break;
                    case Direction::East:
                        if (b.max.x >= 1.0f - kEps) out.push_back({b.min.y, b.min.z, b.max.y, b.max.z});
                        break;
                    case Direction::West:
                        if (b.min.x <= kEps)        out.push_back({b.min.y, b.min.z, b.max.y, b.max.z});
                        break;
                    case Direction::South:
                        if (b.max.z >= 1.0f - kEps) out.push_back({b.min.x, b.min.y, b.max.x, b.max.y});
                        break;
                    case Direction::North:
                        if (b.min.z <= kEps)        out.push_back({b.min.x, b.min.y, b.max.x, b.max.y});
                        break;
                }
            }
        }

        // Does the union of `rects` cover `target`? Cut `target` along every
        // rect edge that falls inside it and test each cell's centre.
        bool RectsCover(const std::vector<FaceRect>& rects, const FaceRect& target) {
            if (rects.empty()) return false;
            if (target.u1 - target.u0 <= kEps || target.v1 - target.v0 <= kEps) return true;
            std::vector<float> us{target.u0, target.u1}, vs{target.v0, target.v1};
            for (const FaceRect& r : rects) {
                us.push_back(std::clamp(r.u0, target.u0, target.u1));
                us.push_back(std::clamp(r.u1, target.u0, target.u1));
                vs.push_back(std::clamp(r.v0, target.v0, target.v1));
                vs.push_back(std::clamp(r.v1, target.v0, target.v1));
            }
            std::sort(us.begin(), us.end());
            std::sort(vs.begin(), vs.end());
            for (size_t i = 0; i + 1 < us.size(); ++i) {
                if (us[i + 1] - us[i] <= kEps) continue;
                const float cu = (us[i] + us[i + 1]) * 0.5f;
                for (size_t j = 0; j + 1 < vs.size(); ++j) {
                    if (vs[j + 1] - vs[j] <= kEps) continue;
                    const float cv = (vs[j] + vs[j + 1]) * 0.5f;
                    bool covered = false;
                    for (const FaceRect& r : rects) {
                        if (cu >= r.u0 - kEps && cu <= r.u1 + kEps &&
                            cv >= r.v0 - kEps && cv <= r.v1 + kEps) {
                            covered = true;
                            break;
                        }
                    }
                    if (!covered) return false;
                }
            }
            return true;
        }

    } // namespace

    FaceRect FluidFaceRect(Direction direction, float height) {
        switch (direction) {
            case Direction::Up:
            case Direction::Down:  return {0.0f, 0.0f, 1.0f, 1.0f};
            case Direction::East:
            case Direction::West:  return {0.0f, 0.0f, height, 1.0f};   // (y, z)
            default:               return {0.0f, 0.0f, 1.0f, height};   // (x, y)
        }
    }

    bool FaceCovers(const BlockRegistry::BlockShapeSet& occluder, Direction occluderFace,
                    const FaceRect& target) {
        std::vector<FaceRect> rects;
        rects.reserve(occluder.count);
        FaceRects(occluder, occluderFace, rects);
        return RectsCover(rects, target);
    }

    bool MergedFaceOccludes(const BlockRegistry::BlockShapeSet& first, bool firstFull,
                            const BlockRegistry::BlockShapeSet& second, bool secondFull,
                            Direction direction) {
        if (firstFull || secondFull) return true;
        std::vector<FaceRect> rects;
        rects.reserve(static_cast<size_t>(first.count) + second.count);
        FaceRects(first,  direction,           rects);
        FaceRects(second, Opposite(direction), rects);
        return RectsCover(rects, FaceRect{0.0f, 0.0f, 1.0f, 1.0f});
    }

} // namespace Game::Shapes
