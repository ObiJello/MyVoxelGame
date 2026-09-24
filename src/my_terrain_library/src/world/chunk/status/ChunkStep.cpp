#include "world/chunk/status/ChunkStep.h"
#include "world/chunk/status/ChunkStatusTasks.h"
#include "world/ProtoChunk.h"

// Reference: net/minecraft/world/level/chunk/status/ChunkStep.java

namespace minecraft {
namespace world {
namespace chunk {
namespace status {

// Apply this step to a chunk
// Reference: ChunkStep.java lines 29-60 (26.3)
std::shared_ptr<util::CompletableFuture<::world::IChunk*>> ChunkStep::apply(
    WorldGenContext& context,
    const std::vector<std::vector<::world::IChunk*>>& cache,
    ::world::IChunk* chunk
) const {
    // A step with no task is MC's default, passThrough (ChunkStep.java:101).
    // Calling the empty std::function threw bad_function_call instead, which
    // applyStep turned into a failed step on every loading-pyramid passthrough.
    auto runTask = [&]() {
        return m_task ? m_task(context, *this, cache, chunk)
                      : ChunkStatusTasks::passThrough(context, *this, cache, chunk);
    };

    // Java: only a chunk still BELOW the target is generated to it and has its
    // status raised; one already at or past it (a chunk loaded from disk going
    // through the loading pyramid) just has the task run.
    const ChunkStatus* persisted = chunk ? chunk->getPersistedStatus() : nullptr;
    if (persisted == nullptr || persisted->isBefore(*m_targetStatus)) {
        const ChunkStatus* targetStatus = m_targetStatus;
        return runTask()->thenApply([targetStatus](::world::IChunk* result) -> ::world::IChunk* {
            // Reference: ChunkStep.java lines 62-67 (completeChunkGeneration):
            // the status only ever RISES. Setting it unconditionally lowered
            // a loaded chunk's saved status to the step being applied.
            if (auto* protoChunk = dynamic_cast<::world::ProtoChunk*>(result)) {
                const ChunkStatus* current = protoChunk->getPersistedStatus();
                if (current == nullptr || current->isBefore(*targetStatus)) {
                    protoChunk->setStatus(targetStatus);
                }
            }
            return result;
        });
    }
    return runTask();
}

} // namespace status
} // namespace chunk
} // namespace world
} // namespace minecraft
