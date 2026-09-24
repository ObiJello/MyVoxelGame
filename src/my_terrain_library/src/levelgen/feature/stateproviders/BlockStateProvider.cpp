#include "levelgen/feature/stateproviders/BlockStateProvider.h"
#include "levelgen/blockpredicates/BlockPredicate.h"

// Most implementations are inline in the header file.

namespace minecraft {
namespace levelgen {
namespace feature {
namespace stateproviders {

BlockState* RandomBlockProvider::getState(WorldgenRandom& random, const core::BlockPos& /*pos*/) const {
    const std::vector<std::string>& blocks = blockpredicates::orderedBlockTagValues(m_tag);
    if (blocks.empty()) {
        return nullptr;
    }
    const std::string& id = blocks[static_cast<size_t>(random.nextInt(static_cast<int32_t>(blocks.size())))];
    return minecraft::world::level::block::Blocks::getDefaultState(id);
}

} // namespace stateproviders
} // namespace feature
} // namespace levelgen
} // namespace minecraft
