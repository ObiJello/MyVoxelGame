import net.minecraft.SharedConstants;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Holder;
import net.minecraft.core.HolderGetter;
import net.minecraft.core.MappedRegistry;
import net.minecraft.core.RegistrationInfo;
import net.minecraft.core.registries.Registries;
import net.minecraft.server.Bootstrap;
import net.minecraft.data.registries.VanillaRegistries;
import net.minecraft.core.HolderLookup;
import net.minecraft.world.level.ChunkPos;
import net.minecraft.world.level.biome.Biome;
import net.minecraft.world.level.biome.BiomeSource;
import net.minecraft.world.level.biome.MultiNoiseBiomeSource;
import net.minecraft.world.level.biome.MultiNoiseBiomeSourceParameterList;
import net.minecraft.world.level.biome.MultiNoiseBiomeSourceParameterLists;
import net.minecraft.world.level.chunk.PalettedContainerFactory;
import net.minecraft.world.level.chunk.PalettedContainer;
import net.minecraft.world.level.chunk.PalettedContainerRO;
import net.minecraft.world.level.chunk.Strategy;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.biome.Biomes;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.chunk.ChunkAccess;
import net.minecraft.world.level.chunk.ProtoChunk;
import net.minecraft.world.level.chunk.UpgradeData;
import net.minecraft.world.level.levelgen.NoiseBasedChunkGenerator;
import net.minecraft.world.level.levelgen.NoiseGeneratorSettings;
import net.minecraft.world.level.levelgen.RandomState;
import net.minecraft.world.level.levelgen.blending.Blender;
import net.minecraft.world.level.levelgen.Beardifier;
import net.minecraft.world.level.levelgen.NoiseChunk;
import net.minecraft.world.level.levelgen.Aquifer;
import java.lang.reflect.Field;
import net.minecraft.world.level.levelgen.synth.NormalNoise;
import net.minecraft.resources.ResourceKey;
import net.minecraft.world.level.LevelHeightAccessor;
import com.mojang.serialization.Lifecycle;
import com.mojang.serialization.Codec;

import java.io.*;
import java.util.*;
import java.util.concurrent.CompletableFuture;

/**
 * Java Multi-Chunk Phase Noise Test
 *
 * Tests noise phase generation across multiple chunk positions including:
 * - Positive coordinates
 * - Negative coordinates
 * - Origin and near-origin
 * - Large coordinates
 *
 * Output format matches C++ for comparison.
 */
public class MultiChunkPhaseNoiseTest {

    // Test parameters - must match C++ test
    static final long SEED = 12345L;
    static final int MIN_Y = -64;
    static final int HEIGHT = 384;
    static final int MAX_Y = MIN_Y + HEIGHT;

    // Chunk positions to test - same as C++ test
    static final int[][] TEST_CHUNKS = {
        // Origin and near-origin
        {0, 0},
        {1, 0},
        {0, 1},
        {1, 1},

        // Negative coordinates
        {-1, 0},
        {0, -1},
        {-1, -1},
        {-2, -2},
        {-5, -5},
        {-10, -10},

        // Mixed positive/negative
        {-1, 1},
        {1, -1},
        {-3, 5},
        {5, -3},

        // Larger coordinates
        {10, 10},
        {-10, 10},
        {10, -10},
        {-10, -10},
        {100, 100},
        {-100, -100},
        {-100, 100},
        {100, -100},
    };

    static class ChunkStats {
        int chunkX, chunkZ;
        int airCount, stoneCount, waterCount, lavaCount, otherCount;
        long contentHash;
    }

    // Simple LevelHeightAccessor implementation
    static class SimpleLevelHeight implements LevelHeightAccessor {
        private final int minY;
        private final int height;

        SimpleLevelHeight(int minY, int height) {
            this.minY = minY;
            this.height = height;
        }

        @Override
        public int getHeight() {
            return height;
        }

        @Override
        public int getMinY() {
            return minY;
        }
    }

    static MappedRegistry<Biome> createBiomeRegistry(HolderLookup.RegistryLookup<Biome> lookup) {
        MappedRegistry<Biome> registry = new MappedRegistry<>(Registries.BIOME, Lifecycle.stable());
        lookup.listElements().forEach(holder -> {
            ResourceKey<Biome> key = holder.key();
            Biome value = holder.value();
            registry.register(key, value, RegistrationInfo.BUILT_IN);
        });
        registry.freeze();
        return registry;
    }

