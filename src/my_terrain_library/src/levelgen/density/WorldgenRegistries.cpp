#include "levelgen/density/WorldgenRegistries.h"

#include "levelgen/density/CoreFunctions.h"
#include "levelgen/density/CubicSpline.h"
#include "levelgen/density/DistanceMetric.h"
#include "levelgen/density/TilingMode.h"
#include "levelgen/density/generator/DistanceToPointFunction.h"
#include "levelgen/density/generator/EndIslandFunction.h"
#include "levelgen/density/generator/GradientFunction.h"
#include "levelgen/density/generator/NoiseFunction.h"
#include "levelgen/density/generator/ShiftNoiseFunction.h"
#include "levelgen/density/generator/SimpleDensityFunction.h"
#include "levelgen/density/op/BinaryFunction.h"
#include "levelgen/density/op/BlendDensityFunction.h"
#include "levelgen/density/op/ClampFunction.h"
#include "levelgen/density/op/FindTopSurfaceFunction.h"
#include "levelgen/density/op/InterpolatedFunction.h"
#include "levelgen/density/op/IntervalSelectFunction.h"
#include "levelgen/density/op/LerpFunction.h"
#include "levelgen/density/op/PowFunction.h"
#include "levelgen/density/op/RangeChoiceFunction.h"
#include "levelgen/density/op/RoundFunction.h"
#include "levelgen/density/op/SplineFunction.h"
#include "levelgen/density/op/UnaryFunction.h"
#include "levelgen/density/synth/BlendedNoise.h"
#include "levelgen/density/synth/NormalNoise.h"

#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <vector>

// Reference: DensityFunctions.bootstrap / DIRECT_CODEC and every function's
// CODEC (26.3), CubicSpline.codec, NormalNoise.CODEC.

