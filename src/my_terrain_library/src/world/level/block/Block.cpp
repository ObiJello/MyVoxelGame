#include "world/level/block/Block.h"

namespace minecraft {
namespace world {
namespace level {
namespace block {

// Static member initialization
core::IdMapper<BlockState*> Block::BLOCK_STATE_REGISTRY;

Block::Block(const Properties& properties)
    : m_properties(properties)
    , m_identifier(properties.getIdentifier())
{
    // Reference: Block.java constructor lines 224-238
    buildStateDefinition();
}

void Block::buildStateDefinition() {
    // Reference: Block.java lines 227-230
    // Create builder
    typename StateDefinition<Block, BlockState>::Builder builder(this);

    // Let subclass add properties
    createBlockStateDefinition(builder);

    // Create state definition using factory functions
    m_stateDefinition = std::make_unique<StateDefinition<Block, BlockState>>(
        builder.create(
            // Default state function (not used, but required by signature)
            [](Block* b) -> BlockState* { return nullptr; },
            // State factory
            [](Block* owner, const state::ValueMap& values) -> BlockState* {
                return new BlockState(owner, values);
            }
        )
    );

    // Register the default state as the first state
    registerDefaultState(m_stateDefinition->any());

    // Initialize cache for all states and register them in the global registry
    for (BlockState* state : m_stateDefinition->getPossibleStates()) {
        state->initCache();
        BLOCK_STATE_REGISTRY.add(state);
    }
}

void Block::createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& /*builder*/) {
    // Reference: Block.java createBlockStateDefinition(Builder) line 465
    // Default implementation does nothing - subclasses override to add properties
}

const StateDefinition<Block, BlockState>& Block::getStateDefinition() const {
    // Reference: Block.java getStateDefinition() lines 468-470
    return *m_stateDefinition;
}

void Block::registerDefaultState(BlockState* state) {
    // Reference: Block.java registerDefaultState(BlockState) lines 472-474
    m_defaultBlockState = state;
}

BlockState* Block::defaultBlockState() const {
    // Reference: Block.java defaultBlockState() lines 476-478
    return m_defaultBlockState;
}

int Block::getId(BlockState* blockState) {
    // Reference: Block.java getId(BlockState) lines 122-129
    if (blockState == nullptr) {
        return 0;
    }
    int id = BLOCK_STATE_REGISTRY.getId(blockState);
    return id == -1 ? 0 : id;
}

BlockState* Block::stateById(int idWithData) {
    // Reference: Block.java stateById(int) lines 131-134
    BlockState* state = BLOCK_STATE_REGISTRY.byId(idWithData);
    // Note: In Minecraft this returns Blocks.AIR.defaultBlockState() if null
    // We return nullptr and let caller handle it
    return state;
}

void Block::rebuildStateDefinition() {
    // Called by derived class constructors to rebuild state definition
    // with their properties (since C++ virtual dispatch doesn't work in base constructors)

    // Note: We don't unregister old states from BLOCK_STATE_REGISTRY because
    // the IdMapper doesn't support removal. The old states will be orphaned
    // but this only happens once during bootstrap, so it's acceptable.

    // Clear old state definition
    m_stateDefinition.reset();
    m_defaultBlockState = nullptr;

    // Rebuild with derived class's createBlockStateDefinition
    buildStateDefinition();
}

} // namespace block
} // namespace level
} // namespace world
} // namespace minecraft
