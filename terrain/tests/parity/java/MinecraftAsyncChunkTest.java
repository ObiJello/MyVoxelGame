/**
 * MinecraftAsyncChunkTest - Tests Minecraft's ACTUAL async chunk generation
 *
 * This test uses the FULL Minecraft server infrastructure:
 * - HeadlessParityServer (GameTestServer pattern: full server, no network bind)
 * - ServerChunkCache.getChunk() (the actual chunk loading API)
 * - DistanceManager + Tickets (chunk loading management)
 * - ChunkMap + ChunkTaskDispatcher (async task scheduling)
 * - ConsecutiveExecutor (task serialization)
 * - All 12 ChunkStatus levels (EMPTY -> FULL)
 *
 * This is the proper comparison for the C++ async pipeline test.
 */

import com.mojang.authlib.GameProfile;
import com.mojang.authlib.GameProfileRepository;
import com.mojang.authlib.minecraft.SessionService;
import com.mojang.authlib.services.ServicesKeySet;
import com.mojang.logging.LogUtils;
import com.mojang.serialization.Lifecycle;
import net.minecraft.CrashReport;
import net.minecraft.DefaultUncaughtExceptionHandler;
import net.minecraft.SharedConstants;
import net.minecraft.commands.Commands;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Direction;
import net.minecraft.core.Registry;
import net.minecraft.core.RegistryAccess;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.core.registries.Registries;
import net.minecraft.server.Bootstrap;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.Services;
import net.minecraft.server.WorldLoader;
import net.minecraft.server.WorldStem;
import net.minecraft.SystemReport;
import net.minecraft.gizmos.GizmoCollector;
import net.minecraft.gizmos.Gizmos;
import net.minecraft.server.level.ServerChunkCache;
import net.minecraft.server.level.progress.LoggingLevelLoadListener;
import net.minecraft.server.notifications.EmptyNotificationService;
import net.minecraft.server.notifications.NotificationManager;
import net.minecraft.server.permissions.PermissionSet;
import net.minecraft.server.players.NameAndId;
import net.minecraft.server.players.PlayerList;
import net.minecraft.server.players.ProfileResolver;
import net.minecraft.server.players.UserNameToIdResolver;
import net.minecraft.util.debugchart.LocalSampleLogger;
import net.minecraft.util.debugchart.SampleLogger;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.packs.repository.PackRepository;
import net.minecraft.server.packs.repository.ServerPacksSource;
import net.minecraft.util.Util;
import net.minecraft.util.datafix.DataFixers;
import net.minecraft.world.Difficulty;
import net.minecraft.world.flag.FeatureFlags;
import net.minecraft.world.level.ChunkPos;
import net.minecraft.world.level.DataPackConfig;
import net.minecraft.world.level.GameType;
import net.minecraft.world.level.Level;
import net.minecraft.world.level.LevelSettings;
import net.minecraft.world.level.WorldDataConfiguration;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.block.state.properties.Property;
import net.minecraft.world.level.levelgen.Heightmap;
import net.minecraft.world.level.chunk.ChunkAccess;
import net.minecraft.world.level.chunk.status.ChunkStatus;
import net.minecraft.world.level.dimension.LevelStem;
import net.minecraft.world.level.gamerules.GameRules;
import net.minecraft.world.level.levelgen.WorldDimensions;
import net.minecraft.world.level.levelgen.WorldOptions;
import net.minecraft.world.level.levelgen.WorldGenSettings;
import net.minecraft.world.level.storage.LevelDataAndDimensions;
import net.minecraft.world.level.levelgen.structure.BoundingBox;
import net.minecraft.world.level.levelgen.structure.Structure;
import net.minecraft.world.level.levelgen.structure.StructurePiece;
import net.minecraft.world.level.levelgen.structure.StructureStart;
import net.minecraft.world.level.levelgen.structure.TemplateStructurePiece;
import net.minecraft.world.level.levelgen.structure.PoolElementStructurePiece;
import net.minecraft.world.level.levelgen.structure.pools.SinglePoolElement;
import net.minecraft.world.level.levelgen.structure.pools.StructurePoolElement;
import net.minecraft.world.level.levelgen.structure.pools.JigsawJunction;
import net.minecraft.world.level.block.Rotation;
import com.mojang.datafixers.util.Either;
import net.minecraft.world.level.levelgen.presets.WorldPresets;
import net.minecraft.world.level.storage.LevelStorageSource;
import net.minecraft.world.level.storage.PrimaryLevelData;
import net.minecraft.world.level.storage.WorldData;
import net.minecraft.server.permissions.LevelBasedPermissionSet;
import it.unimi.dsi.fastutil.longs.LongSet;

import org.slf4j.Logger;

import java.io.File;
import java.io.FileWriter;
import java.io.PrintWriter;
import java.net.Proxy;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicReference;

public class MinecraftAsyncChunkTest {

    private static final Logger LOGGER = LogUtils.getLogger();
    private static long SEED = 12345L;  // overridable via --seed
    // Level-derived after the target ServerLevel is chosen (C1): overworld
    // -64..320, nether/end 0..256. MAX_Y stays EXCLUSIVE (getMaxY()+1).
    private static int MIN_Y = -64;
    private static int MAX_Y = 320;
    private static String dimension = "overworld";
    // World type (MC WorldPresets, overworld only) + flat/single-biome custom.
    static String worldType = "default";
    static String flatPreset = "";     // vanilla flat preset short name ("" = default flat)
    static String flatLayers = "";     // custom "<layers>;<biome>" (PresetFlatWorldScreen)
    static String featureOrderOutput = null;  // --dump-feature-order <file>: featuresPerStep, then exit
    // --block-trace <file>: every block a decoration feature sets, as
    // "BLOCK_SET STEP=<s> IDX=<i> <placed feature> pos=x,y,z old=<id> new=<id>"
    // (the C++ harness's --block-trace lines; ore writes through
    // BulkSectionAccess are not seen here).
    static String blockTracePath = null;
    static java.io.PrintWriter blockTraceWriter = null;
    static String singleBiome = "";    // single_biome_surface biome id  // --dimension overworld|nether|end

    private static ServerLevel getTargetLevel(MinecraftServer server) {
        net.minecraft.resources.ResourceKey<net.minecraft.world.level.Level> key;
        switch (dimension) {
            case "nether": key = net.minecraft.world.level.Level.NETHER; break;
            case "end": key = net.minecraft.world.level.Level.END; break;
            case "overworld": key = net.minecraft.world.level.Level.OVERWORLD; break;
            default: throw new IllegalArgumentException("Unknown dimension: " + dimension);
        }
        ServerLevel level = server.getLevel(key);
        if (level == null) {
            throw new IllegalStateException("Server has no level for dimension " + dimension);
        }
        MIN_Y = level.getMinY();
        MAX_Y = level.getMaxY() + 1;
        System.out.println("  Dimension: " + dimension + " (y " + MIN_Y + ".." + MAX_Y + ")");
        return level;
    }

    // Phase configuration (initialized in main() after Bootstrap)
    private static boolean dumpFull = false;
    private static ChunkStatus targetStatus = null;
    private static boolean generateStructures = true;
    private static String phasesDescription = "all (0-9)";

    /**
     * Parse a --phases spec into (targetStatus, generateStructures, phasesDescription).
     * Must be called AFTER Bootstrap (ChunkStatus requires registries), and must stay
     * semantically identical to configurePhases() in tests/parity/cpp/CppChunkGeneratorTest.cpp.
     *
     * Accepted: "all", or "A-B" where A is 0 (EMPTY start, structures ON) or
     * 3 (BIOMES start, structures OFF) and B in 1..5 (B >= 3 when A is 3):
     * 1=STRUCTURE_STARTS, 2=STRUCTURE_REFS, 3=BIOMES, 4=TERRAIN, 5=FEATURES —
     * MC 26.3's statuses (ChunkStatus.java:111-116; TERRAIN replaced the old
     * NOISE/SURFACE/CARVERS). Unknown specs must fail loudly: silently running
     * "all" (racy FULL pipeline) produced misleading dumps.
     */
    private static void configurePhaseSpec(String phases) {
        if (phases.equals("all")) {
            targetStatus = ChunkStatus.FULL;
            generateStructures = true;
            phasesDescription = "all (0-9, complete generation)";
            return;
        }
        java.util.regex.Matcher m = java.util.regex.Pattern.compile("^([03])-([1-5])$").matcher(phases);
        if (!m.matches()) {
            throw new IllegalArgumentException(
                "unknown --phases spec '" + phases + "' (valid: all, or A-B with A in {0,3}, B in 1..5, e.g. 3-5, 0-2, 3-4)");
        }
        int start = Integer.parseInt(m.group(1));
        int end = Integer.parseInt(m.group(2));
        if (start == 3 && end < 3) {
            throw new IllegalArgumentException(
                "invalid --phases spec '" + phases + "': end phase " + end + " precedes start phase 3");
        }
        ChunkStatus[] endStatuses = {
            null, ChunkStatus.STRUCTURE_STARTS, ChunkStatus.STRUCTURE_REFERENCES, ChunkStatus.BIOMES,
            ChunkStatus.TERRAIN, ChunkStatus.FEATURES
        };
        String[] endNames = {
            null, "STRUCTURE_STARTS", "STRUCTURE_REFS", "BIOMES", "TERRAIN", "FEATURES"
        };
        targetStatus = endStatuses[end];
        generateStructures = (start == 0);
        // Reproduces the legacy strings exactly, e.g. "3-7 (BIOMES->FEATURES, no structures)".
        phasesDescription = phases + " (" + (start == 0 ? "EMPTY" : "BIOMES") + "->" + endNames[end]
            + ", " + (generateStructures ? "with structures" : "no structures") + ")";
    }

    // =========================================================================
    // FEATURE TRACING (for parity debugging - can be removed after fixing)
    // =========================================================================
    private static boolean traceFeatures = false;
    private static String traceOutputPath = null;
    private static String traceFeatureFilter = null;
    private static PrintWriter traceWriter = null;
    private static boolean traceStructures = false;
    private static BlockPos traceWatchPos = null;
    private static String lastWatchScanState = "";
    private static String structureTraceOutputPath = null;
    // Placement-layer trace: raw isStructureChunk decisions + ring positions,
    // no chunk generation involved. Comparison target for the C++ placement port.
    private static String placementTraceOutputPath = null;
    private static int placementRadius = -1;  // -1 = use the run's radius
    // E-line emission gate (FORMAT.md): off until C++ block-entity support lands.
    private static boolean dumpBlockEntities = false;
    private static final AtomicInteger liveTraceEventCounter = new AtomicInteger();
    private static final Set<String> loggedSeedTraceErrors = ConcurrentHashMap.newKeySet();
    private static final Set<Object> liveTraceInstalledTargets =
        Collections.newSetFromMap(new IdentityHashMap<>());


