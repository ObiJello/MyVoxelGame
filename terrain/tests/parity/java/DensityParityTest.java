import java.io.BufferedOutputStream;
import java.io.DataOutputStream;
import java.io.FileOutputStream;
import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.List;
import net.minecraft.SharedConstants;
import net.minecraft.core.HolderGetter;
import net.minecraft.core.HolderLookup;
import net.minecraft.core.LayeredRegistryAccess;
import net.minecraft.core.Registry;
import net.minecraft.core.RegistryAccess;
import net.minecraft.core.registries.Registries;
import net.minecraft.resources.Identifier;
import net.minecraft.resources.RegistryDataLoader;
import net.minecraft.resources.ResourceKey;
import net.minecraft.server.Bootstrap;
import net.minecraft.server.RegistryLayer;
import net.minecraft.server.packs.repository.ServerPacksSource;
import net.minecraft.server.packs.PackType;
import net.minecraft.server.packs.resources.MultiPackResourceManager;
import net.minecraft.server.packs.resources.ResourceManager;
import net.minecraft.tags.TagLoader;
import net.minecraft.world.level.levelgen.NoiseGeneratorSettings;
import net.minecraft.world.level.levelgen.NoiseRouter;
import net.minecraft.world.level.levelgen.RandomState;
import net.minecraft.world.level.levelgen.densityfunction.DensityBuffer;
import net.minecraft.world.level.levelgen.densityfunction.DensityBufferPool;
import net.minecraft.world.level.levelgen.densityfunction.DensityFunction;
import net.minecraft.world.level.levelgen.densityfunction.DensitySamplerSet;
import net.minecraft.world.level.levelgen.densityfunction.DensityVolume;
import net.minecraft.world.level.levelgen.densityfunction.SamplerContext;
import net.minecraft.world.level.levelgen.densityfunction.ScopedDensityBuffer;
import net.minecraft.world.level.levelgen.synth.NormalNoise;

/**
 * Density engine parity: every noise_settings router (and aquifer) function,
 * decoded from the vanilla datapack exactly as a world load does, sampled
 * three ways - uncached points, cached chunk and quart volumes, and cached
 * points answered from those volumes. The C++ twin (cpp/DensityParityTest.cpp)
 * writes the same stream; compare with compare_density_parity.py.
 *
 * Usage: DensityParityTest <seed> <out.txt> <out.bin>
 */
public class DensityParityTest {
    static final String[] SETTINGS = {"overworld", "large_biomes", "amplified", "nether", "end", "caves", "floating_islands"};
    static final int[][] CHUNKS = {{0, 0}, {-7, 12}, {123, -456}, {-3000, 2500}};

