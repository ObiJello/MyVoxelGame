#pragma once

#include "core/BlockPos.h"
#include "core/Direction.h"
#include "levelgen/WorldGenLevel.h"
#include "levelgen/WorldgenRandom.h"
#include "world/level/block/blocks/MultifaceBlock.h"
#include <array>
#include <optional>
#include <vector>

namespace minecraft {
namespace world {
namespace level {
namespace block {

using state::BlockState;

class MultifaceSpreader {
public:
    struct SpreadPos {
        core::BlockPos pos;
        core::Direction face;
    };

    class SpreadConfig {
    public:
        virtual ~SpreadConfig() = default;

        virtual BlockState* getStateForPlacement(
            BlockState* oldState,
            const minecraft::levelgen::WorldGenLevel& level,
            const core::BlockPos& placementPos,
            core::Direction placementDirection
        ) const = 0;

        virtual bool canSpreadInto(
            const minecraft::levelgen::WorldGenLevel& level,
            const core::BlockPos& sourcePos,
            const SpreadPos& spreadPos
        ) const = 0;

        virtual std::vector<int> getSpreadTypes() const {
            return {0, 1, 2};
        }

        virtual bool hasFace(BlockState* state, core::Direction face) const {
            return MultifaceBlock::hasFace(state, face);
        }

        virtual bool isOtherBlockValidAsSource(BlockState* /*state*/) const {
            return false;
        }

        virtual bool canSpreadFrom(BlockState* state, core::Direction face) const {
            return isOtherBlockValidAsSource(state) || hasFace(state, face);
        }

        virtual bool placeBlock(
            minecraft::levelgen::WorldGenLevel* level,
            const SpreadPos& spreadPos,
            BlockState* oldState,
            bool postProcess
        ) const {
            BlockState* spreadState = getStateForPlacement(oldState, *level, spreadPos.pos, spreadPos.face);
            if (!spreadState) {
                return false;
            }

            if (postProcess) {
                if (::world::IChunk* chunk = level->getChunk(spreadPos.pos.getX() >> 4, spreadPos.pos.getZ() >> 4)) {
                    chunk->markPosForPostprocessing(spreadPos.pos);
                }
            }

            return level->setBlock(spreadPos.pos, spreadState, 2);
        }
    };

    class DefaultSpreaderConfig : public SpreadConfig {
    public:
        explicit DefaultSpreaderConfig(const MultifaceBlock* block) : m_block(block) {}

        BlockState* getStateForPlacement(
            BlockState* oldState,
            const minecraft::levelgen::WorldGenLevel& level,
            const core::BlockPos& placementPos,
            core::Direction placementDirection
        ) const override {
            return m_block->getStateForPlacement(oldState, level, placementPos, placementDirection);
        }

        bool canSpreadInto(
            const minecraft::levelgen::WorldGenLevel& level,
            const core::BlockPos& sourcePos,
            const SpreadPos& spreadPos
        ) const override {
            (void)sourcePos;
            BlockState* existingState = level.getBlockState(spreadPos.pos);
            return stateCanBeReplaced(level, spreadPos.pos, spreadPos.face, existingState) &&
                   m_block->isValidStateForPlacement(level, existingState, spreadPos.pos, spreadPos.face);
        }

    protected:
        bool stateCanBeReplaced(
            const minecraft::levelgen::WorldGenLevel& /*level*/,
            const core::BlockPos& /*placementPos*/,
            core::Direction /*placementDirection*/,
            BlockState* existingState
        ) const {
            return existingState &&
                   (existingState->isAir() || existingState->is(m_block) ||
                    existingState->getIdentifier() == "minecraft:water");
        }

    private:
        const MultifaceBlock* m_block;
    };

    explicit MultifaceSpreader(const MultifaceBlock* multifaceBlock)
        : m_ownedConfig(std::make_shared<DefaultSpreaderConfig>(multifaceBlock))
        , m_config(m_ownedConfig.get()) {}

    explicit MultifaceSpreader(std::shared_ptr<SpreadConfig> config)
        : m_ownedConfig(std::move(config))
        , m_config(m_ownedConfig.get()) {}

