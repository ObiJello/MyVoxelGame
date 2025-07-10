// File: src/render/mesh/ChunkMeshData.hpp
#pragma once

#include "SectionMesh.hpp"
#include "../../core/Log.hpp"
#include <glad/glad.h>
#include <memory>

namespace Render {

    // OpenGL buffer management utilities for section meshes
    class ChunkMeshData {
    public:
        // Create GPU buffers for a fresh SectionMesh
        static bool CreateBuffers(const SectionMesh& mesh, GPUSectionData& gpuData);

        // Update existing GPU buffers with new mesh data
        static bool UpdateBuffers(const SectionMesh& mesh, GPUSectionData& gpuData);

        // Upload specific layer to GPU
        static bool UploadLayer(const std::vector<Vertex>& vertices,
                               const std::vector<uint32_t>& indices,
                               GLuint& vao, GLuint& vbo, GLuint& ibo,
                               uint32_t& indexCount);

        // Delete GPU buffers and clean up
        static void DeleteBuffers(GPUSectionData& gpuData);

        // Validate GPU data integrity
        static bool ValidateBuffers(const GPUSectionData& gpuData);

        // Get memory usage statistics
        static size_t CalculateMemoryUsage(const GPUSectionData& gpuData);
        static size_t CalculateMemoryUsage(const SectionMesh& mesh);

        // Batch operations for efficiency
        static void DeleteMultipleBuffers(std::vector<GPUSectionData>& gpuDataList);
        static bool CreateMultipleBuffers(const std::vector<SectionMesh>& meshes,
                                         std::vector<GPUSectionData>& gpuDataList);

        // Debug and validation
        static bool CheckBufferIntegrity(const GPUSectionData& gpuData);
        static void LogBufferStats(const GPUSectionData& gpuData, const std::string& label = "");

    private:
        // Internal buffer creation helpers
        static bool CreateVAO(GLuint& vao);
        static bool CreateVertexBuffer(const std::vector<Vertex>& vertices, GLuint& vbo);
        static bool CreateIndexBuffer(const std::vector<uint32_t>& indices, GLuint& ibo);
        static void SetupVertexAttributes();

        // Error handling
        static bool CheckGLError(const std::string& operation);
        static void LogGLError(const std::string& operation, GLenum error);

        // Buffer validation
        static bool IsValidBuffer(GLuint buffer, GLenum type);
        static size_t GetBufferSize(GLuint buffer, GLenum type);
    };

    // RAII wrapper for GPU section data
    class ManagedGPUSectionData {
    public:
        explicit ManagedGPUSectionData(Game::Math::ChunkPos pos, int sectionY);
        ~ManagedGPUSectionData();

        // Non-copyable but movable
        ManagedGPUSectionData(const ManagedGPUSectionData&) = delete;
        ManagedGPUSectionData& operator=(const ManagedGPUSectionData&) = delete;
        ManagedGPUSectionData(ManagedGPUSectionData&& other) noexcept;
        ManagedGPUSectionData& operator=(ManagedGPUSectionData&& other) noexcept;

        // Upload mesh data
        bool Upload(const SectionMesh& mesh);

        // Get GPU data reference
        const GPUSectionData& GetGPUData() const { return m_gpuData; }
        GPUSectionData& GetGPUData() { return m_gpuData; }

        // Check if uploaded
        bool IsUploaded() const { return m_gpuData.IsUploaded(); }

        // Force cleanup
        void Release();

    private:
        GPUSectionData m_gpuData;
        bool m_released = false;

        void MoveFrom(ManagedGPUSectionData&& other) noexcept;
    };

    // Batch GPU data manager for multiple sections
    class GPUDataManager {
    public:
        GPUDataManager() = default;
        ~GPUDataManager();

        // Non-copyable
        GPUDataManager(const GPUDataManager&) = delete;
        GPUDataManager& operator=(const GPUDataManager&) = delete;

        // Section management
        bool AddSection(Game::Math::ChunkPos chunkPos, int sectionY, const SectionMesh& mesh);
        bool UpdateSection(Game::Math::ChunkPos chunkPos, int sectionY, const SectionMesh& mesh);
        void RemoveSection(Game::Math::ChunkPos chunkPos, int sectionY);
        void RemoveChunk(Game::Math::ChunkPos chunkPos);

        // Access GPU data
        const GPUSectionData* GetSectionData(Game::Math::ChunkPos chunkPos, int sectionY) const;
        GPUSectionData* GetSectionData(Game::Math::ChunkPos chunkPos, int sectionY);

        // Statistics
        size_t GetSectionCount() const { return m_sections.size(); }
        size_t GetTotalMemoryUsage() const;

        // Cleanup
        void Clear();

        // LRU management
        void SetMaxSections(size_t maxSections) { m_maxSections = maxSections; }
        void EnforceLRU();

    private:
        // Section key for mapping
        struct SectionKey {
            Game::Math::ChunkPos chunkPos;
            int sectionY;

            bool operator==(const SectionKey& other) const {
                return chunkPos.x == other.chunkPos.x &&
                       chunkPos.z == other.chunkPos.z &&
                       sectionY == other.sectionY;
            }
        };

        // Hash function for SectionKey
        struct SectionKeyHash {
            std::size_t operator()(const SectionKey& key) const {
                return std::hash<int32_t>{}(key.chunkPos.x) ^
                       (std::hash<int32_t>{}(key.chunkPos.z) << 1) ^
                       (std::hash<int>{}(key.sectionY) << 2);
            }
        };

        std::unordered_map<SectionKey, std::unique_ptr<ManagedGPUSectionData>, SectionKeyHash> m_sections;
        size_t m_maxSections = 10000; // Default limit

        // Helper methods
        SectionKey MakeKey(Game::Math::ChunkPos chunkPos, int sectionY) const;
        void EvictLRUSections();
    };

} // namespace Render