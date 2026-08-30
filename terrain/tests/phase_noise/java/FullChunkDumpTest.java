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
import net.minecraft.world.level.biome.Biomes;
import net.minecraft.world.level.chunk.PalettedContainerFactory;
import net.minecraft.world.level.chunk.PalettedContainer;
import net.minecraft.world.level.chunk.PalettedContainerRO;
import net.minecraft.world.level.chunk.Strategy;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.chunk.ProtoChunk;
import net.minecraft.world.level.chunk.UpgradeData;
import net.minecraft.world.level.levelgen.NoiseGeneratorSettings;
import net.minecraft.world.level.levelgen.RandomState;
import net.minecraft.world.level.levelgen.blending.Blender;
import net.minecraft.world.level.levelgen.Beardifier;
import net.minecraft.world.level.levelgen.NoiseChunk;
import net.minecraft.world.level.levelgen.NoiseSettings;
import net.minecraft.world.level.levelgen.Aquifer;
import net.minecraft.world.level.LevelHeightAccessor;
import net.minecraft.world.level.levelgen.synth.NormalNoise;
import net.minecraft.resources.ResourceKey;
import com.mojang.serialization.Lifecycle;
import com.mojang.serialization.Codec;

import java.io.*;
import java.lang.reflect.*;
import java.util.*;

/**
 * Full Chunk Dump Test
 * Runs the complete fillFromNoise process and dumps block data for comparison with C++.
 * This uses the actual Minecraft terrain generation loop.
 */
public class FullChunkDumpTest {

    static final long SEED = 12345L;
    static final int MIN_Y = -64;
    static final int HEIGHT = 384;
    static final int MAX_Y = MIN_Y + HEIGHT;

    static class SimpleLevelHeight implements LevelHeightAccessor {
        private final int minY;
        private final int height;

        SimpleLevelHeight(int minY, int height) {
            this.minY = minY;
            this.height = height;
        }

        @Override
        public int getHeight() { return height; }

        @Override
        public int getMinY() { return minY; }
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
            int testChunkX = args.length > 0 ? Integer.parseInt(args[0]) : 0;
            int testChunkZ = args.length > 1 ? Integer.parseInt(args[1]) : 0;
            String outputFile = args.length > 2 ? args[2] : "java_chunk_" + testChunkX + "_" + testChunkZ + ".txt";

            System.out.println("=== Java Full Chunk Dump Test ===");
            System.out.println("Seed: " + SEED);
            System.out.println("Chunk: (" + testChunkX + ", " + testChunkZ + ")");
            System.out.println("Output: " + outputFile);
            System.out.println();

            // Initialize Minecraft
            SharedConstants.tryDetectVersion();
            Bootstrap.bootStrap();

            // Get registries
            HolderLookup.Provider registries = VanillaRegistries.createLookup();

            HolderGetter<NoiseGeneratorSettings> noiseSettingsGetter =
                registries.lookupOrThrow(Registries.NOISE_SETTINGS);
            Holder<NoiseGeneratorSettings> overworldSettingsHolder =
                noiseSettingsGetter.getOrThrow(NoiseGeneratorSettings.OVERWORLD);
            NoiseGeneratorSettings noiseGenSettings = overworldSettingsHolder.value();

            HolderLookup.RegistryLookup<Biome> biomeLookup =
                registries.lookupOrThrow(Registries.BIOME);

            HolderGetter<NormalNoise.NoiseParameters> noiseParamsGetter =
                registries.lookupOrThrow(Registries.NOISE);
            RandomState randomState = RandomState.create(
                noiseGenSettings,
                noiseParamsGetter,
                SEED
            );

            // Create biome registry and PalettedContainerFactory
            MappedRegistry<Biome> biomeRegistry = createBiomeRegistry(biomeLookup);
            Strategy<BlockState> blockStateStrategy = Strategy.<BlockState>createForBlockStates(Block.BLOCK_STATE_REGISTRY);
            Strategy<Holder<Biome>> biomeStrategy = Strategy.<Holder<Biome>>createForBiomes(biomeRegistry.asHolderIdMap());
            Holder.Reference<Biome> defaultBiome = biomeRegistry.getOrThrow(Biomes.PLAINS);
            Codec<PalettedContainer<BlockState>> blockStateCodec =
                PalettedContainer.codecRW(BlockState.CODEC, blockStateStrategy, Blocks.AIR.defaultBlockState());
            Codec<PalettedContainerRO<Holder<Biome>>> biomeCodec =
                PalettedContainer.codecRO(biomeRegistry.holderByNameCodec(), biomeStrategy, defaultBiome);
            PalettedContainerFactory containerFactory = new PalettedContainerFactory(
                blockStateStrategy, Blocks.AIR.defaultBlockState(), blockStateCodec,
                biomeStrategy, defaultBiome, biomeCodec
            );

            // Create fluid picker
            Aquifer.FluidStatus lavaStatus = new Aquifer.FluidStatus(-54, Blocks.LAVA.defaultBlockState());
            int seaLevel = noiseGenSettings.seaLevel();
            Aquifer.FluidStatus seaStatus = new Aquifer.FluidStatus(seaLevel, noiseGenSettings.defaultFluid());
            Aquifer.FluidPicker globalFluidPicker = (x, y, z) -> y < Math.min(-54, seaLevel) ? lavaStatus : seaStatus;

            LevelHeightAccessor levelHeight = new SimpleLevelHeight(MIN_Y, HEIGHT);

            // Create ProtoChunk
            ChunkPos chunkPos = new ChunkPos(testChunkX, testChunkZ);
            ProtoChunk chunk = new ProtoChunk(chunkPos, UpgradeData.EMPTY, levelHeight, containerFactory, null);

