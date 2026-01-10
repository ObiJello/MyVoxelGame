#pragma once

#include <string>
#include <unordered_map>

namespace minecraft {
namespace world {

/**
 * Abstract interface for block types.
 * This allows the terrain generation to be engine-agnostic.
 *
 * Implementation can be:
 * - MinecraftBlockType (for verification against Minecraft)
 * - Your custom engine's block system
 */
class IBlockType {
public:
    virtual ~IBlockType() = default;

    /**
     * Check if this block is air (empty space)
     */
    virtual bool isAir() const = 0;

    /**
     * Check if this block contains fluid (water, lava, etc.)
     */
    virtual bool isFluid() const = 0;

    /**
     * Check if this block blocks motion (solid blocks)
     * Reference: BlockBehaviour.BlockStateBase::blocksMotion()
     */
    virtual bool blocksMotion() const = 0;

    /**
     * Check if this block is a leaf block
     * Reference: LeavesBlock check
     */
    virtual bool isLeaves() const = 0;

    /**
     * Get the block's identifier/name
     * For Minecraft: "minecraft:stone", "minecraft:water", etc.
     * For custom engine: whatever your engine uses
     */
    virtual std::string getIdentifier() const = 0;

    /**
     * Alias for getIdentifier() for compatibility
     */
    std::string getBlockName() const { return getIdentifier(); }

    /**
     * Get block state properties (e.g., "facing"="north", "half"="bottom")
     * Reference: StateHolder.java getValues() - returns Map<Property<?>, Comparable<?>>
     * NBT format uses string keys and string values
     * Default implementation returns empty map for blocks without properties.
     */
    virtual std::unordered_map<std::string, std::string> getProperties() const {
        return {};  // Default: no properties
    }

    /**
     * Check if this block has any state properties
     * Reference: StateHolder.java getValues().isEmpty()
     */
    virtual bool hasProperties() const {
        return !getProperties().empty();
    }

    /**
     * Compare two blocks for equality
     */
    virtual bool equals(const IBlockType* other) const = 0;

    /**
     * Check if this block is of the given type
     * Convenience method used throughout Minecraft code
     * Reference: BlockState.is(Block) in Minecraft
     */
    bool is(const IBlockType* block) const {
        return equals(block);
    }
};

} // namespace world
} // namespace minecraft

// For backwards compatibility with code using ::world::IBlockType
namespace world {
    using IBlockType = minecraft::world::IBlockType;
}
