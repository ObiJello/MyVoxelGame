import net.minecraft.SharedConstants;
import net.minecraft.server.Bootstrap;
import net.minecraft.data.registries.VanillaRegistries;
import net.minecraft.core.*;
import net.minecraft.core.registries.Registries;
import net.minecraft.world.level.*;
import net.minecraft.world.level.biome.*;
import net.minecraft.world.level.block.*;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.chunk.*;
import net.minecraft.world.level.levelgen.*;
import net.minecraft.world.level.levelgen.blending.Blender;
import net.minecraft.world.level.levelgen.synth.NormalNoise;
import com.mojang.serialization.*;
import java.io.*;
import java.lang.reflect.*;
import java.util.*;

/**
 * Accurate Noise Phase Test
 *
 * This test creates the NoiseChunk exactly as Minecraft does, but with Beardifier.EMPTY
 * since we're not testing structures. It then pre-sets the NoiseChunk on the chunk
 * and calls doFill via reflection.
 *
 * This approach:
 * 1. Uses the exact same NoiseChunk.forChunk() factory method Minecraft uses
 * 2. Uses the exact same globalFluidPicker Minecraft creates
 * 3. Calls the exact same doFill() method Minecraft calls
 * 4. Only differs in using Beardifier.EMPTY instead of forStructuresInChunk()
 *    (which would also return EMPTY for chunks with no structures)
 */
public class AccurateNoisePhaseTest {
    static final long SEED = 12345L;
    static final int MIN_Y = -64;
    static final int HEIGHT = 384;
    static final int MAX_Y = MIN_Y + HEIGHT;

    static class SimpleLevelHeight implements LevelHeightAccessor {
        private final int minY, height;
        SimpleLevelHeight(int minY, int height) { this.minY = minY; this.height = height; }
        @Override public int getHeight() { return height; }
        @Override public int getMinY() { return minY; }
    }

    static MappedRegistry<Biome> createBiomeRegistry(HolderLookup.RegistryLookup<Biome> lookup) {
        MappedRegistry<Biome> registry = new MappedRegistry<>(Registries.BIOME, Lifecycle.stable());
        lookup.listElements().forEach(holder -> {
            registry.register(holder.key(), holder.value(), RegistrationInfo.BUILT_IN);
        });
        registry.freeze();
        return registry;
    }

    /**
     * Creates the globalFluidPicker exactly as NoiseBasedChunkGenerator does
     * Reference: NoiseBasedChunkGenerator.createFluidPicker()
     */
    static Aquifer.FluidPicker createFluidPicker(NoiseGeneratorSettings settings) {
        Aquifer.FluidStatus lavaStatus = new Aquifer.FluidStatus(-54, Blocks.LAVA.defaultBlockState());
        int seaLevel = settings.seaLevel();
        Aquifer.FluidStatus seaStatus = new Aquifer.FluidStatus(seaLevel, settings.defaultFluid());
        // Note: Not checking DEBUG_DISABLE_FLUID_GENERATION as it's false by default
        return (x, y, z) -> y < Math.min(-54, seaLevel) ? lavaStatus : seaStatus;
    }

