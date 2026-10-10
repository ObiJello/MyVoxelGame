// File: src/common/world/level/VeinMine.cpp
// See the header for the shapes, the size rule and the timing.

#include "VeinMine.hpp"

#include "common/entity/Item.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/block/MiningSpeed.hpp"
#include "common/world/block/entity/BlockEntityTypes.hpp"
#include "common/world/chunk/Chunk.hpp"   // IVec3Hash
#include "common/world/chunk/IBlockAccess.hpp"

#include <algorithm>
#include <cstdlib>
#include <queue>
#include <unordered_set>

namespace Game::VeinMine {

    Settings Settings::Clamped() const {
        Settings s = *this;
        if (static_cast<int>(s.shape) >= static_cast<int>(Shape::Count)) s.shape = Shape::Vein;
        s.width  = static_cast<uint8_t>(std::clamp<int>(s.width,  kMinSize, kMaxSize));
        s.height = static_cast<uint8_t>(std::clamp<int>(s.height, kMinSize, kMaxSize));
        s.length = static_cast<uint8_t>(std::clamp<int>(s.length, kMinSize, kMaxLength));
        return s;
    }

    const char* ShapeName(Shape shape) {
        switch (shape) {
            case Shape::Vein:       return "Vein";
            case Shape::Tunnel:     return "Tunnel";
            case Shape::StairsDown: return "Stairs Down";
            case Shape::StairsUp:   return "Stairs Up";
            case Shape::ShaftDown:  return "Shaft Down";
            case Shape::Count:      break;
        }
        return "Vein";
    }

    Shape NextShape(Shape shape) {
        const int next = (static_cast<int>(shape) + 1) % static_cast<int>(Shape::Count);
        return static_cast<Shape>(next);
    }

    bool ShapeTakes(BlockState state) {
        const BlockID id = state.Block();
        if (id == BlockID::Air) return false;
        if (id == BlockID::Water || id == BlockID::Lava || id == BlockID::ResonantWater ||
            id == BlockID::BubbleColumn) {
            return false;
        }
        if (BlockRegistry::Get(id).destroyTime < 0.0f) return false;   // bedrock, barriers, ...
        if (BlockEntityTypes::HasBlockEntity(id)) return false;        // chests, furnaces, signs, ...
        return true;
    }

    bool CanHarvest(const ItemStack* harvestTool, BlockID block) {
        return !harvestTool || HasCorrectToolForDrops(*harvestTool, BlockRegistry::Get(block));
    }

    namespace {

        glm::ivec3 Vec(Direction d) { return glm::ivec3(StepX(d), StepY(d), StepZ(d)); }

        int Chebyshev(const glm::ivec3& d) {
            return std::max({std::abs(d.x), std::abs(d.y), std::abs(d.z)});
        }

        // RaycastHit::hitFace → the face's outward normal; nullopt-ish zero
        // for an unknown face.
        glm::ivec3 FaceNormal(int face) {
            static const glm::ivec3 kNormals[6] = {
                { 1, 0, 0}, {-1, 0, 0}, {0,  1, 0}, {0, -1, 0}, {0, 0,  1}, {0, 0, -1}
            };
            return (face >= 0 && face < 6) ? kNormals[face] : glm::ivec3(0);
        }

        // The slices of a sized shape: slice k is centred on origin + step*k
        // and spans width along `right`, height along `up`.
        struct Frame {
            glm::ivec3 step{0}, right{0}, up{0};
        };

        Frame FrameFor(Shape shape, int face, Direction facing) {
            if (!IsHorizontal(facing)) facing = Direction::North;
            const glm::ivec3 fwd   = Vec(facing);
            const glm::ivec3 right = Vec(ClockWise(facing));
            const glm::ivec3 worldUp(0, 1, 0);
            Frame f;
            switch (shape) {
                case Shape::Tunnel: {
                    // Into the dug face (its normal points back at the player).
                    const glm::ivec3 normal = FaceNormal(face);
                    const glm::ivec3 depth  = normal == glm::ivec3(0) ? fwd : -normal;
                    f.step = depth;
                    if (depth.y == 0) {
                        // A wall: right of the dig, world up.
                        const Direction d = depth.x > 0 ? Direction::East : depth.x < 0 ? Direction::West
                                          : depth.z > 0 ? Direction::South : Direction::North;
                        f.right = Vec(ClockWise(d));
                        f.up    = worldUp;
                    } else {
                        // The floor or the ceiling: the screen's up is the
                        // player's forward looking down, backward looking up.
                        f.right = right;
                        f.up    = depth.y < 0 ? fwd : -fwd;
                    }
                    break;
                }
                case Shape::StairsDown: f.step = fwd - worldUp; f.right = right; f.up = worldUp; break;
                case Shape::StairsUp:   f.step = fwd + worldUp; f.right = right; f.up = worldUp; break;
                case Shape::ShaftDown:  f.step = -worldUp;      f.right = right; f.up = fwd;     break;
                case Shape::Vein:
                case Shape::Count:      break;
            }
            return f;
        }

