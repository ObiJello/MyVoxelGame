#pragma once

#include "levelgen/DensityFunction.h"
#include "levelgen/DensityFunctions.h"
#include "external/json.hpp"
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <memory>

namespace minecraft {
namespace levelgen {

using json = nlohmann::json;
using namespace density;

/**
 * DensityFunctionLoader - Loads density functions from Minecraft's JSON data files
 *
 * This loader parses the worldgen/density_function JSON files and constructs
 * the corresponding DensityFunction objects, handling references and nested structures.
 */
class DensityFunctionLoader {
public:
    /**
     * Create a loader with a base data directory path
     * @param dataPath Path to the minecraft data directory (e.g., "data/minecraft")
     */
    explicit DensityFunctionLoader(const std::string& dataPath);

    /**
     * Load a density function by name
     * @param name The density function name (e.g., "minecraft:overworld/offset")
     * @return The loaded density function, or nullptr if not found
     */
    DensityFunction* load(const std::string& name);

    /**
     * Parse a density function from JSON
     * @param j The JSON object representing the density function
     * @return The constructed density function
     */
    DensityFunction* parse(const json& j);

    /**
     * Clear the cache of loaded functions
     */
    void clearCache();

private:
    std::string m_dataPath;
    std::unordered_map<std::string, DensityFunction*> m_cache;
    std::unordered_set<std::string> m_stringPool;  // Keeps strings alive for const char* references

    // Helper methods for parsing specific types
    DensityFunction* parseConstant(const json& j);
    DensityFunction* parseAdd(const json& j);
    DensityFunction* parseMul(const json& j);
    DensityFunction* parseAbs(const json& j);
    DensityFunction* parseSpline(const json& j);
    DensityFunction* parseShiftedNoise(const json& j);
    DensityFunction* parseNoise(const json& j);
    DensityFunction* parseYClampedGradient(const json& j);
    DensityFunction* parseBlendAlpha(const json& j);
    DensityFunction* parseBlendOffset(const json& j);
    DensityFunction* parseFlatCache(const json& j);
    DensityFunction* parseCache2D(const json& j);
    DensityFunction* parseCacheOnce(const json& j);
    DensityFunction* parseInterpolated(const json& j);
    DensityFunction* parseCacheAllInCell(const json& j);

    // Helper for parsing spline coordinates and points
    minecraft::density::DensityFunctions::Spline::SplineType* parseSplineDefinition(
        const json& splineJson,
        minecraft::density::DensityFunctions::Spline::Coordinate* coordinate);
    minecraft::density::DensityFunctions::Spline::Coordinate* parseCoordinate(const json& coordJson);

    // Helper to resolve references like "minecraft:overworld/continents"
    DensityFunction* resolveReference(const std::string& ref);

    // Helper to load JSON file
    json loadJsonFile(const std::string& name);

    // Helper to parse argument that can be a number, string reference, or object
    DensityFunction* parseArgument(const json& arg);

    // Helper to store a string permanently and return a const char* to it
    const char* internString(const std::string& str);
};

} // namespace levelgen
} // namespace minecraft