    public static void main(String[] args) throws Exception {
        int chunkX = args.length > 0 ? Integer.parseInt(args[0]) : 0;
        int chunkZ = args.length > 1 ? Integer.parseInt(args[1]) : 0;
        String outputFile = "java_noise_" + chunkX + "_" + chunkZ + ".txt";

        System.out.println("=== Accurate Minecraft Noise Phase Test ===");
        System.out.println("Seed: " + SEED);
        System.out.println("Chunk: (" + chunkX + ", " + chunkZ + ")");
        System.out.println("Output: " + outputFile);
        System.out.println();

        SharedConstants.tryDetectVersion();
        Bootstrap.bootStrap();

        // Get registries
        HolderLookup.Provider registries = VanillaRegistries.createLookup();

        Holder<NoiseGeneratorSettings> overworldSettingsHolder =
            registries.lookupOrThrow(Registries.NOISE_SETTINGS).getOrThrow(NoiseGeneratorSettings.OVERWORLD);
        NoiseGeneratorSettings noiseGenSettings = overworldSettingsHolder.value();

        HolderLookup.RegistryLookup<Biome> biomeLookup = registries.lookupOrThrow(Registries.BIOME);
        HolderGetter<NormalNoise.NoiseParameters> noiseParamsGetter = registries.lookupOrThrow(Registries.NOISE);

        // Create RandomState exactly as Minecraft does
        RandomState randomState = RandomState.create(noiseGenSettings, noiseParamsGetter, SEED);

        // Create the globalFluidPicker exactly as NoiseBasedChunkGenerator does
        Aquifer.FluidPicker globalFluidPicker = createFluidPicker(noiseGenSettings);

        // Create generator (needed for doFill call)
        Holder<Biome> plainsBiome = biomeLookup.getOrThrow(Biomes.PLAINS);
        FixedBiomeSource biomeSource = new FixedBiomeSource(plainsBiome);
        NoiseBasedChunkGenerator generator = new NoiseBasedChunkGenerator(biomeSource, overworldSettingsHolder);

        // Create chunk infrastructure
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

        LevelHeightAccessor levelHeight = new SimpleLevelHeight(MIN_Y, HEIGHT);
        ChunkPos chunkPos = new ChunkPos(chunkX, chunkZ);
        ProtoChunk chunk = new ProtoChunk(chunkPos, UpgradeData.EMPTY, levelHeight, containerFactory, null);

        // Create NoiseChunk using the exact factory method Minecraft uses
        // The only difference is we use Beardifier.EMPTY instead of forStructuresInChunk
        // This is accurate because forStructuresInChunk returns EMPTY when there are no structures
        System.out.println("Creating NoiseChunk with Beardifier.EMPTY...");
        NoiseChunk noiseChunk = NoiseChunk.forChunk(
            chunk,
            randomState,
            Beardifier.EMPTY,  // Same as forStructuresInChunk() when no structures
            noiseGenSettings,
            globalFluidPicker,
            Blender.empty()
        );

        // Pre-set the noiseChunk on the chunk so doFill won't try to create it
        // This is done via reflection to set the private field
        Field noiseChunkField = ChunkAccess.class.getDeclaredField("noiseChunk");
        noiseChunkField.setAccessible(true);
        noiseChunkField.set(chunk, noiseChunk);
        System.out.println("NoiseChunk set on chunk via reflection.");

        // Calculate cell parameters (same as doFill does)
        NoiseSettings noiseSettings = noiseGenSettings.noiseSettings().clampToHeightAccessor(chunk.getHeightAccessorForGeneration());
        int minY = noiseSettings.minY();
        int cellMinY = Math.floorDiv(minY, noiseSettings.getCellHeight());
        int cellCountY = Math.floorDiv(noiseSettings.height(), noiseSettings.getCellHeight());

        System.out.println("Calling doFill via reflection...");
        System.out.println("  cellMinY=" + cellMinY + " cellCountY=" + cellCountY);

        // Call doFill via reflection (it's a private method)
        Method doFillMethod = NoiseBasedChunkGenerator.class.getDeclaredMethod(
            "doFill",
            Blender.class, StructureManager.class, RandomState.class, ChunkAccess.class, int.class, int.class
        );
        doFillMethod.setAccessible(true);

        // Call doFill - structureManager can be null now because we already set noiseChunk
        ChunkAccess resultChunk = (ChunkAccess) doFillMethod.invoke(
            generator,
            Blender.empty(),
            null,  // structureManager - not used because noiseChunk is already set
            randomState,
            chunk,
            cellMinY,
            cellCountY
        );

        System.out.println("doFill complete. Writing output...");

        // Write output
        try (PrintWriter writer = new PrintWriter(new FileWriter(outputFile))) {
            writer.println("# Minecraft Noise Phase Output (via doFill)");
            writer.println("# Seed: " + SEED);
            writer.println("# Chunk: (" + chunkX + ", " + chunkZ + ")");
            writer.println("# Format: x,y,z,block_name");
            writer.println();

            Map<String, Integer> blockCounts = new HashMap<>();
            int minBlockX = chunkPos.getMinBlockX();
            int minBlockZ = chunkPos.getMinBlockZ();

            for (int y = MIN_Y; y < MAX_Y; y++) {
                for (int x = 0; x < 16; x++) {
                    for (int z = 0; z < 16; z++) {
                        BlockPos pos = new BlockPos(minBlockX + x, y, minBlockZ + z);
                        BlockState state = resultChunk.getBlockState(pos);
                        String blockName = state.getBlock().toString();
                        if (blockName.startsWith("Block{") && blockName.endsWith("}")) {
                            blockName = blockName.substring(6, blockName.length() - 1);
                        }

                        writer.println(x + "," + y + "," + z + "," + blockName);
                        blockCounts.merge(blockName, 1, Integer::sum);
                    }
                }
            }

            writer.println();
            writer.println("# Summary:");
            writer.println("# Total blocks: " + (16 * HEIGHT * 16));

            int airCount = blockCounts.getOrDefault("minecraft:air", 0);
            writer.println("# Air blocks: " + airCount);
            writer.println("# Non-air blocks: " + (16 * HEIGHT * 16 - airCount));
            writer.println("# Block types:");

            blockCounts.entrySet().stream()
                .sorted((a, b) -> b.getValue().compareTo(a.getValue()))
                .forEach(e -> writer.println("#   " + e.getKey() + ": " + e.getValue()));
        }

        System.out.println("Output written to: " + outputFile);
        System.out.println("Done.");
    }
}
