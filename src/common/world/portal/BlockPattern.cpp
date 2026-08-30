// File: src/common/world/portal/BlockPattern.cpp
//
// Line references are to minecraft_code/decompiled_net/minecraft/world/level/
// block/state/pattern/BlockPattern.java and BlockPatternBuilder.java.

#include "BlockPattern.hpp"

#include "common/world/chunk/IBlockAccess.hpp"

#include <algorithm>
#include <cassert>

namespace Game {

    namespace {
        // MC iterates Direction.values() in ordinal order and the search
        // depends on it: EnderEyeItem's hardcoded (-3, 0, -3) offset is only
        // correct for the FIRST orientation that matches, so reordering this
        // silently moves the End portal.
        constexpr Direction kAllDirections[6] = {
            Direction::Down, Direction::Up, Direction::North,
            Direction::South, Direction::West, Direction::East,
        };

        inline glm::ivec3 Cross(const glm::ivec3& a, const glm::ivec3& b) {
            return { a.y * b.z - a.z * b.y,
                     a.z * b.x - a.x * b.z,
                     a.x * b.y - a.y * b.x };
        }

        inline glm::ivec3 StepVec(Direction d) {
            return { StepX(d), StepY(d), StepZ(d) };
        }
    } // namespace

    // BlockPattern.java:98
    glm::ivec3 BlockPattern::TranslateAndRotate(const glm::ivec3& origin,
                                                Direction forwardsDir, Direction upDir,
                                                int right, int down, int forwards)
    {
        const glm::ivec3 f = StepVec(forwardsDir);
        const glm::ivec3 u = StepVec(upDir);
        const glm::ivec3 r = Cross(f, u);
        return origin + u * -down + r * right + f * forwards;
    }

    // BlockPattern.java:59
    bool BlockPattern::Matches(const IBlockAccess& level, const glm::ivec3& origin,
                               Direction forwards, Direction up) const
    {
        for (int x = 0; x < m_width; ++x) {
            for (int y = 0; y < m_height; ++y) {
                for (int z = 0; z < m_depth; ++z) {
                    const glm::ivec3 at =
                        TranslateAndRotate(origin, forwards, up, x, y, z);
                    const BlockState state = level.GetBlockState(at.x, at.y, at.z);
                    if (!m_pattern[static_cast<size_t>(z)]
                                  [static_cast<size_t>(y)]
                                  [static_cast<size_t>(x)](state)) {
                        return false;
                    }
                }
            }
        }
        return true;
    }

    // BlockPattern.java:75
    std::optional<BlockPattern::Match> BlockPattern::Find(const IBlockAccess& level,
                                                          const glm::ivec3& origin) const
    {
        const int dist = std::max(std::max(m_width, m_height), m_depth);

        // MC walks BlockPos.betweenClosed(origin, origin + (dist-1)), whose
        // iterator advances X fastest, then Y, then Z. The nesting below is
        // that order. It only matters if a position could match under more
        // than one orientation — but the whole reason EnderEyeItem can use a
        // hardcoded world offset is that the FIRST match is deterministic, so
        // this order is load-bearing rather than incidental.
        for (int dz = 0; dz < dist; ++dz) {
            for (int dy = 0; dy < dist; ++dy) {
                for (int dx = 0; dx < dist; ++dx) {
                    const glm::ivec3 testPos{ origin.x + dx, origin.y + dy, origin.z + dz };
                    for (Direction forwards : kAllDirections) {
                        for (Direction up : kAllDirections) {
                            // MC: up != forwards && up != forwards.getOpposite()
                            // — the two vectors must not be collinear or the
                            // cross product is zero and `right` is undefined.
                            if (up == forwards || up == Opposite(forwards)) continue;
                            if (Matches(level, testPos, forwards, up)) {
                                return Match{ testPos, forwards, up,
                                              m_width, m_height, m_depth };
                            }
                        }
                    }
                }
            }
        }
        return std::nullopt;
    }

    // ── Builder ─────────────────────────────────────────────────────────────

    // BlockPatternBuilder.java:22 — ' ' is always "anything".
    BlockPatternBuilder::BlockPatternBuilder() {
        m_lookup[' '] = [](BlockState) { return true; };
    }

    // BlockPatternBuilder.java:26
    BlockPatternBuilder& BlockPatternBuilder::Aisle(
        std::initializer_list<std::string_view> rows)
    {
        assert(rows.size() > 0 && "empty pattern for aisle");

        if (m_pattern.empty()) {
            m_height = static_cast<int>(rows.size());
            m_width  = static_cast<int>(rows.begin()->size());
        }
        assert(static_cast<int>(rows.size()) == m_height &&
               "every aisle must have the same height");

        std::vector<std::string> aisle;
        aisle.reserve(rows.size());
        for (std::string_view row : rows) {
            assert(static_cast<int>(row.size()) == m_width &&
                   "every row must have the same width");
            aisle.emplace_back(row);
        }
        m_pattern.push_back(std::move(aisle));
        return *this;
    }

    // BlockPatternBuilder.java:62
    BlockPatternBuilder& BlockPatternBuilder::Where(char c,
                                                    BlockPattern::StatePredicate predicate)
    {
        m_lookup[c] = std::move(predicate);
        return *this;
    }

    // BlockPatternBuilder.java:71
    BlockPattern BlockPatternBuilder::Build() const {
        BlockPattern out;
        out.m_depth  = static_cast<int>(m_pattern.size());
        out.m_height = m_height;
        out.m_width  = m_width;

        out.m_pattern.resize(m_pattern.size());
        for (size_t z = 0; z < m_pattern.size(); ++z) {
            out.m_pattern[z].resize(static_cast<size_t>(m_height));
            for (int y = 0; y < m_height; ++y) {
                out.m_pattern[z][static_cast<size_t>(y)].resize(static_cast<size_t>(m_width));
                for (int x = 0; x < m_width; ++x) {
                    const char c = m_pattern[z][static_cast<size_t>(y)][static_cast<size_t>(x)];
                    auto it = m_lookup.find(c);
                    assert(it != m_lookup.end() && "pattern character has no predicate");
                    out.m_pattern[z][static_cast<size_t>(y)][static_cast<size_t>(x)] =
                        it->second;
                }
            }
        }
        return out;
    }

} // namespace Game
