#pragma once

// Twilight Forest 4.9 — the NBT-template configured features:
// world/components/feature/templates/{TemplateFeature, DruidHutFeature,
// SimpleWellFeature, FancyWellFeature, GroveRuinsFeature, StoneCircleFeature,
// GraveyardFeature}.java with their processors (StateTransfiguringProcessor,
// WoodPaletteSwizzle, CobbleVariants, StoneBricksVariants,
// SmartGrassProcessor, GraveyardFeature.WebTemplateProcessor). Templates come
// from data/twilightforest/structure/feature/** through TemplateEngine.
// Registered by TwilightDecorFeatures::bootstrap().

namespace minecraft {
namespace data {
namespace worldgen {
namespace features {

class TwilightTemplateFeatures {
public:
    /** Register druid_hut, simple_well, fancy_well, well_placer, grove_ruins,
     *  stone_circle and graveyard. Idempotent. */
    static void bootstrap();

    static bool isInitialized();
};

} // namespace features
} // namespace worldgen
} // namespace data
} // namespace minecraft
