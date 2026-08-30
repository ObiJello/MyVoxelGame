// Exact imports from working MinecraftAsyncChunkTest
import com.mojang.authlib.yggdrasil.YggdrasilAuthenticationService;
import com.mojang.datafixers.DataFixer;
import com.mojang.serialization.Lifecycle;
import net.minecraft.CrashReport;
import net.minecraft.SharedConstants;
import net.minecraft.commands.Commands;
import net.minecraft.core.Registry;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.core.registries.Registries;
import net.minecraft.server.Bootstrap;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.Services;
import net.minecraft.server.WorldLoader;
import net.minecraft.server.WorldStem;
import net.minecraft.server.dedicated.DedicatedServer;
import net.minecraft.server.dedicated.DedicatedServerSettings;
import net.minecraft.server.level.ServerChunkCache;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.packs.repository.PackRepository;
import net.minecraft.server.packs.repository.ServerPacksSource;
import net.minecraft.server.permissions.LevelBasedPermissionSet;
import net.minecraft.util.Util;
import net.minecraft.util.datafix.DataFixers;
import net.minecraft.world.Difficulty;
import net.minecraft.world.flag.FeatureFlags;
import net.minecraft.world.level.*;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.chunk.ChunkAccess;
import net.minecraft.world.level.chunk.LevelChunkSection;
import net.minecraft.world.level.chunk.status.ChunkStatus;
import net.minecraft.world.level.dimension.LevelStem;
import net.minecraft.world.level.gamerules.GameRules;
import net.minecraft.world.level.levelgen.WorldDimensions;
import net.minecraft.world.level.levelgen.WorldOptions;
import net.minecraft.world.level.levelgen.presets.WorldPresets;
import net.minecraft.world.level.storage.LevelStorageSource;
import net.minecraft.world.level.storage.PrimaryLevelData;
import net.minecraft.world.level.storage.WorldData;

import java.io.*;
import java.net.Proxy;
import java.nio.file.*;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.*;

public class BlockChangeTracer {
    static final long SEED = 12345L;

