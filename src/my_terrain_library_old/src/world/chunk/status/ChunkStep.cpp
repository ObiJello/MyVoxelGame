#include "world/chunk/status/ChunkStep.h"
#include "world/chunk/status/ChunkStatusTasks.h"
#include "world/ProtoChunk.h"

// Reference: net/minecraft/world/level/chunk/status/ChunkStep.java

namespace minecraft {
namespace world {
namespace chunk {
namespace status {

// Apply this step to a chunk
// Reference: ChunkStep.java lines 19-26
// Java: public CompletableFuture<ChunkAccess> apply(WorldGenContext context, StaticCache2D<GenerationChunkHolder> cache, ChunkAccess chunk)
std::shared_ptr<util::CompletableFuture<::world::IChunk*>> ChunkStep::apply(
    WorldGenContext& context,
    const std::vector<std::vector<::world::IChunk*>>& cache,
    ::world::IChunk* chunk
) const {
    // Profile the overall step application
    std::string profileName = "step.apply." + m_targetStatus->getName();

    // Reference: ChunkStep.java lines 19-26
    // Execute the task - it returns a CompletableFuture
    // The task may run async (e.g., NOISE, BIOMES) or sync (everything else)
    auto taskFuture = m_task(context, *this, cache, chunk);

    // Reference: ChunkStep.java lines 22, 28-40 (completeChunkGeneration)
    // Chain status update using thenApply - this runs after the task completes
    // Java: return this.task.doWork(...).thenApply((newCenterChunk) -> this.completeChunkGeneration(newCenterChunk, profiledDuration));
    const ChunkStatus* targetStatus = m_targetStatus;
    return taskFuture->thenApply([targetStatus](::world::IChunk* result) -> ::world::IChunk* {
        // Update the chunk's status after successful generation
        if (result) {
            ::world::ProtoChunk* protoChunk = dynamic_cast<::world::ProtoChunk*>(result);
            if (protoChunk) {
                // Reference: ChunkStep.java lines 30-32
                // if (protochunk.getPersistedStatus().isBefore(this.targetStatus))
                //     protochunk.setPersistedStatus(this.targetStatus)
                protoChunk->setStatus(targetStatus);
            }
        }
        return result;
    });
}

// Create the default Minecraft generation pipeline
// Reference: net/minecraft/world/level/chunk/status/ChunkPyramid.java GENERATION_PYRAMID
ChunkPipeline ChunkPipeline::createDefault() {
    ChunkPipeline pipeline;

    // EMPTY - index 0
    // Reference: ChunkPyramid.java - builder(EMPTY).build()
    ChunkStep emptyStep = ChunkStepBuilder(ChunkStatus::EMPTY)
        .setTask(ChunkStatusTasks::passThrough)
        .build();
    pipeline.addStep(std::move(emptyStep));

    // STRUCTURE_STARTS - index 1
    // Reference: ChunkPyramid.java - new Builder(STRUCTURE_STARTS, empty)
    //     .setTask(ChunkStatusTasks::generateStructureStarts)
    ChunkStep structureStartsStep = ChunkStepBuilder(ChunkStatus::STRUCTURE_STARTS, pipeline.getStep(0))
        .setTask(ChunkStatusTasks::generateStructureStarts)
        .build();
    pipeline.addStep(std::move(structureStartsStep));

    // STRUCTURE_REFERENCES - index 2
    // Reference: ChunkPyramid.java - needs 8 radius for structure distance
    //     .addRequirement(STRUCTURE_STARTS, 8)
    ChunkStep structureRefsStep = ChunkStepBuilder(ChunkStatus::STRUCTURE_REFERENCES, pipeline.getStep(1))
        .addRequirement(ChunkStatus::STRUCTURE_STARTS, ChunkStatus::MAX_STRUCTURE_DISTANCE)
        .setTask(ChunkStatusTasks::generateStructureReferences)
        .build();
    pipeline.addStep(std::move(structureRefsStep));

    // BIOMES - index 3
    // Reference: ChunkPyramid.java
    ChunkStep biomesStep = ChunkStepBuilder(ChunkStatus::BIOMES, pipeline.getStep(2))
        .setTask(ChunkStatusTasks::generateBiomes)
        .build();
    pipeline.addStep(std::move(biomesStep));

    // NOISE - index 4
    // Reference: ChunkPyramid.java - blockStateWriteRadius(0)
    ChunkStep noiseStep = ChunkStepBuilder(ChunkStatus::NOISE, pipeline.getStep(3))
        .setBlockStateWriteRadius(0)
        .setTask(ChunkStatusTasks::generateNoise)
        .build();
    pipeline.addStep(std::move(noiseStep));

    // SURFACE - index 5
    // Reference: ChunkPyramid.java - blockStateWriteRadius(0)
    ChunkStep surfaceStep = ChunkStepBuilder(ChunkStatus::SURFACE, pipeline.getStep(4))
        .setBlockStateWriteRadius(0)
        .setTask(ChunkStatusTasks::generateSurface)
        .build();
    pipeline.addStep(std::move(surfaceStep));

    // CARVERS - index 6
    // Reference: ChunkPyramid.java - blockStateWriteRadius(0)
    ChunkStep carversStep = ChunkStepBuilder(ChunkStatus::CARVERS, pipeline.getStep(5))
        .setBlockStateWriteRadius(0)
        .setTask(ChunkStatusTasks::generateCarvers)
        .build();
    pipeline.addStep(std::move(carversStep));

    // FEATURES - index 7
    // Reference: ChunkPyramid.java - addRequirement(CARVERS, 1), blockStateWriteRadius(1)
    ChunkStep featuresStep = ChunkStepBuilder(ChunkStatus::FEATURES, pipeline.getStep(6))
        .addRequirement(ChunkStatus::CARVERS, 1)
        .setBlockStateWriteRadius(1)
        .setTask(ChunkStatusTasks::generateFeatures)
        .build();
    pipeline.addStep(std::move(featuresStep));

    // INITIALIZE_LIGHT - index 8
    // Reference: ChunkPyramid.java - addRequirement(FEATURES, 1)
    ChunkStep initLightStep = ChunkStepBuilder(ChunkStatus::INITIALIZE_LIGHT, pipeline.getStep(7))
        .addRequirement(ChunkStatus::FEATURES, 1)
        .setTask(ChunkStatusTasks::initializeLight)
        .build();
    pipeline.addStep(std::move(initLightStep));

    // LIGHT - index 9
    // Reference: ChunkPyramid.java - addRequirement(INITIALIZE_LIGHT, 1)
    ChunkStep lightStep = ChunkStepBuilder(ChunkStatus::LIGHT, pipeline.getStep(8))
        .addRequirement(ChunkStatus::INITIALIZE_LIGHT, 1)
        .setTask(ChunkStatusTasks::light)
        .build();
    pipeline.addStep(std::move(lightStep));

    // SPAWN - index 10
    // Reference: ChunkPyramid.java
    ChunkStep spawnStep = ChunkStepBuilder(ChunkStatus::SPAWN, pipeline.getStep(9))
        .setTask(ChunkStatusTasks::generateSpawn)
        .build();
    pipeline.addStep(std::move(spawnStep));

    // FULL - index 11
    // Reference: ChunkPyramid.java - addRequirement(SPAWN, 1)
    ChunkStep fullStep = ChunkStepBuilder(ChunkStatus::FULL, pipeline.getStep(10))
        .addRequirement(ChunkStatus::SPAWN, 1)
        .setTask(ChunkStatusTasks::full)
        .build();
    pipeline.addStep(std::move(fullStep));

    return pipeline;
}

} // namespace status
} // namespace chunk
} // namespace world
} // namespace minecraft
