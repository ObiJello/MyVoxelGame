// File: src/render/mesh/ChunkMeshData.cpp
#include "ChunkMeshData.hpp"
#include "../../core/Log.hpp"
#include <algorithm>

namespace Render {

    bool ChunkMeshData::CreateBuffers(const SectionMesh& mesh, GPUSectionData& gpuData) {
        // Clear any existing data
        DeleteBuffers(gpuData);

        // Set section identification
        gpuData.chunkPos = mesh.chunkPos;
        gpuData.sectionY = mesh.sectionY;

        bool success = true;

        // Upload opaque layer
        if (!mesh.opaqueVerts.empty() && !mesh.opaqueIdxs.empty()) {
            if (!UploadLayer(mesh.opaqueVerts, mesh.opaqueIdxs,
                            gpuData.opaqueVAO, gpuData.opaqueVBO, gpuData.opaqueIBO,
                            gpuData.opaqueIndexCount)) {
                Log::Error("Failed to upload opaque layer for section (%d, %d, %d)",
                          mesh.chunkPos.x, mesh.sectionY, mesh.chunkPos.z);
                success = false;
            }
        }

        // Upload cutout layer
        if (!mesh.cutoutVerts.empty() && !mesh.cutoutIdxs.empty()) {
            if (!UploadLayer(mesh.cutoutVerts, mesh.cutoutIdxs,
                            gpuData.cutoutVAO, gpuData.cutoutVBO, gpuData.cutoutIBO,
                            gpuData.cutoutIndexCount)) {
                Log::Error("Failed to upload cutout layer for section (%d, %d, %d)",
                          mesh.chunkPos.x, mesh.sectionY, mesh.chunkPos.z);
                success = false;
            }
        }

        // Upload translucent layer
        if (!mesh.translucentVerts.empty() && !mesh.translucentIdxs.empty()) {
            if (!UploadLayer(mesh.translucentVerts, mesh.translucentIdxs,
                            gpuData.translucentVAO, gpuData.translucentVBO, gpuData.translucentIBO,
                            gpuData.translucentIndexCount)) {
                Log::Error("Failed to upload translucent layer for section (%d, %d, %d)",
                          mesh.chunkPos.x, mesh.sectionY, mesh.chunkPos.z);
                success = false;
            }
        }

        if (!success) {
             // Clean up on failure
            DeleteBuffers(gpuData);
        }

        return success;
    }

    bool ChunkMeshData::UpdateBuffers(const SectionMesh& mesh, GPUSectionData& gpuData) {
        // For now, just recreate buffers
        // In a more optimized implementation, we could reuse buffers if they're large enough
        return CreateBuffers(mesh, gpuData);
    }

    bool ChunkMeshData::UploadLayer(const std::vector<Vertex>& vertices,
                                   const std::vector<uint32_t>& indices,
                                   GLuint& vao, GLuint& vbo, GLuint& ibo,
                                   uint32_t& indexCount) {

        if (vertices.empty() || indices.empty()) {
            return true; // Nothing to upload
        }

        // Create VAO
        if (!CreateVAO(vao)) {
            return false;
        }

        glBindVertexArray(vao);

        // Create and upload vertex buffer
        if (!CreateVertexBuffer(vertices, vbo)) {
            glDeleteVertexArrays(1, &vao);
            vao = 0;
            return false;
        }

        // Create and upload index buffer
        if (!CreateIndexBuffer(indices, ibo)) {
            glDeleteVertexArrays(1, &vao);
            glDeleteBuffers(1, &vbo);
            vao = vbo = 0;
            return false;
        }

        // Set up vertex attributes
        SetupVertexAttributes();

        // Unbind VAO
        glBindVertexArray(0);

        // Store index count
        indexCount = static_cast<uint32_t>(indices.size());

        // Verify upload
        if (!CheckGLError("UploadLayer")) {
            glDeleteVertexArrays(1, &vao);
            glDeleteBuffers(1, &vbo);
            glDeleteBuffers(1, &ibo);
            vao = vbo = ibo = 0;
            indexCount = 0;
            return false;
        }

        return true;
    }

