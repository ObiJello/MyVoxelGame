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

    using BufferHandle       = uint32_t;
    using TextureHandle      = uint32_t;
    using ShaderHandle       = uint32_t;
    using MeshHandle         = uint32_t;
    using RenderTargetHandle = uint32_t;

    static constexpr BufferHandle       INVALID_BUFFER        = 0;
    static constexpr TextureHandle      INVALID_TEXTURE       = 0;
    static constexpr ShaderHandle       INVALID_SHADER        = 0;
    static constexpr MeshHandle         INVALID_MESH          = 0;
    static constexpr RenderTargetHandle INVALID_RENDER_TARGET = 0;

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
        Depth24Stencil8,

        // HDR color formats — added for the portal feature's HDR + bloom
        // pipeline. Backends map these to GL_RGBA16F / GL_RGBA32F /
        // GL_R11F_G11F_B10F and the equivalent Vulkan VK_FORMAT_*. Use
        // RGBA16F as the default HDR choice (16-bit per channel, ~half
        // the memory of 32F, full HDR range).
        RGBA16F,
        RGBA32F,
        R11G11B10F
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
    // RENDER TARGET (offscreen framebuffer)
    // ========================================================================
    //
    // Describes an offscreen color+depth render target. Backends create
    // these as FBOs (GL) or VkFramebuffers (Vulkan). The portal feature's
    // HDR + bloom + recursion pipelines render into these instead of the
    // default backbuffer.
    //
    // colorFormat MUST be an HDR float format for HDR rendering; depth
    // is typically Depth24Stencil8 to match the default backbuffer.
    struct RenderTargetDesc {
        int           width        = 0;
        int           height       = 0;
        TextureFormat colorFormat  = TextureFormat::RGBA16F;
        TextureFormat depthFormat  = TextureFormat::Depth24Stencil8;
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
        SrcColor,
        OneMinusSrcColor,
        DstColor,
        OneMinusDstColor,
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

    // Stencil operations — what to do with a stencil-buffer value when the
    // stencil test fails / depth fails / both pass. Mirrors GL_KEEP /
    // GL_REPLACE / etc. and Vulkan's VkStencilOp 1-to-1.
    //
    // The portal renderer (Phase 6+) uses these to mark per-portal regions:
    //   • Pass = Replace (with stencilReference = recursion level) writes
    //     the level into stencil where the portal silhouette is drawn.
    //   • Subsequent passes use stencilCompareOp = Equal + the same
    //     reference to confine the scene re-render to that silhouette.
    enum class StencilOp {
        Keep,        // No change
        Zero,        // Set stencil = 0
        Replace,     // Set stencil = stencilReference
        IncrClamp,   // Increment, clamped to 0xFF
        DecrClamp,   // Decrement, clamped to 0
        Invert,      // Bitwise invert
        IncrWrap,    // Increment, wrap on overflow
        DecrWrap     // Decrement, wrap on underflow
    };

    // Attribute data type
    enum class AttribType : uint8_t { Float = 0, UByte = 1 };

    // Describes how one vertex attribute is laid out
    struct VertexAttribute {
        uint32_t location;        // Shader attribute location
        uint32_t componentCount;  // 1-4 (float, vec2, vec3, vec4)
        uint32_t offset;          // Byte offset within vertex
        bool normalized = false;  // Whether to normalize integer data
        AttribType type = AttribType::Float;  // Data type of components
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

        // Color write — disabling lets a draw write only depth/stencil while
        // leaving the framebuffer color untouched. Used by the portal
        // renderer's stencil-mark and depth-refill sub-passes.
        bool      colorWriteEnabled   = true;

        // Stencil — disabled by default so existing pipelines have zero
        // behavioural change. When enabled, applies to BOTH faces (no
        // separate front/back ops here; revisit if a feature actually needs
        // them). Defaults match the OpenGL spec's "no-op" stencil:
        //   compareOp = Always, all ops = Keep, reference = 0, masks = 0xFF.
        bool      stencilTestEnabled  = false;
        CompareOp stencilCompareOp    = CompareOp::Always;
        StencilOp stencilFailOp       = StencilOp::Keep;
        StencilOp stencilDepthFailOp  = StencilOp::Keep;
        StencilOp stencilPassOp       = StencilOp::Keep;
        uint32_t  stencilReadMask     = 0xFFu;
        uint32_t  stencilWriteMask    = 0xFFu;
        uint32_t  stencilReference    = 0;
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
    // STANDARD VERTEX LAYOUT (24 bytes per vertex)
    // ========================================================================

    // The block vertex layout used by the chunk mesh system:
    //   Position (3 floats) + UV (2 floats) + Color (4 ubytes normalized)
    inline VertexLayout GetBlockVertexLayout() {
        VertexLayout layout;
        layout.stride = 24;
        layout.attributes = {
            {0, 3, 0, false, AttribType::Float},                                      // Position: 3 floats at offset 0
            {1, 2, static_cast<uint32_t>(sizeof(float) * 3), false, AttribType::Float},  // UV: 2 floats at offset 12
            {2, 4, static_cast<uint32_t>(sizeof(float) * 5), true, AttribType::UByte},   // Color: 4 ubytes normalized at offset 20
        };
        return layout;
    }

} // namespace Render
