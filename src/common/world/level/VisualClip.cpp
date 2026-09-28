// File: src/common/world/level/VisualClip.cpp
#include "VisualClip.hpp"

#include "BlockClip.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/chunk/IBlockAccess.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace Game {

    namespace {

        // How a block's VISUAL shape relates to its collision shape.
        enum class VisualKind : uint8_t {
            Collision,   // BlockBehaviour's default: getCollisionShape
            Empty,       // TransparentBlock, IronBarsBlock, PowderSnowBlock
            FullBlock,   // MudBlock, SoulSandBlock
            Outline,     // FenceBlock (getShape), SnowLayerBlock (SHAPES[layers])
        };

        bool EndsWith(std::string_view s, std::string_view suffix) {
            return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
        }

        VisualKind ClassifySlug(std::string_view slug) {
            // TransparentBlock and its subclasses (Blocks.GLASS, StainedGlass-
            // Block, TintedGlassBlock, WaterloggedTransparentBlock →
            // WeatheringCopperGrateBlock and the waxed grates).
            if (slug == "glass" || EndsWith(slug, "stained_glass") || slug == "tinted_glass" ||
                EndsWith(slug, "copper_grate")) {
                return VisualKind::Empty;
            }
            // IronBarsBlock and its subclasses (GLASS_PANE, StainedGlassPane-
            // Block, WeatheringCopperBarsBlock and the waxed bars).
            if (slug == "iron_bars" || EndsWith(slug, "glass_pane") || EndsWith(slug, "copper_bars")) {
                return VisualKind::Empty;
            }
            if (slug == "powder_snow") return VisualKind::Empty;
            if (slug == "mud" || slug == "soul_sand") return VisualKind::FullBlock;
            // FenceBlock (every wood fence and the nether brick fence — not
            // the fence GATES, which are FenceGateBlock) and SnowLayerBlock.
            if (EndsWith(slug, "_fence") || slug == "snow") return VisualKind::Outline;
            return VisualKind::Collision;
        }

        VisualKind KindOf(BlockID id) {
            static const std::vector<VisualKind> kinds = [] {
                std::vector<VisualKind> out(static_cast<size_t>(BlockID::Count), VisualKind::Collision);
                for (size_t i = 1; i < out.size(); ++i) {
                    out[i] = ClassifySlug(BlockRegistry::Get(static_cast<BlockID>(i)).registrySlug);
                }
                return out;
            }();
            const size_t i = static_cast<size_t>(id);
            return i < kinds.size() ? kinds[i] : VisualKind::Collision;
        }

        // The slab test with the entry parameter (MC AABB.clip's nearest t).
        bool SegmentEntry(const glm::dvec3& a, const glm::dvec3& d, const glm::dvec3& boxMin,
                          const glm::dvec3& boxMax, double& outT) {
            double tEnter = 0.0;
            double tExit  = 1.0;
            for (int axis = 0; axis < 3; ++axis) {
                const double da = d[axis];
                if (std::abs(da) < 1.0e-12) {
                    if (a[axis] < boxMin[axis] || a[axis] > boxMax[axis]) return false;
                    continue;
                }
                const double inv = 1.0 / da;
                double t0 = (boxMin[axis] - a[axis]) * inv;
                double t1 = (boxMax[axis] - a[axis]) * inv;
                if (t0 > t1) std::swap(t0, t1);
                tEnter = std::max(tEnter, t0);
                tExit  = std::min(tExit, t1);
                if (tEnter > tExit) return false;
            }
            outT = tEnter;
            return true;
        }

    } // namespace

    bool HasEmptyVisualShape(BlockState state) {
        return KindOf(state.Block()) == VisualKind::Empty;
    }

    VisualClipResult ClipVisual(const IBlockAccess& blocks, const glm::dvec3& from, const glm::dvec3& to) {
        VisualClipResult result;
        glm::ivec3 hitCell{0};
        double hitT = 0.0;
        const bool hit = TraverseBlocks(from, to,
            [&](const glm::ivec3& c, const glm::dvec3& a, const glm::dvec3& d) {
                const BlockState st = blocks.GetBlockState(c.x, c.y, c.z);
                const VisualKind kind = KindOf(st.Block());
                if (kind == VisualKind::Empty) return false;
                const glm::dvec3 cellOrigin(static_cast<double>(c.x), static_cast<double>(c.y),
                                            static_cast<double>(c.z));
                double best = std::numeric_limits<double>::max();
                bool any = false;
                const auto test = [&](const glm::vec3& mn, const glm::vec3& mx) {
                    double t = 0.0;
                    if (SegmentEntry(a, d, cellOrigin + glm::dvec3(mn), cellOrigin + glm::dvec3(mx), t)) {
                        any = true;
                        best = std::min(best, t);
                    }
                };
                switch (kind) {
                    case VisualKind::FullBlock:
                        test(glm::vec3(0.0f), glm::vec3(1.0f));
                        break;
                    case VisualKind::Outline:
                        for (const auto& box : BlockRegistry::GetBlockShapeSet(st)) test(box.min, box.max);
                        break;
                    case VisualKind::Collision:
                    default:
                        // AirBlock / LiquidBlock and every no-collision block:
                        // Shapes.empty(), no shape fetch.
                        if (!BlockRegistry::HasCollision(st.Block())) return false;
                        for (const auto& box : BlockRegistry::GetBlockCollisionShapeSet(st)) test(box.min, box.max);
                        break;
                }
                if (any) hitT = best;
                return any;
            },
            &hitCell);
        if (hit) {
            result.hit = true;
            result.blockPos = hitCell;
            // The walk's segment is `from` nudged a hair outward; the entry
            // parameter is along it, which places the point on the face.
            const glm::dvec3 a = from - (to - from) * 1.0e-7;
            const glm::dvec3 b = to - (from - to) * 1.0e-7;
            result.location = a + (b - a) * hitT;
            return result;
        }
        result.blockPos = glm::ivec3(static_cast<int>(std::floor(to.x)), static_cast<int>(std::floor(to.y)),
                                     static_cast<int>(std::floor(to.z)));
        result.location = to;
        return result;
    }

} // namespace Game
