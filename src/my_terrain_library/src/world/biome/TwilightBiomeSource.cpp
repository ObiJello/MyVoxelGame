#include "world/biome/TwilightBiomeSource.h"
#include "random/LegacyRandomSource.h"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// The Twilight Forest biome layout. Every class below names the Java class it
// mirrors (mods_reference/twilightforest/src/main/java/twilightforest/
// world/components/layer/...). The layer parameters (salts, biome lists) are
// the ones twilight/biome_layer_stack/random_forest_biomes.json and
// biomes_along_streams.json carry, and the columns are
// twilight/biome_terrain_data/biome_grid.json.

namespace minecraft {
namespace world {
namespace biome {
namespace twilight {

// ============================================================================
// Biome ids
// ============================================================================

const std::vector<BiomeKey>& possibleBiomeKeys() {
    static const std::vector<BiomeKey> s_keys = {
        "twilightforest:underground",
        "twilightforest:clearing",
        "twilightforest:dark_forest",
        "twilightforest:dark_forest_center",
        "twilightforest:dense_forest",
        "twilightforest:dense_mushroom_forest",
        "twilightforest:enchanted_forest",
        "twilightforest:final_plateau",
        "twilightforest:fire_swamp",
        "twilightforest:firefly_forest",
        "twilightforest:forest",
        "twilightforest:glacier",
        "twilightforest:highlands_underground",
        "twilightforest:highlands",
        "twilightforest:lake",
        "twilightforest:mushroom_forest",
        "twilightforest:oak_savannah",
        "twilightforest:snowy_forest",
        "twilightforest:spooky_forest",
        "twilightforest:stream",
        "twilightforest:swamp",
        "twilightforest:thornlands",
    };
    return s_keys;
}

const BiomeKey& biomeKey(uint8_t id) {
    const auto& keys = possibleBiomeKeys();
    return keys[id < keys.size() ? id : static_cast<uint8_t>(FOREST)];
}

namespace {

// net/minecraft/util/LinearCongruentialGenerator.next — in unsigned
// arithmetic, which is Java's two's-complement long wrap-around without the
// signed-overflow UB.
int64_t lcgNext(int64_t rval, int64_t c) {
    uint64_t r = static_cast<uint64_t>(rval);
    r *= r * 6364136223846793005ULL + 1442695040888963407ULL;
    r += static_cast<uint64_t>(c);
    return static_cast<int64_t>(r);
}

// LazyAreaContext.mixSeed(seed, salt)
int64_t mixSeed(int64_t seed, int64_t salt) {
    int64_t i = lcgNext(salt, salt);
    i = lcgNext(i, salt);
    i = lcgNext(i, salt);
    int64_t j = lcgNext(seed, i);
    j = lcgNext(j, i);
    return lcgNext(j, i);
}

// Math.floorMod(long, int)
int32_t floorMod(int64_t x, int32_t y) {
    int64_t m = x % y;
    if (m != 0 && ((m < 0) != (y < 0))) m += y;
    return static_cast<int32_t>(m);
}

int64_t wrapAdd(int64_t a, int64_t b) {
    return static_cast<int64_t>(static_cast<uint64_t>(a) + static_cast<uint64_t>(b));
}

int64_t wrapMul(int64_t a, int64_t b) {
    return static_cast<int64_t>(static_cast<uint64_t>(a) * static_cast<uint64_t>(b));
}

} // namespace

// ============================================================================
// Layer — LazyArea + LazyAreaContext + RandomContext
// ============================================================================

Layer::Layer(int64_t worldSeed, int64_t salt)
    : m_seed(mixSeed(worldSeed, salt))
    , m_cache(new std::atomic<uint64_t>[size_t{1} << CACHE_BITS]) {
    for (size_t i = 0; i < (size_t{1} << CACHE_BITS); ++i) {
        m_cache[i].store(0, std::memory_order_relaxed);
    }
}

Layer::~Layer() = default;

uint8_t Layer::get(int32_t x, int32_t z) const {
    // Entry: bit 63 valid | x (27 bits) << 35 | z (27 bits) << 8 | biome.
    // 27 bits cover +-67M, well past the +-7.5M quart border.
    const uint64_t tag = (uint64_t{1} << 63)
        | ((static_cast<uint64_t>(static_cast<uint32_t>(x)) & 0x7FFFFFFULL) << 35)
        | ((static_cast<uint64_t>(static_cast<uint32_t>(z)) & 0x7FFFFFFULL) << 8);
    uint32_t h = static_cast<uint32_t>(x) * 0x9E3779B1u ^ static_cast<uint32_t>(z) * 0x85EBCA77u;
    h ^= h >> 15;
    std::atomic<uint64_t>& slot = m_cache[h & ((1u << CACHE_BITS) - 1u)];
    const uint64_t cached = slot.load(std::memory_order_relaxed);
    if ((cached & ~uint64_t{0xFF}) == tag) {
        return static_cast<uint8_t>(cached & 0xFF);
    }
    const uint8_t value = compute(x, z);
    slot.store(tag | value, std::memory_order_relaxed);
    return value;
}

// RandomContext.initRandom
void Layer::RandomContext::initRandom(int64_t x, int64_t z) {
    int64_t i = seed;
    i = lcgNext(i, x);
    i = lcgNext(i, z);
    i = lcgNext(i, x);
    i = lcgNext(i, z);
    rval = i;
}

// RandomContext.nextRandom
int32_t Layer::RandomContext::nextRandom(int32_t limit) {
    const int32_t result = floorMod(rval >> 24, limit);
    rval = lcgNext(rval, seed);
    return result;
}

// RandomContext.random(a, b, c, d)
uint8_t Layer::RandomContext::random(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    const int32_t i = nextRandom(4);
    if (i == 0) return a;
    if (i == 1) return b;
    return i == 2 ? c : d;
}

// AreaTransformer0/1/2.run: a fresh RandomContext per pixel, initRandom(x, z).
Layer::RandomContext Layer::contextAt(int32_t x, int32_t z) const {
    RandomContext rc(m_seed);
    rc.initRandom(x, z);
    return rc;
}

// ============================================================================
// TerrainColumn.getBiome
// ============================================================================

uint8_t TerrainColumn::biomeAt(int32_t quartY) const {
    // reduce((a, b) -> |a - y| <= |b - y| ? a : b), ascending keys; the
    // Double2ObjectAVLTreeMap default (the key biome) only for an empty map.
    if (layers.empty()) return keyBiome;
    const std::pair<double, uint8_t>* best = &layers.front();
    for (size_t i = 1; i < layers.size(); ++i) {
        const double aDelta = best->first - static_cast<double>(quartY);
        const double bDelta = layers[i].first - static_cast<double>(quartY);
        if (!(std::abs(aDelta) <= std::abs(bDelta))) best = &layers[i];
    }
    return best->second;
}

namespace {

// ============================================================================
// The layers
// ============================================================================

// RandomBiomeLayer (AreaTransformer0)
class RandomBiomesLayer final : public Layer {
public:
    RandomBiomesLayer(int64_t worldSeed, int64_t salt, int32_t rareChance,
                      std::vector<uint8_t> common, std::vector<uint8_t> rare)
        : Layer(worldSeed, salt), m_rareChance(rareChance),
          m_common(std::move(common)), m_rare(std::move(rare)) {}

protected:
    uint8_t compute(int32_t x, int32_t z) const override {
        RandomContext rc = contextAt(x, z);
        if (rc.nextRandom(m_rareChance) == 0) {
            return m_rare[static_cast<size_t>(rc.nextRandom(static_cast<int32_t>(m_rare.size())))];
        }
        return m_common[static_cast<size_t>(rc.nextRandom(static_cast<int32_t>(m_common.size())))];
    }

private:
    int32_t m_rareChance;
    std::vector<uint8_t> m_common;
    std::vector<uint8_t> m_rare;
};

// KeyBiomesLayer (AreaTransformer1; reads the parent at (x, z) directly).
// Places one key biome in each 4x4 cell of an 8x8 block, at a java.util.Random
// offset of 1..2 inside the cell, rotating the four keys by a per-16x16
// offset. The layer context's randomness is not used.
class KeyBiomesLayer final : public Layer {
public:
    KeyBiomesLayer(int64_t worldSeed, int64_t salt, const Layer* parent,
                   std::array<uint8_t, 4> keys)
        : Layer(worldSeed, salt), m_worldSeed(worldSeed), m_parent(parent), m_keys(keys) {}

protected:
    uint8_t compute(int32_t x, int32_t z) const override {
        // new Random(seed + (x & -4) * 25117L + (z & -4) * 151121L)
        LegacyRandomSource rand(wrapAdd(wrapAdd(m_worldSeed,
                                                wrapMul(static_cast<int64_t>(x & -4), 25117LL)),
                                        wrapMul(static_cast<int64_t>(z & -4), 151121LL)));
        const int32_t ox = rand.nextInt(2) + 1;
        const int32_t oz = rand.nextInt(2) + 1;
        // x / 8 is Java int division (truncates toward zero), as C++'s.
        rand.setSeed(wrapAdd(wrapAdd(m_worldSeed, wrapMul(static_cast<int64_t>(x / 8), 25117LL)),
                             wrapMul(static_cast<int64_t>(z / 8), 151121LL)));
        const int32_t offset = rand.nextInt(3);
        if ((x & 3) == ox && (z & 3) == oz) {
            if ((x & 4) == 0) {
                return (z & 4) == 0 ? keyFor(offset) : keyFor(offset + 1);
            }
            return (z & 4) == 0 ? keyFor(offset + 2) : keyFor(offset + 3);
        }
        return m_parent->get(x, z);
    }

private:
    uint8_t keyFor(int32_t index) const { return m_keys[static_cast<size_t>(index & 0b11)]; }