namespace minecraft {
namespace levelgen {
namespace density {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

// DimensionType.MIN_Y / MAX_Y.
constexpr int MIN_Y = -2032;
constexpr int MAX_Y = 2031;

[[noreturn]] void fail(const std::string& message) { throw std::runtime_error("density codec: " + message); }

const json& field(const json& object, const char* name) {
    auto it = object.find(name);
    if (it == object.end()) fail(std::string("No key ") + name + " in " + object.dump().substr(0, 200));
    return *it;
}

double doubleField(const json& object, const char* name) {
    const json& value = field(object, name);
    if (!value.is_number()) fail(std::string("Not a number: ") + name);
    return value.get<double>();
}

int intField(const json& object, const char* name) {
    const json& value = field(object, name);
    if (!value.is_number_integer()) fail(std::string("Not an integer: ") + name);
    return value.get<int>();
}

// Codec.FLOAT: the JSON number narrowed to float.
float floatValue(const json& value, const char* what) {
    if (!value.is_number()) fail(std::string("Not a number: ") + what);
    return static_cast<float>(value.get<double>());
}

// DensityFunctions.NOISE_VALUE_CODEC: Codec.floatRange(-1e6, 1e6).
float noiseValue(const json& value, const char* what) {
    const float v = floatValue(value, what);
    if (!(v >= -1000000.0f && v <= 1000000.0f)) {
        fail(std::string("Value ") + std::to_string(v) + " outside of range [-1000000.0:1000000.0] (" + what + ")");
    }
    return v;
}

int positiveInt(const json& object, const char* name) {
    const int v = intField(object, name);
    if (v <= 0) fail(std::string("Value must be positive: ") + name);
    return v;
}

// BlendedNoise.SCALE_RANGE: Codec.doubleRange(0.001, 1000.0).
double scaleRange(const json& object, const char* name) {
    const double v = doubleField(object, name);
    if (!(v >= 0.001 && v <= 1000.0)) fail(std::string("Value outside of range [0.001:1000.0]: ") + name);
    return v;
}

Axis axisValue(const json& value) {
    const std::string s = value.get<std::string>();
    if (s == "x") return Axis::X;
    if (s == "y") return Axis::Y;
    if (s == "z") return Axis::Z;
    fail("Unknown axis " + s);
}

TilingMode tilingValue(const json& value) {
    const std::string s = value.get<std::string>();
    if (s == "clamp_to_edge") return TilingMode::CLAMP_TO_EDGE;
    if (s == "repeat") return TilingMode::REPEAT;
    if (s == "mirrored_repeat") return TilingMode::MIRRORED_REPEAT;
    fail("Unknown tiling mode " + s);
}

DistanceMetric metricValue(const json& value) {
    const std::string s = value.get<std::string>();
    if (s == "euclidean") return DistanceMetric::EUCLIDEAN;
    if (s == "euclidean_squared") return DistanceMetric::EUCLIDEAN_SQUARED;
    if (s == "manhattan") return DistanceMetric::MANHATTAN;
    if (s == "chebyshev") return DistanceMetric::CHEBYSHEV;
    fail("Unknown distance metric " + s);
}

bool unaryType(const std::string& id, UnaryFunction::Type& out) {
    static const std::pair<const char*, UnaryFunction::Type> kTypes[] = {
        {"abs", UnaryFunction::Type::ABS},
        {"square", UnaryFunction::Type::SQUARE},
        {"cube", UnaryFunction::Type::CUBE},
        {"sqrt", UnaryFunction::Type::SQRT},
        {"half_negative", UnaryFunction::Type::HALF_NEGATIVE},
        {"quarter_negative", UnaryFunction::Type::QUARTER_NEGATIVE},
        {"reciprocal", UnaryFunction::Type::RECIPROCAL},
        {"negate", UnaryFunction::Type::NEGATE},
        {"squeeze", UnaryFunction::Type::SQUEEZE},
        {"log", UnaryFunction::Type::LOG},
        {"sign", UnaryFunction::Type::SIGN},
    };
    for (const auto& [name, type] : kTypes) {
        if (id == name) { out = type; return true; }
    }
    return false;
}

bool roundType(const std::string& id, RoundFunction::Type& out) {
    static const std::pair<const char*, RoundFunction::Type> kTypes[] = {
        {"floor", RoundFunction::Type::FLOOR},
        {"round", RoundFunction::Type::ROUND},
        {"ceil", RoundFunction::Type::CEIL},
        {"truncate", RoundFunction::Type::TRUNCATE},
    };
    for (const auto& [name, type] : kTypes) {
        if (id == name) { out = type; return true; }
    }
    return false;
}

bool binaryType(const std::string& id, BinaryFunction::Type& out) {
    static const std::pair<const char*, BinaryFunction::Type> kTypes[] = {
        {"add", BinaryFunction::Type::ADD},
        {"sub", BinaryFunction::Type::SUB},
        {"mul", BinaryFunction::Type::MUL},
        {"div", BinaryFunction::Type::DIV},
        {"min", BinaryFunction::Type::MIN},
        {"max", BinaryFunction::Type::MAX},
    };
    for (const auto& [name, type] : kTypes) {
        if (id == name) { out = type; return true; }
    }
    return false;
}

fs::path locateDataRoot() {
    if (const char* env = std::getenv("MC_DATA_ROOT")) {
        fs::path root(env);
        if (fs::is_directory(root)) return root;
        throw std::runtime_error(std::string("MC_DATA_ROOT not a directory: ") + env);
    }
    fs::path current = fs::current_path();
    while (!current.empty()) {
        fs::path candidate = current / "data";
        if (fs::is_directory(candidate)) return candidate;
        if (current == current.root_path()) break;
        current = current.parent_path();
    }
    throw std::runtime_error("Data root not found for worldgen registries");
}

} // namespace

WorldgenRegistries& WorldgenRegistries::get() {
    static WorldgenRegistries registries(locateDataRoot());
    return registries;
}

WorldgenRegistries::WorldgenRegistries(fs::path dataRoot) : m_dataRoot(std::move(dataRoot)) {}

std::string WorldgenRegistries::normalizeKey(const std::string& key) {
    return key.find(':') == std::string::npos ? "minecraft:" + key : key;
}

fs::path WorldgenRegistries::entryPath(const std::string& registry, const std::string& key) const {
    const std::string id = normalizeKey(key);
    const size_t colon = id.find(':');
    return m_dataRoot / id.substr(0, colon) / "worldgen" / registry / (id.substr(colon + 1) + ".json");
}

bool WorldgenRegistries::hasEntry(const std::string& registry, const std::string& key) const {
    return fs::is_regular_file(entryPath(registry, key));
}

json WorldgenRegistries::readEntry(const std::string& registry, const std::string& key) const {
    const fs::path path = entryPath(registry, key);
    std::ifstream in(path);
    if (!in) throw std::runtime_error("worldgen/" + registry + ": no entry " + normalizeKey(key) + " (" + path.string() + ")");
    try {
        return json::parse(in);
    } catch (const std::exception& e) {
        throw std::runtime_error("worldgen/" + registry + " " + normalizeKey(key) + ": " + e.what());
    }
}

// ---- Registries.NOISE --------------------------------------------------------

void WorldgenRegistries::registerNoise(const std::string& key, std::shared_ptr<const synth::NormalNoise> noise) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_noises[normalizeKey(key)] = std::move(noise);
}

std::shared_ptr<const synth::NormalNoise> WorldgenRegistries::noiseDefinition(const std::string& key) {
    const std::string id = normalizeKey(key);
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    auto it = m_noises.find(id);
    if (it != m_noises.end()) return it->second;
    std::shared_ptr<const synth::NormalNoise> noise;
    try {
        noise = synth::NormalNoise::fromJson(readEntry("noise", id));
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("worldgen/noise ") + id + ": " + e.what());
    }
    m_noises.emplace(id, noise);
    return noise;
}

