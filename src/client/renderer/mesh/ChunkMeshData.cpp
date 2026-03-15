// File: src/client/renderer/mesh/ChunkMeshData.cpp
// NOTE: This is legacy code, no longer included by any active source file.
// It is kept for reference but all active GPU uploads go through ClientMeshManager
// which uses the RenderBackend abstraction.
#include "ChunkMeshData.hpp"
#include "../backend/RenderBackend.hpp"
#include "common/core/Log.hpp"
#include <algorithm>

namespace Render {

    bool ChunkMeshData::CreateBuffers(const SectionMesh& mesh, GPUSectionData& gpuData) {
        DeleteBuffers(gpuData);
        gpuData.chunkPos = mesh.chunkPos;
        gpuData.sectionY = mesh.sectionY;

        // Legacy: GPU resources are now managed by ChunkMegaBuffer.
        // Just set the counts for compatibility.
        gpuData.opaqueIndexCount = static_cast<uint32_t>(mesh.opaqueIdxs.size());
        gpuData.opaqueVertexCount = static_cast<uint32_t>(mesh.opaqueVerts.size());
        gpuData.cutoutIndexCount = static_cast<uint32_t>(mesh.cutoutIdxs.size());
        gpuData.cutoutVertexCount = static_cast<uint32_t>(mesh.cutoutVerts.size());
        gpuData.translucentIndexCount = static_cast<uint32_t>(mesh.translucentIdxs.size());
        gpuData.translucentVertexCount = static_cast<uint32_t>(mesh.translucentVerts.size());

        return true;
    }

    bool ChunkMeshData::UpdateBuffers(const SectionMesh& mesh, GPUSectionData& gpuData) {
        return CreateBuffers(mesh, gpuData);
    }

    bool ChunkMeshData::UploadLayer(const std::vector<Vertex>& vertices,
                                   const std::vector<uint32_t>& indices,
                                   GLuint& vao, GLuint& vbo, GLuint& ibo,
                                   uint32_t& indexCount) {
        // Legacy GL path — kept for API compatibility but no longer called
        return false;
    }

    void ChunkMeshData::DeleteBuffers(GPUSectionData& gpuData) {
        gpuData.DestroyAllResources(g_renderBackend.get());
    }

    bool ChunkMeshData::ValidateBuffers(const GPUSectionData& gpuData) {
        return gpuData.HasGeometry();
    }

    size_t ChunkMeshData::CalculateMemoryUsage(const GPUSectionData& gpuData) {
        return (gpuData.opaqueIndexCount + gpuData.cutoutIndexCount + gpuData.translucentIndexCount) *
               (sizeof(Vertex) + sizeof(uint32_t));
    }

    size_t ChunkMeshData::CalculateMemoryUsage(const SectionMesh& mesh) {
        size_t total = 0;
        total += mesh.opaqueVerts.size() * sizeof(Vertex) + mesh.opaqueIdxs.size() * sizeof(uint32_t);
        total += mesh.cutoutVerts.size() * sizeof(Vertex) + mesh.cutoutIdxs.size() * sizeof(uint32_t);
        total += mesh.translucentVerts.size() * sizeof(Vertex) + mesh.translucentIdxs.size() * sizeof(uint32_t);
        return total;
    }

    void ChunkMeshData::DeleteMultipleBuffers(std::vector<GPUSectionData>& gpuDataList) {
        for (auto& gpuData : gpuDataList) {
            DeleteBuffers(gpuData);
        }
    }

    bool ChunkMeshData::CreateMultipleBuffers(const std::vector<SectionMesh>& meshes,
                                             std::vector<GPUSectionData>& gpuDataList) {
        gpuDataList.clear();
        gpuDataList.reserve(meshes.size());
        bool allSucceeded = true;
        for (const auto& mesh : meshes) {
            if (!mesh.IsEmpty()) {
                GPUSectionData gpuData(mesh.chunkPos, mesh.sectionY);
                if (CreateBuffers(mesh, gpuData)) {
                    gpuDataList.push_back(std::move(gpuData));
                } else {
                    allSucceeded = false;
                }
            }
        }
        return allSucceeded;
    }

    bool ChunkMeshData::CheckBufferIntegrity(const GPUSectionData& gpuData) {
        return ValidateBuffers(gpuData);
    }

    void ChunkMeshData::LogBufferStats(const GPUSectionData& gpuData, const std::string& label) {
        std::string prefix = label.empty() ? "GPU Buffer Stats" : label;
        Log::Debug("%s for section (%d, %d, %d): opaque=%u, cutout=%u, translucent=%u, mem=%zu bytes",
                  prefix.c_str(), gpuData.chunkPos.x, gpuData.sectionY, gpuData.chunkPos.z,
                  gpuData.opaqueIndexCount, gpuData.cutoutIndexCount, gpuData.translucentIndexCount,
                  CalculateMemoryUsage(gpuData));
    }

