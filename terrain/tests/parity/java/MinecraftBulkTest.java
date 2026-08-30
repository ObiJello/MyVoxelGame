import net.minecraft.SharedConstants;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Holder;
import net.minecraft.core.HolderGetter;
import net.minecraft.core.MappedRegistry;
import net.minecraft.core.QuartPos;
import net.minecraft.core.Registry;
import net.minecraft.core.RegistrationInfo;
import net.minecraft.core.RegistryAccess;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.core.registries.Registries;
import net.minecraft.server.Bootstrap;
import net.minecraft.server.packs.*;
import net.minecraft.server.packs.repository.ServerPacksSource;
import net.minecraft.server.packs.resources.*;
import net.minecraft.tags.TagLoader;
import net.minecraft.data.registries.VanillaRegistries;
import net.minecraft.core.HolderLookup;
import net.minecraft.world.level.ChunkPos;
import net.minecraft.world.level.biome.Biome;
import net.minecraft.world.level.biome.BiomeGenerationSettings;
import net.minecraft.world.level.biome.BiomeManager;
import net.minecraft.world.level.biome.BiomeSource;
import net.minecraft.world.level.biome.MultiNoiseBiomeSource;
import net.minecraft.world.level.biome.MultiNoiseBiomeSourceParameterList;
import net.minecraft.world.level.biome.MultiNoiseBiomeSourceParameterLists;
import net.minecraft.world.level.biome.Biomes;
import net.minecraft.world.level.chunk.CarvingMask;
import net.minecraft.world.level.chunk.ChunkAccess;
import net.minecraft.world.level.chunk.PalettedContainerFactory;
import net.minecraft.world.level.chunk.PalettedContainer;
import net.minecraft.world.level.chunk.PalettedContainerRO;
import net.minecraft.world.level.chunk.ProtoChunk;
import net.minecraft.world.level.chunk.Strategy;
import net.minecraft.world.level.chunk.UpgradeData;
import net.minecraft.world.level.chunk.status.ChunkStatus;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.levelgen.NoiseBasedChunkGenerator;
import net.minecraft.world.level.levelgen.NoiseGeneratorSettings;
import net.minecraft.world.level.levelgen.RandomState;
import net.minecraft.world.level.levelgen.RandomSupport;
import net.minecraft.world.level.levelgen.LegacyRandomSource;
import net.minecraft.world.level.levelgen.WorldgenRandom;
import net.minecraft.world.level.levelgen.WorldGenerationContext;
import net.minecraft.world.level.levelgen.blending.Blender;
import net.minecraft.world.level.levelgen.Beardifier;
import net.minecraft.world.level.levelgen.NoiseChunk;
import net.minecraft.world.level.levelgen.Aquifer;
import net.minecraft.world.level.levelgen.carver.CarvingContext;
import net.minecraft.world.level.levelgen.carver.ConfiguredWorldCarver;
import net.minecraft.world.level.levelgen.synth.NormalNoise;
import net.minecraft.world.level.LevelHeightAccessor;
import net.minecraft.resources.Identifier;
import net.minecraft.resources.ResourceKey;
import com.mojang.serialization.Lifecycle;
import com.mojang.serialization.Codec;

import java.io.*;
import java.lang.reflect.Field;
import java.util.*;
import java.util.concurrent.CompletableFuture;
import java.util.stream.Collectors;

/**
 * Minecraft Bulk Chunk Generation Test
 * Generates a grid of chunks and outputs summary data for each.
 */
public class MinecraftBulkTest {

    static long SEED = 12345L;
    static final int MIN_Y = -64;
    static final int HEIGHT = 384;
    static final int MAX_Y = MIN_Y + HEIGHT;