    void ChunkMeshData::DeleteBuffers(GPUSectionData& gpuData) {
        // Delete opaque layer
        if (gpuData.opaqueVAO != 0) {
            glDeleteVertexArrays(1, &gpuData.opaqueVAO);
            gpuData.opaqueVAO = 0;
        }
        if (gpuData.opaqueVBO != 0) {
            glDeleteBuffers(1, &gpuData.opaqueVBO);
            gpuData.opaqueVBO = 0;
        }
        if (gpuData.opaqueIBO != 0) {
            glDeleteBuffers(1, &gpuData.opaqueIBO);
            gpuData.opaqueIBO = 0;
        }

        // Delete cutout layer
        if (gpuData.cutoutVAO != 0) {
            glDeleteVertexArrays(1, &gpuData.cutoutVAO);
            gpuData.cutoutVAO = 0;
        }
        if (gpuData.cutoutVBO != 0) {
            glDeleteBuffers(1, &gpuData.cutoutVBO);
            gpuData.cutoutVBO = 0;
        }
        if (gpuData.cutoutIBO != 0) {
            glDeleteBuffers(1, &gpuData.cutoutIBO);
            gpuData.cutoutIBO = 0;
        }

        // Delete translucent layer
        if (gpuData.translucentVAO != 0) {
            glDeleteVertexArrays(1, &gpuData.translucentVAO);
            gpuData.translucentVAO = 0;
        }
        if (gpuData.translucentVBO != 0) {
            glDeleteBuffers(1, &gpuData.translucentVBO);
            gpuData.translucentVBO = 0;
        }
        if (gpuData.translucentIBO != 0) {
            glDeleteBuffers(1, &gpuData.translucentIBO);
            gpuData.translucentIBO = 0;
        }

        // Reset counts
        gpuData.opaqueIndexCount = 0;
        gpuData.cutoutIndexCount = 0;
        gpuData.translucentIndexCount = 0;
    }

    bool ChunkMeshData::ValidateBuffers(const GPUSectionData& gpuData) {
        // Check if at least one layer has valid data
        bool hasValidData = false;

        if (gpuData.opaqueIndexCount > 0) {
            if (!IsValidBuffer(gpuData.opaqueVAO, GL_VERTEX_ARRAY) ||
                !IsValidBuffer(gpuData.opaqueVBO, GL_ARRAY_BUFFER) ||
                !IsValidBuffer(gpuData.opaqueIBO, GL_ELEMENT_ARRAY_BUFFER)) {
                return false;
            }
            hasValidData = true;
        }

        if (gpuData.cutoutIndexCount > 0) {
            if (!IsValidBuffer(gpuData.cutoutVAO, GL_VERTEX_ARRAY) ||
                !IsValidBuffer(gpuData.cutoutVBO, GL_ARRAY_BUFFER) ||
                !IsValidBuffer(gpuData.cutoutIBO, GL_ELEMENT_ARRAY_BUFFER)) {
                return false;
            }
            hasValidData = true;
        }

        if (gpuData.translucentIndexCount > 0) {
            if (!IsValidBuffer(gpuData.translucentVAO, GL_VERTEX_ARRAY) ||
                !IsValidBuffer(gpuData.translucentVBO, GL_ARRAY_BUFFER) ||
                !IsValidBuffer(gpuData.translucentIBO, GL_ELEMENT_ARRAY_BUFFER)) {
                return false;
            }
            hasValidData = true;
        }

        return hasValidData;
    }

    size_t ChunkMeshData::CalculateMemoryUsage(const GPUSectionData& gpuData) {
        size_t total = 0;

        // Calculate memory for each layer
        total += gpuData.opaqueIndexCount * (sizeof(Vertex) + sizeof(uint32_t));
        total += gpuData.cutoutIndexCount * (sizeof(Vertex) + sizeof(uint32_t));
        total += gpuData.translucentIndexCount * (sizeof(Vertex) + sizeof(uint32_t));

        return total;
    }

    size_t ChunkMeshData::CalculateMemoryUsage(const SectionMesh& mesh) {
        size_t total = 0;

        total += mesh.opaqueVerts.size() * sizeof(Vertex);
        total += mesh.opaqueIdxs.size() * sizeof(uint32_t);
        total += mesh.cutoutVerts.size() * sizeof(Vertex);
        total += mesh.cutoutIdxs.size() * sizeof(uint32_t);
        total += mesh.translucentVerts.size() * sizeof(Vertex);
        total += mesh.translucentIdxs.size() * sizeof(uint32_t);

        return total;
    }

