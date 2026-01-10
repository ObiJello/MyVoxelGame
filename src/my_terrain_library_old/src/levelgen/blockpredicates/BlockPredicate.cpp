#include "levelgen/blockpredicates/BlockPredicate.h"

namespace minecraft {
namespace levelgen {
namespace blockpredicates {

// Static instance
TrueBlockPredicate TrueBlockPredicate::INSTANCE;

// Common predicate instances
// Reference: BlockPredicate.java lines 21-22
std::shared_ptr<BlockPredicate> BlockPredicate::ONLY_IN_AIR_PREDICATE =
    BlockPredicate::matchesBlocks("minecraft:air");

std::shared_ptr<BlockPredicate> BlockPredicate::ONLY_IN_AIR_OR_WATER_PREDICATE =
    BlockPredicate::matchesBlocks(std::vector<std::string>{"minecraft:air", "minecraft:water"});

//=============================================================================
// Static Factory Method Implementations
//=============================================================================

std::shared_ptr<BlockPredicate> BlockPredicate::allOf(
    const std::vector<std::shared_ptr<BlockPredicate>>& predicates
) {
    if (predicates.empty()) {
        return std::make_shared<TrueBlockPredicate>();
    }
    if (predicates.size() == 1) {
        return predicates[0];
    }
    return std::make_shared<AllOfPredicate>(predicates);
}

std::shared_ptr<BlockPredicate> BlockPredicate::allOf(
    std::shared_ptr<BlockPredicate> a,
    std::shared_ptr<BlockPredicate> b
) {
    return allOf(std::vector<std::shared_ptr<BlockPredicate>>{a, b});
}

std::shared_ptr<BlockPredicate> BlockPredicate::anyOf(
    const std::vector<std::shared_ptr<BlockPredicate>>& predicates
) {
    if (predicates.empty()) {
        return not_(std::make_shared<TrueBlockPredicate>()); // False predicate
    }
    if (predicates.size() == 1) {
        return predicates[0];
    }
    return std::make_shared<AnyOfPredicate>(predicates);
}

std::shared_ptr<BlockPredicate> BlockPredicate::anyOf(
    std::shared_ptr<BlockPredicate> a,
    std::shared_ptr<BlockPredicate> b
) {
    return anyOf(std::vector<std::shared_ptr<BlockPredicate>>{a, b});
}

std::shared_ptr<BlockPredicate> BlockPredicate::matchesBlocks(
    const core::Vec3i& offset,
    const std::vector<std::string>& blocks
) {
    return std::make_shared<MatchingBlocksPredicate>(offset, blocks);
}

std::shared_ptr<BlockPredicate> BlockPredicate::matchesBlocks(
    const std::vector<std::string>& blocks
) {
    return matchesBlocks(core::Vec3i::ZERO(), blocks);
}

std::shared_ptr<BlockPredicate> BlockPredicate::matchesBlocks(const std::string& block) {
    return matchesBlocks(std::vector<std::string>{block});
}

std::shared_ptr<BlockPredicate> BlockPredicate::matchesBlocks(
    const core::Vec3i& offset,
    const std::string& block
) {
    return matchesBlocks(offset, std::vector<std::string>{block});
}

std::shared_ptr<BlockPredicate> BlockPredicate::matchesTag(
    const core::Vec3i& offset,
    const std::string& tag
) {
    return std::make_shared<MatchingBlockTagPredicate>(offset, tag);
}

std::shared_ptr<BlockPredicate> BlockPredicate::matchesTag(const std::string& tag) {
    return matchesTag(core::Vec3i::ZERO(), tag);
}

std::shared_ptr<BlockPredicate> BlockPredicate::matchesFluids(
    const core::Vec3i& offset,
    const std::vector<std::string>& fluids
) {
    return std::make_shared<MatchingFluidsPredicate>(offset, fluids);
}

std::shared_ptr<BlockPredicate> BlockPredicate::matchesFluids(
    const std::vector<std::string>& fluids
) {
    return matchesFluids(core::Vec3i::ZERO(), fluids);
}

std::shared_ptr<BlockPredicate> BlockPredicate::matchesFluids(const std::string& fluid) {
    return matchesFluids(std::vector<std::string>{fluid});
}

std::shared_ptr<BlockPredicate> BlockPredicate::not_(std::shared_ptr<BlockPredicate> predicate) {
    return std::make_shared<NotPredicate>(predicate);
}

std::shared_ptr<BlockPredicate> BlockPredicate::replaceable(const core::Vec3i& offset) {
    return std::make_shared<ReplaceablePredicate>(offset);
}

std::shared_ptr<BlockPredicate> BlockPredicate::replaceable() {
    return replaceable(core::Vec3i::ZERO());
}

std::shared_ptr<BlockPredicate> BlockPredicate::wouldSurvive(
    BlockState* state,
    const core::Vec3i& offset
) {
    return std::make_shared<WouldSurvivePredicate>(offset, state);
}

std::shared_ptr<BlockPredicate> BlockPredicate::hasSturdyFace(
    const core::Vec3i& offset,
    Direction direction
) {
    return std::make_shared<HasSturdyFacePredicate>(offset, direction);
}

std::shared_ptr<BlockPredicate> BlockPredicate::hasSturdyFace(Direction direction) {
    return hasSturdyFace(core::Vec3i::ZERO(), direction);
}

std::shared_ptr<BlockPredicate> BlockPredicate::solid(const core::Vec3i& offset) {
    return std::make_shared<SolidPredicate>(offset);
}

std::shared_ptr<BlockPredicate> BlockPredicate::solid() {
    return solid(core::Vec3i::ZERO());
}

std::shared_ptr<BlockPredicate> BlockPredicate::noFluid() {
    return noFluid(core::Vec3i::ZERO());
}

std::shared_ptr<BlockPredicate> BlockPredicate::noFluid(const core::Vec3i& offset) {
    return matchesFluids(offset, std::vector<std::string>{"minecraft:empty"});
}

std::shared_ptr<BlockPredicate> BlockPredicate::insideWorld(const core::Vec3i& offset) {
    return std::make_shared<InsideWorldBoundsPredicate>(offset);
}

std::shared_ptr<BlockPredicate> BlockPredicate::alwaysTrue() {
    return std::make_shared<TrueBlockPredicate>();
}

std::shared_ptr<BlockPredicate> BlockPredicate::unobstructed(const core::Vec3i& offset) {
    return std::make_shared<UnobstructedPredicate>(offset);
}

std::shared_ptr<BlockPredicate> BlockPredicate::unobstructed() {
    return unobstructed(core::Vec3i::ZERO());
}

} // namespace blockpredicates
} // namespace levelgen
} // namespace minecraft