    /**
     * World dimensions for the selected --world-type (MC WorldPresets), with
     * flat/single-biome customization applied exactly like the vanilla
     * create-world flow (replaceOverworldGenerator).
     */
    private static net.minecraft.world.level.levelgen.WorldDimensions createWorldDimensionsForType(
            net.minecraft.core.HolderLookup.Provider wg) {
        if (worldType.equals("default")) {
            return WorldPresets.createNormalWorldDimensions(wg);
        }
        net.minecraft.resources.ResourceKey<net.minecraft.world.level.levelgen.presets.WorldPreset> presetKey;
        switch (worldType) {
            case "flat": presetKey = WorldPresets.FLAT; break;
            case "large_biomes": presetKey = WorldPresets.LARGE_BIOMES; break;
            case "amplified": presetKey = WorldPresets.AMPLIFIED; break;
            case "single_biome_surface": presetKey = WorldPresets.SINGLE_BIOME_SURFACE; break;
            default: throw new IllegalArgumentException(worldType);
        }
        net.minecraft.world.level.levelgen.WorldDimensions dims =
            wg.lookupOrThrow(Registries.WORLD_PRESET).getOrThrow(presetKey).value().createWorldDimensions();
        if (worldType.equals("flat") && (!flatPreset.isEmpty() || !flatLayers.isEmpty())) {
            var biomes = wg.lookupOrThrow(Registries.BIOME);
            var sets = wg.lookupOrThrow(Registries.STRUCTURE_SET);
            var feats = wg.lookupOrThrow(Registries.PLACED_FEATURE);
            var blocks = wg.lookupOrThrow(Registries.BLOCK);
            net.minecraft.world.level.levelgen.flat.FlatLevelGeneratorSettings settings;
            if (!flatPreset.isEmpty()) {
                settings = wg.lookupOrThrow(Registries.FLAT_LEVEL_GENERATOR_PRESET)
                    .getOrThrow(net.minecraft.resources.ResourceKey.create(
                        Registries.FLAT_LEVEL_GENERATOR_PRESET,
                        net.minecraft.resources.Identifier.withDefaultNamespace(flatPreset)))
                    .value().settings();
            } else {
                settings = net.minecraft.world.level.levelgen.flat.FlatLevelGeneratorSettings
                    .getDefault(biomes, sets, feats);
            }
            if (!flatLayers.isEmpty()) {
                settings = net.minecraft.client.gui.screens.PresetFlatWorldScreen.fromString(
                    blocks, biomes, sets, feats, flatLayers, settings);
            }
            dims = dims.replaceOverworldGenerator(wg,
                new net.minecraft.world.level.levelgen.FlatLevelSource(settings));
        } else if (worldType.equals("single_biome_surface") && !singleBiome.isEmpty()) {
            var biomes = wg.lookupOrThrow(Registries.BIOME);
            var biome = biomes.getOrThrow(net.minecraft.resources.ResourceKey.create(
                Registries.BIOME, net.minecraft.resources.Identifier.parse(singleBiome)));
            var noiseSettings = wg.lookupOrThrow(Registries.NOISE_SETTINGS).getOrThrow(
                net.minecraft.world.level.levelgen.NoiseGeneratorSettings.OVERWORLD);
            dims = dims.replaceOverworldGenerator(wg,
                new net.minecraft.world.level.levelgen.NoiseBasedChunkGenerator(
                    new net.minecraft.world.level.biome.FixedBiomeSource(biome), noiseSettings));
        }
        return dims;
    }

    public static void main(String[] args) {
        int radius = -1;  // -1 means not set
        int centerX = 0;
        int centerZ = 0;
        boolean singleMode = false;
        int singleChunkX = 0;
        int singleChunkZ = 0;
        String outputPath = null;
        boolean verbose = true;
        String phases = "all";

        // Parse arguments
        for (int i = 0; i < args.length; i++) {
            switch (args[i]) {
                case "--radius":
                    radius = Integer.parseInt(args[++i]);
                    break;
                case "--single":
                    singleMode = true;
                    singleChunkX = Integer.parseInt(args[++i]);
                    singleChunkZ = Integer.parseInt(args[++i]);
                    break;
                case "--center":
                    centerX = Integer.parseInt(args[++i]);
                    centerZ = Integer.parseInt(args[++i]);
                    break;
                case "--output":
                    outputPath = args[++i];
                    break;
                case "--quiet":
                    verbose = false;
                    break;
                case "--phases":
                    phases = args[++i];
                    break;
                case "--seed":
                    SEED = Long.parseLong(args[++i]);
                    break;
                case "--dump-full":
                    dumpFull = true;
                    break;
                case "--block-trace":
                    blockTracePath = args[++i];
                    break;
                // FEATURE TRACING OPTIONS (remove after fixing parity)
                case "--trace-features":
                    traceFeatures = true;
                    break;
                case "--trace-output":
                    traceOutputPath = args[++i];
                    break;
                case "--trace-feature-filter":
                    traceFeatureFilter = args[++i];
                    break;
                case "--dimension": {
                    dimension = args[++i];
                    if (!dimension.equals("overworld") && !dimension.equals("nether")
                        && !dimension.equals("end")) {
                        System.err.println("Unknown --dimension: " + dimension);
                        System.exit(2);
                    }
                    break;
                }
                case "--world-type": {
                    worldType = args[++i];
                    if (!worldType.equals("default") && !worldType.equals("flat")
                        && !worldType.equals("large_biomes") && !worldType.equals("amplified")
                        && !worldType.equals("single_biome_surface")) {
                        System.err.println("Unknown --world-type: " + worldType);
                        System.exit(2);
                    }
                    break;
                }
                case "--flat-preset":
                    flatPreset = args[++i];
                    break;
                case "--flat-layers":
                    flatLayers = args[++i];
                    break;
                case "--single-biome":
                    singleBiome = args[++i];
                    break;
                case "--trace-watch": {
                    String[] parts = args[++i].split(",");
                    traceWatchPos = new BlockPos(
                        Integer.parseInt(parts[0]),
                        Integer.parseInt(parts[1]),
                        Integer.parseInt(parts[2]));
                    break;
                }
                case "--trace-structures":
                    traceStructures = true;
                    break;
                case "--structure-trace-output":
                    structureTraceOutputPath = args[++i];
                    break;
                case "--trace-placements":
                    placementTraceOutputPath = args[++i];
                    break;
                case "--placement-radius":
                    placementRadius = Integer.parseInt(args[++i]);
                    break;
                case "--dump-block-entities":
                    dumpBlockEntities = true;
                    break;
                case "--dump-feature-order":
                    featureOrderOutput = args[++i];
                    break;
                default:
                    // Unknown args must fail loudly (mirrors the C++ harness):
                    // silently ignoring a mistyped flag compares mismatched configs.
                    System.err.println("Error: unknown argument '" + args[i] + "'");
                    System.exit(2);
            }
        }

        // Validate arguments
        if (!singleMode && radius < 0) {
            System.err.println("Error: Must specify either --radius or --single");
            System.err.println();
            System.err.println("Usage:");
            System.err.println("  MinecraftAsyncChunkTest --radius <radius> [options]");
            System.err.println("  MinecraftAsyncChunkTest --single <chunkX> <chunkZ> [options]   (detailed per-block output)");
            System.err.println();
            System.err.println("Options:");
            System.err.println("  --radius <n>       Radius of chunks to generate");
            System.err.println("  --single <x> <z>   Generate single chunk with detailed per-block output");
            System.err.println("  --center <x> <z>   Center chunk position (default: 0 0)");
            System.err.println("  --output <file>    Output file path");
            System.err.println("  --phases <spec>    Phase configuration (default: all)");
            System.err.println("  --seed <long>      World seed (default: 12345)");
            System.err.println("  --dump-full        Radius mode: write canonical full-state dump (FORMAT.md) instead of histograms");
            System.err.println("  --quiet            Suppress progress output");
            System.err.println("  --trace-features   Enable feature placement tracing (for parity debugging)");
            System.err.println("  --trace-output <f> Output file for feature trace (default: stderr)");
            System.err.println("  --trace-feature-filter <name> Trace only the matching placed feature name");
            System.err.println("  --trace-structures Dump structure references and starts affecting the generated chunks");
            System.err.println("  --structure-trace-output <f> Output file for structure trace (default: /tmp/java_structure_trace.txt)");
            System.exit(1);
        }

        // Set default output path
        if (outputPath == null) {
            if (singleMode) {
                outputPath = "tests/output/java_single_" + singleChunkX + "_" + singleChunkZ + ".txt";
            } else {
                outputPath = "tests/output/java_async_minecraft.txt";
            }
        }

        // Store phases for later configuration (after Bootstrap)
        final String finalPhases = phases;

        int totalChunks = singleMode ? 1 : (radius * 2 + 1) * (radius * 2 + 1);

        System.out.println("=== Java Minecraft Async Chunk Generation Test ===");
        System.out.println(">>> USES FULL HeadlessParityServer + ServerChunkCache.getChunk() <<<");
        System.out.println();
        System.out.println("Seed: " + SEED);
        if (singleMode) {
            System.out.println("Mode: SINGLE CHUNK (detailed per-block output)");
            System.out.println("Chunk: (" + singleChunkX + ", " + singleChunkZ + ")");
        } else {
            System.out.println("Center: (" + centerX + ", " + centerZ + ")");
            System.out.println("Radius: " + radius);
        }
        System.out.println("Total chunks: " + totalChunks);
        System.out.println("Phases: " + phases);
        System.out.println();

        try {
            if (singleMode) {
                runSingleChunkTest(singleChunkX, singleChunkZ, outputPath, verbose, finalPhases);
            } else {
                runFullPipelineTest(centerX, centerZ, radius, outputPath, verbose, finalPhases);
            }
        } catch (Exception e) {
            System.err.println("Error: " + e.getMessage());
            e.printStackTrace();
            System.exit(1);
        }
    }

    /**
     * --dump-feature-order: the generator's FeatureSorter result, one line per
     * placed feature: "<step> <globalIndex> <placed feature id>". The global
     * index is what setFeatureSeed(decorationSeed, index, step) is keyed on.
     */
    @SuppressWarnings("unchecked")
    private static void dumpFeatureOrder(ServerLevel level, String path) throws Exception {
        net.minecraft.world.level.chunk.ChunkGenerator generator = level.getChunkSource().getGenerator();
        java.lang.reflect.Field field = net.minecraft.world.level.chunk.ChunkGenerator.class.getDeclaredField("featuresPerStep");
        field.setAccessible(true);
        java.util.function.Supplier<List<net.minecraft.world.level.biome.FeatureSorter.StepFeatureData>> supplier =
            (java.util.function.Supplier<List<net.minecraft.world.level.biome.FeatureSorter.StepFeatureData>>) field.get(generator);
        net.minecraft.core.Registry<net.minecraft.world.level.levelgen.placement.PlacedFeature> registry =
            level.registryAccess().lookupOrThrow(net.minecraft.core.registries.Registries.PLACED_FEATURE);
        StringBuilder out = new StringBuilder();
        out.append("# possibleBiomes:");
        for (var biome : generator.getBiomeSource().possibleBiomes()) {
            out.append(' ').append(biome.unwrapKey().map(k -> k.identifier().toString()).orElse("?"));
        }
        out.append('\n');
        List<net.minecraft.world.level.biome.FeatureSorter.StepFeatureData> steps = supplier.get();
        for (int step = 0; step < steps.size(); ++step) {
            List<net.minecraft.world.level.levelgen.placement.PlacedFeature> features = steps.get(step).features();
            for (int index = 0; index < features.size(); ++index) {
                var feature = features.get(index);
                String name = registry.getResourceKey(feature).map(k -> k.identifier().toString()).orElse("<inline>");
                out.append(step).append(' ').append(index).append(' ').append(name).append('\n');
            }
        }
        Files.writeString(Path.of(path), out.toString());
        System.out.println("  Feature order written to " + path);
    }

