// File: src/common/world/block/BlockRegistry.hpp
#pragma once

#include "Blocks.hpp"
#include "BlockModel.hpp"
#include <array>
#include <string>

#include <glm/glm.hpp>

namespace Server { class ServerPlayer; }

namespace Game {

    // Forward declarations only — including BlockInteraction.hpp here would
    // also expose its `class World;` forward decl (Game::World) to every TU
    // that pulls BlockRegistry.hpp transitively. That breaks files like
    // SectionDataUnpacker.cpp which write unqualified `World::NBTTagPtr`
    // expecting it to resolve to the global `::World` NBT namespace —
    // making Game::World suddenly visible shadows the lookup. Function
    // pointer typedefs only need declarations, so fwd-decl is enough.
    enum class UseResult : int;
    struct BlockHitResult;
    struct ItemStack;
    class World;

    // Render layer classification — determines which rendering pass a block uses
    enum class RenderLayer : uint8_t {
        Opaque = 0,      // Solid blocks (stone, dirt, wood)
        Cutout = 1,      // Alpha-test blocks (leaves, grass, flowers)
        Translucent = 2  // Blended blocks (glass, water, ice)
    };

    // Empty-hand right-click on this block. Mirrors MC's
    // `BlockBehaviour.useWithoutItem(level, player, hit)` (called from
    // `ServerPlayerGameMode.useItemOn` line 354 when the held item returned
    // TryEmptyHandInteraction). Examples: open door, flip lever, press button,
    // open chest, sit in boat. Default nullptr → Pass.
    using BlockUseWithoutItemFn = UseResult (*)(World* world, const glm::ivec3& pos,
                                                Server::ServerPlayer* player,
                                                const BlockHitResult& hit);

    // Right-click WITH an item on this block. Mirrors MC's
    // `BlockBehaviour.useItemOn(stack, level, player, hand, hit)` — called
    // BEFORE Item.useOn (so blocks can claim certain item interactions, like
    // fitting a banner into a pot). Default nullptr → Pass. Returning
    // TryEmptyHandInteraction from here makes the dispatch fall through to
    // useWithoutItem (i.e., "I'd react to an empty-hand click here, ignore
    // the item").
    using BlockUseItemOnFn = UseResult (*)(struct ItemStack& stack,
                                           World* world, const glm::ivec3& pos,
                                           Server::ServerPlayer* player, uint32_t hand,
                                           const BlockHitResult& hit);

    struct Block {
        std::string name;
        bool opaque;
        std::string modelName;  // Reference to BlockModel instead of texture indices

        // Optional override for blocks that don't use standard models
        std::array<uint16_t, 6> legacyTexIdx{0, 0, 0, 0, 0, 0};
        bool useLegacyTextures = false;

        // Rendering hints
        bool enableBiomeTinting = false;  // Whether this block uses biome coloring
        bool isTransparent = false;       // Whether this block has transparent parts
        RenderLayer renderLayer = RenderLayer::Opaque;

        // Per-block interaction callbacks. Default nullptr → Pass.
        BlockUseWithoutItemFn useWithoutItem = nullptr;
        BlockUseItemOnFn      useItemOn      = nullptr;
    };

    class BlockRegistry {
    public:
        static constexpr size_t Size = static_cast<size_t>(BlockID::Count);

        // Initialize the block registry
        static void Init();

        // Get block definition by ID
        static const Block& Get(BlockID id);

        // Check if a block uses model-based rendering
        static bool UsesModelRendering(BlockID id);

        // Get model for a block (returns default if not found)
        static const BlockModel& GetBlockModel(BlockID id);

        // Backing storage for all blocks
        static std::array<Block, Size> blockDefinitions;

    private:
        BlockRegistry() = delete;

        // Helper to register a block with model-based rendering
        static void RegisterModelBlock(BlockID id, const std::string& name, RenderLayer layer,
                                     const std::string& modelName);

        // Helper to register a block with legacy texture indices
        static void RegisterLegacyBlock(BlockID id, const std::string& name, bool opaque,
                                      const std::array<uint16_t, 6>& texIndices);
    };

} // namespace Game