NoiseHolder WorldgenRegistries::noise(const std::string& key) {
    const std::string id = normalizeKey(key);
    return NoiseHolder{id, noiseDefinition(id)};
}

// NormalNoise.CODEC: RegistryCodecs.holder(NOISE, DIRECT_CODEC).
NoiseHolder WorldgenRegistries::parseNoiseHolder(const json& value) {
    if (value.is_string()) return noise(value.get<std::string>());
    if (value.is_object()) return NoiseHolder{std::string(), synth::NormalNoise::fromJson(value)};
    fail("Not a noise: " + value.dump().substr(0, 200));
}

// ---- Registries.DENSITY_FUNCTION --------------------------------------------

void WorldgenRegistries::registerDensityFunction(const std::string& key, DensityFunctionPtr function) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_functions[normalizeKey(key)] = std::move(function);
}

DensityFunctionPtr WorldgenRegistries::densityFunctionValue(const std::string& key) {
    const std::string id = normalizeKey(key);
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    auto it = m_functions.find(id);
    if (it != m_functions.end()) return it->second;
    if (m_decoding[id]) throw std::runtime_error("worldgen/density_function: " + id + " references itself");
    m_decoding[id] = true;
    DensityFunctionPtr function;
    try {
        // The registry file holds DensityFunctions.DIRECT_CODEC.
        function = parseDensityFunction(readEntry("density_function", id));
    } catch (const std::exception& e) {
        m_decoding.erase(id);
        throw std::runtime_error(std::string("worldgen/density_function ") + id + ": " + e.what());
    }
    m_decoding.erase(id);
    m_functions.emplace(id, function);
    return function;
}

DensityFunctionPtr WorldgenRegistries::densityFunction(const std::string& key) {
    const std::string id = normalizeKey(key);
    return std::make_shared<ReferenceFunction>(id, densityFunctionValue(id));
}

// DensityFunction.CODEC: a registry name, a number (ConstantFunction) or a
// typed object.
DensityFunctionPtr WorldgenRegistries::parseDensityFunction(const json& value) {
    if (value.is_string()) return densityFunction(value.get<std::string>());
    if (value.is_number()) return std::make_shared<ConstantFunction>(noiseValue(value, "constant"));
    if (value.is_object()) return decodeTyped(value);
    fail("Not a density function: " + value.dump().substr(0, 200));
}

