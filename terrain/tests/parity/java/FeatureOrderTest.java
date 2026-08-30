import net.minecraft.SharedConstants;
import net.minecraft.server.Bootstrap;
import net.minecraft.core.registries.Registries;
import net.minecraft.server.*;
import net.minecraft.server.level.*;
import net.minecraft.server.dedicated.*;
import net.minecraft.server.packs.repository.*;
import net.minecraft.world.level.*;
import net.minecraft.world.level.levelgen.*;
import net.minecraft.world.level.levelgen.placement.*;
import net.minecraft.world.level.biome.*;
import net.minecraft.world.level.chunk.*;
import net.minecraft.world.level.chunk.status.*;
import net.minecraft.core.*;
import net.minecraft.resources.*;
import net.minecraft.data.worldgen.*;
import java.util.*;
import java.io.*;
import java.lang.reflect.*;
import java.nio.file.*;

public class FeatureOrderTest {
    public static void main(String[] args) throws Exception {
        // Bootstrap
        SharedConstants.tryDetectVersion();
        Bootstrap.bootStrap();
        
        // Create a minimal server to access registries
        Path tempPath = Files.createTempDirectory("mc_feature_test_");
        
        // Get pack repository
        PackRepository packRepository = ServerPacksSource.createVanillaTrustedRepository();
        packRepository.reload();
        Collection<String> selectedPacks = List.of("vanilla");
        packRepository.setSelected(selectedPacks);
        
        System.out.println("=== Java Feature Order (Step 6) ===\n");
        
        // Load world data using WorldLoader
        WorldLoader.PackConfig packConfig = new WorldLoader.PackConfig(packRepository, WorldDataConfiguration.DEFAULT, false, false);
        WorldLoader.InitConfig initConfig = new WorldLoader.InitConfig(packConfig, Commands.CommandSelection.DEDICATED, 4);
        
        WorldStem worldStem = WorldLoader.load(initConfig, (resourceManager, dataPackConfig) -> {
            LayeredRegistryAccess<RegistryLayer> registries = RegistryLayer.createRegistryAccess();
            
            registries = WorldLoader.loadAndReplaceLayer(
                resourceManager,
                registries,
                RegistryLayer.WORLDGEN,
                RegistryDataLoader.WORLDGEN_REGISTRIES
            );
            
            return new WorldLoader.DataLoadOutput<>(
                registries.compositeAccess(),
                registries
            );
        }, (closeableResourceManager, registries, dataPackConfig, reloadableServerRegistries) -> {
            closeableResourceManager.close();
            
            RegistryAccess.Frozen frozenRegistries = reloadableServerRegistries.compositeAccess();
            
            // Use new PrimaryLevelData pattern
            WorldDimensions worldDimensions = new WorldDimensions(
                frozenRegistries.lookupOrThrow(Registries.LEVEL_STEM)
            );
            
            WorldOptions worldOptions = new WorldOptions(12345L, false, false);
            WorldDimensions.Complete complete = worldDimensions.bake(
                frozenRegistries.lookupOrThrow(Registries.LEVEL_STEM)
            );
            
            PrimaryLevelData worldData = new PrimaryLevelData(
                LevelSettings.parse(
                    net.minecraft.world.Difficulty.NORMAL,
                    GameRules.bootstrap(new GameRules())
                ),
                worldOptions,
                complete,
                PrimaryLevelData.SpecialWorldProperty.NONE,
                com.mojang.serialization.Lifecycle.stable()
            );
            
            return new WorldStem(
                closeableResourceManager,
                reloadableServerRegistries,
                frozenRegistries,
                worldData
            );
        }, Runnable::run, Runnable::run);
        
        // Get biome source
        RegistryAccess registryAccess = worldStem.registries().compositeAccess();
        Registry<Biome> biomeRegistry = registryAccess.lookupOrThrow(Registries.BIOME);
        Registry<PlacedFeature> placedFeatureRegistry = registryAccess.lookupOrThrow(Registries.PLACED_FEATURE);
        
        // Get plains biome to check features
        Biome plainsBiome = biomeRegistry.getValue(ResourceLocation.parse("minecraft:plains"));
        
        System.out.println("Plains biome features:");
        var featureSettings = plainsBiome.getGenerationSettings();
        var featureList = featureSettings.features();
        
        for (int step = 0; step < featureList.size(); step++) {
            var stepFeatures = featureList.get(step);
            System.out.println("\nStep " + step + ": " + stepFeatures.size() + " features");
            if (step == 6) {  // Underground ores step
                int idx = 0;
                for (var featureHolder : stepFeatures) {
                    PlacedFeature feature = featureHolder.value();
                    ResourceKey<PlacedFeature> key = placedFeatureRegistry.getResourceKey(feature).orElse(null);
                    String name = key != null ? key.location().toString() : "unknown";
                    System.out.println("  IDX=" + idx + " " + name);
                    idx++;
                }
            }
        }
        
        System.out.println("\n=== Done ===");
        
        // Cleanup
        worldStem.close();
        Files.walk(tempPath)
            .sorted(Comparator.reverseOrder())
            .forEach(p -> { try { Files.delete(p); } catch (Exception e) {} });
    }
}
