// File: src/common/world/portal/ImmersiveFrame.cpp
#include "common/core/Features.hpp"
#if ENABLE_IMMERSIVE_PORTALS

#include "ImmersiveFrame.hpp"

#include "common/world/chunk/IBlockAccess.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace Game::Immersive {

    namespace {

        struct IVec3Hash {
            size_t operator()(const glm::ivec3& v) const noexcept {
                uint64_t h = (static_cast<uint64_t>(static_cast<uint32_t>(v.x)) << 42) ^
                             (static_cast<uint64_t>(static_cast<uint32_t>(v.y)) << 21) ^
                             static_cast<uint32_t>(v.z);
                h ^= h >> 33; h *= 0xff51afd7ed558ccdULL; h ^= h >> 33;
                return static_cast<size_t>(h);
            }
        };

        // The four in-plane directions for a plane whose normal is `axis`.
        void InPlaneDirections(Axis axis, glm::ivec3 out[4]) {
            switch (axis) {
                case Axis::X: out[0] = { 0, 1, 0}; out[1] = { 0,-1, 0}; out[2] = { 0, 0, 1}; out[3] = { 0, 0,-1}; break;
                case Axis::Y: out[0] = { 1, 0, 0}; out[1] = {-1, 0, 0}; out[2] = { 0, 0, 1}; out[3] = { 0, 0,-1}; break;
                case Axis::Z: out[0] = { 1, 0, 0}; out[1] = {-1, 0, 0}; out[2] = { 0, 1, 0}; out[3] = { 0,-1, 0}; break;
            }
        }

        // The two in-plane components of a cell, (along axisW, along axisH).
        glm::ivec2 InPlane(Axis axis, const glm::ivec3& c) {
            switch (axis) {
                case Axis::X: return { c.z, c.y };
                case Axis::Y: return { c.x, c.z };
                case Axis::Z: return { c.x, c.y };
            }
            return { c.x, c.y };
        }

    } // namespace

    bool FrameShape::IsObsidian(BlockID id) {
        return id == BlockID::Obsidian || id == BlockID::CryingObsidian;
    }

    bool FrameShape::IsAirLike(BlockID id) {
        return id == BlockID::Air || id == BlockID::Fire;
    }

    bool FrameShape::IsRectangle() const {
        if (area.empty()) return false;
        const glm::ivec3 size = maxCell - minCell + glm::ivec3(1);
        const long long cells = static_cast<long long>(size.x) * size.y * size.z;
        return cells == static_cast<long long>(area.size());
    }

    glm::ivec2 FrameShape::Size() const {
        const glm::ivec2 a = InPlane(axis, minCell);
        const glm::ivec2 b = InPlane(axis, maxCell);
        return b - a + glm::ivec2(1);
    }

    std::vector<glm::ivec3> FrameShape::FrameWithCorners() const {
        std::unordered_set<glm::ivec3, IVec3Hash> areaSet(area.begin(), area.end());
        std::unordered_set<glm::ivec3, IVec3Hash> out(frame.begin(), frame.end());
        glm::ivec3 dirs[4];
        InPlaneDirections(axis, dirs);
        // Corners: diagonal in-plane neighbours of area cells that are
        // neither area nor already frame.
        for (const glm::ivec3& c : area) {
            for (int i = 0; i < 2; ++i) {
                for (int j = 2; j < 4; ++j) {
                    const glm::ivec3 d = c + dirs[i] + dirs[j];
                    if (!areaSet.count(d)) out.insert(d);
                }
            }
        }
        return std::vector<glm::ivec3>(out.begin(), out.end());
    }

    void FrameShape::PlaneAxes(glm::dvec3& axisW, glm::dvec3& axisH) const {
        switch (axis) {
            case Axis::X: axisW = { 0, 0, 1 }; axisH = { 0, 1, 0 }; break;
            case Axis::Y: axisW = { 1, 0, 0 }; axisH = { 0, 0, 1 }; break;
            case Axis::Z: axisW = { 1, 0, 0 }; axisH = { 0, 1, 0 }; break;
        }
    }

    glm::dvec3 FrameShape::Center() const {
        return (glm::dvec3(minCell) + glm::dvec3(maxCell)) * 0.5 + glm::dvec3(0.5);
    }

    void FrameShape::FillPortal(Portal& portal) const {
        glm::dvec3 w, h;
        PlaneAxes(w, h);
        portal.axisW  = w;
        portal.axisH  = h;
        portal.origin = Center();
        const glm::ivec2 size = Size();
        portal.width  = size.x;
        portal.height = size.y;
        portal.shape  = PortalShape{};
        if (IsRectangle()) return;

        // One quad per cell, in local (u, v) relative to the origin.
        portal.shape.type = PortalShape::Type::Mesh;
        portal.shape.vertices.reserve(area.size() * 4);
        portal.shape.indices.reserve(area.size() * 6);
        for (const glm::ivec3& c : area) {
            const glm::dvec3 centre = glm::dvec3(c) + glm::dvec3(0.5);
            const double u = glm::dot(centre - portal.origin, w);
            const double v = glm::dot(centre - portal.origin, h);
            const uint32_t base = static_cast<uint32_t>(portal.shape.vertices.size());
            portal.shape.vertices.emplace_back(static_cast<float>(u - 0.5), static_cast<float>(v - 0.5));
            portal.shape.vertices.emplace_back(static_cast<float>(u + 0.5), static_cast<float>(v - 0.5));
            portal.shape.vertices.emplace_back(static_cast<float>(u + 0.5), static_cast<float>(v + 0.5));
            portal.shape.vertices.emplace_back(static_cast<float>(u - 0.5), static_cast<float>(v + 0.5));
            portal.shape.indices.insert(portal.shape.indices.end(),
                                        { base, base + 1, base + 2, base, base + 2, base + 3 });
        }
    }

    bool FrameShape::ContainsFrameCell(const glm::ivec3& pos) const {
        return std::find(frame.begin(), frame.end(), pos) != frame.end();
    }

    bool FrameShape::ContainsAreaCell(const glm::ivec3& pos) const {
        return std::find(area.begin(), area.end(), pos) != area.end();
    }

    FrameShape FrameShape::Translated(const glm::ivec3& delta) const {
        FrameShape t = *this;
        for (auto& c : t.area)  c += delta;
        for (auto& c : t.frame) c += delta;
        t.minCell += delta;
        t.maxCell += delta;
        return t;
    }

    std::optional<FrameShape> FrameShape::Find(const IBlockAccess& level, const glm::ivec3& start,
                                               int lengthLimit, int areaLimit) {
        for (Axis axis : { Axis::X, Axis::Y, Axis::Z }) {
            if (auto s = FindOnAxis(level, start, axis, lengthLimit, areaLimit)) return s;
        }
        return std::nullopt;
    }

    std::optional<FrameShape> FrameShape::FindOnAxis(const IBlockAccess& level, const glm::ivec3& start,
                                                     Axis axis, int lengthLimit, int areaLimit) {
        if (!IsAirLike(level.GetBlock(start.x, start.y, start.z))) return std::nullopt;

        glm::ivec3 dirs[4];
        InPlaneDirections(axis, dirs);

        FrameShape shape;
        shape.axis = axis;
        std::unordered_set<glm::ivec3, IVec3Hash> visited;
        std::unordered_set<glm::ivec3, IVec3Hash> frameSet;
        std::vector<glm::ivec3> queue{ start };
        visited.insert(start);
        shape.minCell = shape.maxCell = start;

        while (!queue.empty()) {
            const glm::ivec3 c = queue.back();
            queue.pop_back();
            shape.area.push_back(c);
            shape.minCell = glm::min(shape.minCell, c);
            shape.maxCell = glm::max(shape.maxCell, c);
            if (static_cast<int>(shape.area.size()) > areaLimit) return std::nullopt;
            const glm::ivec3 extent = shape.maxCell - shape.minCell;
            if (extent.x > lengthLimit || extent.y > lengthLimit || extent.z > lengthLimit) return std::nullopt;

            for (const glm::ivec3& d : dirs) {
                const glm::ivec3 n = c + d;
                if (visited.count(n) || frameSet.count(n)) continue;
                const BlockID id = level.GetBlock(n.x, n.y, n.z);
                if (IsAirLike(id)) {
                    visited.insert(n);
                    queue.push_back(n);
                } else if (IsObsidian(id)) {
                    frameSet.insert(n);
                } else {
                    return std::nullopt;   // the loop is not closed by obsidian
                }
            }
        }
        shape.frame.assign(frameSet.begin(), frameSet.end());
        std::sort(shape.area.begin(), shape.area.end(),
                  [](const glm::ivec3& a, const glm::ivec3& b) {
                      return a.y != b.y ? a.y < b.y : (a.z != b.z ? a.z < b.z : a.x < b.x);
                  });
        return shape;
    }

    bool FrameShape::IsIntact(const IBlockAccess& level) const {
        for (const glm::ivec3& c : frame) {
            if (!IsObsidian(level.GetBlock(c.x, c.y, c.z))) return false;
        }
        for (const glm::ivec3& c : area) {
            if (!IsAirLike(level.GetBlock(c.x, c.y, c.z))) return false;
        }
        return true;
    }

    bool FrameShape::MatchesAt(const IBlockAccess& level, const glm::ivec3& newMin) const {
        return Translated(newMin - minCell).IsIntact(level);
    }

    std::vector<glm::ivec3> FrameShape::Clearance() const {
        glm::ivec3 n(0);
        n[static_cast<int>(axis)] = 1;
        std::vector<glm::ivec3> cells;
        cells.reserve(area.size() * 2);
        for (const glm::ivec3& c : area) {
            cells.push_back(c + n);
            if (axis != Axis::Y) {
                cells.push_back(c - n);
            } else {
                for (int k = 1; k <= kFallClearance; ++k) cells.push_back(c - n * k);
            }
        }
        return cells;
    }

    bool FrameShape::CanBuildAt(const IBlockAccess& level, const glm::ivec3& newMin, bool requireGround,
                                bool requireClearance) const {
        const FrameShape t = Translated(newMin - minCell);
        for (const glm::ivec3& c : t.FrameWithCorners()) {
            const BlockID id = level.GetBlock(c.x, c.y, c.z);
            if (!(IsAirLike(id) || IsObsidian(id))) return false;
        }
        for (const glm::ivec3& c : t.area) {
            if (!IsAirLike(level.GetBlock(c.x, c.y, c.z))) return false;
        }
        if (requireClearance) {
            for (const glm::ivec3& c : t.Clearance()) {
                if (!IsAirLike(level.GetBlock(c.x, c.y, c.z))) return false;
            }
        }
        if (requireGround && axis != Axis::Y) {
            // Something solid under the bottom row of the frame.
            for (const glm::ivec3& c : t.frame) {
                if (c.y != t.minCell.y - 1) continue;
                if (!level.IsBlockSolid(c.x, c.y - 1, c.z)) return false;
            }
        }
        if (requireGround && axis == Axis::Y) {
            // A floor frame floats kFallClearance cells above its ground:
            // something to land on under the reserved drop.
            for (const glm::ivec3& c : t.area) {
                if (!level.IsBlockSolid(c.x, c.y - kFallClearance - 1, c.z)) return false;
            }
        }
        return true;
    }

    std::optional<FrameShape> FrameFromPortal(const Portal& portal) {
        // Only a nether-portal-shaped surface: axes on the block grid, cells
        // on integer boundaries.
        glm::dvec3 w = portal.axisW, h = portal.axisH;
        auto isUnitAxis = [](const glm::dvec3& v, int& axisIndex, int& sign) {
            for (int i = 0; i < 3; ++i) {
                if (std::abs(std::abs(v[i]) - 1.0) < 1e-4) {
                    for (int j = 0; j < 3; ++j) if (j != i && std::abs(v[j]) > 1e-4) return false;
                    axisIndex = i; sign = v[i] > 0 ? 1 : -1;
                    return true;
                }
            }
            return false;
        };
        int wi, ws, hi, hs;
        if (!isUnitAxis(w, wi, ws) || !isUnitAxis(h, hi, hs) || wi == hi) return std::nullopt;
        const int ni = 3 - wi - hi;   // the remaining axis is the normal

        FrameShape shape;
        shape.axis = static_cast<Axis>(ni);

        // Recover the cells: a rectangle from the bounds, a mesh from its quads.
        std::vector<glm::dvec3> centres;
        if (portal.shape.IsRectangle()) {
            const int nw = static_cast<int>(std::lround(portal.width));
            const int nh = static_cast<int>(std::lround(portal.height));
            if (nw <= 0 || nh <= 0 || std::abs(portal.width - nw) > 1e-3 || std::abs(portal.height - nh) > 1e-3) {
                return std::nullopt;
            }
            for (int a = 0; a < nw; ++a) {
                for (int b = 0; b < nh; ++b) {
                    const double u = -portal.HalfWidth()  + 0.5 + a;
                    const double v = -portal.HalfHeight() + 0.5 + b;
                    centres.push_back(portal.origin + w * u + h * v);
                }
            }
        } else {
            for (size_t i = 0; i + 2 < portal.shape.indices.size(); i += 6) {
                // Each quad was emitted as (base, base+1, base+2, base, base+2, base+3).
                const auto& v0 = portal.shape.vertices[portal.shape.indices[i]];
                const auto& v2 = portal.shape.vertices[portal.shape.indices[i + 2]];
                const double u = 0.5 * (v0.x + v2.x);
                const double v = 0.5 * (v0.y + v2.y);
                centres.push_back(portal.origin + w * u + h * v);
            }
        }
        if (centres.empty()) return std::nullopt;

        bool first = true;
        for (const glm::dvec3& c : centres) {
            const glm::ivec3 cell(static_cast<int>(std::floor(c.x)), static_cast<int>(std::floor(c.y)),
                                  static_cast<int>(std::floor(c.z)));
            shape.area.push_back(cell);
            if (first) { shape.minCell = shape.maxCell = cell; first = false; }
            shape.minCell = glm::min(shape.minCell, cell);
            shape.maxCell = glm::max(shape.maxCell, cell);
        }
        // Frame = in-plane 4-neighbours that are not area.
        glm::ivec3 dirs[4];
        InPlaneDirections(shape.axis, dirs);
        std::unordered_set<glm::ivec3, IVec3Hash> areaSet(shape.area.begin(), shape.area.end());
        std::unordered_set<glm::ivec3, IVec3Hash> frameSet;
        for (const glm::ivec3& c : shape.area) {
            for (const glm::ivec3& d : dirs) {
                const glm::ivec3 n = c + d;
                if (!areaSet.count(n)) frameSet.insert(n);
            }
        }
        shape.frame.assign(frameSet.begin(), frameSet.end());
        return shape;
    }

} // namespace Game::Immersive

#endif // ENABLE_IMMERSIVE_PORTALS
