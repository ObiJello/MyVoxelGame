#include "world/chunk/status/ChunkPyramid.h"
#include "world/chunk/status/ChunkStatus.h"
#include "world/chunk/status/ChunkStep.h"
#include "world/chunk/status/ChunkStatusTasks.h"
#include "server/level/GenerationChunkHolder.h"
#include "server/level/ChunkHolder.h"
#include "server/level/ChunkLevel.h"
#include <iostream>

// Reference: net/minecraft/world/level/chunk/status/ChunkPyramid.java

namespace minecraft {
namespace world {
namespace chunk {
namespace status {

// Static member definitions
bool ChunkPyramid::s_initialized = false;
ChunkPyramid ChunkPyramid::s_generationPyramid({});
ChunkPyramid ChunkPyramid::s_loadingPyramid({});

ChunkPyramid::ChunkPyramid(std::vector<ChunkStep> steps)
    : m_steps(std::move(steps))
{}

const ChunkPyramid& ChunkPyramid::getGenerationPyramid() {
    return s_generationPyramid;
}

const ChunkPyramid& ChunkPyramid::getLoadingPyramid() {
    return s_loadingPyramid;
}

bool ChunkPyramid::isInitialized() {
    return s_initialized;
}

void ChunkPyramid::initialize() {
    if (s_initialized) {
        return;
    }

    // Initialize static futures used by chunk holders
    // This must happen before any chunk processing starts
    server::level::GenerationChunkHolder::initializeStatics();
    server::level::ChunkHolder::initializeStatics();

    // Reference: ChunkPyramid.java lines 17-18
    // GENERATION_PYRAMID
    s_generationPyramid = Builder()
        // EMPTY
        .step(ChunkStatus::EMPTY, [](ChunkStepBuilder& s) -> ChunkStepBuilder& {
            return s;
        })
        // STRUCTURE_STARTS
        .step(ChunkStatus::STRUCTURE_STARTS, [](ChunkStepBuilder& s) -> ChunkStepBuilder& {
            return s.setTask(ChunkStatusTasks::generateStructureStarts);
        })
        // STRUCTURE_REFERENCES - needs STRUCTURE_STARTS at radius 8
        .step(ChunkStatus::STRUCTURE_REFERENCES, [](ChunkStepBuilder& s) -> ChunkStepBuilder& {
            return s.addRequirement(ChunkStatus::STRUCTURE_STARTS, 8)
                    .setTask(ChunkStatusTasks::generateStructureReferences);
        })
        // BIOMES - needs STRUCTURE_STARTS at radius 8
        .step(ChunkStatus::BIOMES, [](ChunkStepBuilder& s) -> ChunkStepBuilder& {
            return s.addRequirement(ChunkStatus::STRUCTURE_STARTS, 8)
                    .setTask(ChunkStatusTasks::generateBiomes);
        })
        // NOISE - needs STRUCTURE_STARTS at radius 8, BIOMES at radius 1
        .step(ChunkStatus::NOISE, [](ChunkStepBuilder& s) -> ChunkStepBuilder& {
            return s.addRequirement(ChunkStatus::STRUCTURE_STARTS, 8)
                    .addRequirement(ChunkStatus::BIOMES, 1)
                    .setBlockStateWriteRadius(0)
                    .setTask(ChunkStatusTasks::generateNoise);
        })
        // SURFACE - needs STRUCTURE_STARTS at radius 8, BIOMES at radius 1
        .step(ChunkStatus::SURFACE, [](ChunkStepBuilder& s) -> ChunkStepBuilder& {
            return s.addRequirement(ChunkStatus::STRUCTURE_STARTS, 8)
                    .addRequirement(ChunkStatus::BIOMES, 1)
                    .setBlockStateWriteRadius(0)
                    .setTask(ChunkStatusTasks::generateSurface);
        })
        // CARVERS - needs STRUCTURE_STARTS at radius 8
        .step(ChunkStatus::CARVERS, [](ChunkStepBuilder& s) -> ChunkStepBuilder& {
            return s.addRequirement(ChunkStatus::STRUCTURE_STARTS, 8)
                    .setBlockStateWriteRadius(0)
                    .setTask(ChunkStatusTasks::generateCarvers);
        })
        // FEATURES - needs STRUCTURE_STARTS at radius 8, CARVERS at radius 1
        .step(ChunkStatus::FEATURES, [](ChunkStepBuilder& s) -> ChunkStepBuilder& {
            return s.addRequirement(ChunkStatus::STRUCTURE_STARTS, 8)
                    .addRequirement(ChunkStatus::CARVERS, 1)
                    .setBlockStateWriteRadius(1)
                    .setTask(ChunkStatusTasks::generateFeatures);
        })
        // INITIALIZE_LIGHT
        .step(ChunkStatus::INITIALIZE_LIGHT, [](ChunkStepBuilder& s) -> ChunkStepBuilder& {
            return s.setTask(ChunkStatusTasks::initializeLight);
        })
        // LIGHT - needs INITIALIZE_LIGHT at radius 1
        .step(ChunkStatus::LIGHT, [](ChunkStepBuilder& s) -> ChunkStepBuilder& {
            return s.addRequirement(ChunkStatus::INITIALIZE_LIGHT, 1)
                    .setTask(ChunkStatusTasks::light);
        })
        // SPAWN - needs BIOMES at radius 1
        .step(ChunkStatus::SPAWN, [](ChunkStepBuilder& s) -> ChunkStepBuilder& {
            return s.addRequirement(ChunkStatus::BIOMES, 1)
                    .setTask(ChunkStatusTasks::generateSpawn);
        })
        // FULL
        .step(ChunkStatus::FULL, [](ChunkStepBuilder& s) -> ChunkStepBuilder& {
            return s.setTask(ChunkStatusTasks::full);
        })
        .build();

    // LOADING_PYRAMID - mostly passthroughs for pre-generated chunks
    s_loadingPyramid = Builder()
        .step(ChunkStatus::EMPTY, [](ChunkStepBuilder& s) -> ChunkStepBuilder& {
            return s;
        })
        .step(ChunkStatus::STRUCTURE_STARTS, [](ChunkStepBuilder& s) -> ChunkStepBuilder& {
            return s.setTask(ChunkStatusTasks::loadStructureStarts);
        })
        .step(ChunkStatus::STRUCTURE_REFERENCES, [](ChunkStepBuilder& s) -> ChunkStepBuilder& {
            return s;
        })
        .step(ChunkStatus::BIOMES, [](ChunkStepBuilder& s) -> ChunkStepBuilder& {
            return s;
        })
        .step(ChunkStatus::NOISE, [](ChunkStepBuilder& s) -> ChunkStepBuilder& {
            return s;
        })
        .step(ChunkStatus::SURFACE, [](ChunkStepBuilder& s) -> ChunkStepBuilder& {
            return s;
        })
        .step(ChunkStatus::CARVERS, [](ChunkStepBuilder& s) -> ChunkStepBuilder& {
            return s;
        })
        .step(ChunkStatus::FEATURES, [](ChunkStepBuilder& s) -> ChunkStepBuilder& {
            return s;
        })
        .step(ChunkStatus::INITIALIZE_LIGHT, [](ChunkStepBuilder& s) -> ChunkStepBuilder& {
            return s.setTask(ChunkStatusTasks::initializeLight);
        })
        .step(ChunkStatus::LIGHT, [](ChunkStepBuilder& s) -> ChunkStepBuilder& {
            return s.addRequirement(ChunkStatus::INITIALIZE_LIGHT, 1)
                    .setTask(ChunkStatusTasks::light);
        })
        .step(ChunkStatus::SPAWN, [](ChunkStepBuilder& s) -> ChunkStepBuilder& {
            return s;
        })
        .step(ChunkStatus::FULL, [](ChunkStepBuilder& s) -> ChunkStepBuilder& {
            return s.setTask(ChunkStatusTasks::full);
        })
        .build();

    s_initialized = true;

    // Note: ChunkLevel::initialize() is NOT called here anymore.
    // ChunkLevel will call ChunkPyramid::initialize() itself if needed,
    // and this avoids a circular dependency.
}

const ChunkStep& ChunkPyramid::getStepTo(const ChunkStatus& status) const {
    // Reference: ChunkPyramid.java lines 12-14
    return m_steps[status.getIndex()];
}

// Builder implementation

ChunkPyramid::Builder& ChunkPyramid::Builder::step(
    const ChunkStatus& status,
    std::function<ChunkStepBuilder&(ChunkStepBuilder&)> configurator
) {
    // Reference: ChunkPyramid.java lines 28-38
    ChunkStepBuilder* stepBuilder;
    std::unique_ptr<ChunkStepBuilder> builder;

    if (m_steps.empty()) {
        builder = std::make_unique<ChunkStepBuilder>(status);
    } else {
        builder = std::make_unique<ChunkStepBuilder>(status, m_steps.back());
    }

    m_steps.push_back(configurator(*builder).build());
    return *this;
}

ChunkPyramid ChunkPyramid::Builder::build() {
    // Reference: ChunkPyramid.java lines 24-26
    return ChunkPyramid(std::move(m_steps));
}

} // namespace status
} // namespace chunk
} // namespace world
} // namespace minecraft
