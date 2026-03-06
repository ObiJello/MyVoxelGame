#pragma once

#include "levelgen/feature/stateproviders/BlockStateProvider.h"
#include "levelgen/feature/featuresize/FeatureSize.h"
#include "levelgen/feature/trunkplacers/TrunkPlacer.h"
#include "levelgen/feature/foliageplacers/FoliagePlacer.h"
#include "levelgen/feature/treedecorators/TreeDecorator.h"
#include "levelgen/feature/rootplacers/RootPlacer.h"
#include <vector>
#include <memory>
#include <optional>

// Reference: net/minecraft/world/level/levelgen/feature/configurations/TreeConfiguration.java

namespace minecraft {
namespace levelgen {
namespace feature {
namespace configurations {

/**
 * TreeConfiguration - Complete configuration for tree generation
 * Reference: TreeConfiguration.java
 */
class TreeConfiguration {
public:
    std::shared_ptr<stateproviders::BlockStateProvider> trunkProvider;
    std::shared_ptr<stateproviders::BlockStateProvider> dirtProvider;
    std::shared_ptr<trunkplacers::TrunkPlacer> trunkPlacer;
    std::shared_ptr<stateproviders::BlockStateProvider> foliageProvider;
    std::shared_ptr<foliageplacers::FoliagePlacer> foliagePlacer;
    std::optional<std::shared_ptr<rootplacers::RootPlacer>> rootPlacer;
    std::shared_ptr<featuresize::FeatureSize> minimumSize;
    std::vector<std::shared_ptr<treedecorators::TreeDecorator>> decorators;
    bool ignoreVines;
    bool forceDirt;

    /**
     * Full constructor
     * Reference: TreeConfiguration.java lines 29-40
     */
    TreeConfiguration(
        std::shared_ptr<stateproviders::BlockStateProvider> trunkProv,
        std::shared_ptr<trunkplacers::TrunkPlacer> trunkPlcr,
        std::shared_ptr<stateproviders::BlockStateProvider> foliageProv,
        std::shared_ptr<foliageplacers::FoliagePlacer> foliagePlcr,
        std::optional<std::shared_ptr<rootplacers::RootPlacer>> rootPlcr,
        std::shared_ptr<stateproviders::BlockStateProvider> dirtProv,
        std::shared_ptr<featuresize::FeatureSize> minSize,
        const std::vector<std::shared_ptr<treedecorators::TreeDecorator>>& decors,
        bool ignVines,
        bool frcDirt
    )
        : trunkProvider(trunkProv)
        , dirtProvider(dirtProv)
        , trunkPlacer(trunkPlcr)
        , foliageProvider(foliageProv)
        , foliagePlacer(foliagePlcr)
        , rootPlacer(rootPlcr)
        , minimumSize(minSize)
        , decorators(decors)
        , ignoreVines(ignVines)
        , forceDirt(frcDirt)
    {}
};

/**
 * TreeConfigurationBuilder - Builder pattern for TreeConfiguration
 * Reference: TreeConfiguration.TreeConfigurationBuilder
 */
class TreeConfigurationBuilder {
private:
    std::shared_ptr<stateproviders::BlockStateProvider> m_trunkProvider;
    std::shared_ptr<trunkplacers::TrunkPlacer> m_trunkPlacer;
    std::shared_ptr<stateproviders::BlockStateProvider> m_foliageProvider;
    std::shared_ptr<foliageplacers::FoliagePlacer> m_foliagePlacer;
    std::optional<std::shared_ptr<rootplacers::RootPlacer>> m_rootPlacer;
    std::shared_ptr<stateproviders::BlockStateProvider> m_dirtProvider;
    std::shared_ptr<featuresize::FeatureSize> m_minimumSize;
    std::vector<std::shared_ptr<treedecorators::TreeDecorator>> m_decorators;
    bool m_ignoreVines = false;
    bool m_forceDirt = false;

public:
    /**
     * Constructor with required fields
     * Reference: TreeConfiguration.TreeConfigurationBuilder lines 54-63
     */
    TreeConfigurationBuilder(
        std::shared_ptr<stateproviders::BlockStateProvider> trunkProvider,
        std::shared_ptr<trunkplacers::TrunkPlacer> trunkPlacer,
        std::shared_ptr<stateproviders::BlockStateProvider> foliageProvider,
        std::shared_ptr<foliageplacers::FoliagePlacer> foliagePlacer,
        std::shared_ptr<featuresize::FeatureSize> minimumSize
    )
        : m_trunkProvider(trunkProvider)
        , m_trunkPlacer(trunkPlacer)
        , m_foliageProvider(foliageProvider)
        , m_foliagePlacer(foliagePlacer)
        , m_minimumSize(minimumSize)
        , m_dirtProvider(stateproviders::BlockStateProvider::simple("minecraft:dirt"))
    {}

    /**
     * Constructor with root placer
     */
    TreeConfigurationBuilder(
        std::shared_ptr<stateproviders::BlockStateProvider> trunkProvider,
        std::shared_ptr<trunkplacers::TrunkPlacer> trunkPlacer,
        std::shared_ptr<stateproviders::BlockStateProvider> foliageProvider,
        std::shared_ptr<foliageplacers::FoliagePlacer> foliagePlacer,
        std::shared_ptr<rootplacers::RootPlacer> rootPlacer,
        std::shared_ptr<featuresize::FeatureSize> minimumSize
    )
        : m_trunkProvider(trunkProvider)
        , m_trunkPlacer(trunkPlacer)
        , m_foliageProvider(foliageProvider)
        , m_foliagePlacer(foliagePlacer)
        , m_rootPlacer(rootPlacer)
        , m_minimumSize(minimumSize)
        , m_dirtProvider(stateproviders::BlockStateProvider::simple("minecraft:dirt"))
    {}

    /**
     * Set dirt provider
     * Reference: TreeConfiguration.TreeConfigurationBuilder lines 69-72
     */
    TreeConfigurationBuilder& dirt(std::shared_ptr<stateproviders::BlockStateProvider> dirtProvider) {
        m_dirtProvider = dirtProvider;
        return *this;
    }

    /**
     * Set decorators
     * Reference: TreeConfiguration.TreeConfigurationBuilder lines 74-77
     */
    TreeConfigurationBuilder& decorators(const std::vector<std::shared_ptr<treedecorators::TreeDecorator>>& decorators) {
        m_decorators = decorators;
        return *this;
    }

    /**
     * Enable ignore vines
     * Reference: TreeConfiguration.TreeConfigurationBuilder lines 79-82
     */
    TreeConfigurationBuilder& ignoreVines() {
        m_ignoreVines = true;
        return *this;
    }

    /**
     * Enable force dirt
     * Reference: TreeConfiguration.TreeConfigurationBuilder lines 84-87
     */
    TreeConfigurationBuilder& forceDirt() {
        m_forceDirt = true;
        return *this;
    }

    /**
     * Build the configuration
     * Reference: TreeConfiguration.TreeConfigurationBuilder lines 89-91
     */
    TreeConfiguration build() {
        return TreeConfiguration(
            m_trunkProvider,
            m_trunkPlacer,
            m_foliageProvider,
            m_foliagePlacer,
            m_rootPlacer,
            m_dirtProvider,
            m_minimumSize,
            m_decorators,
            m_ignoreVines,
            m_forceDirt
        );
    }
};

} // namespace configurations
} // namespace feature
} // namespace levelgen
} // namespace minecraft
