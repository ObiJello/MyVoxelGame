#include "world/biome/OverworldBiomeBuilder.h"

namespace minecraft {
namespace world {
namespace biome {

// Reference: OverworldBiomeBuilder.java lines 59-76
OverworldBiomeBuilder::OverworldBiomeBuilder()
    : m_fullRange(Climate::Parameter::span(-1.0F, 1.0F))
    // Temperature ranges
    , m_temperatures{
        Climate::Parameter::span(-1.0F, -0.45F),
        Climate::Parameter::span(-0.45F, -0.15F),
        Climate::Parameter::span(-0.15F, 0.2F),
        Climate::Parameter::span(0.2F, 0.55F),
        Climate::Parameter::span(0.55F, 1.0F)
    }
    // Humidity ranges
    , m_humidities{
        Climate::Parameter::span(-1.0F, -0.35F),
        Climate::Parameter::span(-0.35F, -0.1F),
        Climate::Parameter::span(-0.1F, 0.1F),
        Climate::Parameter::span(0.1F, 0.3F),
        Climate::Parameter::span(0.3F, 1.0F)
    }
    // Erosion ranges
    , m_erosions{
        Climate::Parameter::span(-1.0F, -0.78F),
        Climate::Parameter::span(-0.78F, -0.375F),
        Climate::Parameter::span(-0.375F, -0.2225F),
        Climate::Parameter::span(-0.2225F, 0.05F),
        Climate::Parameter::span(0.05F, 0.45F),
        Climate::Parameter::span(0.45F, 0.55F),
        Climate::Parameter::span(0.55F, 1.0F)
    }
    // Temperature-based ranges
    , m_frozenRange(m_temperatures[0])
    , m_unfrozenRange(Climate::Parameter::span(m_temperatures[1], m_temperatures[4]))
    // Continentalness ranges
    , m_mushroomFieldsContinentalness(Climate::Parameter::span(-1.2F, -1.05F))
    , m_deepOceanContinentalness(Climate::Parameter::span(-1.05F, -0.455F))
    , m_oceanContinentalness(Climate::Parameter::span(-0.455F, -0.19F))
    , m_coastContinentalness(Climate::Parameter::span(-0.19F, -0.11F))
    , m_inlandContinentalness(Climate::Parameter::span(-0.11F, 0.55F))
    , m_nearInlandContinentalness(Climate::Parameter::span(-0.11F, 0.03F))
    , m_midInlandContinentalness(Climate::Parameter::span(0.03F, 0.3F))
    , m_farInlandContinentalness(Climate::Parameter::span(0.3F, 1.0F))
{
    // Initialize ocean biomes - Reference: OverworldBiomeBuilder.java line 70
    // Deep oceans [0], regular oceans [1]
    m_oceans[0][0] = BiomeKeys::DEEP_FROZEN_OCEAN;
    m_oceans[0][1] = BiomeKeys::DEEP_COLD_OCEAN;
    m_oceans[0][2] = BiomeKeys::DEEP_OCEAN;
    m_oceans[0][3] = BiomeKeys::DEEP_LUKEWARM_OCEAN;
    m_oceans[0][4] = BiomeKeys::WARM_OCEAN;

    m_oceans[1][0] = BiomeKeys::FROZEN_OCEAN;
    m_oceans[1][1] = BiomeKeys::COLD_OCEAN;
    m_oceans[1][2] = BiomeKeys::OCEAN;
    m_oceans[1][3] = BiomeKeys::LUKEWARM_OCEAN;
    m_oceans[1][4] = BiomeKeys::WARM_OCEAN;

    // Middle biomes - Reference: OverworldBiomeBuilder.java line 71
    // [temperature][humidity]
    m_middleBiomes[0][0] = BiomeKeys::SNOWY_PLAINS; m_middleBiomes[0][1] = BiomeKeys::SNOWY_PLAINS; m_middleBiomes[0][2] = BiomeKeys::SNOWY_PLAINS; m_middleBiomes[0][3] = BiomeKeys::SNOWY_TAIGA; m_middleBiomes[0][4] = BiomeKeys::TAIGA;
    m_middleBiomes[1][0] = BiomeKeys::PLAINS; m_middleBiomes[1][1] = BiomeKeys::PLAINS; m_middleBiomes[1][2] = BiomeKeys::FOREST; m_middleBiomes[1][3] = BiomeKeys::TAIGA; m_middleBiomes[1][4] = BiomeKeys::OLD_GROWTH_SPRUCE_TAIGA;
    m_middleBiomes[2][0] = BiomeKeys::FLOWER_FOREST; m_middleBiomes[2][1] = BiomeKeys::PLAINS; m_middleBiomes[2][2] = BiomeKeys::FOREST; m_middleBiomes[2][3] = BiomeKeys::BIRCH_FOREST; m_middleBiomes[2][4] = BiomeKeys::DARK_FOREST;
    m_middleBiomes[3][0] = BiomeKeys::SAVANNA; m_middleBiomes[3][1] = BiomeKeys::SAVANNA; m_middleBiomes[3][2] = BiomeKeys::FOREST; m_middleBiomes[3][3] = BiomeKeys::JUNGLE; m_middleBiomes[3][4] = BiomeKeys::JUNGLE;
    m_middleBiomes[4][0] = BiomeKeys::DESERT; m_middleBiomes[4][1] = BiomeKeys::DESERT; m_middleBiomes[4][2] = BiomeKeys::DESERT; m_middleBiomes[4][3] = BiomeKeys::DESERT; m_middleBiomes[4][4] = BiomeKeys::DESERT;

    // Middle biomes variant - Reference: OverworldBiomeBuilder.java line 72
    m_middleBiomesVariant[0][0] = BiomeKeys::ICE_SPIKES; m_middleBiomesVariant[0][1] = ""; m_middleBiomesVariant[0][2] = BiomeKeys::SNOWY_TAIGA; m_middleBiomesVariant[0][3] = ""; m_middleBiomesVariant[0][4] = "";
    m_middleBiomesVariant[1][0] = ""; m_middleBiomesVariant[1][1] = ""; m_middleBiomesVariant[1][2] = ""; m_middleBiomesVariant[1][3] = ""; m_middleBiomesVariant[1][4] = BiomeKeys::OLD_GROWTH_PINE_TAIGA;
    m_middleBiomesVariant[2][0] = BiomeKeys::SUNFLOWER_PLAINS; m_middleBiomesVariant[2][1] = ""; m_middleBiomesVariant[2][2] = ""; m_middleBiomesVariant[2][3] = BiomeKeys::OLD_GROWTH_BIRCH_FOREST; m_middleBiomesVariant[2][4] = "";
    m_middleBiomesVariant[3][0] = ""; m_middleBiomesVariant[3][1] = ""; m_middleBiomesVariant[3][2] = BiomeKeys::PLAINS; m_middleBiomesVariant[3][3] = BiomeKeys::SPARSE_JUNGLE; m_middleBiomesVariant[3][4] = BiomeKeys::BAMBOO_JUNGLE;
    m_middleBiomesVariant[4][0] = ""; m_middleBiomesVariant[4][1] = ""; m_middleBiomesVariant[4][2] = ""; m_middleBiomesVariant[4][3] = ""; m_middleBiomesVariant[4][4] = "";

    // Plateau biomes - Reference: OverworldBiomeBuilder.java line 73
    m_plateauBiomes[0][0] = BiomeKeys::SNOWY_PLAINS; m_plateauBiomes[0][1] = BiomeKeys::SNOWY_PLAINS; m_plateauBiomes[0][2] = BiomeKeys::SNOWY_PLAINS; m_plateauBiomes[0][3] = BiomeKeys::SNOWY_TAIGA; m_plateauBiomes[0][4] = BiomeKeys::SNOWY_TAIGA;
    m_plateauBiomes[1][0] = BiomeKeys::MEADOW; m_plateauBiomes[1][1] = BiomeKeys::MEADOW; m_plateauBiomes[1][2] = BiomeKeys::FOREST; m_plateauBiomes[1][3] = BiomeKeys::TAIGA; m_plateauBiomes[1][4] = BiomeKeys::OLD_GROWTH_SPRUCE_TAIGA;
    m_plateauBiomes[2][0] = BiomeKeys::MEADOW; m_plateauBiomes[2][1] = BiomeKeys::MEADOW; m_plateauBiomes[2][2] = BiomeKeys::MEADOW; m_plateauBiomes[2][3] = BiomeKeys::MEADOW; m_plateauBiomes[2][4] = BiomeKeys::PALE_GARDEN;
    m_plateauBiomes[3][0] = BiomeKeys::SAVANNA_PLATEAU; m_plateauBiomes[3][1] = BiomeKeys::SAVANNA_PLATEAU; m_plateauBiomes[3][2] = BiomeKeys::FOREST; m_plateauBiomes[3][3] = BiomeKeys::FOREST; m_plateauBiomes[3][4] = BiomeKeys::JUNGLE;
    m_plateauBiomes[4][0] = BiomeKeys::BADLANDS; m_plateauBiomes[4][1] = BiomeKeys::BADLANDS; m_plateauBiomes[4][2] = BiomeKeys::BADLANDS; m_plateauBiomes[4][3] = BiomeKeys::WOODED_BADLANDS; m_plateauBiomes[4][4] = BiomeKeys::WOODED_BADLANDS;

    // Plateau biomes variant - Reference: OverworldBiomeBuilder.java line 74
    m_plateauBiomesVariant[0][0] = BiomeKeys::ICE_SPIKES; m_plateauBiomesVariant[0][1] = ""; m_plateauBiomesVariant[0][2] = ""; m_plateauBiomesVariant[0][3] = ""; m_plateauBiomesVariant[0][4] = "";
    m_plateauBiomesVariant[1][0] = BiomeKeys::CHERRY_GROVE; m_plateauBiomesVariant[1][1] = ""; m_plateauBiomesVariant[1][2] = BiomeKeys::MEADOW; m_plateauBiomesVariant[1][3] = BiomeKeys::MEADOW; m_plateauBiomesVariant[1][4] = BiomeKeys::OLD_GROWTH_PINE_TAIGA;
    m_plateauBiomesVariant[2][0] = BiomeKeys::CHERRY_GROVE; m_plateauBiomesVariant[2][1] = BiomeKeys::CHERRY_GROVE; m_plateauBiomesVariant[2][2] = BiomeKeys::FOREST; m_plateauBiomesVariant[2][3] = BiomeKeys::BIRCH_FOREST; m_plateauBiomesVariant[2][4] = "";
    m_plateauBiomesVariant[3][0] = ""; m_plateauBiomesVariant[3][1] = ""; m_plateauBiomesVariant[3][2] = ""; m_plateauBiomesVariant[3][3] = ""; m_plateauBiomesVariant[3][4] = "";
    m_plateauBiomesVariant[4][0] = BiomeKeys::ERODED_BADLANDS; m_plateauBiomesVariant[4][1] = BiomeKeys::ERODED_BADLANDS; m_plateauBiomesVariant[4][2] = ""; m_plateauBiomesVariant[4][3] = ""; m_plateauBiomesVariant[4][4] = "";

    // Shattered biomes - Reference: OverworldBiomeBuilder.java line 75
    m_shatteredBiomes[0][0] = BiomeKeys::WINDSWEPT_GRAVELLY_HILLS; m_shatteredBiomes[0][1] = BiomeKeys::WINDSWEPT_GRAVELLY_HILLS; m_shatteredBiomes[0][2] = BiomeKeys::WINDSWEPT_HILLS; m_shatteredBiomes[0][3] = BiomeKeys::WINDSWEPT_FOREST; m_shatteredBiomes[0][4] = BiomeKeys::WINDSWEPT_FOREST;
    m_shatteredBiomes[1][0] = BiomeKeys::WINDSWEPT_GRAVELLY_HILLS; m_shatteredBiomes[1][1] = BiomeKeys::WINDSWEPT_GRAVELLY_HILLS; m_shatteredBiomes[1][2] = BiomeKeys::WINDSWEPT_HILLS; m_shatteredBiomes[1][3] = BiomeKeys::WINDSWEPT_FOREST; m_shatteredBiomes[1][4] = BiomeKeys::WINDSWEPT_FOREST;
    m_shatteredBiomes[2][0] = BiomeKeys::WINDSWEPT_HILLS; m_shatteredBiomes[2][1] = BiomeKeys::WINDSWEPT_HILLS; m_shatteredBiomes[2][2] = BiomeKeys::WINDSWEPT_HILLS; m_shatteredBiomes[2][3] = BiomeKeys::WINDSWEPT_FOREST; m_shatteredBiomes[2][4] = BiomeKeys::WINDSWEPT_FOREST;
    m_shatteredBiomes[3][0] = ""; m_shatteredBiomes[3][1] = ""; m_shatteredBiomes[3][2] = ""; m_shatteredBiomes[3][3] = ""; m_shatteredBiomes[3][4] = "";
    m_shatteredBiomes[4][0] = ""; m_shatteredBiomes[4][1] = ""; m_shatteredBiomes[4][2] = ""; m_shatteredBiomes[4][3] = ""; m_shatteredBiomes[4][4] = "";
}

// Reference: OverworldBiomeBuilder.java lines 78-82
std::vector<Climate::ParameterPoint> OverworldBiomeBuilder::spawnTarget() const {
    Climate::Parameter surfaceDepth = Climate::Parameter::point(0.0F);
    return {
        Climate::ParameterPoint(
            m_fullRange, m_fullRange,
            Climate::Parameter::span(m_inlandContinentalness, m_fullRange),
            m_fullRange, surfaceDepth,
            Climate::Parameter::span(-1.0F, -0.16F), 0),
        Climate::ParameterPoint(
            m_fullRange, m_fullRange,
            Climate::Parameter::span(m_inlandContinentalness, m_fullRange),
            m_fullRange, surfaceDepth,
            Climate::Parameter::span(0.16F, 1.0F), 0)
    };
}

// Reference: OverworldBiomeBuilder.java lines 84-92
void OverworldBiomeBuilder::addBiomes(std::function<void(const std::pair<Climate::ParameterPoint, BiomeKey>&)> consumer) const {
    addOffCoastBiomes(consumer);
    addInlandBiomes(consumer);
    addUndergroundBiomes(consumer);
}

// Reference: OverworldBiomeBuilder.java lines 411-413
bool OverworldBiomeBuilder::isDeepDarkRegion(
    const density::DensityFunction* erosion,
    const density::DensityFunction* depth,
    const density::DensityFunction::FunctionContext& context
) {
    return erosion->compute(context) < static_cast<double>(EROSION_DEEP_DARK_DRYNESS_THRESHOLD) &&
           depth->compute(context) > static_cast<double>(DEPTH_DEEP_DARK_DRYNESS_THRESHOLD);
}

// Reference: OverworldBiomeBuilder.java lines 120-129
void OverworldBiomeBuilder::addOffCoastBiomes(std::function<void(const std::pair<Climate::ParameterPoint, BiomeKey>&)> consumer) const {
    addSurfaceBiome(consumer, m_fullRange, m_fullRange, m_mushroomFieldsContinentalness, m_fullRange, m_fullRange, 0.0F, BiomeKeys::MUSHROOM_FIELDS);

    for (int temperatureIndex = 0; temperatureIndex < 5; ++temperatureIndex) {
        const Climate::Parameter& temperature = m_temperatures[temperatureIndex];
        addSurfaceBiome(consumer, temperature, m_fullRange, m_deepOceanContinentalness, m_fullRange, m_fullRange, 0.0F, m_oceans[0][temperatureIndex]);
        addSurfaceBiome(consumer, temperature, m_fullRange, m_oceanContinentalness, m_fullRange, m_fullRange, 0.0F, m_oceans[1][temperatureIndex]);
    }
}

// Reference: OverworldBiomeBuilder.java lines 131-145
void OverworldBiomeBuilder::addInlandBiomes(std::function<void(const std::pair<Climate::ParameterPoint, BiomeKey>&)> consumer) const {
    addMidSlice(consumer, Climate::Parameter::span(-1.0F, -0.93333334F));
    addHighSlice(consumer, Climate::Parameter::span(-0.93333334F, -0.7666667F));
    addPeaks(consumer, Climate::Parameter::span(-0.7666667F, -0.56666666F));
    addHighSlice(consumer, Climate::Parameter::span(-0.56666666F, -0.4F));
    addMidSlice(consumer, Climate::Parameter::span(-0.4F, -0.26666668F));
    addLowSlice(consumer, Climate::Parameter::span(-0.26666668F, -0.05F));
    addValleys(consumer, Climate::Parameter::span(-0.05F, 0.05F));
    addLowSlice(consumer, Climate::Parameter::span(0.05F, 0.26666668F));
    addMidSlice(consumer, Climate::Parameter::span(0.26666668F, 0.4F));
    addHighSlice(consumer, Climate::Parameter::span(0.4F, 0.56666666F));
    addPeaks(consumer, Climate::Parameter::span(0.56666666F, 0.7666667F));
    addHighSlice(consumer, Climate::Parameter::span(0.7666667F, 0.93333334F));
    addMidSlice(consumer, Climate::Parameter::span(0.93333334F, 1.0F));
}

// Reference: OverworldBiomeBuilder.java lines 147-174
void OverworldBiomeBuilder::addPeaks(std::function<void(const std::pair<Climate::ParameterPoint, BiomeKey>&)> consumer, const Climate::Parameter& weirdness) const {
    for (int temperatureIndex = 0; temperatureIndex < 5; ++temperatureIndex) {
        const Climate::Parameter& temperature = m_temperatures[temperatureIndex];

        for (int humidityIndex = 0; humidityIndex < 5; ++humidityIndex) {
            const Climate::Parameter& humidity = m_humidities[humidityIndex];

            BiomeKey middleBiome = pickMiddleBiome(temperatureIndex, humidityIndex, weirdness);
            BiomeKey middleBiomeOrBadlandsIfHot = pickMiddleBiomeOrBadlandsIfHot(temperatureIndex, humidityIndex, weirdness);
            BiomeKey middleBiomeOrBadlandsIfHotOrSlopeIfCold = pickMiddleBiomeOrBadlandsIfHotOrSlopeIfCold(temperatureIndex, humidityIndex, weirdness);
            BiomeKey plateauBiome = pickPlateauBiome(temperatureIndex, humidityIndex, weirdness);
            BiomeKey shatteredBiome = pickShatteredBiome(temperatureIndex, humidityIndex, weirdness);
            BiomeKey shatteredBiomeOrWindsweptSavanna = maybePickWindsweptSavannaBiome(temperatureIndex, humidityIndex, weirdness, shatteredBiome);
            BiomeKey peakBiome = pickPeakBiome(temperatureIndex, humidityIndex, weirdness);

            addSurfaceBiome(consumer, temperature, humidity, Climate::Parameter::span(m_coastContinentalness, m_farInlandContinentalness), m_erosions[0], weirdness, 0.0F, peakBiome);
            addSurfaceBiome(consumer, temperature, humidity, Climate::Parameter::span(m_coastContinentalness, m_nearInlandContinentalness), m_erosions[1], weirdness, 0.0F, middleBiomeOrBadlandsIfHotOrSlopeIfCold);
            addSurfaceBiome(consumer, temperature, humidity, Climate::Parameter::span(m_midInlandContinentalness, m_farInlandContinentalness), m_erosions[1], weirdness, 0.0F, peakBiome);
            addSurfaceBiome(consumer, temperature, humidity, Climate::Parameter::span(m_coastContinentalness, m_nearInlandContinentalness), Climate::Parameter::span(m_erosions[2], m_erosions[3]), weirdness, 0.0F, middleBiome);
            addSurfaceBiome(consumer, temperature, humidity, Climate::Parameter::span(m_midInlandContinentalness, m_farInlandContinentalness), m_erosions[2], weirdness, 0.0F, plateauBiome);
            addSurfaceBiome(consumer, temperature, humidity, m_midInlandContinentalness, m_erosions[3], weirdness, 0.0F, middleBiomeOrBadlandsIfHot);
            addSurfaceBiome(consumer, temperature, humidity, m_farInlandContinentalness, m_erosions[3], weirdness, 0.0F, plateauBiome);
            addSurfaceBiome(consumer, temperature, humidity, Climate::Parameter::span(m_coastContinentalness, m_farInlandContinentalness), m_erosions[4], weirdness, 0.0F, middleBiome);
            addSurfaceBiome(consumer, temperature, humidity, Climate::Parameter::span(m_coastContinentalness, m_nearInlandContinentalness), m_erosions[5], weirdness, 0.0F, shatteredBiomeOrWindsweptSavanna);
            addSurfaceBiome(consumer, temperature, humidity, Climate::Parameter::span(m_midInlandContinentalness, m_farInlandContinentalness), m_erosions[5], weirdness, 0.0F, shatteredBiome);
            addSurfaceBiome(consumer, temperature, humidity, Climate::Parameter::span(m_coastContinentalness, m_farInlandContinentalness), m_erosions[6], weirdness, 0.0F, middleBiome);
        }
    }
}

// Reference: OverworldBiomeBuilder.java lines 176-206
void OverworldBiomeBuilder::addHighSlice(std::function<void(const std::pair<Climate::ParameterPoint, BiomeKey>&)> consumer, const Climate::Parameter& weirdness) const {
    for (int temperatureIndex = 0; temperatureIndex < 5; ++temperatureIndex) {
        const Climate::Parameter& temperature = m_temperatures[temperatureIndex];

        for (int humidityIndex = 0; humidityIndex < 5; ++humidityIndex) {
            const Climate::Parameter& humidity = m_humidities[humidityIndex];

            BiomeKey middleBiome = pickMiddleBiome(temperatureIndex, humidityIndex, weirdness);
            BiomeKey middleBiomeOrBadlandsIfHot = pickMiddleBiomeOrBadlandsIfHot(temperatureIndex, humidityIndex, weirdness);
            BiomeKey middleBiomeOrBadlandsIfHotOrSlopeIfCold = pickMiddleBiomeOrBadlandsIfHotOrSlopeIfCold(temperatureIndex, humidityIndex, weirdness);
            BiomeKey plateauBiome = pickPlateauBiome(temperatureIndex, humidityIndex, weirdness);
            BiomeKey shatteredBiome = pickShatteredBiome(temperatureIndex, humidityIndex, weirdness);
            BiomeKey middleBiomeOrWindsweptSavanna = maybePickWindsweptSavannaBiome(temperatureIndex, humidityIndex, weirdness, middleBiome);
            BiomeKey slopeBiome = pickSlopeBiome(temperatureIndex, humidityIndex, weirdness);
            BiomeKey peakBiome = pickPeakBiome(temperatureIndex, humidityIndex, weirdness);

            addSurfaceBiome(consumer, temperature, humidity, m_coastContinentalness, Climate::Parameter::span(m_erosions[0], m_erosions[1]), weirdness, 0.0F, middleBiome);
            addSurfaceBiome(consumer, temperature, humidity, m_nearInlandContinentalness, m_erosions[0], weirdness, 0.0F, slopeBiome);
            addSurfaceBiome(consumer, temperature, humidity, Climate::Parameter::span(m_midInlandContinentalness, m_farInlandContinentalness), m_erosions[0], weirdness, 0.0F, peakBiome);
            addSurfaceBiome(consumer, temperature, humidity, m_nearInlandContinentalness, m_erosions[1], weirdness, 0.0F, middleBiomeOrBadlandsIfHotOrSlopeIfCold);
            addSurfaceBiome(consumer, temperature, humidity, Climate::Parameter::span(m_midInlandContinentalness, m_farInlandContinentalness), m_erosions[1], weirdness, 0.0F, slopeBiome);
            addSurfaceBiome(consumer, temperature, humidity, Climate::Parameter::span(m_coastContinentalness, m_nearInlandContinentalness), Climate::Parameter::span(m_erosions[2], m_erosions[3]), weirdness, 0.0F, middleBiome);
            addSurfaceBiome(consumer, temperature, humidity, Climate::Parameter::span(m_midInlandContinentalness, m_farInlandContinentalness), m_erosions[2], weirdness, 0.0F, plateauBiome);
            addSurfaceBiome(consumer, temperature, humidity, m_midInlandContinentalness, m_erosions[3], weirdness, 0.0F, middleBiomeOrBadlandsIfHot);
            addSurfaceBiome(consumer, temperature, humidity, m_farInlandContinentalness, m_erosions[3], weirdness, 0.0F, plateauBiome);
            addSurfaceBiome(consumer, temperature, humidity, Climate::Parameter::span(m_coastContinentalness, m_farInlandContinentalness), m_erosions[4], weirdness, 0.0F, middleBiome);
            addSurfaceBiome(consumer, temperature, humidity, Climate::Parameter::span(m_coastContinentalness, m_nearInlandContinentalness), m_erosions[5], weirdness, 0.0F, middleBiomeOrWindsweptSavanna);
            addSurfaceBiome(consumer, temperature, humidity, Climate::Parameter::span(m_midInlandContinentalness, m_farInlandContinentalness), m_erosions[5], weirdness, 0.0F, shatteredBiome);
            addSurfaceBiome(consumer, temperature, humidity, Climate::Parameter::span(m_coastContinentalness, m_farInlandContinentalness), m_erosions[6], weirdness, 0.0F, middleBiome);
        }
    }
}

// Reference: OverworldBiomeBuilder.java lines 208-257
void OverworldBiomeBuilder::addMidSlice(std::function<void(const std::pair<Climate::ParameterPoint, BiomeKey>&)> consumer, const Climate::Parameter& weirdness) const {
    addSurfaceBiome(consumer, m_fullRange, m_fullRange, m_coastContinentalness, Climate::Parameter::span(m_erosions[0], m_erosions[2]), weirdness, 0.0F, BiomeKeys::STONY_SHORE);
    addSurfaceBiome(consumer, Climate::Parameter::span(m_temperatures[1], m_temperatures[2]), m_fullRange, Climate::Parameter::span(m_nearInlandContinentalness, m_farInlandContinentalness), m_erosions[6], weirdness, 0.0F, BiomeKeys::SWAMP);
    addSurfaceBiome(consumer, Climate::Parameter::span(m_temperatures[3], m_temperatures[4]), m_fullRange, Climate::Parameter::span(m_nearInlandContinentalness, m_farInlandContinentalness), m_erosions[6], weirdness, 0.0F, BiomeKeys::MANGROVE_SWAMP);

    for (int temperatureIndex = 0; temperatureIndex < 5; ++temperatureIndex) {
        const Climate::Parameter& temperature = m_temperatures[temperatureIndex];

        for (int humidityIndex = 0; humidityIndex < 5; ++humidityIndex) {
            const Climate::Parameter& humidity = m_humidities[humidityIndex];

            BiomeKey middleBiome = pickMiddleBiome(temperatureIndex, humidityIndex, weirdness);
            BiomeKey middleBiomeOrBadlandsIfHot = pickMiddleBiomeOrBadlandsIfHot(temperatureIndex, humidityIndex, weirdness);
            BiomeKey middleBiomeOrBadlandsIfHotOrSlopeIfCold = pickMiddleBiomeOrBadlandsIfHotOrSlopeIfCold(temperatureIndex, humidityIndex, weirdness);
            BiomeKey shatteredBiome = pickShatteredBiome(temperatureIndex, humidityIndex, weirdness);
            BiomeKey plateauBiome = pickPlateauBiome(temperatureIndex, humidityIndex, weirdness);
            BiomeKey beachBiome = pickBeachBiome(temperatureIndex, humidityIndex);
            BiomeKey middleBiomeOrWindsweptSavanna = maybePickWindsweptSavannaBiome(temperatureIndex, humidityIndex, weirdness, middleBiome);
            BiomeKey shatteredCoastBiome = pickShatteredCoastBiome(temperatureIndex, humidityIndex, weirdness);
            BiomeKey slopeBiome = pickSlopeBiome(temperatureIndex, humidityIndex, weirdness);

            addSurfaceBiome(consumer, temperature, humidity, Climate::Parameter::span(m_nearInlandContinentalness, m_farInlandContinentalness), m_erosions[0], weirdness, 0.0F, slopeBiome);
            addSurfaceBiome(consumer, temperature, humidity, Climate::Parameter::span(m_nearInlandContinentalness, m_midInlandContinentalness), m_erosions[1], weirdness, 0.0F, middleBiomeOrBadlandsIfHotOrSlopeIfCold);
            addSurfaceBiome(consumer, temperature, humidity, m_farInlandContinentalness, m_erosions[1], weirdness, 0.0F, temperatureIndex == 0 ? slopeBiome : plateauBiome);
            addSurfaceBiome(consumer, temperature, humidity, m_nearInlandContinentalness, m_erosions[2], weirdness, 0.0F, middleBiome);
            addSurfaceBiome(consumer, temperature, humidity, m_midInlandContinentalness, m_erosions[2], weirdness, 0.0F, middleBiomeOrBadlandsIfHot);
            addSurfaceBiome(consumer, temperature, humidity, m_farInlandContinentalness, m_erosions[2], weirdness, 0.0F, plateauBiome);
            addSurfaceBiome(consumer, temperature, humidity, Climate::Parameter::span(m_coastContinentalness, m_nearInlandContinentalness), m_erosions[3], weirdness, 0.0F, middleBiome);
            addSurfaceBiome(consumer, temperature, humidity, Climate::Parameter::span(m_midInlandContinentalness, m_farInlandContinentalness), m_erosions[3], weirdness, 0.0F, middleBiomeOrBadlandsIfHot);

            if (weirdness.max() < 0) {
                addSurfaceBiome(consumer, temperature, humidity, m_coastContinentalness, m_erosions[4], weirdness, 0.0F, beachBiome);
                addSurfaceBiome(consumer, temperature, humidity, Climate::Parameter::span(m_nearInlandContinentalness, m_farInlandContinentalness), m_erosions[4], weirdness, 0.0F, middleBiome);
            } else {
                addSurfaceBiome(consumer, temperature, humidity, Climate::Parameter::span(m_coastContinentalness, m_farInlandContinentalness), m_erosions[4], weirdness, 0.0F, middleBiome);
            }

            addSurfaceBiome(consumer, temperature, humidity, m_coastContinentalness, m_erosions[5], weirdness, 0.0F, shatteredCoastBiome);
            addSurfaceBiome(consumer, temperature, humidity, m_nearInlandContinentalness, m_erosions[5], weirdness, 0.0F, middleBiomeOrWindsweptSavanna);
            addSurfaceBiome(consumer, temperature, humidity, Climate::Parameter::span(m_midInlandContinentalness, m_farInlandContinentalness), m_erosions[5], weirdness, 0.0F, shatteredBiome);

            if (weirdness.max() < 0) {
                addSurfaceBiome(consumer, temperature, humidity, m_coastContinentalness, m_erosions[6], weirdness, 0.0F, beachBiome);
            } else {
                addSurfaceBiome(consumer, temperature, humidity, m_coastContinentalness, m_erosions[6], weirdness, 0.0F, middleBiome);
            }

            if (temperatureIndex == 0) {
                addSurfaceBiome(consumer, temperature, humidity, Climate::Parameter::span(m_nearInlandContinentalness, m_farInlandContinentalness), m_erosions[6], weirdness, 0.0F, middleBiome);
            }
        }
    }
}

// Reference: OverworldBiomeBuilder.java lines 259-291
void OverworldBiomeBuilder::addLowSlice(std::function<void(const std::pair<Climate::ParameterPoint, BiomeKey>&)> consumer, const Climate::Parameter& weirdness) const {
    addSurfaceBiome(consumer, m_fullRange, m_fullRange, m_coastContinentalness, Climate::Parameter::span(m_erosions[0], m_erosions[2]), weirdness, 0.0F, BiomeKeys::STONY_SHORE);
    addSurfaceBiome(consumer, Climate::Parameter::span(m_temperatures[1], m_temperatures[2]), m_fullRange, Climate::Parameter::span(m_nearInlandContinentalness, m_farInlandContinentalness), m_erosions[6], weirdness, 0.0F, BiomeKeys::SWAMP);
    addSurfaceBiome(consumer, Climate::Parameter::span(m_temperatures[3], m_temperatures[4]), m_fullRange, Climate::Parameter::span(m_nearInlandContinentalness, m_farInlandContinentalness), m_erosions[6], weirdness, 0.0F, BiomeKeys::MANGROVE_SWAMP);

    for (int temperatureIndex = 0; temperatureIndex < 5; ++temperatureIndex) {
        const Climate::Parameter& temperature = m_temperatures[temperatureIndex];

        for (int humidityIndex = 0; humidityIndex < 5; ++humidityIndex) {
            const Climate::Parameter& humidity = m_humidities[humidityIndex];

            BiomeKey middleBiome = pickMiddleBiome(temperatureIndex, humidityIndex, weirdness);
            BiomeKey middleBiomeOrBadlandsIfHot = pickMiddleBiomeOrBadlandsIfHot(temperatureIndex, humidityIndex, weirdness);
            BiomeKey middleBiomeOrBadlandsIfHotOrSlopeIfCold = pickMiddleBiomeOrBadlandsIfHotOrSlopeIfCold(temperatureIndex, humidityIndex, weirdness);
            BiomeKey beachBiome = pickBeachBiome(temperatureIndex, humidityIndex);
            BiomeKey middleBiomeOrWindsweptSavanna = maybePickWindsweptSavannaBiome(temperatureIndex, humidityIndex, weirdness, middleBiome);
            BiomeKey shatteredCoastBiome = pickShatteredCoastBiome(temperatureIndex, humidityIndex, weirdness);

            addSurfaceBiome(consumer, temperature, humidity, m_nearInlandContinentalness, Climate::Parameter::span(m_erosions[0], m_erosions[1]), weirdness, 0.0F, middleBiomeOrBadlandsIfHot);
            addSurfaceBiome(consumer, temperature, humidity, Climate::Parameter::span(m_midInlandContinentalness, m_farInlandContinentalness), Climate::Parameter::span(m_erosions[0], m_erosions[1]), weirdness, 0.0F, middleBiomeOrBadlandsIfHotOrSlopeIfCold);
            addSurfaceBiome(consumer, temperature, humidity, m_nearInlandContinentalness, Climate::Parameter::span(m_erosions[2], m_erosions[3]), weirdness, 0.0F, middleBiome);
            addSurfaceBiome(consumer, temperature, humidity, Climate::Parameter::span(m_midInlandContinentalness, m_farInlandContinentalness), Climate::Parameter::span(m_erosions[2], m_erosions[3]), weirdness, 0.0F, middleBiomeOrBadlandsIfHot);
            addSurfaceBiome(consumer, temperature, humidity, m_coastContinentalness, Climate::Parameter::span(m_erosions[3], m_erosions[4]), weirdness, 0.0F, beachBiome);
            addSurfaceBiome(consumer, temperature, humidity, Climate::Parameter::span(m_nearInlandContinentalness, m_farInlandContinentalness), m_erosions[4], weirdness, 0.0F, middleBiome);
            addSurfaceBiome(consumer, temperature, humidity, m_coastContinentalness, m_erosions[5], weirdness, 0.0F, shatteredCoastBiome);
            addSurfaceBiome(consumer, temperature, humidity, m_nearInlandContinentalness, m_erosions[5], weirdness, 0.0F, middleBiomeOrWindsweptSavanna);
            addSurfaceBiome(consumer, temperature, humidity, Climate::Parameter::span(m_midInlandContinentalness, m_farInlandContinentalness), m_erosions[5], weirdness, 0.0F, middleBiome);
            addSurfaceBiome(consumer, temperature, humidity, m_coastContinentalness, m_erosions[6], weirdness, 0.0F, beachBiome);

            if (temperatureIndex == 0) {
                addSurfaceBiome(consumer, temperature, humidity, Climate::Parameter::span(m_nearInlandContinentalness, m_farInlandContinentalness), m_erosions[6], weirdness, 0.0F, middleBiome);
            }
        }
    }
}

// Reference: OverworldBiomeBuilder.java lines 293-316
void OverworldBiomeBuilder::addValleys(std::function<void(const std::pair<Climate::ParameterPoint, BiomeKey>&)> consumer, const Climate::Parameter& weirdness) const {
    addSurfaceBiome(consumer, m_frozenRange, m_fullRange, m_coastContinentalness, Climate::Parameter::span(m_erosions[0], m_erosions[1]), weirdness, 0.0F, weirdness.max() < 0 ? BiomeKeys::STONY_SHORE : BiomeKeys::FROZEN_RIVER);
    addSurfaceBiome(consumer, m_unfrozenRange, m_fullRange, m_coastContinentalness, Climate::Parameter::span(m_erosions[0], m_erosions[1]), weirdness, 0.0F, weirdness.max() < 0 ? BiomeKeys::STONY_SHORE : BiomeKeys::RIVER);
    addSurfaceBiome(consumer, m_frozenRange, m_fullRange, m_nearInlandContinentalness, Climate::Parameter::span(m_erosions[0], m_erosions[1]), weirdness, 0.0F, BiomeKeys::FROZEN_RIVER);
    addSurfaceBiome(consumer, m_unfrozenRange, m_fullRange, m_nearInlandContinentalness, Climate::Parameter::span(m_erosions[0], m_erosions[1]), weirdness, 0.0F, BiomeKeys::RIVER);
    addSurfaceBiome(consumer, m_frozenRange, m_fullRange, Climate::Parameter::span(m_coastContinentalness, m_farInlandContinentalness), Climate::Parameter::span(m_erosions[2], m_erosions[5]), weirdness, 0.0F, BiomeKeys::FROZEN_RIVER);
    addSurfaceBiome(consumer, m_unfrozenRange, m_fullRange, Climate::Parameter::span(m_coastContinentalness, m_farInlandContinentalness), Climate::Parameter::span(m_erosions[2], m_erosions[5]), weirdness, 0.0F, BiomeKeys::RIVER);
    addSurfaceBiome(consumer, m_frozenRange, m_fullRange, m_coastContinentalness, m_erosions[6], weirdness, 0.0F, BiomeKeys::FROZEN_RIVER);
    addSurfaceBiome(consumer, m_unfrozenRange, m_fullRange, m_coastContinentalness, m_erosions[6], weirdness, 0.0F, BiomeKeys::RIVER);
    addSurfaceBiome(consumer, Climate::Parameter::span(m_temperatures[1], m_temperatures[2]), m_fullRange, Climate::Parameter::span(m_inlandContinentalness, m_farInlandContinentalness), m_erosions[6], weirdness, 0.0F, BiomeKeys::SWAMP);
    addSurfaceBiome(consumer, Climate::Parameter::span(m_temperatures[3], m_temperatures[4]), m_fullRange, Climate::Parameter::span(m_inlandContinentalness, m_farInlandContinentalness), m_erosions[6], weirdness, 0.0F, BiomeKeys::MANGROVE_SWAMP);
    addSurfaceBiome(consumer, m_frozenRange, m_fullRange, Climate::Parameter::span(m_inlandContinentalness, m_farInlandContinentalness), m_erosions[6], weirdness, 0.0F, BiomeKeys::FROZEN_RIVER);

    for (int temperatureIndex = 0; temperatureIndex < 5; ++temperatureIndex) {
        const Climate::Parameter& temperature = m_temperatures[temperatureIndex];

        for (int humidityIndex = 0; humidityIndex < 5; ++humidityIndex) {
            const Climate::Parameter& humidity = m_humidities[humidityIndex];
            BiomeKey middleBiomeOrBadlandsIfHot = pickMiddleBiomeOrBadlandsIfHot(temperatureIndex, humidityIndex, weirdness);
            addSurfaceBiome(consumer, temperature, humidity, Climate::Parameter::span(m_midInlandContinentalness, m_farInlandContinentalness), Climate::Parameter::span(m_erosions[0], m_erosions[1]), weirdness, 0.0F, middleBiomeOrBadlandsIfHot);
        }
    }
}

// Reference: OverworldBiomeBuilder.java lines 318-322
void OverworldBiomeBuilder::addUndergroundBiomes(std::function<void(const std::pair<Climate::ParameterPoint, BiomeKey>&)> consumer) const {
    addUndergroundBiome(consumer, m_fullRange, m_fullRange, Climate::Parameter::span(0.8F, 1.0F), m_fullRange, m_fullRange, 0.0F, BiomeKeys::DRIPSTONE_CAVES);
    addUndergroundBiome(consumer, m_fullRange, Climate::Parameter::span(0.7F, 1.0F), m_fullRange, m_fullRange, m_fullRange, 0.0F, BiomeKeys::LUSH_CAVES);
    addBottomBiome(consumer, m_fullRange, m_fullRange, m_fullRange, Climate::Parameter::span(m_erosions[0], m_erosions[1]), m_fullRange, 0.0F, BiomeKeys::DEEP_DARK);
}

// Reference: OverworldBiomeBuilder.java lines 324-331
BiomeKey OverworldBiomeBuilder::pickMiddleBiome(int temperatureIndex, int humidityIndex, const Climate::Parameter& weirdness) const {
    if (weirdness.max() < 0) {
        return m_middleBiomes[temperatureIndex][humidityIndex];
    } else {
        BiomeKey variant = m_middleBiomesVariant[temperatureIndex][humidityIndex];
        return variant.empty() ? m_middleBiomes[temperatureIndex][humidityIndex] : variant;
    }
}

// Reference: OverworldBiomeBuilder.java lines 333-335
BiomeKey OverworldBiomeBuilder::pickMiddleBiomeOrBadlandsIfHot(int temperatureIndex, int humidityIndex, const Climate::Parameter& weirdness) const {
    return temperatureIndex == 4 ? pickBadlandsBiome(humidityIndex, weirdness) : pickMiddleBiome(temperatureIndex, humidityIndex, weirdness);
}

// Reference: OverworldBiomeBuilder.java lines 337-339
BiomeKey OverworldBiomeBuilder::pickMiddleBiomeOrBadlandsIfHotOrSlopeIfCold(int temperatureIndex, int humidityIndex, const Climate::Parameter& weirdness) const {
    return temperatureIndex == 0 ? pickSlopeBiome(temperatureIndex, humidityIndex, weirdness) : pickMiddleBiomeOrBadlandsIfHot(temperatureIndex, humidityIndex, weirdness);
}

// Reference: OverworldBiomeBuilder.java lines 341-343
BiomeKey OverworldBiomeBuilder::maybePickWindsweptSavannaBiome(int temperatureIndex, int humidityIndex, const Climate::Parameter& weirdness, const BiomeKey& underlyingBiome) const {
    return (temperatureIndex > 1 && humidityIndex < 4 && weirdness.max() >= 0) ? BiomeKeys::WINDSWEPT_SAVANNA : underlyingBiome;
}

// Reference: OverworldBiomeBuilder.java lines 345-348
BiomeKey OverworldBiomeBuilder::pickShatteredCoastBiome(int temperatureIndex, int humidityIndex, const Climate::Parameter& weirdness) const {
    BiomeKey beachOrMiddleBiome = weirdness.max() >= 0 ? pickMiddleBiome(temperatureIndex, humidityIndex, weirdness) : pickBeachBiome(temperatureIndex, humidityIndex);
    return maybePickWindsweptSavannaBiome(temperatureIndex, humidityIndex, weirdness, beachOrMiddleBiome);
}

// Reference: OverworldBiomeBuilder.java lines 350-356
BiomeKey OverworldBiomeBuilder::pickBeachBiome(int temperatureIndex, int humidityIndex) const {
    if (temperatureIndex == 0) {
        return BiomeKeys::SNOWY_BEACH;
    } else {
        return temperatureIndex == 4 ? BiomeKeys::DESERT : BiomeKeys::BEACH;
    }
}

// Reference: OverworldBiomeBuilder.java lines 358-364
BiomeKey OverworldBiomeBuilder::pickBadlandsBiome(int humidityIndex, const Climate::Parameter& weirdness) const {
    if (humidityIndex < 2) {
        return weirdness.max() < 0 ? BiomeKeys::BADLANDS : BiomeKeys::ERODED_BADLANDS;
    } else {
        return humidityIndex < 3 ? BiomeKeys::BADLANDS : BiomeKeys::WOODED_BADLANDS;
    }
}

// Reference: OverworldBiomeBuilder.java lines 366-375
BiomeKey OverworldBiomeBuilder::pickPlateauBiome(int temperatureIndex, int humidityIndex, const Climate::Parameter& weirdness) const {
    if (weirdness.max() >= 0) {
        BiomeKey variant = m_plateauBiomesVariant[temperatureIndex][humidityIndex];
        if (!variant.empty()) {
            return variant;
        }
    }
    return m_plateauBiomes[temperatureIndex][humidityIndex];
}

// Reference: OverworldBiomeBuilder.java lines 377-383
BiomeKey OverworldBiomeBuilder::pickPeakBiome(int temperatureIndex, int humidityIndex, const Climate::Parameter& weirdness) const {
    if (temperatureIndex <= 2) {
        return weirdness.max() < 0 ? BiomeKeys::JAGGED_PEAKS : BiomeKeys::FROZEN_PEAKS;
    } else {
        return temperatureIndex == 3 ? BiomeKeys::STONY_PEAKS : pickBadlandsBiome(humidityIndex, weirdness);
    }
}

// Reference: OverworldBiomeBuilder.java lines 385-391
BiomeKey OverworldBiomeBuilder::pickSlopeBiome(int temperatureIndex, int humidityIndex, const Climate::Parameter& weirdness) const {
    if (temperatureIndex >= 3) {
        return pickPlateauBiome(temperatureIndex, humidityIndex, weirdness);
    } else {
        return humidityIndex <= 1 ? BiomeKeys::SNOWY_SLOPES : BiomeKeys::GROVE;
    }
}

// Reference: OverworldBiomeBuilder.java lines 393-396
BiomeKey OverworldBiomeBuilder::pickShatteredBiome(int temperatureIndex, int humidityIndex, const Climate::Parameter& weirdness) const {
    BiomeKey biome = m_shatteredBiomes[temperatureIndex][humidityIndex];
    return biome.empty() ? pickMiddleBiome(temperatureIndex, humidityIndex, weirdness) : biome;
}

// Reference: OverworldBiomeBuilder.java lines 398-401
void OverworldBiomeBuilder::addSurfaceBiome(
    std::function<void(const std::pair<Climate::ParameterPoint, BiomeKey>&)> consumer,
    const Climate::Parameter& temperature, const Climate::Parameter& humidity,
    const Climate::Parameter& continentalness, const Climate::Parameter& erosion,
    const Climate::Parameter& weirdness, float offset, const BiomeKey& biome) const
{
    consumer(std::make_pair(Climate::parameters(temperature, humidity, continentalness, erosion, Climate::Parameter::point(0.0F), weirdness, offset), biome));
    consumer(std::make_pair(Climate::parameters(temperature, humidity, continentalness, erosion, Climate::Parameter::point(1.0F), weirdness, offset), biome));
}

// Reference: OverworldBiomeBuilder.java lines 403-405
void OverworldBiomeBuilder::addUndergroundBiome(
    std::function<void(const std::pair<Climate::ParameterPoint, BiomeKey>&)> consumer,
    const Climate::Parameter& temperature, const Climate::Parameter& humidity,
    const Climate::Parameter& continentalness, const Climate::Parameter& erosion,
    const Climate::Parameter& weirdness, float offset, const BiomeKey& biome) const
{
    consumer(std::make_pair(Climate::parameters(temperature, humidity, continentalness, erosion, Climate::Parameter::span(0.2F, 0.9F), weirdness, offset), biome));
}

// Reference: OverworldBiomeBuilder.java lines 407-409
void OverworldBiomeBuilder::addBottomBiome(
    std::function<void(const std::pair<Climate::ParameterPoint, BiomeKey>&)> consumer,
    const Climate::Parameter& temperature, const Climate::Parameter& humidity,
    const Climate::Parameter& continentalness, const Climate::Parameter& erosion,
    const Climate::Parameter& weirdness, float offset, const BiomeKey& biome) const
{
    consumer(std::make_pair(Climate::parameters(temperature, humidity, continentalness, erosion, Climate::Parameter::point(1.1F), weirdness, offset), biome));
}

} // namespace biome
} // namespace world
} // namespace minecraft