    public static void main(String[] args) {
        try {
            String outputPath = args.length > 0 ? args[0] : "java_multi_chunk_output.txt";

            System.out.println("=== Java Multi-Chunk Phase Noise Test ===");
            System.out.println("Seed: " + SEED);
            System.out.println("Testing " + TEST_CHUNKS.length + " chunks");
            System.out.println("Y range: " + MIN_Y + " to " + MAX_Y);
            System.out.println();

            // Initialize Minecraft's systems
            System.out.println("Bootstrapping Minecraft...");
            SharedConstants.tryDetectVersion();
            Bootstrap.bootStrap();
            System.out.println("Bootstrap complete.");

            // Get complete registry with all worldgen registries populated
            System.out.println("Setting up registries...");
            HolderLookup.Provider registries = VanillaRegistries.createLookup();

            // Get noise settings for overworld
            HolderGetter<NoiseGeneratorSettings> noiseSettingsGetter =
                registries.lookupOrThrow(Registries.NOISE_SETTINGS);
            Holder<NoiseGeneratorSettings> overworldSettings =
                noiseSettingsGetter.getOrThrow(NoiseGeneratorSettings.OVERWORLD);

            // Get biome registry lookup
            HolderLookup.RegistryLookup<Biome> biomeLookup =
                registries.lookupOrThrow(Registries.BIOME);

            // Get noise parameter list for overworld biomes
            HolderGetter<MultiNoiseBiomeSourceParameterList> paramListGetter =
                registries.lookupOrThrow(Registries.MULTI_NOISE_BIOME_SOURCE_PARAMETER_LIST);
            Holder<MultiNoiseBiomeSourceParameterList> overworldParams =
                paramListGetter.getOrThrow(MultiNoiseBiomeSourceParameterLists.OVERWORLD);

            // Create biome source
            BiomeSource biomeSource = MultiNoiseBiomeSource.createFromPreset(overworldParams);

            // Create noise-based chunk generator
            System.out.println("Creating chunk generator...");
            NoiseBasedChunkGenerator generator = new NoiseBasedChunkGenerator(
                biomeSource,
                overworldSettings
            );

            // Create random state from seed
            HolderGetter<NormalNoise.NoiseParameters> noiseParamsGetter =
                registries.lookupOrThrow(Registries.NOISE);
            RandomState randomState = RandomState.create(
                overworldSettings.value(),
                noiseParamsGetter,
                SEED
            );

            // Create level height accessor
            LevelHeightAccessor levelHeight = new SimpleLevelHeight(MIN_Y, HEIGHT);

            // Create biome registry and PalettedContainerFactory
            MappedRegistry<Biome> biomeRegistry = createBiomeRegistry(biomeLookup);
            Strategy<BlockState> blockStateStrategy = Strategy.<BlockState>createForBlockStates(Block.BLOCK_STATE_REGISTRY);
            BlockState defaultBlockState = Blocks.AIR.defaultBlockState();
            Strategy<Holder<Biome>> biomeStrategy = Strategy.<Holder<Biome>>createForBiomes(biomeRegistry.asHolderIdMap());
            Holder.Reference<Biome> defaultBiome = biomeRegistry.getOrThrow(Biomes.PLAINS);
            Codec<PalettedContainer<BlockState>> blockStateCodec =
                PalettedContainer.codecRW(BlockState.CODEC, blockStateStrategy, defaultBlockState);
            Codec<PalettedContainerRO<Holder<Biome>>> biomeCodec =
                PalettedContainer.codecRO(biomeRegistry.holderByNameCodec(), biomeStrategy, defaultBiome);
            PalettedContainerFactory containerFactory = new PalettedContainerFactory(
                blockStateStrategy,
                defaultBlockState,
                blockStateCodec,
                biomeStrategy,
                defaultBiome,
                biomeCodec
            );

            // Get noise settings for fluid picker
            NoiseGeneratorSettings noiseSettings = overworldSettings.value();
            Aquifer.FluidStatus lavaStatus = new Aquifer.FluidStatus(-54, Blocks.LAVA.defaultBlockState());
            int seaLevel = noiseSettings.seaLevel();
            Aquifer.FluidStatus seaStatus = new Aquifer.FluidStatus(seaLevel, noiseSettings.defaultFluid());
            Aquifer.FluidPicker globalFluidPicker = (x, y, z) -> y < Math.min(-54, seaLevel) ? lavaStatus : seaStatus;

            System.out.println("Setup complete. Generating chunks...");
            System.out.println();

            // Open output file
            PrintWriter out = new PrintWriter(new FileWriter(outputPath));
            out.println("# Java Multi-Chunk Phase Noise Output");
            out.println("# Seed: " + SEED);
            out.println("# Phase: NOISE ONLY");
            out.println("# Format: chunkX,chunkZ,air,stone,water,lava,other,hash");
            out.println();

            List<ChunkStats> allStats = new ArrayList<>();

            for (int[] pos : TEST_CHUNKS) {
                int chunkX = pos[0];
                int chunkZ = pos[1];

                System.out.print("Generating chunk (" + chunkX + ", " + chunkZ + ")...");

                ChunkStats stats = generateAndAnalyzeChunk(
                    generator, randomState, levelHeight, containerFactory,
                    noiseSettings, globalFluidPicker, chunkX, chunkZ
                );
                allStats.add(stats);

                System.out.println(" done. Air: " + stats.airCount +
                    ", Stone: " + stats.stoneCount +
                    ", Water: " + stats.waterCount +
                    ", Hash: " + Long.toHexString(stats.contentHash));

                // Write to file
                out.println(stats.chunkX + "," + stats.chunkZ + "," +
                    stats.airCount + "," + stats.stoneCount + "," +
                    stats.waterCount + "," + stats.lavaCount + "," +
                    stats.otherCount + "," +
                    Long.toHexString(stats.contentHash));
            }

            out.println();
            out.println("# Summary:");
            out.println("# Total chunks tested: " + allStats.size());
            out.close();

            // Print summary
            System.out.println();
            System.out.println("=== Summary ===");
            System.out.printf("%10s%10s%10s%10s%10s%10s%18s%n",
                "ChunkX", "ChunkZ", "Air", "Stone", "Water", "Lava", "Hash");
            System.out.println("-".repeat(78));

            for (ChunkStats stats : allStats) {
                System.out.printf("%10d%10d%10d%10d%10d%10d%18s%n",
                    stats.chunkX, stats.chunkZ,
                    stats.airCount, stats.stoneCount,
                    stats.waterCount, stats.lavaCount,
                    Long.toHexString(stats.contentHash));
            }

            System.out.println();
            System.out.println("Output written to: " + outputPath);
            System.out.println("Compare with C++ MultiChunkPhaseNoiseTest output for parity check.");

        } catch (Exception e) {
            System.err.println("Error during chunk generation:");
            e.printStackTrace();
            System.exit(1);
        }
    }

