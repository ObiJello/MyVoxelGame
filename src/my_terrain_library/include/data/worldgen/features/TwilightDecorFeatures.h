#pragma once

// Twilight Forest 4.9 — decoration configured features (pass two).
//
// Ports the configured features the TF biomes' placed features name beyond
// pass one's trees/flora: berry and oreberry bushes, huge lily pads and water
// lilies, webs, thorns, enchanted-forest vines, fire jets and smokers,
// lampposts, ruined foundations, the monolith, the big mushgloom, the
// mycelium-blob patches, troll-cave mushglooms and the outside stalagmite
// (world/components/feature/**), plus the template features (druid hut,
// wells, grove ruins, stone circle, graveyard — TwilightTemplateFeatures).
// Every feature registers under its configured_feature JSON id through
// TwilightFeatureRegistry; TwilightPlacements builds the placed features.

namespace minecraft {
namespace data {
namespace worldgen {
namespace features {

class TwilightDecorFeatures {
public:
    /**
     * Resolve blocks and register every decoration configured feature.
     * Idempotent; runs TwilightFeatures::bootstrap() first when needed. A
     * feature whose blocks do not resolve is not registered (logged once).
     */
    static void bootstrap();

    static bool isInitialized();
};

} // namespace features
} // namespace worldgen
} // namespace data
} // namespace minecraft
