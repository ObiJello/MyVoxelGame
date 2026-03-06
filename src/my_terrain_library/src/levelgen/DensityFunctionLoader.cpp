#include "levelgen/DensityFunctionLoader.h"
#include "levelgen/DensityFunctions.h"
#include "levelgen/NoiseRegistry.h"
#include <fstream>
#include <stdexcept>
#include <iostream>

namespace minecraft {
namespace levelgen {

using namespace density;
using namespace DensityFunctions;

DensityFunctionLoader::DensityFunctionLoader(const std::string& dataPath)
    : m_dataPath(dataPath)
{
}

void DensityFunctionLoader::clearCache() {
    // Note: We don't delete the functions as they may be owned elsewhere
    m_cache.clear();
}

DensityFunction* DensityFunctionLoader::load(const std::string& name) {
    // Check cache first
    auto it = m_cache.find(name);
    if (it != m_cache.end()) {
        return it->second;
    }

    // Load and parse the JSON file
    json j = loadJsonFile(name);
    DensityFunction* func = parse(j);

    // Cache the result
    m_cache[name] = func;
    return func;
}

json DensityFunctionLoader::loadJsonFile(const std::string& name) {
    // Convert "minecraft:overworld/offset" to "data/minecraft/worldgen/density_function/overworld/offset.json"
    std::string path = name;

    // Remove "minecraft:" prefix if present
    if (path.find("minecraft:") == 0) {
        path = path.substr(10);
    }

    // Build full path
    std::string fullPath = m_dataPath + "/worldgen/density_function/" + path + ".json";

    // Load JSON file
    std::ifstream file(fullPath);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open density function file: " + fullPath);
    }

