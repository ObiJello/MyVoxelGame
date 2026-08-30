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
import com.mojang.authlib.minecraft.MinecraftSessionService;
import com.mojang.authlib.yggdrasil.ServicesKeySet;
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
    private static String phasesDescription = "all (0-11)";

    /**
     * Parse a --phases spec into (targetStatus, generateStructures, phasesDescription).
     * Must be called AFTER Bootstrap (ChunkStatus requires registries), and must stay
     * semantically identical to configurePhases() in tests/parity/cpp/CppChunkGeneratorTest.cpp.
     *
     * Accepted: "all", or "A-B" where A is 0 (EMPTY start, structures ON) or
     * 3 (BIOMES start, structures OFF) and B in 1..7 (B >= 3 when A is 3):
     * 1=STRUCTURE_STARTS, 2=STRUCTURE_REFS, 3=BIOMES, 4=NOISE, 5=SURFACE,
     * 6=CARVERS, 7=FEATURES. Unknown specs must fail loudly: silently running
     * "all" (racy FULL pipeline) produced misleading dumps.
     */
    private static void configurePhaseSpec(String phases) {
        if (phases.equals("all")) {
            targetStatus = ChunkStatus.FULL;
            generateStructures = true;
            phasesDescription = "all (0-11, complete generation)";
            return;
        }
        java.util.regex.Matcher m = java.util.regex.Pattern.compile("^([03])-([1-7])$").matcher(phases);
        if (!m.matches()) {
            throw new IllegalArgumentException(
                "unknown --phases spec '" + phases + "' (valid: all, or A-B with A in {0,3}, B in 1..7, e.g. 3-7, 0-2, 3-5)");
        }
        int start = Integer.parseInt(m.group(1));
        int end = Integer.parseInt(m.group(2));
        if (start == 3 && end < 3) {
            throw new IllegalArgumentException(
                "invalid --phases spec '" + phases + "': end phase " + end + " precedes start phase 3");
        }
        ChunkStatus[] endStatuses = {
            null, ChunkStatus.STRUCTURE_STARTS, ChunkStatus.STRUCTURE_REFERENCES, ChunkStatus.BIOMES,
            ChunkStatus.NOISE, ChunkStatus.SURFACE, ChunkStatus.CARVERS, ChunkStatus.FEATURES
        };
        String[] endNames = {
            null, "STRUCTURE_STARTS", "STRUCTURE_REFS", "BIOMES", "NOISE", "SURFACE", "CARVERS", "FEATURES"
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

                    LevelSettings levelSettings = new LevelSettings(
                        "test_world",
                        GameType.CREATIVE,
                        false,  // hardcore
                        Difficulty.NORMAL,
                        true,   // allowCommands
                        new GameRules(context.dataConfiguration().enabledFeatures()),
                        context.dataConfiguration()
                    );

                    WorldOptions worldOptions = new WorldOptions(SEED, generateStructures, false);  // seed, generateStructures, bonusChest
                    WorldDimensions dimensions = createWorldDimensionsForType(context.datapackWorldgen());
                    WorldDimensions.Complete finalDimensions = dimensions.bake(datapackDimensions);
                    Lifecycle lifecycle = finalDimensions.lifecycle().add(context.datapackWorldgen().allRegistriesLifecycle());

                    PrimaryLevelData levelData = new PrimaryLevelData(levelSettings, worldOptions, finalDimensions.specialWorldProperty(), lifecycle);
                    // Mark initialized so MinecraftServer skips the initial spawn
                    // search, which fully generates chunks around spawn with
                    // parallel FEATURES steps - verified nondeterministic (clay/
                    // moss/sculk cross-chunk reads depend on scheduling).
                    levelData.setInitialized(true);
                    return new WorldLoader.DataLoadOutput<WorldData>(
                        levelData,
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
            System.out.println("  Spawn suggestion: " + chunkCache.randomState().sampler().findSpawnPosition());
            System.out.println("  Shared spawn: " + level.getRespawnData().pos());

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
                                    net.minecraft.world.level.chunk.status.ChunkStatus.NOISE, false);
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
                                        long key = holder.getPos().toLong();
                                        if (!decorOrderSeen.add(key)) continue;
                                        System.out.println("STATUS_SCAN " + holder.getPos().x + " " + holder.getPos().z
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
                                        long key = ChunkPos.asLong(qx, qz);
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
                    .comparingInt((Map.Entry<ChunkPos, ChunkAccess> e) -> e.getKey().x)
                    .thenComparingInt(e -> e.getKey().z));

                if (dumpFull) {
                    writeCanonicalHeader(out, "radius");
                    for (Map.Entry<ChunkPos, ChunkAccess> entry : sortedChunks) {
                        writeCanonicalChunk(out, entry.getValue(), entry.getKey().x, entry.getKey().z);
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
                        writeChunkData(out, entry.getValue(), entry.getKey().x, entry.getKey().z);
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

                    LevelSettings levelSettings = new LevelSettings(
                        "test_world",
                        GameType.CREATIVE,
                        false,
                        Difficulty.NORMAL,
                        true,
                        new GameRules(context.dataConfiguration().enabledFeatures()),
                        context.dataConfiguration()
                    );

                    WorldOptions worldOptions = new WorldOptions(SEED, generateStructures, false);
                    WorldDimensions dimensions = createWorldDimensionsForType(context.datapackWorldgen());
                    WorldDimensions.Complete finalDimensions = dimensions.bake(datapackDimensions);
                    Lifecycle lifecycle = finalDimensions.lifecycle().add(context.datapackWorldgen().allRegistriesLifecycle());

                    PrimaryLevelData levelData = new PrimaryLevelData(levelSettings, worldOptions, finalDimensions.specialWorldProperty(), lifecycle);
                    // Mark initialized so MinecraftServer skips the initial spawn
                    // search, which fully generates chunks around spawn with
                    // parallel FEATURES steps - verified nondeterministic (clay/
                    // moss/sculk cross-chunk reads depend on scheduling).
                    levelData.setInitialized(true);
                    return new WorldLoader.DataLoadOutput<WorldData>(
                        levelData,
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
            System.out.println("  Spawn suggestion: " + chunkCache.randomState().sampler().findSpawnPosition());
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
        Map<Property<?>, Comparable<?>> values = state.getValues();
        if (!values.isEmpty()) {
            List<Map.Entry<Property<?>, Comparable<?>>> entries = new ArrayList<>(values.entrySet());
            entries.sort(Comparator.comparing(e -> e.getKey().getName()));
            sb.append('[');
            boolean first = true;
            for (Map.Entry<Property<?>, Comparable<?>> e : entries) {
                if (!first) sb.append(',');
                first = false;
                sb.append(e.getKey().getName()).append('=').append(propertyValueName(e.getKey(), e.getValue()));
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
                for (long packed : entry.getValue()) refs.add(new ChunkPos(packed));
                refs.sort(Comparator.comparingInt((ChunkPos p) -> p.x).thenComparingInt(p -> p.z));
                StringBuilder sb = new StringBuilder("R," + structureName(registry, entry.getKey()));
                for (ChunkPos p : refs) sb.append(',').append(p.x).append(';').append(p.z);
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

        // B/Q require BIOMES+; H requires NOISE+ (FORMAT.md "Section presence").
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

        // H lines: type outer (WS then OF), then z, then x. NOISE+ only.
        if (!targetStatus.isOrAfter(ChunkStatus.NOISE)) return;
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

    /**
     * Replace matching placed features with live tracing wrappers so the trace
     * runs during real chunk generation instead of replaying after mutation.
     */
    private static void installLiveFeatureTracing(ServerLevel level) {
        installLiveFeatureTracing(
            level.getChunkSource().getGenerator(),
            level.registryAccess().lookupOrThrow(Registries.PLACED_FEATURE)
        );

        try {
            net.minecraft.server.level.ServerChunkCache chunkSource = level.getChunkSource();
            net.minecraft.server.level.ChunkMap chunkMap = chunkSource.chunkMap;
            java.lang.reflect.Field worldGenContextField =
                net.minecraft.server.level.ChunkMap.class.getDeclaredField("worldGenContext");
            worldGenContextField.setAccessible(true);
            net.minecraft.world.level.chunk.status.WorldGenContext worldGenContext =
                (net.minecraft.world.level.chunk.status.WorldGenContext) worldGenContextField.get(chunkMap);
            installLiveFeatureTracing(
                worldGenContext.generator(),
                level.registryAccess().lookupOrThrow(Registries.PLACED_FEATURE)
            );
        } catch (Exception e) {
            traceWriter.println("# LIVE_TRACE_INSTALL_ERROR: chunkMap generator " + e.getMessage());
            e.printStackTrace(traceWriter);
            traceWriter.flush();
        }
    }

    private static void installLiveFeatureTracing(WorldStem worldStem) {
        if (!traceFeatures || traceWriter == null || traceFeatureFilter == null) return;

        try {
            RegistryAccess.Frozen registries = worldStem.registries().compositeAccess();
            Registry<LevelStem> dimensions = registries.lookupOrThrow(Registries.LEVEL_STEM);
            LevelStem overworldStem = dimensions.getValue(LevelStem.OVERWORLD);
            if (overworldStem == null) {
                traceWriter.println("# LIVE_TRACE_INSTALL_ERROR: missing overworld LevelStem");
                traceWriter.flush();
                return;
            }

            installLiveFeatureTracing(
                overworldStem.generator(),
                registries.lookupOrThrow(Registries.PLACED_FEATURE)
            );
        } catch (Exception e) {
            traceWriter.println("# LIVE_TRACE_INSTALL_ERROR: " + e.getMessage());
            e.printStackTrace(traceWriter);
            traceWriter.flush();
        }
    }

    private static void installLiveFeatureTracing(
        net.minecraft.world.level.chunk.ChunkGenerator generator,
        Registry<net.minecraft.world.level.levelgen.placement.PlacedFeature> featureRegistry
    ) {
        if (!traceFeatures || traceWriter == null ||
            (traceFeatureFilter == null && traceWatchPos == null)) return;
        if (!liveTraceInstalledTargets.add(generator)) return;

        try {
            java.lang.reflect.Field featuresField =
                net.minecraft.world.level.chunk.ChunkGenerator.class.getDeclaredField("featuresPerStep");
            featuresField.setAccessible(true);

            @SuppressWarnings("unchecked")
            java.util.function.Supplier<List<net.minecraft.world.level.biome.FeatureSorter.StepFeatureData>> featuresSupplier =
                (java.util.function.Supplier<List<net.minecraft.world.level.biome.FeatureSorter.StepFeatureData>>) featuresField.get(generator);

            List<net.minecraft.world.level.biome.FeatureSorter.StepFeatureData> originalSteps = featuresSupplier.get();

            List<net.minecraft.world.level.biome.FeatureSorter.StepFeatureData> wrappedSteps =
                new ArrayList<>(originalSteps.size());

            int wrappedCount = 0;
            // wrapped-object -> original index, so the biome-step index lookup
            // still resolves features whose registry HOLDER was rebound to a
            // wrapper (Holder::value then returns the wrapped object, which
            // the identity lookup of originals cannot know).
            java.util.IdentityHashMap<net.minecraft.world.level.levelgen.placement.PlacedFeature, Integer> wrappedToIndex =
                new java.util.IdentityHashMap<>();
            java.util.IdentityHashMap<net.minecraft.world.level.levelgen.placement.PlacedFeature, net.minecraft.world.level.levelgen.placement.PlacedFeature> wrapperByOriginal =
                new java.util.IdentityHashMap<>();
            for (int stepIndex = 0; stepIndex < originalSteps.size(); stepIndex++) {
                net.minecraft.world.level.biome.FeatureSorter.StepFeatureData stepData = originalSteps.get(stepIndex);
                List<net.minecraft.world.level.levelgen.placement.PlacedFeature> wrappedFeatures =
                    new ArrayList<>(stepData.features().size());

                wrappedToIndex.clear();
                for (int featureIndex = 0; featureIndex < stepData.features().size(); featureIndex++) {
                    net.minecraft.world.level.levelgen.placement.PlacedFeature feature = stepData.features().get(featureIndex);
                    Optional<net.minecraft.resources.ResourceKey<net.minecraft.world.level.levelgen.placement.PlacedFeature>> keyOpt =
                        featureRegistry.getResourceKey(feature);
                    String featureName = keyOpt.map(k -> k.identifier().toString()).orElse("(unnamed)");

                    if (traceFeatureFilter != null && traceFeatureFilter.equals(featureName)) {
                        net.minecraft.world.level.levelgen.placement.PlacedFeature original = feature;
                        feature = wrapPlacedFeatureForLiveTracing(feature, featureName, stepIndex, featureIndex);
                        wrappedCount++;
                        wrappedToIndex.put(feature, featureIndex);
                        wrapperByOriginal.put(original, feature);
                    } else if (traceWatchPos != null) {
                        feature = wrapPlacedFeatureForWatch(feature, featureName);
                    }

                    wrappedFeatures.add(feature);
                }

                java.util.function.ToIntFunction<net.minecraft.world.level.levelgen.placement.PlacedFeature> originalLookup =
                    Util.createIndexIdentityLookup(stepData.features());
                java.util.IdentityHashMap<net.minecraft.world.level.levelgen.placement.PlacedFeature, Integer> stepWrappedToIndex =
                    new java.util.IdentityHashMap<>(wrappedToIndex);
                java.util.function.ToIntFunction<net.minecraft.world.level.levelgen.placement.PlacedFeature> lookupWithWrapped =
                    f -> {
                        Integer idx = stepWrappedToIndex.get(f);
                        return idx != null ? idx : originalLookup.applyAsInt(f);
                    };

                wrappedSteps.add(
                    new net.minecraft.world.level.biome.FeatureSorter.StepFeatureData(
                        wrappedFeatures,
                        lookupWithWrapped
                    )
                );
            }

            featuresField.set(generator, (java.util.function.Supplier<List<net.minecraft.world.level.biome.FeatureSorter.StepFeatureData>>)() -> wrappedSteps);

            // Structure-pass feature pool elements (FeaturePoolElement) resolve
            // their PlacedFeature through the registry Holder.Reference, not
            // through featuresPerStep - rebind the reference's value to the
            // tracing wrapper so those invocations are traced too.
            int reboundHolders = 0;
            if (traceFeatureFilter != null) {
                List<net.minecraft.core.Holder.Reference<net.minecraft.world.level.levelgen.placement.PlacedFeature>> refs =
                    featureRegistry.listElements()
                        .filter(ref -> ref.key().identifier().toString().equals(traceFeatureFilter))
                        .toList();
                for (net.minecraft.core.Holder.Reference<net.minecraft.world.level.levelgen.placement.PlacedFeature> ref : refs) {
                    // Reuse the step-list wrapper when one exists so the
                    // biome-step index lookup resolves Holder::value; only
                    // pool-element-only features get a fresh wrapper.
                    net.minecraft.world.level.levelgen.placement.PlacedFeature wrapped =
                        wrapperByOriginal.get(ref.value());
                    if (wrapped == null) {
                        wrapped = wrapPlacedFeatureForLiveTracing(ref.value(), traceFeatureFilter, -1, -1);
                    }
                    java.lang.reflect.Field valueField =
                        net.minecraft.core.Holder.Reference.class.getDeclaredField("value");
                    valueField.setAccessible(true);
                    valueField.set(ref, wrapped);
                    reboundHolders++;
                }
            }

            traceWriter.println(
                "# LIVE_TRACE_INSTALLED filter=" + traceFeatureFilter +
                " wrapped=" + wrappedCount +
                " reboundHolders=" + reboundHolders +
                " generator=" + generator.getClass().getName() +
                " id=" + System.identityHashCode(generator)
            );
            traceWriter.flush();
        } catch (Exception e) {
            liveTraceInstalledTargets.remove(generator);
            traceWriter.println("# LIVE_TRACE_INSTALL_ERROR: " + e.getMessage());
            e.printStackTrace(traceWriter);
            traceWriter.flush();
        }
    }

    @SuppressWarnings({"rawtypes", "unchecked"})
    private static net.minecraft.world.level.levelgen.placement.PlacedFeature wrapPlacedFeatureForLiveTracing(
        net.minecraft.world.level.levelgen.placement.PlacedFeature placedFeature,
        String featureName,
        int stepIndex,
        int featureIndex
    ) {
        net.minecraft.world.level.levelgen.feature.ConfiguredFeature originalConfigured =
            (net.minecraft.world.level.levelgen.feature.ConfiguredFeature) placedFeature.feature().value();
        net.minecraft.world.level.levelgen.feature.Feature originalFeature =
            (net.minecraft.world.level.levelgen.feature.Feature) originalConfigured.feature();
        net.minecraft.world.level.levelgen.feature.configurations.FeatureConfiguration originalConfig =
            (net.minecraft.world.level.levelgen.feature.configurations.FeatureConfiguration) originalConfigured.config();
        boolean geodeTrace =
            originalFeature.getClass().getName().equals("net.minecraft.world.level.levelgen.feature.GeodeFeature");

        net.minecraft.world.level.levelgen.feature.Feature tracingFeature =
            new net.minecraft.world.level.levelgen.feature.Feature(
                net.minecraft.world.level.levelgen.feature.configurations.NoneFeatureConfiguration.CODEC
            ) {
                @Override
                public boolean place(net.minecraft.world.level.levelgen.feature.FeaturePlaceContext context) {
                    Map<BlockPos, String> beforeBlocks = captureLiveFeatureBlocks(context.level(), context.origin());
                    logLiveFeaturePlacement(featureName, stepIndex, featureIndex, context);
                    boolean placed;
                    if (geodeTrace &&
                        originalConfig instanceof net.minecraft.world.level.levelgen.feature.configurations.GeodeConfiguration geodeConfig) {
                        List<LoggedRandomCall> randomCalls = new ArrayList<>();
                        net.minecraft.util.RandomSource loggingRandom =
                            wrapRandomSourceForGeodeTracing(context.random(), randomCalls);
                        net.minecraft.world.level.levelgen.feature.FeaturePlaceContext tracedContext =
                            new net.minecraft.world.level.levelgen.feature.FeaturePlaceContext(
                                context.topFeature(),
                                context.level(),
                                context.chunkGenerator(),
                                loggingRandom,
                                context.origin(),
                                geodeConfig
                            );
                        placed = originalFeature.place(tracedContext);
                        logLiveGeodeRandomSummary(
                            featureName,
                            stepIndex,
                            featureIndex,
                            context.origin(),
                            context.level().getSeed(),
                            geodeConfig,
                            randomCalls
                        );
                    } else {
                        // NOTE: logLiveVegetationPatchShadow is intentionally NOT
                        // called - its traceVegetationPatchGround replay performs
                        // real setBlock writes and corrupts the traced run.
                        // Generic chronological RNG + setBlock trace for the
                        // filtered feature (mirrors C++ MC_RNG_TRACE_FEATURE /
                        // MC_RNG_TRACE_FILE).
                        BlockPos rngOrigin = context.origin();
                        traceWriter.println("RNGTRACE_BEGIN origin=" +
                            rngOrigin.getX() + "," + rngOrigin.getY() + "," + rngOrigin.getZ());
                        traceWriter.flush();
                        net.minecraft.util.RandomSource loggingRandom =
                            wrapRandomSourceForSculkTracing(context.random());
                        net.minecraft.world.level.WorldGenLevel loggingLevel =
                            wrapLevelForSculkTracing(context.level());
                        net.minecraft.world.level.levelgen.feature.FeaturePlaceContext rngContext =
                            new net.minecraft.world.level.levelgen.feature.FeaturePlaceContext(
                                context.topFeature(),
                                loggingLevel,
                                context.chunkGenerator(),
                                loggingRandom,
                                rngOrigin,
                                originalConfig
                            );
                        placed = originalFeature.place(rngContext);
                        traceWriter.println("RNGTRACE_END");
                        traceWriter.flush();
                    }
                    logLiveFeatureResult(featureName, stepIndex, featureIndex, context, placed, beforeBlocks);
                    return placed;
                }
            };

        net.minecraft.world.level.levelgen.feature.ConfiguredFeature wrappedConfigured =
            new net.minecraft.world.level.levelgen.feature.ConfiguredFeature(tracingFeature, originalConfig);

        List<net.minecraft.world.level.levelgen.placement.PlacementModifier> wrappedPlacement =
            new ArrayList<>(placedFeature.placement().size());
        for (int modifierIndex = 0; modifierIndex < placedFeature.placement().size(); modifierIndex++) {
            wrappedPlacement.add(
                wrapPlacementModifierForLiveTracing(
                    placedFeature,
                    placedFeature.placement().get(modifierIndex),
                    featureName,
                    stepIndex,
                    featureIndex,
                    modifierIndex
                )
            );
        }

        return new net.minecraft.world.level.levelgen.placement.PlacedFeature(
            net.minecraft.core.Holder.direct(wrappedConfigured),
            wrappedPlacement
        );
    }

    private static final class LoggedRandomCall {
        final String method;
        final int bound;
        final long longValue;
        final double doubleValue;

        private LoggedRandomCall(String method, int bound, long longValue, double doubleValue) {
            this.method = method;
            this.bound = bound;
            this.longValue = longValue;
            this.doubleValue = doubleValue;
        }

        static LoggedRandomCall nextInt(int bound, int value) {
            return new LoggedRandomCall("nextInt", bound, value, value);
        }

        static LoggedRandomCall nextFloat(float value) {
            return new LoggedRandomCall("nextFloat", -1, 0L, value);
        }

        static LoggedRandomCall nextDouble(double value) {
            return new LoggedRandomCall("nextDouble", -1, 0L, value);
        }
    }

    /**
     * Light wrapper applied to EVERY feature when --trace-watch is set: logs a
     * WATCH line whenever the watched position's block changes across a
     * feature's place() call, attributing the change to that feature.
     */
    @SuppressWarnings({"rawtypes", "unchecked"})
    private static net.minecraft.world.level.levelgen.placement.PlacedFeature wrapPlacedFeatureForWatch(
        net.minecraft.world.level.levelgen.placement.PlacedFeature placedFeature,
        String featureName
    ) {
        net.minecraft.world.level.levelgen.feature.ConfiguredFeature originalConfigured =
            (net.minecraft.world.level.levelgen.feature.ConfiguredFeature) placedFeature.feature().value();
        net.minecraft.world.level.levelgen.feature.Feature originalFeature =
            (net.minecraft.world.level.levelgen.feature.Feature) originalConfigured.feature();

        net.minecraft.world.level.levelgen.feature.Feature watchingFeature =
            new net.minecraft.world.level.levelgen.feature.Feature(
                net.minecraft.world.level.levelgen.feature.configurations.NoneFeatureConfiguration.CODEC
            ) {
                @Override
                public boolean place(net.minecraft.world.level.levelgen.feature.FeaturePlaceContext context) {
                    BlockState before = context.level().getBlockState(traceWatchPos);
                    boolean placed = originalFeature.place(context);
                    BlockState after = context.level().getBlockState(traceWatchPos);
                    if (before != after && traceWriter != null) {
                        BlockPos origin = context.origin();
                        traceWriter.println("WATCH " + featureName +
                            " origin=" + origin.getX() + "," + origin.getY() + "," + origin.getZ() +
                            " pos=" + traceWatchPos.getX() + "," + traceWatchPos.getY() + "," + traceWatchPos.getZ() +
                            " " + serializeState(before) + " -> " + serializeState(after));
                        traceWriter.flush();
                    }
                    return placed;
                }
            };

        net.minecraft.world.level.levelgen.feature.ConfiguredFeature wrappedConfigured =
            new net.minecraft.world.level.levelgen.feature.ConfiguredFeature(
                watchingFeature, originalConfigured.config());

        // Modifiers must see the ORIGINAL placed feature as topFeature or the
        // BiomeFilter check fails and the feature silently never places.
        List<net.minecraft.world.level.levelgen.placement.PlacementModifier> watchPlacement =
            new ArrayList<>(placedFeature.placement().size());
        for (net.minecraft.world.level.levelgen.placement.PlacementModifier modifier : placedFeature.placement()) {
            watchPlacement.add(new net.minecraft.world.level.levelgen.placement.PlacementModifier() {
                @Override
                public java.util.stream.Stream<BlockPos> getPositions(
                    net.minecraft.world.level.levelgen.placement.PlacementContext context,
                    net.minecraft.util.RandomSource random,
                    BlockPos origin
                ) {
                    net.minecraft.world.level.levelgen.placement.PlacementContext delegateContext =
                        new net.minecraft.world.level.levelgen.placement.PlacementContext(
                            context.getLevel(),
                            context.generator(),
                            Optional.of(placedFeature)
                        );
                    return modifier.getPositions(delegateContext, random, origin);
                }

                @Override
                public net.minecraft.world.level.levelgen.placement.PlacementModifierType<?> type() {
                    return modifier.type();
                }
            });
        }

        return new net.minecraft.world.level.levelgen.placement.PlacedFeature(
            net.minecraft.core.Holder.direct(wrappedConfigured),
            watchPlacement
        );
    }

    /**
     * Dynamic proxy over WorldGenLevel that logs every setBlock chronologically
     * (interleaved with the RNG trace lines), mirroring the C++ hook in
     * WorldGenRegion::setBlock.
     */
    private static net.minecraft.world.level.WorldGenLevel wrapLevelForSculkTracing(
        net.minecraft.world.level.WorldGenLevel delegate
    ) {
        return (net.minecraft.world.level.WorldGenLevel) java.lang.reflect.Proxy.newProxyInstance(
            net.minecraft.world.level.WorldGenLevel.class.getClassLoader(),
            new Class<?>[]{ net.minecraft.world.level.WorldGenLevel.class },
            (proxy, method, args) -> {
                if (method.getName().equals("getHeightmapPos")
                    && args != null && args.length == 2
                    && args[1] instanceof BlockPos hmPos) {
                    Object result;
                    try {
                        result = method.invoke(delegate, args);
                    } catch (java.lang.reflect.InvocationTargetException e) {
                        throw e.getCause();
                    }
                    BlockPos rp = (BlockPos) result;
                    traceWriter.println("CALL getHeightmapPos " + args[0] + " " +
                        hmPos.getX() + "," + hmPos.getZ() + "=" + rp.getY());
                    return result;
                }
                if (method.getName().equals("getBlockState")
                    && args != null && args.length == 1
                    && args[0] instanceof BlockPos readPos) {
                    Object result;
                    try {
                        result = method.invoke(delegate, args);
                    } catch (java.lang.reflect.InvocationTargetException e) {
                        throw e.getCause();
                    }
                    traceWriter.println("GET " + readPos.getX() + "," + readPos.getY() + ","
                        + readPos.getZ() + "=" + serializeState((BlockState) result));
                    return result;
                }
                if (method.getName().equals("isEmptyBlock")
                    && args != null && args.length == 1
                    && args[0] instanceof BlockPos emptyPos) {
                    Object result;
                    try {
                        result = method.invoke(delegate, args);
                    } catch (java.lang.reflect.InvocationTargetException e) {
                        throw e.getCause();
                    }
                    traceWriter.println("CALL isEmptyBlock " + emptyPos.getX() + "," +
                        emptyPos.getY() + "," + emptyPos.getZ() + "=" + result);
                    return result;
                }
                boolean isSetBlock = method.getName().equals("setBlock")
                    && args != null && args.length >= 3
                    && args[0] instanceof BlockPos
                    && args[1] instanceof BlockState;
                String setLine = null;
                if (isSetBlock) {
                    BlockPos pos = (BlockPos) args[0];
                    BlockState newState = (BlockState) args[1];
                    BlockState before = delegate.getBlockState(pos);
                    setLine = "SET " + pos.getX() + "," + pos.getY() + "," + pos.getZ() +
                        " " + serializeState(before) + " -> " + serializeState(newState) +
                        " flags=" + args[2];
                }
                try {
                    Object result = method.invoke(delegate, args);
                    if (setLine != null) {
                        traceWriter.println(setLine);
                    }
                    return result;
                } catch (java.lang.reflect.InvocationTargetException e) {
                    throw e.getCause();
                }
            });
    }

    /**
     * Logs every interface-level RNG draw (nextInt(bound), nextFloat) with the
     * calling frame, mirroring the C++ MC_SCULK_RNG_TRACE hook so the two
     * chronological streams can be diffed line-by-line.
     */
    private static net.minecraft.util.RandomSource wrapRandomSourceForSculkTracing(
        net.minecraft.util.RandomSource delegate
    ) {
        final StackWalker walker = StackWalker.getInstance();
        return new net.minecraft.util.RandomSource() {
            private String callSite() {
                try {
                    return walker.walk(frames -> frames
                        .map(StackWalker.StackFrame::toStackTraceElement)
                        .filter(e -> !e.getClassName().contains("MinecraftAsyncChunkTest"))
                        .limit(3)
                        .map(e -> {
                            String cn = e.getClassName();
                            int idx = cn.lastIndexOf('.');
                            return (idx >= 0 ? cn.substring(idx + 1) : cn)
                                + "." + e.getMethodName() + ":" + e.getLineNumber();
                        })
                        .reduce((a, b) -> a + "<" + b)
                        .orElse("?"));
                } catch (Exception e) {
                    return "?";
                }
            }

            @Override
            public net.minecraft.util.RandomSource fork() {
                return delegate.fork();
            }

            @Override
            public net.minecraft.world.level.levelgen.PositionalRandomFactory forkPositional() {
                return delegate.forkPositional();
            }

            @Override
            public void setSeed(long seed) {
                delegate.setSeed(seed);
            }

            @Override
            public int nextInt() {
                return delegate.nextInt();
            }

            @Override
            public int nextInt(int bound) {
                int value = delegate.nextInt(bound);
                traceWriter.println("RNG nextInt(" + bound + ")=" + value + " @" + callSite());
                return value;
            }

            @Override
            public long nextLong() {
                return delegate.nextLong();
            }

            @Override
            public boolean nextBoolean() {
                return delegate.nextBoolean();
            }

            @Override
            public float nextFloat() {
                float value = delegate.nextFloat();
                traceWriter.println(
                    "RNG nextFloat=" + String.format(java.util.Locale.ROOT, "%.9e", value) +
                    " @" + callSite());
                return value;
            }

            @Override
            public double nextDouble() {
                return delegate.nextDouble();
            }

            @Override
            public double nextGaussian() {
                return delegate.nextGaussian();
            }
        };
    }

    private static net.minecraft.util.RandomSource wrapRandomSourceForGeodeTracing(
        net.minecraft.util.RandomSource delegate,
        List<LoggedRandomCall> calls
    ) {
        return new net.minecraft.util.RandomSource() {
            @Override
            public net.minecraft.util.RandomSource fork() {
                return delegate.fork();
            }

            @Override
            public net.minecraft.world.level.levelgen.PositionalRandomFactory forkPositional() {
                return delegate.forkPositional();
            }

            @Override
            public void setSeed(long seed) {
                delegate.setSeed(seed);
            }

            @Override
            public int nextInt() {
                return delegate.nextInt();
            }

            @Override
            public int nextInt(int bound) {
                int value = delegate.nextInt(bound);
                calls.add(LoggedRandomCall.nextInt(bound, value));
                return value;
            }

            @Override
            public long nextLong() {
                return delegate.nextLong();
            }

            @Override
            public boolean nextBoolean() {
                return delegate.nextBoolean();
            }

            @Override
            public float nextFloat() {
                float value = delegate.nextFloat();
                calls.add(LoggedRandomCall.nextFloat(value));
                return value;
            }

            @Override
            public double nextDouble() {
                double value = delegate.nextDouble();
                calls.add(LoggedRandomCall.nextDouble(value));
                return value;
            }

            @Override
            public double nextGaussian() {
                return delegate.nextGaussian();
            }
        };
    }

    private static void logLiveGeodeRandomSummary(
        String featureName,
        int stepIndex,
        int featureIndex,
        BlockPos origin,
        long levelSeed,
        net.minecraft.world.level.levelgen.feature.configurations.GeodeConfiguration config,
        List<LoggedRandomCall> calls
    ) {
        if (traceWriter == null) return;

        try {
            int[] cursor = new int[]{0};
            int numPoints = decodeIntProviderSample(config.distributionPoints, calls, cursor);
            double crackRoll = takeNextDouble(calls, cursor);
            float crackChanceRoll = takeNextFloat(calls, cursor);
            boolean shouldGenerateCrack = (double)crackChanceRoll < config.geodeCrackSettings.generateCrackChance;
            double crackSizeAdjustment =
                (double)numPoints / (double)config.outerWallDistance.getMaxValue();
            double innerAir = (double)1.0F / Math.sqrt(config.geodeLayerSettings.filling);
            double innermostBlockLayer =
                (double)1.0F / Math.sqrt(config.geodeLayerSettings.innerLayer + crackSizeAdjustment);
            double innerCrust =
                (double)1.0F / Math.sqrt(config.geodeLayerSettings.middleLayer + crackSizeAdjustment);
            double outerCrust =
                (double)1.0F / Math.sqrt(config.geodeLayerSettings.outerLayer + crackSizeAdjustment);
            double crackSize =
                (double)1.0F / Math.sqrt(
                    config.geodeCrackSettings.baseCrackSize +
                    crackRoll / (double)2.0F +
                    (numPoints > 3 ? crackSizeAdjustment : (double)0.0F)
                );

            traceWriter.println(
                "LIVE_GEODE[" + liveTraceEventCounter.getAndIncrement() + "] STEP=" + stepIndex +
                " IDX=" + featureIndex +
                " " + featureName +
                " ORIGIN=" + origin.getX() + "," + origin.getY() + "," + origin.getZ() +
                " NUM_POINTS=" + numPoints +
                " OUTER_WALL_MAX=" + config.outerWallDistance.getMaxValue() +
                " CRACK_ROLL=" + crackRoll +
                " CRACK_CHANCE_ROLL=" + crackChanceRoll +
                " SHOULD_CRACK=" + shouldGenerateCrack +
                " INNER_AIR=" + innerAir +
                " INNERMOST=" + innermostBlockLayer +
                " INNER_CRUST=" + innerCrust +
                " OUTER_CRUST=" + outerCrust +
                " CRACK_SIZE=" + crackSize
            );

            List<BlockPos> pointPositions = new ArrayList<>(numPoints);
            List<Integer> pointOffsets = new ArrayList<>(numPoints);
            for (int i = 0; i < numPoints; i++) {
                int x = decodeIntProviderSample(config.outerWallDistance, calls, cursor);
                int y = decodeIntProviderSample(config.outerWallDistance, calls, cursor);
                int z = decodeIntProviderSample(config.outerWallDistance, calls, cursor);
                int pointOffset = decodeIntProviderSample(config.pointOffset, calls, cursor);
                BlockPos pointPos = origin.offset(x, y, z);
                pointPositions.add(pointPos);
                pointOffsets.add(pointOffset);
                traceWriter.println(
                    "  GEODE_POINT[" + i + "]=" +
                    pointPos.getX() + "," + pointPos.getY() + "," + pointPos.getZ() +
                    " offset=" + pointOffset
                );
            }

            List<BlockPos> crackPoints = List.of();
            if (shouldGenerateCrack) {
                int offsetIndex = takeNextInt(calls, cursor, 4);
                int crackOffset = numPoints * 2 + 1;
                crackPoints = computeGeodeCrackPoints(origin, offsetIndex, crackOffset);
                traceWriter.println(
                    "  GEODE_CRACK_OFFSET_INDEX=" + offsetIndex +
                    " CRACK_OFFSET=" + crackOffset
                );
                for (BlockPos crackPoint : crackPoints) {
                    traceWriter.println(
                        "  GEODE_CRACK_POINT=" +
                        crackPoint.getX() + "," + crackPoint.getY() + "," + crackPoint.getZ()
                    );
                }
            }

            traceWriter.println(
                "  GEODE_RANDOM_CALLS_USED=" + cursor[0] +
                " TOTAL_RANDOM_CALLS=" + calls.size()
            );
            for (int i = cursor[0]; i < Math.min(cursor[0] + 12, calls.size()); i++) {
                LoggedRandomCall call = calls.get(i);
                if ("nextInt".equals(call.method)) {
                    traceWriter.println(
                        "  GEODE_RANDOM[" + i + "]=nextInt(" + call.bound + ") -> " + call.longValue
                    );
                } else {
                    traceWriter.println(
                        "  GEODE_RANDOM[" + i + "]=" + call.method + " -> " + call.doubleValue
                    );
                }
            }

            net.minecraft.world.level.levelgen.WorldgenRandom noiseRandom =
                new net.minecraft.world.level.levelgen.WorldgenRandom(
                    new net.minecraft.world.level.levelgen.LegacyRandomSource(levelSeed)
                );
            net.minecraft.world.level.levelgen.synth.NormalNoise noise =
                net.minecraft.world.level.levelgen.synth.NormalNoise.create(noiseRandom, -4, (double)1.0F);
            StringBuilder noiseConfig = new StringBuilder();
            noise.parityConfigString(noiseConfig);
            traceWriter.println("  GEODE_NOISE_CONFIG=" + noiseConfig);

            for (BlockPos pointInside : BlockPos.betweenClosed(
                origin.offset(config.minGenOffset, config.minGenOffset, config.minGenOffset),
                origin.offset(config.maxGenOffset, config.maxGenOffset, config.maxGenOffset)
            )) {
                double noiseOffset =
                    noise.getValue((double)pointInside.getX(), (double)pointInside.getY(), (double)pointInside.getZ()) *
                    config.noiseMultiplier;
                double distSumShell = (double)0.0F;
                double distSumCrack = (double)0.0F;

                for (int i = 0; i < pointPositions.size(); i++) {
                    distSumShell += net.minecraft.util.Mth.invSqrt(
                        pointInside.distSqr(pointPositions.get(i)) + (double)pointOffsets.get(i)
                    ) + noiseOffset;
                }

                for (BlockPos crackPoint : crackPoints) {
                    distSumCrack += net.minecraft.util.Mth.invSqrt(
                        pointInside.distSqr(crackPoint) + (double)config.geodeCrackSettings.crackPointOffset
                    ) + noiseOffset;
                }

                if (!(distSumShell < outerCrust)) {
                    String branch = "outer";
                    if (shouldGenerateCrack && distSumCrack >= crackSize && distSumShell < innerAir) {
                        branch = "crack_air";
                    } else if (distSumShell >= innerAir) {
                        branch = "filling";
                    } else if (distSumShell >= innermostBlockLayer) {
                        branch = "inner";
                    } else if (distSumShell >= innerCrust) {
                        branch = "middle";
                    }

                    traceWriter.println(
                        "  GEODE_SAMPLE pos=" +
                        pointInside.getX() + "," + pointInside.getY() + "," + pointInside.getZ() +
                        " noise=" + noiseOffset +
                        " shell=" + distSumShell +
                        " crack=" + distSumCrack +
                        " branch=" + branch
                    );
                }
            }
            traceWriter.flush();
        } catch (Exception e) {
            traceWriter.println(
                "LIVE_GEODE[" + liveTraceEventCounter.getAndIncrement() + "] STEP=" + stepIndex +
                " IDX=" + featureIndex +
                " " + featureName +
                " TRACE_ERROR=" + e.getMessage()
            );
            e.printStackTrace(traceWriter);
            traceWriter.flush();
        }
    }

    private static int decodeIntProviderSample(
        net.minecraft.util.valueproviders.IntProvider provider,
        List<LoggedRandomCall> calls,
        int[] cursor
    ) {
        if (provider instanceof net.minecraft.util.valueproviders.ConstantInt constantInt) {
            return constantInt.getMinValue();
        }

        if (provider instanceof net.minecraft.util.valueproviders.UniformInt uniformInt) {
            int min = uniformInt.getMinValue();
            int max = uniformInt.getMaxValue();
            int bound = max - min + 1;
            return min + takeNextInt(calls, cursor, bound);
        }

        throw new IllegalStateException("Unsupported IntProvider for geode tracing: " + provider.getClass().getName());
    }

    private static int takeNextInt(List<LoggedRandomCall> calls, int[] cursor, int expectedBound) {
        LoggedRandomCall call = calls.get(cursor[0]++);
        if (!"nextInt".equals(call.method) || call.bound != expectedBound) {
            throw new IllegalStateException(
                "Expected nextInt(" + expectedBound + "), got " + call.method + "(" + call.bound + ")"
            );
        }
        return (int)call.longValue;
    }

    private static float takeNextFloat(List<LoggedRandomCall> calls, int[] cursor) {
        LoggedRandomCall call = calls.get(cursor[0]++);
        if (!"nextFloat".equals(call.method)) {
            throw new IllegalStateException("Expected nextFloat, got " + call.method);
        }
        return (float)call.doubleValue;
    }

    private static double takeNextDouble(List<LoggedRandomCall> calls, int[] cursor) {
        LoggedRandomCall call = calls.get(cursor[0]++);
        if (!"nextDouble".equals(call.method)) {
            throw new IllegalStateException("Expected nextDouble, got " + call.method);
        }
        return call.doubleValue;
    }

    private static List<BlockPos> computeGeodeCrackPoints(BlockPos origin, int offsetIndex, int crackOffset) {
        List<BlockPos> points = new ArrayList<>(3);
        if (offsetIndex == 0) {
            points.add(origin.offset(crackOffset, 7, 0));
            points.add(origin.offset(crackOffset, 5, 0));
            points.add(origin.offset(crackOffset, 1, 0));
        } else if (offsetIndex == 1) {
            points.add(origin.offset(0, 7, crackOffset));
            points.add(origin.offset(0, 5, crackOffset));
            points.add(origin.offset(0, 1, crackOffset));
        } else if (offsetIndex == 2) {
            points.add(origin.offset(crackOffset, 7, crackOffset));
            points.add(origin.offset(crackOffset, 5, crackOffset));
            points.add(origin.offset(crackOffset, 1, crackOffset));
        } else {
            points.add(origin.offset(0, 7, 0));
            points.add(origin.offset(0, 5, 0));
            points.add(origin.offset(0, 1, 0));
        }
        return points;
    }

    private static void logLiveVegetationPatchShadow(
        String featureName,
        int stepIndex,
        int featureIndex,
        net.minecraft.world.level.levelgen.feature.FeaturePlaceContext<?> context,
        net.minecraft.world.level.levelgen.feature.configurations.VegetationPatchConfiguration config
    ) {
        if (traceWriter == null) return;
        if (!(context.random() instanceof net.minecraft.world.level.levelgen.WorldgenRandom worldgenRandom)) return;

        try {
            net.minecraft.world.level.levelgen.WorldgenRandom shadowRandom = cloneWorldgenRandom(worldgenRandom);
            if (shadowRandom == null) {
                traceWriter.println(
                    "LIVE_VEG_PATCH[" + liveTraceEventCounter.getAndIncrement() + "] STEP=" + stepIndex +
                    " IDX=" + featureIndex +
                    " " + featureName +
                    " TRACE_ERROR=clone_random_failed"
                );
                traceWriter.flush();
                return;
            }

            BlockPos origin = context.origin();
            int xRadius = config.xzRadius.sample(shadowRandom) + 1;
            int zRadius = config.xzRadius.sample(shadowRandom) + 1;
            Set<BlockPos> surface = traceVegetationPatchSurface(
                context.level(),
                config,
                shadowRandom,
                origin,
                xRadius,
                zRadius
            );

            traceWriter.println(
                "LIVE_VEG_PATCH[" + liveTraceEventCounter.getAndIncrement() + "] STEP=" + stepIndex +
                " IDX=" + featureIndex +
                " " + featureName +
                " ORIGIN=" + origin.getX() + "," + origin.getY() + "," + origin.getZ() +
                " X_RADIUS=" + xRadius +
                " Z_RADIUS=" + zRadius +
                " SURFACE_COUNT=" + surface.size()
            );

            int surfaceIndex = 0;
            for (BlockPos surfacePos : surface) {
                traceWriter.println(
                    "  LIVE_VEG_SURFACE[" + surfaceIndex + "]=" +
                    surfacePos.getX() + "," + surfacePos.getY() + "," + surfacePos.getZ()
                );
                surfaceIndex++;
            }
            traceWriter.flush();
        } catch (Exception e) {
            traceWriter.println(
                "LIVE_VEG_PATCH[" + liveTraceEventCounter.getAndIncrement() + "] STEP=" + stepIndex +
                " IDX=" + featureIndex +
                " " + featureName +
                " TRACE_ERROR=" + e.getMessage()
            );
            e.printStackTrace(traceWriter);
            traceWriter.flush();
        }
    }

    private static Set<BlockPos> traceVegetationPatchSurface(
        net.minecraft.world.level.WorldGenLevel level,
        net.minecraft.world.level.levelgen.feature.configurations.VegetationPatchConfiguration config,
        net.minecraft.util.RandomSource random,
        BlockPos origin,
        int xRadius,
        int zRadius
    ) {
        BlockPos.MutableBlockPos pos = origin.mutable();
        BlockPos.MutableBlockPos belowPos = pos.mutable();
        Direction inwards = config.surface.getDirection();
        Direction outwards = inwards.getOpposite();
        Set<BlockPos> surface = new HashSet<>();
        java.util.function.Predicate<BlockState> replaceable = (state) -> state.is(config.replaceable);

        pos.set(origin);
        belowPos.set(origin);
        int columnIndex = 0;

        for (int dx = -xRadius; dx <= xRadius; ++dx) {
            boolean isXEdge = dx == -xRadius || dx == xRadius;

            for (int dz = -zRadius; dz <= zRadius; ++dz) {
                boolean isZEdge = dz == -zRadius || dz == zRadius;
                boolean isEdge = isXEdge || isZEdge;
                boolean isCorner = isXEdge && isZEdge;
                boolean isEdgeButNotCorner = isEdge && !isCorner;
                float edgeRoll = Float.NaN;
                if (isCorner) {
                    if (traceWriter != null) {
                        traceWriter.println("  LIVE_VEG_COLUMN[" + columnIndex + "] dx=" + dx + " dz=" + dz + " skipped=corner");
                    }
                    columnIndex++;
                    continue;
                }

                if (isEdgeButNotCorner) {
                    if (config.extraEdgeColumnChance == 0.0F) {
                        if (traceWriter != null) {
                            traceWriter.println("  LIVE_VEG_COLUMN[" + columnIndex + "] dx=" + dx + " dz=" + dz + " skipped=edge_chance_zero");
                        }
                        columnIndex++;
                        continue;
                    }

                    edgeRoll = random.nextFloat();
                    if (edgeRoll > config.extraEdgeColumnChance) {
                        if (traceWriter != null) {
                            traceWriter.println(
                                "  LIVE_VEG_COLUMN[" + columnIndex + "] dx=" + dx +
                                " dz=" + dz +
                                " skipped=edge_roll roll=" + edgeRoll +
                                " chance=" + config.extraEdgeColumnChance
                            );
                        }
                        columnIndex++;
                        continue;
                    }
                }

                pos.setWithOffset(origin, dx, 0, dz);
                int airMoves = 0;
                for (int offset = 0; level.isStateAtPosition(pos, net.minecraft.world.level.block.state.BlockBehaviour.BlockStateBase::isAir) && offset < config.verticalRange; ++offset) {
                    pos.move(inwards);
                    airMoves++;
                }

                int solidMoves = 0;
                for (int offset = 0; level.isStateAtPosition(pos, (state) -> !state.isAir()) && offset < config.verticalRange; ++offset) {
                    pos.move(outwards);
                    solidMoves++;
                }

                belowPos.setWithOffset(pos, inwards);
                BlockState belowState = level.getBlockState(belowPos);
                BlockPos surfacePos = pos.immutable();
                BlockPos groundPosBefore = belowPos.immutable();
                boolean empty = level.isEmptyBlock(pos);
                boolean sturdy = empty && belowState.isFaceSturdy(level, belowPos, inwards.getOpposite());
                int depth = -1;
                boolean groundPlaced = false;
                boolean surfaceAdded = false;
                if (empty && sturdy) {
                    depth = config.depth.sample(random) + (config.extraBottomBlockChance > 0.0F && random.nextFloat() < config.extraBottomBlockChance ? 1 : 0);
                    BlockPos groundPos = belowPos.immutable();
                    groundPlaced = traceVegetationPatchGround(level, config, replaceable, random, belowPos, depth, columnIndex);
                    if (groundPlaced) {
                        surfaceAdded = surface.add(groundPos);
                    }
                }

                if (traceWriter != null) {
                    traceWriter.println(
                        "  LIVE_VEG_COLUMN[" + columnIndex + "] dx=" + dx +
                        " dz=" + dz +
                        " edge_roll=" + (Float.isNaN(edgeRoll) ? "NaN" : Float.toString(edgeRoll)) +
                        " air_moves=" + airMoves +
                        " solid_moves=" + solidMoves +
                        " pos=" + surfacePos.getX() + "," + surfacePos.getY() + "," + surfacePos.getZ() +
                        " below=" + groundPosBefore.getX() + "," + groundPosBefore.getY() + "," + groundPosBefore.getZ() +
                        " below_block=" + BuiltInRegistries.BLOCK.getKey(belowState.getBlock()) +
                        " empty=" + empty +
                        " sturdy=" + sturdy +
                        " depth=" + depth +
                        " ground=" + groundPlaced +
                        " surface_add=" + surfaceAdded
                    );
                }
                columnIndex++;
            }
        }

        return surface;
    }

    private static boolean traceVegetationPatchGround(
        net.minecraft.world.level.WorldGenLevel level,
        net.minecraft.world.level.levelgen.feature.configurations.VegetationPatchConfiguration config,
        java.util.function.Predicate<BlockState> replaceable,
        net.minecraft.util.RandomSource random,
        BlockPos.MutableBlockPos belowPos,
        int depth,
        int columnIndex
    ) {
        for (int i = 0; i < depth; ++i) {
            BlockState stateToPlace = config.groundState.getState(random, belowPos);
            BlockState belowState = level.getBlockState(belowPos);
            BlockPos currentPos = belowPos.immutable();
            if (!stateToPlace.is(belowState.getBlock())) {
                if (!replaceable.test(belowState)) {
                    if (traceWriter != null) {
                        traceWriter.println(
                            "  LIVE_VEG_GROUND[" + columnIndex + "][" + i + "] pos=" +
                            currentPos.getX() + "," + currentPos.getY() + "," + currentPos.getZ() +
                            " below_block=" + BuiltInRegistries.BLOCK.getKey(belowState.getBlock()) +
                            " place_block=" + BuiltInRegistries.BLOCK.getKey(stateToPlace.getBlock()) +
                            " action=stop_not_replaceable"
                        );
                    }
                    return i != 0;
                }
                if (traceWriter != null) {
                    traceWriter.println(
                        "  LIVE_VEG_GROUND[" + columnIndex + "][" + i + "] pos=" +
                        currentPos.getX() + "," + currentPos.getY() + "," + currentPos.getZ() +
                        " below_block=" + BuiltInRegistries.BLOCK.getKey(belowState.getBlock()) +
                        " place_block=" + BuiltInRegistries.BLOCK.getKey(stateToPlace.getBlock()) +
                        " action=replace"
                    );
                }
                level.setBlock(belowPos, stateToPlace, 2);
                belowPos.move(config.surface.getDirection());
            } else if (traceWriter != null) {
                traceWriter.println(
                    "  LIVE_VEG_GROUND[" + columnIndex + "][" + i + "] pos=" +
                    currentPos.getX() + "," + currentPos.getY() + "," + currentPos.getZ() +
                    " below_block=" + BuiltInRegistries.BLOCK.getKey(belowState.getBlock()) +
                    " place_block=" + BuiltInRegistries.BLOCK.getKey(stateToPlace.getBlock()) +
                    " action=same_block"
                );
            }
        }

        return true;
    }

    private static net.minecraft.world.level.levelgen.WorldgenRandom cloneWorldgenRandom(
        net.minecraft.world.level.levelgen.WorldgenRandom random
    ) {
        try {
            long[] seedState = getSeedState(random);
            return new net.minecraft.world.level.levelgen.WorldgenRandom(
                new net.minecraft.world.level.levelgen.XoroshiroRandomSource(seedState[0], seedState[1])
            );
        } catch (Exception e) {
            logSeedTraceError("cloneWorldgenRandom", e);
            return null;
        }
    }

    private static net.minecraft.world.level.levelgen.placement.PlacementModifier wrapPlacementModifierForLiveTracing(
        net.minecraft.world.level.levelgen.placement.PlacedFeature originalPlacedFeature,
        net.minecraft.world.level.levelgen.placement.PlacementModifier originalModifier,
        String featureName,
        int stepIndex,
        int featureIndex,
        int modifierIndex
    ) {
        return new net.minecraft.world.level.levelgen.placement.PlacementModifier() {
            @Override
            public java.util.stream.Stream<BlockPos> getPositions(
                net.minecraft.world.level.levelgen.placement.PlacementContext context,
                net.minecraft.util.RandomSource random,
                BlockPos origin
            ) {
                // Pass the OUTER context through unchanged: with the registry
                // holder rebound, the biome's memoized featureSet contains the
                // WRAPPED feature, so BiomeFilter.hasFeature must see it (the
                // outer topFeature). Substituting the original here made every
                // BiomeFilter reject and silently suppressed the feature.
                List<BlockPos> results = originalModifier.getPositions(context, random, origin).toList();
                logLiveModifierPlacement(featureName, stepIndex, featureIndex, modifierIndex, originalModifier, random, origin, results);
                return results.stream();
            }

            @Override
            public net.minecraft.world.level.levelgen.placement.PlacementModifierType<?> type() {
                return originalModifier.type();
            }
        };
    }

    private static void logLiveFeaturePlacement(
        String featureName,
        int stepIndex,
        int featureIndex,
        net.minecraft.world.level.levelgen.feature.FeaturePlaceContext<?> context
    ) {
        if (!traceFeatures || traceWriter == null) return;

        try {
            BlockPos origin = context.origin();
            BlockState stateAtOrigin = context.level().getBlockState(origin);
            BlockState stateBelow = context.level().getBlockState(origin.below());
            String originName = BuiltInRegistries.BLOCK.getKey(stateAtOrigin.getBlock()).toString();
            String belowName = BuiltInRegistries.BLOCK.getKey(stateBelow.getBlock()).toString();

            long seedLo = 0L;
            long seedHi = 0L;
            if (context.random() instanceof net.minecraft.world.level.levelgen.WorldgenRandom worldgenRandom) {
                long[] seedState = getSeedState(worldgenRandom);
                seedLo = seedState[0];
                seedHi = seedState[1];
            }

            traceWriter.println(
                "LIVE_FEATURE[" + liveTraceEventCounter.getAndIncrement() + "] STEP=" + stepIndex +
                " IDX=" + featureIndex +
                " " + featureName +
                " ORIGIN=" + origin.getX() + "," + origin.getY() + "," + origin.getZ() +
                " ORIGIN_BLOCK=" + originName +
                " BELOW_BLOCK=" + belowName +
                " SEED_LO=" + seedLo +
                " SEED_HI=" + seedHi
            );
            traceWriter.flush();
        } catch (Exception e) {
            traceWriter.println("# LIVE_TRACE_LOG_ERROR: " + e.getMessage());
            traceWriter.flush();
        }
    }

    private static void logLiveModifierPlacement(
        String featureName,
        int stepIndex,
        int featureIndex,
        int modifierIndex,
        net.minecraft.world.level.levelgen.placement.PlacementModifier modifier,
        net.minecraft.util.RandomSource random,
        BlockPos origin,
        List<BlockPos> results
    ) {
        if (!traceFeatures || traceWriter == null) return;

        try {
            long seedLo = 0L;
            long seedHi = 0L;
            if (random instanceof net.minecraft.world.level.levelgen.WorldgenRandom worldgenRandom) {
                long[] seedState = getSeedState(worldgenRandom);
                seedLo = seedState[0];
                seedHi = seedState[1];
            }

            traceWriter.println(
                "LIVE_MOD[" + liveTraceEventCounter.getAndIncrement() + "] STEP=" + stepIndex +
                " IDX=" + featureIndex +
                " MOD=" + modifierIndex +
                " TYPE=" + modifier.getClass().getSimpleName() +
                " " + featureName +
                " INPUT=" + origin.getX() + "," + origin.getY() + "," + origin.getZ() +
                " OUT_COUNT=" + results.size() +
                " SEED_LO=" + seedLo +
                " SEED_HI=" + seedHi
            );
            for (int i = 0; i < results.size(); i++) {
                BlockPos result = results.get(i);
                traceWriter.println("  LIVE_OUT[" + i + "]=" + result.getX() + "," + result.getY() + "," + result.getZ());
            }
            traceWriter.flush();
        } catch (Exception e) {
            traceWriter.println("# LIVE_TRACE_LOG_ERROR: " + e.getMessage());
            traceWriter.flush();
        }
    }

    private static void logLiveFeatureResult(
        String featureName,
        int stepIndex,
        int featureIndex,
        net.minecraft.world.level.levelgen.feature.FeaturePlaceContext<?> context,
        boolean placed,
        Map<BlockPos, String> beforeBlocks
    ) {
        if (!traceFeatures || traceWriter == null) return;

        try {
            List<String> blockChanges = collectLiveFeatureBlockChanges(context.level(), beforeBlocks);
            BlockPos origin = context.origin();

            traceWriter.println(
                "LIVE_PLACE[" + liveTraceEventCounter.getAndIncrement() + "] STEP=" + stepIndex +
                " IDX=" + featureIndex +
                " " + featureName +
                " ORIGIN=" + origin.getX() + "," + origin.getY() + "," + origin.getZ() +
                " PLACED=" + placed +
                " BLOCK_CHANGES=" + blockChanges.size()
            );
            for (String change : blockChanges) {
                traceWriter.println("  " + change);
            }
            traceWriter.flush();
        } catch (Exception e) {
            traceWriter.println("# LIVE_TRACE_LOG_ERROR: " + e.getMessage());
            traceWriter.flush();
        }
    }

    private static Map<BlockPos, String> captureLiveFeatureBlocks(
        net.minecraft.world.level.WorldGenLevel level,
        BlockPos origin
    ) {
        Map<BlockPos, String> blocks = new HashMap<>();
        ChunkPos centerChunk = new ChunkPos(origin);

        for (int chunkZ = centerChunk.z - 1; chunkZ <= centerChunk.z + 1; chunkZ++) {
            for (int chunkX = centerChunk.x - 1; chunkX <= centerChunk.x + 1; chunkX++) {
                ChunkAccess chunk = level.getChunk(chunkX, chunkZ);
                int minBlockX = chunk.getPos().getMinBlockX();
                int minBlockZ = chunk.getPos().getMinBlockZ();

                for (int z = 0; z < 16; z++) {
                    for (int y = MIN_Y; y < MAX_Y; y++) {
                        for (int x = 0; x < 16; x++) {
                            BlockPos pos = new BlockPos(minBlockX + x, y, minBlockZ + z);
                            BlockState state = chunk.getBlockState(pos);
                            String blockName = BuiltInRegistries.BLOCK.getKey(state.getBlock()).toString();
                            blocks.put(pos, blockName);
                        }
                    }
                }
            }
        }

        return blocks;
    }

    private static List<String> collectLiveFeatureBlockChanges(
        net.minecraft.world.level.WorldGenLevel level,
        Map<BlockPos, String> beforeBlocks
    ) {
        List<BlockPos> positions = new ArrayList<>(beforeBlocks.keySet());
        positions.sort(Comparator
            .<BlockPos>comparingInt(pos -> pos.getZ())
            .thenComparingInt(pos -> pos.getY())
            .thenComparingInt(pos -> pos.getX()));

        List<String> changes = new ArrayList<>();
        for (BlockPos pos : positions) {
            String before = beforeBlocks.get(pos);
            String after = BuiltInRegistries.BLOCK.getKey(level.getBlockState(pos).getBlock()).toString();
            if (!Objects.equals(before, after)) {
                changes.add(
                    "BLOCK pos=" + pos.getX() + "," + pos.getY() + "," + pos.getZ() +
                    " old=" + before +
                    " new=" + after
                );
            }
        }

        return changes;
    }

    /**
     * Trace features for a chunk - runs actual placement with tracing
     */
    private static void traceChunkFeatures(ServerLevel level, int chunkX, int chunkZ) {
        if (!traceFeatures || traceWriter == null) return;

        try {
            traceWriter.println("# CHUNK (" + chunkX + ", " + chunkZ + ")");

            net.minecraft.world.level.chunk.ChunkGenerator generator = level.getChunkSource().getGenerator();

            // Calculate origin
            int minSectionY = level.getMinSectionY();
            int originX = chunkX * 16;
            int originY = minSectionY * 16;
            int originZ = chunkZ * 16;
            BlockPos origin = new BlockPos(originX, originY, originZ);
            traceWriter.println("# Origin: " + originX + ", " + originY + ", " + originZ);

            // Create random and get decoration seed
            net.minecraft.world.level.levelgen.WorldgenRandom random = new net.minecraft.world.level.levelgen.WorldgenRandom(
                new net.minecraft.world.level.levelgen.XoroshiroRandomSource(
                    net.minecraft.world.level.levelgen.RandomSupport.generateUniqueSeed()));
            long decorationSeed = random.setDecorationSeed(level.getSeed(), originX, originZ);
            traceWriter.println("# DecorationSeed: " + decorationSeed);

            // Get featuresPerStep via reflection
            java.lang.reflect.Field featuresField = net.minecraft.world.level.chunk.ChunkGenerator.class.getDeclaredField("featuresPerStep");
            featuresField.setAccessible(true);
            @SuppressWarnings("unchecked")
            java.util.function.Supplier<List<net.minecraft.world.level.biome.FeatureSorter.StepFeatureData>> featuresSupplier =
                (java.util.function.Supplier<List<net.minecraft.world.level.biome.FeatureSorter.StepFeatureData>>) featuresField.get(generator);

            List<net.minecraft.world.level.biome.FeatureSorter.StepFeatureData> featureList = featuresSupplier.get();

            Registry<net.minecraft.world.level.levelgen.placement.PlacedFeature> featureRegistry =
                level.registryAccess().lookupOrThrow(Registries.PLACED_FEATURE);

            traceWriter.println();

            // Trace each step
            for (int stepIndex = 0; stepIndex < featureList.size(); stepIndex++) {
                net.minecraft.world.level.biome.FeatureSorter.StepFeatureData stepData = featureList.get(stepIndex);
                List<net.minecraft.world.level.levelgen.placement.PlacedFeature> features = stepData.features();

                if (features.isEmpty()) continue;

                boolean wroteStepHeader = false;

                for (int idx = 0; idx < features.size(); idx++) {
                    net.minecraft.world.level.levelgen.placement.PlacedFeature feature = features.get(idx);

                    Optional<net.minecraft.resources.ResourceKey<net.minecraft.world.level.levelgen.placement.PlacedFeature>> keyOpt =
                        featureRegistry.getResourceKey(feature);
                    String featureName = keyOpt.map(k -> k.identifier().toString()).orElse("(unnamed)");

                    if (traceFeatureFilter != null && !traceFeatureFilter.equals(featureName)) {
                        continue;
                    }

                    if (!wroteStepHeader) {
                        traceWriter.println("# ===== STEP " + stepIndex + " (" + features.size() + " features) =====");
                        wroteStepHeader = true;
                    }

                    // Set feature seed
                    random.setFeatureSeed(decorationSeed, idx, stepIndex);

                    traceWriter.println("FEATURE STEP=" + stepIndex + " IDX=" + idx + " " + featureName);

                    // Get seed state before tracing
                    long[] savedSeedState = getSeedState(random);
                    long savedSeedLo = savedSeedState[0];
                    long savedSeedHi = savedSeedState[1];
                    traceWriter.println("  SEED_BEFORE: lo=" + savedSeedLo + " hi=" + savedSeedHi);

                    // Trace through modifiers
                    traceFeatureModifiers(feature, level, generator, random, origin, traceFeatureFilter != null);

                    traceWriter.println();
                }
            }

            traceWriter.println("# END CHUNK (" + chunkX + ", " + chunkZ + ")");
            traceWriter.flush();

        } catch (Exception e) {
            traceWriter.println("# ERROR: " + e.getMessage());
            e.printStackTrace(traceWriter);
        }
    }

    /**
     * Trace a feature's placement modifiers
     */
    private static void traceFeatureModifiers(
        net.minecraft.world.level.levelgen.placement.PlacedFeature feature,
        ServerLevel level,
        net.minecraft.world.level.chunk.ChunkGenerator generator,
        net.minecraft.world.level.levelgen.WorldgenRandom random,
        BlockPos origin,
        boolean dumpAllPositions
    ) {
        try {
            List<net.minecraft.world.level.levelgen.placement.PlacementModifier> modifiers = feature.placement();
            net.minecraft.world.level.levelgen.placement.PlacementContext context =
                new net.minecraft.world.level.levelgen.placement.PlacementContext(level, generator, Optional.of(feature));

            List<BlockPos> currentPositions = List.of(origin);

            for (int i = 0; i < modifiers.size(); i++) {
                net.minecraft.world.level.levelgen.placement.PlacementModifier modifier = modifiers.get(i);
                List<BlockPos> prevPositions = new ArrayList<>(currentPositions);

                // Apply modifier to all current positions
                List<BlockPos> newPositions = new ArrayList<>();
                List<String> transformations = new ArrayList<>();

                for (BlockPos pos : currentPositions) {
                    List<BlockPos> results = modifier.getPositions(context, random, pos).toList();
                    for (BlockPos result : results) {
                        newPositions.add(result);
                        if (dumpAllPositions || transformations.size() < 10) {
                            transformations.add("      POS[" + (newPositions.size() - 1) + "]: " +
                                pos.getX() + "," + pos.getY() + "," + pos.getZ() + " -> " +
                                result.getX() + "," + result.getY() + "," + result.getZ());
                        }
                    }
                }

                traceWriter.println("  [" + i + "] " + modifier.getClass().getSimpleName() +
                    ": " + prevPositions.size() + " -> " + newPositions.size() + " positions");

                for (String t : transformations) {
                    traceWriter.println(t);
                }
                if (!dumpAllPositions && newPositions.size() > transformations.size()) {
                    traceWriter.println("      ... and " + (newPositions.size() - transformations.size()) + " more");
                }

                // Log seed state after
                long[] seedAfter = getSeedState(random);
                traceWriter.println("      SEED_AFTER: lo=" + seedAfter[0] + " hi=" + seedAfter[1]);

                currentPositions = newPositions;
                if (currentPositions.isEmpty()) break;
            }

            traceWriter.println("  TRACE_FINAL: " + currentPositions.size() + " positions");
            if (dumpAllPositions) {
                for (int i = 0; i < currentPositions.size(); i++) {
                    BlockPos pos = currentPositions.get(i);
                    traceWriter.println("      FINAL_POS[" + i + "]: " +
                        pos.getX() + "," + pos.getY() + "," + pos.getZ());
                }
            }

        } catch (Exception e) {
            traceWriter.println("  ERROR: " + e.getMessage());
        }
    }

    private static long[] getSeedState(net.minecraft.world.level.levelgen.WorldgenRandom random) {
        try {
            java.lang.reflect.Field randomField = net.minecraft.world.level.levelgen.WorldgenRandom.class.getDeclaredField("randomSource");
            randomField.setAccessible(true);
            Object randomSource = randomField.get(random);
            if (randomSource == null) {
                throw new IllegalStateException("WorldgenRandom.randomSource is null");
            }

            java.lang.reflect.Field implField = randomSource.getClass().getDeclaredField("randomNumberGenerator");
            implField.setAccessible(true);
            Object generator = implField.get(randomSource);
            if (generator == null) {
                throw new IllegalStateException("XoroshiroRandomSource.randomNumberGenerator is null");
            }

            java.lang.reflect.Field loField = generator.getClass().getDeclaredField("seedLo");
            java.lang.reflect.Field hiField = generator.getClass().getDeclaredField("seedHi");
            loField.setAccessible(true);
            hiField.setAccessible(true);
            return new long[]{loField.getLong(generator), hiField.getLong(generator)};
        } catch (Exception e) {
            logSeedTraceError("getSeedState", e);
            return new long[]{0L, 0L};
        }
    }

    private static void logSeedTraceError(String source, Exception e) {
        String key = source + ":" + e.getClass().getName() + ":" + String.valueOf(e.getMessage());
        if (!loggedSeedTraceErrors.add(key)) {
            return;
        }

        String message = "# SEED_TRACE_ERROR[" + source + "]: " + e.getClass().getSimpleName() + ": " + e.getMessage();
        if (traceWriter != null) {
            traceWriter.println(message);
            e.printStackTrace(traceWriter);
            traceWriter.flush();
        } else {
            System.err.println(message);
            e.printStackTrace(System.err);
        }
    }

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
                            out.println("RING," + setId + "," + (index++) + "," + p.x + "," + p.z);
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
            .comparingInt((Map.Entry<ChunkPos, ChunkAccess> entry) -> entry.getKey().x)
            .thenComparingInt(entry -> entry.getKey().z));

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
                out.println("CHUNK " + chunkPos.x + "," + chunkPos.z);

                Map<Structure, LongSet> references = chunk.getAllReferences();
                if (references.isEmpty()) {
                    out.println("  REFERENCES none");
                } else {
                    List<Map.Entry<Structure, LongSet>> refEntries = new ArrayList<>(references.entrySet());
                    refEntries.sort(Comparator.comparing(e -> structureName(structuresRegistry, e.getKey())));
                    for (Map.Entry<Structure, LongSet> refEntry : refEntries) {
                        List<String> refChunks = new ArrayList<>();
                        for (long ref : refEntry.getValue()) {
                            ChunkPos refPos = new ChunkPos(ref);
                            referencedStartChunks.add(ref);
                            refChunks.add(refPos.x + "," + refPos.z);
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
                ChunkPos startChunkPos = new ChunkPos(ref);
                ChunkAccess startChunk = level.getChunk(startChunkPos.x, startChunkPos.z, ChunkStatus.STRUCTURE_STARTS);
                out.println("SOURCE_CHUNK " + startChunkPos.x + "," + startChunkPos.z);

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
            " start_chunk=" + start.getChunkPos().x + "," + start.getChunkPos().z +
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
            (MinecraftSessionService) null,
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
            super(serverThread, storage, packRepository, worldStem, Proxy.NO_PROXY,
                  DataFixers.getDataFixer(), NO_SERVICES,
                  net.minecraft.server.level.progress.LoggingLevelLoadListener.forDedicatedServer());
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