    int64_t m_worldSeed;
    const Layer* m_parent;
    std::array<uint8_t, 4> m_keys;
};

// CompanionBiomesLayer (CastleTransformer: parent offset -1, cross neighbours)
class CompanionBiomesLayer final : public Layer {
public:
    CompanionBiomesLayer(int64_t worldSeed, int64_t salt, const Layer* parent,
                         std::vector<std::pair<uint8_t, uint8_t>> keysToCompanions)
        : Layer(worldSeed, salt), m_parent(parent), m_pairs(std::move(keysToCompanions)) {}

protected:
    uint8_t compute(int32_t x, int32_t z) const override {
        const uint8_t up = m_parent->get(x, z - 1);
        const uint8_t right = m_parent->get(x + 1, z);
        const uint8_t down = m_parent->get(x, z + 1);
        const uint8_t left = m_parent->get(x - 1, z);
        const uint8_t center = m_parent->get(x, z);
        for (const auto& pair : m_pairs) {
            const uint8_t key = pair.first;
            if (center != key && (left == key || right == key || up == key || down == key)) {
                return pair.second;
            }
        }
        return center;
    }

private:
    const Layer* m_parent;
    std::vector<std::pair<uint8_t, uint8_t>> m_pairs;
};

// ZoomLayer.NORMAL (vanillalegacy/ZoomLayer.java). FUZZY is never configured
// by the shipped stacks ("fuzzy": false everywhere) but is ported for parity.
class ZoomLayer final : public Layer {
public:
    ZoomLayer(int64_t worldSeed, int64_t salt, const Layer* parent, bool fuzzy)
        : Layer(worldSeed, salt), m_parent(parent), m_fuzzy(fuzzy) {}

protected:
    uint8_t compute(int32_t x, int32_t z) const override {
        const uint8_t i = m_parent->get(x >> 1, z >> 1);
        RandomContext rc(m_seed);
        rc.initRandom((x >> 1) << 1, (z >> 1) << 1);
        const int32_t j = x & 1;
        const int32_t k = z & 1;
        if (j == 0 && k == 0) return i;
        const uint8_t l = m_parent->get(x >> 1, (z + 1) >> 1);
        const uint8_t i1 = rc.random(i, l);
        if (j == 0 && k == 1) return i1;
        const uint8_t j1 = m_parent->get((x + 1) >> 1, z >> 1);
        const uint8_t k1 = rc.random(i, j1);
        if (j == 1 && k == 0) return k1;
        const uint8_t l1 = m_parent->get((x + 1) >> 1, (z + 1) >> 1);
        return m_fuzzy ? rc.random(i, j1, l, l1) : modeOrRandom(rc, i, j1, l, l1);
    }

private:
    static uint8_t modeOrRandom(RandomContext& rc, uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
        if (b == c && c == d) return b;
        if (a == b && a == c) return a;
        if (a == b && a == d) return a;
        if (a == c && a == d) return a;
        if (a == b && c != d) return a;
        if (a == c && b != d) return a;
        if (a == d && b != c) return a;
        if (b == c && a != d) return b;
        if (b == d && a != c) return b;
        return (c == d && a != b) ? c : rc.random(a, b, c, d);
    }

