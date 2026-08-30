// File: src/common/world/portal/BlockPattern.hpp
//
// Port of net.minecraft.world.level.block.state.pattern.BlockPattern +
// BlockPatternBuilder, reduced to what the End portal frame needs.
//
// WHY PORT THE GENERIC MATCHER INSTEAD OF HAND-CODING THE RING
// ------------------------------------------------------------
// The End portal frame is a 5x5 pattern with a `?` (any block) at each
// corner and a DIRECTION-SPECIFIC predicate on each of the twelve frame
// cells, and vanilla finds it by brute-forcing every (origin, forwards, up)
// triple. That brute force is not incidental: `EnderEyeItem.useOn` fills the
// portal at `match.getFrontTopLeft().offset(-3, 0, -3)` with raw world
// offsets, which is only correct because the search order guarantees the
// match comes back with forwards=DOWN and up=SOUTH — the one orientation
// where the returned corner is the ring's south-east cell. Hand-rolling "scan
// the twelve ring positions" would produce a different corner and put the
// 3x3 of portal blocks in the wrong place, and the bug would only show on
// some rotations.
//
// So the matcher is ported as-is, and the caller stays a transcription of
// EnderEyeItem.
//
// Simplifications that do not change behaviour:
//   * MC memoises world reads in a Guava LoadingCache because `find` can test
//     the same position under many orientations. Here the predicate reads a
//     BlockState straight from IBlockAccess, which is already a cheap array
//     lookup through the chunk cache — the cache would cost more than it saves.
//   * MC's builder validates aisle rectangularity and unknown characters at
//     construction. The only pattern in the engine is a compile-time constant
//     in EndPortalFrame.cpp, so those checks are asserts rather than throws.
#pragma once

#include "common/world/block/BlockState.hpp"
#include "common/world/block/Direction.hpp"

#include <functional>
#include <initializer_list>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <glm/glm.hpp>

namespace Game {

    struct IBlockAccess;

    class BlockPattern {
    public:
        // MC's Predicate<BlockInWorld>. BlockInWorld also carries the position
        // and the block entity; nothing in the patterns this engine models
        // looks at either, so the state alone is the argument.
        using StatePredicate = std::function<bool(BlockState)>;

        struct Match {
            // MC BlockPatternMatch.frontTopLeft — the world position of
            // pattern cell (right=0, down=0, forwards=0).
            glm::ivec3 frontTopLeft{0, 0, 0};
            Direction  forwards = Direction::Down;
            Direction  up       = Direction::South;
            int width  = 0;
            int height = 0;
            int depth  = 0;
        };

        // BlockPattern.java:75. Scans origin..origin+(dist-1) on each axis,
        // where dist = max(width, height, depth), against every legal
        // (forwards, up) pair, and returns the first full match.
        std::optional<Match> Find(const IBlockAccess& level,
                                  const glm::ivec3& origin) const;

        // BlockPattern.java:98. `right`/`down`/`forwards` are pattern-space
        // indices; the result is the world position they name. Exposed
        // because a caller that wants a specific cell of a match needs it.
        static glm::ivec3 TranslateAndRotate(const glm::ivec3& origin,
                                             Direction forwardsDir, Direction upDir,
                                             int right, int down, int forwards);

        int Width()  const { return m_width; }
        int Height() const { return m_height; }
        int Depth()  const { return m_depth; }

    private:
        friend class BlockPatternBuilder;

        // [depth][height][width], matching MC's pattern[z][y][x].
        std::vector<std::vector<std::vector<StatePredicate>>> m_pattern;
        int m_depth  = 0;
        int m_height = 0;
        int m_width  = 0;

        bool Matches(const IBlockAccess& level, const glm::ivec3& origin,
                     Direction forwards, Direction up) const;
    };

    class BlockPatternBuilder {
    public:
        BlockPatternBuilder();

        // One aisle = one slice along the DEPTH axis. Each string in the list
        // is a row (height, top first); each character is a column (width,
        // left first).
        BlockPatternBuilder& Aisle(std::initializer_list<std::string_view> rows);

        BlockPatternBuilder& Where(char c, BlockPattern::StatePredicate predicate);

        BlockPattern Build() const;

    private:
        std::vector<std::vector<std::string>>                   m_pattern;
        std::unordered_map<char, BlockPattern::StatePredicate>  m_lookup;
        int m_height = 0;
        int m_width  = 0;
    };

} // namespace Game