    // Grid parameters - 100x100 chunks centered around origin
    static final int GRID_SIZE = 100;
    static final int START_X = -GRID_SIZE / 2;  // -50
    static final int START_Z = -GRID_SIZE / 2;  // -50

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
            registry.register(holder.key(), holder.value(), RegistrationInfo.BUILT_IN);
        });
        registry.freeze();
        return registry;
    }

    public static void main(String[] args) {
        try {
            String outputPath = args.length > 0 ? args[0] : "../output/java_bulk_100x100_old.txt";
            if (args.length > 1) {
                SEED = Long.parseLong(args[1]);
            }

            System.out.println("=== Minecraft Bulk Chunk Generation Test ===");
            System.out.println("Seed: " + SEED);
            System.out.println("Grid: " + GRID_SIZE + "x" + GRID_SIZE + " chunks");
            System.out.println("Range: (" + START_X + "," + START_Z + ") to (" + (START_X + GRID_SIZE - 1) + "," + (START_Z + GRID_SIZE - 1) + ")");
            System.out.println("Total chunks: " + (GRID_SIZE * GRID_SIZE));
            System.out.println();

            // Initialize Minecraft
            System.out.println("Initializing Minecraft Bootstrap...");
            SharedConstants.tryDetectVersion();
            Bootstrap.bootStrap();
            System.out.println("Bootstrap complete.");

            // Load block tags (required for carvers to work!)
            System.out.println("Loading block tags...");
            VanillaPackResources vanillaPack = ServerPacksSource.createVanillaPackSource();
            List<PackResources> packs = List.of(vanillaPack);
            MultiPackResourceManager resourceManager = new MultiPackResourceManager(PackType.SERVER_DATA, packs);
            List<Registry.PendingTags<?>> pendingTags = TagLoader.loadTagsForExistingRegistries(
                resourceManager,
                RegistryAccess.fromRegistryOfRegistries(BuiltInRegistries.REGISTRY)
            );
            System.out.println("Loaded pending tags for " + pendingTags.size() + " registries");
            for (Registry.PendingTags<?> pt : pendingTags) {
                pt.apply();
            }
            System.out.println("Block tags applied.");

            // Setup registries
            HolderLookup.Provider registries = VanillaRegistries.createLookup();

            HolderGetter<NoiseGeneratorSettings> noiseSettingsGetter =
                registries.lookupOrThrow(Registries.NOISE_SETTINGS);
            Holder<NoiseGeneratorSettings> overworldSettings =
                noiseSettingsGetter.getOrThrow(NoiseGeneratorSettings.OVERWORLD);

            HolderLookup.RegistryLookup<Biome> biomeLookup =
                registries.lookupOrThrow(Registries.BIOME);

            HolderGetter<MultiNoiseBiomeSourceParameterList> paramListGetter =
                registries.lookupOrThrow(Registries.MULTI_NOISE_BIOME_SOURCE_PARAMETER_LIST);
            Holder<MultiNoiseBiomeSourceParameterList> overworldParams =
                paramListGetter.getOrThrow(MultiNoiseBiomeSourceParameterLists.OVERWORLD);

            BiomeSource biomeSource = MultiNoiseBiomeSource.createFromPreset(overworldParams);

            NoiseBasedChunkGenerator generator = new NoiseBasedChunkGenerator(
                biomeSource,
                overworldSettings
            );

            HolderGetter<NormalNoise.NoiseParameters> noiseParamsGetter =
                registries.lookupOrThrow(Registries.NOISE);
            RandomState randomState = RandomState.create(
                overworldSettings.value(),
                noiseParamsGetter,
                SEED
            );

            LevelHeightAccessor levelHeight = new SimpleLevelHeight(MIN_Y, HEIGHT);
            MappedRegistry<Biome> biomeRegistry = createBiomeRegistry(biomeLookup);

            // Setup palette factory
            Strategy<BlockState> blockStateStrategy = Strategy.<BlockState>createForBlockStates(Block.BLOCK_STATE_REGISTRY);
            BlockState defaultBlockState = Blocks.AIR.defaultBlockState();
            Strategy<Holder<Biome>> biomeStrategy = Strategy.<Holder<Biome>>createForBiomes(biomeRegistry.asHolderIdMap());
            Holder.Reference<Biome> defaultBiome = biomeRegistry.getOrThrow(Biomes.PLAINS);

            Codec<PalettedContainer<BlockState>> blockStateCodec =
                PalettedContainer.codecRW(BlockState.CODEC, blockStateStrategy, defaultBlockState);
            Codec<PalettedContainerRO<Holder<Biome>>> biomeCodec =
                PalettedContainer.codecRO(biomeRegistry.holderByNameCodec(), biomeStrategy, defaultBiome);

            PalettedContainerFactory containerFactory = new PalettedContainerFactory(
                blockStateStrategy, defaultBlockState, blockStateCodec,
                biomeStrategy, defaultBiome, biomeCodec
            );

            NoiseGeneratorSettings noiseSettings = overworldSettings.value();
            Aquifer.FluidStatus lavaStatus = new Aquifer.FluidStatus(-54, Blocks.LAVA.defaultBlockState());
            int seaLevel = noiseSettings.seaLevel();
            Aquifer.FluidStatus seaStatus = new Aquifer.FluidStatus(seaLevel, noiseSettings.defaultFluid());
            Aquifer.FluidPicker globalFluidPicker = (x, y, z) -> y < Math.min(-54, seaLevel) ? lavaStatus : seaStatus;

            // Create RegistryAccess that includes both built-in and data-driven registries
            // Need biome registry for CarvingContext.topMaterial()
            List<Registry<?>> allRegistries = new ArrayList<>();
            BuiltInRegistries.REGISTRY.forEach(allRegistries::add);
            allRegistries.add(biomeRegistry);
            RegistryAccess.Frozen registryAccess = new RegistryAccess.ImmutableRegistryAccess(allRegistries).freeze();

            System.out.println("Setup complete. Starting chunk generation...");
            System.out.println();

            // Generate all chunks
            try (PrintWriter writer = new PrintWriter(new FileWriter(outputPath))) {
                writer.println("# Minecraft Bulk Chunk Test Output");
                writer.println("# Seed: " + SEED);
                writer.println("# Grid: " + GRID_SIZE + "x" + GRID_SIZE);
                writer.println("# Phases: BIOMES, NOISE, SURFACE, CARVERS");
                writer.println("# Format: chunkX,chunkZ,airBlocks,nonAirBlocks,block1:count1,block2:count2,...");
                writer.println();

                int totalChunks = GRID_SIZE * GRID_SIZE;
                int completed = 0;
                long startTime = System.currentTimeMillis();

                for (int cz = START_Z; cz < START_Z + GRID_SIZE; cz++) {
                    for (int cx = START_X; cx < START_X + GRID_SIZE; cx++) {
                        ChunkPos chunkPos = new ChunkPos(cx, cz);

                        // Create chunk
                        ProtoChunk chunk = new ProtoChunk(
                            chunkPos, UpgradeData.EMPTY, levelHeight, containerFactory, null
                        );

                        // Pre-create NoiseChunk
                        NoiseChunk noiseChunk = NoiseChunk.forChunk(
                            chunk, randomState, Beardifier.EMPTY, noiseSettings, globalFluidPicker, Blender.empty()
                        );
                        Field noiseChunkField = ChunkAccess.class.getDeclaredField("noiseChunk");
                        noiseChunkField.setAccessible(true);
                        noiseChunkField.set(chunk, noiseChunk);

                        // Phase 1: Biomes
                        generator.createBiomes(randomState, Blender.empty(), null, chunk);
                        chunk.setPersistedStatus(ChunkStatus.BIOMES);

                        // Phase 2: Noise
                        generator.fillFromNoise(Blender.empty(), randomState, null, chunk).join();
                        chunk.setPersistedStatus(ChunkStatus.NOISE);

                        // Phase 3: Surface
                        WorldGenerationContext genContext = new WorldGenerationContext(generator, levelHeight);
                        long obfuscatedSeed = BiomeManager.obfuscateSeed(SEED);
                        // FIX: Use biomeSource directly instead of chunk to avoid & 3 coordinate wrapping
                        // When using chunk, queries to quarts outside this chunk get wrapped to wrong biomes
                        BiomeManager biomeManager = new BiomeManager(
                            (quartX, quartY, quartZ) -> biomeSource.getNoiseBiome(quartX, quartY, quartZ, randomState.sampler()),
                            obfuscatedSeed
                        );
                        generator.buildSurface(chunk, genContext, randomState, null, biomeManager, biomeRegistry, Blender.empty());
                        chunk.setPersistedStatus(ChunkStatus.SURFACE);

                        // Phase 4: Carvers
                        applyCarvers(generator, chunk, randomState, biomeManager, biomeRegistry, noiseSettings, levelHeight, registryAccess);
                        chunk.setPersistedStatus(ChunkStatus.CARVERS);

                        // Calculate summary
                        ChunkSummary summary = calculateChunkSummary(chunk);

                        // Write result: chunkX,chunkZ,airBlocks,nonAirBlocks,block1:count1,block2:count2,...
                        StringBuilder sb = new StringBuilder();
                        sb.append(cx).append(",").append(cz).append(",");
                        sb.append(summary.airBlocks).append(",").append(summary.nonAirBlocks);

                        // Add block counts (sorted by count descending)
                        List<Map.Entry<String, Long>> sortedBlocks = new ArrayList<>(summary.blockCounts.entrySet());
                        sortedBlocks.sort((a, b) -> Long.compare(b.getValue(), a.getValue()));
                        for (Map.Entry<String, Long> entry : sortedBlocks) {
                            sb.append(",").append(entry.getKey()).append(":").append(entry.getValue());
                        }
                        writer.println(sb.toString());

                        completed++;
                        if (completed % 100 == 0) {
                            double elapsed = (System.currentTimeMillis() - startTime) / 1000.0;
                            double rate = completed / elapsed;
                            double remaining = (totalChunks - completed) / rate;
                            System.out.printf("Progress: %d/%d chunks (%.1f%%) - %.1f chunks/sec - ETA: %.0f sec%n",
                                completed, totalChunks, 100.0 * completed / totalChunks, rate, remaining);
                        }
                    }
                }

                long totalTime = System.currentTimeMillis() - startTime;
                System.out.println();
                System.out.println("Generation complete!");
                System.out.println("Total time: " + (totalTime / 1000.0) + " seconds");
                System.out.println("Average: " + (totalTime / (double)totalChunks) + " ms per chunk");
                System.out.println("Output written to: " + outputPath);
            }

            // Cleanup
            resourceManager.close();

        } catch (Exception e) {
            System.err.println("Error during bulk chunk generation:");
            e.printStackTrace();
            System.exit(1);
        }
    }

    static void applyCarvers(
            NoiseBasedChunkGenerator generator,
            ProtoChunk chunk,
            RandomState randomState,
            BiomeManager biomeManager,
            Registry<Biome> biomeRegistry,
            NoiseGeneratorSettings noiseSettings,
            LevelHeightAccessor levelHeight,
            RegistryAccess.Frozen registryAccess) throws Exception {

        NoiseChunk noiseChunk = chunk.getOrCreateNoiseChunk((c) -> {
            throw new IllegalStateException("NoiseChunk should already exist");
        });

        Aquifer aquifer = noiseChunk.aquifer();

        CarvingContext carvingContext = new CarvingContext(
            generator, registryAccess, levelHeight, noiseChunk, randomState, noiseSettings.surfaceRule()
        );

        CarvingMask mask = chunk.getOrCreateCarvingMask();
        WorldgenRandom random = new WorldgenRandom(new LegacyRandomSource(RandomSupport.generateUniqueSeed()));

        ChunkPos pos = chunk.getPos();

        BiomeManager correctBiomeManager = biomeManager.withDifferentSource(
            (quartX, quartY, quartZ) -> generator.getBiomeSource().getNoiseBiome(quartX, quartY, quartZ, randomState.sampler())
        );

        for (int dx = -8; dx <= 8; dx++) {
            for (int dz = -8; dz <= 8; dz++) {
                ChunkPos sourcePos = new ChunkPos(pos.x + dx, pos.z + dz);

                int quartX = QuartPos.fromBlock(sourcePos.getMinBlockX() + 8);
                int quartZ = QuartPos.fromBlock(sourcePos.getMinBlockZ() + 8);
                Holder<Biome> biome = generator.getBiomeSource().getNoiseBiome(quartX, 0, quartZ, randomState.sampler());

                BiomeGenerationSettings genSettings = biome.value().getGenerationSettings();
                Iterable<Holder<ConfiguredWorldCarver<?>>> carvers = genSettings.getCarvers();

                int carverIndex = 0;
                for (Holder<ConfiguredWorldCarver<?>> carverHolder : carvers) {
                    ConfiguredWorldCarver<?> carver = carverHolder.value();

                    random.setLargeFeatureSeed(SEED + (long)carverIndex, sourcePos.x, sourcePos.z);

                    if (carver.isStartChunk(random)) {
                        carver.carve(carvingContext, chunk, correctBiomeManager::getBiome, random, aquifer, sourcePos, mask);
                    }

                    carverIndex++;
                }
            }
        }
    }

    static class ChunkSummary {
        long airBlocks;
        long nonAirBlocks;
        Map<String, Long> blockCounts;

        ChunkSummary(long airBlocks, long nonAirBlocks, Map<String, Long> blockCounts) {
            this.airBlocks = airBlocks;
            this.nonAirBlocks = nonAirBlocks;
            this.blockCounts = blockCounts;
        }
    }

    static ChunkSummary calculateChunkSummary(ChunkAccess chunk) {
        int startX = chunk.getPos().getMinBlockX();
        int startZ = chunk.getPos().getMinBlockZ();

        long airCount = 0;
        long nonAirCount = 0;
        Map<String, Long> blockCounts = new HashMap<>();  // Will be sorted by count when output

        for (int y = MIN_Y; y < MAX_Y; y++) {
            for (int x = 0; x < 16; x++) {
                for (int z = 0; z < 16; z++) {
                    BlockState state = chunk.getBlockState(new BlockPos(startX + x, y, startZ + z));
                    String blockName = getBlockName(state);

                    blockCounts.merge(blockName, 1L, Long::sum);

                    if (blockName.equals("minecraft:air")) {
                        airCount++;
                    } else {
                        nonAirCount++;
                    }
                }
            }
        }

        return new ChunkSummary(airCount, nonAirCount, blockCounts);
    }

    static String getBlockName(BlockState state) {
        Identifier loc = BuiltInRegistries.BLOCK.getKey(state.getBlock());
        return loc.toString();
    }
}