    void ChunkMeshData::DeleteMultipleBuffers(std::vector<GPUSectionData>& gpuDataList) {
        for (auto& gpuData : gpuDataList) {
            DeleteBuffers(gpuData);
        }
        Log::Debug("Deleted %zu GPU buffer sets", gpuDataList.size());
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
                    Log::Warning("Failed to create buffers for section (%d, %d, %d)",
                                mesh.chunkPos.x, mesh.sectionY, mesh.chunkPos.z);
                    allSucceeded = false;
                }
            }
        }

        Log::Debug("Created %zu/%zu GPU buffer sets", gpuDataList.size(), meshes.size());
        return allSucceeded;
    }

    bool ChunkMeshData::CheckBufferIntegrity(const GPUSectionData& gpuData) {
        // Perform comprehensive buffer validation
        if (!ValidateBuffers(gpuData)) {
            return false;
        }

        // Additional integrity checks could go here
        // For example, checking buffer sizes match expected values

        return true;
    }

    void ChunkMeshData::LogBufferStats(const GPUSectionData& gpuData, const std::string& label) {
        std::string prefix = label.empty() ? "GPU Buffer Stats" : label;

        Log::Debug("%s for section (%d, %d, %d):", prefix.c_str(),
                  gpuData.chunkPos.x, gpuData.sectionY, gpuData.chunkPos.z);
        Log::Debug("  Opaque: VAO=%u, indices=%u", gpuData.opaqueVAO, gpuData.opaqueIndexCount);
        Log::Debug("  Cutout: VAO=%u, indices=%u", gpuData.cutoutVAO, gpuData.cutoutIndexCount);
        Log::Debug("  Translucent: VAO=%u, indices=%u", gpuData.translucentVAO, gpuData.translucentIndexCount);
        Log::Debug("  Memory usage: %zu bytes", CalculateMemoryUsage(gpuData));
    }

    // Private helper methods
    bool ChunkMeshData::CreateVAO(GLuint& vao) {
        glGenVertexArrays(1, &vao);
        if (vao == 0) {
            Log::Error("Failed to generate VAO");
            return false;
        }
        return CheckGLError("CreateVAO");
    }

    bool ChunkMeshData::CreateVertexBuffer(const std::vector<Vertex>& vertices, GLuint& vbo) {
        glGenBuffers(1, &vbo);
        if (vbo == 0) {
            Log::Error("Failed to generate VBO");
            return false;
        }

        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER,
                    vertices.size() * sizeof(Vertex),
                    vertices.data(),
                    GL_STATIC_DRAW);

        return CheckGLError("CreateVertexBuffer");
    }

    bool ChunkMeshData::CreateIndexBuffer(const std::vector<uint32_t>& indices, GLuint& ibo) {
        glGenBuffers(1, &ibo);
        if (ibo == 0) {
            Log::Error("Failed to generate IBO");
            return false;
        }

        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                    indices.size() * sizeof(uint32_t),
                    indices.data(),
                    GL_STATIC_DRAW);

        return CheckGLError("CreateIndexBuffer");
    }

    void ChunkMeshData::SetupVertexAttributes() {
        // Position attribute (location 0)
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                             reinterpret_cast<void*>(offsetof(Vertex, pos)));

        // Normal attribute (location 1)
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                             reinterpret_cast<void*>(offsetof(Vertex, nrm)));

        // Texture coordinate attribute (location 2)
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                             reinterpret_cast<void*>(offsetof(Vertex, uv)));

        // Color attribute (location 3)
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                             reinterpret_cast<void*>(offsetof(Vertex, color)));
    }

    bool ChunkMeshData::CheckGLError(const std::string& operation) {
        GLenum error = glGetError();
        if (error != GL_NO_ERROR) {
            LogGLError(operation, error);
            return false;
        }
        return true;
    }

    void ChunkMeshData::LogGLError(const std::string& operation, GLenum error) {
        const char* errorString = "Unknown";
        switch (error) {
            case GL_INVALID_ENUM: errorString = "GL_INVALID_ENUM"; break;
            case GL_INVALID_VALUE: errorString = "GL_INVALID_VALUE"; break;
            case GL_INVALID_OPERATION: errorString = "GL_INVALID_OPERATION"; break;
            case GL_OUT_OF_MEMORY: errorString = "GL_OUT_OF_MEMORY"; break;
            case GL_INVALID_FRAMEBUFFER_OPERATION: errorString = "GL_INVALID_FRAMEBUFFER_OPERATION"; break;
        }
        Log::Error("OpenGL error in %s: %s (0x%x)", operation.c_str(), errorString, error);
    }

    bool ChunkMeshData::IsValidBuffer(GLuint buffer, GLenum type) {
        if (buffer == 0) return false;

        switch (type) {
            case GL_VERTEX_ARRAY:
                return glIsVertexArray(buffer) == GL_TRUE;
            case GL_ARRAY_BUFFER:
            case GL_ELEMENT_ARRAY_BUFFER:
                return glIsBuffer(buffer) == GL_TRUE;
            default:
                return false;
        }
    }

    size_t ChunkMeshData::GetBufferSize(GLuint buffer, GLenum type) {
        if (!IsValidBuffer(buffer, type)) return 0;

        GLint size = 0;
        switch (type) {
            case GL_ARRAY_BUFFER:
                glBindBuffer(GL_ARRAY_BUFFER, buffer);
                glGetBufferParameteriv(GL_ARRAY_BUFFER, GL_BUFFER_SIZE, &size);
                break;
            case GL_ELEMENT_ARRAY_BUFFER:
                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffer);
                glGetBufferParameteriv(GL_ELEMENT_ARRAY_BUFFER, GL_BUFFER_SIZE, &size);
                break;
        }
        return static_cast<size_t>(size);
    }

    // ManagedGPUSectionData implementation
    ManagedGPUSectionData::ManagedGPUSectionData(Game::Math::ChunkPos pos, int sectionY)
        : m_gpuData(pos, sectionY) {
    }

    ManagedGPUSectionData::~ManagedGPUSectionData() {
        if (!m_released) {
            Release();
        }
    }

    ManagedGPUSectionData::ManagedGPUSectionData(ManagedGPUSectionData&& other) noexcept {
        MoveFrom(std::move(other));
    }

    ManagedGPUSectionData& ManagedGPUSectionData::operator=(ManagedGPUSectionData&& other) noexcept {
        if (this != &other) {
            if (!m_released) {
                Release();
            }
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
        other.m_released = true; // Prevent cleanup in moved-from object
    }

    // GPUDataManager implementation
    GPUDataManager::~GPUDataManager() {
        Clear();
    }

    bool GPUDataManager::AddSection(Game::Math::ChunkPos chunkPos, int sectionY, const SectionMesh& mesh) {
        if (mesh.IsEmpty()) {
            return true; // Nothing to add
        }

        SectionKey key = MakeKey(chunkPos, sectionY);

        auto managed = std::make_unique<ManagedGPUSectionData>(chunkPos, sectionY);
        if (!managed->Upload(mesh)) {
            return false;
        }

        m_sections[key] = std::move(managed);

        // Enforce LRU if needed
        if (m_sections.size() > m_maxSections) {
            EnforceLRU();
        }

        return true;
    }

    bool GPUDataManager::UpdateSection(Game::Math::ChunkPos chunkPos, int sectionY, const SectionMesh& mesh) {
        SectionKey key = MakeKey(chunkPos, sectionY);

        auto it = m_sections.find(key);
        if (it != m_sections.end()) {
            return it->second->Upload(mesh);
        } else {
            return AddSection(chunkPos, sectionY, mesh);
        }
    }

    void GPUDataManager::RemoveSection(Game::Math::ChunkPos chunkPos, int sectionY) {
        SectionKey key = MakeKey(chunkPos, sectionY);
        m_sections.erase(key);
    }

    void GPUDataManager::RemoveChunk(Game::Math::ChunkPos chunkPos) {
        auto it = m_sections.begin();
        while (it != m_sections.end()) {
            if (it->first.chunkPos.x == chunkPos.x && it->first.chunkPos.z == chunkPos.z) {
                it = m_sections.erase(it);
            } else {
                ++it;
            }
        }
    }

    const GPUSectionData* GPUDataManager::GetSectionData(Game::Math::ChunkPos chunkPos, int sectionY) const {
        SectionKey key = MakeKey(chunkPos, sectionY);
        auto it = m_sections.find(key);
        return (it != m_sections.end()) ? &it->second->GetGPUData() : nullptr;
    }

    GPUSectionData* GPUDataManager::GetSectionData(Game::Math::ChunkPos chunkPos, int sectionY) {
        SectionKey key = MakeKey(chunkPos, sectionY);
        auto it = m_sections.find(key);
        return (it != m_sections.end()) ? &it->second->GetGPUData() : nullptr;
    }

    size_t GPUDataManager::GetTotalMemoryUsage() const {
        size_t total = 0;
        for (const auto& [key, managed] : m_sections) {
            total += ChunkMeshData::CalculateMemoryUsage(managed->GetGPUData());
        }
        return total;
    }

    void GPUDataManager::Clear() {
        m_sections.clear();
    }

    void GPUDataManager::EnforceLRU() {
        // Simple LRU: remove oldest sections until under limit
        // In a full implementation, this would track actual access times
        while (m_sections.size() > m_maxSections) {
            auto it = m_sections.begin();
            m_sections.erase(it);
        }
    }

    GPUDataManager::SectionKey GPUDataManager::MakeKey(Game::Math::ChunkPos chunkPos, int sectionY) const {
        return SectionKey{chunkPos, sectionY};
    }

    void GPUDataManager::EvictLRUSections() {
        // Placeholder for more sophisticated LRU implementation
        EnforceLRU();
    }

} // namespace Render