    public static void main(String[] args) throws Exception {
        int chunkX = 500, chunkZ = 500;
        String outputPath = null;

        for (int i = 0; i < args.length; i++) {
            if (args[i].equals("--chunk") && i + 2 < args.length) {
                chunkX = Integer.parseInt(args[++i]);
                chunkZ = Integer.parseInt(args[++i]);
            } else if (args[i].equals("--output") && i + 1 < args.length) {
                outputPath = args[++i];
            }
        }
        if (outputPath == null) outputPath = "tests/output/java_block_trace_" + chunkX + "_" + chunkZ + ".txt";

        System.out.println("=== Java Block Change Tracer ===");
        System.out.println("Chunk: (" + chunkX + ", " + chunkZ + ")");

        SharedConstants.tryDetectVersion();
        CrashReport.preload();
        Bootstrap.bootStrap();
        Bootstrap.validate();
        Util.startTimerHackThread();

        Path tempWorldPath = Files.createTempDirectory("mc_block_trace_");
        DedicatedServer server = null;

        try {
            LevelStorageSource lss = LevelStorageSource.createDefault(tempWorldPath);
            LevelStorageSource.LevelStorageAccess storage = lss.validateAndCreateAccess("test_world");
            PackRepository packs = ServerPacksSource.createPackRepository(storage);
            packs.reload();

            WorldDataConfiguration dataConfig = new WorldDataConfiguration(
                new DataPackConfig(List.of("vanilla"), List.of()), FeatureFlags.DEFAULT_FLAGS);
            WorldLoader.PackConfig packConfig = new WorldLoader.PackConfig(packs, dataConfig, false, true);
            WorldLoader.InitConfig initConfig = new WorldLoader.InitConfig(
                packConfig, Commands.CommandSelection.DEDICATED, LevelBasedPermissionSet.GAMEMASTER);

            WorldStem worldStem = Util.blockUntilDone(executor -> WorldLoader.load(
                initConfig,
                context -> {
                    Registry<LevelStem> dims = context.datapackDimensions().lookupOrThrow(Registries.LEVEL_STEM);
                    LevelSettings ls = new LevelSettings("test_world", GameType.CREATIVE, false,
                        Difficulty.NORMAL, true, new GameRules(context.dataConfiguration().enabledFeatures()),
                        context.dataConfiguration());
                    WorldOptions wo = new WorldOptions(SEED, false, false);
                    WorldDimensions wd = WorldPresets.createNormalWorldDimensions(context.datapackWorldgen());
                    WorldDimensions.Complete c = wd.bake(dims);
                    Lifecycle lc = c.lifecycle().add(context.datapackWorldgen().allRegistriesLifecycle());
                    return new WorldLoader.DataLoadOutput<WorldData>(
                        new PrimaryLevelData(ls, wo, c.specialWorldProperty(), lc),
                        c.dimensionsRegistryAccess());
                },
                WorldStem::new, Util.backgroundExecutor(), executor
            )).get();

            Path propsPath = tempWorldPath.resolve("server.properties");
            Files.writeString(propsPath, "online-mode=false\nmax-tick-time=-1\n");
            DedicatedServerSettings settings = new DedicatedServerSettings(propsPath);
            Services services = Services.create(new YggdrasilAuthenticationService(Proxy.NO_PROXY), tempWorldPath.toFile());

            final var fs = storage; final var fp = packs; final var fw = worldStem;
            final var fst = settings; final var fsv = services;
            server = MinecraftServer.spin(t ->
                new DedicatedServer(t, fs, fp, fw, fst, DataFixers.getDataFixer(), fsv));

            while (!server.isReady()) { Thread.sleep(100); if (!server.isRunning()) throw new RuntimeException("Server died"); }
            System.out.println("Server ready.");

            ServerLevel level = server.overworld();
            ServerChunkCache cc = level.getChunkSource();

            // Step 1: Generate to CARVERS, snapshot
            System.out.println("Generating to CARVERS...");
            final int cx = chunkX, cz = chunkZ;
            CompletableFuture<ChunkAccess> f1 = new CompletableFuture<>();
            server.execute(() -> { try { f1.complete(cc.getChunk(cx, cz, ChunkStatus.CARVERS, true)); } catch (Exception e) { f1.completeExceptionally(e); } });
            ChunkAccess carversChunk = f1.get(120, TimeUnit.SECONDS);

            int numSections = carversChunk.getSections().length;
            int minY = carversChunk.getMinY();

            // Snapshot all non-air blocks
            Map<Long, String> snapshot = new HashMap<>();
            for (int sy = 0; sy < numSections; sy++) {
                int baseY = minY + sy * 16;
                LevelChunkSection sec = carversChunk.getSection(sy);
                for (int x = 0; x < 16; x++)
                    for (int y = 0; y < 16; y++)
                        for (int z = 0; z < 16; z++) {
                            BlockState st = sec.getBlockState(x, y, z);
                            String name = BuiltInRegistries.BLOCK.getKey(st.getBlock()).toString();
                            if (!name.equals("minecraft:air"))
                                snapshot.put(pk(x, baseY + y, z), name);
                        }
            }
            System.out.println("Snapshot: " + snapshot.size() + " non-air blocks");

            // Step 2: Generate to FEATURES
            System.out.println("Generating to FEATURES...");
            CompletableFuture<ChunkAccess> f2 = new CompletableFuture<>();
            server.execute(() -> { try { f2.complete(cc.getChunk(cx, cz, ChunkStatus.FEATURES, true)); } catch (Exception e) { f2.completeExceptionally(e); } });
            ChunkAccess featChunk = f2.get(120, TimeUnit.SECONDS);

            // Step 3: Diff
            System.out.println("Diffing...");
            new File(outputPath).getParentFile().mkdirs();
            try (PrintWriter out = new PrintWriter(new FileWriter(outputPath))) {
                out.println("# Java Block Change Trace");
                out.println("# Chunk: (" + cx + ", " + cz + ")");
                out.println("# Seed: " + SEED);
                out.println("# Format: BLOCK_SET pos=worldX,worldY,worldZ old=block new=block");
                out.println();

                int changes = 0;
                int bx = cx * 16, bz = cz * 16;
                TreeMap<String, String> sorted = new TreeMap<>();

                for (int sy = 0; sy < numSections; sy++) {
                    int baseY = minY + sy * 16;
                    LevelChunkSection sec = featChunk.getSection(sy);
                    for (int x = 0; x < 16; x++)
                        for (int y = 0; y < 16; y++)
                            for (int z = 0; z < 16; z++) {
                                BlockState st = sec.getBlockState(x, y, z);
                                int wy = baseY + y;
                                String nn = BuiltInRegistries.BLOCK.getKey(st.getBlock()).toString();
                                String on = snapshot.getOrDefault(pk(x, wy, z), "minecraft:air");
                                if (!nn.equals(on)) {
                                    String key = String.format("%05d,%05d,%05d", bx + x, wy + 64, bz + z);
                                    sorted.put(key, "BLOCK_SET pos=" + (bx+x) + "," + wy + "," + (bz+z) +
                                        " old=" + on + " new=" + nn);
                                    changes++;
                                }
                            }
                }
                for (String line : sorted.values()) out.println(line);
                System.out.println("Total changes: " + changes);
                System.out.println("Output: " + outputPath);
            }

            server.halt(true);
        } finally {
            if (tempWorldPath != null) delDir(tempWorldPath.toFile());
        }
    }

    static long pk(int lx, int y, int lz) { return ((long)(lx&0xF)<<14)|((long)((y+64)&0x3FF)<<4)|(lz&0xF); }
    static void delDir(File d) { File[] fs=d.listFiles(); if(fs!=null) for(File f:fs){if(f.isDirectory())delDir(f);else f.delete();} d.delete(); }
}