    /**
     * Run the test using the FULL Minecraft server pipeline.
     * This is identical to how Minecraft generates chunks in actual gameplay.
     */
    private static void runFullPipelineTest(int centerX, int centerZ, int radius, String outputPath, boolean verbose, String phases) throws Exception {
        int totalChunks = (radius * 2 + 1) * (radius * 2 + 1);
        Path tempWorldPath = null;
        MinecraftServer server = null;

        try {
            // ========== Step 1: Bootstrap Minecraft ==========
            System.out.println("Step 1: Bootstrapping Minecraft...");
            SharedConstants.tryDetectVersion();
            CrashReport.preload();
            Bootstrap.bootStrap();
            Bootstrap.validate();
            Util.startTimerHackThread();
            System.out.println("  Bootstrap complete.");

            // Configure phases AFTER Bootstrap (ChunkStatus requires registries)
            configurePhaseSpec(phases);
            System.out.println("  Phase config: " + phasesDescription);
            System.out.println("  Target status: " + targetStatus);
            System.out.println("  Generate structures: " + generateStructures);

            // Initialize feature tracing if enabled (REMOVE AFTER FIXING PARITY)
            initFeatureTracing();

            // ========== Step 2: Create temporary world storage ==========
            System.out.println("Step 2: Creating temporary world storage...");
            tempWorldPath = Files.createTempDirectory("mc_async_test_");
            System.out.println("  Temp path: " + tempWorldPath);

            LevelStorageSource levelStorageSource = LevelStorageSource.createDefault(tempWorldPath);
            LevelStorageSource.LevelStorageAccess storage = levelStorageSource.validateAndCreateAccess("test_world");
            System.out.println("  Storage created.");

            // ========== Step 3: Create pack repository ==========
            System.out.println("Step 3: Creating pack repository...");
            PackRepository packRepository = ServerPacksSource.createPackRepository(storage);
            packRepository.reload();
            System.out.println("  Packs loaded: " + packRepository.getAvailableIds());

            // ========== Step 4: Load world data via WorldLoader ==========
            System.out.println("Step 4: Loading world data via WorldLoader...");

            WorldDataConfiguration dataConfig = new WorldDataConfiguration(
                new DataPackConfig(List.of("vanilla"), List.of()),
                FeatureFlags.DEFAULT_FLAGS
            );

            WorldLoader.PackConfig packConfig = new WorldLoader.PackConfig(
                packRepository, dataConfig, false, true
            );

            WorldLoader.InitConfig initConfig = new WorldLoader.InitConfig(
                packConfig, Commands.CommandSelection.DEDICATED, LevelBasedPermissionSet.GAMEMASTER
            );

            // Load registries and create new world
            WorldStem worldStem = Util.blockUntilDone(executor -> WorldLoader.load(
                initConfig,
                context -> {
                    // Create new world with specified seed
                    Registry<LevelStem> datapackDimensions = context.datapackDimensions().lookupOrThrow(Registries.LEVEL_STEM);

                    // 26.3: difficulty/hardcore are one DifficultySettings
                    // record, game rules go to the server, and the seed
                    // options travel in WorldGenSettings next to the level
                    // data (GameTestServer.create is the reference pattern).
                    LevelSettings levelSettings = new LevelSettings(
                        "test_world",
                        GameType.CREATIVE,
                        new LevelSettings.DifficultySettings(Difficulty.NORMAL, false, false),
                        true,   // allowCommands
                        context.dataConfiguration()
                    );

                    WorldOptions worldOptions = new WorldOptions(SEED, generateStructures, false);  // seed, generateStructures, bonusChest
                    WorldDimensions dimensions = createWorldDimensionsForType(context.datapackWorldRegistries());
                    WorldDimensions.Complete finalDimensions = dimensions.bake(datapackDimensions);

                    PrimaryLevelData levelData = new PrimaryLevelData(levelSettings, finalDimensions.specialWorldProperty(), finalDimensions.lifecycle());
                    // Mark initialized so MinecraftServer skips the initial spawn
                    // search, which fully generates chunks around spawn with
                    // parallel FEATURES steps - verified nondeterministic (clay/
                    // moss/sculk cross-chunk reads depend on scheduling).
                    levelData.setInitialized(true);
                    return new WorldLoader.DataLoadOutput<>(
                        new LevelDataAndDimensions.WorldDataAndGenSettings(levelData, new WorldGenSettings(worldOptions, dimensions)),
                        finalDimensions.dimensionsRegistryAccess()
                    );
                },
                WorldStem::new,
                Util.backgroundExecutor(),
                executor
            )).get();

            System.out.println("  WorldStem created successfully.");
            if (traceFeatures && (traceFeatureFilter != null || traceWatchPos != null)) {
                installLiveFeatureTracing(worldStem);
            }

            // ========== Step 6: Create and start headless server ==========
            System.out.println("Step 6: Creating HeadlessParityServer (GameTestServer pattern, no network bind)...");

            // Use MinecraftServer.spin() pattern - this is EXACTLY how Minecraft does it
            final AtomicBoolean testComplete = new AtomicBoolean(false);
            final AtomicLong testStartTime = new AtomicLong(0);
            final AtomicLong testEndTime = new AtomicLong(0);
            final Map<ChunkPos, ChunkAccess> generatedChunks = new ConcurrentHashMap<>();
            final AtomicInteger completedCount = new AtomicInteger(0);
            final int finalRadius = radius;
            final int finalCenterX = centerX;
            final int finalCenterZ = centerZ;
            final boolean finalVerbose = verbose;

            // Create temporary storage references for lambda
            final LevelStorageSource.LevelStorageAccess finalStorage = storage;
            final PackRepository finalPackRepository = packRepository;
            final WorldStem finalWorldStem = worldStem;

            System.out.println("  Launching server via MinecraftServer.spin()...");
            server = MinecraftServer.spin((thread) -> new HeadlessParityServer(
                thread,
                finalStorage,
                finalPackRepository,
                finalWorldStem
            ));

            // Wait for server to be ready
            System.out.println("  Waiting for server to initialize...");
            while (!server.isReady()) {
                Thread.sleep(100);
                if (!server.isRunning()) {
                    throw new RuntimeException("Server stopped during initialization");
                }
            }
            System.out.println("  Server is ready!");

            // ========== Step 7: Get ServerLevel and ServerChunkCache ==========
            System.out.println("Step 7: Getting ServerLevel and ServerChunkCache...");
            ServerLevel level = getTargetLevel(server);
            if (level == null) {
                throw new RuntimeException("Failed to get overworld");
            }
            ServerChunkCache chunkCache = level.getChunkSource();
            System.out.println("  Got ServerChunkCache: " + chunkCache.getClass().getName());
            System.out.println("  Shared spawn: " + level.getRespawnData().pos());

            if (featureOrderOutput != null) {
                dumpFeatureOrder(level, featureOrderOutput);
                server.halt(true);
                return;
            }

            if (blockTracePath != null) {
                installBlockTrace(level);
            }

            // Install live feature wrappers before generation so tracing reflects
            // the actual async execution context rather than a post-generation replay.
            if (traceFeatures && (traceFeatureFilter != null || traceWatchPos != null)) {
                installLiveFeatureTracing(level);
            }

            // ========== Step 8: Request chunks through FULL pipeline ==========
            System.out.println();
            System.out.println("Step 8: Requesting " + totalChunks + " chunks through ServerChunkCache.getChunk()...");
            System.out.println("  This goes through: DistanceManager -> ChunkMap -> ChunkTaskDispatcher -> etc.");
            System.out.println();

            // Schedule chunk generation on the server's main thread
            final MinecraftServer finalServer = server;
            CompletableFuture<Void> chunkGenerationFuture = new CompletableFuture<>();

            server.execute(() -> {
                try {
                    testStartTime.set(System.currentTimeMillis());
                    int lastReported = 0;

                    java.util.Set<Long> decorOrderSeen = new java.util.HashSet<>();
                    for (int z = -finalRadius; z <= finalRadius; z++) {
                        for (int x = -finalRadius; x <= finalRadius; x++) {
                            int cx = finalCenterX + x;
                            int cz = finalCenterZ + z;

                            // THIS IS THE ACTUAL MINECRAFT CHUNK LOADING FLOW!
                            // ServerChunkCache.getChunk() goes through:
                            // 1. Cache check
                            // 2. getChunkFutureMainThread() -> adds ticket
                            // 3. runDistanceManagerUpdates()
                            // 4. ChunkHolder.scheduleChunkGenerationTask()
                            // 5. ChunkMap.scheduleGenerationTask()
                            // 6. ChunkTaskDispatcher.submit() -> ConsecutiveExecutor
                            // 7. ChunkGenerationTask runs through all 12 statuses
                            // 8. managedBlock() pumps tasks until complete
                            ChunkAccess chunk = chunkCache.getChunk(cx, cz, targetStatus, true);

                            if (chunk != null) {
                                generatedChunks.put(new ChunkPos(cx, cz), chunk);
                                // Trace features if enabled (REMOVE AFTER FIXING PARITY)
                                // CAUTION: this replay calls level.getChunk ->
                                // FULL requests -> ring-1 chunks decorate early
                                // (a wavefront the real run does NOT have).
                                // NO_TRACE_REPLAY=1 skips it for clean live traces.
                                if (traceFeatures && System.getenv("NO_TRACE_REPLAY") == null) {
                                    traceChunkFeatures(level, cx, cz);
                                }
                            }

                            // WATCH_SCAN: after each request, non-forcing read
                            // of the --trace-watch position; a change brackets
                            // the write to this request's generation work.
                            if (traceWatchPos != null && System.getenv("WATCH_SCAN") != null) {
                                ChunkAccess wc = chunkCache.getChunk(
                                    traceWatchPos.getX() >> 4, traceWatchPos.getZ() >> 4,
                                    net.minecraft.world.level.chunk.status.ChunkStatus.TERRAIN, false);
                                String stateNow = wc == null ? "(chunk not ready)"
                                    : serializeState(wc.getBlockState(traceWatchPos));
                                if (!stateNow.equals(lastWatchScanState)) {
                                    System.out.println("WATCHSCAN afterRequest " + cx + " " + cz
                                        + " -> " + stateNow);
                                    lastWatchScanState = stateNow;
                                }
                            }

                            // STATUS_SCAN: after each request, report chunks
                            // newly at persisted status >= FEATURES via the
                            // ChunkMap holders (real status, no ticket-path
                            // false negatives like the non-forcing getChunk
                            // probe).
                            if (System.getenv("STATUS_SCAN") != null) {
                                try {
                                    net.minecraft.server.level.ChunkMap chunkMap = chunkCache.chunkMap;
                                    java.lang.reflect.Field mapField =
                                        net.minecraft.server.level.ChunkMap.class.getDeclaredField("updatingChunkMap");
                                    mapField.setAccessible(true);
                                    it.unimi.dsi.fastutil.longs.Long2ObjectLinkedOpenHashMap<net.minecraft.server.level.ChunkHolder> holders =
                                        (it.unimi.dsi.fastutil.longs.Long2ObjectLinkedOpenHashMap<net.minecraft.server.level.ChunkHolder>) mapField.get(chunkMap);
                                    for (net.minecraft.server.level.ChunkHolder holder : holders.values()) {
                                        ChunkAccess ha = holder.getLatestChunk();
                                        if (ha == null) continue;
                                        net.minecraft.world.level.chunk.status.ChunkStatus st = ha.getPersistedStatus();
                                        if (!st.isOrAfter(net.minecraft.world.level.chunk.status.ChunkStatus.FEATURES)) continue;
                                        long key = ChunkPos.pack(holder.getPos().x(), holder.getPos().z());
                                        if (!decorOrderSeen.add(key)) continue;
                                        System.out.println("STATUS_SCAN " + holder.getPos().x() + " " + holder.getPos().z()
                                            + " reached " + st + " afterRequest " + cx + " " + cz);
                                    }
                                } catch (Exception e) {
                                    System.out.println("STATUS_SCAN error: " + e);
                                }
                            }

                            // DECOR_ORDER_PROBE: report chunks newly at
                            // >= FEATURES after each request (execution-order
                            // ground truth at request granularity; the
                            // feature-trace order is a post-hoc replay).
                            if (System.getenv("DECOR_ORDER_PROBE") != null) {
                                for (int pz = -finalRadius - 2; pz <= finalRadius + 2; pz++) {
                                    for (int px = -finalRadius - 2; px <= finalRadius + 2; px++) {
                                        int qx = finalCenterX + px;
                                        int qz = finalCenterZ + pz;
                                        long key = ChunkPos.pack(qx, qz);
                                        if (decorOrderSeen.contains(key)) continue;
                                        ChunkAccess probe = chunkCache.getChunk(
                                            qx, qz, net.minecraft.world.level.chunk.status.ChunkStatus.FEATURES, false);
                                        if (probe != null) {
                                            decorOrderSeen.add(key);
                                            System.out.println("DECOR_ORDER " + qx + " " + qz
                                                + " afterRequest " + cx + " " + cz);
                                        }
                                    }
                                }
                            }

                            int completed = completedCount.incrementAndGet();

                            // Progress reporting
                            if (finalVerbose && completed / 10 != lastReported / 10) {
                                lastReported = completed;
                                double elapsed = (System.currentTimeMillis() - testStartTime.get()) / 1000.0;
                                double rate = elapsed > 0 ? completed / elapsed : 0;
                                System.out.printf("Progress: %d/%d - %.1f chunks/sec%n", completed, totalChunks, rate);
                            }
                        }
                    }

                    testEndTime.set(System.currentTimeMillis());
                    testComplete.set(true);
                    chunkGenerationFuture.complete(null);
                } catch (Exception e) {
                    chunkGenerationFuture.completeExceptionally(e);
                }
            });

            // Wait for chunk generation to complete
            chunkGenerationFuture.get(600, TimeUnit.SECONDS);  // 10 minute timeout

            double totalTime = (testEndTime.get() - testStartTime.get()) / 1000.0;
            double rate = totalChunks / totalTime;

            System.out.println();
            System.out.println("All chunks generated!");

            if (traceStructures) {
                String structureOutput = structureTraceOutputPath != null
                    ? structureTraceOutputPath
                    : "/tmp/java_structure_trace.txt";
                System.out.println("Step 8.5: Tracing structures...");
                traceStructures(level, generatedChunks, structureOutput);
                System.out.println("Structure trace: " + structureOutput);
            }

            if (placementTraceOutputPath != null) {
                System.out.println("Step 8.6: Tracing structure placements...");
                int scanRadius = placementRadius >= 0 ? placementRadius : radius;
                tracePlacements(level, centerX, centerZ, scanRadius, placementTraceOutputPath);
                System.out.println("Placement trace: " + placementTraceOutputPath);
            }

            // ========== Step 9: Write output ==========
            System.out.println("Step 9: Writing output...");
            canonicalStructuresRegistry = level.registryAccess().lookupOrThrow(Registries.STRUCTURE);
            canonicalRegistryAccess = level.registryAccess();
            new File(outputPath).getParentFile().mkdirs();
            try (PrintWriter out = new PrintWriter(new FileWriter(outputPath))) {
                // Deterministic order matching FORMAT.md / C++ std::map<pair>:
                // (chunkX, chunkZ) ascending.
                List<Map.Entry<ChunkPos, ChunkAccess>> sortedChunks = new ArrayList<>(generatedChunks.entrySet());
                sortedChunks.sort(Comparator
                    .comparingInt((Map.Entry<ChunkPos, ChunkAccess> e) -> e.getKey().x())
                    .thenComparingInt(e -> e.getKey().z()));

                if (dumpFull) {
                    writeCanonicalHeader(out, "radius");
                    for (Map.Entry<ChunkPos, ChunkAccess> entry : sortedChunks) {
                        writeCanonicalChunk(out, entry.getValue(), entry.getKey().x(), entry.getKey().z());
                    }
                } else {
                    out.println("# Java Minecraft Async Chunk Test Output");
                    out.println("# USES: HeadlessParityServer + ServerChunkCache.getChunk()");
                    out.println("# Pipeline: DistanceManager -> ChunkMap -> ChunkTaskDispatcher -> ConsecutiveExecutor");
                    out.println("# Seed: " + SEED);
                    out.println("# Center: (" + centerX + ", " + centerZ + ")");
                    out.println("# Radius: " + radius);
                    out.println("# Phases: " + phasesDescription);
                    out.println("# Target: " + targetStatus);
                    if (!dimension.equals("overworld")) out.println("# Dimension: " + dimension);
                    out.println("# Structures: " + generateStructures);
                    out.println("# Format: chunkX,chunkZ,airBlocks,nonAirBlocks,block1:count1,...");
                    out.println();

                    for (Map.Entry<ChunkPos, ChunkAccess> entry : sortedChunks) {
                        writeChunkData(out, entry.getValue(), entry.getKey().x(), entry.getKey().z());
                    }
                }
            }

            // ========== Step 10: Print results ==========
            System.out.println();
            System.out.println("=== Java Minecraft FULL PIPELINE Test Complete ===");
            System.out.printf("Total time: %.2f seconds%n", totalTime);
            System.out.printf("Rate: %.1f chunks/sec%n", rate);
            System.out.println("Chunks generated: " + generatedChunks.size());
            System.out.println("Output: " + outputPath);

            // ========== Step 11: Shutdown server ==========
            System.out.println();
            System.out.println("Step 11: Shutting down server...");
            // Reference: Main.java line 213 - halt(true) waits for shutdown
            server.halt(true);

            // Close feature tracing (REMOVE AFTER FIXING PARITY)
            closeFeatureTracing();

        } finally {
            // Clean up temp directory
            if (tempWorldPath != null) {
                if (blockTraceWriter != null) blockTraceWriter.flush();
                System.out.println("Cleaning up temp directory...");
                deleteDirectory(tempWorldPath.toFile());
            }
        }
    }