    const Layer* m_parent;
    bool m_fuzzy;
};

// StabilizeLayer: near the centre of each 4x4 cell, spread the centre's biome
// over the surrounding 3x3. getParentX(x << 4) is (x << 4) & 3 == 0, so the
// offsets vanish.
class StabilizeLayer final : public Layer {
public:
    StabilizeLayer(int64_t worldSeed, int64_t salt, const Layer* parent)
        : Layer(worldSeed, salt), m_parent(parent) {}

protected:
    uint8_t compute(int32_t x, int32_t z) const override {
        const int32_t offX = (x << 4) & 3;
        const int32_t offZ = (z << 4) & 3;
        const int32_t centerX = ((x + offX + 1) & -4) - offX;
        const int32_t centerZ = ((z + offZ + 1) & -4) - offZ;
        if (x <= centerX + 1 && x >= centerX - 1 && z <= centerZ + 1 && z >= centerZ - 1) {
            return m_parent->get(centerX, centerZ);
        }
        return m_parent->get(x, z);
    }

private:
    const Layer* m_parent;
};

// BorderLayer (IThornsTransformer: parent offset -1, all eight neighbours).
// onBorder over the cross, then over the diagonals: any neighbour being the
// target while the centre is not puts the border biome here.
class BorderLayer final : public Layer {
public:
    BorderLayer(int64_t worldSeed, int64_t salt, const Layer* parent,
                uint8_t target, uint8_t border)
        : Layer(worldSeed, salt), m_parent(parent), m_target(target), m_border(border) {}

protected:
    uint8_t compute(int32_t x, int32_t z) const override {
        const uint8_t center = m_parent->get(x, z);
        if (center == m_target) return center;
        // north (x, z-1), east (x+1, z), south (x, z+1), west (x-1, z)
        if (m_parent->get(x, z - 1) == m_target || m_parent->get(x + 1, z) == m_target
            || m_parent->get(x, z + 1) == m_target || m_parent->get(x - 1, z) == m_target) {
            return m_border;
        }
        // (x+1, z-1), (x+1, z+1), (x-1, z+1), (x-1, z-1)
        if (m_parent->get(x + 1, z - 1) == m_target || m_parent->get(x + 1, z + 1) == m_target
            || m_parent->get(x - 1, z + 1) == m_target || m_parent->get(x - 1, z - 1) == m_target) {
            return m_border;
        }
        return center;
    }

private:
    const Layer* m_parent;
    uint8_t m_target;
    uint8_t m_border;
};

// SeamLayer (CastleTransformer): a stream between any two differing
// neighbours unless one of them is an excluded neighbour or the pair is an
// excluded intersection (a key biome and its companion).
class SeamLayer final : public Layer {
public:
    SeamLayer(int64_t worldSeed, int64_t salt, const Layer* parent, uint8_t dividing,
              std::vector<uint8_t> excludedNeighbors,
              std::vector<std::pair<uint8_t, uint8_t>> excludedIntersections)
        : Layer(worldSeed, salt), m_parent(parent), m_dividing(dividing),
          m_excludedNeighbors(std::move(excludedNeighbors)),
          m_excludedIntersections(std::move(excludedIntersections)) {}

protected:
    uint8_t compute(int32_t x, int32_t z) const override {
        const uint8_t mid = m_parent->get(x, z);
        if (shouldPartition(mid, m_parent->get(x - 1, z))
            || shouldPartition(mid, m_parent->get(x + 1, z))
            || shouldPartition(mid, m_parent->get(x, z + 1))
            || shouldPartition(mid, m_parent->get(x, z - 1))) {
            return m_dividing;
        }
        return mid;
    }

private:
    bool shouldPartition(uint8_t a, uint8_t b) const {
        if (a == b) return false;
        for (uint8_t excluded : m_excludedNeighbors) {
            if (a == excluded || b == excluded) return false;
        }
        for (const auto& pair : m_excludedIntersections) {
            if ((a == pair.first && b == pair.second) || (b == pair.first && a == pair.second)) {
                return false;
            }
        }
        return true;
    }