    // Private helpers — legacy GL functions, no longer used
    bool ChunkMeshData::CreateVAO(GLuint& vao) { return false; }
    bool ChunkMeshData::CreateVertexBuffer(const std::vector<Vertex>& vertices, GLuint& vbo) { return false; }
    bool ChunkMeshData::CreateIndexBuffer(const std::vector<uint32_t>& indices, GLuint& ibo) { return false; }
    void ChunkMeshData::SetupVertexAttributes() {}
    bool ChunkMeshData::CheckGLError(const std::string& operation) { return true; }
    void ChunkMeshData::LogGLError(const std::string& operation, GLenum error) {}
    bool ChunkMeshData::IsValidBuffer(GLuint buffer, GLenum type) { return false; }
    size_t ChunkMeshData::GetBufferSize(GLuint buffer, GLenum type) { return 0; }

    // ManagedGPUSectionData implementation
    ManagedGPUSectionData::ManagedGPUSectionData(Game::Math::ChunkPos pos, int sectionY)
        : m_gpuData(pos, sectionY) {}

    ManagedGPUSectionData::~ManagedGPUSectionData() {
        if (!m_released) Release();
    }

    ManagedGPUSectionData::ManagedGPUSectionData(ManagedGPUSectionData&& other) noexcept {
        MoveFrom(std::move(other));
    }

    ManagedGPUSectionData& ManagedGPUSectionData::operator=(ManagedGPUSectionData&& other) noexcept {
        if (this != &other) {
            if (!m_released) Release();
            MoveFrom(std::move(other));
        }
        return *this;
    }

    bool ManagedGPUSectionData::Upload(const SectionMesh& mesh) {
        return ChunkMeshData::CreateBuffers(mesh, m_gpuData);
    }

    void ManagedGPUSectionData::Release() {
        if (!m_released) {
            ChunkMeshData::DeleteBuffers(m_gpuData);
            m_released = true;
        }
    }

    void ManagedGPUSectionData::MoveFrom(ManagedGPUSectionData&& other) noexcept {
        m_gpuData = std::move(other.m_gpuData);
        m_released = other.m_released;
        other.m_released = true;
    }

    // GPUDataManager implementation
    GPUDataManager::~GPUDataManager() { Clear(); }

    bool GPUDataManager::AddSection(Game::Math::ChunkPos chunkPos, int sectionY, const SectionMesh& mesh) {
        if (mesh.IsEmpty()) return true;
        SectionKey key = MakeKey(chunkPos, sectionY);
        auto managed = std::make_unique<ManagedGPUSectionData>(chunkPos, sectionY);
        if (!managed->Upload(mesh)) return false;
        m_sections[key] = std::move(managed);
        if (m_sections.size() > m_maxSections) EnforceLRU();
        return true;
    }

    bool GPUDataManager::UpdateSection(Game::Math::ChunkPos chunkPos, int sectionY, const SectionMesh& mesh) {
        SectionKey key = MakeKey(chunkPos, sectionY);
        auto it = m_sections.find(key);
        if (it != m_sections.end()) return it->second->Upload(mesh);
        return AddSection(chunkPos, sectionY, mesh);
    }

    void GPUDataManager::RemoveSection(Game::Math::ChunkPos chunkPos, int sectionY) {
        m_sections.erase(MakeKey(chunkPos, sectionY));
    }

    void GPUDataManager::RemoveChunk(Game::Math::ChunkPos chunkPos) {
        for (auto it = m_sections.begin(); it != m_sections.end(); ) {
            if (it->first.chunkPos.x == chunkPos.x && it->first.chunkPos.z == chunkPos.z)
                it = m_sections.erase(it);
            else
                ++it;
        }
    }

    const GPUSectionData* GPUDataManager::GetSectionData(Game::Math::ChunkPos chunkPos, int sectionY) const {
        auto it = m_sections.find(MakeKey(chunkPos, sectionY));
        return (it != m_sections.end()) ? &it->second->GetGPUData() : nullptr;
    }

    GPUSectionData* GPUDataManager::GetSectionData(Game::Math::ChunkPos chunkPos, int sectionY) {
        auto it = m_sections.find(MakeKey(chunkPos, sectionY));
        return (it != m_sections.end()) ? &it->second->GetGPUData() : nullptr;
    }

    size_t GPUDataManager::GetTotalMemoryUsage() const {
        size_t total = 0;
        for (const auto& [key, managed] : m_sections)
            total += ChunkMeshData::CalculateMemoryUsage(managed->GetGPUData());
        return total;
    }

    void GPUDataManager::Clear() { m_sections.clear(); }

    void GPUDataManager::EnforceLRU() {
        while (m_sections.size() > m_maxSections) m_sections.erase(m_sections.begin());
    }

    GPUDataManager::SectionKey GPUDataManager::MakeKey(Game::Math::ChunkPos chunkPos, int sectionY) const {
        return SectionKey{chunkPos, sectionY};
    }

    void GPUDataManager::EvictLRUSections() { EnforceLRU(); }

} // namespace Render
