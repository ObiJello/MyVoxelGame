#pragma once

#include "IBlockType.h"
#include <string>
#include <unordered_map>
#include <memory>

namespace world {

/**
 * Concrete implementation of IBlockType for Minecraft blocks.
 * Used for verification against Minecraft's terrain generation.
 */
class MinecraftBlockType : public IBlockType {
private:
    std::string m_identifier;
    bool m_isAir;
    bool m_isFluid;
    bool m_blocksMotion;
    bool m_isLeaves;

public:
    MinecraftBlockType(const std::string& identifier, bool isAir, bool isFluid,
                       bool blocksMotion = true, bool isLeaves = false)
        : m_identifier(identifier), m_isAir(isAir), m_isFluid(isFluid),
          m_blocksMotion(blocksMotion), m_isLeaves(isLeaves) {
        // Air and fluids don't block motion by default
        if (isAir || isFluid) {
            m_blocksMotion = false;
        }
    }

    bool isAir() const override {
        return m_isAir;
    }

    bool isFluid() const override {
        return m_isFluid;
    }

    bool blocksMotion() const override {
        return m_blocksMotion;
    }

    bool isLeaves() const override {
        return m_isLeaves;
    }

    std::string getIdentifier() const override {
        return m_identifier;
    }

    bool equals(const IBlockType* other) const override {
        if (other == nullptr) return false;
        return m_identifier == other->getIdentifier();
    }
};

/**
 * Registry of commonly used Minecraft blocks.
 * This is a singleton that provides access to block instances.
 */
class MinecraftBlocks {
private:
    MinecraftBlocks() = default;

public:
    static MinecraftBlocks& instance() {
        static MinecraftBlocks inst;
        return inst;
    }

    // Common blocks
    static IBlockType* AIR() {
        static MinecraftBlockType air("minecraft:air", true, false);
        return &air;
    }

    static IBlockType* CAVE_AIR() {
        static MinecraftBlockType caveAir("minecraft:cave_air", true, false);
        return &caveAir;
    }

    static IBlockType* STONE() {
        static MinecraftBlockType stone("minecraft:stone", false, false);
        return &stone;
    }

    static IBlockType* WATER() {
        static MinecraftBlockType water("minecraft:water", false, true);
        return &water;
    }

    static IBlockType* LAVA() {
        static MinecraftBlockType lava("minecraft:lava", false, true);
        return &lava;
    }

    static IBlockType* DEEPSLATE() {
        static MinecraftBlockType deepslate("minecraft:deepslate", false, false);
        return &deepslate;
    }

    static IBlockType* BEDROCK() {
        static MinecraftBlockType bedrock("minecraft:bedrock", false, false);
        return &bedrock;
    }

    static IBlockType* GRASS_BLOCK() {
        static MinecraftBlockType grass("minecraft:grass_block", false, false);
        return &grass;
    }

    static IBlockType* DIRT() {
        static MinecraftBlockType dirt("minecraft:dirt", false, false);
        return &dirt;
    }

    static IBlockType* SAND() {
        static MinecraftBlockType sand("minecraft:sand", false, false);
        return &sand;
    }

    static IBlockType* GRAVEL() {
        static MinecraftBlockType gravel("minecraft:gravel", false, false);
        return &gravel;
    }

    // Ore vein blocks
    static IBlockType* COPPER_ORE() {
        static MinecraftBlockType copperOre("minecraft:copper_ore", false, false);
        return &copperOre;
    }

    static IBlockType* DEEPSLATE_IRON_ORE() {
        static MinecraftBlockType ironOre("minecraft:deepslate_iron_ore", false, false);
        return &ironOre;
    }

    static IBlockType* RAW_COPPER_BLOCK() {
        static MinecraftBlockType rawCopper("minecraft:raw_copper_block", false, false);
        return &rawCopper;
    }

    static IBlockType* RAW_IRON_BLOCK() {
        static MinecraftBlockType rawIron("minecraft:raw_iron_block", false, false);
        return &rawIron;
    }