    private static ChunkStats generateAndAnalyzeChunk(
        NoiseBasedChunkGenerator generator,
        RandomState randomState,
        LevelHeightAccessor levelHeight,
        PalettedContainerFactory containerFactory,
        NoiseGeneratorSettings noiseSettings,
        Aquifer.FluidPicker globalFluidPicker,
        int chunkX, int chunkZ
    ) throws Exception {
        ChunkStats stats = new ChunkStats();
        stats.chunkX = chunkX;
        stats.chunkZ = chunkZ;

        ChunkPos chunkPos = new ChunkPos(chunkX, chunkZ);

        // Create ProtoChunk
        ProtoChunk chunk = new ProtoChunk(
            chunkPos,
            UpgradeData.EMPTY,
            levelHeight,
            containerFactory,
            null   // BlendingData
        );

        // Pre-create the NoiseChunk with Beardifier.EMPTY
        NoiseChunk noiseChunk = NoiseChunk.forChunk(
            chunk,
            randomState,
            Beardifier.EMPTY,
            noiseSettings,
            globalFluidPicker,
            Blender.empty()
        );

        // Use reflection to set the noiseChunk field on the chunk
        Field noiseChunkField = ChunkAccess.class.getDeclaredField("noiseChunk");
        noiseChunkField.setAccessible(true);
        noiseChunkField.set(chunk, noiseChunk);

        // Fill from noise (NOISE PHASE ONLY - no biomes, no surface)
        CompletableFuture<ChunkAccess> noiseFuture = generator.fillFromNoise(
            Blender.empty(),
            randomState,
            null,  // StructureManager - not needed since NoiseChunk already created
            chunk
        );
        noiseFuture.join();

        // Count blocks
        int startX = chunkPos.getMinBlockX();
        int startZ = chunkPos.getMinBlockZ();

        for (int y = MIN_Y; y < MAX_Y; y++) {
            for (int x = 0; x < 16; x++) {
                for (int z = 0; z < 16; z++) {
                    BlockState blockState = chunk.getBlockState(new BlockPos(startX + x, y, startZ + z));
                    Block block = blockState.getBlock();

                    if (block == Blocks.AIR || blockState.isAir()) {
                        stats.airCount++;
                    } else if (block == Blocks.STONE) {
                        stats.stoneCount++;
                    } else if (block == Blocks.WATER) {
                        stats.waterCount++;
                    } else if (block == Blocks.LAVA) {
                        stats.lavaCount++;
                    } else {
                        stats.otherCount++;
                    }
                }
            }
        }

        // Compute content hash (same formula as C++)
        stats.contentHash = ((long)stats.airCount * 31) ^
                           ((long)stats.stoneCount * 37) ^
                           ((long)stats.waterCount * 41) ^
                           ((long)stats.lavaCount * 43) ^
                           ((long)chunkX * 53) ^
                           ((long)chunkZ * 59);

        return stats;
    }
}
