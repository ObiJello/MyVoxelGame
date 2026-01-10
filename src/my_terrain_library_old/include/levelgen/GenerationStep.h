#pragma once

#include <cstdint>
#include <string>

// Reference: net/minecraft/world/level/levelgen/GenerationStep.java

namespace minecraft {
namespace levelgen {

/**
 * GenerationStep - Decoration steps for world generation
 * Reference: GenerationStep.java Decoration enum
 */
class GenerationStep {
public:
    /**
     * Decoration - Feature generation steps in order
     * Reference: GenerationStep.java lines 7-18
     */
    enum class Decoration : int32_t {
        RAW_GENERATION = 0,
        LAKES = 1,
        LOCAL_MODIFICATIONS = 2,
        UNDERGROUND_STRUCTURES = 3,
        SURFACE_STRUCTURES = 4,
        STRONGHOLDS = 5,
        UNDERGROUND_ORES = 6,
        UNDERGROUND_DECORATION = 7,
        FLUID_SPRINGS = 8,
        VEGETAL_DECORATION = 9,
        TOP_LAYER_MODIFICATION = 10
    };

    /**
     * Get the number of decoration steps
     */
    static constexpr int32_t DECORATION_COUNT = 11;

    /**
     * Get the name of a decoration step
     * Reference: GenerationStep.java lines 27-29
     */
    static std::string getName(Decoration step) {
        switch (step) {
            case Decoration::RAW_GENERATION: return "raw_generation";
            case Decoration::LAKES: return "lakes";
            case Decoration::LOCAL_MODIFICATIONS: return "local_modifications";
            case Decoration::UNDERGROUND_STRUCTURES: return "underground_structures";
            case Decoration::SURFACE_STRUCTURES: return "surface_structures";
            case Decoration::STRONGHOLDS: return "strongholds";
            case Decoration::UNDERGROUND_ORES: return "underground_ores";
            case Decoration::UNDERGROUND_DECORATION: return "underground_decoration";
            case Decoration::FLUID_SPRINGS: return "fluid_springs";
            case Decoration::VEGETAL_DECORATION: return "vegetal_decoration";
            case Decoration::TOP_LAYER_MODIFICATION: return "top_layer_modification";
        }
        return "unknown";
    }
};

} // namespace levelgen
} // namespace minecraft