            // Create NoiseChunk
            NoiseChunk noiseChunk = NoiseChunk.forChunk(
                chunk,
                randomState,
                Beardifier.EMPTY,
                noiseGenSettings,
                globalFluidPicker,
                Blender.empty()
            );

            System.out.println("Running fillFromNoise...");

            // Get noise settings for cell dimensions
            NoiseSettings noiseSettings = noiseGenSettings.noiseSettings();
            int cellWidth = noiseSettings.getCellWidth();
            int cellHeight = noiseSettings.getCellHeight();
            int cellCountXZ = 16 / cellWidth;  // Usually 4
            int cellCountY = noiseSettings.height() / cellHeight;  // Usually 48
            int cellNoiseMinY = noiseSettings.minY() / cellHeight;  // Usually -8

            int minBlockX = chunkPos.getMinBlockX();
            int minBlockZ = chunkPos.getMinBlockZ();

            // Get the protected getInterpolatedState method
            Method getStateMethod = NoiseChunk.class.getDeclaredMethod("getInterpolatedState");
            getStateMethod.setAccessible(true);

            // Store block data
            Map<String, BlockState> blockData = new HashMap<>();

            // Run the actual fillFromNoise loop (Reference: NoiseBasedChunkGenerator.fillFromNoise)
            noiseChunk.initializeForFirstCellX();

            for (int cellXIndex = 0; cellXIndex < cellCountXZ; cellXIndex++) {
                noiseChunk.advanceCellX(cellXIndex);

                for (int cellZIndex = 0; cellZIndex < cellCountXZ; cellZIndex++) {
                    // Fill from top to bottom (for heightmap calculation in real Minecraft)
                    for (int cellYIndex = cellCountY - 1; cellYIndex >= 0; cellYIndex--) {
                        noiseChunk.selectCellYZ(cellYIndex, cellZIndex);

                        for (int dy = cellHeight - 1; dy >= 0; dy--) {
                            int blockY = (cellYIndex + cellNoiseMinY) * cellHeight + dy;
                            double factorY = (double) dy / (double) cellHeight;
                            noiseChunk.updateForY(blockY, factorY);

                            for (int dx = 0; dx < cellWidth; dx++) {
                                int blockX = minBlockX + cellXIndex * cellWidth + dx;
                                double factorX = (double) dx / (double) cellWidth;
                                noiseChunk.updateForX(blockX, factorX);

                                for (int dz = 0; dz < cellWidth; dz++) {
                                    int blockZ = minBlockZ + cellZIndex * cellWidth + dz;
                                    double factorZ = (double) dz / (double) cellWidth;
                                    noiseChunk.updateForZ(blockZ, factorZ);

                                    BlockState state = (BlockState) getStateMethod.invoke(noiseChunk);

                                    // Store block data
                                    String key = (blockX - minBlockX) + "," + blockY + "," + (blockZ - minBlockZ);
                                    blockData.put(key, state);
                                }
                            }
                        }
                    }
                }

                noiseChunk.swapSlices();
            }

            noiseChunk.stopInterpolation();

            System.out.println("fillFromNoise complete. Writing output...");

            // Write output file
            try (PrintWriter writer = new PrintWriter(new FileWriter(outputFile))) {
                writer.println("# Java Chunk Output (fillFromNoise only)");
                writer.println("# Seed: " + SEED);
                writer.println("# Chunk: (" + testChunkX + ", " + testChunkZ + ")");
                writer.println("# Format: x,y,z,block_name");
                writer.println();

                int blockCount = 0;
                int airCount = 0;
                Map<String, Integer> blockCounts = new HashMap<>();

                for (int y = MIN_Y; y < MAX_Y; y++) {
                    for (int x = 0; x < 16; x++) {
                        for (int z = 0; z < 16; z++) {
                            String key = x + "," + y + "," + z;
                            BlockState state = blockData.get(key);
                            String blockName = (state == null) ? "minecraft:air" : state.getBlock().toString();
                            // Clean up block name format (Block{minecraft:stone} -> minecraft:stone)
                            if (blockName.startsWith("Block{") && blockName.endsWith("}")) {
                                blockName = blockName.substring(6, blockName.length() - 1);
                            }

                            writer.println(x + "," + y + "," + z + "," + blockName);

                            blockCount++;
                            if (blockName.equals("minecraft:air")) {
                                airCount++;
                            }
                            blockCounts.merge(blockName, 1, Integer::sum);
                        }
                    }
                }

                writer.println();
                writer.println("# Summary:");
                writer.println("# Total blocks: " + blockCount);
                writer.println("# Air blocks: " + airCount);
                writer.println("# Non-air blocks: " + (blockCount - airCount));
                writer.println("# Block types:");

                blockCounts.entrySet().stream()
                    .sorted((a, b) -> b.getValue().compareTo(a.getValue()))
                    .forEach(e -> writer.println("#   " + e.getKey() + ": " + e.getValue()));
            }

            System.out.println("Output written to: " + outputFile);
            System.out.println();

            // Print summary to console
            int stoneCount = 0;
            int waterCount = 0;
            int airCount = 0;
            for (Map.Entry<String, BlockState> entry : blockData.entrySet()) {
                BlockState state = entry.getValue();
                if (state == null) {
                    airCount++;
                } else {
                    String name = state.getBlock().toString();
                    if (name.contains("stone")) stoneCount++;
                    else if (name.contains("water")) waterCount++;
                    else if (name.contains("air")) airCount++;
                }
            }

            System.out.println("Summary:");
            System.out.println("  Stone-like blocks: " + stoneCount);
            System.out.println("  Water blocks: " + waterCount);
            System.out.println("  Air blocks: " + airCount);
            System.out.println();
            System.out.println("Done.");

        } catch (Exception e) {
            e.printStackTrace();
            System.exit(1);
        }
    }
}