    public static void main(String[] args) throws Exception {
        SharedConstants.tryDetectVersion();
        Bootstrap.bootStrap();
        long seed = Long.parseLong(args[0]);

        // The vanilla datapack (server data), as WorldLoader opens it.
        ResourceManager resources = new MultiPackResourceManager(PackType.SERVER_DATA,
            List.of(ServerPacksSource.createVanillaPackSource().fullResources()));
        LayeredRegistryAccess<RegistryLayer> layers = RegistryLayer.createRegistryAccess();
        List<Registry.PendingTags<?>> staticTags = TagLoader.loadTagsForExistingRegistries(resources, layers.getLayer(RegistryLayer.STATIC));
        RegistryAccess.Frozen context = layers.getAccessForLoading(RegistryLayer.WORLD);
        List<HolderLookup.RegistryLookup<?>> contextRegistries = TagLoader.buildUpdatedLookups(context, staticTags);
        RegistryAccess.Frozen world = RegistryDataLoader.load(resources, contextRegistries, RegistryDataLoader.WORLD_REGISTRIES, Runnable::run).join();
        HolderGetter<NormalNoise> noises = world.lookupOrThrow(Registries.NOISE);

        try (PrintWriter text = new PrintWriter(args[1]);
             DataOutputStream bin = new DataOutputStream(new BufferedOutputStream(new FileOutputStream(args[2])))) {
            for (String name : SETTINGS) {
                NoiseGeneratorSettings settings = world.lookupOrThrow(Registries.NOISE_SETTINGS)
                    .getOrThrow(ResourceKey.create(Registries.NOISE_SETTINGS, Identifier.withDefaultNamespace(name))).value();
                RandomState randomState = RandomState.create(noises, seed, settings);
                List<String> labels = new ArrayList<>();
                List<DensityFunction> functions = new ArrayList<>();
                NoiseRouter r = settings.noiseRouter();
                labels.add("temperature"); functions.add(r.temperature());
                labels.add("vegetation"); functions.add(r.vegetation());
                labels.add("continents"); functions.add(r.continents());
                labels.add("erosion"); functions.add(r.erosion());
                labels.add("depth"); functions.add(r.depth());
                labels.add("ridges"); functions.add(r.ridges());
                labels.add("chunk_surface_level"); functions.add(r.chunkSurfaceLevel());
                labels.add("final_density"); functions.add(r.finalDensity());
                settings.aquifers().ifPresent(a -> {
                    labels.add("aq_barrier"); functions.add(a.barrierNoise());
                    labels.add("aq_floodedness"); functions.add(a.fluidLevelFloodednessNoise());
                    labels.add("aq_spread"); functions.add(a.fluidLevelSpreadNoise());
                    labels.add("aq_lava"); functions.add(a.lavaNoise());
                    labels.add("aq_exclusion"); functions.add(a.exclusion());
                    labels.add("aq_surface_level"); functions.add(a.surfaceLevel());
                });
                int minY = settings.noiseSettings().minY();
                int height = settings.noiseSettings().height();

                // Uncached points.
                for (int f = 0; f < functions.size(); f++) {
                    for (int i = 0; i < 64; i++) {
                        int x = pointX(i), y = minY + Math.floorMod(i * 37, height), z = pointZ(i);
                        float v = randomState.sampleBlockValueUncached(functions.get(f), x, y, z);
                        text.println("P " + name + " " + labels.get(f) + " " + x + " " + y + " " + z + " " + hex(v));
                    }
                }
                // Cached volumes, then cached points inside them, one context per chunk.
                for (int[] c : CHUNKS) {
                    DensityBufferPool pool = randomState.acquireDensityBufferPool();
                    SamplerContext sc = SamplerContext.builder().useBufferArena(pool).enableCaches().build();
                    DensitySamplerSet samplers = randomState.samplersWithContext(sc);
                    DensityVolume chunk = new DensityVolume(16, height, 16, c[0] * 16, minY, c[1] * 16);
                    DensityVolume quart = new DensityVolume(4, height / 4, 4, c[0] * 16, minY, c[1] * 16, 4, 4, 4);
                    for (int f = 0; f < functions.size(); f++) {
                        for (DensityVolume v : new DensityVolume[]{chunk, quart}) {
                            try (ScopedDensityBuffer b = samplers.get(functions.get(f)).sampleVolume(v)) {
                                text.println("V " + name + " " + labels.get(f) + " " + c[0] + " " + c[1] + " " + v.sizeX() + "x" + v.sizeY() + "x" + v.sizeZ() + " " + v.size());
                                for (int i = 0; i < v.size(); i++) bin.writeInt(Float.floatToRawIntBits(b.get(i)));
                            }
                        }
                        for (int i = 0; i < 16; i++) {
                            int x = c[0] * 16 + (i * 5) % 16, y = minY + Math.floorMod(i * 53, height), z = c[1] * 16 + (i * 11) % 16;
                            float v = samplers.sampleValue(functions.get(f), x, y, z);
                            text.println("C " + name + " " + labels.get(f) + " " + x + " " + y + " " + z + " " + hex(v));
                        }
                    }
                    randomState.releaseDensityBufferPool(pool);
                }
            }
        }
    }

    static int pointX(int i) { return (int) ((i * 2654435761L) % 60001L) - 30000; }
    static int pointZ(int i) { return (int) ((i * 40503L + 17L) % 60001L) - 30000; }
    static String hex(float v) { return Integer.toHexString(Float.floatToRawIntBits(v)); }
}
