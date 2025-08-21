// File: src/common/world/block/Block.hpp
#pragma once

#include "Blocks.hpp"
#include "BlockInteraction.hpp"
#include <glm/glm.hpp>
#include <string>

namespace Game {
    
    class World;
    
    // Base class for all block types
    class Block {
    public:
        Block(BlockID id, const std::string& name) 
            : m_id(id), m_name(name) {}
        
        virtual ~Block() = default;
        
        // === PROPERTIES ===
        
        // Get the block ID
        BlockID getId() const { return m_id; }
        
        // Get the block name
        const std::string& getName() const { return m_name; }
        
        // Check if this block can be replaced by another block (like air, water, grass, snow)
        virtual bool isReplaceable(World* world, const glm::ivec3& pos) const {
            // By default, only air is replaceable
            return m_id == BlockID::Air;
        }
        
        // Check if this block can survive at the given position
        virtual bool canSurvive(World* world, const glm::ivec3& pos) const {
            // Most blocks can survive anywhere by default
            return true;
        }
        
        // Check if this block is solid (blocks movement)
        virtual bool isSolid() const {
            // Most blocks are solid by default
            return m_id != BlockID::Air && m_id != BlockID::Water && m_id != BlockID::Lava;
        }
        
        // Check if this block is transparent
        virtual bool isTransparent() const {
            return m_id == BlockID::Air || m_id == BlockID::Glass || 
                   m_id == BlockID::Water || m_id == BlockID::Lava ||
                   m_id == BlockID::OakLeaves || m_id == BlockID::BirchLeaves ||
                   m_id == BlockID::SpruceLeaves || m_id == BlockID::CherryLeaves;
        }
        
        // Check if this block requires a block entity (tile entity)
        virtual bool hasBlockEntity() const {
            // TODO: Implement for chests, furnaces, etc.
            return false;
        }
        
        // === INTERACTIONS ===
        
        // Called when the block is right-clicked (before item use)
        virtual UseResult use(World* world, const glm::ivec3& pos, 
                            Server::ServerPlayer* player, uint32_t hand, 
                            const BlockHitResult& hit) {
            // Most blocks don't have a use action
            return UseResult::Pass;
        }
        
        // === EVENTS ===
        
        // Called when this block is placed
        virtual void onPlace(World* world, const glm::ivec3& pos, 
                           Server::ServerPlayer* player) {
            // Default: no special placement behavior
        }
        
        // Called when this block is removed
        virtual void onRemove(World* world, const glm::ivec3& pos) {
            // Default: no special removal behavior
        }
        
        // Called when a neighbor block changes
        virtual void onNeighborChanged(World* world, const glm::ivec3& pos, 
                                      const glm::ivec3& neighborPos) {
            // Default: check if we can still survive
            if (!canSurvive(world, pos)) {
                // TODO: Schedule block removal
            }
        }
        
        // === BLOCK STATE ===
        
        // Get the default state for placement
        virtual uint32_t getDefaultState() const {
            return 0; // No additional state by default
        }
        
        // Get state for placement based on context
        virtual uint32_t getStateForPlacement(const UseOnContext& context) const {
            return getDefaultState();
        }
        
        // === RENDERING ===
        
        // Get the render layer for this block
        enum class RenderLayer {
            OPAQUE,      // Solid blocks
            CUTOUT,      // Alpha-tested blocks (leaves, grass)
            TRANSLUCENT  // Blended blocks (glass, water)
        };
        
        virtual RenderLayer getRenderLayer() const {
            if (isTransparent()) {
                if (m_id == BlockID::Glass || m_id == BlockID::Water || 
                    m_id == BlockID::Lava || m_id == BlockID::Ice) {
                    return RenderLayer::TRANSLUCENT;
                }
                return RenderLayer::CUTOUT;
            }
            return RenderLayer::OPAQUE;
        }
        
    protected:
        BlockID m_id;
        std::string m_name;
    };
    
    // === SPECIFIC BLOCK IMPLEMENTATIONS ===
    
    // Air block - always replaceable
    class AirBlock : public Block {
    public:
        AirBlock() : Block(BlockID::Air, "air") {}
        
        bool isReplaceable(World* world, const glm::ivec3& pos) const override {
            return true;
        }
        
        bool isSolid() const override {
            return false;
        }
        
        bool isTransparent() const override {
            return true;
        }
    };
    
    // Snow layer - replaceable
    class SnowBlock : public Block {
    public:
        SnowBlock() : Block(BlockID::Snow, "snow") {}
        
        bool isReplaceable(World* world, const glm::ivec3& pos) const override {
            // TODO: Check snow layer height, only replaceable if thin
            return true;
        }
        
        bool canSurvive(World* world, const glm::ivec3& pos) const override {
            // TODO: Check if block below is solid
            return true;
        }
    };
    
    // Grass (tall grass) - replaceable
    class TallGrassBlock : public Block {
    public:
        TallGrassBlock() : Block(BlockID::Grass, "grass") {}
        
        bool isReplaceable(World* world, const glm::ivec3& pos) const override {
            return true;
        }
        
        bool isSolid() const override {
            return false;
        }
        
        bool canSurvive(World* world, const glm::ivec3& pos) const override {
            // TODO: Check if block below is grass block or dirt
            return true;
        }
    };
    
    // Water - partially replaceable (for waterlogging)
    class WaterBlock : public Block {
    public:
        WaterBlock() : Block(BlockID::Water, "water") {}
        
        bool isReplaceable(World* world, const glm::ivec3& pos) const override {
            // TODO: Only replaceable by waterloggable blocks
            return false;
        }
        
        bool isSolid() const override {
            return false;
        }
        
        bool isTransparent() const override {
            return true;
        }
        
        UseResult use(World* world, const glm::ivec3& pos,
                     Server::ServerPlayer* player, uint32_t hand,
                     const BlockHitResult& hit) override {
            // TODO: Check if holding bucket
            return UseResult::Pass;
        }
    };
    
} // namespace Game