    static IBlockType* GRANITE() {
        static MinecraftBlockType granite("minecraft:granite", false, false);
        return &granite;
    }

    static IBlockType* TUFF() {
        static MinecraftBlockType tuff("minecraft:tuff", false, false);
        return &tuff;
    }

    static IBlockType* SNOW_BLOCK() {
        static MinecraftBlockType snowBlock("minecraft:snow_block", false, false);
        return &snowBlock;
    }

    static IBlockType* PACKED_ICE() {
        static MinecraftBlockType packedIce("minecraft:packed_ice", false, false);
        return &packedIce;
    }

    static IBlockType* ICE() {
        static MinecraftBlockType ice("minecraft:ice", false, false);
        return &ice;
    }

    static IBlockType* SANDSTONE() {
        static MinecraftBlockType sandstone("minecraft:sandstone", false, false);
        return &sandstone;
    }

    static IBlockType* POWDER_SNOW() {
        static MinecraftBlockType powderSnow("minecraft:powder_snow", false, false, false);
        return &powderSnow;
    }

    // Leaves blocks (don't block motion for heightmap purposes)
    static IBlockType* OAK_LEAVES() {
        static MinecraftBlockType leaves("minecraft:oak_leaves", false, false, false, true);
        return &leaves;
    }

    static IBlockType* SPRUCE_LEAVES() {
        static MinecraftBlockType leaves("minecraft:spruce_leaves", false, false, false, true);
        return &leaves;
    }

    static IBlockType* BIRCH_LEAVES() {
        static MinecraftBlockType leaves("minecraft:birch_leaves", false, false, false, true);
        return &leaves;
    }

    static IBlockType* JUNGLE_LEAVES() {
        static MinecraftBlockType leaves("minecraft:jungle_leaves", false, false, false, true);
        return &leaves;
    }

    static IBlockType* ACACIA_LEAVES() {
        static MinecraftBlockType leaves("minecraft:acacia_leaves", false, false, false, true);
        return &leaves;
    }

    static IBlockType* DARK_OAK_LEAVES() {
        static MinecraftBlockType leaves("minecraft:dark_oak_leaves", false, false, false, true);
        return &leaves;
    }

    static IBlockType* AZALEA_LEAVES() {
        static MinecraftBlockType leaves("minecraft:azalea_leaves", false, false, false, true);
        return &leaves;
    }

    static IBlockType* FLOWERING_AZALEA_LEAVES() {
        static MinecraftBlockType leaves("minecraft:flowering_azalea_leaves", false, false, false, true);
        return &leaves;
    }

    static IBlockType* MANGROVE_LEAVES() {
        static MinecraftBlockType leaves("minecraft:mangrove_leaves", false, false, false, true);
        return &leaves;
    }

    static IBlockType* CHERRY_LEAVES() {
        static MinecraftBlockType leaves("minecraft:cherry_leaves", false, false, false, true);
        return &leaves;
    }

    // Terracotta blocks (used in badlands surface system)
    static IBlockType* TERRACOTTA() {
        static MinecraftBlockType block("minecraft:terracotta", false, false);
        return &block;
    }

    static IBlockType* WHITE_TERRACOTTA() {
        static MinecraftBlockType block("minecraft:white_terracotta", false, false);
        return &block;
    }

    static IBlockType* ORANGE_TERRACOTTA() {
        static MinecraftBlockType block("minecraft:orange_terracotta", false, false);
        return &block;
    }

    static IBlockType* YELLOW_TERRACOTTA() {
        static MinecraftBlockType block("minecraft:yellow_terracotta", false, false);
        return &block;
    }

    static IBlockType* BROWN_TERRACOTTA() {
        static MinecraftBlockType block("minecraft:brown_terracotta", false, false);
        return &block;
    }

    static IBlockType* RED_TERRACOTTA() {
        static MinecraftBlockType block("minecraft:red_terracotta", false, false);
        return &block;
    }