    private static void writeChunkData(PrintWriter out, ChunkAccess chunk, int chunkX, int chunkZ) {
        Map<String, Integer> blockCounts = new TreeMap<>();
        int airCount = 0;
        int nonAirCount = 0;

        for (int y = MIN_Y; y < MAX_Y; y++) {
            for (int z = 0; z < 16; z++) {
                for (int x = 0; x < 16; x++) {
                    BlockPos pos = new BlockPos(chunkX * 16 + x, y, chunkZ * 16 + z);
                    BlockState state = chunk.getBlockState(pos);
                    Block block = state.getBlock();

                    if (state.isAir()) {
                        airCount++;
                    } else {
                        nonAirCount++;
                        String blockName = BuiltInRegistries.BLOCK.getKey(block).toString();
                        blockCounts.merge(blockName, 1, Integer::sum);
                    }
                }
            }
        }

        StringBuilder sb = new StringBuilder();
        sb.append(chunkX).append(",").append(chunkZ).append(",");
        sb.append(airCount).append(",").append(nonAirCount);

        // Sort by count descending
        List<Map.Entry<String, Integer>> sorted = new ArrayList<>(blockCounts.entrySet());
        sorted.sort((a, b) -> b.getValue().compareTo(a.getValue()));

        for (Map.Entry<String, Integer> entry : sorted) {
            sb.append(",").append(entry.getKey()).append(":").append(entry.getValue());
        }

        out.println(sb.toString());
    }

