// File: src/common/world/block/pattern/BlockPattern.hpp
//
// Port of MC's net.minecraft.world.level.block.state.pattern package —
// BlockInWorld, BlockPattern (with its BlockPatternMatch) and
// BlockPatternBuilder — the multiblock matcher behind every build-a-mob
// structure: the snow, iron and copper golems (CarvedPumpkinBlock) and the
// wither (WitherSkullBlock).
//
// A pattern is a stack of "aisles", each a grid of characters read as
//
//     aisle[row][column]   row = DOWN from the top, column = RIGHT,
//                          aisle index = FORWARDS (away from the viewer),
//
// and each character names a predicate on the BlockInWorld at that cell (' '
// is always "anything"). BlockPattern::Find tries the grid with its
// front-top-left corner at every cell of the max(width, height, depth)^3 cube
// starting at the probe position, in every (forwards, up ⊥ forwards)
// orientation — which is why vanilla golems and the wither may be built
// facing any way, lying on their side, or with any cell of the head row
// placed last. All of that falls out of porting find() literally.
//
// Like MC, the cells a search reads are cached per search (MC's
// LoadingCache<BlockPos, BlockInWorld>) and a match keeps that cache: a
// BlockInWorld remembers the state it first read, so callers that rewrite the
// pattern (clearPatternBlocks) still see the blocks as they were matched.
#pragma once

#include "common/world/block/BlockState.hpp"
#include "common/world/block/Direction.hpp"

#include <glm/glm.hpp>

#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Game {

    struct IBlockAccess;

    // MC BlockInWorld: one cell of the level, its state read lazily and then
    // kept. getEntity() is not ported — no pattern in this engine reads a
    // block entity through the matcher.
    class BlockInWorld {
    public:
        BlockInWorld(const IBlockAccess& level, const glm::ivec3& pos, bool loadChunks);

        // MC getState: the state at the cell, read on first use and kept.
        // Empty (MC null) while the cell's chunk is not loaded and the cache
        // was made without loadChunks — retried on the next call.
        std::optional<BlockState> GetState();

        const IBlockAccess& GetLevel() const { return *m_level; }
        const glm::ivec3&   GetPos() const { return m_pos; }

        // MC Predicate<@Nullable BlockInWorld>.
        using Predicate = std::function<bool(BlockInWorld*)>;

        // MC BlockInWorld.hasState(predicate): a non-null cell whose state is
        // known and passes `predicate`. (MC hands a null state of an unloaded
        // cell to the predicate; every vanilla predicate then answers false
        // or would fault, so an unloaded cell never matches here.)
        static Predicate HasState(std::function<bool(BlockState)> predicate);

    private:
        const IBlockAccess*       m_level;
        glm::ivec3                m_pos;
        bool                      m_loadChunks;
        std::optional<BlockState> m_state;
    };

    class BlockPattern {
    public:
        // MC BlockPattern.createLevelCache's LoadingCache: one BlockInWorld
        // per position, made on first request. Shared between a search and
        // the match it returns.
        class BlockCache {
        public:
            BlockCache(const IBlockAccess& level, bool loadChunks)
                : m_level(&level), m_loadChunks(loadChunks) {}
            BlockInWorld& Get(const glm::ivec3& pos);

        private:
            struct PosHash {
                size_t operator()(const glm::ivec3& v) const noexcept;
            };
            const IBlockAccess* m_level;
            bool                m_loadChunks;
            // Node-based: references stay valid as the map grows.
            std::unordered_map<glm::ivec3, BlockInWorld, PosHash> m_cells;
        };

        // MC BlockPattern.BlockPatternMatch.
        class Match {
        public:
            Match(const glm::ivec3& frontTopLeft, Direction forwards, Direction up,
                  std::shared_ptr<BlockCache> cache, int width, int height, int depth)
                : m_frontTopLeft(frontTopLeft), m_forwards(forwards), m_up(up),
                  m_cache(std::move(cache)), m_width(width), m_height(height), m_depth(depth) {}

            const glm::ivec3& GetFrontTopLeft() const { return m_frontTopLeft; }
            Direction GetForwards() const { return m_forwards; }
            Direction GetUp() const { return m_up; }
            int GetWidth() const { return m_width; }
            int GetHeight() const { return m_height; }
            int GetDepth() const { return m_depth; }

            // MC getBlock(right, down, forwards): the cell at those pattern
            // coordinates, from the search's cache.
            BlockInWorld& GetBlock(int right, int down, int forwards) const;

        private:
            glm::ivec3                  m_frontTopLeft;
            Direction                   m_forwards;
            Direction                   m_up;
            std::shared_ptr<BlockCache> m_cache;
            int                         m_width;
            int                         m_height;
            int                         m_depth;
        };

        // `pattern` is MC's Predicate[depth][height][width], flattened
        // aisle-major: index (z * height + y) * width + x.
        BlockPattern(std::vector<BlockInWorld::Predicate> pattern, int depth, int height, int width);

        int GetDepth() const { return m_depth; }
        int GetHeight() const { return m_height; }
        int GetWidth() const { return m_width; }

        // MC matches(level, origin, forwards, up): the pattern in exactly
        // that placement, or nothing.
        std::optional<Match> Matches(const IBlockAccess& level, const glm::ivec3& origin,
                                     Direction forwards, Direction up) const;

        // MC find(level, origin): the first placement, in MC's search order,
        // that matches anywhere in the cube [origin, origin + dist - 1]^3.
        std::optional<Match> Find(const IBlockAccess& level, const glm::ivec3& origin) const;

        // MC translateAndRotate: pattern coordinates (right, down, forwards)
        // from `origin` (the front-top-left corner) to a world position.
        // `up` must not be `forwards` or its opposite.
        static glm::ivec3 TranslateAndRotate(const glm::ivec3& origin, Direction forwardsDirection,
                                             Direction upDirection, int right, int down, int forwards);

    private:
        std::optional<Match> Matches(const glm::ivec3& origin, Direction forwards, Direction up,
                                     const std::shared_ptr<BlockCache>& cache) const;

        std::vector<BlockInWorld::Predicate> m_pattern;
        int m_depth;
        int m_height;
        int m_width;
    };

    // MC BlockPatternBuilder. Throws std::invalid_argument / std::logic_error
    // where MC throws IllegalArgumentException / IllegalStateException — the
    // patterns are fixed in code, so either is a programming error.
    class BlockPatternBuilder {
    public:
        static BlockPatternBuilder Start() { return BlockPatternBuilder(); }

        // One aisle: its rows top to bottom, each the same width.
        BlockPatternBuilder& Aisle(std::initializer_list<std::string_view> rows);
        BlockPatternBuilder& Where(char character, BlockInWorld::Predicate predicate);
        BlockPattern Build() const;

    private:
        BlockPatternBuilder();

        std::vector<std::vector<std::string>>             m_pattern;
        std::unordered_map<char, BlockInWorld::Predicate> m_lookup;
        int                                               m_height = 0;
        int                                               m_width = 0;
        std::string                                       m_unknownCharacters;
    };

} // namespace Game