        // The middle-cell rule: the dug block is the middle, an even size
        // grows on the positive (right / up) side.
        int LowOffset(int size) { return -((size - 1) / 2); }

        // The shape's cells holding `kind`.
        std::vector<Target> CollectSized(const IBlockAccess& level, const glm::ivec3& origin, int face,
                                         Direction facing, const Settings& s, BlockID kind) {
            const Frame f = FrameFor(s.shape, face, facing);
            const int lowW = LowOffset(s.width),  highW = lowW + s.width - 1;
            const int lowH = LowOffset(s.height), highH = lowH + s.height - 1;
            std::unordered_set<glm::ivec3, IVec3Hash> seen;
            std::vector<Target> out;
            for (int k = 0; k < s.length; ++k) {
                const glm::ivec3 centre = origin + f.step * k;
                for (int v = lowH; v <= highH; ++v) {
                    for (int u = lowW; u <= highW; ++u) {
                        const glm::ivec3 p = centre + f.right * u + f.up * v;
                        if (p == origin || !seen.insert(p).second) continue;
                        const BlockState state = level.GetBlockState(p.x, p.y, p.z);
                        if (state.Block() != kind || !ShapeTakes(state)) continue;
                        out.push_back({p, Chebyshev(p - origin)});
                    }
                }
            }
            // Nearest first — the order the spread breaks them in: by ring,
            // then by true distance, so each ring goes out from the middle
            // rather than sweeping across from a corner.
            const auto dist2 = [&](const Target& t) {
                const glm::ivec3 d = t.pos - origin;
                return d.x * d.x + d.y * d.y + d.z * d.z;
            };
            std::stable_sort(out.begin(), out.end(), [&](const Target& a, const Target& b) {
                if (a.ring != b.ring) return a.ring < b.ring;
                return dist2(a) < dist2(b);
            });
            return out;
        }

        std::vector<Target> CollectVein(const IBlockAccess& level, const glm::ivec3& origin, BlockID kind,
                                        int face, Direction facing, const Settings& s) {
            // An unloaded cell reads as air and simply ends the walk there.
            const auto isKind = [&](const glm::ivec3& p) { return level.GetBlock(p.x, p.y, p.z) == kind; };

            // Breadth-first over the 26-neighbourhood (ore veins are
            // generated with diagonal contact, so face adjacency alone would
            // split most of them), stopping as soon as the cluster is known
            // to be ground (kVeinMassBlocks). The origin seeds the walk and
            // is never counted. The ring is the BFS depth.
            std::unordered_set<glm::ivec3, IVec3Hash> seen;
            std::vector<Target> cluster;
            std::queue<Target> frontier;
            seen.insert(origin);
            frontier.push({origin, 0});
            while (!frontier.empty() && static_cast<int>(cluster.size()) <= kVeinMassBlocks) {
                const Target at = frontier.front();
                frontier.pop();
                for (int dx = -1; dx <= 1; ++dx)
                for (int dy = -1; dy <= 1; ++dy)
                for (int dz = -1; dz <= 1; ++dz) {
                    if (dx == 0 && dy == 0 && dz == 0) continue;
                    const glm::ivec3 next = at.pos + glm::ivec3(dx, dy, dz);
                    if (!seen.insert(next).second) continue;
                    if (!isKind(next)) continue;
                    cluster.push_back({next, at.ring + 1});
                    frontier.push({next, at.ring + 1});
                }
            }
            if (static_cast<int>(cluster.size()) <= kVeinMassBlocks) return cluster;

            // Ground: the Tunnel shape at the chosen size, into the dug face.
            Settings tunnel = s;
            tunnel.shape = Shape::Tunnel;
            return CollectSized(level, origin, face, facing, tunnel, kind);
        }

    } // namespace

    std::vector<Target> CollectTargets(const IBlockAccess& level, const glm::ivec3& origin, BlockID kind,
                                       int face, Direction facing, const Settings& settings,
                                       const ItemStack* harvestTool) {
        if (kind == BlockID::Air) return {};
        // A tool that cannot harvest the dug block does a plain dig.
        if (!CanHarvest(harvestTool, kind)) return {};
        const Settings s = settings.Clamped();
        if (s.shape == Shape::Vein) return CollectVein(level, origin, kind, face, facing, s);
        return CollectSized(level, origin, face, facing, s, kind);
    }

} // namespace Game::VeinMine