DensityFunctionPtr WorldgenRegistries::decodeTyped(const json& object) {
    const std::string type = normalizeKey(field(object, "type").get<std::string>());
    if (type.rfind("minecraft:", 0) != 0) fail("Unknown density function type " + type);
    const std::string id = type.substr(10);
    auto df = [&](const char* name) { return parseDensityFunction(field(object, name)); };
    auto optionalDf = [&](const char* name, DensityFunctionPtr fallback) {
        auto it = object.find(name);
        return it == object.end() ? fallback : parseDensityFunction(*it);
    };

    if (id == "constant") return std::make_shared<ConstantFunction>(noiseValue(field(object, "value"), "value"));
    if (id == "blend_alpha") return SimpleDensityFunction::blendAlpha();
    if (id == "blend_offset") return SimpleDensityFunction::blendOffset();
    if (id == "beardifier") return SimpleDensityFunction::beardifier();
    if (id == "noise") {
        NoiseHolder noise = parseNoiseHolder(field(object, "noise"));
        const double xzScale = doubleField(object, "xz_scale");
        const double yScale = doubleField(object, "y_scale");
        DensityFunctionPtr shiftX = optionalDf("shift_x", zero());
        DensityFunctionPtr shiftY = optionalDf("shift_y", zero());
        DensityFunctionPtr shiftZ = optionalDf("shift_z", zero());
        return std::make_shared<NoiseFunction>(std::move(noise), xzScale, yScale, shiftX, shiftY, shiftZ);
    }
    if (id == "end_outer_islands") return std::make_shared<EndIslandFunction>();
    if (id == "distance_to_point") {
        const json& point = field(object, "point");
        if (!point.is_array() || point.size() != 3) fail("distance_to_point: point must be 3 integers");
        return std::make_shared<DistanceToPointFunction>(
            core::Vec3i(point[0].get<int>(), point[1].get<int>(), point[2].get<int>()),
            metricValue(field(object, "metric")));
    }
    if (id == "gradient") {
        const Axis axis = axisValue(field(object, "axis"));
        auto tilingIt = object.find("tiling");
        const TilingMode tiling = tilingIt == object.end() ? TilingMode::CLAMP_TO_EDGE : tilingValue(*tilingIt);
        return std::make_shared<GradientFunction>(axis, tiling, intField(object, "from_coordinate"),
                                                  intField(object, "to_coordinate"),
                                                  noiseValue(field(object, "from_value"), "from_value"),
                                                  noiseValue(field(object, "to_value"), "to_value"));
    }
    if (id == "shift_a") return std::make_shared<ShiftNoiseFunction::ShiftA>(parseNoiseHolder(field(object, "noise")));
    if (id == "shift_b") return std::make_shared<ShiftNoiseFunction::ShiftB>(parseNoiseHolder(field(object, "noise")));
    if (id == "shift") return std::make_shared<ShiftNoiseFunction::Shift>(parseNoiseHolder(field(object, "noise")));
    {
        UnaryFunction::Type unary;
        if (unaryType(id, unary)) return std::make_shared<UnaryFunction>(unary, df("input"));
        RoundFunction::Type round;
        if (roundType(id, round)) {
            DensityFunctionPtr input = df("input");
            DensityFunctionPtr multiple = optionalDf("multiple", constant(1.0f));
            return std::make_shared<RoundFunction>(round, input, multiple);
        }
        BinaryFunction::Type binary;
        if (binaryType(id, binary)) {
            DensityFunctionPtr left = df("left");
            DensityFunctionPtr right = df("right");
            return std::make_shared<BinaryFunction>(binary, left, right);
        }
    }
    if (id == "pow") {
        DensityFunctionPtr base = df("base");
        DensityFunctionPtr exponent = df("exponent");
        return std::make_shared<PowFunction>(base, exponent);
    }
    if (id == "spline") return std::make_shared<SplineFunction>(parseSpline(field(object, "spline")));
    if (id == "lerp") {
        DensityFunctionPtr alpha = df("alpha");
        DensityFunctionPtr first = df("first");
        DensityFunctionPtr second = df("second");
        return std::make_shared<LerpFunction>(alpha, first, second);
    }
    if (id == "clamp") {
        DensityFunctionPtr input = df("input");
        const float min = noiseValue(field(object, "min"), "min");
        const float max = noiseValue(field(object, "max"), "max");
        if (max < min) fail("clamp: max must be >= min");
        return std::make_shared<ClampFunction>(input, min, max);
    }
    if (id == "range_choice") {
        DensityFunctionPtr input = df("input");
        const float minInclusive = noiseValue(field(object, "min_inclusive"), "min_inclusive");
        const float maxExclusive = noiseValue(field(object, "max_exclusive"), "max_exclusive");
        DensityFunctionPtr whenInRange = df("when_in_range");
        DensityFunctionPtr whenOutOfRange = df("when_out_of_range");
        return std::make_shared<RangeChoiceFunction>(input, minInclusive, maxExclusive, whenInRange, whenOutOfRange);
    }
    if (id == "interval_select") {
        DensityFunctionPtr input = df("input");
        std::vector<float> thresholds;
        for (const json& t : field(object, "thresholds")) thresholds.push_back(noiseValue(t, "thresholds"));
        std::vector<DensityFunctionPtr> functions;
        for (const json& f : field(object, "functions")) functions.push_back(parseDensityFunction(f));
        if (functions.size() < 2) fail("interval_select: at least 2 functions");
        // IntervalSelectFunction.validate.
        if (thresholds.size() != functions.size() - 1) {
            fail("Expected " + std::to_string(functions.size() - 1) + " thresholds for " +
                 std::to_string(functions.size()) + " functions, but got " + std::to_string(thresholds.size()));
        }
        for (size_t i = 1; i < thresholds.size(); ++i) {
            if (thresholds[i] < thresholds[i - 1]) fail("Threshold values must be ordered from smallest to largest");
        }
        return std::make_shared<IntervalSelectFunction>(input, std::move(thresholds), std::move(functions));
    }
    if (id == "cache") return std::make_shared<CacheFunction>(df("input"));
    if (id == "blend_density") return std::make_shared<BlendDensityFunction>(df("input"));
    if (id == "interpolated") {
        DensityFunctionPtr input = df("input");
        return std::make_shared<InterpolatedFunction>(input, positiveInt(object, "cell_size_xz"),
                                                      positiveInt(object, "cell_size_y"));
    }
    if (id == "slice") {
        const Axis axis = axisValue(field(object, "axis"));
        const int coordinate = intField(object, "coordinate");
        return std::make_shared<SliceFunction>(axis, coordinate, df("input"));
    }
    if (id == "find_top_surface") {
        DensityFunctionPtr density = df("density");
        DensityFunctionPtr upperBound = df("upper_bound");
        const int lowerBound = intField(object, "lower_bound");
        if (lowerBound < MIN_Y * 2 || lowerBound > MAX_Y * 2) fail("find_top_surface: lower_bound out of range");
        return std::make_shared<FindTopSurfaceFunction>(density, upperBound, lowerBound,
                                                        positiveInt(object, "cell_height"));
    }
    if (id == "old_blended_noise") {
        const double smear = doubleField(object, "smear_scale_multiplier");
        if (!(smear >= 1.0 && smear <= 8.0)) fail("old_blended_noise: smear_scale_multiplier outside [1.0:8.0]");
        return std::make_shared<synth::BlendedNoise>(scaleRange(object, "xz_scale"), scaleRange(object, "y_scale"),
                                                     scaleRange(object, "xz_factor"), scaleRange(object, "y_factor"),
                                                     smear);
    }
    fail("Unknown density function type " + type);
}

