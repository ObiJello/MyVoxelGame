#pragma once

#include "levelgen/carver/WorldCarver.h"
#include "levelgen/carver/CarverConfiguration.h"
#include "levelgen/feature/stateproviders/BlockStateProvider.h"
#include "synth/ImprovedNoise.h"
#include "random/AnyPositionalRandomFactory.h"
#include "random/LegacyRandomSource.h"
#include "random/XoroshiroRandomSource.h"
#include "core/BlockPos.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// The Twilight Forest's cave carvers — world/components/TFCavesCarver.java,
// registered in init/TFCaveCarvers.java as twilightforest:tf_caves (the
// ordinary TF underground: few, wide caves roofed with dirt) and
// twilightforest:highland_caves (one cave per landmark-grid chunk under the
// highlands, walled with trollsteinn / stone). Both take a vanilla
// CaveCarverConfiguration (worldgen/configured_carver/{tf_caves,
// highland_caves}.json) but carve with their own tunnel walk, room shape and
// carveBlock: cave_air instead of the aquifer's substance, no carving next to
// water, a y > minY + 6 floor, and a post-carve pass that lines the cave with
// the wall provider.

namespace minecraft {
namespace levelgen {
namespace carver {

class TwilightCavesCarver : public WorldCarver<CaveCarverConfiguration> {
public:
    /**
     * CarverWallProvider: TF's NoiseCarverWallProvider for tf_caves (the
     * noise picks the state, the random is unused), a weighted list for
     * highland_caves (the random picks it).
     */
    using WallProvider = std::function<BlockState*(random::AnyRandomSource& random, const core::BlockPos& pos)>;

    TwilightCavesCarver(bool isHighlands, WallProvider wallBlocks);

    /** TFCaveCarvers.TF_CAVES: NoiseCarverWallProvider(6972119253061020355, (0, [1.0]), 0.5, dirt mix). */
    static std::unique_ptr<TwilightCavesCarver> createTwilightCaves();
    /** TFCaveCarvers.HIGHLAND_CAVES: weighted trollsteinn 1 / stone 3. */
    static std::unique_ptr<TwilightCavesCarver> createHighlandCaves();

    bool isStartChunk(const CaveCarverConfiguration& configuration, XoroshiroRandomSource& random) override;
    bool isStartChunk(const CaveCarverConfiguration& configuration, LegacyRandomSource& random) override;

    bool carve(
        CarvingContext& context,
        const CaveCarverConfiguration& configuration,
        ::world::IChunk* chunk,
        std::function<void*(const core::BlockPos&)> biomeGetter,
        XoroshiroRandomSource& random,
        density::Aquifer* aquifer,
        const ::world::ChunkPos& sourceChunkPos,
        CarvingMask& mask
    ) override;

    bool carve(
        CarvingContext& context,
        const CaveCarverConfiguration& configuration,
        ::world::IChunk* chunk,
        std::function<void*(const core::BlockPos&)> biomeGetter,
        LegacyRandomSource& random,
        density::Aquifer* aquifer,
        const ::world::ChunkPos& sourceChunkPos,
        CarvingMask& mask
    ) override;

protected:
    bool carveBlock(
        CarvingContext& context,
        const CaveCarverConfiguration& configuration,
        ::world::IChunk* chunk,
        std::function<void*(const core::BlockPos&)> biomeGetter,
        CarvingMask& mask,
        core::BlockPos::MutableBlockPos& blockPos,
        core::BlockPos::MutableBlockPos& helperPos,
        density::Aquifer* aquifer,
        bool& isSurface
    ) override;

private:
    template<typename R>
    bool carveImpl(CarvingContext& context, const CaveCarverConfiguration& configuration,
                   ::world::IChunk* chunk, std::function<void*(const core::BlockPos&)> biomeGetter,
                   R& random, density::Aquifer* aquifer, const ::world::ChunkPos& sourceChunkPos,
                   CarvingMask& mask);

    template<typename R>
    static float getThickness(R& random);

    void createRoom(CarvingContext& context, const CaveCarverConfiguration& configuration,
                    ::world::IChunk* chunk, std::function<void*(const core::BlockPos&)> biomeGetter,
                    density::Aquifer* aquifer, double x, double y, double z, float radius,
                    double horizToVertRatio, CarvingMask& mask, CarveSkipChecker checker);

    void createTunnel(CarvingContext& context, const CaveCarverConfiguration& configuration,
                      ::world::IChunk* chunk, std::function<void*(const core::BlockPos&)> biomeGetter,
                      int64_t seed, density::Aquifer* aquifer, double posX, double posY, double posZ,
                      double horizMult, double vertMult, float thickness, float yaw, float pitch,
                      int32_t branchIndex, int32_t branchCount, double horizToVertRatio,
                      CarvingMask& mask, CarveSkipChecker checker);

    bool canReplace(const CaveCarverConfiguration& configuration, const BlockState* state) const;
    void postCarveBlock(::world::IChunk* chunk, const core::BlockPos& pos,
                        const CaveCarverConfiguration& configuration,
                        random::AnyRandomSource& random, const core::BlockPos& chunkOrigin);
    bool checkNoiseThreshold(const core::BlockPos& pos, double posScalar, double threshold) const;

    bool m_isHighlands;
    WallProvider m_wallBlocks;
    // new ImprovedNoise(new LegacyRandomSource(6972119253061020355L)): the
    // dirt-roof noise. ImprovedNoise::noise is non-const but pure.
    mutable std::unique_ptr<ImprovedNoise> m_noise;
};

/**
 * LegacyLandmarkPlacements.getNearestCenterXZ(chunkX, chunkZ) (block X/Z of
 * the nearest landmark centre on TF's 256-block grid) and
 * manhattanDistanceFromLandmarkCenter (in chunks). Used by highland_caves.
 */
core::BlockPos twilightNearestLandmarkCenter(int32_t chunkX, int32_t chunkZ);
int32_t twilightManhattanDistanceFromLandmarkCenter(int32_t chunkX, int32_t chunkZ);

} // namespace carver
} // namespace levelgen
} // namespace minecraft
