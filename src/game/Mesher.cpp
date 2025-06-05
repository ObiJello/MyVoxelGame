#include "Mesher.hpp"
#include "Log.hpp"
#include <cassert>

namespace Game {

    // Static, internal queue and mutex for mesh uploads
    static std::mutex             s_uploadMutex;
    static std::queue<MeshData*>  s_uploadQueue;

    void MesherJob(ChunkSection* section, MeshData* outData) {
        // Stub implementation: for Phase 2, we simply mark the mesh as empty.
        // In Step 4 we’ll fill outData->vertices and outData->indices with actual geometry.

        assert(section != nullptr);
        assert(outData != nullptr);

        // Example debug log; in a real mesher you’d inspect 'section->blocks' and generate quads.
        Log::Debug("MesherJob started for one ChunkSection at address %p", static_cast<void*>(section));

        // *** Placeholder: no faces emitted yet ***
        outData->vertices.clear();
        outData->indices.clear();

        // Once finished, enqueue this MeshData for the render thread to upload:
        PushMeshData(outData);

        Log::Debug("MesherJob completed; pushed MeshData %p to upload queue", static_cast<void*>(outData));
    }

    void PushMeshData(MeshData* data) {
        std::lock_guard<std::mutex> lock(s_uploadMutex);
        s_uploadQueue.push(data);
    }

    bool PopMeshData(MeshData*& outData) {
        std::lock_guard<std::mutex> lock(s_uploadMutex);
        if (s_uploadQueue.empty()) {
            return false;
        }
        outData = s_uploadQueue.front();
        s_uploadQueue.pop();
        return true;
    }

} // namespace Game