    const Layer* m_parent;
    uint8_t m_dividing;
    std::vector<uint8_t> m_excludedNeighbors;
    std::vector<std::pair<uint8_t, uint8_t>> m_excludedIntersections;
};

// vanillalegacy/SmoothLayer (CastleTransformer)
class SmoothLayer final : public Layer {
public:
    SmoothLayer(int64_t worldSeed, int64_t salt, const Layer* parent)
        : Layer(worldSeed, salt), m_parent(parent) {}

protected:
    uint8_t compute(int32_t x, int32_t z) const override {
        RandomContext rc = contextAt(x, z);
        const uint8_t up = m_parent->get(x, z - 1);
        const uint8_t right = m_parent->get(x + 1, z);
        const uint8_t down = m_parent->get(x, z + 1);
        const uint8_t left = m_parent->get(x - 1, z);
        const bool xMatch = right == left;
        const bool zMatch = up == down;
        if (xMatch == zMatch) {
            if (xMatch) return rc.nextRandom(2) == 0 ? left : up;
            return m_parent->get(x, z);
        }
        return xMatch ? left : up;
    }

private:
    const Layer* m_parent;
};

// FilteredBiomeLayer (AreaTransformer2, offset 0): the filtered layer where it
// is the kept biome (the stream), the fallback everywhere else.
class FilteredBiomeLayer final : public Layer {
public:
    FilteredBiomeLayer(int64_t worldSeed, int64_t salt, const Layer* fallback,
                       const Layer* filtered, uint8_t keep)
        : Layer(worldSeed, salt), m_fallback(fallback), m_filtered(filtered), m_keep(keep) {}

protected:
    uint8_t compute(int32_t x, int32_t z) const override {
        const uint8_t river = m_filtered->get(x, z);
        return river == m_keep ? river : m_fallback->get(x, z);
    }

private:
    const Layer* m_fallback;
    const Layer* m_filtered;
    uint8_t m_keep;
};

std::atomic<uint64_t> g_nextLayoutUid{1};

} // namespace
} // namespace twilight

// ============================================================================
// TwilightBiomeLayout
// ============================================================================

using namespace twilight;

TwilightBiomeLayout::TwilightBiomeLayout(int64_t seed)
    : m_seed(seed)
    , m_uid(g_nextLayoutUid.fetch_add(1, std::memory_order_relaxed))
    , m_root(nullptr) {
    auto add = [this](std::unique_ptr<Layer> layer) -> const Layer* {
        const Layer* raw = layer.get();
        m_layers.push_back(std::move(layer));
        return raw;
    };

    // ---- random_forest_biomes.json (innermost parent first) ----
    const Layer* layer = add(std::make_unique<RandomBiomesLayer>(seed, 1, 15,
        std::vector<uint8_t>{FOREST, DENSE_FOREST, MUSHROOM_FOREST, OAK_SAVANNAH, FIREFLY_FOREST},
        std::vector<uint8_t>{LAKE, DENSE_MUSHROOM_FOREST, ENCHANTED_FOREST, CLEARING, SPOOKY_FOREST}));
    layer = add(std::make_unique<KeyBiomesLayer>(seed, 1000, layer,
        std::array<uint8_t, 4>{GLACIER, FIRE_SWAMP, DARK_FOREST_CENTER, FINAL_PLATEAU}));
    layer = add(std::make_unique<CompanionBiomesLayer>(seed, 1000, layer,
        std::vector<std::pair<uint8_t, uint8_t>>{
            {FIRE_SWAMP, SWAMP},
            {GLACIER, SNOWY_FOREST},
            {DARK_FOREST_CENTER, DARK_FOREST},
            {FINAL_PLATEAU, HIGHLANDS}}));
    layer = add(std::make_unique<ZoomLayer>(seed, 1000, layer, false));
    layer = add(std::make_unique<ZoomLayer>(seed, 1001, layer, false));
    layer = add(std::make_unique<StabilizeLayer>(seed, 700, layer));
    layer = add(std::make_unique<BorderLayer>(seed, 500, layer, FINAL_PLATEAU, THORNLANDS));
    layer = add(std::make_unique<ZoomLayer>(seed, 1002, layer, false));
    layer = add(std::make_unique<ZoomLayer>(seed, 1003, layer, false));
    layer = add(std::make_unique<ZoomLayer>(seed, 1004, layer, false));
    const Layer* randomForestBiomes = add(std::make_unique<ZoomLayer>(seed, 1005, layer, false));

    // ---- biomes_along_streams.json ----
    // Java builds random_forest_biomes twice (the fallback and the seam's
    // parent) from identical factories; both are pure functions of the seed,
    // so one instance serves both.
    const Layer* seam = add(std::make_unique<SeamLayer>(seed, 1, randomForestBiomes, STREAM,
        std::vector<uint8_t>{LAKE, THORNLANDS, CLEARING, OAK_SAVANNAH},
        std::vector<std::pair<uint8_t, uint8_t>>{
            {SNOWY_FOREST, GLACIER},
            {MUSHROOM_FOREST, DENSE_MUSHROOM_FOREST},
            {SWAMP, FIRE_SWAMP},
            {DARK_FOREST, DARK_FOREST_CENTER},
            {HIGHLANDS, FINAL_PLATEAU}}));
    const Layer* smooth = add(std::make_unique<SmoothLayer>(seed, 7000, seam));
    m_root = add(std::make_unique<FilteredBiomeLayer>(seed, 100, randomForestBiomes, smooth, STREAM));

    // ---- biome_grid.json "biome_landscape" ----
    m_hasColumn.fill(false);
    auto column = [this](uint8_t key, double depth, double scale, double weight,
                         std::vector<std::pair<double, uint8_t>> layers) {
        TerrainColumn& c = m_columns[key];
        c.keyBiome = key;
        c.depth = depth;
        c.scale = scale;
        c.weight = weight;
        c.layers = std::move(layers);   // already ascending
        m_hasColumn[key] = true;
    };
    column(CLEARING,              0.05,  1.15,  1.0,   {{-3.0, UNDERGROUND}, {-1.0, CLEARING}});
    column(DARK_FOREST,           0.1,   1.25,  0.5,   {{-3.0, UNDERGROUND}, {-1.0, DARK_FOREST}});
    column(DARK_FOREST_CENTER,    0.1,   1.125, 1.0,   {{-3.0, UNDERGROUND}, {-1.0, DARK_FOREST_CENTER}});
    column(DENSE_FOREST,          -0.8,  3.9,   1.0,   {{-3.8, UNDERGROUND}, {-1.8, DENSE_FOREST}});
    column(DENSE_MUSHROOM_FOREST, 0.0,   1.75,  1.0,   {{-3.0, UNDERGROUND}, {-1.0, DENSE_MUSHROOM_FOREST}});
    column(ENCHANTED_FOREST,      -0.5,  4.0,   1.0,   {{-3.5, UNDERGROUND}, {-1.5, ENCHANTED_FOREST}});
    column(FINAL_PLATEAU,         12.0,  0.75,  1.0,   {{0.0, FINAL_PLATEAU}});
    column(FIRE_SWAMP,            0.2,   1.25,  1.0,   {{-3.0, UNDERGROUND}, {-1.0, FIRE_SWAMP}});
    column(FIREFLY_FOREST,        -0.75, 4.15,  1.0,   {{-3.75, UNDERGROUND}, {-1.75, FIREFLY_FOREST}});
    column(FOREST,                -0.7,  4.2,   1.0,   {{-3.7, UNDERGROUND}, {-1.7, FOREST}});
    column(GLACIER,               -0.05, 1.75,  1.0,   {{-3.05, UNDERGROUND}, {-1.05, GLACIER}});
    column(HIGHLANDS,             3.0,   2.25,  0.135, {{-3.0, HIGHLANDS_UNDERGROUND}, {-1.0, HIGHLANDS}});
    column(LAKE,                  -2.2,  2.0,   1.0,   {{-5.2, UNDERGROUND}, {-3.2, LAKE}});
    column(MUSHROOM_FOREST,       0.0,   2.0,   1.0,   {{-3.0, UNDERGROUND}, {-1.0, MUSHROOM_FOREST}});
    column(OAK_SAVANNAH,          -0.05, 2.0,   1.0,   {{-3.05, UNDERGROUND}, {-1.05, OAK_SAVANNAH}});
    column(SNOWY_FOREST,          0.0,   2.45,  0.5,   {{-3.0, UNDERGROUND}, {-1.0, SNOWY_FOREST}});
    column(SPOOKY_FOREST,         0.0,   2.25,  1.0,   {{-3.0, UNDERGROUND}, {-1.0, SPOOKY_FOREST}});
    column(STREAM,                -0.1,  0.001, 1.35,  {{-3.1, UNDERGROUND}, {-1.1, STREAM}});
    column(SWAMP,                 -0.6,  1.7,   1.0,   {{-3.6, UNDERGROUND}, {-1.6, SWAMP}});
    column(THORNLANDS,            5.5,   1.75,  1.15,  {{0.0, THORNLANDS}});
}

TwilightBiomeLayout::~TwilightBiomeLayout() = default;

uint8_t TwilightBiomeLayout::keyBiomeAt(int32_t quartX, int32_t quartZ) const {
    return m_root->get(quartX, quartZ);
}

const TerrainColumn& TwilightBiomeLayout::column(uint8_t keyBiome) const {
    // The stack only ever emits biomes that have a column (the random,
    // key, companion, border and stream biomes are all in the grid);
    // fall back to the forest column rather than index out of the array.
    if (keyBiome < BIOME_COUNT && m_hasColumn[keyBiome]) return m_columns[keyBiome];
    return m_columns[FOREST];
}

uint8_t TwilightBiomeLayout::noiseBiomeAt(int32_t quartX, int32_t quartY, int32_t quartZ) const {
    return column(keyBiomeAt(quartX, quartZ)).biomeAt(quartY);
}

DensityData TwilightBiomeLayout::sampleTerrain(int32_t blockX, int32_t blockZ) const {
    // BiomeDensitySource.sampleTerrain. QuartPos.BITS = 2, SIZE = 4.
    constexpr double BLEND_RADIUS = 8.75;
    constexpr int32_t BLEND_RADIUS_INT = 9;   // Mth.floor(BLEND_RADIUS + 1.0)
    constexpr int32_t BLOCK_XYZ_OFFSET = 2;   // QuartPos.SIZE / 2
    constexpr double RADIUS_SQ = BLEND_RADIUS * BLEND_RADIUS;
    // (distSq * 2f + depth) * -0.4f: float constants widened to double.
    const double two = static_cast<double>(2.0f);
    const double falloffRate = static_cast<double>(-0.4f);

    double totalMappedDepth = 0.0;
    double totalContribution = 0.0;
    double totalScale = 0.0;
    double totalScaleContribution = 0.0;

    const int32_t blockXWithOffset = blockX - BLOCK_XYZ_OFFSET;
    const int32_t blockZWithOffset = blockZ - BLOCK_XYZ_OFFSET;

    const int32_t xQuartStart = (blockXWithOffset - BLEND_RADIUS_INT) >> 2;
    const int32_t zQuartStart = (blockZWithOffset - BLEND_RADIUS_INT) >> 2;
    const int32_t xQuartEnd = (blockXWithOffset + BLEND_RADIUS_INT) >> 2;
    const int32_t zQuartEnd = (blockZWithOffset + BLEND_RADIUS_INT) >> 2;
    const int32_t xCount = xQuartEnd - xQuartStart + 1;
    const int32_t zCount = zQuartEnd - zQuartStart + 1;

    const double xQuartDelta = (blockXWithOffset - (xQuartStart << 2)) * (1.0 / 4.0);
    const double zQuartDelta = (blockZWithOffset - (zQuartStart << 2)) * (1.0 / 4.0);

    // Java walks cz fastest, cx slowest; the sums keep that order.
    for (int32_t cx = 0; cx < xCount; ++cx) {
        for (int32_t cz = 0; cz < zCount; ++cz) {
            const double dX = xQuartDelta - cx;
            const double dZ = zQuartDelta - cz;
            const double distSq = dX * dX + dZ * dZ;
            if (distSq >= RADIUS_SQ) continue;

            const TerrainColumn& c = column(keyBiomeAt(cx + xQuartStart, cz + zQuartStart));
            double falloff = RADIUS_SQ * c.weight;
            double scaleFalloff = RADIUS_SQ * c.weight;

            falloff *= std::exp((distSq * two + c.depth) * falloffRate);
            totalMappedDepth += c.depth * falloff;
            totalContribution += falloff;

            scaleFalloff *= std::exp((distSq * two + c.scale) * falloffRate);
            totalScale += c.scale * scaleFalloff;
            totalScaleContribution += scaleFalloff;
        }
    }

    return DensityData{totalMappedDepth / totalContribution, totalScale / totalScaleContribution};
}

// ============================================================================
// TwilightBiomeSource — TFBiomeProvider
// ============================================================================

TwilightBiomeSource::TwilightBiomeSource(int64_t seed)
    : m_layout(std::make_shared<TwilightBiomeLayout>(seed)) {
    for (const BiomeKey& key : possibleBiomeKeys()) {
        m_possibleBiomes.insert(key);
    }
}

BiomeKey TwilightBiomeSource::getNoiseBiome(int32_t quartX, int32_t quartY, int32_t quartZ,
                                            const Climate::Sampler& sampler) {
    (void)sampler;
    return biomeKey(m_layout->noiseBiomeAt(quartX, quartY, quartZ));
}

BiomeKey TwilightBiomeSource::getMainBiome(int32_t quartX, int32_t quartZ) const {
    return biomeKey(m_layout->keyBiomeAt(quartX, quartZ));
}

void TwilightBiomeSource::addDebugInfo(std::vector<std::string>& info,
                                       int32_t x, int32_t y, int32_t z,
                                       const Climate::Sampler& sampler) const {
    (void)sampler;
    // BiomeDensitySource.addDebugInfo
    const uint8_t key = m_layout->keyBiomeAt(x >> 2, z >> 2);
    const TerrainColumn& c = m_layout->column(key);
    info.push_back("Twilight Biome Column:");
    for (const auto& layer : c.layers) {
        char line[128];
        std::snprintf(line, sizeof line, "%.2f: %s", layer.first, biomeKey(layer.second).c_str());
        info.push_back(line);
    }
    info.push_back("Primary Biome: " + biomeKey(key));
    info.push_back("Biome at elevation: " + biomeKey(c.biomeAt(y >> 2)));
}

} // namespace biome
} // namespace world
} // namespace minecraft