    static IBlockType* LIGHT_GRAY_TERRACOTTA() {
        static MinecraftBlockType block("minecraft:light_gray_terracotta", false, false);
        return &block;
    }

    /**
     * Get a block by name - creates a static instance if needed
     * For commonly used blocks, prefer the named methods (STONE(), WATER(), etc.)
     */
    static IBlockType* get(const std::string& name) {
        // Check common blocks first
        if (name == "minecraft:air") return AIR();
        if (name == "minecraft:cave_air") return CAVE_AIR();
        if (name == "minecraft:stone") return STONE();
        if (name == "minecraft:water") return WATER();
        if (name == "minecraft:lava") return LAVA();
        if (name == "minecraft:deepslate") return DEEPSLATE();
        if (name == "minecraft:bedrock") return BEDROCK();
        if (name == "minecraft:grass_block") return GRASS_BLOCK();
        if (name == "minecraft:dirt") return DIRT();
        if (name == "minecraft:sand") return SAND();
        if (name == "minecraft:gravel") return GRAVEL();
        if (name == "minecraft:copper_ore") return COPPER_ORE();
        if (name == "minecraft:deepslate_iron_ore") return DEEPSLATE_IRON_ORE();
        if (name == "minecraft:raw_copper_block") return RAW_COPPER_BLOCK();
        if (name == "minecraft:raw_iron_block") return RAW_IRON_BLOCK();
        if (name == "minecraft:granite") return GRANITE();
        if (name == "minecraft:tuff") return TUFF();
        if (name == "minecraft:snow_block") return SNOW_BLOCK();
        if (name == "minecraft:packed_ice") return PACKED_ICE();
        if (name == "minecraft:ice") return ICE();
        if (name == "minecraft:sandstone") return SANDSTONE();
        if (name == "minecraft:powder_snow") return POWDER_SNOW();

        // Terracotta blocks
        if (name == "minecraft:terracotta") return TERRACOTTA();
        if (name == "minecraft:white_terracotta") return WHITE_TERRACOTTA();
        if (name == "minecraft:orange_terracotta") return ORANGE_TERRACOTTA();
        if (name == "minecraft:yellow_terracotta") return YELLOW_TERRACOTTA();
        if (name == "minecraft:brown_terracotta") return BROWN_TERRACOTTA();
        if (name == "minecraft:red_terracotta") return RED_TERRACOTTA();
        if (name == "minecraft:light_gray_terracotta") return LIGHT_GRAY_TERRACOTTA();

        // Leaves blocks
        if (name == "minecraft:oak_leaves") return OAK_LEAVES();
        if (name == "minecraft:spruce_leaves") return SPRUCE_LEAVES();
        if (name == "minecraft:birch_leaves") return BIRCH_LEAVES();
        if (name == "minecraft:jungle_leaves") return JUNGLE_LEAVES();
        if (name == "minecraft:acacia_leaves") return ACACIA_LEAVES();
        if (name == "minecraft:dark_oak_leaves") return DARK_OAK_LEAVES();
        if (name == "minecraft:azalea_leaves") return AZALEA_LEAVES();
        if (name == "minecraft:flowering_azalea_leaves") return FLOWERING_AZALEA_LEAVES();
        if (name == "minecraft:mangrove_leaves") return MANGROVE_LEAVES();
        if (name == "minecraft:cherry_leaves") return CHERRY_LEAVES();

        // For unknown blocks, create a generic block with appropriate properties
        static std::unordered_map<std::string, std::unique_ptr<MinecraftBlockType>> dynamicBlocks;
        auto it = dynamicBlocks.find(name);
        if (it == dynamicBlocks.end()) {
            // Check if it's a leaves block by name pattern
            bool isLeaves = name.find("_leaves") != std::string::npos;
            bool blocksMotion = !isLeaves;
            dynamicBlocks[name] = std::make_unique<MinecraftBlockType>(name, false, false, blocksMotion, isLeaves);
        }
        return dynamicBlocks[name].get();
    }
};

} // namespace world