    /**
     * Run single chunk test with detailed per-block output.
     * Output format matches C++ async_chunk_test --single mode.
     */
    private static void runSingleChunkTest(int chunkX, int chunkZ, String outputPath, boolean verbose, String phases) throws Exception {
        Path tempWorldPath = null;
        MinecraftServer server = null;

        try {
            // ========== Step 1: Bootstrap Minecraft ==========
            System.out.println("Step 1: Bootstrapping Minecraft...");
            SharedConstants.tryDetectVersion();
            CrashReport.preload();
            Bootstrap.bootStrap();
            Bootstrap.validate();
            Util.startTimerHackThread();
            System.out.println("  Bootstrap complete.");

            // Configure phases AFTER Bootstrap
            configurePhaseSpec(phases);
            System.out.println("  Phase config: " + phasesDescription);
            System.out.println("  Target status: " + targetStatus);

            // Initialize feature tracing if enabled (REMOVE AFTER FIXING PARITY)
            initFeatureTracing();

            // ========== Step 2: Create temporary world storage ==========
            System.out.println("Step 2: Creating temporary world storage...");
            tempWorldPath = Files.createTempDirectory("mc_single_test_");
            System.out.println("  Temp path: " + tempWorldPath);

            LevelStorageSource levelStorageSource = LevelStorageSource.createDefault(tempWorldPath);
            LevelStorageSource.LevelStorageAccess storage = levelStorageSource.validateAndCreateAccess("test_world");
            System.out.println("  Storage created.");

            // ========== Step 3: Create pack repository ==========
            System.out.println("Step 3: Creating pack repository...");
            PackRepository packRepository = ServerPacksSource.createPackRepository(storage);
            packRepository.reload();
            System.out.println("  Packs loaded: " + packRepository.getAvailableIds());

            // ========== Step 4: Load world data via WorldLoader ==========
            System.out.println("Step 4: Loading world data via WorldLoader...");

            WorldDataConfiguration dataConfig = new WorldDataConfiguration(
                new DataPackConfig(List.of("vanilla"), List.of()),
                FeatureFlags.DEFAULT_FLAGS
            );

            WorldLoader.PackConfig packConfig = new WorldLoader.PackConfig(
                packRepository, dataConfig, false, true
            );

            WorldLoader.InitConfig initConfig = new WorldLoader.InitConfig(
                packConfig, Commands.CommandSelection.DEDICATED, LevelBasedPermissionSet.GAMEMASTER
            );

            WorldStem worldStem = Util.blockUntilDone(executor -> WorldLoader.load(
                initConfig,
                context -> {
                    Registry<LevelStem> datapackDimensions = context.datapackDimensions().lookupOrThrow(Registries.LEVEL_STEM);

                    // 26.3: difficulty/hardcore are one DifficultySettings
                    // record, game rules go to the server, and the seed
                    // options travel in WorldGenSettings next to the level
                    // data (GameTestServer.create is the reference pattern).
                    LevelSettings levelSettings = new LevelSettings(
                        "test_world",
                        GameType.CREATIVE,
                        new LevelSettings.DifficultySettings(Difficulty.NORMAL, false, false),
                        true,   // allowCommands
                        context.dataConfiguration()
                    );

                    WorldOptions worldOptions = new WorldOptions(SEED, generateStructures, false);  // seed, generateStructures, bonusChest
                    WorldDimensions dimensions = createWorldDimensionsForType(context.datapackWorldRegistries());
                    WorldDimensions.Complete finalDimensions = dimensions.bake(datapackDimensions);

                    PrimaryLevelData levelData = new PrimaryLevelData(levelSettings, finalDimensions.specialWorldProperty(), finalDimensions.lifecycle());
                    // Mark initialized so MinecraftServer skips the initial spawn
                    // search, which fully generates chunks around spawn with
                    // parallel FEATURES steps - verified nondeterministic (clay/
                    // moss/sculk cross-chunk reads depend on scheduling).
                    levelData.setInitialized(true);
                    return new WorldLoader.DataLoadOutput<>(
                        new LevelDataAndDimensions.WorldDataAndGenSettings(levelData, new WorldGenSettings(worldOptions, dimensions)),
                        finalDimensions.dimensionsRegistryAccess()
                    );
                },
                WorldStem::new,
                Util.backgroundExecutor(),
                executor
            )).get();

            System.out.println("  WorldStem created successfully.");
            if (traceFeatures && (traceFeatureFilter != null || traceWatchPos != null)) {
                installLiveFeatureTracing(worldStem);
            }

            // ========== Step 6: Create and start headless server ==========
            System.out.println("Step 6: Creating HeadlessParityServer (GameTestServer pattern, no network bind)...");

            final LevelStorageSource.LevelStorageAccess finalStorage = storage;
            final PackRepository finalPackRepository = packRepository;
            final WorldStem finalWorldStem = worldStem;

            System.out.println("  Launching server via MinecraftServer.spin()...");
            server = MinecraftServer.spin((thread) -> new HeadlessParityServer(
                thread,
                finalStorage,
                finalPackRepository,
                finalWorldStem
            ));

            System.out.println("  Waiting for server to initialize...");
            while (!server.isReady()) {
                Thread.sleep(100);
                if (!server.isRunning()) {
                    throw new RuntimeException("Server stopped during initialization");
                }
            }
            System.out.println("  Server is ready!");

            // ========== Step 7: Get ServerLevel and ServerChunkCache ==========
            System.out.println("Step 7: Getting ServerLevel and ServerChunkCache...");
            ServerLevel level = getTargetLevel(server);
            if (level == null) {
                throw new RuntimeException("Failed to get overworld");
            }
            ServerChunkCache chunkCache = level.getChunkSource();
            System.out.println("  Got ServerChunkCache: " + chunkCache.getClass().getName());
            System.out.println("  Shared spawn: " + level.getRespawnData().pos());

            // Install live feature wrappers before generation so tracing reflects
            // the actual async execution context rather than a post-generation replay.
            if (traceFeatures && (traceFeatureFilter != null || traceWatchPos != null)) {
                installLiveFeatureTracing(level);
            }

            // ========== Step 8: Request single chunk ==========
            System.out.println();
            System.out.println("Step 8: Requesting chunk (" + chunkX + ", " + chunkZ + ") through ServerChunkCache.getChunk()...");

            final int finalChunkX = chunkX;
            final int finalChunkZ = chunkZ;
            final AtomicReference<ChunkAccess> generatedChunk = new AtomicReference<>();
            CompletableFuture<Void> chunkGenerationFuture = new CompletableFuture<>();

            long startTime = System.currentTimeMillis();
            server.execute(() -> {
                try {
                    ChunkAccess chunk = chunkCache.getChunk(finalChunkX, finalChunkZ, targetStatus, true);
                    generatedChunk.set(chunk);
                    chunkGenerationFuture.complete(null);
                } catch (Exception e) {
                    chunkGenerationFuture.completeExceptionally(e);
                }
            });

            chunkGenerationFuture.get(300, TimeUnit.SECONDS);  // 5 minute timeout
            long endTime = System.currentTimeMillis();
            double totalTime = (endTime - startTime) / 1000.0;

            System.out.println("Chunk generated!");

            // Trace features if enabled (REMOVE AFTER FIXING PARITY)
            if (traceFeatures) {
                System.out.println("Tracing features...");
                final int fChunkX = chunkX;
                final int fChunkZ = chunkZ;
                final ServerLevel fLevel = level;
                server.execute(() -> traceChunkFeatures(fLevel, fChunkX, fChunkZ));
                Thread.sleep(500); // Give time for trace to complete
            }

            // ========== Step 9: Write detailed output ==========
            System.out.println("Step 9: Writing detailed per-block output...");
            canonicalStructuresRegistry = level.registryAccess().lookupOrThrow(Registries.STRUCTURE);
            canonicalRegistryAccess = level.registryAccess();
            new File(outputPath).getParentFile().mkdirs();
            try (PrintWriter out = new PrintWriter(new FileWriter(outputPath))) {
                writeCanonicalHeader(out, "single");
                writeCanonicalChunk(out, generatedChunk.get(), chunkX, chunkZ);
            }

            // ========== Step 10: Print results ==========
            System.out.println();
            System.out.println("=== Java Single Chunk Test Complete ===");
            System.out.printf("Total time: %.2f seconds%n", totalTime);
            System.out.println("Output: " + outputPath);

            // ========== Step 11: Shutdown server ==========
            System.out.println();
            System.out.println("Step 11: Shutting down server...");
            server.halt(true);

            // Close feature tracing (REMOVE AFTER FIXING PARITY)
            closeFeatureTracing();

        } finally {
            if (tempWorldPath != null) {
                System.out.println("Cleaning up temp directory...");
                deleteDirectory(tempWorldPath.toFile());
            }
        }
    }

    // =========================================================================
    // CANONICAL PARITY DUMP (see tests/parity/FORMAT.md)
    // Must stay byte-identical to the C++ writeCanonicalChunk() in
    // tests/parity/cpp/CppChunkGeneratorTest.cpp.
    // =========================================================================

    @SuppressWarnings("unchecked")
    private static <T extends Comparable<T>> String propertyValueName(Property<T> property, Comparable<?> value) {
        return property.getName((T) value);
    }

    /**
     * "minecraft:name[prop1=val1,prop2=val2]" with properties sorted
     * alphabetically by name. StateDefinition already stores properties in an
     * ImmutableSortedMap, but sort explicitly so the contract is visible.
     */
    private static String serializeState(BlockState state) {
        StringBuilder sb = new StringBuilder(BuiltInRegistries.BLOCK.getKey(state.getBlock()).toString());
        // 26.3: getValues() streams Property.Value records.
        List<Property.Value<?>> entries = new ArrayList<>(state.getValues().toList());
        if (!entries.isEmpty()) {
            entries.sort(Comparator.comparing(v -> v.property().getName()));
            sb.append('[');
            boolean first = true;
            for (Property.Value<?> v : entries) {
                if (!first) sb.append(',');
                first = false;
                sb.append(v.property().getName()).append('=').append(propertyValueName(v.property(), v.value()));
            }
            sb.append(']');
        }
        return sb.toString();
    }

    private static void writeCanonicalHeader(PrintWriter out, String mode) {
        out.println("# Java Canonical Parity Dump (see tests/parity/FORMAT.md)");
        out.println("# USES: HeadlessParityServer + ServerChunkCache.getChunk()");
        out.println("# Mode: " + mode);
        out.println("# Seed: " + SEED);
        out.println("# Phases: " + phasesDescription);
        out.println("# Target: " + targetStatus);
        if (!dimension.equals("overworld")) out.println("# Dimension: " + dimension);
    }

    // Structures registry for S/P/R canonical lines (set after server boot).
    private static Registry<Structure> canonicalStructuresRegistry = null;
    // Registry access for block-entity saving (set alongside the registry).
    private static RegistryAccess canonicalRegistryAccess = null;

    /**
     * Canonical single-line NBT per FORMAT.md "E block-entity lines".
     * Deterministic by construction: sorted compound keys, typed suffixes,
     * float/double as IEEE-754 bit patterns (NOT shortest-round-trip text).
     */
    private static void appendCanonicalNbt(StringBuilder sb, net.minecraft.nbt.Tag tag) {
        if (tag instanceof net.minecraft.nbt.CompoundTag compound) {
            appendCanonicalCompound(sb, compound, Collections.emptySet());
        } else if (tag instanceof net.minecraft.nbt.ListTag list) {
            sb.append('[');
            for (int i = 0; i < list.size(); i++) {
                if (i > 0) sb.append(',');
                appendCanonicalNbt(sb, list.get(i));
            }
            sb.append(']');
        } else if (tag instanceof net.minecraft.nbt.ByteTag b) {
            sb.append(b.value()).append('b');
        } else if (tag instanceof net.minecraft.nbt.ShortTag s) {
            sb.append(s.value()).append('s');
        } else if (tag instanceof net.minecraft.nbt.IntTag i) {
            sb.append(i.value());
        } else if (tag instanceof net.minecraft.nbt.LongTag l) {
            sb.append(l.value()).append('l');
        } else if (tag instanceof net.minecraft.nbt.FloatTag f) {
            sb.append("f0x").append(String.format("%08x", Float.floatToRawIntBits(f.value())));
        } else if (tag instanceof net.minecraft.nbt.DoubleTag d) {
            sb.append("d0x").append(String.format("%016x", Double.doubleToRawLongBits(d.value())));
        } else if (tag instanceof net.minecraft.nbt.StringTag str) {
            appendCanonicalString(sb, str.value());
        } else if (tag instanceof net.minecraft.nbt.ByteArrayTag ba) {
            sb.append("[B;");
            byte[] bytes = ba.getAsByteArray();
            for (int i = 0; i < bytes.length; i++) {
                if (i > 0) sb.append(',');
                sb.append(bytes[i]).append('b');
            }
            sb.append(']');
        } else if (tag instanceof net.minecraft.nbt.IntArrayTag ia) {
            sb.append("[I;");
            int[] ints = ia.getAsIntArray();
            for (int i = 0; i < ints.length; i++) {
                if (i > 0) sb.append(',');
                sb.append(ints[i]);
            }
            sb.append(']');
        } else if (tag instanceof net.minecraft.nbt.LongArrayTag la) {
            sb.append("[L;");
            long[] longs = la.getAsLongArray();
            for (int i = 0; i < longs.length; i++) {
                if (i > 0) sb.append(',');
                sb.append(longs[i]).append('l');
            }
            sb.append(']');
        } else {
            throw new IllegalArgumentException("unsupported NBT tag type: " + tag.getClass());
        }
    }