// CubicSpline.codec(SplineFunction.Coordinate.CODEC): a float is a constant
// spline, an object a multipoint one (Multipoint.createFromPoints).
std::shared_ptr<const CubicSpline> WorldgenRegistries::parseSpline(const json& value) {
    if (value.is_number()) return std::make_shared<CubicSpline::Constant>(floatValue(value, "spline"));
    if (!value.is_object()) fail("Not a spline: " + value.dump().substr(0, 200));
    auto coordinate = std::make_shared<SplineFunction::Coordinate>(parseDensityFunction(field(value, "coordinate")));
    const json& points = field(value, "points");
    if (!points.is_array() || points.empty()) fail("spline: points must be a non-empty list");
    std::vector<float> locations;
    std::vector<std::shared_ptr<const CubicSpline>> values;
    std::vector<float> derivatives;
    locations.reserve(points.size());
    values.reserve(points.size());
    derivatives.reserve(points.size());
    for (const json& point : points) {
        locations.push_back(floatValue(field(point, "location"), "location"));
        values.push_back(parseSpline(field(point, "value")));
        derivatives.push_back(floatValue(field(point, "derivative"), "derivative"));
    }
    return std::make_shared<CubicSpline::Multipoint>(coordinate, std::move(locations), std::move(values),
                                                     std::move(derivatives));
}

} // namespace density
} // namespace levelgen
} // namespace minecraft