    json j;
    file >> j;
    return j;
}

DensityFunction* DensityFunctionLoader::parse(const json& j) {
    // Handle references (strings)
    if (j.is_string()) {
        return resolveReference(j.get<std::string>());
    }

    // Handle constants (numbers)
    if (j.is_number()) {
        return constant(j.get<double>());
    }

    // Handle objects with "type" field
    if (!j.contains("type")) {
        throw std::runtime_error("Density function JSON missing 'type' field");
    }

    std::string type = j["type"];

    if (type == "minecraft:constant" || type == "constant") {
        return parseConstant(j);
    } else if (type == "minecraft:add" || type == "add") {
        return parseAdd(j);
    } else if (type == "minecraft:mul" || type == "mul") {
        return parseMul(j);
    } else if (type == "minecraft:abs" || type == "abs") {
        return parseAbs(j);
    } else if (type == "minecraft:spline" || type == "spline") {
        return parseSpline(j);
    } else if (type == "minecraft:shifted_noise" || type == "shifted_noise") {
        return parseShiftedNoise(j);
    } else if (type == "minecraft:noise" || type == "noise") {
        return parseNoise(j);
    } else if (type == "minecraft:y_clamped_gradient" || type == "y_clamped_gradient") {
        return parseYClampedGradient(j);
    } else if (type == "minecraft:blend_alpha" || type == "blend_alpha") {
        return parseBlendAlpha(j);
    } else if (type == "minecraft:blend_offset" || type == "blend_offset") {
        return parseBlendOffset(j);
    } else if (type == "minecraft:flat_cache" || type == "flat_cache") {
        return parseFlatCache(j);
    } else if (type == "minecraft:cache_2d" || type == "cache_2d") {
        return parseCache2D(j);
    } else if (type == "minecraft:cache_once" || type == "cache_once") {
        return parseCacheOnce(j);
    } else if (type == "minecraft:interpolated" || type == "interpolated") {
        return parseInterpolated(j);
    } else if (type == "minecraft:cache_all_in_cell" || type == "cache_all_in_cell") {
        return parseCacheAllInCell(j);
    } else if (type == "minecraft:shift_a" || type == "shift_a") {
        // shift_a samples noise at (blockX, 0, blockZ) * 0.25 and multiplies by 4.0
        // Argument is always a noise reference
        std::string noiseRef = j["argument"];
        if (noiseRef.find("minecraft:") == 0) {
            noiseRef = noiseRef.substr(10);
        }
        DensityFunction::NoiseHolder* noiseHolder = new DensityFunction::NoiseHolder(internString(noiseRef));
        return DensityFunctions::shiftA(noiseHolder);
    } else if (type == "minecraft:shift_b" || type == "shift_b") {
        // shift_b samples noise at (0, blockY, 0) * 0.25 and multiplies by 4.0
        // Argument is always a noise reference
        std::string noiseRef = j["argument"];
        if (noiseRef.find("minecraft:") == 0) {
            noiseRef = noiseRef.substr(10);
        }
        DensityFunction::NoiseHolder* noiseHolder = new DensityFunction::NoiseHolder(internString(noiseRef));
        return DensityFunctions::shiftB(noiseHolder);
    } else {
        throw std::runtime_error("Unknown density function type: " + type);
    }
}

DensityFunction* DensityFunctionLoader::parseArgument(const json& arg) {
    return parse(arg);
}

const char* DensityFunctionLoader::internString(const std::string& str) {
    auto result = m_stringPool.insert(str);
    return result.first->c_str();
}

DensityFunction* DensityFunctionLoader::resolveReference(const std::string& ref) {
    // First try to load as a density function
    // Density function references can be:
    // - "minecraft:overworld/offset" (with namespace and path)
    // - "minecraft:shift_x" (root level density functions)
    // Only if the file doesn't exist as a density function, treat it as a noise

    std::string refName = ref;
    if (refName.find("minecraft:") == 0) {
        refName = refName.substr(10);
    }

    // Check if density function file exists
    std::string testPath = m_dataPath + "/worldgen/density_function/" + refName + ".json";
    std::ifstream testFile(testPath);

    if (testFile.good()) {
        testFile.close();
        // It's a density function
        return load(ref);
    } else {
        // It's a noise - create a NoiseHolder wrapped in Noise
        DensityFunction::NoiseHolder* noiseHolder = new DensityFunction::NoiseHolder(internString(refName));
        return DensityFunctions::noise(noiseHolder, 1.0, 1.0);
    }
}

// ============================================================================
// Type-specific parsers
// ============================================================================

DensityFunction* DensityFunctionLoader::parseConstant(const json& j) {
    if (j.contains("argument")) {
        return constant(j["argument"].get<double>());
    } else if (j.contains("value")) {
        return constant(j["value"].get<double>());
    }
    throw std::runtime_error("Constant density function missing value");
}

DensityFunction* DensityFunctionLoader::parseAdd(const json& j) {
    DensityFunction* arg1 = parseArgument(j["argument1"]);
    DensityFunction* arg2 = parseArgument(j["argument2"]);
    return add(arg1, arg2);
}

DensityFunction* DensityFunctionLoader::parseMul(const json& j) {
    DensityFunction* arg1 = parseArgument(j["argument1"]);
    DensityFunction* arg2 = parseArgument(j["argument2"]);
    return mul(arg1, arg2);
}

DensityFunction* DensityFunctionLoader::parseAbs(const json& j) {
    DensityFunction* arg = parseArgument(j["argument"]);
    return abs(arg);
}

DensityFunction* DensityFunctionLoader::parseSpline(const json& j) {
    using SplineType = DensityFunctions::Spline::SplineType;
    using Coordinate = DensityFunctions::Spline::Coordinate;

    const json& splineData = j["spline"];

    // Parse the coordinate
    Coordinate* coordinate = parseCoordinate(splineData["coordinate"]);

    // Parse the spline definition
    SplineType* splineObj = parseSplineDefinition(splineData, coordinate);

    return spline(splineObj);
}

minecraft::density::DensityFunctions::Spline::Coordinate* DensityFunctionLoader::parseCoordinate(const json& coordJson) {
    DensityFunction* func = parse(coordJson);
    return new minecraft::density::DensityFunctions::Spline::Coordinate(func);
}

minecraft::density::DensityFunctions::Spline::SplineType* DensityFunctionLoader::parseSplineDefinition(
    const json& splineJson,
    minecraft::density::DensityFunctions::Spline::Coordinate* coordinate)
{
    using SplineType = DensityFunctions::Spline::SplineType;
    using Coordinate = DensityFunctions::Spline::Coordinate;

    const json& points = splineJson["points"];

    std::vector<float> locations;
    std::vector<SplineType*> values;
    std::vector<float> derivatives;

    for (size_t i = 0; i < points.size(); i++) {
        const auto& point = points[i];
        float location = point["location"].get<float>();
        float derivative = point["derivative"].get<float>();

        locations.push_back(location);
        derivatives.push_back(derivative);

        // Parse the value (can be a number or nested spline)
        const json& valueJson = point["value"];
        SplineType* value;

        if (valueJson.is_number()) {
            float constVal = valueJson.get<float>();
            value = SplineType::constant(constVal);
        } else if (valueJson.is_object() && valueJson.contains("coordinate")) {
            // Nested spline
            Coordinate* nestedCoord = parseCoordinate(valueJson["coordinate"]);
            value = parseSplineDefinition(valueJson, nestedCoord);
        } else {
            throw std::runtime_error("Invalid spline point value");
        }

        values.push_back(value);
    }

    return SplineType::Multipoint::create(coordinate, locations, values, derivatives);
}

DensityFunction* DensityFunctionLoader::parseShiftedNoise(const json& j) {
    // Parse the noise name - this is always just a noise, not a density function
    std::string noiseNameStr = j["noise"];
    // Remove "minecraft:" prefix if present
    if (noiseNameStr.find("minecraft:") == 0) {
        noiseNameStr = noiseNameStr.substr(10);
    }
    DensityFunction::NoiseHolder* noise = new DensityFunction::NoiseHolder(internString(noiseNameStr));

    // Parse shift functions if provided - these ARE density functions
    DensityFunction* shiftX = nullptr;
    DensityFunction* shiftZ = nullptr;

    if (j.contains("shift_x")) {
        shiftX = parseArgument(j["shift_x"]);  // Will resolve string references
    }

    if (j.contains("shift_z")) {
        shiftZ = parseArgument(j["shift_z"]);  // Will resolve string references
    }

    double xzScale = j.value("xz_scale", 1.0);
    double yScale = j.value("y_scale", 1.0);

    if (shiftX && shiftZ) {
        return shiftedNoise2d(shiftX, shiftZ, xzScale, noise);
    } else {
        // No shift - just use regular noise
        return DensityFunctions::noise(noise, xzScale, yScale);
    }
}

DensityFunction* DensityFunctionLoader::parseNoise(const json& j) {
    std::string noiseName = j["noise"];
    DensityFunction::NoiseHolder* noise = new DensityFunction::NoiseHolder(internString(noiseName));

    double xzScale = j.value("xz_scale", 1.0);
    double yScale = j.value("y_scale", 1.0);

    return DensityFunctions::noise(noise, xzScale, yScale);
}

DensityFunction* DensityFunctionLoader::parseYClampedGradient(const json& j) {
    int fromY = j["from_y"];
    int toY = j["to_y"];
    double fromValue = j["from_value"];
    double toValue = j["to_value"];

    return yClampedGradient(fromY, toY, fromValue, toValue);
}

DensityFunction* DensityFunctionLoader::parseBlendAlpha(const json& j) {
    // For Phase 1, return a constant 1.0 (no blending)
    return constant(1.0);
}

DensityFunction* DensityFunctionLoader::parseBlendOffset(const json& j) {
    // For Phase 1, return a constant 0.0 (no blending)
    return constant(0.0);
}

DensityFunction* DensityFunctionLoader::parseFlatCache(const json& j) {
    DensityFunction* arg = parseArgument(j["argument"]);
    return flatCache(arg);
}

DensityFunction* DensityFunctionLoader::parseCache2D(const json& j) {
    DensityFunction* arg = parseArgument(j["argument"]);
    return cache2d(arg);
}

DensityFunction* DensityFunctionLoader::parseCacheOnce(const json& j) {
    DensityFunction* arg = parseArgument(j["argument"]);
    return cacheOnce(arg);
}

DensityFunction* DensityFunctionLoader::parseInterpolated(const json& j) {
    DensityFunction* arg = parseArgument(j["argument"]);
    return interpolated(arg);
}

DensityFunction* DensityFunctionLoader::parseCacheAllInCell(const json& j) {
    DensityFunction* arg = parseArgument(j["argument"]);
    return cacheAllInCell(arg);
}

} // namespace levelgen
} // namespace minecraft