    private static void appendCanonicalCompound(StringBuilder sb, net.minecraft.nbt.CompoundTag compound,
                                                Set<String> dropKeys) {
        sb.append('{');
        List<String> keys = new ArrayList<>(compound.keySet());
        Collections.sort(keys);
        boolean first = true;
        for (String key : keys) {
            if (dropKeys.contains(key)) continue;
            if (!first) sb.append(',');
            first = false;
            if (key.matches("[A-Za-z0-9_.+-]+")) {
                sb.append(key);
            } else {
                appendCanonicalString(sb, key);
            }
            sb.append(':');
            appendCanonicalNbt(sb, compound.get(key));
        }
        sb.append('}');
    }

    private static void appendCanonicalString(StringBuilder sb, String value) {
        sb.append('"').append(value.replace("\\", "\\\\").replace("\"", "\\\"")).append('"');
    }

    /** E lines: y outer, then z, then x; top-level x/y/z keys dropped (FORMAT.md). */
    private static void writeCanonicalBlockEntities(PrintWriter out, ChunkAccess chunk, int chunkX, int chunkZ) {
        List<BlockPos> positions = new ArrayList<>(chunk.getBlockEntitiesPos());
        positions.sort(Comparator.comparingInt((BlockPos p) -> p.getY())
            .thenComparingInt(p -> p.getZ()).thenComparingInt(p -> p.getX()));
        Set<String> dropKeys = Set.of("x", "y", "z");
        for (BlockPos pos : positions) {
            net.minecraft.nbt.CompoundTag tag = chunk.getBlockEntityNbtForSaving(pos, canonicalRegistryAccess);
            if (tag == null) continue;
            StringBuilder sb = new StringBuilder();
            appendCanonicalCompound(sb, tag, dropKeys);
            out.println("E," + (pos.getX() - chunkX * 16) + "," + pos.getY() + ","
                + (pos.getZ() - chunkZ * 16) + "," + sb);
        }
    }

    /**
     * S/P/R lines per FORMAT.md "Section presence by phase spec":
     * S+P when structures are enabled; R additionally requires target >=
     * STRUCTURE_REFERENCES. Starts sorted by structure id; P lines in
     * piece-list order (build order - parity-load-bearing, never re-sorted);
     * R chunk lists sorted numerically by (cx, cz).
     */
    private static void writeCanonicalStructures(PrintWriter out, ChunkAccess chunk) {
        Registry<Structure> registry = canonicalStructuresRegistry;

        List<Map.Entry<Structure, StructureStart>> startEntries = new ArrayList<>(chunk.getAllStarts().entrySet());
        startEntries.sort(Comparator.comparing(e -> structureName(registry, e.getKey())));
        for (Map.Entry<Structure, StructureStart> entry : startEntries) {
            StructureStart start = entry.getValue();
            BoundingBox bb = start.getBoundingBox();
            out.println("S," + structureName(registry, entry.getKey()) + "," + start.getReferences()
                + "," + bb.minX() + "," + bb.minY() + "," + bb.minZ()
                + "," + bb.maxX() + "," + bb.maxY() + "," + bb.maxZ());
            for (StructurePiece piece : start.getPieces()) {
                BoundingBox pb = piece.getBoundingBox();
                Rotation rotation = piece.getRotation();
                out.println("P," + structureName(registry, entry.getKey())
                    + "," + BuiltInRegistries.STRUCTURE_PIECE.getKey(piece.getType())
                    + "," + (rotation == null ? "-" : rotation.name())
                    + "," + piece.getGenDepth()
                    + "," + pb.minX() + "," + pb.minY() + "," + pb.minZ()
                    + "," + pb.maxX() + "," + pb.maxY() + "," + pb.maxZ()
                    + "," + pieceDetail(piece));
            }
        }

        if (targetStatus.isOrAfter(ChunkStatus.STRUCTURE_REFERENCES)) {
            List<Map.Entry<Structure, LongSet>> refEntries = new ArrayList<>(chunk.getAllReferences().entrySet());
            refEntries.sort(Comparator.comparing(e -> structureName(registry, e.getKey())));
            for (Map.Entry<Structure, LongSet> entry : refEntries) {
                if (entry.getValue().isEmpty()) continue;
                List<ChunkPos> refs = new ArrayList<>();
                for (long packed : entry.getValue()) refs.add(ChunkPos.unpack(packed));
                refs.sort(Comparator.comparingInt((ChunkPos p) -> p.x()).thenComparingInt(p -> p.z()));
                StringBuilder sb = new StringBuilder("R," + structureName(registry, entry.getKey()));
                for (ChunkPos p : refs) sb.append(',').append(p.x()).append(';').append(p.z());
                out.println(sb);
            }
        }
    }

    /** Per-piece-kind identity string; grammar in FORMAT.md (no commas allowed). */
    private static String pieceDetail(StructurePiece piece) {
        if (piece instanceof TemplateStructurePiece template) {
            return readTemplateName(template);
        }
        if (piece instanceof PoolElementStructurePiece pool) {
            StructurePoolElement element = pool.getElement();
            String elementTypeId = String.valueOf(
                net.minecraft.core.registries.BuiltInRegistries.STRUCTURE_POOL_ELEMENT.getKey(element.getType()));
            String templateId = "-";
            if (element instanceof SinglePoolElement) {
                templateId = readSingleElementTemplateId((SinglePoolElement) element);
            }
            StringBuilder sb = new StringBuilder(elementTypeId).append(':').append(templateId)
                .append('#').append(pool.getGroundLevelDelta());
            List<JigsawJunction> junctions = pool.getJunctions();
            if (!junctions.isEmpty()) {
                sb.append("#J:");
                boolean first = true;
                for (JigsawJunction junction : junctions) {
                    if (!first) sb.append('|');
                    first = false;
                    sb.append(junction.getSourceX()).append(';').append(junction.getSourceGroundY())
                      .append(';').append(junction.getSourceZ()).append(';').append(junction.getDeltaY());
                }
            }
            return sb.toString();
        }
        return "-";
    }

    /** TemplateStructurePiece.templateName is protected; read reflectively (test harness only). */
    private static String readTemplateName(TemplateStructurePiece piece) {
        try {
            java.lang.reflect.Field field = TemplateStructurePiece.class.getDeclaredField("templateName");
            field.setAccessible(true);
            return String.valueOf(field.get(piece));
        } catch (ReflectiveOperationException e) {
            throw new RuntimeException("cannot read TemplateStructurePiece.templateName", e);
        }
    }

    /** SinglePoolElement.template is a protected Either<Identifier, StructureTemplate>. */
    private static String readSingleElementTemplateId(SinglePoolElement element) {
        try {
            java.lang.reflect.Field field = SinglePoolElement.class.getDeclaredField("template");
            field.setAccessible(true);
            Either<?, ?> either = (Either<?, ?>) field.get(element);
            return either.left().map(String::valueOf).orElse("-");
        } catch (ReflectiveOperationException e) {
            throw new RuntimeException("cannot read SinglePoolElement.template", e);
        }
    }

    private static void writeCanonicalChunk(PrintWriter out, ChunkAccess chunk, int chunkX, int chunkZ) {
        out.println("C," + chunkX + "," + chunkZ);

        // S/P/R lines (FORMAT.md): only when structures are enabled.
        if (generateStructures) {
            writeCanonicalStructures(out, chunk);
        }

        // B/Q require BIOMES+; H requires TERRAIN+ (FORMAT.md "Section presence").
        if (!targetStatus.isOrAfter(ChunkStatus.BIOMES)) return;

        // B lines: y outer, then z, then x. Plain air omitted; cave_air/void_air emitted.
        for (int y = MIN_Y; y < MAX_Y; y++) {
            for (int z = 0; z < 16; z++) {
                for (int x = 0; x < 16; x++) {
                    BlockPos pos = new BlockPos(chunkX * 16 + x, y, chunkZ * 16 + z);
                    BlockState state = chunk.getBlockState(pos);
                    if (state.getBlock() == net.minecraft.world.level.block.Blocks.AIR) continue;
                    out.println("B," + x + "," + y + "," + z + "," + serializeState(state));
                }
            }
        }

        // Q lines: absolute quart coords; qx outer, then qy, then qz.
        int quartMinX = net.minecraft.core.QuartPos.fromBlock(chunkX * 16);
        int quartMinZ = net.minecraft.core.QuartPos.fromBlock(chunkZ * 16);
        int quartMinY = net.minecraft.core.QuartPos.fromBlock(MIN_Y);
        int quartMaxY = net.minecraft.core.QuartPos.fromBlock(MAX_Y) - 1;
        for (int qx = quartMinX; qx < quartMinX + 4; qx++) {
            for (int qy = quartMinY; qy <= quartMaxY; qy++) {
                for (int qz = quartMinZ; qz < quartMinZ + 4; qz++) {
                    var biome = chunk.getNoiseBiome(qx, qy, qz);
                    out.println("Q," + qx + "," + qy + "," + qz + "," + biome.getRegisteredName());
                }
            }
        }

        // H lines: type outer (WS then OF), then z, then x. TERRAIN+ only (the worldgen heightmaps are written by TERRAIN).
        if (!targetStatus.isOrAfter(ChunkStatus.TERRAIN)) return;
        Heightmap.Types[] hmTypes = { Heightmap.Types.WORLD_SURFACE_WG, Heightmap.Types.OCEAN_FLOOR_WG };
        String[] hmLabels = { "WS", "OF" };
        for (int t = 0; t < hmTypes.length; t++) {
            for (int z = 0; z < 16; z++) {
                for (int x = 0; x < 16; x++) {
                    out.println("H," + hmLabels[t] + "," + x + "," + z + "," + chunk.getHeight(hmTypes[t], x, z));
                }
            }
        }

        // E lines: flag-gated, FEATURES+ only (FORMAT.md).
        if (dumpBlockEntities && targetStatus.isOrAfter(ChunkStatus.FEATURES)) {
            writeCanonicalBlockEntities(out, chunk, chunkX, chunkZ);
        }
    }

    private static void deleteDirectory(File dir) {
        if (dir == null || !dir.exists()) return;
        File[] files = dir.listFiles();
        if (files != null) {
            for (File file : files) {
                if (file.isDirectory()) {
                    deleteDirectory(file);
                } else {
                    file.delete();
                }
            }
        }
        dir.delete();
    }

    // =========================================================================
    // FEATURE TRACING CODE - FOR PARITY DEBUGGING
    // This entire section can be removed after fixing feature parity issues.
    // =========================================================================

    /**
     * Initialize feature tracing. Call after bootstrap but before chunk generation.
     */
    private static void initFeatureTracing() {
        if (!traceFeatures) return;

        try {
            liveTraceEventCounter.set(0);
            liveTraceInstalledTargets.clear();
            if (traceOutputPath != null) {
                new File(traceOutputPath).getParentFile().mkdirs();
                traceWriter = new PrintWriter(new FileWriter(traceOutputPath));
            } else {
                traceWriter = new PrintWriter(System.err, true);
            }
            traceWriter.println("# Java Feature Trace");
            traceWriter.println("# Seed: " + SEED);
            if (traceFeatureFilter != null) {
                traceWriter.println("# FeatureFilter: " + traceFeatureFilter);
            }
            traceWriter.println();
        } catch (Exception e) {
            System.err.println("Failed to initialize feature tracing: " + e.getMessage());
            traceFeatures = false;
        }
    }

