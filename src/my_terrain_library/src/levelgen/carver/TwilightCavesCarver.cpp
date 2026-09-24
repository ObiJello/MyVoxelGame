#include "levelgen/carver/TwilightCavesCarver.h"
#include "levelgen/RandomState.h"
#include "levelgen/blockpredicates/BlockPredicate.h"
#include "levelgen/feature/stateproviders/BlockStateProvider.h"
#include "synth/NormalNoise.h"
#include "world/level/block/Blocks.h"
#include "core/SectionPos.h"
#include "math/Mth.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

// world/components/TFCavesCarver.java, init/TFCaveCarvers.java,
// world/components/NoiseCarverWallProvider.java,
// util/landmarks/LegacyLandmarkPlacements.java (Twilight Forest 4.9).

namespace minecraft {
namespace levelgen {
namespace carver {

namespace {

constexpr float PI_F = 3.14159265358979323846f;

using minecraft::world::level::block::Blocks;

// Direction.values(): DOWN, UP, NORTH, SOUTH, WEST, EAST.
constexpr int DIRECTIONS[6][3] = {
    {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}
};

BlockState* stateOf(const char* name) {
    return Blocks::getDefaultState(name);
}

// A block tag resolved through the runtime tag registry
// (data/minecraft/tags/block/<tag>.json), with the vanilla contents as the
// fallback so a missing tag file cannot silently change the carve.
std::unordered_set<std::string> tagSet(const std::string& tag,
                                       std::initializer_list<const char*> fallback) {
    const auto& values = blockpredicates::orderedBlockTagValues(tag);
    std::unordered_set<std::string> out(values.begin(), values.end());
    if (out.empty()) {
        for (const char* v : fallback) out.insert(v);
    }
    return out;
}

const std::unordered_set<std::string>& iceTag() {
    static const std::unordered_set<std::string> s_ice = tagSet("minecraft:ice", {
        "minecraft:ice", "minecraft:packed_ice", "minecraft:blue_ice", "minecraft:frosted_ice"});
    return s_ice;
}

const std::unordered_set<std::string>& baseStoneOverworldTag() {
    static const std::unordered_set<std::string> s_stone = tagSet("minecraft:base_stone_overworld", {
        "minecraft:stone", "minecraft:granite", "minecraft:diorite", "minecraft:andesite",
        "minecraft:tuff", "minecraft:deepslate"});
    return s_stone;
}

bool isInsideChunk(const core::BlockPos& relative, const core::BlockPos& chunkOrigin) {
    const int32_t deltaX = relative.getX() - chunkOrigin.getX();
    const int32_t deltaZ = relative.getZ() - chunkOrigin.getZ();
    return deltaX >= 0 && deltaZ >= 0 && deltaX <= 15 && deltaZ <= 15;
}

bool shouldSkip(double posX, double posY, double posZ, double minY) {
    if (posY <= minY) return true;
    return posX * posX + posY * posY + posZ * posZ >= 1.0;
}

// NoiseProvider with the noise lookup exposed: NoiseCarverWallProvider
// .getState(random, pos) = getRandomState(states, pos, scale), random unused.
class NoiseWallProvider : public feature::stateproviders::NoiseProvider {
public:
    using NoiseProvider::NoiseProvider;
    BlockState* at(const core::BlockPos& pos) const {
        return getRandomState(m_states, pos, static_cast<double>(m_scale));
    }
};

} // namespace

// ============================================================================
// LegacyLandmarkPlacements
// ============================================================================

core::BlockPos twilightNearestLandmarkCenter(int32_t chunkX, int32_t chunkZ) {
    // generate random number for the whole biome area
    const int32_t regionX = (chunkX + 8) >> 4;
    const int32_t regionZ = (chunkZ + 8) >> 4;

    // long seed = regionX * 3129871L ^ regionZ * 116129781L;
    // seed = seed * seed * 42317861L + seed * 7L;   (two's-complement wrap)
    uint64_t seed = (static_cast<uint64_t>(static_cast<int64_t>(regionX)) * 3129871ULL)
                  ^ (static_cast<uint64_t>(static_cast<int64_t>(regionZ)) * 116129781ULL);
    seed = seed * seed * 42317861ULL + seed * 7ULL;
    const int64_t s = static_cast<int64_t>(seed);

    const int32_t num0 = static_cast<int32_t>((s >> 12) & 3LL);
    const int32_t num1 = static_cast<int32_t>((s >> 15) & 3LL);
    const int32_t num2 = static_cast<int32_t>((s >> 18) & 3LL);
    const int32_t num3 = static_cast<int32_t>((s >> 21) & 3LL);

    // slightly randomize center of biome (+/- 3)
    const int32_t centerX = 8 + num0 - num1;
    const int32_t centerZ = 8 + num2 - num3;

    // centers are offset strangely depending on +/-
    const int32_t ccz = regionZ >= 0 ? (regionZ * 16 + centerZ - 8) * 16 + 8
                                     : (regionZ * 16 + (16 - centerZ) - 8) * 16 + 9;
    const int32_t ccx = regionX >= 0 ? (regionX * 16 + centerX - 8) * 16 + 8
                                     : (regionX * 16 + (16 - centerX) - 8) * 16 + 9;
    return core::BlockPos(ccx, 0, ccz);
}

int32_t twilightManhattanDistanceFromLandmarkCenter(int32_t chunkX, int32_t chunkZ) {
    const core::BlockPos nearestCenter = twilightNearestLandmarkCenter(chunkX, chunkZ);
    const int32_t deltaChunkX = std::abs(chunkX - (nearestCenter.getX() >> 4));
    const int32_t deltaChunkZ = std::abs(chunkZ - (nearestCenter.getZ() >> 4));
    return deltaChunkX + deltaChunkZ;
}

// ============================================================================
// TFCavesCarver
// ============================================================================

TwilightCavesCarver::TwilightCavesCarver(bool isHighlands, WallProvider wallBlocks)
    : m_isHighlands(isHighlands)
    , m_wallBlocks(std::move(wallBlocks)) {
    // "Since this object is constructed on game bootup instead of world
    // creation, we can't use world seed" — a fixed seed, as in Java.
    LegacyRandomSource noiseRandom(6972119253061020355LL);
    m_noise = std::make_unique<ImprovedNoise>(noiseRandom);
}

std::unique_ptr<TwilightCavesCarver> TwilightCavesCarver::createTwilightCaves() {
    // NoiseCarverWallProvider(6972119253061020355L, NoiseParameters(0, 1.0),
    // 0.5f, [dirt, dirt, rooted_dirt, dirt, dirt, coarse_dirt, dirt, dirt]).
    BlockState* dirt = stateOf("minecraft:dirt");
    BlockState* rooted = stateOf("minecraft:rooted_dirt");
    BlockState* coarse = stateOf("minecraft:coarse_dirt");
    if (!rooted) rooted = dirt;
    if (!coarse) coarse = dirt;
    auto provider = std::make_shared<NoiseWallProvider>(
        6972119253061020355LL,
        NormalNoise::NoiseParameters(0, std::vector<double>{1.0}),
        0.5f,
        std::vector<BlockState*>{dirt, dirt, rooted, dirt, dirt, coarse, dirt, dirt});
    return std::make_unique<TwilightCavesCarver>(false,
        [provider](random::AnyRandomSource& random, const core::BlockPos& pos) -> BlockState* {
            (void)random;
            return provider->at(pos);
        });
}

std::unique_ptr<TwilightCavesCarver> TwilightCavesCarver::createHighlandCaves() {
    // WeightedList: trollsteinn 1, stone 3 — getRandomOrThrow draws
    // nextInt(totalWeight) and walks the entries in order. Trollsteinn is
    // registered (minecraft:trollsteinn); deepslate is only the fallback for
    // a registry without it (same weights, same draws).
    BlockState* stone = stateOf("minecraft:stone");
    BlockState* trollsteinn = stateOf("minecraft:trollsteinn");
    if (!trollsteinn) trollsteinn = stateOf("minecraft:deepslate");
    if (!trollsteinn) trollsteinn = stone;
    return std::make_unique<TwilightCavesCarver>(true,
        [trollsteinn, stone](random::AnyRandomSource& random, const core::BlockPos& pos) -> BlockState* {
            (void)pos;
            const int32_t roll = random.nextInt(4);
            return roll < 1 ? trollsteinn : stone;
        });
}

bool TwilightCavesCarver::isStartChunk(const CaveCarverConfiguration& configuration,
                                       XoroshiroRandomSource& random) {
    // Highland caves instead spawn with special location rules (no draw).
    return m_isHighlands || random.nextFloat() <= configuration.probability;
}

bool TwilightCavesCarver::isStartChunk(const CaveCarverConfiguration& configuration,
                                       LegacyRandomSource& random) {
    return m_isHighlands || random.nextFloat() <= configuration.probability;
}

bool TwilightCavesCarver::carve(CarvingContext& context, const CaveCarverConfiguration& configuration,
                                ::world::IChunk* chunk, std::function<void*(const core::BlockPos&)> biomeGetter,
                                XoroshiroRandomSource& random, density::Aquifer* aquifer,
                                const ::world::ChunkPos& sourceChunkPos, CarvingMask& mask) {
    return carveImpl(context, configuration, chunk, std::move(biomeGetter), random, aquifer,
                     sourceChunkPos, mask);
}

bool TwilightCavesCarver::carve(CarvingContext& context, const CaveCarverConfiguration& configuration,
                                ::world::IChunk* chunk, std::function<void*(const core::BlockPos&)> biomeGetter,
                                LegacyRandomSource& random, density::Aquifer* aquifer,
                                const ::world::ChunkPos& sourceChunkPos, CarvingMask& mask) {
    return carveImpl(context, configuration, chunk, std::move(biomeGetter), random, aquifer,
                     sourceChunkPos, mask);
}

template<typename R>
float TwilightCavesCarver::getThickness(R& random) {
    // Java evaluates rand.nextFloat() * 2.0F + rand.nextFloat() left to right.
    const float r1 = random.nextFloat();
    const float r2 = random.nextFloat();
    float f = r1 * 2.0f + r2;
    if (random.nextInt(10) == 0) {
        const float r3 = random.nextFloat();
        const float r4 = random.nextFloat();
        f *= r3 * r4 * 3.0f + 1.0f;
    }
    return f;
}

template<typename R>
bool TwilightCavesCarver::carveImpl(CarvingContext& context, const CaveCarverConfiguration& configuration,
                                    ::world::IChunk* chunk, std::function<void*(const core::BlockPos&)> biomeGetter,
                                    R& random, density::Aquifer* aquifer, const ::world::ChunkPos& accessPos,
                                    CarvingMask& mask) {
    // If highlands, enforces a binary grid (diagonal range of 4 chunks) of
    // possible placements around the structure center, with center being one
    // of the zero tiles.
    if (m_isHighlands
        && (std::clamp(twilightManhattanDistanceFromLandmarkCenter(accessPos.x(), accessPos.z()), 0, 3) & 1) == 1) {
        return false;
    }

    const int32_t i = core::SectionPos::sectionToBlockCoord(getRange() * 2 - 1);

    // If highlands, only roll chance to generate even 1 cave. Otherwise,
    // limited caves spawn for regular TF underground (getCaveBound() = 4).
    const int32_t caveCount = m_isHighlands ? random.nextInt(2) : random.nextInt(4);

    for (int32_t caveIndex = 0; caveIndex < caveCount; ++caveIndex) {
        const double x = static_cast<double>(accessPos.getBlockX(random.nextInt(16)));
        const double y = static_cast<double>(configuration.y->sample(random, context));
        const double z = static_cast<double>(accessPos.getBlockZ(random.nextInt(16)));
        const double horiz = static_cast<double>(configuration.horizontalRadiusMultiplier->sample(random));
        const double vert = static_cast<double>(configuration.verticalRadiusMultiplier->sample(random));
        const double floor = static_cast<double>(configuration.floorLevel->sample(random));
        CarveSkipChecker checker = [floor](const CarvingContext&, double dX, double dY, double dZ, int32_t) {
            return shouldSkip(dX, dY, dZ, floor);
        };

        int32_t tunnelCount = 1;
        if (m_isHighlands || random.nextInt(4) == 0) {
            const double horizToVertRatio = static_cast<double>(configuration.yScale->sample(random));
            const float radius = 1.0f + random.nextFloat() * 6.0f;
            createRoom(context, configuration, chunk, biomeGetter, aquifer, x, y, z, radius,
                       horizToVertRatio, mask, checker);
            tunnelCount += random.nextInt(4);
        }

        for (int32_t tunnelIndex = 0; tunnelIndex < tunnelCount; ++tunnelIndex) {
            const float randomRadian = random.nextFloat() * (PI_F * 2.0f);
            const float randomPitch = (random.nextFloat() - 0.5f) / 4.0f;
            const float thickness = getThickness(random);
            const int32_t branchCount = i - random.nextInt(i / 4);
            const int64_t tunnelSeed = random.nextLong();
            createTunnel(context, configuration, chunk, biomeGetter, tunnelSeed, aquifer, x, y, z,
                         horiz, vert, thickness, randomRadian, randomPitch, 0, branchCount,
                         1.0 /* getYScale() */, mask, checker);
        }
    }
    return true;
}

void TwilightCavesCarver::createRoom(CarvingContext& context, const CaveCarverConfiguration& configuration,
                                     ::world::IChunk* chunk, std::function<void*(const core::BlockPos&)> biomeGetter,
                                     density::Aquifer* aquifer, double posX, double posY, double posZ, float radius,
                                     double horizToVertRatio, CarvingMask& mask, CarveSkipChecker checker) {
    // Unlike vanilla's createRoom there is no +1 on x.
    const double d0 = 1.5 + static_cast<double>(Mth::sin(static_cast<double>(PI_F / 2.0f)) * radius);
    const double d1 = d0 * horizToVertRatio;
    carveEllipsoid(context, configuration, chunk, biomeGetter, aquifer, posX, posY, posZ, d0, d1,
                   mask, checker);
}

void TwilightCavesCarver::createTunnel(CarvingContext& context, const CaveCarverConfiguration& configuration,
                                       ::world::IChunk* chunk, std::function<void*(const core::BlockPos&)> biomeGetter,
                                       int64_t seed, density::Aquifer* aquifer, double posX, double posY, double posZ,
                                       double horizMult, double vertMult, float thickness, float yaw, float pitch,
                                       int32_t branchIndex, int32_t branchCount, double horizToVertRatio,
                                       CarvingMask& mask, CarveSkipChecker checker) {
    // RandomSource.create(seed) is a LegacyRandomSource.
    LegacyRandomSource random(seed);
    const int32_t i = random.nextInt(branchCount / 2) + branchCount / 4;
    const bool flag = random.nextInt(6) == 0;
    float f = 0.0f;
    float f1 = 0.0f;
    const int32_t minY = chunk->getMinBuildHeight();

    for (int32_t j = branchIndex; j < branchCount; ++j) {
        const double horizontalRadius = 1.5 + static_cast<double>(
            Mth::sin(static_cast<double>(PI_F * static_cast<float>(j) / static_cast<float>(branchCount))) * thickness);
        const double verticalRadius = horizontalRadius * horizToVertRatio;
        const float f2 = Mth::cos(static_cast<double>(pitch));
        posX += static_cast<double>(Mth::cos(static_cast<double>(yaw)) * f2);

        const float yShift = Mth::sin(static_cast<double>(pitch));
        // If posY nears bedrock, "slow" its descent if marching downwards
        posY += (yShift > 0.0f || posY + static_cast<double>(yShift) > static_cast<double>(minY + 10))
                    ? static_cast<double>(yShift)
                    : static_cast<double>(yShift * 0.25f);

        posZ += static_cast<double>(Mth::sin(static_cast<double>(yaw)) * f2);
        pitch = pitch * (flag ? 0.92f : 0.7f);
        pitch = pitch + f1 * 0.1f;
        yaw += f * 0.1f;
        f1 = f1 * 0.9f;
        f = f * 0.75f;
        {
            const float a = random.nextFloat();
            const float b = random.nextFloat();
            const float c = random.nextFloat();
            f1 = f1 + (a - b) * c * 2.0f;
        }
        {
            const float a = random.nextFloat();
            const float b = random.nextFloat();
            const float c = random.nextFloat();
            f = f + (a - b) * c * 4.0f;
        }
        if (j == i && thickness > 1.0f) {
            const int64_t seed1 = random.nextLong();
            const float thickness1 = random.nextFloat() * 0.5f + 0.5f;
            createTunnel(context, configuration, chunk, biomeGetter, seed1, aquifer, posX, posY, posZ,
                         horizMult, vertMult, thickness1, yaw - PI_F / 2.0f, pitch / 3.0f, j, branchCount,
                         1.0, mask, checker);
            const int64_t seed2 = random.nextLong();
            const float thickness2 = random.nextFloat() * 0.5f + 0.5f;
            createTunnel(context, configuration, chunk, biomeGetter, seed2, aquifer, posX, posY, posZ,
                         horizMult, vertMult, thickness2, yaw + PI_F / 2.0f, pitch / 3.0f, j, branchCount,
                         1.0, mask, checker);
            return;
        }

        if (random.nextInt(4) != 0) {
            if (!canReach(chunk->getPos(), posX, posZ, j, branchCount, thickness)) {
                return;
            }

            // Additional size-boosting to make wider & taller spherical rooms
            // (short-circuit: the nextInt(48) only above minY + 12).
            const bool shouldEnlargeSphere = posY > static_cast<double>(minY + 12) && random.nextInt(48) == 0;
            float sizeMultiplier = 1.0f;
            if (shouldEnlargeSphere) {
                const float a = random.nextFloat();
                const float b = random.nextFloat();
                sizeMultiplier = a * b * 2.0f + 1.0f;
            }

            const double sphereHRadius = std::min(horizontalRadius * horizMult * sizeMultiplier, 10.0);
            const double sphereVRadius = verticalRadius * vertMult * sizeMultiplier;
            // If side-boosting is applied, squish the sphere's edge-steeped floor into a dish
            const double sphereVRadiusLimited = shouldEnlargeSphere
                ? std::min(sphereVRadius, sphereHRadius * static_cast<double>(0.65f))
                : sphereVRadius;

            carveEllipsoid(context, configuration, chunk, biomeGetter, aquifer, posX, posY, posZ,
                           sphereHRadius, sphereVRadiusLimited, mask, checker);
        }
    }
}

bool TwilightCavesCarver::canReplace(const CaveCarverConfiguration& configuration,
                                     const BlockState* state) const {
    // !state.is(BlockTags.ICE) && !fluid.is(WATER) && super.canReplaceBlock
    if (!state) return false;
    if (iceTag().count(state->getBlockName()) > 0) return false;
    if (state->hasWaterFluid()) return false;
    return canReplaceBlock(configuration, state);
}

bool TwilightCavesCarver::carveBlock(CarvingContext& context, const CaveCarverConfiguration& configuration,
                                     ::world::IChunk* chunk, std::function<void*(const core::BlockPos&)> biomeGetter,
                                     CarvingMask& mask, core::BlockPos::MutableBlockPos& blockPos,
                                     core::BlockPos::MutableBlockPos& helperPos, density::Aquifer* aquifer,
                                     bool& isSurface) {
    (void)mask;
    (void)helperPos;
    (void)aquifer;
    const core::BlockPos pos(blockPos.getX(), blockPos.getY(), blockPos.getZ());
    BlockState* stateBeforeReplacement = chunk->getBlockState(pos);
    if (!stateBeforeReplacement) return false;
    const std::string& name = stateBeforeReplacement->getBlockName();
    if (name == "minecraft:grass_block" || name == "minecraft:mycelium"
        || name == "minecraft:podzol" || name == "minecraft:dirt_path") {
        isSurface = true;
    }

    // We dont want caves to go so far down you can see bedrock
    if (pos.getY() < chunk->getMinBuildHeight() + 6) return false;

    if (!canReplace(configuration, stateBeforeReplacement)) return false;

    const core::BlockPos chunkOrigin = chunk->getPos().getWorldPosition();
    for (const auto& d : DIRECTIONS) {
        const core::BlockPos relative(pos.getX() + d[0], pos.getY() + d[1], pos.getZ() + d[2]);
        if (isInsideChunk(relative, chunkOrigin)) {
            BlockState* neighbour = chunk->getBlockState(relative);
            if (neighbour && neighbour->hasWaterFluid()) {
                // Replacing this block would expose neighbouring water.
                return false;
            }
        }
    }

    // getCarveState: always cave_air (no aquifer, no lava level).
    BlockState* blockStateToPlace = Blocks::CAVE_AIR->defaultBlockState();
    random::AnyRandomSource randomFromPos =
        context.randomState()->getOrCreateRandomFactory("minecraft:ore")->at(pos.getX(), pos.getY(), pos.getZ());

    // Sand doesn't quite generate until after the carvers, so look for liquid
    // above possible sand instead.
    BlockState* aboveTwo = chunk->getBlockState(core::BlockPos(pos.getX(), pos.getY() + 2, pos.getZ()));
    if (aboveTwo && aboveTwo->hasAnyFluid()) {
        blockStateToPlace = randomFromPos.nextBoolean()
            ? Blocks::ROOTED_DIRT->defaultBlockState()
            : Blocks::getDefaultState("minecraft:coarse_dirt");
        if (!blockStateToPlace) blockStateToPlace = Blocks::DIRT->defaultBlockState();
    }

    const bool blockPlaced = chunk->setBlockState(pos, blockStateToPlace, false) != nullptr;
    // aquifer.shouldScheduleFluidUpdate() && fluid state: never — every state
    // placed here is fluid-free.

    if (isSurface) {
        const core::BlockPos posDown(pos.getX(), pos.getY() - 1, pos.getZ());
        BlockState* below = chunk->getBlockState(posDown);
        if (below && below->is(Blocks::DIRT)) {
            BlockState* top = context.topMaterial(biomeGetter, chunk, posDown, blockStateToPlace->hasAnyFluid());
            if (top) chunk->setBlockState(posDown, top, false);
        }
    }

    if (blockPlaced) postCarveBlock(chunk, pos, configuration, randomFromPos, chunkOrigin);
    return blockPlaced;
}

void TwilightCavesCarver::postCarveBlock(::world::IChunk* chunk, const core::BlockPos& pos,
                                         const CaveCarverConfiguration& configuration,
                                         random::AnyRandomSource& random, const core::BlockPos& chunkOrigin) {
    for (int dir = 0; dir < 6; ++dir) {
        const int* d = DIRECTIONS[dir];
        const core::BlockPos relative(pos.getX() + d[0], pos.getY() + d[1], pos.getZ() + d[2]);
        if (!isInsideChunk(relative, chunkOrigin)) continue;

        if (m_isHighlands) {
            if (random.nextInt(4) == 0 && canReplace(configuration, chunk->getBlockState(relative))) {
                chunk->setBlockState(relative, m_wallBlocks(random, relative), false);
            }
            continue;
        }

        const bool isDown = dir == 0;
        const bool isUp = dir == 1;
        if (isDown) continue;   // dirt is never placed below
        bool roof = isUp;
        if (!roof) {
            BlockState* aboveRelative = chunk->getBlockState(
                core::BlockPos(relative.getX(), relative.getY() + 1, relative.getZ()));
            roof = (aboveRelative && aboveRelative->isAir())
                || checkNoiseThreshold(relative, static_cast<double>(0.25f), static_cast<double>(0.5f));
        }
        if (!roof) continue;

        BlockState* neighbouringBlock = chunk->getBlockState(relative);
        if (neighbouringBlock
            && (baseStoneOverworldTag().count(neighbouringBlock->getBlockName()) > 0
                || neighbouringBlock->hasWaterFluid())) {
            chunk->setBlockState(relative, m_wallBlocks(random, relative), false);
        }
    }
}

bool TwilightCavesCarver::checkNoiseThreshold(const core::BlockPos& pos, double posScalar,
                                              double threshold) const {
    const double noise = m_noise->noise(pos.getX() * posScalar, pos.getY() * posScalar,
                                        pos.getZ() * posScalar);
    // Noise outputs values between -1 to 1, normalized with n * 0.5 + 0.5
    return noise * 0.5 + 0.5 > threshold;
}

} // namespace carver
} // namespace levelgen
} // namespace minecraft
