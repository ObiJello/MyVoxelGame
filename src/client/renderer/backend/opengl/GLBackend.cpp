// File: src/client/renderer/backend/opengl/GLBackend.cpp
#include "GLBackend.hpp"
#include "common/core/Log.hpp"
#include "common/core/Profiling_Tracy.hpp"
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <glm/gtc/type_ptr.hpp>
#include <fstream>
#include <sstream>
#include <cstring>
#include <algorithm>
#include <cctype>
#include <string>
#include <cstdlib>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

namespace Render {

    // ========================================================================
    // LIFECYCLE
    // ========================================================================

    GLBackend::GLBackend() = default;

    GLBackend::~GLBackend() {
        try {
            Shutdown();
        } catch (...) {
            // Swallow exceptions during static destruction — the GL context
            // may already be torn down by the time this destructor runs.
        }
    }

    bool GLBackend::Initialize(GLFWwindow* window) {
        m_window = window;
        Log::Info("GLBackend: Initializing OpenGL 3.3 backend");

        // Set initial GL state to match our default PipelineState
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
        glFrontFace(GL_CCW);
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        // Stencil starts disabled — matches PipelineState defaults. Set the
        // op + func to known no-ops so even if a buggy caller somehow flips
        // GL_STENCIL_TEST without a fresh SetPipelineState, behaviour is sane.
        glDisable(GL_STENCIL_TEST);
        glStencilFunc(GL_ALWAYS, 0, 0xFFu);
        glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
        glStencilMask(0xFFu);
        // Color writes default-on (matches PipelineState::colorWriteEnabled = true).
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

        // User clip plane 0 — used by the chunk vertex shader's
        // gl_ClipDistance[0] output for the portal see-through
        // rendering. Mirrors Portal's PushCustomClipPlane (Source SDK).
        // Always enabled; the shader writes gl_ClipDistance[0]=1 (always
        // pass) when the uniform plane is vec4(0), so this has no effect
        // outside the portal pass.
        glEnable(GL_CLIP_DISTANCE0);

        // Captured once for the F3 overlay (glGetString is a driver round trip).
        {
            auto str = [](GLenum e) { const GLubyte* v = glGetString(e); return v ? std::string(reinterpret_cast<const char*>(v)) : std::string(); };
            m_deviceInfo.vendorName  = str(GL_VENDOR);
            m_deviceInfo.name        = str(GL_RENDERER);
            m_deviceInfo.driverInfo  = str(GL_VERSION);
            m_deviceInfo.backendName = GetName();
            // GL has no device-class query; Apple's integrated parts and every
            // software renderer announce themselves in the renderer string.
            std::string lower = m_deviceInfo.name;
            for (char& c : lower) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
            if (lower.find("llvmpipe") != std::string::npos || lower.find("software") != std::string::npos ||
                lower.find("swiftshader") != std::string::npos) {
                m_deviceInfo.type = GpuDeviceInfo::Type::Cpu;
            } else if (lower.find("apple m") != std::string::npos || lower.find("intel") != std::string::npos ||
                       lower.find("iris") != std::string::npos || lower.find("uhd") != std::string::npos) {
                m_deviceInfo.type = GpuDeviceInfo::Type::Integrated;
            } else if (!lower.empty()) {
                m_deviceInfo.type = GpuDeviceInfo::Type::Discrete;
            }
        }

        m_stateInitialized = true;
        Log::Info("GLBackend: Initialized successfully");
        return true;
    }

    void GLBackend::Shutdown() {
        // Clean up all resources
        for (auto& [handle, mesh] : m_meshes) {
            if (mesh.vao != 0) glDeleteVertexArrays(1, &mesh.vao);
        }
        m_meshes.clear();

        for (auto& [handle, buf] : m_buffers) {
            if (buf.glId != 0) glDeleteBuffers(1, &buf.glId);
        }
        m_buffers.clear();

        for (auto& [handle, tex] : m_textures) {
            if (tex.glId != 0) glDeleteTextures(1, &tex.glId);
        }
        m_textures.clear();

        for (auto& [handle, shader] : m_shaders) {
            if (shader.programId != 0) glDeleteProgram(shader.programId);
        }
        m_shaders.clear();

        for (auto& [handle, timer] : m_timers) {
            if (timer.queryId != 0) glDeleteQueries(1, &timer.queryId);
        }
        m_timers.clear();

        DestroyUploadBuffer();
        DestroySceneTarget();
        for (GLsync& fence : m_frameFences) {
            if (fence) glDeleteSync(fence);
            fence = nullptr;
        }

        m_memStats = {};
        Log::Info("GLBackend: Shutdown complete");
    }

    void GLBackend::SetVSync(bool enabled) {
        glfwSwapInterval(enabled ? 1 : 0);
    }

    // ========================================================================
    // FRAME
    // ========================================================================

    void GLBackend::BeginFrame() {
        // The frame begins implicitly; only a scaled scene has anything to
        // set up — the request is this frame's and is consumed here.
        const int reqW = m_sceneReqWidth;
        const int reqH = m_sceneReqHeight;
        m_sceneReqWidth = m_sceneReqHeight = 0;
        if (m_sceneActive) ResolveScaledScene();   // never left open across frames
        if (reqW <= 0 || reqH <= 0 || m_sceneBroken) {
            // Back at 100 %: the scene's memory is not kept for a scale that
            // may never come back.
            if (reqW <= 0 && m_sceneFbo != 0) DestroySceneTarget();
            return;
        }
        if (!EnsureSceneTarget(reqW, reqH)) return;
        m_defaultFbo  = m_sceneFbo;
        m_sceneActive = true;
        glBindFramebuffer(GL_FRAMEBUFFER, m_sceneFbo);
        glViewport(0, 0, m_sceneWidth, m_sceneHeight);
        m_viewportHeight = m_sceneHeight;
    }

