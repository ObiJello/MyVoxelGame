// File: src/client/renderer/backend/RenderTypes.hpp
#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <string>

namespace Render {

    // ========================================================================
    // OPAQUE HANDLES FOR GPU RESOURCES
    // ========================================================================

    using BufferHandle  = uint32_t;
    using TextureHandle = uint32_t;
    using ShaderHandle  = uint32_t;
    using MeshHandle    = uint32_t;

    static constexpr BufferHandle  INVALID_BUFFER  = 0;
    static constexpr TextureHandle INVALID_TEXTURE = 0;
    static constexpr ShaderHandle  INVALID_SHADER  = 0;
    static constexpr MeshHandle    INVALID_MESH    = 0;

    // ========================================================================
    // BACKEND TYPE
    // ========================================================================

    enum class BackendType { OpenGL, Vulkan };

    // ========================================================================
    // BUFFER TYPES
    // ========================================================================

    enum class BufferUsage {
        Vertex,
        Index,
        Uniform,
        Staging
    };

    enum class BufferAccess {
        Static,     // Uploaded once, never updated (GL_STATIC_DRAW / DEVICE_LOCAL)
        Dynamic,    // Updated occasionally (GL_DYNAMIC_DRAW)
        Streaming   // Updated every frame (GL_STREAM_DRAW / HOST_VISIBLE)
    };

    // ========================================================================
    // TEXTURE TYPES
    // ========================================================================

    enum class TextureFormat {
        RGBA8,
        SRGB8_A8,
        Depth24Stencil8
    };

    enum class TextureFilter {
        Nearest,
        Linear,
        NearestMipmapLinear,
        NearestMipmapNearest,
        LinearMipmapLinear,
        LinearMipmapNearest
    };

    enum class TextureWrap {
        Repeat,
        ClampToEdge,
        MirroredRepeat
    };

    // ========================================================================
    // SHADER TYPES
    // ========================================================================

    enum class ShaderStage {
        Vertex,
        Fragment
    };

    // ========================================================================
    // PIPELINE STATE
    // ========================================================================

    enum class BlendFactor {
        Zero,
        One,
        SrcAlpha,
        OneMinusSrcAlpha,
        DstAlpha,
        OneMinusDstAlpha
    };

    enum class CompareOp {
        Never,
        Less,
        Equal,
        LessEqual,
        Greater,
        NotEqual,
        GreaterEqual,
        Always
    };

    enum class CullMode {
        None,
        Front,
        Back
    };

    enum class FrontFace {
        CounterClockwise,
        Clockwise
    };

    enum class PolygonMode {
        Fill,
        Line
    };

    enum class PrimitiveType {
        Triangles,
        Lines,
        TriangleStrip,
        LineStrip
    };

    // Describes how one vertex attribute is laid out
    struct VertexAttribute {
        uint32_t location;        // Shader attribute location
        uint32_t componentCount;  // 1-4 (float, vec2, vec3, vec4)
        uint32_t offset;          // Byte offset within vertex
        bool normalized = false;  // Whether to normalize integer data
    };

    // Describes the full vertex layout (stride + attributes)
    struct VertexLayout {
        uint32_t stride = 0;
        std::vector<VertexAttribute> attributes;
    };

    // Complete pipeline state description (replaces scattered GL state calls)
    struct PipelineState {
        // Depth
        bool depthTestEnabled  = true;
        bool depthWriteEnabled = true;
        CompareOp depthCompareOp = CompareOp::LessEqual;

        // Blending
        bool blendEnabled = false;
        BlendFactor srcBlendFactor = BlendFactor::SrcAlpha;
        BlendFactor dstBlendFactor = BlendFactor::OneMinusSrcAlpha;

        // Rasterizer
        CullMode cullMode     = CullMode::Back;
        FrontFace frontFace   = FrontFace::CounterClockwise;
        PolygonMode polygonMode = PolygonMode::Fill;
        float lineWidth       = 1.0f;

        // Depth bias (polygon offset) to prevent z-fighting
        bool depthBiasEnabled = false;
        float depthBiasConstant = 0.0f;
        float depthBiasSlope = 0.0f;

        // Topology
        PrimitiveType primitiveType = PrimitiveType::Triangles;
    };

    // ========================================================================
    // GPU MEMORY TRACKING
    // ========================================================================

    struct GPUMemoryStats {
        size_t totalAllocated  = 0;  // Total bytes allocated on GPU
        size_t bufferMemory    = 0;  // Bytes used by vertex/index buffers
        size_t textureMemory   = 0;  // Bytes used by textures
        size_t peakUsage       = 0;  // Peak total allocated
        size_t bufferCount     = 0;  // Number of active buffers
        size_t textureCount    = 0;  // Number of active textures
        size_t meshCount       = 0;  // Number of active mesh objects
        size_t shaderCount     = 0;  // Number of active shaders
    };

    // ========================================================================
    // GPU TIMER QUERY
    // ========================================================================

    using GPUTimerHandle = uint32_t;
    static constexpr GPUTimerHandle INVALID_GPU_TIMER = 0;

    // ========================================================================
    // STANDARD VERTEX LAYOUT (12 floats per vertex)
    // ========================================================================

    // The block vertex layout used by the chunk mesh system:
    //   Position (3 floats) + Normal (3 floats) + UV (2 floats) + Color (4 floats)
    inline VertexLayout GetBlockVertexLayout() {
        VertexLayout layout;
        layout.stride = sizeof(float) * 12;
        layout.attributes = {
            {0, 3, 0,                    false},  // Position
            {1, 3, sizeof(float) * 3,    false},  // Normal
            {2, 2, sizeof(float) * 6,    false},  // UV
            {3, 4, sizeof(float) * 8,    false},  // Color/Tint
        };
        return layout;
    }

} // namespace Render
