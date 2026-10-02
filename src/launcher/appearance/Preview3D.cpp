// File: src/launcher/appearance/Preview3D.cpp
#include "Preview3D.hpp"
#include "common/core/Log.hpp"

#include <glad/glad.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace Launcher::Appearance {

    namespace {

        // Textured or flat, cut out below a tenth of alpha (MC's entity
        // cutout; the outer skin layer's clear texels), shaded by the two
        // fixed lights MC's ENTITY_IN_UI lighting uses — given in view space,
        // so the model is lit the same from every orbit angle, as an
        // inventory model is.
        const char* kVertex = R"(#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec3 aNormal;
layout(location = 3) in vec4 aColor;
uniform mat4 uViewProj;
uniform mat4 uView;
out vec2 vUV;
out vec4 vColor;
out vec3 vNormal;
void main() {
    gl_Position = uViewProj * vec4(aPos, 1.0);
    vUV = aUV;
    vColor = aColor;
    vNormal = mat3(uView) * aNormal;
}
)";

        const char* kFragment = R"(#version 330 core
in vec2 vUV;
in vec4 vColor;
in vec3 vNormal;
uniform sampler2D uTex;
uniform int uTextured;
uniform int uLit;
out vec4 FragColor;
void main() {
    vec4 c = vColor;
    if (uTextured == 1) c *= texture(uTex, vUV);
    if (c.a < 0.1) discard;
    if (uLit == 1) {
        vec3 n = normalize(vNormal);
        vec3 l0 = normalize(vec3( 0.2, 1.0, 1.0));
        vec3 l1 = normalize(vec3(-0.2, 1.0, 0.0));
        float light = 0.4 + 0.6 * (max(dot(n, l0), 0.0) + max(dot(n, l1), 0.0));
        c.rgb *= min(light, 1.0);
    }
    FragColor = vec4(c.rgb, 1.0);
}
)";

        GLuint g_program = 0;
        int g_programUsers = 0;
        GLint g_uViewProj = -1, g_uView = -1, g_uTex = -1, g_uTextured = -1, g_uLit = -1;

        GLuint Compile(GLenum type, const char* src) {
            const GLuint s = glCreateShader(type);
            glShaderSource(s, 1, &src, nullptr);
            glCompileShader(s);
            GLint ok = 0;
            glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
            if (!ok) {
                char log[1024];
                glGetShaderInfoLog(s, sizeof(log), nullptr, log);
                Log::Error("[Preview3D] shader compile failed: %s", log);
                glDeleteShader(s);
                return 0;
            }
            return s;
        }

        bool AcquireProgram() {
            if (g_program != 0) {
                ++g_programUsers;
                return true;
            }
            const GLuint vs = Compile(GL_VERTEX_SHADER, kVertex);
            const GLuint fs = Compile(GL_FRAGMENT_SHADER, kFragment);
            if (!vs || !fs) {
                if (vs) glDeleteShader(vs);
                if (fs) glDeleteShader(fs);
                return false;
            }
            const GLuint p = glCreateProgram();
            glAttachShader(p, vs);
            glAttachShader(p, fs);
            glLinkProgram(p);
            glDeleteShader(vs);
            glDeleteShader(fs);
            GLint ok = 0;
            glGetProgramiv(p, GL_LINK_STATUS, &ok);
            if (!ok) {
                char log[1024];
                glGetProgramInfoLog(p, sizeof(log), nullptr, log);
                Log::Error("[Preview3D] program link failed: %s", log);
                glDeleteProgram(p);
                return false;
            }
            g_program = p;
            g_uViewProj = glGetUniformLocation(p, "uViewProj");
            g_uView = glGetUniformLocation(p, "uView");
            g_uTex = glGetUniformLocation(p, "uTex");
            g_uTextured = glGetUniformLocation(p, "uTextured");
            g_uLit = glGetUniformLocation(p, "uLit");
            g_programUsers = 1;
            return true;
        }

        void ReleaseProgram() {
            if (g_programUsers <= 0) return;
            if (--g_programUsers == 0 && g_program != 0) {
                glDeleteProgram(g_program);
                g_program = 0;
            }
        }

        // Everything Draw changes, put back afterwards.
        struct GlStateGuard {
            GLint drawFbo = 0, readFbo = 0, program = 0, vao = 0, arrayBuffer = 0;
            GLint activeTexture = 0, texture = 0, renderbuffer = 0;
            GLint viewport[4] = {0, 0, 0, 0};
            GLfloat clear[4] = {0, 0, 0, 0};
            GLboolean depthTest = GL_FALSE, cull = GL_FALSE, blend = GL_FALSE, scissor = GL_FALSE;
            GLboolean depthMask = GL_TRUE;
            GLint frontFace = GL_CCW, cullFace = GL_BACK, depthFunc = GL_LESS;

            GlStateGuard() {
                glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFbo);
                glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFbo);
                glGetIntegerv(GL_CURRENT_PROGRAM, &program);
                glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
                glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &arrayBuffer);
                glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture);
                glActiveTexture(GL_TEXTURE0);
                glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
                glGetIntegerv(GL_RENDERBUFFER_BINDING, &renderbuffer);
                glGetIntegerv(GL_VIEWPORT, viewport);
                glGetFloatv(GL_COLOR_CLEAR_VALUE, clear);
                depthTest = glIsEnabled(GL_DEPTH_TEST);
                cull = glIsEnabled(GL_CULL_FACE);
                blend = glIsEnabled(GL_BLEND);
                scissor = glIsEnabled(GL_SCISSOR_TEST);
                glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);
                glGetIntegerv(GL_FRONT_FACE, &frontFace);
                glGetIntegerv(GL_CULL_FACE_MODE, &cullFace);
                glGetIntegerv(GL_DEPTH_FUNC, &depthFunc);
            }
            ~GlStateGuard() {
                glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(drawFbo));
                glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(readFbo));
                glUseProgram(static_cast<GLuint>(program));
                glBindVertexArray(static_cast<GLuint>(vao));
                glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(arrayBuffer));
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(texture));
                glActiveTexture(static_cast<GLenum>(activeTexture));
                glBindRenderbuffer(GL_RENDERBUFFER, static_cast<GLuint>(renderbuffer));
                glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
                glClearColor(clear[0], clear[1], clear[2], clear[3]);
                const auto set = [](GLenum cap, GLboolean on) { if (on) glEnable(cap); else glDisable(cap); };
                set(GL_DEPTH_TEST, depthTest);
                set(GL_CULL_FACE, cull);
                set(GL_BLEND, blend);
                set(GL_SCISSOR_TEST, scissor);
                glDepthMask(depthMask);
                glFrontFace(static_cast<GLenum>(frontFace));
                glCullFace(static_cast<GLenum>(cullFace));
                glDepthFunc(static_cast<GLenum>(depthFunc));
            }
        };

    } // namespace

    Preview3D::~Preview3D() {
        Release();
    }

    void Preview3D::Release() {
        ReleaseTargets();
        const bool held = m_vao != 0;
        if (m_vbo) glDeleteBuffers(1, &m_vbo);
        if (m_vao) glDeleteVertexArrays(1, &m_vao);
        m_vbo = m_vao = 0;
        m_vboBytes = 0;
        if (held) ReleaseProgram();
    }

    void Preview3D::Clear() {
        m_verts.clear();
        m_batches.clear();
    }

    void Preview3D::AddBatch(const std::vector<PreviewVertex>& verts, GLuint texture, bool cull, bool lit) {
        if (verts.empty()) return;
        Batch b;
        b.texture = texture;
        b.cull = cull;
        b.lit = lit;
        b.first = m_verts.size();
        b.count = verts.size();
        m_verts.insert(m_verts.end(), verts.begin(), verts.end());
        m_batches.push_back(b);
    }

    glm::vec3 Preview3D::Eye() const {
        const float yaw = glm::radians(yawDeg), pitch = glm::radians(pitchDeg);
        const glm::vec3 offset(-std::sin(yaw) * std::cos(pitch), std::sin(pitch), -std::cos(yaw) * std::cos(pitch));
        return target + offset * distance;
    }

    glm::mat4 Preview3D::View() const {
        return glm::lookAt(Eye(), target, glm::vec3(0.0f, 1.0f, 0.0f));
    }

    glm::mat4 Preview3D::Projection(float aspect) const {
        return glm::perspective(glm::radians(fovDeg), aspect > 0.0f ? aspect : 1.0f, 0.05f, 50.0f);
    }

    void Preview3D::Ray(const ImVec2& local, const ImVec2& size, glm::vec3& origin, glm::vec3& dir) const {
        const float aspect = size.y > 0.0f ? size.x / size.y : 1.0f;
        const glm::mat4 inv = glm::inverse(Projection(aspect) * View());
        const float nx = (size.x > 0.0f ? local.x / size.x : 0.5f) * 2.0f - 1.0f;
        const float ny = 1.0f - (size.y > 0.0f ? local.y / size.y : 0.5f) * 2.0f;
        glm::vec4 nearP = inv * glm::vec4(nx, ny, -1.0f, 1.0f);
        glm::vec4 farP = inv * glm::vec4(nx, ny, 1.0f, 1.0f);
        nearP /= nearP.w;
        farP /= farP.w;
        origin = glm::vec3(nearP);
        dir = glm::normalize(glm::vec3(farP - nearP));
    }

    void Preview3D::Orbit(const ImVec2& delta) {
        yawDeg -= delta.x * 0.6f;
        yawDeg = std::fmod(yawDeg, 360.0f);
        if (yawDeg < 0.0f) yawDeg += 360.0f;
        pitchDeg = std::clamp(pitchDeg + delta.y * 0.4f, -60.0f, 75.0f);
    }

    void Preview3D::Zoom(float wheel, float minZoom, float maxZoom) {
        if (wheel == 0.0f) return;
        zoom = std::clamp(zoom * std::pow(0.9f, wheel), minZoom, maxZoom);
    }

    void Preview3D::FitTo(const ImVec2& size) {
        const float aspect = size.y > 0.0f ? size.x / size.y : 1.0f;
        const float tanV = std::tan(glm::radians(fovDeg) * 0.5f);
        const float tanH = tanV * aspect;
        const float fill = fitFill > 0.0f ? fitFill : 1.0f;
        // Half the extent over the half-angle's tangent puts the extent edge
        // to edge; dividing by `fill` leaves the padding. The tighter axis wins.
        const float dV = (fitExtent.y * 0.5f) / (tanV * fill);
        const float dH = (fitExtent.x * 0.5f) / (tanH * fill);
        distance = std::max(dV, dH) * zoom;
    }

    void Preview3D::ReleaseTargets() {
        if (m_fbo) glDeleteFramebuffers(1, &m_fbo);
        if (m_color) glDeleteTextures(1, &m_color);
        if (m_msFbo) glDeleteFramebuffers(1, &m_msFbo);
        if (m_msColor) glDeleteRenderbuffers(1, &m_msColor);
        if (m_msDepth) glDeleteRenderbuffers(1, &m_msDepth);
        m_fbo = m_color = m_msFbo = m_msColor = m_msDepth = 0;
        m_w = m_h = 0;
    }

    bool Preview3D::EnsureTargets(int w, int h) {
        if (w == m_w && h == m_h && m_fbo != 0) return true;
        ReleaseTargets();
        if (w <= 0 || h <= 0) return false;

        GLint maxSamples = 0;
        glGetIntegerv(GL_MAX_SAMPLES, &maxSamples);
        m_samples = std::clamp(4, 0, static_cast<int>(maxSamples));

        // Multisampled colour + depth, resolved into a plain texture.
        glGenFramebuffers(1, &m_msFbo);
        glBindFramebuffer(GL_FRAMEBUFFER, m_msFbo);
        glGenRenderbuffers(1, &m_msColor);
        glBindRenderbuffer(GL_RENDERBUFFER, m_msColor);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, m_samples, GL_RGBA8, w, h);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, m_msColor);
        glGenRenderbuffers(1, &m_msDepth);
        glBindRenderbuffer(GL_RENDERBUFFER, m_msDepth);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, m_samples, GL_DEPTH_COMPONENT24, w, h);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, m_msDepth);
        const bool msOk = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;

        glGenTextures(1, &m_color);
        glBindTexture(GL_TEXTURE_2D, m_color);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glGenFramebuffers(1, &m_fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_color, 0);
        const bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        if (!msOk || !ok) {
            Log::Error("[Preview3D] framebuffer incomplete (%dx%d, %d samples)", w, h, m_samples);
            ReleaseTargets();
            return false;
        }
        m_w = w;
        m_h = h;
        return true;
    }

    void Preview3D::Draw(ImDrawList* dl, const ImVec2& p0, const ImVec2& p1, ImU32 background, float rounding) {
        const ImVec2 size(p1.x - p0.x, p1.y - p0.y);
        const ImVec2 scale = ImGui::GetIO().DisplayFramebufferScale;
        const int w = std::max(1, static_cast<int>(std::lround(size.x * (scale.x > 0.0f ? scale.x : 1.0f))));
        const int h = std::max(1, static_cast<int>(std::lround(size.y * (scale.y > 0.0f ? scale.y : 1.0f))));

        {
            GlStateGuard guard;
            if (m_vao == 0) {
                if (!AcquireProgram()) return;
                glGenVertexArrays(1, &m_vao);
                glGenBuffers(1, &m_vbo);
                glBindVertexArray(m_vao);
                glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
                const GLsizei stride = sizeof(PreviewVertex);
                glEnableVertexAttribArray(0);
                glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride,
                                      reinterpret_cast<const void*>(offsetof(PreviewVertex, x)));
                glEnableVertexAttribArray(1);
                glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride,
                                      reinterpret_cast<const void*>(offsetof(PreviewVertex, u)));
                glEnableVertexAttribArray(2);
                glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, stride,
                                      reinterpret_cast<const void*>(offsetof(PreviewVertex, nx)));
                glEnableVertexAttribArray(3);
                glVertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride,
                                      reinterpret_cast<const void*>(offsetof(PreviewVertex, r)));
            }
            if (!EnsureTargets(w, h)) return;

            glBindFramebuffer(GL_FRAMEBUFFER, m_msFbo);
            glViewport(0, 0, w, h);
            const ImVec4 bg = ImGui::ColorConvertU32ToFloat4(background);
            glClearColor(bg.x, bg.y, bg.z, 1.0f);
            glDisable(GL_SCISSOR_TEST);
            glDisable(GL_BLEND);
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(GL_LEQUAL);
            glDepthMask(GL_TRUE);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

            if (!m_verts.empty() && g_program != 0) {
                glBindVertexArray(m_vao);
                glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
                const size_t bytes = m_verts.size() * sizeof(PreviewVertex);
                if (bytes > m_vboBytes) {
                    m_vboBytes = bytes + bytes / 2;
                    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(m_vboBytes), nullptr, GL_STREAM_DRAW);
                }
                glBufferSubData(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(bytes), m_verts.data());

                glUseProgram(g_program);
                const glm::mat4 view = View();
                const glm::mat4 viewProj = Projection(size.y > 0.0f ? size.x / size.y : 1.0f) * view;
                glUniformMatrix4fv(g_uViewProj, 1, GL_FALSE, glm::value_ptr(viewProj));
                glUniformMatrix4fv(g_uView, 1, GL_FALSE, glm::value_ptr(view));
                glUniform1i(g_uTex, 0);
                glActiveTexture(GL_TEXTURE0);
                glFrontFace(GL_CCW);
                glCullFace(GL_BACK);
                for (const Batch& b : m_batches) {
                    if (b.cull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
                    glUniform1i(g_uTextured, b.texture != 0 ? 1 : 0);
                    glUniform1i(g_uLit, b.lit ? 1 : 0);
                    glBindTexture(GL_TEXTURE_2D, b.texture);
                    glDrawArrays(GL_TRIANGLES, static_cast<GLint>(b.first), static_cast<GLsizei>(b.count));
                }
            }

            // Resolve the samples into the texture ImGui draws.
            glBindFramebuffer(GL_READ_FRAMEBUFFER, m_msFbo);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_fbo);
            glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        }

        // GL's rows run bottom-up: flip V.
        const ImTextureID id = static_cast<ImTextureID>(static_cast<uintptr_t>(m_color));
        if (rounding > 0.0f) {
            dl->AddImageRounded(id, p0, p1, ImVec2(0, 1), ImVec2(1, 0), IM_COL32_WHITE, rounding);
        } else {
            dl->AddImage(id, p0, p1, ImVec2(0, 1), ImVec2(1, 0));
        }
    }

} // namespace Launcher::Appearance
