// File: src/client/renderer/backend/opengl/GLBackend.cpp
#include "GLBackend.hpp"
#include "common/core/Log.hpp"
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <glm/gtc/type_ptr.hpp>
#include <fstream>
#include <sstream>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

namespace Render {

    // Global instance
    std::unique_ptr<RenderBackend> g_renderBackend = nullptr;

    // Default multi-draw: loops individual draws (GL overrides with native call)
    void RenderBackend::MultiDrawIndexedBaseVertex(const int32_t* indexCounts,
                                                    const size_t* indexByteOffsets,
                                                    const int32_t* baseVertices,
                                                    uint32_t drawCount) {
        for (uint32_t i = 0; i < drawCount; i++) {
            if (indexCounts[i] > 0)
                DrawIndexedBaseVertex(indexCounts[i], indexByteOffsets[i], baseVertices[i]);
        }
    }

    std::unique_ptr<RenderBackend> CreateRenderBackend(BackendType type) {
        switch (type) {
            case BackendType::OpenGL:
                return std::make_unique<GLBackend>();
            case BackendType::Vulkan:
#ifdef HAS_VULKAN
                {
                    // Include here to avoid circular dependency
                    extern std::unique_ptr<RenderBackend> CreateVulkanBackend();
                    return CreateVulkanBackend();
                }
#else
                Log::Error("Vulkan backend not available (compiled without HAS_VULKAN)");
                return nullptr;
#endif
        }
        return nullptr;
    }

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
        // Nothing special for OpenGL - frame begins implicitly
    }

    void GLBackend::EndFrame(GLFWwindow* window) {
        glfwSwapBuffers(window);
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
    }

    // ========================================================================
    // BUFFERS
    // ========================================================================

    BufferHandle GLBackend::CreateBuffer(BufferUsage usage, size_t size,
                                        const void* data, BufferAccess access) {
        GLuint glId = 0;
        glGenBuffers(1, &glId);
        if (glId == 0) return INVALID_BUFFER;

        GLenum target = ToGLBufferTarget(usage);
        GLenum glUsage = ToGLBufferUsage(access);

        glBindBuffer(target, glId);
        glBufferData(target, static_cast<GLsizeiptr>(size), data, glUsage);
        glBindBuffer(target, 0);

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

        glBindBuffer(it->second.target, it->second.glId);
        glBufferSubData(it->second.target, static_cast<GLintptr>(offset),
                       static_cast<GLsizeiptr>(size), data);
        glBindBuffer(it->second.target, 0);
    }

    void GLBackend::DestroyBuffer(BufferHandle handle) {
        auto it = m_buffers.find(handle);
        if (it == m_buffers.end()) return;

        glDeleteBuffers(1, &it->second.glId);
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
        m_textures[handle] = {glId, width, height, memSize};

        m_memStats.textureMemory += memSize;
        m_memStats.totalAllocated += memSize;
        m_memStats.textureCount++;
        if (m_memStats.totalAllocated > m_memStats.peakUsage)
            m_memStats.peakUsage = m_memStats.totalAllocated;

        return handle;
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

        glBindTexture(GL_TEXTURE_2D, it->second.glId);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, ToGLFilter(min));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, ToGLFilter(mag));
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    void GLBackend::SetTextureWrap(TextureHandle handle, TextureWrap s, TextureWrap t) {
        auto it = m_textures.find(handle);
        if (it == m_textures.end()) return;

        glBindTexture(GL_TEXTURE_2D, it->second.glId);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, ToGLWrap(s));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, ToGLWrap(t));
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    void GLBackend::GenerateMipmaps(TextureHandle handle) {
        auto it = m_textures.find(handle);
        if (it == m_textures.end()) return;

        glBindTexture(GL_TEXTURE_2D, it->second.glId);
        glGenerateMipmap(GL_TEXTURE_2D);
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
            glBindTexture(GL_TEXTURE_2D, it->second.glId);
        } else {
            glBindTexture(GL_TEXTURE_2D, 0);
        }
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

        uint32_t handle = AllocHandle();
        m_shaders[handle] = {program, {}};
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
        if (handle == m_boundShader) return;
        auto it = m_shaders.find(handle);
        if (it != m_shaders.end()) {
            glUseProgram(it->second.programId);
            m_boundShader = handle;
        }
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
        auto it = m_shaders.find(handle);
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
        auto it = m_shaders.find(handle);
        if (it == m_shaders.end()) return;
        GLint loc = it->second.GetUniform(name);
        if (loc != -1) glUniform4fv(loc, 1, glm::value_ptr(value));
    }

    void GLBackend::SetUniformVec3(ShaderHandle handle, const std::string& name,
                                   const glm::vec3& value) {
        auto it = m_shaders.find(handle);
        if (it == m_shaders.end()) return;
        GLint loc = it->second.GetUniform(name);
        if (loc != -1) glUniform3fv(loc, 1, glm::value_ptr(value));
    }

    void GLBackend::SetUniformVec2(ShaderHandle handle, const std::string& name,
                                   const glm::vec2& value) {
        auto it = m_shaders.find(handle);
        if (it == m_shaders.end()) return;
        GLint loc = it->second.GetUniform(name);
        if (loc != -1) glUniform2fv(loc, 1, glm::value_ptr(value));
    }

    void GLBackend::SetUniformFloat(ShaderHandle handle, const std::string& name, float value) {
        auto it = m_shaders.find(handle);
        if (it == m_shaders.end()) return;
        GLint loc = it->second.GetUniform(name);
        if (loc != -1) glUniform1f(loc, value);
    }

    void GLBackend::SetUniformInt(ShaderHandle handle, const std::string& name, int value) {
        auto it = m_shaders.find(handle);
        if (it == m_shaders.end()) return;
        GLint loc = it->second.GetUniform(name);
        if (loc != -1) glUniform1i(loc, value);
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
            }
            glVertexAttribPointer(attr.location, attr.componentCount, glType,
                                attr.normalized ? GL_TRUE : GL_FALSE,
                                layout.stride, reinterpret_cast<void*>(static_cast<uintptr_t>(attr.offset)));
            glEnableVertexAttribArray(attr.location);
        }

        // Bind index buffer if provided
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

        // 2. Depth+stencil renderbuffer. Cheap, not sampleable. (Use a
        //    depth texture instead if a future feature needs to sample
        //    the depth buffer.)
        glGenRenderbuffers(1, &info.depthRBO);
        glBindRenderbuffer(GL_RENDERBUFFER, info.depthRBO);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8,
                              desc.width, desc.height);
        glBindRenderbuffer(GL_RENDERBUFFER, 0);

        // 3. FBO with the color texture + depth RBO attached.
        glGenFramebuffers(1, &info.fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, info.fbo);

        auto colorIt = m_textures.find(info.colorTexture);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, colorIt->second.glId, 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                                  GL_RENDERBUFFER, info.depthRBO);

        const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            // Roll back.
            glDeleteFramebuffers(1, &info.fbo);
            glDeleteRenderbuffers(1, &info.depthRBO);
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
        glDeleteRenderbuffers(1, &it->second.depthRBO);
        DestroyTexture(it->second.colorTexture);
        m_renderTargets.erase(it);
    }

    void GLBackend::BindRenderTarget(RenderTargetHandle rt) {
        if (rt == INVALID_RENDER_TARGET) {
            // Bind default backbuffer.
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
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
        glDeleteRenderbuffers(1, &it->second.depthRBO);

        it->second.colorTexture = CreateTexture2D(w, h, it->second.colorFormat, nullptr);
        SetTextureFilter(it->second.colorTexture,
                         TextureFilter::Linear, TextureFilter::Linear);

        glGenRenderbuffers(1, &it->second.depthRBO);
        glBindRenderbuffer(GL_RENDERBUFFER, it->second.depthRBO);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
        glBindRenderbuffer(GL_RENDERBUFFER, 0);

        glBindFramebuffer(GL_FRAMEBUFFER, it->second.fbo);
        auto colorIt = m_textures.find(it->second.colorTexture);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, colorIt->second.glId, 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                                  GL_RENDERBUFFER, it->second.depthRBO);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

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
        auto it = m_meshes.find(mesh);
        if (it == m_meshes.end()) return;

        glBindVertexArray(it->second.vao);
        glDrawElements(ToGLPrimitive(m_currentState.primitiveType), indexCount, GL_UNSIGNED_INT,
                      reinterpret_cast<void*>(static_cast<uintptr_t>(indexOffset * sizeof(uint32_t))));
        // VAO intentionally left bound — next DrawIndexed will rebind, avoiding redundant unbind/rebind cycles
    }

    void GLBackend::DrawArrays(MeshHandle mesh, uint32_t vertexCount, uint32_t firstVertex) {
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
            glBindBuffer(GL_ARRAY_BUFFER, it->second.glId);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE,
                                  static_cast<GLsizei>(stride),
                                  reinterpret_cast<void*>(static_cast<uintptr_t>(0)));
            glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE,
                                  static_cast<GLsizei>(stride),
                                  reinterpret_cast<void*>(static_cast<uintptr_t>(3 * sizeof(float))));
            glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE,
                                  static_cast<GLsizei>(stride),
                                  reinterpret_cast<void*>(static_cast<uintptr_t>(5 * sizeof(float))));
        }
    }

    void GLBackend::BindIndexBuffer(BufferHandle ibo) {
        auto it = m_buffers.find(ibo);
        if (it == m_buffers.end()) return;
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, it->second.glId);
    }

    void GLBackend::DrawIndexedBaseVertex(uint32_t indexCount, size_t indexByteOffset, int32_t baseVertex) {
        glDrawElementsBaseVertex(
            GL_TRIANGLES,
            static_cast<GLsizei>(indexCount),
            GL_UNSIGNED_INT,
            reinterpret_cast<const void*>(indexByteOffset),
            static_cast<GLint>(baseVertex));
    }

    void GLBackend::MultiDrawIndexedBaseVertex(const int32_t* indexCounts,
                                                const size_t* indexByteOffsets,
                                                const int32_t* baseVertices,
                                                uint32_t drawCount) {
        // Convert size_t byte offsets to const void* for GL
        // Stack-allocate for typical draw counts, heap for large batches
        if (drawCount <= 256) {
            const void* offsets[256];
            for (uint32_t i = 0; i < drawCount; i++)
                offsets[i] = reinterpret_cast<const void*>(indexByteOffsets[i]);
            glMultiDrawElementsBaseVertex(
                GL_TRIANGLES,
                reinterpret_cast<const GLsizei*>(indexCounts),
                GL_UNSIGNED_INT,
                offsets,
                static_cast<GLsizei>(drawCount),
                const_cast<GLint*>(reinterpret_cast<const GLint*>(baseVertices)));
        } else {
            std::vector<const void*> offsets(drawCount);
            for (uint32_t i = 0; i < drawCount; i++)
                offsets[i] = reinterpret_cast<const void*>(indexByteOffsets[i]);
            glMultiDrawElementsBaseVertex(
                GL_TRIANGLES,
                reinterpret_cast<const GLsizei*>(indexCounts),
                GL_UNSIGNED_INT,
                offsets.data(),
                static_cast<GLsizei>(drawCount),
                const_cast<GLint*>(reinterpret_cast<const GLint*>(baseVertices)));
        }
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
            glVertexAttribFormat(0, 3, GL_FLOAT, GL_FALSE, 0);
            glVertexAttribBinding(0, 0);
            glVertexAttribFormat(1, 2, GL_FLOAT, GL_FALSE, 3 * sizeof(float));
            glVertexAttribBinding(1, 0);
            glVertexAttribFormat(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, 5 * sizeof(float));
            glVertexAttribBinding(2, 0);
        }

        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);
        glEnableVertexAttribArray(2);

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
        if (it == m_timers.end()) return 0.0f;

        if (!it->second.resultReady) {
            GLint available = 0;
            glGetQueryObjectiv(it->second.queryId, GL_QUERY_RESULT_AVAILABLE, &available);
            if (available) {
                GLuint64 timeNs = 0;
                glGetQueryObjectui64v(it->second.queryId, GL_QUERY_RESULT, &timeNs);
                it->second.resultMs = static_cast<float>(timeNs) / 1000000.0f;
                it->second.resultReady = true;
            }
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