    bool canSpreadInAnyDirection(
        BlockState* state,
        const minecraft::levelgen::WorldGenLevel& level,
        const core::BlockPos& pos,
        core::Direction startingFace
    ) const {
        for (core::Direction spreadDirection : kAllDirections) {
            if (getSpreadFromFaceTowardDirection(state, level, pos, startingFace, spreadDirection).has_value()) {
                return true;
            }
        }
        return false;
    }

    std::optional<SpreadPos> spreadFromFaceTowardRandomDirection(
        BlockState* state,
        minecraft::levelgen::WorldGenLevel* level,
        const core::BlockPos& pos,
        core::Direction startingFace,
        minecraft::levelgen::WorldgenRandom& random,
        bool postProcess
    ) const {
        auto shuffledDirections = shuffledAllDirections(random);
        for (core::Direction spreadDirection : shuffledDirections) {
            std::optional<SpreadPos> spreadPos =
                spreadFromFaceTowardDirection(state, level, pos, startingFace, spreadDirection, postProcess);
            if (spreadPos.has_value()) {
                return spreadPos;
            }
        }
        return std::nullopt;
    }

private:
    static constexpr std::array<core::Direction, 6> kAllDirections = {
        core::Direction::DOWN,
        core::Direction::UP,
        core::Direction::NORTH,
        core::Direction::SOUTH,
        core::Direction::WEST,
        core::Direction::EAST
    };

    std::shared_ptr<SpreadConfig> m_ownedConfig;
    const SpreadConfig* m_config;

    static std::array<core::Direction, 6> shuffledAllDirections(minecraft::levelgen::WorldgenRandom& random) {
        std::array<core::Direction, 6> directions = kAllDirections;
        for (size_t i = directions.size(); i > 1; --i) {
            size_t j = static_cast<size_t>(random.nextInt(static_cast<int>(i)));
            std::swap(directions[i - 1], directions[j]);
        }
        return directions;
    }

    static SpreadPos getSpreadPos(
        int spreadType,
        const core::BlockPos& pos,
        core::Direction spreadDirection,
        core::Direction fromFace
    ) {
        switch (spreadType) {
            case 0:
                return SpreadPos{pos, spreadDirection};
            case 1:
                return SpreadPos{pos.relative(spreadDirection), fromFace};
            default:
                return SpreadPos{
                    pos.relative(spreadDirection).relative(fromFace),
                    core::getOpposite(spreadDirection)
                };
        }
    }

    std::optional<SpreadPos> getSpreadFromFaceTowardDirection(
        BlockState* state,
        const minecraft::levelgen::WorldGenLevel& level,
        const core::BlockPos& pos,
        core::Direction startingFace,
        core::Direction spreadDirection
    ) const {
        if (core::getAxis(spreadDirection) == core::getAxis(startingFace)) {
            return std::nullopt;
        }

        if (!(m_config->isOtherBlockValidAsSource(state) ||
              (m_config->hasFace(state, startingFace) && !m_config->hasFace(state, spreadDirection)))) {
            return std::nullopt;
        }

        for (int spreadType : m_config->getSpreadTypes()) {
            SpreadPos spreadPos = getSpreadPos(spreadType, pos, spreadDirection, startingFace);
            if (m_config->canSpreadInto(level, pos, spreadPos)) {
                return spreadPos;
            }
        }

        return std::nullopt;
    }

    std::optional<SpreadPos> spreadFromFaceTowardDirection(
        BlockState* state,
        minecraft::levelgen::WorldGenLevel* level,
        const core::BlockPos& pos,
        core::Direction fromFace,
        core::Direction spreadDirection,
        bool postProcess
    ) const {
        std::optional<SpreadPos> spreadPos =
            getSpreadFromFaceTowardDirection(state, *level, pos, fromFace, spreadDirection);
        if (!spreadPos.has_value()) {
            return std::nullopt;
        }

        BlockState* oldState = level->getBlockState(spreadPos->pos);
        if (!m_config->placeBlock(level, *spreadPos, oldState, postProcess)) {
            return std::nullopt;
        }

        return spreadPos;
    }
};

} // namespace block
} // namespace level
} // namespace world
} // namespace minecraft