    /**
     * Close feature tracing output.
     */
    private static void closeFeatureTracing() {
        if (traceWriter != null && traceOutputPath != null) {
            traceWriter.close();
        }
    }

    // --trace-features (live feature tracing, sculk/geode/vegetation-patch
    // shadows) was written against 26.1's feature API — ConfiguredFeature,
    // FeaturePlaceContext, levelgen.feature.configurations — which 26.3
    // replaced. The 26.1 code is kept in java/legacy/FeatureTracing_26_1.txt
    // until it is ported; asking for it fails instead of tracing nothing.
    /**
     * --block-trace: rebind every configured feature's registry holder to a
     * wrapper whose place() hands the feature a WorldGenLevel proxy that logs
     * each successful setBlock / setBlockAndUpdate. The PlacedFeature objects
     * stay untouched (BiomeFilter matches them by equality with the biome's
     * list); nested inline features inherit the proxy from their parent.
     * Labels are the top-level placed feature (WorldGenRegion's
     * currentlyGenerating) and the configured feature doing the write.
     */
    @SuppressWarnings("unchecked")
    private static void installBlockTrace(ServerLevel level) throws Exception {
        blockTraceWriter = new java.io.PrintWriter(new java.io.BufferedWriter(new java.io.FileWriter(blockTracePath)));
        net.minecraft.core.Registry<net.minecraft.world.level.levelgen.feature.Feature> registry =
            level.registryAccess().lookupOrThrow(net.minecraft.core.registries.Registries.FEATURE);
        java.lang.reflect.Field valueField = net.minecraft.core.Holder.Reference.class.getDeclaredField("value");
        valueField.setAccessible(true);
        int rebound = 0;
        for (net.minecraft.core.Holder.Reference<net.minecraft.world.level.levelgen.feature.Feature> ref :
                registry.listElements().toList()) {
            net.minecraft.world.level.levelgen.feature.Feature inner = ref.value();
            if (inner instanceof BlockTraceFeature) continue;
            valueField.set(ref, new BlockTraceFeature(inner, ref.key().identifier().toString()));
            ++rebound;
        }
        System.out.println("  Block trace installed (" + rebound + " configured features) -> " + blockTracePath);
    }

    private static java.lang.reflect.Field currentlyGeneratingField;

    private static String currentlyGenerating(net.minecraft.world.level.WorldGenLevel level) {
        try {
            if (currentlyGeneratingField == null) {
                currentlyGeneratingField = net.minecraft.server.level.WorldGenRegion.class.getDeclaredField("currentlyGenerating");
                currentlyGeneratingField.setAccessible(true);
            }
            if (level instanceof net.minecraft.server.level.WorldGenRegion) {
                Object supplier = currentlyGeneratingField.get(level);
                if (supplier instanceof java.util.function.Supplier<?> sup) return String.valueOf(sup.get());
            }
        } catch (Exception ignored) {
        }
        return "?";
    }

    private static final class BlockTraceFeature implements net.minecraft.world.level.levelgen.feature.Feature {
        private final net.minecraft.world.level.levelgen.feature.Feature inner;
        private final String label;

        BlockTraceFeature(net.minecraft.world.level.levelgen.feature.Feature inner, String label) {
            this.inner = inner;
            this.label = label;
        }

        @Override
        public com.mojang.serialization.MapCodec<? extends net.minecraft.world.level.levelgen.feature.Feature> codec() {
            return inner.codec();
        }

        @Override
        public java.util.stream.Stream<net.minecraft.core.Holder<net.minecraft.world.level.levelgen.feature.Feature>> getSubFeatures() {
            return inner.getSubFeatures();
        }

        @Override
        public boolean place(net.minecraft.world.level.WorldGenLevel level,
                             net.minecraft.world.level.chunk.ChunkGenerator chunkGenerator,
                             net.minecraft.util.RandomSource random, BlockPos origin) {
            if (java.lang.reflect.Proxy.isProxyClass(level.getClass())) {
                return inner.place(level, chunkGenerator, random, origin);   // nested: already traced
            }
            String top = currentlyGenerating(level);
            String want = System.getenv("MC_RNG_TRACE_FEATURE");
            String rngPath = System.getenv("MC_RNG_TRACE_FILE");
            if (want == null || rngPath == null || !label.equals(want)) {
                return inner.place(tracingLevel(level, top + " " + label, null), chunkGenerator, random, origin);
            }
            // Parity-debug: the C++ side's MC_RNG_TRACE_* stream (RNG nextInt /
            // nextFloat draws interleaved with SET lines), buffered per call so
            // concurrent chunk workers never interleave.
            StringBuilder buf = new StringBuilder();
            buf.append("RNGTRACE_BEGIN origin=").append(origin.getX()).append(',').append(origin.getY()).append(',').append(origin.getZ()).append('\n');
            try {
                return inner.place(tracingLevel(level, top + " " + label, buf), chunkGenerator, tracingRandom(random, buf), origin);
            } finally {
                buf.append("RNGTRACE_END\n");
                synchronized (BlockTraceFeature.class) {
                    try (java.io.FileWriter w = new java.io.FileWriter(rngPath, true)) {
                        w.write(buf.toString());
                    } catch (java.io.IOException e) {
                        throw new java.io.UncheckedIOException(e);
                    }
                }
            }
        }
    }

    private static net.minecraft.util.RandomSource tracingRandom(net.minecraft.util.RandomSource real, StringBuilder buf) {
        return (net.minecraft.util.RandomSource) java.lang.reflect.Proxy.newProxyInstance(
            net.minecraft.util.RandomSource.class.getClassLoader(),
            new Class<?>[]{net.minecraft.util.RandomSource.class},
            (proxy, method, args) -> {
                String m = method.getName();
                if (m.equals("nextIntBetweenInclusive") && args != null && args.length == 2) {
                    int min = (Integer) args[0], max = (Integer) args[1];
                    int r = real.nextInt(max - min + 1);
                    buf.append("RNG nextInt(").append(max - min + 1).append(")=").append(r).append('\n');
                    return min + r;
                }
                Object result;
                try {
                    result = method.invoke(real, args);
                } catch (java.lang.reflect.InvocationTargetException e) {
                    throw e.getCause();
                }
                if (m.equals("nextInt") && args != null && args.length == 1) {
                    buf.append("RNG nextInt(").append(args[0]).append(")=").append(result).append('\n');
                } else if (m.equals("nextFloat") && (args == null || args.length == 0)) {
                    buf.append("RNG nextFloat=").append(String.format("%.9e", (Float) result)).append('\n');
                }
                return result;
            });
    }

    private static net.minecraft.world.level.WorldGenLevel tracingLevel(net.minecraft.world.level.WorldGenLevel real, String label, StringBuilder rngBuf) {
        return (net.minecraft.world.level.WorldGenLevel) java.lang.reflect.Proxy.newProxyInstance(
            net.minecraft.world.level.WorldGenLevel.class.getClassLoader(),
            new Class<?>[]{net.minecraft.world.level.WorldGenLevel.class},
            (proxy, method, args) -> {
                String m = method.getName();
                boolean isSet = (m.equals("setBlock") || m.equals("setBlockAndUpdate")) && args != null
                    && args.length >= 2 && args[0] instanceof BlockPos && args[1] instanceof BlockState;
                BlockState before = isSet ? real.getBlockState((BlockPos) args[0]) : null;
                Object result;
                try {
                    result = method.invoke(real, args);
                } catch (java.lang.reflect.InvocationTargetException e) {
                    throw e.getCause();
                }
                if (isSet && !Boolean.FALSE.equals(result)) {
                    BlockPos pos = (BlockPos) args[0];
                    BlockState state = (BlockState) args[1];
                    if (rngBuf != null) {
                        rngBuf.append("SET ").append(pos.getX()).append(',').append(pos.getY()).append(',').append(pos.getZ())
                            .append(' ').append(net.minecraft.core.registries.BuiltInRegistries.BLOCK.getKey(before.getBlock()))
                            .append(" -> ").append(net.minecraft.core.registries.BuiltInRegistries.BLOCK.getKey(state.getBlock())).append('\n');
                    }
                    synchronized (blockTraceWriter) {
                        blockTraceWriter.println("BLOCK_SET " + label + " pos=" + pos.getX() + "," + pos.getY() + "," + pos.getZ()
                            + " old=" + net.minecraft.core.registries.BuiltInRegistries.BLOCK.getKey(before.getBlock())
                            + " new=" + net.minecraft.core.registries.BuiltInRegistries.BLOCK.getKey(state.getBlock()));
                    }
                }
                return result;
            });
    }

    private static UnsupportedOperationException featureTracingNotPorted() {
        return new UnsupportedOperationException(
            "--trace-features is not ported to MC 26.3's feature API yet (see java/legacy/FeatureTracing_26_1.txt)");
    }
    private static void installLiveFeatureTracing(ServerLevel level) { throw featureTracingNotPorted(); }
    private static void installLiveFeatureTracing(WorldStem worldStem) { throw featureTracingNotPorted(); }
    private static void traceChunkFeatures(ServerLevel level, int chunkX, int chunkZ) { throw featureTracingNotPorted(); }

    /**
     * Placement-layer trace: no chunk generation involved. Line grammar (all
     * deterministic, comparable with plain diff against the C++ equivalent):
     *   POSSIBLE,<set_id>                    biome-filtered structure sets, in
     *                                        possibleStructureSets() list order
     *                                        (the createStructures iteration order)
     *   RING,<set_id>,<index>,<cx>,<cz>      full concentric-ring position list
     *   PS,<set_id>,<cx>,<cz>                isStructureChunk == true, scan over
     *                                        [center-R, center+R]^2, x outer then
     *                                        z, both ascending; sets in POSSIBLE order
     */
    private static void tracePlacements(ServerLevel level, int centerX, int centerZ,
                                        int scanRadius, String outputPath) {
        net.minecraft.world.level.chunk.ChunkGeneratorStructureState state =
            level.getChunkSource().getGeneratorState();
        state.ensureStructuresGenerated();
        Registry<net.minecraft.world.level.levelgen.structure.StructureSet> setRegistry =
            level.registryAccess().lookupOrThrow(Registries.STRUCTURE_SET);

        List<net.minecraft.core.Holder<net.minecraft.world.level.levelgen.structure.StructureSet>> sets =
            state.possibleStructureSets();

        try (PrintWriter out = new PrintWriter(new FileWriter(outputPath))) {
            out.println("# Java Placement Trace");
            out.println("# Seed: " + SEED);
            out.println("# Scan: center (" + centerX + "," + centerZ + ") radius " + scanRadius);

            for (var setHolder : sets) {
                out.println("POSSIBLE," + setRegistry.getKey(setHolder.value()));
            }
            for (var setHolder : sets) {
                String setId = String.valueOf(setRegistry.getKey(setHolder.value()));
                var placement = setHolder.value().placement();
                if (placement instanceof net.minecraft.world.level.levelgen.structure.placement.ConcentricRingsStructurePlacement rings) {
                    List<ChunkPos> positions = state.getRingPositionsFor(rings);
                    if (positions != null) {
                        int index = 0;
                        for (ChunkPos p : positions) {
                            out.println("RING," + setId + "," + (index++) + "," + p.x() + "," + p.z());
                        }
                    }
                }
                for (int cx = centerX - scanRadius; cx <= centerX + scanRadius; cx++) {
                    for (int cz = centerZ - scanRadius; cz <= centerZ + scanRadius; cz++) {
                        if (placement.isStructureChunk(state, cx, cz)) {
                            out.println("PS," + setId + "," + cx + "," + cz);
                        }
                    }
                }
            }
        } catch (Exception e) {
            throw new RuntimeException("Failed to trace placements", e);
        }
    }

