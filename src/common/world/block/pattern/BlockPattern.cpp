// File: src/common/world/block/pattern/BlockPattern.cpp
//
// See BlockPattern.hpp. Every function names the MC method it ports.
#include "common/world/block/pattern/BlockPattern.hpp"

#include "common/world/chunk/IBlockAccess.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace Game {

    // ── BlockInWorld ─────────────────────────────────────────────────────────

    BlockInWorld::BlockInWorld(const IBlockAccess& level, const glm::ivec3& pos, bool loadChunks)
        : m_level(&level), m_pos(pos), m_loadChunks(loadChunks) {}

    std::optional<BlockState> BlockInWorld::GetState() {
        // MC: `if (state == null && (loadChunks || level.hasChunkAt(pos)))`.
        // hasChunkAt is the column's chunk, whatever the height.
        if (!m_state && (m_loadChunks || m_level->IsChunkLoaded(m_pos.x >> 4, m_pos.z >> 4))) {
            m_state = m_level->GetBlockState(m_pos.x, m_pos.y, m_pos.z);
        }
        return m_state;
    }

    BlockInWorld::Predicate BlockInWorld::HasState(std::function<bool(BlockState)> predicate) {
        return [predicate = std::move(predicate)](BlockInWorld* input) {
            if (!input) return false;
            const std::optional<BlockState> state = input->GetState();
            return state.has_value() && predicate(*state);
        };
    }

    // ── BlockPattern.BlockCacheLoader ────────────────────────────────────────

    size_t BlockPattern::BlockCache::PosHash::operator()(const glm::ivec3& v) const noexcept {
        uint64_t h = static_cast<uint64_t>(static_cast<uint32_t>(v.x)) * 0x9E3779B97F4A7C15ULL;
        h ^= static_cast<uint64_t>(static_cast<uint32_t>(v.y)) + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
        h ^= static_cast<uint64_t>(static_cast<uint32_t>(v.z)) + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
        return static_cast<size_t>(h);
    }

    BlockInWorld& BlockPattern::BlockCache::Get(const glm::ivec3& pos) {
        // LoadingCache.getUnchecked → BlockCacheLoader.load on a miss.
        return m_cells.try_emplace(pos, *m_level, pos, m_loadChunks).first->second;
    }

    // ── BlockPattern ─────────────────────────────────────────────────────────

    BlockPattern::BlockPattern(std::vector<BlockInWorld::Predicate> pattern, int depth, int height, int width)
        : m_pattern(std::move(pattern)), m_depth(depth), m_height(height), m_width(width) {}

    glm::ivec3 BlockPattern::TranslateAndRotate(const glm::ivec3& origin, Direction forwardsDirection,
                                                Direction upDirection, int right, int down, int forwards) {
        if (forwardsDirection == upDirection || forwardsDirection == Opposite(upDirection)) {
            throw std::invalid_argument("Invalid forwards & up combination");
        }
        const glm::ivec3 forwardsVector(StepX(forwardsDirection), StepY(forwardsDirection),
                                        StepZ(forwardsDirection));
        const glm::ivec3 upVector(StepX(upDirection), StepY(upDirection), StepZ(upDirection));
        // Vec3i.cross.
        const glm::ivec3 rightVector(forwardsVector.y * upVector.z - forwardsVector.z * upVector.y,
                                     forwardsVector.z * upVector.x - forwardsVector.x * upVector.z,
                                     forwardsVector.x * upVector.y - forwardsVector.y * upVector.x);
        return origin + upVector * -down + rightVector * right + forwardsVector * forwards;
    }

    std::optional<BlockPattern::Match> BlockPattern::Matches(const IBlockAccess& level, const glm::ivec3& origin,
                                                             Direction forwards, Direction up) const {
        // MC: createLevelCache(level, false).
        return Matches(origin, forwards, up, std::make_shared<BlockCache>(level, false));
    }

    std::optional<BlockPattern::Match> BlockPattern::Matches(const glm::ivec3& origin, Direction forwards,
                                                             Direction up,
                                                             const std::shared_ptr<BlockCache>& cache) const {
        for (int x = 0; x < m_width; ++x) {
            for (int y = 0; y < m_height; ++y) {
                for (int z = 0; z < m_depth; ++z) {
                    const BlockInWorld::Predicate& predicate =
                        m_pattern[static_cast<size_t>((z * m_height + y) * m_width + x)];
                    if (!predicate(&cache->Get(TranslateAndRotate(origin, forwards, up, x, y, z)))) {
                        return std::nullopt;
                    }
                }
            }
        }
        return Match(origin, forwards, up, cache, m_width, m_height, m_depth);
    }

    std::optional<BlockPattern::Match> BlockPattern::Find(const IBlockAccess& level,
                                                          const glm::ivec3& origin) const {
        const auto cache = std::make_shared<BlockCache>(level, false);
        const int dist = std::max(std::max(m_width, m_height), m_depth);
        // BlockPos.betweenClosed(origin, origin.offset(dist - 1, ...)): x
        // fastest, then y, then z. Then Direction.values() for forwards and
        // for up (D U N S W E — the engine enum's order), skipping the two
        // that are not perpendicular.
        for (int dz = 0; dz < dist; ++dz) {
            for (int dy = 0; dy < dist; ++dy) {
                for (int dx = 0; dx < dist; ++dx) {
                    const glm::ivec3 testPos = origin + glm::ivec3(dx, dy, dz);
                    for (int f = 0; f < 6; ++f) {
                        const Direction forwards = static_cast<Direction>(f);
                        for (int u = 0; u < 6; ++u) {
                            const Direction up = static_cast<Direction>(u);
                            if (up == forwards || up == Opposite(forwards)) continue;
                            if (auto match = Matches(testPos, forwards, up, cache)) return match;
                        }
                    }
                }
            }
        }
        return std::nullopt;
    }

    BlockInWorld& BlockPattern::Match::GetBlock(int right, int down, int forwards) const {
        return m_cache->Get(BlockPattern::TranslateAndRotate(m_frontTopLeft, m_forwards, m_up,
                                                             right, down, forwards));
    }

    // ── BlockPatternBuilder ──────────────────────────────────────────────────

    BlockPatternBuilder::BlockPatternBuilder() {
        m_lookup.emplace(' ', [](BlockInWorld*) { return true; });
    }

    BlockPatternBuilder& BlockPatternBuilder::Aisle(std::initializer_list<std::string_view> rows) {
        if (rows.size() == 0 || rows.begin()->empty()) {
            throw std::invalid_argument("Empty pattern for aisle");
        }
        if (m_pattern.empty()) {
            m_height = static_cast<int>(rows.size());
            m_width  = static_cast<int>(rows.begin()->size());
        }
        if (static_cast<int>(rows.size()) != m_height) {
            throw std::invalid_argument("Expected aisle with height of " + std::to_string(m_height) +
                                        ", but was given one with a height of " +
                                        std::to_string(rows.size()) + ")");
        }
        std::vector<std::string> aisle;
        aisle.reserve(rows.size());
        for (std::string_view row : rows) {
            if (static_cast<int>(row.size()) != m_width) {
                throw std::invalid_argument("Not all rows in the given aisle are the correct width (expected " +
                                            std::to_string(m_width) + ", found one with " +
                                            std::to_string(row.size()) + ")");
            }
            for (char c : row) {
                if (!m_lookup.count(c) && m_unknownCharacters.find(c) == std::string::npos) {
                    m_unknownCharacters.push_back(c);
                }
            }
            aisle.emplace_back(row);
        }
        m_pattern.push_back(std::move(aisle));
        return *this;
    }

    BlockPatternBuilder& BlockPatternBuilder::Where(char character, BlockInWorld::Predicate predicate) {
        m_lookup[character] = std::move(predicate);
        const size_t at = m_unknownCharacters.find(character);
        if (at != std::string::npos) m_unknownCharacters.erase(at, 1);
        return *this;
    }

    BlockPattern BlockPatternBuilder::Build() const {
        // createPattern.
        if (!m_unknownCharacters.empty()) {
            throw std::logic_error("Predicates for character(s) " + m_unknownCharacters + " are missing");
        }
        const int depth = static_cast<int>(m_pattern.size());
        std::vector<BlockInWorld::Predicate> result;
        result.reserve(static_cast<size_t>(depth * m_height * m_width));
        for (int aisle = 0; aisle < depth; ++aisle) {
            for (int row = 0; row < m_height; ++row) {
                for (int col = 0; col < m_width; ++col) {
                    result.push_back(m_lookup.at(m_pattern[static_cast<size_t>(aisle)][static_cast<size_t>(row)]
                                                          [static_cast<size_t>(col)]));
                }
            }
        }
        return BlockPattern(std::move(result), depth, m_height, m_width);
    }

} // namespace Game