    void GLBackend::RequestScaledScene(int width, int height) {
        // Same size as the window = no scene: the zero-cost path.
        int fbW = 0, fbH = 0;
        if (m_window) glfwGetFramebufferSize(m_window, &fbW, &fbH);
        if (width <= 0 || height <= 0 || (width == fbW && height == fbH)) {
            m_sceneReqWidth = m_sceneReqHeight = 0;
            return;
        }
        if (m_sceneMaxDim == 0) {   // asked once: a glGet is a driver round trip
            GLint maxRb = 0, maxTex = 0;
            glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &maxRb);
            glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTex);
            m_sceneMaxDim = std::max(1, static_cast<int>(maxRb > 0 && maxTex > 0 ? std::min(maxRb, maxTex)
                                                                                  : std::max(maxRb, maxTex)));
        }
        width  = std::min(width, m_sceneMaxDim);
        height = std::min(height, m_sceneMaxDim);
        m_sceneReqWidth  = width;
        m_sceneReqHeight = height;
    }

    bool GLBackend::EnsureSceneTarget(int width, int height) {
        if (m_sceneFbo != 0 && m_sceneWidth == width && m_sceneHeight == height) return true;
        DestroySceneTarget();
        glGenTextures(1, &m_sceneColorTex);
        glBindTexture(GL_TEXTURE_2D, m_sceneColorTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glGenTextures(1, &m_sceneDepthTex);
        glBindTexture(GL_TEXTURE_2D, m_sceneDepthTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, width, height, 0,
                     GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glBindTexture(GL_TEXTURE_2D, 0);

        glGenFramebuffers(1, &m_sceneFbo);
        glBindFramebuffer(GL_FRAMEBUFFER, m_sceneFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_sceneColorTex, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, m_sceneDepthTex, 0);
        const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            Log::Error("GLBackend: scaled scene target %dx%d incomplete (0x%x) - Render Resolution off this session",
                       width, height, status);
            DestroySceneTarget();
            m_sceneBroken = true;
            return false;
        }
        m_sceneWidth  = width;
        m_sceneHeight = height;
        const size_t bytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 8u;   // RGBA8 + D24S8
        m_memStats.textureMemory  += bytes;
        m_memStats.totalAllocated += bytes;
        return true;
    }

    void GLBackend::DestroySceneTarget() {
        if (m_sceneActive) {
            m_sceneActive = false;
            m_defaultFbo  = 0;
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
        }
        if (m_sceneFbo != 0 && m_sceneWidth > 0 && m_sceneHeight > 0) {
            const size_t bytes = static_cast<size_t>(m_sceneWidth) * static_cast<size_t>(m_sceneHeight) * 8u;
            m_memStats.textureMemory  -= bytes;
            m_memStats.totalAllocated -= bytes;
        }
        if (m_sceneFbo != 0)      glDeleteFramebuffers(1, &m_sceneFbo);
        if (m_sceneColorTex != 0) glDeleteTextures(1, &m_sceneColorTex);
        if (m_sceneDepthTex != 0) glDeleteTextures(1, &m_sceneDepthTex);
        m_sceneFbo = m_sceneColorTex = m_sceneDepthTex = 0;
        m_sceneWidth = m_sceneHeight = 0;
    }

    void GLBackend::ResolveScaledScene() {
        if (!m_sceneActive) return;
        PROFILE_ZONE_N("GL.ResolveScaledScene");
        if (m_oitPassOpen) OitEndPass();
        m_sceneActive = false;
        m_defaultFbo  = 0;
        int fbW = 0, fbH = 0;
        glfwGetFramebufferSize(m_window, &fbW, &fbH);
        // The blit honours the scissor test, and the clear below the depth
        // and stencil write masks: open them, then let the state cache
        // re-apply whatever comes next.
        glDisable(GL_SCISSOR_TEST);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, m_sceneFbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        // Bilinear both ways: a smooth upscale below 100 %, a 2×2 box at
        // 200 % (supersampling) and between.
        glBlitFramebuffer(0, 0, m_sceneWidth, m_sceneHeight, 0, 0, fbW, fbH,
                          GL_COLOR_BUFFER_BIT, GL_LINEAR);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        // The window's depth and stencil hold whatever an older frame left;
        // the GUI starts from cleared ones, as the native frame's world pass
        // leaves them no worse.
        glDepthMask(GL_TRUE);
        glStencilMask(0xFFu);
        glClearDepth(1.0);
        glClearStencil(0);
        glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        glViewport(0, 0, fbW, fbH);
        m_viewportHeight = fbH;
        m_stateInitialized = false;
    }

    void GLBackend::EndFrame(GLFWwindow* window) {
        if (m_sceneActive) ResolveScaledScene();
        EndUploadFrame();   // fence this frame's staged uploads
        // This frame's fence (UpdateBufferStreaming), replacing the one from
        // kFrameFences frames ago.
        GLsync& fence = m_frameFences[m_frameNumber % kFrameFences];
        if (fence) glDeleteSync(fence);
        fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        ++m_frameNumber;
        glfwSwapBuffers(window);
    }

    void GLBackend::WaitFrameComplete(uint64_t frame) {
        if (frame == 0 || frame >= m_frameNumber || m_frameNumber - frame >= kFrameFences) return;
        GLsync fence = m_frameFences[frame % kFrameFences];
        if (!fence) return;
        PROFILE_ZONE_N("GL.StreamFenceWait");
        glClientWaitSync(fence, GL_SYNC_FLUSH_COMMANDS_BIT, 1'000'000'000ull);
    }

    void GLBackend::UpdateBufferStreaming(BufferHandle handle, size_t offset,
                                          size_t size, const void* data) {
        auto it = m_buffers.find(handle);
        if (it == m_buffers.end() || size == 0) return;
        // OBEY_GL_SYNC_STREAM=1: A/B switch back to the synchronised write.
        static const bool s_sync = std::getenv("OBEY_GL_SYNC_STREAM") != nullptr;
        if (s_sync) { UpdateBuffer(handle, offset, size, data); return; }
        // The frame that last wrote (and drew) this buffer must be done with
        // it; its later writes this frame are already covered.
        if (it->second.lastStreamFrame != m_frameNumber) {
            WaitFrameComplete(it->second.lastStreamFrame);
            it->second.lastStreamFrame = m_frameNumber;
        }
        UpdateBufferUnsynchronized(handle, offset, size, data);
    }

    void GLBackend::SetClearColor(float r, float g, float b, float a) {
        glClearColor(r, g, b, a);
    }

    void GLBackend::Clear(bool color, bool depth, bool stencil) {
        GLbitfield mask = 0;
        if (color)   mask |= GL_COLOR_BUFFER_BIT;
        if (depth)   mask |= GL_DEPTH_BUFFER_BIT;
        if (stencil) {
            // glClear honours glStencilMask — must be 0xFF or partial bits
            // won't actually be cleared. Force full mask before clearing,
            // then restore to current PipelineState mask. (m_stateInitialized
            // means there IS a current state; otherwise restore to 0xFF
            // which matches the default PipelineState.)
            glStencilMask(0xFFu);
            mask |= GL_STENCIL_BUFFER_BIT;
        }
        if (mask) glClear(mask);
        if (stencil && m_stateInitialized) {
            glStencilMask(m_currentState.stencilWriteMask);
        }
    }

    void GLBackend::SetViewport(int x, int y, int width, int height) {
        glViewport(x, y, width, height);
        m_viewportHeight = height;
    }

    void GLBackend::SetScissorRect(int x, int y, int w, int h) {
        if (w <= 0 || h <= 0) {
            // Degenerate rect — clip everything rather than disabling the test,
            // which would wrongly let the draw through unclipped.
            glEnable(GL_SCISSOR_TEST);
            glScissor(0, 0, 0, 0);
            return;
        }
        int vpH = m_viewportHeight;
        if (vpH <= 0) {
            // SetViewport hasn't run yet this session — ask GL once rather than
            // flipping against 0 and clipping everything away.
            GLint vp[4] = {0, 0, 0, 0};
            glGetIntegerv(GL_VIEWPORT, vp);
            vpH = vp[3];
            m_viewportHeight = vpH;
        }
        glEnable(GL_SCISSOR_TEST);
        // Callers pass top-left origin (GUI convention); GL's is bottom-left.
        glScissor(x, vpH - (y + h), w, h);
    }

    void GLBackend::ClearScissorRect() {
        glDisable(GL_SCISSOR_TEST);
    }

    // ========================================================================
    // BUFFERS
    // ========================================================================


    // ========================================================================
    // ELEMENT-ARRAY BINDING SAFETY
    // ========================================================================
    //
    // GL_ELEMENT_ARRAY_BUFFER is per-VAO state, NOT global. DrawIndexed
    // deliberately leaves its VAO bound ("next DrawIndexed will rebind"), so
    // any glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ...) done afterwards edits
    // THAT vao's index binding — and glDeleteBuffers resets it in whatever VAO
    // is current.
    //
    // That is how a dropped item crashed the NVIDIA driver: drawing item A left
    // VAO_A bound, building item B's mesh called CreateBuffer(Index, ...) which
    // bound the new EBO and then bound 0, clearing VAO_A's index buffer. The
    // next glDrawElements on VAO_A took the offset as a CLIENT pointer and
    // dereferenced it inside the driver — an access violation, not a GL error,
    // so the debug callback never saw it either.
    //
    // Detach the VAO for the duration and put it back, so a buffer operation
    // can never be seen by an unrelated VAO and every caller's binding survives.
    namespace {
        struct DetachedVertexArray {
            GLint prev = 0;
            bool  active = false;
            explicit DetachedVertexArray(GLenum target) {
                if (target != GL_ELEMENT_ARRAY_BUFFER) return;
                glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prev);
                if (prev == 0) return;          // nothing bound: nothing to protect
                glBindVertexArray(0);
                active = true;
            }
            ~DetachedVertexArray() { if (active) glBindVertexArray(static_cast<GLuint>(prev)); }
            DetachedVertexArray(const DetachedVertexArray&) = delete;
            DetachedVertexArray& operator=(const DetachedVertexArray&) = delete;
        };
    } // namespace

    BufferHandle GLBackend::CreateBuffer(BufferUsage usage, size_t size,
                                        const void* data, BufferAccess access) {
        GLuint glId = 0;
        glGenBuffers(1, &glId);
        if (glId == 0) return INVALID_BUFFER;

        GLenum target = ToGLBufferTarget(usage);
        GLenum glUsage = ToGLBufferUsage(access);

        const DetachedVertexArray noVao(target);   // see DetachedVertexArray
        glBindBuffer(target, glId);
        while (glGetError() != GL_NO_ERROR) {}          // clear anything stale
        glBufferData(target, static_cast<GLsizeiptr>(size), data, glUsage);
        const GLenum bufErr = glGetError();
        glBindBuffer(target, 0);
        if (bufErr != GL_NO_ERROR) {
            // Most often GL_OUT_OF_MEMORY. The name stays valid but has no
            // storage, and drawing from it faults inside the driver rather
            // than raising a GL error — so refuse the buffer here.
            Log::Error("[GLBackend] CreateBuffer: %zu bytes failed (GL error 0x%04X)",
                       size, static_cast<unsigned>(bufErr));
            glDeleteBuffers(1, &glId);
            return INVALID_BUFFER;
        }

        uint32_t handle = AllocHandle();
        m_buffers[handle] = {glId, target, size};

        m_memStats.bufferMemory += size;
        m_memStats.totalAllocated += size;
        m_memStats.bufferCount++;
        if (m_memStats.totalAllocated > m_memStats.peakUsage)
            m_memStats.peakUsage = m_memStats.totalAllocated;

        return handle;
    }

    void GLBackend::UpdateBuffer(BufferHandle handle, size_t offset,
                                size_t size, const void* data) {
        auto it = m_buffers.find(handle);
        if (it == m_buffers.end()) return;

        // No unbind afterwards: every draw path binds its own buffers before use,
        // so restoring binding 0 here was pure driver churn (2+ redundant calls
        // per mesh upload). Note ELEMENT_ARRAY binding is per-VAO state either
        // way — BindSlab rebinds the IBO before every terrain draw.
        const DetachedVertexArray noVao(it->second.target);   // see DetachedVertexArray
        glBindBuffer(it->second.target, it->second.glId);
        glBufferSubData(it->second.target, static_cast<GLintptr>(offset),
                       static_cast<GLsizeiptr>(size), data);
    }

    void GLBackend::UpdateBufferUnsynchronized(BufferHandle handle, size_t offset,
                                               size_t size, const void* data) {
        auto it = m_buffers.find(handle);
        if (it == m_buffers.end() || size == 0) return;
        // OBEY_SYNC_UPLOADS=1: A/B switch back to the synchronised path
        // (glBufferSubData, which Apple's driver serialises against pending
        // draws of the buffer) — for measuring what the unsynchronised map
        // saves, and as the fallback if a caller ever writes a range a
        // queued draw still reads.
        static const bool s_forceSync = std::getenv("OBEY_SYNC_UPLOADS") != nullptr;
        if (s_forceSync) { UpdateBuffer(handle, offset, size, data); return; }

        const DetachedVertexArray noVao(it->second.target);   // see DetachedVertexArray
        glBindBuffer(it->second.target, it->second.glId);

        // GL_MAP_UNSYNCHRONIZED_BIT is the whole point: glBufferSubData on a
        // buffer with draws in flight makes the driver serialise, which measured
        // 0.18ms per translucency re-sort — 16x the cost of the sort itself.
        //
        // Deliberately NOT GL_MAP_INVALIDATE_RANGE_BIT. Invalidating would let
        // the driver treat the old contents as undefined, and the safety argument
        // here depends on the GPU only ever seeing a mix of the OLD and NEW
        // orderings, both of which are valid permutations of the same quads.
        void* dst = glMapBufferRange(it->second.target,
                                     static_cast<GLintptr>(offset),
                                     static_cast<GLsizeiptr>(size),
                                     GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT);
        if (!dst) {
            // Mapping can fail (out of address space, driver refusal). Falling
            // back keeps us correct — just synchronised, i.e. the old behaviour.
            glBufferSubData(it->second.target, static_cast<GLintptr>(offset),
                            static_cast<GLsizeiptr>(size), data);
            return;
        }

        std::memcpy(dst, data, size);
        glUnmapBuffer(it->second.target);
    }

    void GLBackend::DestroyBuffer(BufferHandle handle) {
        auto it = m_buffers.find(handle);
        if (it == m_buffers.end()) return;

        {
            const DetachedVertexArray noVao(it->second.target);   // see DetachedVertexArray
            glDeleteBuffers(1, &it->second.glId);
        }
        m_memStats.bufferMemory -= it->second.size;
        m_memStats.totalAllocated -= it->second.size;
        m_memStats.bufferCount--;
        m_buffers.erase(it);
    }

    // ========================================================================
    // TEXTURES
    // ========================================================================

    TextureHandle GLBackend::CreateTexture2D(int width, int height,
                                            TextureFormat format, const void* data) {
        GLuint glId = 0;
        glGenTextures(1, &glId);
        if (glId == 0) return INVALID_TEXTURE;

        // Default to RGBA8 (LDR). Override for HDR + depth formats below.
        GLenum internalFormat = GL_RGBA8;
        GLenum dataFormat     = GL_RGBA;
        GLenum dataType       = GL_UNSIGNED_BYTE;
        int    bytesPerPixel  = 4;
        switch (format) {
            case TextureFormat::RGBA8:
                /* defaults */ break;
            case TextureFormat::SRGB8_A8:
                internalFormat = GL_SRGB8_ALPHA8; break;
            case TextureFormat::RGBA16F:
                internalFormat = GL_RGBA16F;
                dataType       = GL_HALF_FLOAT;
                bytesPerPixel  = 8;
                break;
            case TextureFormat::RGBA32F:
                internalFormat = GL_RGBA32F;
                dataType       = GL_FLOAT;
                bytesPerPixel  = 16;
                break;
            case TextureFormat::R11G11B10F:
                internalFormat = GL_R11F_G11F_B10F;
                dataFormat     = GL_RGB;
                dataType       = GL_UNSIGNED_INT_10F_11F_11F_REV;
                bytesPerPixel  = 4;
                break;
            case TextureFormat::Depth24Stencil8:
                internalFormat = GL_DEPTH24_STENCIL8;
                dataFormat     = GL_DEPTH_STENCIL;
                dataType       = GL_UNSIGNED_INT_24_8;
                bytesPerPixel  = 4;
                break;
        }

        glBindTexture(GL_TEXTURE_2D, glId);
        glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, width, height, 0,
                    dataFormat, dataType, data);

        // Default filtering
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glBindTexture(GL_TEXTURE_2D, 0);

        size_t memSize = static_cast<size_t>(width) * height * bytesPerPixel;
        uint32_t handle = AllocHandle();
        m_textures[handle] = {glId, width, height, memSize,
                              internalFormat, dataFormat, dataType};

        m_memStats.textureMemory += memSize;
        m_memStats.totalAllocated += memSize;
        m_memStats.textureCount++;
        if (m_memStats.totalAllocated > m_memStats.peakUsage)
            m_memStats.peakUsage = m_memStats.totalAllocated;

        return handle;
    }

    TextureHandle GLBackend::CreateTexture2DArray(int width, int height, int layers, int mipLevels,
                                                 TextureFormat format) {
        // RGBA8 only: the one caller is the block atlas's sprite array. The
        // context is 3.3 core, so the levels are declared one glTexImage3D
        // each (glTexStorage3D is 4.2) and capped with MAX_LEVEL.
        if (width <= 0 || height <= 0 || layers <= 0 || mipLevels <= 0) return INVALID_TEXTURE;
        if (format != TextureFormat::RGBA8) {
            Log::Error("GLBackend::CreateTexture2DArray: only RGBA8 arrays are supported");
            return INVALID_TEXTURE;
        }
        GLuint glId = 0;
        glGenTextures(1, &glId);
        if (glId == 0) return INVALID_TEXTURE;
        glBindTexture(GL_TEXTURE_2D_ARRAY, glId);
        size_t memSize = 0;
        for (int level = 0; level < mipLevels; ++level) {
            const int lw = std::max(1, width >> level), lh = std::max(1, height >> level);
            glTexImage3D(GL_TEXTURE_2D_ARRAY, level, GL_RGBA8, lw, lh, layers, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            memSize += static_cast<size_t>(lw) * static_cast<size_t>(lh) * 4u * static_cast<size_t>(layers);
        }
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_BASE_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL, mipLevels - 1);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D_ARRAY, 0);

        const uint32_t handle = AllocHandle();
        m_textures[handle] = {glId, width, height, memSize, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE};
        m_textures[handle].target = GL_TEXTURE_2D_ARRAY;
        m_memStats.textureMemory += memSize;
        m_memStats.totalAllocated += memSize;
        m_memStats.textureCount++;
        if (m_memStats.totalAllocated > m_memStats.peakUsage)
            m_memStats.peakUsage = m_memStats.totalAllocated;
        return handle;
    }

    int GLBackend::MaxTextureArrayLayers() const {
        GLint layers = 0;
        glGetIntegerv(GL_MAX_ARRAY_TEXTURE_LAYERS, &layers);   // at least 256 in 3.3 core
        return std::max(0, static_cast<int>(layers));
    }

    void GLBackend::UploadTextureArrayLevel(TextureHandle handle, int level, int width, int height, int layers,
                                            const void* data) {
        auto it = m_textures.find(handle);
        if (it == m_textures.end() || it->second.target != GL_TEXTURE_2D_ARRAY || !data) return;
        if (level < 0 || width <= 0 || height <= 0 || layers <= 0) return;
        glBindTexture(GL_TEXTURE_2D_ARRAY, it->second.glId);
        // `data` is `layers` images back to back — exactly a depth-`layers`
        // 3D sub-image.
        glTexSubImage3D(GL_TEXTURE_2D_ARRAY, level, 0, 0, 0, width, height, layers,
                        it->second.dataFormat, it->second.dataType, data);
        glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
    }

    void GLBackend::UpdateTexture2D(TextureHandle handle, int x, int y,
                                   int width, int height, const void* data) {
        auto it = m_textures.find(handle);
        if (it == m_textures.end()) return;

        glBindTexture(GL_TEXTURE_2D, it->second.glId);
        glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, width, height,
                       GL_RGBA, GL_UNSIGNED_BYTE, data);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    void GLBackend::SetTextureFilter(TextureHandle handle,
                                    TextureFilter min, TextureFilter mag) {
        auto it = m_textures.find(handle);
        if (it == m_textures.end()) return;

        // Whatever the texture is (2D, or the sprite array): the parameters
        // belong to the object, set through its own target.
        const GLenum target = it->second.target;
        glBindTexture(target, it->second.glId);
        glTexParameteri(target, GL_TEXTURE_MIN_FILTER, ToGLFilter(min));
        glTexParameteri(target, GL_TEXTURE_MAG_FILTER, ToGLFilter(mag));
        glBindTexture(target, 0);
    }

    void GLBackend::SetTextureWrap(TextureHandle handle, TextureWrap s, TextureWrap t) {
        auto it = m_textures.find(handle);
        if (it == m_textures.end()) return;

        const GLenum target = it->second.target;
        glBindTexture(target, it->second.glId);
        glTexParameteri(target, GL_TEXTURE_WRAP_S, ToGLWrap(s));
        glTexParameteri(target, GL_TEXTURE_WRAP_T, ToGLWrap(t));
        glBindTexture(target, 0);
    }

    void GLBackend::GenerateMipmaps(TextureHandle handle) {
        auto it = m_textures.find(handle);
        if (it == m_textures.end()) return;

        glBindTexture(GL_TEXTURE_2D, it->second.glId);
        glGenerateMipmap(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    void GLBackend::UploadTextureMipLevel(TextureHandle handle, int level,
                                          int width, int height, const void* data) {
        auto it = m_textures.find(handle);
        if (it == m_textures.end()) return;

        glBindTexture(GL_TEXTURE_2D, it->second.glId);
        // glTexImage2D rather than glTexSubImage2D: level > 0 has no storage
        // until it is declared, and the atlas hands us a complete level.
        glTexImage2D(GL_TEXTURE_2D, level,
                     static_cast<GLint>(it->second.internalFormat),
                     width, height, 0,
                     it->second.dataFormat, it->second.dataType, data);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    void GLBackend::UpdateTexture2DLevel(TextureHandle handle, int level, int x, int y,
                                         int width, int height, const void* data) {
        auto it = m_textures.find(handle);
        if (it == m_textures.end()) return;

        glBindTexture(GL_TEXTURE_2D, it->second.glId);
        glTexSubImage2D(GL_TEXTURE_2D, level, x, y, width, height,
                        it->second.dataFormat, it->second.dataType, data);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    void GLBackend::DestroyUploadBuffer() {
        for (UploadSlot& slot : m_uploadSlots) {
            if (slot.fence) glDeleteSync(slot.fence);
            slot = UploadSlot{};
        }
        if (m_uploadPbo != 0) glDeleteBuffers(1, &m_uploadPbo);
        m_uploadPbo = 0;
        m_uploadSlotSize = 0;
        m_uploadSlot = 0;
        m_uploadSlotReady = false;
    }

    void GLBackend::EndUploadFrame() {
        UploadSlot& slot = m_uploadSlots[m_uploadSlot];
        if (slot.used > 0) {
            // The GPU is done reading this region once everything submitted
            // so far has run; the region is reused kUploadSlots frames on.
            if (slot.fence) glDeleteSync(slot.fence);
            slot.fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
            m_uploadSlot = (m_uploadSlot + 1) % kUploadSlots;
            m_uploadSlotReady = false;
        }
    }

    void GLBackend::UpdateTexture2DLevelStaged(TextureHandle handle, int level, int x, int y,
                                               int width, int height, const void* data) {
        StagedSubImage(handle, -1, level, x, y, width, height, data);
    }

    void GLBackend::UpdateTextureArrayLevel(TextureHandle handle, int layer, int level, int x, int y,
                                            int width, int height, const void* data) {
        if (layer < 0) return;
        StagedSubImage(handle, layer, level, x, y, width, height, data);
    }

    void GLBackend::StagedSubImage(TextureHandle handle, int layer, int level, int x, int y,
                                   int width, int height, const void* data) {
        auto it = m_textures.find(handle);
        if (it == m_textures.end() || !data || width <= 0 || height <= 0) return;
        const bool array = layer >= 0;
        if (array != (it->second.target == GL_TEXTURE_2D_ARRAY)) return;
        // The direct copy — a CPU wait on Apple's driver when the texture is
        // in use (TextureAnimator.hpp) — for whatever the ring cannot take.
        auto plain = [&] {
            if (!array) {
                UpdateTexture2DLevel(handle, level, x, y, width, height, data);
                return;
            }
            glBindTexture(GL_TEXTURE_2D_ARRAY, it->second.glId);
            glTexSubImage3D(GL_TEXTURE_2D_ARRAY, level, x, y, layer, width, height, 1,
                            it->second.dataFormat, it->second.dataType, data);
            glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
        };
        // RGBA8 only (4 bytes a texel); anything else takes the plain path.
        if (it->second.dataFormat != GL_RGBA || it->second.dataType != GL_UNSIGNED_BYTE) {
            plain();
            return;
        }
        const size_t bytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
        const size_t aligned = (bytes + 255u) & ~static_cast<size_t>(255u);

        // One region must hold a frame's uploads: grow (re-specify the whole
        // buffer — the driver hands back fresh storage, so no fence is
        // needed for the old contents) when this one alone would not fit.
        if (m_uploadPbo == 0 || aligned > m_uploadSlotSize) {
            const size_t want = std::max<size_t>(size_t(2) << 20, aligned * 2);
            if (m_uploadPbo == 0) glGenBuffers(1, &m_uploadPbo);
            for (UploadSlot& slot : m_uploadSlots) {
                if (slot.fence) glDeleteSync(slot.fence);
                slot = UploadSlot{};
            }
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, m_uploadPbo);
            glBufferData(GL_PIXEL_UNPACK_BUFFER, static_cast<GLsizeiptr>(want * kUploadSlots), nullptr, GL_STREAM_DRAW);
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
            m_uploadSlotSize = want;
            m_uploadSlotReady = true;   // fresh storage: nothing to wait for
        }
        UploadSlot& slot = m_uploadSlots[m_uploadSlot];
        if (!m_uploadSlotReady) {
            // First write this frame into a region last used kUploadSlots
            // frames ago: normally long finished, so this returns at once.
            if (slot.fence) {
                PROFILE_ZONE_N("GL.UploadFenceWait");
                glClientWaitSync(slot.fence, 0, 1'000'000'000ull);
                glDeleteSync(slot.fence);
                slot.fence = nullptr;
            }
            slot.used = 0;
            m_uploadSlotReady = true;
        }
        if (slot.used + aligned > m_uploadSlotSize) {
            // The region is full this frame: fall back to the direct path.
            plain();
            return;
        }

        const size_t offset = static_cast<size_t>(m_uploadSlot) * m_uploadSlotSize + slot.used;
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, m_uploadPbo);
        // Unsynchronized: the fence above proved no queued command reads
        // this range any more.
        void* dst = glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, static_cast<GLintptr>(offset),
                                     static_cast<GLsizeiptr>(bytes),
                                     GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT | GL_MAP_INVALIDATE_RANGE_BIT);
        if (!dst) {
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
            plain();
            return;
        }
        std::memcpy(dst, data, bytes);
        glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
        const GLenum target = it->second.target;
        glBindTexture(target, it->second.glId);
        // With a pixel-unpack buffer bound the "pointer" is a byte offset
        // into it: the copy into the texture is queued, not done here.
        if (array) {
            glTexSubImage3D(target, level, x, y, layer, width, height, 1, GL_RGBA, GL_UNSIGNED_BYTE,
                            reinterpret_cast<const void*>(offset));
        } else {
            glTexSubImage2D(target, level, x, y, width, height, GL_RGBA, GL_UNSIGNED_BYTE,
                            reinterpret_cast<const void*>(offset));
        }
        glBindTexture(target, 0);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        slot.used += aligned;
    }

    void GLBackend::ReserveTextureMipLevels(TextureHandle handle, int maxLevel) {
        auto it = m_textures.find(handle);
        if (it == m_textures.end()) return;

        // GL grows a mip chain lazily as levels are declared, so unlike Vulkan
        // there is nothing to reallocate here — capping the range is enough,
        // and level 0's contents survive.
        glBindTexture(GL_TEXTURE_2D, it->second.glId);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, maxLevel);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    void GLBackend::DestroyTexture(TextureHandle handle) {
        auto it = m_textures.find(handle);
        if (it == m_textures.end()) return;

        glDeleteTextures(1, &it->second.glId);
        m_memStats.textureMemory -= it->second.memorySize;
        m_memStats.totalAllocated -= it->second.memorySize;
        m_memStats.textureCount--;
        m_textures.erase(it);
    }

    void GLBackend::BindTexture(TextureHandle handle, uint32_t slot) {
        glActiveTexture(GL_TEXTURE0 + slot);
        auto it = m_textures.find(handle);
        if (it != m_textures.end()) {
            glBindTexture(it->second.target, it->second.glId);
        } else {
            glBindTexture(GL_TEXTURE_2D, 0);
        }
    }

    TextureHandle GLBackend::CreateBufferTexture(BufferHandle buffer, TextureFormat format) {
        auto bit = m_buffers.find(buffer);
        if (bit == m_buffers.end()) return INVALID_TEXTURE;
        // Core since GL 3.1 (this context is 3.3). Only the formats the face
        // map needs are mapped; add to the switch when another caller wants
        // more.
        GLenum internalFormat = 0;
        switch (format) {
            case TextureFormat::RGBA8:   internalFormat = GL_RGBA8;   break;
            case TextureFormat::RGBA16:  internalFormat = GL_RGBA16;  break;
            case TextureFormat::RGBA16UI: internalFormat = GL_RGBA16UI; break;
            case TextureFormat::RGBA16F: internalFormat = GL_RGBA16F; break;
            case TextureFormat::RGBA32F: internalFormat = GL_RGBA32F; break;
            default:
                Log::Error("GLBackend::CreateBufferTexture: unsupported format");
                return INVALID_TEXTURE;
        }
        // The implementation's texel limit — the whole buffer must fit or
        // texelFetch beyond it returns undefined data (an 8 MB slab is 2 M
        // RGBA8 texels; the limit is typically 128 M).
        GLint maxTexels = 0;
        glGetIntegerv(GL_MAX_TEXTURE_BUFFER_SIZE, &maxTexels);
        const size_t bytesPerTexel = (format == TextureFormat::RGBA8) ? 4
                                   : (format == TextureFormat::RGBA16F || format == TextureFormat::RGBA16 ||
                                      format == TextureFormat::RGBA16UI) ? 8 : 16;
        if (maxTexels > 0 && bit->second.size / bytesPerTexel > static_cast<size_t>(maxTexels)) {
            Log::Error("GLBackend::CreateBufferTexture: buffer of %zu bytes exceeds GL_MAX_TEXTURE_BUFFER_SIZE (%d texels)",
                       bit->second.size, maxTexels);
            return INVALID_TEXTURE;
        }

        GLuint glId = 0;
        glGenTextures(1, &glId);
        if (glId == 0) return INVALID_TEXTURE;
        glBindTexture(GL_TEXTURE_BUFFER, glId);
        glTexBuffer(GL_TEXTURE_BUFFER, internalFormat, bit->second.glId);
        glBindTexture(GL_TEXTURE_BUFFER, 0);

        uint32_t handle = AllocHandle();
        GLTextureInfo info{};
        info.glId = glId;
        info.width = static_cast<int>(bit->second.size / bytesPerTexel);
        info.height = 1;
        info.memorySize = 0;   // the buffer owns the bytes
        info.target = GL_TEXTURE_BUFFER;
        info.internalFormat = internalFormat;
        m_textures[handle] = info;
        m_memStats.textureCount++;
        return handle;
    }

    uintptr_t GLBackend::GetNativeTextureID(TextureHandle handle) const {
        auto it = m_textures.find(handle);
        if (it != m_textures.end()) {
            return static_cast<uintptr_t>(it->second.glId);
        }
        return 0;
    }

    // ========================================================================
    // SHADERS
    // ========================================================================

    ShaderHandle GLBackend::CreateShader(const std::string& vertexSource,
                                        const std::string& fragmentSource) {
        GLuint vShader = CompileGLShader(GL_VERTEX_SHADER, vertexSource);
        if (vShader == 0) return INVALID_SHADER;

        GLuint fShader = CompileGLShader(GL_FRAGMENT_SHADER, fragmentSource);
        if (fShader == 0) {
            glDeleteShader(vShader);
            return INVALID_SHADER;
        }

        GLuint program = glCreateProgram();
        glAttachShader(program, vShader);
        glAttachShader(program, fShader);
        glLinkProgram(program);

        GLint success;
        glGetProgramiv(program, GL_LINK_STATUS, &success);
        glDeleteShader(vShader);
        glDeleteShader(fShader);

        if (!success) {
            char infoLog[512];
            glGetProgramInfoLog(program, 512, nullptr, infoLog);
            Log::Error("GLBackend: Shader link failed: %s", infoLog);
            glDeleteProgram(program);
            return INVALID_SHADER;
        }

        // The one user uniform block (RenderBackend::BindUniformBuffer):
        // programs that declare it read binding point 0. Everything else
        // (glGetUniformBlockIndex returns GL_INVALID_INDEX) is untouched.
        {
            const GLuint blockIndex = glGetUniformBlockIndex(program, "SectionOrigins");
            if (blockIndex != GL_INVALID_INDEX) glUniformBlockBinding(program, blockIndex, 0);
        }

        uint32_t handle = AllocHandle();
        m_shaders[handle] = {program, {}, vertexSource, fragmentSource};
        m_memStats.shaderCount++;
        return handle;
    }

    ShaderHandle GLBackend::CreateShaderFromFiles(const std::string& vertexPath,
                                                  const std::string& fragmentPath) {
        std::string vertSrc = ReadFileContents(vertexPath);
        std::string fragSrc = ReadFileContents(fragmentPath);
        if (vertSrc.empty() || fragSrc.empty()) return INVALID_SHADER;
        return CreateShader(vertSrc, fragSrc);
    }

    void GLBackend::DestroyShader(ShaderHandle handle) {
        auto it = m_shaders.find(handle);
        if (it == m_shaders.end()) return;
        glDeleteProgram(it->second.programId);
        m_memStats.shaderCount--;
        m_shaders.erase(it);
    }

    void GLBackend::BindShader(ShaderHandle handle) {
        if (m_oitStage != OitStage::None) {
            // Improved Transparency: the engine shader's variant for the
            // stage, or no draws at all with it.
            const ShaderHandle variant = OitVariantFor(handle);
            m_oitSkipDraw = variant == INVALID_SHADER;
            if (!m_oitSkipDraw) {
                auto vit = m_shaders.find(variant);
                if (vit != m_shaders.end()) {
                    glUseProgram(vit->second.programId);
                    const GLint loc = vit->second.GetUniform("OitProjParams");
                    if (loc != -1) glUniform4fv(loc, 1, glm::value_ptr(m_oitProjParams));
                }
            }
            m_boundShader = handle;
            return;
        }
        if (!m_overrideMode) {
            if (handle == m_boundShader) return;
            auto it = m_shaders.find(handle);
            if (it != m_shaders.end()) {
                glUseProgram(it->second.programId);
                m_boundShader = handle;
            }
            return;
        }
        // Override mode: the pack's program and its render target, or the
        // engine's program into the default target.
        RenderTargetHandle target = m_overrideDefaultTarget;
        ShaderHandle real = handle;
        auto ov = m_shaderOverrides.find(handle);
        if (ov != m_shaderOverrides.end()) {
            if (ov->second.shader != INVALID_SHADER) real = ov->second.shader;
            if (ov->second.target != INVALID_RENDER_TARGET) target = ov->second.target;
        }
        auto it = m_shaders.find(real);
        if (it != m_shaders.end()) {
            glUseProgram(it->second.programId);
            m_boundShader = handle;
        }
        if (target != INVALID_RENDER_TARGET) BindRenderTarget(target);
    }

    void GLBackend::SetShaderOverrideMode(bool on, RenderTargetHandle defaultTarget) {
        m_overrideMode = on;
        m_overrideDefaultTarget = on ? defaultTarget : INVALID_RENDER_TARGET;
        m_boundShader = INVALID_SHADER;   // force the next bind through
    }

    void GLBackend::SetShaderOverride(ShaderHandle engine, ShaderHandle pack, RenderTargetHandle target) {
        if (engine == INVALID_SHADER) return;
        if (pack == INVALID_SHADER && target == INVALID_RENDER_TARGET) { m_shaderOverrides.erase(engine); return; }
        m_shaderOverrides[engine] = {pack, target};
    }

    void GLBackend::ClearShaderOverrides() {
        m_shaderOverrides.clear();
        m_overrideMode = false;
        m_overrideDefaultTarget = INVALID_RENDER_TARGET;
    }

    void GLBackend::CheckErrors(const char* where) {
        static std::unordered_map<std::string, bool> s_seen;
        for (GLenum err = glGetError(); err != GL_NO_ERROR; err = glGetError()) {
            const std::string key = std::string(where) + "#" + std::to_string(err);
            if (s_seen.emplace(key, true).second) {
                const char* name = err == GL_INVALID_ENUM ? "INVALID_ENUM" : err == GL_INVALID_VALUE ? "INVALID_VALUE"
                                 : err == GL_INVALID_OPERATION ? "INVALID_OPERATION"
                                 : err == GL_INVALID_FRAMEBUFFER_OPERATION ? "INVALID_FRAMEBUFFER_OPERATION"
                                 : err == GL_OUT_OF_MEMORY ? "OUT_OF_MEMORY" : "?";
                Log::Error("GLBackend: GL error %s (0x%x) at %s", name, err, where);
            }
        }
    }

    bool GLBackend::ReadDepthPixel(RenderTargetHandle rt, int x, int y, float& out) {
        auto it = m_renderTargets.find(rt);
        if (it == m_renderTargets.end()) return false;
        GLint bound = 0;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &bound);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, it->second.fbo);
        float depth = -1.0f;
        glReadPixels(x, y, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &depth);
        glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(bound));
        out = depth;
        return true;
    }

    std::string GLBackend::DebugStateSummary() {
        GLint vp[4] = {0, 0, 0, 0}, sc[4] = {0, 0, 0, 0};
        glGetIntegerv(GL_VIEWPORT, vp);
        glGetIntegerv(GL_SCISSOR_BOX, sc);
        GLboolean mask[4] = {0, 0, 0, 0};
        glGetBooleanv(GL_COLOR_WRITEMASK, mask);
        GLint drawFbo = 0, readFbo = 0, program = 0, vao = 0, drawBuffer = 0, stencilFunc = 0, stencilRef = 0;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFbo);
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFbo);
        glGetIntegerv(GL_CURRENT_PROGRAM, &program);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
        glGetIntegerv(GL_DRAW_BUFFER, &drawBuffer);
        glGetIntegerv(GL_STENCIL_FUNC, &stencilFunc);
        glGetIntegerv(GL_STENCIL_REF, &stencilRef);
        GLint polyMode[2] = {0, 0};
        glGetIntegerv(GL_POLYGON_MODE, polyMode);
        char buf[512];
        std::snprintf(buf, sizeof(buf),
                      "viewport %d,%d %dx%d | scissor %s %d,%d %dx%d | colormask %d%d%d%d | depth %s mask %d | stencil %s func 0x%x ref %d | "
                      "cull %s | blend %s | rasterDiscard %s | polygon 0x%x | fbo draw %d read %d drawbuffer 0x%x | program %d vao %d",
                      vp[0], vp[1], vp[2], vp[3],
                      glIsEnabled(GL_SCISSOR_TEST) ? "on" : "off", sc[0], sc[1], sc[2], sc[3],
                      mask[0], mask[1], mask[2], mask[3],
                      glIsEnabled(GL_DEPTH_TEST) ? "on" : "off", [] { GLboolean m = 0; glGetBooleanv(GL_DEPTH_WRITEMASK, &m); return static_cast<int>(m); }(),
                      glIsEnabled(GL_STENCIL_TEST) ? "on" : "off", stencilFunc, stencilRef,
                      glIsEnabled(GL_CULL_FACE) ? "on" : "off", glIsEnabled(GL_BLEND) ? "on" : "off",
                      glIsEnabled(GL_RASTERIZER_DISCARD) ? "on" : "off", polyMode[0],
                      drawFbo, readFbo, drawBuffer, program, vao);
        return buf;
    }

    std::vector<ShaderHandle> GLBackend::FindShadersBySource(
        const std::function<bool(const std::string&, const std::string&)>& match) {
        std::vector<ShaderHandle> out;
        for (const auto& [handle, info] : m_shaders) {
            if (match(info.vertexSource, info.fragmentSource)) out.push_back(handle);
        }
        return out;
    }

    GLint GLBackend::GLShaderInfo::GetUniform(const std::string& name) const {
        if (programId == 0) return -1;
        auto it = uniformCache.find(name);
        if (it != uniformCache.end()) return it->second;
        GLint loc = glGetUniformLocation(programId, name.c_str());
        uniformCache[name] = loc;
        return loc;
    }

    void GLBackend::SetUniformMat4(ShaderHandle handle, const std::string& name,
                                   const glm::mat4& value) {
        auto it = m_shaders.find(ResolveShader(handle));
        if (it == m_shaders.end()) return;
        if (it->second.programId == 0) {
            Log::Error("GLBackend: SetUniformMat4 called with shader handle %u but programId is 0", handle);
            return;
        }
        GLint loc = it->second.GetUniform(name);
        if (loc != -1) glUniformMatrix4fv(loc, 1, GL_FALSE, glm::value_ptr(value));
    }

    void GLBackend::SetUniformVec4(ShaderHandle handle, const std::string& name,
                                   const glm::vec4& value) {
        auto it = m_shaders.find(ResolveShader(handle));
        if (it == m_shaders.end()) return;
        GLint loc = it->second.GetUniform(name);
        if (loc != -1) glUniform4fv(loc, 1, glm::value_ptr(value));
    }

    void GLBackend::SetUniformVec3(ShaderHandle handle, const std::string& name,
                                   const glm::vec3& value) {
        auto it = m_shaders.find(ResolveShader(handle));
        if (it == m_shaders.end()) return;
        GLint loc = it->second.GetUniform(name);
        if (loc != -1) glUniform3fv(loc, 1, glm::value_ptr(value));
    }

    void GLBackend::SetUniformVec2(ShaderHandle handle, const std::string& name,
                                   const glm::vec2& value) {
        auto it = m_shaders.find(ResolveShader(handle));
        if (it == m_shaders.end()) return;
        GLint loc = it->second.GetUniform(name);
        if (loc != -1) glUniform2fv(loc, 1, glm::value_ptr(value));
    }

    void GLBackend::SetUniformFloat(ShaderHandle handle, const std::string& name, float value) {
        auto it = m_shaders.find(ResolveShader(handle));
        if (it == m_shaders.end()) return;
        GLint loc = it->second.GetUniform(name);
        if (loc != -1) glUniform1f(loc, value);
    }

    void GLBackend::SetUniformInt(ShaderHandle handle, const std::string& name, int value) {
        auto it = m_shaders.find(ResolveShader(handle));
        if (it == m_shaders.end()) return;
        GLint loc = it->second.GetUniform(name);
        if (loc != -1) glUniform1i(loc, value);
    }

    void GLBackend::BlitRenderTargetDepth(RenderTargetHandle src, RenderTargetHandle dst) {
        auto s = m_renderTargets.find(src);
        auto d = m_renderTargets.find(dst);
        if (s == m_renderTargets.end() || d == m_renderTargets.end()) return;
        GLint bound = 0;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &bound);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, s->second.fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, d->second.fbo);
        glBlitFramebuffer(0, 0, s->second.width, s->second.height,
                          0, 0, d->second.width, d->second.height,
                          GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(bound));
    }

    bool GLBackend::CopyFramebufferToRenderTarget(RenderTargetHandle dst) {
        auto d = m_renderTargets.find(dst);
        if (d == m_renderTargets.end() || d->second.fbo == 0) return false;
        GLint bound = 0;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &bound);
        if (static_cast<GLuint>(bound) != m_defaultFbo) return false;   // only the frame (window or scaled scene) is "main"
        // glBlitFramebuffer honours the scissor test: a stale scissor rect
        // would copy part of the frame.
        const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
        if (scissor) glDisable(GL_SCISSOR_TEST);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, m_defaultFbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, d->second.fbo);
        glBlitFramebuffer(0, 0, d->second.width, d->second.height,
                          0, 0, d->second.width, d->second.height,
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, m_defaultFbo);
        if (scissor) glEnable(GL_SCISSOR_TEST);
        return true;
    }

    void GLBackend::SetUniformIVec2(ShaderHandle handle, const std::string& name, const glm::ivec2& value) {
        auto it = m_shaders.find(ResolveShader(handle));
        if (it == m_shaders.end()) return;
        GLint loc = it->second.GetUniform(name);
        if (loc != -1) glUniform2i(loc, value.x, value.y);
    }

    void GLBackend::SetUniformIVec3(ShaderHandle handle, const std::string& name, const glm::ivec3& value) {
        auto it = m_shaders.find(ResolveShader(handle));
        if (it == m_shaders.end()) return;
        GLint loc = it->second.GetUniform(name);
        if (loc != -1) glUniform3iv(loc, 1, glm::value_ptr(value));
    }

    // ========================================================================
    // MESHES (VAO wrapper)
    // ========================================================================

    MeshHandle GLBackend::CreateMesh(BufferHandle vertexBuffer, BufferHandle indexBuffer,
                                    const VertexLayout& layout) {
        auto vbIt = m_buffers.find(vertexBuffer);
        if (vbIt == m_buffers.end()) return INVALID_MESH;

        GLuint vao = 0;
        glGenVertexArrays(1, &vao);
        if (vao == 0) return INVALID_MESH;

        glBindVertexArray(vao);

        // Bind vertex buffer
        glBindBuffer(GL_ARRAY_BUFFER, vbIt->second.glId);

        // Setup vertex attributes
        for (const auto& attr : layout.attributes) {
            GLenum glType = GL_FLOAT;
            if (attr.type == AttribType::UByte) {
                glType = GL_UNSIGNED_BYTE;
            } else if (attr.type == AttribType::UShort) {
                glType = GL_UNSIGNED_SHORT;
            }
            glVertexAttribPointer(attr.location, attr.componentCount, glType,
                                attr.normalized ? GL_TRUE : GL_FALSE,
                                layout.stride, reinterpret_cast<void*>(static_cast<uintptr_t>(attr.offset)));
            glEnableVertexAttribArray(attr.location);
        }

        // Bind index buffer if provided. A handle that was asked for but does
        // not resolve is a FAILURE, not something to shrug off: the mesh would
        // come back looking usable and take DrawIndexed into the driver with no
        // index buffer bound.
        if (indexBuffer != INVALID_BUFFER) {
            auto ibIt = m_buffers.find(indexBuffer);
            if (ibIt == m_buffers.end()) {
                glBindVertexArray(0);
                glDeleteVertexArrays(1, &vao);
                Log::Error("[GLBackend] CreateMesh: index buffer %u does not exist", indexBuffer);
                return INVALID_MESH;
            }
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibIt->second.glId);
        }

        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);

        uint32_t handle = AllocHandle();
        m_meshes[handle] = {vao, vertexBuffer, indexBuffer};
        m_memStats.meshCount++;
        return handle;
    }

    void GLBackend::DestroyMesh(MeshHandle handle) {
        auto it = m_meshes.find(handle);
        if (it == m_meshes.end()) return;
        glDeleteVertexArrays(1, &it->second.vao);
        m_memStats.meshCount--;
        m_meshes.erase(it);
    }

    // ========================================================================
    // PIPELINE STATE (with render state caching)
    // ========================================================================

    void GLBackend::InvalidateStateCache() {
        m_stateInitialized = false;
    }

    void GLBackend::CopyFramebufferToTexture(TextureHandle dst) {
        auto it = m_textures.find(dst);
        if (it == m_textures.end()) return;
        // glCopyTexSubImage2D copies from the currently-bound READ
        // framebuffer (default = window) into the bound 2D texture.
        // No reformat — texture must be RGBA8 sized to (w, h).
        glBindTexture(GL_TEXTURE_2D, it->second.glId);
        glCopyTexSubImage2D(GL_TEXTURE_2D, 0,
                            /*xoffset*/ 0, /*yoffset*/ 0,
                            /*x*/       0, /*y*/       0,
                            it->second.width, it->second.height);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    bool GLBackend::CopyFramebufferDepthToTexture(TextureHandle dst) {
        if (m_depthCopyBroken) return false;
        auto it = m_textures.find(dst);
        if (it == m_textures.end() || it->second.internalFormat != GL_DEPTH24_STENCIL8) return false;
        // The default framebuffer only: its depth-stencil is D24S8 (the
        // window hints ask for 24 + 8), the texture's exact format, so the
        // copy is a straight transfer. An offscreen target (a shader pack's)
        // has its own formats and size.
        GLint readFbo = 0;
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFbo);
        if (static_cast<GLuint>(readFbo) != m_defaultFbo) return false;

        const bool check = !m_depthCopyChecked;
        if (check) {
            while (glGetError() != GL_NO_ERROR) {}   // only this copy's error counts
        }
        // With a DEPTH_STENCIL internal format glCopyTexSubImage2D reads the
        // read framebuffer's depth and stencil, not its colour.
        glBindTexture(GL_TEXTURE_2D, it->second.glId);
        glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, it->second.width, it->second.height);
        glBindTexture(GL_TEXTURE_2D, 0);
        if (check) {
            m_depthCopyChecked = true;
            const GLenum err = glGetError();
            if (err != GL_NO_ERROR) {
                m_depthCopyBroken = true;
                Log::Warning("GLBackend: depth framebuffer copy failed (0x%04X) — disabled for this session", err);
                return false;
            }
        }
        return true;
    }

    void GLBackend::UploadTextureRegionNow(TextureHandle handle, int level, int x, int y,
                                           int width, int height, const void* data) {
        auto it = m_textures.find(handle);
        if (it == m_textures.end() || !data) return;
        // The level's storage must already exist (glTexImage2D declared it —
        // CreateTexture2D for level 0, UploadTextureMipLevel for the rest).
        glBindTexture(GL_TEXTURE_2D, it->second.glId);
        glTexSubImage2D(GL_TEXTURE_2D, level, x, y, width, height,
                        it->second.dataFormat, it->second.dataType, data);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    void GLBackend::SetTextureAnisotropy(TextureHandle handle, float maxAnisotropy) {
        if (!GLAD_GL_EXT_texture_filter_anisotropic && !GLAD_GL_ARB_texture_filter_anisotropic) return;
        auto it = m_textures.find(handle);
        if (it == m_textures.end()) return;
        GLfloat deviceMax = 1.0f;
        glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &deviceMax);
        const float value = std::clamp(maxAnisotropy, 1.0f, std::max(1.0f, static_cast<float>(deviceMax)));
        glBindTexture(it->second.target, it->second.glId);
        glTexParameterf(it->second.target, GL_TEXTURE_MAX_ANISOTROPY_EXT, value);
        glBindTexture(it->second.target, 0);
    }

    bool GLBackend::RequestBackbufferReadback(int x, int y, int w, int h) {
        if (w <= 0 || h <= 0) return false;
        const size_t stride = static_cast<size_t>(w) * 4u;
        std::vector<uint8_t> rows(stride * static_cast<size_t>(h));
        // Whatever is being drawn to: the window, or a bound render target
        // (a shader pack's scene, whose leave capture reads the same way).
        GLint drawFbo = 0;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFbo);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(drawFbo));
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rows.data());
        // GL hands rows back bottom-up; the contract is top-down.
        Readback rb;
        rb.pixels.resize(rows.size());
        for (int r = 0; r < h; ++r) {
            std::memcpy(&rb.pixels[static_cast<size_t>(r) * stride],
                        &rows[static_cast<size_t>(h - 1 - r) * stride], stride);
        }
        rb.w = w;
        rb.h = h;
        m_readbacks.push_back(std::move(rb));
        return true;
    }

    bool GLBackend::TakeBackbufferReadback(std::vector<uint8_t>& outRgba, int& outW, int& outH) {
        if (m_readbacks.empty()) return false;
        Readback& rb = m_readbacks.front();
        outRgba.swap(rb.pixels);
        outW = rb.w;
        outH = rb.h;
        m_readbacks.pop_front();
        return true;
    }

    // ========================================================================
    // RENDER TARGETS (offscreen FBOs)
    // ========================================================================

    RenderTargetHandle GLBackend::CreateRenderTarget(const RenderTargetDesc& desc) {
        if (desc.width <= 0 || desc.height <= 0) return INVALID_RENDER_TARGET;

        GLRenderTargetInfo info;
        info.width       = desc.width;
        info.height      = desc.height;
        info.colorFormat = desc.colorFormat;
        info.depthFormat = desc.depthFormat;

        // 1. Color texture (sampleable). Reuse CreateTexture2D — handles
        //    HDR formats. nullptr data = uninitialized backing storage.
        info.colorTexture = CreateTexture2D(desc.width, desc.height,
                                            desc.colorFormat, nullptr);
        if (info.colorTexture == INVALID_TEXTURE) {
            return INVALID_RENDER_TARGET;
        }
        // Override the default nearest filter — RTs are typically
        // sampled with linear filtering by post-process shaders.
        SetTextureFilter(info.colorTexture, TextureFilter::Linear, TextureFilter::Linear);

        // 2. Depth+stencil renderbuffer, unless the target asked for none
        //    (its draws never depth-test). Cheap, not sampleable. (Use a
        //    depth texture instead if a future feature needs to sample
        //    the depth buffer.)
        info.hasDepth = desc.depth;
        if (info.hasDepth) {
            glGenRenderbuffers(1, &info.depthRBO);
            glBindRenderbuffer(GL_RENDERBUFFER, info.depthRBO);
            glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8,
                                  desc.width, desc.height);
            glBindRenderbuffer(GL_RENDERBUFFER, 0);
        }

        // 3. FBO with the color texture + depth RBO attached.
        glGenFramebuffers(1, &info.fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, info.fbo);

        auto colorIt = m_textures.find(info.colorTexture);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, colorIt->second.glId, 0);
        if (info.hasDepth) {
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                                      GL_RENDERBUFFER, info.depthRBO);
        }

        const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        glBindFramebuffer(GL_FRAMEBUFFER, m_defaultFbo);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            // Roll back.
            glDeleteFramebuffers(1, &info.fbo);
            if (info.depthRBO) glDeleteRenderbuffers(1, &info.depthRBO);
            DestroyTexture(info.colorTexture);
            return INVALID_RENDER_TARGET;
        }

        const uint32_t handle = AllocHandle();
        m_renderTargets[handle] = info;
        return handle;
    }

    void GLBackend::DestroyRenderTarget(RenderTargetHandle rt) {
        if (rt == INVALID_RENDER_TARGET) return;
        auto it = m_renderTargets.find(rt);
        if (it == m_renderTargets.end()) return;
        glDeleteFramebuffers(1, &it->second.fbo);
        if (it->second.ownsTextures) {
            if (it->second.depthRBO) glDeleteRenderbuffers(1, &it->second.depthRBO);
            if (it->second.colorTexture != INVALID_TEXTURE) DestroyTexture(it->second.colorTexture);
        }
        m_renderTargets.erase(it);
    }

    RenderTargetHandle GLBackend::CreateRenderTargetFromTextures(const TextureHandle* colors, int colorCount,
                                                                 TextureHandle depth) {
        if (colorCount < 0 || colorCount > 8 || (colorCount > 0 && !colors)) return INVALID_RENDER_TARGET;
        if (colorCount == 0 && depth == INVALID_TEXTURE) return INVALID_RENDER_TARGET;
        GLRenderTargetInfo info;
        info.ownsTextures = false;
        info.colorTexture = colorCount > 0 ? colors[0] : INVALID_TEXTURE;
        if (colorCount == 0) {
            auto dit = m_textures.find(depth);
            if (dit == m_textures.end()) return INVALID_RENDER_TARGET;
            info.width = dit->second.width;
            info.height = dit->second.height;
        }

        glGenFramebuffers(1, &info.fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, info.fbo);
        GLenum drawBuffers[8];
        for (int i = 0; i < colorCount; ++i) {
            auto it = m_textures.find(colors[i]);
            if (it == m_textures.end()) {
                glBindFramebuffer(GL_FRAMEBUFFER, m_defaultFbo);
                glDeleteFramebuffers(1, &info.fbo);
                return INVALID_RENDER_TARGET;
            }
            if (i == 0) { info.width = it->second.width; info.height = it->second.height; info.colorFormat = TextureFormat::RGBA8; }
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + static_cast<GLenum>(i),
                                   GL_TEXTURE_2D, it->second.glId, 0);
            drawBuffers[i] = GL_COLOR_ATTACHMENT0 + static_cast<GLenum>(i);
        }
        if (colorCount > 0) {
            glDrawBuffers(colorCount, drawBuffers);
        } else {
            glDrawBuffer(GL_NONE);   // depth-only: a snapshot target
            glReadBuffer(GL_NONE);
        }
        if (depth != INVALID_TEXTURE) {
            auto dit = m_textures.find(depth);
            if (dit != m_textures.end()) {
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, dit->second.glId, 0);
            }
        }
        const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        glBindFramebuffer(GL_FRAMEBUFFER, m_defaultFbo);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            Log::Error("GLBackend: wrapped render target incomplete (status 0x%x, %d colour attachment(s), depth %s)",
                       status, colorCount, depth != INVALID_TEXTURE ? "yes" : "no");
            glDeleteFramebuffers(1, &info.fbo);
            return INVALID_RENDER_TARGET;
        }
        const uint32_t handle = AllocHandle();
        m_renderTargets[handle] = info;
        return handle;
    }

    void GLBackend::BindRenderTarget(RenderTargetHandle rt) {
        if (rt == INVALID_RENDER_TARGET) {
            // Bind default backbuffer.
            glBindFramebuffer(GL_FRAMEBUFFER, m_defaultFbo);
            return;
        }
        auto it = m_renderTargets.find(rt);
        if (it == m_renderTargets.end()) return;
        glBindFramebuffer(GL_FRAMEBUFFER, it->second.fbo);
        glViewport(0, 0, it->second.width, it->second.height);
    }

    TextureHandle GLBackend::GetRenderTargetColorTexture(RenderTargetHandle rt) const {
        auto it = m_renderTargets.find(rt);
        if (it == m_renderTargets.end()) return INVALID_TEXTURE;
        return it->second.colorTexture;
    }


    void GLBackend::ResizeRenderTarget(RenderTargetHandle rt, int w, int h) {
        if (rt == INVALID_RENDER_TARGET || w <= 0 || h <= 0) return;
        auto it = m_renderTargets.find(rt);
        if (it == m_renderTargets.end()) return;
        if (it->second.width == w && it->second.height == h) return;

        // Destroy + recreate the color texture and depth RBO at the
        // new size; reuse the FBO id.
        DestroyTexture(it->second.colorTexture);
        if (it->second.depthRBO) glDeleteRenderbuffers(1, &it->second.depthRBO);
        it->second.depthRBO = 0;

        it->second.colorTexture = CreateTexture2D(w, h, it->second.colorFormat, nullptr);
        SetTextureFilter(it->second.colorTexture,
                         TextureFilter::Linear, TextureFilter::Linear);

        if (it->second.hasDepth) {
            glGenRenderbuffers(1, &it->second.depthRBO);
            glBindRenderbuffer(GL_RENDERBUFFER, it->second.depthRBO);
            glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
            glBindRenderbuffer(GL_RENDERBUFFER, 0);
        }

        glBindFramebuffer(GL_FRAMEBUFFER, it->second.fbo);
        auto colorIt = m_textures.find(it->second.colorTexture);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, colorIt->second.glId, 0);
        if (it->second.hasDepth) {
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                                      GL_RENDERBUFFER, it->second.depthRBO);
        }
        glBindFramebuffer(GL_FRAMEBUFFER, m_defaultFbo);

        it->second.width  = w;
        it->second.height = h;
    }

    void GLBackend::SetStencilOverride(bool enabled,
                                       CompareOp compareOp,
                                       StencilOp passOp,
                                       uint32_t  reference,
                                       uint32_t  readMask,
                                       uint32_t  writeMask) {
        m_stencilOverride = {enabled, compareOp, passOp, reference, readMask, writeMask};
        // Force the next SetPipelineState call to re-apply EVERYTHING.
        // Without this, callers that re-set the same PipelineState would
        // skip the stencil branch via the dirty-tracking optimisation and
        // the override would silently fail to take effect.
        m_stateInitialized = false;
    }

    void GLBackend::SetPipelineState(const PipelineState& state_) {
        // If a stencil override is active, splice its values into the state
        // BEFORE the dirty-tracking compare. The fail / depthFail ops stay
        // Keep — the portal renderer only ever wants to mark or compare,
        // not modify-on-fail — and the override always enables stencil
        // testing.
        PipelineState state = state_;
        m_requestedState = state_;
        if (m_stencilOverride.enabled) {
            state.stencilTestEnabled = true;
            state.stencilCompareOp   = m_stencilOverride.compareOp;
            state.stencilFailOp      = StencilOp::Keep;
            state.stencilDepthFailOp = StencilOp::Keep;
            state.stencilPassOp      = m_stencilOverride.passOp;
            state.stencilReference   = m_stencilOverride.reference;
            state.stencilReadMask    = m_stencilOverride.readMask;
            state.stencilWriteMask   = m_stencilOverride.writeMask;
        }
        // A mirrored view flips every triangle's screen winding. Inverting
        // by flipping the FRONT-FACE RULE (not by swapping the cull mode)
        // culls the same triangles and, unlike the swap, keeps
        // gl_FrontFacing meaning the geometric front — the two-sided plant
        // quads (TerrainVertex::kTwoSidedFlag) mirror their texture on the
        // geometric back and must see the same answer in a mirror.
        if (m_cullInvert) {
            state.frontFace = (state.frontFace == FrontFace::CounterClockwise)
                                  ? FrontFace::Clockwise : FrontFace::CounterClockwise;
        }
        // Improved Transparency (MC 26.3 OIT): inside a stage every draw is
        // MC's OIT snippet — depth tested, not written (the clouds' depth
        // bounds excepted), blended One/One (the equation — MAX for the
        // depth bounds, ADD otherwise — is SetOitStage's). No stage: the
        // caller's state, untouched.
        if (m_oitStage != OitStage::None) {
            state.depthTestEnabled  = true;
            state.depthCompareOp    = CompareOp::LessEqual;
            state.depthWriteEnabled = m_oitStage == OitStage::DepthBounds && m_oitDbWritesDepth;
            state.blendEnabled      = true;
            state.srcBlendFactor    = BlendFactor::One;
            state.dstBlendFactor    = BlendFactor::One;
            state.colorWriteEnabled = true;
        }

        // Depth test
        if (!m_stateInitialized || state.depthTestEnabled != m_currentState.depthTestEnabled) {
            if (state.depthTestEnabled) {
                glEnable(GL_DEPTH_TEST);
            } else {
                glDisable(GL_DEPTH_TEST);
            }
        }

        if (!m_stateInitialized || state.depthCompareOp != m_currentState.depthCompareOp) {
            glDepthFunc(ToGLCompareOp(state.depthCompareOp));
        }

        if (!m_stateInitialized || state.depthWriteEnabled != m_currentState.depthWriteEnabled) {
            glDepthMask(state.depthWriteEnabled ? GL_TRUE : GL_FALSE);
        }

        // Blending
        if (!m_stateInitialized || state.blendEnabled != m_currentState.blendEnabled) {
            if (state.blendEnabled) {
                glEnable(GL_BLEND);
            } else {
                glDisable(GL_BLEND);
            }
        }

        if (state.blendEnabled &&
            (!m_stateInitialized ||
             state.blendEnabled != m_currentState.blendEnabled ||
             state.srcBlendFactor != m_currentState.srcBlendFactor ||
             state.dstBlendFactor != m_currentState.dstBlendFactor)) {
            glBlendFunc(ToGLBlendFactor(state.srcBlendFactor),
                       ToGLBlendFactor(state.dstBlendFactor));
        }

        // Culling
        if (!m_stateInitialized || state.cullMode != m_currentState.cullMode) {
            if (state.cullMode == CullMode::None) {
                glDisable(GL_CULL_FACE);
            } else {
                glEnable(GL_CULL_FACE);
                glCullFace(state.cullMode == CullMode::Front ? GL_FRONT : GL_BACK);
            }
        }

        if (!m_stateInitialized || state.frontFace != m_currentState.frontFace) {
            glFrontFace(state.frontFace == FrontFace::CounterClockwise ? GL_CCW : GL_CW);
        }

        // Polygon mode
        if (!m_stateInitialized || state.polygonMode != m_currentState.polygonMode) {
            glPolygonMode(GL_FRONT_AND_BACK,
                         state.polygonMode == PolygonMode::Fill ? GL_FILL : GL_LINE);
        }

        // Line width
        if (!m_stateInitialized || state.lineWidth != m_currentState.lineWidth) {
            glLineWidth(state.lineWidth);
        }

        // Color write mask — all 4 channels on or all off. The portal
        // renderer (Phase 6) toggles this when stencil-marking and
        // depth-refilling so those sub-passes don't disturb framebuffer
        // colors that the prior scene draw established.
        if (!m_stateInitialized || state.colorWriteEnabled != m_currentState.colorWriteEnabled) {
            const GLboolean m = state.colorWriteEnabled ? GL_TRUE : GL_FALSE;
            glColorMask(m, m, m, m);
        }

        // Polygon offset (depth bias). Used by the portal renderer to push
        // the portal mesh's depth toward the camera so it reliably wins
        // z-fighting against the wall block it's stuck to (the portal sits
        // ~1 mm in front of the wall surface, which collapses to z-fighting
        // at grazing angles — visible to the user as "the wall shows
        // through the portal at low viewing angles"). Negative units pull
        // depth toward the near plane.
        if (!m_stateInitialized || state.depthBiasEnabled != m_currentState.depthBiasEnabled) {
            if (state.depthBiasEnabled) glEnable(GL_POLYGON_OFFSET_FILL);
            else                        glDisable(GL_POLYGON_OFFSET_FILL);
        }
        if (state.depthBiasEnabled &&
            (!m_stateInitialized
             || state.depthBiasConstant != m_currentState.depthBiasConstant
             || state.depthBiasSlope    != m_currentState.depthBiasSlope)) {
            glPolygonOffset(state.depthBiasSlope, state.depthBiasConstant);
        }

        // Depth clamp (see PipelineState::depthClampEnabled).
        if (!m_stateInitialized || state.depthClampEnabled != m_currentState.depthClampEnabled) {
            if (state.depthClampEnabled) glEnable(GL_DEPTH_CLAMP);
            else                         glDisable(GL_DEPTH_CLAMP);
        }

        // Stencil — only do work when the test flips on/off OR the
        // reference / ops / masks change while it's enabled. Same shape as
        // the depth/blend branches above.
        if (!m_stateInitialized || state.stencilTestEnabled != m_currentState.stencilTestEnabled) {
            if (state.stencilTestEnabled) glEnable(GL_STENCIL_TEST);
            else                          glDisable(GL_STENCIL_TEST);
        }
        if (state.stencilTestEnabled) {
            const bool funcChanged =
                !m_stateInitialized
                || state.stencilCompareOp != m_currentState.stencilCompareOp
                || state.stencilReference != m_currentState.stencilReference
                || state.stencilReadMask  != m_currentState.stencilReadMask;
            if (funcChanged) {
                glStencilFunc(ToGLCompareOp(state.stencilCompareOp),
                              static_cast<GLint>(state.stencilReference),
                              state.stencilReadMask);
            }
            const bool opChanged =
                !m_stateInitialized
                || state.stencilFailOp      != m_currentState.stencilFailOp
                || state.stencilDepthFailOp != m_currentState.stencilDepthFailOp
                || state.stencilPassOp      != m_currentState.stencilPassOp;
            if (opChanged) {
                glStencilOp(ToGLStencilOp(state.stencilFailOp),
                            ToGLStencilOp(state.stencilDepthFailOp),
                            ToGLStencilOp(state.stencilPassOp));
            }
            if (!m_stateInitialized || state.stencilWriteMask != m_currentState.stencilWriteMask) {
                glStencilMask(state.stencilWriteMask);
            }
        }

        m_currentState = state;
        m_stateInitialized = true;
    }

    // ========================================================================
    // DRAWING
    // ========================================================================

    static GLenum ToGLPrimitive(PrimitiveType type) {
        switch (type) {
            case PrimitiveType::Lines:         return GL_LINES;
            case PrimitiveType::LineStrip:     return GL_LINE_STRIP;
            case PrimitiveType::TriangleStrip: return GL_TRIANGLE_STRIP;
            case PrimitiveType::Triangles:
            default:                           return GL_TRIANGLES;
        }
    }

    void GLBackend::DrawIndexed(MeshHandle mesh, uint32_t indexCount, uint32_t indexOffset) {
        if (m_oitSkipDraw) return;   // an OIT stage, and the shader has no variant
        auto it = m_meshes.find(mesh);
        if (it == m_meshes.end()) return;

        // With no ELEMENT_ARRAY_BUFFER in the VAO, glDrawElements treats the
        // offset as a CLIENT pointer and dereferences address 0 inside the
        // driver — an access violation in nvoglv64/atio6axx, not a GL error we
        // could ever see. A zero count is merely pointless. Neither is worth a
        // draw, and both are cheap to rule out here.
        if (indexCount == 0 || it->second.indexBuffer == INVALID_BUFFER) return;

        glBindVertexArray(it->second.vao);
        glDrawElements(ToGLPrimitive(m_currentState.primitiveType), indexCount, GL_UNSIGNED_INT,
                      reinterpret_cast<void*>(static_cast<uintptr_t>(indexOffset * sizeof(uint32_t))));
        // VAO intentionally left bound — next DrawIndexed will rebind, avoiding redundant unbind/rebind cycles
    }

    MeshHandle GLBackend::CreateInstancedMesh(BufferHandle vertexBuffer,
                                              BufferHandle indexBuffer,
                                              BufferHandle instanceBuffer,
                                              const VertexLayout& vertexLayout,
                                              const VertexLayout& instanceLayout) {
        auto vbIt = m_buffers.find(vertexBuffer);
        auto ibufIt = m_buffers.find(instanceBuffer);
        if (vbIt == m_buffers.end() || ibufIt == m_buffers.end()) return INVALID_MESH;

        GLuint vao = 0;
        glGenVertexArrays(1, &vao);
        if (vao == 0) return INVALID_MESH;
        glBindVertexArray(vao);

        // Two ARRAY_BUFFER bindings, one after the other. This works because
        // glVertexAttribPointer captures whatever is bound to ARRAY_BUFFER *at
        // the moment it is called* into the VAO's per-attribute state — the
        // binding itself is not VAO state in GL 3.3, the captured source is.
        const auto bindAttribs = [](GLuint bufferId, const VertexLayout& layout) {
            glBindBuffer(GL_ARRAY_BUFFER, bufferId);
            for (const auto& attr : layout.attributes) {
                GLenum glType = (attr.type == AttribType::UByte)  ? GL_UNSIGNED_BYTE
                              : (attr.type == AttribType::UShort) ? GL_UNSIGNED_SHORT
                                                                  : GL_FLOAT;
                glVertexAttribPointer(attr.location, attr.componentCount, glType,
                                      attr.normalized ? GL_TRUE : GL_FALSE,
                                      layout.stride,
                                      reinterpret_cast<void*>(
                                          static_cast<uintptr_t>(attr.offset)));
                glEnableVertexAttribArray(attr.location);
                // Zero is the GL default, but set it explicitly: a VAO is fresh
                // here, yet leaving the per-vertex case implicit makes the two
                // layouts read as if only one of them had a divisor rule.
                glVertexAttribDivisor(attr.location, attr.instanceDivisor);
            }
        };

        bindAttribs(vbIt->second.glId, vertexLayout);
        bindAttribs(ibufIt->second.glId, instanceLayout);

        if (indexBuffer != INVALID_BUFFER) {
            auto ibIt = m_buffers.find(indexBuffer);
            if (ibIt != m_buffers.end()) {
                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibIt->second.glId);
            }
        }

        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);

        uint32_t handle = AllocHandle();
        m_meshes[handle] = {vao, vertexBuffer, indexBuffer};
        {
            auto& gm = m_meshes[handle];
            gm.instanceBuffer = instanceBuffer;
            gm.instanceLayout = instanceLayout;
        }
        m_memStats.meshCount++;
        return handle;
    }

    void GLBackend::DrawIndexedInstanced(MeshHandle mesh, uint32_t indexCount,
                                         uint32_t indexOffset, uint32_t instanceCount,
                                         uint32_t instanceByteOffset) {
        if (instanceCount == 0 || indexCount == 0 || m_oitSkipDraw) return;
        auto it = m_meshes.find(mesh);
        if (it == m_meshes.end()) return;

        glBindVertexArray(it->second.vao);

        // Re-point the instance attributes when this group's data starts at
        // a different offset than the pointers currently baked into the VAO.
        // Attribute pointers are VAO state, so this persists until changed.
        if (instanceByteOffset != it->second.lastInstanceOffset &&
            it->second.instanceBuffer != INVALID_BUFFER) {
            auto bufIt = m_buffers.find(it->second.instanceBuffer);
            if (bufIt != m_buffers.end()) {
                glBindBuffer(GL_ARRAY_BUFFER, bufIt->second.glId);
                for (const auto& attr : it->second.instanceLayout.attributes) {
                    GLenum glType = (attr.type == AttribType::UByte)  ? GL_UNSIGNED_BYTE
                              : (attr.type == AttribType::UShort) ? GL_UNSIGNED_SHORT
                                                                  : GL_FLOAT;
                    glVertexAttribPointer(attr.location, attr.componentCount, glType,
                                          attr.normalized ? GL_TRUE : GL_FALSE,
                                          it->second.instanceLayout.stride,
                                          reinterpret_cast<void*>(static_cast<uintptr_t>(
                                              instanceByteOffset + attr.offset)));
                }
                glBindBuffer(GL_ARRAY_BUFFER, 0);
                it->second.lastInstanceOffset = instanceByteOffset;
            }
        }

        glDrawElementsInstanced(
            ToGLPrimitive(m_currentState.primitiveType), indexCount, GL_UNSIGNED_INT,
            reinterpret_cast<void*>(static_cast<uintptr_t>(indexOffset * sizeof(uint32_t))),
            instanceCount);
    }

    void GLBackend::DrawArrays(MeshHandle mesh, uint32_t vertexCount, uint32_t firstVertex) {
        if (m_oitSkipDraw) return;
        auto it = m_meshes.find(mesh);
        if (it == m_meshes.end()) return;

        glBindVertexArray(it->second.vao);
        glDrawArrays(ToGLPrimitive(m_currentState.primitiveType), firstVertex, vertexCount);
    }

    void GLBackend::UnbindMesh() {
        glBindVertexArray(0);
    }

    // ========================================================================
    // MEGA-BUFFER RENDERING
    // ========================================================================

    void GLBackend::BindVertexBuffer(BufferHandle vbo, uint32_t stride) {
        auto it = m_buffers.find(vbo);
        if (it == m_buffers.end()) return;

        if (m_hasVertexAttribBinding) {
            glBindVertexBuffer(0, it->second.glId, 0, static_cast<GLsizei>(stride));
        } else {
            // Packed 20-byte TerrainVertex — keep in step with
            // SetupBlockVertexFormat and GetTerrainVertexLayout.
            glBindBuffer(GL_ARRAY_BUFFER, it->second.glId);
            glVertexAttribPointer(0, 4, GL_UNSIGNED_SHORT, GL_TRUE,
                                  static_cast<GLsizei>(stride),
                                  reinterpret_cast<void*>(static_cast<uintptr_t>(0)));
            glVertexAttribPointer(1, 2, GL_UNSIGNED_SHORT, GL_TRUE,
                                  static_cast<GLsizei>(stride),
                                  reinterpret_cast<void*>(static_cast<uintptr_t>(8)));
            glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE,
                                  static_cast<GLsizei>(stride),
                                  reinterpret_cast<void*>(static_cast<uintptr_t>(12)));
            glVertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, GL_TRUE,
                                  static_cast<GLsizei>(stride),
                                  reinterpret_cast<void*>(static_cast<uintptr_t>(16)));
        }
    }

    void GLBackend::BindUniformBuffer(BufferHandle handle, size_t offset, size_t size) {
        auto it = m_buffers.find(handle);
        if (it == m_buffers.end()) return;
        // Binding point 0 — every program's "SectionOrigins" block is wired to
        // it at link (CreateShader). glBindBufferRange offsets must be
        // GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT-aligned; callers pass multiples
        // of 16 KB.
        glBindBufferRange(GL_UNIFORM_BUFFER, 0, it->second.glId,
                          static_cast<GLintptr>(offset), static_cast<GLsizeiptr>(size));
    }

    void GLBackend::BindIndexBuffer(BufferHandle ibo) {
        auto it = m_buffers.find(ibo);
        if (it == m_buffers.end()) return;
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, it->second.glId);
    }

    void GLBackend::DrawIndexedBaseVertex(uint32_t indexCount, size_t indexByteOffset, int32_t baseVertex,
                                          IndexType indexType) {
        if (m_oitSkipDraw) return;
        glDrawElementsBaseVertex(
            GL_TRIANGLES,
            static_cast<GLsizei>(indexCount),
            indexType == IndexType::Uint16 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT,
            reinterpret_cast<const void*>(indexByteOffset),
            static_cast<GLint>(baseVertex));
    }

    void GLBackend::MultiDrawIndexedBaseVertex(const int32_t* indexCounts,
                                                const size_t* indexByteOffsets,
                                                const int32_t* baseVertices,
                                                uint32_t drawCount,
                                                IndexType indexType) {
        if (drawCount == 0 || m_oitSkipDraw) return;
        const GLenum glIndexType =
            indexType == IndexType::Uint16 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT;
        // Convert size_t byte offsets to const void* for GL. Persistent scratch:
        // a slab of a wide view is thousands of sub-draws, and this used to
        // heap-allocate a fresh vector for every one of those calls, every
        // frame. GL is single-threaded by contract, so a function-local static
        // is safe; it grows to the largest batch ever issued and stays there.
        static std::vector<const void*> s_offsetScratch;
        if (s_offsetScratch.size() < drawCount) s_offsetScratch.resize(drawCount);
        for (uint32_t i = 0; i < drawCount; i++)
            s_offsetScratch[i] = reinterpret_cast<const void*>(indexByteOffsets[i]);
        glMultiDrawElementsBaseVertex(
            GL_TRIANGLES,
            reinterpret_cast<const GLsizei*>(indexCounts),
            glIndexType,
            s_offsetScratch.data(),
            static_cast<GLsizei>(drawCount),
            const_cast<GLint*>(reinterpret_cast<const GLint*>(baseVertices)));
    }

    // ========================================================================
    // SHARED BLOCK VERTEX FORMAT
    // ========================================================================

    void GLBackend::SetupBlockVertexFormat() {
        m_hasVertexAttribBinding = (GLAD_GL_ARB_vertex_attrib_binding != 0);

        glGenVertexArrays(1, &m_sharedBlockVAO);
        glBindVertexArray(m_sharedBlockVAO);

        if (m_hasVertexAttribBinding) {
            // Vertex format decoupled from buffer binding.
            // VBO switching uses glBindVertexBuffer — cheapest possible path.
            //
            // This shared VAO serves ONLY the chunk-terrain mega-buffers (bound
            // via BindBlockVertexFormat from ClientMeshManager), so it carries
            // the packed 20-byte TERRAIN format (Render::TerrainVertex /
            // GetTerrainVertexLayout): px py pz slot as 4 unorm16, u v as 2
            // unorm16, rgba8 colour, rgba8 light. All normalized — the shader
            // recovers the integers (value * 65535 / * 255), so no I-format
            // attributes are needed.
            glVertexAttribFormat(0, 4, GL_UNSIGNED_SHORT, GL_TRUE, 0);
            glVertexAttribBinding(0, 0);
            glVertexAttribFormat(1, 2, GL_UNSIGNED_SHORT, GL_TRUE, 8);
            glVertexAttribBinding(1, 0);
            glVertexAttribFormat(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, 12);
            glVertexAttribBinding(2, 0);
            glVertexAttribFormat(3, 4, GL_UNSIGNED_BYTE, GL_TRUE, 16);
            glVertexAttribBinding(3, 0);
        }

        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);
        glEnableVertexAttribArray(2);
        glEnableVertexAttribArray(3);

        glBindVertexArray(0);
    }

    void GLBackend::BindBlockVertexFormat() {
        glBindVertexArray(m_sharedBlockVAO);
    }

    void GLBackend::DestroyBlockVertexFormat() {
        if (m_sharedBlockVAO) {
            glDeleteVertexArrays(1, &m_sharedBlockVAO);
            m_sharedBlockVAO = 0;
        }
    }

    // ========================================================================
    // GPU TIMER QUERIES
    // ========================================================================

    GPUTimerHandle GLBackend::BeginGPUTimer(const std::string& name) {
        GLuint queryId = 0;
        glGenQueries(1, &queryId);
        if (queryId == 0) return INVALID_GPU_TIMER;

        glBeginQuery(GL_TIME_ELAPSED, queryId);

        uint32_t handle = AllocHandle();
        m_timers[handle] = {queryId, true, false, 0.0f};
        return handle;
    }

    void GLBackend::EndGPUTimer(GPUTimerHandle handle) {
        auto it = m_timers.find(handle);
        if (it == m_timers.end() || !it->second.active) return;

        glEndQuery(GL_TIME_ELAPSED);
        it->second.active = false;
    }

    float GLBackend::GetGPUTimerResultMs(GPUTimerHandle handle) {
        auto it = m_timers.find(handle);
        if (it == m_timers.end()) return -1.0f;

        if (!it->second.resultReady) {
            GLint available = 0;
            glGetQueryObjectiv(it->second.queryId, GL_QUERY_RESULT_AVAILABLE, &available);
            if (!available) {
                // Result not on the GPU yet — keep the query alive so the
                // caller can poll again next frame (never blocks the pipeline).
                return -1.0f;
            }
            GLuint64 timeNs = 0;
            glGetQueryObjectui64v(it->second.queryId, GL_QUERY_RESULT, &timeNs);
            it->second.resultMs = static_cast<float>(timeNs) / 1000000.0f;
            it->second.resultReady = true;
        }

        float result = it->second.resultMs;

        // Clean up
        glDeleteQueries(1, &it->second.queryId);
        m_timers.erase(it);

        return result;
    }

    // ========================================================================
    // MEMORY STATS
    // ========================================================================

    GPUMemoryStats GLBackend::GetMemoryStats() const {
        return m_memStats;
    }

    // ========================================================================
    // IMGUI
    // ========================================================================

    void GLBackend::ImGuiInit(GLFWwindow* window) {
        ImGui_ImplGlfw_InitForOpenGL(window, true);
        ImGui_ImplOpenGL3_Init("#version 330 core");
    }

    void GLBackend::ImGuiNewFrame() {
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
    }

    void GLBackend::ImGuiRender() {
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    }

    void GLBackend::ImGuiShutdown() {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
    }

    // ========================================================================
    // HELPERS
    // ========================================================================

    GLenum GLBackend::ToGLBufferTarget(BufferUsage usage) const {
        switch (usage) {
            case BufferUsage::Vertex:  return GL_ARRAY_BUFFER;
            case BufferUsage::Index:   return GL_ELEMENT_ARRAY_BUFFER;
            case BufferUsage::Uniform: return GL_UNIFORM_BUFFER;
            case BufferUsage::Staging: return GL_COPY_READ_BUFFER;
        }
        return GL_ARRAY_BUFFER;
    }

    GLenum GLBackend::ToGLBufferUsage(BufferAccess access) const {
        switch (access) {
            case BufferAccess::Static:    return GL_STATIC_DRAW;
            case BufferAccess::Dynamic:   return GL_DYNAMIC_DRAW;
            case BufferAccess::Streaming: return GL_STREAM_DRAW;
        }
        return GL_STATIC_DRAW;
    }

    GLenum GLBackend::ToGLFilter(TextureFilter filter) const {
        switch (filter) {
            case TextureFilter::Nearest:              return GL_NEAREST;
            case TextureFilter::Linear:               return GL_LINEAR;
            case TextureFilter::NearestMipmapLinear:   return GL_NEAREST_MIPMAP_LINEAR;
            case TextureFilter::NearestMipmapNearest:  return GL_NEAREST_MIPMAP_NEAREST;
            case TextureFilter::LinearMipmapLinear:    return GL_LINEAR_MIPMAP_LINEAR;
            case TextureFilter::LinearMipmapNearest:   return GL_LINEAR_MIPMAP_NEAREST;
        }
        return GL_NEAREST;
    }

    GLenum GLBackend::ToGLWrap(TextureWrap wrap) const {
        switch (wrap) {
            case TextureWrap::Repeat:         return GL_REPEAT;
            case TextureWrap::ClampToEdge:    return GL_CLAMP_TO_EDGE;
            case TextureWrap::MirroredRepeat: return GL_MIRRORED_REPEAT;
        }
        return GL_CLAMP_TO_EDGE;
    }

    GLenum GLBackend::ToGLBlendFactor(BlendFactor factor) const {
        switch (factor) {
            case BlendFactor::Zero:              return GL_ZERO;
            case BlendFactor::One:               return GL_ONE;
            case BlendFactor::SrcColor:          return GL_SRC_COLOR;
            case BlendFactor::OneMinusSrcColor:  return GL_ONE_MINUS_SRC_COLOR;
            case BlendFactor::DstColor:          return GL_DST_COLOR;
            case BlendFactor::OneMinusDstColor:  return GL_ONE_MINUS_DST_COLOR;
            case BlendFactor::SrcAlpha:          return GL_SRC_ALPHA;
            case BlendFactor::OneMinusSrcAlpha:  return GL_ONE_MINUS_SRC_ALPHA;
            case BlendFactor::DstAlpha:          return GL_DST_ALPHA;
            case BlendFactor::OneMinusDstAlpha:  return GL_ONE_MINUS_DST_ALPHA;
        }
        return GL_ONE;
    }

    GLenum GLBackend::ToGLCompareOp(CompareOp op) const {
        switch (op) {
            case CompareOp::Never:        return GL_NEVER;
            case CompareOp::Less:         return GL_LESS;
            case CompareOp::Equal:        return GL_EQUAL;
            case CompareOp::LessEqual:    return GL_LEQUAL;
            case CompareOp::Greater:      return GL_GREATER;
            case CompareOp::NotEqual:     return GL_NOTEQUAL;
            case CompareOp::GreaterEqual: return GL_GEQUAL;
            case CompareOp::Always:       return GL_ALWAYS;
        }
        return GL_LEQUAL;
    }

    GLenum GLBackend::ToGLStencilOp(StencilOp op) const {
        switch (op) {
            case StencilOp::Keep:      return GL_KEEP;
            case StencilOp::Zero:      return GL_ZERO;
            case StencilOp::Replace:   return GL_REPLACE;
            case StencilOp::IncrClamp: return GL_INCR;
            case StencilOp::DecrClamp: return GL_DECR;
            case StencilOp::Invert:    return GL_INVERT;
            case StencilOp::IncrWrap:  return GL_INCR_WRAP;
            case StencilOp::DecrWrap:  return GL_DECR_WRAP;
        }
        return GL_KEEP;
    }

    // ========================================================================
    // IMPROVED TRANSPARENCY — MC 26.3 WAVELET OIT
    // ========================================================================

    ShaderHandle GLBackend::ResolveOit(ShaderHandle handle) const {
        auto it = m_oitVariants.find(handle);
        if (it == m_oitVariants.end()) return handle;
        const ShaderHandle v = it->second.shader[static_cast<size_t>(m_oitStage)];
        return v != INVALID_SHADER ? v : handle;
    }

    ShaderHandle GLBackend::OitVariantFor(ShaderHandle engine) {
        const size_t stage = static_cast<size_t>(m_oitStage);
        OitVariants& v = m_oitVariants[engine];
        if (v.tried[stage]) return v.shader[stage];
        v.tried[stage] = true;
        auto it = m_shaders.find(engine);
        if (it == m_shaders.end()) return INVALID_SHADER;

        // A shader takes part only if its fragment source says where MC's
        // oit.glsl goes (under #ifdef OIT, so the engine's own compile never
        // sees it).
        // Copies: CreateShader below inserts into m_shaders.
        const std::string vert = it->second.vertexSource;
        std::string frag = it->second.fragmentSource;
        if (!SpliceOitLibrary(frag)) return INVALID_SHADER;
        // MC's pipeline defines, right after #version.
        const char* stageDefines =
            m_oitStage == OitStage::DepthBounds   ? "#define OIT\n#define OIT_ALPHA_ONLY\n#define OIT_DEPTH_BOUNDS\n" :
            m_oitStage == OitStage::Transmittance ? "#define OIT\n#define OIT_ALPHA_ONLY\n#define OIT_TRANSMITTANCE\n" :
                                                    "#define OIT\n#define OIT_ACCUMULATE\n";
        const size_t version = frag.find("#version");
        const size_t lineEnd = version == std::string::npos ? std::string::npos : frag.find('\n', version);
        if (lineEnd == std::string::npos) return INVALID_SHADER;
        frag.insert(lineEnd + 1, stageDefines);

        const ShaderHandle variant = CreateShader(vert, frag);
        if (variant == INVALID_SHADER) {
            Log::Error("GLBackend: OIT variant (stage %zu) of shader %u failed to compile", stage, engine);
            return INVALID_SHADER;
        }
        AssignOitSamplerUnits(variant);
        v.shader[stage] = variant;
        return variant;
    }

    bool GLBackend::SpliceOitLibrary(std::string& fragmentSource) {
        static const std::string kMarker = "#pragma oit_library";
        const size_t marker = fragmentSource.find(kMarker);
        if (marker == std::string::npos) return false;
        if (m_oitLibrary.empty()) m_oitLibrary = ReadFileContents("shaders/oit_lib.glsl");
        if (m_oitLibrary.empty()) {
            Log::Error("GLBackend: shaders/oit_lib.glsl missing - Improved Transparency unavailable");
            return false;
        }
        fragmentSource.replace(marker, kMarker.size(), m_oitLibrary);
        return true;
    }

    void GLBackend::AssignOitSamplerUnits(ShaderHandle shader) {
        // MC's DepthBoundsSampler / Coeff0 / Coeff1 on their fixed units
        // (SetOitStage binds them there); Sampler0 — the composite's and the
        // helper passes' own input — on unit 0.
        auto it = m_shaders.find(shader);
        if (it == m_shaders.end()) return;
        GLint current = 0;
        glGetIntegerv(GL_CURRENT_PROGRAM, &current);
        glUseProgram(it->second.programId);
        const GLint db = it->second.GetUniform("DepthBoundsSampler");
        const GLint c0 = it->second.GetUniform("Coeff0");
        const GLint c1 = it->second.GetUniform("Coeff1");
        const GLint s0 = it->second.GetUniform("Sampler0");
        if (db != -1) glUniform1i(db, kOitUnitDepthBounds);
        if (c0 != -1) glUniform1i(c0, kOitUnitCoeff0);
        if (c1 != -1) glUniform1i(c1, kOitUnitCoeff1);
        if (s0 != -1) glUniform1i(s0, 0);
        glUseProgram(static_cast<GLuint>(current));
    }

    ShaderHandle GLBackend::CreateOitShaderFromFiles(const std::string& vertexPath,
                                                     const std::string& fragmentPath) {
        const std::string vert = ReadFileContents(vertexPath);
        std::string frag = ReadFileContents(fragmentPath);
        if (vert.empty() || frag.empty()) return INVALID_SHADER;
        // Only the composite and the cull include the library; the blits
        // have no marker and compile as they are.
        if (frag.find("#pragma oit_library") != std::string::npos && !SpliceOitLibrary(frag)) {
            return INVALID_SHADER;
        }
        const ShaderHandle shader = CreateShader(vert, frag);
        if (shader != INVALID_SHADER) AssignOitSamplerUnits(shader);
        return shader;
    }

    void GLBackend::SetOitStage(OitStage stage, const glm::vec4& projParams, bool depthBoundsWriteDepth) {
        m_oitStage = stage;
        m_oitProjParams = projParams;
        m_oitDbWritesDepth = depthBoundsWriteDepth;
        m_oitSkipDraw = false;
        m_boundShader = INVALID_SHADER;   // the next BindShader resolves the variant
        // MC's OIT colour targets: MAX into the depth bounds, ADD elsewhere.
        glBlendEquation(stage == OitStage::DepthBounds ? GL_MAX : GL_FUNC_ADD);
        if (stage != OitStage::None) {
            // The stage's samplers: the depth bounds (the original while the
            // depth bounds are being drawn — the cull reads it — the culled
            // ones after), and the coefficients for the accumulation.
            m_oitSamplerStage = stage;
            const TextureHandle bounds = stage == OitStage::DepthBounds ? m_oit.depthBounds : m_oit.culled;
            const bool acc = stage == OitStage::Accumulate;
            BindTexture(bounds, kOitUnitDepthBounds);
            BindTexture(acc ? m_oit.coeff0 : m_oit.dummy, kOitUnitCoeff0);
            BindTexture(acc ? m_oit.coeff1 : m_oit.dummy, kOitUnitCoeff1);
            glActiveTexture(GL_TEXTURE0);   // a texture made mid-stage must not unbind a stage sampler
        }
        if (m_stateInitialized) SetPipelineState(m_requestedState);   // re-splice for the new stage
    }

    bool GLBackend::OitEnsureTargets(int width, int height) {
        if (width <= 0 || height <= 0) return false;
        if (m_oit.depthBounds != INVALID_TEXTURE && m_oit.width == width && m_oit.height == height) return true;
        OitDestroyTargets();
        OitTargets& t = m_oit;
        t.width = width;
        t.height = height;
        t.depthBounds = CreateTexture2D(width, height, TextureFormat::RGBA32F, nullptr);
        t.culled      = CreateTexture2D(width, height, TextureFormat::RGBA32F, nullptr);
        t.coeff0      = CreateTexture2D(width, height, TextureFormat::RGBA16F, nullptr);
        t.coeff1      = CreateTexture2D(width, height, TextureFormat::RGBA16F, nullptr);
        t.accumulate  = CreateTexture2D(width, height, TextureFormat::RGBA16F, nullptr);
        t.depthCopy   = CreateTexture2D(width, height, TextureFormat::Depth24Stencil8, nullptr);
        t.cloudDepth  = CreateTexture2D(width, height, TextureFormat::Depth24Stencil8, nullptr);
        const unsigned char none[4] = {0, 0, 0, 0};
        t.dummy       = CreateTexture2D(1, 1, TextureFormat::RGBA8, none);
        const TextureHandle all[] = {t.depthBounds, t.culled, t.coeff0, t.coeff1, t.accumulate,
                                     t.depthCopy, t.cloudDepth, t.dummy};
        for (TextureHandle h : all) {
            if (h == INVALID_TEXTURE) { OitDestroyTargets(); return false; }
        }
        const TextureHandle coeffs[2] = {t.coeff0, t.coeff1};
        auto make = [&](OitPass pass, const TextureHandle* colors, int count, TextureHandle depth) {
            t.fbos[static_cast<size_t>(pass)] = CreateRenderTargetFromTextures(colors, count, depth);
            return t.fbos[static_cast<size_t>(pass)] != INVALID_RENDER_TARGET;
        };
        const bool ok =
            make(OitPass::DepthBounds,        &t.depthBounds, 1, t.depthCopy) &&
            make(OitPass::DepthBoundsCull,    &t.culled,      1, t.depthCopy) &&
            make(OitPass::CloudDepthBounds,   &t.culled,      1, t.cloudDepth) &&
            make(OitPass::Transmittance,      coeffs,         2, t.depthCopy) &&
            make(OitPass::CloudTransmittance, coeffs,         2, t.cloudDepth) &&
            make(OitPass::Accumulate,         &t.accumulate,  1, t.depthCopy) &&
            make(OitPass::CloudAccumulate,    &t.accumulate,  1, t.cloudDepth);
        if (!ok) { OitDestroyTargets(); return false; }
        return true;
    }

    void GLBackend::OitDestroyTargets() {
        for (RenderTargetHandle& fbo : m_oit.fbos) {
            if (fbo != INVALID_RENDER_TARGET) DestroyRenderTarget(fbo);   // wraps: textures stay
            fbo = INVALID_RENDER_TARGET;
        }
        for (TextureHandle* h : {&m_oit.depthBounds, &m_oit.culled, &m_oit.coeff0, &m_oit.coeff1,
                                 &m_oit.accumulate, &m_oit.depthCopy, &m_oit.cloudDepth, &m_oit.dummy}) {
            if (*h != INVALID_TEXTURE) DestroyTexture(*h);
            *h = INVALID_TEXTURE;
        }
        m_oit.width = m_oit.height = 0;
        // The variants live with the option: compiled again when it comes back.
        for (auto& [engine, v] : m_oitVariants) {
            for (ShaderHandle h : v.shader) {
                if (h != INVALID_SHADER) DestroyShader(h);
            }
        }
        m_oitVariants.clear();
    }

    bool GLBackend::OitBeginPass(OitPass pass, bool clearColor) {
        if (m_oitPassOpen || m_oit.depthBounds == INVALID_TEXTURE) return false;
        // The frame's depth, snapshotted where the first pass of the frame
        // opens: the OIT passes can't attach FBO 0's own.
        if (pass == OitPass::DepthBounds) {
            glBindFramebuffer(GL_FRAMEBUFFER, m_defaultFbo);
            if (!CopyFramebufferDepthToTexture(m_oit.depthCopy)) return false;
        }
        const RenderTargetHandle fbo = m_oit.fbos[static_cast<size_t>(pass)];
        if (fbo == INVALID_RENDER_TARGET) return false;
        BindRenderTarget(fbo);
        if (clearColor) {
            // MC's clear values: the depth bounds (-FLT_MAX, 0, 0, 0) so MAX
            // blending finds the nearest and farthest, the rest zero. The
            // colour mask and scissor would limit the clear: open both.
            const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
            if (scissor) glDisable(GL_SCISSOR_TEST);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            const bool bounds = pass == OitPass::DepthBounds || pass == OitPass::DepthBoundsCull ||
                                pass == OitPass::CloudDepthBounds;
            const GLfloat boundsClear[4] = {-3.4028235e38f, 0.0f, 0.0f, 0.0f};
            const GLfloat zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            glClearBufferfv(GL_COLOR, 0, bounds ? boundsClear : zero);
            if (pass == OitPass::Transmittance || pass == OitPass::CloudTransmittance) {
                glClearBufferfv(GL_COLOR, 1, zero);
            }
            if (scissor) glEnable(GL_SCISSOR_TEST);
            m_stateInitialized = false;   // the colour mask moved under the cache
        }
        m_oitPassOpen = true;
        return true;
    }

    void GLBackend::OitEndPass() {
        if (!m_oitPassOpen) return;
        m_oitPassOpen = false;
        BindRenderTarget(INVALID_RENDER_TARGET);
    }

    TextureHandle GLBackend::OitTexture(OitImage image) const {
        switch (image) {
            case OitImage::DepthBounds:       return m_oit.depthBounds;
            case OitImage::DepthBoundsCulled: return m_oit.culled;
            case OitImage::Coeff0:            return m_oit.coeff0;
            case OitImage::Coeff1:            return m_oit.coeff1;
            case OitImage::Accumulate:        return m_oit.accumulate;
            case OitImage::FrameDepth:        return m_oit.depthCopy;
        }
        return INVALID_TEXTURE;
    }

    std::string GLBackend::ReadFileContents(const std::string& path) const {
        std::ifstream file(path);
        if (!file.is_open()) {
            Log::Error("GLBackend: Failed to open file: %s", path.c_str());
            return "";
        }
        std::stringstream ss;
        ss << file.rdbuf();
        return ss.str();
    }

    GLuint GLBackend::CompileGLShader(GLenum type, const std::string& source) const {
        GLuint shader = glCreateShader(type);
        const char* src = source.c_str();
        glShaderSource(shader, 1, &src, nullptr);
        glCompileShader(shader);

        GLint success;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
        if (!success) {
            char infoLog[512];
            glGetShaderInfoLog(shader, 512, nullptr, infoLog);
            const char* typeName = (type == GL_VERTEX_SHADER) ? "vertex" : "fragment";
            Log::Error("GLBackend: %s shader compile failed: %s", typeName, infoLog);
            glDeleteShader(shader);
            return 0;
        }
        return shader;
    }

} // namespace Render