    private static void traceStructures(
        ServerLevel level,
        Map<ChunkPos, ChunkAccess> generatedChunks,
        String outputPath
    ) {
        Registry<Structure> structuresRegistry = level.registryAccess().lookupOrThrow(Registries.STRUCTURE);
        List<Map.Entry<ChunkPos, ChunkAccess>> sortedChunks = new ArrayList<>(generatedChunks.entrySet());
        sortedChunks.sort(Comparator
            .comparingInt((Map.Entry<ChunkPos, ChunkAccess> entry) -> entry.getKey().x())
            .thenComparingInt(entry -> entry.getKey().z()));

        Set<Long> referencedStartChunks = new TreeSet<>();

        try (PrintWriter out = new PrintWriter(new FileWriter(outputPath))) {
            out.println("# Java Structure Trace");
            out.println("# Seed: " + SEED);
            out.println("# Target: " + targetStatus);
            if (!dimension.equals("overworld")) out.println("# Dimension: " + dimension);
            out.println("# Structures: " + generateStructures);
            out.println();

            for (Map.Entry<ChunkPos, ChunkAccess> entry : sortedChunks) {
                ChunkPos chunkPos = entry.getKey();
                ChunkAccess chunk = entry.getValue();
                out.println("CHUNK " + chunkPos.x() + "," + chunkPos.z());

                Map<Structure, LongSet> references = chunk.getAllReferences();
                if (references.isEmpty()) {
                    out.println("  REFERENCES none");
                } else {
                    List<Map.Entry<Structure, LongSet>> refEntries = new ArrayList<>(references.entrySet());
                    refEntries.sort(Comparator.comparing(e -> structureName(structuresRegistry, e.getKey())));
                    for (Map.Entry<Structure, LongSet> refEntry : refEntries) {
                        List<String> refChunks = new ArrayList<>();
                        for (long ref : refEntry.getValue()) {
                            ChunkPos refPos = ChunkPos.unpack(ref);
                            referencedStartChunks.add(ref);
                            refChunks.add(refPos.x() + "," + refPos.z());
                        }
                        Collections.sort(refChunks);
                        out.println("  REFERENCES " + structureName(structuresRegistry, refEntry.getKey()) + " -> " + String.join(" | ", refChunks));
                    }
                }

                Map<Structure, StructureStart> starts = chunk.getAllStarts();
                if (starts.isEmpty()) {
                    out.println("  STARTS none");
                } else {
                    List<Map.Entry<Structure, StructureStart>> startEntries = new ArrayList<>(starts.entrySet());
                    startEntries.sort(Comparator.comparing(e -> structureName(structuresRegistry, e.getKey())));
                    for (Map.Entry<Structure, StructureStart> startEntry : startEntries) {
                        dumpStructureStart(out, structuresRegistry, "  START", startEntry.getKey(), startEntry.getValue());
                    }
                }

                out.println();
            }

            out.println("# REFERENCED START CHUNKS");
            for (long ref : referencedStartChunks) {
                ChunkPos startChunkPos = ChunkPos.unpack(ref);
                ChunkAccess startChunk = level.getChunk(startChunkPos.x(), startChunkPos.z(), ChunkStatus.STRUCTURE_STARTS);
                out.println("SOURCE_CHUNK " + startChunkPos.x() + "," + startChunkPos.z());

                Map<Structure, StructureStart> starts = startChunk.getAllStarts();
                if (starts.isEmpty()) {
                    out.println("  STARTS none");
                } else {
                    List<Map.Entry<Structure, StructureStart>> startEntries = new ArrayList<>(starts.entrySet());
                    startEntries.sort(Comparator.comparing(e -> structureName(structuresRegistry, e.getKey())));
                    for (Map.Entry<Structure, StructureStart> startEntry : startEntries) {
                        dumpStructureStart(out, structuresRegistry, "  SOURCE_START", startEntry.getKey(), startEntry.getValue());
                    }
                }

                out.println();
            }
        } catch (Exception e) {
            throw new RuntimeException("Failed to trace structures", e);
        }
    }

    private static String structureName(Registry<Structure> registry, Structure structure) {
        return registry.getKey(structure).toString();
    }

    private static void dumpStructureStart(
        PrintWriter out,
        Registry<Structure> structuresRegistry,
        String prefix,
        Structure structure,
        StructureStart start
    ) {
        BoundingBox bb = start.getBoundingBox();
        out.println(
            prefix + " " + structureName(structuresRegistry, structure) +
            " valid=" + start.isValid() +
            " start_chunk=" + start.getChunkPos().x() + "," + start.getChunkPos().z() +
            " refs=" + start.getReferences() +
            " bb=" + bb.minX() + "," + bb.minY() + "," + bb.minZ() +
            " -> " + bb.maxX() + "," + bb.maxY() + "," + bb.maxZ()
        );

        List<StructurePiece> pieces = new ArrayList<>(start.getPieces());
        pieces.sort(Comparator
            .comparingInt((StructurePiece piece) -> piece.getBoundingBox().minX())
            .thenComparingInt(piece -> piece.getBoundingBox().minY())
            .thenComparingInt(piece -> piece.getBoundingBox().minZ())
            .thenComparing(piece -> piece.getClass().getSimpleName()));

        for (StructurePiece piece : pieces) {
            BoundingBox pieceBox = piece.getBoundingBox();
            out.println(
                prefix + "_PIECE " + piece.getClass().getSimpleName() +
                " bb=" + pieceBox.minX() + "," + pieceBox.minY() + "," + pieceBox.minZ() +
                " -> " + pieceBox.maxX() + "," + pieceBox.maxY() + "," + pieceBox.maxZ()
            );
        }
    }
    // =========================================================================
    // END FEATURE TRACING CODE
    // =========================================================================

    // =========================================================================
    // HEADLESS SERVER (GameTestServer pattern - no network bind)
    // =========================================================================

    /**
     * MinecraftServer subclass that skips the TCP listener entirely, modeled on
     * net.minecraft.gametest.framework.GameTestServer. DedicatedServer.initServer()
     * unconditionally binds a server socket, which fails in sandboxed environments
     * ("FAILED TO BIND TO PORT ... Operation not permitted"). Worldgen does not
     * need networking, so initServer() here just sets a player list and loads the
     * level - Mojang's own GameTestServer proves the full chunk pipeline runs
     * identically without a listener.
     */
    private static class HeadlessParityServer extends MinecraftServer {
        private static final Services NO_SERVICES = new Services(
            (SessionService) null,
            ServicesKeySet.EMPTY,
            (GameProfileRepository) null,
            new MockUserNameToIdResolver(),
            new MockProfileResolver()
        );

        private final LocalSampleLogger sampleLogger = new LocalSampleLogger(4);

        HeadlessParityServer(Thread serverThread,
                             LevelStorageSource.LevelStorageAccess storage,
                             PackRepository packRepository,
                             WorldStem worldStem) {
            // 26.3: game rules are handed to the server (they left
            // LevelSettings), plus the notification manager.
            super(serverThread, storage, packRepository, worldStem,
                  Optional.of(new GameRules(FeatureFlags.DEFAULT_FLAGS)), Proxy.NO_PROXY,
                  DataFixers.getDataFixer(), NO_SERVICES,
                  net.minecraft.server.level.progress.LoggingLevelLoadListener.forDedicatedServer(),
                  false, new NotificationManager());
        }

        @Override
        protected void loadLevel() {
            // Deliberately skip prepareLevels(): spawn-area pregeneration runs
            // FEATURES steps for adjacent chunks in parallel, making cross-chunk
            // features (clay/moss patches, sculk) scheduling-dependent. The
            // parity request loop serializes every FEATURES step instead, so
            // dumps are byte-reproducible run to run.
            this.worldData.setModdedInfo(this.getServerModName(), this.getModdedStatus().shouldReportAsModified());
            this.createLevels();
            this.forceDifficulty();
        }

        @Override
        protected boolean initServer() {
            this.setPlayerList(new PlayerList(this, this.registries(), this.playerDataStorage,
                                              new EmptyNotificationService()) {});
            Gizmos.withCollector(GizmoCollector.NOOP);
            this.loadLevel();
            return true;
        }

        @Override public boolean isHardcore() { return false; }
        @Override public LevelBasedPermissionSet operatorUserPermissions() { return LevelBasedPermissionSet.ALL; }
        @Override public PermissionSet getFunctionCompilationPermissions() { return LevelBasedPermissionSet.OWNER; }
        @Override public boolean shouldRconBroadcast() { return false; }
        @Override public boolean isDedicatedServer() { return false; }
        @Override public int getRateLimitPacketsPerSecond() { return 0; }
        @Override public int getCommandSpamThresholdSeconds() { return 0; }
        @Override public int getChatSpamThresholdSeconds() { return 0; }
        @Override public boolean useNativeTransport() { return false; }
        @Override public boolean isPublished() { return false; }
        @Override public boolean shouldInformAdmins() { return false; }
        @Override public boolean isSingleplayerOwner(NameAndId nameAndId) { return false; }
        @Override public int getMaxPlayers() { return 1; }
        @Override protected SampleLogger getTickTimeLogger() { return this.sampleLogger; }
        @Override public boolean isTickTimeLoggingEnabled() { return false; }

        @Override
        public SystemReport fillServerSystemReport(SystemReport report) {
            report.setDetail("Type", "Headless parity test server");
            return report;
        }
    }

    private static class MockUserNameToIdResolver implements UserNameToIdResolver {
        private final Set<NameAndId> savedIds = new HashSet<>();

        @Override public void add(NameAndId nameAndId) { this.savedIds.add(nameAndId); }

        @Override
        public Optional<NameAndId> get(String name) {
            return this.savedIds.stream().filter(e -> e.name().equals(name)).findFirst()
                .or(() -> Optional.of(NameAndId.createOffline(name)));
        }

        @Override
        public Optional<NameAndId> get(UUID id) {
            return this.savedIds.stream().filter(e -> e.id().equals(id)).findFirst();
        }

        @Override public void resolveOfflineUsers(boolean value) {}
        @Override public void save() {}
    }

    private static class MockProfileResolver implements ProfileResolver {
        @Override public Optional<GameProfile> fetchByName(String name) { return Optional.empty(); }
        @Override public Optional<GameProfile> fetchById(UUID id) { return Optional.empty(); }
    }
}
